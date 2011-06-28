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

#ifndef U300_RIL_PDP_H
#define U300_RIL_PDP_H 1

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RIL will use Cid's counting from "STARTING CID" and up to
 * STARTING_CID" + "MAX_NUMBER_OF_PDP_CONTEXTS" - 1. E.g. 1+6-1 => 1,2,3,4,5,6
 * PS: If changing MAX number also change the initialization in u300-ril-pdp.c
 */
#define RIL_FIRST_CID_INDEX                 1   /* Note: must be > 0 */
#define RIL_MAX_NUMBER_OF_PDP_CONTEXTS      6

void onPDPContextListChanged(void *param);
void requestPDPContextList(void *data, size_t datalen, RIL_Token t);
void requestSetupDataCall(void *data, size_t datalen, RIL_Token t);
void requestDeactivateDataCall(void *data, size_t datalen, RIL_Token t);
void requestLastPDPFailCause(void *data, size_t datalen, RIL_Token t);

void onEPSBReceived(const char *s);

/*
 * Used to set OEM's framework callback to deactivated PDP context indications.
 *
 * When callback is issued the specific profileId entry will already have been
 * removed from PDP Context lists and in the modem. The callback is always
 * invoked on the command thread so further AT sendcommand() is allowed.
 */
void pdpSetOnOemDeactivated(void (*onOemDeactivated)(int profileId));

bool pdpListExist(const char *cidToFind, const char *profileToFind,
                  int *cid, int *profile, char ifName[], int *active, int *oem);
int  pdpListGetFree(int *cid, char ifName[]);
int  pdpListGet(const char *cidToFind, const char *profileIdToFind,
                int *cid, int *profile, char ifName[], char **apn, int *active,
                int *oem);
bool pdpListPut(int pdpListHandle, int profile, const char *apn, int activated,
                int oem);
bool pdpListFree(int pdpListHandle);
bool pdpListUndo(int pdpListHandle);

#ifdef __cplusplus
}
#endif
#endif
