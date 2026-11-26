#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sys/uio.h>
extern long process_vm_readv(int,const struct iovec*,unsigned long,const struct iovec*,unsigned long,unsigned long);
#include <unistd.h>
#include <stdarg.h>
static void blog(const char*fmt,...){
  FILE*f=fopen("/data/data/com.mojang.minecraftpe/files/mcstage.log","a");
  if(!f)return; va_list ap; va_start(ap,fmt);
  fputs("[blood] ",f); vfprintf(f,fmt,ap); fputc(10,f);
  va_end(ap); fclose(f);
}
#define CHUNK (1<<16)
#define MAXC 2048
static volatile int g_on=0, g_run=0, g_needscan=0, g_phase=0;
static volatile unsigned long g_addr=0;
static char g_msg[96]="未开启";
static unsigned long g_c[MAXC];
static float g_b[MAXC];
static volatile int g_nc=0;
static int g_failsegs=0;
int blood_on(void){ return g_on; }
const char* blood_status(void){ return g_msg; }
void blood_reset(void){ __atomic_store_n(&g_addr,0,__ATOMIC_RELAXED); g_needscan=1; }
void blood_set_on(int v){
  g_on=v;
  if(!v){ __atomic_store_n(&g_addr,0,__ATOMIC_RELAXED); g_phase=0; snprintf(g_msg,sizeof g_msg,"已关闭"); }
  else { g_needscan=1; g_phase=0; snprintf(g_msg,sizeof g_msg,"扫描中..."); }
}
static int scan_all(int fd){
  FILE*mf=fopen("/proc/self/maps","r"); if(!mf)return -1;
  unsigned char*buf=(unsigned char*)malloc(CHUNK); if(!buf){fclose(mf);return -1;}
  int n=0; char line[512];
  while(fgets(line,sizeof line,mf)){
    unsigned long s,e; char perms[8],path[256]={0};
    int k=sscanf(line,"%lx-%lx %7s %*s %*s %*s %255s",&s,&e,perms,path);
    if(k<3)continue;
    if(perms[0]!='r'||perms[1]!='w')continue;
    if(k>=4&&strstr(path,".so"))continue;
    for(unsigned long a=s;a<e&&n<MAXC;a+=CHUNK){
      unsigned long ln=CHUNK; if(a+ln>e)ln=e-a;
      struct iovec li={buf,ln}, ri={(void*)a,ln};
      if((unsigned long)process_vm_readv(getpid(),&li,1,&ri,1,0)!=ln) memcpy(buf,(void*)a,ln);
      for(unsigned long o=0;o+0x80<=ln;o+=8){
        float f68,f74,f7c;
        memcpy(&f68,buf+o+0x68,4); memcpy(&f74,buf+o+0x74,4); memcpy(&f7c,buf+o+0x7c,4);
        if(fabsf(f68-20.f)<0.01f&&fabsf(f74)<0.001f&&f7c>0.05f&&f7c<=20.05f){
          g_c[n]=a+o; g_b[n]=f7c; n++;
          if(n>=MAXC)goto full;
        }
      }
    }
  }
full:
  free(buf);fclose(mf);g_nc=n;blog("scan hits=%d fail=%d",n,g_failsegs); g_failsegs=0;
  for(int i=0;i<n;i++) blog("cand[%d] %#lx f7c=%.5f",i,g_c[i],(double)g_b[i]);
  return n;
}
static int rd4(int fd,unsigned long a,float*v){
  struct iovec li={v,4}, ri={(void*)a,4};
  if((unsigned long)process_vm_readv(getpid(),&li,1,&ri,1,0)==4) return 4;
  memcpy(v,(void*)a,4); return 4;
}
static void wr4(unsigned long a,float v){ memcpy((void*)a,&v,4); }
static void* blood_thr(void*arg){
  (void)arg; int fd=-1;
  while(!g_run) usleep(2000);
  fd=open("/proc/self/mem",O_RDWR);
  while(1){
    if(!g_on){ usleep(20000); continue; }
    if(g_needscan){ g_needscan=0; g_phase=1; __atomic_store_n(&g_addr,0,__ATOMIC_RELAXED); }
    if(g_phase==1){
      int n=scan_all(fd);
      if(n<=0){ snprintf(g_msg,sizeof g_msg,"没找到候选"); usleep(800000); continue; }
      snprintf(g_msg,sizeof g_msg,"监控中（候选%d），掉一点血自动锁定",n);
      g_phase=2;
    } else if(g_phase==2){
      int drop=-1; float dmax=0.2f; int n=g_nc;
      for(int i=0;i<n&&i<MAXC;i++){
        float v=0; if(rd4(fd,g_c[i],&v)==4){
          if(v>0.05f && g_b[i]-v>dmax){ dmax=g_b[i]-v; drop=i; }
        }
      }
      static int hb=0; if((hb++%100)==0) blog("mon n=%d drop=%d",n,drop);
      if(drop>=0){
        blog("DROP idx=%d addr=%#lx base=%.2f now=%.2f",drop,g_c[drop],(double)g_b[drop],(double)(g_b[drop]-dmax));
        __atomic_store_n(&g_addr,g_c[drop],__ATOMIC_RELAXED);
        snprintf(g_msg,sizeof g_msg,"已锁定 %#lx (掉到%.1f) 已补满",g_c[drop],(float)(g_b[drop]-dmax));
        g_phase=3;
      }
      usleep(40000);
    } else if(g_phase==3){
      unsigned long a=g_addr; float v=0.f,mx=20.f;
      if(rd4(fd,a,&v)==4){
        if(rd4(fd,a+0x68,&mx)!=4||mx<=0.f)mx=20.f;
        if(v<mx-0.01f&&v>0.f){ wr4(a,mx); }
      } else { __atomic_store_n(&g_addr,0,__ATOMIC_RELAXED); g_phase=0; g_needscan=1; }
      usleep(3000);
    } else usleep(20000);
  }
  return 0;
}
void blood_start(void){
  if(g_run) return; g_run=1;
  pthread_t t; pthread_create(&t,0,blood_thr,0);
}
