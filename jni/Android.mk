LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := Dobby
LOCAL_SRC_FILES := Dobby/${TARGET_ARCH_ABI}/libdobby.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := mcmod

LOCAL_CFLAGS := -w -funwind-tables -fexceptions
LOCAL_CPPFLAGS := -w -std=c++17 -fexceptions
LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-all
LOCAL_LDLIBS += -llog -landroid -lEGL -lGLESv3

LOCAL_C_INCLUDES += $(LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)/imgui
LOCAL_C_INCLUDES += $(LOCAL_PATH)/imgui/backends
LOCAL_C_INCLUDES += $(LOCAL_PATH)/Includes
LOCAL_C_INCLUDES += $(LOCAL_PATH)/Dobby

LOCAL_SRC_FILES := Main.cpp \
    Mod/McMod.cpp \
    Mod/Aim.c \
    Mod/Blood.c \
    Mod/VmHook.c \
    Mod/VmHookAsm.S \
    Menu/ImGui.cpp \
    Menu/BlurBg.cpp \
    Includes/Utils.cpp \
    Includes/Logger.cpp \
    KittyMemory/KittyMemory.cpp \
    KittyMemory/MemoryPatch.cpp \
    KittyMemory/MemoryBackup.cpp \
    KittyMemory/KittyUtils.cpp \
    imgui/backends/imgui_impl_opengl3.cpp \
    imgui/backends/imgui_impl_android.cpp \
    imgui/imgui.cpp \
    imgui/imgui_draw.cpp \
    imgui/imgui_tables.cpp \
    imgui/imgui_widgets.cpp

LOCAL_STATIC_LIBRARIES := Dobby
include $(BUILD_SHARED_LIBRARY)
