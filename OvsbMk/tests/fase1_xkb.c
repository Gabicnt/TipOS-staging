#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
static void wlog(const char *s){ write(1,s,strlen(s)); }
int main(void){
    const char *paths[]={
        "/USR/SHARE/X11/XKB/RULES/EVDEV.XML",
        "/usr/share/X11/xkb/rules/evdev.xml",
        "/USR/SHARE/X11/XKB/RULES/evdev.xml",
        0};
    int fd=-1;
    for(int i=0;paths[i];i++){
        wlog("[FASE1-XKB] try open "); wlog(paths[i]); wlog("\n");
        fd=open(paths[i], O_RDONLY);
        if(fd>=0){ char tmp[128]; snprintf(tmp,sizeof(tmp),"[FASE1-XKB] open ok %s fd=%d\n",paths[i],fd); wlog(tmp); break; }
        wlog("[FASE1-XKB] fail\n");
    }
    if(fd<0){ perror("open evdev.xml"); wlog("[FASE1-XKB] FAIL open\n"); return 1; }
    char buf[256]={0};
    int n=read(fd,buf,200);
    if(n<=0){ perror("read"); return 1; }
    buf[n]=0;
    wlog("[FASE1-XKB] first 200 bytes:\n");
    write(1,buf,n); wlog("\n");
    close(fd);
    wlog("[FASE1-XKB] PASS\n");
    return 0;
}
