// VmHook.c - 虚表记录型 hook（只记录+原样转发，不改行为）
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#define MAXT   12
#define SLOTS  48
#define CLOG   "/data/data/com.mojang.minecraftpe/files/vmhook.log"

extern void vmhook_common(void);
extern void vmhook_ret0(void);

typedef struct { unsigned long vt, off; int n; char tag[16]; unsigned long orig[SLOTS]; unsigned long thr[SLOTS]; } TBL;
static TBL g_t[MAXT];
static int g_nt;
static unsigned long g_base, g_tlo, g_thi;
static unsigned char *g_code; static long g_off; static long g_csize;
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static long g_qps[2] = {0,0};

static double nowd(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}

static unsigned long find_base(void){
  FILE*f=fopen("/proc/self/maps","r");if(!f)return 0;char l[512];unsigned long b=0;
  while(fgets(l,sizeof l,f)){ if(!strstr(l,"libminecraftpe.so"))continue;
    if(strstr(l,"r-xp")){unsigned long lo,hi;if(sscanf(l,"%lx-%lx",&lo,&hi)==2){b=lo;break;}}}fclose(f);return b;}

static int isvt(unsigned long vt){
  int ok=0;for(int i=0;i<16;i++){unsigned long f=*(unsigned long*)(vt+i*8);if(f>=g_tlo&&f<g_thi)ok++;}return ok>=13;
}

static void* mkcode_patch(int t,int s){
  unsigned int*p=(unsigned int*)(g_code+g_off);
  p[0]=0xD2800009u|((unsigned)s<<5);
  p[1]=0xD280000Au|((unsigned)t<<5);
  p[2]=0x58000050u;
  p[3]=0xD61F0200u;
  *(unsigned long*)(p+4)=(unsigned long)&vmhook_common;
  __builtin___clear_cache((char*)p,(char*)p+24);
  g_off+=24;return p;
}

static long g_last_ms[ MAXT ][ SLOTS ];

void* vmhook_log(long slot,long t,unsigned long*a){
  if(t<0||t>=g_nt||slot<0||slot>=g_t[t].n)return (void*)&vmhook_ret0;
  long ms=(long)(nowd()*1000);
  int rec=0;
  pthread_mutex_lock(&g_lk);
  long prev=ms/1000;
  if(prev!=g_qps[0]){g_qps[0]=prev;g_qps[1]=0;}
  if(g_qps[1]<400 && (ms-g_last_ms[t][slot])>500){ g_last_ms[t][slot]=ms; g_qps[1]++; rec=1; }
  pthread_mutex_unlock(&g_lk);
  if(rec){
    FILE*f=fopen(CLOG,"a");
    if(f){fprintf(f,"%.3f %s s=%ld this=%lx a1=%lx a2=%lx a3=%lx a4=%lx\n",nowd(),g_t[t].tag,slot,a?a[0]:0,a?a[1]:0,a?a[2]:0,a?a[3]:0,a?a[4]:0);fclose(f);}
  }
  unsigned long o=g_t[t].orig[slot];
  return o?(void*)o:(void*)&vmhook_ret0;
}

int vmhook_install(unsigned long vt_off,int n,const char*tag){
  if(g_nt>=MAXT)return 0;
  unsigned long vt=g_base+vt_off;
  if(!isvt(vt))return -1;
  unsigned long pg=vt&~0xFFFUL,end=(vt+n*8+0xFFFUL)&~0xFFFUL;
  int r=syscall(SYS_mprotect,(void*)pg,end-pg,PROT_READ|PROT_WRITE);
  if(r){return -2;}
  TBL*T=&g_t[g_nt];T->vt=vt;T->off=vt_off;T->n=n;strncpy(T->tag,tag,15);
  int good=0;
  for(int i=0;i<n;i++){
    unsigned long o=*(unsigned long*)(vt+i*8);
    if(o>=g_tlo&&o<g_thi&&g_off+24<=g_csize){ T->orig[i]=o; T->thr[i]=(unsigned long)mkcode_patch(g_nt,i); *(unsigned long*)(vt+i*8)=T->thr[i]; good++; }
    else T->orig[i]=0;
  }
  syscall(SYS_mprotect,(void*)pg,end-pg,PROT_READ);
  g_nt++;
  FILE*f=fopen(CLOG,"a");if(f){fprintf(f,"install %s vt=%#lx off=%#lx good=%d/%d\n",tag,vt,vt_off,good,n);fclose(f);}return good;
}

void vmhook_install_all(void){
  static int done=0;if(done)return;done=1;
  g_base=find_base();
  if(!g_base)return;
  g_tlo=g_base+0x6834c80UL; g_thi=g_base+0x135d39c8UL;
  g_csize=96*1024;
  g_code=(unsigned char*)mmap(NULL,g_csize,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(g_code==MAP_FAILED){g_code=NULL;return;}
  FILE*f=fopen(CLOG,"a");if(f){fprintf(f,"=== vmhook start base=%#lx tlo=%#lx\n",g_base,g_tlo);fclose(f);}
  vmhook_install(0x139F74E0UL,40,"LP");
  vmhook_install(0x136A4BF8UL,32,"c1");
  vmhook_install(0x13D07200UL,32,"c2");
  vmhook_install(0x13D09418UL,32,"c3");
  vmhook_install(0x138837E8UL,32,"c4");
  vmhook_install(0x13971AD8UL,32,"c5");
  vmhook_install(0x13F3D140UL,40,"c6");
  vmhook_install(0x13D32DA0UL,24,"c7");
}
