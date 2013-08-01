LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
        SinkPlayer.cpp             \

LOCAL_C_INCLUDES:= \
        $(TOP)/miracast-sink/native/wifi-display\


LOCAL_SHARED_LIBRARIES:= \
        libwfd                          \
        libstagefright_foundation       \
        libutils                        \


LOCAL_MODULE:= libwfd_jni

LOCAL_MODULE_TAGS:= optional

include $(BUILD_SHARED_LIBRARY)

