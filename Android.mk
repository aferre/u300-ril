# Copyright (C) ST-Ericsson AB 2008-2009
# Copyright 2006 The Android Open Source Project
#
# Based on reference-ril
# Modified for ST-Ericsson U300 modems.
# Author: Christian Bejram <christian.bejram@stericsson.com>
#
# XXX using libutils for simulator build only...
#
LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	u300-ril.c \
	u300-ril-manager.c \
	u300-ril-callhandling.c \
	u300-ril-messaging.c \
	u300-ril-network.c \
	u300-ril-pdp.c \
	u300-ril-requestdatahandler.c \
	u300-ril-services.c \
	u300-ril-sim.c \
	u300-ril-stk.c \
	u300-ril-audio.c \
	u300-ril-information.c \
	u300-ril-oem.cpp \
	u300-ril-oem-parser.cpp \
	atchannel.c \
	misc.c \
	fcp_parser.c \
	at_tok.c

LOCAL_SHARED_LIBRARIES := \
	libcutils \
	libutils \
	libbinder \
	libril \
	libnetutils

# For asprinf
LOCAL_CFLAGS := -D_GNU_SOURCE

LOCAL_C_INCLUDES := \
	$(KERNEL_HEADERS) \
	$(TOP)/hardware/ril/libril/ \
	$(TOP)/system/core/libnetutils/ \
	$(TOP)/external/astl/include/

# Disable prelink, or add to build/core/prelink-linux-arm.map
LOCAL_PRELINK_MODULE := false

LOCAL_LDLIBS += -lpthread

LOCAL_CFLAGS += -DRIL_SHLIB
LOCAL_CFLAGS += -DU300_RIL_OEM_MSG_SELFTEST

ifneq ($(ENABLE_REPORTING_ALERTING_UPON_MISSING_CALL_STATE_FROM_NETWORK),)
LOCAL_CFLAGS += DENABLE_REPORTING_ALERTING_UPON_MISSING_CALL_STATE_FROM_NETWORK
endif

ifneq ($(LTE_COMMAND_SET_ENABLED),)
LOCAL_CFLAGS += -DLTE_COMMAND_SET_ENABLED
endif

ifneq ($(EXTERNAL_MODEM_CONTROL_MODULE_DISABLED),)
LOCAL_CFLAGS += -DEXTERNAL_MODEM_CONTROL_MODULE_DISABLED
else
LOCAL_C_INCLUDES += \
	$(TOP)/external/dbus/

LOCAL_SHARED_LIBRARIES += \
	libdbus

LOCAL_LDLIBS += -ldbus
endif

ifneq ($(CAIF_SOCKET_SUPPORT_DISABLED),)
LOCAL_CFLAGS += -DCAIF_SOCKET_SUPPORT_DISABLED
else
LOCAL_SRC_FILES += u300-ril-netif.c
endif

ifneq ($(USE_EARLY_NITZ_TIME_SUBSCRIPTION),)
LOCAL_CFLAGS += -DUSE_EARLY_NITZ_TIME_SUBSCRIPTION
endif

ifneq ($(RIL_MAX_MTU),)
LOCAL_CFLAGS += -DRIL_MAX_MTU=$(RIL_MAX_MTU)
else
LOCAL_CFLAGS += -DRIL_MAX_MTU=1500
endif

ifneq ($(USE_LEGACY_SAT_AT_CMDS),)
LOCAL_CFLAGS += -DUSE_LEGACY_SAT_AT_CMDS
endif

ifneq ($(USE_EXT1_INSTEAD_OF_EXT5_WHEN_SIM_CARD_IS_2G_TYPE),)
LOCAL_CFLAGS += -DUSE_EXT1_INSTEAD_OF_EXT5_WHEN_SIM_CARD_IS_2G_TYPE
endif

LOCAL_MODULE := libu300-ril
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

##########

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
        atchannel.c \
        misc.c \
        at_tok.c

LOCAL_SHARED_LIBRARIES := libcutils libdbus

# Disable prelink, or add to build/core/prelink-linux-arm.map
LOCAL_PRELINK_MODULE := false

LOCAL_MODULE := libu300-parser
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)
