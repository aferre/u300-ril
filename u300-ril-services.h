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

#ifndef U300_RIL_SERVICES_H
#define U300_RIL_SERVICES_H 1

void requestQueryClip(void *data, size_t datalen, RIL_Token t);
void requestCancelUSSD(void *data, size_t datalen, RIL_Token t);
void requestSendUSSD(void *data, size_t datalen, RIL_Token t);
void requestGetCLIR(void *data, size_t datalen, RIL_Token t);
void requestSetCLIR(void *data, size_t datalen, RIL_Token t);
void requestQueryCallForwardStatus(void *data, size_t datalen,
                                   RIL_Token t);
void requestSetCallForward(void *data, size_t datalen, RIL_Token t);
void requestQueryCallWaiting(void *data, size_t datalen, RIL_Token t);
void requestSetCallWaiting(void *data, size_t datalen, RIL_Token t);
void requestSetSuppSvcNotification(void *data, size_t datalen,
                                   RIL_Token t);

void onSuppServiceNotification(const char *s, int type);
void onUSSDReceived(const char *s);

#endif
