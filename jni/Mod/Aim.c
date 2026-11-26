// aimd3.c - MCBE 纯内存自动瞄准 v3
// 数据源：Actor 对象（RTTI 自动枚举全部实体类）+ *(actor+0x228) 位置记录 +0x00 实时坐标
// 死亡判据：位置指针变垃圾/越界（对象被回收）→ 每帧立刻剔除，不再瞄尸体
// 用法: ./aimd3 [半径=5]
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/uio.h>
#include <sys/resource.h>
#include <signal.h>
#include <setjmp.h>

#define PKG "com.mojang.minecraftpe"

#define AIMLOG(...) do { FILE* _af = fopen("/data/data/com.mojang.minecraftpe/files/mcstage.log", "a"); if (_af) { fprintf(_af, "[aimdbg] "); fprintf(_af, __VA_ARGS__); fputc(10, _af); fflush(_af); fclose(_af); } } while (0)

#define CHUNK (1024 * 1024)
#define MAXREG 4096
#define MAXVPT 512
#define MAXTGT 128
#define AHS 2048          /* accepted 集合 */
#define CMAP (1 << 16)    /* 直接映射解析缓存 */

static int g_fd = -1;
static volatile long g_pid = 0;
static unsigned long g_lib_lo, g_lib_hi, g_vt_lo, g_vt_hi, g_any_lo = ~0UL, g_any_hi;
extern volatile float g_trueYaw, g_truePitch;
extern void aim_apply(float y, float p);
extern void aim_release(void);
static unsigned long g_look = 0;
static int g_lookRad = 1;
static int g_lookSign = 1;
static unsigned long g_x_addr;
static unsigned long g_rot = 0;
static float g_wy = 999.f, g_wp = 999.f;

typedef struct { unsigned long lo, hi; } REG;
static REG g_reg[MAXREG];
static int g_nreg;
static unsigned long g_totb;

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static pid_t find_pid(void) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e; pid_t ret = 0;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char path[64], buf[256];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        int f = open(path, O_RDONLY);
        if (f < 0) continue;
        int n = read(f, buf, sizeof(buf) - 1);
        close(f);
        if (n <= 0) continue;
        buf[n] = 0;
        if (strstr(buf, PKG)) { ret = atoi(e->d_name); break; }
    }
    closedir(d);
    return ret;
}

static int load_maps(void) {
    char p[64]; snprintf(p, sizeof(p), "/proc/%ld/maps", g_pid);
    FILE *m = fopen(p, "r");
    if (!m) return 0;
    g_nreg = 0; g_lib_lo = g_lib_hi = 0; g_totb = 0;
    char line[512];
    while (fgets(line, sizeof(line), m)) {
        unsigned long lo, hi; char perms[8], path[256] = "";
        int got = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]", &lo, &hi, perms, path);
        if (got < 3) continue;
        if (g_any_lo == ~0UL || lo < g_any_lo) g_any_lo = lo;
        if (hi > g_any_hi) g_any_hi = hi;
        if (strstr(path, "libminecraftpe")) {
            if (!g_lib_lo || lo < g_lib_lo) g_lib_lo = lo;
            if (hi > g_lib_hi) g_lib_hi = hi;
            continue;
        }
        if (perms[0] != 'r' && perms[0] != 'R') continue;
        if (strstr(path, ".so") || strstr(path, ".apk") || strstr(path, ".odex") ||
            strstr(path, "/system/") || strstr(path, "/vendor/") || strstr(path, "/dev/") ||
            strstr(path, "memfd") || strstr(path, "[vvar]") || strstr(path, "[vdso]") ||
            strstr(path, "[vsyscall]") || strstr(path, "[stack") || strstr(path, "[vectors") ||
            strstr(path, "fonts") || strstr(path, "/data/fonts")) continue;
        /* 只扫 scudo（主堆+次堆≈1.1GB）；把 [heap]/dalvik 等一起扫会飙到 2.4GB，
           IO 峰值会拖垮房主连接（之前房解散就是它）。 */
        if (!strstr(path, "scudo")) continue;
        if (hi - lo > 600UL * 1024 * 1024) continue;
        if (g_nreg < MAXREG) { g_reg[g_nreg].lo = lo; g_reg[g_nreg].hi = hi; g_nreg++; g_totb += hi - lo; }
    }
    fclose(m);
    fprintf(stderr, "[maps] regs=%d bytes=%lu\n", g_nreg, g_totb);
    return (g_lib_hi > g_lib_lo && g_nreg > 0);
}

static inline unsigned long mte(unsigned long v) { return v & 0x00FFFFFFFFFFFFFFUL; }
/* ===== 堆扫描安全垫：游戏并发回收页面会让 memcpy 踩空，用 SIGSEGV 兜底跳过 ===== */
static __thread sigjmp_buf g_jb;
static __thread volatile int g_jb_on = 0;
static void _seg_h(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    if (g_jb_on) { g_jb_on = 0; siglongjmp(g_jb, 1); }
    AIMLOG("FAULT-ESC sig=%d addr=%#lx", sig, (unsigned long)(si ? si->si_addr : 0));
    signal(sig, SIG_DFL);
}
static void _seg_install(void) {
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = _seg_h; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_NODEFER | SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}
static int safe_memcpy(unsigned long a, void *buf, size_t n) {
    if (sigsetjmp(g_jb, 1) == 0) {
        g_jb_on = 1;
        memcpy(buf, (void *)(a & 0x00FFFFFFFFFFFFFFUL), n);
        g_jb_on = 0;
        return 1;
    }
    g_jb_on = 0;
    return 0;
}
static int safe_memcpy(unsigned long a, void *buf, size_t n);
static int rd(unsigned long a, void *buf, size_t n) {
    return safe_memcpy(a, buf, n);
}
static int wr(unsigned long a, void *buf, size_t n) {
    memcpy((void *)(a & 0x00FFFFFFFFFFFFFFUL), buf, n);
    return 1;
}
static unsigned long rd64(unsigned long a) { unsigned long v = 0; rd(a, &v, 8); return v; }

static int in_heap(unsigned long a) {
    if (!a) return 0;
    if (a >= g_any_lo && a + 64 <= g_any_hi) return 1;
    for (int i = 0; i < g_nreg; i++) if (a >= g_reg[i].lo && a + 64 <= g_reg[i].hi) return 1;
    return 0;
}

/* ---------- RTTI ---------- */
typedef struct { unsigned long key; int flag; } CT;
static CT g_ct[CMAP];

typedef struct { unsigned long key; int idx; int used; } AE;
static AE g_ae[AHS];

static unsigned long g_vptr[MAXVPT];
static char g_vname[MAXVPT][48];
static int g_vflag[MAXVPT];
static int g_vcat[MAXVPT];
static int g_nvpt;
static volatile int g_mask = 0x1F;   /* 位=阵营允许：0亡灵 1善良 2中立 3敌对 4玩家；默认全部（含生物） */

static void cam_scan(void);
static void pick_x(void);
static void scan_objects(int full);

/* ===== mod 内目标状态 ===== */
static volatile int   g_run = 1;
static volatile int   g_levelReady = 0;
static volatile int   g_enable = 1;
static float          g_radius = 5.0f;
static volatile int   g_atkEn = 0;      /* 靠近自动挨打 */
static volatile float g_atkR = 3.0f;    /* 进入多少格就打 */
static volatile int   g_ntap = 0;
static volatile float g_atkDmg = 6.0f;   /* 每次扣多少血 */
static volatile int   g_hasT = 0;
static volatile int   g_tcat = -1;
static volatile float g_tdist = 0.f;
static char           g_tname[64] = "";

static void ae_reset(void) { memset(g_ae, 0, sizeof(g_ae)); g_nvpt = 0; }
static int ae_get(unsigned long k) {
    unsigned long h = (k >> 3) & (AHS - 1);
    for (int i = 0; i < 16; i++) {
        int idx = (int)((h + i) & (AHS - 1));
        if (!g_ae[idx].used) return -1;
        if (g_ae[idx].key == k) return g_ae[idx].idx;
    }
    return -1;
}
static void ae_put(unsigned long k, int idx) {
    unsigned long h = (k >> 3) & (AHS - 1);
    for (int i = 0; i < 16; i++) {
        int j = (int)((h + i) & (AHS - 1));
        if (!g_ae[j].used) { g_ae[j].used = 1; g_ae[j].key = k; g_ae[j].idx = idx; return; }
        if (g_ae[j].key == k) { g_ae[j].idx = idx; return; }
    }
}

static void strip_digits(const char *s, char *out, int n) {
    int i = 0;
    while (*s >= '0' && *s <= '9' && *s) s++;
    while (*s && i < n - 1) out[i++] = *s++;
    out[i] = 0;
}

static int g_rvdbg = 300;
static int resolve_vptr(unsigned long vptr, char *name_out) {
    unsigned long ti = mte(rd64(vptr - 8));
    int _d = (g_rvdbg < 12);
    if (_d) { g_rvdbg++; AIMLOG("rv vptr=%#lx off=%#lx ti=%#lx inr=%d", vptr, vptr - g_lib_lo, ti, (ti >= g_lib_lo && ti < g_lib_hi)); }
    if (ti < g_lib_lo || ti >= g_lib_hi) return 0;
    int flag = 0; unsigned long cur = ti; int leaf_local = 0;
    for (int depth = 0; depth < 16 && cur >= g_lib_lo && cur < g_lib_hi; depth++) {
        unsigned long np = mte(rd64(cur + 8));
        if (_d) AIMLOG("rv   np=%#lx inr=%d", np, (np >= g_lib_lo && np < g_lib_hi));
        if (np < g_lib_lo || np >= g_lib_hi) break;
        char raw[128];
        if (!rd(np, raw, sizeof(raw) - 1)) break;
        raw[sizeof(raw) - 1] = 0;
        char s[48]; strip_digits(raw, s, sizeof(s));
        if (_d) AIMLOG("rv   name=%s", s);
        if (depth == 0 && name_out) { strncpy(name_out, s, 47); name_out[47] = 0; }
        if (!strcmp(s, "LocalPlayer")) leaf_local = 1;
        if (!strcmp(s, "Actor")) flag |= 1;                 /* 实体 */
        if (!strcmp(s, "Mob")) flag |= 1 | 2;               /* 生物 */
        if (!strcmp(s, "Player")) flag |= 1 | 2 | 4;        /* 玩家 */
        if (depth == 0) {
            if (!strcmp(s, "RemotePlayer") || !strcmp(s, "ServerPlayer") || !strcmp(s, "Player")) flag |= 1 | 2 | 4;
            else if (!strcmp(s, "LocalPlayer")) flag |= 1 | 2 | 4;
            else if (!strcmp(s, "Actor")) flag |= 1;
            else if (!strcmp(s, "Mob") || !strcmp(s, "Monster") || !strcmp(s, "Animal")) flag |= 1 | 2;
        }
        unsigned long base = mte(rd64(cur + 0x10));
        if (base == cur) break;
        cur = base;
    }
    if (_d) AIMLOG("rv   flag=%d", flag);
    if (!flag) return 0;
    if (leaf_local) flag |= 8;
    return flag;
}

/* 阵营分类：0 亡灵,1 善良,2 中立,3 敌对,4 玩家,5 其他 */
static const char *g_catname[6] = { "亡灵", "善良", "中立", "敌对", "玩家", "其他" };
static volatile int g_stop;

#define NAMECHK(s, str) (strstr(s, str) != 0)
static int classify(const char *n);
/* 返回 accepted 索引(-1 表示非实体) */
static int vptr_accept(unsigned long w) {
    int idx = ae_get(w);
    if (idx >= 0) return idx;
    unsigned long h = ((w >> 7) ^ (w >> 3)) & (CMAP - 1);
    if (g_ct[h].key == w && g_ct[h].flag >= 0) {
        int f = g_ct[h].flag;
        if (f == 0) return -1;
        return ae_get(w);
    }
    char nm[48] = "";
    int f = resolve_vptr(w, nm);
    g_ct[h].key = w; g_ct[h].flag = f;
    if (!f) return -1;
    if (g_nvpt < MAXVPT) {
        g_vptr[g_nvpt] = w;
        strncpy(g_vname[g_nvpt], nm, 47);
        g_vflag[g_nvpt] = f;
        g_vcat[g_nvpt] = classify(nm);
        ae_put(w, g_nvpt);
        fprintf(stderr, "[vpt] #%d off=%#lx %s %s%s\n", g_nvpt, w - g_lib_lo, nm,
                g_catname[g_vcat[g_nvpt]],
                (f & 8) ? " <SELF>" : ((f & 6) ? " <TGT>" : " <skip:非生物>"));
        g_nvpt++;
        return g_nvpt - 1;
    }
    return -1;
}

/* classify() 定义如下 */
static int classify(const char *n) {
    if (NAMECHK(n, "Player")) return 4;
    if (NAMECHK(n, "Zombie") || NAMECHK(n, "Skeleton") || NAMECHK(n, "Drowned") ||
        NAMECHK(n, "Husk") || NAMECHK(n, "Stray") || NAMECHK(n, "Wither") ||
        NAMECHK(n, "Phantom") || NAMECHK(n, "Bogged") || NAMECHK(n, "Zoglin")) return 0;
    if (NAMECHK(n, "Cow") || NAMECHK(n, "Sheep") || NAMECHK(n, "Pig") ||
        NAMECHK(n, "Chicken") || NAMECHK(n, "Rabbit") || NAMECHK(n, "Horse") ||
        NAMECHK(n, "Donkey") || NAMECHK(n, "Mule") || NAMECHK(n, "Llama") ||
        NAMECHK(n, "Wolf") || NAMECHK(n, "Cat") || NAMECHK(n, "Ocelot") ||
        NAMECHK(n, "Parrot") || NAMECHK(n, "Bat") || NAMECHK(n, "Squid") ||
        NAMECHK(n, "Fish") || NAMECHK(n, "Fox") || NAMECHK(n, "Bee") ||
        NAMECHK(n, "Turtle") || NAMECHK(n, "Axolotl") || NAMECHK(n, "Panda") ||
        NAMECHK(n, "Goat") || NAMECHK(n, "Camel") || NAMECHK(n, "Sniffer") ||
        NAMECHK(n, "Frog") || NAMECHK(n, "Allay") || NAMECHK(n, "Villager") ||
        NAMECHK(n, "Golem") || NAMECHK(n, "Trader") || NAMECHK(n, "IronGolem") ||
        NAMECHK(n, "SnowGolem") || NAMECHK(n, "Mooshroom") || NAMECHK(n, "Strider")) return 1;
    if (NAMECHK(n, "Enderman") || NAMECHK(n, "PolarBear") || NAMECHK(n, "Piglin") ||
        NAMECHK(n, "Spider")) return 2;
    if (NAMECHK(n, "Creeper") || NAMECHK(n, "Slime") || NAMECHK(n, "Blaze") ||
        NAMECHK(n, "Witch") || NAMECHK(n, "Pillager") || NAMECHK(n, "Vindicator") ||
        NAMECHK(n, "Evoker") || NAMECHK(n, "Ravager") || NAMECHK(n, "Vex") ||
        NAMECHK(n, "Shulker") || NAMECHK(n, "Guardian") || NAMECHK(n, "Warden") ||
        NAMECHK(n, "Hoglin") || NAMECHK(n, "Silverfish") || NAMECHK(n, "Endermite") ||
        NAMECHK(n, "Breeze") || NAMECHK(n, "Ghast")) return 3;
    return 5;
}

/* ---- 运行期筛选配置：/data/local/tmp/aim.cfg（每秒读一次，不是数据文件，只是开关） ---- */
static int parse_mask(const char *s) {
    int m = 0;
    char buf[256]; strncpy(buf, s, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    for (char *p = buf; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    char *save = 0;
    for (char *tok = strtok_r(buf, ", \t\r\n", &save); tok; tok = strtok_r(0, ", \t\r\n", &save)) {
        if (!strcmp(tok, "all")) m = 0x3F;
        else if (!strcmp(tok, "none") || !strcmp(tok, "off")) m = 0;
        else if (!strcmp(tok, "undead") || !strcmp(tok, "亡灵")) m |= 1 << 0;
        else if (!strcmp(tok, "passive") || !strcmp(tok, "善良")) m |= 1 << 1;
        else if (!strcmp(tok, "neutral") || !strcmp(tok, "中立")) m |= 1 << 2;
        else if (!strcmp(tok, "hostile") || !strcmp(tok, "敌对")) m |= 1 << 3;
        else if (!strcmp(tok, "player") || !strcmp(tok, "玩家")) m |= 1 << 4;
        else if (!strcmp(tok, "other") || !strcmp(tok, "其他")) m |= 1 << 5;
        else if (tok[0] >= '0' && tok[0] <= '5') m |= 1 << (tok[0] - '0');
    }
    return m;
}

static void show_mask(int m) {
    fprintf(stderr, "[filter] ");
    if (!m) fprintf(stderr, "none(关闭)");
    for (int i = 0; i < 6; i++) if (m & (1 << i)) fprintf(stderr, "%s ", g_catname[i]);
    fprintf(stderr, "\n");
}

static void *cfg_poller(void *arg) {
    (void)arg;
    char last[256] = "";
    while (!g_stop) {
        int f = open("/data/local/tmp/aim.cfg", O_RDONLY);
        if (f >= 0) {
            char buf[256] = ""; int n = read(f, buf, sizeof(buf) - 1); close(f);
            if (n > 0) {
                buf[n] = 0;
                if (strcmp(buf, last)) {
                    strncpy(last, buf, sizeof(last) - 1);
                    int m = parse_mask(buf);
                    if (m != g_mask) { g_mask = m; show_mask(m); }
                }
            }
        }
        usleep(1000000);
    }
    return 0;
}

/* 位置记录里的 y 是脚底，往上抬到躯干/胸口 */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_hot[64]; static int g_nhot;

/* ===== AABB 碰撞箱池（8 float: x1,y1,z1,x2,y2,z2,w,h） =====
   死生物箱子高会塔到 0.2 -> 尸体剔除；活体用箱中心瞄胸口 */
#define MAXBOX 512
typedef struct { float x1,y1,z1,x2,y2,z2; } BOX;
static BOX g_box[MAXBOX];
static int g_nbox = 0;

static int box_valid(const float *v) {
    float x1=v[0],y1=v[1],z1=v[2],x2=v[3],y2=v[4],z2=v[5],w=v[6],h=v[7];
    if (!(x2>x1 && y2>y1 && z2>z1)) return 0;
    if (fabsf((x2-x1)-w) > 0.03f) return 0;
    if (fabsf((z2-z1)-w) > 0.03f) return 0;
    if (fabsf((y2-y1)-h) > 0.03f) return 0;
    if (w < 0.18f || w > 3.4f || h < 0.12f || h > 4.1f) return 0;
    if (fabsf(x1) > 30000.f || fabsf(z1) > 30000.f) return 0;
    if (y1 < -220.f || y1 > 420.f) return 0;
    return 1;
}

static void aabb_rescan(void);

static int aabb_lookup(float tx, float ty, float tz, float *h_out, float *cy_out) {
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < g_nbox; i++) {
        float cx = (g_box[i].x1 + g_box[i].x2) * 0.5f;
        float cz = (g_box[i].z1 + g_box[i].z2) * 0.5f;
        float dx = cx - tx, dz = cz - tz, dy = g_box[i].y1 - ty;
        float dd = dx*dx + dz*dz;
        if (dd > 1.44f) continue;
        if (fabsf(dy) > 1.2f) continue;
        if (dd < bd) { bd = dd; best = i; }
    }
    if (best < 0) return 0;
    *h_out = g_box[best].y2 - g_box[best].y1;
    *cy_out = (g_box[best].y1 + g_box[best].y2) * 0.5f;
    return 1;
}
#define DEAD_H 0.28f

static void aabb_rescan(void) {
    double t0 = now_s();
    BOX tmp[MAXBOX]; int nt = 0;
    int nrun = g_nhot > 0 ? g_nhot : g_nreg;
    for (int k = 0; k < nrun; k++) {
        int r = (g_nhot > 0) ? g_hot[k] : k;
        if (r < 0 || r >= g_nreg) continue;
        unsigned long p = g_reg[r].lo, hi = g_reg[r].hi;
        while (p < hi) {
            unsigned long len = 0x20000;
            if (p + len > hi) len = hi - p;
            if (len < 0x100) break;
            unsigned char *buf = malloc(len);
            if (!buf) break;
            if (safe_memcpy(p, buf, len)) {
                int nf = (int)(len / 4);
                for (int i = 0; i + 64 < nf; i += 4) {
                    const float *v = (const float *)(buf + i * 4);
                    if (v[7] < 0.12f || v[7] > 4.1f) continue;
                    if (v[6] < 0.15f || v[6] > 3.5f) continue;
                    if (!box_valid(v)) continue;
                    if (nt < MAXBOX) {
                        tmp[nt].x1 = v[0]; tmp[nt].y1 = v[1]; tmp[nt].z1 = v[2];
                        tmp[nt].x2 = v[3]; tmp[nt].y2 = v[4]; tmp[nt].z2 = v[5];
                        nt++;
                    }
                }
            }
            free(buf);
            p += len - 0x400;
            if (p + 0x1000 >= hi) break;
        }
    }
    pthread_mutex_lock(&g_mtx);
    int n = nt > MAXBOX ? MAXBOX : nt;
    memcpy(g_box, tmp, sizeof(BOX) * n);
    g_nbox = n;
    pthread_mutex_unlock(&g_mtx);
    AIMLOG("aabb nbox=%d %.1fs", n, now_s() - t0);
}

static float body_up(const char *n) {
    if (strstr(n, "Player")) return 1.15f;
    if (strstr(n, "IronGolem") || strstr(n, "Enderman") || strstr(n, "Wither") ||
        strstr(n, "Horse") || strstr(n, "Ravager") || strstr(n, "Ghast") ||
        strstr(n, "EnderDragon") || strstr(n, "Warden")) return 1.55f;
    if (strstr(n, "Chicken") || strstr(n, "Parrot") || strstr(n, "Bat") ||
        strstr(n, "Rabbit") || strstr(n, "Cat") || strstr(n, "Ocelot") ||
        strstr(n, "Silverfish") || strstr(n, "Endermite") || strstr(n, "Bee")) return 0.38f;
    if (strstr(n, "Spider")) return 0.5f;
    return 0.9f;   /* 僵尸/村民/苦力怕/骷髅/牛/羊/猪… 约胸口高度 */
}

/* ---------- 目标 ---------- */
typedef struct { unsigned long obj, posp; int flag; int vi; } TGT;
static TGT g_tg[MAXTGT];
static int g_ntg;

static unsigned long g_self_obj;



static unsigned long g_cand = 0;
static unsigned long g_skip = 0, g_readb = 0, g_nlibptr = 0;
extern volatile unsigned long g_camCi;
static int g_sr_n = 0;
static void scan_region(unsigned long base, unsigned long range, const char *tag)
{
    if (!base) { AIMLOG("RG no %s", tag); return; }
    g_rvdbg = 0;
    int cnt = 0;
    for (unsigned long a = base; a + 8 <= base + range; a += 8) {
        unsigned long p = mte(rd64(a));
        if (p < g_any_lo || p > g_any_hi || (p & 7)) continue;
        unsigned long vt = mte(rd64(p));
        if (vt < g_lib_lo || vt >= g_lib_hi) continue;
        char nm[48]; nm[0] = 0;
        if (!resolve_vptr(vt, nm)) continue;
        cnt++;
        if (g_sr_n < 80) { g_sr_n++; AIMLOG("RG %s off=%#lx obj=%#lx name=%s", tag, a - base, p, nm); }
    }
    AIMLOG("RG %s done cnt=%d", tag, cnt);
}
static void level_hunt(void);
static void scan_vptrs(void) {
    int _nc = 0;
    double _t0 = now_s();
    unsigned char *buf = malloc(CHUNK + 0x40);
    if (!buf) return;
    for (int r = 0; r < g_nreg; r++) {
        unsigned long p = g_reg[r].lo, hi = g_reg[r].hi;
        while (p < hi) {
            size_t len = CHUNK;
            if (p + len > hi) len = hi - p;
            if (len < 8) break;
            ssize_t n = safe_memcpy(p, buf, len) ? ((g_readb += (unsigned long)len), (ssize_t)len) : ((g_skip += (unsigned long)len), (ssize_t)0);
            if (n < 8) { p += len; continue; }
            size_t nw = (size_t)n / 8;
            for (size_t i = 0; i < nw; i++) {
                unsigned long w = mte(((unsigned long *)buf)[i]);
                if (w < g_vt_lo || w >= g_vt_hi) { if ((w >= g_lib_lo && w < g_lib_hi)) g_nlibptr++; continue; }
                if (g_cand < 3) AIMLOG("cand vptr=%#lx off=%#lx ti=%#lx inlib=%d", w, w - g_lib_lo, (unsigned long)mte(rd64(w - 8)), (int)(mte(rd64(w - 8)) >= g_lib_lo && mte(rd64(w - 8)) < g_lib_hi));
                g_cand++;
                vptr_accept(w);
            }
            p += len;
            if ((++_nc & 31) == 0) usleep(15000);
        }
    }
    free(buf);
    AIMLOG("scan cand=%lu libptr=%lu nvpt=%d read=%luMB skip=%luMB %.1fs", g_cand, g_nlibptr, g_nvpt, g_readb>>20, g_skip>>20, now_s()-_t0);
    for (int i = 0; i < g_nvpt; i++) AIMLOG("vpt[%d] off=%#lx %s flag=%d", i, g_vptr[i]-g_lib_lo, g_vname[i], g_vflag[i]);
}

static void scan_objects(int full) {
    TGT tmp[MAXTGT]; int nt = 0;
    unsigned long self = 0;
    int hot[64]; int nh = 0;
    unsigned char *buf = malloc(CHUNK + 0x40);
    if (!buf) return;
    int nrun = full ? g_nreg : g_nhot;
    int _nc2 = 0;
    for (int k = 0; k < nrun; k++) {
        int r = full ? k : g_hot[k];
        if (r < 0 || r >= g_nreg) continue;
        unsigned long p = g_reg[r].lo, hi = g_reg[r].hi;
        int hit = 0;
        while (p < hi) {
            size_t len = CHUNK;
            if (p + len > hi) len = hi - p;
            if (len < 8) break;
            ssize_t n = safe_memcpy(p, buf, len) ? (ssize_t)len : 0;
            if (n < 8) { p += len; continue; }
            size_t nw = (size_t)n / 8;
            for (size_t i = 0; i < nw; i++) {
                unsigned long w = mte(((unsigned long *)buf)[i]);
                if (w < g_vt_lo || w >= g_vt_hi) continue;
                int idx = ae_get(w);
                if (idx < 0) continue;
                unsigned long obj = p + (unsigned long)i * 8;
                hit = 1;
                if (g_vflag[idx] & 8) {
                    unsigned long sp0 = mte(rd64(obj + 0x228));
                    float f0[3];
                    if (in_heap(sp0) && rd(sp0, f0, 12) &&
                        isfinite(f0[0]) && isfinite(f0[1]) && isfinite(f0[2]) &&
                        fabsf(f0[0]) < 3e7f && fabsf(f0[2]) < 3e7f && fabsf(f0[1]) < 4000.f) {
                        if (!self) { self = obj; static int _sc=0; if (_sc<1) { _sc=1; AIMLOG("self-cand obj=%#lx pos=(%.1f,%.1f,%.1f) vi=%d", obj, f0[0], f0[1], f0[2], idx); } }
                    } else {
                        AIMLOG("self-cand reject obj=%#lx sp=%#lx f=%.1f,%.1f,%.1f", obj, sp0, f0[0], f0[1], f0[2]);
                    }
                    continue;
                }
                if (!(g_vflag[idx] & 6)) continue;   /* 只要 Mob / Player，纯 Actor(物品/箭/矿车) 一律不要 */
                if (nt < MAXTGT) {
                    tmp[nt].obj = obj;
                    tmp[nt].flag = g_vflag[idx];
                    tmp[nt].posp = mte(rd64(obj + 0x228));
                    tmp[nt].vi = idx;
                    nt++;
                }
            }
            p += len;
            if ((++_nc2 & 31) == 0) usleep(15000);
        }
        if (hit && nh < 64) { hot[nh++] = r; }
    }
    free(buf);
    pthread_mutex_lock(&g_mtx);
    g_self_obj = self;
    g_ntg = nt;
    memcpy(g_tg, tmp, sizeof(TGT) * nt);
    g_nhot = nh;
    memcpy(g_hot, hot, sizeof(int) * nh);
    pthread_mutex_unlock(&g_mtx);
}

static void *scanner(void *arg) {
    (void)arg;
    setpriority(PRIO_PROCESS, 0, 19);
    double last_full = 0;
    while (!g_stop) {
        double now = now_s();

        static double _lastRetry = 0;
        static double _rtry = 3.0;
        if (!g_self_obj || !g_x_addr) {
            /* 还没进世界 / 还没定位到自身与相机：不扫描、不写内存，定期重试 */
            g_hasT = 0;
            if (now - _lastRetry > _rtry) {
                _lastRetry = now;
                if (_rtry < 20.0) _rtry *= 2.0;
                scan_objects(1);
    AIMLOG("obj scan done ntg=%d self=%#lx", g_ntg, g_self_obj);
                if (!g_x_addr) { cam_scan(); pick_x(); }
            }
            usleep(200000);
            continue;
        }
        _rtry = 3.0;
        if (now - last_full > 90.0) {
            AIMLOG("before scan_vptrs");
    scan_vptrs();
    AIMLOG("vpt=%d", g_nvpt);
    AIMLOG("vptr scan done nvpt=%d", g_nvpt);
            scan_objects(1);
            aabb_rescan();
            last_full = now;
        } else {
            scan_objects(0);
        }
        usleep(300000);
    }
    return 0;
}

/* ---------- 相机结构自定位 ---------- */
static int is_x(unsigned long base, float *out) {
    float p[12];
    if (!rd(base, p, sizeof(p))) return 0;
    if (fabsf(p[0]) > 10.0f || fabsf(p[1]) > 3.5f) return 0;
    if (!(p[3] < -1.55f && p[3] > -1.60f)) return 0;
    if (!(p[4] > 1.55f && p[4] < 1.60f)) return 0;
    if (fabsf(fabsf(p[3]) - fabsf(p[4])) > 0.002f) return 0;
    if (!(p[8] < -1.56f && p[8] > -1.58f)) return 0;
    if (!(p[9] > 1.56f && p[9] < 1.58f)) return 0;
    if (base < 0x7000000000UL) return 0;
    if (out) { out[0] = p[0]; out[1] = p[1]; }
    return 1;
}
#define XCAND 96
static unsigned long g_xc[XCAND];
static int g_nxc;

static void cam_scan(void) {
    unsigned char *buf = malloc(CHUNK + 0x40);
    if (!buf) return;
    for (int r = 0; r < g_nreg; r++) {
        unsigned long p = g_reg[r].lo, hi = g_reg[r].hi;
        while (p < hi) {
            size_t len = CHUNK;
            if (p + len > hi) len = hi - p;
            if (len < 0x40) break;
            ssize_t n = safe_memcpy(p, buf, len) ? (ssize_t)len : 0;
            if (n >= 0x40) {
                int nf = (int)(n / 4);
                for (int i = 0; i + 6 <= nf; i++) {
                    float a = ((float *)buf)[i], b = ((float *)buf)[i + 1];
                    if (a * b < 0) {
                        float m2 = fabsf(a);
                        if (m2 > 1.4f && m2 < 1.7f && fabsf(b) > 1.4f && fabsf(b) < 1.7f) {
                            unsigned long cand = p + (unsigned long)i * 4 - 0xc;
                            if (is_x(cand, 0) && g_nxc < XCAND) {
                                int dup = 0;
                                for (int k = 0; k < g_nxc; k++) if (g_xc[k] == cand) { dup = 1; break; }
                                if (!dup) g_xc[g_nxc++] = cand;
                            }
                        }
                    }
                }
            }
            p += len;
        }
    }
    free(buf);
}

static void pick_x(void) {
    g_x_addr = 0;
    for (int round = 0; round < 3 && !g_x_addr; round++) {
        usleep(250000);
        for (int i = 0; i < g_nxc; i++) {
            float v[2];
            if (!is_x(g_xc[i], v)) continue;
            if (!g_x_addr || g_xc[i] < g_x_addr) g_x_addr = g_xc[i];
        }
    }
    if (g_x_addr) fprintf(stderr, "[x] cam %#lx (cand %d)\n", g_x_addr, g_nxc);
}

/* ---------- 玩家自身旋转自定位（相机结构只是镜像时，必须写这里） ---------- */
static float wrap180f(float d) { while (d > 180.f) d -= 360.f; while (d < -180.f) d += 360.f; return d; }
static int look_write(float yawDeg, float pitDeg)
{
    if (!g_look) return 0;
    float f[2];
    if (!rd(g_look, f, 8)) return 0;
    float u = g_lookRad ? 0.0174533f : 1.f;
    float dy = wrap180f(yawDeg - g_trueYaw) * u * (float)g_lookSign;
    float dp = (pitDeg - g_truePitch) * u * (float)g_lookSign;
    if (fabsf(dy) < 0.0004f && fabsf(dp) < 0.0004f) return 1;
    f[0] += dy; f[1] += dp;
    return wr(g_look, f, 8) ? 1 : 0;
}
static void look_calib(void)
{
    unsigned long selfobj;
    pthread_mutex_lock(&g_mtx); selfobj = g_self_obj; pthread_mutex_unlock(&g_mtx);
    if (!selfobj) { AIMLOG("calib no self"); return; }
    float ty = g_trueYaw, tp = g_truePitch;
    if (!isfinite(ty) || !isfinite(tp)) { AIMLOG("calib bad true"); return; }
    int cand = 0;
    unsigned long endp = selfobj + 0x8000;
    for (unsigned long a = selfobj; a + 8 <= endp; a += 4) {
        float f[2];
        if (!rd(a, f, 8)) continue;
        if (!isfinite(f[0]) || !isfinite(f[1])) continue;
        int isRad = (fabsf(f[0]) <= 3.35f && fabsf(f[1]) <= 1.75f);
        int isDeg = (fabsf(f[0]) <= 190.f && fabsf(f[1]) <= 95.f);
        if (!isRad && !isDeg) continue;
        float u = isRad ? 57.29578f : 1.f;
        if (fabsf(wrap180f(f[0] * u - ty)) > 3.5f) continue;
        if (fabsf(f[1] * u - tp) > 3.5f) continue;
        cand++;
        float d = isRad ? 0.30f : 10.0f;
        float t[2] = { f[0] + d, f[1] };
        wr(a, t, 8);
        usleep(150000);
        float y2 = g_trueYaw;
        wr(a, f, 8);
        usleep(70000);
        float dd = wrap180f(y2 - ty);
        if (fabsf(dd) > 1.5f) {
            g_look = a; g_lookRad = isRad ? 1 : 0; g_lookSign = (dd > 0) ? 1 : -1;
            AIMLOG("LOOK HIT %#lx rad=%d sign=%d cand=%d %.2f -> %.2f", a, g_lookRad, g_lookSign, cand, ty, y2);
            return;
        }
        if (cand > 60) break;
    }
    AIMLOG("LOOK NONE cand=%d true=(%.2f,%.2f)", cand, ty, tp);
}

/* ---------------- 自定位（EC 思路）：在玩家对象里找存 yaw/pitch 的字段 ---------------- */
static void hunt_self(void)
{
    if (g_self_obj) return;
    if (!isfinite(g_trueYaw) || g_trueYaw == 0.f) return;
    float ty = g_trueYaw, tp = g_truePitch;
    int npl = 0;
    for (int i = 0; i < g_ntg; i++) {
        if (!(g_mask & (1 << g_vcat[g_tg[i].vi]))) continue;
        unsigned long o = g_tg[i].obj;
        npl++;
        for (unsigned long a = o + 0x40; a + 8 <= o + 0x900; a += 4) {
            float f[2];
            if (!rd(a, f, 8)) continue;
            if (!isfinite(f[0]) || !isfinite(f[1])) continue;
            int isRad = (fabsf(f[0]) <= 6.30f && fabsf(f[1]) <= 1.62f && fabsf(f[0]) > 0.0001f);
            int isDeg = (fabsf(f[0]) <= 360.f && fabsf(f[1]) <= 90.f);
            if (!isRad && !isDeg) continue;
            float u = isRad ? 57.29578f : 1.f;
            if (fabsf(wrap180f(f[0] * u - ty)) > 2.5f) continue;
            if (fabsf(f[1] * u - tp) > 2.5f) continue;
            float t2[2] = { f[0] + (isRad ? 0.30f : 10.f), f[1] };
            if (!wr(a, t2, 8)) continue;
            usleep(130000);
            float y2 = g_trueYaw;
            wr(a, f, 8);
            usleep(60000);
            float dd = wrap180f(y2 - ty);
            if (fabsf(dd) > 1.5f) {
                g_self_obj = o;
                g_look = a; g_lookRad = isRad ? 1 : 0; g_lookSign = (dd > 0) ? 1 : -1;
                AIMLOG("SELF HIT obj=%#lx yaw@%#lx rad=%d sign=%d ty=%.1f", o, a, g_lookRad, g_lookSign, ty);
                return;
            }
        }
    }
    AIMLOG("SELF NONE npl=%d ty=%.1f tp=%.1f", npl, ty, tp);
}

static void rot_hunt(void) {
    if (!g_x_addr) return;
    float cam[2];
    if (!rd(g_x_addr, cam, 8) || !isfinite(cam[0]) || !isfinite(cam[1])) return;
    float cy = cam[0], cp = cam[1];
    unsigned long selfobj;
    pthread_mutex_lock(&g_mtx); selfobj = g_self_obj; pthread_mutex_unlock(&g_mtx);
    if (!selfobj) return;
    int tried = 0;
    for (unsigned long a = selfobj; a + 8 <= selfobj + 0x4000; a += 4) {
        float f[2];
        if (!rd(a, f, 8)) continue;
        if (fabsf(f[0] - cy) > 0.08f || fabsf(f[1] - cp) > 0.08f) continue;
        tried++;
        float orig[2] = { f[0], f[1] };
        float tst[2] = { cy + 0.22f, cp - 0.04f };
        if (!wr(a, tst, 8)) continue;
        usleep(70000);
        float c2[2];
        int ok = rd(g_x_addr, c2, 8);
        if (ok && fabsf(c2[0] - tst[0]) < 0.10f && fabsf(c2[1] - tst[1]) < 0.06f) {
            g_rot = a;
            AIMLOG("rothent %#lx HIT tried=%d cam=(%.3f,%.3f)", a, tried, c2[0], c2[1]);
            return;
        }
        wr(a, orig, 8);
        if (tried > 48) break;
    }
    AIMLOG("rothent NONE tried=%d cam=(%.3f,%.3f)", tried, cy, cp);
}

/* ---- 从 level 直接找真玩家对象（不扫 2GB） ---- */
extern volatile unsigned long g_levelAddr;
static unsigned long g_lp_obj = 0;
static void level_hunt(void)
{
    unsigned long lv = g_levelAddr;
    if (!lv) { AIMLOG("LV no level"); return; }
    g_rvdbg = 0;
    int nsaw = 0, nhit = 0;
    for (unsigned long a = lv; a + 8 <= lv + 0x8000; a += 8) {
        unsigned long p = mte(rd64(a));
        if (p < g_any_lo || p > g_any_hi) continue;
        if (p & 7) continue;
        unsigned long vt = mte(rd64(p));
        if (vt < g_lib_lo || vt >= g_lib_hi) continue;
        char nm[48]; nm[0]=0;
        int f = resolve_vptr(vt, nm);
        if (!f) continue;
        nsaw++;
        if (!strcmp(nm,"LocalPlayer") || !strcmp(nm,"ServerPlayer") || !strcmp(nm,"RemotePlayer") || !strcmp(nm,"Player")) {
            nhit++;
            if (nhit <= 12) AIMLOG("LVHIT off=%#lx obj=%#lx name=%s", a - lv, p, nm);
            if (!strcmp(nm,"LocalPlayer") && !g_lp_obj) g_lp_obj = p;
        }
    }
    AIMLOG("LV done saw=%d hit=%d lp=%#lx", nsaw, nhit, g_lp_obj);
}

/* 只读：在 LocalPlayer 里找朝向字段候选 */
static void self_probe(void)
{
    unsigned long so;
    pthread_mutex_lock(&g_mtx); so = g_self_obj; pthread_mutex_unlock(&g_mtx);
    float ty = g_trueYaw, tp = g_truePitch;
    AIMLOG("PROBE self=%#lx true=(%.2f,%.2f)", so, ty, tp);
    if (!so) return;
    int n = 0;
    for (unsigned long a = so + 0x30; a + 8 <= so + 0x2000; a += 4) {
        float f[2];
        if (!rd(a, f, 8)) continue;
        if (!isfinite(f[0]) || !isfinite(f[1])) continue;
        int isRad = (fabsf(f[0]) <= 6.30f && fabsf(f[1]) <= 1.62f);
        int isDeg = (fabsf(f[0]) <= 360.f && fabsf(f[1]) <= 90.f);
        if (!isRad && !isDeg) continue;
        float u = isRad ? 57.29578f : 1.f;
        if (fabsf(wrap180f(f[0] * u - ty)) > 3.0f) continue;
        if (fabsf(f[1] * u - tp) > 3.0f) continue;
        if (n < 30) { n++; AIMLOG("PROBE off=%#lx rad=%d f=(%.4f,%.4f) deg=(%.2f,%.2f)", a - so, isRad, f[0], f[1], f[0]*u, f[1]*u); }
    }
    AIMLOG("PROBE done cand=%d", n);
}

/* ============ 视角通道：clamp 常量对 ±1.5691 定位 X ============ */
static unsigned long g_vaddr[96];
static int g_vn = 0;
static volatile int g_von = 0;
static volatile float g_vyaw = 0.f, g_vpit = 0.f;
static int is_clamp_pair(unsigned long a)
{
    float v0 = 0.f, v1 = 0.f;
    if (!rd(a, &v0, 4)) return 0;
    if (!rd(a + 4, &v1, 4)) return 0;
    if (!isfinite(v0) || !isfinite(v1)) return 0;
    return (fabsf(v0 + 1.5691f) < 0.002f) && (fabsf(v1 - 1.5691f) < 0.002f);
}
static void find_view(void)
{
    unsigned char *buf = malloc(CHUNK + 0x40);
    if (!buf) return;
    double t0 = now_s();
    g_vn = 0;
    int _nc = 0;
    for (int r = 0; r < g_nreg && g_vn < 90; r++) {
        unsigned long p = g_reg[r].lo, hi = g_reg[r].hi;
        while (p < hi) {
            size_t len = CHUNK; if (p + len > hi) len = hi - p;
            if (len < 16) break;
            if (!safe_memcpy(p, buf, len)) { p += len; continue; }
            size_t nf = len / 4;
            for (size_t i = 0; i + 2 < nf; i++) {
                float v0, v1;
                memcpy(&v0, buf + i*4, 4); memcpy(&v1, buf + i*4 + 4, 4);
                if (!(fabsf(v0 + 1.5691f) < 0.002f && fabsf(v1 - 1.5691f) < 0.002f)) continue;
                unsigned long a = p + i*4;
                history_check:;
                float yw = 0.f, pt = 0.f;
                if (!rd(a - 0xC, &yw, 4) || !rd(a - 0x8, &pt, 4)) continue;
                if (!isfinite(yw) || !isfinite(pt)) continue;
                if (fabsf(yw) > 7.f || fabsf(pt) > 2.f) continue;
                if (yw == 0.f && pt == 0.f) continue;   /* 未初始化的空副本：写了也没用，反而污染 */
                if (g_vn < 90) {
                    g_vaddr[g_vn++] = a - 0xC;
                    AIMLOG("VIEW cand#%d X=%#lx yaw=%.3f pitch=%.3f", g_vn-1, a - 0xC, yw, pt);
                }
            }
            p += len;
            if ((++_nc & 31) == 0) usleep(10000);
        }
    }
    free(buf);
    int nf = g_vn;
    AIMLOG("VIEW total=%d %.1fs", g_vn, now_s()-t0);
    /* 挑“活副本”：写探针±0.06rad，120ms 后读回；被游戏改回去的才是渲染真正用的 */
    if (nf > 1) {
        unsigned long kept[90]; int keep = 0;
        for (int i = 0; i < nf; i++) {
            unsigned long a = g_vaddr[i];
            float y0 = 0.f, p0 = 0.f;
            if (!rd(a, &y0, 4) || !rd(a + 4, &p0, 4)) continue;
            float probe = (y0 > 2.6f) ? y0 - 0.06f : y0 + 0.06f;
            *(volatile float*)a = probe;
            usleep(120000);
            float y1 = 0.f;
            if (!rd(a, &y1, 4)) continue;
            if (fabsf(y1 - probe) > 0.02f) { kept[keep++] = a;
                AIMLOG("VIEW keep %#lx y0=%.3f -> y1=%.3f (live)", a, y0, y1); }
            else AIMLOG("VIEW drop %#lx y0=%.3f (dead copy)", a, y0);
        }
        if (keep > 0) { g_vn = keep; for (int i = 0; i < keep; i++) g_vaddr[i] = kept[i]; }
        else AIMLOG("VIEW keep none, fallback all");
        AIMLOG("VIEW live=%d", g_vn);
    }
}
static void *vwriter(void *arg)
{
    (void)arg;
    while (!g_stop) {
        if (!g_von) { usleep(300); continue; }
        float y = g_vyaw, pt = g_vpit;
        { static int _vw = 0; if (_vw < 6) { _vw++;
            AIMLOG("viewwr n=%d a0=%#lx yaw=%.3f pit=%.3f", g_vn, g_vn ? g_vaddr[0] : 0UL, y, pt); } }
        for (int i = 0; i < g_vn; i++) {
            unsigned long a = g_vaddr[i];
            *(volatile float*)a = y;
            *(volatile float*)(a + 4) = pt;
        }
    }
    return 0;
}

static void *aim_thread(void *arg) {
    AIMLOG("thread ENTER");
    float radius = g_radius;

    g_pid = getpid();
    AIMLOG("thread start selfpid=%ld", (long)g_pid);
    if (!g_pid) { fprintf(stderr, "no game pid\n"); return NULL; }
    char mem[64]; snprintf(mem, sizeof(mem), "/proc/%ld/mem", g_pid);
    g_fd = open(mem, O_RDWR);
    AIMLOG("mem fd=%d", g_fd);

rescan:
    _seg_install();
    if (!load_maps()) { AIMLOG("load_maps FAILED"); return NULL; }
    AIMLOG("maps ok nreg=%d tot=%luMB", g_nreg, (unsigned long)(g_totb >> 20));
    fprintf(stderr, "pid=%ld lib=%#lx-%#lx\n", g_pid, g_lib_lo, g_lib_hi);
    g_vt_lo = g_lib_lo + 0x13000000UL;
    g_vt_hi = g_lib_lo + 0x14100000UL;
    AIMLOG("lib=%#lx-%#lx vt=%#lx-%#lx reg=%d", g_lib_lo, g_lib_hi, g_vt_lo, g_vt_hi, g_nreg);
    ae_reset(); memset(g_ct, 0, sizeof(g_ct)); g_nxc = 0; g_nhot = 0; g_ntg = 0; g_self_obj = 0;
    double t0 = now_s();
    scan_vptrs();
    AIMLOG("SCAN1 vpt=%d", g_nvpt);
    fprintf(stderr, "[d] vpt=%d %.1fs\n", g_nvpt, now_s() - t0);
    if (!g_nvpt) return NULL;
    scan_objects(1);
    aabb_rescan();
    AIMLOG("SCAN2 ntg=%d self=%#lx boxes=%d", g_ntg, g_self_obj, g_nbox);
    fprintf(stderr, "[d] objs=%d self=%#lx hot=%d\n", g_ntg, g_self_obj, g_nhot);
    { /* 类型名统计 */
        int cnt[512]={0};
        for (int i=0;i<g_ntg;i++) if (g_tg[i].vi>=0 && g_tg[i].vi<512) cnt[g_tg[i].vi]++;
        int shown=0;
        for (int v=0; v<512 && shown<40; v++) {
            if (!cnt[v]) continue;
            AIMLOG("NAME vi=%d cat=%d n=%d vname=[%s]", v, g_vcat[v], cnt[v], g_vname[v]);
            shown++;
        }
    }
    find_view();
    for (int q = 0; q < 4; q++) { pthread_t vt; pthread_create(&vt, 0, vwriter, 0); pthread_detach(vt); }
    cam_scan();
    pick_x();
    g_look = 0;
    self_probe();
    AIMLOG("SCAN3 x=%#lx look=%#lx", g_x_addr, g_look);
    AIMLOG("cam=%#lx", g_x_addr);
    if (!g_x_addr) fprintf(stderr, "no cam (continue)");

    pthread_t th; pthread_create(&th, 0, scanner, 0);
    pthread_t tc; pthread_create(&tc, 0, cfg_poller, 0);
    show_mask(g_mask);

    static double lchk = 0, lastrot = 0;
    double last_pidchk = now_s();
    unsigned long locked = 0;
    unsigned long lockPosp = 0;
    float lockPos[3] = {0,0,0};
    double lastdbg = 0;

    while (g_run) {
        radius = g_radius;   /* 滑块改了要即时生效，原来只在启动时快照 */
        double now = now_s();
        if (now - last_pidchk > 1.5) {
            last_pidchk = now;
            pid_t p = find_pid();
            if (p != g_pid) {
                g_stop = 1; pthread_join(th, 0); g_stop = 0;
                if (!p) { fprintf(stderr, "game gone\n"); return 0; }
                g_pid = p; close(g_fd);
                snprintf(mem, sizeof(mem), "/proc/%ld/mem", g_pid);
                g_fd = open(mem, O_RDWR);
                if (g_fd < 0) { sleep(1); return 0; }
                g_wy = g_wp = 999; locked = 0;
                goto rescan;
            }
        }

        unsigned long selfobj;
        pthread_mutex_lock(&g_mtx); selfobj = g_self_obj; pthread_mutex_unlock(&g_mtx);
        if (!selfobj) { usleep(30000); continue; }
        { static int _vtd = 0; if (!_vtd) { _vtd = 1;
            unsigned long vp = mte(rd64(selfobj));
            AIMLOG("VT self=%#lx vptr=%#lx heap=%d", selfobj, vp, in_heap(vp));
            if (in_heap(vp)) for (int _i = 0; _i < 64; _i++) {
                unsigned long f = mte(rd64(vp + _i * 8));
                AIMLOG("VT[%d] fn=%#lx heap=%d", _i, f, in_heap(f));
            }
        } }
        /* 攻击机制探针（只读）：盯 LocalPlayer+0x5F0..0x6A0 的整数字段变化 */
        { static int _pv[44]; static int _inited = 0; static double _lastp = 0;
          int _cv[44];
          if (now - _lastp > 0.008 && rd(selfobj + 0x5F0, _cv, 176)) {
            _lastp = now;
            if (_inited) for (int _i = 0; _i < 44; _i++)
                if (_cv[_i] != _pv[_i]) AIMLOG("ATKD +%#x %d -> %d", 0x5F0 + _i * 4, _pv[_i], _cv[_i]);
            for (int _i = 0; _i < 44; _i++) _pv[_i] = _cv[_i];
            _inited = 1;
          } }
        float meq[3]; int okq = rd(selfobj + 0x560, meq, 12);
        /* okq 时不再用 sp 做堆校验（原来 sp=1 会被 in_heap 判失败而 continue，永远锁不到人） */
        if (okq && !(isfinite(meq[0]) && isfinite(meq[1]) && isfinite(meq[2]) &&
                     fabsf(meq[1]) < 5000.f && fabsf(meq[0]) < 5.0e6f && fabsf(meq[2]) < 5.0e6f)) okq = 0;
        unsigned long sp = okq ? 0UL : mte(rd64(selfobj + 0x228));
        if (now - lastdbg > 1.5) {
            lastdbg = now;
            AIMLOG("loop self=%#lx inline=%d meq=%.2f,%.2f,%.2f sp=%#lx", selfobj, okq,
                   meq[0], meq[1], meq[2], sp);
        }
        float me[3];
        if (okq) { me[0]=meq[0]; me[1]=meq[1]; me[2]=meq[2]; }
        else {
            if (!in_heap(sp)) { usleep(8000); continue; }
            if (!rd(sp, me, 12)) { usleep(8000); continue; }
        }
        if (!isfinite(me[0]) || !isfinite(me[1]) || !isfinite(me[2]) ||
            fabsf(me[0]) > 3e7f || fabsf(me[2]) > 3e7f || fabsf(me[1]) > 5000.f) { usleep(8000); continue; }

        float bestd = 1e30f, bx = 0, by = 0, bz = 0, bhor = 0, alld = 1e30f;
        unsigned long lk_tmp_p = 0; float lk_tmp_x=0, lk_tmp_y=0, lk_tmp_z=0;
        float lockd = 1e30f, lkx = 0, lky = 0, lkz = 0, lkhor = 0;
        int bvi = -1;
        unsigned long besta = 0;
        pthread_mutex_lock(&g_mtx);
        for (int i = 0; i < g_ntg; i++) {
            unsigned long pp = g_tg[i].posp;
            if (!(g_mask & (1 << g_vcat[g_tg[i].vi]))) continue;   /* 阵营筛选 */
            if (!in_heap(pp)) continue;           /* 指针成垃圾 → 尸体/已回收，立刻跳过 */
            if (g_tg[i].obj == selfobj) continue;
            float t[3];
            if (!rd(pp, t, 12)) continue;
            if (!isfinite(t[0]) || !isfinite(t[1]) || !isfinite(t[2]) ||
                fabsf(t[0]) > 3e7f || fabsf(t[2]) > 3e7f) continue;
            /* 排除“自己”：服务端代理/副本就在我脚下，锁它会一直看自己后脑勺 */
            if (fabsf(t[0] - me[0]) < 1.4f && fabsf(t[2] - me[2]) < 1.4f &&
                fabsf(t[1] - me[1]) < 2.2f) continue;
            float dx = t[0] - me[0], dz = t[2] - me[2];
            float up = body_up(g_vname[g_tg[i].vi]);
            float ay = t[1] + up;                 /* 脚底 → 躯干（无箱时兜底）*/
            float bh = 0.f, bcy = 0.f;
            if (aabb_lookup(t[0], t[1], t[2], &bh, &bcy)) {
                if (bh < DEAD_H) {                /* 碰撞箱高塔到 0.2 → 已死 */
                    if (g_tg[i].obj == locked) AIMLOG("dead-drop %#lx h=%.2f", g_tg[i].obj, bh);
                    continue;
                }
                ay = bcy;                        /* 活体：瞄箱中心(胸口) */
            }
            float dy = ay - me[1];
            float dy0 = t[1] - me[1];
            float hor = sqrtf(dx * dx + dz * dz);
            /* 自身/自身服务端副本：水平 1.2 格内且上下 2.5 格内一律排除（ServerPlayer 会跟着你走） */
            if (hor < 1.2f && fabsf(dy0) < 2.5f) continue;
            float d = hor * hor + dy * dy;
            if (d < alld) alld = d;
            float lim = (g_tg[i].obj == locked) ? radius + 0.9f : radius;  /* 迟滞：锁定的允许跑到 5.9 格再放 */
            if (d > lim * lim) continue;
            if (d < bestd) { bestd = d; besta = g_tg[i].obj; bx = t[0]; by = ay; bz = t[2]; bhor = hor; bvi = g_tg[i].vi; lk_tmp_p = pp; lk_tmp_x = t[0]; lk_tmp_y = ay; lk_tmp_z = t[2]; }
            if (g_tg[i].obj == locked && d < lockd) { lockd = d; lkx = t[0]; lky = ay; lkz = t[2]; lkhor = hor; }
        }
        pthread_mutex_unlock(&g_mtx);

        if (now - lastdbg > 2.0) {
            lastdbg = now;
            fprintf(stderr, "[dbg] me=%.2f,%.2f,%.2f tg=%d near=%.2f nearest_all=%.2f\n",
                    me[0], me[1], me[2], g_ntg, besta ? sqrtf(bestd) : -1.0f,
                    alld < 1e29f ? sqrtf(alld) : -1.0f);
        }

        if (besta) {
            /* 粘性：已锁定的目标仍然有效时，只有新目标明显更近(近 10% 以上)才换，避免两只怪之间来回抽搐 */
            if (locked && lockd < 1e29f && besta != locked && bestd > lockd * 0.49f) {
                besta = locked; bestd = lockd; bx = lkx; by = lky; bz = lkz; bhor = lkhor;
            }
            if (besta != locked && lk_tmp_p) lockPosp = lk_tmp_p;
            if (besta != locked) AIMLOG("lock %#lx %s[%s] d=%.2f tgt=(%.1f,%.1f,%.1f)",
                                         besta, bvi >= 0 ? g_vname[bvi] : "?",
                                         bvi >= 0 ? g_catname[g_vcat[bvi]] : "?",
                                         sqrtf(bestd), bx, by, bz);
            locked = besta;
        } else {
            if (locked && lockPosp && rd(lockPosp, lockPos, 12) &&
                isfinite(lockPos[0]) && isfinite(lockPos[2]) &&
                fabsf(lockPos[0]) < 3e7f && fabsf(lockPos[2]) < 3e7f) {
                float dx = lockPos[0] - me[0], dz = lockPos[2] - me[2];
                float hor = sqrtf(dx*dx + dz*dz);
                if (hor < g_radius + 1.5f) { bx = lockPos[0]; by = lockPos[1] + 0.9f; bz = lockPos[2]; bhor = hor; besta = locked; bestd = hor*hor; }
            }
            if (!besta) { g_von = 0; if (locked) { AIMLOG("release %#lx", locked); aim_release(); g_wy = g_wp = 999; lockPosp = 0; } locked = 0; usleep(5000); continue; }
            locked = 0;
            usleep(5000);
            continue;
        }

        g_hasT = 1;
        snprintf(g_tname, sizeof(g_tname), "%s", (bvi >= 0) ? g_vname[bvi] : "?");
        g_tcat = (bvi >= 0) ? g_vcat[bvi] : -1;
        g_tdist = sqrtf(bestd);
        if (!g_enable) { g_von = 0; aim_release(); g_wy = g_wp = 999; usleep(4000); continue; }
        float yaw = atan2f(bx - me[0], bz - me[2]) + (float)M_PI;
        while (yaw > (float)M_PI) yaw -= 2 * (float)M_PI;
        while (yaw < -(float)M_PI) yaw += 2 * (float)M_PI;
        float pit = atan2f(by - me[1], bhor);

        (void)lastrot;
        /* 对照实验通道：/sdcard/mcaim_f.txt 写 “yaw pitch” 就强制写这个值 */
        { static double lastF = 0; static int fOn = 0; static float fY = 0, fP = 0;
          if (now - lastF > 0.5) { lastF = now;
            FILE* ff = fopen("/sdcard/mcaim_f.txt", "r");
            if (ff) { float a2 = 0, b2 = 0; int n2 = fscanf(ff, "%f %f", &a2, &b2); fclose(ff);
                      if (n2 == 2) { fOn = 1; fY = a2; fP = b2; } else fOn = 0; }
            else fOn = 0; }
          if (fOn) { yaw = fY; pit = fP; } }
        /* 靠近被打：进入设定距离，直接扣目标 Actor 的 health（+0x7C），不点屏幕、不需要看目标 */
        if (g_atkEn && g_tdist <= g_atkR && besta) {
            static double lastTap = 0;
            if (now - lastTap > 0.42) { lastTap = now;
                float hp = 0.f;
                if (rd(besta + 0x7C, &hp, 4) && hp > 0.f && hp < 2000.f) {
                    float nh = hp - g_atkDmg; if (nh < 0.f) nh = 0.f;
                    wr(besta + 0x7C, &nh, 4);
                    g_ntap++;
                    AIMLOG("atkhp obj=%#lx hp=%.2f -> %.2f d=%.2f", besta, hp, nh, g_tdist);
                } else AIMLOG("atkhp obj=%#lx bad hp=%.2f", besta, hp);
            }
        } else g_ntap = 0;
        if (pit >  1.56f) pit =  1.56f;
        if (pit < -1.56f) pit = -1.56f;
        g_vyaw = yaw; g_vpit = pit; g_von = (g_vn > 0) ? 1 : 0;
        if (fabsf(yaw - g_wy) > 0.0006f || fabsf(pit - g_wp) > 0.0006f) {
            g_wy = yaw; g_wp = pit;
            if (now - lastdbg > 2.0) { lastdbg = now; AIMLOG("viewwr yaw=%.2f pit=%.2f adr=%d", yaw*57.29578f, pit*57.29578f, g_vn); }
        }
        if (now - lchk > 1.0) {
            lchk = now;
            float cy[2] = { 0, 0 };
            int cok = rd(g_x_addr, cy, 8);
            AIMLOG("camchk ok=%d rot=%#lx cam=(%.3f,%.3f) want=(%.3f,%.3f)", cok, g_rot, cy[0], cy[1], yaw, pit);
        }
        usleep(1200);
    }
    return 0;
}




/* ================= 对外 API（给 McMod.cpp 用） ================= */
static void *aim_thread(void *arg);
volatile int g_aim_alive = 0;
static void *aim_thread_wrap(void *a) { g_aim_alive = 1; void *r = aim_thread(a); g_aim_alive = 0; AIMLOG("aim_thread exit"); return r; }

void aim_rot_probe(float dy) {
    if (!g_x_addr) { AIMLOG("rotprobe no cam"); return; }
    float c[2];
    if (!rd(g_x_addr, c, 8)) { AIMLOG("rotprobe read fail"); return; }
    float w[2] = { c[0] + dy, c[1] };
    int o1 = g_rot ? (wr(g_rot, w, 8) ? 1 : 0) : -1;
    int o2 = wr(g_x_addr, w, 8) ? 1 : 0;
    g_wy = 999; g_wp = 999;
    usleep(150000);
    float c2[2];
    int ok = rd(g_x_addr, c2, 8);
    AIMLOG("rotprobe dy=%.2f rot=%#lx o1=%d o2=%d cam=(%.3f,%.3f)->(%.3f,%.3f)",
           dy, g_rot, o1, o2, c[0], c[1], c2[0], c2[1]);
}

void aim_start(float radius, int mask)
{
    static int started = 0;
    (void)0;
    if (radius >= 3.f && radius <= 20.f) g_radius = radius;
    if (mask >= 0) g_mask = mask;
    if (started) return;
    started = 1;
    g_run = 1;
    pthread_t t;
    { if (started && g_aim_alive) return; { static double _lt = 0; double _n = now_s(); if (_n - _lt < 3.0) return; _lt = _n; } int _rc = pthread_create(&t, 0, aim_thread_wrap, 0); AIMLOG("aim_start rc=%d", _rc); if (_rc == 0) pthread_detach(t); else started = 0; }
}
void aim_stop(void)      { g_run = 0; }
void aim_set_enabled(int on) { g_enable = on ? 1 : 0; if (!on) g_hasT = 0; }
void aim_set_level(int ok) { g_levelReady = ok ? 1 : 0; }
int  aim_level_ready(void) { return g_levelReady; }
int  aim_enabled(void)   { return g_enable; }
void aim_set_mask(int m) { g_mask = m; show_mask(m); }
int  aim_mask(void)      { return g_mask; }
void aim_set_radius(float r) { if (r >= 3.f && r <= 20.f) g_radius = r; }
void  aim_atk_set_en(int v) { g_atkEn = v; }
int   aim_atk_enabled(void) { return g_atkEn; }
void  aim_atk_set_radius(float r) { if (r >= 2.f && r <= 8.f) g_atkR = r; }
float aim_atk_radius(void) { return g_atkR; }
int   aim_atk_taps(void) { return g_ntap; }
float aim_radius(void)   { return g_radius; }
int  aim_has(void)       { return g_hasT; }
int  aim_cat(void)       { return g_tcat; }
float aim_dist(void)     { return g_tdist; }
const char *aim_name(void){ return g_tname; }
const char *aim_catname_of(int c) { return (c >= 0 && c < 6) ? g_catname[c] : "?"; }
int  aim_vpt_count(void) { return g_nvpt; }
int  aim_obj_count(void) { return g_ntg; }
