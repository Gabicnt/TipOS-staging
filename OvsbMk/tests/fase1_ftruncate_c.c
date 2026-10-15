#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdint.h>
static void wlog(const char *s){ write(1,s,strlen(s)); }
int main(void){
    wlog("[FASE1-FTR-C] start Caminho B /cache/fontconfig\n");
    wlog("[FASE1-FTR-C] mkdir /cache/fontconfig/sub\n");
    int r=mkdir("/cache/fontconfig/sub",0755);
    if(r==0) wlog("[FASE1-FTR-C] mkdir sub OK\n");
    else {
        if(errno==17) wlog("[FASE1-FTR-C] mkdir sub EEXIST OK\n");
        else { char b[128]; snprintf(b,sizeof(b),"[FASE1-FTR-C] mkdir sub FAIL errno=%d %s\n",errno,strerror(errno)); wlog(b); }
    }
    // verify stat
    struct stat st;
    if(stat("/cache/fontconfig/sub",&st)==0) wlog("[FASE1-FTR-C] stat sub OK\n");
    else wlog("[FASE1-FTR-C] stat sub FAIL\n");

    wlog("[FASE1-FTR-C] open /cache/fontconfig/test.cache O_CREAT\n");
    int fd=open("/cache/fontconfig/test.cache", O_RDWR|O_CREAT|O_TRUNC, 0644);
    if(fd<0){ char b[128]; snprintf(b,sizeof(b),"[FASE1-FTR-C] open FAIL errno=%d %s\n",errno,strerror(errno)); wlog(b); perror("open"); return 1; }
    char tmp[64]; snprintf(tmp,sizeof(tmp),"[FASE1-FTR-C] open ok fd=%d\n",fd); wlog(tmp);

    wlog("[FASE1-FTR-C] ftruncate 4096\n");
    if(ftruncate(fd,4096)<0){ char b[128]; snprintf(b,sizeof(b),"[FASE1-FTR-C] ftruncate FAIL errno=%d %s\n",errno,strerror(errno)); wlog(b); perror("ftruncate"); return 1; }
    wlog("[FASE1-FTR-C] ftruncate OK\n");

    wlog("[FASE1-FTR-C] mmap MAP_SHARED 4096\n");
    void *p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(p==MAP_FAILED){ char b[128]; snprintf(b,sizeof(b),"[FASE1-FTR-C] mmap FAIL errno=%d %s\n",errno,strerror(errno)); wlog(b); perror("mmap"); return 1; }
    wlog("[FASE1-FTR-C] mmap ok\n");

    wlog("[FASE1-FTR-C] write 0xCAFEBABE\n");
    *(volatile uint32_t*)p = 0xCAFEBABE;
    // ensure writeback via msync if exists
    // re-read via same mapping
    uint32_t v = *(volatile uint32_t*)p;
    char hex[64]; snprintf(hex,sizeof(hex),"[FASE1-FTR-C] re-read 0x%08x\n",v); wlog(hex);
    if(v==0xCAFEBABE) wlog("[FASE1-FTR-C] re-read PASS\n");
    else wlog("[FASE1-FTR-C] re-read FAIL\n");

    // also test persistence after munmap+reopen
    munmap(p,4096);
    close(fd);
    wlog("[FASE1-FTR-C] reopen verify\n");
    fd=open("/cache/fontconfig/test.cache", O_RDONLY);
    if(fd<0){ wlog("[FASE1-FTR-C] reopen FAIL\n"); return 1; }
    uint32_t v2=0;
    if(read(fd,&v2,4)!=4) wlog("[FASE1-FTR-C] read FAIL\n");
    else {
        snprintf(hex,sizeof(hex),"[FASE1-FTR-C] file read 0x%08x\n",v2); wlog(hex);
        if(v2==0xCAFEBABE) wlog("[FASE1-FTR-C] file PASS\n");
        else wlog("[FASE1-FTR-C] file FAIL\n");
    }
    close(fd);

    // also verify fstat size
    fd=open("/cache/fontconfig/test.cache", O_RDONLY);
    if(fd>=0){
        if(fstat(fd,&st)==0){ char b[64]; snprintf(b,sizeof(b),"[FASE1-FTR-C] fstat size=%ld\n",(long)st.st_size); wlog(b); }
        close(fd);
    }

    wlog("[FASE1-FTR-C] done PASS\n");
    return (v==0xCAFEBABE && v2==0xCAFEBABE)?0:1;
}
