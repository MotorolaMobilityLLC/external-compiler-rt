#
# Copyright (C) 2012 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
#

LOCAL_PATH:= $(call my-dir)

ifeq ($(TARGET_ARCH),arm)

ASAN_NEEDS_SEGV=0
ASAN_HAS_EXCEPTIONS=1
ASAN_FLEXIBLE_MAPPING_AND_OFFSET=0


asan_rtl_files := \
	asan_rtl.cc \
	asan_allocator.cc	\
	asan_globals.cc	\
	asan_interceptors.cc	\
	asan_linux.cc \
	asan_mac.cc \
	asan_malloc_linux.cc \
	asan_malloc_mac.cc \
	asan_poisoning.cc	\
	asan_posix.cc \
	asan_printf.cc	\
	asan_stack.cc	\
	asan_stats.cc	\
	asan_thread.cc	\
	asan_thread_registry.cc	\
	interception/interception_linux.cc

asan_rtl_cflags := \
	-fvisibility=hidden \
	-fno-exceptions \
	-DASAN_LOW_MEMORY=1 \
	-DASAN_NEEDS_SEGV=$(ASAN_NEEDS_SEGV) \
	-DASAN_HAS_EXCEPTIONS=$(ASAN_HAS_EXCEPTIONS) \
	-DASAN_FLEXIBLE_MAPPING_AND_OFFSET=$(ASAN_FLEXIBLE_MAPPING_AND_OFFSET) \
	-Wno-covered-switch-default \
	-Wno-sign-compare

asan_test_files := \
	tests/asan_test.cc \
	tests/asan_globals_test.cc \
	tests/asan_break_optimization.cc \
	tests/asan_interface_test.cc

asan_test_cflags := \
	-DADDRESS_SANITIZER=1 \
	-faddress-sanitizer \
	-mllvm -asan-blacklist=external/asan/tests/asan_test.ignore \
	-mllvm -asan-stack=1 \
	-mllvm -asan-globals=1 \
	-mllvm -asan-mapping-scale=0 \
	-mllvm -asan-mapping-offset-log=-1 \
	-mllvm -asan-use-after-return=0 \
	-DASAN_LOW_MEMORY=1 \
	-DASAN_UAR=0 \
	-DASAN_NEEDS_SEGV=$(ASAN_NEEDS_SEGV) \
	-DASAN_HAS_EXCEPTIONS=$(ASAN_HAS_EXCEPTIONS) \
	-DASAN_HAS_BLACKLIST=1 \
	-Wno-covered-switch-default


include $(CLEAR_VARS)

LOCAL_C_INCLUDES := \
	        bionic

LOCAL_MODULE := libasan
LOCAL_MODULE_TAGS := optional
LOCAL_CFLAGS += \
	$(asan_rtl_cflags) \
	-Wno-unused-parameter \
	-D__WORDSIZE=32
LOCAL_SRC_FILES := asan_android_stub.cc
LOCAL_CPP_EXTENSION := .cc
LOCAL_CLANG := true
include $(BUILD_STATIC_LIBRARY)



include $(CLEAR_VARS)

LOCAL_C_INCLUDES := \
	        bionic

LOCAL_MODULE := libasan_preload
LOCAL_MODULE_TAGS := optional
LOCAL_CFLAGS += \
	$(asan_rtl_cflags) \
	-Wno-unused-parameter \
	-D__WORDSIZE=32
LOCAL_SRC_FILES := $(asan_rtl_files)
LOCAL_CPP_EXTENSION := .cc
LOCAL_SHARED_LIBRARIES := libc libstlport libdl
LOCAL_CLANG := true
include $(BUILD_SHARED_LIBRARY)


include $(CLEAR_VARS)

LOCAL_MODULE := asanwrapper
LOCAL_MODULE_TAGS := optional

LOCAL_C_INCLUDES := \
        bionic \
        external/stlport/stlport

LOCAL_SRC_FILES := asanwrapper.cc
LOCAL_CPP_EXTENSION := .cc
LOCAL_SHARED_LIBRARIES := libstlport libc

include $(BUILD_EXECUTABLE)


include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	../../../../frameworks/base/cmds/app_process/app_main.cpp

LOCAL_SHARED_LIBRARIES := \
	libcutils \
	libutils \
	libbinder \
	libandroid_runtime

LOCAL_MODULE:= asan/app_process
LOCAL_MODULE_TAGS := optional

LOCAL_CFLAGS += \
       -faddress-sanitizer
LOCAL_LDFLAGS += -Wl,-u,__asan_preinit

LOCAL_STATIC_LIBRARIES += libasan
LOCAL_SHARED_LIBRARIES += libdl libasan_preload
LOCAL_CLANG:=true

include $(BUILD_EXECUTABLE)

endif # ifeq($(TARGET_ARCH),arm)
