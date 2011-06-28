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
** Author: Sverre Vegge <sverre.vegge@stericsson.com>
*/

#include <stdio.h>
#include <string.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include <telephony/ril.h>
#include "u300-ril.h"

#define LOG_TAG "RILV"
#include <utils/Log.h>

enum SimResetMode {
    SAT_SIM_INITIALIZATION_AND_FULL_FILE_CHANGE_NOTIFICATION = 0,
    SAT_FILE_CHANGE_NOTIFICATION = 1,
    SAT_SIM_INITIALIZATION_AND_FILE_CHANGE_NOTIFICATION = 2,
    SAT_SIM_INITIALIZATION = 3,
    SAT_SIM_RESET = 4,
    SAT_NAA_APPLICATION_RESET = 5,
    SAT_NAA_SESSION_RESET = 6,
    SAT_STEERING_OF_ROAMING = 7
};

typedef struct {
    int cmdNumber;
    int cmdQualifier;
    int Result;
} REFRESH_Status;

static REFRESH_Status s_refeshStatus = {-1,-1,-1};

/**
 * RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE
 *
 * Requests to send a terminal response to SIM for a received
 * proactive command.
 */
void requestStkSendTerminalResponse(void *data, size_t datalen,
                                    RIL_Token t)
{
    char *cmd = NULL;
    int err;
    ATResponse *atresponse = NULL;
    const char *stkResponse = (const char *) data;
#ifndef USE_LEGACY_SAT_AT_CMDS
    asprintf(&cmd, "AT+CUSATT=\"%s\"", stkResponse);
#else
    asprintf(&cmd, "AT*STKR=\"%s\"", stkResponse);
#endif
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
    return;
}

/**
 * RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND
 *
 * Requests to send a SAT/USAT envelope command to SIM.
 * The SAT/USAT envelope command refers to 3GPP TS 11.14 and 3GPP TS 31.111.
 */
void requestStkSendEnvelopeCommand(void *data, size_t datalen, RIL_Token t)
{
    char *cmd = NULL;
    char *line = NULL;
    char *stkResponse = NULL;
    int err;
    ATResponse *atresponse = NULL;
    const char *ec = (const char *) data;

#ifndef USE_LEGACY_SAT_AT_CMDS
    asprintf(&cmd, "AT+CUSATE=\"%s\"", ec);
    err = at_send_command_multiline(cmd, "+CUSATER:", &atresponse);
#else
    asprintf(&cmd, "AT*STKE=\"%s\"", ec);
    err = at_send_command_multiline(cmd, "*STKE:", &atresponse);
#endif
    free(cmd);

    if (err < 0 || atresponse->success == 0)
        goto error;

    if (atresponse->p_intermediates) {
        line = atresponse->p_intermediates->line;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextstr(&line, &stkResponse);
        if (err < 0)
            goto error;

        RIL_onRequestComplete(t, RIL_E_SUCCESS, stkResponse,
                              sizeof(char *));
    } else
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

    goto exit;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

exit:
    at_response_free(atresponse);
    return;
}

/**
 * RIL_REQUEST_STK_GET_PROFILE
 *
 * Requests the profile of SIM tool kit.
 * The profile indicates the SAT/USAT features supported by ME.
 * The SAT/USAT features refer to 3GPP TS 11.14 and 3GPP TS 31.111.
 */
void requestStkGetProfile(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    char *line = NULL;
    char *response = NULL;
    int err = 0;
    int skip = 0;

#ifndef USE_LEGACY_SAT_AT_CMDS
    err = at_send_command_singleline("AT+CUSATR=3", "+CUSATR:", &atresponse);
#else
    err = at_send_command_singleline("AT*STKC?", "*STKC:", &atresponse);
#endif
    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &skip);

    if (err < 0)
        goto error;

    err = at_tok_nextstr(&line, &response);
    if (err < 0 || response == NULL)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(char *));
    goto exit;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

exit:
    at_response_free(atresponse);
    return;
}

/**
 * RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING
 *
 * Turn on STK unsol commands.
 */
void requestReportStkServiceIsRunning(void *data, size_t datalen, RIL_Token t)
{
    char *cmd = NULL;
    int err;
    ATResponse *atresponse = NULL;

    /*
     * REFRESH proactive SAT command information
     *   onoff = 1 Enable REFRESH information reporting.
     * Android does not support handling of REFRESH as a proactive command.
     * *ESIMRF is a proprietary AT command to handle RIL_UNSOL_SIM_REFRESH.
     */
#ifndef USE_LEGACY_SAT_AT_CMDS
    err = at_send_command("AT*ESHLREF=1", &atresponse);
#else
    err = at_send_command("AT*ESIMRF=1", &atresponse);
#endif
    if (err < 0 || atresponse->success == 0)
        LOGE("%s(): Failed to enable REFRESH reporting!", __func__);
    at_response_free(atresponse);
    atresponse = NULL;

#ifndef USE_LEGACY_SAT_AT_CMDS
    /*
     * SET UP CALL proactive SAT command information
     *   onoff = 1 Enable SET UP CALL information reporting.
     * Android does not support handling of SET UP CALL as a proactive command.
     * *ESHLVOC is a proprietary AT command for passing the raw proactive
     * command to Android using RIL_UNSOL_STK_EVENT_NOTIFY.
     */
    err = at_send_command("AT*ESHLVOC=1", &atresponse);
    if (err < 0 || atresponse->success == 0)
        LOGE("%s(): Failed to enable high level SETUP CALL reporting!",
             __func__);
    at_response_free(atresponse);
    atresponse = NULL;

    /*
     * SEND SS proactive SAT command information
     *   onoff = 1 Enable SEND SS information reporting.
     * Android does not support handling of SEND SS as a proactive command.
     * *ESHLSS is a proprietary AT command for passing the raw proactive
     * command to Android using RIL_UNSOL_STK_EVENT_NOTIFY.
     */
    err = at_send_command("AT*ESHLSS=1", &atresponse);
    if (err < 0 || atresponse->success == 0)
        LOGE("%s(): Failed to enable high level SEND SS reporting!", __func__);
    at_response_free(atresponse);
    atresponse = NULL;

    /*
     * SEND USSD proactive SAT command information
     *   onoff = 1 Enable SEND UUSD reporting.
     * Android does not support handling of SEND USSD as a proactive command.
     * *ESHLUSS is a proprietary AT command for passing the raw proactive
     * command to Android using RIL_UNSOL_STK_EVENT_NOTIFY.
     */
    err = at_send_command("AT*ESHLUSS=1", &atresponse);
    if (err < 0 || atresponse->success == 0)
        LOGE("%s(): Failed to enable high level SEND USSD reporting!",
             __func__);
    at_response_free(atresponse);
    atresponse = NULL;

    /*
     * SEND DTMF proactive SAT command information
     *   onoff = 1 Enable SEND DTMF information reporting.
     * Android does not support handling of SEND DTMF as a proactive command.
     * *ESHLDTMF is a proprietary AT command for passing the raw proactive
     * command to Android using RIL_UNSOL_STK_EVENT_NOTIFY.
     */
    err = at_send_command("AT*ESHLDTMF=1", &atresponse);
    if (err < 0 || atresponse->success == 0)
        LOGE("%s(): Failed to enable high level SEND DTMF reporting!",
             __func__);
    at_response_free(atresponse);
    atresponse = NULL;

    /*
     * SEND SHORT MESSAGE proactive SAT command information
     *   onoff = 1 Enable SEND SHORT MESSAGE information reporting.
     * Android does not support handling of SEND SHORT MESSAGE as a
     * proactive command.
     * *ESHLSMS is a proprietary AT command for passing the raw proactive
     * command to Android using RIL_UNSOL_STK_EVENT_NOTIFY.
     */
    err = at_send_command("AT*ESHLSMS=1", &atresponse);
    if (err < 0 || atresponse->success == 0)
        LOGE("%s(): Failed to enable HIGH LEVEL SEND SHORT MESSAGE reporting!",
             __func__);
    at_response_free(atresponse);
    atresponse = NULL;
#endif

    /* Activate (U)SAT profile */
#ifndef USE_LEGACY_SAT_AT_CMDS
    asprintf(&cmd, "AT+CUSATA=1");
#else
    asprintf(&cmd, "AT*STKC=1,\"000000000000000000\"");
#endif
    err = at_send_command(cmd, &atresponse);
    free(cmd);
    if (err < 0 || atresponse->success == 0) {
        LOGE("%s(): Failed to activate (U)SAT profile", __func__);
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    goto exit;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

exit:
    at_response_free(atresponse);
    return;
}

/**
 * RIL_REQUEST_STK_SET_PROFILE
 *
 * Download the STK terminal profile as part of SIM initialization
 * procedure.
 */
void requestStkSetProfile(void *data, size_t datalen, RIL_Token t)
{
    char *cmd = NULL;
    int err;
    ATResponse *atresponse = NULL;
    const char *profile = (const char *) data;
#ifndef USE_LEGACY_SAT_AT_CMDS
    asprintf(&cmd, "AT+CUSATW=0,\"%s\"", profile);
#else
    asprintf(&cmd, "AT*STKC=0,\"%s\"", profile);
#endif
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
    return;

}

/**
 * RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM
 *
 * When STK application gets RIL_UNSOL_STK_CALL_SETUP, the call actually has
 * been initialized by ME already. (We could see the call has been in the 'call
 * list') So, STK application needs to accept/reject the call according as user
 * operations.
 */
void requestStkHandleCallSetupRequestedFromSIM(void *data,
                                               size_t datalen, RIL_Token t)
{
    char *cmd = NULL;
    int err;
    ATResponse *atresponse = NULL;
    int stkResponse = ((int *) data)[0];

    if (stkResponse < 0)
        goto error;

    /* Accept call for any value > 0, reject for 0 */
    asprintf(&cmd, "AT*ESHLVOCR=%d", (stkResponse ? 1 : 0));
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
    return;
}

/**
 * RIL_UNSOL_STK_PROACTIVE_COMMAND
 *
 * Indicate when SIM issue a STK proactive command to applications.
 *
 */
void onStkProactiveCommand(const char *s)
{
    char *str = NULL;
    char *line = NULL;
    char *tok = NULL;
    int err;

    tok = line = strdup(s);

    err = at_tok_start(&tok);
    if (err < 0)
        goto error;

    err = at_tok_nextstr(&tok, &str);
    if (err < 0)
        goto error;

    RIL_onUnsolicitedResponse(RIL_UNSOL_STK_PROACTIVE_COMMAND, str,
                              sizeof(char *));
    goto exit;

error:
    LOGE("%s failed to parse proactive command!", __func__);

exit:
    free(line);
    return;
}

/**
 * Any client using +CGLA must always use this function to
 * decide whether to obtain a new sessionid.
 */
int checkAndClear_SIM_NAA_SESSION_RESET(void)
{
    if (s_refeshStatus.cmdQualifier != SAT_NAA_SESSION_RESET)
        return 0;
    s_refeshStatus.cmdQualifier = -1;
    return 1;
}

/**
 * Send TERMINAL RESPONSE after processing REFRESH proactive command
 */
static void sendRefreshTerminalResponse(void *param)
{
    ATResponse *atresponse = NULL;
    char *cmd = NULL;
    int err;

    asprintf(&cmd, "AT*STKR=\"8103%02x01%02x820282818301%02x\"",
             s_refeshStatus.cmdNumber, s_refeshStatus.cmdQualifier,
             s_refeshStatus.Result);

    err = at_send_command(cmd, &atresponse);
    free(cmd);
    if (err < 0 || atresponse->success == 0)
        goto error;

   goto exit;

error:
    LOGE("%s failed!", __func__);

exit:
    at_response_free(atresponse);
    return;
}

/**
 * RIL_UNSOL_SIM_REFRESH
 *
 * Indicate when SIM issue a REFRESH proactive command to applications.
 */
void onStkSimRefresh(const char *s)
{
    int commas = 0;
    char *line = NULL;
    char *tok = NULL;
    int i, skip;
    int err = -1;
    int response[2];

    /*
     * Legacy: *ESIMRF: <cmdnumber>,<type>, [,<fileid>,<pathid>][,<fileid>…]
     * New:    *ESHLREF: <cmdnumber>,<mode>, [,<fileid>,<pathid>][,<fileid>…]
     */

    tok = line = strdup(s);

    err = at_tok_charcounter(tok, ',', &commas);
    if (err < 0)
        commas = 0;
    else
        commas -= 1;

    err = at_tok_start(&tok);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&tok, &(s_refeshStatus.cmdNumber));
    if (err < 0)
        goto error;

    err = at_tok_nextint(&tok, &(s_refeshStatus.cmdQualifier));
    if (err < 0)
        goto error;

    switch(s_refeshStatus.cmdQualifier) {
    case SAT_SIM_INITIALIZATION_AND_FULL_FILE_CHANGE_NOTIFICATION:
    case SAT_SIM_INITIALIZATION_AND_FILE_CHANGE_NOTIFICATION:
    case SAT_SIM_INITIALIZATION:
    case SAT_NAA_APPLICATION_RESET:
        /* SIM initialized.  All files should be re-read. */
        response[0] = SIM_INIT;
        s_refeshStatus.Result = 3; /* success, EFs read */
        break;
    case SAT_FILE_CHANGE_NOTIFICATION:
        /* one or more files on SIM has been updated */
        response[0] = SIM_FILE_UPDATE;
        s_refeshStatus.Result = 3; /* success, EFs read */
        break;
    case SAT_SIM_RESET:
        /* SIM reset. All files should be re-read. */
        response[0] = SIM_RESET;
        break;
    case SAT_NAA_SESSION_RESET:
        /* one or more files on SIM has been updated */
        response[0] = SIM_FILE_UPDATE;
        s_refeshStatus.Result = 3; /* success, EFs read */
        break;
    case SAT_STEERING_OF_ROAMING:
       /* Pass through. Not supported by Android, should never happen */
    default:
        goto error;
    }

    if (response[0] != SIM_FILE_UPDATE) {
        response[1] = 0;
        RIL_onUnsolicitedResponse(RIL_UNSOL_SIM_REFRESH,
                                  response, sizeof(response));
        goto exit;
    }

    for (i = 0; i < commas; i += 2) {
        err = at_tok_nextint(&tok, &(response[1]));
        if (err < 0) {
            /* check if response is already sent to Android */
            if (i > 0)
                goto exit;
            else
                goto error;
        }
        /* <pathid> is not used by Android */
        err = at_tok_nextint(&tok, &skip);
        if (err < 0) {
            /* check if response is already sent to Android */
            if (i > 0)
                goto exit;
            else
                goto error;
        }
        RIL_onUnsolicitedResponse(RIL_UNSOL_SIM_REFRESH,
                                  response, sizeof(response));
    }
    goto exit;

error:
    LOGE("%s: failed to parse %s, default to SIM_INITIALIZATION", __func__, s);
    if (s_refeshStatus.cmdNumber < 0)
        s_refeshStatus.cmdNumber = 1;
    if (s_refeshStatus.cmdQualifier < 0)
        s_refeshStatus.cmdQualifier = SAT_SIM_INITIALIZATION;
    if (s_refeshStatus.Result < 0)
        s_refeshStatus.Result = 2; /* command performed with missing info */
    response[0] = SIM_INIT;
    response[1] = 0;
    RIL_onUnsolicitedResponse(RIL_UNSOL_SIM_REFRESH,
                              response, sizeof(response));

exit:
    if (response[0] != SIM_RESET) {
        /* AT commands cannot be sent from the at reader thread */
        enqueueRILEvent(CMD_QUEUE_DEFAULT,
                        sendRefreshTerminalResponse, NULL, NULL);
    }
    free(line);
    return;
}

void onStkEventNotify(const char *s)
{
    char *str = NULL;
    char *line = NULL;
    char *tok = NULL;
    int err;

    tok = line = strdup(s);

    err = at_tok_start(&tok);
    if (err < 0)
        goto error;

    err = at_tok_nextstr(&tok, &str);
    if (err < 0)
        goto error;

    RIL_onUnsolicitedResponse(RIL_UNSOL_STK_EVENT_NOTIFY, str,
                              sizeof(char *));

    goto exit;

error:
    LOGW("Failed to parse STK Notify Event");

exit:
    free(line);
    return;
}
