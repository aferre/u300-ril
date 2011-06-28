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

#ifndef U300_RIL_OEM_MSG_H
#define U300_RIL_OEM_MSG_H 1

#include "utils/String8.h"

#define U300_RIL_OEM_SIG        (0x73742D65) /*'st-e'*/

struct u300_ril_oem_hdr {
    uint32_t sig;                /* = U300_RIL_OEM_SIG */
    uint32_t msg_id;
};

/* Needs to be in sync with com.stericsson.telephony.OemRil */
enum u300_ril_oem_msg_id {
    U300_RIL_OEM_MSG_UNSOL_FREQUENCY_REPORT         = -3,

    U300_RIL_OEM_MSG_PING                           = 1,
    U300_RIL_OEM_MSG_NETWORK_SEARCH_AND_SET         = 2,
    U300_RIL_OEM_MSG_REQUEST_FREQUENCY_REPORT       = 3,
    U300_RIL_OEM_MSG_UPDATE_FREQUENCY_SUBSCRIPTION  = 4,
    U300_RIL_OEM_MSG_OPEN_LOGICAL_CHANNEL           = 5,
    U300_RIL_OEM_MSG_CLOSE_LOGICAL_CHANNEL          = 6,
    U300_RIL_OEM_MSG_SIM_COMMAND                    = 7,
    U300_RIL_OEM_MSG_LAST, /* Should be last */
};


#ifdef U300_RIL_OEM_MSG_SELFTEST
/* ping */

struct u300_ril_oem_ping_request {
    android::String8  val_string;
    uint32_t val_i32;
};

struct u300_ril_oem_ping_response {
    android::String8  val_string;
    uint32_t val_i32;
};
#endif /* #ifdef U300_RIL_OEM_MSG_SELFTEST */

/* network_search_and_set */
/* network_search_and_set_request has no arguments */
/* network_search_and_set_request has no return value */

/* frequency subscription */

struct u300_ril_oem_frequency_subscription_request {
    uint32_t enabled;
};

struct u300_ril_oem_open_logical_channel_request {
    android::String8 application_id_string;
};

struct u300_ril_oem_open_logical_channel_response {
    uint32_t session_id;
};

struct u300_ril_oem_close_logical_channel_request {
    uint32_t channel_session_id;
};

struct u300_ril_oem_sim_command_request {
    uint32_t channel_session_id_val_i32;
    android::String8 command_val_string;
};

struct u300_ril_oem_sim_command_response {
    android::String8 response_val_string;
};
#endif
