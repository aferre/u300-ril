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

#include "u300-ril-information.h"
#include "u300-ril-network.h"
#include "u300-ril.h"
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>

#define LOG_TAG "RILV"
#include <utils/Log.h>

#define RIL_IMEISV_VERSION  "02"

/**
 * RIL_REQUEST_GET_IMSI
 */
void requestGetIMSI(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int err;

    err = at_send_command_numeric("AT+CIMI", &atresponse);

    if (err < 0 || atresponse->success == 0)
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS,
                              atresponse->p_intermediates->line,
                              sizeof(char *));
    }
    at_response_free(atresponse);
    return;
}

/* RIL_REQUEST_DEVICE_IDENTITY
 *
 * Request the device ESN / MEID / IMEI / IMEISV.
 *
 */
void requestDeviceIdentity(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    char *response[4];
    int err;

    /* IMEI */
    err = at_send_command_numeric("AT+CGSN", &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto error;

    response[0] = atresponse->p_intermediates->line;

    /* IMEISV */
    response[1] = RIL_IMEISV_VERSION;

    /* CDMA not supported */
    response[2] = "";
    response[3] = "";

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/* Deprecated */
/**
 * RIL_REQUEST_GET_IMEI
 *
 * Get the device IMEI, including check digit.
 */
void requestGetIMEI(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int err;

    err = at_send_command_numeric("AT+CGSN", &atresponse);

    if (err < 0 || atresponse->success == 0)
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS,
                              atresponse->p_intermediates->line,
                              sizeof(char *));
    }
    at_response_free(atresponse);
    return;
}

/* Deprecated */
/**
 * RIL_REQUEST_GET_IMEISV
 *
 * Get the device IMEISV, which should be two decimal digits.
 */
void requestGetIMEISV(void *data, size_t datalen, RIL_Token t)
{
    char *response = NULL;

    response = RIL_IMEISV_VERSION;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(char *));
}

/**
 * RIL_REQUEST_RADIO_POWER
 *
 * Toggle radio on and off (for "airplane" mode).
 */
void requestRadioPower(void *data, size_t datalen, RIL_Token t)
{
    int onOff;
    int err;
    ATResponse *atresponse = NULL;

    if(datalen < sizeof(int))
        goto error;

    onOff = ((int *) data)[0];

    if (onOff == 0 && getCurrentState() != RADIO_STATE_OFF) {
        err = at_send_command("AT+CFUN=4", &atresponse);
        if (err < 0 || atresponse->success == 0)
            goto error;
        setRadioState(RADIO_STATE_OFF);
    } else if (onOff > 0 && getCurrentState() == RADIO_STATE_OFF) {
        err = at_send_command("AT+CFUN=99", &atresponse);
        if (err < 0 || atresponse->success == 0) {
            LOGW("AT+CFUN=99 failed, falling back to AT+CFUN=1");
            at_response_free(atresponse);
            err = at_send_command("AT+CFUN=1", &atresponse);
            if (err < 0 || atresponse->success == 0)
                goto error;
        }
        setRadioState(RADIO_STATE_SIM_NOT_READY);
    } else {
        LOGE("Erroneous input to requestRadioPower()!");
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

/**
 * Queries the signal strength and sends the signal strength
 * as Unsolicited response to Android.
 */
void pollAndDispatchSignalStrength(void *param)
{
    RIL_SignalStrength signalStrength;

    if (querySignalStrength(&signalStrength))
        RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH,
                                  &signalStrength, sizeof(RIL_SignalStrength));
}

void requestScreenState(void *data, size_t datalen, RIL_Token t)
{
    int screenState = 0;

    if(datalen < sizeof(int))
        goto error;

    getScreenStateLock();
    screenState = ((int *) data)[0];
    setScreenState(screenState);

    if (screenState == 1) {
        /* Screen is on - be sure to enable all unsolicited notifications. */
#ifdef LTE_COMMAND_SET_ENABLED
        if (at_send_command("AT+CEREG=2", NULL) < 0)
            goto error;
        if (at_send_command("AT+CREG=2", NULL) < 0)
#else
        if (at_send_command("AT*EREG=2", NULL) < 0)
#endif
            goto error;
        if (at_send_command("AT+CGREG=2", NULL) < 0)
            goto error;
        if (at_send_command("AT*EPSB=1", NULL) < 0)
            goto error;
        if (at_send_command("AT+CMER=3,0,0,1", NULL) < 0)
            goto error;
        /*
         * Android will not poll for update of signal strength after switch of
         * screen state, we need to poll to update screen signal strength bar.
         */
        enqueueRILEvent(CMD_QUEUE_AUXILIARY, pollAndDispatchSignalStrength,
                        NULL, NULL);
    } else if (screenState == 0) {
        /* Screen is off - disable all unsolicited notifications. */
#ifdef LTE_COMMAND_SET_ENABLED
        if (at_send_command("AT+CEREG=0", NULL) < 0)
            LOGI("Failed to disable CEREG notifications");
        if (at_send_command("AT+CREG=0", NULL) < 0)
            LOGI("Failed to disable CREG notifications");
#else
        if (at_send_command("AT*EREG=0", NULL) < 0)
            LOGI("Failed to disable EREG notifications");
#endif
        if (at_send_command("AT+CGREG=0", NULL) < 0)
            LOGI("Failed to disable CGREG notifications");
        if (at_send_command("AT*EPSB=0", NULL) < 0)
            LOGI("Failed to disable EPSB notifications");
        if (at_send_command("AT+CMER=3,0,0,0", NULL) < 0)
            LOGI("Failed to disable CMER notifications");
    } else
        /* Not a defined value - error. */
        goto error;

    releaseScreenStateLock();

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
    /* Trigger a rehash of network values, just to be sure. */
    if (screenState == 1)
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED, NULL, 0);

    return;

error:
    LOGE("ERROR: requestScreenState failed");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

    releaseScreenStateLock();
    goto finally;
}

/**
 * RIL_REQUEST_BASEBAND_VERSION
 *
 * Return string value indicating baseband version, eg
 * response from AT+CGMR.
 */
void requestBasebandVersion(void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *atresponse = NULL;
    ATLine *atline;
    char *line;

    /* TODO: Check if we really should pass an empty string here */
    err = at_send_command_multiline("AT+CGMR", "", &atresponse);

    if (err < 0 ||
            atresponse->success == 0 || atresponse->p_intermediates == NULL)
        goto error;

    /* When local echo is enabled, first line contains "AT+CGMR" echoed
     * back by the modem. This line needs to be skipped.
     */
    for (atline = atresponse->p_intermediates;
            atline->p_next; atline = atline->p_next) {
        LOGW("CGMR: Skipping local echo.");
    }

    line = atline->line;
    /* The returned value is used by Android in a system property.
     * The RIL should have no knowledge about this, but since Android
     * system properties only allow values with length < 90 and causes an
     * exception if the length of the returned string is > 90 this needs to
     * be checked here.
     * Todo:  Until Android implements limit handling on the string we need
     * to have a workaround in the RIL to chop the string.
     */
    if (strlen(line) > 90)
        line[90] = '\0';

    RIL_onRequestComplete(t, RIL_E_SUCCESS, line, sizeof(char *));

finally:
    at_response_free(atresponse);
    return;

error:
    LOGE("Error in requestBasebandVersion()");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

