/*
 * ST-Ericsson U300 RIL
 *
 * Copyright (C) ST-Ericsson AB 2008-2011
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
 * Heavily modified for ST-Ericsson modems.
 * Author: Christian Bejram <christian.bejram@stericsson.com>
 * Author: Sverre Vegge <sverre.vegge@stericsson.com>
 */

#include <cutils/properties.h>
#include <telephony/ril.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <alloca.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cutils/sockets.h>
#include <termios.h>
#include <stdbool.h>
#ifndef CAIF_SOCKET_SUPPORT_DISABLED
#include <linux/errno.h>
#include <linux/caif/caif_socket.h>
#include <linux/rtnetlink.h>
#endif

#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"

#include "u300-ril.h"
#include "u300-ril-callhandling.h"
#include "u300-ril-messaging.h"
#include "u300-ril-network.h"
#include "u300-ril-pdp.h"
#include "u300-ril-services.h"
#include "u300-ril-sim.h"
#include "u300-ril-stk.h"
#include "u300-ril-oem.h"
#include "u300-ril-requestdatahandler.h"
#include "u300-ril-audio.h"
#include "u300-ril-information.h"

#define LOG_TAG "RILV"
#include <utils/Log.h>

#define RIL_VERSION_STRING  "ST-Ericsson u300-ril Gingerbread"

#define timespec_cmp(a, b, op)   \
    ((a).tv_sec == (b).tv_sec    \
     ? (a).tv_nsec op(b).tv_nsec \
     : (a).tv_sec op(b).tv_sec)


bool managerRelease = false;
pthread_mutex_t ril_manager_wait_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ril_manager_wait_cond = PTHREAD_COND_INITIALIZER;

pthread_mutex_t ril_manager_queue_exit_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ril_manager_queue_exit_cond = PTHREAD_COND_INITIALIZER;

/*** Declarations ***/
static void onRequest(int request, void *data, size_t datalen,
                      RIL_Token t);
static int supports(int requestCode);
static void onCancel(RIL_Token t);
static const char *getVersion(void);
static int isRadioOn(void);
static void signalManager(void);
extern const char *requestToString(int request);

static RIL_RadioState onStateRequest(void);

/*** Static Variables ***/
const RIL_RadioFunctions g_callbacks = {
    RIL_VERSION,
    onRequest,
    onStateRequest,
    supports,
    onCancel,
    getVersion
};

char ril_iface[MAX_IFNAME_LEN] = "";
const struct RIL_Env *s_rilenv;

static RIL_RadioState s_state = RADIO_STATE_UNAVAILABLE;
static int s_restrictedState = RIL_RESTRICTED_STATE_NONE;

static pthread_mutex_t s_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_screen_state_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool s_screenState = true;

static RequestQueue s_requestQueueDefault = {
    .queueMutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .requestList = NULL,
    .eventList = NULL,
    .enabled = 0,
    .closed = 1
};

static RequestQueue s_requestQueueAuxiliary = {
    .queueMutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .requestList = NULL,
    .eventList = NULL,
    .enabled = 0,
    .closed = 1
};

static RequestQueue *s_requestQueues[] = {
    &s_requestQueueDefault,
    &s_requestQueueAuxiliary
};

#define RIL_REQUEST_LAST_ELEMENT 0xFFFF

/*
 * Groups of requests that will go on a dedicated queue
 * instead of the auxiliary queue.
 */

static int defaultRequests[] = {
    RIL_REQUEST_SCREEN_STATE,
    RIL_REQUEST_SMS_ACKNOWLEDGE,
    RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION,
    RIL_REQUEST_LAST_ELEMENT
};

static RILRequestGroup RILRequestGroups[] = {
    {CMD_QUEUE_DEFAULT, "DEFAULT", defaultRequests, &s_requestQueueDefault},
    {CMD_QUEUE_AUXILIARY, "AUXILIARY", NULL, &s_requestQueueAuxiliary}
};

void enqueueRILEventOnList(RequestQueue* q, RILEvent* e)
{
    int err;

    if ((err = pthread_mutex_lock(&q->queueMutex)) != 0) {
        LOGE("%s() failed to take queue mutex: %s!", __func__, strerror(err));
        assert(0);
    }
    if (q->eventList == NULL)
        q->eventList = e;
    else {
        if (timespec_cmp(q->eventList->abstime, e->abstime, >)) {
            e->next = q->eventList;
            q->eventList->prev = e;
            q->eventList = e;
        } else {
            RILEvent *tmp = q->eventList;
            do {
                if (timespec_cmp(tmp->abstime, e->abstime, >)) {
                    tmp->prev->next = e;
                    e->prev = tmp->prev;
                    tmp->prev = e;
                    e->next = tmp;
                    break;
                } else if (tmp->next == NULL) {
                    tmp->next = e;
                    e->prev = tmp;
                    break;
                }
                tmp = tmp->next;
            } while (tmp);
        }
    }

    if ((err = pthread_cond_broadcast(&q->cond)) != 0)
        LOGE("%s() failed to take broadcast queue update: %s!",
            __func__, strerror(err));

    if ((err = pthread_mutex_unlock(&q->queueMutex)) != 0)
        LOGE("%s() failed to release queue mutex: %s!",
            __func__, strerror(err));
}

/*
 * Enqueue a RIL event on an event queue.
 * Each QueueRunner thread has one request and one event queue.
 *
 * When DEFAULT and AUXILIARY groups are enabled the DEFAULT AT channel
 * shall not be blocked by slow AT commnds. Events posted on the DEFAULT
 * queue must execute AT commands that gives immediate response.
 * Non-prioritized events are typically put on the AUXILIARY queue,
 * which may be temporarily blocked by "slow" AT commands.
 */
void enqueueRILEvent(int eventQueue, void (*callback)(void *param),
                     void *param, const struct timeval *relativeTime)
{
    struct timeval tv;

    RILEvent *e = malloc(sizeof(RILEvent));

    e->eventCallback = callback;
    e->param = param;
    memset(&(e->abstime), 0, sizeof(e->abstime));
    e->next = NULL;
    e->prev = NULL;

    if (relativeTime == NULL) {
        relativeTime = alloca(sizeof(struct timeval));
        memset((struct timeval *) relativeTime, 0, sizeof(struct timeval));
    }

    gettimeofday(&tv, NULL);

    e->abstime.tv_sec = tv.tv_sec + relativeTime->tv_sec;
    e->abstime.tv_nsec = (tv.tv_usec + relativeTime->tv_usec) * 1000;

    if (e->abstime.tv_nsec > 1000000000) {
        e->abstime.tv_sec++;
        e->abstime.tv_nsec -= 1000000000;
    }

    switch(eventQueue) {
    case CMD_QUEUE_DEFAULT:
        /* DEFAULT group is always enabled */
        enqueueRILEventOnList(&s_requestQueueDefault, e);
        break;
    case CMD_QUEUE_AUXILIARY:
        if (!RILRequestGroups[CMD_QUEUE_AUXILIARY].requestQueue->enabled) {
            LOGW("%s(): AUXILIARY group is not enabled! "
                "Posting event on DEFAULT queue", __func__);
            enqueueRILEventOnList(&s_requestQueueDefault, e);
        } else
            enqueueRILEventOnList(&s_requestQueueAuxiliary, e);
        break;
    default:
        LOGW("%s(): Unknown event queue!"
            " Posting event on DEFAULT queue.", __func__);
        enqueueRILEventOnList(&s_requestQueueDefault, e);
    }

    return;
}

static void setPreferredMessageStorage()
{
    ATResponse *atresponse = NULL;
    char *tok = NULL;
    int used1, total1;
    int err = -1;

    err = at_send_command_singleline("AT+CPMS=\"SM\",\"SM\"","+CPMS: ",
                                     &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    /*
     * Depending on the host boot time the indication that message storage
     * on SIM is full (+CIEV: 10,1) may be sent before the RIL is started.
     * The RIL will explicitly check status of SIM messages storage using
     * +CPMS intermediate response and inform Android if storage is full.
     * +CPMS: <used1>,<total1>,<used2>,<total2>,<used3>,<total3>
     */
    tok = atresponse->p_intermediates->line;

    err = at_tok_start(&tok);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&tok, &used1);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&tok, &total1);
    if (err < 0)
        goto error;

    if (used1 >= total1)
        RIL_onUnsolicitedResponse(RIL_UNSOL_SIM_SMS_STORAGE_FULL,NULL, 0);

    goto exit;

error:
    LOGE("%s() failed during AT+CPMS sending/handling!", __func__);

exit:
    at_response_free(atresponse);
    return;
}

/** Do post- SIM ready initialization. */
static void onSIMReady()
{
    LOGI("%s()", __func__);

    /*
     * Configure preferred message storage
     *  mem1 = SM, mem2 = SM
     */
    setPreferredMessageStorage();

    /* Select message service */
    if (at_send_command("AT+CSMS=0", NULL) < 0)
        LOGW("%s(): Failed to send AT+CSMS", __func__);

    /*
     * Configure new messages indication
     *  mode = 2 - Buffer unsolicited result code in TA when TA-TE link is
     *             reserved(e.g. in on.line data mode) and flush them to the
     *             TE after reservation. Otherwise forward them directly to
     *             the TE.
     *  mt   = 2 - SMS-DELIVERs (except class 2 messages and messages in the
     *             message waiting indication group (store message)) are
     *             routed directly to TE using unsolicited result code:
     *             +CMT: [<alpha>],<length><CR><LF><pdu> (PDU mode)
     *             Class 2 messages are handled as if <mt> = 1
     *  bm   = 0 - No CBM indications are routed to the TE.
     *  ds   = 1 - SMS-STATUS-REPORTs are routed to the TE using unsolicited
     *             result code: +CDS: <length><CR><LF><pdu> (PDU mode)
     *  bfr  = 0 - TA buffer of unsolicited result codes defined within this
     *             command is flushed to the TE when <mode> 1...3 is entered
     *             (OK response is given before flushing the codes).
     */
    if (at_send_command("AT+CNMI=2,2,0,1,0", NULL) < 0)
        LOGW("%s(): Failed to send AT+CNMI", __func__);

    /* Configure ST-Ericsson current PS bearer Reporting. */
    if (at_send_command("AT*EPSB=1", NULL) < 0)
        LOGW("%s(): Failed to send AT+EPSB", __func__);

#ifdef LTE_COMMAND_SET_ENABLED
    /*
     * Subscribe to network registration events.
     *  n = 2 - Enable network registration and location information
     *          unsolicited result code +CREG: <stat>[,<lac>,<ci>]
     */
    if (at_send_command("AT+CREG=2", NULL) < 0)
        LOGW("%s(): Failed to send AT+CREG", __func__);

    if (at_send_command("AT+CEREG=2", NULL) < 0)
        LOGW("%s(): Failed to send AT+CEREG", __func__);
#else
    /* Subscribe to network registration events.
     *  n = 2 - Enable network registration and location information
     *          unsolicited result code *EREG: <stat>[,<lac>,<ci>]
     */
    if (at_send_command("AT*EREG=2", NULL) < 0)
        LOGW("%s(): Failed to send AT*EREG", __func__);
#endif

    /*
     * Subsctibe to Call Waiting Notifications.
     *  n = 1 - Enable call waiting notifications
     */
    if (at_send_command("AT+CCWA=1", NULL) < 0)
        LOGW("%s(): Failed to send AT+CCWA", __func__);

    /*
     * Subscribe to Supplementary Services Notification
     *  n = 1 - Enable the +CSSI result code presentation status.
     *          Intermediaate result codes. When enabled and a supplementary
     *          service notification is received after a mobile originated
     *          call setup.
     *  m = 1 - Enable the +CSSU result code presentation status.
     *          Unsolicited result code. When a supplementary service
     *          notification is received during a mobile terminated call
     *          setup or during a call, or when a forward check supplementary
     *          service notification is received.
     */
    if (at_send_command("AT+CSSN=1,1", NULL) < 0)
        LOGW("%s(): Failed to send AT+CSSN", __func__);

    /*
     * Subscribe to Unstuctured Supplementary Service Data (USSD) notifications.
     *  n = 1 - Enable result code presentation in the TA.
     */
    if (at_send_command("AT+CUSD=1", NULL) < 0)
        LOGW("%s(): Failed to send AT+CUSD", __func__);

    /*
     * Subscribe to Packet Domain Event Reporting.
     *  mode = 1 - Discard unsolicited result codes when ME-TE link is reserved
     *             (e.g. in on-line data mode); otherwise forward them directly
     *             to the TE.
     *   bfr = 0 - MT buffer of unsolicited result codes defined within this
     *             command is cleared when <mode> 1 is entered.
     */
    if (at_send_command("AT+CGEREP=1,0", NULL) < 0)
        LOGW("%s(): Failed to send AT+CGEREP", __func__);

    /*
     * Configure Short Message (SMS) Format
     *  mode = 0 - PDU mode.
     */
    if (at_send_command("AT+CMGF=0", NULL) < 0)
        LOGW("%s(): Failed to send AT+CMGF", __func__);

#ifndef USE_EARLY_NITZ_TIME_SUBSCRIPTION
    /* Subscribe to ST-Ericsson time zone/NITZ reporting */
    if (at_send_command("AT*ETZR=3", NULL) < 0)
        LOGW("%s(): Failed to send AT+ETZR", __func__);
#endif

    /*
     * Configure Mobile Equipment Event Reporting.
     *  mode = 3 - Forward unsolicited result codes directly to the TE;
     *             There is no inband technique used to embed result codes
     *             and data when TA is in on-line data mode.
     */
    if (at_send_command("AT+CMER=3,0,0,1", NULL) < 0)
        LOGW("%s(): Failed to send AT+CMER", __func__);

    /*
     * EACE should be sent to modem after SIM ready state.
     * Support notifications for comfort tone to Android.
     */
    if (at_send_command("AT*EACE=1", NULL) < 0)
        LOGW("%s(): Failed to enable comfort tone notifications", __func__);

    /*
     * Configure Minimum Interval Between RSSI Reports.
     *  gsm_interval   = 2 - Set reporting interval for GSM RAT RSSI change
     *  wcdma_interval = 2 - Set reporting interval for WCDMA RAT RSSI change
     */
    if (at_send_command("AT*EMIBRR=2,2", NULL) < 0)
        LOGW("%s(): Failed to send AT*EMIBRR", __func__);

    /*
     * To prevent Gsm/Cdma-ServiceStateTracker.java from polling RIL
     * with numerous RIL_REQUEST_SIGNAL_STRENGTH after power on
     * we get current signal strength using AT+CIND and and send
     * RIL_UNSOL_SIGNAL_STRENGTH up to stops further requests.
     */
    pollAndDispatchSignalStrength(NULL);
}

/**
 * Will LOCK THE MUTEX! MAKE SURE TO RELEASE IT!
 */
void getScreenStateLock(void)
{
    int err;

    /* Making sure we're not changing anything with regards to screen state. */
    if ((err = pthread_mutex_lock(&s_screen_state_mutex)) != 0)
        LOGE("%s() failed to take screen state mutex: %s!",
            __func__, strerror(err));
}

bool getScreenState(void)
{
    return s_screenState;
}

void setScreenState(bool screenIsOn)
{
    s_screenState = screenIsOn;
}

void releaseScreenStateLock(void)
{
    int err;

    /* Changing screen state is safe again */
    if ((err = pthread_mutex_unlock(&s_screen_state_mutex)) != 0)
        LOGW("%s() failed to release screen state mutex: %s",
            __func__,  strerror(err));
}

static RequestQueue *getRequestQueue(int request)
{
    size_t i, j;

    /* We are using only one RIL command group/AT channel. */
    if (!RILRequestGroups[CMD_QUEUE_AUXILIARY].requestQueue->enabled)
        return RILRequestGroups[CMD_QUEUE_DEFAULT].requestQueue;

    for (i = 0; i < RIL_MAX_NR_OF_CHANNELS; i++) {
        if (RILRequestGroups[i].requestQueue->enabled)
        {
            if (RILRequestGroups[i].group == CMD_QUEUE_AUXILIARY)
                continue;

            if (RILRequestGroups[i].requests == NULL)
                continue;

            for (j = 0;
                 RILRequestGroups[i].requests[j] != RIL_REQUEST_LAST_ELEMENT;
                 j++) {
                if (request == RILRequestGroups[i].requests[j])
                    return RILRequestGroups[i].requestQueue;
            }
        }
    }

    /*
     * If the request is not mapped to any particular
     * group it shall be put on the AUXILIARY queue.
     */
    return RILRequestGroups[CMD_QUEUE_AUXILIARY].requestQueue;
}

/*** Callback methods from the RIL library to us ***/
static const RIL_CardStatus staticSimStatus = {
    .card_state = RIL_CARDSTATE_ABSENT,
    .universal_pin_state = RIL_PINSTATE_UNKNOWN,
    .gsm_umts_subscription_app_index = 0,
    .cdma_subscription_app_index = 0,
    .num_applications = 0
};

static bool requestStateFilter(int request, RIL_Token t)
{
    /*
     * These commands will not accept RADIO_NOT_AVAILABLE and cannot be executed
     * before we are in SIM_STATE_READY so we just return GENERIC_FAILURE if
     * not in SIM_STATE_READY.
     */
    if (s_state != RADIO_STATE_SIM_READY
        && (request == RIL_REQUEST_WRITE_SMS_TO_SIM ||
            request == RIL_REQUEST_DELETE_SMS_ON_SIM)) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return true;
    }

    /* Ignore all requsts while is radio_state_unavailable */
    if (s_state == RADIO_STATE_UNAVAILABLE) {
        /*
         * The following command(s) must never fail. Return static state for
         * these command(s) while in RADIO_STATE_UNAVAILABLE.
         */
        if (request == RIL_REQUEST_GET_SIM_STATUS) {
            RIL_onRequestComplete(t, RIL_REQUEST_GET_SIM_STATUS,
                                  (char *) &staticSimStatus,
                                  sizeof(staticSimStatus));
        }
        /*
         * The following command must never fail. Return static state for this
         * command while in RADIO_STATE_UNAVAILABLE.
         */
        else if (request == RIL_REQUEST_SCREEN_STATE) {
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        }
        /* Ignore all other requests when RADIO_STATE_UNAVAILABLE */
        else {
            RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        }
        return true;
    }

    /*
     * Ignore all non-power requests when RADIO_STATE_OFF
     * (except RIL_REQUEST_RADIO_POWER and
     * RIL_REQUEST_GET_SIM_STATUS and a few more).
     * This is according to reference RIL implementation.
     * Note that returning RIL_E_RADIO_NOT_AVAILABLE for all ignored requests
     * causes Android Telephony to enter state RADIO_NOT_AVAILABLE and block
     * all communication with the RIL.
     */
    if (s_state == RADIO_STATE_OFF
        && !(request == RIL_REQUEST_RADIO_POWER ||
             request == RIL_REQUEST_STK_GET_PROFILE ||
             request == RIL_REQUEST_STK_SET_PROFILE ||
             request == RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING ||
             request == RIL_REQUEST_GET_SIM_STATUS ||
             request == RIL_REQUEST_GET_IMEISV ||
             request == RIL_REQUEST_GET_IMEI ||
             request == RIL_REQUEST_DEVICE_IDENTITY ||
             request == RIL_REQUEST_BASEBAND_VERSION ||
             request == RIL_REQUEST_SCREEN_STATE)) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return true;
    }

    /*
     * Ignore all non-power requests when RADIO_STATE_OFF
     * and RADIO_STATE_SIM_NOT_READY (except RIL_REQUEST_RADIO_POWER
     * and a few more).
     */
    if ((s_state == RADIO_STATE_OFF || s_state == RADIO_STATE_SIM_NOT_READY)
        && !(request == RIL_REQUEST_RADIO_POWER ||
             request == RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING ||
             request == RIL_REQUEST_GET_SIM_STATUS ||
             request == RIL_REQUEST_GET_IMEISV ||
             request == RIL_REQUEST_GET_IMEI ||
             request == RIL_REQUEST_DEVICE_IDENTITY ||
             request == RIL_REQUEST_BASEBAND_VERSION ||
             request == RIL_REQUEST_SCREEN_STATE)) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return true;
    }

    /*
     * Don't allow radio operations when sim is absent or locked!
     * DIAL, GET_CURRENT_CALLS, HANGUP and LAST_CALL_FAIL_CAUSE are
     * required to handle emergency calls.
     */
    if (s_state == RADIO_STATE_SIM_LOCKED_OR_ABSENT
        && !(request == RIL_REQUEST_ENTER_SIM_PIN ||
             request == RIL_REQUEST_ENTER_SIM_PUK ||
             request == RIL_REQUEST_ENTER_SIM_PIN2 ||
             request == RIL_REQUEST_ENTER_SIM_PUK2 ||
             request == RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION ||
             request == RIL_REQUEST_GET_SIM_STATUS ||
             request == RIL_REQUEST_RADIO_POWER ||
             request == RIL_REQUEST_GET_IMEISV ||
             request == RIL_REQUEST_GET_IMEI ||
             request == RIL_REQUEST_BASEBAND_VERSION ||
             request == RIL_REQUEST_DIAL ||
             request == RIL_REQUEST_GET_CURRENT_CALLS ||
             request == RIL_REQUEST_HANGUP ||
             request == RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND ||
             request == RIL_REQUEST_SET_TTY_MODE ||
             request == RIL_REQUEST_QUERY_TTY_MODE ||
             request == RIL_REQUEST_DTMF ||
             request == RIL_REQUEST_DTMF_START ||
             request == RIL_REQUEST_DTMF_STOP ||
             request == RIL_REQUEST_LAST_CALL_FAIL_CAUSE ||
             request == RIL_REQUEST_SCREEN_STATE)) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return true;
    }

    return false;
}

static void processRequest(int request, void *data, size_t datalen,
                           RIL_Token t)
{
    LOGI("processRequest: %s", requestToString(request));

    if (requestStateFilter(request, t))
        goto finally;

    switch (request) {

    /* Basic Voice Call */
    case RIL_REQUEST_LAST_CALL_FAIL_CAUSE:
        requestLastCallFailCause(data, datalen, t);
        break;
    case RIL_REQUEST_GET_CURRENT_CALLS:
        requestGetCurrentCalls(data, datalen, t);
        break;
    case RIL_REQUEST_DIAL:
        requestDial(data, datalen, t);
        break;
    case RIL_REQUEST_HANGUP:
        requestHangup(data, datalen, t);
        break;
    case RIL_REQUEST_ANSWER:
        requestAnswer(data, datalen, t);
        break;

    /* Advanced Voice Call */
    case RIL_REQUEST_GET_CLIR:
        requestGetCLIR(data, datalen, t);
        break;
    case RIL_REQUEST_SET_CLIR:
        requestSetCLIR(data, datalen, t);
        break;
    case RIL_REQUEST_QUERY_CALL_FORWARD_STATUS:
        requestQueryCallForwardStatus(data, datalen, t);
        break;
    case RIL_REQUEST_SET_CALL_FORWARD:
        requestSetCallForward(data, datalen, t);
        break;
    case RIL_REQUEST_QUERY_CALL_WAITING:
        requestQueryCallWaiting(data, datalen, t);
        break;
    case RIL_REQUEST_SET_CALL_WAITING:
        requestSetCallWaiting(data, datalen, t);
        break;
    case RIL_REQUEST_UDUB:
        requestUDUB(data, datalen, t);
        break;
    case RIL_REQUEST_GET_MUTE:
        requestGetMute(data, datalen, t);
        break;
    case RIL_REQUEST_SET_MUTE:
        requestSetMute(data, datalen, t);
        break;
    case RIL_REQUEST_SCREEN_STATE:
        requestScreenState(data, datalen, t);
        break;
    case RIL_REQUEST_QUERY_CLIP:
        requestQueryClip(data, datalen, t);
        break;
    case RIL_REQUEST_DTMF:
        requestDTMF(data, datalen, t);
        break;
    case RIL_REQUEST_DTMF_START:
        requestDTMFStart(data, datalen, t);
        break;
    case RIL_REQUEST_DTMF_STOP:
        requestDTMFStop(data, datalen, t);
        break;

    /* Multiparty Voice Call */
    case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
        requestHangupWaitingOrBackground(data, datalen, t);
        break;
    case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
        requestHangupForegroundResumeBackground(data, datalen, t);
        break;
    case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
        requestSwitchWaitingOrHoldingAndActive(data, datalen, t);
        break;
    case RIL_REQUEST_CONFERENCE:
        requestConference(data, datalen, t);
        break;
    case RIL_REQUEST_SEPARATE_CONNECTION:
        requestSeparateConnection(data, datalen, t);
        break;
    case RIL_REQUEST_EXPLICIT_CALL_TRANSFER:
        requestExplicitCallTransfer(data, datalen, t);
        break;

    /* Data Call Requests */
    case RIL_REQUEST_SETUP_DATA_CALL:
        requestSetupDataCall(data, datalen, t);
        break;
    case RIL_REQUEST_DEACTIVATE_DATA_CALL:
        requestDeactivateDataCall(data, datalen, t);
        break;
    case RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE:
        requestLastPDPFailCause(data, datalen, t);
        break;
    case RIL_REQUEST_DATA_CALL_LIST:
        requestPDPContextList(data, datalen, t);
        break;

    /* SMS Requests */
    case RIL_REQUEST_SEND_SMS:
        requestSendSMS(data, datalen, t);
        break;
    case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
        requestSendSMSExpectMore(data, datalen, t);
        break;
    case RIL_REQUEST_WRITE_SMS_TO_SIM:
        requestWriteSmsToSim(data, datalen, t);
        break;
    case RIL_REQUEST_DELETE_SMS_ON_SIM:
        requestDeleteSmsOnSim(data, datalen, t);
        break;
    case RIL_REQUEST_GET_SMSC_ADDRESS:
        requestGetSMSCAddress(data, datalen, t);
        break;
    case RIL_REQUEST_SET_SMSC_ADDRESS:
        requestSetSMSCAddress(data, datalen, t);
        break;
    case RIL_REQUEST_REPORT_SMS_MEMORY_STATUS:
        requestSmsStorageFull(data, datalen, t);
        break;
    case RIL_REQUEST_SMS_ACKNOWLEDGE:
        requestSMSAcknowledge(data, datalen, t);
        break;
    case RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG:
        requestGSMGetBroadcastSMSConfig(data, datalen, t);
        break;
    case RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG:
        requestGSMSetBroadcastSMSConfig(data, datalen, t);
        break;
    case RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION:
        requestGSMSMSBroadcastActivation(data, datalen, t);
        break;

    /* SIM Handling Requests */
    case RIL_REQUEST_SIM_IO:
        requestSIM_IO(data, datalen, t);
        break;
    case RIL_REQUEST_GET_SIM_STATUS:
        requestGetSimStatus(data, datalen, t);
        break;
    case RIL_REQUEST_ENTER_SIM_PIN:
    case RIL_REQUEST_ENTER_SIM_PUK:
    case RIL_REQUEST_ENTER_SIM_PIN2:
    case RIL_REQUEST_ENTER_SIM_PUK2:
        requestEnterSimPin(data, datalen, t, request);
        break;
    case RIL_REQUEST_CHANGE_SIM_PIN:
        requestChangeSimPin(data, datalen, t, request);
        break;
    case RIL_REQUEST_CHANGE_SIM_PIN2:
        requestChangeSimPin2(data, datalen, t, request);
        break;
    case RIL_REQUEST_CHANGE_BARRING_PASSWORD:
        requestChangeBarringPassword(data, datalen, t, request);
        break;
    case RIL_REQUEST_QUERY_FACILITY_LOCK:
        requestQueryFacilityLock(data, datalen, t);
        break;
    case RIL_REQUEST_SET_FACILITY_LOCK:
        requestSetFacilityLock(data, datalen, t);
        break;

    /* USSD Requests */
    case RIL_REQUEST_SEND_USSD:
        requestSendUSSD(data, datalen, t);
        break;
    case RIL_REQUEST_CANCEL_USSD:
        requestCancelUSSD(data, datalen, t);
        break;

    /* Network Selection */
    case RIL_REQUEST_SET_BAND_MODE:
        requestSetBandMode(data, datalen, t);
        break;
    case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE:
        requestQueryAvailableBandMode(data, datalen, t);
        break;
    case RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION:
        requestEnterNetworkDepersonalization(data, datalen, t);
        break;
    case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
        requestQueryNetworkSelectionMode(data, datalen, t);
        break;
    case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
        requestSetNetworkSelectionAutomatic(data, datalen, t);
        break;
    case RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL:
        requestSetNetworkSelectionManual(data, datalen, t);
        break;
    case RIL_REQUEST_QUERY_AVAILABLE_NETWORKS:
        requestQueryAvailableNetworks(data, datalen, t);
        break;
    case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
        requestSetPreferredNetworkType(data, datalen, t);
        break;
    case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
        requestGetPreferredNetworkType(data, datalen, t);
        break;
    case RIL_REQUEST_REGISTRATION_STATE:
        requestRegistrationState(data, datalen, t);
        break;
    case RIL_REQUEST_GPRS_REGISTRATION_STATE:
        requestGprsRegistrationState(data, datalen, t);
        break;
    case RIL_REQUEST_SET_LOCATION_UPDATES:
        requestSetLocationUpdates(data, datalen, t);
        break;

    /* OEM */
    case RIL_REQUEST_OEM_HOOK_RAW:
        requestOEMHookRaw(data, datalen, t);
        break;
    case RIL_REQUEST_OEM_HOOK_STRINGS:
        requestOEMHookStrings(data, datalen, t);
        break;

    /* Misc */
    case RIL_REQUEST_SIGNAL_STRENGTH:
        requestSignalStrength(data, datalen, t);
        break;
    case RIL_REQUEST_OPERATOR:
        requestOperator(data, datalen, t);
        break;
    case RIL_REQUEST_RADIO_POWER:
        requestRadioPower(data, datalen, t);
        break;
    case RIL_REQUEST_GET_IMSI:
        requestGetIMSI(data, datalen, t);
        break;
    case RIL_REQUEST_GET_IMEI: /* Deprecated */
        requestGetIMEI(data, datalen, t);
        break;
    case RIL_REQUEST_GET_IMEISV:   /* Deprecated */
        requestGetIMEISV(data, datalen, t);
        break;
    case RIL_REQUEST_DEVICE_IDENTITY:
        requestDeviceIdentity(data, datalen, t);
        break;
    case RIL_REQUEST_BASEBAND_VERSION:
        requestBasebandVersion(data, datalen, t);
        break;
    case RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION:
        requestSetSuppSvcNotification(data, datalen, t);
        break;

    /* SIM Application Toolkit */
    case RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE:
        requestStkSendTerminalResponse(data, datalen, t);
        break;
    case RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND:
        requestStkSendEnvelopeCommand(data, datalen, t);
        break;
    case RIL_REQUEST_STK_GET_PROFILE:
        requestStkGetProfile(data, datalen, t);
        break;
    case RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING:
        requestReportStkServiceIsRunning(data, datalen, t);
        break;
    case RIL_REQUEST_STK_SET_PROFILE:
        requestStkSetProfile(data, datalen, t);
        break;
    case RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM:
        requestStkHandleCallSetupRequestedFromSIM(data, datalen, t);
        break;

    /* Network neighbors */
    case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
        requestNeighboringCellIDs(data, datalen, t);
        break;

    /* TTY mode */
    case RIL_REQUEST_SET_TTY_MODE:
        requestSetTtyMode(data, datalen, t);
        break;
    case RIL_REQUEST_QUERY_TTY_MODE:
        requestQueryTtyMode(data, datalen, t);
        break;

    default:
        LOGW("%s(): FIXME: Unsupported request logged: %s!",
             __func__, requestToString(request));
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        break;
    }

finally:
    return;
}

/**
 * Call from RIL to us to make a RIL_REQUEST.
 *
 * Must be completed with a call to RIL_onRequestComplete().
 */
static void onRequest(int request, void *data, size_t datalen, RIL_Token t)
{
    RILRequest *r;
    RequestQueue *q = &s_requestQueueDefault;
    int err;

    /* In radio state unavailable no requests are to enter the queues */
    if (s_state == RADIO_STATE_UNAVAILABLE) {
        (void)requestStateFilter(request, t);
        goto finally;
    }

    q = getRequestQueue(request);

    r = calloc(1, sizeof(RILRequest));
    assert(r != NULL);

    /* Formulate a RILRequest and put it in the queue. */
    r->request = request;
    r->data = dupRequestData(request, data, datalen);
    r->datalen = datalen;
    r->token = t;
    r->next = NULL;

    if ((err = pthread_mutex_lock(&q->queueMutex)) != 0) {
        LOGE("%s() failed to take queue mutex: %s!", __func__, strerror(err));
        assert(0);
    }

    /* Queue empty, just throw r on top. */
    if (q->requestList == NULL)
        q->requestList = r;
    else {
        RILRequest *l = q->requestList;
        while (l->next != NULL)
            l = l->next;

        l->next = r;
    }

    if ((err = pthread_cond_broadcast(&q->cond)) != 0)
        LOGE("%s() failed to broadcast queue update: %s!",
            __func__, strerror(err));

    if ((err = pthread_mutex_unlock(&q->queueMutex)) != 0)
        LOGE("%s() failed to release queue mutex: %s!",
            __func__, strerror(err));

finally:
    return;
}

int getRestrictedState(void)
{
    return s_restrictedState;
}

/**
 * Returns current RIL radio state.
 */
RIL_RadioState getCurrentState(void)
{
    return s_state;
}

/**
 * Synchronous call from the RIL to us to return current radio state.
 * RADIO_STATE_UNAVAILABLE should be the initial state.
 */
static RIL_RadioState onStateRequest(void)
{
    return getCurrentState();
}

/**
 * Call from RIL to us to find out whether a specific request code
 * is supported by this implementation.
 *
 * Return 1 for "supported" and 0 for "unsupported".
 *
 * Currently just stubbed with the default value of one. This is currently
 * not used by android, and therefore not implemented here. We return
 * RIL_E_REQUEST_NOT_SUPPORTED when we encounter unsupported requests.
 */
static int supports(int requestCode)
{
    LOGW("Unimplemented function \"%s\" called!", __func__);

    return 1;
}

/**
 * onCancel() is currently stubbed, because android doesn't use it and
 * our implementation will depend on how a cancellation is handled in
 * the upper layers.
 */
static void onCancel(RIL_Token t)
{
    LOGW("Unimplemented function \"%s\" called!", __func__);
}

static const char *getVersion(void)
{
    return RIL_VERSION_STRING;
}

const char *radioStateToString(RIL_RadioState radioState)
{
    const char *state;

    switch (radioState) {
    case RADIO_STATE_OFF:
        state = "RADIO_STATE_OFF";
        break;
    case RADIO_STATE_UNAVAILABLE:
        state = "RADIO_STATE_UNAVAILABLE";
        break;
    case RADIO_STATE_SIM_NOT_READY:
        state = "RADIO_STATE_SIM_NOT_READY";
        break;
    case RADIO_STATE_SIM_LOCKED_OR_ABSENT:
        state = "RADIO_STATE_SIM_LOCKED_OR_ABSENT";
        break;
    case RADIO_STATE_SIM_READY:
        state = "RADIO_STATE_SIM_READY";
        break;
    case RADIO_STATE_RUIM_NOT_READY:
        state = "RADIO_STATE_RUIM_NOT_READY";
        break;
    case RADIO_STATE_RUIM_LOCKED_OR_ABSENT:
        state = "RADIO_STATE_RUIM_READY";
        break;
    case RADIO_STATE_NV_NOT_READY:
        state = "RADIO_STATE_NV_NOT_READY";
        break;
    case RADIO_STATE_NV_READY:
        state = "RADIO_STATE_NV_READY";
        break;
    default:
        state = "RADIO_STATE_<> Unknown!";
        break;
    }

    return state;
}

void setRadioState(RIL_RadioState newState)
{
    RIL_RadioState oldState;
    int err;

    if ((err = pthread_mutex_lock(&s_state_mutex)) != 0) {
        LOGE("%s() failed to take state mutex: %s!", __func__, strerror(err));
        assert(0);
    }

    oldState = s_state;

    LOGI("setRadioState: oldState=%s newState=%s", radioStateToString(oldState),
         radioStateToString(newState));

    if (s_state != newState)
        s_state = newState;

    if ((err = pthread_mutex_unlock(&s_state_mutex)) != 0)
        LOGW("%s(): Failed to release state mutex: %s", __func__,
             strerror(err));

    /* Do these outside of the mutex. */
    if (s_state != oldState || s_state == RADIO_STATE_SIM_LOCKED_OR_ABSENT) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
                                  NULL, 0);

        if (s_state == RADIO_STATE_SIM_READY)
            enqueueRILEvent(CMD_QUEUE_DEFAULT, onSIMReady, NULL, NULL);
        else if (s_state == RADIO_STATE_SIM_NOT_READY)
            enqueueRILEvent(CMD_QUEUE_DEFAULT, pollSIMState, NULL,
                            NULL);
    }
}

/** Returns 1 if on, 0 if off, and -1 on error. */
static int isRadioOn(void)
{
    ATResponse *atresponse = NULL;
    int err;
    char *line;
    int ret;

    err = at_send_command_singleline("AT+CFUN?", "+CFUN:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &ret);
    if (err < 0)
        goto error;

    switch (ret) {
    case 1:                    /* Full functionality (switched on) */
    case 5:                    /* GSM only */
    case 6:                    /* WCDMA only */
        ret = 1;
        break;

    default:
        ret = 0;
    }

    at_response_free(atresponse);

    return ret;

error:
    at_response_free(atresponse);
    return -1;
}

static bool supportsECAM(int* version)
{
    ATResponse *atresponse = NULL;
    char *line = NULL;
    char *found = NULL;
    int ecamHiRange;

    if (at_send_command_singleline("AT*ECAM=?", "*ECAM:", &atresponse) < 0)
        goto error;   /* AT send error */

    if (atresponse->success == 0)
        return false; /* Likely no support */

    /* Find substring and decode number into version */
    line = atresponse->p_intermediates->line;
    found = strstr(line, "(0-");
    if (found == NULL)
        goto error;   /* Parsing error */

    ecamHiRange = atoi(found+3);
    if (ecamHiRange == 0)
        goto error;   /* Invalid range */

    *version = ecamHiRange;
    return true;

error:
    LOGE("%s() failed to check support for AT*ECAM, "
        "assuming no support!", __func__);
    return false;
}

static bool initializeCommon(void)
{
    int err = 0;

    LOGI("%s()", __func__);

    if (at_handshake() < 0) {
        LOG_FATAL("Handshake failed!");
        goto error;
    }

    /* Configure/set
     *   command echo (E), result code suppression (Q), DCE response format (V)
     *
     *  E0 = DCE does not echo characters during command state and online
     *       command state
     *  Q0 = DCE transmits result codes
     *  V1 = Display verbose result codes
     */
    err = at_send_command("ATE0Q0V1", NULL);
    if (err < 0)
        goto error;

    /* Set default character set. */
    err = at_send_command("AT+CSCS=\"UTF-8\"", NULL);
    if (err < 0)
        goto error;

    /* Disable automatic answer. */
    err = at_send_command("ATS0=0", NULL);
    if (err < 0)
        goto error;

    /* Enable +CME ERROR: <err> result code and use numeric <err> values. */
    err = at_send_command("AT+CMEE=1", NULL);
    if (err < 0)
        goto error;

    /* Enable Connected Line Identification Presentation. */
    err = at_send_command("AT+COLP=0", NULL);
    if (err < 0)
        goto error;

    /* Disable Service Reporting. */
    err = at_send_command("AT+CR=0", NULL);
    if (err < 0)
        goto error;

    /* Configure carrier detect signal - 1 = DCD follows the connection. */
    err = at_send_command("AT&C=1", NULL);
    if (err < 0)
        goto error;

    /* Configure DCE response to Data Termnal Ready signal - 0 = ignore. */
    err = at_send_command("AT&D=0", NULL);
    if (err < 0)
        goto error;

    /* Configure Cellular Result Codes - 0 = Disables extended format. */
    err = at_send_command("AT+CRC=0", NULL);
    if (err < 0)
        goto error;

    return true;
error:
    return false;
}

/**
 * Initialize everything that can be configured while we're still in
 * AT+CFUN=0.
 */
static bool initializeDefault()
{
    int err;
    int support = 0;

    LOGI("%s()", __func__);

    /*
     * Set phone functionality.
     * 4 = Disable the phone's transmit and receive RF circuits.
     */
    if (at_send_command("AT+CFUN=4", NULL) < 0)
        goto error;

    setRadioState(RADIO_STATE_OFF);

    /*
     * SIM Application Toolkit Configuration
     *  n = 0 - Disable SAT unsolicited result codes
     *  stkPrfl = - SIM application toolkit profile in hexadecimal format
     *              starting with first byte of the profile.
     *              See 3GPP TS 11.14[1] for details.
     *
     * Terminal profile is currently empty because stkPrfl is currently
     * overriden by the default profile stored in the modem.
     */
#ifdef USE_LEGACY_SAT_AT_CMDS
    if (at_send_command("AT*STKC=0,\"000000000000000000\"", NULL) < 0)
        LOGW("%s(): Failed to initialize STK", __func__);
#endif

    /*
     * Configure Packet Domain Network Registration Status events
     *    2 = Enable network registration and location information
     *        unsolicited result code
     */
    if (at_send_command("AT+CGREG=2", NULL) < 0)
        goto error;

    /* Subscribe to ST-Ericsson Pin code event.
     *   The command requests the MS to report when the PIN code has been
     *   inserted and accepted.
     *      1 = Request for report on inserted PIN code is activated (on)
     */
    if (at_send_command("AT*EPEE=1", NULL) < 0)
        goto error;

    /* Subscribe to ST-Ericsson SIM State Reporting.
     *   Enable SIM state reporting on the format *ESIMSR: <sim_state>
     */
    if (at_send_command("AT*ESIMSR=1", NULL) < 0)
        goto error;

    /* Subscribe to ST-Ericsson Call monitoring events.
     * Done here to handle during emergency calls without SIM.
     *  onoff = 1 - Call monitoring is on and supports <ccstatus> 0-7
     *  onoff = 2 - Call monitoring is on and supports <ccstatus> 0-8
     *
     * Check modem support before setting best support.
     */
    (void) supportsECAM(&support);
    if (at_send_command(support > 1?"AT*ECAM=2":"AT*ECAM=1", NULL) < 0)
        LOGW("%s(): Failed to subscribe to ST-Ericsson "
            "Call monitoring events", __func__);

    /* Enable barred status reporting used for reporting restricted state. */
    if (at_send_command("AT*EBSR=1", NULL) < 0)
        LOGW("%s(): Failed to enable barred status reporting", __func__);

#ifdef USE_EARLY_NITZ_TIME_SUBSCRIPTION
    /* Subscribe to ST-Ericsson time zone/NITZ reporting */
    if (at_send_command("AT*ETZR=3", NULL) < 0)
        LOGW("%s(): Failed to send early AT*ETZR", __func__);
#endif

    /*
     * Emergency numbers from 3GPP TS 22.101, chapter 10.1.1.
     * 911 and 112 should always be set in the system property, but if SIM is
     * absent, these numbers also has to be added: 000, 08, 110, 999, 118
     * and 119.
     */
    err = property_set(PROP_EMERGENCY_LIST_RW,
                        "911,112,000,08,110,999,118,119");

    /*
     * We do not go to error in this case. Even though we cannot set emergency
     * numbers it is better to continue and at least be able to call some
     * numbers.
     */
    if (err < 0)
        LOGE("[ECC] Creating emergency list ril.ecclist"
            " in system properties failed!");
    else
        LOGD("[ECC] Set initial defaults to system property ril.ecclist");

    /*
     * Older versions of Android does not support ril.ecclist. For legacy
     * reasons ro.ril.ecclist is therefore set up with emergency numbers from
     * 3GPP TS 22.101, chapter 10.1.1.
     */
    err = property_set(PROP_EMERGENCY_LIST_RO,
                        "911,112,000,08,110,999,118,119");

    if (err < 0)
        LOGE("[ECC] Creating emergency list ro.ril.ecclist in "
            "system properties failed!");
    else
        LOGD("[ECC] Set initial defaults to system property ro.ril.ecclist");

    /*
     * Fetch emergency call code list from EF_ECC
     * and store it into PROP_EMERGENCY_LIST_RW (ril.ecclist)
     * property. Do not analyse attached network: ME is not
     * connected to a BSS yet.
     */
    if (!isSimAbsent())
        setupECCList(0);
    else
        LOGI("[ECC]: SIM is absent, keeping default ECCs");

    return true;

error:
    return false;
}

/**
 * Called by atchannel when an unsolicited line appears.
 * This is called on atchannel's reader thread. AT commands may
 * not be issued here.
 */
static void onUnsolicited(const char *s, const char *sms_pdu)
{
    LOGI("onUnsolicited: %s", s);

    /* Ignore unsolicited responses until we're initialized.
     * This is OK because the RIL library will poll for initial state.
     */
    if (s_state == RADIO_STATE_UNAVAILABLE)
        return;

    if (strStartsWith(s, "*ETZV:")) {
        /* If we're in screen state, we have disabled CREG, but the ETZV
         * will catch those few cases. So we send network state changed as
         * well on NITZ.
         */
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED,
                                  NULL, 0);

        onNetworkTimeReceived(s);
    } else if (strStartsWith(s, "*EPEV"))
        /* Pin event, poll SIM State! */
        enqueueRILEvent(CMD_QUEUE_DEFAULT, pollSIMState, NULL, NULL);
    else if (strStartsWith(s, "*ESIMSR"))
        onSimStateChanged(s);
    else if (strStartsWith(s, "+CRING:")
             || strStartsWith(s, "RING"))
        RIL_onUnsolicitedResponse(RIL_UNSOL_CALL_RING, NULL, 0);
    else if (strStartsWith(s, "+CCWA"))
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
                                  NULL, 0);
    else if (strStartsWith(s, "*EREG:")
             || strStartsWith(s, "+CGREG:")
             || strStartsWith(s, "+CREG:"))
        onNetworkStateChanged(s);
    else if (strStartsWith(s, "+CMT:"))
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NEW_SMS, sms_pdu,
                                  strlen(sms_pdu));
    else if (strStartsWith(s, "+CBM:"))
        onNewBroadcastSms(sms_pdu);
    else if (strStartsWith(s, "+CMTI:"))
        onNewSmsOnSIM(s);
    else if (strStartsWith(s, "+CDS:"))
        onNewStatusReport(sms_pdu);
    else if (strStartsWith(s, "+CGEV:")) {
        /* Really, we can ignore NW CLASS and ME CLASS events here,
         * but right now we don't since extranous
         * RIL_UNSOL_PDP_CONTEXT_LIST_CHANGED calls are tolerated.
         */
        enqueueRILEvent(CMD_QUEUE_AUXILIARY, onPDPContextListChanged,
                        NULL, NULL);
    } else if (strStartsWith(s, "+CIEV: 2"))
        unsolSignalStrength(s);
    else if (strStartsWith(s, "+CIEV: 10"))
        unsolSimSmsFull(s);
    else if (strStartsWith(s, "*EBSRU:"))
        onRestrictedStateChanged(s, &s_restrictedState);
    else if (strStartsWith(s, "+CSSI:"))
        onSuppServiceNotification(s, 0);
    else if (strStartsWith(s, "+CSSU:"))
        onSuppServiceNotification(s, 1);
    else if (strStartsWith(s, "+CUSD:"))
        onUSSDReceived(s);
    else if (strStartsWith(s, "*ECAV:"))
        onECAVReceived(s);
#ifndef USE_LEGACY_SAT_AT_CMDS
    else if (strStartsWith(s, "+CUSATEND"))
        RIL_onUnsolicitedResponse(RIL_UNSOL_STK_SESSION_END, NULL, 0);
#else
    else if (strStartsWith(s, "*STKEND"))
        RIL_onUnsolicitedResponse(RIL_UNSOL_STK_SESSION_END, NULL, 0);
#endif
#ifndef USE_LEGACY_SAT_AT_CMDS
    else if (strStartsWith(s, "+CUSATP:"))
        onStkProactiveCommand(s);
#else
    else if (strStartsWith(s, "*STKI:"))
        onStkProactiveCommand(s);
#endif
#ifndef USE_LEGACY_SAT_AT_CMDS
    else if (strStartsWith(s, "*ESHLREF:"))
        onStkSimRefresh(s);
#else
    else if (strStartsWith(s, "*ESIMRF:"))
        onStkSimRefresh(s);
#endif
    else if (strStartsWith(s, "*STKN:"))
        onStkEventNotify(s);
    else if (strStartsWith(s, "*ESHLVOCU:"))
        onStkEventNotify(s);
    else if (strStartsWith(s, "*ESHLSSU:"))
        onStkEventNotify(s);
    else if (strStartsWith(s, "*ESHLUSSU:"))
        onStkEventNotify(s);
    else if (strStartsWith(s, "*ESHLDTMFU:"))
        onStkEventNotify(s);
    else if (strStartsWith(s, "*ESHLSMSU:"))
        onStkEventNotify(s);
    else if (strStartsWith(s, "*EACE:"))
        onAudioCallEventNotify(s);
    else if (strStartsWith(s, "*EPSB:")) {
        onNetworkStateChanged(s);
        onEPSBReceived(s);
    } else
        onOemUnsolHook(s);
}

void signalCloseQueues(void)
{
    unsigned int i;
    setRadioState(RADIO_STATE_UNAVAILABLE);

    for (i = 0; i < NUM_ELEMS(s_requestQueues); i++) {
        int err;
        RequestQueue *q = s_requestQueues[i];
        if ((err = pthread_mutex_lock(&q->queueMutex)) != 0)
            LOGW("%s() failed to take queue mutex: %s",
                __func__, strerror(err));

        q->closed = 1;
        if ((err = pthread_cond_signal(&q->cond)) != 0)
            LOGW("%s() failed to broadcast queue update: %s",
                __func__, strerror(err));

        if ((err = pthread_mutex_unlock(&q->queueMutex)) != 0)
            LOGW("%s() failed to take queue mutex: %s", __func__,
                 strerror(err));
    }
}

static void signalManager(void)
{
    int err;

    if ((err = pthread_mutex_lock(&ril_manager_queue_exit_mutex)) != 0)
        LOG_FATAL("%s() failed to take RIL Manager AT fail mutex: %s",
                  __func__, strerror(err));

    if ((err = pthread_cond_signal(&ril_manager_queue_exit_cond)) != 0)
        LOGW("%s() failed to signal RIL Manager: %s",
             __func__, strerror(err));

    if ((err = pthread_mutex_unlock(&ril_manager_queue_exit_mutex)) != 0)
        LOG_FATAL("%s() failed to take RIL Manager AT Fail mutex: %s",
                  __func__, strerror(err));
}

/* Called on command or reader thread. */
static void onATReaderClosed()
{
    LOGI("AT channel closed, closing queues!");
    signalCloseQueues();
}

/* Callback from AT Channel. Called on command thread. */
static void onATTimeout()
{
    LOGI("AT channel timeout. Trying to abort command and check channel.");

    /* Throw escape on the channel and check sanity with handshake */
    at_send_escape();

    if (at_handshake() >= 0) {
        LOGI("AT channel sanity check successful. Continuing...");
    }
    else {
        LOG_FATAL("%s() Channel sanity check failed!", __func__);
        signalCloseQueues();

        /* Prevent further command execution */
        at_close();
    }
}

int parseGroups(char* groups, RILRequestGroup **parsedGroups)
{
    int n = 0;

    if (parsedGroups == NULL)
        return -1;

    /* DEFAULT group is mandatory */
    RILRequestGroups[CMD_QUEUE_DEFAULT].requestQueue->enabled = 1;
    parsedGroups[n] = &RILRequestGroups[CMD_QUEUE_DEFAULT];
    n++;

    /*
     * If only the DEFAULT group is specified on the command line
     * this is considered as a special case used for test purposes
     * and the AUXILIARY group will not be added.
     */
    if (strcasestr(groups, RILRequestGroups[CMD_QUEUE_DEFAULT].name) &&
        !strcasestr(groups, RILRequestGroups[CMD_QUEUE_AUXILIARY].name)) {
        LOGW("Only DEFAULT group is enabled!"
            " Using one group/AT channel is only for testing purposes.");
        goto exit;
    }

    /* AUXILIARY group is mandatory */
    RILRequestGroups[CMD_QUEUE_AUXILIARY].requestQueue->enabled = 1;
    parsedGroups[n] = &RILRequestGroups[CMD_QUEUE_AUXILIARY];
    n++;

exit:
    return n;
}

void *queueRunner(void *param)
{
    int fd;
    int ret;
    struct queueArgs *queueArgs = (struct queueArgs *) param;
    struct RequestQueue *q = NULL;

    LOGI("%s() thread index %d waiting for Manager release flag", __func__,
         queueArgs->index);

    ret = pthread_mutex_lock(&ril_manager_wait_mutex);
    if (ret != 0)
        LOGE("%s(): Failed to get mutex lock. err: %s", __func__,
                strerror(-ret));

    while (!managerRelease) {
        ret = pthread_cond_wait(&ril_manager_wait_cond,
                                &ril_manager_wait_mutex);
        if (ret != 0)
            LOGE("%s(): pthread_cond_wait Failed. err: %s", __func__,
                strerror(-ret));
    }

    ret = pthread_mutex_unlock(&ril_manager_wait_mutex);
    if (ret != 0)
        LOGE("%s(): Failed to unlock mutex. err: %s", __func__,
                strerror(-ret));

    LOGI("%s() index %d setting up AT socket channel", __func__,
         queueArgs->index);
    fd = -1;
    while (fd < 0) {
        if (queueArgs->type == NULL) {
            LOGE("%s(): Unsupported channel type. Bailing out!", __func__);
            free(queueArgs);
            return NULL;
        }

        if (!strncmp(queueArgs->type, "CAIF", 4)) {
#ifndef CAIF_SOCKET_SUPPORT_DISABLED
            int cf_prio = CAIF_PRIO_HIGH;

            struct sockaddr_caif addr = {
                .family = AF_CAIF,
                .u.at.type = CAIF_ATTYPE_PLAIN
            };

            fd = socket(AF_CAIF, SOCK_SEQPACKET, CAIFPROTO_AT);
            if (fd < 0) {
                LOGE("%s(): failed to create socket. errno: %d(%s).",
                    __func__, errno, strerror(-errno));
            }

            if (setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &cf_prio,
                sizeof(cf_prio)) != 0)
                LOGE("%s(): Not able to set socket priority. Errno:%d(%s).",
                     __func__, errno, strerror(-errno));

            ret = connect(fd, (struct sockaddr *) &addr, sizeof(addr));
            if (ret != 0)
                LOGE("%s(): Failed to connect. errno: %d(%s).", __func__,
                    errno, strerror(-errno));
#else
            LOGE("%s(): Unsupported channel type CAIF. Bailing out!",
                __func__);
            free(queueArgs);
            return NULL;
#endif
        } else if (!strncmp(queueArgs->type, "UNIX", 4)) {
            struct sockaddr_un addr;
            int len;
            if (queueArgs->arg == NULL) {
                LOGE("%s(): No path specified for UNIX socket!"
                    " Bailing out!", __func__);
                free(queueArgs);
                return NULL;
            }
            bzero((char *) &addr, sizeof(addr));
            addr.sun_family = AF_UNIX;

            strncpy(addr.sun_path, queueArgs->arg,
                    sizeof(addr.sun_path));
            len = strlen(addr.sun_path) + sizeof(addr.sun_family);
            fd = socket(AF_UNIX, SOCK_STREAM, 0);
            (void)connect(fd, (struct sockaddr *) &addr, len);
        } else if (!strncmp(queueArgs->type, "IP", 2)) {
            int port;
            if (!queueArgs->arg) {
                LOGE("%s(): No port specified for IP socket! "
                    "Bailing out!", __func__);
                free(queueArgs);
                return NULL;
            }
            port = atoi(queueArgs->arg);
            if (queueArgs->xarg) {
                char *host = queueArgs->xarg;
                fd = socket_network_client(host, port, SOCK_STREAM);
            } else
                fd = socket_loopback_client(port, SOCK_STREAM);
        } else if (!strncmp(queueArgs->type, "TTY", 3)) {
            struct termios ios;
            fd = open(queueArgs->arg, O_RDWR);

            /* Disable echo on serial ports. */
            tcgetattr(fd, &ios);
            cfmakeraw(&ios);
            cfsetospeed(&ios, B115200);
            cfsetispeed(&ios, B115200);
            ios.c_cflag |= CREAD | CLOCAL;
            tcflush(fd, TCIOFLUSH);
            tcsetattr(fd, TCSANOW, &ios);
        } else if (!strncmp(queueArgs->type, "CHAR", 4))
            fd = open(queueArgs->arg, O_RDWR);

        if (fd < 0) {
            LOGE("%s() failed to open AT channel type:%s %s %s err:%s. "
                 "retrying in 10 s!",__func__,  queueArgs->type,
                 queueArgs->arg ? queueArgs->arg : "",
                 queueArgs->xarg ? queueArgs->xarg : "",
                 strerror(errno));
            sleep(10);
        }
    }
    ret = at_open(fd, onUnsolicited);

    if (ret < 0) {
        LOGE("%s(): AT error %d on at_open!", __func__, ret);
        goto exit;
    }

    at_set_on_reader_closed(onATReaderClosed);
    at_set_on_timeout(onATTimeout);

    if (!initializeCommon()) {
        LOGE("%s(): initializeCommon() failed!", __func__);
        goto exit;
    }

    q = queueArgs->group->requestQueue;
    q->closed = 0;

    if (queueArgs->group->group == CMD_QUEUE_DEFAULT) {
        if (!initializeDefault()) {
            LOGE("%s() failed to initialize default AT channel!",
                __func__);
            goto exit;
        }
        at_make_default_channel();
    }

    at_set_timeout_msec(1000 * 60 * 3);

    RILRequest *r = NULL;
    RILEvent   *e = NULL;

    LOGI("Looping the requestQueue for index %d!", queueArgs->index);
    for (;;) {
        struct timeval tv;
        struct timespec ts;
        int err;

        memset(&ts, 0, sizeof(ts));

        if ((err = pthread_mutex_lock(&q->queueMutex)) != 0) {
            LOGE("%s() failed to take queue mutex: %s!",
                __func__, strerror(err));
            /* Need to restart all threads and restart modem.*/
            break;
        }

        if (q->closed != 0) {
            LOGW("%s() index %d queue close indication, ending current thread!",
                __func__, queueArgs->index);
            if ((err = pthread_mutex_unlock(&q->queueMutex)) != 0)
                LOGE("Failed to release queue mutex: %s!", strerror(err));
            break;
        }

        while (q->closed == 0 && q->requestList == NULL &&
               q->eventList == NULL) {
            if ((err = pthread_cond_wait(&q->cond, &q->queueMutex)) != 0)
                LOGE("%s() failed to broadcast queue update: %s!",
                    __func__, strerror(err));
        }

        /* eventList is prioritized, smallest abstime first. */
        if (q->closed == 0 && q->requestList == NULL && q->eventList) {
            err = pthread_cond_timedwait(&q->cond, &q->queueMutex,
                                         &q->eventList->abstime);
            if (err && err != ETIMEDOUT)
                LOGE("%s(): Timedwait returned unexpected error: %s!",
                     __func__, strerror(err));
        }

        if (q->closed != 0) {
            if ((err = pthread_mutex_unlock(&q->queueMutex)) != 0)
                LOGW("%s(): Failed to release queue mutex: %s!",
                    __func__, strerror(err));
            break;
        }

        e = NULL;
        r = NULL;

        gettimeofday(&tv, NULL);

        ts.tv_sec = tv.tv_sec;
        ts.tv_nsec = tv.tv_usec * 1000;

        if (q->eventList != NULL &&
            timespec_cmp(q->eventList->abstime, ts, <)) {
            e = q->eventList;
            q->eventList = e->next;
        }

        if (q->requestList != NULL) {
            r = q->requestList;
            q->requestList = r->next;
        }

        if ((err = pthread_mutex_unlock(&q->queueMutex)) != 0)
            LOGW("%s(): Failed to release queue mutex: %s!",
                __func__, strerror(err));

        if (e) {
            e->eventCallback(e->param);
            free(e);
        }

        if (r) {
            processRequest(r->request, r->data, r->datalen, r->token);
            freeRequestData(r->request, r->data, r->datalen);
            free(r);
        }
    }

    /* Final cleanup of queues. Radio state must be unavailable at this point */
    assert(s_state == RADIO_STATE_UNAVAILABLE);

    LOGI("%s() index %d start flushing all remaining requests and events!",
         __func__, queueArgs->index);
    /*
     * NOTE: There cannot be events that will generate response to earlier
     * requests. If so we have to let all events trigger immediatly and refuse
     * further events to be put on the queue.
     */
    /* Request queue cleanup */
    while (q != NULL && q->requestList != NULL) {
        r = q->requestList;
        q->requestList = r->next;
        if(!requestStateFilter(r->request, r->token)) {
            LOGE("%s() tried to send immidiate response to request but it was "
                 "not stopped by filter. Undefined behavior expected! Error!",
                 __func__);
        }
        freeRequestData(r->request, r->data, r->datalen);
        free(r);
    }
    /* Event queue cleanup */
    while (q != NULL && q->eventList != NULL) {
        e = q->eventList;
        q->eventList = e->next;
        free(e);
    }
    LOGI("%s() index %d finished flushing, queues emptied", __func__,
         queueArgs->index);

exit:
    /* Make sure A channel is closed in case queueRunner triggered the exit */
    at_close();
    /*
     * Finally signal RIL Manager that this queueRunner and
     * AT channel is closed.
     */
    signalManager();

    LOGD("%s() thread with index %d ending", __func__, queueArgs->index);
    free(queueArgs);
    return NULL;
}
