#include <jni.h>
#include <pthread.h>
#include "Menu/ImGui.h"
#include "Mod/McMod.h"
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#define ML(...) do { FILE* f = fopen("/sdcard/Android/data/com.mojang.minecraftpe/mcmod.log", "a"); if (!f) f = fopen("/storage/emulated/0/Android/data/com.mojang.minecraftpe/mcmod.log", "a"); if (f) { fprintf(f, __VA_ARGS__); fputc('\n', f); fclose(f); } } while(0)

void on_init() { ML("[main] on_init called"); McMod_Init(); }

void *hack_thread(void *) {
    ML("[main] thread start");
    initModMenu((void *)McMod_Draw, (void *)on_init);
    ML("[main] initModMenu returned");
    return nullptr;
}

__attribute__((constructor)) void lib_main() {
    ML("[main] ctor pid=%d", (int)getpid());
    pthread_t ptid;
    pthread_create(&ptid, nullptr, hack_thread, nullptr);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    return JNI_VERSION_1_6;
}
