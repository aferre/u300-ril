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

#ifndef U300_RIL_SIM_H
#define U300_RIL_SIM_H 1

#define PROP_EMERGENCY_LIST_RO ("ro.ril.ecclist")
#define PROP_EMERGENCY_LIST_RW ("ril.ecclist")

void onSimStateChanged(const char *s);

void requestGetSimStatus(void *data, size_t datalen, RIL_Token t);
void requestSIM_IO(void *data, size_t datalen, RIL_Token t);
void requestEnterSimPin(void *data, size_t datalen, RIL_Token t, int request);
void requestChangeSimPin(void *data, size_t datalen, RIL_Token t, int request);
void requestChangeSimPin2(void *data, size_t datalen, RIL_Token t, int request);
void requestChangeBarringPassword(void *data, size_t datalen, RIL_Token t, int request);
void requestSetFacilityLock(void *data, size_t datalen, RIL_Token t);
void requestQueryFacilityLock(void *data, size_t datalen, RIL_Token t);

bool isSimAbsent();
void pollSIMState(void *param);

void setupECCList(int check_attached_network);

#endif
