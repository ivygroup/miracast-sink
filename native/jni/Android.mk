LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
        SinkPlayer.cpp             \
        ivygroup_wfdplayer_sinkplayer.cpp \

LOCAL_C_INCLUDES:= \
        $(TOP)/frameworks/av/include/media/stagefright \
        $(TOP)/miracast-sink/native/wifi-display \
        $(TOP)/frameworks/native/include/media/openmax \
        $(PV_INCLUDES) \
        $(JNI_H_INCLUDE) \
        $(call include-path-for, corecg graphics)

LOCAL_SHARED_LIBRARIES:= \
        libandroid_runtime \
        libwfd                          \
        libstagefright_foundation       \
        libutils                        \
        libnativehelper \
        libui \
        libgui \
        libskia \




LOCAL_MODULE:= libwfd_jni

LOCAL_MODULE_TAGS:= optional

include $(BUILD_SHARED_LIBRARY)

