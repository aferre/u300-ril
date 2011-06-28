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
#include <assert.h>
#include <telephony/ril.h>
#include "atchannel.h"
#include "at_tok.h"
#include "u300-ril.h"
#define LOG_TAG "RILV"
#include <utils/Log.h>

/**
 * RIL_REQUEST_QUERY_CLIP
 *
 * Queries the status of the CLIP supplementary service.
 *
 * (for MMI code "*#30#")
 */
void requestQueryClip(void *data, size_t datalen, RIL_Token t)
{
    /* AT+CLIP? */
    char *line = NULL;
    int err = 0;
    int response = 2;
    ATResponse *atresponse = NULL;

    err = at_send_command_singleline("AT+CLIP?", "+CLIP:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    /* Read the first int and ignore it, we just want to know if
       CLIP is provisioned. */
    err = at_tok_nextint(&line, &response);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &response);
    if (err < 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));

finally:
    at_response_free(atresponse);

    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_CANCEL_USSD
 *
 * Cancel the current USSD session if one exists.
 */
void requestCancelUSSD(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int err;

    err = at_send_command_numeric("AT+CUSD=2", &atresponse);

    if (err < 0 || atresponse->success == 0)
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(atresponse);
    return;
}

/**
 * RIL_REQUEST_SEND_USSD
 *
 * Send a USSD message.
 *
 * See also: RIL_REQUEST_CANCEL_USSD, RIL_UNSOL_ON_USSD.
 */
void requestSendUSSD(void *data, size_t datalen, RIL_Token t)
{
    const char *ussdRequest;
    char *cmd = NULL;
    int err = -1;
    ATResponse *response = NULL;

    ussdRequest = (const char *)data;

    /*
     * ussdRequest should be checked for invalid characters that can be used to
     * send invalid AT commands. However Android have complete check of
     * ussd strings before they are sent to the RIL.
     *
     * AT+CUSD=[<n>[,<str>[,<dcs>]]]
     * <n> = 0,1,2 Dissable, Enable, Cancel
     * <str> = CUSD string in UTF8
     * <dcs> = Cell Brodcast Data Coding Scheme:
     *   0000 German
     *   0001 English
     *   0010 Italian
     *   0011 French
     *   0100 Spanish
     *   0101 Dutch
     *   0110 Swedish
     *   0111 Danish
     *   1000 Portuguese
     *   1001 Finnish
     *   1010 Norwegian
     *   1011 Greek
     *   1100 Turkish
     *   1101..1110 Reserved for European languages
     *   1111 Language unspecified
     *
     * According to Android ril.h , CUSD messages are allways sent as utf8,
     * but the dcs field does not have an entry for this.
     * The nearest "most correct" would be 15 = unspecified,
     * not adding the dcs would result in the default "0" meanig German,
     * and some networks are not happy with this.
     */

    asprintf(&cmd, "AT+CUSD=1,\"%s\",15", ussdRequest);

    err = at_send_command(cmd, &response);
    free(cmd);

    if (err < 0 || response->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
    if (response != NULL)
        at_response_free(response);

    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_UNSOL_SUPP_SVC_NOTIFICATION
 *
 * Reports supplementary service related notification for
 * MO and MT voice calls from the network.
 */
void onSuppServiceNotification(const char *s, int type)
{
    RIL_SuppSvcNotification ssnResponse;
    char *line = NULL;
    char *tok = NULL;
    int skip;
    int err = -1;

    line = tok = strdup(s);

    memset(&ssnResponse, 0, sizeof(ssnResponse));
    ssnResponse.notificationType = type;
    ssnResponse.number = NULL;

    /*
     * Type = 0 (MO call): +CSSI: <code1>[,<index>]
     * Type = 1 (MT call): +CSSU: <code2>[,<index>[,<number>,<type>
     *                            [,<subaddr>,<satype>]]]
     *
     * <subaddr> and <satype> are not supported by Android.
     */

    err = at_tok_start(&tok);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&tok, &ssnResponse.code);
    if (err < 0)
        goto error;

    if ((type == 0 && ssnResponse.code == 4) ||
        (type == 1 && ssnResponse.code == 1)) {
        err = at_tok_nextint(&tok, &ssnResponse.index);
        if (err < 0)
            goto error;
    } else {
        (void)at_tok_nextint(&tok, &skip);
    }

    if (type == 0)
        goto finally;

    err = at_tok_nextstr(&tok, &ssnResponse.number);
    if (err < 0)
        goto finally;

    err = at_tok_nextint(&tok, &ssnResponse.type);
    if (err < 0)
    {
        LOGE("%s() <number> present but <type> missing for +CSSU!", __func__);
        /*
         * According to ril.h number may be NULL if not present. To comply with
         * 27.007 where number and type may be optional we omit both in the response.
         */
        ssnResponse.number = NULL;
    }

    goto finally;

error:
    LOGE("%s() failed to parse %s", __func__, s);
    goto exit;
finally:
    RIL_onUnsolicitedResponse(RIL_UNSOL_SUPP_SVC_NOTIFICATION,
                              &ssnResponse, sizeof(ssnResponse));
exit:
    free(line);
}

/**
 * RIL_UNSOL_ON_USSD
 *
 * Called when a new USSD message is received.
 */
void onUSSDReceived(const char *s)
{
    char* m_as_string = NULL;
    char* str = NULL;
    const char *response[2];
    char *line;
    int err = -1;
    int m = 0;
    int n = 0;

    line = alloca(strlen(s) + 1);
    strcpy(line, s);
    line[strlen(s)] = '\0';

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &m);
    if (err < 0)
        goto error;

    if (m < 0 || m > 5)
        goto error;

    asprintf(&m_as_string, "%d", m);

    if (m < 2) {
        n = 2;

        err = at_tok_nextstr(&line, &str);
        if (err < 0)
            goto error;
    } else {
        n = 1;
    }

    response[0] = m_as_string;
    response[1] = str;

    /* TODO: We ignore the <dcs> parameter, might need this some day. */

    RIL_onUnsolicitedResponse(RIL_UNSOL_ON_USSD, response,
                              n * sizeof(const char *));

error:
    return;
}

/**
 * RIL_REQUEST_GET_CLIR
 *
 * Gets current CLIR status.
 */
void requestGetCLIR(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int err;
    int response[2];            // <n> and <m>
    char *line;

    err = at_send_command_singleline("AT+CLIR?", "+CLIR:", &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    /* Parse and store <n> as first repsonse parameter. */
    err = at_tok_nextint(&line, &(response[0]));
    if (err < 0)
        goto error;

    /* Parse and store <m> as second response parameter. */
    err = at_tok_nextint(&line, &(response[1]));
    if (err < 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_SET_CLIR
 */
void requestSetCLIR(void *data, size_t datalen, RIL_Token t)
{
    char *cmd = NULL;
    int err;
    ATResponse *atresponse = NULL;

    asprintf(&cmd, "AT+CLIR=%d", ((int *) data)[0]);

    err = at_send_command(cmd, &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
    free(cmd);
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_QUERY_CALL_FORWARD_STATUS
 */
void requestQueryCallForwardStatus(void *data, size_t datalen, RIL_Token t)
{
    char *cmd = NULL;
    int err;
    ATResponse *atresponse = NULL;
    ATLine *cursor = NULL;
    const RIL_CallForwardInfo *pCallForwardInfo =
        (const RIL_CallForwardInfo *) data;
    int n = 0;
    int i = 0;
    RIL_CallForwardInfo *rilResponse = NULL;
    RIL_CallForwardInfo **rilResponseArray = NULL;
    int classx = pCallForwardInfo->serviceClass;

    /*
     * Android sends class 0 for USSD strings that didn't contain a class.
     * Class 0 is not considered a valid value and according to 3GPP 24.080 a
     * missing BasicService (BS) parameter in the Supplementary Service string
     * indicates all BS'es.
     *
     * Therefore we convert a class of 0 into 255 (all classes) before sending
     * the AT command.
     */
    if (classx == 0)
        classx = 255;

    /*
     * AT+CCFC=<reason>,<mode>[,<number>[,<type>[,<class> [,<subaddr> [,<satype>
     * [,<time>]]]]]]
     */
    asprintf(&cmd, "AT+CCFC=%d,2,,,%d", pCallForwardInfo->reason, classx);

    err = at_send_command_multiline(cmd, "+CCFC:", &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto error;

    for (cursor = atresponse->p_intermediates; cursor != NULL;
         cursor = cursor->p_next)
        n++;

    if (n > 0) {
        rilResponse = alloca(n * sizeof(RIL_CallForwardInfo));
        rilResponseArray = alloca(n * sizeof(RIL_CallForwardInfo *));
        memset(rilResponse, 0, sizeof(RIL_CallForwardInfo) * n);

        for (i = 0; i < n; i++) {
            rilResponseArray[i] = &(rilResponse[i]);
            rilResponse[i].number = NULL;
        }

        /* When <mode>=2 and command successful:
         * +CCFC: <status>,<class1>[,<number>,<type>
         * [,<subaddr>,<satype>[,<time>]]][
         * <CR><LF>
         * +CCFC: <status>,<class2>[,<number>,<type>
         * [,<subaddr>,<satype>[,<time>]]]
         * [...]]
         */
        for (i = 0, cursor = atresponse->p_intermediates;
             cursor != NULL && i < n; cursor = cursor->p_next, ++i) {
            char *line = NULL;
            line = cursor->line;

            rilResponse[i].reason = pCallForwardInfo->reason;

            err = at_tok_start(&line);
            if (err < 0)
                goto error;

            err = at_tok_nextint(&line, &rilResponse[i].status);
            if (err < 0)
                goto error;

            err = at_tok_nextint(&line, &rilResponse[i].serviceClass);
            if (err < 0)
                goto error;

            if (at_tok_hasmore(&line)) {
                err = at_tok_nextstr(&line, &rilResponse[i].number);
                if (err < 0)
                    goto error;

                err = at_tok_nextint(&line, &rilResponse[i].toa);
                if (err < 0)
                    goto error;
            }
        }
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, rilResponseArray,
                          n * sizeof(RIL_CallForwardInfo *));

finally:
    if (cmd != NULL)
        free(cmd);
    if (atresponse != NULL)
        at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_SET_CALL_FORWARD
 *
 * Configure call forward rule.
 */
void requestSetCallForward(void *data, size_t datalen, RIL_Token t)
{
    char *cmd = NULL;
    int err;
    ATResponse *atresponse = NULL;
    const RIL_CallForwardInfo *callForwardInfo =
        (RIL_CallForwardInfo *) data;
    int classx = callForwardInfo->serviceClass;

    /*
     * Android sends class 0 for USSD strings that didn't contain a class.
     * Class 0 is not considered a valid value and according to 3GPP 24.080 a
     * missing BasicService (BS) parameter in the Supplementary Service string
     * indicates all BS'es.
     *
     * Therefore we convert a class of 0 into 255 (all classes) before sending
     * the AT command.
     */
    if (classx == 0)
        classx = 255;

    /*
     * Android may send down the phone number even if mode = 0 (disable) or
     * mode = 4 (erasure). This will give ERROR from the network, which means
     * that the dialstring must be disregarded by the RIL.
     *
     * The phone number must always be included when mode = 3 (registration).
     * If a phone number has been registered for reason the phone number
     * may not be included for mode = 1 (enable).
     *
     * AT+CCFC=<reason>,<mode>[,<number>[,<type>[,<class> [,<subaddr> [,<satype>
     * [,<time>]]]]]]
     */
    if (((callForwardInfo->status == 1) ||
        (callForwardInfo->status == 3)) &&
        (callForwardInfo->number != NULL)) {
        asprintf(&cmd, "AT+CCFC=%d,%d,\"%s\",%d,%d",
                 callForwardInfo->reason,
                 callForwardInfo->status,
                 callForwardInfo->number,
                 callForwardInfo->toa,
                 classx);
    } else if ((callForwardInfo->status == 0) ||
               (callForwardInfo->status == 1) ||
               (callForwardInfo->status == 4)) {
        asprintf(&cmd, "AT+CCFC=%d,%d,,,%d",
                 callForwardInfo->reason, callForwardInfo->status, classx);
    } else {
        goto error;
    }

    err = at_send_command(cmd, &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
    free(cmd);
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_QUERY_CALL_WAITING
 *
 * Query current call waiting state.
 */
void requestQueryCallWaiting(void *data, size_t datalen, RIL_Token t)
{
    char *cmd = NULL;
    char *line = NULL;
    int response[2] = { 0, 0 };
    int err, size = 1;
    ATResponse *atresponse = NULL;
    ATLine *cursor = NULL;
    int classx = ((int *) data)[0];

    /*
     * Android sends class 0 for USSD strings that didn't contain a class.
     * Class 0 is not considered a valid value and according to 3GPP 24.080 a
     * missing BasicService (BS) parameter in the Supplementary Service string
     * indicates all BS'es.
     *
     * Therefore we convert a class of 0 into 255 (all classes) before sending
     * the AT command.
     */
    if (classx == 0)
        classx = 255;

    /* AT+CCWA=[<n>[,<mode>[,<class>]]] n=0 (default) mode=2 (query) */
    asprintf(&cmd, "AT+CCWA=0,2,%d", classx);

    err = at_send_command_multiline(cmd, "+CCWA:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    /* When <mode> =2 and command successful:
       +CCWA: <status>,<class1>[<CR><LF>+CCWA: <status>,<class2>[...]]  */
    for (cursor = atresponse->p_intermediates; cursor != NULL;
         cursor = cursor->p_next) {
        int serviceClass = 0;
        int status = 0;
        line = cursor->line;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &status);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &serviceClass);
        if (err < 0)
            goto error;

        if (status == 1 && serviceClass > 0 && serviceClass <= 128)
            response[1] |= serviceClass;
    }

    if (response[1] > 0) {
        response[0] = 1;
        size = 2;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(int) * size);
    goto exit;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

exit:
    free(cmd);
    at_response_free(atresponse);
}

/**
 * RIL_REQUEST_SET_CALL_WAITING
 *
 * Configure current call waiting state.
 */
void requestSetCallWaiting(void *data, size_t datalen, RIL_Token t)
{
    char *pCmd = NULL;
    int err;
    ATResponse *atresponse = NULL;
    const int mode = ((const int *) data)[0];
    int classx = ((const int *) data)[1];

    /*
     * Android sends class 0 for USSD strings that didn't contain a class.
     * Class 0 is not considered a valid value and according to 3GPP 24.080 a
     * missing BasicService (BS) parameter in the Supplementary Service string
     * indicates all BS'es.
     *
     * Therefore we convert a class of 0 into 255 (all classes) before sending
     * the AT command.
     */
    if (classx == 0)
        classx = 255;

    /* AT+CCWA=[<n>[,<mode>[,<classx>]]] n=0 (default) */
    asprintf(&pCmd, "AT+CCWA=1,%d,%d", mode, classx);

    err = at_send_command(pCmd, &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    goto exit;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

exit:
    free(pCmd);
    at_response_free(atresponse);
}

/**
 * RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION
 *
 * Enables/disables supplementary service related notifications
 * from the network.
 *
 * Notifications are reported via RIL_UNSOL_SUPP_SVC_NOTIFICATION.
 *
 * See also: RIL_UNSOL_SUPP_SVC_NOTIFICATION.
 */
void requestSetSuppSvcNotification(void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *atresponse = NULL;
    int ssn = ((int *) data)[0];

    if (ssn != 0 && ssn != 1)
        goto error;

    err = at_send_command(ssn?"AT+CSSN=1,1":"AT+CSSN=0,0", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    goto exit;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

exit:
    at_response_free(atresponse);
}
