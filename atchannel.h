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

#ifndef ATCHANNEL_H
#define ATCHANNEL_H 1

#ifdef __cplusplus
extern "C" {
#endif

#include <telephony/ril.h>

/* Define AT_DEBUG to send AT traffic to "/tmp/radio-at.log" */
#define AT_DEBUG  0

#if AT_DEBUG
    extern void AT_DUMP(const char *prefix, const char *buff, int len);
#else
#define  AT_DUMP(prefix, buff, len)  do {} while (0)
#endif

#define AT_ERROR_GENERIC -1
#define AT_ERROR_COMMAND_PENDING -2
#define AT_ERROR_CHANNEL_CLOSED -3
#define AT_ERROR_TIMEOUT -4
#define AT_ERROR_INVALID_THREAD   -5    /* AT commands may not be issued from
                                         * reader thread (or unsolicited
                                         * response callback) */
#define AT_ERROR_INVALID_RESPONSE -6    /* E.g. an at_send_command_singleline
                                         * that did not get back an intermediate
                                         * response */

typedef enum {
    CME_PHONE_FAILURE = 0,
    CME_NO_CONNECTION_TO_PHONE = 1,
    CME_PHONE_ADAPTOR_LINK_RESERVED = 2,
    CME_OPERATION_NOT_ALLOWED = 3,
    CME_OPERATION_NOT_SUPPORTED = 4,
    CME_PH_SIM_PIN_REQUIRED = 5,
    CME_PH_FSIM_PIN_REQUIRED = 6,
    CME_PH_FSIM_PUK_REQUIRED = 7,
    CME_SIM_NOT_INSERTED = 10,
    CME_SIM_PIN_REQUIRED = 11,
    CME_SIM_PUK_REQUIRED = 12,
    CME_SIM_FAILURE = 13,
    CME_SIM_BUSY = 14,
    CME_SIM_WRONG = 15,
    CME_INCORRECT_PASSWORD = 16,
    CME_SIM_PIN2_REQUIRED = 17,
    CME_SIM_PUK2_REQUIRED = 18,
    CME_MEMORY_FULL = 20,
    CME_INVALID_INDEX = 21,
    CME_NOT_FOUND = 22,
    CME_MEMORY_FAILURE = 23,
    CME_TEXT_STRING_TOO_LONG = 24,
    CME_INVALID_CHARACTERS_IN_TEXT_STRING = 25,
    CME_DIAL_STRING_TOO_LONG = 26,
    CME_INVALID_CHARACTERS_IN_DIAL_STRING = 27,
    CME_NO_NETWORK_SERVICE = 30,
    CME_NETWORK_TIMEOUT = 31,
    CME_NETWORK_NOT_ALLOWED_EMERGENCY_CALLS_ONLY = 32,
    CME_NETWORK_PERSONALIZATION_PIN_REQUIRED = 40,
    CME_NETWORK_PERSONALIZATION_PUK_REQUIRED = 41,
    CME_NETWORK_SUBSET_PERSONALIZATION_PIN_REQUIRED = 42,
    CME_NETWORK_SUBSET_PERSONALIZATION_PUK_REQUIRED = 43,
    CME_SERVICE_PROVIDER_PERSONALIZATION_PIN_REQUIRED = 44,
    CME_SERVICE_PROVIDER_PERSONALIZATION_PUK_REQUIRED = 45,
    CME_CORPORATE_PERSONALIZATION_PIN_REQUIRED = 46,
    CME_CORPORATE_PERSONALIZATION_PUK_REQUIRED = 47,
    CME_HIDDEN_KEY_REQUIRED = 48,
    CME_EAP_METHOD_NOT_SUPPORTED = 49,
    CME_INCORRECT_PARAMETERS = 50,
    CME_UNKNOWN = 100,
    CME_ILLEGAL_MS = 103,
    CME_ILLEGAL_ME = 106,
    CME_GPRS_SERVICES_NOT_ALLOWED = 107,
    CME_PLMN_NOT_ALLOWED = 111,
    CME_LOCATION_AREA_NOT_ALLOWED = 112,
    CME_ROAMING_NOT_ALLOWED_IN_THIS_LOCATION_AREA = 113,
    CME_SERVICE_OPTION_NOT_SUPPORTED = 132,
    CME_REQUESTED_SERVICE_OPTION_NOT_SUBSCRIBED = 133,
    CME_SERVICE_OPTION_TEMPORARILY_OUT_OF_ORDER = 134,
    CME_UNSPECIFIED_GPRS_ERROR = 148,
    CME_PDP_AUTHENTICATION_FAILURE = 149,
    CME_INVALID_MOBILE_CLASS = 150,
    CME_PH_SIMLOCK_PIN_REQUIRED = 200,
    CME_PRE_DIAL_CHECK_ERROR = 350,
} ATCmeError;

typedef enum {
    CMS_UNASSIGNED_NUMBER = 1,
    CMS_OPERATOR_DETERMINED_BARRING = 8,
    CMS_CALL_BARRED = 10,
    CMS_SHORT_MESSAGE_TRANSFER_REJECTED = 21,
    CMS_DESTINATION_OUT_OF_SERVICE = 27,
    CMS_UNIDENTIFIED_SUBSCRIBER = 28,
    CMS_FACILITY_REJECTED = 29,
    CMS_UNKNOWN_SUBSCRIBER = 30,
    CMS_NETWORK_OUT_OF_ORDER = 38,
    CMS_TEMPORARY_FAILURE = 41,
    CMS_CONGESTION = 42,
    CMS_RESOURCES_UNAVAILABLE_UNSPECIFIED = 47,
    CMS_REQUESTED_FACILITY_NOT_SUBSCRIBED = 50,
    CMS_REQUESTED_FACILITY_NOT_IMPLEMENTED = 69,
    CMS_INVALID_SHORT_MESSAGE_TRANSFER_REFERENCE_VALUE = 81,
    CMS_INVALID_MESSAGE_UNSPECIFIED = 95,
    CMS_INVALID_MANDATORY_INFORMATION = 96,
    CMS_MESSAGE_TYPE_NON_EXISTENT_OR_NOT_IMPLEMENTED = 97,
    CMS_MESSAGE_NOT_COMPATIBLE_WITH_SHORT_MESSAGE_PROTOCOL_STATE = 98,
    CMS_INFORMATION_ELEMENT_NON_EXISTENT_OR_NOT_IMPLEMENTED = 99,
    CMS_PROTOCOL_ERROR_UNSPECIFIED = 111,
    CMS_INTERWORKING_UNSPECIFIED = 127,
    CMS_TELEMATIC_INTERWORKING_NOT_SUPPORTED = 128,
    CMS_SHORT_MESSAGE_TYPE_0_NOT_SUPPORTED = 129,
    CMS_CANNOT_REPLACE_SHORT_MESSAGE = 130,
    CMS_UNSPECIFIED_TP_PID_ERROR = 143,
    CMS_DATA_CODING_SCHEME_NOT_SUPPORTED = 144,
    CMS_MESSAGE_CLASS_NOT_SUPPORTED = 145,
    CMS_UNSPECIFIED_TP_DCS_ERROR = 159,
    CMS_COMMAND_CANNOT_BE_ACTIONED = 160,
    CMS_COMMAND_UNSUPPORTED = 161,
    CMS_UNSPECIFIED_TP_COMMAND_ERROR = 175,
    CMS_TPDU_NOT_SUPPORTED = 176,
    CMS_SC_BUSY = 192,
    CMS_NO_SC_SUBSCRIPTION = 193,
    CMS_SC_SYSTEM_FAILURE = 194,
    CMS_INVALID_SME_ADDRESS = 195,
    CMS_DESTINATION_SME_BARRED = 196,
    CMS_SM_REJECTED_DUPLICATE_SM = 197,
    CMS_SIM_SMS_STORAGE_FULL = 208,
    CMS_NO_SMS_STORAGE_CAPABILITY_IN_SIM = 209,
    CMS_ERROR_IN_MS = 210,
    CMS_MEMORY_CAPACITY_EXCEEDED = 211,
    CMS_UNSPECIFIED_ERROR_CAUSE = 255,
    CMS_ME_FAILURE = 300,
    CMS_SMS_SERVICE_OF_ME_RESERVED = 301,
    CMS_OPERATION_NOT_ALLOWED = 302,
    CMS_OPERATION_NOT_SUPPORTED = 303,
    CMS_INVALID_PDU_MODE_PARAMETER = 304,
    CMS_INVALID_TEXT_MODE_PARAMETER = 305,
    CMS_USIM_NOT_INSERTED = 310,
    CMS_USIM_PIN_REQUIRED = 311,
    CMS_PH_USIM_PIN_REQUIRED = 312,
    CMS_USIM_FAILURE = 313,
    CMS_USIM_BUSY = 314,
    CMS_USIM_WRONG = 315,
    CMS_USIM_PUK_REQUIRED = 316,
    CMS_USIM_PIN2_REQUIRED = 317,
    CMS_USIM_PUK2_REQUIRED = 318,
    CMS_MEMORY_FAILURE = 320,
    CMS_INVALID_MEMORY_INDEX = 321,
    CMS_MEMORY_FULL = 322,
    CMS_SMSC_ADDRESS_UNKNOWN = 330,
    CMS_NO_NETWORK_SERVICE = 331,
    CMS_NETWORK_TIMEOUT = 332,
    CMS_NO_CNMA_ACKNOWLEDGEMENT_EXPECTED = 340,
    CMS_PRE_DIAL_CHECK_ERROR = 350,
    CMS_UNKNOWN_ERROR = 500,
    CMS_CMS_OK = 999,
} ATCmsError;

typedef enum {
    DEFAULT_VALUE = -1,
    GENERAL = 0,
    AUTHENTICATION_FAILURE = 1,
    IMSI_UNKNOWN_IN_HLR = 2,
    ILLEGAL_MS = 3,
    ILLEGAL_ME = 4,
    PLMN_NOT_ALLOWED = 5,
    LOCATION_AREA_NOT_ALLOWED = 6,
    ROAMING_NOT_ALLOWED = 7,
    NO_SUITABLE_CELL_IN_LOCATION_AREA = 8,
    NETWORK_FAILURE = 9,
    PERSISTEN_LOCATION_UPDATE_REJECT = 10 /* Not supported */
} Reg_Deny_DetailReason;

typedef enum {
    NO_RESULT,              /* No intermediate response expected. */
    NUMERIC,                /* A single intermediate response starting with a 0-9. */
    SINGLELINE,             /* A single intermediate response starting with a prefix. */
    MULTILINE               /* Multiple line intermediate response
                               starting with a prefix. */
} ATCommandType;

/** A singly-linked list of intermediate responses. */
typedef struct ATLine {
    struct ATLine *p_next;
    char *line;
} ATLine;

/** Free this with at_response_free(). */
typedef struct {
    int success;            /* True if final response indicates
                               success (eg "OK"). */
    char *finalResponse;    /* Eg OK, ERROR */
    ATLine *p_intermediates;    /* Any intermediate responses. */
} ATResponse;

/**
 * A user-provided unsolicited response handler function.
 * This will be called from the reader thread, so do not block.
 * "s" is the line, and "sms_pdu" is either NULL or the PDU response
 * for multi-line TS 27.005 SMS PDU responses (eg +CMT:).
 */
typedef void (*ATUnsolHandler)(const char *s, const char *sms_pdu);

int at_open(int fd, ATUnsolHandler h);
void at_close();

/*
 * Set default timeout for at commands. Let it be reasonable high
 * since some commands take their time. Default is 10 minutes.
 */
void at_set_timeout_msec(int timeout);

/*
 * This callback is invoked on the command thread.
 * You should reset or handshake here to avoid getting out of sync.
 */
void at_set_on_timeout(void (*onTimeout)(void));

/*
 * This callback is invoked on the reader thread (like ATUnsolHandler), when the
 * input stream closes before you call at_close (not when you call at_close()).
 * You should still call at_close(). It may also be invoked immediately from the
 * current thread if the read channel is already closed.
 */
void at_set_on_reader_closed(void (*onClose)(void));

void at_send_escape(void);

int at_send_command_singleline(const char *command,
                               const char *responsePrefix,
                               ATResponse **pp_outResponse);

int at_send_command_singleline_with_timeout(const char *command,
                                            const char *responsePrefix,
                                            ATResponse **pp_outResponse,
                                            long long timeoutMsec);

int at_send_command_numeric(const char *command,
                            ATResponse **pp_outResponse);

int at_send_command_multiline(const char *command,
                              const char *responsePrefix,
                              ATResponse **pp_outResponse);

int at_send_command_multiline_with_timeout(const char *command,
                                           const char *responsePrefix,
                                           ATResponse **pp_outResponse,
                                           long long  timeoutMsec);

int at_handshake();

int at_send_command(const char *command, ATResponse **pp_outResponse);

int at_send_command_with_timeout(const char *command,
                                 ATResponse **pp_outResponse,
                                 long long timeoutMsec);

int at_send_command_sms(const char *command, const char *pdu,
                        const char *responsePrefix,
                        ATResponse **pp_outResponse);

int at_send_command_with_pdu(const char *command, const char *pdu,
                             ATResponse **pp_outResponse);

void at_response_free(ATResponse *p_response);

void at_make_default_channel(void);

int at_get_cme_error(const ATResponse *p_response, ATCmeError *p_cme_error_code);

int at_get_cms_error(const ATResponse *p_response, ATCmsError *p_cms_error_code);

RIL_LastDataCallActivateFailCause at_get_sm_cause(const ATResponse *p_response);

#ifdef __cplusplus
}
#endif
#endif
