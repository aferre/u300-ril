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
** Author: Dmitry Tarnyagin <dmitry.tarnyagin@stericsson.com>
*/

#ifndef U300_RIL_OEM_PARSER_H
#define U300_RIL_OEM_PARSER_H 1

#include <binder/Parcel.h>
#include <sys/endian.h>

#include <vector>

#include "u300-ril-oem-msg.h"

/* private */
namespace u300_ril
{

/** U300 OEM RIL serializer/deserializer */
class OemRilParser
{
    typedef android::status_t status_t;

public:
    typedef struct {
        long frequency;
        long strength;
     } pairFrequencyReportItem_t;
    typedef std::vector<pairFrequencyReportItem_t> vecFrequencyReport_t;

    OemRilParser();
    ~OemRilParser();

    /** Set data buffer
     *
     * This method should be called before any parseXXX methods.
     * OemRilParser makes a private copy of a buffer (according to
     * android::Parcel implementation).
     *
     * \param buffer:   [in] input buffer.
     * \param len:      [in] size of the buffer in bytes.
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NO_MEMORY indicates memory allocation error.
     */
    inline status_t     setData(uint8_t *buffer, size_t len);

    /** Get data buffer
     *
     * This method returns a pointer to the internal data buffer.
     *
     * \retval          Pointer to the internal data buffer.
     */
    inline const uint8_t *data() const;

    /** Get data buffer size
     *
     * This method returns actual size of data in the internal data buffer.
     *
     * \retval          Data size in bytes.
     */
    inline size_t         dataSize() const;

    /** Parse header of U300 OEM RIL message.
     *
     * This method should be called before any other
     * parseXXX methods.
     *
     * \param msg_id:   [out] u300_ril_oem_msg_id of the request.
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          BAD_TYPE indicates invalid U300 OEM signature.
     * \retval          NOT_ENOUGH_DATA indicates buffer underrun.
     * \retval          NAME_NOT_FOUND indicates invalid message id.
     */
    status_t            parseHeader(/*out*/ uint32_t *msg_id);

#ifdef U300_RIL_OEM_MSG_SELFTEST
    /** Parse OEM PING request.
     *
     * \param request:  [out] structure to be filled by the request.
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NOT_ENOUGH_DATA indicates buffer underrun.
     * \retval          NO_MEMORY indicates memory allocation error.
     */
    status_t            parsePing(/*out*/ struct u300_ril_oem_ping_request *request);
#endif /* U300_RIL_OEM_MSG_SELFTEST */

    /** Parse OEM UPDATE_FREQUENCY_SUBSCRIPTION request.
     *
     * \param request:  [out] structure to be filled by the request.
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NOT_ENOUGH_DATA indicates buffer underrun.
     * \retval          NO_MEMORY indicates memory allocation error.
     */
    status_t            parseUpdateFrequencySubscription(/*out*/ struct u300_ril_oem_frequency_subscription_request *request);

    /** Parse OEM OPEN_LOGICAL_CHANNEL request
     *
     * \param request:  [out] structure to be filled by the request.
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NOT_ENOUGH_DATA indicates buffer underrun.
     * \retval          NO_MEMORY indicates memory allocation error.
     */
    status_t            parseOpenLogicalChannelRequest(/*out*/ struct
                            u300_ril_oem_open_logical_channel_request *request);

    /** Parse OEM CLOSE_LOGICAL_CHANNEL request
     *
     * \param request:  [out] structure to be filled by the request.
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NOT_ENOUGH_DATA indicates buffer underrun.
     * \retval          NO_MEMORY indicates memory allocation error.
     */
    status_t            parseCloseLogicalChannelRequest(/*out*/ struct
                           u300_ril_oem_close_logical_channel_request *request);

    /** Parse OEM SIM_COMMAND request
     *
     * \param request:  [out] structure to be filled by the request.
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NOT_ENOUGH_DATA indicates buffer underrun.
     * \retval          NO_MEMORY indicates memory allocation error.
     */
    status_t            parseSimCommandRequest(/*out*/ struct
                            u300_ril_oem_sim_command_request *request);
    /* TODO: Define new parseXXX methods here */



    /** Explicitly reset the parser.
     *
     * \retval          NO_ERROR indicates success.
     */
    inline status_t     reset();

#ifdef U300_RIL_OEM_MSG_SELFTEST
    /** Build OEM PING response.
     *
     * \param response: [in] structure to be serialized.
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NO_MEMORY indicates memory allocation error.
     */
    status_t            writePingResponse(/*in*/ const struct u300_ril_oem_ping_response *response);
#endif /* U300_RIL_OEM_MSG_SELFTEST */

     /** Build OEM NETWORK_SEARCH_AND_SET response.
     *
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NO_MEMORY indicates memory allocation error.
     */
    status_t            writeNetworkSearchAndSetResponse();

     /** Build OEM REQUEST_FREQUENCY_REPORT response.
     *
     * \param           pairCurrent:  current cell
     * \param           vecNeighbors: neighbor cells
     *
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NO_MEMORY indicates memory allocation error.
     */
    status_t            writeRequestFrequencyReportResponse(const pairFrequencyReportItem_t &pairCurrent,
                                                            const vecFrequencyReport_t &vecNeighbors);

     /** Build OEM UPDATE_FREQUENCY_SUBSCRIPTION response.
     *
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NO_MEMORY indicates memory allocation error.
     */
    status_t            writeUpdateFrequencySubscriptionResponse();

     /** Build OEM UNSOL_FREQUENCY_REPORT response.
     *
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NO_MEMORY indicates memory allocation error.
     */
    status_t            writeUnsolFrequencyNotification();

     /** Build OEM OPEN_LOGICAL_CHANNEL response.
     *
     * \param response: [in] structure to be serialized.
     *
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NO_MEMORY indicates memory allocation error.
     */
    status_t            writeOpenLogicalChannelResponse(const struct
                          u300_ril_oem_open_logical_channel_response *response);

     /** Build OEM CLOSE_LOGICAL_CHANNEL response.
     *
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NO_MEMORY indicates memory allocation error.
     */
    status_t            writeCloseLogicalChannelResponse();

    /** Build OEM SIM_COMMAND response.
     *
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NO_MEMORY indicates memory allocation error.
     */
    status_t            writeSimCommandResponse(const struct
                                   u300_ril_oem_sim_command_response *response);
    /* TODO: Define new writeXXX methods here */

private:
    /** No copy semantic*/
    OemRilParser(const OemRilParser &o);
    /** No copy semantic*/
    OemRilParser       &operator=(const OemRilParser &o);

private:
    /** Write an OEM RIL header to the stream.
     *
     * \param msg_id:   [in] Message ID.
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NOT_ENOUGH_DATA indicates buffer underrun.
     */
    status_t            writeHeader(/*in*/ uint32_t msg_id);

    /** Get an integer from the stream.
     *
     * \param val:      [out] Integer value taken from the stream.
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NOT_ENOUGH_DATA indicates buffer underrun.
     */
    status_t            readInt32(/*out*/ uint32_t *val);

    /** Write an integer to the stream.
     *
     * \param val:      [in] Integer value put to the stream.
     * \retval          NO_ERROR indicates success.
     * \retval          NO_MEMORY indicates memory allocation failure.
     */
    status_t            writeInt32(/*in*/ uint32_t val);

    /** Get an string from the stream.
     *
     * \param val:      [out] String value taken from the stream.
     * \retval          NO_ERROR indicates success.
     * \retval          BAD_VALUE indicates invalid argument.
     * \retval          NOT_ENOUGH_DATA indicates buffer underrun.
     */
    status_t            readString(/*out*/ android::String8 *val);

    /** Write a string to the stream.
     *
     * \param val:      [in] String value put to the stream.
     * \retval          NO_ERROR indicates success.
     * \retval          NO_MEMORY indicates memory allocation failure.
     */
    status_t            writeString(/*in*/ const android::String8 &val);

    /** Write a frequency report item to the stream.
     *
     * \param val:      [in] Frequency report item put to the stream.
     * \retval          NO_ERROR indicates success.
     * \retval          NO_MEMORY indicates memory allocation failure.
     */
    status_t            writeFrequencyReportItem(const pairFrequencyReportItem_t &val);
private:
    android::Parcel     mParcel;
};

/* Implementation: inline delegates */

inline android::status_t OemRilParser::setData(uint8_t *buffer, size_t len)
{
    return mParcel.setData(buffer, len);
}

inline const uint8_t *OemRilParser::data() const
{
    return mParcel.data();
}

inline size_t OemRilParser::dataSize() const
{
    return mParcel.dataSize();
}

inline android::status_t OemRilParser::reset()
{
    return mParcel.setDataSize(0);
}

inline android::status_t OemRilParser::readInt32(/*out*/ uint32_t *val)
{
    return mParcel.readInt32((int32_t *)val);
}

inline android::status_t OemRilParser::writeInt32(/*in*/ uint32_t val)
{
    return mParcel.writeInt32(val);
}

} /* namespace u300_ril */

#endif
