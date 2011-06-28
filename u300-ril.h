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
** Author: Sverre Vegge <sverre.vegge@stericsson.com>
*/

#ifndef U300_RIL_H
#define U300_RIL_H 1

#include "telephony/ril.h"
#include <stdbool.h>
#include <pthread.h>

RIL_RadioState getCurrentState(void);
void setRadioState(RIL_RadioState newState);
void getScreenStateLock(void);
bool getScreenState(void);
void setScreenState(bool screenIsOn);
void releaseScreenStateLock(void);
void *queueRunner(void *param);
void signalCloseQueues(void);

/*
 * Maximum number of neighborhood cells
 * 15 is set based on AT specification. It can maximum handle 16 and that
 * includes the current cell, meaning you can have 15 neighbor cells.
 */
#define MAX_NUM_NEIGHBOR_CELLS 15

#define MAX_IFNAME_LEN 16
extern char ril_iface[MAX_IFNAME_LEN];

extern const struct RIL_Env *s_rilenv;
extern const RIL_RadioFunctions g_callbacks;

extern bool managerRelease;
extern pthread_mutex_t ril_manager_wait_mutex;
extern pthread_cond_t ril_manager_wait_cond;

extern pthread_mutex_t ril_manager_queue_exit_mutex;
extern pthread_cond_t ril_manager_queue_exit_cond;

int getRestrictedState(void);

#define RIL_onRequestComplete(t, e, response, responselen) s_rilenv->OnRequestComplete(t, e, response, responselen)
#define RIL_onUnsolicitedResponse(a, b, c) s_rilenv->OnUnsolicitedResponse(a, b, c)

void enqueueRILEvent(int isPrio, void (*callback) (void *param),
                     void *param, const struct timeval *relativeTime);

typedef struct RILRequest {
    int request;
    void *data;
    size_t datalen;
    RIL_Token token;
    struct RILRequest *next;
} RILRequest;

typedef struct RILEvent {
    void (*eventCallback)(void *param);
    void *param;
    struct timespec abstime;
    struct RILEvent *next;
    struct RILEvent *prev;
} RILEvent;

typedef struct RequestQueue {
    pthread_mutex_t queueMutex;
    pthread_cond_t cond;
    RILRequest *requestList;
    RILEvent *eventList;
    char enabled;
    char closed;
} RequestQueue;

typedef struct RILRequestGroup {
    int group;
    char *name;
    int *requests;
    RequestQueue *requestQueue;
} RILRequestGroup;

struct queueArgs {
    char channels;
    RILRequestGroup *group;
    const char *type;
    char *arg;
    char *xarg;
    char index;
};

int parseGroups(char* groups, RILRequestGroup **parsedGroups);

#define RIL_MAX_NR_OF_CHANNELS 2 /* DEFAULT, AUXILIARY */

enum RequestGroups {
    CMD_QUEUE_DEFAULT = 0,
    CMD_QUEUE_AUXILIARY = 1
};

#endif
