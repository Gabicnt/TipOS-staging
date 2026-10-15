#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
static void wlog(const char *s){ write(1,s,strlen(s)); }
int main(void){
    const char *paths[]={
        "/USR/SHARE/FONTS/DEJAVUS.TTF",
        "/usr/share/fonts/DEJAVUS.TTF",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/USR/SHARE/FONTS/DEJAVU~1.TTF",
        0};
    int fd=-1;
    for(int i=0;paths[i];i++){
        wlog("[FASE1-MAP] try open "); wlog(paths[i]); wlog("\n");
        fd=open(paths[i], O_RDONLY);
        if(fd>=0){ char tmp[128]; snprintf(tmp,sizeof(tmp),"[FASE1-MAP] open ok %s fd=%d\n",paths[i],fd); wlog(tmp); break; }
        char err[128]; snprintf(err,sizeof(err),"[FASE1-MAP] open fail %s\n",paths[i]); wlog(err);
    }
    if(fd<0){ perror("open ttf"); wlog("[FASE1-MAP] FAIL open all\n"); return 1; }
    char hdr[4]={0};
    if(read(fd,hdr,4)!=4){ perror("read"); return 1; }
    unsigned char *h=(unsigned char*)hdr;
    char hex[64]; int o=0;
    for(int i=0;i<4;i++){ hex[o++]="0123456789abcdef"[h[i]>>4]; hex[o++]="0123456789abcdef"[h[i]&15]; hex[o++]=' '; } hex[o]=0;
    wlog("[FASE1-MAP] read 4 bytes: "); wlog(hex); wlog("\n");
    if(h[0]==0 && h[1]==1 && h[2]==0 && h[3]==0) wlog("[FASE1-MAP] header 00 01 00 00 PASS\n");
    else wlog("[FASE1-MAP] header unexpected\n");
    lseek(fd,0,0);
    void *p=mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if(p==MAP_FAILED){ perror("mmap"); wlog("[FASE1-MAP] FAIL mmap\n"); return 1; }
    h=(unsigned char*)p;
    o=0; for(int i=0;i<4;i++){ hex[o++]="0123456789abcdef"[h[i]>>4]; hex[o++]="0123456789abcdef"[h[i]&15]; hex[o++]=' '; } hex[o]=0;
    wlog("[FASE1-MAP] mmap 4 bytes: "); wlog(hex); wlog("\n");
    if(h[0]==0 && h[1]==1 && h[2]==0 && h[3]==0) wlog("[FASE1-MAP] mmap PASS 00 01 00 00\n");
    else wlog("[FASE1-MAP] mmap FAIL\n");
    munmap(p,4096);
    close(fd);
    wlog("[FASE1-MAP] done\n");
    return 0;
}
