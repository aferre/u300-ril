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

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <telephony/ril.h>
#include <assert.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include "u300-ril.h"
#include "u300-ril-sim.h"

#define LOG_TAG "RILV"
#include <utils/Log.h>

#define REPOLL_OPERATOR_SELECTED 30 /* 30 * 2 = 1M = ok? */
#define COPS_AT_TIMEOUT_MSEC (5 * 60 * 1000)

static const struct timeval TIMEVAL_OPERATOR_SELECT_POLL = { 2, 0 };

static void pollOperatorSelected(void *params);

/*
 * s_registrationDenyReason is used to keep track of registration deny
 * reason for which is called by pollOperatorSelected from
 * RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC, so that in case
 * of invalid SIM/ME, Android will not continuously poll for operator.
 *
 * s_registrationDenyReason is set when receives the registration deny
 * and detail reason from "at*ereg?" command, and is reset to
 * DEFAULT_VALUE otherwise.
 */
static Reg_Deny_DetailReason s_registrationDenyReason = DEFAULT_VALUE;

struct operatorPollParams {
    RIL_Token t;
    int loopcount;
};

enum BarringState {
    BARRING_STATE_UNKNOWN = 0,
    NO_BARRING = 1,
    EMERGENCY_CALLS_BARRED = 2,
    EMERGENCY_CALLS_ONLY_ALLOWED = 3,
    ALL_CALLS_BARRED = 4
};

/* +CGREG AcT values */
enum CREG_AcT {
    CGREG_ACT_GSM               = 0,
    CGREG_ACT_GSM_COMPACT       = 1, /* Not Supported */
    CGREG_ACT_UTRAN             = 2,
    CGREG_ACT_GSM_EGPRS         = 3,
    CGREG_ACT_UTRAN_HSDPA       = 4,
    CGREG_ACT_UTRAN_HSUPA       = 5,
    CGREG_ACT_UTRAN_HSUPA_HSDPA = 6
};

/**
 * Poll +COPS?, if operator is retrieved, returns success,
 * if registration is denied, returns RIL_E_ILLEGAL_SIM_OR_ME;
 * or if the loop counter reaches REPOLL_OPERATOR_SELECTED,
 * return generic failure.
 */
static void pollOperatorSelected(void *params)
{
    int err = 0;
    int response = 0;
    char *line = NULL;
    ATResponse *atresponse = NULL;
    struct operatorPollParams *poll_params;
    RIL_Token t;

    assert(params != NULL);

    poll_params = (struct operatorPollParams *) params;
    t = poll_params->t;

    if (poll_params->loopcount >= REPOLL_OPERATOR_SELECTED)
        goto error;

    err = at_send_command_singleline("AT+COPS?", "+COPS:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &response);
    if (err < 0)
        goto error;

    /* If we don't get more than the COPS: {0-4} we are not registered.
       Loop and try again. */
    if (!at_tok_hasmore(&line)) {
        switch (s_registrationDenyReason) {
        case IMSI_UNKNOWN_IN_HLR: /* fall through */
        case ILLEGAL_ME:
            RIL_onRequestComplete(t, RIL_E_ILLEGAL_SIM_OR_ME, NULL, 0);
            break;
        default:
            poll_params->loopcount++;
            enqueueRILEvent(CMD_QUEUE_AUXILIARY, pollOperatorSelected,
                            poll_params, &TIMEVAL_OPERATOR_SELECT_POLL);
            goto exit;
        }
    } else {
        /* We got operator, throw a success! */
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    goto finally;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
finally:
    free(poll_params);
exit:
    at_response_free(atresponse);
    return;
}

/**
 * GSM Network Neighborhood Cell IDs
 */
static void GSMNeighboringCellIDs(void *data, size_t datalen, RIL_Token t)
{
    int err = 0;
    char *p = NULL;
    int n = 0;
    ATLine *tmp = NULL;
    ATResponse *atresponse = NULL;
    RIL_NeighboringCell *ptr_cells[MAX_NUM_NEIGHBOR_CELLS];

    err = at_send_command_multiline("AT*EGNCI", "*EGNCI:", &atresponse);
    if (err < 0 ||
        atresponse->success == 0 || atresponse->p_intermediates == NULL)
        goto error;

    tmp = atresponse->p_intermediates;
    while (tmp) {
        if (n > MAX_NUM_NEIGHBOR_CELLS)
            goto error;
        p = tmp->line;
        if (*p == '*') {
            char *line = p;
            char *plmn = NULL;
            char *lac = NULL;
            char *cid = NULL;
            int arfcn = 0;
            int bsic = 0;
            int rxlvl = 0;
            int ilac = 0;
            int icid = 0;

            err = at_tok_start(&line);
            if (err < 0)
                goto error;

            /* PLMN */
            err = at_tok_nextstr(&line, &plmn);
            if (err < 0)
                goto error;

            /* LAC */
            err = at_tok_nextstr(&line, &lac);
            if (err < 0)
                goto error;

            /* CellID */
            err = at_tok_nextstr(&line, &cid);
            if (err < 0)
                goto error;

            /* ARFCN */
            err = at_tok_nextint(&line, &arfcn);
            if (err < 0)
                goto error;

            /* BSIC */
            err = at_tok_nextint(&line, &bsic);
            if (err < 0)
                goto error;

            /* RxLevel */
            err = at_tok_nextint(&line, &rxlvl);
            if (err < 0)
                goto error;

            /* process data for each cell */
            ptr_cells[n] = alloca(sizeof(RIL_NeighboringCell));
            ptr_cells[n]->rssi = rxlvl;
            ptr_cells[n]->cid = alloca(9 * sizeof(char));
            sscanf(lac,"%x",&ilac);
            sscanf(cid,"%x",&icid);
            sprintf(ptr_cells[n]->cid, "%08x", ((ilac << 16) + icid));
            n++;
        }
        tmp = tmp->p_next;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, ptr_cells,
                          n * sizeof(RIL_NeighboringCell *));

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * WCDMA Network Neighborhood Cell IDs
 */
static void WCDMANeighboringCellIDs(void *data, size_t datalen,
                                     RIL_Token t)
{
    int err = 0;
    char *p = NULL;
    int n = 0;
    ATLine *tmp = NULL;
    ATResponse *atresponse = NULL;
    RIL_NeighboringCell *ptr_cells[MAX_NUM_NEIGHBOR_CELLS];

    err = at_send_command_multiline("AT*EWNCI", "*EWNCI:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    tmp = atresponse->p_intermediates;
    while (tmp) {
        if (n > MAX_NUM_NEIGHBOR_CELLS)
            goto error;
        p = tmp->line;
        if (*p == '*') {
            char *line = p;
            int uarfcn = 0;
            int psc = 0;
            int rscp = 0;
            int ecno = 0;
            int pathloss = 0;

            err = at_tok_start(&line);
            if (err < 0)
                goto error;

            /* UARFCN */
            err = at_tok_nextint(&line, &uarfcn);
            if (err < 0)
                goto error;

            /* PSC */
            err = at_tok_nextint(&line, &psc);
            if (err < 0)
                goto error;

            /* RSCP */
            err = at_tok_nextint(&line, &rscp);
            if (err < 0)
                goto error;

            /* ECNO */
            err = at_tok_nextint(&line, &ecno);
            if (err < 0)
                goto error;

            /* PathLoss */
            err = at_tok_nextint(&line, &pathloss);
            if (err < 0)
                goto error;

            /* process data for each cell */
            ptr_cells[n] = alloca(sizeof(RIL_NeighboringCell));
            ptr_cells[n]->rssi = rscp;
            ptr_cells[n]->cid = alloca(9 * sizeof(char));
            sprintf(ptr_cells[n]->cid, "%08x", psc);
            n++;
        }
        tmp = tmp->p_next;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, ptr_cells,
                          n * sizeof(RIL_NeighboringCell *));

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * Get network identity (MCC/MNC) of the Home network.
 *
 * \param mcc: [out] MCC value.
 * \param mnc: [out] MNC value.
 * \return Negative value indicates failure.
 */
 int getHomeNetworkIdentity(int *mcc, int *mnc)
{
    char *tok, *mcc_mnc, *end;
    ATResponse *atresponse = NULL;
    int status = 0;

    status = at_send_command_singleline("AT*EHNET=2", "*EHNET", &atresponse);
    if (status < 0)
        goto out;
    if (atresponse->success == 0) {
        status = -1;
        goto out;
    }
    tok = atresponse->p_intermediates->line;

    status = at_tok_start(&tok);
    if (status < 0)
        goto out;

    status = at_tok_nextstr(&tok, &mcc_mnc);
    if (status < 0)
        goto out;

    if (strlen(mcc_mnc) < 5) {
        status = -1;
        goto out;
    }

    if (mnc) {
        *mnc = strtol(&mcc_mnc[3], &end, 10);
        if (*end != '\0') {
            status =  -1;
            goto out;
        }
    }

    mcc_mnc[3] = '\0';
    if (mcc) {
        *mcc = strtol(&mcc_mnc[0], &end, 10);
        if (*end != '\0') {
            status =  -1;
            goto out;
        }
    }

out:
    at_response_free(atresponse);
    return status;
}

/**
 * Get network identity (MCC/MNC) of the Attached network.
 *
 * \param mcc: [out] MCC value.
 * \param mnc: [out] MNC value.
 * \return Negative value indicates failure.
 */
int getAttachedNetworkIdentity(int *mcc, int *mnc)
{
    char *tok, *mcc_mnc, *end;
    ATResponse *atresponse = NULL;
    int status = 0;

    status = at_send_command_singleline("AT+COPS=3,2;+COPS?", "+COPS:",
                                        &atresponse);
    if (status < 0)
        goto out;
    if (atresponse->success == 0) {
        status = -1;
        goto out;
    }
    tok = atresponse->p_intermediates->line;

    status = at_tok_start(&tok);
    if (status < 0)
        goto out;

    status = at_tok_nextstr(&tok, &mcc_mnc);
    if (status < 0)
        goto out;

    if (strlen(mcc_mnc) < 5) {
        status = -1;
        goto out;
    }

    if (mnc) {
        *mnc = strtol(&mcc_mnc[3], &end, 10);
        if (*end != '\0') {
            status =  -1;
            goto out;
        }
    }

    mcc_mnc[3] = '\0';
    if (mcc) {
        *mcc = strtol(&mcc_mnc[0], &end, 10);
        if (*end != '\0') {
            status =  -1;
            goto out;
        }
    }

out:
    at_response_free(atresponse);
    return status;
}

/**
 * setupECCListAsyncAdapter: async adapter for enqueueRILEvent()
 */
static void setupECCListAsyncAdapter(void *param)
{
    setupECCList(1);
}

/**
 * RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED
 *
 * Called when modem has send one of registration status update unsolicited
 * results. It can be one of:
 *  *EREG:
 *  +CREG:
 *  +CEREG:
 *  +CGREG:
 *  *EPSB:
 */
void onNetworkStateChanged(const char *s)
{
    /* If roaming to Japan a few extra emergency numbers are required. */
    if (strStartsWith(s, "+CREG:") || strStartsWith(s, "*EREG:")) {

        char buf[16], *tok = buf;
        int status = -1;

        strncpy(buf, s, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;

        if (!at_tok_start(&tok) && !at_tok_nextint(&tok, &status))
            if (status == 5) /* Registred, roaming */
                /* Check for Japan extensions and update ECC list */
                enqueueRILEvent(CMD_QUEUE_AUXILIARY,
                                setupECCListAsyncAdapter, NULL, NULL);
    }

    /* Always send network state change event */
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED,
                              NULL, 0);
}

/**
 * RIL_UNSOL_NITZ_TIME_RECEIVED
 *
 * Called when radio has received a NITZ time message.
 */
void onNetworkTimeReceived(const char *s)
{
    int err = 0;
    char *line, *tok, *response, *tz, *nitz, *timestamp, *dst;

    tok = line = strdup(s);
    err = at_tok_start(&tok);
    if (err < 0)
        goto error;

    if (at_tok_nextstr(&tok, &tz) != 0)
        LOGE("Failed to parse NITZ line %s\n", s);
    else if (at_tok_nextstr(&tok, &nitz) != 0)
        LOGE("Failed to parse NITZ line %s\n", s);
    else if (at_tok_nextstr(&tok, &timestamp) != 0)
        LOGE("Failed to parse NITZ line %s\n", s);
    else if (at_tok_nextstr(&tok, &dst) != 0)
        LOGE("Failed to parse NITZ line %s\n", s);
    else {
        asprintf(&response, "%s%s,%s", nitz + 2, tz, dst);

        RIL_onUnsolicitedResponse(RIL_UNSOL_NITZ_TIME_RECEIVED,
                                  response, sizeof(char *));
        free(response);
    }

error:
    free(line);
}

/**
 * Parser function for the response of CIND and URC +CIEV.
 * pSignalStrength is the output parameter for RIL_SignalStrength sturct.
 * s is the input string that does not include prefix.
 *
 * Returns true if success, false otherwise.
 */
static bool parseSignalStrength(const char *s,
                                RIL_SignalStrength *pSignalStrength)
{
    char *line = NULL;
    char *orig = NULL;
    int err;
    int skip;
    int signalQuality;

    if (s == NULL)
        return false;
    line = strdup(s);
    orig = line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &skip);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &signalQuality);
    if (err < 0)
        goto error;

    /*
     * RIL API specifies the range 0-31 for SignalStrength.
     * In Android GUI handling we find:
     * * * * * * * *  * * * * * * * * * * * * * * * *
     * ASU ranges from 0 to 31 - TS 27.007 Sec 8.5  *
     * if(asu <= 0 || asu == 99) iconLevel = 0;     *
     * else if(asu >= 16) iconLevel = 4;            *
     * else if(asu >= 8) iconLevel = 3;             *
     * else if(asu >= 4) iconLevel = 2;             *
     * else iconLevel = 1;                          *
     * * * * * * *  * * * * * * * * * * * * * * * * *
     * CIEV and CIND gives a range from 0-5.
     * By using the formula (4*CIEV-1) we get the following mapping:
     * CIEV/CIND = 5 -> ASU = 19 -> Lines = 4
     * CIEV/CIND = 4 -> ASU = 15 -> Lines = 3
     * CIEV/CIND = 3 -> ASU = 11 -> Lines = 3
     * CIEV/CIND = 2 -> ASU =  7 -> Lines = 2
     * CIEV/CIND = 1 -> ASU =  3 -> Lines = 1
     * CIEV/CIND = 0 -> ASU =  0 -> Lines = 0
     */
    if (signalQuality > 0) {
        signalQuality *= 4;
        signalQuality--;
    }

    /*
     * Assigning values to RIL structure
     * RIL does not get bit error rate from URC +CIEV, set it to 99,
     * i.e undefined. For case of request signal strength, bitErrorRate
     * will be updated with result from AT+CSQ.
     */
    pSignalStrength->GW_SignalStrength.signalStrength = signalQuality;
    pSignalStrength->GW_SignalStrength.bitErrorRate = 99;

    free(orig);
    return true;

error:
    free(orig);
    LOGE("%s(): Failed to parse singal strength.\n", __func__);
    return false;
}

/**
 * RIL_UNSOL_SIGNAL_STRENGTH
 *
 * Radio may report signal strength rather than have it polled.
 *
 * "data" is a const RIL_SignalStrength *
 */
void unsolSignalStrength(const char *s)
{
    RIL_SignalStrength response;

    if (parseSignalStrength(s, &response))
        RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH,
                                  &response, sizeof(RIL_SignalStrength));
}

/**
 * RIL_UNSOL_SIM_SMS_STORAGE_FULL
 *
 * SIM SMS storage area is full, cannot receive
 * more messages until memory freed
 */
void unsolSimSmsFull(const char *s)
{
    char *line = NULL;

    int err;
    int skip;
    int response;

    line = strdup(s);
    assert(line != NULL);

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &skip);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &response);
    if (err < 0)
        goto error;

    if (response == 1)
        RIL_onUnsolicitedResponse(RIL_UNSOL_SIM_SMS_STORAGE_FULL,
            NULL, 0);
    else
        LOGI("Got indication SIM have SMS memory locations available again. "
             "Ignored");
    free(line);
    return;

error:
    LOGE("Failed to decode SIM SMS Full indication");
    free(line);
}

/**
 * RIL_UNSOL_RESTRICTED_STATE_CHANGED
 */
void onRestrictedStateChanged(const char *s, int *restrictedState)
{
    char *line = NULL;
    char *tok = NULL;
    int state = RIL_RESTRICTED_STATE_NONE;
    int barredCS = -1;
    int barredPS = -1;
    int err = -1;

    /* *EBSRU: <Barred_CS>,<Barred_PS> */

    tok = line = strdup(s);

    err = at_tok_start(&tok);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&tok, &barredCS);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&tok, &barredPS);
    if (err < 0)
        goto error;

    /* set CS restriction */
    switch (barredCS) {
    case BARRING_STATE_UNKNOWN:
    case NO_BARRING:
        state = RIL_RESTRICTED_STATE_NONE;
        break;
    case EMERGENCY_CALLS_BARRED:
        state |= RIL_RESTRICTED_STATE_CS_EMERGENCY;
        break;
    case EMERGENCY_CALLS_ONLY_ALLOWED:
        state |= RIL_RESTRICTED_STATE_CS_NORMAL;
        break;
    case ALL_CALLS_BARRED:
        state |= RIL_RESTRICTED_STATE_CS_ALL;
        break;
    default:
        goto error;
    }

    /* set PS restriction */
    if (barredPS == ALL_CALLS_BARRED)
        state |= RIL_RESTRICTED_STATE_PS_ALL;

finally:
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESTRICTED_STATE_CHANGED,
                              &state, sizeof(int *));
    free(line);
    *restrictedState = state;
    return;

error:
    LOGE("%s: failed to parse %s. Defaulting to"
         " RIL_RESTRICTED_STATE_NONE", __func__, s);
    goto finally;
}

/**
 * RIL_REQUEST_SET_BAND_MODE
 *
 * Assign a specified band for RF configuration.
 */
void requestSetBandMode(void *data, size_t datalen, RIL_Token t)
{
    int bandMode = ((int *) data)[0];

    /* Currently only allow automatic. */
    if (bandMode == 0)
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    else
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

/**
 * RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE
 *
 * Query the list of band mode supported by RF.
 *
 * See also: RIL_REQUEST_SET_BAND_MODE
 */
void requestQueryAvailableBandMode(void *data, size_t datalen, RIL_Token t)
{
    int response[2];

    response[0] = 2;
    response[1] = 0;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
}

/**
 * RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC
 *
 * Specify that the network should be selected automatically.
 */
void requestSetNetworkSelectionAutomatic(void *data, size_t datalen,
                                         RIL_Token t)
{
    int err = 0;
    struct operatorPollParams *poll_params = NULL;

    err = at_send_command("AT+COPS=0", NULL);
    if (err < 0)
        goto error;

    poll_params = malloc(sizeof(struct operatorPollParams));

    poll_params->loopcount = 0;
    poll_params->t = t;

    enqueueRILEvent(CMD_QUEUE_AUXILIARY, pollOperatorSelected,
                    poll_params, &TIMEVAL_OPERATOR_SELECT_POLL);

    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    return;
}

/**
 * RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL
 *
 * Manually select a specified network.
 *
 * The radio baseband/RIL implementation will try to camp on the manually
 * selected network regardless of coverage, i.e. there is no fallback to
 * automatic network selection.
 */
void requestSetNetworkSelectionManual(void *data, size_t datalen,
                                      RIL_Token t)
{
    /*
     * AT+COPS=[<mode>[,<format>[,<oper>[,<AcT>]]]]
     *    <mode>   = 1 = Manual (<oper> field shall be present and AcT
     *                   optionally)
     *    <format> = 2 = Numeric <oper>, the number has structure:
     *                   (country code digit 3)(country code digit 2)
     *                   (country code digit 1)(network code digit 2)
     *                   (network code digit 1)
     */

    int err = 0;
    char *cmd = NULL;
    ATResponse *atresponse = NULL;
    const char *mccMnc = (const char *) data;

    /* Check inparameter. */
    if (mccMnc == NULL)
        goto error;
    /* Build and send command. */
    asprintf(&cmd, "AT+COPS=1,2,\"%s\"", mccMnc);
    err = at_send_command_with_timeout(cmd, &atresponse, COPS_AT_TIMEOUT_MSEC);
    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
finally:

    at_response_free(atresponse);

    if (cmd != NULL)
        free(cmd);

    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/*
 * The parameters of the response to RIL_REQUEST_QUERY_AVAILABLE_NETWORKS are
 * defined in ril.h
 */
#define QUERY_NW_NUM_PARAMS 4

/**
 * RIL_REQUEST_QUERY_AVAILABLE_NETWORKS
 *
 * Scans for available networks.
 */
void requestQueryAvailableNetworks(void *data, size_t datalen, RIL_Token t)
{
    /*
     * AT+COPS=?
     *   +COPS: [list of supported (<stat>,long alphanumeric <oper>
     *           ,short alphanumeric <oper>,numeric <oper>[,<AcT>])s]
     *          [,,(list of supported <mode>s),(list of supported <format>s)]
     *
     *   <stat>
     *     0 = unknown
     *     1 = available
     *     2 = current
     *     3 = forbidden
     */

    int err = 0;
    ATResponse *atresponse = NULL;
    const char *statusTable[] =
    { "unknown", "available", "current", "forbidden" };
    char **responseArray = NULL;
    char *p;
    int n = 0, i = 0, j = 0, numStoredNetworks = 0;
    char *s = NULL;

    err = at_send_command_multiline_with_timeout("AT+COPS=?", "+COPS:",
                                                 &atresponse,
                                                 COPS_AT_TIMEOUT_MSEC);
    if (err < 0 ||
        atresponse->success == 0 || atresponse->p_intermediates == NULL)
        goto error;

    p = atresponse->p_intermediates->line;

    /* count number of '('. */
    err = at_tok_charcounter(p, '(', &n);
    if (err < 0) goto error;

    /* Allocate array of strings, blocks of 4 strings. */
    responseArray = alloca(n * QUERY_NW_NUM_PARAMS * sizeof(char *));

    /* Loop and collect response information into the response array. */
    for (i = 0; i < n; i++) {
        int status = 0;
        char *line = NULL;
        char *longAlphaNumeric = NULL;
        char *shortAlphaNumeric = NULL;
        char *numeric = NULL;
        char *remaining = NULL;
        bool continueOuterLoop = false;

        s = line = getFirstElementValue(p, "(", ")", &remaining);
        p = remaining;

        if (line == NULL) {
            LOGE("Null pointer while parsing COPS response. This should not "
                 "happen.");
            goto error;
        }
        /* <stat> */
        err = at_tok_nextint(&line, &status);
        if (err < 0)
            goto error;

        /* long alphanumeric <oper> */
        err = at_tok_nextstr(&line, &longAlphaNumeric);
        if (err < 0)
            goto error;

        /* short alphanumeric <oper> */
        err = at_tok_nextstr(&line, &shortAlphaNumeric);
        if (err < 0)
            goto error;

        /* numeric <oper> */
        err = at_tok_nextstr(&line, &numeric);
        if (err < 0)
            goto error;

        /*
         * The response of AT+COPS=? returns GSM networks and WCDMA networks as
         * separate network search hits. The RIL API does not support network
         * type parameter and the RIL must prevent duplicates.
         */
        for (j = numStoredNetworks - 1; j >= 0; j--)
            if (strcmp(responseArray[j * QUERY_NW_NUM_PARAMS + 2],
                       numeric) == 0) {
                LOGD("%s(): Skipped storing duplicate operator: %s.",
                     __func__, longAlphaNumeric);
                continueOuterLoop = true;
                break;
            }

        if (continueOuterLoop) {
            free(s);
            s = NULL;
            continue; /* Skip storing this duplicate operator */
        }

        responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 0] =
            alloca(strlen(longAlphaNumeric) + 1);
        strcpy(responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 0],
                             longAlphaNumeric);

        responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 1] =
            alloca(strlen(shortAlphaNumeric) + 1);
        strcpy(responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 1],
                             shortAlphaNumeric);

        responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 2] =
            alloca(strlen(numeric) + 1);
        strcpy(responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 2],
               numeric);

        /* Fill long alpha with MNC/MCC if it is empty */
        if (responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 0] &&
            strlen(responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 0])
            == 0) {
            responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 0] =
                alloca(strlen(responseArray[numStoredNetworks *
                QUERY_NW_NUM_PARAMS + 2]) + 1);
            strcpy(responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 0],
                   responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 2]);
        }
        /* Fill short alpha with MNC/MCC if it is empty */
        if (responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 1]
            && strlen(responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS
            + 1]) == 0) {
            responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 1] =
                alloca(strlen(responseArray[numStoredNetworks *
                QUERY_NW_NUM_PARAMS + 2]) + 1);
            strcpy(responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 1],
                   responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 2]);
        }

        /* Add status */
        responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 3] =
            alloca(strlen(statusTable[status]) + 1);
        sprintf(responseArray[numStoredNetworks * QUERY_NW_NUM_PARAMS + 3],
                "%s", statusTable[status]);

        numStoredNetworks++;
        free(s);
        s = NULL;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseArray, numStoredNetworks *
                          QUERY_NW_NUM_PARAMS * sizeof(char *));
    goto exit;

error:
    free(s);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

exit:
    at_response_free(atresponse);
}

/**
 * RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE
 *
 * Requests to set the preferred network type for searching and registering
 * (CS/PS domain, RAT, and operation mode).
 */
void requestSetPreferredNetworkType(void *data, size_t datalen,
                                    RIL_Token t)
{
    ATResponse *atresponse = NULL;
    int err = 0;
    int rat;
    int arg;
    char *cmd = NULL;
    RIL_Errno errno = RIL_E_GENERIC_FAILURE;

    rat = ((int *) data)[0];

    switch (rat) {
    case 0:
        arg = 1;
        break;
    case 1:
        arg = 5;
        break;
    case 2:
        arg = 6;
        break;
    default:
        errno = RIL_E_MODE_NOT_SUPPORTED;
        goto error;
    }

    asprintf(&cmd, "AT+CFUN=%d", arg);

    err = at_send_command(cmd, &atresponse);
    free(cmd);
    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, errno, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE
 *
 * Query the preferred network type (CS/PS domain, RAT, and operation mode)
 * for searching and registering.
 */
void requestGetPreferredNetworkType(void *data, size_t datalen,
                                    RIL_Token t)
{
    int err = 0;
    int response = 0;
    int cfun;
    char *line;
    ATResponse *atresponse;

    err = at_send_command_singleline("AT+CFUN?", "+CFUN:", &atresponse);
    if (err < 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &cfun);
    if (err < 0)
        goto error;

    assert(cfun >= 0 && cfun < 7);

    switch (cfun) {
    case 5:
        response = 1;
        break;
    case 6:
        response = 2;
        break;
    default:
        response = 0;
        break;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION
 *
 * Requests that network personlization be deactivated.
 */
void requestEnterNetworkDepersonalization(void *data, size_t datalen,
                                          RIL_Token t)
{
    /*
     * AT+CLCK=<fac>,<mode>[,<passwd>[,<class>]]
     *     <fac>    = "PN" = Network Personalization (refer 3GPP TS 22.022)
     *     <mode>   = 0 = Unlock
     *     <passwd> = inparam from upper layer
     */

    int err = 0;
    char *cmd = NULL;
    ATResponse *atresponse = NULL;
    const char *passwd = ((const char **) data)[0];
    RIL_Errno rilerr = RIL_E_GENERIC_FAILURE;
    int num_retries = -1;
    ATCmeError cme_error_code = -1;

    /* Check inparameter. */
    if (passwd == NULL)
        goto error;
    /* Build and send command. */
    asprintf(&cmd, "AT+CLCK=\"PN\",0,\"%s\"", passwd);
    err = at_send_command(cmd, &atresponse);

    free(cmd);

    if (err < 0) {
        goto error;
    }
    if (atresponse->success == 0) {
        if (at_get_cme_error(atresponse, &cme_error_code)) {
            switch (cme_error_code) {
            case CME_INCORRECT_PASSWORD:
                rilerr = RIL_E_PASSWORD_INCORRECT;
                break;

            default:
                break;
            }
        }
        goto error;
    }

    /* TODO: Return number of retries left. */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &num_retries, sizeof(int *));

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, rilerr, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE
 *
 * Query current network selectin mode.
 */
void requestQueryNetworkSelectionMode(void *data, size_t datalen,
                                      RIL_Token t)
{
    int err;
    ATResponse *atresponse = NULL;
    int response = 0;
    char *line;

    err = at_send_command_singleline("AT+COPS?", "+COPS:", &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);

    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &response);

    if (err < 0)
        goto error;

    /*
     * Android accepts 0(automatic) and 1(manual).
     * Modem may return mode 4(Manual/automatic).
     * Convert it to 1(Manual) as android expects.
     */
    if (response == 4)
        response = 1;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));

finally:
    at_response_free(atresponse);
    return;

error:
    LOGE("requestQueryNetworkSelectionMode must never return error when radio "
         "is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * Queries the signal strength with AT+CIND?, retrieves the bit error rate with
 * AT+CSQ, returns true if success, false otherwise.
 * p_signalStrength is the output parameter of RIL_SignalStrength structure.
 */
bool querySignalStrength(RIL_SignalStrength *p_signalStrength)
{
    ATResponse *atresponse = NULL;
    int err, rssi, ber;
    bool res = true;
    char *line;

    /*
     * AT+CIND will give indication on what signal strength we got both for
     * GSM and WCDMA.
     * Android calculates rssi and dBm values from this value, so the dBm
     * value presented in android will be wrong, but this is an error on
     * android's end.
     */
#ifndef LTE_COMMAND_SET_ENABLED
    err = at_send_command_singleline("AT+CIND?", "+CIND:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;

    if (!parseSignalStrength(line, p_signalStrength))
        goto error;

    at_response_free(atresponse);
    atresponse = NULL;
#endif
    /* Retrieve bit error rate from AT+CSQ. */
    err = at_send_command_singleline("AT+CSQ", "+CSQ:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &rssi);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &ber);
    if (err < 0)
        goto error;

#ifdef LTE_COMMAND_SET_ENABLED
    p_signalStrength->GW_SignalStrength.signalStrength = rssi;
#endif
    p_signalStrength->GW_SignalStrength.bitErrorRate = ber;

    goto exit;

error:
    res = false;

exit:
    at_response_free(atresponse);
    return res;
}

/**
 * RIL_REQUEST_SIGNAL_STRENGTH
 *
 * Requests current signal strength and bit error rate.
 *
 * Must succeed if radio is on.
 */
void requestSignalStrength(void *data, size_t datalen, RIL_Token t)
{
    RIL_SignalStrength signalStrength;

    if (querySignalStrength(&signalStrength)) {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &signalStrength,
                              sizeof(RIL_SignalStrength));
    } else {
        LOGE("requestSignalStrength must never return an error "
             "when radio is on");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
}

/**
 * Convert detailedReason from modem to what Android expects.
 * Called in requestRegistrationState().
 */
static Reg_Deny_DetailReason ConvertRegistrationDenyReason(int detailedReason)
{
    Reg_Deny_DetailReason reason;

    switch (detailedReason) {
    case 2:
        reason = IMSI_UNKNOWN_IN_HLR;
        break;
    case 3:
        reason = ILLEGAL_MS;
        break;
    case 6:
        reason = ILLEGAL_ME;
        break;
    case 11:
        reason = PLMN_NOT_ALLOWED;
        break;
    case 12:
        reason = LOCATION_AREA_NOT_ALLOWED;
        break;
    case 13:
        reason = ROAMING_NOT_ALLOWED;
        break;
    case 15:
        reason = NO_SUITABLE_CELL_IN_LOCATION_AREA;
        break;
    case 17:
        reason = NETWORK_FAILURE;
        break;
    case 257:
        reason = AUTHENTICATION_FAILURE;
        break;
    default:
        reason = GENERAL;
        break;
    }

    return reason;
}

/**
 * RIL_REQUEST_REGISTRATION_STATE
 *
 * Request current registration state.
 */
void requestRegistrationState(void *data, size_t datalen, RIL_Token t)
{
    int err = 0;
    int response[15];
    char *responseStr[15];
    ATResponse *atresponse = NULL;
    char *line;
    int commas = 0;
    int skip, tmp;
    int count = 3;
    int detailedReason;
    unsigned int i;
    int getAcT = 0; /* set to 1 to send CGREG to retrieve AcT. */

    /*
     * NOTE: xxxREG UR codes are not subscribed to on this channel. In
     * order to get extended information from xxxREG a subscription
     * needs to be enabled. We turn off extended reporting again
     * when leaving this function.
     */
#ifdef LTE_COMMAND_SET_ENABLED
    (void)at_send_command("AT+CGREG=2;+CREG=2", NULL);
#else
    (void)at_send_command("AT+CGREG=2;*EREG=2", NULL);
#endif

    /* Setting default values in case values are not returned by AT command */
    for (i = 0; i < NUM_ELEMS(responseStr); i++) {
        responseStr[i] = NULL;
    }

    memset(response, 0, sizeof(response));
    response[1] = -1;
    response[2] = -1;

#ifdef LTE_COMMAND_SET_ENABLED
    err = at_send_command_singleline("AT+CREG?", "+CREG:", &atresponse);
#else
    err = at_send_command_singleline("AT*EREG?", "*EREG:", &atresponse);
#endif

    if (err < 0 || atresponse->success == 0 ||
    atresponse->p_intermediates == NULL)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);

    if (err < 0)
        goto error;

    /*
     * The solicited version of the *EREG response is
     * *EREG: n, stat, [lac, cid [,<AcT>]]
     * and the unsolicited version is
     * *EREG: stat, [lac, cid [,<AcT>]]
     * The <n> parameter is basically "is unsolicited creg on?"
     * which it should always be.
     *
     * Now we should normally get the solicited version here,
     * but the unsolicited version could have snuck in
     * so we have to handle both.
     *
     * Also since the LAC, CID and AcT are only reported when
     * registered, we can have 1, 2, 3, 4 or 5 arguments here.
     */

    /* Count number of commas */
    err = at_tok_charcounter(line, ',', &commas);

    if (err < 0)
        goto error;

    switch (commas) {
    case 0:                    /* *EREG: <stat> */
        err = at_tok_nextint(&line, &response[0]);

        if (err < 0)
            goto error;

        break;

    case 1:                    /* *EREG: <n>, <stat> */
        err = at_tok_nextint(&line, &skip);

        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &response[0]);

        if (err < 0)
            goto error;

        break;

    case 2:                    /* *EREG: <stat>, <lac>, <cid> */
        err = at_tok_nextint(&line, &response[0]);

        if (err < 0)
            goto error;

        err = at_tok_nexthexint(&line, &response[1]);

        if (err < 0)
            goto error;

        err = at_tok_nexthexint(&line, &response[2]);

        if (err < 0)
            goto error;

        break;

    case 3:
        err = at_tok_nextint(&line, &tmp);

        if (err < 0)
            goto error;

        /* We need to check if the second parameter is <lac> */
        if (*(line) == '"') { /* *EREG: <stat>, <lac>, <cid>, <AcT> */
            response[0] = tmp; /* <stat> */
            err = at_tok_nexthexint(&line, &response[1]); /* <lac> */

            if (err < 0)
                goto error;

            err = at_tok_nexthexint(&line, &response[2]); /* <cid> */

            if (err < 0)
                goto error;

            err = at_tok_nextint(&line, &response[3]); /* <AcT> */

            if (err < 0)
                goto error;

            count = 4;
            getAcT = 1;
        } else { /* *EREG: <n>, <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &response[0]); /* <stat> */

            if (err < 0)
                goto error;

            err = at_tok_nexthexint(&line, &response[1]); /* <lac> */

            if (err < 0)
                goto error;

            err = at_tok_nexthexint(&line, &response[2]); /* <cid> */

            if (err < 0)
                goto error;
        }

        break;

    case 4:
        err = at_tok_nextint(&line, &tmp); /* <n> */

        if (err < 0)
            goto error;

        /* *EREG: <stat>, <lac>, <cid>, <AcT>, <detailedReason> */
        if (*(line) == '"' || *(line) == ',') {
            response[0] = tmp; /* <stat> */

            /* skip lac, cid and AcT if <stat> is 3, i.e. registration denied. */
            if (tmp == 3) {
                (void)at_tok_nexthexint(&line, &skip); /* <lac> */
                (void)at_tok_nexthexint(&line, &skip); /* <cid> */
                (void)at_tok_nextint(&line, &skip); /* <AcT> */
                err = at_tok_nextint(&line, &detailedReason); /*<detailedReason>*/

                if (err < 0)
                    goto error;

                /* In case of registration denied, set AcT to 0, i.e unknown */
                response[3] = 0;
                count = 14;

            } else {
                /*
                 * modem might return detailedReason if <stat> is 0, 2, 3, 4.
                 * not returning AcT.
                 */
                count = 3;
            }
        } else { /* *EREG: <n>, <stat>, <lac>, <cid>, <AcT> */
            err = at_tok_nextint(&line, &response[0]); /* <stat> */

            if (err < 0)
                goto error;

            err = at_tok_nexthexint(&line, &response[1]); /* <lac> */

            if (err < 0)
                goto error;

            err = at_tok_nexthexint(&line, &response[2]); /* <cid> */

            if (err < 0)
                goto error;

            err = at_tok_nextint(&line, &response[3]); /* <AcT> */

            if (err < 0)
                goto error;

            count = 4;
            getAcT = 1;
        }

        break;
    case 5:     /* *EREG: <n>, <stat>, <lac>, <cid>, <AcT>, <detailedReason> */
        err = at_tok_nextint(&line, &skip); /* <n> */

        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &response[0]); /* <stat> */

        if (err < 0)
            goto error;

        /* skip lac, cid and AcT if registration denied. */
        if (response[0] == 3) {
            (void)at_tok_nexthexint(&line, &skip); /* <lac> */
            (void)at_tok_nexthexint(&line, &skip); /* <cid> */
            (void)at_tok_nextint(&line, &skip); /* <AcT> */
            err = at_tok_nextint(&line, &detailedReason); /*<detailedReason>*/

            if (err < 0)
                goto error;

            /* In case of registration denied, set AcT to 0, i.e Unknown */
            response[3] = 0;
            count = 14;

        } else {
            /*
             * Modem might return detailedReason if <stat> is 0, 2, 3, 4.
             * We should not return AcT.
             */
            count = 3;
        }

        break;
    default:
        LOGE("Invalid input.\r\n");
        goto error;
    }

    /* Update stat value to enable the emergency dialer */
    switch (response[0]) {
    case 0:
        /*
         * 0 - Not registered, MT is not currently searching
         * a new operator to register
         * Converted to
         * 10 - Same as 0, but indicates that emergency calls
         * are enabled.
         */
    case 2:
        /*
         * 2 - Not registered, but MT is currently searching
         * a new operator to register
         * Converted to
         * 12 - Same as 2, but indicates that emergency calls
         * are enabled.
         */
    case 3:
        /*
         * 3 - Registration denied
         * Converted to
         * 13 - Same as 3, but indicates that emergency calls
         * are enabled.
         */
        response[0] += 10;
        break;
    default:
        /* No update */
        break;
    }

    /* Converting to stringlist for Android */
    asprintf(&responseStr[0], "%d", response[0]); /* stat */

    if (count == 14) {
        /* Registration denied with reason received */
        response[13] = ConvertRegistrationDenyReason(detailedReason);
        s_registrationDenyReason = response[13];
        asprintf(&responseStr[3], "%d", response[3]); /* AcT */
        asprintf(&responseStr[13], "%d", response[13]); /* detailedReason */
    } else {
        /* Registered */
        s_registrationDenyReason = DEFAULT_VALUE;

        if (response[1] >= 0)
            asprintf(&responseStr[1], "%04x", response[1]); /* LAC */
        else
            responseStr[1] = NULL;

        if (response[2] >= 0)
            asprintf(&responseStr[2], "%08x", response[2]); /* CID */
        else
            responseStr[2] = NULL;

        if (getAcT) {
            /*
            * Android expects this for response[3]:
            *
            *    0 - Unknown, 1 - GPRS, 2 - EDGE, 3 - UMTS,
            *    4 - IS95A, 5 - IS95B, 6 - 1xRTT,
            *    7 - EvDo Rev. 0, 8 - EvDo Rev. A,
            *    9 - HSDPA, 10 - HSUPA, 11 - HSPA
            *
            * *EREG response:
            *    0 GSM
            *    1 GSM Compact                Not Supported
            *    2 UTRAN
            *
            * +CGREG response:
            *    0 GSM
            *    1 GSM Compact                Not Supported
            *    2 UTRAN
            *    3 GSM w/EGPRS
            *    4 UTRAN w/HSDPA
            *    5 UTRAN w/HSUPA
            *    6 UTRAN w/HSUPA and HSDPA
            *
            * Workaround: Use +CGREG AcT to comply with Android NetworkType
            */
            int networkType;
            /****** WORKAROUND START ******/
            int cgregAcT;
            int actSet = 0;
            ATResponse *atresponse2 = NULL;

            LOGI("Trying to replace network type with CGREG result...");

            err = at_send_command_singleline("AT+CGREG?", "+CGREG:",
                                             &atresponse2);

            if (err < 0 ||
                atresponse2->success == 0 || atresponse2->p_intermediates == NULL)
                goto wa_final;

            line = atresponse2->p_intermediates->line;
            err = at_tok_start(&line);

            if (err < 0)
                goto wa_final;

            /* Count number of commas */
            commas = 0;
            err = at_tok_charcounter(line, ',', &commas);

            if (err < 0)
                goto error;

            /* +CGREG: <n>, <stat>, <lac>, <cid> */
            if (commas == 3) {
                /* +CGREG: <stat>, <lac>, <cid>, <AcT> */
                err = at_tok_nextint(&line, &tmp);

                if (err < 0)
                    goto wa_final;

                /* We need to check if the second parameter is <lac> */
                if (*(line) == '"') {
                    err = at_tok_nexthexint(&line, &skip); /* <lac> */

                    if (err < 0)
                        goto wa_final;

                    err = at_tok_nexthexint(&line, &skip); /* <cid> */

                    if (err < 0)
                        goto wa_final;

                    err = at_tok_nextint(&line, &cgregAcT); /* <AcT> */

                    if (err < 0)
                        goto wa_final;

                    actSet = 1;
                }
            /* +CGREG: <n>, <stat>, <lac>, <cid>, <AcT> */
            } else if (commas == 4) {
                err = at_tok_nextint(&line, &skip); /* <n> */

                if (err < 0)
                    goto wa_final;

                err = at_tok_nextint(&line, &skip); /* <stat> */

                if (err < 0)
                    goto wa_final;

                err = at_tok_nexthexint(&line, &skip); /* <lac> */

                if (err < 0)
                    goto wa_final;

                err = at_tok_nexthexint(&line, &skip); /* <cid> */

                if (err < 0)
                    goto wa_final;

                err = at_tok_nextint(&line, &cgregAcT); /* <AcT> */

                if (err < 0)
                    goto wa_final;

                actSet = 1;
            }

wa_final:
            if (actSet) {
                LOGI("AcT switched from %d to %d", response[3], cgregAcT);
                response[3] = cgregAcT;
            }

            at_response_free(atresponse2);
            /****** WORKAROUND END ******/

            /* Conversion between AT AcT and Android NetworkType */
            switch (response[3]) {
            case CGREG_ACT_GSM:
                networkType = 1;
                break;
            case CGREG_ACT_UTRAN:
                networkType = 3;
                break;
            case CGREG_ACT_GSM_EGPRS:
                networkType = 2;
                break;
            case CGREG_ACT_UTRAN_HSDPA:
                networkType = 9;
                break;
            case CGREG_ACT_UTRAN_HSUPA:
                networkType = 10;
                break;
            case CGREG_ACT_UTRAN_HSUPA_HSDPA:
                networkType = 11;
                break;
            default:
                networkType = 0;
                break;
            }

            /* Available radio technology */
            asprintf(&responseStr[3], "%d", networkType);
        }
    }

#ifndef SUPPORT_FROYO
    if ((response[3] != CGREG_ACT_GSM) &&
        (response[3] != CGREG_ACT_GSM_EGPRS)) {
        err = at_send_command_multiline("AT*EWSCI", "*EWSCI:", &atresponse);

        if (err < 0 ||
            atresponse->success == 0 || atresponse->p_intermediates == NULL)
            goto finally;

        line = atresponse->p_intermediates->line;

        err = at_tok_start(&line);
        if (err < 0)
            goto finally;

        err = at_tok_nextint(&line, &skip);
        if (err < 0)
            goto finally;

        err = at_tok_nextint(&line, &response[14]);
        if (err < 0)
            goto finally;

        if (response[14] >= 0) {
            /* PrimaryScramblingCode */
            asprintf(&responseStr[14], "%04x", response[14]);
            count = 15;
        } else
            responseStr[14] = NULL;
    }
#endif

    /*
     * Note that Ril.h specifies that all 15 bytes of the response is
     * mandatory. However the Android reference RIL cuts the length of the
     * response based on what information is available to report to Android.
     * We are currently following the same way and not returning more than the
     * responses that we have available information for. See "count" below.
     */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr,
                          count * sizeof(char *));

finally:
#ifdef LTE_COMMAND_SET_ENABLED
    (void)at_send_command("AT+CGREG=0;+CREG=0", NULL);
#else
    (void)at_send_command("AT+CGREG=0;*EREG=0", NULL);
#endif

    for (i = 0; i < NUM_ELEMS(responseStr); i++) {
        if (responseStr[i])
            free(responseStr[i]);
    }

    at_response_free(atresponse);
    return;

error:
    LOGE("requestRegistrationState must never return an error when radio is "
         "on.");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_GPRS_REGISTRATION_STATE
 *
 * Request current GPRS registration state.
 */
void requestGprsRegistrationState(void *data, size_t datalen, RIL_Token t)
{
    int err = 0;
    int response[4];
    char *responseStr[4];
    ATResponse *atresponse = NULL;
    char *line, *p;
    int commas = 0;
    int skip, tmp;
    int count = 3;

    /*
     * NOTE: xxxREG UR codes are not subscribed to on this channel. In
     * order to get extended information from xxxREG a subscription
     * needs to be enabled. We turn off extended reporting again
     * when leaving this function.
     */
    (void)at_send_command("AT+CGREG=2", NULL); /* Response not vital */

    memset(responseStr, 0, sizeof(responseStr));
    memset(response, 0, sizeof(response));
    response[1] = -1;
    response[2] = -1;

    err = at_send_command_singleline("AT+CGREG?", "+CGREG: ", &atresponse);
    if (err < 0 || atresponse->success == 0 ||
        atresponse->p_intermediates == NULL) {
        goto error;
    }

    line = atresponse->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;
    /*
     * The solicited version of the *EREG/+CGREG response is
     * +CGREG: n, stat, [lac, cid [,<AcT>]]
     * and the unsolicited version is
     * +CGREG: stat, [lac, cid [,<AcT>]]
     * The <n> parameter is basically "is unsolicited creg on?"
     * which it should always be.
     *
     * Now we should normally get the solicited version here,
     * but the unsolicited version could have snuck in
     * so we have to handle both.
     *
     * Also since the LAC, CID and AcT are only reported when registered,
     * we can have 1, 2, 3, 4 or 5 arguments here.
     */
    /* Count number of commas */
    p = line;
    err = at_tok_charcounter(line, ',', &commas);
    if (err < 0) {
        LOGE("at_tok_charcounter failed.\r\n");
        goto error;
    }

    switch (commas) {
    case 0:                    /* +CGREG: <stat> */
        err = at_tok_nextint(&line, &response[0]);
        if (err < 0) goto error;
        break;

    case 1:                    /* +CGREG: <n>, <stat> */
        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[0]);
        if (err < 0) goto error;
        break;

    case 2:                    /* +CGREG: <stat>, <lac>, <cid> */
        err = at_tok_nextint(&line, &response[0]);
        if (err < 0) goto error;
        err = at_tok_nexthexint(&line, &response[1]);
        if (err < 0) goto error;
        err = at_tok_nexthexint(&line, &response[2]);
        if (err < 0) goto error;
        break;

    case 3:                    /* +CGREG: <n>, <stat>, <lac>, <cid> */
                               /* +CGREG: <stat>, <lac>, <cid>, <AcT> */
        err = at_tok_nextint(&line, &tmp);
        if (err < 0) goto error;

        /* We need to check if the second parameter is <lac> */
        if (*(line) == '"') {
            response[0] = tmp; /* <stat> */
            err = at_tok_nexthexint(&line, &response[1]); /* <lac> */
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]); /* <cid> */
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[3]); /* <AcT> */
            if (err < 0) goto error;
            count = 4;
        } else {
            err = at_tok_nextint(&line, &response[0]); /* <stat> */
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]); /* <lac> */
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]); /* <cid> */
            if (err < 0) goto error;
        }
        break;

    case 4:                    /* +CGREG: <n>, <stat>, <lac>, <cid>, <AcT> */
        err = at_tok_nextint(&line, &skip); /* <n> */
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[0]); /* <stat> */
        if (err < 0) goto error;
        err = at_tok_nexthexint(&line, &response[1]); /* <lac> */
        if (err < 0) goto error;
        err = at_tok_nexthexint(&line, &response[2]); /* <cid> */
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[3]); /* <AcT> */
        if (err < 0) goto error;
        count = 4;
        break;

    default:
        LOGE("Invalid input.\r\n");
        goto error;
    }

    /* Converting to stringlist for Android */
    asprintf(&responseStr[0], "%d", response[0]); /* state */

    if (response[1] >= 0)
        asprintf(&responseStr[1], "%04x", response[1]); /* LAC */
    else
        responseStr[1] = NULL;

    if (response[2] >= 0)
        asprintf(&responseStr[2], "%08x", response[2]); /* CID */
    else
        responseStr[2] = NULL;

    if (count > 3) {
        /*
         * Android expects something like this here:
         *
         *    0 == unknown
         *    1 == GPRS only
         *    2 == EDGE
         *    3 == UMTS
         *    9 == HSDPA
         *    10 == HSUPA
         *    11 == HSPA
         *
         * +CGREG response:
         *    0 GSM
         *    1 GSM Compact (Not Supported)
         *    2 UTRAN
         *    3 GSM w/EGPRS
         *    4 UTRAN w/HSDPA
         *    5 UTRAN w/HSUPA
         *    6 UTRAN w/HSUPA and HSDPA
         */
        int networkType;

        /* Converstion between AT AcT and Android NetworkType */
        switch (response[3]) {
        case CGREG_ACT_GSM:
            networkType = 1;
            break;
        case CGREG_ACT_UTRAN:
            networkType = 3;
            break;
        case CGREG_ACT_GSM_EGPRS:
            networkType = 2;
            break;
        case CGREG_ACT_UTRAN_HSDPA:
            networkType = 9;
            break;
        case CGREG_ACT_UTRAN_HSUPA:
            networkType = 10;
            break;
        case CGREG_ACT_UTRAN_HSUPA_HSDPA:
            networkType = 11;
            break;
        default:
            networkType = 0;
            break;
        }
        /* available radio technology */
        asprintf(&responseStr[3], "%d", networkType);
    }

    /*
     * Note that Ril.h specifies that all 4 values of the response is
     * mandatory. However the Android reference RIL cuts the length of the
     * response based on what information is available to report to Android.
     * We are currently following the same way and not returning more than the
     * responses that we have available information for. See "count" below.
     */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, count * sizeof(char *));

finally:
    (void)at_send_command("AT+CGREG=0", NULL);

    if (responseStr[0])
        free(responseStr[0]);
    if (responseStr[1])
        free(responseStr[1]);
    if (responseStr[2])
        free(responseStr[2]);
    if (responseStr[3])
        free(responseStr[3]);

    at_response_free(atresponse);
    return;

error:
    LOGE("requestRegistrationState must never return an error when radio is "
         "on.");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_OPERATOR
 *
 * Request current operator ONS or EONS.
 */
void requestOperator(void *data, size_t datalen, RIL_Token t)
{
    int err;
    int i;
    int skip;
    int stat;
    char *line = NULL;
    ATLine *cursor = NULL;
    static const int num_resp_lines = 3;
    char *response[num_resp_lines];
    ATResponse *atresponse = NULL;

    memset(response, 0, sizeof(response));

    /* Avoid executing +COPS (slow!) before we are registered. */
#ifdef LTE_COMMAND_SET_ENABLED
    err = at_send_command_singleline("AT+CREG?", "+CREG:", &atresponse);
#else
    err = at_send_command_singleline("AT*EREG?", "*EREG:", &atresponse);
#endif
    /*
     * Don't attempt +COPS if *EREG fails.
     * Android prints an error message if we return RIL_E_GENERIC_FAILURE.
     * NULL strings and SUCCESS are accepted and will result in continuous
     * polling until successful registration or stop if registration is
     * permanently denied.
     */
    if (err < 0 || atresponse->success == 0 ||
        atresponse->p_intermediates == NULL)
        goto finally;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto finally;

    err = at_tok_nextint(&line, &skip);
    if (err < 0)
        goto finally;

    err = at_tok_nextint(&line, &stat);
    if (err < 0 || (stat != 1 && stat != 5))
        goto finally;

    at_response_free(atresponse);
    atresponse = NULL;

    err = at_send_command_multiline
        ("AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?;+COPS=3,2;+COPS?", "+COPS:",
         &atresponse);

    /*
     * We expect 3 lines here:
     * +COPS: 0,0,"T - Mobile"
     * +COPS: 0,1,"TMO"
     * +COPS: 0,2,"310170"
     */

    if (err < 0)
        goto error;

    for (i = 0, cursor = atresponse->p_intermediates;
         cursor != NULL && i < num_resp_lines;
         cursor = cursor->p_next, i++) {
        char *line = cursor->line;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &skip);
        if (err < 0)
            goto error;

        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextint(&line, &skip);
        if (err < 0)
            goto error;

        /* A "+COPS: 0, n" response is also possible. */
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextstr(&line, &(response[i]));
        if (err < 0)
            goto error;
    }

    if (i != num_resp_lines)
        goto error;

    /*
     * Check if modem returned an empty string, and fill it
     * with MNC/MMC if that's the case.
     */
    if (response[2] && response[0] && strlen(response[0]) == 0) {
        response[0] = alloca(strlen(response[2]) + 1);
        strcpy(response[0], response[2]);
    }

    if (response[2] && response[1] && strlen(response[1]) == 0) {
        response[1] = alloca(strlen(response[2]) + 1);
        strcpy(response[1], response[2]);
    }

    goto finally;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto exit;

finally:
    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));

exit:
    at_response_free(atresponse);
    return;
}

/**
 * RIL_REQUEST_SET_LOCATION_UPDATES
 *
 * Enables/disables network state change notifications due to changes in
 * LAC and/or CID (basically, *EREG=2 vs. *EREG=1).
 *
 * Note:  The RIL implementation should default to "updates enabled"
 * when the screen is on and "updates disabled" when the screen is off.
 *
 * See also: RIL_REQUEST_SCREEN_STATE, RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED.
 */
void requestSetLocationUpdates(void *data, size_t datalen, RIL_Token t)
{
    int enable = 0;
    int err = 0;
    char *cmd;
    ATResponse *atresponse = NULL;

    enable = ((int *) data)[0];
    assert(enable == 0 || enable == 1);

#ifdef LTE_COMMAND_SET_ENABLED
    asprintf(&cmd, "AT+CREG=%d", (enable == 0 ? 1 : 2));
#else
    asprintf(&cmd, "AT*EREG=%d", (enable == 0 ? 1 : 2));
#endif
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
 * RIL_REQUEST_NEIGHBORINGCELL_IDS
 */
void requestNeighboringCellIDs(void *data, size_t datalen, RIL_Token t)
{
    int network = -1;
    int dummy = 0;
    char *dummyStr = NULL;
    int err = 0;
    ATResponse *atresponse = NULL;
    char *line = NULL;

    /* Determine GSM or WCDMA */
    err = at_send_command_singleline("AT+COPS?", "+COPS:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &dummy);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &dummy);
    if (err < 0)
        goto error;

    err = at_tok_nextstr(&line, &dummyStr);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &network);
    if (err < 0)
        goto error;

    if (/* GSM */ network == 0)
        GSMNeighboringCellIDs(data, datalen, t);
    else if (/* WCDMA */ network == 2)
        WCDMANeighboringCellIDs(data, datalen, t);
    else
        goto error;

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}
