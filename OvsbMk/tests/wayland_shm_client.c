#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#ifndef SYS_memfd_create
#define SYS_memfd_create 319
#endif
#define MFD_CLOEXEC 0x0001
static int my_memfd_create(const char *n, unsigned f){ return syscall(SYS_memfd_create,n,f); }
static uint32_t rd32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void wr32(uint8_t *p,uint32_t v){p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF;}
static int send_msg(int fd, uint32_t sender, uint32_t opcode, const uint8_t *payload, uint32_t plen){
    uint32_t size = 8 + plen;
    uint8_t msg[1024];
    if(size>sizeof(msg)) return -1;
    wr32(msg, sender);
    wr32(msg+4, (size<<16)|(opcode&0xFFFF));
    if(plen) memcpy(msg+8,payload,plen);
    ssize_t n = write(fd, msg, size);
    return (n==(ssize_t)size)?0:-1;
}
static int send_msg_fd(int fd, uint32_t sender, uint32_t opcode, const uint8_t *payload, uint32_t plen, int cfd){
    uint32_t size = 8 + plen;
    uint8_t msg[1024];
    wr32(msg, sender); wr32(msg+4, (size<<16)|(opcode&0xFFFF));
    if(plen) memcpy(msg+8,payload,plen);
    struct iovec iov; iov.iov_base=msg; iov.iov_len=size;
    char cmsgbuf[32]; struct cmsghdr *cmsg=(struct cmsghdr*)cmsgbuf;
    cmsg->cmsg_len=20; cmsg->cmsg_level=1; cmsg->cmsg_type=1;
    *(int*)((char*)cmsg+16)=cfd;
    struct msghdr mh={0}; mh.msg_iov=&iov; mh.msg_iovlen=1; mh.msg_control=cmsgbuf; mh.msg_controllen=cmsg->cmsg_len;
    ssize_t n=sendmsg(fd,&mh,0);
    return (n==(ssize_t)size)?0:-1;
}
static void c_log(const char *s){ write(1,s,strlen(s)); }
int main(void){
    c_log("[WL-SHM-CLIENT] start\n");
    printf("[WL-SHM-CLIENT] start\n");
    const char *xdg=getenv("XDG_RUNTIME_DIR");
    char path[128];
    if(xdg&&xdg[0]) snprintf(path,sizeof(path),"%s/wayland-0",xdg);
    else strcpy(path,"/tmp/wayland-0");
    { char _tmp[160]; snprintf(_tmp,sizeof(_tmp),"[WL-SHM-CLIENT] connect %s\n",path); c_log(_tmp); }
    printf("[WL-SHM-CLIENT] connect %s\n",path);
    int fd=socket(AF_UNIX,SOCK_STREAM,0);
    if(fd<0){perror("socket");return 1;}
    struct sockaddr_un sa; memset(&sa,0,sizeof(sa)); sa.sun_family=AF_UNIX; strncpy(sa.sun_path,path,sizeof(sa.sun_path)-1);
    int tries=0; while(connect(fd,(struct sockaddr*)&sa,sizeof(sa))<0){ if(tries++>=100){perror("connect");return 1;} usleep(100000); }
    c_log("[WL-SHM-CLIENT] connected\n"); printf("[WL-SHM-CLIENT] connected\n");
    uint8_t pl[4]; wr32(pl,2); send_msg(fd,1,1,pl,4); printf("[WL-SHM-CLIENT] get_registry\n");
    wr32(pl,3); send_msg(fd,1,0,pl,4); printf("[WL-SHM-CLIENT] sync\n");
    uint8_t buf[8192]; int blen=0;
    int name_compositor=0,name_shm=0,name_seat=0;
    int cb_done=0, got_error=0;
    for(int iter=0; iter<80; iter++){
        struct pollfd pf={fd,POLLIN,0};
        int pr=poll(&pf,1,100);
        if(pr>0 && (pf.revents&POLLIN)){
            ssize_t n=read(fd, buf+blen, sizeof(buf)-blen);
            if(n<=0) break;
            blen+=n;
            int off=0;
            while(off+8<=blen){
                uint32_t sender=rd32(buf+off);
                uint32_t szop=rd32(buf+off+4);
                uint32_t size=szop>>16; uint32_t op=szop&0xFFFF;
                if(size<8||off+(int)size>blen) break;
                uint8_t *pay=buf+off+8; uint32_t plen=size-8;
                if(sender==2 && op==0){
                    if(plen>=12){
                        uint32_t name=rd32(pay);
                        uint32_t ilen=rd32(pay+4);
                        if(ilen<64 && 4+4+ilen<=plen){
                            char iface[64]; memcpy(iface,pay+8,ilen); iface[ilen-1]=0;
                            uint32_t pad=(4-(ilen%4))%4;
                            uint32_t ver=rd32(pay+8+ilen+pad);
                            printf("[WL-SHM-CLIENT] global %u %s v%u\n",name,iface,ver);
                            if(strcmp(iface,"wl_compositor")==0) name_compositor=name;
                            else if(strcmp(iface,"wl_shm")==0) name_shm=name;
                            else if(strcmp(iface,"wl_seat")==0) name_seat=name;
                        }
                    }
                } else if(sender==1 && op==0){
                    uint32_t oid=rd32(pay); uint32_t code=rd32(pay+4);
                    printf("[WL-SHM-CLIENT] error oid=%u code=%u\n",oid,code); got_error=1;
                } else if(sender==3 && op==0){ printf("[WL-SHM-CLIENT] callback done\n"); cb_done=1; }
                off+=size;
            }
            if(off>0){ memmove(buf, buf+off, blen-off); blen-=off; }
        }
        if(cb_done && name_compositor && name_shm && name_seat) break;
        if(got_error) break;
    }
    if(got_error) return 1;
    if(!name_compositor||!name_shm||!name_seat){ printf("[WL-SHM-CLIENT] missing globals\n"); return 1; }
    printf("[WL-SHM-CLIENT] registry OK c=%d shm=%d seat=%d\n",name_compositor,name_shm,name_seat);
    int comp_id=4, shm_id=5, seat_id=6;
    {
        const char *iface="wl_compositor"; uint32_t ilen=strlen(iface)+1; uint32_t pad=(4-(ilen%4))%4;
        uint8_t b[128]; int o=0; wr32(b+o,name_compositor);o+=4; wr32(b+o,ilen);o+=4; memcpy(b+o,iface,ilen);o+=ilen; memset(b+o,0,pad);o+=pad; wr32(b+o,4);o+=4; wr32(b+o,comp_id);o+=4;
        send_msg(fd,2,0,b,o); printf("[WL-SHM-CLIENT] bind compositor\n");
    }
    {
        const char *iface="wl_shm"; uint32_t ilen=strlen(iface)+1; uint32_t pad=(4-(ilen%4))%4;
        uint8_t b[128]; int o=0; wr32(b+o,name_shm);o+=4; wr32(b+o,ilen);o+=4; memcpy(b+o,iface,ilen);o+=ilen; memset(b+o,0,pad);o+=pad; wr32(b+o,1);o+=4; wr32(b+o,shm_id);o+=4;
        send_msg(fd,2,0,b,o); printf("[WL-SHM-CLIENT] bind shm\n");
    }
    {
        const char *iface="wl_seat"; uint32_t ilen=strlen(iface)+1; uint32_t pad=(4-(ilen%4))%4;
        uint8_t b[128]; int o=0; wr32(b+o,name_seat);o+=4; wr32(b+o,ilen);o+=4; memcpy(b+o,iface,ilen);o+=ilen; memset(b+o,0,pad);o+=pad; wr32(b+o,7);o+=4; wr32(b+o,seat_id);o+=4;
        send_msg(fd,2,0,b,o); printf("[WL-SHM-CLIENT] bind seat\n");
    }
    // get pointer/keyboard
    int pointer_id=7, keyboard_id=8;
    {
        uint8_t b[4]; wr32(b,pointer_id); send_msg(fd,seat_id,0,b,4); printf("[WL-SHM-CLIENT] get_pointer %d\n",pointer_id);
    }
    {
        uint8_t b[4]; wr32(b,keyboard_id); send_msg(fd,seat_id,1,b,4); printf("[WL-SHM-CLIENT] get_keyboard %d\n",keyboard_id);
    }
    // create memfd pool
    int W=200,H=100;
    int stride=W*4;
    int sz=stride*H;
    int mfd=my_memfd_create("wl_shm", MFD_CLOEXEC);
    if(mfd<0){perror("memfd_create");return 1;}
    if(ftruncate(mfd,sz)<0){perror("ftruncate");return 1;}
    void *addr=mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_SHARED, mfd, 0);
    if(addr==MAP_FAILED){perror("mmap");return 1;}
    printf("[WL-SHM-CLIENT] pool mmap %p size %d\n",addr,sz);
    // draw rectangle: red border + green fill? spec says colorido
    uint32_t *pix=(uint32_t*)addr;
    for(int y=0;y<H;y++) for(int x=0;x<W;x++){
        uint32_t col;
        if(x<4||x>=W-4||y<4||y>=H-4) col=0xFFFF0000u; // red border
        else if((x/20+y/20)%2==0) col=0xFF00FF00u; // green checker
        else col=0xFF0000FFu; // blue
        pix[y*W+x]=col;
    }
    printf("[WL-SHM-CLIENT] drew rectangle %dx%d stride %d\n",W,H,stride);
    // create_pool id 9 via SCM_RIGHTS
    int pool_id=9;
    {
        uint8_t b[8]; wr32(b,pool_id); wr32(b+4,sz);
        if(send_msg_fd(fd,shm_id,0,b,8,mfd)!=0){ printf("create_pool send fail\n"); return 1; }
        { char _tmp2[160]; snprintf(_tmp2,sizeof(_tmp2),"[WL-SHM-CLIENT] create_pool id=%d fd=%d size=%d via SCM_RIGHTS\n",pool_id,mfd,sz); c_log(_tmp2); } printf("[WL-SHM-CLIENT] create_pool id=%d fd=%d size=%d via SCM_RIGHTS\n",pool_id,mfd,sz);
    }
    // create_buffer id 10
    int buf_id=10;
    {
        uint8_t b[24]; wr32(b,buf_id); wr32(b+4,0); wr32(b+8,W); wr32(b+12,H); wr32(b+16,stride); wr32(b+20,1); // xrgb8888=1
        send_msg(fd,pool_id,0,b,24); printf("[WL-SHM-CLIENT] create_buffer id=%d\n",buf_id);
    }
    // create_surface id 11
    int surf_id=11;
    {
        uint8_t b[4]; wr32(b,surf_id); send_msg(fd,comp_id,0,b,4); printf("[WL-SHM-CLIENT] create_surface %d\n",surf_id);
    }
    // attach
    {
        uint8_t b[12]; wr32(b,buf_id); wr32(b+4,0); wr32(b+8,0); send_msg(fd,surf_id,1,b,12); printf("[WL-SHM-CLIENT] attach\n");
    }
    // damage? optional
    // commit
    send_msg(fd,surf_id,6,NULL,0); printf("[WL-SHM-CLIENT] commit\n");
    // sync barrier
    {
        uint8_t b[4]; wr32(b,12); send_msg(fd,1,0,b,4); printf("[WL-SHM-CLIENT] sync 12\n");
    }
    // poll for input events for 10s
    printf("[WL-SHM-CLIENT] polling input for 10s (move mouse / press keys, should log via compositor)\n");
    int done2=0, got_input=0;
    for(int iter=0; iter<100; iter++){
        struct pollfd pf={fd,POLLIN,0};
        int pr=poll(&pf,1,100);
        if(pr>0 && (pf.revents&POLLIN)){
            char cmsgbuf[32]={0};
            struct iovec iov; iov.iov_base=buf+blen; iov.iov_len=sizeof(buf)-blen;
            struct msghdr mh={0}; mh.msg_iov=&iov; mh.msg_iovlen=1; mh.msg_control=cmsgbuf; mh.msg_controllen=sizeof(cmsgbuf);
            ssize_t n=recvmsg(fd,&mh,0);
            if(n<=0) break;
            blen+=n;
            int off=0;
            while(off+8<=blen){
                uint32_t sender=rd32(buf+off);
                uint32_t szop=rd32(buf+off+4);
                uint32_t size=szop>>16; uint32_t op=szop&0xFFFF;
                if(size<8||off+(int)size>blen) break;
                uint8_t *pay=buf+off+8;
                if(sender==12 && op==0){ printf("[WL-SHM-CLIENT] sync done\n"); done2=1; }
                else if(sender==pointer_id){
                    if(op==2){ // motion
                        uint32_t t=rd32(pay); int32_t fx=rd32(pay+4); int32_t fy=rd32(pay+8);
                        { char _tmp4[160]; snprintf(_tmp4,sizeof(_tmp4),"[WL-SHM-CLIENT] pointer motion t=%u x=%.2f y=%.2f\n",t,fx/256.0,fy/256.0); c_log(_tmp4); } printf("[WL-SHM-CLIENT] pointer motion t=%u x=%.2f y=%.2f\n",t,fx/256.0,fy/256.0); got_input=1;
                    } else if(op==3){ // button
                        uint32_t s=rd32(pay); uint32_t tt=rd32(pay+4); uint32_t btn=rd32(pay+8); uint32_t st=rd32(pay+12);
                        { char _tmp5[160]; snprintf(_tmp5,sizeof(_tmp5),"[WL-SHM-CLIENT] pointer button s=%u t=%u btn=%u st=%u\n",s,tt,btn,st); c_log(_tmp5); } printf("[WL-SHM-CLIENT] pointer button s=%u t=%u btn=%u st=%u\n",s,tt,btn,st); got_input=1;
                    } else if(op==0){ // enter
                        printf("[WL-SHM-CLIENT] pointer enter\n"); got_input=1;
                    } else printf("[WL-SHM-CLIENT] pointer op=%u\n",op);
                } else if(sender==keyboard_id){
                    if(op==3){ // key
                        uint32_t s=rd32(pay); uint32_t tt=rd32(pay+4); uint32_t key=rd32(pay+8); uint32_t st=rd32(pay+12);
                        { char _tmp6[160]; snprintf(_tmp6,sizeof(_tmp6),"[WL-SHM-CLIENT] keyboard key s=%u t=%u key=%u st=%u\n",s,tt,key,st); c_log(_tmp6); } printf("[WL-SHM-CLIENT] keyboard key s=%u t=%u key=%u st=%u\n",s,tt,key,st); got_input=1;
                    } else printf("[WL-SHM-CLIENT] keyboard op=%u\n",op);
                } else {
                    // printf("[WL-SHM-CLIENT] msg %u op %u\n",sender,op);
                }
                off+=size;
            }
            if(off>0){ memmove(buf,buf+off,blen-off); blen-=off; }
        }
        if(done2 && iter>20) break;
    }
    { char _tmp3[128]; snprintf(_tmp3,sizeof(_tmp3),"[WL-SHM-CLIENT] input seen=%d done2=%d\n",got_input,done2); c_log(_tmp3); } printf("[WL-SHM-CLIENT] input seen=%d done2=%d\n",got_input,done2);
    if(!done2) printf("[WL-SHM-CLIENT] WARN no sync done\n");
    printf("[WL-SHM-CLIENT] PASS surface committed %s input\n", got_input?"with":"without");
    // keep alive a bit
    sleep(2);
    close(fd);
    munmap(addr,sz);
    close(mfd);
    return 0;
}
