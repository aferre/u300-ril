/* ST-Ericsson U300 RIL
 *
 * Copyright (C) ST-Ericsson AB 2008-2010
 * Copyright 2006, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Based on reference-ril by The Android Open Source Project.
 *
 * Heavily modified for ST-Ericsson U300 modems.
 * Author: Christian Bejram <christian.bejram@stericsson.com>
 */
#include <telephony/ril.h>
#include <string.h>

#include "atchannel.h"
#include "at_tok.h"
#include "u300-ril.h"
#include "u300-ril-audio.h"

#define LOG_TAG "RILV"
#include <utils/Log.h>

/*
 * Current code does not require a mutex on this static as no potential
 * multithread problem have be found.
 */
static int s_ttyMode = 0;

#ifdef ENABLE_REPORTING_ALERTING_UPON_MISSING_CALL_STATE_FROM_NETWORK
#include <stdbool.h>
static volatile bool g_voice_call_start = false;
bool getVoiceCallStartState()
{
    return g_voice_call_start;
}
#endif

/**
 * RIL_REQUEST_SET_TTY_MODE
 *
 * Ask the modem to set the TTY mode
 */
void requestSetTtyMode(void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *atresponse = NULL;
    int mode = ((int *) data)[0];

    /*
     * The modem supports one TTY mode where voice and TTY tones are
     * automatically detected. FULL (1), HCO (2) and VCO (3) are therefore
     * automatically handled by the modem TTY enabled mode (1).
     */
    err = at_send_command(mode?"AT*ETTY=1":"AT*ETTY=0", &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto error;

    s_ttyMode = mode;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_QUERY_TTY_MODE
 *
 * Requests the current TTY mode.
 */
void requestQueryTtyMode(void *data, size_t datalen, RIL_Token t)
{
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &s_ttyMode, sizeof(int));
}

/**
 * *EACE: Ringback tone received
 */
void onAudioCallEventNotify(const char *s)
{
    char *line;
    char *tok;
    int err;
    int res;

    tok = line = strdup(s);

    err = at_tok_start(&tok);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&tok, &res);
    if (err < 0)
        goto error;

    /*
     * *EACE:0/1 indicates stop/start of comfort tone.
     * *EACE:2 indicates voice call stopped.
     * *EACE:3 indicates voice call start, RIL uses this to inform Android
     * call state changed. This indicates call state has changed to ALERTING.
     */
    if (res == 3) {
#ifdef ENABLE_REPORTING_ALERTING_UPON_MISSING_CALL_STATE_FROM_NETWORK
        g_voice_call_start = true;
#endif
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
                                  NULL, 0);
        goto exit;
    } else if (res == 2) {
#ifdef ENABLE_REPORTING_ALERTING_UPON_MISSING_CALL_STATE_FROM_NETWORK
        g_voice_call_start = false;
#endif
        goto exit;
    }

    goto finally;

error:
    LOGE("EACE: Failed to parse %s.", s);
    /* Stop a potential ringback tone from going forever due to failed parsing*/
    res = 0;

finally:
    RIL_onUnsolicitedResponse(RIL_UNSOL_RINGBACK_TONE, &res, sizeof(int *));

exit:
    free(line);
    return;
}
