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
** Modified for ST-Ericsson U300 modems.
** Author: Christian Bejram <christian.bejram@stericsson.com>
*/

#include "atchannel.h"
#include "at_tok.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <stdbool.h>

#include <poll.h>

#define LOG_NDEBUG 0
#define LOG_TAG "RILVAT"
#include <utils/Log.h>

#ifdef HAVE_ANDROID_OS
/* For IOCTL's */
#include <linux/omap_csmi.h>
#endif                          /*HAVE_ANDROID_OS */

#include "misc.h"

#ifdef HAVE_ANDROID_OS
#define USE_NP 1
#endif                          /* HAVE_ANDROID_OS */

#define MAX_AT_RESPONSE (8 * 1024)
#define HANDSHAKE_RETRY_COUNT 8
#define HANDSHAKE_TIMEOUT_MSEC 250
#define DEFAULT_AT_TIMEOUT_MSEC (3 * 60 * 1000)

enum eolresult {
    EOL_SMS = 0,
    EOL_FOUND = 1,
    EOL_NOTFOUND = 2
};

struct atcontext {
    pthread_t tid_reader;
    int fd;                     /* fd of the AT channel. */
    int readerCmdFds[2];
    int isInitialized;
    ATUnsolHandler unsolHandler;

    /* For input buffering. */
    char ATBuffer[MAX_AT_RESPONSE + 1];
    char *ATBufferCur;

    /*
     * For current pending command, these are protected by commandmutex.
     *
     * The mutex and cond struct is memset in the getAtChannel() function,
     * so no initializer should be needed.
     */
    pthread_mutex_t requestmutex;
    pthread_mutex_t commandmutex;
    pthread_cond_t requestcond;
    pthread_cond_t commandcond;

    ATCommandType type;
    const char *responsePrefix;
    const char *smsPDU;
    ATResponse *response;

    void (*onTimeout)(void);
    void (*onReaderClosed)(void);
    int readerClosed;

    int timeoutMsec;
};

static struct atcontext *s_defaultAtContext = NULL;

static pthread_key_t key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;

static int writeCtrlZ(const char *s);

static void make_key()
{
    (void) pthread_key_create(&key, NULL);
}

/**
 * Set the atcontext pointer. Useful for sub-threads that needs to hold
 * the same state information.
 *
 * The caller IS responsible for freeing any memory already allocated
 * for any previous atcontexts.
 */
static void setAtContext(struct atcontext *ac)
{
    (void) pthread_once(&key_once, make_key);
    (void) pthread_setspecific(key, ac);
}

static void ac_free(void)
{
    struct atcontext *ac = NULL;
    (void) pthread_once(&key_once, make_key);
    if ((ac = pthread_getspecific(key)) != NULL) {
        free(ac);
        LOGD("%s() freed current thread AT context", __func__);
    } else {
        LOGW("%s() No AT context exist for current thread, cannot free it",
            __func__);
    }
}

static int initializeAtContext()
{
    struct atcontext *ac = NULL;

    if (pthread_once(&key_once, make_key)) {
        LOGE("%s() Pthread_once failed!", __func__);
        goto error;
    }

    ac = pthread_getspecific(key);

    if (ac == NULL) {
        ac = malloc(sizeof(struct atcontext));
        assert(ac != NULL);

        memset(ac, 0, sizeof(struct atcontext));

        ac->fd = -1;
        ac->readerCmdFds[0] = -1;
        ac->readerCmdFds[1] = -1;
        ac->ATBufferCur = ac->ATBuffer;

        if (pipe(ac->readerCmdFds)) {
            LOGE("%s(): Failed to create pipe: %s", __func__,
                 strerror(errno));
            goto error;
        }

        pthread_mutex_init(&ac->commandmutex, NULL);
        pthread_mutex_init(&ac->requestmutex, NULL);
        pthread_cond_init(&ac->requestcond, NULL);
        pthread_cond_init(&ac->commandcond, NULL);

        ac->timeoutMsec = DEFAULT_AT_TIMEOUT_MSEC;

        if (pthread_setspecific(key, ac)) {
            LOGE("%s() calling pthread_setspecific failed!", __func__);
            goto error;
        }
    }
    LOGI("Initialized new AT Context!");
    return 0;

error:
    LOGE("%s() failed initializing new AT Context!", __func__);
    free(ac);
    return -1;
}

static struct atcontext *getAtContext() {
    struct atcontext *ac = NULL;

    (void) pthread_once(&key_once, make_key);

    if ((ac = pthread_getspecific(key)) == NULL) {
        if (s_defaultAtContext) {
            LOGE("WARNING! external thread use default AT Context");
            ac = s_defaultAtContext;
        } else {
            LOGE("WARNING! getAtContext() called from external thread with "
                 "no defaultAtContext set!! This IS a bug! "
                 "A crash is probably nearby!");
        }
    }

    return ac;
}

/**
 * This function will make the current at thread the default channel,
 * meaning that calls from a thread that is not a queuerunner will
 * be executed in this context.
 */
void at_make_default_channel(void)
{
    struct atcontext *ac = getAtContext();

    if (ac->isInitialized)
        s_defaultAtContext = ac;
}

#if AT_DEBUG
void AT_DUMP(const char *prefix, const char *buff, int len)
{
    int i;
    char *s = alloca(3 * len + 1);
    char *t = s;

    if (len < 0)
        len = strlen(buff);

    for (i = 0; i < len; i++) t += sprintf(t, "%02x ", buff[i]);

    LOGD("%s(%d): %s (\"%.*s\")", prefix, len, s, len, buff);
}
#endif

#ifndef USE_NP
static void setTimespecRelative(struct timespec *p_ts, long long msec)
{
    struct timeval tv;

    gettimeofday(&tv, (struct timezone *) NULL);

    /* What's really funny about this is that I know
       pthread_cond_timedwait just turns around and makes this
       a relative time again. */
    p_ts->tv_sec = tv.tv_sec + (msec / 1000);
    p_ts->tv_nsec = (tv.tv_usec + (msec % 1000) * 1000L) * 1000L;
}
#endif                          /*USE_NP */

static void sleepMsec(long long msec)
{
    struct timespec ts;
    int err;

    ts.tv_sec = (msec / 1000);
    ts.tv_nsec = (msec % 1000) * 1000 * 1000;

    do
        err = nanosleep(&ts, &ts);

    while (err < 0 && errno == EINTR);
}



/** Add an intermediate response to sp_response. */
static void addIntermediate(const char *line)
{
    ATLine *p_new = NULL;
    struct atcontext *ac = getAtContext();

    p_new = (ATLine *) malloc(sizeof(ATLine));
    assert(p_new != NULL);

    memset(p_new, 0, sizeof(ATLine));

    p_new->line = strdup(line);

    /* Note: This adds to the head of the list, so the list will
       be in reverse order of lines received. the order is flipped
       again before passing on to the command issuer. */
    p_new->p_next = ac->response->p_intermediates;
    ac->response->p_intermediates = p_new;
}


/**
 * Returns 1 if line is a final response indicating error.
 * See 27.007 annex B.
 * WARNING: NO CARRIER and others are sometimes unsolicited.
 */
static const char *s_finalResponsesError[] = {
    "ERROR",
    "+CMS ERROR:",
    "+CME ERROR:",
    "NO CARRIER",               /* Sometimes! */
    "NO ANSWER",
    "NO DIALTONE",
};

static int isFinalResponseError(const char *line)
{
    size_t i;

    for (i = 0; i < NUM_ELEMS(s_finalResponsesError); i++)
        if (strStartsWith(line, s_finalResponsesError[i]))
            return 1;

    return 0;
}

/**
 * Returns 1 if line is a final response indicating success.
 * See 27.007 annex B.
 * WARNING: NO CARRIER and others are sometimes unsolicited.
 */
static const char *s_finalResponsesSuccess[] = {
    "OK",
    "CONNECT"    /* Some stacks start up data on another channel. */
};

static int isFinalResponseSuccess(const char *line)
{
    size_t i;

    for (i = 0; i < NUM_ELEMS(s_finalResponsesSuccess); i++)
        if (strStartsWith(line, s_finalResponsesSuccess[i]))
            return 1;

    return 0;
}


/**
 * Returns 1 if line is the first line in (what will be) a two-line
 * SMS unsolicited response.
 */
static const char *s_smsUnsoliciteds[] = {
    "+CMT:",
    "+CDS:",
    "+CBM:"
};

static int isSMSUnsolicited(const char *line)
{
    size_t i;

    for (i = 0; i < NUM_ELEMS(s_smsUnsoliciteds); i++)
        if (strStartsWith(line, s_smsUnsoliciteds[i]))
            return 1;

    return 0;
}


/** Assumes commandmutex is held. */
static void handleFinalResponse(const char *line)
{
    struct atcontext *ac = getAtContext();

    ac->response->finalResponse = strdup(line);

    pthread_cond_signal(&ac->commandcond);
}

static void handleUnsolicited(const char *line)
{
    struct atcontext *ac = getAtContext();

    if (ac->unsolHandler != NULL)
        ac->unsolHandler(line, NULL);
}

static void processLine(const char *line)
{
    struct atcontext *ac = getAtContext();

    pthread_mutex_lock(&ac->commandmutex);

    if (ac->response == NULL)
        /* No command pending. */
        handleUnsolicited(line);
    else if (isFinalResponseSuccess(line)) {
        ac->response->success = 1;
        handleFinalResponse(line);
    } else if (isFinalResponseError(line)) {
        ac->response->success = 0;
        handleFinalResponse(line);
    } else if (ac->smsPDU != NULL && 0 == strcmp(line, "> ")) {
        /* See eg. TS 27.005 4.3.
           Commands like AT+CMGS have a "> " prompt. */
        writeCtrlZ(ac->smsPDU);
        ac->smsPDU = NULL;
    } else
        switch (ac->type) {
        case NO_RESULT:
            handleUnsolicited(line);
            break;
        case NUMERIC:

            if (ac->response->p_intermediates == NULL && isdigit(line[0])
               )
                addIntermediate(line);
            else
                /* Either we already have an intermediate response or
                   the line doesn't begin with a digit. */
                handleUnsolicited(line);

            break;
        case SINGLELINE:

            if (ac->response->p_intermediates == NULL
                    && strStartsWith(line, ac->responsePrefix)
               )
                addIntermediate(line);
            else
                /* We already have an intermediate response. */
                handleUnsolicited(line);

            break;
        case MULTILINE:

            if (strStartsWith(line, ac->responsePrefix))
                addIntermediate(line);
            else
                handleUnsolicited(line);

            break;

        default:               /* This should never be reached */
            LOGE("Unsupported AT command type %d", ac->type);
            handleUnsolicited(line);
            break;
        }

    pthread_mutex_unlock(&ac->commandmutex);
}


/**
 * Returns a pointer to the end of the next line,
 * special-cases the "> " SMS prompt.
 *
 * returns NULL if there is no complete line.
 *
 * State machine for handling escaped characters.
 *
 *             Double quote              Backslash
 * .--------. -------------> .--------. ---------->  .--------.
 * | Normal |  Double quote  | String |     Any      | Escape |
 * '--------' <------------  '--------' <---------   '--------'
 *      | |                       |                      |
 * CR,LF| '-------.               | NUL                  |NUL
 *      V         |NUL            V                      |
 * .--------.     |          .--------.                  |
 * |  End   |     '--------->| Error  |<-----------------'
 * '--------'                '--------'
 */
static char *findNextEOL(char *cur, enum eolresult *p_eolres)
{
    char c;
    enum State {NORMAL, ERROR, END, STRING, ESCAPE} state = NORMAL;

    if (cur[0] == '>' && cur[1] == ' ' && cur[2] == '\0') {
        *p_eolres = EOL_SMS;
        return cur + 2;
    }

    while(state != ERROR && state != END)
    {
        c = *cur;
        switch (state) {
        case NORMAL:
            switch (c) {
            case '"':
                state = STRING;
                break;
            case '\r':
            case '\n':
                state = END;
                break;
            case '\0':
                state = ERROR;
                break;
            default:
                /* Stay in Normal state */
                break;
            }
            break;
        case STRING:
            switch (c) {
            case '"':
                state = NORMAL;
                break;
            case '\0':
                state = ERROR;
                break;
            case '\\':
                state = ESCAPE;
                break;
            default:
                /* Stay in String state */
                break;
            }
            break;
        case ESCAPE:
            switch (c) {
            case '\0':
                state = ERROR;
                break;
            default:
                state = STRING;
                break;
            }
            break;
        default:
            /* break for Error or End state, should never happen. */
            break;
        }
        cur++;
    }

    if (state == ERROR) {
        *p_eolres = EOL_NOTFOUND;
        return NULL;
    } else {
        *p_eolres = EOL_FOUND;
        /*
         * In End state, cur increment once too much, therefore we need to
         * decrease it before returning.
         */
        return cur-1;
    }
}

/**
 * Reads a line from the AT channel, returns NULL on timeout.
 * Assumes it has exclusive read access to the FD.
 *
 * This line is valid only until the next call to readline.
 *
 * This function exists because as of writing, android libc does not
 * have buffered stdio.
 */

static const char *readline()
{
    ssize_t count;
    enum eolresult eolres = EOL_NOTFOUND;

    char *p_read = NULL;
    char *p_eol = NULL;
    char *ret = NULL;

    struct atcontext *ac = getAtContext();

    /* This is a little odd. I use *s_ATBufferCur == 0 to mean
     * "buffer consumed completely". If it points to a character,
     * then the buffer continues until a \0.
     */
    if (*ac->ATBufferCur == '\0') {
        /* Empty buffer. */
        ac->ATBufferCur = ac->ATBuffer;
        *ac->ATBufferCur = '\0';
        p_read = ac->ATBuffer;
    } else {
        /* *s_ATBufferCur != '\0'
         * There's data in the buffer from the last read.
         * skip over leading newlines
         */
        while (*ac->ATBufferCur == '\r' || *ac->ATBufferCur == '\n')
            ac->ATBufferCur++;

        p_eol = findNextEOL(ac->ATBufferCur, &eolres);

        if (p_eol == NULL) {
            /* A partial line. Move it up and prepare to read more. */
            size_t len;

            len = strlen(ac->ATBufferCur);

            memmove(ac->ATBuffer, ac->ATBufferCur, len + 1);
            p_read = ac->ATBuffer + len;
            ac->ATBufferCur = ac->ATBuffer;
        }

        /* Otherwise, (p_eol !- NULL) there is a complete line
           that will be returned from the while () loop below. */
    }

    while (p_eol == NULL) {
        int err;
        struct pollfd pfds[2];

        if (0 >= MAX_AT_RESPONSE - (p_read - ac->ATBuffer)) {
            LOGE("%s() ERROR: Input line exceeded buffer", __func__);
            /* Ditch buffer and start over again. */
            ac->ATBufferCur = ac->ATBuffer;
            *ac->ATBufferCur = '\0';
            p_read = ac->ATBuffer;
        }

        /* If our fd is invalid, we are probably closed. Return. */
        if (ac->fd < 0)
            return NULL;

        pfds[0].fd = ac->fd;
        pfds[0].events = POLLIN | POLLERR;

        pfds[1].fd = ac->readerCmdFds[0];
        pfds[1].events = POLLIN;

        err = poll(pfds, 2, -1);

        if (err < 0) {
            LOGE("%s() poll: error: %s", __func__, strerror(errno));
            return NULL;
        }

        if (pfds[1].revents & POLLIN) {
            char buf[10];

            /* Just drain it. We don't care, this is just for waking up. */
            read(pfds[1].fd, &buf, 1);
            continue;
        }

        if (pfds[0].revents & POLLERR) {
            LOGE("%s() POLLERR event! Returning...", __func__);
            return NULL;
        }

        if (!(pfds[0].revents & POLLIN))
            continue;

        do
            count = read(ac->fd, p_read,
                         MAX_AT_RESPONSE - (p_read - ac->ATBuffer));

        while (count < 0 && errno == EINTR);

        if (count > 0) {
            AT_DUMP("<< ", p_read, count);

            p_read[count] = '\0';

            /* Skip over leading newlines. */
            while (*ac->ATBufferCur == '\r' || *ac->ATBufferCur == '\n')
                ac->ATBufferCur++;

            p_eol = findNextEOL(ac->ATBufferCur, &eolres);
            p_read += count;
        } else if (count <= 0) {
            /* Read error encountered or EOF reached. */
            if (count == 0)
                LOGD("%s() atchannel: EOF reached.", __func__);
            else
                LOGD("%s() atchannel: read error %s", __func__, strerror(errno));

            return NULL;
        }
    }

    /* A full line in the buffer. Place a \0 over the \r and return. */

    ret = ac->ATBufferCur;

    switch (eolres) {
    case EOL_SMS:
        *p_eol = '\0';
        ac->ATBufferCur = p_eol;
        break;

    case EOL_FOUND:
        *p_eol = '\0';
        ac->ATBufferCur = p_eol + 1;    /* This will always be <= p_read,
                                           and there will be a \0 at *p_read. */
        break;

    case EOL_NOTFOUND:  /* fall through */
    default:
        assert(false &&
               "Did not find the EOL in a line that should be complete");
        break;
    }

    LOGI("AT(%d)< %s", ac->fd, ret);
    return ret;
}


static void onReaderClosed()
{
    struct atcontext *ac = getAtContext();

    if (ac->onReaderClosed != NULL && ac->readerClosed == 0) {

        pthread_mutex_lock(&ac->commandmutex);

        ac->readerClosed = 1;

        pthread_cond_signal(&ac->commandcond);

        pthread_mutex_unlock(&ac->commandmutex);

        ac->onReaderClosed();
    }
}


static void *readerLoop(void *arg)
{
    struct atcontext *ac = NULL;

    LOGI("Entering readerloop!");

    setAtContext((struct atcontext *) arg);
    ac = getAtContext();

    for (;;) {
        const char *line;

        line = readline();

        if (line == NULL)
            break;

        if (isSMSUnsolicited(line)) {
            char *line1;
            const char *line2;

            /* The scope of string returned by 'readline()' is valid only
               until next call to 'readline()' hence making a copy of line
               before calling readline again. */
            line1 = strdup(line);
            line2 = readline();

            if (line2 == NULL) {
                free(line1);
                break;
            }

            if (ac->unsolHandler != NULL)
                ac->unsolHandler(line1, line2);

            free(line1);
        } else
            processLine(line);
    }

    onReaderClosed();
    return NULL;
}

/**
 * Appends \r to string and sends it to radio.
 * Returns AT_ERROR_* on error, 0 on success.
 *
 * This function exists because as of writing, android libc does not
 * have buffered stdio.
 */
static int writeline(const char *s)
{
    size_t cur = 0;
    size_t len = 0;
    ssize_t written;
    int err;
    int ret = 0;
    char *p_s = NULL;

    /* append "/r" to string. */
    err = asprintf(&p_s, "%s\r", s);
    if (err < 0) {
        LOGE("%s(): Failed to append \\r to string.", __func__);
        ret = AT_ERROR_GENERIC;
        goto exit;
    }
    len = strlen(p_s);

    struct atcontext *ac = getAtContext();
    if (ac->fd < 0 || ac->readerClosed > 0) {
        LOGE("Attempt to write to the closed AT channel.");
        ret = AT_ERROR_CHANNEL_CLOSED;
        goto error;
    }

    LOGD("AT(%d)> %s", ac->fd, p_s);
    AT_DUMP(">> ", p_s, strlen(p_s));

    /* The main string. */
    while (cur < len) {
        do
            written = write(ac->fd, p_s + cur, len - cur);

        while (written < 0 && errno == EINTR);

        if (written < 0) {
            LOGE("Error writing to the AT channel: %ld", (long)written);
            ret = AT_ERROR_GENERIC;
            goto error;
        }

        cur += written;
    }

error:
    free(p_s);
exit:
    return ret;
}

/**
 * Appends ^Z to string and sends it to radio.
 * Returns AT_ERROR_* on error, 0 on success.
 */
static int writeCtrlZ(const char *s)
{
    size_t cur = 0;
    size_t len;
    ssize_t written;
    char *p_s = NULL;
    int err;
    int ret = 0;

    err = asprintf(&p_s, "%s\032", s);
    if (err < 0) {
        LOGE("%s(): Failed to append ^Z to string.", __func__);
        ret = AT_ERROR_GENERIC;
        goto exit;
    }
    len = strlen(p_s);

    struct atcontext *ac = getAtContext();

    if (ac->fd < 0 || ac->readerClosed > 0) {
        ret = AT_ERROR_CHANNEL_CLOSED;
        goto error;
    }

    LOGD("AT> %s", p_s);
    AT_DUMP(">* ", p_s, strlen(p_s));

    /* The main string. */
    while (cur < len) {
        do
            written = write(ac->fd, p_s + cur, len - cur);

        while (written < 0 && errno == EINTR);

        if (written < 0) {
            ret = AT_ERROR_GENERIC;
            goto error;
        }

        cur += written;
    }

error:
    free(p_s);
exit:
    return ret;
}

static void clearPendingCommand()
{
    struct atcontext *ac = getAtContext();

    if (ac->response != NULL)
        at_response_free(ac->response);

    ac->response = NULL;
    ac->responsePrefix = NULL;
    ac->smsPDU = NULL;
}


/**
 * Starts AT handler on stream "fd'.
 * returns 0 on success, -1 on error.
 */
int at_open(int fd, ATUnsolHandler h)
{
    int ret;
    pthread_attr_t attr;

    struct atcontext *ac = NULL;

    if (initializeAtContext()) {
        LOGE("InitializeAtContext failed!");
        goto error;
    }

    ac = getAtContext();

    ac->fd = fd;
    ac->isInitialized = 1;
    ac->unsolHandler = h;
    ac->readerClosed = 0;

    ac->responsePrefix = NULL;
    ac->smsPDU = NULL;
    ac->response = NULL;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    ret = pthread_create(&ac->tid_reader, &attr, readerLoop, ac);
    if (ret < 0) {
        perror("pthread_create");
        goto error;
    }

    return 0;
error:
    ac_free();
    return -1;
}

/* FIXME is it ok to call this from the reader and the command thread? */
void at_close()
{
    struct atcontext *ac;
    ssize_t written;

    /* Find AT context to current thead */
    (void) pthread_once(&key_once, make_key);
    if ((ac = pthread_getspecific(key)) != NULL) {
        if (ac->fd >= 0)
            if (close(ac->fd) != 0)
                LOGE("FAILED to close fd %d!", ac->fd);

        ac->fd = -1;

        pthread_mutex_lock(&ac->commandmutex);

        ac->readerClosed = 1;

        pthread_cond_signal(&ac->commandcond);

        pthread_mutex_unlock(&ac->commandmutex);

        /* Kick readerloop. */
        written = write(ac->readerCmdFds[1], "x", 1);

        if (written < 0)
            LOGE("FAILED to kick readerloop!");
    }
}

void at_response_free(ATResponse *p_response)
{
    ATLine *p_line = NULL;

    if (p_response == NULL)
        return;

    p_line = p_response->p_intermediates;

    while (p_line != NULL) {
        ATLine *p_toFree;

        p_toFree = p_line;
        p_line = p_line->p_next;

        free(p_toFree->line);
        free(p_toFree);
    }

    free(p_response->finalResponse);
    free(p_response);
    p_response = NULL;
}

/**
 * The line reader places the intermediate responses in reverse order,
 * here we flip them back.
 */
static void reverseIntermediates(ATResponse *p_response)
{
    ATLine *pcur = NULL;
    ATLine *pnext = NULL;

    pcur = p_response->p_intermediates;
    p_response->p_intermediates = NULL;

    while (pcur != NULL) {
        pnext = pcur->p_next;
        pcur->p_next = p_response->p_intermediates;
        p_response->p_intermediates = pcur;
        pcur = pnext;
    }
}

/**
 * Internal send_command implementation.
 * Doesn't lock or call the timeout callback.
 *
 * timeoutMsec == 0 means infinite timeout.
 */

static int at_send_command_full_nolock(const char *command,
                                       ATCommandType type,
                                       const char *responsePrefix,
                                       const char *smspdu,
                                       long long timeoutMsec,
                                       ATResponse **pp_outResponse)
{
    int err = 0;

#ifndef USE_NP
    struct timespec ts;
#endif /*USE_NP */

    struct atcontext *ac = getAtContext();

    /* FIXME This is to prevent future problems due to calls from other threads;
     * should be revised.
     */
    /* FIXME: This will attempt to wait on ac->requestcond without holding
     * ac->commandmutex if pthread_mutex_trylock reports that ac->requestmutex
     * is busy. That is undefeined behaviour according to SUSv2
     */
    while (pthread_mutex_trylock(&ac->requestmutex) == EBUSY)
        pthread_cond_wait(&ac->requestcond, &ac->commandmutex);

    if (ac->response != NULL) {
        err = AT_ERROR_COMMAND_PENDING;
        goto error;
    }

    err = writeline(command);

    if (err < 0)
        goto error;

    ac->type = type;
    ac->responsePrefix = responsePrefix;
    ac->smsPDU = smspdu;

    ac->response = (ATResponse *) calloc(1, sizeof(ATResponse));
    assert(ac->response != NULL);

#ifndef USE_NP

    if (timeoutMsec != 0)
        setTimespecRelative(&ts, timeoutMsec);

#endif /*USE_NP */

    while (ac->response->finalResponse == NULL && ac->readerClosed == 0) {
        if (timeoutMsec != 0) {
#ifndef USE_NP
            err =
                pthread_cond_timedwait(&ac->commandcond, &ac->commandmutex,
                                       &ts);
#else
            err =
                pthread_cond_timeout_np(&ac->commandcond,
                                        &ac->commandmutex, timeoutMsec);
#endif /*USE_NP */
        } else
            err = pthread_cond_wait(&ac->commandcond, &ac->commandmutex);

        if (err == ETIMEDOUT) {
            err = AT_ERROR_TIMEOUT;
            goto error;
        }
    }

    if (ac->readerClosed > 0) {
        err = AT_ERROR_CHANNEL_CLOSED;
        goto error;
    }

    if (pp_outResponse == NULL)
        at_response_free(ac->response);
    else {
        /* Line reader stores intermediate responses in reverse order. */
        reverseIntermediates(ac->response);
        *pp_outResponse = ac->response;
    }

    ac->response = NULL;
    err = 0;

error:
    clearPendingCommand();

    pthread_cond_broadcast(&ac->requestcond);
    pthread_mutex_unlock(&ac->requestmutex);

    return err;
}

/**
 * Internal send_command implementation.
 *
 * timeoutMsec == 0 means infinite timeout.
 */
static int at_send_command_full(const char *command, ATCommandType type,
                                const char *responsePrefix,
                                const char *smspdu, long long timeoutMsec,
                                ATResponse **pp_outResponse)
{
    int err;

    struct atcontext *ac = getAtContext();

    if (0 != pthread_equal(ac->tid_reader, pthread_self()))
        /* Cannot be called from reader thread. */
        return AT_ERROR_INVALID_THREAD;

    pthread_mutex_lock(&ac->commandmutex);

    err = at_send_command_full_nolock(command, type,
                                      responsePrefix, smspdu,
                                      timeoutMsec, pp_outResponse);

    pthread_mutex_unlock(&ac->commandmutex);

    if (err == AT_ERROR_TIMEOUT && ac->onTimeout != NULL)
        ac->onTimeout();

    return err;
}

/**
 * Only call this from onTimeout, since we're not locking or anything.
 */
void at_send_escape(void)
{
    struct atcontext *ac = getAtContext();
    int written;

    do
        written = write(ac->fd, " ", 1);
    while ((written < 0 && errno == EINTR) || (written == 0));

    LOGI("%s() sent space on at channel to abort command", __func__);
}

/**
 * Issue a single normal AT command with no intermediate response expected.
 *
 * "command" should not include \r.
 * pp_outResponse can be NULL.
 *
 * if non-NULL, the resulting ATResponse * must be eventually freed with
 * at_response_free.
 */
int at_send_command(const char *command, ATResponse **pp_outResponse)
{
    int err;

    struct atcontext *ac = getAtContext();

    err = at_send_command_full(command, NO_RESULT, NULL,
                               NULL, ac->timeoutMsec, pp_outResponse);

    return err;
}

int at_send_command_with_timeout(const char *command,
                                 ATResponse **pp_outResponse,
                                 long long timeoutMsec)
{
    int err;

    err = at_send_command_full(command, NO_RESULT, NULL,
                               NULL, timeoutMsec, pp_outResponse);

    return err;
}

int at_send_command_singleline(const char *command,
                               const char *responsePrefix,
                               ATResponse **pp_outResponse)
{
    int err;

    struct atcontext *ac = getAtContext();

    err = at_send_command_full(command, SINGLELINE, responsePrefix,
                               NULL, ac->timeoutMsec, pp_outResponse);

    if (err == 0 && pp_outResponse != NULL
            && (*pp_outResponse) != NULL
            && (*pp_outResponse)->success > 0
            && (*pp_outResponse)->p_intermediates == NULL) {
        /* Successful command must have an intermediate response. */
        at_response_free(*pp_outResponse);
        *pp_outResponse = NULL;
        err = AT_ERROR_INVALID_RESPONSE;
    }

    return err;
}

int at_send_command_singleline_with_timeout(const char *command,
                                            const char *responsePrefix,
                                            ATResponse **pp_outResponse,
                                            long long timeoutMsec)
{
    int err;

    err = at_send_command_full(command, SINGLELINE, responsePrefix,
                               NULL, timeoutMsec, pp_outResponse);

    if (err == 0 && pp_outResponse != NULL
            && (*pp_outResponse) != NULL
            && (*pp_outResponse)->success > 0
            && (*pp_outResponse)->p_intermediates == NULL) {
        /* Successful command must have an intermediate response. */
        at_response_free(*pp_outResponse);
        *pp_outResponse = NULL;
        err = AT_ERROR_INVALID_RESPONSE;
    }

    return err;
}


int at_send_command_numeric(const char *command,
                            ATResponse **pp_outResponse)
{
    int err;

    struct atcontext *ac = getAtContext();

    err = at_send_command_full(command, NUMERIC, NULL,
                               NULL, ac->timeoutMsec, pp_outResponse);

    if (err == 0 && pp_outResponse != NULL
            && (*pp_outResponse) != NULL
            && (*pp_outResponse)->success > 0
            && (*pp_outResponse)->p_intermediates == NULL) {
        /* Successful command must have an intermediate response. */
        at_response_free(*pp_outResponse);
        *pp_outResponse = NULL;
        err = AT_ERROR_INVALID_RESPONSE;
    }

    return err;
}

int at_send_command_sms(const char *command,
                        const char *pdu,
                        const char *responsePrefix,
                        ATResponse **pp_outResponse)
{
    int err;

    struct atcontext *ac = getAtContext();

    err = at_send_command_full(command, SINGLELINE, responsePrefix,
                               pdu, ac->timeoutMsec, pp_outResponse);

    if (err == 0 && pp_outResponse != NULL
            && (*pp_outResponse) != NULL
            && (*pp_outResponse)->success > 0
            && (*pp_outResponse)->p_intermediates == NULL) {
        /* Successful command must have an intermediate response. */
        at_response_free(*pp_outResponse);
        *pp_outResponse = NULL;
        err = AT_ERROR_INVALID_RESPONSE;
    }

    return err;
}

int at_send_command_with_pdu(const char *command,
                             const char *pdu,
                             ATResponse **pp_outResponse)
{
    int err;

    struct atcontext *ac = getAtContext();

    err = at_send_command_full(command, NO_RESULT, NULL,
                               pdu, ac->timeoutMsec, pp_outResponse);

    return err;
}

int at_send_command_multiline(const char *command,
                              const char *responsePrefix,
                              ATResponse **pp_outResponse)
{
    int err;

    struct atcontext *ac = getAtContext();

    err = at_send_command_full(command, MULTILINE, responsePrefix,
                               NULL, ac->timeoutMsec, pp_outResponse);

    return err;
}

int at_send_command_multiline_with_timeout(const char *command,
                                           const char *responsePrefix,
                                           ATResponse **pp_outResponse,
                                           long long  timeoutMsec)
{
    int err;

    err = at_send_command_full(command, MULTILINE, responsePrefix,
                               NULL, timeoutMsec, pp_outResponse);

    return err;
}

/**
 * Set the default timeout. Let it be reasonably high, some commands
 * take their time. Default is 10 minutes.
 */
void at_set_timeout_msec(int timeout)
{
    struct atcontext *ac = getAtContext();

    ac->timeoutMsec = timeout;
}

/** This callback is invoked on the command thread. */
void at_set_on_timeout(void (*onTimeout)(void))
{
    struct atcontext *ac = getAtContext();

    ac->onTimeout = onTimeout;
}


/*
 * This callback is invoked on the reader thread (like ATUnsolHandler), when the
 * input stream closes before you call at_close (not when you call at_close()).
 * You should still call at_close(). It may also be invoked immediately from the
 * current thread if the read channel is already closed.
 */
void at_set_on_reader_closed(void (*onClose)(void))
{
    struct atcontext *ac = getAtContext();

    ac->onReaderClosed = onClose;
}


/**
 * Periodically issue an AT command and wait for a response.
 * Used to ensure channel has start up and is active.
 */
int at_handshake()
{
    int i;
    int err = 0;

    struct atcontext *ac = getAtContext();

    if (0 != pthread_equal(ac->tid_reader, pthread_self()))
        /* Cannot be called from reader thread. */
        return AT_ERROR_INVALID_THREAD;

    pthread_mutex_lock(&ac->commandmutex);

    for (i = 0; i < HANDSHAKE_RETRY_COUNT; i++) {
        /* Some stacks start with verbose off. */
        err = at_send_command_full_nolock("ATE0Q0V1", NO_RESULT,
                                          NULL, NULL,
                                          HANDSHAKE_TIMEOUT_MSEC, NULL);

        if (err == 0)
            break;
    }

    if (err == 0) {
        /*
         * Pause for a bit to let the input buffer drain any unmatched OK's
         * (they will appear as extraneous unsolicited responses).
         */
        LOGD("%s() pausing %d ms to drain unmatched OK's...",
             __func__, HANDSHAKE_TIMEOUT_MSEC);
        sleepMsec(HANDSHAKE_TIMEOUT_MSEC);
    }

    pthread_mutex_unlock(&ac->commandmutex);

    return err;
}

/**
 * Return 1 for errorcode found and 0 for not found.
 * *p_errorCode returns error code from response for CME ERROR and CMS ERROR.
 */
static int at_get_error(const ATResponse *p_response,
                        const char *p_errorStr,
                        int *p_errorCode)
{
    int errorCode;
    int err;
    char *p_cur = NULL;

    if (p_response == NULL)
        goto error;

    if (p_response->finalResponse == NULL
            || !strStartsWith(p_response->finalResponse, p_errorStr)
       )
        goto error;

    p_cur = p_response->finalResponse;
    err = at_tok_start(&p_cur);

    if (err < 0)
        goto error;

    err = at_tok_nextint(&p_cur, &errorCode);

    if (err < 0)
        goto error;

    *p_errorCode = errorCode;
    return 1;

error:
    return 0;
}

/**
 * used to parse CMS ERROR codes.
 */
int at_get_cms_error(const ATResponse *p_response, ATCmsError *p_cms_error_code)
{
    int ret;
    ATCmsError cms_error_code;
    ret = at_get_error(p_response, "+CMS ERROR:", (int *)&cms_error_code);
    *p_cms_error_code = cms_error_code;
    return ret;
}

/**
 * Assumes AT+CMEE=1 (numeric) mode.
 * used to parse CME ERROR codes.
 */
int at_get_cme_error(const ATResponse *p_response, ATCmeError *p_cme_error_code)
{
    int ret;
    ATCmeError cme_error_code;
    ret = at_get_error(p_response, "+CME ERROR:", (int *)&cme_error_code);
    *p_cme_error_code = cme_error_code;
    return ret;
}

/**
 * Returns SM cause code from response to AT+CEER command.
 */
RIL_LastDataCallActivateFailCause at_get_sm_cause(const ATResponse *p_response)
{
    int ret;
    ATLine *p = NULL;
    char *line = NULL;
    char *p_cur = NULL;

    /* Check every line of the response for the expected output. */
    p = p_response->p_intermediates;

    while (p != NULL) {
        line = p->line;

        if (strstr(line, "+CEER: Deactivate Cause: SM") != NULL)
            break;

        if (p->p_next == NULL)
            return PDP_FAIL_ERROR_UNSPECIFIED;

        p = p->p_next;
    }

    if (p == NULL)
        return PDP_FAIL_ERROR_UNSPECIFIED;

    p_cur = strchr(line, 'M');

    if (p_cur == NULL)
        return PDP_FAIL_ERROR_UNSPECIFIED;

    p_cur++;
    ret = strtol(p_cur, NULL, 10);

    return (RIL_LastDataCallActivateFailCause) ret;
}
