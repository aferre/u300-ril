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
#include <stdbool.h>
#include <telephony/ril.h>

#include <assert.h>

#include "atchannel.h"
#include "at_tok.h"
#include "u300-ril.h"
#include "u300-ril-audio.h"

#define LOG_TAG "RILV"
#include <utils/Log.h>

enum ccstatus {
    IDLE = 0,
    CALLING_MO = 1,
    CONNECTING_MO = 2,
    ACTIVE = 3, /* connection between A and B */
    HOLD = 4,
    WAITING_MT = 5,
    ALERTING_MT = 6,
    BUSY = 7,
    RELEASED = 8,
    UNKNOWN = 10
};

enum clccNoCLI {
    CLCC_NOCLI_NOT_SET = -1,
    CLCC_NOCLI_UNKNOWN = 0,
    CLCC_NOCLI_RESTRICTED = 1,
    CLCC_NOCLI_OTHER_SERVICE = 2,
    CLCC_NOCLI_PAYPHONE = 3,
    CLCC_NOCLI_UNAVAILABLE = 4
};

enum clccState {
    CLCC_STATE_ACTIVE = 0,
    CLCC_STATE_HELD = 1,
    CLCC_STATE_DIALING = 2,
    CLCC_STATE_ALERTING = 3,
    CLCC_STATE_INCOMMING = 4,
    CLCC_STATE_WAITING = 5
};

/* Last call fail cause, obtained by *ECAV. */
static int s_lastCallFailCause = CALL_FAIL_ERROR_UNSPECIFIED;

static int clccStateToRILState(int state, RIL_CallState *p_state)
{
    switch (state) {
    case CLCC_STATE_ACTIVE:
        *p_state = RIL_CALL_ACTIVE;
        return 0;
    case CLCC_STATE_HELD:
        *p_state = RIL_CALL_HOLDING;
        return 0;
    case CLCC_STATE_DIALING:
        *p_state = RIL_CALL_DIALING;
#ifdef ENABLE_REPORTING_ALERTING_UPON_MISSING_CALL_STATE_FROM_NETWORK
        /*
         * Some networks will NOT return call status report for special numbers
         * (like 113 in Japan). In some cases the caller is supposed to use the
         * dial pad (DTMF tones). If Android only receives call state DIALING
         * and is not informed about call states ALERTING and/or ACTIVE it will
         * never show the dialpad.
         * *EACE: 3 informs about audio path is open. This information can be
         * used to fake the ALERTING state and enable dialpad/DTMF tones.
         * When the RIL receives *EACE:3 it will generate the
         * RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED command. Which will trigger
         * Android to send RIL_REQUEST_GET_CURRENT_CALLS. If there is a call in
         * state DIALING and *EACE:3 has been received it means that the state
         * should have bee ALERTING or ACTIVE. In this case the state for that
         * call is altered to ALERTING.
         */
        if (getVoiceCallStartState()) {
            *p_state = RIL_CALL_ALERTING;
        }
#endif
        return 0;
    case CLCC_STATE_ALERTING:
        *p_state = RIL_CALL_ALERTING;
        return 0;
    case CLCC_STATE_INCOMMING:
        *p_state = RIL_CALL_INCOMING;
        return 0;
    case CLCC_STATE_WAITING:
        *p_state = RIL_CALL_WAITING;
        return 0;
    default:
        return -1;
    }
}

static int clccCauseNoCLIToRILPres(int clccCauseNoCLI, int *rilPresentation)
{
    /*
     * Converting clccCauseNoCLI to RIL_numberPresentation
     * AT+CLCC cause_no_CLI     <> RIL number/name presentation
     * --------------------------------------------------------
     *-1=Parameter non existant <> 2=Not Specified/Unknown (Legacy CLCC
     *                                                      adaptation)
     * 0=Unknown                <> 2=Not Specified/Unknown (CLCC customization
     *                                                      adaptation)
     * 1=Restricted             <> 1=Restricted
     * 2=Other service          <> 2=Not Specified/Unknown
     * 3=Payphone               <> 3=Payphone
     * 4=Unavailable            <> 2=Not Specified/Unknown
     */
    switch (clccCauseNoCLI) {
    case CLCC_NOCLI_NOT_SET:
    case CLCC_NOCLI_UNKNOWN:
    case CLCC_NOCLI_UNAVAILABLE:
        *rilPresentation = 2;
        return 0;
    case CLCC_NOCLI_RESTRICTED:
    case CLCC_NOCLI_OTHER_SERVICE:
    case CLCC_NOCLI_PAYPHONE:
        *rilPresentation = clccCauseNoCLI;
        return 0;
    default: /* clccCauseNoCLIval -> unknown cause */
        *rilPresentation = 2;
        return -1;
    }
}

/**
 * Note: Directly modified line and has *p_call point directly into
 * modified line.
 * Returns -1bha if failed to decode line, 0 on success.
 */
static int callFromCLCCLine(char *line, RIL_Call *p_call)
{
    /*
     * +CLCC: index,isMT,state,mode,isMpty(,number,type(,alpha(,priority(,cause_of_no_cli))))
     * example of individual values +CLCC: 1,0,2,0,0,"+15161218005",145,"Hansen",0,1
     */
    int err;
    int state;
    int mode;
    int priority;
    int causeNoCLI = -1;
    int success = 0;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &(p_call->index));
    if (err < 0)
        goto error;

    err = at_tok_nextbool(&line, &(p_call->isMT));
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &state);
    if (err < 0)
        goto error;

    err = clccStateToRILState(state, &(p_call->state));
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &mode);
    if (err < 0)
        goto error;

    p_call->isVoice = (mode == 0);

    err = at_tok_nextbool(&line, &(p_call->isMpty));
    if (err < 0)
        goto error;

    if (at_tok_hasmore(&line)) { /* optional number and toa */
        err = at_tok_nextstr(&line, &(p_call->number)); /* accepting empty string */
        err = at_tok_nextint(&line, &p_call->toa);
        if (err < 0 && (p_call->number != NULL && strlen(p_call->number) > 0))
            goto error;

        if (at_tok_hasmore(&line)) { /* optional alphanummeric name */
            err = at_tok_nextstr(&line, &(p_call->name)); /* accepting empty string */

            if (at_tok_hasmore(&line)) { /* optional priority */
                err = at_tok_nextint(&line, &priority); /* accepting empty string */

                if (at_tok_hasmore(&line)) { /* optional cause_no_CLI */
                    err = at_tok_nextint(&line, &causeNoCLI);
                    if (err < 0)
                        goto error;
                }
            }
        }
    }

    /* If number exist it is "Allowed", no need to check cause of no CLI */
    if (p_call->number != NULL && strlen(p_call->number) > 0)
        p_call->numberPresentation = 0;
    else {
        err = clccCauseNoCLIToRILPres(causeNoCLI, &p_call->numberPresentation);
        if (err < 0)
            LOGE("%s(): cause of no CLI contained an unknown value, update "
                 "required?", __func__);
    }

    /*
     * Cause is mainly related to Number. Name comes from phonebook in modem.
     * Based on Name availability set namePresentation.
     */
    if ((p_call->name == NULL || strlen(p_call->name) == 0) &&
        p_call->numberPresentation == 0)
        p_call->namePresentation = 2;
    else
        p_call->namePresentation = p_call->numberPresentation;

finally:
    return success;

error:
    LOGE("%s: invalid CLCC line\n", __func__);
    success = -1;
    goto finally;
}

/**
 * Hangup MO call.
 * This is needed either when the remote end is BUSY
 * or something went wrong during outgoing call setup.
 */
static void hangupCall(void *param)
{
    ATResponse *atresponse = NULL;
    int err;

    err = at_send_command("ATH", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

finally:
    at_response_free(atresponse);
    return;

error:
    LOGE("hangupCall() failed");
    goto finally;
}

/**
 * AT*ECAV handler function.
 */
void onECAVReceived(const char *s)
{
    char *line;
    char *tok;
    int err;
    int res;
    int skip;
    int lastCallFailCause = CALL_FAIL_ERROR_UNSPECIFIED;

    tok = line = strdup(s);

    err = at_tok_start(&tok);
    if (err < 0)
        goto error;

    /* Read CID. Ignored - not needed */
    err = at_tok_nextint(&tok, &res);
    if (err < 0)
        goto error;

    /* Read ccstate. Saved for later */
    err = at_tok_nextint(&tok, &res);
    if (err < 0)
        goto error;

    /* If IDLE or RELEASED, further check why... */
    if (res == IDLE || res == RELEASED) {
        int rState;

         /* Read call type. Ignored - not needed */
        err = at_tok_nextint(&tok, &skip);
        if (err < 0)
            goto error;

        /* Read process id, Ignored - not needed */
        err = at_tok_nextint(&tok, &skip);
        if (err < 0)
            goto error;

        /* Read exit cause. Saved for later */
        err = at_tok_nextint(&tok, &lastCallFailCause);
        if (err < 0)
            goto error;

        /*
         * The STE modems support these additional proprietary exit cause
         * values:
         * 150 - Radio path not available
         * 151 - Access class barred
         * 160 - Illegal command
         * 161 - Collision
         * 222 - Failure not off hook
         * 255 - Empty cause
         *
         * Limit to the cause values standardized in 3GPP 24.008 Annex H since
         * Android does not support the proprietary values listed above.
         */
        if (lastCallFailCause > 127) {
            LOGD("%s(): Proprietary exit cause %d returned by modem, "
                 "replacing with CALL_FAIL_ERROR_UNSPECIFIED", __func__,
                 lastCallFailCause);
            lastCallFailCause = CALL_FAIL_ERROR_UNSPECIFIED;
        }

        /*
         * If restricted state ril.h specifies that we should return
         * unspecified error.
         */
        rState = getRestrictedState();
        if (rState == RIL_RESTRICTED_STATE_CS_EMERGENCY ||
            rState == RIL_RESTRICTED_STATE_CS_NORMAL ||
            rState == RIL_RESTRICTED_STATE_CS_ALL)
            lastCallFailCause = CALL_FAIL_ERROR_UNSPECIFIED;

        if (res == RELEASED) {
            /*
             * When receiving RELEASED state from AT it means that something has
             * gone wrong when trying to do a MO call. To notify Android about
             * this the RIL has to hang up the call to remove it from the call
             * list(AT+CLCC). End user will see an update in the screen and get
             * a comfort tone notification.
             */
            enqueueRILEvent(CMD_QUEUE_DEFAULT, hangupCall, NULL, NULL);
        }
    }
    else if (res == BUSY)
    {
        /*
         * When receiving RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED Android
         * will immediately send RIL_REQUEST_GET_CURRENT_CALLS to obtain
         * call state of the current call(s). It is not possible for the
         * RIL to report BUSY. Android detects BUSY when a call is removed
         * from the current call(s) list and the last call state for that
         * particular call was RIL_CALL_ALERTING.
         *
         * When *ECAV reports BUSY the RIL has to hangup the call,
         * otherwise it will take up to 20 s until it is taken down by the
         * network.
         */
        lastCallFailCause = CALL_FAIL_BUSY;
        enqueueRILEvent(CMD_QUEUE_AUXILIARY, hangupCall, NULL, NULL);
    }
    goto exit;

error:
    LOGE("ECAV: Failed to parse %s.", s);
    /* Reset lastCallFailCause */
    lastCallFailCause = CALL_FAIL_ERROR_UNSPECIFIED;

exit:
    free(line);
    s_lastCallFailCause = lastCallFailCause; /* Update static variable */

    /* Send the response even if we failed.. */
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED, NULL, 0);
    return;
}

/**
 * RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND
 *
 * Hang up waiting or held (like AT+CHLD=0)
 */
void requestHangupWaitingOrBackground(void *data, size_t datalen,
                                      RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int err;

    /*
     * 3GPP 22.030 6.5.5
     * "Releases all held calls or sets User Determined User Busy
     * (UDUB) for a waiting call."
     */
    err = at_send_command("AT+CHLD=0", &atresponse);
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
 * RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND
 *
 * Hang up waiting or held (like AT+CHLD=1)
 */
void requestHangupForegroundResumeBackground(void *data, size_t datalen,
                                             RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int err;

    /*
     * For some reason Android is using this RIL command to hangup emergency
     * call when SIM is absent. +CHLD=1 is a Supplementary Service call release
     * which is network dependent.
     * When SIM is locked or absent only emergency calls are allowed and the
     * RIL will use ATH to do hangup.
     */
    if (getCurrentState() == RADIO_STATE_SIM_LOCKED_OR_ABSENT) {
        err = at_send_command("ATH", &atresponse);
    } else {
        /*
         * 3GPP 22.030 6.5.5
         * "Releases all active calls (if any exist) and accepts
         * the other (held or waiting) call."
         */
        err = at_send_command("AT+CHLD=1", &atresponse);
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
 * RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE
 *
 * Switch waiting or holding call and active call (like AT+CHLD=2)
 */
void requestSwitchWaitingOrHoldingAndActive(void *data, size_t datalen,
                                            RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int err;

    /* 3GPP 22.030 6.5.5
       "Places all active calls (if any exist) on hold and accepts
       the other (held or waiting) call." */
    err = at_send_command("AT+CHLD=2", &atresponse);
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
 * RIL_REQUEST_CONFERENCE
 *
 * Conference holding and active (like AT+CHLD=3)
 */
void requestConference(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int err;

    /* 3GPP 22.030 6.5.5
       "Adds a held call to the conversation." */
    err = at_send_command("AT+CHLD=3", &atresponse);
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
 * RIL_REQUEST_SEPARATE_CONNECTION
 *
 * Separate a party from a multiparty call placing the multiparty call
 * (less the specified party) on hold and leaving the specified party
 * as the only other member of the current (active) call
 *
 * Like AT+CHLD=2x
 *
 * See TS 22.084 1.3.8.2 (iii)
 * TS 22.030 6.5.5 "Entering "2X followed by send"
 * TS 27.007 "AT+CHLD=2x"
 */
void requestSeparateConnection(void *data, size_t datalen, RIL_Token t)
{
    char* cmd = NULL;
    int party = ((int *) data)[0];
    int err;
    ATResponse *atresponse = NULL;

    asprintf(&cmd, "AT+CHLD=2%d", party);
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
 * RIL_REQUEST_EXPLICIT_CALL_TRANSFER
 *
 * Connects the two calls and disconnects the subscriber from both calls.
 */
void requestExplicitCallTransfer(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int err;

    /* 3GPP TS 22.091
       Connects the two calls and disconnects the subscriber from both calls. */
    err = at_send_command("AT+CHLD=4", &atresponse);
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
 * RIL_REQUEST_UDUB
 *
 * Comment in ril.h says:
 *
 * * Send UDUB (user determined used busy) to ringing or
 * * waiting call answer (RIL_BasicRequest r).
 * *
 * * "data" is NULL
 * * "response" is NULL
 *
 * Note, however, that RIL_BasicRequest does not exist. We assume that
 * response should be NULL, and ignore RIL_BasicRequest.
 */
void requestUDUB(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int err;

    /*
     * 3GPP 22.030 6.5.5
     * "Releases all held calls or sets User Determined User Busy
     * (UDUB) for a waiting call."
     */
    err = at_send_command("AT+CHLD=0", &atresponse);
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
 * RIL_REQUEST_SET_MUTE
 *
 * Turn on or off uplink (microphone) mute.
 *
 * Will only be sent while voice call is active.
 * Will always be reset to "disable mute" when a new voice call is initiated.
 */
void requestSetMute(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int mute = ((int *) data)[0];
    int err = 0;

    if (mute!=0 && mute!=1)
        goto error;

    err = at_send_command(mute?"AT+CMUT=1":"AT+CMUT=0", &atresponse);
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
 * RIL_REQUEST_GET_MUTE
 *
 * Queries the current state of the uplink mute setting.
 */
void requestGetMute(void *data, size_t datalen, RIL_Token t)
{
    char *line = NULL;
    int err = 0;
    int response = 0;
    ATResponse *atresponse;

    err = at_send_command_singleline("AT+CMUT?", "+CMUT:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
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
 * RIL_REQUEST_LAST_CALL_FAIL_CAUSE
 *
 * Requests the failure cause code for the most recently terminated call.
 *
 * See also: RIL_REQUEST_LAST_PDP_FAIL_CAUSE
 */
void requestLastCallFailCause(void *data, size_t datalen, RIL_Token t)
{
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &s_lastCallFailCause,
                          sizeof(int));
}

/**
 * RIL_REQUEST_GET_CURRENT_CALLS
 *
 * Requests current call list.
 */
void requestGetCurrentCalls(void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *atresponse = NULL;
    ATLine *cursor;
    int countCalls;
    int countValidCalls = 0;
    RIL_Call *calls = NULL;
    RIL_Call **response = NULL;
    RIL_Call *call = NULL;
    int i;

    err = at_send_command_multiline("AT+CLCC", "+CLCC:", &atresponse);

    if (err != 0 || atresponse->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    /* Count the calls. */
    for (countCalls = 0, cursor = atresponse->p_intermediates;
         cursor != NULL; cursor = cursor->p_next)
        countCalls++;

    if (countCalls == 0)
        goto exit;

    /* Yes, there's an array of pointers and then an array of structures. */
    response = (RIL_Call **) alloca(countCalls * sizeof(RIL_Call *));
    calls = (RIL_Call *) alloca(countCalls * sizeof(RIL_Call));

    /* Init the pointer array. */
    for (i = 0; i < countCalls; i++)
        response[i] = &(calls[i]);

    for (countValidCalls = 0, cursor = atresponse->p_intermediates;
         cursor != NULL; cursor = cursor->p_next) {
        call = calls + countValidCalls;
        memset(call, 0, sizeof(*call));
        call->number  = NULL;
        call->name    = NULL;
        call->uusInfo = NULL;

        err = callFromCLCCLine(cursor->line, call);
        if (err != 0)
            continue;

        countValidCalls++;
    }

exit:
    RIL_onRequestComplete(t, RIL_E_SUCCESS, response,
                          countValidCalls * sizeof(RIL_Call *));

    at_response_free(atresponse);
    return;
}

/**
 * Returns false if FDN is not active, not available or failed to get result
 * for AT+CLCK,true if FDN is enabled
 */
static bool isFdnEnabled(void)
{
    int err = -1;
    int status = 0;
    bool res = false;
    char *line = NULL;
    ATResponse *atresponse = NULL;

    err = at_send_command_multiline("AT+CLCK=\"FD\",2", "+CLCK:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &status);
    if (err < 0 )
        goto error;

    /* status = 1 means that FDN facility is active */
    if (status == 1)
        res = true;

    goto exit;
error:
    LOGE("%s(): Failed to parse facility check response.\n", __func__);
exit:
    at_response_free(atresponse);
    return res;
}

/**
 * RIL_REQUEST_DIAL
 *
 * Initiate voice call.
 */
void requestDial(void *data, size_t datalen, RIL_Token t)
{
    const RIL_Dial *dial;
    ATResponse *atresponse = NULL;
    char *cmd;
    const char *clir;
    int err;
    ATCmeError cme_error_code;

    dial = (const RIL_Dial *) data;

    switch (dial->clir) {
    case 1:
        clir = "I";
        break;                  /* Invocation */
    case 2:
        clir = "i";
        break;                  /* Suppression */
    default:
    case 0:
        clir = "";
        break;                  /* Subscription default */
    }

    asprintf(&cmd, "ATD%s%s;", dial->address, clir);

    err = at_send_command(cmd, &atresponse);

    free(cmd);

    if (err < 0)
        goto error;

    if (atresponse->success == 0) {
        if (at_get_cme_error(atresponse, &cme_error_code)) {
            /*
             * Android will ask for last call fail cause even if
             * RIL_REQUEST_DIAL returns GENERIC_FAILURE. If pre-dial check
             * has failed and FDN is enabled we conclude that the reason
             * for failed pre-dial check is that the number is not in the
             * FDN list.
             */
            if (cme_error_code == CME_PRE_DIAL_CHECK_ERROR && isFdnEnabled())
                s_lastCallFailCause = CALL_FAIL_FDN_BLOCKED;
            else
                s_lastCallFailCause = CALL_FAIL_ERROR_UNSPECIFIED;
        } else {
            s_lastCallFailCause = CALL_FAIL_ERROR_UNSPECIFIED;
        }
        goto error;
    }

    /*
     * Success or failure is ignored by the upper layer here,
     * it will call GET_CURRENT_CALLS and determine success that way.
     */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    goto exit;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

exit:
    at_response_free(atresponse);
    return;
}

/**
 * RIL_REQUEST_ANSWER
 *
 * Answer incoming call.
 *
 * Will not be called for WAITING calls.
 * RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE will be used in this case
 * instead.
 */
void requestAnswer(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int err;

    err = at_send_command("ATA", &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto error;

    /* Success or failure is ignored by the upper layer here,
       it will call GET_CURRENT_CALLS and determine success that way. */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_HANGUP
 *
 * Hang up a specific line (like AT+CHLD=1x).
 */
void requestHangup(void *data, size_t datalen, RIL_Token t)
{
    int cid;
    char *cmd = NULL;
    ATLine *cursor;
    int err;
    int i;
    int countCalls;
    ATResponse *atresponse = NULL;
    RIL_Call *calls = NULL;
    RIL_Call *call = NULL;

    assert(data != NULL);

    cid = ((int *) data)[0];

    /*
     * Until we get some silver bullet AT-command that will kill whatever
     * call we have, we need to check what state we're in and act accordingly.
     *
     * TODO: Refactor this and merge with the other query to CLCC.
     */
    err = at_send_command_multiline("AT+CLCC", "+CLCC:", &atresponse);

    if (err != 0 || atresponse->success == 0) {
        goto error;
    }

    /* Count the calls. */
    for (countCalls = 0, cursor = atresponse->p_intermediates;
         cursor != NULL; cursor = cursor->p_next)
        countCalls++;

    if (countCalls <= 0)
        goto error;

    calls = (RIL_Call *) alloca(countCalls * sizeof(RIL_Call));

    for (i = 0, cursor = atresponse->p_intermediates; cursor != NULL;
         cursor = cursor->p_next) {
        call = calls + i;
        memset(call, 0, sizeof(*call));

        err = callFromCLCCLine(cursor->line, call);
        if (err != 0)
            continue;

        if (calls[i].index == cid)
            break;

        i++;
    }

    at_response_free(atresponse);
    atresponse = NULL;

    /* We didn't find the call. Just drop the request and let android decide. */
    if (calls[i].index != cid)
        goto error;

    if (calls[i].state == RIL_CALL_DIALING ||
        calls[i].state == RIL_CALL_ALERTING)
        asprintf(&cmd, "ATH");
    else
        /* 3GPP 22.030 6.5.5
           "Releases a specific active call X" */
        asprintf(&cmd, "AT+CHLD=1%d", cid);

    err = at_send_command(cmd, &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
    if (cmd)
        free(cmd);
    at_response_free(atresponse);
    return;

error:
    /* Success or failure is ignored by the upper layer here,
       it will call GET_CURRENT_CALLS and determine success that way. */
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_DTMF
 *
 * Send a DTMF tone
 *
 * If the implementation is currently playing a tone requested via
 * RIL_REQUEST_DTMF_START, that tone should be cancelled and the new tone
 * should be played instead.
 */
void requestDTMF(void *data, size_t datalen, RIL_Token t)
{
    char c = *((char *) data);
    char *cmd = NULL;
    ATResponse *atresponse = NULL;
    int err = -1;

    if (!((c>='0' && c<='9') || c=='*' || c=='#'))
        goto error;

    /* Set duration to default (manufacturer specific, 70ms in our case). */
    err = at_send_command("AT+VTD=0", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    at_response_free(atresponse);
    atresponse = NULL;

    asprintf(&cmd, "AT+VTS=%c", c);
    err = at_send_command(cmd, &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
    if (cmd != NULL)
        free(cmd);
    at_response_free(atresponse);

    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_DTMF_START
 *
 * Start playing a DTMF tone. Continue playing DTMF tone until
 * RIL_REQUEST_DTMF_STOP is received .
 *
 * If a RIL_REQUEST_DTMF_START is received while a tone is currently playing,
 * it should cancel the previous tone and play the new one.
 *
 * See also: RIL_REQUEST_DTMF, RIL_REQUEST_DTMF_STOP.
 */
void requestDTMFStart(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    char c = *((char *) data);
    char *cmd = NULL;
    int err = 0;

    /* Set duration to maximum, 10000000  n/10 ms = 10000s. */
    err = at_send_command("AT+VTD=10000000", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    at_response_free(atresponse);
    atresponse = NULL;

    /* Start the DTMF tone. */
    asprintf(&cmd, "AT+VTS=%c", c);
    err = at_send_command(cmd, &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
    if (cmd != NULL)
        free(cmd);

    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_DTMF_STOP
 *
 * Stop playing a currently playing DTMF tone.
 *
 * See also: RIL_REQUEST_DTMF, RIL_REQUEST_DTMF_START.
 */
void requestDTMFStop(void *data, size_t datalen, RIL_Token t)
{
    int err = 0;
    ATResponse *atresponse = NULL;

    err = at_send_command("AT+VTD=0", &atresponse);
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
