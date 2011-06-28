/* ST-Ericsson U300 RIL
**
** Copyright (C) ST-Ericsson AB 2008-2010
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
**
** Based on reference-ril by The Android Open Source Project.
**
** Heavily modified for ST-Ericsson U300 modems.
** Author: Christian Bejram <christian.bejram@stericsson.com>
*/

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <assert.h>
#include <telephony/ril.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include "u300-ril.h"

#define LOG_TAG "RILV"
#include <utils/Log.h>

void onNewStatusReport(const char *sms_pdu)
{
    char *response = NULL;

    /* Baseband will not prepend SMSC addr, but Android expects it. */
    asprintf(&response, "%s%s", "00", sms_pdu);

    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT,
                              response, strlen(response));
}

void onNewBroadcastSms(const char *pdu)
{
    char *message = NULL;
    int i;

    if (strlen(pdu) != (2 * 88)) {
        LOGE("Broadcast Message length error! Discarding!");
        goto error;
    }

    message = alloca(88);
    memset(message, 0, 88);

    for (i = 0; i < 88; i++) {
        message[i] |= (char2nib(pdu[i * 2]) << 4);
        message[i] |= char2nib(pdu[i * 2 + 1]);
    }

    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS,
                              message, sizeof(char *));

error:
    return;
}

void onNewSmsOnSIM(const char *s)
{
    char *line;
    char *mem;
    char *tok;
    int err = 0;
    int index = -1;

    tok = line = strdup(s);

    err = at_tok_start(&tok);
    if (err < 0)
        goto error;

    err = at_tok_nextstr(&tok, &mem);
    if (err < 0)
        goto error;

    if (strncmp(mem, "SM", 2) != 0)
        goto error;

    err = at_tok_nextint(&tok, &index);
    if (err < 0)
        goto error;

    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NEW_SMS_ON_SIM,
                              &index, sizeof(int *));

finally:
    free(line);
    return;

error:
    LOGE("Failed to parse +CMTI.");
    goto finally;
}

#define BROADCAST_MAX_RANGES_SUPPORTED 10

/**
 * RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG
 */
void requestGSMGetBroadcastSMSConfig(void *data, size_t datalen,
                                     RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int mode, err = 0;
    unsigned int i, count = 0;
    char *mids;
    char *range;
    char *tok = NULL;

    RIL_GSM_BroadcastSmsConfigInfo *configInfo[BROADCAST_MAX_RANGES_SUPPORTED];

    err = at_send_command_singleline("AT+CSCB?", "+CSCB:", &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto error;

    tok = atresponse->p_intermediates->line;

    err = at_tok_start(&tok);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&tok, &mode);
    if (err < 0)
        goto error;

    /**
     * Get the string that yields the service ids (mids). AT+CSCB <mids>
     * parameter may contain a mix of single service ids (,%d,) and service id
     * ranges (,%d-%d,).
     */
    err = at_tok_nextstr(&tok, &mids);
    if (err < 0)
        goto error;

    while (at_tok_nextstr(&mids, &range) == 0) {
        if (count < BROADCAST_MAX_RANGES_SUPPORTED) {
            configInfo[count] = calloc(1,
                sizeof(RIL_GSM_BroadcastSmsConfigInfo));
            assert(configInfo[count] != NULL);

            /* No support for "Not accepted mids", selected is always 1 */
            configInfo[count]->selected = 1;

            /* Fetch fromServiceId value */
            err = at_tok_nextint(&range, &(configInfo[count]->fromServiceId));
            if (err < 0)
                goto error;
            /* Try to fetch toServiceId value if it exist */
            err = at_tok_nextint(&range, &(configInfo[count]->toServiceId));
            if (err < 0)
                configInfo[count]->toServiceId =
                    configInfo[count]->fromServiceId;

            count++;
        } else {
            LOGW("%s(): Max limit (%d) passed, can not send all ranges "
                 "reported by modem.", __func__,
                 BROADCAST_MAX_RANGES_SUPPORTED);
            break;
        }
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &configInfo,
                          sizeof(RIL_GSM_BroadcastSmsConfigInfo *) * count);

    goto exit;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

exit:
    at_response_free(atresponse);
    for (i = 0; i < count; i++)
        free(configInfo[i]);
}

/**
 * RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG
 */
void requestGSMSetBroadcastSMSConfig(void *data, size_t datalen,
                                     RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int err, count, i;
    char *cmd, *tmp, *mids = NULL;
    RIL_GSM_BroadcastSmsConfigInfo **configInfoArray =
        (RIL_GSM_BroadcastSmsConfigInfo **) data;
    RIL_GSM_BroadcastSmsConfigInfo *configInfo = NULL;

    count = NUM_ELEMS(configInfoArray);
    LOGI("Number of MID ranges in BROADCAST_SMS_CONFIG: %d", count);

    for (i = 0; i < count; i++) {
        configInfo = configInfoArray[i];
        /* No support for "Not accepted mids" in AT */
        if (configInfo->selected) {
            tmp = mids;
            asprintf(&mids, "%s%d-%d%s", (tmp ? tmp : ""),
                configInfo->fromServiceId, configInfo->toServiceId,
                (i == (count - 1) ? "" : ",")); /* Last one? Skip comma */
            free(tmp);
        }
    }

    if (mids == NULL)
        goto error;

    asprintf(&cmd, "AT+CSCB=0,\"%s\"", mids);
    free(mids);

    err = at_send_command(cmd, &atresponse);
    free(cmd);

    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    goto exit;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

exit:
    at_response_free(atresponse);
}

/**
 * RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION
 */
void requestGSMSMSBroadcastActivation(void *data, size_t datalen,
                                      RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int mode, mt, bm, ds, bfr, skip;
    int activation;
    char *cmd = NULL;
    char *tok;
    int err;

    /* AT+CNMI=[<mode>[,<mt>[,<bm>[,<ds>[,<bfr>]]]]] */
    err = at_send_command_singleline("AT+CNMI?", "+CNMI:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    tok = atresponse->p_intermediates->line;

    err = at_tok_start(&tok);
    if (err < 0)
        goto error;
    err = at_tok_nextint(&tok, &mode);
    if (err < 0)
        goto error;
    err = at_tok_nextint(&tok, &mt);
    if (err < 0)
        goto error;
    err = at_tok_nextint(&tok, &skip);
    if (err < 0)
        goto error;
    err = at_tok_nextint(&tok, &ds);
    if (err < 0)
        goto error;
    err = at_tok_nextint(&tok, &bfr);
    if (err < 0)
        goto error;

    at_response_free(atresponse);

    /* 0 - Activate, 1 - Turn off */
    activation = *((const int *)data);
    if (activation == 0)
        bm = 2;
    else
        bm = 0;

    asprintf(&cmd, "AT+CNMI=%d,%d,%d,%d,%d", mode, mt, bm, ds, bfr);

    err = at_send_command(cmd, &atresponse);
    free(cmd);

    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/*
 * Returns: 0 if FDN is not enabled
 *          1 if FDN is enabled
 */
static int isFdnEnabled(void)
{
    int err = -1;
    int status = 0;
    char *line = NULL;
    ATResponse *atresponse = NULL;

    err = at_send_command_multiline("AT+CLCK=\"FD\",2", "+CLCK:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto finally;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto finally;

    err = at_tok_nextint(&line, &status);
    if (err < 0 )
        goto finally;

    /* status = 1 means that FDN facility is active */
    if (status != 1)
        status = 0;

finally:
    at_response_free(atresponse);
    return status;
}

/**
 * RIL_REQUEST_SEND_SMS
 *
 * Sends an SMS message.
 */
void requestSendSMS(void *data, size_t datalen, RIL_Token t)
{
    int err;
    const char *smsc;
    const char *pdu;
    char *line;
    int tpLayerLength;
    char *cmd1, *cmd2;
    RIL_SMS_Response response;
    RIL_Errno ret = RIL_E_GENERIC_FAILURE;
    ATResponse *atresponse = NULL;
    ATCmsError cms_error_code;

    smsc = ((const char **) data)[0];
    pdu = ((const char **) data)[1];

    tpLayerLength = strlen(pdu) / 2;

    /* NULL for default SMSC. */
    if (smsc == NULL)
        smsc = "00";

    asprintf(&cmd1, "AT+CMGS=%d", tpLayerLength);
    asprintf(&cmd2, "%s%s", smsc, pdu);

    err = at_send_command_sms(cmd1, cmd2, "+CMGS:", &atresponse);
    free(cmd1);
    free(cmd2);

    if (err != 0) {
        goto error;
    }
    if (atresponse->success == 0) {
        if (at_get_cms_error(atresponse, &cms_error_code)) {
            switch (cms_error_code) {
            case CMS_NETWORK_TIMEOUT:
                ret = RIL_E_SMS_SEND_FAIL_RETRY;
                break;
            case CMS_PRE_DIAL_CHECK_ERROR:
                /* failing pre-dial check may indicate FDN check failure */
                if (isFdnEnabled())
                   ret = RIL_E_FDN_CHECK_FAILURE;
                break;
            default:
                break;
            }
        }
        goto error;
    }

    memset(&response, 0, sizeof(response));
    response.errorCode = -1;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &response.messageRef);
    if (err < 0)
        goto error;

    /* ackPDU is not supported */

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, ret, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_SEND_SMS_EXPECT_MORE
 *
 * Send an SMS message. Identical to RIL_REQUEST_SEND_SMS,
 * except that more messages are expected to be sent soon. If possible,
 * keep SMS relay protocol link open (eg TS 27.005 AT+CMMS command).
 */
void requestSendSMSExpectMore(void *data, size_t datalen, RIL_Token t)
{
    /* Throw the command on the channel and ignore any errors, since we
       need to send the SMS anyway and subsequent SMSes will be sent anyway. */
    (void)at_send_command("AT+CMMS=1", NULL);

    requestSendSMS(data, datalen, t);
}

/**
 * RIL_REQUEST_SMS_ACKNOWLEDGE
 *
 * Acknowledge successful or failed receipt of SMS previously indicated
 * via RIL_UNSOL_RESPONSE_NEW_SMS .
 */
void requestSMSAcknowledge(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int ack;
    int err;

    assert(datalen >= sizeof(int));
    ack = ((int *)data)[0];

    switch (ack) {
    case 0: /* Failed receipt */

        assert(datalen >= 2 * sizeof(int));
        switch (((int *)data)[1]) {
        case 0xD3: /* Memory capacity exceeded */
            err = at_send_command_with_pdu("AT+CNMA=2,3", "00D300", &atresponse);
            break;
        case 0xFF: /* Unspecified error */
            err = at_send_command_with_pdu("AT+CNMA=2,3", "00FF00", &atresponse);
            break;
        default:
            LOGE("%s(): Invalid failure cause from Android.\r\n", __func__);
            goto error;
        }

        break;
    case 1: /* Successful receipt */
        err = at_send_command("AT+CNMA=1", &atresponse);
        break;
    default:
        LOGE("%s(): Invalid parameter.\r\n", __func__);
        goto error;
    }

    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_WRITE_SMS_TO_SIM
 *
 * Stores a SMS message to SIM memory.
 */
void requestWriteSmsToSim(void *data, size_t datalen, RIL_Token t)
{
    RIL_SMS_WriteArgs *args;
    char *cmd;
    char *pdu;
    char *line;
    int length;
    int index;
    int err;
    ATResponse *atresponse = NULL;

    args = (RIL_SMS_WriteArgs *) data;

    length = strlen(args->pdu) / 2;
    asprintf(&cmd, "AT+CMGW=%d,%d", length, args->status);
    asprintf(&pdu, "%s%s", (args->smsc ? args->smsc : "00"), args->pdu);

    err = at_send_command_sms(cmd, pdu, "+CMGW:", &atresponse);
    free(cmd);
    free(pdu);

    if (err < 0 || atresponse->success == 0)
        goto error;

    if (atresponse->p_intermediates->line == NULL)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &index);
    if (err < 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &index, sizeof(int *));

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_DELETE_SMS_ON_SIM
 *
 * Deletes a SMS message from SIM memory.
 */
void requestDeleteSmsOnSim(void *data, size_t datalen, RIL_Token t)
{
    char *cmd;
    ATResponse *atresponse = NULL;
    int err;

    asprintf(&cmd, "AT+CMGD=%d", ((int *) data)[0]);
    err = at_send_command(cmd, &atresponse);
    free(cmd);
    if (err < 0 || atresponse->success == 0)
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    else
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(atresponse);
    return;
}

/**
 * RIL_REQUEST_GET_SMSC_ADDRESS
 */
void requestGetSMSCAddress(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int err;
    char *line;
    char *response;

    err = at_send_command_singleline("AT+CSCA?", "+CSCA:", &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextstr(&line, &response);
    if (err < 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(char *));

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_SET_SMSC_ADDRESS
 */
void requestSetSMSCAddress(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int err;
    char *cmd;
    const char *smsc = (const char *) data;

    asprintf(&cmd, "AT+CSCA=\"%s\"", smsc);
    err = at_send_command(cmd, &atresponse);
    free(cmd);
    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_REPORT_SMS_MEMORY_STATUS
 */
void requestSmsStorageFull(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int ack;
    int err;

    assert(datalen >= sizeof(int));
    ack = ((int *) data)[0];

    /* Android will call RIL_REQUEST_REPORT_SMS_MEMORY_STATUS in case of:
    0. memory is full
    1. memory was full and been cleaned up, inform modem memory is available now. */
    switch (ack) {
    case 0:
        /* Android will handle this, no need to inform modem. always return success. */
        LOGI("SMS storage full.\r\n");
        break;
    case 1:
        err = at_send_command("AT*ESMSMEMAVAIL", &atresponse);

        if (err < 0 || atresponse->success == 0) {
            LOGE("%s(): Failed to reset memory status to network, incoming SMS might be blocked.\n ", __func__);
            goto error;
        }
        break;
    default:
        LOGE("%s(): Invalid parameter.\r\n", __func__);
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
finally:

    at_response_free(atresponse);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}
