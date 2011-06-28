/* ST-Ericsson U300 RIL
**
** Copyright (C) ST-Ericsson AB 2008-2010
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
** http://www.apache.org/licenses/LICENSE-2.0
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
** Author: Dmitry Tarnyagin <dmitry.tarnyagin@stericsson.com>
*/

#include <cutils/jstring.h>

#define LOG_TAG "RILV"
#include <utils/Log.h>

#include "u300-ril-oem-parser.h"

namespace u300_ril
{

using android::NO_ERROR;
using android::NO_MEMORY;
using android::BAD_VALUE;
using android::BAD_TYPE;
using android::NOT_ENOUGH_DATA;
using android::NAME_NOT_FOUND;

OemRilParser::OemRilParser()
    : mParcel()
{}

OemRilParser::~OemRilParser()
{}

android::status_t
OemRilParser::parseHeader(uint32_t *msg_id)
{
    status_t status;
    struct u300_ril_oem_hdr hdr;

    if (!msg_id)
        return BAD_VALUE;

    hdr.sig = hdr.msg_id = 0;

    status = readInt32(&hdr.sig);
    if (status != NO_ERROR)
        return status;

    status = readInt32(&hdr.msg_id);
    if (status != NO_ERROR)
        return status;

    if (hdr.sig != U300_RIL_OEM_SIG)
        return BAD_TYPE;

    if (hdr.msg_id >= U300_RIL_OEM_MSG_LAST)
        return NAME_NOT_FOUND;

    *msg_id = hdr.msg_id;
    return NO_ERROR;
}

#ifdef U300_RIL_OEM_MSG_SELFTEST
android::status_t
OemRilParser::parsePing(struct u300_ril_oem_ping_request *request)
{
    status_t status;
    if (!request)
        return BAD_VALUE;

    /* Example: parseXXX shall verify that header was read properly */
    if (mParcel.dataPosition() != sizeof(struct u300_ril_oem_hdr))
        return BAD_VALUE;

    /* Example: deserialization of a string */
    status = readString(&request->val_string);
    if (status != NO_ERROR)
        return status;

    /* Example: deserialization of an int32 */
    status = readInt32(&request->val_i32);

    return status;
}
#endif /* U300_RIL_OEM_MSG_SELFTEST */

android::status_t
OemRilParser::parseUpdateFrequencySubscription(struct u300_ril_oem_frequency_subscription_request *request)
{
    if (!request)
        return BAD_VALUE;

    /* Example: parseXXX shall verify that header was read properly */
    if (mParcel.dataPosition() != sizeof(struct u300_ril_oem_hdr))
        return BAD_VALUE;

    return readInt32(&request->enabled);
}

android::status_t
OemRilParser::parseOpenLogicalChannelRequest(struct
                            u300_ril_oem_open_logical_channel_request *request)
{
    if (!request)
        return BAD_VALUE;
    if (mParcel.dataPosition() != sizeof(struct u300_ril_oem_hdr))
        return BAD_VALUE;
    return readString(&request->application_id_string);
}

android::status_t
OemRilParser::parseCloseLogicalChannelRequest( struct
                            u300_ril_oem_close_logical_channel_request *request)
{
    if (!request)
        return BAD_VALUE;
    if (mParcel.dataPosition() != sizeof(struct u300_ril_oem_hdr))
        return BAD_VALUE;
    return readInt32(&request->channel_session_id);
}

android::status_t
OemRilParser::parseSimCommandRequest( struct
                            u300_ril_oem_sim_command_request *request)
{
    status_t status;
    if (!request)
        return BAD_VALUE;

    if (mParcel.dataPosition() != sizeof(struct u300_ril_oem_hdr))
        return BAD_VALUE;

    status = readInt32(&request->channel_session_id_val_i32);
    if (status != NO_ERROR)
        return status;

    return readString(&request->command_val_string);
}
/* TODO: Implement new parseXXX methods here */

#ifdef U300_RIL_OEM_MSG_SELFTEST
android::status_t
OemRilParser::writePingResponse(const struct u300_ril_oem_ping_response *response)
{
    status_t status;

    status = mParcel.setDataSize(0);
    if (status != NO_ERROR) {
        return status;
    }

    status = writeHeader(U300_RIL_OEM_MSG_PING);
    if (status != NO_ERROR)
        return status;

    /* Example: serialization of a string */
    status = writeString(response->val_string);
    if (status != NO_ERROR)
        return status;

    /* Example: serialization of an int32 */
    status = writeInt32(response->val_i32);
    return status;
}
#endif /* U300_RIL_OEM_MSG_SELFTEST */

android::status_t
OemRilParser::writeNetworkSearchAndSetResponse()
{
    status_t status;

    status = mParcel.setDataSize(0);
    if (status != NO_ERROR)
        return status;

    status = writeHeader(U300_RIL_OEM_MSG_NETWORK_SEARCH_AND_SET);
    return status;
}

android::status_t
OemRilParser::writeFrequencyReportItem(const pairFrequencyReportItem_t &val)
{
    status_t status;
    status = writeInt32(val.frequency);
    if (status != NO_ERROR)
        return status;
    status = writeInt32(val.strength);
    return status;
}

android::status_t
OemRilParser::writeRequestFrequencyReportResponse(const pairFrequencyReportItem_t &pairCurrent,
                                                  const vecFrequencyReport_t &vecNeighbors)
{
    vecFrequencyReport_t::const_iterator it;
    vecFrequencyReport_t::const_iterator end = vecNeighbors.end();
    status_t status;

    status = mParcel.setDataSize(0);
    if (status != NO_ERROR)
        return status;

    status = writeHeader(U300_RIL_OEM_MSG_REQUEST_FREQUENCY_REPORT);
    if (status != NO_ERROR)
        return status;

    status = writeInt32(vecNeighbors.size() + 1);
    if (status != NO_ERROR)
        return status;

    status = writeFrequencyReportItem(pairCurrent);
    if (status != NO_ERROR)
        return status;

    for (it = vecNeighbors.begin(); it != end; ++it) {
        status = writeFrequencyReportItem(*it);
        if (status != NO_ERROR)
            return status;
    }

    return status;
}

android::status_t
OemRilParser::writeUpdateFrequencySubscriptionResponse()
{
    status_t status;

    status = mParcel.setDataSize(0);
    if (status != NO_ERROR)
        return status;

    status = writeHeader(U300_RIL_OEM_MSG_UPDATE_FREQUENCY_SUBSCRIPTION);
    return status;
}

android::status_t
OemRilParser::writeUnsolFrequencyNotification()
{
    status_t status;

    status = mParcel.setDataSize(0);
    if (status != NO_ERROR)
        return status;

    status = writeHeader(U300_RIL_OEM_MSG_UNSOL_FREQUENCY_REPORT);
    return status;
}

android::status_t
OemRilParser::writeOpenLogicalChannelResponse(const struct
                        u300_ril_oem_open_logical_channel_response *response)
{
    status_t status;
    status = mParcel.setDataSize(0);
    if (status != NO_ERROR)
        return status;

    status = writeHeader(U300_RIL_OEM_MSG_OPEN_LOGICAL_CHANNEL);
    if (status != NO_ERROR)
        return status;

    status = writeInt32(response->session_id);
    return status;
}

android::status_t
OemRilParser::writeCloseLogicalChannelResponse()
{
    status_t status;
    status = mParcel.setDataSize(0);
    if (status != NO_ERROR)
        return status;

    status = writeHeader(U300_RIL_OEM_MSG_CLOSE_LOGICAL_CHANNEL);
    return status;
}

android::status_t
OemRilParser::writeSimCommandResponse(const struct
                        u300_ril_oem_sim_command_response *response)
{
    status_t status;
    status = mParcel.setDataSize(0);
    if (status != NO_ERROR)
        return status;

    status = writeHeader(U300_RIL_OEM_MSG_SIM_COMMAND);
    if (status != NO_ERROR)
        return status;

    if (response != NULL)
        status = writeString(response->response_val_string);
    return status;
}
/* TODO: Implement new writeXXX methods here */




/* ***************************************************** */
/* Implementation: privates                              */

android::status_t
OemRilParser::writeHeader(/*in*/ uint32_t msg_id)
{
    status_t status;

    status = writeInt32(U300_RIL_OEM_SIG);
    if (status != NO_ERROR)
        return status;

    status = writeInt32(msg_id);
    return status;
}

android::status_t
OemRilParser::readString(/*out*/ android::String8 *val)
{
    size_t stringlen = 0;
    const char16_t *s16 = NULL;

    if (!val)
        return BAD_VALUE;

    s16 = mParcel.readString16Inplace(&stringlen);
    if (!s16) {
        return NOT_ENOUGH_DATA;
    }

    *val = android::String8(s16, stringlen);

    return NO_ERROR;
}

android::status_t
OemRilParser::writeString(/*in*/ const android::String8 &val)
{
    status_t status;
    char16_t *s16;
    size_t s16_len;

    s16 = strdup8to16(val.string(), &s16_len);
    if (s16) {
        status = mParcel.writeString16(s16, s16_len);
        free(s16);
        return status;
    } else {
        return NO_MEMORY;
    }
}

} /* namespace u300_ril */
