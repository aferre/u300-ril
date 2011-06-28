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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#ifndef CAIF_SOCKET_SUPPORT_DISABLED
#include <linux/caif/if_caif.h>
#include "u300-ril-netif.h"
#endif

#include "u300-ril.h"
#include "u300-ril-pdp.h"

#define LOG_TAG "RILV"
#include <utils/Log.h>

typedef struct managerArgs {
    int channels;
    RILRequestGroup *parsedGroups[RIL_MAX_NR_OF_CHANNELS];
    char *type;
    char *args[RIL_MAX_NR_OF_CHANNELS];
    char *xarg;
} managerArgs;

static struct managerArgs mgrArgs;
static bool dbusIsHere = false;

pthread_t s_tid_managerRunner;

static void releaseCommandThreads(void)
{
    pthread_mutex_lock(&ril_manager_wait_mutex);
    managerRelease = true;
    pthread_cond_broadcast(&ril_manager_wait_cond);
    LOGD("%s():Released command execution queue thread(s)", __func__);
    pthread_mutex_unlock(&ril_manager_wait_mutex);
}

static void haltCommandThreads(void)
{
    pthread_mutex_lock(&ril_manager_wait_mutex);
    managerRelease = false;
    LOGD("%s():Halted command execution queue thread(s)", __func__);
    pthread_mutex_unlock(&ril_manager_wait_mutex);
}

/******************************************************************************
 * Start section that includes DBUS communication with MID module             *
 ******************************************************************************/
#ifndef EXTERNAL_MODEM_CONTROL_MODULE_DISABLED
#include <dbus/dbus.h>
#include <poll.h>

#define BUF_MID_RESPONSE_SIZE 32
#define DBUS_MAX_WATCHERS 2         /* 1 for reading, 1 for writing. */
#define DBUS_CONNECTION_NAME        "com.stericsson.mid"
#define DBUS_OBJECT_PATH            "/com/stericsson/mid"
#define DBUS_OBJECT_INTERFACE       "com.stericsson.mid.Modem"

static DBusWatch *used_watches[DBUS_MAX_WATCHERS];
static DBusWatch *unused_watches[DBUS_MAX_WATCHERS];

static int dbus_used_fds = 0;
static int dbus_not_used_fds = 0;

static struct pollfd dbus_used_pollfds_tab[DBUS_MAX_WATCHERS];
static struct pollfd dbus_not_used_pollfds_tab[DBUS_MAX_WATCHERS];

static pthread_mutex_t s_dbus_watch_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_t s_tid_dbusRunner;
#define UNUSED(expr) do { (void)(expr); } while (0)

/* MID signal message handler */
static DBusHandlerResult midSignalHandler(DBusConnection *dbcon, DBusMessage
                                          *msg, void *data)
{
    DBusMessageIter args;
    DBusPendingCall* pending;
    DBusMessage *dbmsg;
    char return_buf[BUF_MID_RESPONSE_SIZE];
    UNUSED(data);
    const char* signame = return_buf;

    /*
     * managerRelease is here used a state indication of QueueRunners.
     * TRUE   = Indicates queuethreads are running normally, any event is can be
     *          considered an new state indication.
     * FASLSE = Indicates queuethreads are already aware of the "restart" and
     *          can be considered in the restarting state.
     */
    if (dbus_message_is_signal(msg, "com.stericsson.mid.Modem",
                               "StateChange")) {
        if (!dbus_message_iter_init(msg, &args)) {
            LOGD("%s(): Message has no arguments!", __func__);
        } else if (DBUS_TYPE_STRING != dbus_message_iter_get_arg_type(&args)) {
            LOGD("%s(): Argument is not string!", __func__);
        } else {
            dbus_message_iter_get_basic(&args, &signame);
            LOGD("%s(): Got Signal with value %s", __func__, signame);

            if (strncmp(signame, "on", 2) == 0) {
                if (managerRelease) {
                    LOGD("%s() Received unexpected \"on\" in already running "
                         "state. Ignored...", __func__);
                } else {
                    LOGD("%s() Received \"on\". Releasing queue threads..."
                         , __func__);
                    releaseCommandThreads();
                }
            } else if (strncmp(signame, "prepare_off", 11) == 0) {
                if (managerRelease) {
                    LOGD("%s(): Received \"prepare_off\". Unhandled..."
                         , __func__);
                    /* TODO: add functionality of early modemcleanup here */
                } else {
                    LOGD("%s(): Received \"prepare_off\". Queue threads (already) "
                         "stopped waiting for \"on\"...", __func__);
                }
            } else if (strncmp(signame, "off", 3) == 0) {
                if (managerRelease) {
                    LOGD("%s(): Received \"off\". Signal queue threads and "
                         "prepare to go back to initial state...", __func__);
                    haltCommandThreads();
                    signalCloseQueues();
                } else {
                    LOGD("%s(): Received \"off\". Queue threads (already) "
                         "stopped waiting for \"on\"...", __func__);
                }
            } else
                LOGD("%s(): message \"%s\" ignored.", __func__, signame);

            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static dbus_bool_t addWatch(DBusWatch *watch, void *data)
{
    UNUSED(data);
    short cond = POLLHUP | POLLERR;
    int fd;
    dbus_bool_t ret = TRUE;
    unsigned int flags;
    dbus_bool_t res;
    int mutexRes;

    fd = dbus_watch_get_fd(watch);
    flags = dbus_watch_get_flags(watch);

    if (flags & DBUS_WATCH_READABLE) {
        cond |= POLLIN;
        LOGD("%s(): RIL get new dbus watch for READABLE condition", __func__);
    }
    if (flags & DBUS_WATCH_WRITABLE) {
        cond |= POLLOUT;
        LOGD("%s(): RIL get new dbus watch for WRITABLE condition", __func__);
    }

    mutexRes = pthread_mutex_lock(&s_dbus_watch_mutex);
    if (mutexRes != 0)
        LOGE("%s(): Unable to take dbus watch mutex", __func__);

    res = dbus_watch_get_enabled(watch);
    if (res) {
        if (dbus_used_fds < DBUS_MAX_WATCHERS) {
            LOGD("%s(): RIL new dbus watch id: %d is marked USED", __func__,
                 dbus_used_fds);
            used_watches[dbus_used_fds] = watch;
            dbus_used_pollfds_tab[dbus_used_fds].fd = fd;
            dbus_used_pollfds_tab[dbus_used_fds].events = cond;
            dbus_used_fds++;
        } else {
            LOGE("%s(): new dbus watch id: %d is marked USED. BUT can not be "
                 "added", __func__, dbus_used_fds);
            goto error;
        }
    } else {
        if (dbus_not_used_fds < DBUS_MAX_WATCHERS) {
            LOGD("%s(): RIL new dbus watch id: %d is marked UNUSED", __func__,
                 dbus_not_used_fds);
            unused_watches[dbus_not_used_fds] = watch;
            dbus_not_used_pollfds_tab[dbus_not_used_fds].fd = fd;
            dbus_not_used_pollfds_tab[dbus_not_used_fds].events = cond;
            dbus_not_used_fds++;
        } else {
            LOGE("%s(): new dbus watch id: %d is marked UNUSED. BUT can not be "
                 "added", __func__, dbus_not_used_fds);
            goto error;
        }
    }

    goto exit;
error:
    ret = FALSE;
exit:
    mutexRes = pthread_mutex_unlock(&s_dbus_watch_mutex);
    if (mutexRes != 0)
        LOGE("%s(): Unable to release dbus watch mutex.", __func__);
    return ret;
}

static void removeWatch(DBusWatch *watch, void *data)
{
    UNUSED(data);
    int i, res, index;
    bool found = false;
    int mutexRes;

    LOGD("%s(): RIL Removing dbus watch", __func__);

    mutexRes = pthread_mutex_lock(&s_dbus_watch_mutex);
    if (mutexRes != 0)
        LOGE("%s(): Unable to take dbus watch mutex", __func__);

    for (i = 0; i < dbus_not_used_fds; i++) {
        if (unused_watches[i] == watch) {
            found = true;
            index = i;
            break;
        }
    }
    if (!found) {
        LOGD("%s(): RIL watch %p not found in unused pool, try used pool...",
             __func__, (void*)watch);
        for (i = 0; i < dbus_used_fds; i++) {
            if (used_watches[i] == watch) {
                found = true;
                index = i;
                break;
            }
        }
        if (!found) {
            LOGE("%s(): RIL watch %p not found in any pool...", __func__,
                 (void*)watch);
            goto exit;
        } else {
            LOGD("%s(): RIL watch %p found in used pool. Removed", __func__,
                 (void*)watch);
            for (i = index; i < (dbus_used_fds - 1); i++) {
                used_watches[i] = used_watches[i + 1];
                memcpy(&dbus_used_pollfds_tab[i], &dbus_used_pollfds_tab[i + 1],
                        sizeof(dbus_used_pollfds_tab[i + 1]));
            }
            used_watches[i] = NULL;
            memset(&dbus_used_pollfds_tab[i], 0,
                    sizeof(dbus_used_pollfds_tab[i]));
            dbus_used_fds--;
        }
    } else {
        LOGD("%s(): RIL watch %p found in unused pool. Removed", __func__,
             (void*)watch);
        for (i = index; i < (dbus_not_used_fds - 1); i++) {
            unused_watches[i] = unused_watches[i + 1];
            memcpy(&dbus_not_used_pollfds_tab[i],
                   &dbus_not_used_pollfds_tab[i + 1],
                   sizeof(dbus_not_used_pollfds_tab[i + 1]));
        }
        unused_watches[i] = NULL;
        memset(&dbus_not_used_pollfds_tab[i], 0,
                sizeof(dbus_not_used_pollfds_tab[i]));
        dbus_not_used_fds--;
    }

exit:
    mutexRes = pthread_mutex_unlock(&s_dbus_watch_mutex);
    if (mutexRes != 0)
        LOGE("%s(): Unable to release dbus watch mutex.", __func__);
}

static void notifyForEvent(int index, short event)  {
    unsigned int flags = 0;

    if (event & POLLIN)
        flags |= DBUS_WATCH_READABLE;
    if (event & POLLOUT)
        flags |= DBUS_WATCH_WRITABLE;
    if (event & POLLHUP)
        flags |= DBUS_WATCH_HANGUP;
    if (event & POLLERR)
        flags |= DBUS_WATCH_ERROR;

    while (!dbus_watch_handle(used_watches[index], flags)) {
        LOGD("%s(): dbus_watch_handle needs more memory. Spinning", __func__);
        sleep(1);
    }
    LOGD("%s(): used id: %d dbus_watch_handle selected for DBUS operation",
         __func__, index);
}

static void processDbusEvent(DBusConnection *dbcon)  {
    int res = 0;
    for (;;) {
        res = dbus_connection_dispatch(dbcon);
        switch (res) {
            case DBUS_DISPATCH_COMPLETE:
                return;
            case DBUS_DISPATCH_NEED_MEMORY:
                LOGD("%s(): dbus_connection_dispatch needs more memory."
                     "spinning", __func__);
                sleep(1);
                break;
            case DBUS_DISPATCH_DATA_REMAINS:
                LOGD("%s(): dispatch: remaining data for DBUS operation."
                     "Spinning", __func__);
                break;
            default:
                /* This should not happen */
                break;
        }
    }
}

static int sendMsgToMidWithoutReply(DBusConnection *dbcon, char *msg)
{
    DBusError err;
    DBusMessage *dbmsg;
    int res = 0;

    /* Initialize the dbus error return value */
    dbus_error_init(&err);

    dbmsg = dbus_message_new_method_call(DBUS_CONNECTION_NAME,
                                         DBUS_OBJECT_PATH,
                                         DBUS_OBJECT_INTERFACE,
                                         msg);
    if (dbmsg == NULL) {
        LOGE("%s(): cannot create message", __func__);
        res = -1;
        goto finally1;
    }

    dbus_message_set_no_reply(dbmsg, TRUE);

    if (!dbus_connection_send(dbcon, dbmsg, NULL)) {
        LOGE("IPC message send error");
        res =  -1;
        goto finally2;
    };

    dbus_connection_flush(dbcon);

finally2:
    /* Drop reference count on the message */
    dbus_message_unref(dbmsg);
finally1:
    dbus_error_free(&err);

    return res;
}

static int requestMIDWithResponse(DBusConnection *dbcon,
                                    char *requestMethod,
                                    char *response)
{
    char temp[BUF_MID_RESPONSE_SIZE];
    char *pTemp = temp;
    int ret = 0;

    DBusPendingCall *pending = NULL;
    DBusMessage *dbmsg = NULL;
    DBusMessageIter args;

    dbmsg = dbus_message_new_method_call(DBUS_CONNECTION_NAME,
                                         DBUS_OBJECT_PATH,
                                         DBUS_OBJECT_INTERFACE,
                                         requestMethod);
    if (dbmsg == NULL) {
        LOGE("%s(): Failed to create a method call", __func__);
        goto error;
    }

    if (!dbus_connection_send_with_reply(dbcon, dbmsg, &pending, -1)) {
        LOGE("%s(): Failed to send method call", __func__);
        goto error;
    }

    if (pending == NULL) {
        LOGE("%s(): Failed to send method call, connection closed", __func__);
        goto error;
    }

    dbus_connection_flush(dbcon);
    dbus_message_unref(dbmsg);
    dbus_pending_call_block(pending);

    dbmsg = dbus_pending_call_steal_reply(pending);
    dbus_pending_call_unref(pending);

    if (dbmsg == NULL) {
        LOGE("%s(): Error on the received message.", __func__);
        goto error;
    }

    if (!dbus_message_iter_init(dbmsg, &args)) {
        LOGE(" %s(): Received message has no arguments!", __func__);
        goto error;
    } else if (DBUS_TYPE_STRING != dbus_message_iter_get_arg_type(&args)) {
        LOGD("%s(): Argument is not a string!", __func__);
        goto error;
    } else {
        dbus_message_iter_get_basic(&args, &pTemp);
        strncpy(response, pTemp, BUF_MID_RESPONSE_SIZE);
        LOGD("%s(): Got message, pTemp:\"%s\", response: \"%s\"",
             __func__, pTemp, response);
    }

    goto exit;
error:
    ret = -1;
exit:
    dbus_message_unref(dbmsg);
    return ret;
}

static bool queryModemOn(DBusConnection *dbcon) {
    char responseArray[BUF_MID_RESPONSE_SIZE];
    char *pResponse = responseArray;

    if (requestMIDWithResponse(dbcon, "GetState", pResponse) != 0) {
        LOGE("%s(): Failed to query state of MID.", __func__);
    } else {
        if (strncmp(pResponse, "on", 2) == 0) {
            return true;
        }
        else
            LOGD("%s(): %s returned and ignored.", __func__, pResponse);
    }
    return false;
}

static void *dbusAndThreadRunner(void *param)
{
    DBusConnection *dbcon = (DBusConnection *)param;
    DBusError err;
    int ret;

    int i;

    if (!dbus_connection_set_watch_functions(dbcon, addWatch,
                     removeWatch, NULL, NULL, NULL)) {
        LOGE("%s(): dbus_connection_set_watch_functions failed.", __func__);
        goto error;
    }

    dbus_error_init(&err);
    /*
     * Adds a match rule to match messages going through the message bus
     * Listen only signal from com.stericsson.mid.Modem interface (MID
     * state changes)
     */
    dbus_bus_add_match(dbcon,
        "type='signal', interface='com.stericsson.mid.Modem'", &err);
    if (dbus_error_is_set(&err)) {
        LOGE("%s(): DBUS match error %s: %s.", __func__, err.name, err.message);
        goto error;
    }

    /* Add a message filter to process incoming messages */
    if (!dbus_connection_add_filter(dbcon,
        (DBusHandleMessageFunction)midSignalHandler, NULL, NULL)) {

        LOGE("%s(): DBUS filter error.", __func__);
        goto error;
    }

    if (queryModemOn(dbcon))
        releaseCommandThreads();

    for (;;) {
        ret = poll(dbus_used_pollfds_tab, DBUS_MAX_WATCHERS, -1);
        if (ret > 0) {
            for (i = 0; i < DBUS_MAX_WATCHERS; i++) {
                if (dbus_used_pollfds_tab[i].revents) {
                    notifyForEvent(i, dbus_used_pollfds_tab[i].revents);
                    processDbusEvent(dbcon);
                }
            }
        }
    }

    return 0;

error:
    LOGE("%s(): Disconnection clean up.", __func__);
    /* Disconnection clean up */
    dbus_bus_remove_match (dbcon,
            "type='signal', interface='com.stericsson.mid.Modem'", &err);
    if (dbus_error_is_set(&err)) {
        LOGE("%s(): DBUS match error %s: %s.", __func__, err.name, err.message);
        dbus_error_free(&err);
        return (void*)-1;
    }
    dbus_connection_remove_filter(dbcon,
        (DBusHandleMessageFunction)midSignalHandler, NULL);
    dbus_connection_unref(dbcon);
    return NULL;
}
#endif /* EXTERNAL_MODEM_CONTROL_MODULE_DISABLED */
/******************************************************************************
 * End section that includes DBUS communication with MID module               *
 ******************************************************************************/

pthread_t s_tid_queueRunner[RIL_MAX_NR_OF_CHANNELS];

static void *managerRunner(void *param)
{
    int activeThreads;
    int ret;
    int i;
    pthread_attr_t attr;
    struct queueArgs *queueArgs[RIL_MAX_NR_OF_CHANNELS] = { NULL, NULL };

    for(;;) {
        activeThreads = 0;
        ret = 0;

        for (i = 0; i < mgrArgs.channels; i++) {
            int err;
            queueArgs[i] = malloc(sizeof(struct queueArgs));
            memset(queueArgs[i], 0, sizeof(struct queueArgs));

            queueArgs[i]->channels = mgrArgs.channels;
            queueArgs[i]->group = mgrArgs.parsedGroups[i];
            queueArgs[i]->type = mgrArgs.type;
            queueArgs[i]->index = i;
            queueArgs[i]->arg = mgrArgs.args[i];
            queueArgs[i]->xarg = mgrArgs.xarg;

            if ((err = pthread_attr_init(&attr)) != 0)
                LOGE("%s() failed to initialize pthread attribute: %s",
                    __func__, strerror(err));

            if ((err = pthread_attr_setdetachstate(&attr,
                    PTHREAD_CREATE_DETACHED)) != 0)
                LOGE("%s() failed to set the "
                     "PTHREAD_CREATE_DETACHED attribute: %s",
                     __func__, strerror(err));

            if ((err = pthread_create(&s_tid_queueRunner[i], &attr,
                 queueRunner, queueArgs[i])) != 0) {
                LOGE("%s() failed to create queue runner thread: %s",
                    __func__, strerror(err));
                free(queueArgs[i]);
            } else {
                activeThreads++;
            }
            /* memory is freed within the queuerunner assigned the args */
            queueArgs[i] = NULL;
        }

        /* Since dbus is not enabled we start threads instantly */
        if (!dbusIsHere)
            releaseCommandThreads();

        ret = pthread_mutex_lock(&ril_manager_queue_exit_mutex);
        if (ret != 0)
            LOGE("%s(): Failed to get mutex lock. err: %s", __func__,
                 strerror(-ret));

        while (activeThreads > 0) {
            ret = pthread_cond_wait(&ril_manager_queue_exit_cond,
                                    &ril_manager_queue_exit_mutex);
            if (ret != 0)
                LOGE("%s(): pthread_cond_wait Failed. err: %s", __func__,
                    strerror(-ret));
            else {
                activeThreads--;
            }
        }
        ret = pthread_mutex_unlock(&ril_manager_queue_exit_mutex);
        if (ret != 0)
            LOGE("%s(): Failed to unlock mutex. err: %s", __func__,
                 strerror(-ret));

#ifndef EXTERNAL_MODEM_CONTROL_MODULE_DISABLED
        DBusConnection *dbcon = (DBusConnection *)param;
        char responseArray[BUF_MID_RESPONSE_SIZE];
        char *pResponse = responseArray;

        /* Signal MID to restart Modem */
        if (requestMIDWithResponse(dbcon, "Reboot", pResponse) != 0) {
            LOGE("%s(): Failed to reboot modem."
                    "Restarting threads anyway.", __func__);
        } else {
            if (strncmp(pResponse, "OK", 2) == 0) {
                LOGI("%s(): %s returned. Modem restarting!",
                        __func__, pResponse);
                /* Instruct QueueRunners to wait for 'on' from MID */
                haltCommandThreads();
            }
            else
                /*
                 * In the event we are not allowed to do a modem reboot we have
                 * little to do but try a direct restart of the queuerunners.
                 * AT channels will anyway be re-opened.
                 */
                LOGD("%s(): %s returned from MID on \"reboot\" request. "
                     "Continuing... (letting queue runners execute immediatly)",
                     __func__, pResponse);
        }
#endif /* EXTERNAL_MODEM_CONTROL_MODULE_DISABLED */
    }
    return NULL;
}

#ifndef CAIF_SOCKET_SUPPORT_DISABLED
static bool createNetworkInterface(const char *ifname, int connection_id)
{
    int ret;
    int ifindex = -1;
    char loop = 0;
    char ifnamecpy[MAX_IFNAME_LEN];
    bool success = true;

    strncpy(ifnamecpy, ifname, MAX_IFNAME_LEN);
    ifnamecpy[MAX_IFNAME_LEN - 1] = '\0';

    ret = rtnl_create_caif_interface(IFLA_CAIF_IPV4_CONNID, connection_id,
                                     ifnamecpy, &ifindex, loop);
    if (!ret)
        LOGI("%s() created CAIF net-interface: Name = %s, connection ID = %d, "
             "Index = %d", __func__, ifnamecpy, connection_id, ifindex);
    else if (ret == -EEXIST) /* Use the existing interface, NOT an error. */
        LOGI("%s() found existing CAIF net-interface with same name, reusing: "
             "Name = %s, connection ID = %d, Index = %d",
             __func__, ifnamecpy, connection_id, ifindex );
    else {
        LOGE("%s() failed creating CAIF net-interface. errno: %d (%s)!",
             __func__, errno, strerror(errno));
        success = false;
    }

    if (strncmp(ifnamecpy, ifname, MAX_IFNAME_LEN) != 0) {
        LOGE("%s() did not get required interface name. Suggested %s but got "
             "%s. This is considered an error.", __func__, ifname, ifnamecpy);
        success = false;
    }

    return success;
}
#endif

static void usage(char *s)
{
    fprintf(stderr, "usage: %s [-c <connection type>]"
            "[-c <channel type>] "
            "[-g <groups of RIL commands tied to separate AT channels>] "
            "[-p <primary channel argument>] "
            "[-s <secondary channel argument>] "
            "[-x <extra argument>] "
            "[-i <network interface>]\n", s);
    exit(-1);
}

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc,
                                   char **argv)
{
    int opt;
    int i;
    int err;
    char *groups = NULL;
    pthread_attr_t attr;

    /* Initialize manager arguments */
    mgrArgs.channels = 2;
    mgrArgs.type = NULL;
    mgrArgs.xarg = NULL;

    LOGI("**************************************************\n"
        "Starting ST-Ericsson RIL...\n"
        "**************************************************");
    LOGI("%s()", __func__);

    s_rilenv = env;

    while (-1 != (opt = getopt(argc, argv, "c:n:g:p:s:x:i:"))) {
        switch (opt) {
        case 'c':
            mgrArgs.type = optarg;
            LOGI("Using channel type %s.", mgrArgs.type);
            break;

        case 'n':
            LOGW("-n is deprecated. Use -g instead.");
            break;

        case 'g':
            groups = optarg;
            mgrArgs.channels = parseGroups(groups, mgrArgs.parsedGroups);
            LOGI("RIL command group(s) "
                "(DEFAULT and AUXILIARY may be omitted): %s", groups);
            break;

        case 'p':
            mgrArgs.args[0] = optarg;
            LOGI("Primary AT channel: %s", mgrArgs.args[0]);
            break;

        case 's':
            mgrArgs.args[1] = optarg;
            LOGI("Secondary AT channel: %s", mgrArgs.args[1]);
            break;

        case 'x':
            mgrArgs.xarg = optarg;
            LOGI("Extra argument %s.", mgrArgs.xarg);
            break;

        case 'i':
            strncpy(ril_iface, optarg, MAX_IFNAME_LEN);
            ril_iface[MAX_IFNAME_LEN - 1] = '\0';
            LOGI("Using network interface %s as prefix for data channel.",
                 ril_iface);
            break;

        default:
            goto error;
        }
    }

    if (groups == NULL || strcmp(groups, "") == 0) {
        LOGI("%s(): RIL command groups was not supplied. Using default "
            "configuration DEFAULT and AUXILIARY groups (2 AT channels).",
            __func__);
        mgrArgs.channels = parseGroups("", mgrArgs.parsedGroups);
    }

    if (ril_iface == NULL || strcmp(ril_iface, "") == 0) {
        LOGW("%s(): Network interface was not supplied."
            " Falling back to rmnet!", __func__);
        strcpy(ril_iface, "rmnet");
    }

#ifndef CAIF_SOCKET_SUPPORT_DISABLED
    if (mgrArgs.type == NULL || strncasecmp(mgrArgs.type, "", 1) == 0) {
        LOGW("%s: AT/Data channel type was not supplied."
             " Falling back to CAIF!", __func__);
        mgrArgs.type = "CAIF";
    }

    if (strncasecmp(mgrArgs.type, "CAIF", 4) == 0) {
        for (i = 0; i < RIL_MAX_NUMBER_OF_PDP_CONTEXTS; i++) {
            char ifaceName[MAX_IFNAME_LEN];
            snprintf(ifaceName, MAX_IFNAME_LEN, "%s%d", ril_iface, i);
            if (!createNetworkInterface(ifaceName, i + RIL_FIRST_CID_INDEX))
                goto error;
        }
    }
#else
    if (mgrArgs.type == NULL || strcmp(mgrArgs.type, "") == 0) {
        LOGE("%s(): AT/Data channel type was not supplied!", __func__);
        goto error;
    }
#endif

#ifndef EXTERNAL_MODEM_CONTROL_MODULE_DISABLED
    DBusError dbusErr;
    DBusConnection *dbcon;
    dbus_error_init(&dbusErr);

    /* Connect to system dbus */
    dbcon = dbus_bus_get(DBUS_BUS_SYSTEM, &dbusErr);
    haltCommandThreads();
    if (dbcon == NULL || dbus_error_is_set(&dbusErr)) {
        LOGW("[DBUS]: DBUS interface unavailable. No communication with MID.");
    } else {
        LOGI("[DBUS]: Connected to system dbus.");
        dbusIsHere = true;
        err = pthread_attr_init(&attr);
        if (err != 0)
            LOGW("%s(): Failed to initialize dbus pthread attribute: %s",
                 __func__, strerror(err));

        err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (err != 0)
            LOGW("%s(): Failed to set the dbus PTHREAD_CREATE_DETACHED "
                 "attribute: %s", __func__, strerror(err));

        err = pthread_create(&s_tid_dbusRunner, &attr,
                             dbusAndThreadRunner, (void *)dbcon);
        if (err != 0) {
            LOGE("%s(): Failed to create dbus runner thread: %s", __func__,
                 strerror(err));
            dbusIsHere = false;
        }
    }
#endif /* EXTERNAL_MODEM_CONTROL_MODULE_DISABLED */

    /* Start Manager thread. */
    err = pthread_attr_init(&attr);
    if (err != 0)
        LOGW("%s(): Failed to initialize RIL Manager pthread attribute: %s",
             __func__, strerror(err));

    err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (err != 0)
        LOGW("%s(): Failed to set the RIL Manager PTHREAD_CREATE_DETACHED "
             "attribute: %s", __func__, strerror(err));

#ifndef EXTERNAL_MODEM_CONTROL_MODULE_DISABLED
    err = pthread_create(&s_tid_managerRunner, &attr,
                         managerRunner, (void *)dbcon);
#else
    err = pthread_create(&s_tid_managerRunner, &attr,
                         managerRunner, NULL);
#endif /* EXTERNAL_MODEM_CONTROL_MODULE_DISABLED */

    if (err != 0)
        LOGE("%s(): Failed to create RIL manager runner thread: %s", __func__,
             strerror(err));

    return &g_callbacks;

error:
    LOGE("%s() failed to parse RIL command line!", __func__);
    usage(argv[0]);
    return NULL;
}
