#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <string.h>
static void wlog(const char *s){ write(1,s,__builtin_strlen(s)); }
int main(void){
    wlog("[FASE1-FTR] mkdir /root/.cache/fontconfig\n");
    if(mkdir("/root/.cache/fontconfig", 0755)==0) wlog("[FASE1-FTR] mkdir ok\n");
    else { perror("mkdir"); wlog("[FASE1-FTR] mkdir maybe exists\n"); }
    wlog("[FASE1-FTR] open /root/.cache/fontconfig/test.cache\n");
    int fd=open("/root/.cache/fontconfig/test.cache", O_RDWR|O_CREAT|O_TRUNC, 0644);
    if(fd<0){ perror("open"); wlog("[FASE1-FTR] FAIL open\n"); return 1; }
    wlog("[FASE1-FTR] ftruncate 8192\n");
    if(ftruncate(fd,8192)<0){ perror("ftruncate"); wlog("[FASE1-FTR] FAIL ftruncate\n"); return 1; }
    wlog("[FASE1-FTR] ftruncate ok\n");
    void *p=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(p==MAP_FAILED){ perror("mmap"); wlog("[FASE1-FTR] FAIL mmap\n"); return 1; }
    wlog("[FASE1-FTR] mmap ok\n");
    memset(p,0xAB,8192);
    wlog("[FASE1-FTR] wrote AB\n");
    munmap(p,8192);
    struct stat st; if(fstat(fd,&st)==0){ char tmp[64]; int n=snprintf(tmp,sizeof(tmp),"[FASE1-FTR] fstat size=%ld\n",(long)st.st_size); write(1,tmp,n); }
    close(fd);
    wlog("[FASE1-FTR] done PASS\n");
    return 0;
}
