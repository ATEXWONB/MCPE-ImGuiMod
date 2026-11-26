#include "KittyMemory/KittyMemory.h"
#include "ImGui.h"
#include "BlurBg.h"
#define STAGE(...) do { FILE* _sf = fopen("/data/data/com.mojang.minecraftpe/files/mcstage.log", "a"); if (!_sf) _sf = fopen("/storage/emulated/0/Android/data/com.mojang.minecraftpe/files/mcstage.log", "a"); if (_sf) { fprintf(_sf, __VA_ARGS__); fputc(10, _sf); fflush(_sf); fclose(_sf); } } while (0)

#include <stdio.h>
#define MENULOG(...) do { FILE* _m=fopen("/storage/emulated/0/Android/data/com.mojang.minecraftpe/mcmod.log","a"); if(!_m) _m=fopen("/sdcard/Android/data/com.mojang.minecraftpe/mcmod.log","a"); if(_m){fprintf(_m,__VA_ARGS__);fputc('\n',_m);fclose(_m);} } while(0)
#include "dobby.h"
#include "Includes/Utils.h"
#include "Includes/obfuscate.h"
#include "Includes/Logger.h"
#include "imgui/imgui.h"
#include "Includes/Roboto-Regular.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "imgui/backends/imgui_impl_android.h"
#include <GLES3/gl3.h>
#include <android/input.h>
#include <unistd.h>
#include "imgui/font.h"
#ifdef LIB_INPUT
#include "get_device_api_level_inlines.h"

#define STAGE(...) do { \
    FILE* _sf = fopen("/data/data/com.mojang.minecraftpe/files/mcstage.log", "a"); \
    if (!_sf) _sf = fopen("/storage/emulated/0/Android/data/com.mojang.minecraftpe/files/mcstage.log", "a"); \
    if (_sf) { fprintf(_sf, __VA_ARGS__); fputc('\n', _sf); fflush(_sf); fclose(_sf); } } while (0)

#endif
using swapbuffers_orig = EGLBoolean (*)(EGLDisplay dpy, EGLSurface surf);
EGLBoolean swapbuffers_hook(EGLDisplay dpy, EGLSurface surf);
swapbuffers_orig o_swapbuffers = nullptr;

void (*menuAddress)();
void (*onInitAddr)();

bool isInitialized = false;
extern bool mod_ui_swallow(float x, float y);
int glWidth = 0;
int glHeight = 0;

int getGlWidth()
{
    return glWidth;
}
int getGlHeight()
{
    return glHeight;
}

// Taken from https://github.com/fedes1to/Zygisk-ImGui-Menu/blob/main/module/src/main/cpp/hook.cpp
#define HOOKINPUT(ret, func, ...)                                                                                      \
    ret (*orig##func)(__VA_ARGS__);                                                                                    \
    ret my##func(__VA_ARGS__)

HOOKINPUT(void, Input, void *thiz, void *ex_ab, void *ex_ac)
{
    { static int _in = 0; if (isInitialized && _in < 6) { _in++; STAGE("[input] Input ev=%p", thiz); } }
    origInput(thiz, ex_ab, ex_ac);
    if (isInitialized)
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent *)thiz);
    return;
}

HOOKINPUT(bool,MotionEventCopy,void* thiz, void* other, bool keepHistory){
    { static int _cn = 0;
      if (isInitialized && other && _cn < 8) { _cn++;
        STAGE("[input] copy act=%d xy=%.0f,%.0f", (int)AMotionEvent_getAction((const AInputEvent*)other),
              AMotionEvent_getX((const AInputEvent*)other, 0), AMotionEvent_getY((const AInputEvent*)other, 0)); } }
    if (isInitialized && other) {
        int32_t act = AMotionEvent_getAction((const AInputEvent*)other);
        int32_t at  = act & AMOTION_EVENT_ACTION_MASK;
        if (at != AMOTION_EVENT_ACTION_CANCEL) {
            float tx = AMotionEvent_getX((const AInputEvent*)other, 0);
            float ty = AMotionEvent_getY((const AInputEvent*)other, 0);
            if (mod_ui_swallow(tx, ty)) {
                /* 防触摸穿透：事件喂给 ImGui，但不复制给游戏 → 点在面板上不会打中世界 */
                ImGui_ImplAndroid_HandleInputEvent((AInputEvent *)other);
                return true;
            }
        }
    }
   bool ret = origMotionEventCopy(thiz, other, keepHistory);
    if (isInitialized)
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent *)thiz);
    return ret;
}

ImVec2 initialScreenSize;
// This menu_addr is used to allow for multiple game support in the future
bool needClear = true;
void *initModMenu(void *menu_addr, void *on_init_addr)
{
    STAGE("[stage] initModMenu enter");
    menuAddress = (void (*)())menu_addr;
    onInitAddr = (void (*)())on_init_addr;
    // do
    // {
    //     sleep(1);
    // } while (!isLibraryLoaded(OBFUSCATE("libEGL.so")));
        needClear = false;
        STAGE("[stage] wait libEGL");
        while (!isLibraryLoaded(OBFUSCATE("libEGL.so")))
        {
            sleep(1);
        }
        auto swapBuffers = ((uintptr_t)DobbySymbolResolver(OBFUSCATE("libEGL.so"), OBFUSCATE("eglSwapBuffers")));
        STAGE("[stage] eglSwapBuffers=%p", (void*)swapBuffers);
        KittyMemory::ProtectAddr((void *)swapBuffers, sizeof(swapBuffers), PROT_READ | PROT_WRITE | PROT_EXEC);
        { int _r = DobbyHook((void *)swapBuffers, (void *)swapbuffers_hook, (void **)&o_swapbuffers); STAGE("[stage] hook swap rc=%d orig=%p", _r, (void*)o_swapbuffers); }

// // Taken from https://github.com/fedes1to/Zygisk-ImGui-Menu/blob/main/module/src/main/cpp/hook.cpp
#ifdef LIB_INPUT
        if(api_level() < 35){
            void *sym_input = DobbySymbolResolver(
            OBFUSCATE("libinput.so"),
            OBFUSCATE("_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE"));
            if (sym_input != nullptr)
            {
               KittyMemory::ProtectAddr((void *)sym_input, sizeof(sym_input), PROT_READ | PROT_WRITE | PROT_EXEC);
               { int _r = DobbyHook((void *)sym_input, (void *)myInput, (void **)&origInput); STAGE("[stage] hook input rc=%d", _r); }
            }
        }else{
            void *sym_input2 = DobbySymbolResolver(
            OBFUSCATE("libinput.so"),
            OBFUSCATE("_ZN7android11MotionEvent8copyFromEPKS0_b"));
            if(sym_input2 != nullptr){
               KittyMemory::ProtectAddr((void *)sym_input2, sizeof(sym_input2), PROT_READ | PROT_WRITE | PROT_EXEC);
               { int _r = DobbyHook((void *)sym_input2, (void *)myMotionEventCopy, (void **)&origMotionEventCopy); STAGE("[stage] hook copy rc=%d orig=%p", _r, (void*)origMotionEventCopy); }
            }
        }
#endif
    STAGE("[stage] initModMenu done");
    LOGI("%s", (char *)OBFUSCATE("ImGUI Hooks initialized"));
    return nullptr;
}

void setupMenu()
{
    if (isInitialized)
        return;

    STAGE("[stage] setupMenu enter gl=%dx%d", glWidth, glHeight);
    MENULOG("[menu] setupMenu enter gl=%dx%d", glWidth, glHeight);
    auto ctx = ImGui::CreateContext();
    if (!ctx)
    {
        LOGI("%s", (char *)OBFUSCATE("Failed to create context"));
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)glWidth, (float)glHeight);
    // io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.IniFilename = nullptr;
    // enable docking
//    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Setup Platform/Renderer backends
    STAGE("[stage] ctx ok");
    MENULOG("[menu] ctx ok");
    ImGui_ImplAndroid_Init();
    STAGE("[stage] android backend ok");
    MENULOG("[menu] android backend ok");
    ImGui_ImplOpenGL3_Init("#version 300 es");
    STAGE("[stage] gl backend ok");
    MENULOG("[menu] gl backend called");

    ImFontConfig font_cfg;
    font_cfg.SizePixels = 16.0f;
    io.Fonts->AddFontFromMemoryTTF((void *)font_v, font_v_size, 22.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    io.Fonts->AddFontDefault(&font_cfg);

    ImGuiStyle &style = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    style.WindowPadding    = ImVec2(22.f, 18.f);
    style.FramePadding      = ImVec2(14.f, 10.f);
    style.CellPadding       = ImVec2(10.f, 8.f);
    style.ItemSpacing       = ImVec2(14.f, 14.f);
    style.ItemInnerSpacing  = ImVec2(10.f, 10.f);
    style.ScrollbarSize     = 30.f;
    style.GrabMinSize       = 34.f;
    style.WindowRounding    = 16.f;
    style.ChildRounding     = 14.f;
    style.FrameRounding     = 12.f;
    style.PopupRounding     = 12.f;
    style.ScrollbarRounding = 14.f;
    style.GrabRounding      = 12.f;
    style.TabRounding       = 12.f;
    style.WindowBorderSize  = 0.f;
    style.ChildBorderSize   = 0.f;
    style.PopupBorderSize   = 1.f;
    style.FrameBorderSize   = 0.f;
    style.WindowTitleAlign  = ImVec2(0.04f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SeparatorTextBorderSize  = 2.f;
    ImVec4 accent = ImVec4(0.22f, 0.56f, 1.00f, 1.f);
    ImVec4 accentH = ImVec4(0.34f, 0.66f, 1.00f, 1.f);
    ImVec4 accentA = ImVec4(0.16f, 0.44f, 0.86f, 1.f);
    style.Colors[ImGuiCol_WindowBg]        = ImVec4(0.055f, 0.062f, 0.075f, 0.97f);
    style.Colors[ImGuiCol_ChildBg]         = ImVec4(0.075f, 0.082f, 0.100f, 0.90f);
    style.Colors[ImGuiCol_PopupBg]         = ImVec4(0.075f, 0.082f, 0.100f, 0.98f);
    style.Colors[ImGuiCol_Border]          = ImVec4(0.20f, 0.24f, 0.32f, 0.60f);
    style.Colors[ImGuiCol_TitleBg]         = ImVec4(0.10f, 0.13f, 0.19f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive]   = ImVec4(0.12f, 0.16f, 0.24f, 1.00f);
    style.Colors[ImGuiCol_TitleBgCollapsed]= ImVec4(0.10f, 0.13f, 0.19f, 1.00f);
    style.Colors[ImGuiCol_Text]            = ImVec4(0.92f, 0.94f, 0.98f, 1.00f);
    style.Colors[ImGuiCol_TextDisabled]    = ImVec4(0.52f, 0.56f, 0.64f, 1.00f);
    style.Colors[ImGuiCol_Button]          = accent;
    style.Colors[ImGuiCol_ButtonHovered]   = accentH;
    style.Colors[ImGuiCol_ButtonActive]    = accentA;
    style.Colors[ImGuiCol_FrameBg]         = ImVec4(0.13f, 0.15f, 0.19f, 0.90f);
    style.Colors[ImGuiCol_FrameBgHovered]  = ImVec4(0.17f, 0.20f, 0.26f, 0.95f);
    style.Colors[ImGuiCol_FrameBgActive]   = ImVec4(0.20f, 0.24f, 0.31f, 1.00f);
    style.Colors[ImGuiCol_SliderGrab]      = accent;
    style.Colors[ImGuiCol_SliderGrabActive]= accentH;
    style.Colors[ImGuiCol_CheckMark]       = accentH;
    style.Colors[ImGuiCol_Separator]       = ImVec4(0.20f, 0.24f, 0.32f, 0.55f);
    style.Colors[ImGuiCol_ScrollbarBg]     = ImVec4(0.09f, 0.10f, 0.13f, 0.40f);
    style.Colors[ImGuiCol_ScrollbarGrab]   = ImVec4(0.28f, 0.42f, 0.72f, 0.65f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = accent;
    style.Colors[ImGuiCol_ScrollbarGrabActive]  = accentH;

    STAGE("[stage] font ok -> onInit");
    MENULOG("[menu] font ok, calling onInit");
    if (onInitAddr)
        onInitAddr();
    STAGE("[stage] onInit returned");
    MENULOG("[menu] onInit returned");

    isInitialized = true;
    STAGE("[stage] setup done isInitialized=1");
    MENULOG("[menu] setup done isInitialized=1");
    LOGI("setup done.");
}
void internalDrawMenu(int width, int height)
{
    if (!isInitialized)
        return;

    static int _dn = 0; _dn++;
    int _dbg = (_dn <= 4);
    if (_dbg) STAGE("[stage] draw#%d enter", _dn);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(width, height);
    ImGui::NewFrame();
    if (_dbg) STAGE("[stage] draw#%d newframe ok", _dn);
    { ImGuiIO& _io = ImGui::GetIO();
      if (_dn < 6) STAGE("[stage] disp now %.0fx%.0f draw %.0fx%.0f", _io.DisplaySize.x, _io.DisplaySize.y, (float)width, (float)height); }

    ImGui::SetNextWindowSize(ImVec2((float)width / 2, (float)height / 2), ImGuiCond_Once);
    if (_dbg) STAGE("[stage] draw#%d call menu", _dn);
    menuAddress();
    if (_dbg) STAGE("[stage] draw#%d menu returned", _dn);

    ImGui::Render();
    if (_dbg) STAGE("[stage] draw#%d render ok", _dn);

    if (needClear)
    {
        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (_dbg) STAGE("[stage] draw#%d drawdata ok", _dn);
}

EGLBoolean swapbuffers_hook(EGLDisplay dpy, EGLSurface surf)
{
    static int _nf = 0;
    _nf++;
    if (_nf <= 3 || (_nf % 60) == 0) STAGE("[stage] swap #%d init=%d", _nf, (int)isInitialized);
    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    glWidth = w;
    glHeight = h;
    static bool initialScreenSet = false;
    if (!initialScreenSet)
    {
        initialScreenSize.x = w;
        initialScreenSize.y = h;
        initialScreenSet = true;
    }
    { static EGLContext s_lastGlCtx = EGL_NO_CONTEXT;
      EGLContext curGlCtx = eglGetCurrentContext();
      if (s_lastGlCtx != EGL_NO_CONTEXT && curGlCtx != s_lastGlCtx && isInitialized) {
          STAGE("[stage] gl ctx changed, rebuild gl objects");
          ImGui_ImplOpenGL3_DestroyDeviceObjects();
          BlurBg::onContextReset();
      }
      s_lastGlCtx = curGlCtx; }
    setupMenu();
    internalDrawMenu(w, h);
    if (_nf <= 3) STAGE("[stage] swap #%d frame done", _nf);

    return o_swapbuffers(dpy, surf);
}
