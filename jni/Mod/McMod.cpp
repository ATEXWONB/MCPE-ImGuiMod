// McMod.cpp — 《我的世界》Bedrock 局域网人数模组
#include "McMod.h"
#include <unistd.h>
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string>
#include <vector>
#include <fstream>
#include <thread>
#include <android/log.h>
#include <android/input.h>
#include <fcntl.h>
#include <time.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "Menu/island.h"
#include "Menu/BlurBg.h"
#include "imgui/backends/imgui_impl_android.h"
#include "KittyMemory/KittyMemory.h"
#include "dobby.h"

extern int  glWidth;
extern int  glHeight;
extern bool isInitialized;

#define TAG "McMod"
#define LI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define MCLOG(...) do { FILE* _f = fopen("/storage/emulated/0/Android/data/com.mojang.minecraftpe/mcmod.log", "a"); if (!_f) _f = fopen("/sdcard/Android/data/com.mojang.minecraftpe/mcmod.log", "a"); if (_f) { fprintf(_f, __VA_ARGS__); fputc('\n', _f); fclose(_f); } __android_log_print(ANDROID_LOG_INFO, "McMod", __VA_ARGS__); } while(0)

static const char*    kLib       = "libminecraftpe.so";
static const uint32_t OFF_SETMAX = 0xCBFBF58u;   // ServerLevel::setMaxPlayers(this,int)
static const uint32_t OFF_CAPCSE = 0xCBFBF98u;   // csel w8,w8,w9,gt  ->  mov w8,w9
static const uint32_t OFF_VTBL   = 0x13B2D598u;  // ServerLevel 虚表
static const int      OFF_MAXVAL = 0x320;

static uintptr_t g_libBase = 0;
static void*     g_level   = nullptr;
volatile unsigned long g_levelAddr = 0;
static int       g_target  = 20;
static bool      g_panelOpen = false;
static bool      g_hold    = false;
static float     g_uiAlpha = 1.0f;
static const char* kCfg = "/sdcard/Android/data/com.mojang.minecraftpe/mcmod.cfg";      // 持续保持
static float     g_ballX = -1.f, g_ballY = -1.f;
static bool      g_dragging = false;
static float     g_pressX = 0, g_pressY = 0, g_ballStartX = 0, g_ballStartY = 0;
static char      g_msg[200] = "";
static bool      g_capDone = false;
static bool      g_scanning = false;
static bool      g_spinning_ready = false;
static bool      g_rotsScanned = false;

static bool      g_inputHooked = false;
typedef bool (*fn_setmax)(void*, int);
static fn_setmax g_setmax = nullptr;

// ---------- 基础工具 ----------
extern "C" {
void  aim_start(float radius, int mask);
void vmhook_install_all(void);
void  aim_atk_set_en(int v);
int blood_on(void);
void blood_set_on(int);
void blood_reset(void);
void blood_start(void);
const char* blood_status(void);
static int g_bloodStarted=0;
int   aim_atk_enabled(void);
void  aim_atk_set_radius(float r);
float aim_atk_radius(void);
int   aim_atk_taps(void);
void  aim_stop(void);
void  aim_set_enabled(int on);
int   aim_enabled(void);
void  aim_set_mask(int m);
int   aim_mask(void);
void  aim_set_radius(float r);
void  aim_rot_probe(float dy);
void  aim_set_level(int ok);
float aim_radius(void);
int   aim_has(void);
int   aim_cat(void);
float aim_dist(void);
const char* aim_name(void);
const char* aim_catname_of(int c);
int   aim_vpt_count(void);
int   aim_obj_count(void);
}

static void McStage(const char* s) {
    FILE* f = fopen("/data/data/com.mojang.minecraftpe/files/mcstage.log", "a");
    if (!f) f = fopen("/storage/emulated/0/Android/data/com.mojang.minecraftpe/files/mcstage.log", "a");
    if (f) { fprintf(f, "%s\n", s); fflush(f); fclose(f); }
}

static uintptr_t FindLibBase(const char* name)
{
    std::ifstream f("/proc/self/maps");
    std::string line;
    while (std::getline(f, line)) {
        if (line.find(name) != std::string::npos) {
            uintptr_t s = 0, e = 0; unsigned long off = 0; char perms[8] = {0};
            if (sscanf(line.c_str(), "%lx-%lx %7s %lx", &s, &e, perms, &off) == 4)
                return s - (uintptr_t)off;
        }
    }
    return 0;
}

// 验证对象是否仍是 ServerLevel（虚表指针未变）
static bool ValidLevel(void* p)
{
    if (!p || !g_libBase) return false;
    uintptr_t vt = g_libBase + OFF_VTBL;
    return *(uintptr_t*)p == vt;
}

// —— 带信号的保护读：任何非法地址都不会崩，只会回退 ——
#include <signal.h>
#include <setjmp.h>
static __thread sigjmp_buf g_jb;
static __thread volatile int g_guard = 0;
static struct sigaction g_old_seg, g_old_bus;

static void mcmod_fault(int sig, siginfo_t* si, void* uc)
{
    if (g_guard) { g_guard = 0; siglongjmp(g_jb, 1); }
    // 不在保护区里：还原默认处理，重新抛给系统
    signal(sig, SIG_DFL);
    raise(sig);
}

static void InstallFaultGuard()
{
    static bool done = false;
    if (done) return; done = true;
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = mcmod_fault; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_seg);
    sigaction(SIGBUS,  &sa, &g_old_bus);
}

static long vm_read(void* dst, uintptr_t src, unsigned long len)
{
    g_guard = 1;
    if (sigsetjmp(g_jb, 1) == 0) {
        memcpy(dst, (void*)src, len);
        g_guard = 0;
        return (long)len;
    }
    g_guard = 0;
    return -1;
}

// 扫描堆，找首字段 == ServerLevel 虚表的对象
static int   g_hits = 0;
static void* g_firstHit = nullptr;
static int   g_firstVal = 0;
static int   g_regions = 0;

static void* ScanLevelOnce()
{
    MCLOG("[scan] enter");
    InstallFaultGuard();
    if (!g_libBase) g_libBase = FindLibBase(kLib);
    if (!g_libBase) { MCLOG("[scan] libbase null"); return nullptr; }
    uintptr_t vt = g_libBase + OFF_VTBL;
    g_hits = 0; g_firstHit = nullptr; g_firstVal = 0; g_regions = 0;
    MCLOG("[scan] base=%p vt=%p", (void*)g_libBase, (void*)vt);
    static unsigned char* buf = nullptr;
    const unsigned long CHUNK = 1u << 20;
    if (!buf) buf = (unsigned char*)malloc(CHUNK);
    if (!buf) return nullptr;

    FILE* mf = fopen("/proc/self/maps", "r");
    if (!mf) return nullptr;

    void* found = nullptr;
    char line[512];
    while (!found && fgets(line, sizeof(line), mf)) {
        unsigned long s = 0, e = 0;
        char perms[8] = {0}, path[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &s, &e, perms, path);
        if (n < 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w') continue;
        if (n >= 4) {
            if (strstr(path, "libminecraftpe") || strstr(path, "/system/") ||
                strstr(path, "/apex/") || strstr(path, "/dev/ashmem/dalvik") ||
                strstr(path, "boot.oat") || strstr(path, "libmcmod") ||
                strstr(path, "dalvik-cache") || strstr(path, "libart.so")) continue;
        }
        g_regions++;
        unsigned long sz = e - s;
        for (unsigned long off = 0; off + 16 < sz; off += CHUNK) {
            unsigned long len = sz - off; if (len > CHUNK) len = CHUNK;
            long got = vm_read(buf, s + off, len);
            if (got <= 0) break;
            for (unsigned long i = 0; i + 16 < (unsigned long)got; i += 8) {
                uintptr_t val;
                memcpy(&val, buf + i, 8);
                if (val != vt) continue;
                g_hits++;
                { int tv = 0; if (vm_read(&tv, s + off + i + OFF_MAXVAL, 4) == 4) { if (!g_firstHit) { g_firstHit = (void*)(s + off + i); g_firstVal = tv; } MCLOG("[scan] hit=%p val=%d", (void*)(s + off + i), tv); } }
                uintptr_t cand = s + off + i;
                int mv = 0;
                if (vm_read(&mv, cand + OFF_MAXVAL, 4) != 4) continue;
                if (mv < 1 || mv > 100000) continue;
                uintptr_t f250 = 0;
                if (vm_read(&f250, cand + 0x250, 8) != 8) continue;
                if (f250 == 0) continue;   // 允许带 tag 的堆指针(b400...)
                found = (void*)cand;
                break;
            }
            if (found) break;
        }
    }
    fclose(mf);
    MCLOG("[scan] exit found=%p hits=%d regions=%d firstHit=%p firstVal=%d", found, g_hits, g_regions, g_firstHit, g_firstVal);
    return found;
}

// ===== 探针 v5：按文件给出的虚表偏移扫描对象 =====
static volatile int g_probeRun = 0;
static const uint32_t VT_ITEMSTACK5 = 0x13D96650u;

#define MAX_VT   24
#define MAX_HITS5 65536
static uintptr_t g_vtVal[MAX_VT];
static char      g_vtName[MAX_VT][40];
static int       g_vtN = 0;
static uintptr_t g_hitAddr[MAX_HITS5];
static int       g_hitVt[MAX_HITS5];
static int       g_hitN = 0;

static void LoadVtList()
{
    g_vtN = 0;
    FILE* f = fopen("/sdcard/vt_list.txt", "r");
    if (!f) { MCLOG("[p5] no vt_list.txt, use builtin");
        const char* b[] = {
            "13C7B7D0 ServerLevel", "139F8518 Player", "139F74E0 LocalPlayer",
            "13D43160 Inventory", "13D43B78 PlayerInventory", "13D96650 ItemStack",
            "138B65C8 CraftingScreenController", "13771760 ContainerScreenController" };
        for (int i = 0; i < 8 && g_vtN < MAX_VT; i++) {
            unsigned long off = 0; char nm[40] = {0};
            sscanf(b[i], "%lx %39s", &off, nm);
            g_vtVal[g_vtN] = (uintptr_t)off; strncpy(g_vtName[g_vtN], nm, 39); g_vtN++;
        }
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f) && g_vtN < MAX_VT) {
        unsigned long off = 0; char nm[40] = {0};
        if (sscanf(line, "%lx %39s", &off, nm) != 2) continue;
        if (off == 0) continue;
        g_vtVal[g_vtN] = (uintptr_t)off; strncpy(g_vtName[g_vtN], nm, 39); g_vtN++;
    }
    fclose(f);
    MCLOG("[p5] loaded %d vts", g_vtN);
}

static void DumpObj5(uintptr_t obj, int idx)
{
    uintptr_t vtIS = g_libBase + VT_ITEMSTACK5;
    MCLOG("[obj] %s %p", g_vtName[idx], (void*)obj);
    for (int o = 0; o < 0x800; o += 8) {
        uintptr_t v = 0;
        if (vm_read(&v, obj + o, 8) <= 0) break;
        if (!v) continue;
        uintptr_t t = 0;
        if (vm_read(&t, v, 8) > 0) {
            if (t == vtIS) { MCLOG("   +%03x %p -> ITEMSTACK", o, (void*)v); continue; }
            for (int k = 0; k < g_vtN; k++) {
                if (t == g_libBase + g_vtVal[k]) { MCLOG("   +%03x %p -> %s", o, (void*)v, g_vtName[k]); goto nxt; }
            }
        }
        MCLOG("   +%03x %p", o, (void*)v);
        nxt:;
    }
}

static void ProbeScan5()
{
    InstallFaultGuard();
    if (!g_libBase) g_libBase = FindLibBase(kLib);
    if (!g_libBase) { MCLOG("[p5] no libbase"); return; }
    LoadVtList();
    MCLOG("[p5] begin base=%p n=%d", (void*)g_libBase, g_vtN);
    static unsigned char* buf = nullptr;
    const unsigned long CHUNK = 1u << 20;
    if (!buf) buf = (unsigned char*)malloc(CHUNK);
    if (!buf) return;
    FILE* mf = fopen("/proc/self/maps", "r");
    if (!mf) return;
    char line[512]; g_hitN = 0;
    while (fgets(line, sizeof(line), mf)) {
        unsigned long s=0, e=0; char perms[8]={0}, path[256]={0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &s, &e, perms, path);
        if (n < 3 || perms[0] != 'r' || perms[1] != 'w') continue;
        if (strstr(path, "boot.oat") || strstr(path, "dalvik-cache") || strstr(path, "libart.so")) continue;
        unsigned long sz = e - s;
        for (unsigned long off=0; off+16 < sz; off += CHUNK) {
            unsigned long len = sz - off; if (len > CHUNK) len = CHUNK;
            long got = vm_read(buf, s + off, len);
            if (got <= 0) break;
            for (unsigned long i=0; i+16 < (unsigned long)got; i += 8) {
                uintptr_t v; memcpy(&v, buf + i, 8);
                for (int k = 0; k < g_vtN; k++) {
                    if (v == g_libBase + g_vtVal[k]) {
                        if (g_hitN < MAX_HITS5) { g_hitAddr[g_hitN] = s + off + i; g_hitVt[g_hitN] = k; g_hitN++; }
                        break;
                    }
                }
            }
        }
    }
    fclose(mf);
    for (int k = 0; k < g_vtN; k++) {
        int c = 0, shown = 0;
        for (int i = 0; i < g_hitN; i++) if (g_hitVt[i] == k) {
            c++;
            uintptr_t o = g_hitAddr[i];
            uintptr_t prev = 0; int inarr = 0;
            if (vm_read(&prev, o - 0x98, 8) > 0 && prev == g_libBase + g_vtVal[k]) inarr = 1;
            if (!inarr && shown < 8) { DumpObj5(o, k); shown++; }
        }
        MCLOG("[p5] count %s = %d", g_vtName[k], c);
    }
    MCLOG("[p5] end hits=%d", g_hitN);
}

static void* ProbeThread(void*)
{
    ProbeScan5();
    g_probeRun = 0;
    return nullptr;
}
static void StartProbeAsync()
{
    if (g_probeRun) return;
    g_probeRun = 1;
    pthread_t th;
    if (pthread_create(&th, nullptr, ProbeThread, nullptr) == 0) pthread_detach(th);
    else g_probeRun = 0;
}

// ===== 进程内 读写/转储 指令通道（/sdcard/act.txt）=====
static long vm_write(void* dst, uintptr_t dstA, const void* src, unsigned long len)
{
    g_guard = 1;
    if (sigsetjmp(g_jb, 1) == 0) {
        memcpy((void*)dstA, src, len);
        g_guard = 0;
        return (long)len;
    }
    g_guard = 0;
    return -1;
}

// ===== 背包直连（运行时自定位，不依赖绝对地址）=====
#define OFF_CLONE    0x11BA3270u   // Tag::clone()  ->  new CompoundTag(0x20) + copy_tree
#define VT_PLAYERINV 0x13D43B78u
#define VT_ITEMSTACK 0x13D96650u

static uintptr_t g_invBegin = 0;
static int       g_invN = 0;
static uintptr_t g_invList[8];
static int       g_invListN[8];
static int       g_invCntFound = 0;
static uintptr_t g_piObj[8];

static uintptr_t StripTag(uintptr_t v) { return v & 0x0000FFFFFFFFFFFFULL; }
static void NbtDumpSlot(int slot);
static const char* EnchNameCn(int id)
{
    switch (id) {
        case 0: return "保护";
        case 1: return "火焰保护";
        case 2: return "坪落保护";
        case 3: return "爆炸保护";
        case 4: return "弹射物保护";
        case 5: return "荆棘";
        case 6: return "水下呼吸";
        case 7: return "水下速掘";
        case 8: return "深海探索者";
        case 9: return "锋利";
        case 10: return "亡灵杀手";
        case 11: return "节肢杀手";
        case 12: return "击退";
        case 13: return "火焰附加";
        case 14: return "抢夺";
        case 15: return "效率";
        case 16: return "精准采集";
        case 17: return "耐久";
        case 18: return "时运";
        case 19: return "力量";
        case 20: return "冲击";
        case 21: return "火矢";
        case 22: return "无限";
        case 23: return "海之眷顾";
        case 24: return "饵钓";
        case 25: return "经验修补";
        case 28: return "消失诅咒";
        case 29: return "穿刺";
        case 30: return "激流";
        case 31: return "忠诚";
        case 32: return "引雷";
        case 33: return "多重射击";
        case 34: return "穿透";
        case 35: return "快速装填";
        case 36: return "灵魂疾行";
        case 37: return "迅捷潜行";
        default: return nullptr;
    }
}

static int PathIsEnchField(const char* p, const char* tail)
{
    int n = (int)strlen(p), t = (int)strlen(tail);
    if (n <= t) return 0;
    if (strstr(p, "ench[") == nullptr) return 0;
    return !strcmp(p + n - t, tail);
}

static const char* NbtTypeName(int t);

static uintptr_t CallClone(uintptr_t fn, uintptr_t self)
{
    volatile uintptr_t res = 0;
    uintptr_t outaddr = (uintptr_t)&res;
    __asm__ __volatile__("mov x0, %0\n\tmov x8, %1\n\tblr %2\n\t"
        :: "r"(self), "r"(outaddr), "r"(fn)
        : "x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11",
          "x12","x13","x14","x15","x16","x17","x30","memory");
    return res;
}

static uintptr_t FindInventoryNow()
{
    g_invBegin = 0; g_invN = 0; g_invCntFound = 0;
    if (!g_libBase) g_libBase = FindLibBase(kLib);
    if (!g_libBase) return 0;
    uintptr_t vtPI = g_libBase + VT_PLAYERINV;
    uintptr_t vtIS = g_libBase + VT_ITEMSTACK;
    static unsigned char* buf = nullptr;
    const unsigned long CHUNK = 1u << 20;
    if (!buf) buf = (unsigned char*)malloc(CHUNK);
    if (!buf) return 0;
    FILE* mf = fopen("/proc/self/maps", "r");
    if (!mf) return 0;
    char line[512];
    uintptr_t bestBeg = 0; int bestN = 0;
    while (fgets(line, sizeof(line), mf)) {
        unsigned long s = 0, e = 0; char perms[8] = {0}, path[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &s, &e, perms, path);
        if (n < 3 || perms[0] != 'r' || perms[1] != 'w') continue;
        if (s < 0x7000000000UL) continue;
        if (strstr(path, "boot.oat") || strstr(path, "dalvik-cache") || strstr(path, "libart.so")) continue;
        unsigned long sz = e - s;
        for (unsigned long off = 0; off + 16 < sz; off += CHUNK) {
            unsigned long len = sz - off; if (len > CHUNK) len = CHUNK;
            long got = vm_read(buf, s + off, len);
            if (got <= 0) break;
            for (long i = 0; i + 16 < got; i += 8) {
                uintptr_t v; memcpy(&v, buf + i, 8);
                if (v != vtPI) continue;
                uintptr_t obj = s + off + i;
                uintptr_t inv = 0;
                if (vm_read(&inv, obj + 0xB8, 8) <= 0) continue;
                inv = StripTag(inv);
                if (!inv) continue;
                uintptr_t beg = 0, end = 0;
                if (vm_read(&beg, inv + 0x140, 8) <= 0) continue;
                if (vm_read(&end, inv + 0x148, 8) <= 0) continue;
                beg = StripTag(beg); end = StripTag(end);
                if (!beg || end <= beg) continue;
                uintptr_t d = end - beg;
                if (d % 0x98) continue;
                int cnt = (int)(d / 0x98);
                if (cnt < 9 || cnt > 36) continue;
                uintptr_t v0 = 0;
                if (vm_read(&v0, beg, 8) <= 0) continue;
                if (v0 != vtIS) continue;
                int dup = 0;
                for (int q = 0; q < g_invCntFound; q++) if (g_invList[q] == beg) dup = 1;
                if (!dup && g_invCntFound < 8) { g_invList[g_invCntFound] = beg; g_invListN[g_invCntFound] = cnt; g_piObj[g_invCntFound] = obj; g_invCntFound++; }
                MCLOG("[inv] cand#%d obj=0x%lx inv=0x%lx beg=0x%lx n=%d", g_invCntFound - 1, (unsigned long)obj, (unsigned long)inv, (unsigned long)beg, cnt);
                if (cnt > bestN) { bestN = cnt; bestBeg = beg; }
            }
        }
    }
    fclose(mf);
    g_invBegin = bestBeg; g_invN = bestN;
    return g_invBegin;
}

static uintptr_t SlotAddr(int slot)
{
    if (!g_invBegin) FindInventoryNow();
    if (!g_invBegin || slot < 0 || slot >= g_invN) return 0;
    return g_invBegin + (uintptr_t)slot * 0x98;
}

// ===== NBT 树遍历 / 附魔等级读写 =====
static int KeyHas(uintptr_t node, const char* want)
{
    unsigned char k[24] = {0};
    if (vm_read(k, node + 0x20, 24) <= 0) return 0;
    long wl = (long)strlen(want);
    for (int i = 0; i + wl <= 20; i++)
        if (!memcmp(k + i, want, wl)) return 1;
    return 0;
}

static int NodeType(uintptr_t node)
{
    int t = -1;
    if (vm_read(&t, node + 0x60, 4) <= 0) return -1;
    return t;
}

// 在 beginNode 这棵树里找 key，返回节点地址
static uintptr_t FindKey(uintptr_t beginNode, uintptr_t sentinel, const char* want, int maxNodes)
{
    uintptr_t stack[64]; int sp = 0;
    if (!beginNode) return 0;
    stack[sp++] = beginNode;
    int seen = 0;
    while (sp > 0 && seen < maxNodes) {
        uintptr_t n = StripTag(stack[--sp]);
        if (!n || n == sentinel) continue;
        seen++;
        if (KeyHas(n, want)) return n;
        uintptr_t l = 0, r = 0;
        if (vm_read(&l, n, 8) > 0 && l) { l = StripTag(l); if (sp < 64 && l != sentinel) stack[sp++] = l; }
        if (vm_read(&r, n + 8, 8) > 0 && r) { r = StripTag(r); if (sp < 64 && r != sentinel) stack[sp++] = r; }
    }
    return 0;
}

// full scan ASCII lvl, try base offsets, validate type==2 & value
static int ScanSetLvl(int newLvl)
{
    static unsigned char* sb = nullptr;
    const unsigned long CH = 1u << 20;
    if (!sb) sb = (unsigned char*)malloc(CH);
    if (!sb) return -1;
    FILE* mf = fopen("/proc/self/maps", "r");
    if (!mf) return -1;
    char line[512];
    int patched = 0, scanned = 0;
    while (fgets(line, sizeof(line), mf)) {
        unsigned long s = 0, e = 0; char perms[8] = {0}, path[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &s, &e, perms, path);
        if (n < 3 || perms[0] != 'r' || perms[1] != 'w') continue;
        if (s < 0x7000000000UL) continue;
        if (strstr(path, "boot.oat") || strstr(path, "dalvik-cache") || strstr(path, "libart.so")) continue;
        unsigned long sz = e - s;
        for (unsigned long off = 0; off + 16 < sz; off += CH) {
            unsigned long len = sz - off; if (len > CH) len = CH;
            long got = vm_read(sb, s + off, len);
            if (got <= 0) break;
            for (long i = 0; i + 4 < got; i++) {
                if (memcmp(sb + i, "lvl", 3)) continue;
                uintptr_t hit = s + off + (unsigned long)i;
                scanned++;
                for (int d = 0; d <= 3; d++) {
                    uintptr_t node = hit - 0x20 + (uintptr_t)d;
                    int t = -1; vm_read(&t, node + 0x60, 4);
                    if (t != 2) continue;
                    unsigned short v = 0; vm_read(&v, node + 0x40, 2);
                    if (v == 0 || v > 200) continue;
                    MCLOG("[nbt] cand hit=0x%lx d=%d node=0x%lx v=%d", (unsigned long)hit, d, (unsigned long)node, (int)v);
                    if (patched < 4 && (v == 2 || v == 100)) {
                        unsigned short nv = (unsigned short)newLvl;
                        vm_write(nullptr, node + 0x40, &nv, 2);
                        unsigned short ck = 0; vm_read(&ck, node + 0x40, 2);
                        MCLOG("[nbt] PATCHED 0x%lx %d -> %d", (unsigned long)(node + 0x40), (int)v, (int)ck);
                        patched++;
                    }
                }
            }
        }
    }
    fclose(mf);
    MCLOG("[nbt] ScanSetLvl2 scanned=%d patched=%d target=%d", scanned, patched, newLvl);
    return patched;
}

// 诊断：扫堆找 ASCII 串，打印上下文
static int ScanStrCtx(const char* pat)
{
    static unsigned char* sb = nullptr;
    const unsigned long CH = 1u << 20;
    if (!sb) sb = (unsigned char*)malloc(CH);
    if (!sb) return -1;
    long pl = (long)strlen(pat);
    if (pl < 2) return -2;
    FILE* mf = fopen("/proc/self/maps", "r");
    if (!mf) return -1;
    char line[512];
    int hits = 0;
    while (hits < 6 && fgets(line, sizeof(line), mf)) {
        unsigned long s = 0, e = 0; char perms[8] = {0}, path[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &s, &e, perms, path);
        if (n < 3 || perms[0] != 'r' || perms[1] != 'w') continue;
        if (s < 0x7000000000UL) continue;
        if (strstr(path, "boot.oat") || strstr(path, "dalvik-cache") || strstr(path, "libart.so")) continue;
        unsigned long sz = e - s;
        for (unsigned long off = 0; off + 16 < sz; off += CH) {
            unsigned long len = sz - off; if (len > CH) len = CH;
            long got = vm_read(sb, s + off, len);
            if (got <= 0) break;
            for (long i = 0; i + pl < got; i++) {
                if (memcmp(sb + i, pat, pl)) continue;
                uintptr_t hit = s + off + (unsigned long)i;
                MCLOG("[sstr] hit 0x%lx : %02x %02x %02x | %02x %02x %02x", (unsigned long)hit,
                      (int)sb[i-3 > 0 ? i-3 : 0], (int)sb[i-2 > 0 ? i-2 : 0], (int)sb[i-1 > 0 ? i-1 : 0],
                      (int)sb[i+pl], (int)sb[i+pl+1], (int)sb[i+pl+2]);
                char ctx[64] = {0}; int kk = 0;
                for (long q = -16; q < 32; q++) {
                    long idx = i + q;
                    if (idx < 0 || idx >= got) continue;
                    kk += snprintf(ctx + kk, sizeof(ctx) - kk, "%02x", sb[idx]);
                    if (kk > 55) break;
                }
                MCLOG("[sstr]   ctx@-16 %s", ctx);
                hits++;
                if (hits >= 6) break;
            }
        }
    }
    fclose(mf);
    MCLOG("[sstr] pat=%s hits=%d", pat, hits);
    return hits;
}

#define VT_ITEM 0x137E6C38u

#define VT_ITEM 0x137E6C38u

static uintptr_t ItemFromStrPtr(uintptr_t strAddr)
{
    // 在字符串地址 ±2MB 内找指向它的 qword，再用 Item vptr 回推对象基址
    const unsigned long CH = 1u << 20;
    static unsigned char* sb = nullptr;
    if (!sb) sb = (unsigned char*)malloc(CH);
    if (!sb) return 0;
    uintptr_t vtItem = g_libBase + VT_ITEM;
    for (long delta = -0x200000; delta <= 0x200000; delta += (long)CH - 0x1000) {
        uintptr_t a0 = strAddr + (uintptr_t)(delta < 0 ? 0 : delta);
        if (delta < 0) a0 = strAddr - (uintptr_t)(-delta);
        long got = vm_read(sb, a0, CH);
        if (got <= 0) continue;
        for (long i = 0; i + 8 < got; i += 8) {
            uintptr_t v; memcpy(&v, sb + i, 8);
            if (StripTag(v) != strAddr) continue;
            uintptr_t P = a0 + (uintptr_t)i;
            // 验证 std::string {ptr,size,cap}
            uintptr_t sizev = 0, capv = 0;
            vm_read(&sizev, P + 8, 8); vm_read(&capv, P + 16, 8);
            MCLOG("[make] ref@0x%lx size=%lu cap=%lu", (unsigned long)P, (unsigned long)sizev, (unsigned long)capv);
            for (int k = 0; k <= 0x100; k += 8) {
                uintptr_t base = P - (uintptr_t)k;
                uintptr_t vv = 0;
                if (vm_read(&vv, base, 8) <= 0) continue;
                if (vv == vtItem) {
                    MCLOG("[make] ITEM=0x%lx (field off 0x%x)", (unsigned long)base, k);
                    return base;
                }
            }
        }
    }
    return 0;
}

static uintptr_t FindItemByName(const char* name)
{
    static unsigned char* sb = nullptr;
    const unsigned long CH = 1u << 20;
    if (!sb) sb = (unsigned char*)malloc(CH);
    if (!sb) return 0;
    long pl = (long)strlen(name);
    if (pl < 4) return 0;
    uintptr_t vtItem = g_libBase + VT_ITEM;
    FILE* mf = fopen("/proc/self/maps", "r");
    if (!mf) return 0;
    char line[512];
    uintptr_t found = 0; int tried = 0;
    while (!found && fgets(line, sizeof(line), mf)) {
        unsigned long s = 0, e = 0; char perms[8] = {0}, path[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &s, &e, perms, path);
        if (n < 3 || perms[0] != 'r' || perms[1] != 'w') continue;
        if (s < 0x7000000000UL) continue;
        if (strstr(path, "boot.oat") || strstr(path, "dalvik-cache") || strstr(path, "libart.so")) continue;
        unsigned long sz = e - s;
        for (unsigned long off = 0; !found && off + 16 < sz; off += CH) {
            unsigned long len = sz - off; if (len > CH) len = CH;
            long got = vm_read(sb, s + off, len);
            if (got <= 0) break;
            for (long i = 0; i + pl < got; i++) {
                if (memcmp(sb + i, name, pl)) continue;
                uintptr_t hitAddr = s + off + (unsigned long)i;
                for (int o = 0; o <= 0x100; o += 8) {   // 第一轮：内联 SSO
                    uintptr_t v = 0;
                    if (vm_read(&v, hitAddr - (uintptr_t)o, 8) <= 0) continue;
                    if (v == vtItem) { found = hitAddr - (uintptr_t)o; break; }
                }
                if (found) break;
                if (tried < 6) {
                    tried++;
                    found = ItemFromStrPtr(hitAddr);
                    if (found) break;
                }
            }
        }
    }
    fclose(mf);
    MCLOG("[make] name=%s item=0x%lx", name, (unsigned long)found);
    return found;
}

static int MakeItem(int slot, const char* name)
{
    uintptr_t item = FindItemByName(name);
    if (!item) return 0;
    uintptr_t p = SlotAddr(slot);
    if (!p) { MCLOG("[make] slot %d out of range", slot); return 0; }
    uint64_t old[4] = {0,0,0,0};
    vm_read(old, p, 32);
    MCLOG("[make] slot%d old: vt=0x%lx item=0x%lx tag=0x%lx blk=0x%lx", slot,
          (unsigned long)old[0], (unsigned long)old[1], (unsigned long)old[2], (unsigned long)old[3]);
    uintptr_t vtIS = g_libBase + VT_ITEMSTACK;
    vm_write(nullptr, p + 0x00, &vtIS, 8);
    vm_write(nullptr, p + 0x08, &item, 8);
    uint64_t zero = 0;
    vm_write(nullptr, p + 0x10, &zero, 8);   // tag
    vm_write(nullptr, p + 0x18, &zero, 8);   // block
    unsigned char cnt = 1, one = 1;
    vm_write(nullptr, p + 0x22, &cnt, 1);
    vm_write(nullptr, p + 0x23, &one, 1);
    uint8_t b[0x98];
    if (vm_read(b, p, 0x98) <= 0) { MCLOG("[make] readback failed"); return 0; }
    MCLOG("[make] slot%d now vt=0x%lx cnt=%d item=0x%lx", slot, (unsigned long)*(uintptr_t*)b, (int)b[0x22], (unsigned long)*(uintptr_t*)(b+8));
    return 1;
}

// 整格复制：物品指针 + 方块指针 + 数量 + NBT深拷贝
static int CopyStack(int dst, int src)
{
    uintptr_t pd = SlotAddr(dst);
    uintptr_t ps = SlotAddr(src);
    if (!pd || !ps) { MCLOG("[copy] slot range err d=%d s=%d", dst, src); return 0; }
    uint64_t item = 0, blk = 0, tagRaw = 0;
    vm_read(&item, ps + 0x08, 8);
    vm_read(&blk, ps + 0x18, 8);
    vm_read(&tagRaw, ps + 0x10, 8);
    unsigned char cnt = 0; vm_read(&cnt, ps + 0x22, 1);
    MCLOG("[copy] src%d item=0x%lx cnt=%d blk=0x%lx tag=0x%lx", src, (unsigned long)item, (int)cnt, (unsigned long)blk, (unsigned long)tagRaw);
    if (!item) { MCLOG("[copy] src empty"); return 0; }
    uintptr_t vtIS = g_libBase + VT_ITEMSTACK;
    vm_write(nullptr, pd + 0x00, &vtIS, 8);
    vm_write(nullptr, pd + 0x08, &item, 8);
    vm_write(nullptr, pd + 0x18, &blk, 8);
    uintptr_t nt = 0;
    if (tagRaw) {
        g_guard = 1;
        if (sigsetjmp(g_jb, 1) == 0) { nt = CallClone(g_libBase + OFF_CLONE, (uintptr_t)tagRaw); g_guard = 0; }
        else { g_guard = 0; nt = 0; MCLOG("[copy] clone FAULTED"); }
    }
    uint64_t tagw = (uint64_t)nt;
    vm_write(nullptr, pd + 0x10, &tagw, 8);
    vm_write(nullptr, pd + 0x22, &cnt, 1);
    unsigned char one = 1; vm_write(nullptr, pd + 0x23, &one, 1);
    uint64_t ri = 0, rt = 0; unsigned char rc = 0;
    vm_read(&ri, pd + 0x08, 8); vm_read(&rt, pd + 0x10, 8); vm_read(&rc, pd + 0x22, 1);
    MCLOG("[copy] dst%d item=0x%lx cnt=%d tag=0x%lx", dst, (unsigned long)ri, (int)rc, (unsigned long)rt);
    return 1;
}

// 整格 0x98 全复制 + 新唯一 id（避免“影子物品”拿不起来）
static int CopyStackFull(int dst, int src)
{
    uintptr_t pd = SlotAddr(dst);
    uintptr_t ps = SlotAddr(src);
    if (!pd || !ps) { MCLOG("[copy] range err d=%d s=%d", dst, src); return 0; }
    unsigned char buf[0x98];
    if (vm_read(buf, ps, 0x98) <= 0) { MCLOG("[copy] read src fail"); return 0; }
    uint64_t item = *(uint64_t*)(buf + 0x08);
    unsigned char cnt = buf[0x22];
    uint64_t tagRaw = *(uint64_t*)(buf + 0x10);
    uint64_t uid = *(uint64_t*)(buf + 0x28);
    int seq = *(int*)(buf + 0x80);
    MCLOG("[copy] src%d base=0x%lx item=0x%lx cnt=%d uid=0x%lx seq=%d", src, (unsigned long)ps, (unsigned long)item, (int)cnt, (unsigned long)uid, seq);
    if (!item) return 0;
    // 先整格复制（此时 tag 指针是共享的，下面立刻换掉）
    if (vm_write(nullptr, pd, buf, 0x98) <= 0) { MCLOG("[copy] write fail"); return 0; }
    uintptr_t nt = 0;
    if (tagRaw) {
        g_guard = 1;
        if (sigsetjmp(g_jb, 1) == 0) { nt = CallClone(g_libBase + OFF_CLONE, (uintptr_t)tagRaw); g_guard = 0; }
        else { g_guard = 0; nt = 0; MCLOG("[copy] clone FAULTED"); }
    }
    uint64_t tagw = (uint64_t)nt;
    vm_write(nullptr, pd + 0x10, &tagw, 8);
    // 新唯一 id：源 uid 改一个高位，保证不重复
    uint64_t nid = uid ? (uid ^ 0x0100000000000000ULL) : 0x5A5A000000000001ULL;
    if (uid) nid = uid + 0x0001000000000001ULL;
    vm_write(nullptr, pd + 0x28, &nid, 8);
    int nseq = seq + 1;
    vm_write(nullptr, pd + 0x80, &nseq, 4);
    unsigned char b2[0x98];
    vm_read(b2, pd, 0x98);
    MCLOG("[copy] dst%d base=0x%lx item=0x%lx cnt=%d uid=0x%lx seq=%d tag=0x%lx", dst, (unsigned long)pd,
          (unsigned long)*(uint64_t*)(b2 + 8), (int)b2[0x22], (unsigned long)*(uint64_t*)(b2 + 0x28), *(int*)(b2 + 0x80), (unsigned long)*(uint64_t*)(b2 + 0x10));
    return 1;
}

static int SetEnchLevel(int slot, int newLvl, int onlyIndex)
{
    uintptr_t p = SlotAddr(slot);
    if (!p) { MCLOG("[nbt] slot %d out of range", slot); return 0; }
    uint64_t tagRaw = 0; vm_read(&tagRaw, p + 0x10, 8);
    if (!tagRaw) { MCLOG("[nbt] slot%d no tag", slot); return 0; }
    uintptr_t tag = StripTag((uintptr_t)tagRaw);
    uintptr_t sentinel = tag + 0x10;
    uintptr_t root = 0; vm_read(&root, tag + 0x10, 8);
    root = StripTag(root);
    if (!root) { vm_read(&root, tag + 0x08, 8); root = StripTag(root); }
    MCLOG("[nbt] slot%d tag=0x%lx root=0x%lx", slot, (unsigned long)tag, (unsigned long)root);
    uintptr_t enchNode = FindKey(root, sentinel, "ench", 64);
    if (!enchNode) { MCLOG("[nbt] no ench node"); return 0; }
    int et = NodeType(enchNode);
    MCLOG("[nbt] ench node=0x%lx type=%d", (unsigned long)enchNode, et);
    uintptr_t lb = 0, le = 0;
    vm_read(&lb, enchNode + 0x38 + 0x08, 8);
    vm_read(&le, enchNode + 0x38 + 0x10, 8);
    lb = StripTag(lb); le = StripTag(le);
    int n = (le > lb) ? (int)((le - lb) / 8) : 0;
    MCLOG("[nbt] ench list begin=0x%lx n=%d", (unsigned long)lb, n);
    int done = 0;
    for (int i = 0; i < n && i < 8; i++) {
        if (onlyIndex >= 0 && i != onlyIndex) continue;
        uintptr_t ep = 0; vm_read(&ep, lb + (uintptr_t)i * 8, 8);
        ep = StripTag(ep);
        if (!ep) continue;
        uintptr_t esent = ep + 0x10;
        uintptr_t eroot = 0; vm_read(&eroot, ep + 0x10, 8);
        eroot = StripTag(eroot);
        if (!eroot) { vm_read(&eroot, ep + 0x08, 8); eroot = StripTag(eroot); }
        uintptr_t idNode = FindKey(eroot, esent, "id", 32);
        uintptr_t lvNode = FindKey(eroot, esent, "lvl", 32);
        unsigned short idv = 0, lvOld = 0;
        if (idNode) vm_read(&idv, idNode + 0x40, 2);
        if (lvNode) vm_read(&lvOld, lvNode + 0x40, 2);
        MCLOG("[nbt] elem%d id=%d lvl=%d lvNode=0x%lx", i, (int)idv, (int)lvOld, (unsigned long)lvNode);
        if (lvNode && newLvl > 0) {
            unsigned short nv = (unsigned short)newLvl;
            vm_write(nullptr, lvNode + 0x40, &nv, 2);
            unsigned short ck = 0; vm_read(&ck, lvNode + 0x40, 2);
            MCLOG("[nbt] PATCH lvl %d -> %d (rb=%d)", (int)lvOld, newLvl, (int)ck);
            done++;
        }
    }
    return done;
}


static bool IsItemPtr(uintptr_t v)
{
    if (!v) return false;
    if (v < 0x7000000000UL || v > 0x8000000000UL) return false;
    uint64_t vt = 0;
    if (vm_read(&vt, v, 8) <= 0) return false;
    return (vt >> 56) == 0xb4ULL;
}

static int ItemIdFromSlot(int slot)
{
    if (!g_invBegin) FindInventoryNow();
    uintptr_t item = 0;
    for (int q = 0; q < g_invCntFound; q++) {
        if (slot < 0 || slot >= g_invListN[q]) continue;
        uintptr_t p = g_invList[q] + (uintptr_t)slot * 0x98;
        uint64_t v = 0;
        if (vm_read(&v, p + 0x08, 8) <= 0) continue;
        uintptr_t it = StripTag(v);
        if (it) { item = it; break; }
    }
    if (!item) { MCLOG("[iid] slot%d no item", slot); return -1; }
    const unsigned long CH = 1u << 20;
    static unsigned char* sb = nullptr;
    if (!sb) sb = (unsigned char*)malloc(CH);
    if (!sb) return -1;
    FILE* mf = fopen("/proc/self/maps", "r");
    if (!mf) return -1;
    char line[512];
    int hits = 0, bestId = -1, bestRun = 0;
    MCLOG("[iid] slot=%d item=0x%lx", slot, (unsigned long)item);
    while (hits < 6 && fgets(line, sizeof(line), mf)) {
        unsigned long s = 0, e = 0; char perms[8] = {0}, path[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &s, &e, perms, path);
        if (n < 3 || perms[0] != 'r' || perms[1] != 'w') continue;
        if (s < 0x7000000000UL) continue;
        if (strstr(path, "boot.oat") || strstr(path, "dalvik-cache") || strstr(path, "libart.so")) continue;
        unsigned long sz = e - s;
        for (unsigned long off = 0; off + 16 < sz; off += CH) {
            unsigned long len = sz - off; if (len > CH) len = CH;
            long got = vm_read(sb, s + off, len);
            if (got <= 0) break;
            for (long i = 0; i + 8 <= got; i += 8) {
                uintptr_t v; memcpy(&v, sb + i, 8);
                if (StripTag(v) != item) continue;
                uintptr_t h = s + off + (unsigned long)i;
                int L = 0, R = 0;
                for (int k = 1; k <= 512; k++) {
                    uint64_t q = 0;
                    if (vm_read(&q, h - (uintptr_t)(k * 8), 8) <= 0 || !IsItemPtr(StripTag(q))) break;
                    L++;
                }
                for (int k = 1; k <= 512; k++) {
                    uint64_t q = 0;
                    if (vm_read(&q, h + (uintptr_t)(k * 8), 8) <= 0 || !IsItemPtr(StripTag(q))) break;
                    R++;
                }
                MCLOG("[iid]   hit=0x%lx L=%d R=%d", (unsigned long)h, L, R);
                if (L + R >= 16 && L + R > bestRun) { bestRun = L + R; bestId = L; }
                hits++;
                if (hits >= 6) break;
            }
        }
    }
    fclose(mf);
    MCLOG("[iid] best: idx=%d run=%d", bestId, bestRun);
    return bestId;
}


static int FindPtrRefs(uintptr_t val, int maxHits)
{
    static unsigned char* sb = nullptr;
    const unsigned long CH = 1u << 20;
    if (!sb) sb = (unsigned char*)malloc(CH);
    if (!sb) return 0;
    FILE* mf = fopen("/proc/self/maps", "r");
    if (!mf) return 0;
    char line[512];
    int hits = 0;
    while (fgets(line, sizeof(line), mf)) {
        unsigned long s = 0, e = 0; char perms[8] = {0}, path[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &s, &e, perms, path);
        if (n < 3 || perms[0] != 'r' || perms[1] != 'w') continue;
        if (s < 0x7000000000UL) continue;
        if (strstr(path, "boot.oat") || strstr(path, "dalvik-cache") || strstr(path, "libart.so")) continue;
        unsigned long sz = e - s;
        for (unsigned long off = 0; off + 16 < sz; off += CH) {
            unsigned long len = sz - off; if (len > CH) len = CH;
            long got = vm_read(sb, s + off, len);
            if (got <= 0) break;
            for (long i = 0; i + 8 <= got; i += 8) {
                uint64_t v; memcpy(&v, sb + i, 8);
                if ((uintptr_t)(v & 0x00ffffffffffffffULL) != val) continue;
                uintptr_t hit = s + off + (unsigned long)i;
                MCLOG("[fref] hit=0x%lx raw=0x%llx", (unsigned long)hit, (unsigned long long)v);
                hits++;
                if (hits >= maxHits) { fclose(mf); return hits; }
            }
        }
    }
    fclose(mf);
    return hits;
}


static uintptr_t GetMcBase()
{
    static uintptr_t cached = 0;
    if (cached) return cached;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "libminecraftpe.so")) continue;
        unsigned long s = 0, o = 0;
        if (sscanf(line, "%lx-%*lx %*s %lx", &s, &o) == 2 && o == 0) { cached = (uintptr_t)s; break; }
    }
    fclose(f);
    return cached;
}

static int CallGetItemId(uintptr_t stackPtr)
{
    uintptr_t base = GetMcBase();
    if (!base || !stackPtr) return -1;
    typedef int (*Fn)(void*);
    Fn fn = (Fn)(base + 0x10777a50);
    return fn((void*)stackPtr);
}


static uintptr_t g_vtItem = 0;

static int CallItemLegacyId(uintptr_t itemObj, int aux)
{
    (void)aux;
    if (!itemObj) return -1;
    uintptr_t base = GetMcBase();
    if (!base) return -1;
    static unsigned char tmp[0x100] __attribute__((aligned(16)));
    memset(tmp, 0, sizeof tmp);
    *(uintptr_t*)(tmp + 0x00) = base + VT_ITEMSTACK;
    *(uintptr_t*)(tmp + 0x08) = itemObj;
    unsigned char one = 1; *(unsigned char*)(tmp + 0x22) = one; *(unsigned char*)(tmp + 0x23) = one;
    typedef int (*Fn)(void*);
    Fn fn = (Fn)(base + 0x10777a50);
    return fn((void*)tmp);
}

static uintptr_t HeldItemObj(int* outSlot)
{
    int slot = -1;
    if (vm_read(&slot, (uintptr_t)g_piObj + 0x10, 4) <= 0) return 0;
    if (outSlot) *outSlot = slot;
    uintptr_t p = SlotAddr(slot);
    if (!p) return 0;
    uintptr_t it = 0;
    if (vm_read(&it, p + 0x08, 8) <= 0) return 0;
    return StripTag(it);
}

static int MakeItemFromPtr(int slot, uintptr_t item, int count)
{
    if (!item) return 0;
    uintptr_t p = SlotAddr(slot);
    if (!p) return 0;
    uintptr_t vtIS = g_libBase + VT_ITEMSTACK;
    vm_write(nullptr, p + 0x00, &vtIS, 8);
    vm_write(nullptr, p + 0x08, &item, 8);
    uint64_t zero = 0;
    vm_write(nullptr, p + 0x10, &zero, 8);
    vm_write(nullptr, p + 0x18, &zero, 8);
    unsigned short aux = 0;
    vm_write(nullptr, p + 0x20, &aux, 2);
    unsigned char cnt = (count < 1) ? 1 : (count > 255 ? 255 : count), one = 1;
    vm_write(nullptr, p + 0x22, &cnt, 1);
    vm_write(nullptr, p + 0x23, &one, 1);
    return 1;
}

static int MakeItemFromPtrAt(uintptr_t p, uintptr_t item, int count)
{
    if (!item || !p) return 0;
    uintptr_t vtIS = g_libBase + VT_ITEMSTACK;
    vm_write(nullptr, p + 0x00, &vtIS, 8);
    vm_write(nullptr, p + 0x08, &item, 8);
    uint64_t zero = 0;
    vm_write(nullptr, p + 0x10, &zero, 8);
    vm_write(nullptr, p + 0x18, &zero, 8);
    unsigned short aux = 0;
    vm_write(nullptr, p + 0x20, &aux, 2);
    unsigned char cnt = (count < 1) ? 1 : (count > 255 ? 255 : count), one = 1;
    vm_write(nullptr, p + 0x22, &cnt, 1);
    vm_write(nullptr, p + 0x23, &one, 1);
    return 1;
}

static int BuildIdTable()
{
    int hs = -1;
    uintptr_t heldItem = HeldItemObj(&hs);
    if (!heldItem) { MCLOG("[idtb] no held item"); return 0; }
    vm_read(&g_vtItem, heldItem, 8);
    MCLOG("[idtb] heldItem=0x%lx", (unsigned long)heldItem);
    MCLOG("[idtb] vtItem=0x%lx", (unsigned long)g_vtItem);
    static unsigned char* sb = nullptr;
    const unsigned long CH = 1u << 20;
    if (!sb) sb = (unsigned char*)malloc(CH);
    if (!sb) return 0;
    FILE* mf = fopen("/proc/self/maps", "r");
    FILE* of = fopen("/sdcard/mcmod_idtbl.txt", "w");
    int cnt = 0, dup = 0;
    char line[512];
    while (mf && sb && fgets(line, sizeof(line), mf)) {
        unsigned long s0 = 0, e0 = 0; char perms[8] = {0}, path[256] = {0};
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &s0, &e0, perms, path) < 3) continue;
        if (perms[0] != 'r') continue;
        if (s0 < 0x7000000000UL) continue;
        if (strstr(path, "libminecraftpe")) continue;
        unsigned long sz = e0 - s0;
        for (unsigned long off = 0; off + 16 < sz; off += CH) {
            unsigned long len = sz - off; if (len > CH) len = CH;
            long got = vm_read(sb, s0 + off, len);
            if (got <= 0) break;
            for (long i = 0; i + 8 <= got; i += 8) {
                uint64_t v; memcpy(&v, sb + i, 8);
                if ((v & 0x00ffffffffffffffULL) != (g_vtItem & 0x00ffffffffffffffULL)) continue;
                uintptr_t cand = s0 + off + (unsigned long)i;
                if (cnt < 3) MCLOG("[idtb] cand=0x%lx", (unsigned long)cand);
                if (cnt >= 1200) break;
                int id = CallItemLegacyId(cand, 0);
                if (cnt < 10) MCLOG("[idtb] try cand=0x%lx id=%d", (unsigned long)cand, id);
                if (id > 0xFFFF) id = id >> 16;
                if (id <= 0 || id > 0xFFFF) continue;
                cnt++;
                if (of) fprintf(of, "%d 0x%lx\n", id, (unsigned long)cand);
            }
        }
    }
    if (mf) fclose(mf);
    if (of) fclose(of);
    MCLOG("[idtb] total=%d", cnt);
    return cnt;
}


static int IsPlausibleVt(uintptr_t vp)
{
    vp &= 0x00ffffffffffffffULL;
    if (vp < 0x7000000000ULL) return 0;
    uint64_t q0 = 0, q1 = 0;
    if (vm_read(&q0, vp + 0x10, 8) <= 0) return 0;
    if (vm_read(&q1, vp + 0x18, 8) <= 0) return 0;
    if (!q0 || !q1) return 0;
    return 1;
}

static int BuildItemMap()
{
    FILE* mf = fopen("/proc/self/maps", "r");
    FILE* of = fopen("/sdcard/mcmod_itemmap.txt", "w");
    int cnt = 0;
    char line[512];
    static unsigned char* nb = nullptr;
    const unsigned long CH = 1u << 20;
    if (!nb) nb = (unsigned char*)malloc(CH);
    while (mf && nb && fgets(line, sizeof(line), mf)) {
        unsigned long s0 = 0, e0 = 0; char perms[8] = {0}, path[256] = {0};
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &s0, &e0, perms, path) < 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w') continue;
        if (s0 < 0x7000000000UL) continue;
        unsigned long sz = e0 - s0;
        for (unsigned long off = 0; off + 16 < sz; off += CH) {
            unsigned long len = sz - off; if (len > CH) len = CH;
            long got = vm_read(nb, s0 + off, len);
            if (got <= 0) break;
            for (long i = 10; i + 12 < got; i++) {
                if (memcmp(nb + i, "minecraft:", 10) != 0) continue;
                long st = i - 1;
                unsigned char ln = nb[st];
                if (ln < 11 || ln > 64 || st + 1 + ln > got) continue;
                unsigned char* ns = nb + st + 1;
                int ok = 1, hasUpper = 0;
                for (int k = 0; k < (int)ln; k++) {
                    if (ns[k] < 32 || ns[k] > 126) { ok = 0; break; }
                    if (ns[k] >= 'A' && ns[k] <= 'Z') hasUpper = 1;
                }
                if (!ok || hasUpper) continue;
                uintptr_t nameAddr = s0 + off + (unsigned long)st;
                long endi = st + 1 + ln;
                uintptr_t foundItem = 0;
                for (int d = 0; d <= 0x48; d += 8) {
                    if (endi + d + 8 > got) break;
                    uint64_t p; memcpy(&p, nb + endi + d, 8);
                    uintptr_t pu = (uintptr_t)(p & 0x00ffffffffffffffULL);
                    if (pu < 0x7000000000ULL) continue;
                    uint64_t vp = 0;
                    if (vm_read(&vp, pu, 8) <= 0) continue;
                    if (!IsPlausibleVt((uintptr_t)vp)) continue;
                    foundItem = pu;
                    break;
                }
                if (!foundItem) continue;
                int id = CallItemLegacyId(foundItem, 0);
                if (id > 0xFFFF) id = id >> 16;
                if (id <= 0) continue;
                if (cnt < 5000) {
                    if (of) fprintf(of, "%d %.*s 0x%lx 0x%lx\n", id, (int)ln, (const char*)ns, (unsigned long)nameAddr, (unsigned long)foundItem);
                    cnt++;
                }
            }
        }
    }
    if (mf) fclose(mf);
    if (of) fclose(of);
    MCLOG("[idmap] total=%d", cnt);
    return cnt;
}


#define CLIPMAX 12
static unsigned char g_clip[CLIPMAX][0x98];
static unsigned char g_tpl[0x98];
static int    g_tplOk = 0;
static int    g_onlyNbt = 0;
static int    g_clipNbt[CLIPMAX];
static int    g_clipN = 0;
static int    g_clipSel = 0;
static int    g_clipCntV[CLIPMAX] = {0};
static int    g_clipIdV[CLIPMAX];

static uint64_t rd64v(uintptr_t a);

static int IsNameBlob(uintptr_t p, char* out, int cap)
{
    unsigned char b[96];
    if (!p || p < 0x100000ULL) return 0;
    if (vm_read(b, p, 96) <= 0) return 0;
    int len = (int)b[0];
    if (len < 4 || len > 70) return 0;
    if (memcmp(b + 1, "minecraft:", 10) != 0) return 0;
    if (len >= cap) len = cap - 1;
    memcpy(out, b + 1, (size_t)len); out[len] = 0;
    return 1;
}

static int ItemNameOf(uintptr_t item, char* out, int cap)
{
    out[0] = 0;
    if (!item) return 0;
    for (int off = 0; off <= 0x200; off += 8) {
        uintptr_t w = rd64v(item + (uintptr_t)off);
        uintptr_t p = StripTag(w);
        if (p < 0x100000ULL) continue;
        if (IsNameBlob(p, out, cap)) {
            MCLOG("[iname] direct off=0x%x name=%s", off, out);
            return 1;
        }
        uintptr_t q = StripTag(rd64v(p));
        if (q > 0x100000ULL && IsNameBlob(q, out, cap)) {
            MCLOG("[iname] indirect off=0x%x name=%s", off, out);
            return 1;
        }
        uintptr_t sz = rd64v(p + 8);
        if (sz > 4 && sz < 70) {
            unsigned char tb[80];
            if (vm_read(tb, p + 16, 80) > 0 && tb[0] >= 4 && tb[0] <= 70 &&
                memcmp(tb + 1, "minecraft:", 10) == 0) {
                memcpy(out, tb + 1, (size_t)tb[0]); out[tb[0]] = 0;
                MCLOG("[iname] strmem off=0x%x name=%s", off, out);
                return 1;
            }
        }
    }
    MCLOG("[iname] not found item=0x%lx", (unsigned long)item);
    return 0;
}

static int TplSave(void)
{
    int held = -1; HeldItemObj(&held);
    uintptr_t p = SlotAddr(held);
    if (!p) return 0;
    unsigned char buf[0x98];
    if (vm_read(buf, p, 0x98) <= 0) return 0;
    if (!*(uint64_t*)(buf + 0x08)) { MCLOG("[tpl] hand empty"); return 0; }
    uintptr_t tagRaw = *(uintptr_t*)(buf + 0x10);
    uintptr_t nt = 0;
    if (tagRaw) {
        g_guard = 1;
        if (sigsetjmp(g_jb, 1) == 0) { nt = CallClone(g_libBase + OFF_CLONE, tagRaw); g_guard = 0; }
        else { g_guard = 0; nt = 0; MCLOG("[tpl] clone fault"); }
    }
    if (!nt) { MCLOG("[tpl] no nbt in hand"); return 0; }
    memcpy(g_tpl, buf, 0x98);
    *(uintptr_t*)(g_tpl + 0x10) = nt;
    g_tplOk = 1;
    MCLOG("[tpl] saved tag=0x%lx", (unsigned long)nt);
    return 1;
}

static int TplApplyStack(uintptr_t p)
{
    if (!g_tplOk) return 0;
    uintptr_t tagRaw = *(uintptr_t*)(g_tpl + 0x10);
    uintptr_t nt = 0;
    if (tagRaw) {
        g_guard = 1;
        if (sigsetjmp(g_jb, 1) == 0) { nt = CallClone(g_libBase + OFF_CLONE, tagRaw); g_guard = 0; }
        else { g_guard = 0; nt = 0; MCLOG("[tpl] apply clone fault"); }
    }
    if (!nt) return 0;
    uint64_t tw = (uint64_t)nt;
    vm_write(nullptr, p + 0x10, &tw, 8);
    MCLOG("[tpl] applied -> 0x%lx tag=0x%lx", (unsigned long)p, (unsigned long)nt);
    return 1;
}

static int ClipCopy(void)
{
    int held = -1; HeldItemObj(&held);
    uintptr_t p = SlotAddr(held);
    if (!p) { MCLOG("[clip] no slot"); return 0; }
    unsigned char buf[0x98];
    if (vm_read(buf, p, 0x98) <= 0) return 0;
    if (!*(uint64_t*)(buf + 0x08)) { MCLOG("[clip] hand empty"); return 0; }
    if (g_clipN >= CLIPMAX) { MCLOG("[clip] full"); return 0; }
    if (g_onlyNbt) {
        memset(g_clip[g_clipN], 0, 0x98);
        g_clipNbt[g_clipN] = 1;
    } else {
        memcpy(g_clip[g_clipN], buf, 0x98);
        g_clipNbt[g_clipN] = 0;
    }
    uintptr_t tagRaw = *(uintptr_t*)(buf + 0x10);
    uintptr_t ntag = 0;
    if (tagRaw) {
        g_guard = 1;
        if (sigsetjmp(g_jb, 1) == 0) { ntag = CallClone(g_libBase + OFF_CLONE, (uintptr_t)tagRaw); g_guard = 0; }
        else { g_guard = 0; ntag = 0; MCLOG("[clip] clone fault"); }
    }
    *(uintptr_t*)(g_clip[g_clipN] + 0x10) = ntag;
    g_clipCntV[g_clipN] = buf[0x22];
    int packed = CallGetItemId(p);
    g_clipIdV[g_clipN] = (packed > 0) ? (packed >> 16) : -1;
    MCLOG("[clip] saved #%d id=%d cnt=%d tag_raw=0x%lx clone=0x%lx", g_clipN, g_clipIdV[g_clipN], g_clipCntV[g_clipN], (unsigned long)tagRaw, (unsigned long)ntag);
    g_clipN++;
    if (g_clipN == 1) g_clipSel = 0;
    return 1;
}

static int ClipPasteTo(uintptr_t p)
{
    if (g_clipN <= 0 || g_clipSel < 0 || g_clipSel >= g_clipN) { MCLOG("[clip] nothing"); return 0; }
    if (!p) return 0;
    unsigned char buf[0x98];
    memcpy(buf, g_clip[g_clipSel], 0x98);
    int nbtOnly = g_clipNbt[g_clipSel];
    if (nbtOnly) {
        unsigned char cur[0x98];
        if (vm_read(cur, p, 0x98) <= 0) return 0;
        if (!*(uint64_t*)(cur + 0x08)) { MCLOG("[clip] target empty, skip nbt-only"); return 0; }
    } else {
        if (vm_write(nullptr, p, buf, 0x98) <= 0) { MCLOG("[clip] write fail"); return 0; }
    }
    uintptr_t tagRaw = *(uintptr_t*)(buf + 0x10);
    uintptr_t ntag = 0;
    if (tagRaw) {
        g_guard = 1;
        if (sigsetjmp(g_jb, 1) == 0) { ntag = CallClone(g_libBase + OFF_CLONE, (uintptr_t)tagRaw); g_guard = 0; }
        else { g_guard = 0; ntag = 0; MCLOG("[clip] paste clone fault"); }
    }
    uint64_t tagw = (uint64_t)ntag;
    vm_write(nullptr, p + 0x10, &tagw, 8);
    if (nbtOnly) { MCLOG("[clip] nbt-only applied -> 0x%lx tag=0x%lx", (unsigned long)p, (unsigned long)ntag); return 1; }
    static unsigned long seqc = 0;
    uint64_t nid = 0x5A5A000000000000ULL | (++seqc & 0xFFFFFF);
    vm_write(nullptr, p + 0x28, &nid, 8);
    unsigned char rb[0x98];
    if (vm_read(rb, p, 0x98) > 0) {
        MCLOG("[clip] pasted #%d -> 0x%lx item=0x%lx blk=0x%lx cnt=%d tag=0x%lx",
              g_clipSel, (unsigned long)p, (unsigned long)*(uint64_t*)(rb + 8),
              (unsigned long)*(uint64_t*)(rb + 0x18), (int)rb[0x22], (unsigned long)*(uint64_t*)(rb + 0x10));
    }
    return 1;
}


// ================= 原生 NBT 构造 + JSON 导入 =================
#define NB_VT_BYTE    0x13EBE698u
#define NB_VT_SHORT   0x13EBEBE0u
#define NB_VT_INT     0x13EBE938u
#define NB_VT_LONG    0x13EBE858u
#define NB_VT_FLOAT   0x13EBE7E8u
#define NB_VT_DOUBLE  0x13EBE778u
#define NB_VT_STRING  0x13EBEC50u
#define NB_VT_LIST    0x13EBE9A8u
#define NB_VT_COMP    0x13EBE708u
#define NB_FN_NEW     0x135D3F30u
#define NB_FN_STRINIT 0xE831AFCu
#define NB_FN_PUT     0x11BA0E40u

static uintptr_t NbNewObj(size_t n)
{
    static uintptr_t fn = 0;
    if (!fn) { if (!g_libBase) g_libBase = FindLibBase(kLib); if (!g_libBase) return 0; fn = g_libBase + NB_FN_NEW; }
    typedef void* (*NF)(size_t);
    return (uintptr_t)((NF)fn)(n);
}

static void NbStrSet(void* dst, const char* s, unsigned long n)
{
    unsigned char* d = (unsigned char*)dst;
    memset(d, 0, 24);
    if (n <= 22) {
        d[0] = (unsigned char)(n << 1);
        if (n) memcpy(d + 1, s, n);
        return;
    }
    static uintptr_t fn = 0;
    if (!fn) { if (!g_libBase) g_libBase = FindLibBase(kLib); if (!g_libBase) return; fn = g_libBase + NB_FN_STRINIT; }
    typedef void (*SF)(void*, const char*, unsigned long);
    ((SF)fn)(dst, s, n);
}

static uintptr_t NbNum(uint32_t vt, const void* v, int sz)
{
    uintptr_t p = NbNewObj(0x10);
    if (!p) return 0;
    *(uintptr_t*)p = g_libBase + vt;
    memcpy((void*)(p + 8), v, sz);
    return p;
}

static uintptr_t NbString(const char* s, unsigned long n)
{
    uintptr_t p = NbNewObj(0x20);
    if (!p) return 0;
    *(uintptr_t*)p = g_libBase + NB_VT_STRING;
    NbStrSet((void*)(p + 8), s, n);
    return p;
}

static uintptr_t NbCompound(void)
{
    uintptr_t p = NbNewObj(0x20);
    if (!p) return 0;
    *(uintptr_t*)p = g_libBase + NB_VT_COMP;
    *(uintptr_t*)(p + 0x08) = p + 0x10;
    *(uintptr_t*)(p + 0x10) = 0;
    *(uintptr_t*)(p + 0x18) = 0;
    return p;
}

static uintptr_t NbList(int elemType, const uintptr_t* el, int n)
{
    uintptr_t p = NbNewObj(0x28);
    if (!p) return 0;
    *(uintptr_t*)p = g_libBase + NB_VT_LIST;
    uintptr_t buf = 0;
    if (n > 0) {
        buf = NbNewObj((size_t)n * 8);
        if (!buf) return 0;
        memcpy((void*)buf, el, (size_t)n * 8);
    }
    *(uintptr_t*)(p + 0x08) = buf;
    *(uintptr_t*)(p + 0x10) = buf + (uintptr_t)n * 8;
    *(uintptr_t*)(p + 0x18) = buf + (uintptr_t)n * 8;
    *(unsigned char*)(p + 0x20) = (unsigned char)elemType;
    return p;
}

static int NbPut(uintptr_t cmp, const char* key, uintptr_t val)
{
    static uintptr_t fn = 0;
    if (!fn) { if (!g_libBase) g_libBase = FindLibBase(kLib); if (!g_libBase) return 0; fn = g_libBase + NB_FN_PUT; }
    unsigned char k[32];
    memset(k, 0, sizeof k);
    NbStrSet(k, key, (unsigned long)strlen(key));
    typedef uintptr_t (*PF)(uintptr_t, const void*, uintptr_t);
    ((PF)fn)(cmp, k, val);
    return 1;
}

static const char* NbKeyAt(uintptr_t node, int* outLen)
{
    static char buf[64];
    unsigned char k[24];
    if (vm_read(k, node + 0x20, 24) <= 0) return 0;
    int n = k[0] >> 1;
    if (n <= 0 || n > 23) n = 0;
    memcpy(buf, k + 1, n);
    buf[n] = 0;
    if (outLen) *outLen = n;
    return buf;
}

static void NbLogTree(uintptr_t comp, int maxn)
{
    uintptr_t cnt = 0, root = 0;
    if (!comp) { MCLOG("[nb] comp null"); return; }
    vm_read(&cnt, comp + 0x18, 8);
    if (vm_read(&root, comp + 0x10, 8) <= 0) return;
    uintptr_t stack[64]; int sp = 0;
    uintptr_t head = StripTag(root);
    if (head && head != comp + 0x10) stack[sp++] = head;
    int seen = 0;
    MCLOG("[nb] entries=%lu", (unsigned long)cnt);
    while (sp > 0 && seen < maxn) {
        uintptr_t n = StripTag(stack[--sp]);
        if (!n) continue;
        seen++;
        int len = 0;
        const char* k = NbKeyAt(n, &len);
        MCLOG("   #%d key=%s type=%d node=0x%lx", seen, k ? k : "?", NodeType(n), (unsigned long)n);
        uintptr_t a = 0, b = 0, c = 0;
        if (vm_read(&a, n, 8) > 0) { a = StripTag(a); if (a && a != comp + 0x10) stack[sp++] = a; }
        if (vm_read(&b, n + 8, 8) > 0) { b = StripTag(b); if (b && b != comp + 0x10) stack[sp++] = b; }
        if (vm_read(&c, n + 0x10, 8) > 0) { c = StripTag(c); if (c && c != comp + 0x10) stack[sp++] = c; }
    }
}

// ---------- 极简 JSON 解析 ----------
struct JV;
struct JPair { char* k; JV* v; };
struct JV {
    int kind;   // 0 null 1 bool 2 num 3 str 4 arr 5 obj
    int bval;
    double num;
    char* str;
    JV** arr; int n;
    JPair* kv; int nk;
};

static const char* g_jp = 0;
static JV* JNew(int kind)
{
    JV* v = (JV*)calloc(1, sizeof(JV));
    v->kind = kind;
    return v;
}
static void JSkip(void)
{
    while (*g_jp && (*g_jp == 32 || *g_jp == 10 || *g_jp == 13 || *g_jp == 9)) g_jp++;
}
static JV* JParse(void);
static char* JParseStr(void)
{
    if (*g_jp != 34) return 0;
    g_jp++;
    char* out = (char*)calloc(1, 256);
    int i = 0;
    while (*g_jp && *g_jp != 34) {
        char ch = *g_jp++;
        if (ch == 92) {
            char e = *g_jp++;
            if (e == 110) ch = 10; else if (e == 116) ch = 9; else if (e == 114) ch = 13;
            else if (e == 117) {
                unsigned int cp = 0;
                for (int k = 0; k < 4 && *g_jp; k++) { char h = *g_jp++; cp <<= 4;
                    if (h >= 48 && h <= 57) cp |= (unsigned)(h - 48);
                    else if (h >= 97 && h <= 102) cp |= (unsigned)(h - 87);
                    else if (h >= 65 && h <= 70) cp |= (unsigned)(h - 55); }
                ch = (char)(cp & 0x7F);
            } else ch = e;
        }
        if (i < 250) out[i++] = ch;
    }
    if (*g_jp == 34) g_jp++;
    out[i] = 0;
    return out;
}
static JV* JParse(void)
{
    JSkip();
    if (!*g_jp) return 0;
    if (*g_jp == 123) {
        g_jp++;
        JV* v = JNew(5);
        v->kv = (JPair*)calloc(64, sizeof(JPair));
        while (1) {
            JSkip();
            if (*g_jp == 125) { g_jp++; break; }
            char* k = JParseStr();
            JSkip();
            if (*g_jp == 58) g_jp++;
            JV* val = JParse();
            if (!val) break;
            if (v->nk < 64) { v->kv[v->nk].k = k; v->kv[v->nk].v = val; v->nk++; }
            JSkip();
            if (*g_jp == 44) { g_jp++; continue; }
            if (*g_jp == 125) { g_jp++; break; }
            break;
        }
        return v;
    }
    if (*g_jp == 91) {
        g_jp++;
        JV* v = JNew(4);
        v->arr = (JV**)calloc(64, sizeof(JV*));
        while (1) {
            JSkip();
            if (*g_jp == 93) { g_jp++; break; }
            JV* val = JParse();
            if (!val) break;
            if (v->n < 64) v->arr[v->n++] = val;
            JSkip();
            if (*g_jp == 44) { g_jp++; continue; }
            if (*g_jp == 93) { g_jp++; break; }
            break;
        }
        return v;
    }
    if (*g_jp == 34) { JV* v = JNew(3); v->str = JParseStr(); return v; }
    if (!strncmp(g_jp, "true", 4)) { g_jp += 4; JV* v = JNew(1); v->bval = 1; return v; }
    if (!strncmp(g_jp, "false", 5)) { g_jp += 5; JV* v = JNew(1); v->bval = 0; return v; }
    if (!strncmp(g_jp, "null", 4)) { g_jp += 4; return JNew(0); }
    {
        JV* v = JNew(2);
        char tmp[64]; int i = 0;
        while (*g_jp && i < 62) {
            char ch = *g_jp;
            if ((ch >= 48 && ch <= 57) || ch == 45 || ch == 43 || ch == 46 || ch == 101 || ch == 69) { tmp[i++] = ch; g_jp++; }
            else break;
        }
        tmp[i] = 0;
        v->num = atof(tmp);
        return v;
    }
}

static int NbSuffixType(const char* s, double* numOut)
{
    int n = (int)strlen(s);
    if (n < 2) return 0;
    char c = s[n - 1];
    if (!(c == 98 || c == 66 || c == 115 || c == 83 || c == 108 || c == 76 || c == 102 || c == 70)) return 0;
    char buf[64]; if (n > 62) return 0;
    memcpy(buf, s, n - 1); buf[n - 1] = 0;
    int ok = 1;
    for (int i = 0; buf[i]; i++) if (!((buf[i] >= 48 && buf[i] <= 57) || buf[i] == 45 || buf[i] == 46)) { ok = 0; break; }
    if (!ok) return 0;
    *numOut = atof(buf);
    if (c == 98 || c == 66) return 1;
    if (c == 115 || c == 83) return 2;
    if (c == 108 || c == 76) return 4;
    if (c == 102 || c == 70) return 5;
    return 0;
}

static uintptr_t J2Tag(const JV* v)
{
    if (!v) return 0;
    if (v->kind == 1) { signed char b = v->bval ? 1 : 0; return NbNum(NB_VT_BYTE, &b, 1); }
    if (v->kind == 2) {
        double d = v->num;
        int isInt = (d == (double)(long long)d) && d > -2147483648.0 && d < 2147483648.0;
        if (isInt) { int i = (int)d; return NbNum(NB_VT_INT, &i, 4); }
        return NbNum(NB_VT_DOUBLE, &d, 8);
    }
    if (v->kind == 3) {
        double d = 0;
        int t = NbSuffixType(v->str, &d);
        if (t == 1) { signed char b = (signed char)d; return NbNum(NB_VT_BYTE, &b, 1); }
        if (t == 2) { short s2 = (short)d; return NbNum(NB_VT_SHORT, &s2, 2); }
        if (t == 4) { long long l = (long long)d; return NbNum(NB_VT_LONG, &l, 8); }
        if (t == 5) { float f = (float)d; return NbNum(NB_VT_FLOAT, &f, 4); }
        return NbString(v->str, (unsigned long)strlen(v->str));
    }
    if (v->kind == 4) {
        uintptr_t el[64]; int n = v->n; if (n > 64) n = 64;
        int elemType = 10;
        if (n == 0) return NbList(10, 0, 0);
        for (int i = 0; i < n; i++) {
            el[i] = J2Tag(v->arr[i]);
            if (i == 0 && el[i]) { int ty = NodeType(el[i]); elemType = (ty >= 0) ? ty : 10; }
        }
        return NbList(elemType, el, n);
    }
    if (v->kind == 5) {
        if (v->nk == 0) return NbCompound();
        uintptr_t c = NbCompound();
        for (int i = 0; i < v->nk; i++) {
            if (!v->kv[i].k) continue;
            uintptr_t t = J2Tag(v->kv[i].v);
            if (!t) continue;
            NbPut(c, v->kv[i].k, t);
            { uintptr_t szz = 0; if (vm_read(&szz, c + 0x18, 8) == 8) MCLOG("[nj]   put [%s] -> size=%lu", v->kv[i].k, (unsigned long)szz); else MCLOG("[nj]   put [%s] -> size-unreadable", v->kv[i].k); }
        }
        return c;
    }
    return 0;
}

static JV* JFind(const JV* obj, const char* key)
{
    if (!obj || obj->kind != 5) return 0;
    for (int i = 0; i < obj->nk; i++)
        if (obj->kv[i].k && !strcmp(obj->kv[i].k, key)) return obj->kv[i].v;
    return 0;
}

static int NbImportJsonFile(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) { MCLOG("[nj] open fail %s", path); return 0; }
    long sz = 0; fseek(f, 0, SEEK_END); sz = ftell(f);
    if (sz <= 0 || sz > (16L << 20)) { fclose(f); MCLOG("[nj] bad size %ld", sz); return 0; }
    char* buf = (char*)malloc((size_t)sz + 1);
    fseek(f, 0, SEEK_SET);
    long rd2 = (long)fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd2] = 0;
    g_jp = buf;
    JV* root = JParse();
    if (!root) { MCLOG("[nj] json parse fail"); free(buf); return 0; }
    MCLOG("[nj] parsed root kind=%d", root->kind);
    uintptr_t tag = 0;
    JV* inj = JFind(root, "tag");
    uintptr_t srcCmp = (inj && inj->kind == 5) ? J2Tag(inj) : (root->kind == 5 ? J2Tag(root) : 0);
    if (!srcCmp) { MCLOG("[nj] build tag fail"); free(buf); return 0; }
    tag = srcCmp;
    MCLOG("[nj] built tag=0x%lx (self-test, not writing yet)", (unsigned long)tag);
    NbLogTree(tag, 20);
    if (!g_invBegin) FindInventoryNow();
    if (!g_invBegin) { MCLOG("[nj] no inventory (not in world), skip write"); free(buf); return 1; }
    int slot = -1;
    if (g_invCntFound) vm_read(&slot, g_piObj[0] + 0x10, 4);
    if (slot < 0 || slot >= 36) { MCLOG("[nj] bad slot %d", slot); free(buf); return 0; }
    uintptr_t ps = SlotAddr(slot);
    if (!ps) { MCLOG("[nj] no slot addr"); free(buf); return 0; }
    uintptr_t oldTag = 0; vm_read(&oldTag, ps + 0x10, 8);
    MCLOG("[nj] slot=%d stack=0x%lx oldTag=0x%lx newTag=0x%lx", slot, (unsigned long)ps, (unsigned long)StripTag(oldTag), (unsigned long)tag);
    NbLogTree(tag, 16);
    long wr = vm_write(0, ps + 0x10, &tag, 8);
    MCLOG("[nj] write tag -> %ld", wr);
    uintptr_t rdTag = 0; vm_read(&rdTag, ps + 0x10, 8);
    MCLOG("[nj] readback tag=0x%lx", (unsigned long)StripTag(rdTag));
    NbLogTree(StripTag(rdTag), 16);
    JV* cntV = JFind(root, "Count");
    if (cntV && g_invCntFound) {
        double d = 0;
        if (cntV->kind == 3) NbSuffixType(cntV->str, &d);
        else if (cntV->kind == 2) d = cntV->num;
        int c = (int)d; if (c < 1) c = 1; if (c > 255) c = 255;
        unsigned char cb = (unsigned char)c;
        vm_write(0, ps + 0x22, &cb, 1);
        MCLOG("[nj] set count=%d", c);
    }
    free(buf);
    return 1;
}

static void ProcessActFile()
{
    FILE* f = fopen("/sdcard/act.txt", "r");
    if (!f) { f = fopen("/storage/emulated/0/act.txt", "r"); if (!f) return; }
    MCLOG("[act] begin");
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char op[16] = {0}, a[40] = {0};
        unsigned long long off = 0, val = 0;
        int r = sscanf(line, "%15s %39s %llx %llx", op, a, &off, &val);
        if (r < 2) continue;
        if (!strcmp(op, "NJ")) { MCLOG("[nj] file=%s", a); NbImportJsonFile(a); continue; }
        if (!strcmp(op, "AIMON")) { aim_set_enabled(0); MCLOG("[aim] disabled by build"); continue; }
        if (!strcmp(op, "AIMOFF")) { aim_set_enabled(0); MCLOG("[aim] off"); continue; }
        if (!strcmp(op, "AIMR")) { float rv = (float)atof(a); aim_set_radius(rv); MCLOG("[aim] r=%.1f", rv); continue; }
        if (!strcmp(op, "AIMASK")) { int mk = atoi(a); aim_set_mask(mk); MCLOG("[aim] mask=%d", mk); continue; }
        if (!strcmp(op, "ROTPROBE")) { aim_rot_probe(a[0] ? (float)atof(a) : 0.8f); MCLOG("[aim] rotprobe %s", a); continue; }
        if (!strcmp(op, "AIMSTAT")) { MCLOG("[aim] en=%d mask=%d r=%.1f has=%d name=%s cat=%d d=%.1f vpt=%d obj=%d", aim_enabled(), aim_mask(), aim_radius(), aim_has(), aim_name(), aim_cat(), aim_dist(), aim_vpt_count(), aim_obj_count()); continue; }
        if (!strcmp(op, "INV")) { FindInventoryNow(); MCLOG("[inv] begin=0x%lx slots=%d", (unsigned long)g_invBegin, g_invN); continue; }
        if (!strcmp(op, "ENCH")) {
            int s1 = (int)strtoull(a, nullptr, 16);
            int nlvl = (int)off;
            if (!g_invBegin) FindInventoryNow();
            int tot = 0;
            for (int q = 0; q < g_invCntFound; q++) {
                if (s1 < 0 || s1 >= g_invListN[q]) continue;
                g_invBegin = g_invList[q]; g_invN = g_invListN[q];
                MCLOG("[nbt] inv%d scan-write", q);
                tot += SetEnchLevel(s1, nlvl, -1);
                MCLOG("[nbt] inv%d done", q);
            }
            MCLOG("[nbt] ENCH slot%d lvl=%d total=%d", s1, nlvl, tot);
            continue;
        }
        if (!strncmp(op, "ITEMV", 5)) {
            uintptr_t it = FindItemByName(a);
            uint64_t vt = 0; uint32_t f0 = 0, f1 = 0;
            if (it) { vm_read(&vt, it, 8); vm_read(&f0, it + 0x08, 4); vm_read(&f1, it + 0x0C, 4); }
            MCLOG("[itemv] name=%s item=0x%lx vptr=0x%lx f8=%u fc=%u", a, (unsigned long)it, (unsigned long)vt, f0, f1);
            if (!g_invBegin) FindInventoryNow();
            int slot = 0; if (g_invCntFound) vm_read(&slot, g_piObj[0] + 0x10, 4); if (slot < 0 || slot > 8) slot = 0;
            uintptr_t ps = (slot >= 0) ? SlotAddr(slot) : 0;
            if (ps) {
                uint64_t raw = 0; vm_read(&raw, ps + 0x08, 8);
                uintptr_t mine = StripTag(raw);
                uint64_t mvt = 0; if (mine) vm_read(&mvt, mine, 8);
                MCLOG("[itemv] held item=0x%lx vptr=0x%lx same=%d", (unsigned long)mine, (unsigned long)mvt,
                      (int)(mine && it && mvt == vt));
            }
            continue;
        }
        if (!strncmp(op, "ITEMID", 6)) {
            int s1 = (int)strtoull(a, nullptr, 10);
            int id = ItemIdFromSlot(s1);
            MCLOG("[iid] ITEMID slot%d = %d", s1, id);
            continue;
        }
        if (!strcmp(op, "FINDP")) {
            MCLOG("[fref] scan 0x%llx", (unsigned long long)off);
            FindPtrRefs((uintptr_t)off, 8);
            continue;
        }
        if (!strcmp(op, "S16")) {
            int want = atoi(a);
            uintptr_t st = (uintptr_t)off;
            unsigned long len = (unsigned long)(val ? val : 0x2000);
            MCLOG("[s16] want=%d start=0x%lx len=0x%lx", want, (unsigned long)st, len);
            unsigned char buf[0x400];
            int hits = 0;
            for (unsigned long p = 0; p + 2 <= len; p += 0x400) {
                unsigned long nread = len - p; if (nread > 0x400) nread = 0x400;
                if (vm_read(buf, st + p, nread) <= 0) break;
                for (unsigned long i = 0; i + 2 <= nread; i += 2) {
                    int16_t v; memcpy(&v, buf + i, 2);
                    if (v != (int16_t)want) continue;
                    uintptr_t hit = st + p + i;
                    unsigned char nb[16] = {0};
                    vm_read(nb, hit - 6, 16);
                    MCLOG("[s16] hit=0x%lx ctx=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                          (unsigned long)hit, nb[0],nb[1],nb[2],nb[3],nb[4],nb[5],nb[6],nb[7],
                          nb[8],nb[9],nb[10],nb[11],nb[12],nb[13],nb[14],nb[15]);
                    if (++hits >= 20) break;
                }
                if (hits >= 20) break;
            }
            MCLOG("[s16] done hits=%d", hits);
            continue;
        }
        if (!strcmp(op, "VT")) {
            uintptr_t a0 = (uintptr_t)off;
            uint64_t vt = 0, fp = 0;
            vm_read(&vt, a0, 8);
            uintptr_t vtu = (uintptr_t)(vt & 0x00ffffffffffffffULL);
            long rr = vm_read(&fp, vtu + 0x280, 8);
            MCLOG("[vt] obj=0x%lx vt=0x%lx vtu=0x%lx rr=%ld fp=0x%lx", (unsigned long)a0, (unsigned long)vt, (unsigned long)vtu, rr, (unsigned long)fp);
            if (fp) { typedef int (*Fn)(void*, int); int r = ((Fn)(uintptr_t)(fp & 0x00ffffffffffffffULL))((void*)a0, 0); MCLOG("[vt] call=%d", r); }
            continue;
        }
        if (!strcmp(op, "IDMAP")) { BuildItemMap(); continue; }
        if (!strcmp(op, "IDTBL")) { BuildIdTable(); continue; }
        if (!strcmp(op, "INAME")) {
            int held = -1; HeldItemObj(&held);
            uintptr_t p = SlotAddr(held);
            if (p) {
                uintptr_t it = StripTag(rd64v(p + 0x08));
                char nm[80];
                MCLOG("[iname] held slot=%d item=0x%lx", held, (unsigned long)it);
                ItemNameOf(it, nm, sizeof nm);
            }
            continue;
        }
        if (!strcmp(op, "CLIPC")) { ClipCopy(); continue; }
        if (!strcmp(op, "CLIPV")) {
            int slot = (int)off;
            FindInventoryNow();
            for (int q = 0; q < g_invCntFound; q++) {
                uintptr_t ps = g_invList[q] + (uintptr_t)slot * 0x98;
                ClipPasteTo(ps);
            }
            continue;
        }
        if (!strcmp(op, "GIVEAT")) {
            int id = (int)strtol(a, nullptr, 10);
            uintptr_t item = (uintptr_t)off;
            int slot = -1; HeldItemObj(&slot);
            FindInventoryNow();
            for (int q = 0; q < g_invCntFound; q++) {
                uintptr_t ps = g_invList[q] + (uintptr_t)slot * 0x98;
                MakeItemFromPtrAt(ps, item, 1);
            }
            MCLOG("[give] GIVEAT id=%d item=0x%lx slot=%d invs=%d", id, (unsigned long)item, slot, g_invCntFound);
            continue;
        }
        if (!strcmp(op, "GIVEID")) {
            int id = (int)strtol(a, nullptr, 10);
            int slot = -1; HeldItemObj(&slot);
            uintptr_t item = 0;
            FILE* tf = fopen("/sdcard/mcmod_idtbl.txt", "r");
            if (tf) {
                int rid = 0; unsigned long rip = 0;
                while (fscanf(tf, "%d %lx", &rid, &rip) == 2) { if (rid == id) { item = (uintptr_t)rip; break; } }
                fclose(tf);
            }
            if (!item) { MCLOG("[give] id=%d not found", id); continue; }
            FindInventoryNow();
            for (int q = 0; q < g_invCntFound; q++) {
                uintptr_t ps = g_invList[q] + (uintptr_t)slot * 0x98;
                MakeItemFromPtrAt(ps, item, 1);
                MCLOG("[give] write inv#%d ps=0x%lx", q, (unsigned long)ps);
            }
            MCLOG("[give] id=%d item=0x%lx slot=%d invs=%d", id, (unsigned long)item, slot, g_invCntFound);
            continue;
        }
        if (!strcmp(op, "NAMES")) {
            FILE* mf = fopen("/proc/self/maps", "r");
            FILE* nfp = nullptr;
            int cnt = 0;
            if (mf) {
                static unsigned char* nb = nullptr;
                const unsigned long CH = 1u << 20;
                if (!nb) nb = (unsigned char*)malloc(CH);
                char line[512];
                while (nb && fgets(line, sizeof(line), mf)) {
                    unsigned long s0 = 0, e0 = 0; char perms[8] = {0}, path[256] = {0};
                    if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &s0, &e0, perms, path) < 3) continue;
                    if (perms[0] != 'r') continue;
                    if (s0 < 0x7000000000UL) continue;
                    unsigned long sz = e0 - s0;
                    for (unsigned long off = 0; off + 16 < sz; off += CH) {
                        unsigned long len = sz - off; if (len > CH) len = CH;
                        long got = vm_read(nb, s0 + off, len);
                        if (got <= 0) break;
                        for (long i = 8; i + 9 < got; i++) {
                            if (memcmp(nb + i, "minecraft:", 10) != 0) continue;
                            long st = i - 1;
                            unsigned char ln = nb[st];
                            if (ln < 10 || ln > 64 || st + 1 + ln > got) continue;
                            unsigned char* ns = nb + st + 1;
                            int ok = 1;
                            for (int k = 0; k < (int)ln; k++) if (ns[k] < 32 || ns[k] > 126) { ok = 0; break; }
                            if (!ok) continue;
                            if (cnt < 6000) {
                                if (!nfp) nfp = fopen("/sdcard/mcmod_names.txt", "w");
                                if (nfp) fprintf(nfp, "%.*s\n", (int)ln, (const char*)ns);
                                cnt++;
                            }
                        }
                    }
                }
                fclose(mf);
            }
            if (nfp) fclose(nfp);
            MCLOG("[names] total=%d", cnt);
            continue;
        }
        if (!strcmp(op, "SCANSTR")) { MCLOG("[sstr] ===== begin"); ScanStrCtx(a); continue; }
        if (!strncmp(op, "NBTH", 4)) {
            NbtDumpSlot(-1);
            continue;
        }
        if (!strncmp(op, "NBTD", 4)) {
            NbtDumpSlot((int)off);
            continue;
        }
        if (!strncmp(op, "COPY", 4)) {
            int s1 = (int)strtoull(op + 4, nullptr, 16);
            int s2 = (int)off;
            if (!g_invBegin) FindInventoryNow();
            for (int q = 0; q < g_invCntFound; q++) {
                if (s1 < 0 || s2 < 0 || s1 >= g_invListN[q] || s2 >= g_invListN[q]) continue;
                g_invBegin = g_invList[q]; g_invN = g_invListN[q];
                int ok = CopyStackFull(s1, s2);
                MCLOG("[copy] inv%d d%d<-s%d ok=%d", q, s1, s2, ok);
            }
            continue;
        }
        if (!strncmp(op, "MAKE", 4)) {
            int s1 = (int)strtoull(op + 4, nullptr, 16);
            if (!g_libBase) g_libBase = FindLibBase(kLib);
            if (!g_invBegin) FindInventoryNow();
            MCLOG("[make] cmd slot=%d name=%s found_inv=%d", s1, a, g_invCntFound);
            for (int q = 0; q < g_invCntFound; q++) {
                if (s1 < 0 || s1 >= g_invListN[q]) continue;
                g_invBegin = g_invList[q]; g_invN = g_invListN[q];
                int ok = MakeItem(s1, a);
                MCLOG("[make] inv%d slot%d result=%d", q, s1, ok);
            }
            continue;
        }
        if (!strcmp(op, "SLOTI") || !strcmp(op, "CNT") || !strcmp(op, "TAG") || !strcmp(op, "CLONE")) {
            int s1 = (int)strtoull(a, nullptr, 16);
            int s2 = (int)off;
            uintptr_t p1 = SlotAddr(s1);
            if (!p1) { MCLOG("[inv] slot %d out of range (n=%d)", s1, g_invN); continue; }
            if (!strcmp(op, "SLOTI")) {
                uint64_t vt = 0, item = 0, tag = 0; vm_read(&vt, p1, 8); vm_read(&item, p1 + 8, 8); vm_read(&tag, p1 + 0x10, 8);
                unsigned char cnt = 0; vm_read(&cnt, p1 + 0x22, 1);
                MCLOG("[inv] slot%d vt=0x%lx cnt=%d item=0x%lx tag=0x%lx", s1, (unsigned long)vt, (int)cnt, (unsigned long)item, (unsigned long)tag);
            } else if (!strcmp(op, "CNT")) {
                unsigned char ov = 0, nv = (unsigned char)off;
                vm_read(&ov, p1 + 0x22, 1); vm_write(nullptr, p1 + 0x22, &nv, 1);
                unsigned char ck = 0; vm_read(&ck, p1 + 0x22, 1);
                MCLOG("[inv] slot%d cnt %d -> %d now=%d", s1, (int)ov, (int)nv, (int)ck);
            } else if (!strcmp(op, "TAG")) {
                uint64_t tag = 0; vm_read(&tag, p1 + 0x10, 8);
                MCLOG("[inv] slot%d tag=0x%lx", s1, (unsigned long)tag);
            } else if (!strcmp(op, "CLONE")) {
                FindInventoryNow();
                int applied = 0;
                for (int q = 0; q < g_invCntFound; q++) {
                    int nn = g_invListN[q];
                    if (s1 < 0 || s2 < 0 || s1 >= nn || s2 >= nn) continue;
                    uintptr_t d1 = g_invList[q] + (uintptr_t)s1 * 0x98;
                    uintptr_t d2 = g_invList[q] + (uintptr_t)s2 * 0x98;
                    uint64_t srcTag = 0; vm_read(&srcTag, d2 + 0x10, 8);
                    if (!srcTag) { MCLOG("[inv] inv%d src slot%d has no tag", q, s2); continue; }
                    uintptr_t nt = 0;
                    g_guard = 1;
                    if (sigsetjmp(g_jb, 1) == 0) {
                        nt = CallClone(g_libBase + OFF_CLONE, (uintptr_t)srcTag);
                        g_guard = 0;
                    } else {
                        g_guard = 0;
                        MCLOG("[inv] inv%d clone FAULTED", q);
                        continue;
                    }
                    if (!nt) { MCLOG("[inv] inv%d clone null", q); continue; }
                    vm_write(nullptr, d1 + 0x10, &nt, 8);
                    uint64_t ck = 0; vm_read(&ck, d1 + 0x10, 8);
                    MCLOG("[inv] inv%d CLONE d%d <- s%d new=0x%lx rb=0x%lx", q, s1, s2, (unsigned long)nt, (unsigned long)ck);
                    applied++;
                }
                MCLOG("[inv] CLONE found=%d applied=%d", g_invCntFound, applied);
            }
            continue;
        }
        uintptr_t addr = (uintptr_t)strtoull(a, nullptr, 16);
        if (!addr) continue;
        if (!strcmp(op, "DUMP")) {
            long len = (long)off;
            MCLOG("[act] DUMP %p size=%ld", (void*)addr, len);
            unsigned char buf[32];
            for (long i = 0; i < len; i += 32) {
                unsigned long n = (len - i > 32) ? 32 : (unsigned long)(len - i);
                if (vm_read(buf, addr + i, n) <= 0) { MCLOG("[act]   unreadable %p", (void*)(addr + i)); break; }
                char s[96]; int k2 = 0;
                for (unsigned long k = 0; k < n; k++) k2 += snprintf(s + k2, sizeof(s) - k2, "%02x", buf[k]);
                MCLOG("[act]   %p %s", (void*)(addr + i), s);
            }
        } else if (!strcmp(op, "W32")) {
            uintptr_t p = addr + (uintptr_t)off; int ov = 0, nv = (int)val;
            vm_read(&ov, p, 4);
            vm_write(nullptr, p, &nv, 4);
            int chk = 0; vm_read(&chk, p, 4);
            MCLOG("[act] W32 %p %d -> %d now=%d", (void*)p, ov, nv, chk);
        } else if (!strcmp(op, "W8")) {
            uintptr_t p = addr + (uintptr_t)off; unsigned char ov = 0, nv = (unsigned char)val;
            vm_read(&ov, p, 1);
            vm_write(nullptr, p, &nv, 1);
            unsigned char chk = 0; vm_read(&chk, p, 1);
            MCLOG("[act] W8 %p %d -> %d now=%d", (void*)p, (int)ov, (int)nv, (int)chk);
        } else if (!strcmp(op, "Q")) {
            unsigned long long v = 0; vm_read(&v, addr + (uintptr_t)off, 8);
            MCLOG("[act] Q %p = 0x%llx", (void*)(addr + (uintptr_t)off), v);
        } else if (!strcmp(op, "R32")) {
            int v = 0; vm_read(&v, addr + (uintptr_t)off, 4);
            MCLOG("[act] R32 %p = %d (0x%x)", (void*)(addr + (uintptr_t)off), v, v);
        }
    }
    fclose(f);
    remove("/sdcard/act.txt");
    remove("/storage/emulated/0/act.txt");
    MCLOG("[act] end");
}

bool mod_ui_swallow(float x, float y);

static void InstallPatch()
{
    if (!g_libBase) g_libBase = FindLibBase(kLib);
    if (!g_libBase) return;

    if (!g_capDone) {
        void* p = (void*)(g_libBase + OFF_CAPCSE);
        uint32_t old = 0;
        if (KittyMemory::memRead(&old, p, 4) == KittyMemory::SUCCESS) {
            if (old == 0x1A89C108u || old == 0x2A0903E8u) {
                uint32_t w = 0x2A0903E8u;   // mov w8, w9
                g_capDone = (KittyMemory::memWrite(p, &w, 4) == KittyMemory::SUCCESS);
                LI("cap patch %s (0x%08X)", g_capDone ? "ok" : "failed", old);
            } else {
                LE("cap 原值异常 0x%08X，跳过", old);
            }
        }
        void* fn = (void*)(g_libBase + OFF_SETMAX);
        g_setmax = (fn_setmax)fn;
    }
}

// ---------- 输入注入 ----------
static void (*o_input)(void*, void*, void*) = nullptr;
static bool (*o_copy)(void*, const void*, bool) = nullptr;

static void myInput(void* thiz, void* a, void* b)
{
    if (o_input) o_input(thiz, a, b);
    if (isInitialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}
static void (*o_offloc)(void*, float, float) = nullptr;
static int s_shield = 0;

static bool myCopy(void* thiz, const void* other, bool keepHistory)
{
    bool r = o_copy ? o_copy(thiz, other, keepHistory) : false;
    { static int _e = 0;
      if (_e < 16) { _e++;
        FILE* _f = fopen("/data/data/com.mojang.minecraftpe/files/mcstage.log", "a");
        if (_f) { fprintf(_f, "[copyE] thiz=%p o=%p r=%d", thiz, (void*)o_copy, (int)r); fputc(10, _f); fclose(_f); } } }
    (void)r;  // copyFrom may legitimately return false; do NOT gate input on it
    if (isInitialized && thiz) {
        AInputEvent* ev = (AInputEvent*)thiz;
        int32_t at = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;
        float tx = AMotionEvent_getX(ev, 0);
        float ty = AMotionEvent_getY(ev, 0);

        { static int _n = 0;
          if (_n < 16) { _n++;
            FILE* _f = fopen("/data/data/com.mojang.minecraftpe/files/mcstage.log", "a");
            if (_f) { fprintf(_f, "[copy] at=%d xy=%.0f,%.0f shield=%d off=%p", at, tx, ty, s_shield, (void*)o_offloc);
                     fputc(10, _f); fclose(_f); } } }
        ImGui_ImplAndroid_HandleInputEvent(ev);


        if (!o_offloc) o_offloc = (void (*)(void*, float, float))DobbySymbolResolver("libinput.so", "_ZN7android11MotionEvent14offsetLocationEff");

        if (at == AMOTION_EVENT_ACTION_DOWN)
            s_shield = mod_ui_swallow(tx, ty) ? 1 : 0;

        if (s_shield && o_offloc) o_offloc(thiz, -10000.0f, -10000.0f);

        if (at == AMOTION_EVENT_ACTION_UP || at == AMOTION_EVENT_ACTION_CANCEL)
            s_shield = 0;
    }
    return r;
}

static void HookInput()
{
    if (g_inputHooked) return;
    void* s1 = DobbySymbolResolver("libinput.so",
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE");
    if (s1) { DobbyHook(s1, (void*)myInput, (void**)&o_input); LI("input hook A ok"); }
    void* s2 = DobbySymbolResolver("libinput.so",
        "_ZN7android11MotionEvent8copyFromEPKS0_b");
    if (s2) { int rc = DobbyHook(s2, (void*)myCopy, (void**)&o_copy); LI("input hook B rc=%d fn=%p tramp=%p", rc, s2, (void*)o_copy); }
    else LI("input hook B symbol null");
    g_inputHooked = (s1 != nullptr || s2 != nullptr);
}


static int  LoadCfg()
{
    FILE* f = fopen(kCfg, "r");
    if (!f) f = fopen("/storage/emulated/0/Android/data/com.mojang.minecraftpe/mcmod.cfg", "r");
    if (!f) return 0;
    int v = 0; fscanf(f, "%d", &v); fclose(f);
    if (v < 1 || v > 100) v = 0;
    return v;
}

static void SaveCfg(int v)
{
    FILE* f = fopen(kCfg, "w");
    if (!f) f = fopen("/storage/emulated/0/Android/data/com.mojang.minecraftpe/mcmod.cfg", "w");
    if (!f) return;
    fprintf(f, "%d\n", v); fclose(f);
}

// ---------- 核心动作 ----------
static void ApplyMax();
static void StartScan()
{
    MCLOG("[StartScan] called scan=%d", (int)g_scanning);
    if (g_scanning) return;
    g_scanning = true;
    snprintf(g_msg, sizeof(g_msg), "正在扫描世界对象…");
    std::thread([]{
        MCLOG("[thread] begin");
        void* p = ScanLevelOnce();
        MCLOG("[thread] got %p", p);
        if (p) {
            g_level = p;
            int saved = LoadCfg();
            if (saved > 0) { g_target = saved; }
            ApplyMax();
            snprintf(g_msg, sizeof(g_msg), "已锁定世界对象，当前上限 %d", *(int*)((uintptr_t)p + OFF_MAXVAL));
        }
        else   { snprintf(g_msg, sizeof(g_msg), "没找到：请先进入一个世界再扫描"); }
        g_scanning = false;
    }).detach();
}

static void ApplyMax()
{
    if (!g_libBase) InstallPatch();
    g_levelAddr = (unsigned long)g_level;
    if (!ValidLevel(g_level)) g_level = nullptr;
    if (!g_level) {
        g_level = ScanLevelOnce();
        if (!g_level) {
            snprintf(g_msg, sizeof(g_msg), "没找到世界对象：请先进入一个世界");
            return;
        }
    }
    if (g_setmax) {
        g_setmax(g_level, g_target);                     // 走游戏自己的实现，会通知网络层
        snprintf(g_msg, sizeof(g_msg), "已设置 %d 人（游戏内生效）", g_target);
    } else {
        int v = g_target;
        KittyMemory::memWrite((void*)((uintptr_t)g_level + OFF_MAXVAL), &v, 4);
        snprintf(g_msg, sizeof(g_msg), "直接写入 %d（未找到函数）", g_target);
    }
}

static int ReadCurrent()
{
    if (!ValidLevel(g_level)) return -1;
    return *(int*)((uintptr_t)g_level + OFF_MAXVAL);
}

static bool g_showFps = true;

// ---------- 悬浮球（长方形蓝色 Tools） ----------
static bool IsBadPath(const char* path)
{
    if (!path[0]) return false;
    if (strstr(path, "/dev/")) return true;
    if (strstr(path, "libEGL") || strstr(path, "libGLES") || strstr(path, "gralloc")) return true;
    return false;
}
// ==== 可见区块距离：字节特征定位 + 滑块写入 ====
#define VD_MAX 24
static uintptr_t g_vdList[VD_MAX];
static int       g_vdCount = 0;
static int       g_vdRef = 0;
static int       g_viewDist = 12;

static void VdAdd(uintptr_t a)
{
    if (!a) return;
    for (int i = 0; i < g_vdCount; i++) if (g_vdList[i] == a) return;
    if (g_vdCount < VD_MAX) g_vdList[g_vdCount++] = a;
}

static long MemFind(const unsigned char* buf, long len, const unsigned char* pat, long plen)
{
    if (len < plen) return -1;
    const unsigned char* p = buf;
    const unsigned char* end = buf + len - plen + 1;
    while (p < end) {
        p = (const unsigned char*)memchr(p, pat[0], (size_t)(end - p));
        if (!p) return -1;
        if (memcmp(p, pat, (size_t)plen) == 0) return (long)(p - buf);
        p++;
    }
    return -1;
}

static void ScanVd()
{
    MCLOG("[vd] scan enter");
    g_vdCount = 0;
    g_vdRef = 0;
    static const unsigned char MA[8] = {0x06,0x01,0x06,0x00,0x00,0x00,0x91,0x89};
    static const unsigned char MC[8] = {0x16,0x00,0x00,0x00,0x05,0x00,0x00,0x00};
    static const unsigned char MB[8] = {0x16,0x00,0x00,0x00,0x00,0x00,0x01,0x01};
    const unsigned long CH = 1u << 20;
    unsigned char* buf = (unsigned char*)malloc(CH + 0x100);
    if (!buf) return;
    FILE* mf = fopen("/proc/self/maps", "r");
    if (!mf) { free(buf); return; }
    char line[512];
    int nreg = 0, nchunk = 0;
    while (fgets(line, sizeof(line), mf)) {
        unsigned long s = 0, e = 0; char perms[8] = {0}, path[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &s, &e, perms, path);
        if (n < 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w') continue;
        if (n >= 4 && IsBadPath(path)) continue;
        unsigned long sz = e - s;
        if ((++nreg % 256) == 0) MCLOG("[vd] ..regions=%d count=%d", nreg, g_vdCount);
        if ((++nreg % 512) == 0) MCLOG("[vd] ..regions=%d count=%d", nreg, g_vdCount);
        for (unsigned long off = 0; off + 0x40 < sz; off += CH) {
            unsigned long len = sz - off; if (len > CH + 0x40) len = CH + 0x40;
            long got = vm_read(buf, s + off, len);
            if ((++nchunk % 64) == 0) MCLOG("[vd] chunk %d base=%lx len=%ld got=%ld cnt=%d", nchunk, (unsigned long)(s+off), len, got, g_vdCount);
            if (got <= 0) break;
            long i = 0;
            while ((i = MemFind(buf + i, got - i, MA, 8)) >= 0) {
                long base = i - 16;
                if (base >= 0) {
                    int v = 0; memcpy(&v, buf + base, 4);
                    if (v >= 5 && v <= 4096) {
                        uintptr_t t = s + off + base;
                        if (!g_vdRef) g_vdRef = v;
                        VdAdd(t); MCLOG("[vd] A %p v=%d", (void*)t, v);
                    }
                }
                i += 8;
            }
            i = 0;
            while ((i = MemFind(buf + i, got - i, MC, 8)) >= 0) {
                if (i + 16 <= got && buf[i+12] == 0x0c && buf[i+13] == 0 && buf[i+14] == 0 && buf[i+15] == 0) {
                    int v = 0; memcpy(&v, buf + i + 8, 4);
                    if (v >= 5 && v <= 4096) {
                        uintptr_t t = s + off + i + 8;
                        if (!g_vdRef) g_vdRef = v;
                        VdAdd(t); MCLOG("[vd] C %p v=%d", (void*)t, v);
                    }
                }
                i += 8;
            }
            i = 0;
            while ((i = MemFind(buf + i, got - i, MB, 8)) >= 0) {
                if (i + 32 <= got) {
                    int ok = 1;
                    for (int k = 8; k < 24; k++) if (buf[i + k]) { ok = 0; break; }
                    if (ok) {
                        int a = 0, b = 0;
                        memcpy(&a, buf + i + 24, 4); memcpy(&b, buf + i + 28, 4);
                        if (a == b && a >= 5 && a <= 4096) {
                            uintptr_t t = s + off + i + 24;
                            if (!g_vdRef) g_vdRef = a;
                            VdAdd(t); VdAdd(t + 4); MCLOG("[vd] B %p v=%d", (void*)t, a);
                        }
                    }
                }
                i += 8;
            }
        }
    }
    fclose(mf);
    free(buf);
    MCLOG("[vd] scan done count=%d ref=%d", g_vdCount, g_vdRef);
}

// ---- 安全扫描（vm_read 受保护）：相邻相等 int，向前回溯找虚表 ----
static int VdPread(int fd, void* dst, unsigned long src, unsigned long len)
{
    (void)fd;
    long r = vm_read(dst, (uintptr_t)src, len);
    return r > 0 ? (int)r : 0;
}

#define VD2_MAX 16384
struct VdHit { unsigned long vt; unsigned long delta; int v; };
static VdHit g_hitCur[VD2_MAX]; static int g_hitCurN = 0;
static VdHit g_hitPrev[VD2_MAX]; static int g_hitPrevN = 0;
static int       g_vd2Count = 0;
static int       g_vdDiffN = 0;
struct VdDiff { unsigned long vt; unsigned long delta; int a; int b; };
static VdDiff g_diff[64];

static void Vd2Scan()
{
    g_vd2Count = 0;
    memcpy(g_hitPrev, g_hitCur, sizeof(VdHit) * (size_t)g_hitCurN);
    g_hitPrevN = g_hitCurN;
    g_hitCurN = 0;
    FILE* mf = fopen("/proc/self/maps", "r");
    if (!mf) return;
    const unsigned long CH = 1u << 20;
    unsigned char* buf = (unsigned char*)malloc(CH + 0x80);
    if (!buf) { fclose(mf); return; }
    char line[512];
    unsigned long libEnd = g_libBase ? g_libBase + 0x14660000ull : 0;
    int nreg = 0, nhit = 0, nlog = 0;
    while (fgets(line, sizeof(line), mf)) {
        unsigned long s = 0, e = 0; char perms[8] = {0}, path[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &s, &e, perms, path);
        if (n < 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w' || perms[3] != 'p') continue;
        if (n >= 4 && IsBadPath(path)) continue;
        nreg++;
        unsigned long sz = e - s;
        if (sz > (1024ull << 20)) sz = 1024ull << 20;
        for (unsigned long off = 0; off + 0x1000 < sz; off += CH) {
            unsigned long len = sz - off; if (len > CH + 0x40) len = CH + 0x40;
            int got = VdPread(-1, buf, s + off, len);
            if (got <= 0) continue;
            for (int i = 0; i + 0x10 < got; i += 4) {
                int v = 0;
                memcpy(&v, buf + i, 4);
                if (v < 5 || v > 22) continue;
                int v2 = 0;
                memcpy(&v2, buf + i + 4, 4);
                if (v != v2) continue;
                unsigned long cand = s + off + (unsigned long)i;
                unsigned long base = 0, vtoff = 0;
                for (unsigned long back = 0; back < 0x3000; back += 8) {
                    unsigned long pos = cand - back;
                    unsigned long q = 0;
                    if (VdPread(-1, &q, pos, 8) != 8) break;
                    if (!g_libBase || q < g_libBase || q > libEnd) continue;
                    unsigned long slot0 = 0;
                    if (VdPread(-1, &slot0, q, 8) != 8) continue;
                    if (slot0 < g_libBase + 0x6834C80ull || slot0 > g_libBase + 0x14666000ull) continue;
                    base = pos; vtoff = q - g_libBase; break;
                }
                if (!base) continue;
                nhit++;
                if (g_hitCurN < VD2_MAX) { g_hitCur[g_hitCurN].vt = vtoff; g_hitCur[g_hitCurN].delta = cand - base; g_hitCur[g_hitCurN].v = v; g_hitCurN++; }
            }
        }
    }
    fclose(mf); free(buf);
    g_vdDiffN = 0;
    if (g_hitPrevN > 0) {
        for (int i = 0; i < g_hitCurN; i++) {
            for (int j = 0; j < g_hitPrevN; j++) {
                if (g_hitCur[i].vt != g_hitPrev[j].vt || g_hitCur[i].delta != g_hitPrev[j].delta) continue;
                if (g_hitCur[i].v == g_hitPrev[j].v) break;
                if (g_vdDiffN < 64) {
                    g_diff[g_vdDiffN].vt = g_hitCur[i].vt; g_diff[g_vdDiffN].delta = g_hitCur[i].delta;
                    g_diff[g_vdDiffN].a = g_hitPrev[j].v; g_diff[g_vdDiffN].b = g_hitCur[i].v;
                    g_vdDiffN++;
                    MCLOG("[vd3] CHANGED vt=0x%lx delta=0x%lx %d->%d", g_hitCur[i].vt, g_hitCur[i].delta, g_hitPrev[j].v, g_hitCur[i].v);
                }
                break;
            }
        }
    }
    MCLOG("[vd2] scan end regions=%d hits=%d diff=%d", nreg, nhit, g_vdDiffN);
}


static void LoadVdFile()
{
    FILE* f = fopen("/sdcard/vd_addr.txt", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '#') continue;
        uintptr_t a = (uintptr_t)strtoull(p, nullptr, 16);
        if (a > 0x10000) VdAdd(a);
    }
    fclose(f);
    MCLOG("[vd] file targets=%d", g_vdCount);
}

static volatile int g_vdScanning = 0;
static void* VdScanThread(void*)
{
    MCLOG("[vd] thread start");
    ScanVd();
    g_vdScanning = 0;
    return nullptr;
}
static void StartVdScanAsync()
{
    if (g_vdScanning) return;
    g_vdScanning = 1;
    pthread_t th;
    if (pthread_create(&th, nullptr, VdScanThread, nullptr) == 0) pthread_detach(th);
    else g_vdScanning = 0;
}

static void ApplyViewDist()
{
    if (g_vdCount == 0) { LoadVdFile(); }
    if (g_vdCount == 0) { MCLOG("[vd] no target"); return; }
    int v = g_viewDist; if (v < 5) v = 5;
    int ok = 0;
    for (int i = 0; i < g_vdCount; i++) {
        uintptr_t o = g_vdList[i];
        int old = -1;
        if (vm_read(&old, o, 4) <= 0) { MCLOG("[vd] skip %p unreadable", (void*)o); continue; }
        memcpy((void*)o, &v, 4);
        int chk = -1; vm_read(&chk, o, 4);
        MCLOG("[vd] w %p %d->%d now=%d", (void*)o, old, v, chk);
        ok++;
    }
    MCLOG("[vd] applied %d/%d -> %d", ok, g_vdCount, v);
}

static void DrawBall()
{
    const float D = 190.f;
    const float H = 92.f;
    if (g_ballX < 0.f) { g_ballX = (float)glWidth - D - 40.f; g_ballY = (float)glHeight * 0.34f; }

    ImGui::SetNextWindowPos(ImVec2(g_ballX, g_ballY));
    ImGui::SetNextWindowSize(ImVec2(D, H));
    ImGui::Begin("##mcball", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImVec2 wp = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a(wp.x + 2.f, wp.y + 2.f);
    ImVec2 b(wp.x + D - 2.f, wp.y + H - 2.f);
    dl->AddRectFilled(ImVec2(a.x + 4.f, a.y + 6.f), ImVec2(b.x + 4.f, b.y + 6.f), IM_COL32(0, 0, 0, 110), 20.f);
    dl->AddRectFilled(a, b, IM_COL32(38, 110, 220, 245), 20.f);
    dl->AddRect(a, b, IM_COL32(150, 200, 255, 255), 20.f, 0, 3.f);

    const char* t = "Tools";
    ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2((a.x + b.x) * 0.5f - ts.x * 0.5f, (a.y + b.y) * 0.5f - ts.y * 0.5f),
        IM_COL32_WHITE, t);

    ImGui::SetCursorScreenPos(wp);
    ImGui::InvisibleButton("##ballbtn", ImVec2(D, H));

    if (ImGui::IsItemActivated()) {
        g_dragging = false;
        g_pressX = ImGui::GetIO().MousePos.x;
        g_pressY = ImGui::GetIO().MousePos.y;
        g_ballStartX = g_ballX; g_ballStartY = g_ballY;
    }
    if (ImGui::IsItemActive()) {
        float dx = ImGui::GetIO().MousePos.x - g_pressX;
        float dy = ImGui::GetIO().MousePos.y - g_pressY;
        if (!g_dragging && (fabsf(dx) > 16.f || fabsf(dy) > 16.f)) g_dragging = true;
        if (g_dragging) {
            g_ballX = g_ballStartX + dx;
            g_ballY = g_ballStartY + dy;
            if (g_ballX < 0) g_ballX = 0;
            if (g_ballY < 0) g_ballY = 0;
            if (g_ballX > (float)glWidth - D) g_ballX = (float)glWidth - D;
            if (g_ballY > (float)glHeight - H) g_ballY = (float)glHeight - H;
        }
    }
    if (ImGui::IsItemDeactivated()) {
        if (!g_dragging) g_panelOpen = true;
        g_dragging = false;
    }
    ImGui::End();
}

// ---------- 右下角 FPS ----------
static void DrawFps()
{
    if (!g_showFps) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "FPS %.0f", ImGui::GetIO().Framerate);
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.f, 2.f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::Begin("##fps", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
    ImGui::TextColored(ImVec4(0.55f, 1.f, 0.65f, 1.f), "%s", buf);
    ImVec2 sz = ImGui::GetWindowSize();
    ImGui::SetWindowPos(ImVec2(24.f, (float)glHeight - sz.y - 24.f));
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

// ================= 物品 NBT 编辑器 =================
#define NBT_MAXE 400
struct NbtEnt { char path[160]; int type; int64_t iv; double dv; char sv[160]; };
static NbtEnt g_nbtE[NBT_MAXE];
static int    g_nbtN = 0;
static int    g_nbtSlot = 9;          // 快捷栏第 1 格
static char   g_idBuf[32] = "0x0";
static int    g_itemLegacyId = -1;
static int    g_srcSlot = 0;
static int    g_dstSlot = 0;
static int    g_itemAux = 0;
static int    g_nbtCnt = 0;
static bool   g_nbtLoaded = false;
static bool   g_autoNbt = false;

static uint64_t rd64v(uintptr_t a) { uint64_t v = 0; vm_read(&v, a, 8); return v; }
static uint32_t rd32v(uintptr_t a) { uint32_t v = 0; vm_read(&v, a, 4); return v; }
static int16_t  rds16v(uintptr_t a) { int16_t v = 0; vm_read(&v, a, 2); return v; }
static int8_t   rds8v(uintptr_t a)  { int8_t v = 0; vm_read(&v, a, 1); return v; }
static float    rdfv(uintptr_t a)   { float v = 0; vm_read(&v, a, 4); return v; }
static double   rddv(uintptr_t a)   { double v = 0; vm_read(&v, a, 8); return v; }

static int KeyOfNode(uintptr_t node, char* out, int osz)
{
    unsigned char c = 0;
    if (vm_read(&c, node + 0x20, 1) <= 0) return 0;
    int len = (int)c / 2;
    if (len <= 0 || len >= osz) return 0;
    if (vm_read(out, node + 0x21, (unsigned long)len) <= 0) return 0;
    out[len] = 0;
    return len;
}

static uintptr_t CmpFirst(uintptr_t tag, uintptr_t* rootOut)
{
    uintptr_t root = StripTag(rd64v(tag + 0x10));
    *rootOut = root;
    if (!root) return 0;
    uintptr_t n = root;
    for (int g = 0; g < 64; g++) {
        uintptr_t l = StripTag(rd64v(n + 0x00));
        if (!l) break;
        n = l;
    }
    return n;
}

static uintptr_t CmpNext(uintptr_t n, uintptr_t sentinel)
{
    uintptr_t r = StripTag(rd64v(n + 0x08));
    if (r) {
        for (int g = 0; g < 64; g++) {
            uintptr_t l = StripTag(rd64v(r + 0x00));
            if (!l) break;
            r = l;
        }
        return r;
    }
    uintptr_t cur = n;
    for (int g = 0; g < 64; g++) {
        uintptr_t par = StripTag(rd64v(cur + 0x10));
        if (!par || par == sentinel) return 0;
        uintptr_t pl = StripTag(rd64v(par + 0x00));
        if (pl == cur) return par;
        cur = par;
    }
    return 0;
}

static void CollectTagObj(uintptr_t tagObj, int t, const char* pref, int depth);

static void CollectCmpRec(uintptr_t node, const char* pref, int depth, int* safety)
{
    if (!node || *safety > 700 || g_nbtN >= NBT_MAXE) return;
    uintptr_t l = StripTag(rd64v(node + 0x00));
    uintptr_t r = StripTag(rd64v(node + 0x08));
    if (l) CollectCmpRec(l, pref, depth, safety);
    (*safety)++;
    char k[48]; k[0] = 0;
    KeyOfNode(node, k, sizeof k);
    int t = (int)rd32v(node + 0x60);
    char p[96];
    if (pref[0]) snprintf(p, sizeof p, "%s/%s", pref, k);
    else         snprintf(p, sizeof p, "%s", k);
    CollectTagObj(node + 0x38, t, p, depth);
    if (r) CollectCmpRec(r, pref, depth, safety);
}

static void CollectCmp(uintptr_t tag, const char* pref, int depth)
{
    if (depth > 6 || g_nbtN >= NBT_MAXE) return;
    uintptr_t root = StripTag(rd64v(tag + 0x10));
    if (!root) return;
    int safety = 0;
    CollectCmpRec(root, pref, depth, &safety);
}

static void CollectTagObj(uintptr_t tagObj, int t, const char* pref, int depth)
{
    if (t == 10) { CollectCmp(tagObj, pref, depth + 1); return; }
    if (t == 9) {
        uintptr_t b = StripTag(rd64v(tagObj + 0x08));
        uintptr_t e = StripTag(rd64v(tagObj + 0x10));
        int n = (int)((e - b) / 8);
        if (n < 0) n = 0;
        if (n > 64) n = 64;
        for (int i = 0; i < n && g_nbtN < NBT_MAXE; i++) {
            uintptr_t el = StripTag(rd64v(b + (uintptr_t)i * 8));
            if (!el) continue;
            char pp[160];
            snprintf(pp, sizeof pp, "%s[%d]", pref, i);
            uintptr_t ev = StripTag(rd64v(el + 0));
            int isCmp = (ev == (uintptr_t)(g_libBase + 0x13EBE708u));
            if (isCmp) { CollectCmp(el, pp, depth + 1); }
            else {
                if (g_nbtN >= NBT_MAXE) break;
                NbtEnt* ee = &g_nbtE[g_nbtN];
                memset(ee, 0, sizeof(NbtEnt));
                snprintf(ee->path, sizeof ee->path, "%s", pp);
                ee->type = 3;
                ee->iv = (int64_t)(int32_t)rd32v(el + 0x08);
                g_nbtN++;
            }
        }
        return;
    }
    if (g_nbtN >= NBT_MAXE) return;
    NbtEnt* e = &g_nbtE[g_nbtN];
    memset(e, 0, sizeof(NbtEnt));
    snprintf(e->path, sizeof e->path, "%s", pref);
    e->type = t;
    switch (t) {
        case 1: e->iv = (int64_t)rds8v(tagObj + 0x08); break;
        case 2: e->iv = (int64_t)rds16v(tagObj + 0x08); break;
        case 3: e->iv = (int64_t)(int32_t)rd32v(tagObj + 0x08); break;
        case 4: e->iv = (int64_t)rd64v(tagObj + 0x08); break;
        case 5: e->dv = (double)rdfv(tagObj + 0x08); break;
        case 6: e->dv = rddv(tagObj + 0x08); break;
        case 8: {
            unsigned char raw[160];
            if (vm_read(raw, tagObj + 0x08, 160) > 0) {
                int len = (int)raw[0], off = 1;
                if (len >= 0x80) { len = (len & 0x7F) | ((int)raw[1] << 7); off = 2; }
                if (len > 0 && len < 150) {
                    memcpy(e->sv, raw + off, (size_t)len);
                    e->sv[len] = 0;
                }
            }
            break;
        }
        default: return;
    }
    g_nbtN++;
}


static uintptr_t FindKeyRec(uintptr_t node, const char* key)
{
    if (!node) return 0;
    uintptr_t got = FindKeyRec(StripTag(rd64v(node + 0x00)), key);
    if (got) return got;
    char k[48]; k[0] = 0;
    if (KeyOfNode(node, k, sizeof k) && !strcmp(k, key)) return node;
    return FindKeyRec(StripTag(rd64v(node + 0x08)), key);
}

static uintptr_t ResolveCmpKey(uintptr_t tag, const char* key)
{
    uintptr_t root = StripTag(rd64v(tag + 0x10));
    if (!root) return 0;
    return FindKeyRec(root, key);
}

static uintptr_t ResolvePath(uintptr_t tag, const char* path)
{
    uintptr_t cur = tag;
    const char* p = path;
    if (!*p) return 0;
    while (*p) {
        char seg[48]; int si = 0;
        while (*p && *p != '/' && *p != '[' && si < 47) seg[si++] = *p++;
        seg[si] = 0;
        int idx = -1;
        if (*p == '[') {
            p++; idx = 0;
            while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
            while (*p && *p != ']') p++;
            if (*p == ']') p++;
        }
        if (si > 0) {
            uintptr_t node = ResolveCmpKey(cur, seg);
            if (!node) return 0;
            int t = (int)rd32v(node + 0x60);
            if (t == 10 || t == 9) cur = node + 0x38;
            else return node;      // 叶子
        }
        if (idx >= 0) {
            uintptr_t b = StripTag(rd64v(cur + 0x08));
            uintptr_t el = StripTag(rd64v(b + (uintptr_t)idx * 8));
            if (!el) return 0;
            cur = el;
        }
        if (*p == '/') p++;
    }
    return cur;
}

static void WriteLeaf(uintptr_t node, int t, int64_t iv, double dv)
{
    switch (t) {
        case 1: { int8_t v = (int8_t)iv; vm_write(nullptr, node + 0x40, &v, 1); break; }
        case 2: { int16_t v = (int16_t)iv; vm_write(nullptr, node + 0x40, &v, 2); break; }
        case 3: { int32_t v = (int32_t)iv; vm_write(nullptr, node + 0x40, &v, 4); break; }
        case 4: { int64_t v = iv; vm_write(nullptr, node + 0x40, &v, 8); break; }
        case 5: { float v = (float)dv; vm_write(nullptr, node + 0x40, &v, 4); break; }
        case 6: { double v = dv; vm_write(nullptr, node + 0x40, &v, 8); break; }
        default: break;
    }
}

static void NbtRefresh()
{
    g_nbtN = 0;
    if (!g_invCntFound) FindInventoryNow();
    if (!g_invCntFound) { MCLOG("[nbt] no inv"); return; }
    g_invBegin = g_invList[0]; g_invN = g_invListN[0];
    if (g_nbtSlot < 0) {
        int sel = 0;
        vm_read(&sel, g_piObj[0] + 0x10, 4);
        if (sel < 0 || sel > 8) sel = 0;
        g_nbtSlot = sel;   // 0..8 = 快捷栏 1..9
    }
    MCLOG("[nbt] held slot=%d", g_nbtSlot);
    uintptr_t ps = SlotAddr(g_nbtSlot);
    if (!ps) { MCLOG("[nbt] slot addr fail"); return; }
    uintptr_t item = StripTag(rd64v(ps + 0x08));
    {
        int packed = CallGetItemId(ps);
        g_itemLegacyId = (packed > 0) ? (packed >> 16) : -1;
        g_itemAux = (packed > 0) ? (packed & 0xFFFF) : 0;
        if (g_itemLegacyId >= 0) snprintf(g_idBuf, sizeof g_idBuf, "%d", g_itemLegacyId);
        else                     snprintf(g_idBuf, sizeof g_idBuf, "0x%lx", (unsigned long)item);
    }
    { unsigned char c8 = 0; vm_read(&c8, ps + 0x22, 1); g_nbtCnt = (int)c8; }
    {
        uintptr_t blk = StripTag(rd64v(ps + 0x18));
        int b18 = 0, b1a = 0, i18 = 0, i1a = 0;
        if (blk) { vm_read(&b18, blk + 0x18, 2); vm_read(&b1a, blk + 0x1a, 2); }
        if (item) { vm_read(&i18, item + 0x18, 2); vm_read(&i1a, item + 0x1a, 2); }
        MCLOG("[id] stack=0x%lx legacyid=%d", (unsigned long)ps, CallGetItemId(ps));
        MCLOG("[nbt] ps=0x%lx item=0x%lx blk=0x%lx b18=%d b1a=%d i18=%d i1a=%d",
              (unsigned long)ps, (unsigned long)item, (unsigned long)blk, b18, b1a, i18, i1a);
    }
    uintptr_t tag = StripTag(rd64v(ps + 0x10));
    g_nbtLoaded = true;
    if (!tag) { MCLOG("[nbt] slot%d item=0x%lx no tag", g_nbtSlot, (unsigned long)item); return; }
    CollectCmp(tag, "", 0);
    MCLOG("[nbt] slot%d item=0x%lx entries=%d", g_nbtSlot, (unsigned long)item, g_nbtN);
}

static void NbtApplyAll(const char* path, int t, int64_t iv, double dv)
{
    int done = 0;
    for (int q = 0; q < g_invCntFound; q++) {
        uintptr_t ps = g_invList[q] + (uintptr_t)g_nbtSlot * 0x98;
        uintptr_t tag = StripTag(rd64v(ps + 0x10));
        if (!tag) continue;
        uintptr_t node = ResolvePath(tag, path);
        if (!node) continue;
        WriteLeaf(node, t, iv, dv);
        done++;
    }
    MCLOG("[nbt] set %s type=%d val=%lld on %d inv", path, t, (long long)iv, done);
}

static void CountApplyAll(int c)
{
    unsigned char v = (unsigned char)(c < 1 ? 1 : (c > 255 ? 255 : c));
    for (int q = 0; q < g_invCntFound; q++) {
        uintptr_t ps = g_invList[q] + (uintptr_t)g_nbtSlot * 0x98;
        vm_write(nullptr, ps + 0x22, &v, 1);
    }
    MCLOG("[nbt] count=%d applied", (int)v);
}

static void NbtDumpSlot(int slot)
{
    g_nbtSlot = slot;
    NbtRefresh();
    MCLOG("[nbt] dump slot=%d entries=%d", slot, g_nbtN);
    for (int i = 0; i < g_nbtN; i++) {
        NbtEnt* e = &g_nbtE[i];
        if (e->type == 5 || e->type == 6)      MCLOG("[nbt]   %s (%s) = %.4f", e->path, NbtTypeName(e->type), e->dv);
        else if (e->type == 8)                  MCLOG("[nbt]   %s (String) = [%s]", e->path, e->sv);
        else                                    MCLOG("[nbt]   %s (%s) = %lld", e->path, NbtTypeName(e->type), (long long)e->iv);
    }
}

static const char* NbtTypeName(int t)
{
    switch (t) {
        case 1: return "Byte"; case 2: return "Short"; case 3: return "Int"; case 4: return "Long";
        case 5: return "Float"; case 6: return "Double"; case 8: return "String";
        default: return "?";
    }
}


// ================= 悬浮数字键盘 =================
static int   g_kpTarget = -2;   // -2 未选, -1 数量, >=0 标签行
static char  g_kpBuf[40] = "";
static int   g_kpPos = 0;
static float g_panelX = 0.f, g_panelY = 0.f, g_panelW = 0.f;

static void KpLoad(int t)
{
    MCLOG("[kp] load t=%d n=%d", t, g_nbtN);
    if (t >= g_nbtN) t = -2;
    g_kpTarget = t;
    g_kpBuf[0] = 0; g_kpPos = 0;   // 选中后直接清空，直接输新值
}

static void KpIns(char c)
{
    int n = (int)strlen(g_kpBuf);
    if (n >= 30) return;
    if (g_kpPos < 0) g_kpPos = 0;
    if (g_kpPos > n) g_kpPos = n;
    memmove(g_kpBuf + g_kpPos + 1, g_kpBuf + g_kpPos, (size_t)(n - g_kpPos + 1));
    g_kpBuf[g_kpPos] = c;
    g_kpPos++;
}

static void KpDel(void)
{
    int n = (int)strlen(g_kpBuf);
    if (g_kpPos > 0 && g_kpPos <= n) {
        memmove(g_kpBuf + g_kpPos - 1, g_kpBuf + g_kpPos, (size_t)(n - g_kpPos + 1));
        g_kpPos--;
    }
}

static void KpApply(void)
{
    if (g_kpTarget == -2) return;
    long long v = atoll(g_kpBuf);
    double d = atof(g_kpBuf);
    if (g_kpTarget == -1) {
        int c = (int)v; if (c < 1) c = 1; if (c > 255) c = 255;
        g_nbtCnt = c;
        CountApplyAll(c);
        MCLOG("[kp] count=%d applied", c);
        return;
    }
    if (g_kpTarget < 0 || g_kpTarget >= g_nbtN) return;
    NbtEnt* e = &g_nbtE[g_kpTarget];
    if (e->type == 8) return;
    if (e->type == 5 || e->type == 6) e->dv = d;
    else e->iv = v;
    NbtApplyAll(e->path, e->type, e->iv, e->dv);
    MCLOG("[kp] %s type=%d val=%lld", e->path, e->type, (long long)e->iv);
}

static int g_kpFrame = 0;

static void DrawKeypadFloat(void)
{
    if ((g_kpFrame++ % 120) == 0) MCLOG("[kp] float f=%d tgt=%d cnt=%d", g_kpFrame, g_kpTarget, g_nbtCnt);
    ImGui::SetNextWindowSize(ImVec2(340.f, 520.f), ImGuiCond_FirstUseEver);
    {
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        float x = ds.x * 0.62f;
        if (x + 340.f > ds.x - 8.f) x = ds.x - 340.f - 8.f;
        if (x < 8.f) x = 8.f;
        float y = (ds.y - 520.f) * 0.5f;
        if (y < 8.f) y = 8.f;
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
    }
    ImGui::Begin("数字键盘##kp", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    {
        char show[48] = {0}; int k = 0, n = (int)strlen(g_kpBuf);
        for (int i = 0; i <= n && k < 44; i++) {
            if (i == g_kpPos) show[k++] = '|';
            if (i < n) show[k++] = g_kpBuf[i];
        }
        ImGui::TextColored(ImVec4(1.f, 0.92f, 0.6f, 1.f), "%s", show[0] ? show : "|");
    }
    const ImVec2 bs(84.f, 62.f);
    const char* keys[3][3] = { {"1","2","3"}, {"4","5","6"}, {"7","8","9"} };
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            if (c) ImGui::SameLine(0.f, 8.f);
            if (ImGui::Button(keys[r][c], bs)) KpIns(keys[r][c][0]);
        }
    }
    if (ImGui::Button("←", bs)) { if (g_kpPos > 0) g_kpPos--; }
    ImGui::SameLine(0.f, 8.f);
    if (ImGui::Button("0", bs)) KpIns('0');
    ImGui::SameLine(0.f, 8.f);
    if (ImGui::Button("→", bs)) { if (g_kpPos < (int)strlen(g_kpBuf)) g_kpPos++; }
    if (ImGui::Button("删除", ImVec2(130.f, 56.f))) KpDel();
    ImGui::SameLine(0.f, 8.f);
    if (ImGui::Button("-", bs)) KpIns('-');
    ImGui::SameLine(0.f, 8.f);
    if (ImGui::Button(".", bs)) KpIns('.');
    if (ImGui::Button("清空", ImVec2(130.f, 56.f))) { g_kpBuf[0] = 0; g_kpPos = 0; }
    ImGui::SameLine(0.f, 8.f);
    if (ImGui::Button("写入（客户端+服务端）", ImVec2(-1.f, 56.f))) KpApply();
    if ((g_kpFrame % 120) == 0) MCLOG("[kp] float drawn ok");
    ImGui::End();
}


static char  g_verStr[40] = "unknown";
static bool  g_verOK = false;
static const char* kSupportedVer = "1.26.62.1";

static void DetectVersion(void)
{
    static unsigned char* sb = nullptr;
    const unsigned long CH = 1u << 20;
    if (!sb) sb = (unsigned char*)malloc(CH);
    if (!sb) return;
    FILE* mf = fopen("/proc/self/maps", "r");
    if (!mf) return;
    char line[512];
    int regions = 0;
    char best[40] = {0};
    while (fgets(line, sizeof(line), mf)) {
        unsigned long s = 0, e = 0; char perms[8] = {0}, path[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &s, &e, perms, path);
        if (n < 3 || perms[0] != 'r') continue;
        if (!strstr(path, "libminecraftpe.so")) continue;
        regions++;
        unsigned long sz = e - s;
        for (unsigned long off = 0; off + 16 < sz; off += CH) {
            unsigned long len = sz - off; if (len > CH) len = CH;
            long got = vm_read(sb, s + off, len);
            if (got <= 0) break;
            for (long i = 1; i + 8 < got; i++) {
                if (!isdigit((unsigned char)sb[i - 1])) continue;
                if (isdigit((unsigned char)sb[i - 2]) || sb[i - 2] == '.') continue;
                // 尝试解析 d.d.d.d
                char tmp[40]; int k = 0; long j = i - 1; int dots = 0, glen = 0, ok = 1;
                for (; j < got && k < 38; j++) {
                    char c = (char)sb[j];
                    if (isdigit((unsigned char)c)) { glen++; if (glen > 3) { ok = 0; break; } tmp[k++] = c; }
                    else if (c == '.') {
                        if (glen == 0 || dots >= 3) { ok = 0; break; }
                        dots++; glen = 0; tmp[k++] = c;
                    } else break;
                }
                if (!ok || dots != 3 || glen == 0) continue;
                if (j < got && (isdigit((unsigned char)sb[j]) || sb[j] == '.')) continue;
                tmp[k] = 0;
                if (strlen(tmp) < 7) continue;
                int p0 = 0, p1 = 0, p2 = 0, p3 = 0;
                if (sscanf(tmp, "%d.%d.%d.%d", &p0, &p1, &p2, &p3) != 4) continue;
                if (!(p0 == 1 && p1 >= 5 && p1 <= 60)) continue;   // 只要 1.x 形式的游戏版本
                MCLOG("[ver] cand %s", tmp);
                if (!best[0]) snprintf(best, sizeof best, "%s", tmp);
                else {
                    int q0 = 0, q1 = 0, q2 = 0, q3 = 0;
                    sscanf(best, "%d.%d.%d.%d", &q0, &q1, &q2, &q3);
                    if (q1 < p1 || (q1 == p1 && (q2 < p2 || (q2 == p2 && q3 < p3))))
                        snprintf(best, sizeof best, "%s", tmp);
                }
                if (!strcmp(tmp, kSupportedVer)) goto done;
            }
        }
        if (regions >= 3) break;
    }
done:
    fclose(mf);
    snprintf(g_verStr, sizeof g_verStr, "%s", best[0] ? best : "unknown");
    {
        int a = 0, b = 0, c = 0, d = 0;
        bool parsed = (sscanf(g_verStr, "%d.%d.%d.%d", &a, &b, &c, &d) == 4);
        g_verOK = parsed && (a == 1 && b == 26 && c == 62 && d == 1);
    }
    MCLOG("[ver] detected=%s supported=%d", g_verStr, (int)g_verOK);
}


#define IMP_MAX 200
static char g_impPath[256] = "/sdcard/nbt_test.json";
static char g_impDir[256]  = "/sdcard";
static char g_impName[IMP_MAX][128];
static int  g_impIsDir[IMP_MAX];
static int  g_impN = 0;
static bool g_impBrowse = false;
static int  g_impMsg = 0;
static char g_impMsgText[192] = "";

static void ImpRefresh()
{
    g_impN = 0;
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "ls -1p '%s' 2>/dev/null | head -n %d", g_impDir, IMP_MAX);
    FILE* f = popen(cmd, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f) && g_impN < IMP_MAX) {
        int n = (int)strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (!n) continue;
        int isdir = (line[n-1] == '/');
        if (isdir) line[n-1] = 0;
        snprintf(g_impName[g_impN], sizeof(g_impName[0]), "%s", line);
        g_impIsDir[g_impN] = isdir;
        g_impN++;
    }
    pclose(f);
}

static void DrawNbtImportSection()
{
    ImGui::Dummy(ImVec2(0.f, 8.f));
    ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "NBT JSON 导入（整体替换手持 NBT）");
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputText("##impPath", g_impPath, sizeof(g_impPath));
    if (ImGui::Button("浏览文件夹", ImVec2(160.f, 0.f))) {
        g_impBrowse = !g_impBrowse;
        if (g_impBrowse) ImpRefresh();
    }
    ImGui::SameLine();
    if (ImGui::Button("导入到手持", ImVec2(220.f, 0.f))) {
        FindInventoryNow();
        int ok = NbImportJsonFile(g_impPath);
        g_impMsg = ok ? 1 : 2;
        snprintf(g_impMsgText, sizeof(g_impMsgText), "%s: %s", ok ? "导入成功" : "导入失败", g_impPath);
        MCLOG("[ui] import json %s -> %d", g_impPath, ok);
    }
    if (g_impMsg) {
        ImGui::SameLine();
        if (g_impMsg == 1) ImGui::TextColored(ImVec4(0.5f, 1.f, 0.6f, 1.f), "%s", g_impMsgText);
        else               ImGui::TextColored(ImVec4(1.f, 0.45f, 0.45f, 1.f), "%s", g_impMsgText);
    }

    if (g_impBrowse) {
        ImGui::Dummy(ImVec2(0.f, 4.f));
        ImGui::TextDisabled("目录: %s    (点目录进入，点文件选中)", g_impDir);
        ImGui::BeginChild("##impbrowse", ImVec2(-1.f, 300.f), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        char label[192];
        snprintf(label, sizeof(label), ".. 上级目录");
        if (ImGui::Selectable(label, false)) {
            char* sl = strrchr(g_impDir, '/');
            if (sl && sl != g_impDir) *sl = 0; else snprintf(g_impDir, sizeof(g_impDir), "/");
            ImpRefresh();
        }
        for (int i = 0; i < g_impN; i++) {
            snprintf(label, sizeof(label), "%s%s", g_impIsDir[i] ? "[目录] " : "[文件] ", g_impName[i]);
            if (ImGui::Selectable(label, false)) {
                if (g_impIsDir[i]) {
                    snprintf(g_impDir, sizeof(g_impDir), "%s/%s", g_impDir, g_impName[i]);
                    ImpRefresh();
                } else {
                    snprintf(g_impPath, sizeof(g_impPath), "%s/%s", g_impDir, g_impName[i]);
                    g_impBrowse = false;
                }
            }
        }
        ImGui::EndChild();
    }
}

// ===== 瞄准模块（jni/Mod/Aim.c）=====
extern "C" {
void  aim_start(float radius, int mask);
void  aim_stop(void);
void  aim_set_enabled(int on);
int   aim_enabled(void);
void  aim_set_mask(int m);
int   aim_mask(void);
void  aim_set_radius(float r);
float aim_radius(void);
int   aim_has(void);
int   aim_cat(void);
float aim_dist(void);
const char* aim_name(void);
const char* aim_catname_of(int c);
int   aim_vpt_count(void);
int   aim_obj_count(void);
}

float g_uiRect[4] = { 0.f, 0.f, 0.f, 0.f };   /* 当前 UI 窗口屏幕矩形 x,y,w,h */
int   g_blurOn = 1;                        /* 背景高斯模糊开关 */
int   g_blurStrength = 3;                 /* 模糊强度 1..200 */
bool  g_uiVisible = false;

int g_swCount = 0;
bool mod_ui_swallow(float x, float y) {
    if (!g_panelOpen || !g_uiVisible) return false;
    float m = 8.f;
    bool in = (x >= g_uiRect[0] - m && x <= g_uiRect[0] + g_uiRect[2] + m &&
               y >= g_uiRect[1] - m && y <= g_uiRect[1] + g_uiRect[3] + m);
    if (in) {
        if (g_swCount < 6) {
            g_swCount++;
            FILE* _f = fopen("/data/data/com.mojang.minecraftpe/files/mcstage.log", "a");
            if (_f) { fprintf(_f, "[touch] swallow %.0f,%.0f rect=%.0f,%.0f,%.0fx%.0f", x, y, g_uiRect[0], g_uiRect[1], g_uiRect[2], g_uiRect[3]); fputc(10, _f); fclose(_f); }
        }
    }
    return in;
}

static IslandContext s_island;
static bool s_islandInit = false;


void DrawPanel()
{
    float SW = (float)glWidth, SH = (float)glHeight;
    float W = SW * 0.94f; if (W > 1200.f) W = 1200.f;
    float H = SH * 0.56f; if (H < 1000.f) H = 1000.f; if (H > SH - 200.f) H = SH - 200.f;
    if (!s_islandInit) {
        s_islandInit = true;
        s_island.userX = (SW - W) * 0.5f;
        s_island.userY = (SH - H) * 0.5f;
        s_island.userW = W;
        s_island.userH = H;
        s_island.curX = s_island.userX; s_island.curY = s_island.userY;
        s_island.curW = s_island.userW; s_island.curH = s_island.userH;
        s_island.state = STATE_FULL;
    }
    ImGuiIO& io = ImGui::GetIO();
    float dt = io.DeltaTime; if (dt <= 0.f || dt > 0.2f) dt = 1.f / 60.f;
    char mini_txt[200];
    if (aim_enabled() && aim_has())
        snprintf(mini_txt, sizeof mini_txt, "MCMOD  ·  %s %.1f 格", aim_name(), aim_dist());
    else if (aim_enabled())
        snprintf(mini_txt, sizeof mini_txt, "MCMOD  ·  范围内无目标");
    else
        snprintf(mini_txt, sizeof mini_txt, "MCMOD  ·  辅助菜单");
    {
        float twpx = ImGui::CalcTextSize(mini_txt).x * 1.55f;
        float mw = twpx + 150.f;
        if (mw < 640.f) mw = 640.f;
        if (mw > SW * 0.86f) mw = SW * 0.86f;
        s_island.miniW = mw;
        if (s_island.state == STATE_MINIMIZED && !s_island.transitioning && fabsf(s_island.curW - mw) > 6.f)
            s_island.transitioning = true;
    }
    Island_UpdateTarget(s_island, SW, SH, dt);


    bool mini = (s_island.state == STATE_MINIMIZED);
    if (!s_island.transitioning && s_island.contentAlpha < 0.15f) mini = true;
    float rx, ry, rw, rh;
    if (s_island.transitioning) { rx = s_island.curX; ry = s_island.curY; rw = s_island.curW; rh = s_island.curH; }
    else if (mini) { rw = s_island.miniW; rh = s_island.miniH; rx = (SW - rw) * 0.5f; ry = s_island.miniY; }
    else { rx = s_island.userX; ry = s_island.userY; rw = s_island.userW; rh = s_island.userH; }
    if (rx < 0.f) rx = 0.f;
    if (ry < 0.f) ry = 0.f;
    if (rx + rw > SW) rw = SW - rx;
    if (ry + rh > SH) rh = SH - ry;

    float round_ = (rw < 1000.f) ? 26.f : 20.f;

    /* 模糊贴图与抓帧矩形必须一致：都用上一帧矩形；过渡动画期间不画模糊（避免拉伸闪烁） */
    bool blurOK = (g_blurOn != 0) && g_uiVisible;
    float brx = g_uiRect[0], bry = g_uiRect[1], brw = g_uiRect[2], brh = g_uiRect[3];
    if (brw < 24.f || brh < 24.f) { brx = rx; bry = ry; brw = rw; brh = rh; }
    BlurBg::draw(SW, SH, brx, bry, brw, brh, rx, ry, rw, rh, round_, g_blurStrength, blurOK);
    ImGui::SetWindowFontScale(1.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, round_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.f, 16.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.f, 10.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.f, 12.f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, mini ? ImVec4(0.05f, 0.06f, 0.09f, 0.45f)
                                                 : ImVec4(0.05f, 0.06f, 0.09f, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.35f, 0.55f, 0.95f, 0.30f));
    ImGui::SetNextWindowPos(ImVec2(rx, ry), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(rw, rh), ImGuiCond_Always);
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##islandwin", nullptr, wf);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    g_uiVisible = true;
    g_uiRect[0] = wp.x; g_uiRect[1] = wp.y; g_uiRect[2] = rw; g_uiRect[3] = rh;
    g_panelX = wp.x; g_panelY = wp.y; g_panelW = rw;

    if (mini) {
        ImVec2 ts = ImGui::CalcTextSize(mini_txt);
        ts.x *= 1.55f; ts.y *= 1.55f;
        dl->AddText(ImVec2(wp.x + (rw - ts.x) * 0.5f, wp.y + (rh - ts.y) * 0.5f),
                    IM_COL32(170, 235, 255, 255), mini_txt);
        ImGui::SetCursorPos(ImVec2(0.f, 0.f));
        ImGui::InvisibleButton("##mini", ImVec2(rw, rh));
        bool miniTap = ImGui::IsItemActivated() || ImGui::IsItemClicked(0);
        if (!miniTap) {
            ImVec2 m = ImGui::GetIO().MousePos;
            miniTap = ImGui::IsMouseClicked(0) && m.x >= wp.x && m.x <= wp.x + rw && m.y >= wp.y && m.y <= wp.y + rh;
        }
        if (miniTap) {
            s_island.curX = wp.x; s_island.curY = wp.y; s_island.curW = rw; s_island.curH = rh;
            s_island.state = STATE_FULL; s_island.transitioning = true;
            s_island.contentAlpha = 0.f;
        }
        ImGui::End();
        ImGui::PopStyleVar(5);
        ImGui::PopStyleColor(2);
        return;
    }

    float stripH = 96.f;
    float dotY = wp.y + stripH * 0.5f;
    float dotX[3];
    dotX[0] = wp.x + 50.f; dotX[1] = wp.x + 104.f; dotX[2] = wp.x + 158.f;

    int hit = -1;
    static const char* kDotId[3] = { "##dotR", "##dotY", "##dotG" };
    ImVec2 dotC[3];
    for (int i = 0; i < 3; ++i) {
        ImGui::SetCursorPos(ImVec2(0.f + 100.f * (float)i, 0.f));
        ImGui::InvisibleButton(kDotId[i], ImVec2(100.f, 100.f));
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        dotC[i] = ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        if (ImGui::IsItemActivated()) hit = i;
    }

    for (int i = 0; i < 3; ++i) {
        ImU32 c = (i == 0) ? IM_COL32(255, 95, 87, 255) : ((i == 1) ? IM_COL32(255, 189, 46, 255) : IM_COL32(40, 205, 90, 255));
        dl->AddCircleFilled(dotC[i], 15.f, c, 24);
    }

    if (hit == 0) {
        g_panelOpen = false;
    } else if (hit == 1) {
        s_island.curX = wp.x; s_island.curY = wp.y; s_island.curW = rw; s_island.curH = rh;
        s_island.state = STATE_MINIMIZED; s_island.transitioning = true;
        s_island.contentAlpha = 1.f;
    } else if (hit == 2) {
        float cx = (SW - W) * 0.5f, cy = (SH - H) * 0.5f;
        s_island.state = STATE_FULL;
        s_island.transitioning = false;
        s_island.contentAlpha = 1.f;
        s_island.userX = cx; s_island.userY = cy; s_island.userW = W; s_island.userH = H;
        s_island.curX = cx; s_island.curY = cy; s_island.curW = W; s_island.curH = H;
        ImGui::SetWindowPos(ImVec2(cx, cy));
        ImGui::SetWindowSize(ImVec2(W, H));
    }

    ImGui::SetCursorPos(ImVec2(210.f, 0.f));
    ImGui::InvisibleButton("##dragstrip", ImVec2(rw - 230.f, stripH));
    bool stripActive = ImGui::IsItemActive();

    if (hit < 0 && stripActive && ImGui::IsMouseDragging(0)) {
        float nx = wp.x + io.MouseDelta.x, ny = wp.y + io.MouseDelta.y;
        if (nx < 0.f) nx = 0.f;
        if (ny < 0.f) ny = 0.f;
        if (nx + rw > SW) nx = SW - rw;
        if (ny + rh > SH) ny = SH - rh;
        s_island.userX = nx; s_island.userY = ny;
    }

    ImGui::SetCursorPos(ImVec2(rw - 150.f, stripH * 0.5f - 22.f));
    ImGui::TextColored(ImVec4(0.72f, 0.82f, 1.f, 0.95f), "MCMOD");

    ImGui::SetCursorPos(ImVec2(20.f, stripH));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::BeginChild("##body", ImVec2(rw - 40.f, rh - stripH - 20.f), false);

    ImGui::TextColored(ImVec4(0.75f, 0.82f, 0.9f, 1.f), "版本 %s", g_verStr);
    ImGui::SameLine();
    if (g_verOK) ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.6f, 1.f), "  版本支持");
    else         ImGui::TextColored(ImVec4(1.f, 0.45f, 0.45f, 1.f), "  版本不支持");
    if (!g_verOK) ImGui::BeginDisabled(true);
    ImGui::SeparatorText("联机");
    ImGui::SetNextItemWidth(-1.f);
    int cur = g_target;
    if (ImGui::SliderInt("##mp", &cur, 1, 999, "%d 人")) {
        g_target = cur; ApplyMax(); SaveCfg(cur);
    }

    ImGui::SeparatorText("显示");
    ImGui::Checkbox("显示帧率 (FPS)", &g_showFps);
    ImGui::Checkbox("背景高斯模糊", (bool*)&g_blurOn);
    if (g_blurOn) {
        ImGui::SetNextItemWidth(-1.f);
        int bl = g_blurStrength;
        if (ImGui::SliderInt("##blur", &bl, 1, 200, "模糊强度 %d")) g_blurStrength = bl;
    }

    g_levelAddr = (unsigned long)g_level;

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(2);
}

static void ScanVd();
static void Vd2Scan();
static volatile int g_vd2Run = 0;
static void* Vd2Thread(void*) { MCLOG("[vd2] thread start"); Vd2Scan(); g_vd2Run = 0; return nullptr; }
static void StartVd2ScanAsync() { if (g_vd2Run) return; g_vd2Run = 1; pthread_t t; if (pthread_create(&t,nullptr,Vd2Thread,nullptr)==0) pthread_detach(t); else g_vd2Run = 0; }

// ====== 视角矩阵劫持（aim）======
volatile float g_trueYaw = 0.f, g_truePitch = 0.f;
volatile int   g_aimOn = 0;
volatile int   g_aimMode = 0;
static float* g_aimAddr[8];
static volatile int g_aimAddrCount = 0;   // 0=只dump不写 1=写非转置M 2=写转置T
volatile float g_aimYaw = 0.f, g_aimMatPitch = 0.f;
volatile unsigned long g_camCi = 0;
typedef void (*fn_cp16)(void*, const void*);
static fn_cp16 o_cp16 = nullptr;
typedef void (*fn_camup)(void*);
static fn_camup o_camup = nullptr;
static bool g_cp16Hooked = false;

static void BuildViewMat(float yawDeg, float matPitchDeg, float* m)
{
    const float D = 3.14159265358979f / 180.f;
    float y = yawDeg * D, p = matPitchDeg * D;
    float sy = sinf(y), cy = cosf(y), sp = sinf(p), cp = cosf(p);
    m[0] = -cy;   m[1] = -sp*sy; m[2]  = sy*cp;  m[3] = 0.f;
    m[4] = 0.f;   m[5] = cp;     m[6]  = sp;     m[7] = 0.f;
    m[8] = -sy;   m[9] = sp*cy;  m[10] = -cy*cp; m[11]= 0.f;
    m[12]= 0.f;   m[13]= 0.f;    m[14] = 0.f;   m[15]= 1.f;
}

static void* (*o_gcsrc)(void*) = nullptr;
static float DecodeYawDeg(const float* m);
static void BuildViewMat(float yawDeg, float matPitchDeg, float* m);

static void* my_gcsrc(void* a)
{
    void* r = o_gcsrc ? o_gcsrc(a) : nullptr;
    static int once = 0;
    if (!once && r && access("/data/data/com.mojang.minecraftpe/files/mcdump", F_OK) == 0) {
        once = 1;
        float* p = (float*)r;
        MCLOG("[aim] camsrc obj=%p a1=%p", r, a);
        for (int i = 0; i < 24; i++) MCLOG("[aim]  c[%02d]=%.6f", i, p[i]);
    }
    if (g_aimOn && g_aimMode == 3 && r) {
        float yawDeg = g_aimYaw;
        if (yawDeg > 998.0f || yawDeg < -998.0f) yawDeg = DecodeYawDeg((float*)r);
        float m[16];
        BuildViewMat(yawDeg, g_aimMatPitch, m);
        float* dr = (float*)r;
        for (int i = 0; i < 16; i++) dr[i] = m[i];
    }
    return r;
}

typedef void (*fn_unimtx)(int, int, unsigned char, const float*);
static fn_unimtx o_unimtx = nullptr;
static int g_uniLog = 0;

static void my_unimtx(int loc, int count, unsigned char tr, const float* v)
{
    /* ESP 用：把镜头上传的 4x4 矩阵全部记下（前 40 次） */
    static int _mxN = 0;
    if (count == 1 && v && _mxN < 40) {
        _mxN++;
        MCLOG("[mx] loc=%d lr=%p m0=%.4f m5=%.4f m10=%.4f m11=%.4f m15=%.4f | [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]",
              loc, __builtin_return_address(0), v[0], v[5], v[10], v[11], v[15],
              v[0],v[1],v[2],v[3], v[4],v[5],v[6],v[7], v[8],v[9],v[10],v[11], v[12],v[13],v[14],v[15]);
    }
    if (count == 1 && v && g_uniLog < 8) {
        float n0 = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
        float n1 = v[4]*v[4] + v[5]*v[5] + v[6]*v[6];
        float n2 = v[8]*v[8] + v[9]*v[9] + v[10]*v[10];
        if (fabs(n0 - 1.f) < 0.03f && fabs(n1 - 1.f) < 0.03f && fabs(n2 - 1.f) < 0.03f
            && fabs(v[3]) < 0.03f && fabs(v[7]) < 0.03f && fabs(v[11]) < 0.03f
            && fabs(v[15] - 1.f) < 0.03f) {
            g_uniLog++;
            void* lr = __builtin_return_address(0);
            MCLOG("[gl] uni loc=%d tr=%d lr=%p v=%p p=%.2f y=%.2f | %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f",
                  loc, (int)tr, lr, v,
                  -atan2(v[6], v[5]) * 57.29578f,
                  atan2(-v[8], -v[0]) * 57.29578f,
                  v[0], v[1], v[2], v[4], v[5], v[6], v[8], v[9], v[10]);
        }
    }
    if (o_unimtx) o_unimtx(loc, count, tr, v);
}

static float DecodeYawDeg(const float* m)
{
    return (float)(atan2(-m[8], -m[0]) * (180.0 / 3.14159265358979));
}

// 相机更新函数的 hook：它已经把游戏自己的矩阵算好写进 ci+0x418，我们紧跟其后覆盖
static void my_camup(void* ci)
{
    if (o_camup) o_camup(ci);

    // 一次性 dump：存在 files/mcdump 才写日志，绝不写内存
    static int dumped = 0;
    if (!dumped && access("/data/data/com.mojang.minecraftpe/files/mcdump", F_OK) == 0) {
        dumped = 1;
        float* q = (float*)ci;
        MCLOG("[aim] scan a1=%p", ci);
        for (int off = 0; off <= 0x3C0; off += 4) {
            float* b = (float*)((char*)ci + off);
            float n0 = b[0]*b[0] + b[1]*b[1] + b[2]*b[2];
            float n1 = b[4]*b[4] + b[5]*b[5] + b[6]*b[6];
            float n2 = b[8]*b[8] + b[9]*b[9] + b[10]*b[10];
            if (fabs(n0 - 1.f) < 0.02f && fabs(n1 - 1.f) < 0.02f && fabs(n2 - 1.f) < 0.02f
                && fabs(b[15] - 1.f) < 0.02f && fabs(b[3]) < 0.02f && fabs(b[7]) < 0.02f) {
                MCLOG("[aim] BLK +0x%03X pitch=%7.2f yaw=%8.2f  [%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f]",
                      off,
                      -atan2(b[6], b[5]) * 57.29578f,
                      atan2(-b[8], -b[0]) * 57.29578f,
                      b[0], b[1], b[2], b[4], b[5], b[6], b[8], b[9], b[10]);
            }
        }
        (void)q;
    }

    {   /* 导出真实渲染朝向，供 Aim.c 标定使用 */
        g_camCi = (unsigned long)ci;
        float* dd = (float*)((char*)ci + 0xD8);
        if (g_trueYaw == 0.f || 1) { static int _once = 0; if (!_once) { _once = 1; MCLOG("[aim] mt t=(%.1f,%.1f,%.1f) f=%.1f", dd[12], dd[13], dd[14], dd[15]); } }
        g_trueYaw = (float)(atan2(-dd[8], -dd[0]) * 57.29578f);
        g_truePitch = (float)(-atan2(dd[6], dd[5]) * 57.29578f);
    }
    if (!g_aimOn) return;
    if (g_aimMode == 0) return;            // 只读模式，不动内存
    float* d = (float*)((char*)ci + 0xD8);   // a1 == CI+0x340，则 a1+0xD8 == CI+0x418
    float yawDeg = g_aimYaw;
    if (yawDeg > 998.0f || yawDeg < -998.0f) yawDeg = DecodeYawDeg(d);
    float m[16];
    BuildViewMat(yawDeg, g_aimMatPitch, m);
    if (g_aimMode == 1) {
        memcpy(d, m, sizeof(m));          // 非转置
    } else if (g_aimMode == 2) {
        float t[16];
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) t[r * 4 + c] = m[c * 4 + r];
        memcpy(d, t, sizeof(t));          // 转置
    }
}

// 在游戏自己的矩阵上叠旋转（保持布局，不会变形）
static void my_cp16(void* dst, const void* src)
{
    if (!g_aimOn) { if (o_cp16) o_cp16(dst, src); return; }
    const float* m = (const float*)src;
    float a = g_aimMatPitch * (3.14159265358979f / 180.f);
    float b = g_aimYaw     * (3.14159265358979f / 180.f);
    float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
    // Rx(a) : 绕行轴（低头/抬头）;  Ry(b) : 绕列轴（左右）
    float out[16];
    for (int c = 0; c < 4; c++) {
        // 先 yaw（Ry），再 pitch（Rx）
        float y0 =  cb * m[0*4+c] + sb * m[2*4+c];
        float y1 =  m[1*4+c];
        float y2 = -sb * m[0*4+c] + cb * m[2*4+c];
        float y3 =  m[3*4+c];
        out[0*4+c] = y0;
        out[1*4+c] = ca * y1 - sa * y2;
        out[2*4+c] = sa * y1 + ca * y2;
        out[3*4+c] = y3;
    }
    memcpy((char*)dst + 0x418, out, sizeof(out));
}

static void HookViewCopy()
{
    if (g_cp16Hooked) return;
    if (!g_libBase) g_libBase = FindLibBase(kLib);
    if (!g_libBase) { LE("[aim] no libbase"); return; }
    void* fn = (void*)(g_libBase + 0x11A1C67Cu);
    uint32_t w = 0;
    if (KittyMemory::memRead(&w, fn, 4) != KittyMemory::SUCCESS) { LE("[aim] read fail"); return; }
    int rc = DobbyHook(fn, (void*)my_camup, (void**)&o_camup);
    {
        void* gf = dlsym(RTLD_DEFAULT, "glUniformMatrix4fv");
        int rc3 = -99;
        if (gf) rc3 = DobbyHook(gf, (void*)my_unimtx, (void**)&o_unimtx);
        LI("[aim] unifmt hook rc=%d fn=%p", rc3, gf);
    }
    {
        void* f2 = (void*)(g_libBase + 0x11A19CFCu);
        int rc2 = DobbyHook(f2, (void*)my_gcsrc, (void**)&o_gcsrc);
        LI("[aim] gcsrc hook rc=%d fn=%p", rc2, f2);
    }
    g_cp16Hooked = (rc == 0);
    LI("[aim] camup hook rc=%d fn=%p first=%08X", rc, fn, w);
}

#include <pthread.h>

// ==== 玩家朝向标量结构 ====
// 特征：连续两个 float 为 -1.5691 / +1.5691（俯仰上下限）
//   结构 X-0xC = 左右角(rad)，X-0x8 = 俯仰(rad)
static float* g_lookAddr[16];
static volatile int g_lookCount = 0;
static volatile int g_lookStarted = 0;

static void* LookScanProc(void*)
{
    FILE* mf = fopen("/proc/self/maps", "r");
    if (!mf) return nullptr;
    char line[512];
    const uint32_t p0 = 0xBFC8D6AAu, p1 = 0x3FC8D6AAu;
    while (fgets(line, sizeof(line), mf)) {
        unsigned long a = 0, b = 0; char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &a, &b, perms) != 3) continue;
        if (perms[0] != 'r') continue;
        if (b <= a || (b - a) > 0x20000000UL) continue;
        const uint32_t* q = (const uint32_t*)a;
        size_t n = (size_t)((b - a) / 4);
        for (size_t i = 0; i + 1 < n; i++) {
            if (q[i] == p0 && q[i + 1] == p1) {
                int c = g_lookCount;
                if (c < 16) { g_lookAddr[c] = (float*)(a + i * 4); g_lookCount = c + 1; }
            }
        }
    }
    fclose(mf);
    LI("[aim] look structs=%d", (int)g_aimAddrCount);
    return nullptr;
}

static void EnsureLookScan()
{
    if (g_lookStarted) return;
    g_lookStarted = 1;
    pthread_t th;
    if (pthread_create(&th, nullptr, LookScanProc, nullptr) == 0) pthread_detach(th);
}

static void WriteLookScalar(float yawRad, float pitchRad)
{
    for (int i = 0; i < g_aimAddrCount; i++) {
        float* base = g_aimAddr[i];
        float* py = (float*)((char*)base - 0xC);
        float* pp = (float*)((char*)base - 0x8);
        py[0] = yawRad;
        pp[0] = pitchRad;
    }
}

extern "C" void aim_release(void)
{
    g_aimOn = 0; g_aimMode = 0;
}

extern "C" void aim_apply(float y, float p)
{
    g_aimYaw = y; g_aimMatPitch = p; g_aimMode = 1; g_aimOn = 1;
}

static void PollAim()
{
    static int64_t lastMs = 0;
    int64_t ms = (int64_t)(clock() * 1000 / CLOCKS_PER_SEC);
    if (ms - lastMs < 200) return;
    lastMs = ms;
    FILE* f = fopen("/data/data/com.mojang.minecraftpe/files/mcaim.txt", "r");
    if (!f) f = fopen("/sdcard/mcaim.txt", "r");
    if (!f) f = fopen("/storage/emulated/0/mcaim.txt", "r");
    if (!f) {
        return;
        return;
    }
    char f_line[256] = {0};
    if (!fgets(f_line, sizeof(f_line), f)) { fclose(f); return; }
    int on = 0; float yaw = 0.f, pit = 0.f; int mode = 0;
    int got = sscanf(f_line, "%d %f %f %d", &on, &yaw, &pit, &mode);
    {
        int na = 0;
        char* q = f_line;
        while (na < 8) {
            char* h = strstr(q, "0x");
            if (!h) break;
            unsigned long long av = strtoull(h, nullptr, 16);
            if (av > 0x1000) g_aimAddr[na++] = (float*)av;
            q = h + 2;
        }
        g_aimAddrCount = na;
    }
    if (got < 4) mode = 0;
    if (got >= 2) {
        if (on != g_aimOn || yaw != g_aimYaw || pit != g_aimMatPitch)
            MCLOG("[aim] poll on=%d yaw=%.1f pit=%.1f", on, yaw, pit);
        g_aimMode = mode;
        g_aimYaw = yaw; g_aimMatPitch = pit; g_aimOn = on;
        if (on && mode == 4) {
            float yawR = yaw;                     // 模式4：yaw/pitch 直接按弧度传
            float pitR = pit;
            if (yawR > 998.0f || yawR < -998.0f) {
                if (g_aimAddrCount > 0) yawR = *(float*)((char*)g_aimAddr[0] - 0xC);
            }
            WriteLookScalar(yawR, pitR);
            LI("[aim] scalar yaw=%.4f pitch=%.4f structs=%d", yawR, pitR, (int)g_aimAddrCount);
        }
    }
    fclose(f);
}

void McMod_Init()
{
    LI("[init] enter pid=%d", (int)getpid());
    InstallFaultGuard();
    g_libBase = FindLibBase(kLib);
    LI("[init] libbase=%p", (void*)g_libBase);
    InstallPatch();
    HookViewCopy();
    DetectVersion();
    int v = LoadCfg();
    if (v > 0) g_target = v;
    HookInput();
    /* 不在 init 里扫描：ScanLevelOnce 要读 1GB+ 堆，注入瞬间做会把主线程卡
       死/被 LMK 杀，改由 draw 里延时触发（!g_level && !g_scanning）。 */
    LI("[init] done target=%d no-scan", g_target);
}

void McMod_Draw()
{
    if (!isInitialized) return;
    if (g_panelOpen && g_blurOn)
        BlurBg::capture((float)glWidth, (float)glHeight, g_uiRect[0], g_uiRect[1], g_uiRect[2], g_uiRect[3]);
    static int _sd = 0; _sd++;
    bool _sdbg = (_sd <= 3);
    if (_sdbg) McStage("[mc] draw enter");
    aim_set_level(g_level ? 1 : 0);
    if (_sdbg) McStage("[mc] aim level ok");
    {
        static int _df = 0; _df++;
        if ((_df % 60) == 0) {
            FILE* sf = fopen("/data/data/com.mojang.minecraftpe/files/mcstage.log", "a");
            if (!sf) sf = fopen("/storage/emulated/0/Android/data/com.mojang.minecraftpe/files/mcstage.log", "a");
            if (sf) { fprintf(sf, "[stage] draw #%d level=%p aim=%d has=%d vpt=%d obj=%d", _df, g_level, aim_enabled(), aim_has(), aim_vpt_count(), aim_obj_count()); fputc('\n', sf); fflush(sf); fclose(sf); }
        }
    }
    // PollAim();  // 旧 mcaim.txt 劫持通道已废弃（会写陈旧地址导致闪退）
    // 外部哨兵：存在 /sdcard/mcprobe_go 则跑一次探针
    {
        static int64_t lastChk = 0;
        if (_sdbg) McStage("[mc] probe enter");
        int64_t t = (int64_t)time(nullptr);
        if (_sdbg) McStage("[mc] probe time ok");
        if (t - lastChk >= 2) {
            lastChk = t;
            FILE* f = fopen("/sdcard/mcprobe_go", "r");
            if (_sdbg) McStage("[mc] probe fopen ok");
            if (f) { fclose(f); remove("/sdcard/mcprobe_go"); StartProbeAsync(); MCLOG("[probe] triggered"); }
            if (_sdbg) McStage("[mc] probe go checked");
            { FILE* g2 = fopen("/sdcard/act.txt", "r"); if (_sdbg) McStage("[mc] act fopen done"); if (g2) { fclose(g2); ProcessActFile(); if (_sdbg) McStage("[mc] act processed"); } }
        }
    }
    if (_sdbg) McStage("[mc] act block ok");
    static int64_t lastTry = 0;
    int64_t now = (int64_t)time(nullptr);
    if (!g_level && !g_scanning && (now - lastTry) >= 3) {
        lastTry = now;
        StartScan();
    }
    if (_sdbg) McStage("[mc] startscan block ok");
    DrawFps();
    if (_sdbg) McStage("[mc] DrawFps ok");
    /* aim thread disabled (removed feature) */
    if (g_panelOpen) DrawPanel();
    else DrawBall();
    if (_sdbg) McStage("[mc] ball/panel ok");
    {
        ImGuiWindow* fw = ImGui::FindWindowByName("Debug##Default");
        if (fw) fw->Hidden = true;
    }
    if (_sdbg) McStage("[mc] draw end");
}
