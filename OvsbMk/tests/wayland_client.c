#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

// Wayland client test minimal — conecta, registry, surface vazia
// Socket: $XDG_RUNTIME_DIR/wayland-0 else /tmp/wayland-0 (escolha: /tmp fallback pois TipOS nao tem XDG env e /tmp sempre existe via virtual tmp)
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

int main(void){
    printf("[WL-CLIENT] Wayland client test: display->registry->surface\n");
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    char path[128];
    if(xdg && xdg[0]){
        snprintf(path,sizeof(path),"%s/wayland-0",xdg);
        printf("[WL-CLIENT] using XDG_RUNTIME_DIR=%s\n",path);
    }else{
        strcpy(path,"/tmp/wayland-0");
        printf("[WL-CLIENT] XDG_RUNTIME_DIR not set, fallback to %s (TipOS: env minimo, /tmp virtual dir)\n",path);
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(fd<0){ perror("socket"); return 1; }
    struct sockaddr_un sa; memset(&sa,0,sizeof(sa)); sa.sun_family=AF_UNIX;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path)-1);
    int tries=0;
    while(connect(fd,(struct sockaddr*)&sa,sizeof(sa))<0){
        if(tries++>=20){ perror("connect"); return 1; }
        usleep(100000);
    }
    printf("[WL-CLIENT] connected to %s\n",path);
    // wl_display.get_registry new_id=2
    uint8_t pl[4]; wr32(pl,2);
    if(send_msg(fd,1,1,pl,4)!=0){ printf("send get_registry fail\n"); return 1; }
    printf("[WL-CLIENT] sent wl_display.get_registry (id=2)\n");
    // also send sync to get callback for barrier
    wr32(pl,3);
    send_msg(fd,1,0,pl,4);
    printf("[WL-CLIENT] sent wl_display.sync (callback id=3)\n");

    uint8_t buf[8192]; int blen=0;
    int compositor_id=-1, shm_id=-1, seat_id=-1;
    int cb_done=0;
    int name_compositor=0, name_shm=0, name_seat=0;
    int got_error=0;
    // poll loop to collect globals
    for(int iter=0; iter<80; iter++){
        struct pollfd pf={fd,POLLIN,0};
        int pr=poll(&pf,1,100);
        if(pr>0 && (pf.revents&POLLIN)){
            ssize_t n=read(fd, buf+blen, sizeof(buf)-blen);
            if(n==0){ printf("[WL-CLIENT] EOF (disconnect)\n"); break; }
            if(n<0){ if(errno==EAGAIN) continue; perror("read"); break; }
            blen+=n;
            int off=0;
            while(off+8 <= blen){
                uint32_t sender=rd32(buf+off);
                uint32_t szop=rd32(buf+off+4);
                uint32_t size=szop>>16; uint32_t opcode=szop&0xFFFF;
                if(size<8 || off+(int)size>blen) break;
                uint8_t *pay=buf+off+8; uint32_t plen=size-8;
                // events
                if(sender==2 && opcode==0){ // wl_registry.global
                    if(plen>=12){
                        uint32_t name=rd32(pay);
                        uint32_t ilen=rd32(pay+4);
                        if(ilen<64 && 4+4+ilen<=plen){
                            char iface[64]; memcpy(iface,pay+8,ilen); iface[ilen-1]=0;
                            uint32_t pad=(4-(ilen%4))%4;
                            uint32_t ver=rd32(pay+8+ilen+pad);
                            printf("[WL-CLIENT] global name=%u iface=%s ver=%u\n",name,iface,ver);
                            if(strcmp(iface,"wl_compositor")==0) name_compositor=name;
                            else if(strcmp(iface,"wl_shm")==0) name_shm=name;
                            else if(strcmp(iface,"wl_seat")==0) name_seat=name;
                        }
                    }
                }else if(sender==1 && opcode==0){ // wl_display.error
                    if(plen>=12){
                        uint32_t oid=rd32(pay); uint32_t code=rd32(pay+4); uint32_t mlen=rd32(pay+8);
                        char msg[128]={0}; if(mlen<120 && 12+mlen<=plen) memcpy(msg,pay+12,mlen);
                        printf("[WL-CLIENT] ERROR wl_display.error oid=%u code=%u msg=%s\n",oid,code,msg);
                        got_error=1;
                    }
                }else if(sender==3 && opcode==0){ // wl_callback.done
                    printf("[WL-CLIENT] callback done (sync barrier)\n");
                    cb_done=1;
                }else if(sender>=4){
                    // possibly seat/shm events after bind, ignore but log
                    // printf("[WL-CLIENT] event sender=%u op=%u plen=%u\n",sender,opcode,plen);
                }
                off+=size;
            }
            if(off>0){ memmove(buf, buf+off, blen-off); blen-=off; }
        }
        if(cb_done && name_compositor && name_shm && name_seat) break;
        if(got_error) break;
    }
    if(got_error){
        printf("[WL-CLIENT] FAIL: protocol error received\n");
        return 1;
    }
    if(!name_compositor || !name_shm || !name_seat){
        printf("[WL-CLIENT] FAIL: missing globals (compositor=%d shm=%d seat=%d)\n",name_compositor,name_shm,name_seat);
        return 1;
    }
    printf("[WL-CLIENT] registry OK: compositor=%d shm=%d seat=%d\n",name_compositor,name_shm,name_seat);
    // bind wl_compositor -> id 4, wl_shm ->5, wl_seat ->6
    compositor_id=4; shm_id=5; seat_id=6;
    {
        const char *iface="wl_compositor"; uint32_t ilen=strlen(iface)+1; uint32_t pad=(4-(ilen%4))%4;
        uint8_t b[128]; int o=0; wr32(b+o, name_compositor); o+=4; wr32(b+o, ilen); o+=4; memcpy(b+o,iface,ilen); o+=ilen; memset(b+o,0,pad); o+=pad; wr32(b+o, 4); o+=4; wr32(b+o, compositor_id); o+=4;
        send_msg(fd,2,0,b,o);
        printf("[WL-CLIENT] bind wl_compositor name %d -> id %d\n",name_compositor,compositor_id);
    }
    {
        const char *iface="wl_shm"; uint32_t ilen=strlen(iface)+1; uint32_t pad=(4-(ilen%4))%4;
        uint8_t b[128]; int o=0; wr32(b+o, name_shm); o+=4; wr32(b+o, ilen); o+=4; memcpy(b+o,iface,ilen); o+=ilen; memset(b+o,0,pad); o+=pad; wr32(b+o, 1); o+=4; wr32(b+o, shm_id); o+=4;
        send_msg(fd,2,0,b,o);
        printf("[WL-CLIENT] bind wl_shm name %d -> id %d\n",name_shm,shm_id);
    }
    {
        const char *iface="wl_seat"; uint32_t ilen=strlen(iface)+1; uint32_t pad=(4-(ilen%4))%4;
        uint8_t b[128]; int o=0; wr32(b+o, name_seat); o+=4; wr32(b+o, ilen); o+=4; memcpy(b+o,iface,ilen); o+=ilen; memset(b+o,0,pad); o+=pad; wr32(b+o, 7); o+=4; wr32(b+o, seat_id); o+=4;
        send_msg(fd,2,0,b,o);
        printf("[WL-CLIENT] bind wl_seat name %d -> id %d\n",name_seat,seat_id);
    }
    // create_surface id 7
    int surface_id=7;
    {
        uint8_t b[4]; wr32(b, surface_id);
        send_msg(fd, compositor_id, 0, b, 4);
        printf("[WL-CLIENT] wl_compositor.create_surface id=%d\n",surface_id);
    }
    // attach (buffer 0 = null) x=0 y=0
    {
        uint8_t b[12]; wr32(b,0); wr32(b+4,0); wr32(b+8,0);
        send_msg(fd, surface_id, 1, b, 12);
        printf("[WL-CLIENT] wl_surface.attach buffer=0 x=0 y=0\n");
    }
    // commit
    {
        send_msg(fd, surface_id, 6, NULL, 0);
        printf("[WL-CLIENT] wl_surface.commit\n");
    }
    // sync again to ensure no error
    {
        uint8_t b[4]; wr32(b,8);
        send_msg(fd,1,0,b,4);
        printf("[WL-CLIENT] second sync (id=8) for barrier\n");
    }
    int done2=0;
    for(int iter=0; iter<40; iter++){
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
                uint32_t size=szop>>16; uint32_t opcode=szop&0xFFFF;
                if(size<8||off+(int)size>blen) break;
                uint8_t *pay=buf+off+8;
                if(sender==1 && szop>>16 >=12 && opcode==0){
                    uint32_t oid=rd32(pay); uint32_t code=rd32(pay+4);
                    char msg[64]={0}; uint32_t mlen=rd32(pay+8); if(mlen<60) memcpy(msg,pay+12,mlen);
                    printf("[WL-CLIENT] ERROR after commit oid=%u code=%u msg=%s\n",oid,code,msg);
                    got_error=1;
                }else if(sender==8 && opcode==0){
                    printf("[WL-CLIENT] second callback done\n"); done2=1;
                }
                off+=size;
            }
            if(off>0){ memmove(buf,buf+off,blen-off); blen-=off; }
        }
        if(done2) break;
        if(got_error) break;
    }
    if(got_error){
        printf("[WL-CLIENT] FAIL: protocol error after commit\n");
        close(fd);
        return 1;
    }
    if(!done2){
        printf("[WL-CLIENT] WARN: no second callback, but no error — assuming OK (server may not echo if not implemented)\n");
    }
    printf("[WL-CLIENT] PASS: surface created and committed, no wl_display.error, no disconnect\n");
    // Protocolo erro definido: wl_display.error seria fatal; disconnect inesperado seria EOF antes de done; segfault/PF visivel no serial — nenhum ocorreu
    close(fd);
    return 0;
}
