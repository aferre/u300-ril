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
** Author: Kim Tommy Humborstad <kim.tommy.humborstad@stericsson.com>
*/

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <linux/route.h>
#include <cutils/properties.h>
#include <ifc_utils.h>

#include <telephony/ril.h>

#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include "u300-ril.h"
#include "u300-ril-pdp.h"

#define LOG_TAG "RILV"
#include <utils/Log.h>

/* This should have been exported by ifc_utils.h */
void ifc_close(void);

/* Last data call fail cause, obtained by *CEER. */
static RIL_LastDataCallActivateFailCause s_lastDataCallFailCause =
                                                    PDP_FAIL_ERROR_UNSPECIFIED;

/* OEM callback issued when a OEM activated context is deactivated */
static void (*pdpOemDeativatedCB)(int profileId) = NULL;

/* Connection ID will be implicitly decided by array index */
typedef struct pdpContextEntry {
    int state;      /* free(0), in use(1), reserved(2) */
    int active;     /* deactivated(0), activated(1) */
    int pid;        /* Profil ID */
    char *APN;      /* Access Point Name */
    int OEM;        /* locally created or external (OEM use) */
} pdpContextEntry;

/* Maintained list of PDP contexts */
static pdpContextEntry pdpContextList[RIL_MAX_NUMBER_OF_PDP_CONTEXTS] =
                        {{0,0,-1,NULL,-1},{0,0,-1,NULL,-1},{0,0,-1,NULL,-1},
                         {0,0,-1,NULL,-1},{0,0,-1,NULL,-1},{0,0,-1,NULL,-1}};

static pthread_mutex_t contextListMutex = PTHREAD_MUTEX_INITIALIZER;

/* convertAuthenticationMethod */
static char* convertAuthenticationMethod(const char *authentication)
{
    long int auth;
    char *end;

    /*
     * AT requires a bitstring for the authentication methods accepted
     * bit 0 - none authentication
     * bit 1 - pap
     * bit 2 - chap
     */
    if (authentication == NULL) /* chap + pap + none */
        return "111";

    auth = strtol(authentication, &end, 10);

    switch (auth) {
    case 0:    /*PAP and CHAP is never performed., only none*/
        return "001";
    case 1:    /*PAP may be performed; CHAP is never performed.*/
        return "011";
    case 2:    /*CHAP may be performed; PAP is never performed.*/
        return "101";
    case 3:    /*PAP / CHAP may be performed - baseband dependent.*/
        return "111";
    default:
        break;
    }

    return NULL;
}

/**
 * getCurrentPacketSwitchedBearer
 *
 * Queries the status of current packet switched bearer.
 * It returns 1 if the bearer is found, 0 if not.
 */
static int getCurrentPacketSwitchedBearer(int *curr_bearer)
{
    ATResponse *atresponse = NULL;
    char *line = NULL;
    int err, success = 0;
    int temp;

    if (curr_bearer == NULL) {
        LOGE("%s called with invalid input parameters!", __func__);
        return 0;
    }

    (void)at_send_command("AT*EPSB=1", NULL);

    err = at_send_command_singleline("AT*EPSB?", "*EPSB:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &temp);
    if (err < 0)
        goto error;

    /* In case we didn't fetch the UR we need to check next parameter as well */
    if (at_tok_hasmore(&line)){
        err = at_tok_nextint(&line, &temp);
        if (err < 0)
            goto error;
    }

    *curr_bearer = temp;
    success = 1;
    goto exit;

error:
    LOGE("%s failed to execute AT*EPSB correctly, check AT log", __func__);

exit:
    (void)at_send_command("AT*EPSB=0", NULL);
    at_response_free(atresponse);

    return success;
}

/**
 * cleanupPDPContextList
 *
 * Update internal list compared to modem list.
 * Deactivated entries in the modem will be removed both from internal list and
 * modem.
 */
static void cleanupPDPContextList(RIL_Data_Call_Response *list, int entries)
{
    int i;
    int profileId = -1;
    int oem = 0;
    int active = 0;
    int handle = 0;
    char *cmd = NULL;
    char curIfName[MAX_IFNAME_LEN] = "";
    char *property = NULL;

    if (list == NULL)
        return; /* TODO: clean all entries? */

    /* Match deactivated entries with pdpcontext list */
    for (i = 0; i < entries; i++) {
        /* Find non activated entries in modem list */
        if (list[i].active > 0)
            continue;

        /* Find corresponding active entry in RIL list */
        char cidStr[3];
        (void)snprintf(cidStr, 3, "%d", list[i].cid);
        handle = pdpListGet(cidStr, NULL,
                            NULL, &profileId, curIfName, NULL, &active, &oem);
        if (handle < 0)
            continue;

        /*
         * NOTE: List handle must be unreserved (put|undo|free) before break or
         * continue is called.
         */
        if (active == 0) { /* Normal case. Configured not active */
            (void)pdpListUndo(handle);
            continue;
        }

        /* An entry is deactivated in modem but active in list - remove it */
        /*  -> from list */
        if (!pdpListFree(handle)) {
            LOGE("%s() failed to remove entry from PDP context list", __func__);
            continue;
        }

        /*  -> from modem */
        asprintf(&cmd, "AT*EIAD=%d,1", list[i].cid);
        (void)at_send_command(cmd, NULL);
        free(cmd);

        /*  -> from interfaces (DOWN) */
        if (!ifc_init()) {
            if (ifc_down(curIfName))
                LOGE("%s() failed to bring down %s!", __func__, curIfName);
            ifc_close();
        } else {
            LOGE("%s() failed to set up ifc. Can not bring down interface %s!",
                 __func__, curIfName);
        }

        /*  -> from properties */
        if (curIfName != NULL) {
            asprintf(&property, "net.%s.gw", curIfName);
            (void)property_set(property, "");
            free(property);
            asprintf(&property, "net.%s.dns1", curIfName);
            (void)property_set(property, "");
            free(property);
            asprintf(&property, "net.%s.dns2", curIfName);
            (void)property_set(property, "");
            free(property);
        }

        /*  -> from OEM framework */
        if (pdpOemDeativatedCB != NULL && oem) {
            pdpOemDeativatedCB(profileId);
        }
    }
}

/* requestOrSendPDPContextList */
static void requestOrSendPDPContextList(RIL_Token *token)
{
    ATResponse *atresponse = NULL;
    RIL_Data_Call_Response *responses = NULL;
    ATLine *cursor;
    int err;
    int number_of_contexts = 0;
    int i = 0;
    int curr_bearer, fetched;
    char *out;

    /* Read the activation states */
    err = at_send_command_multiline("AT+CGACT?", "+CGACT:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    /* Calculate size of buffer to allocate*/
    for (cursor = atresponse->p_intermediates; cursor != NULL;
         cursor = cursor->p_next)
        number_of_contexts++;

    if (number_of_contexts == 0)
        /* return empty list (NULL with size 0) */
        goto finally;

    responses = alloca(number_of_contexts * sizeof(RIL_Data_Call_Response));
    memset(responses, 0, sizeof(responses));

    for (i = 0; i < number_of_contexts; i++) {
        responses[i].cid = -1;
        responses[i].active = -1;
    }

    /*parse the result*/
    i = 0;
    fetched = 0;

    for (cursor = atresponse->p_intermediates; cursor != NULL;
         cursor = cursor->p_next) {
        char *line = cursor->line;
        int state;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &responses[i].cid);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &state);
        if (err < 0)
            goto error;

        if (state == 0)
            responses[i].active = 0;  /* 0=inactive */
        else {
            /* active, but we need to find out if physical link is up */
            if (!fetched && getCurrentPacketSwitchedBearer(&curr_bearer))
                fetched = 1;

            /*
             * curr_bearer set to 0 mean that the physical link is down.
             * Any other bearer number indicate that the physical link is up.
             */
            if (fetched && curr_bearer == 0)
                responses[i].active = 1; /* 1=active/physical link down */
            else /* (defaulting to physical link up) */
                responses[i].active = 2; /* 2=active/physical link up */
        }

        i++;
    }
    at_response_free(atresponse);
    atresponse = NULL;

    /* Read the currend pdp settings */
    err = at_send_command_multiline("AT+CGDCONT?", "+CGDCONT:", &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto error;

    for (cursor = atresponse->p_intermediates; cursor != NULL;
         cursor = cursor->p_next) {
        char *line = cursor->line;
        int cid;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &cid);
        if (err < 0)
            goto error;

        for (i = 0; i < number_of_contexts; i++)
            if (responses[i].cid == cid)
                break;

        if (i >= number_of_contexts)
            /* Details for a context we didn't hear about in the last request.*/
            continue;

        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        responses[i].type = alloca(strlen(out) + 1);
        strcpy(responses[i].type, out);

        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        responses[i].apn = alloca(strlen(out) + 1);
        strcpy(responses[i].apn, out);

        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        responses[i].address = alloca(strlen(out) + 1);
        strcpy(responses[i].address, out);
    }

finally:
    if (token != NULL)
        RIL_onRequestComplete(*token, RIL_E_SUCCESS, responses,
                           number_of_contexts * sizeof(RIL_Data_Call_Response));
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, responses,
                           number_of_contexts * sizeof(RIL_Data_Call_Response));

    /*
     * To keep internal list up to date all deactivated contexts are removed
     * from modem and interface is set to DOWN...
     */
    cleanupPDPContextList(responses, number_of_contexts);

    goto exit;

error:
    if (token != NULL)
        RIL_onRequestComplete(*token, RIL_E_GENERIC_FAILURE, NULL, 0);
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, NULL, 0);

exit:
    at_response_free(atresponse);
}

/**
 * pdpListExist()
 *
 * Looks for matching existing indexes of Connection ID, APN and Profile ID in
 * pdp context list. Note that cid/profileId set to NULL will not be
 * evaluated.
 *
 * Fills cid, ifName, profileid, active and oem if given and entry exists.
 * (can be set to NULL if not needed)
 *
 * Returns true if found, false if not found.
 */
bool pdpListExist(const char *cidToFind, const char *profileToFind,
                  int *cid, int *profile, char ifName[], int *active, int *oem)
{
    int i;
    char *end = NULL;
    bool found = false;
    int err;

    if ((err = pthread_mutex_lock(&contextListMutex)) != 0) {
        LOGE("%s() failed to take list mutex: %s!", __func__, strerror(err));
        assert(0);
    }

    for (i = 0; i < RIL_MAX_NUMBER_OF_PDP_CONTEXTS; i++) {
        if ((cidToFind != NULL &&
             strtol(cidToFind, &end, 10) == i + RIL_FIRST_CID_INDEX) ||
            (profileToFind != NULL &&
             strtol(profileToFind, &end, 10) == pdpContextList[i].pid)) {

            found = true;
            if (cid != NULL)
                *cid = i + RIL_FIRST_CID_INDEX;
            if (profile != NULL)
                *profile = pdpContextList[i].pid;
            if (active != NULL)
                *active = pdpContextList[i].active;
            if (oem != NULL)
                *oem = pdpContextList[i].OEM;
            if (ifName != NULL) {
                (void)snprintf(ifName, MAX_IFNAME_LEN, "%s%d", ril_iface, i);
                ifName[MAX_IFNAME_LEN - 1] = '\0';
            }

            break;
        }
    }

    if ((err = pthread_mutex_unlock(&contextListMutex)) != 0) {
        LOGE("%s() failed to release state mutex: %s", __func__, strerror(err));
        assert(0);
    }

    return found;
}


/**
 * pdpListGet()
 *
 * Looks for matching indexes of Connection ID and Profile ID in pdp
 * context list. Note that cid/profileId set to NULL will not be evaluated.
 *
 * Fills cid, profile, ifName, apn, active and oem if given and entry is found.
 * (can be set to NULL if not needed)
 *
 * Note that if entry is found and a handle is returned the handle needs to be
 * released later through put/undo/free!
 *
 * Returns
 *  pdplistentry handle if found,
 *  -1 if not found.
 *  -2 if already reserved
 */
int pdpListGet(const char *cidToFind, const char *profileIdToFind,
               int *cid, int *profile, char ifName[], char **apn, int *active,
               int *oem)
{
    int i;
    int pdpListHandle = -1;
    char *end = NULL;
    int err;

    if ((err = pthread_mutex_lock(&contextListMutex)) != 0) {
        LOGE("%s() failed to take list mutex: %s!", __func__, strerror(err));
        assert(0);
    }

    for (i = 0; i < RIL_MAX_NUMBER_OF_PDP_CONTEXTS; i++) {

        if (pdpContextList[i].state == 0)
            continue;

        if ((cidToFind != NULL &&
             strtol(cidToFind, &end, 10) == i + RIL_FIRST_CID_INDEX) ||
            (profileIdToFind != NULL &&
             strtol(profileIdToFind, &end, 10) == pdpContextList[i].pid)) {

            if (pdpContextList[i].state != 1) {
                LOGD("%s() attempted on already reserved index", __func__);
                pdpListHandle = -2;
                break;
            }

            /* Entry found */
            pdpContextList[i].state = 2; /* Reserved */
            pdpListHandle = i;

            if (cid != NULL)
                *cid = i + RIL_FIRST_CID_INDEX;
            if (profile != NULL)
                *profile = pdpContextList[i].pid;
            if (apn != NULL)
                *apn = pdpContextList[i].APN;
            if (active != NULL)
                *active = pdpContextList[i].active;
            if (oem != NULL)
                *oem = pdpContextList[i].OEM;
            if (ifName != NULL) {
                (void)snprintf(ifName, MAX_IFNAME_LEN, "%s%d", ril_iface, i);
                ifName[MAX_IFNAME_LEN - 1] = '\0';
            }
            break;
        }
    }

    if ((err = pthread_mutex_unlock(&contextListMutex)) != 0) {
        LOGE("%s() failed to release state mutex: %s", __func__,
             strerror(err));
        assert(0);
    }

    return pdpListHandle;
}

/**
 * pdpListGetFree()
 *
 * Finds a free entry in the pdp context list and RESERVES it.
 *
 * cid cannot be NULL and will always be set in successful case.
 * ifName will be set in case of success and if it is not NULL.
 *
 * Returns handle to entry if found, -1 if not found.
 */
int pdpListGetFree(int *cid, char ifName[])
{
    int i = -1;
    bool found = false;
    int err;

    if (cid == NULL)
        goto exit;

    if ((err = pthread_mutex_lock(&contextListMutex)) != 0) {
        LOGE("%s() failed to take list mutex: %s!", __func__, strerror(err));
        assert(0);
    }

    for (i = 0; i < RIL_MAX_NUMBER_OF_PDP_CONTEXTS; i++) {
        if (pdpContextList[i].state == 0) {
            found = true;
            break;
        }
    }

    if (found) {
        /*
         * Generating:
         *  Connection ID (array index + first cid index)
         *  Interface Name (ril_iface(Connection ID - 1))
         */
        *cid = i + RIL_FIRST_CID_INDEX;
        if (ifName != NULL) {
            (void) snprintf(ifName, MAX_IFNAME_LEN, "%s%d", ril_iface, i);
            ifName[MAX_IFNAME_LEN -1] = '\0';
        }
        pdpContextList[i].state = 2; /* reserved */
    } else {
        i = -1;
    }

    if ((err = pthread_mutex_unlock(&contextListMutex)) != 0) {
        LOGE("%s() failed to release state mutex: %s", __func__,
             strerror(err));
        assert(0);
    }

exit: /* Note: label does not release lock, use with caution */
    return i;
}

/**
 * pdpListPut()
 *
 * Sets an entry in the PDP context list and UNRESERVES it.
 * Returns true if successful, false if unsuccessful.
 */
bool pdpListPut(int pdpListHandle, int profile, const char *apn, int activated,
                int oem)
{
    bool success = true;
    int err;

    if ((err = pthread_mutex_lock(&contextListMutex)) != 0) {
        LOGE("%s() failed to take list mutex: %s!", __func__, strerror(err));
        assert(0);
    }

    if (pdpListHandle < 0 || pdpListHandle >= RIL_MAX_NUMBER_OF_PDP_CONTEXTS ||
        pdpContextList[pdpListHandle].state != 2) {
        LOGD("%s() attempted on a non-reserved list entry, error!", __func__);
        success = false;
    }

    if (success) {
        pdpContextList[pdpListHandle].pid = profile;
        if (apn != NULL)
            pdpContextList[pdpListHandle].APN = strdup(apn);
        pdpContextList[pdpListHandle].OEM = oem;
        pdpContextList[pdpListHandle].active = activated;
        pdpContextList[pdpListHandle].state = 1; /* in use */
    }

    if ((err = pthread_mutex_unlock(&contextListMutex)) != 0) {
        LOGE("%s() failed to release state mutex: %s", __func__,
             strerror(err));
        assert(0);
    }

    return success;
}

/**
 * pdpListFree()
 *
 * Free/clears an entry in the PDP context list and UNRESERVES it.
 * Returns true if successful, false if unsuccessful.
 */
bool pdpListFree(int pdpListHandle)
{
    bool success = true;
    int err;

    if ((err = pthread_mutex_lock(&contextListMutex)) != 0) {
        LOGE("%s() failed to take list mutex: %s!", __func__, strerror(err));
        assert(0);
    }

    if (pdpListHandle < 0 || pdpListHandle >= RIL_MAX_NUMBER_OF_PDP_CONTEXTS ||
        pdpContextList[pdpListHandle].state != 2) {
        LOGD("%s() attempted on a non-reserved list entry, error!", __func__);
        success = false;
    }

    if (success) {
        pdpContextList[pdpListHandle].state = 0; /* free */
        pdpContextList[pdpListHandle].active = -1;
        pdpContextList[pdpListHandle].pid = -1;
        free(pdpContextList[pdpListHandle].APN);
        pdpContextList[pdpListHandle].APN = NULL;
        pdpContextList[pdpListHandle].OEM = -1;
    }

    if ((err = pthread_mutex_unlock(&contextListMutex)) != 0) {
        LOGE("%s() failed to release state mutex: %s", __func__,
             strerror(err));
        assert(0);
    }

    return success;
}

/**
 * pdpListUndo()
 *
 * Unreserves an entry in the PDP context list without doing any changes to
 * pdp list entry. This also frees and newly reserved free entry.
 * Returns true if successful, false if unsuccessful.
 */
bool pdpListUndo(int pdpListHandle)
{
    bool success = true;
    int err;

    if ((err = pthread_mutex_lock(&contextListMutex)) != 0) {
        LOGE("%s() failed to take list mutex: %s!", __func__, strerror(err));
        assert(0);
    }

    if (pdpListHandle < 0 || pdpListHandle >= RIL_MAX_NUMBER_OF_PDP_CONTEXTS ||
        pdpContextList[pdpListHandle].state != 2) {
        LOGD("%s() attempted on a non-reserved list entry, error!", __func__);
        success = false;
    }

    if (success) {
        /* if just created... set free */
        if (pdpContextList[pdpListHandle].APN == NULL &&
            pdpContextList[pdpListHandle].pid == -1)
            pdpContextList[pdpListHandle].state = 0; /* free */
        else
            pdpContextList[pdpListHandle].state = 1; /* in use */
    }

    if ((err = pthread_mutex_unlock(&contextListMutex)) != 0) {
        LOGE("%s() failed to release state mutex: %s", __func__,
             strerror(err));
        assert(0);
    }

    return success;
}

/**
 * onEPSBReceived()
 * Handling of unsolicited event *EPSB
 */
void onEPSBReceived(const char *s)
{
    static int lastBearer = -1;

    char *line;
    char *tok;
    int err;
    int currBearer;

    /*
     * Checking if there was a change in dormancy if so report
     * PDP Context List changed.
     */
    tok = line = strdup(s);
    assert(tok != NULL);

    /* <curr_bearer> */
    err = at_tok_start(&tok);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&tok, &currBearer);
    if (err < 0)
        goto error;

    currBearer = (currBearer?1:0);
    /*
     * curr   last (send event)
     *   0  ^ -1  =  send event
     *   0  ^  0  =  -
     *   0  ^  1  =  send event
     *   1  ^ -1  =  send event
     *   1  ^  0  =  send event
     *   1  ^  1  =  -
     */
    if (lastBearer^currBearer) {
        lastBearer = currBearer;
        /*
         * Note: There is a small chance that the bearer change again before
         * we get to send the changelist. In this case we might end up sending
         * the same info two time in contextlistchanged event.
         * This is not considered to be a problem for Android.
         */
        enqueueRILEvent(CMD_QUEUE_AUXILIARY, onPDPContextListChanged,
                        NULL, NULL);
    }

finally:
    free(line);
    return;

error:
    LOGE("Func %s failed to decode *EPSB AT string", __func__);
    goto finally;
}

/**
 * RIL_UNSOL_DATA_CALL_LIST_CHANGED
 *
 * Indicate a PDP context state has changed, or a new context
 * has been activated or deactivated.
 *
 * See also: RIL_REQUEST_DATA_CALL_LIST
 */
void onPDPContextListChanged(void *param)
{
    requestOrSendPDPContextList(NULL);
}

/**
 * RIL_REQUEST_DATA_CALL_LIST
 *
 * Queries the status of PDP contexts, returning for each
 * its CID, whether or not it is active, and its PDP type,
 * APN, and PDP adddress.
 */
void requestPDPContextList(void *data, size_t datalen, RIL_Token t)
{
    requestOrSendPDPContextList(&t);
}

/**
 * RIL_REQUEST_SETUP_DATA_CALL
 *
 * Configure and activate PDP context for default IP connection.
 *
 * See also: RIL_REQUEST_DEACTIVATE_DATA_CALL
 */
void requestSetupDataCall(void *data, size_t datalen, RIL_Token t)
{
    const char *radioTech = NULL;
    const char *dataProfile = NULL;
    const char *APN = NULL;
    const char *username = NULL;
    const char *password = NULL;
    const char *authentication = NULL;

    char *cmd = NULL;
    ATResponse *atresponse = NULL;
    ATLine *currentLine = NULL;
    char *auth = NULL;
    char *end = NULL;
    int err;

    char *property = NULL;
    char *ipAddrStr = NULL;
    char *subnetMaskStr = NULL;
    char *defaultGatewayStr = NULL;
    char *mtuStr = NULL;
    char **rilResponse = NULL;
    in_addr_t addr, subaddr;

    char curIfName[MAX_IFNAME_LEN] = "";
    char *curCidStr = NULL;
    int curCid = -1;
    int curActive = -1;
    int curOem = -1;
    char *curApn = NULL;
    int pdpListHandle = -1;

    /* Assigning parameters */
    radioTech = ((const char **) data)[0];
    dataProfile = ((const char **) data)[1];
    APN = ((const char **) data)[2];
    username = ((const char **) data)[3];
    password = ((const char **) data)[4];
    authentication = ((const char **) data)[5];

    /* Check type, only GSM/WCDMA support */
    if (!strtol(radioTech, &end, 10))
        goto error;

    auth = convertAuthenticationMethod(authentication);
    if (auth == NULL)
        goto error;

    /* ---------------------------------------------------------------------- *
     * ------------------ FINDING AVAILABLE CONNECTION ID ------------------- *
     * ---------------------------------------------------------------------- */
    /* Check for already existing entry to use (configured via OEM) */
    pdpListHandle = pdpListGet(NULL, dataProfile, &curCid, NULL, curIfName,
                               &curApn, &curActive, &curOem);
    if (pdpListHandle >= 0) {
        /* Found existing entry in internal list */
        if (curActive == 1) {
            LOGE("%s() was called with setup on already activated PDP Context. "
                 "Rejecting data call setup.", __func__);
            goto error__unreserve_list_entry;
        }

        /* entry is found but not activated */
        /* Use APN from stored profile if not defined in request */
        if (APN == NULL)
            APN = curApn;

        LOGI("%s() using existing but not activated Connection ID (%d) and "
             "Interface Name (%s)", __func__, curCid, curIfName);

    } else {
        /* Finding free entry in PDP List */
        if ((pdpListHandle = pdpListGetFree(&curCid, curIfName)) < 0) {
            LOGE("%s() was called with already maximum number of activated PDP "
                 "contexts. Rejecting data call setup.", __func__);
            goto error;
        }

        LOGI("%s() selected new Connection ID (%d) and Interface Name (%s)",
             __func__, curCid, curIfName);
    }
    /*
     * NOTE:
     *  - List handle must be unreserved (put|undo|free) before exit.
     *    (goto exit or error not allowed!!)
     *  - Some networks support setting up PDP context without giving an APN.
     */
    /* ---------------------------------------------------------------------- *
     * ----------------- SETTING UP PDP ACCOUNT IN MODEM -------------------- *
     * ---------------------------------------------------------------------- */
    /* AT+CGDCONT=<cid>,<PDP_type>,<APN>,<PDP_addr> */
    asprintf(&cmd, "AT+CGDCONT=%d,\"IP\",\"%s\",\"\"",
             curCid, (APN ? APN : ""));
    err = at_send_command(cmd, &atresponse);
    free(cmd);

    if (err < 0 || atresponse->success == 0)
        goto error__unreserve_list_entry;

    at_response_free(atresponse);
    atresponse = NULL;

    /* AT*EIAAUW=<cid>,<bearer_id>,<userid>,<password>,<auth_prot>,<ask4pwd> */
    asprintf(&cmd, "AT*EIAAUW=%d,1,\"%s\",\"%s\",%s,0", curCid,
             (username ? username : ""), (password ? password : ""), auth);
    err = at_send_command(cmd, &atresponse);
    free(cmd);

    if (err < 0 || atresponse->success == 0)
        goto error__delete_account;

    at_response_free(atresponse);
    atresponse = NULL;

    /* ---------------------------------------------------------------------- *
     * ---------------------- ACTIVATING PDP CONTEXT ------------------------ *
     * ---------------------------------------------------------------------- */
    /* AT*EPPSD=<state>,<channel_id>,<cid> */
    asprintf(&cmd, "AT*EPPSD=1,%x,%d", curCid, curCid);
    err = at_send_command_multiline(cmd, "   <", &atresponse);
    free(cmd);

    if (err < 0)
        goto error__delete_account;

    if (atresponse->success == 0) {
        at_response_free(atresponse);
        atresponse = NULL;
        err = at_send_command_singleline("AT+CEER", "+CEER:", &atresponse);
        if (err < 0 || atresponse->success == 0)
            goto error__delete_account;

        /*
         * s_lastDataCallFailCause should map to TS 24.008 6.1.3.1.3 according
         * to RIL.h as expected received from AT+CEER
         */
        s_lastDataCallFailCause = at_get_sm_cause(atresponse);
        LOGE("PDP Context Activate failed with SM Cause Code %i",
             s_lastDataCallFailCause);

        goto error__delete_account;
    }

    /* Parse response from EPPSD */
    int buflen = 0;
    int pos = 0;
    char *doc = NULL;
    char *docTail = NULL;
    char *line = NULL;
    char *value = NULL;

    /* Loop once to calculate buffer length. */
    for (currentLine = atresponse->p_intermediates;
         currentLine != NULL; currentLine = currentLine->p_next) {
        char *line = currentLine->line;
        if (line != NULL)
            buflen += strlen(line);
    }
    if (buflen > 0) {
        doc = malloc(buflen + 1);
        assert(doc != NULL);

        /* Build the buffer containing all lines. */
        for (currentLine = atresponse->p_intermediates;
             currentLine != NULL; currentLine = currentLine->p_next) {
            line = currentLine->line;
            if (line != NULL) {
                strcpy(doc + pos, line);
                pos += strlen(line);
            }
        }

        /* Get IP address */
        value = getFirstElementValue(doc,"<ip_address>","</ip_address>", NULL);
        if (value != NULL) {
            LOGI("IP Address: %s", value);
            ipAddrStr = value;
            value = NULL;
        }

        /* Get Subnet */
        value = getFirstElementValue(doc, "<subnet_mask>","</subnet_mask>",
                                     NULL);
        if (value != NULL) {
            LOGI("Subnet Mask: %s", value);
            subnetMaskStr = value;
            value = NULL;
        }

        /* Get mtu */
        value = getFirstElementValue(doc, "<mtu>", "</mtu>", NULL);
        if (value != NULL) {
            LOGI("MTU: %s", value);
            mtuStr = value;
            value = NULL;
        }

        /* We support two DNS servers */
        docTail = NULL;
        value = getFirstElementValue(doc,"<dns_server>","</dns_server>",
                                     &docTail);
        if (value != NULL) {
            asprintf(&property, "net.%s.dns1", curIfName);
            LOGI("1st DNS Server: %s", value);
            if (property_set_verified(property, value) < 0)
                LOGE("FAILED to set dns1 property!");
            free(value);
            free(property);
            value = NULL;
        }

        if (docTail != NULL) {
            /* One more DNS server found */
            value = getFirstElementValue(docTail,"<dns_server>",
                                         "</dns_server>", NULL);
            if (value != NULL) {
                asprintf(&property, "net.%s.dns2", curIfName);
                LOGI("2nd DNS Server: %s", value);
                if (property_set_verified(property, value) < 0)
                    LOGE("FAILED to set dns2 property!");
                free(value);
                free(property);
                value = NULL;
            }
        }

        /* Note GW is not fetched. Default GW is calculated later. */
        free(doc);
    }

    at_response_free(atresponse);
    atresponse = NULL;

    /* ---------------------------------------------------------------------- *
     * -------------------- CONFIGURING NET INTERFACE ----------------------- *
     * ---------------------------------------------------------------------- */
    /* Disabling any existing old interface with same ID */
    if (ifc_init()) {
        LOGE("%s() failed to set up ifc!", __func__);
        goto error__deactivate_pdp;
    }

    if (ifc_down(curIfName)) {
        LOGE("%s() failed to bring down %s!", __func__, curIfName);
        goto error__deactivate_pdp;
    }

    /* Setup interface address and subnet using libnetutils. */
    if (inet_pton(AF_INET, ipAddrStr, &addr) <= 0) {
        LOGE("%s() failed when calling inet_pton() for %s!", __func__,
            ipAddrStr);
        goto error__deactivate_pdp;
    }

    if (ifc_set_addr(curIfName, addr)) {
        LOGE("%s() failed to setup address for interface %s!", __func__,
            curIfName);
        goto error__deactivate_pdp;
    }

    if (inet_pton(AF_INET, subnetMaskStr, &subaddr) <= 0) {
        LOGE("%s() failed when calling inet_pton() for %s!", __func__,
             subnetMaskStr);
        goto error__deactivate_pdp;
    }

    /*
     * This will fake a /31 CIDR network as defined in RFC 3021 to enable us to
     * have 'normal' routes in the routing table.
     */
    if (defaultGatewayStr == NULL && subaddr == htonl(0xFFFFFFFF)) {
        in_addr_t gw;
        struct in_addr gwaddr;
        subaddr = htonl(0xFFFFFFFE);    /* 255.255.255.254, CIDR /31. */

        gw = ntohl(addr) & 0xFFFFFF00;
        gw |= (ntohl(addr) & 0x000000FF) ^ 1;

        gwaddr.s_addr = htonl(gw);

        defaultGatewayStr = strdup(inet_ntoa(gwaddr));

        asprintf(&property, "net.%s.gw", curIfName);
        if (property_set_verified(property, defaultGatewayStr) < 0)
            LOGE("%s() failed to set fake %s.", __func__, property);
        free(property);

        LOGI("%s generated new fake /31 subnet with gw: %s", __func__,
            defaultGatewayStr);
    }

    /* We should have ifc_set_mtu()... */
    if (mtuStr != NULL) {
        int ifc_ctl_sock;
        int mtu = 0;

        mtu = atoi(mtuStr);
        /* Default value of RIL_MAX_MTU is 1500, see Android.mk for details */
        if (mtu > RIL_MAX_MTU) {
            mtu = RIL_MAX_MTU;
            LOGI("%s(): MTU is overridden and limited to %d!", __func__, mtu);
        }

        if (mtu > 1) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));

            strcpy(ifr.ifr_name, curIfName);
            ifr.ifr_mtu = mtu;

            ifc_ctl_sock = socket(AF_INET, SOCK_DGRAM, 0);

            if (ifc_ctl_sock < 0) {
                LOGE("%s() failed to obtain ifc control socket", __func__);
                goto error__deactivate_pdp;
            }

            if (ioctl(ifc_ctl_sock, SIOCSIFMTU, &ifr)) {
                LOGE("%s() failed to set MTU to %d!", __func__, mtu);
                goto error__deactivate_pdp;
            }

            close(ifc_ctl_sock);
        }
    }

    if (ifc_set_mask(curIfName, subaddr)) {
        LOGE("%s() failed to set subnet mask!", __func__);
        goto error__deactivate_pdp;
    }

    if (ifc_up(curIfName)) {
        LOGE("%s() failed to bring up %s!", __func__, curIfName);
        goto error__deactivate_pdp;
    }

    if (defaultGatewayStr != NULL) {
        in_addr_t gw;
        if (inet_pton(AF_INET, defaultGatewayStr, &gw) <= 0) {
            LOGE("%s() failed calling inet_pton for gw %s!", __func__,
                defaultGatewayStr);
            goto error__netdev_down;
        }
    }

    /* Create new entry in Contextlist */
    int pid = strtol(dataProfile, &end, 10);
    if (!pdpListPut(pdpListHandle, pid, APN, 1, 0)) {
        LOGE("%s() failed to add PDP to context list", __func__);
        goto error__netdev_down;
    }

    /* Allocate and fill in response */
    asprintf(&curCidStr, "%d", curCid);
    rilResponse = alloca(3 * sizeof(char *));
    rilResponse[0] = curCidStr;
    rilResponse[1] = curIfName;
    rilResponse[2] = ipAddrStr;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, rilResponse, 3 * sizeof(char *));
    free(curCidStr);

    goto exit;

error__netdev_down: /* Only goto if ifc have been initiated */
    LOGD("%s() errorhandler: Trying to take down net interface", __func__);

    if (ifc_down(curIfName))
        LOGE("%s() failed to bring down %s!", __func__, curIfName);

error__deactivate_pdp:
    LOGD("%s() errorhandler: Trying to disconnect pdp context", __func__);

    /* remove any set properties */
    if (curIfName != NULL) {
        asprintf(&property, "net.%s.gw", curIfName);
        (void)property_set(property, "");
        free(property);
        asprintf(&property, "net.%s.dns1", curIfName);
        (void)property_set(property, "");
        free(property);
        asprintf(&property, "net.%s.dns2", curIfName);
        (void)property_set(property, "");
        free(property);
    }

    asprintf(&cmd, "AT*EPPSD=0,%d,%d", curCid, curCid);
    err = at_send_command(cmd, &atresponse);
    free(cmd);

    if (err < 0 || atresponse->success == 0)
        LOGE("%s() failed deactivating cid %d!", __func__, curCid);
    free(atresponse);
    atresponse = NULL;

error__delete_account:
    LOGD("%s() errorhandler: Trying to remove account %d",
         __func__, curCid);

    asprintf(&cmd, "AT*EIAD=%d,1", curCid);
    (void) at_send_command(cmd, NULL);
    free(cmd);

error__unreserve_list_entry:
    LOGD("%s() errorhandler: Trying to unreserve list entry", __func__);
    (void)pdpListUndo(pdpListHandle);

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

exit:
    free(ipAddrStr);
    free(subnetMaskStr);
    free(mtuStr);
    free(defaultGatewayStr);
    at_response_free(atresponse);
    ifc_close();
}

/**
 * RIL_REQUEST_DEACTIVATE_DATA_CALL
 *
 * Deactivate PDP context created by RIL_REQUEST_SETUP_DATA_CALL.
 *
 * See also: RIL_REQUEST_SETUP_DATA_CALL.
 */
void requestDeactivateDataCall(void *data, size_t datalen, RIL_Token t)
{
    const char *cidStr = NULL;
    int pdpListHandle = -1;
    char *cmd = NULL;
    ATResponse *atresponse = NULL;
    char *property = NULL;
    int err;
    char curIfName[MAX_IFNAME_LEN] = "";

    cidStr = ((const char **) data)[0];

    /* Finding element */
    pdpListHandle = pdpListGet(cidStr, NULL,
                               NULL, NULL, curIfName, NULL, NULL, NULL);
    if (pdpListHandle < 0) {
        LOGD("%s() issued with non existing Connection ID (cid(%s))",
             __func__, cidStr);
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        goto exit;
    }

    /*
     * Important: handle must be unreserved before exit.
     * (goto exit not allowed)!!
     */
    LOGI("%s() found Connection ID (%s) and Interface Name (%s). Deactivating.",
         __func__, cidStr, curIfName);

    /*
     * Disconnect a PDP context,
     * AT*EPPSD=<state>,<channel_id>,<cid>  state=0 to disconnect
     */
    asprintf(&cmd, "AT*EPPSD=0,%s,%s", cidStr, cidStr);
    err = at_send_command(cmd, &atresponse);
    free(cmd);
    if (err < 0 || atresponse->success == 0)
        LOGE("%s() failed sending AT*EPPSD for cid %s!", __func__, cidStr);

    /* remove any set properties for the given interface name */
    if (curIfName != NULL) {
        asprintf(&property, "net.%s.gw", curIfName);
        (void)property_set(property, "");
        free(property);
        asprintf(&property, "net.%s.dns1", curIfName);
        (void)property_set(property, "");
        free(property);
        asprintf(&property, "net.%s.dns2", curIfName);
        (void)property_set(property, "");
        free(property);
    }

    /* Bringing down the interface */
    if (ifc_init()) {
        LOGE("%s() failed to set up ifc!", __func__);
        goto error;
    }

    if (ifc_down(curIfName)) {
        LOGE("%s() failed to bring down %s!", __func__, curIfName);
        ifc_close();
        goto error;
    }

    ifc_close();

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    goto exit_remove_account;

error:
    LOGE("%s() failed for cid %s!", __func__, cidStr);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

exit_remove_account:
    /* regardless of outcome we remove the entry */
    asprintf(&cmd, "AT*EIAD=%s,1", cidStr);
    (void) at_send_command(cmd, NULL);
    free(cmd);

    pdpListFree(pdpListHandle);

 exit:
    free(atresponse);
}

/**
 * RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE
 *
 * Requests the failure cause code for the most recently failed PDP
 * context activate.
 *
 * See also: RIL_REQUEST_LAST_CALL_FAIL_CAUSE.
 *
 */
void requestLastPDPFailCause(void *data, size_t datalen, RIL_Token t)
{
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &s_lastDataCallFailCause,
                          sizeof(int));

    /* Clear on read. */
    s_lastDataCallFailCause = PDP_FAIL_ERROR_UNSPECIFIED;
}

/**
 * pdpSetOnOemDeactivated
 *
 * Sets the callback to be called when an OEM activated context is deactivated.
 */
void pdpSetOnOemDeactivated(void (*onOemDeactivated)(int profileId))
{
    pdpOemDeativatedCB = onOemDeactivated;
}
