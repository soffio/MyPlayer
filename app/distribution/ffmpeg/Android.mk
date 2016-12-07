LOCAL_PATH := $(call my-dir)


# FFmpeg library
include $(CLEAR_VARS)
LOCAL_MODULE := ffmpeg
LOCAL_SRC_FILES := libffmpeg.so
# LOCAL_C_INCLUDES := $(LOCAL_PATH)

# LOCAL_EXPORT_C_INCLUDES := $(LOCAL_C_INCLUDES)\
#  $(LOCAL_C_INCLUDES)/libavcodec\
#  $(LOCAL_C_INCLUDES)/libavfilter\
#  $(LOCAL_C_INCLUDES)/libavformat\
#  $(LOCAL_C_INCLUDES)/libavutil\
#  $(LOCAL_C_INCLUDES)/libswresample\
#  $(LOCAL_C_INCLUDES)/libswscale

include $(PREBUILT_SHARED_LIBRARY)
