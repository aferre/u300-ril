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
** Author: Kim Tommy Humborstad <kim.tommy.humborstad@stericsson.com>
*/

#include <telephony/ril.h>
#include <cutils/properties.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#include "atchannel.h"
#include "at_tok.h"
#include "fcp_parser.h"
#include "u300-ril.h"
#include "u300-ril-sim.h"
#include "u300-ril-network.h"
#include "misc.h"

#define LOG_TAG "RILV"
#include <utils/Log.h>

/*
 * The following SIM_Status list consist of indexes to combine the result
 * string of 3GPP AT command “AT+CPIN?” (ST-Ericsson version) with RIL API
 * "RIL_AppStatus" structure. To fill this structure the SIM_Status value is
 * matched to an entry in the static app_status_array[] below.
 */
typedef enum {
    SIM_ABSENT = 0,                     /* SIM card is not inserted */
    SIM_NOT_READY = 1,                  /* SIM card is not ready */
    SIM_READY = 2,                      /* radiostate = RADIO_STATE_SIM_READY */
    SIM_PIN = 3,                        /* SIM PIN code lock */
    SIM_PUK = 4,                        /* SIM PUK code lock */
    SIM_NETWORK_PERSO = 5,              /* Network Personalization lock */
    SIM_PIN2 = 6,                       /* SIM PIN2 lock */
    SIM_PUK2 = 7,                       /* SIM PUK2 lock */
    SIM_NETWORK_SUBSET_PERSO = 8,       /* Network Subset Personalization */
    SIM_SERVICE_PROVIDER_PERSO = 9,     /* Service Provider Personalization */
    SIM_CORPORATE_PERSO = 10,           /* Corporate Personalization */
    SIM_SIM_PERSO = 11,                 /* SIM/USIM Personalization */
    SIM_STERICSSON_LOCK = 12,           /* ST-Ericsson Extended SIM */
    SIM_BLOCKED = 13,                   /* SIM card is blocked */
    SIM_PERM_BLOCKED = 14,              /* SIM card is permanently blocked */
    SIM_NETWORK_PERSO_PUK = 15,         /* Network Personalization PUK */
    SIM_NETWORK_SUBSET_PERSO_PUK = 16,  /* Network Subset Perso. PUK */
    SIM_SERVICE_PROVIDER_PERSO_PUK = 17,/* Service Provider Perso. PUK */
    SIM_CORPORATE_PERSO_PUK = 18,       /* Corporate Personalization PUK */
    SIM_SIM_PERSO_PUK = 19,             /* SIM Personalization PUK (unused) */
    SIM_PUK2_PERM_BLOCKED = 20          /* PUK2 is permanently blocked */
} SIM_Status;

/*
 * The following list contains values for the structure "RIL_AppStatus" to be
 * sent to Android on a given SIM state. It is indexed by the SIM_Status above.
 */
static const RIL_AppStatus app_status_array[] = {
    /*
     * RIL_AppType,  RIL_AppState,
     * RIL_PersoSubstate,
     * Aid pointer, App Label pointer, PIN1 replaced,
     * RIL_PinState (PIN1),
     * RIL_PinState (PIN2)
     */
    /* SIM_ABSENT = 0 */
    {
        RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN,
        RIL_PERSOSUBSTATE_UNKNOWN,
        NULL, NULL, 0,
        RIL_PINSTATE_UNKNOWN,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_NOT_READY = 1 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_DETECTED,
        RIL_PERSOSUBSTATE_UNKNOWN,
        NULL, NULL, 0,
        RIL_PINSTATE_UNKNOWN,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_READY = 2 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_READY,
        RIL_PERSOSUBSTATE_READY,
        NULL, NULL, 0,
        RIL_PINSTATE_UNKNOWN,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_PIN = 3 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_PIN,
        RIL_PERSOSUBSTATE_UNKNOWN,
        NULL, NULL, 0,
        RIL_PINSTATE_ENABLED_NOT_VERIFIED,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_PUK = 4 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_PUK,
        RIL_PERSOSUBSTATE_UNKNOWN,
        NULL, NULL, 0,
        RIL_PINSTATE_ENABLED_BLOCKED,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_NETWORK_PERSO = 5 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
        RIL_PERSOSUBSTATE_SIM_NETWORK,
        NULL, NULL, 0,
        RIL_PINSTATE_ENABLED_NOT_VERIFIED,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_PIN2 = 6 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_READY,
        RIL_PERSOSUBSTATE_UNKNOWN,
        NULL, NULL, 0,
        RIL_PINSTATE_UNKNOWN,
        RIL_PINSTATE_ENABLED_NOT_VERIFIED
    },
    /* SIM_PUK2 = 7 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_READY,
        RIL_PERSOSUBSTATE_UNKNOWN,
        NULL, NULL, 0,
        RIL_PINSTATE_UNKNOWN,
        RIL_PINSTATE_ENABLED_BLOCKED
    },
    /* SIM_NETWORK_SUBSET_PERSO = 8 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
        RIL_PERSOSUBSTATE_SIM_NETWORK_SUBSET,
        NULL, NULL, 0,
        RIL_PINSTATE_ENABLED_NOT_VERIFIED,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_SERVICE_PROVIDER_PERSO = 9 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
        RIL_PERSOSUBSTATE_SIM_SERVICE_PROVIDER,
        NULL, NULL, 0,
        RIL_PINSTATE_ENABLED_NOT_VERIFIED,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_CORPORATE_PERSO = 10 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
        RIL_PERSOSUBSTATE_SIM_CORPORATE,
        NULL, NULL, 0,
        RIL_PINSTATE_ENABLED_NOT_VERIFIED,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_SIM_PERSO = 11 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
        RIL_PERSOSUBSTATE_SIM_SIM,
        NULL, NULL, 0,
        RIL_PINSTATE_ENABLED_NOT_VERIFIED,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_STERICSSON_LOCK = 12 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
        RIL_PERSOSUBSTATE_UNKNOWN,    /* API (ril.h) does not have this lock */
        NULL, NULL, 0,
        RIL_PINSTATE_ENABLED_NOT_VERIFIED,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_BLOCKED = 13 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_UNKNOWN,
        RIL_PERSOSUBSTATE_UNKNOWN,
        NULL, NULL, 0,
        RIL_PINSTATE_ENABLED_BLOCKED,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_PERM_BLOCKED = 14 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_UNKNOWN,
        RIL_PERSOSUBSTATE_UNKNOWN,
        NULL, NULL, 0,
        RIL_PINSTATE_ENABLED_PERM_BLOCKED,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_NETWORK_PERSO_PUK = 15 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
        RIL_PERSOSUBSTATE_SIM_NETWORK_PUK,
        NULL, NULL, 0,
        RIL_PINSTATE_ENABLED_NOT_VERIFIED,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_NETWORK_SUBSET_PERSO_PUK = 16 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
        RIL_PERSOSUBSTATE_SIM_NETWORK_SUBSET_PUK,
        NULL, NULL, 0,
        RIL_PINSTATE_ENABLED_NOT_VERIFIED,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_SERVICE_PROVIDER_PERSO_PUK = 17 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
        RIL_PERSOSUBSTATE_SIM_SERVICE_PROVIDER_PUK,
        NULL, NULL, 0,
        RIL_PINSTATE_ENABLED_NOT_VERIFIED,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_CORPORATE_PERSO_PUK = 18 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
        RIL_PERSOSUBSTATE_SIM_CORPORATE_PUK,
        NULL, NULL, 0,
        RIL_PINSTATE_ENABLED_NOT_VERIFIED,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_SIM_PERSO_PUK = 19 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
        RIL_PERSOSUBSTATE_SIM_SIM_PUK,
        NULL, NULL, 0,
        RIL_PINSTATE_ENABLED_NOT_VERIFIED,
        RIL_PINSTATE_UNKNOWN
    },
    /* SIM_PUK2_PERM_BLOCKED = 20 */
    {
        RIL_APPTYPE_SIM, RIL_APPSTATE_UNKNOWN,
        RIL_PERSOSUBSTATE_UNKNOWN,
        NULL, NULL, 0,
        RIL_PINSTATE_UNKNOWN,
        RIL_PINSTATE_ENABLED_PERM_BLOCKED
    }
};

enum PIN_PUK_Verification {
    SIM_PIN_VERIFICATION = 1,
    SIM_PIN2_VERIFICATION = 2,
    SIM_PUK_VERIFICATION = 3,
    SIM_PUK2_VERIFICATION = 4
};

typedef enum {
    UICC_TYPE_UNKNOWN,
    UICC_TYPE_SIM,
    UICC_TYPE_USIM,
} UICC_Type;

static const struct timeval TIMEVAL_SIMPOLL = { 1, 0 };
static const struct timeval TIMEVAL_SIMRESET = { 60, 0 };

#ifdef USE_EXT1_INSTEAD_OF_EXT5_WHEN_SIM_CARD_IS_2G_TYPE
#define FILE_ID_EF_EXT1 0x6F4A
#define FILE_ID_EF_EXT5 0x6F4E
#define PATH_DF_TELECOM_DIRECTORY "3F007FFF"
#endif
#define PATH_ADF_USIM_DIRECTORY   "3F007F10"

/* All files listed under ADF_USIM in 3GPP TS 31.102 */
static const int ef_usim_files[] = {
    0x6F05, 0x6F06, 0x6F07, 0x6F08, 0x6F09,
    0x6F2C, 0x6F31, 0x6F32, 0x6F37, 0x6F38,
    0x6F39, 0x6F3B, 0x6F3C, 0x6F3E, 0x6F3F,
    0x6F40, 0x6F41, 0x6F42, 0x6F43, 0x6F45,
    0x6F46, 0x6F47, 0x6F48, 0x6F49, 0x6F4B,
    0x6F4C, 0x6F4D, 0x6F4E, 0x6F4F, 0x6F50,
    0x6F55, 0x6F56, 0x6F57, 0x6F58, 0x6F5B,
    0x6F5C, 0x6F60, 0x6F61, 0x6F62, 0x6F73,
    0x6F78, 0x6F7B, 0x6F7E, 0x6F80, 0x6F81,
    0x6F82, 0x6F83, 0x6FAD, 0x6FB1, 0x6FB2,
    0x6FB3, 0x6FB4, 0x6FB5, 0x6FB6, 0x6FB7,
    0x6FC3, 0x6FC4, 0x6FC5, 0x6FC6, 0x6FC7,
    0x6FC8, 0x6FC9, 0x6FCA, 0x6FCB, 0x6FCC,
    0x6FCD, 0x6FCE, 0x6FCF, 0x6FD0, 0x6FD1,
    0x6FD2, 0x6FD3, 0x6FD4, 0x6FD5, 0x6FD6,
    0x6FD7, 0x6FD8, 0x6FD9, 0x6FDA, 0x6FDB,
};

/* Returns true if SIM is absent */
bool isSimAbsent()
{
    ATResponse *atresponse = NULL;
    int err;
    ATCmeError cme_error_code;
    bool simAbsent = true;

    err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &atresponse);

    if (err < 0 || atresponse == NULL) {
        LOGE("%s(): failed to get SIM status", __func__);
        goto exit;
    }

    if (atresponse->success == 1)
        simAbsent = false;
    else if (at_get_cme_error(atresponse, &cme_error_code))
        if (cme_error_code != CME_SIM_NOT_INSERTED)
            simAbsent = false;

    at_response_free(atresponse);

exit:
    return simAbsent;
}

static void resetSim(void *param)
{
    ATResponse *atresponse = NULL;
    int err, state, skip;
    char *line = NULL;

    err =
        at_send_command_singleline("AT*ESIMSR?", "*ESIMSR:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &skip);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &state);
    if (err < 0)
        goto error;

    if (state == 7) {
        (void)at_send_command("AT*ESIMR", NULL);

        enqueueRILEvent(CMD_QUEUE_DEFAULT, resetSim, NULL,
                        &TIMEVAL_SIMRESET);
    } else {
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                                  NULL, 0);
        pollSIMState(NULL);
    }

finally:
    at_response_free(atresponse);
    return;

error:
    goto finally;
}

void onSimStateChanged(const char *s)
{
    int err, state;
    char *tok = NULL;
    char *line = tok = strdup(s);
    assert(tok != NULL);

    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL,
                              0);

    /* Also check sim state, that will trigger radio state to sim absent. */
    enqueueRILEvent(CMD_QUEUE_DEFAULT, pollSIMState, (void *) 1, NULL);

    /*
     * Now, find out if we went to poweroff-state. If so, enqueue some loop
     * to try to reset the SIM for a minute or so to try to recover.
     */
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &state);
    if (err < 0)
        goto error;

    if (state == 7)
        enqueueRILEvent(CMD_QUEUE_DEFAULT, resetSim, NULL, NULL);

finally:
    free(tok);
    return;

error:
    LOGE("ERROR in onSimStateChanged!");
    goto finally;
}

/**
 * Get the number of retries left for pin functions
 */
static int getNumRetries(int request)
{
    ATResponse *atresponse = NULL;
    int err = 0;
    char *cmd = NULL;
    char *line = NULL;
    int num_retries = -1;
    int type;

    switch (request) {
    case RIL_REQUEST_ENTER_SIM_PIN:
    case RIL_REQUEST_CHANGE_SIM_PIN:
        type = SIM_PIN_VERIFICATION;
        break;
    case RIL_REQUEST_ENTER_SIM_PIN2:
    case RIL_REQUEST_CHANGE_SIM_PIN2:
        type = SIM_PIN2_VERIFICATION;
        break;
    case RIL_REQUEST_ENTER_SIM_PUK:
        type = SIM_PUK_VERIFICATION;
        break;
    case RIL_REQUEST_ENTER_SIM_PUK2:
        type = SIM_PUK2_VERIFICATION;
        break;
    default:
        num_retries = -1;
        LOGE("%s(): Unknown request type", __func__);
        goto exit;
    }

    asprintf(&cmd, "AT*EPINR=%d", type);
    err = at_send_command_singleline(cmd, "*EPINR:", &atresponse);
    free(cmd);
    if (err < 0 || atresponse->success == 0) {
        LOGE("%s(): AT*EPINR error", __func__);
        goto exit;
    }

    line = atresponse->p_intermediates->line;
    sscanf(line, "*EPINR: %d", &num_retries);

exit:
    at_response_free(atresponse);
    return num_retries;
}

/** Returns one of SIM_*. Returns SIM_NOT_READY on error. */
static SIM_Status getSIMStatus()
{
    ATResponse *atresponse = NULL;
    SIM_Status ret = SIM_ABSENT;
    char *cpinLine = NULL;
    char *cpinResult = NULL;
    ATCmeError cme_error_code;

    if (getCurrentState() == RADIO_STATE_OFF ||
        getCurrentState() == RADIO_STATE_UNAVAILABLE) {
        ret = SIM_NOT_READY;
        goto exit;
    }

    if (at_send_command_singleline("AT+CPIN?", "+CPIN:", &atresponse) != 0) {
        ret = SIM_NOT_READY;
        goto exit;
    }

    if (atresponse->success == 0) {
        if (at_get_cme_error(atresponse, &cme_error_code)) {
            switch (cme_error_code) {
            case CME_SIM_NOT_INSERTED:
                ret = SIM_ABSENT;
                break;
            case CME_SIM_PIN_REQUIRED:
                ret = SIM_PIN;
                break;
            case CME_SIM_PUK_REQUIRED:
                ret = SIM_PUK;
                break;
            case CME_SIM_PIN2_REQUIRED:
                ret = SIM_PIN2;
                break;
            case CME_SIM_PUK2_REQUIRED:
                ret = SIM_PUK2;
                break;
            case CME_NETWORK_PERSONALIZATION_PIN_REQUIRED:
                ret = SIM_NETWORK_PERSO;
                break;
            case CME_NETWORK_PERSONALIZATION_PUK_REQUIRED:
                ret = SIM_NETWORK_PERSO_PUK;
                break;
            case CME_NETWORK_SUBSET_PERSONALIZATION_PIN_REQUIRED:
                ret = SIM_NETWORK_SUBSET_PERSO;
                break;
            case CME_NETWORK_SUBSET_PERSONALIZATION_PUK_REQUIRED:
                ret = SIM_NETWORK_SUBSET_PERSO_PUK;
                break;
            case CME_SERVICE_PROVIDER_PERSONALIZATION_PIN_REQUIRED:
                ret = SIM_SERVICE_PROVIDER_PERSO;
                break;
            case CME_SERVICE_PROVIDER_PERSONALIZATION_PUK_REQUIRED:
                ret = SIM_SERVICE_PROVIDER_PERSO_PUK;
                break;
            case CME_PH_SIMLOCK_PIN_REQUIRED: /* PUK not in use by modem */
                ret = SIM_SIM_PERSO;
                break;
            case CME_CORPORATE_PERSONALIZATION_PIN_REQUIRED:
                ret = SIM_CORPORATE_PERSO;
                break;
            case CME_CORPORATE_PERSONALIZATION_PUK_REQUIRED:
                ret = SIM_CORPORATE_PERSO_PUK;
                break;
            default:
                ret = SIM_NOT_READY;
                break;
            }
        }
        goto exit;
    }

    /* CPIN? has succeeded, now look at the result. */
    cpinLine = atresponse->p_intermediates->line;

    if (at_tok_start(&cpinLine) < 0) {
        ret = SIM_NOT_READY;
        goto exit;
    }

    if (at_tok_nextstr(&cpinLine, &cpinResult) < 0) {
        ret = SIM_NOT_READY;
        goto exit;
    }

    if (0 == strcmp(cpinResult, "READY")) {
        ret = SIM_READY;
    } else if (0 == strcmp(cpinResult, "SIM PIN")) {
        ret = SIM_PIN;
    } else if (0 == strcmp(cpinResult, "SIM PUK")) {
        ret = SIM_PUK;
    } else if (0 == strcmp(cpinResult, "SIM PIN2")) {
        ret = SIM_PIN2;
    } else if (0 == strcmp(cpinResult, "SIM PUK2")) {
        ret = SIM_PUK2;
    } else if (0 == strcmp(cpinResult, "PH-NET PIN")) {
        ret = SIM_NETWORK_PERSO;
    } else if (0 == strcmp(cpinResult, "PH-NETSUB PIN")) {
        ret = SIM_NETWORK_SUBSET_PERSO;
    } else if (0 == strcmp(cpinResult, "PH-SP PIN")) {
        ret = SIM_SERVICE_PROVIDER_PERSO;
    } else if (0 == strcmp(cpinResult, "PH-CORP PIN")) {
        ret = SIM_CORPORATE_PERSO;
    } else if (0 == strcmp(cpinResult, "PH-SIMLOCK PIN")) {
        ret = SIM_SIM_PERSO;
    } else if (0 == strcmp(cpinResult, "PH-ESL PIN")) {
        ret = SIM_STERICSSON_LOCK;
    } else if (0 == strcmp(cpinResult, "BLOCKED")) {
        int numRetries = getNumRetries(RIL_REQUEST_ENTER_SIM_PUK);
        if (numRetries == -1 || numRetries == 0)
            ret = SIM_PERM_BLOCKED;
        else
            ret = SIM_PUK2_PERM_BLOCKED;
    } else if (0 == strcmp(cpinResult, "PH-SIM PIN")) {
        /*
         * Should not happen since lock must first be set from the phone.
         * Setting this lock is not supported by Android.
         */
        ret = SIM_BLOCKED;
    } else {
        /* Unknown locks should not exist. Defaulting to "sim absent" */
        ret = SIM_ABSENT;
    }

exit:
    at_response_free(atresponse);
    return ret;
}

/**
 * Fetch information about UICC card type (SIM/USIM)
 *
 * \return UICC_Type: type of UICC card.
 */
static UICC_Type getUICCType()
{
    ATResponse *atresponse = NULL;
    static UICC_Type UiccType = UICC_TYPE_UNKNOWN; /* FIXME: Static variable */
    int err;
    char *line = NULL;
    char *dir = NULL;

    if (getCurrentState() == RADIO_STATE_OFF ||
        getCurrentState() == RADIO_STATE_UNAVAILABLE) {
        UiccType = UICC_TYPE_UNKNOWN;
        goto exit;
    }

    /* No need to get type again, it is stored */
    if (UiccType != UICC_TYPE_UNKNOWN)
        goto exit;

    /* AT+CUAD will respond with the contents of the EF_DIR file on the SIM */
    err = at_send_command_multiline("AT+CUAD", "+CUAD:", &atresponse);

    if (err != 0 || !atresponse->success)
        goto error;

    /*
     * Run multiple tests for USIM detection, EF_DIR must be present and contain
     * a valid USIM application ID (refer to ETSI TS 101 220).
     */
    if (atresponse->p_intermediates != NULL) {
        line = atresponse->p_intermediates->line;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextstr(&line, &dir);
        if (err < 0)
            goto error;

        if (strstr(dir, "A000000087") != NULL) {
            UiccType = UICC_TYPE_USIM;
            LOGI("Detected card type USIM - stored");
            goto finally;
        }
    }

    UiccType = UICC_TYPE_SIM;
    LOGI("Detected card type SIM - stored");
    goto finally;

error:
    UiccType = UICC_TYPE_UNKNOWN;
    LOGW("%s(): Failed to detect card type - Retry at next request", __func__);

finally:
    at_response_free(atresponse);

exit:
    return UiccType;
}

/**
 * Get the current card status.
 *
 * @return: On success returns RIL_E_SUCCESS.
 */
static int getCardStatus(RIL_CardStatus *p_card_status)
{
    SIM_Status sim_status;

    /* Initialize base card status. */
    p_card_status->card_state = RIL_CARDSTATE_ABSENT;
    p_card_status->universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->gsm_umts_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->cdma_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->num_applications = 0;

    /* Initialize application status. */
    int i;
    for (i = 0; i < RIL_CARD_MAX_APPS; i++)
        p_card_status->applications[i] = app_status_array[SIM_ABSENT];

    sim_status = getSIMStatus();

    if (sim_status > SIM_ABSENT) {
        p_card_status->card_state = RIL_CARDSTATE_PRESENT;

        /* Only support one app, gsm/wcdma. */
        p_card_status->num_applications = 1;
        p_card_status->gsm_umts_subscription_app_index = 0;

        /* Get the correct app status. */
        p_card_status->applications[0] = app_status_array[sim_status];

        /* Get the correct app type */
        if (getUICCType() == UICC_TYPE_SIM)
            LOGI("[Card type discovery]: Legacy SIM");
        else { /* defaulting to USIM */
            LOGI("[Card type discovery]: USIM");
            p_card_status->applications[0].app_type = RIL_APPTYPE_USIM;
        }
    }

    return RIL_E_SUCCESS;
}

/**
 * SIM ready means any commands that access the SIM will work, including:
 *  AT+CPIN, AT+CSMS, AT+CNMI, AT+CRSM
 *  (all SMS-related commands).
 */
void pollSIMState(void *param)
{
    if (((int) param) != 1 &&
            getCurrentState() != RADIO_STATE_SIM_NOT_READY &&
            getCurrentState() != RADIO_STATE_SIM_LOCKED_OR_ABSENT)
        /* No longer valid to poll. */
        return;

    switch (getSIMStatus()) {
    case SIM_NOT_READY:
        LOGI("%s(): SIM_NOT_READY, poll for sim state.\n", __func__);
        enqueueRILEvent(CMD_QUEUE_DEFAULT, pollSIMState, NULL,
                        &TIMEVAL_SIMPOLL);
        return;

    case SIM_PIN2:
    case SIM_PUK2:
    case SIM_PUK2_PERM_BLOCKED:
    case SIM_READY:
        setRadioState(RADIO_STATE_SIM_READY);
        return;

    case SIM_ABSENT:
    case SIM_PIN:
    case SIM_PUK:
    case SIM_NETWORK_PERSO:
    case SIM_NETWORK_SUBSET_PERSO:
    case SIM_SERVICE_PROVIDER_PERSO:
    case SIM_CORPORATE_PERSO:
    case SIM_SIM_PERSO:
    case SIM_STERICSSON_LOCK:
    case SIM_BLOCKED:
    case SIM_PERM_BLOCKED:
    case SIM_NETWORK_PERSO_PUK:
    case SIM_NETWORK_SUBSET_PERSO_PUK:
    case SIM_SERVICE_PROVIDER_PERSO_PUK:
    case SIM_CORPORATE_PERSO_PUK:
    /* pass through, do not break */
    default:
        setRadioState(RADIO_STATE_SIM_LOCKED_OR_ABSENT);
        return;
    }
}

/**
 * RIL_REQUEST_GET_SIM_STATUS
 *
 * Requests status of the SIM interface and the SIM card.
 *
 * Valid errors:
 *  Must never fail.
 */
void requestGetSimStatus(void *data, size_t datalen, RIL_Token t)
{
    RIL_CardStatus *card_status = NULL;

    card_status = malloc(sizeof(*card_status));
    assert(card_status != NULL);

    (void)getCardStatus(card_status);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, (char *) card_status,
                          sizeof(*card_status));

    free(card_status);

    return;
}

static int simIOGetLogicalChannel()
{
    ATResponse *atresponse = NULL;
    static int g_lc = 0;
    char *cmd = NULL;
    int err = 0;

    if (g_lc == 0) {
        struct tlv tlvApp, tlvAppId;
        char *line;
        char *resp;

        err = at_send_command_singleline("AT+CUAD", "+CUAD:", &atresponse);
        if (err < 0)
            goto error;
        if (atresponse->success == 0) {
            err = -1;
            goto error;
        }

        line = atresponse->p_intermediates->line;
        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextstr(&line, &resp);
        if (err < 0)
            goto error;

        err = parseTlv(resp, &resp[strlen(resp)], &tlvApp);
        if (err < 0)
            goto error;
        if (tlvApp.tag != 0x61) { /* Application */
            err = -1;
            goto error;
        }

        err = parseTlv(tlvApp.data, tlvApp.end, &tlvAppId);
        if (err < 0)
            goto error;
        if (tlvAppId.tag != 0x4F) { /* Application ID */
            err = -1;
            goto error;
        }

        asprintf(&cmd, "AT+CCHO=\"%.*s\"",
            (int)(tlvAppId.end - tlvAppId.data), tlvAppId.data);
        if (cmd == NULL) {
            err = -1;
            goto error;
        }

        at_response_free(atresponse);
        err = at_send_command_singleline(cmd, "+CCHO:", &atresponse);
        if (err < 0)
            goto error;

        if (atresponse->success == 0) {
            err = -1;
            goto error;
        }

        line = atresponse->p_intermediates->line;
        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &g_lc);
        if (err < 0)
            goto error;
    }

finally:
    at_response_free(atresponse);
    free(cmd);
    return g_lc;

error:
    goto finally;
}

static int simIOSelectFile(unsigned short fileid)
{
    int err = 0;
    char *cmd = NULL;
    unsigned short lc = simIOGetLogicalChannel();
    ATResponse *atresponse = NULL;
    char *line = NULL;
    char *resp = NULL;
    int resplen;

    if (lc == 0) {
        err = -1;
        goto error;
    }

    asprintf(&cmd, "AT+CGLA=%d,14,\"00A4000C02%.4X\"",
        lc, fileid);
    if (cmd == NULL) {
        err = -1;
        goto error;
    }

    err = at_send_command_singleline(cmd, "+CGLA:", &atresponse);
    if (err < 0)
        goto error;
    if (atresponse->success == 0) {
        err = -1;
        goto error;
    }

    line = atresponse->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &resplen);
    if (err < 0)
        goto error;

    err = at_tok_nextstr(&line, &resp);
    if (err < 0)
        goto error;

    /* Std resp code: "9000" */
    if (resplen != 4 || strcmp(resp, "9000") != 0) {
        err = -1;
        goto error;
    }

finally:
    at_response_free(atresponse);
    free(cmd);
    return err;

error:
    goto finally;
}

static int simIOSelectPath(const char *path, unsigned short fileid)
{
    int err = 0;
    size_t path_len = 0;
    size_t pos;
    static char cashed_path[4 * 10 + 1] = {0};
    static unsigned short cashed_fileid = 0;

    if (path == NULL) {
        path = "3F00";
    }
    path_len = strlen(path);

    if (path_len & 3) {
        err = -1;
        goto error;
    }

    if ((fileid != cashed_fileid) || (strcmp(path, cashed_path) != 0)) {
        for (pos = 0; pos < path_len; pos += 4) {
            unsigned val;
            if (sscanf(&path[pos], "%4X", &val) != 1) {
                err = -1;
                goto error;
            }
            err = simIOSelectFile(val);
            if (err < 0)
                goto error;
        }
        err = simIOSelectFile(fileid);
    }
    if (path_len < sizeof(cashed_path)) {
        strcpy(cashed_path, path);
        cashed_fileid = fileid;
    } else {
        cashed_path[0] = 0;
        cashed_fileid = 0;
    }

finally:
    return err;

error:
    goto finally;
}

int sendSimIOCmdUICC(const RIL_SIM_IO *ioargs, ATResponse **atresponse, RIL_SIM_IO_Response *sr)
{
    int err = 0;
    int resplen;
    char *line = NULL, *resp = NULL;
    char *cmd = NULL, *data = NULL;
    unsigned short lc = simIOGetLogicalChannel();
    unsigned char sw1, sw2;

    if (lc == 0) {
        err = -1;
        goto error;
    }

    memset(sr, 0, sizeof(*sr));

    switch (ioargs->command) {
        case 0xC0: /* Get response */
            /* Convert Get response to Select. */
            asprintf(&data, "00A4000402%.4X00",
                ioargs->fileid);
            break;

        case 0xB0: /* Read binary */
        case 0xB2: /* Read record */
            asprintf(&data, "00%.2X%.2X%.2X%.2X",
                (unsigned char)ioargs->command,
                (unsigned char)ioargs->p1,
                (unsigned char)ioargs->p2,
                (unsigned char)ioargs->p3);
            break;

        case 0xD6: /* Update binary */
        case 0xDC: /* Update record */
            if (!ioargs->data) {
                err = -1;
                goto error;
            }
            asprintf(&data, "00%.2X%.2X%.2X%.2X%s",
                (unsigned char)ioargs->command,
                (unsigned char)ioargs->p1,
                (unsigned char)ioargs->p2,
                (unsigned char)ioargs->p3,
                ioargs->data);
            break;

        default:
            err = -1;
            goto error;
    }
    if (data == NULL) {
        err = -1;
        goto error;
    }

    asprintf(&cmd, "AT+CGLA=%d,%d,\"%s\"", lc, (int) strlen(data), data);
    if (cmd == NULL) {
        err = -1;
        goto error;
    }

    err = simIOSelectPath(ioargs->path, ioargs->fileid);
    if (err < 0)
        goto error;

    err = at_send_command_singleline(cmd, "+CGLA:", atresponse);
    if (err < 0)
        goto error;

    if ((*atresponse)->success == 0) {
        err = -1;
        goto error;
    }

    line = (*atresponse)->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &resplen);
    if (err < 0)
        goto error;

    err = at_tok_nextstr(&line, &resp);
    if (err < 0)
        goto error;

    if ((resplen < 4) || ((size_t)resplen != strlen(resp))) {
        err = -1;
        goto error;
    }

    err = stringToBinary(&resp[resplen - 4], 2, &sw1);
    if (err < 0)
        goto error;

    err = stringToBinary(&resp[resplen - 2], 2, &sw2);
    if (err < 0)
        goto error;

    sr->sw1 = sw1;
    sr->sw2 = sw2;
    resp[resplen - 4] = 0;
    sr->simResponse = resp;

finally:
    free(cmd);
    free(data);
    return err;

error:
    goto finally;

}

int sendSimIOCmdICC(const RIL_SIM_IO *ioargs, ATResponse **atresponse, RIL_SIM_IO_Response *sr)
{
    int err = 0;
    char *cmd = NULL;
    char *fmt = NULL;
    char *arg6 = NULL;
    char *arg7 = NULL;
    char *line = NULL;

    arg6 = ioargs->data;
    arg7 = ioargs->path;

    if (arg7 && arg6) {
        fmt = "AT+CRSM=%d,%d,%d,%d,%d,\"%s\",\"%s\"";
    } else if (arg7) {
        fmt = "AT+CRSM=%d,%d,%d,%d,%d,,\"%s\"";
        arg6 = arg7;
    } else if (arg6) {
        fmt = "AT+CRSM=%d,%d,%d,%d,%d,\"%s\"";
    } else {
        fmt = "AT+CRSM=%d,%d,%d,%d,%d";
    }

    asprintf(&cmd, fmt,
             ioargs->command, ioargs->fileid,
             ioargs->p1, ioargs->p2, ioargs->p3,
             arg6, arg7);

    if (cmd == NULL) {
        err = -1;
        goto error;
    }

    err = at_send_command_singleline(cmd, "+CRSM:", atresponse);
    if (err < 0)
        goto error;

    if ((*atresponse)->success == 0) {
        err = -1;
        goto error;
    }

    line = (*atresponse)->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &(sr->sw1));
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &(sr->sw2));
    if (err < 0)
        goto error;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(sr->simResponse));
        if (err < 0)
            goto error;
    }

finally:
    free(cmd);
    return err;

error:
    goto finally;
}

static int sendSimIOCmd(const RIL_SIM_IO *ioargs, ATResponse **atresponse, RIL_SIM_IO_Response *sr)
{
    int err = 0;
    UICC_Type UiccType;
    ATCmeError cme_error_code = -1;

    if (sr == NULL)
        return -1;

    /* Detect card type to determine which SIM access command to use */
    UiccType = getUICCType();

    /*
     * FIXME WORKAROUND: Currently GCLA works from some files on some cards
     * and CRSM works on some files for some cards...
     * Trying with CRSM first and retry with CGLA if needed
     */
    err = sendSimIOCmdICC(ioargs, atresponse, sr);
    if (err < 0 || (sr->sw1 != 0x90 && sr->sw2 != 0x00)) {
        /*
         * If file operation failed it might be that PIN2 or PUK2 is required
         * for file access. This is detected and if PIN2 is provided another
         * round is attempted. If not provided a PIN2/PUK2 error is reported.
         */
        if (*atresponse != NULL && (*atresponse)->success == 0 &&
            at_get_cme_error(*atresponse, &cme_error_code)) {
            if (cme_error_code == CME_SIM_PIN2_REQUIRED ||
                cme_error_code == CME_SIM_PUK2_REQUIRED) {
                err = 0;
                goto exit;
            }
        }

        if (UiccType != UICC_TYPE_SIM) {
          at_response_free(*atresponse);
          *atresponse = NULL;
          LOGD("%s(): Retrying with CGLA access...", __func__);
          err = sendSimIOCmdUICC(ioargs, atresponse, sr);
        }
    }
    /* END WORKAROUND */

    /* reintroduce below code when workaround is not needed */
    /* if (UiccType == UICC_TYPE_SIM)
        err = sendSimIOCmdICC(ioargs, atresponse, sr);
    else {
        err = sendSimIOCmdUICC(ioargs, atresponse, sr);
    } */

exit:
    return err;
}

static int convertSimIoFcp(RIL_SIM_IO_Response *sr, char **cvt)
{
    int err = 0;
    size_t fcplen;
    struct ts_51011_921_resp resp;
    void *cvt_buf = NULL;

    if (!sr->simResponse || !cvt) {
        err = -1;
        goto error;
    }

    fcplen = strlen(sr->simResponse);
    if ((fcplen == 0) || (fcplen & 1)) {
        err = -1;
        goto error;
    }

    err = fcp_to_ts_51011(sr->simResponse, fcplen, &resp);
    if (err < 0)
        goto error;

    cvt_buf = malloc(sizeof(resp) * 2 + 1);
    if (!cvt_buf) {
        err = -1;
        goto error;
    }

    err = binaryToString((unsigned char*)(&resp),
                   sizeof(resp), cvt_buf);
    if (err < 0)
        goto error;

    /* cvt_buf ownership is moved to the caller */
    *cvt = cvt_buf;
    cvt_buf = NULL;

finally:
    return err;

error:
    free(cvt_buf);
    goto finally;
}

/**
 * enterSimPIN() - Enter PIN to pass PIN(2) verification
 *
 * Returns:  0 on success
 *          -1 on generic CPIN failure
 *          -2 on PIN2 required
 *          -3 on PUK2 required
 *          -4 on other unknown CPIN response
 */
static int enterSimPin(char *pin2) {
    ATResponse *atresponse = NULL;
    int err;
    int ret = -1;  /* generic failure */
    ATCmeError cme_error_code = -1;
    char *cpinLine = NULL;
    char *cpinResult = NULL;
    char *cmd = NULL;

    asprintf(&cmd, "AT+CPIN=\"%s\"", pin2);
    err = at_send_command(cmd, &atresponse);

    if (err < 0)
        goto exit;

    if (atresponse->success == 0) {
        if (at_get_cme_error(atresponse, &cme_error_code)) {
            switch (cme_error_code) {
            case CME_SIM_PIN2_REQUIRED:
                ret = -2;
                break;
            case CME_SIM_PUK2_REQUIRED:
                ret = -3;
                break;
            default:
                ret = -4;
                break;
            }
        }
        goto exit;
    }

    at_response_free(atresponse);
    atresponse = NULL;

    /* CPIN set has succeeded, now look at the result. */
    err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto exit;

    cpinLine = atresponse->p_intermediates->line;

    if (at_tok_start(&cpinLine) < 0)
        goto exit;

    if (at_tok_nextstr(&cpinLine, &cpinResult) < 0)
        goto exit;

    if (0 == strcmp(cpinResult, "READY"))
        ret = 0;
    else if (0 == strcmp(cpinResult, "SIM PIN2"))
        ret = -2;
    else if (0 == strcmp(cpinResult, "SIM PUK2"))
        ret = -3;
    else
        ret = -4;

exit:
    free(cmd);
    at_response_free(atresponse);
    return ret;
}

/**
 * RIL_REQUEST_SIM_IO
 *
 * Request SIM I/O operation.
 * This is similar to the TS 27.007 "restricted SIM" operation
 * where it assumes all of the EF selection will be done by the
 * callee.
 */
void requestSIM_IO(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *atresponse = NULL;
    RIL_SIM_IO_Response sr;
    RIL_SIM_IO ioargsDup;
    int cvt_done = 0;
    int rilErrorCode;
    bool pinTried = false;
    ATCmeError cme_error_code = -1;
    bool pathReplaced = false;

    /*
     * Android telephony framework does not support USIM cards properly,
     * and RIL needs to change the file path of all files listed under the
     * ADF_USIM directory in TS 31.102.
     */
    memcpy(&ioargsDup, data, sizeof(RIL_SIM_IO));
    if (UICC_TYPE_SIM != getUICCType()) {
        unsigned int i;
        unsigned int count = sizeof(ef_usim_files) / sizeof(int);

        for (i = 0; i < count; i++) {
            if (ef_usim_files[i] == ioargsDup.fileid) {
                asprintf(&ioargsDup.path, PATH_ADF_USIM_DIRECTORY);
                pathReplaced = true;
                break;
            }
        }
#ifdef USE_EXT1_INSTEAD_OF_EXT5_WHEN_SIM_CARD_IS_2G_TYPE
    /*
     * Due to a limitation in the Android framework, Android does not have 2G/3G
     * SIM awareness when it starts sending SIM_IO RIL request. The SIM file
     * EF_MSISDN may use extension files, EF_EXT1 in case of 2G SIM and EF_EXT5
     * in case of 3G USIM.
     *
     * The problem is that EF_EXT1 may be used as extension to other files than
     * EF_MSISDN. This workaround in the RIL is dependent on a change in the
     * Android framework to always use EF_EXT5 as extension to EF_MSISDN. This
     * can be done because unlike EF_EXT1, EF_EXT5 is not used as extension for
     * other SIM files than EF_MSISDN.
     *
     * Since the RIL have 2G/3G awareness we can change back to EF_EXT1 if the
     * SIM card is 2G type.
     */
    } else if (ioargsDup.fileid == FILE_ID_EF_EXT5) {
        ioargsDup.fileid = FILE_ID_EF_EXT1;
        asprintf(&ioargsDup.path, PATH_DF_TELECOM_DIRECTORY);
        pathReplaced = true;
    }
#else
    }
#endif

    do {
        /* Reset values for file access */
        rilErrorCode = RIL_E_GENERIC_FAILURE;
        memset(&sr, 0, sizeof(sr));
        sr.simResponse = NULL;
        at_response_free(atresponse);
        atresponse = NULL;

        /* Requesting SIM IO */
        if (sendSimIOCmd(&ioargsDup, &atresponse, &sr) < 0)
            break;

        /* If success break early and finish */
        if (atresponse->success > 0 && sr.sw1 == 0x90 && sr.sw2 == 0x00) {
            rilErrorCode = RIL_E_SUCCESS;
            break;
        }

        /*
         * If file operation failed it might be that PIN2 or PUK2 is required
         * for file access. This is detected and if PIN2 is provided another
         * round is attempted. If not provided a PIN2/PUK2 error is reported:
         */
        /* AT Command Error Check */
        if (atresponse->success == 0 &&
            at_get_cme_error(atresponse, &cme_error_code)) {
            if (cme_error_code == CME_SIM_PIN2_REQUIRED)
                rilErrorCode = RIL_E_SIM_PIN2;
            else if (cme_error_code == CME_SIM_PUK2_REQUIRED)
                rilErrorCode = RIL_E_SIM_PUK2;
        }
        /* Sw1, Sw2 Error Check (0x6982 = Access conditions not fulfilled) */
        else if (sr.sw1 == 0x69 && sr.sw2 == 0x82) {
            SIM_Status simState = getSIMStatus();
            if (simState == SIM_PIN2)
                rilErrorCode = RIL_E_SIM_PIN2;
            else if (simState == SIM_PUK2)
                rilErrorCode = RIL_E_SIM_PUK2;
        }

        /*
         * Check if there is a reason to try PIN2 code.
         * (If nothing more to do exit file access attempts.)
         */
        if (ioargsDup.pin2 == NULL ||
            rilErrorCode != RIL_E_SIM_PIN2 ||
            pinTried)
            break;

        /* PIN is entered to pass PIN2 verification for file access */
        int pinRes = enterSimPin(ioargsDup.pin2);
        if (pinRes == -1) { /* Unknown error entering PIN2 */
            LOGD("%s(): Failed entering PIN2 for SIM IO, "
                 "unknown error", __func__);
            rilErrorCode = RIL_E_GENERIC_FAILURE;
            break;
        } else if (pinRes == -2) { /* Entered PIN2 state */
            LOGD("%s(): Failed entering PIN2 for SIM IO, "
                "probably incorrect PIN2", __func__);
            rilErrorCode = RIL_E_SIM_PIN2;
            break;
        } else if (pinRes == -3) { /* Entered PUK2 state */
            LOGD("%s(): Failed entering PIN2 for SIM IO, "
                "probably incorrect PIN2 leading to PUK2 state", __func__);
            rilErrorCode = RIL_E_SIM_PUK2;
            break;
        } else { /* PIN2 verified successfully or unknown error code */
            /*
             * Another file access attempt will done by doing another loop
             * and SIM IO request towards modem.
             * "pinTried" flag is set to make sure we only try the PIN once
             * and exit the loop after a total of 2 rounds.
             */
            pinTried = true;
        }
        /*
         * Loop will only iterate if PIN2 seems to have been verified.
         * This gives a total of 2 rounds.
         */
    } while (true);


    if (rilErrorCode != RIL_E_SUCCESS)
        goto error;

    /*
     * In case the command is GET_RESPONSE and cardtype is 3G SIM
     * conversion to 2G FCP is required
     */
    if (ioargsDup.command == 0xC0 && getUICCType() != UICC_TYPE_SIM) {
        if (convertSimIoFcp(&sr, &sr.simResponse) < 0) {
            rilErrorCode = RIL_E_GENERIC_FAILURE;
            goto error;
        }
        cvt_done = 1; /* sr.simResponse needs to be freed */
    }

    /* Finally send response to Android */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    goto exit;

error:
    RIL_onRequestComplete(t, rilErrorCode, NULL, 0);

exit:
    at_response_free(atresponse);
    if (cvt_done)
        free(sr.simResponse);
    if (pathReplaced)
        free(ioargsDup.path);
}


/**
 * Enter SIM PIN, might be PIN, PIN2, PUK, PUK2, etc.
 *
 * Data can hold pointers to one or two strings, depending on what we
 * want to enter. (PUK requires new PIN, etc.).
 *
 */
void requestEnterSimPin(void *data, size_t datalen, RIL_Token t, int request)
{
    ATResponse *atresponse = NULL;
    int err;
    char *cmd = NULL;
    const char **strings = (const char **) data;
    int num_retries = -1;
    ATCmeError cme_error_code;

    if (datalen == sizeof(char *)) {
        /*
         * Entering PIN(2) is not possible using AT+CPIN unless SIM state is
         * PIN(2) required. The workaround is to change PIN(2) to the same value
         * using AT+CPWD.
         */
        if (request == RIL_REQUEST_ENTER_SIM_PIN && getSIMStatus() != SIM_PIN)
            asprintf(&cmd, "AT+CPWD=\"SC\",\"%s\",\"%s\"", strings[0],
                     strings[0]);
        else if (request == RIL_REQUEST_ENTER_SIM_PIN2 &&
                 getSIMStatus() != SIM_PIN2)
            asprintf(&cmd, "AT+CPWD=\"P2\",\"%s\",\"%s\"", strings[0],
                     strings[0]);
        else
            asprintf(&cmd, "AT+CPIN=\"%s\"", strings[0]);
    } else if (datalen == 2 * sizeof(char *)) {
        /*
         * Unblocking PIN(2) is not possible using AT+CPIN unless SIM state is
         * PUK(2) required. We need to support this due to 3GPP TS 31.121
         * section 6.1.3. Using ATD for unblocking PIN only works when ME is
         * camping on network.
         */
        if (request == RIL_REQUEST_ENTER_SIM_PUK && getSIMStatus() != SIM_PUK)
            asprintf(&cmd, "ATD**05*%s*%s*%s#;", strings[0], strings[1],
                     strings[1]);
        else if (request == RIL_REQUEST_ENTER_SIM_PUK2 &&
                 getSIMStatus() != SIM_PUK2)
            asprintf(&cmd, "ATD**052*%s*%s*%s#;", strings[0], strings[1],
                     strings[1]);
        else
            asprintf(&cmd, "AT+CPIN=\"%s\",\"%s\"", strings[0], strings[1]);
    } else
        goto error;

    err = at_send_command(cmd, &atresponse);
    free(cmd);

    if (err < 0) {
        goto error;
    }
    if (atresponse->success == 0) {
        if (at_get_cme_error(atresponse, &cme_error_code)) {
            switch (cme_error_code) {
            case CME_SIM_PIN_REQUIRED:
            case CME_SIM_PUK_REQUIRED:
            case CME_INCORRECT_PASSWORD:
            case CME_SIM_PIN2_REQUIRED:
            case CME_SIM_PUK2_REQUIRED:
            case CME_SIM_FAILURE:
                num_retries = getNumRetries(request);
                RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &num_retries, sizeof(int *));
                goto finally;

            default:
                break;
            }
        }
        goto error;
    }

    /* Got OK, return success and wait for *EPEV to trigger poll of SIM state. */
    num_retries = getNumRetries(request);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &num_retries, sizeof(int *));

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

static void requestChangePassword(char *facility, void *data, size_t datalen,
                           RIL_Token t, int request)
{
    int err = 0;
    char *oldPassword = NULL;
    char *newPassword = NULL;
    char *cmd = NULL;
    ATResponse *atresponse = NULL;
    int num_retries = -1;
    RIL_Errno errorril = RIL_E_GENERIC_FAILURE;
    ATCmeError cme_error_code;

    if (datalen != 2 * sizeof(char **) || strlen(facility) != 2) {
        goto error;
    }

    oldPassword = ((char **) data)[0];
    newPassword = ((char **) data)[1];

    asprintf(&cmd, "AT+CPWD=\"%s\",\"%s\",\"%s\"", facility, oldPassword,
             newPassword);

    err = at_send_command(cmd, &atresponse);
    free(cmd);

    num_retries = getNumRetries(request);

    if (err < 0) {
        goto error;
    }
    if (atresponse->success == 0) {
        if (at_get_cme_error(atresponse, &cme_error_code)) {
            switch (cme_error_code) {
            case CME_INCORRECT_PASSWORD: /* CME ERROR 16: "Incorrect password" */
                LOGI("%s(): Incorrect password", __func__);
                errorril = RIL_E_PASSWORD_INCORRECT;
                break;
            case CME_SIM_PUK2_REQUIRED: /* CME ERROR 18: "SIM PUK2 required" happens when wrong
                PIN2 is used 3 times in a row */
                LOGI("%s(): PIN2 locked, change PIN2 with PUK2", __func__);
                num_retries = 0;/* PUK2 required */
                errorril = RIL_E_SIM_PUK2;
                break;
            default: /* some other error */
                break;
            }
        }
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &num_retries, sizeof(int *));
finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, errorril, &num_retries, sizeof(int *));
    goto finally;
}

/**
 * RIL_REQUEST_CHANGE_SIM_PIN
 *
 * Change PIN 1.
 */
void requestChangeSimPin(void *data, size_t datalen, RIL_Token t, int request)
{
    requestChangePassword("SC", data, datalen, t, request);
}

/**
 * RIL_REQUEST_CHANGE_SIM_PIN2
 *
 * Change PIN 2.
 */
void requestChangeSimPin2(void *data, size_t datalen, RIL_Token t, int request)
{
    requestChangePassword("P2", data, datalen, t, request);
}

/**
 * RIL_REQUEST_CHANGE_BARRING_PASSWORD
 *
 * Change barring password.
 */
void requestChangeBarringPassword(void *data, size_t datalen, RIL_Token t, int request)
{
    requestChangePassword(((char **) data)[0],
                          ((char *) data) + sizeof(char *),
                          datalen - sizeof(char *), t, request);
}

/**
 * RIL_REQUEST_SET_FACILITY_LOCK
 *
 * Enable/disable one facility lock.
 */
void requestSetFacilityLock(void *data, size_t datalen, RIL_Token t)
{
    int err;
    ATResponse *atresponse = NULL;
    char *cmd = NULL;
    char *facility_string = NULL;
    int facility_mode = -1;
    char *facility_mode_str = NULL;
    char *facility_password = NULL;
    char *facility_class = NULL;
    int num_retries = -1;
    int classx;
    size_t i;
    RIL_Errno errorril = RIL_E_GENERIC_FAILURE;
    ATCmeError cme_error_code;
    char *barr_facilities[] = {"AO", "OI", "AI", "IR", "OX", "AB", "AG",
        "AC", NULL};

    assert(datalen >= (4 * sizeof(char **)));

    facility_string = ((char **) data)[0];
    facility_mode_str = ((char **) data)[1];
    facility_password = ((char **) data)[2];
    facility_class = ((char **) data)[3];
    classx = atoi(facility_class);

    assert(*facility_mode_str == '0' || *facility_mode_str == '1');
    facility_mode = atoi(facility_mode_str);

    /*
     * Android send class 0 for USSD strings that didn't contain a class.
     * Class 0 is not considered a valid value and according to 3GPP 24.080 a
     * missing BasicService (BS) parameter in the Supplementary Service string
     * indicates all BS'es.
     *
     * Therefore we convert a class of 0 into 255 (all classes) before sending
     * the AT command for the following barrings:
     *
     *  "AO": barr All Outgoing calls
     *  "OI": barr Outgoing International calls
     *  "AI": barr All Incomming calls
     *  "IR": barr Incoming calls when Roaming outside the home country
     *  "OX": barr Outgoing international calls eXcept to home country
     *  "AB": all barring services (only unlock mode=0)
     *  "AG": all outgoing barring services (only unlock mode=0)
     *  "AC": all incoming barring services (only unlock mode=0)
     */
    for (i = 0; barr_facilities[i] != NULL; i++) {
        if (!strncmp(facility_string, barr_facilities[i], 2)) {
            if (classx == 0)
                classx = 255;
            break;
        }
    }

    /*
     * Skip adding facility_password to AT command parameters if it is NULL,
     * printing NULL with %s will give string "(null)".
     */
    asprintf(&cmd, "AT+CLCK=\"%s\",%d,\"%s\",%d", facility_string,
        facility_mode, (facility_password ? facility_password : "" ), classx);

    err = at_send_command(cmd, &atresponse);
    free(cmd);
    if (err < 0) {
        goto exit;
    }
    if (atresponse->success == 0) {
        if (at_get_cme_error(atresponse, &cme_error_code)) {
            switch (cme_error_code) {
            /* CME ERROR 11: "SIM PIN required" happens when PIN is wrong */
            case CME_SIM_PIN_REQUIRED:
                LOGI("requestSetFacilityLock(): wrong PIN");
                num_retries = getNumRetries(RIL_REQUEST_ENTER_SIM_PIN);
                errorril = RIL_E_PASSWORD_INCORRECT;
                break;
            /*
             * CME ERROR 12: "SIM PUK required" happens when wrong PIN is used
             * 3 times in a row
             */
            case CME_SIM_PUK_REQUIRED:
                LOGI("requestSetFacilityLock() PIN locked,"
                " change PIN with PUK");
                num_retries = 0;/* PUK required */
                errorril = RIL_E_PASSWORD_INCORRECT;
                break;
            /* CME ERROR 16: "Incorrect password" happens when PIN is wrong */
            case CME_INCORRECT_PASSWORD:
                LOGI("%s(): Incorrect password, Facility: %s", __func__,
                     facility_string);
                errorril = RIL_E_PASSWORD_INCORRECT;
                break;
            /* CME ERROR 17: "SIM PIN2 required" happens when PIN2 is wrong */
            case CME_SIM_PIN2_REQUIRED:
                LOGI("requestSetFacilityLock() wrong PIN2");
                num_retries = getNumRetries(RIL_REQUEST_ENTER_SIM_PIN2);
                errorril = RIL_E_PASSWORD_INCORRECT;
                break;
            /*
             * CME ERROR 18: "SIM PUK2 required" happens when wrong PIN2 is used
             * 3 times in a row
             */
            case CME_SIM_PUK2_REQUIRED:
                LOGI("requestSetFacilityLock() PIN2 locked, change PIN2"
                "with PUK2");
                num_retries = 0;/* PUK2 required */
                errorril = RIL_E_SIM_PUK2;
                break;
            default: /* some other error */
                num_retries = -1;
                break;
            }
        }
        goto finally;
    }

    errorril = RIL_E_SUCCESS;

finally:
    if (num_retries == -1 && strncmp(facility_string, "SC", 2) == 0)
        num_retries = getNumRetries(RIL_REQUEST_ENTER_SIM_PIN);
exit:
    at_response_free(atresponse);
    RIL_onRequestComplete(t, errorril, &num_retries,  sizeof(int *));
}


/**
 * RIL_REQUEST_QUERY_FACILITY_LOCK
 *
 * Query the status of a facility lock state.
 */
void requestQueryFacilityLock(void *data, size_t datalen, RIL_Token t)
{
    int err, response = 0;
    ATResponse *atresponse = NULL;
    char *cmd = NULL;
    char *line = NULL;
    char *facility_string = NULL;
    char *facility_class = NULL;
    int classx;
    ATLine *cursor;
    size_t i;
    bool barring_service = false;

    /*
     * The following barring services may return multiple lines of intermediate
     * result codes and will return two parameters in the +CLCK response.
     *
     *  "AO": barr All Outgoing calls
     *  "OI": barr Outgoing International calls
     *  "AI": barr All Incomming calls
     *  "IR": barr Incoming calls when Roaming outside the home country
     *  "OX": barr Outgoing international calls eXcept to home country
     */
    char *barr_facilities[] = {"AO", "OI", "AI", "IR", "OX", NULL};

    assert(datalen >= (3 * sizeof(char **)));

    facility_string = ((char **) data)[0];
    facility_class = ((char **) data)[2];
    classx = atoi(facility_class);

    /*
     * Android send class 0 for USSD strings that didn't contain a class.
     * Class 0 is not considered a valid value and according to 3GPP 24.080 a
     * missing BasicService (BS) parameter in the Supplementary Service string
     * indicates all BS'es.
     *
     * Therefore we convert a class of 0 into 255 (all classes) before sending
     * the AT command.
     */
    if (classx == 0)
        classx = 255;

    for (i = 0; barr_facilities[i] != NULL; i++) {
        if (!strncmp(facility_string, barr_facilities[i], 2)) {
            barring_service = true;
        }
    }

    /* password is not needed for query of facility lock. */
    asprintf(&cmd, "AT+CLCK=\"%s\",2,,%d", facility_string, classx);

    err = at_send_command_multiline(cmd, "+CLCK:", &atresponse);
    free(cmd);
    if (err < 0 || atresponse->success == 0)
        goto error;

    for (cursor = atresponse->p_intermediates; cursor != NULL;
         cursor = cursor->p_next) {
        int status = 0;

        line = cursor->line;

        err = at_tok_start(&line);

        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &status);

        if (err < 0)
            goto error;

        if (barring_service) {
            err = at_tok_nextint(&line, &classx);

            if (err < 0)
                goto error;

            if (status == 1)
                response += classx;
        } else {
            switch (status) {
            case 1:
                /* Default value including voice, data and fax services */
                response = 7;
                break;
            default:
                response = 0;
            }
            /*
             * There will be only 1 line of intermediate result codes when <fac>
             * is not a barring service.
             */
            break;
        }
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int *));

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * Check if string array contains the passed string.
 *
 * \param str: string to examine.
 * \param list: NULL-terminated string array to examine.
 * \return non zero if string is found.
 */
static int isStringInList(const char *str, const char **list)
{
    size_t pos;
    for (pos = 0; list[pos]; ++pos) {
        if (!strcmp(str, list[pos]))
            return 1;
    }
    return 0;
}

/**
 * Append a string to the comma-separated list.
 *
 * \param phone_list: Pointer to the comma-separated list.
 * \param str: String to be appended.
 * \note Function allocates memory for the phone_list.
 */
static void phoneListAppend(char **phone_list, const char *str)
{
    if (*phone_list) {
        char *tmp;
        asprintf(&tmp, "%s,%s", *phone_list, str);
        free(*phone_list);
        *phone_list = tmp;
    } else {
        asprintf(phone_list, "%s", str);
    }
}

/**
 * Append a NULL-terminated string array to the comma-separated list.
 *
 * \param phone_list: Pointer to the comma-separated list.
 * \param list: NULL-terminated string array to be appended.
 * \note Function allocates memory for the phone_list.
 */
static void phoneListAppendList(char **phone_list, const char **list)
{
    size_t pos;
    for (pos = 0; list[pos]; ++pos) {
        phoneListAppend(phone_list, list[pos]);
    }
}

/**
 * Convert a Called party BCD number (defined in 3GPP TS 24.008)
 * to an Ascii number.
 *
 * \param bcd: input character
 *
 * \return Ascii value
 */
static char bcdToAscii(const unsigned char bcd)
{
    switch(bcd & 0x0F) {
        case 0x0A:
            return '*';
        case 0x0B:
            return '#';
        case 0x0C:
            return 'a';
        case 0x0D:
            return 'b';
        case 0x0E:
            return 'c';
        case 0x0F:
            return 0;
        default:
            return (bcd + '0');
    }
}

/**
 * Store an ECC list in the r/w ECC list property (ril.ecclist).
 *
 * \param list: ECC list in 3GPP TS 51.011, 10.3.27 format.
 * \param use_japan_extensions: Use Japan-specific ECC numbers.
 * \note Function appends standard ECC numbers to the list provided.
 */
static void storeEccList(const char *list, int use_japan_extensions)
{
    /* Phone number conversion as per 3GPP TS 51.011, 10.3.27 */
    char *buf = NULL;
    size_t pos;
    size_t len = strlen(list);
    static const char *std_ecc[] = {"112", "911", NULL};
    static const char *std_ecc_jpn[] = {"110", "118", "119", NULL};

    LOGD("[ECC]: ECC list from SIM (length: %d): %s", (int) len, list);

    for (pos = 0; pos + 6 <= len; pos += 6) {
        size_t i;
        char dst[7];

        for (i = 0; i < 6; i += 2) {
            char digit1 = bcdToAscii(char2nib(list[pos + i + 1]));
            char digit2 = bcdToAscii(char2nib(list[pos + i + 0]));

            dst[i + 0] = digit1;
            dst[i + 1] = digit2;
        }
        dst[i] = 0;

        if (dst[0]) {
            if (isStringInList(dst, std_ecc))
                continue;
            if (use_japan_extensions && isStringInList(dst, std_ecc_jpn))
                continue;

            phoneListAppend(&buf, dst);
        }
    }

    if (buf == NULL || strlen(buf) == 0) {
        LOGI("[ECC]: No valid ECC numbers on SIM, keeping defaults");
        goto exit;
    }

    phoneListAppendList(&buf, std_ecc);
    if (use_japan_extensions)
        phoneListAppendList(&buf, std_ecc_jpn);
    if (buf) {
        LOGD("[ECC]: ECC phone numbers: %s", buf);
        (void)property_set(PROP_EMERGENCY_LIST_RW, buf);
        free(buf);
    }
exit:
    return;
}

/*
 * Reads the emergency call codes from the EF_ECC file in the SIM
 * card from path "3F007F20" using "READ BINARY" command.
 */
void read2GECCFile(int use_japan_extensions)
{
    int err = 0;
    ATResponse *atresponse = NULL;
    RIL_SIM_IO ioargs;
    RIL_SIM_IO_Response sr;
    memset(&ioargs, 0, sizeof(ioargs));
    memset(&sr, 0, sizeof(sr));
    sr.simResponse = NULL;

    ioargs.command = 176;   /* READ_BINARY */
    ioargs.fileid  = 0x6FB7; /* EF_ECC */
    ioargs.path = "3F007F20"; /* GSM directory */
    ioargs.data = NULL;
    ioargs.p3 = 15;          /* length */

    err = sendSimIOCmd(&ioargs, &atresponse, &sr);
    if (!err && sr.sw1 == 0x90 && sr.sw2 == 0x00) {
        storeEccList(sr.simResponse, use_japan_extensions);
    } else {
        LOGI("[ECC]: No valid ECC numbers on SIM, keeping defaults");
    }
    at_response_free(atresponse);
}

/*
 * Reads the emergency call codes from the EF_ECC file in the SIM
 * card from path "7FFF" using "READ RECORD" commands.
 */
bool read3GECCFile(int use_japan_extensions)
{
    int err = 0;
    ATResponse *atresponse = NULL;
    int numRecords = 0;
    int fileSize = 0;
    int recordSize = 0;
    int i = 1;
    char *ecc_list = NULL;

    RIL_SIM_IO ioargs;
    RIL_SIM_IO_Response sr;
    memset(&ioargs, 0, sizeof(ioargs));
    memset(&sr, 0, sizeof(sr));
    sr.simResponse = NULL;

    ioargs.command = 192;   /* GET RESPONSE */
    ioargs.fileid  = 0x6FB7; /* EF_ECC */
    ioargs.path = "7FFF"; /* USIM directory */
    ioargs.data = NULL;
    ioargs.p3 = 15;          /* length */

    err = sendSimIOCmd(&ioargs, &atresponse, &sr);

    if (err || sr.sw1 != 0x90 || sr.sw2 != 0x00) {
        LOGW("[ECC]: GET RESPONSE command on 3G EFecc file failed, error "
            "%.2X:%.2X.", sr.sw1, sr.sw2);
        at_response_free(atresponse);
        return false;
    }

    /*
     * Convert response from GET_RESPONSE using convertSimIoFcp() to
     * simplify fetching record size and file size using fixed offsets.
     */
    err = convertSimIoFcp(&sr, &sr.simResponse);
    if (err < 0) {
        LOGW("[ECC]: Conversion of GET RESPONSE data failed.");
        goto error2;
    }

    /* Convert hex string to int's and calculate number of records */
    recordSize = ((char2nib(sr.simResponse[28]) * 16) +
        char2nib(sr.simResponse[29])) & 0xff;
    fileSize = ((((char2nib(sr.simResponse[4]) * 16) +
        char2nib(sr.simResponse[5])) & 0xff) << 8) +
        (((char2nib(sr.simResponse[6]) * 16) +
        char2nib(sr.simResponse[7])) & 0xff);
    numRecords = fileSize / recordSize;

    LOGI("[ECC]: Number of records in EFecc file: %d", numRecords);

    if (numRecords > 254) {
        goto error1;
    }

    free(sr.simResponse); /* sr.simResponse needs to be freed */
    at_response_free(atresponse);
    atresponse = NULL;

    /*
     * Allocate memory for a list containing the emergency call codes in
     * raw format. Each emergency call code is coded on three bytes.
     */
    ecc_list = malloc((numRecords * 3 * 2) + 1);
    if(!ecc_list) {
        LOGE("[ECC]: Failed to allocate memory for SIM fetched ECC's");
        goto error1;
    }
    memset(ecc_list, 0, (numRecords * 3 * 2) + 1);

    /*
     * Loop and fetch all the records using READ RECORD command.
     * Linear fixed EF files uses 1-based counting of records.
     */
    for(i = 1; i <= numRecords; i++) {
        char *p;
        p = (ecc_list + ((i - 1) * 6));
        memset(&ioargs, 0, sizeof(ioargs));
        memset(&sr, 0, sizeof(sr));
        sr.simResponse = NULL;

        ioargs.command = 178;   /* READ_RECORD */
        ioargs.fileid  = 0x6FB7; /* EF_ECC */
        ioargs.path = "7FFF";
        ioargs.data = NULL;
        ioargs.p1 = i;          /* record number */
        ioargs.p2 = 4;          /* absolute method */
        ioargs.p3 = recordSize; /* length */

        err = sendSimIOCmd(&ioargs, &atresponse, &sr);
        if (!err && sr.sw1 == 0x90 && sr.sw2 == 0x00)
            memcpy(p, sr.simResponse, 6);
        else
            LOGW("[ECC]: Can't fetch ECC record from 3G USIM card: error"
                "%.2X:%.2X. Continuing.", sr.sw1, sr.sw2);

        at_response_free(atresponse);
        atresponse = NULL;
    }
    goto finally;

error1:
    if(sr.simResponse)
        free(sr.simResponse); /* sr.simResponse needs to be freed */
error2:
    at_response_free(atresponse);

finally:
    if(ecc_list) {
        storeEccList(ecc_list, use_japan_extensions);
        free(ecc_list);
    } else {
        LOGI("[ECC]: No valid ECC numbers on SIM, keeping defaults");
    }
    return true;
}

/*
 * Setup r/w ECC list property (ril.ecclist) with values from EF_ECC
 * and predefined values.
 *
 * \param check_attached_network: Check attached network for the MCC code
 *                                (Japan extensions)
 */
void setupECCList(int check_attached_network)
{
    int use_japan_extensions = 0;
    int mcc;

    /* Check for Japan expensions. */
    if (check_attached_network && (0 == getAttachedNetworkIdentity(&mcc, NULL))
           && (mcc == 440)) {
        LOGD("[ECC]: Using Japan extensions: detected by %s network.",
            "attached");
        use_japan_extensions = 1;
    } else if ((0 == getHomeNetworkIdentity(&mcc, NULL)) && (mcc == 440)) {
        LOGD("[ECC]: Using Japan extensions: detected by %s network.", "home");
        use_japan_extensions = 1;
    } else {
        LOGD("[ECC]: Using world rules.");
    }

    /*
     * Fetch emergency call code list from EF_ECC
     * as described in 3GPP TS 51.011, section 10.3.27.
     */
    if (getUICCType() == UICC_TYPE_SIM) {
        LOGI("[ECC]: 2G SIM card detected, using read binary method.");
        read2GECCFile(use_japan_extensions);
    }
    /*
     * Fetch emergency call code list from EF_ECC
     * as described in 3GPP TS 31.102, section 4.2.21.
     */
    else {
        LOGI("[ECC]: 3G USIM card detected, using read record method.");
        if(!read3GECCFile(use_japan_extensions)) {
            /*
             * A SIM card that have the EFecc file stored in the 2G SIM path
             * despite having a UICC application running was found during
             * testing. This is the reasoning for having the below fallback
             * solution.
             */
            LOGI("[ECC]: ECC file does not exist in USIM directory, "
                "try reading from GSM directory.");
            read2GECCFile(use_japan_extensions);
        }
    }
}
