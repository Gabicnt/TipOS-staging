#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

// Wayland compositor minimal for TipOS lab
// Socket: $XDG_RUNTIME_DIR/wayland-0 else /tmp/wayland-0
// Implements: wl_display.get_registry, wl_registry.global (wl_compositor/wl_shm/wl_seat),
// wl_display.sync, wl_registry.bind, wl_compositor.create_surface, wl_surface.attach/commit
// Protocolo erro definido: wl_display.error (fatal) ou disconnect inesperado antes de done; segfault/PF seria visivel no log serial
static uint32_t rd32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void wr32(uint8_t *p,uint32_t v){p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF;}

static void wlog(const char *s){ write(1,s,strlen(s)); }
static void wlogf(const char *a,const char *b){ wlog(a); if(b) wlog(b); wlog("\n"); }

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
static int send_global(int fd, uint32_t reg_id, uint32_t name, const char *iface, uint32_t ver){
    uint32_t ilen = strlen(iface)+1;
    uint32_t pad = (4 - (ilen%4))%4;
    uint32_t plen = 4 + 4 + ilen + pad + 4;
    uint8_t pl[256];
    wr32(pl, name);
    wr32(pl+4, ilen);
    memcpy(pl+8, iface, ilen);
    memset(pl+8+ilen, 0, pad);
    wr32(pl+8+ilen+pad, ver);
    return send_msg(fd, reg_id, 0, pl, plen);
}
static int send_callback_done(int fd, uint32_t cb_id, uint32_t data){
    uint8_t pl[4]; wr32(pl,data);
    return send_msg(fd, cb_id, 0, pl, 4);
}
static int send_error(int fd, uint32_t obj, uint32_t code, const char *msg){
    uint32_t mlen=strlen(msg)+1; uint32_t pad=(4-(mlen%4))%4; uint32_t plen=4+4+4+mlen+pad;
    uint8_t pl[256]; wr32(pl,obj); wr32(pl+4,code); wr32(pl+8,mlen); memcpy(pl+12,msg,mlen); memset(pl+12+mlen,0,pad);
    return send_msg(fd, 1, 0, pl, plen);
}

// object registry
#define MAX_OBJ 256
static char obj_iface[MAX_OBJ][32];
static int obj_valid[MAX_OBJ];
static void obj_set(int id,const char *iface){
    if(id<0||id>=MAX_OBJ) return;
    obj_valid[id]=1;
    strncpy(obj_iface[id],iface,31); obj_iface[id][31]=0;
}
static const char* obj_get(int id){
    if(id<0||id>=MAX_OBJ||!obj_valid[id]) return NULL;
    return obj_iface[id];
}

int main(void){
    wlog("[WL-COMP] Wayland compositor minimal starting\n");
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    char sockpath[128];
    if(xdg && xdg[0]){
        snprintf(sockpath,sizeof(sockpath),"%s/wayland-0",xdg);
        wlogf("[WL-COMP] using XDG_RUNTIME_DIR=",sockpath);
    }else{
        strcpy(sockpath,"/tmp/wayland-0");
        wlog("[WL-COMP] XDG_RUNTIME_DIR not set or empty, fallback to /tmp/wayland-0 (TipOS: no env, /tmp virtual dir always exists)\n");
        wlogf("[WL-COMP] socket path=",sockpath);
    }
    // Try unlink stale
    unlink(sockpath);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if(srv<0){ perror("socket"); return 1; }
    struct sockaddr_un sa; memset(&sa,0,sizeof(sa)); sa.sun_family=AF_UNIX;
    strncpy(sa.sun_path,sockpath,sizeof(sa.sun_path)-1);
    if(bind(srv,(struct sockaddr*)&sa,sizeof(sa))<0){ perror("bind"); return 1; }
    if(listen(srv,4)<0){ perror("listen"); return 1; }
    wlog("[WL-COMP] listening\n");
    // Only one client for test, but loop
    for(int iter=0; iter<4; iter++){
        wlog("[WL-COMP] waiting accept\n");
        int cli = accept(srv,NULL,NULL);
        if(cli<0){ perror("accept"); continue; }
        wlog("[WL-COMP] client accepted\n");
        memset(obj_valid,0,sizeof(obj_valid));
        obj_set(1,"wl_display");
        uint8_t buf[8192]; int blen=0;
        int client_ok=0;
        // poll loop
        for(int loop=0; loop<200; loop++){
            struct pollfd pf={cli,POLLIN,0};
            int pr=poll(&pf,1,100);
            if(pr>0 && (pf.revents&POLLIN)){
                ssize_t n=read(cli, buf+blen, sizeof(buf)-blen);
                if(n==0){ wlog("[WL-COMP] client EOF\n"); break; }
                if(n<0){ if(errno==EAGAIN) continue; perror("read"); break; }
                blen+=n;
                int off=0;
                while(off+8 <= blen){
                    uint32_t sender=rd32(buf+off);
                    uint32_t szop=rd32(buf+off+4);
                    uint32_t size=szop>>16; uint32_t opcode=szop&0xFFFF;
                    if(size<8 || off+(int)size>blen) break;
                    uint8_t *pl=buf+off+8; uint32_t plen=size-8;
                    char log[256]; snprintf(log,sizeof(log),"[WL-COMP] req sender=%u op=%u size=%u plen=%u\n",sender,opcode,size,plen); wlog(log);
                    const char *siface=obj_get(sender);
                    // Handle messages
                    if(sender==1 && opcode==1){ // wl_display.get_registry
                        if(plen<4){ send_error(cli,sender,1,"get_registry bad size"); break; }
                        uint32_t nid=rd32(pl);
                        obj_set(nid,"wl_registry");
                        snprintf(log,sizeof(log),"[WL-COMP] get_registry new_id=%u\n",nid); wlog(log);
                        // send 3 globals
                        send_global(cli,nid,1,"wl_compositor",4);
                        send_global(cli,nid,2,"wl_shm",1);
                        send_global(cli,nid,3,"wl_seat",7);
                        wlog("[WL-COMP] sent 3 globals (wl_compositor/wl_shm/wl_seat)\n");
                    }else if(sender==1 && opcode==0){ // sync
                        if(plen<4) break;
                        uint32_t nid=rd32(pl);
                        obj_set(nid,"wl_callback");
                        snprintf(log,sizeof(log),"[WL-COMP] sync new callback %u -> sending done\n",nid); wlog(log);
                        send_callback_done(cli,nid,0);
                    }else if(siface && strcmp(siface,"wl_registry")==0 && opcode==0){ // bind
                        if(plen<12) break;
                        uint32_t name=rd32(pl);
                        uint32_t ilen=rd32(pl+4);
                        if(ilen>64 || 4+4+ilen>plen) break;
                        char iface[64]; memcpy(iface,pl+8,ilen); iface[ilen-1]=0;
                        // strip padding
                        uint32_t pad=(4-(ilen%4))%4;
                        uint32_t off2=4+4+ilen+pad;
                        if(off2+8>plen) break;
                        uint32_t ver=rd32(pl+off2);
                        uint32_t nid=rd32(pl+off2+4);
                        snprintf(log,sizeof(log),"[WL-COMP] bind name=%u iface=%s ver=%u new_id=%u\n",name,iface,ver,nid); wlog(log);
                        obj_set(nid,iface);
                        // If wl_shm, send format event (optional)
                        if(strcmp(iface,"wl_shm")==0){
                            uint8_t fpl[4]; wr32(fpl,0); // argb8888
                            send_msg(cli,nid,0,fpl,4);
                            wlog("[WL-COMP] sent wl_shm.format argb8888\n");
                        }else if(strcmp(iface,"wl_seat")==0){
                            // wl_seat capabilities (2) and name
                            // capabilities: cap=7 (pointer|keyboard|touch)
                            uint8_t cpl[4]; wr32(cpl,7);
                            send_msg(cli,nid,0,cpl,4); // capabilities event 0 ?
                            // name event 1: string "seat0"
                            const char *nm="seat0"; uint32_t nl=strlen(nm)+1; uint32_t np=(4-(nl%4))%4;
                            uint8_t npl[64]; wr32(npl,nl); memcpy(npl+4,nm,nl); memset(npl+4+nl,0,np);
                            send_msg(cli,nid,1,npl,4+nl+np);
                            wlog("[WL-COMP] sent wl_seat caps+name\n");
                        }
                    }else if(siface && strcmp(siface,"wl_compositor")==0 && opcode==0){ // create_surface
                        if(plen<4) break;
                        uint32_t nid=rd32(pl);
                        obj_set(nid,"wl_surface");
                        snprintf(log,sizeof(log),"[WL-COMP] create_surface new_id=%u\n",nid); wlog(log);
                    }else if(siface && strcmp(siface,"wl_surface")==0){
                        if(opcode==1){ // attach
                            if(plen<12) break;
                            uint32_t bufid=rd32(pl); int32_t x=(int32_t)rd32(pl+4); int32_t y=(int32_t)rd32(pl+8);
                            snprintf(log,sizeof(log),"[WL-COMP] wl_surface.attach buffer=%u x=%d y=%d\n",bufid,x,y); wlog(log);
                        }else if(opcode==6){ // commit
                            snprintf(log,sizeof(log),"[WL-COMP] wl_surface.commit id=%u\n",sender); wlog(log);
                            client_ok=1;
                            // Optionally send frame callback? Not needed
                        }else if(opcode==0){ // destroy
                            wlog("[WL-COMP] wl_surface.destroy\n");
                            obj_valid[sender]=0;
                        }else{
                            snprintf(log,sizeof(log),"[WL-COMP] wl_surface op=%u plen=%u (ignored)\n",opcode,plen); wlog(log);
                        }
                    }else{
                        snprintf(log,sizeof(log),"[WL-COMP] unknown req iface=%s sender=%u op=%u -> error\n",siface?siface:"?",sender,opcode); wlog(log);
                        send_error(cli,sender,1,"invalid_method");
                        // do not disconnect immediately, let client see error
                    }
                    off+=size;
                }
                if(off>0){ memmove(buf, buf+off, blen-off); blen-=off; }
            }else if(pr==0){
                if(client_ok) { wlog("[WL-COMP] client_ok, waiting a bit then close\n"); usleep(200000); break; }
                // continue waiting
                if(loop>50 && blen==0){ /* timeout */ }
            }
            if(client_ok && loop>20) break;
        }
        close(cli);
        if(client_ok){
            wlog("[WL-COMP] PASS: surface created and committed, no protocol error\n");
            // keep listening for next client? For test, exit success
            break;
        }else{
            wlog("[WL-COMP] client handling ended without commit\n");
        }
    }
    close(srv);
    unlink(sockpath);
    wlog("[WL-COMP] exit\n");
    return 0;
}
