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
**  Author: Sjur Brendeland <sjur.brandeland@stericsson.com>
*/

#ifndef U300_RIL_NETIF_H
#define U300_RIL_NETIF_H 1

/**
 * Returns 0 on success, sets errno and returns negative on error.
 * *ifindex is set on success, but not modified on error.
 * Note ifname is in/out and must be minimum size MAX_IFNAME_LEN.
 */
int rtnl_create_caif_interface(int type, int conn_id, char *ifname,
                               int *ifindex, char loop);

/**
 * Returns 0 on success, sets errno and returns negative on error.
 */
int rtnl_delete_caif_interface(int ifindex,char * ifname);

#endif