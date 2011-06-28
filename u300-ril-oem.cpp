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
#include <telephony/ril.h>
#include <utils/Errors.h>
#include <utils/String8.h>

#define LOG_TAG "RILV"
#include <utils/Log.h>

#include "u300-ril.h"
#include "u300-ril-oem.h"
#include "u300-ril-oem-parser.h"
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include <stdlib.h>

using android::NO_ERROR;
using android::BAD_VALUE;
using android::NAME_NOT_FOUND;
using android::String8;

enum FrequencySubscriptionType
{
    FREQUENCY_SUBSCRIPTION_OFF,
    FREQUENCY_SUBSCRIPTION_ON,
    FREQUENCY_SUBSCRIPTION_QUERY,
};

static android::status_t handleOemPing(
                         u300_ril::OemRilParser &parser, RIL_Errno *ril_errno);
static android::status_t handleOemNetworkSearchAndSet(
                         u300_ril::OemRilParser &parser, RIL_Errno *ril_errno);
static android::status_t handleOemRequestFrequencyReport(
                         u300_ril::OemRilParser &parser, RIL_Errno *ril_errno);
static android::status_t handleOemUpdateFrequencySubscription(
                         u300_ril::OemRilParser &parser, RIL_Errno *ril_errno);
static android::status_t handleOemRequestOpenLogicalChannel(
                         u300_ril::OemRilParser &parser, RIL_Errno *ril_errno);
static android::status_t handleOemRequestCloseLogicalChannel (
                         u300_ril::OemRilParser &parser, RIL_Errno *ril_errno);
static android::status_t handleOemRequestSimCommand (
                         u300_ril::OemRilParser &parser, RIL_Errno *ril_errno);

static void onFrequencyNotification(const char *str);

static android::status_t updateFrequencySubscription(ATResponse **atresponse,
                         enum FrequencySubscriptionType status);
static int parseFrequencyNotification(const char *str,
        u300_ril::OemRilParser::pairFrequencyReportItem_t &frequencyReportItem);

/**
 * RIL_REQUEST_OEM_HOOK_RAW
 *
 * This request reserved for OEM-specific uses. It passes raw byte arrays
 * back and forth.
 */
void requestOEMHookRaw(void *data, size_t datalen, RIL_Token t)
{
    u300_ril::OemRilParser parser;
    android::status_t status;
    RIL_Errno ril_errno = RIL_E_SUCCESS;
    uint32_t msg_id;

    do {
        status = parser.setData(static_cast<uint8_t*>(data), datalen);
        if (status != NO_ERROR)
            break;

        status = parser.parseHeader(&msg_id);
        if (status != NO_ERROR)
            break;

        switch (msg_id) {
#ifdef U300_RIL_OEM_MSG_SELFTEST
        case U300_RIL_OEM_MSG_PING:
            status = handleOemPing(parser, &ril_errno);
            break;
#endif /* U300_RIL_OEM_MSG_SELFTEST */
        case U300_RIL_OEM_MSG_NETWORK_SEARCH_AND_SET:
            status = handleOemNetworkSearchAndSet(parser, &ril_errno);
            break;

        case U300_RIL_OEM_MSG_REQUEST_FREQUENCY_REPORT:
            status = handleOemRequestFrequencyReport(parser, &ril_errno);
            break;

        case U300_RIL_OEM_MSG_UPDATE_FREQUENCY_SUBSCRIPTION:
            status = handleOemUpdateFrequencySubscription(parser, &ril_errno);
            break;

        case U300_RIL_OEM_MSG_OPEN_LOGICAL_CHANNEL:
            status = handleOemRequestOpenLogicalChannel(parser, &ril_errno);
            break;

        case U300_RIL_OEM_MSG_CLOSE_LOGICAL_CHANNEL:
            status = handleOemRequestCloseLogicalChannel(parser, &ril_errno);
            break;
        case U300_RIL_OEM_MSG_SIM_COMMAND:
            status = handleOemRequestSimCommand(parser, &ril_errno);
            break;
        default:
            status = NAME_NOT_FOUND;
            break;
        }
    } while (false);

    if (status == NO_ERROR) {
        RIL_onRequestComplete(t, ril_errno, const_cast<uint8_t*>(parser.data()),
                              parser.dataSize());
    } else {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
    return;
}

/**
 * RIL_REQUEST_OEM_HOOK_STRINGS
 *
 * This request reserved for OEM-specific uses. It passes strings
 * back and forth.
 */
void requestOEMHookStrings(void *data, size_t datalen, RIL_Token t)
{
    int i;
    const char **cur;

    for (i = (datalen / sizeof(char *)), cur = (const char **) data;
         i > 0; cur++, i--)
        LOGD("> '%s'", *cur);

    /* Echo back strings. */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
    return;
}

/**
 * Hook for unsolicited responses processing.
 */
void onOemUnsolHook(const char *s)
{
    if (strStartsWith(s, "*EFBR:")) {
        onFrequencyNotification(s);
    }
    // TODO: add your unsolicited handler calls here.
}

/**
 * Handler for *EFBR unsolicited response.
 */
static void onFrequencyNotification(const char *str)
{
    u300_ril::OemRilParser parser;
    parser.writeUnsolFrequencyNotification();
    RIL_onUnsolicitedResponse(RIL_UNSOL_OEM_HOOK_RAW,
                              const_cast<uint8_t*>(parser.data()),
                              parser.dataSize());
    return;
}

// TODO: add your unsolicited handlers here.



#ifdef U300_RIL_OEM_MSG_SELFTEST
/**
 * OEM PING handler.
 *
 * This function is an example of OEM RIL command handler. It expects "PING" in
 * u300_ril_oem_ping_request::val_string and returns "PONG" in
 * u300_ril_oem_ping_response::val_string. Also it negates value of the val_i32
 * field.
 */
static android::status_t handleOemPing(u300_ril::OemRilParser &parser,
                                       RIL_Errno *ril_errno)
{
    android::status_t status;
    static String8 strPing("PING");
    static String8 strPong("PONG");
    struct u300_ril_oem_ping_request req;
    struct u300_ril_oem_ping_response resp;

    status = parser.parsePing(&req);
    if (status != NO_ERROR)
        return status;

    if (req.val_string != strPing)
        *ril_errno = RIL_E_MODE_NOT_SUPPORTED;

    resp.val_string = strPong;
    resp.val_i32 = -req.val_i32;

    status = parser.writePingResponse(&resp);

    return status;
}
#endif /* U300_RIL_OEM_MSG_SELFTEST */

/**
 * OEM NETWORK_SEARCH_AND_SET handler
 */
static android::status_t handleOemNetworkSearchAndSet(
                         u300_ril::OemRilParser &parser, RIL_Errno *ril_errno)
{
    int status;
    ATResponse *atresponse = NULL;

    status = at_send_command("AT*EICS", &atresponse);
    if (status < 0 || atresponse->success == 0)
        *ril_errno = RIL_E_GENERIC_FAILURE;
    at_response_free(atresponse);

    return parser.writeNetworkSearchAndSetResponse();
}

/**
 * *EFBRS / *EFBRN parser helper function.
 *
 * \param str: [in] *EFBR[SN] response line to be parsed.
 * \param frequencyReportItem: [out] frequency report item struct to be filled
 *                             by parsed data.
 * \return status of the parsing. Negative value indicates failure.
 */
static int parseFrequencyNotification(const char *str,
        u300_ril::OemRilParser::pairFrequencyReportItem_t &frequencyReportItem)
{
    char *tok, *line;
    int channel, frequency, strength;
    int status;

    line = tok = strdup(str);

    /* +EFBRS: <channel>,<frequency>,<strength> */
    status = at_tok_start(&tok);
    if (status < 0)
        goto out;

    status = at_tok_nextint(&tok, &channel);
    if (status < 0)
        goto out;
    status = at_tok_nextint(&tok, &frequency);
    if (status < 0)
        goto out;
    status = at_tok_nextint(&tok, &strength);
    if (status < 0)
        goto out;

    frequencyReportItem.frequency = frequency;
    frequencyReportItem.strength = strength;
out:
    free(line);
    return status;
}

/**
 * OEM REQUEST_FREQUENCY_REPORT handler
 */
static android::status_t handleOemRequestFrequencyReport(
                         u300_ril::OemRilParser &parser, RIL_Errno *ril_errno)
{
    u300_ril::OemRilParser::pairFrequencyReportItem_t pairCurrent;
    u300_ril::OemRilParser::vecFrequencyReport_t vecNeighbors;
    ATResponse *atresponse = NULL;
    ATLine *p_line;
    int status;

    pairCurrent.frequency = 0;
    pairCurrent.strength = 0;

    status = at_send_command_multiline("AT*EFBR?", "*EFBR", &atresponse);
    if (status < 0 || atresponse->success == 0) {
        *ril_errno = RIL_E_GENERIC_FAILURE;
    } else {
        for (p_line = atresponse->p_intermediates;
             p_line; p_line = p_line->p_next) {
            if (strStartsWith(p_line->line, "*EFBRS:")) {
                status = parseFrequencyNotification(p_line->line, pairCurrent);
            } else if (strStartsWith(p_line->line, "*EFBRN:")) {
                u300_ril::OemRilParser::pairFrequencyReportItem_t pairTemp;
                status = parseFrequencyNotification(p_line->line, pairTemp);
                vecNeighbors.push_back(pairTemp);
            }
            if (status < 0) {
                *ril_errno = RIL_E_GENERIC_FAILURE;
                break;
            }
        }
    }

    at_response_free(atresponse);
    return parser.writeRequestFrequencyReportResponse(pairCurrent, vecNeighbors);
}

/**
 * OEM UPDATE_FREQUENCY_SUBSCRIPTION handler
 */
static android::status_t handleOemUpdateFrequencySubscription(
                         u300_ril::OemRilParser &parser, RIL_Errno *ril_errno)
{
    int status;
    const char *cmd;
    u300_ril_oem_frequency_subscription_request req;
    ATResponse *atresponse = NULL;

    status = parser.parseUpdateFrequencySubscription(&req);
    if (status != NO_ERROR)
        return status;

    if (req.enabled)
        cmd = "AT*EFBR=1";
    else
        cmd = "AT*EFBR=0";

    status = at_send_command_multiline(cmd, "*EFBR", &atresponse);
    if (status < 0 || atresponse->success == 0)
        *ril_errno = RIL_E_GENERIC_FAILURE;
    at_response_free(atresponse);

    return parser.writeUpdateFrequencySubscriptionResponse();
}

/**
 * OEM OPEN_LOGICAL_CHANNEL handler
 */
static android::status_t handleOemRequestOpenLogicalChannel(
                         u300_ril::OemRilParser &parser, RIL_Errno *ril_errno)
{
    int status = 0;
    char *cmd = NULL;
    char *line = NULL;
    int session_id = 0;
    u300_ril_oem_open_logical_channel_response response;
    u300_ril_oem_open_logical_channel_request req;
    ATResponse *atresponse = NULL;
    char *application_id_string_as_hex;
    char *curr;
    int i;

    status = parser.parseOpenLogicalChannelRequest(&req);
    if (status != NO_ERROR)
        return status;

    asprintf(&cmd,"AT+CCHO=\"%s\"", req.application_id_string.string());
    status = at_send_command_singleline(cmd, "+CCHO:", &atresponse);
    if (status < 0 || atresponse->success == 0) {
        goto error;
    }

    line = atresponse->p_intermediates->line;
    status = at_tok_start(&line);
    if (status < 0)
        goto error;

    status = at_tok_nextint(&line, &session_id);
    if (status < 0)
        goto error;

    goto exit;
error:
    *ril_errno = RIL_E_GENERIC_FAILURE;
exit:
    response.session_id = session_id;
    at_response_free(atresponse);
    free(cmd);

    return parser.writeOpenLogicalChannelResponse(&response);
}

/**
 * OEM CLOSE_LOGICAL_CHANNEL handler
 */
static android::status_t handleOemRequestCloseLogicalChannel(
                         u300_ril::OemRilParser &parser, RIL_Errno *ril_errno)
{
    int status = 0;
    char *cmd = NULL;
    ATResponse *atresponse = NULL;
    u300_ril_oem_close_logical_channel_request req;

    status = parser.parseCloseLogicalChannelRequest(&req);
    if (status != NO_ERROR)
        return status;

    asprintf(&cmd, "AT+CCHC=%d", req.channel_session_id);
    status = at_send_command(cmd, &atresponse);
    if (status < 0 || atresponse->success == 0)
        *ril_errno = RIL_E_GENERIC_FAILURE;

    at_response_free(atresponse);
    free(cmd);

    return parser.writeCloseLogicalChannelResponse();
}

/**
 * OEM SIM_COMMAND handler
 */
static android::status_t handleOemRequestSimCommand(
                         u300_ril::OemRilParser &parser, RIL_Errno *ril_errno)
{
    int status = 0;
    int resplen = 0;
    char *resp = NULL;
    char *cmd = NULL;
    char *line = NULL;
    unsigned char sw1, sw2;
    ATResponse *atresponse = NULL;
    ATCmeError cmeError;
    u300_ril_oem_sim_command_request req;
    u300_ril_oem_sim_command_response *response = NULL;

    status = parser.parseSimCommandRequest(&req);
    if (status != NO_ERROR)
        return status;

    asprintf(&cmd, "AT+CGLA=%d, %d, \"%s\"",
             req.channel_session_id_val_i32,
             req.command_val_string.length(),
             req.command_val_string.string());
    status = at_send_command_singleline(cmd, "+CGLA:", &atresponse);
    if (status < 0)
        goto error;

    if (atresponse->success == 0) {
        if (at_get_cme_error(atresponse, &cmeError)) {
            if (cmeError == CME_SIM_FAILURE)
                /*
                 * Modem returns CME_SIM_FAILURE if channel session id is not
                 * valid, this happens when modem has been restarted silently
                 * without application noticed. In this case, application is
                 * supposed to re-open the channel to start over.
                 */
                LOGI("Sim command failed probably due to session id is "
                     "invalid, the modem may have been restarted silently.\n");
        }
        goto error;
    }

    line = atresponse->p_intermediates->line;

    status = at_tok_start(&line);
    if (status < 0)
        goto error;

    status = at_tok_nextint(&line, &resplen);
    if (status < 0)
        goto error;

    status = at_tok_nextstr(&line, &resp);
    if (status < 0)
        goto error;

    if ((resplen < 4) || ((size_t)resplen != strlen(resp))) {
        status = -1;
        goto error;
    }

    resp[resplen - 4] = 0;
    response->response_val_string = resp;
    goto exit;

error:
    *ril_errno = RIL_E_GENERIC_FAILURE;
exit:
    at_response_free(atresponse);
    free(cmd);
    return parser.writeSimCommandResponse(response);
}