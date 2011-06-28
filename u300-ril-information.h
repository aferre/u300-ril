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
#ifndef _U300_RIL_INFORMATION_H
#define _U300_RIL_INFORMATION_H 1

#include "atchannel.h"

void requestGetIMSI(void *data, size_t datalen, RIL_Token t);
void requestDeviceIdentity(void *data, size_t datalen, RIL_Token t);
void requestGetIMEI(void *data, size_t datalen, RIL_Token t);
void requestGetIMEISV(void *data, size_t datalen, RIL_Token t);
void requestRadioPower(void *data, size_t datalen, RIL_Token t);
void requestScreenState(void *data, size_t datalen, RIL_Token t);
void requestBasebandVersion(void *data, size_t datalen, RIL_Token t);

void pollAndDispatchSignalStrength(void *param);
#endif
