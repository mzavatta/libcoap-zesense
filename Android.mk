# Marco Zavatta
# TELECOM Bretagne
# Android.mk build file for libcoap by Olaf Bergmann

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := libcoap-3.0.0-android
LOCAL_SRC_FILES := async.c block.c coap_list.c debug.c encode.c hashkey.c net.c option.c pdu.c resource.c str.c subscribe.c uri.c asynchronous.c
# LOCAL_C_INCLUDES := $(LOCAL_PATH)/../workspace/ZeSenseCoAP
# LOCAL_LDLIBS  := -llog -landroid -lEGL -lGLESv1_CM
LOCAL_CFLAGS :=  -Wall -Wextra -std=c99 -pedantic -g -O2
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)

include $(BUILD_STATIC_LIBRARY)
