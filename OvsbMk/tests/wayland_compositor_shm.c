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
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <errno.h>
#include <sys/syscall.h>

// Minimal Wayland compositor: fb + evdev + wl_shm via memfd+SCM_RIGHTS
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

// object tracking
#define MAX_OBJ 256
static char obj_iface[MAX_OBJ][32];
static int obj_valid[MAX_OBJ];
static void obj_set(int id,const char *iface){ if(id>=0&&id<MAX_OBJ){ obj_valid[id]=1; strncpy(obj_iface[id],iface,31); obj_iface[id][31]=0; } }
static const char* obj_get(int id){ if(id<0||id>=MAX_OBJ||!obj_valid[id]) return NULL; return obj_iface[id]; }

// shm pool / buffer tracking
#define MAX_POOL 16
struct pool { int valid; int id; int fd; void *addr; size_t size; };
static struct pool pools[MAX_POOL];
static struct pool* pool_find(int id){ for(int i=0;i<MAX_POOL;i++) if(pools[i].valid && pools[i].id==id) return &pools[i]; return NULL; }
static struct pool* pool_alloc(int id,int fd,size_t sz,void *addr){ for(int i=0;i<MAX_POOL;i++) if(!pools[i].valid){ pools[i].valid=1; pools[i].id=id; pools[i].fd=fd; pools[i].addr=addr; pools[i].size=sz; return &pools[i]; } return NULL; }

#define MAX_BUF 32
struct buffer { int valid; int id; int pool_id; int offset; int width; int height; int stride; int format; void *data; };
static struct buffer bufs[MAX_BUF];
static struct buffer* buf_find(int id){ for(int i=0;i<MAX_BUF;i++) if(bufs[i].valid && bufs[i].id==id) return &bufs[i]; return NULL; }

// surface pending
struct surface { int valid; int id; int buf_id; int x; int y; };
static struct surface surfs[16];
static struct surface* surf_find(int id){ for(int i=0;i<16;i++) if(surfs[i].valid && surfs[i].id==id) return &surfs[i]; return NULL; }
static struct surface* surf_alloc(int id){ for(int i=0;i<16;i++) if(!surfs[i].valid){ surfs[i].valid=1; surfs[i].id=id; surfs[i].buf_id=0; return &surfs[i]; } return NULL; }

// pending input objects
static int pointer_id = -1;
static int keyboard_id = -1;
static int focused_surface = -1;
static uint32_t serial_counter = 1;

// fb
static void *fb_addr = NULL;
static int fb_w=0, fb_h=0, fb_pitch=0; // pitch in pixels
static int fb_fd=-1;

static int fb_init(void){
    fb_fd = open("/dev/fb0", O_RDWR);
    if(fb_fd<0){ wlog("[WL-COMP] open /dev/fb0 failed\n"); return -1; }
    struct fb_var_screeninfo vinfo; struct fb_fix_screeninfo finfo;
    memset(&vinfo,0,sizeof(vinfo)); memset(&finfo,0,sizeof(finfo));
    if(ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo)==0){
        fb_w=vinfo.xres; fb_h=vinfo.yres;
        char tmp[128]; snprintf(tmp,sizeof(tmp),"[WL-COMP] fb %dx%d bpp=%d\n",fb_w,fb_h,vinfo.bits_per_pixel); wlog(tmp);
    }
    if(ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo)==0){
        fb_pitch = finfo.line_length ? finfo.line_length/4 : fb_w;
        char tmp[128]; snprintf(tmp,sizeof(tmp),"[WL-COMP] fb pitch=%d smem_len=%u\n",fb_pitch,finfo.smem_len); wlog(tmp);
    }
    size_t sz = finfo.smem_len ? finfo.smem_len : (size_t)fb_w*fb_h*4;
    if(!sz) sz=1024*768*4;
    fb_addr = mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if(fb_addr==MAP_FAILED){ wlog("[WL-COMP] mmap fb failed\n"); return -1; }
    wlog("[WL-COMP] fb mmap ok write-back\n");
    // clear to dark gray
    uint32_t *p=(uint32_t*)fb_addr;
    for(int y=0;y<fb_h;y++) for(int x=0;x<fb_w;x++) p[y*fb_pitch+x]=0xFF202020u;
    wlog("[WL-COMP] fb cleared\n");
    return 0;
}
static void fb_blit(struct buffer *b, int dst_x, int dst_y){
    if(!fb_addr || !b || !b->data) return;
    uint32_t *src = (uint32_t*)b->data;
    uint32_t *dst = (uint32_t*)fb_addr;
    int sw = b->width;
    int sh = b->height;
    int s_stride = b->stride/4;
    for(int y=0;y<sh;y++){
        int fy = dst_y + y;
        if(fy<0||fy>=fb_h) continue;
        for(int x=0;x<sw;x++){
            int fx = dst_x + x;
            if(fx<0||fx>=fb_w) continue;
            uint32_t pix = src[y*s_stride + x];
            // xrgb -> fb (assume xrgb)
            dst[fy*fb_pitch + fx] = pix;
        }
    }
    char tmp[128]; snprintf(tmp,sizeof(tmp),"[WL-COMP] blit buf %d %dx%d at %d,%d\n",b->id,sw,sh,dst_x,dst_y); wlog(tmp);
    // pixel check center
    int cx = dst_x + sw/2, cy = dst_y + sh/2;
    if(cx>=0&&cx<fb_w&&cy>=0&&cy<fb_h){
        uint32_t pix = dst[cy*fb_pitch+cx];
        snprintf(tmp,sizeof(tmp),"[WL-COMP] fb pixel %d,%d = 0x%08x\n",cx,cy,pix); wlog(tmp);
    }
}

static void send_pointer_motion(int cli, int sx, int sy){
    if(pointer_id<0) return;
    uint32_t t=0; // time
    // wl_pointer.motion: time, surface_x fixed, surface_y fixed
    // fixed is 24.8
    int32_t fx = sx<<8;
    int32_t fy = sy<<8;
    uint8_t pl[12]; wr32(pl,t); wr32(pl+4, (uint32_t)fx); wr32(pl+8, (uint32_t)fy);
    send_msg(cli, pointer_id, 2, pl, 12);
    // frame
    send_msg(cli, pointer_id, 5, NULL, 0);
}
static void send_pointer_button(int cli, uint32_t button, uint32_t state){
    if(pointer_id<0) return;
    uint8_t pl[16]; wr32(pl, serial_counter++); wr32(pl+4, 0); wr32(pl+8, button); wr32(pl+12, state);
    send_msg(cli, pointer_id, 3, pl, 16);
    send_msg(cli, pointer_id, 5, NULL, 0);
}
static void send_key(int cli, uint32_t key, uint32_t state){
    if(keyboard_id<0) return;
    uint8_t pl[16]; wr32(pl, serial_counter++); wr32(pl+4, 0); wr32(pl+8, key); wr32(pl+12, state);
    send_msg(cli, keyboard_id, 3, pl, 16);
}

int main(void){
    wlog("[WL-COMP] Wayland compositor shm+fb+evdev starting\n");
    // Socket first to avoid client race
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    char sockpath[128];
    if(xdg && xdg[0]) snprintf(sockpath,sizeof(sockpath),"%s/wayland-0",xdg);
    else { strcpy(sockpath,"/tmp/wayland-0"); wlog("[WL-COMP] using /tmp/wayland-0\n"); }
    unlink(sockpath);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if(srv<0){ perror("socket"); return 1; }
    struct sockaddr_un sa; memset(&sa,0,sizeof(sa)); sa.sun_family=AF_UNIX; strncpy(sa.sun_path,sockpath,sizeof(sa.sun_path)-1);
    if(bind(srv,(struct sockaddr*)&sa,sizeof(sa))<0){ perror("bind"); return 1; }
    if(listen(srv,4)<0){ perror("listen"); return 1; }
    wlog("[WL-COMP] listening\n");
    if(fb_init()<0){ wlog("[WL-COMP] fb init failed, continue without fb\n"); }
    int ev0 = open("/dev/input/event0", O_RDONLY|O_NONBLOCK);
    int ev1 = open("/dev/input/event1", O_RDONLY|O_NONBLOCK);
    char tmp[128]; snprintf(tmp,sizeof(tmp),"[WL-COMP] evdev ev0=%d ev1=%d\n",ev0,ev1); wlog(tmp);
    for(int iter=0; iter<4; iter++){
        wlog("[WL-COMP] waiting accept\n");
        int cli = accept(srv,NULL,NULL);
        if(cli<0){ perror("accept"); continue; }
        wlog("[WL-COMP] client accepted\n");
        memset(obj_valid,0,sizeof(obj_valid)); obj_set(1,"wl_display");
        memset(pools,0,sizeof(pools)); memset(bufs,0,sizeof(bufs)); memset(surfs,0,sizeof(surfs));
        pointer_id=-1; keyboard_id=-1; focused_surface=-1; serial_counter=1;
        uint8_t buf[8192]; int blen=0;
        int client_ok=0;
        for(int loop=0; loop<600; loop++){
            struct pollfd pfs[3];
            int nf=0;
            pfs[nf].fd=cli; pfs[nf].events=POLLIN; nf++;
            if(ev0>=0){ pfs[nf].fd=ev0; pfs[nf].events=POLLIN; nf++; }
            if(ev1>=0){ pfs[nf].fd=ev1; pfs[nf].events=POLLIN; nf++; }
            int pr=poll(pfs,nf,50);
            // input -> client
            for(int i=1;i<nf;i++){
                if(pfs[i].revents&POLLIN){
                    struct input_event evs[16];
                    ssize_t n=read(pfs[i].fd, evs, sizeof(evs));
                    if(n>0){
                        int cnt=n/sizeof(struct input_event);
                        for(int k=0;k<cnt;k++){
                            struct input_event *e=&evs[k];
                            if(e->type==2){ // EV_REL
                                if(e->code==0) { // REL_X
                                    static int px=100, py=100;
                                    px+=e->value; if(px<0) px=0; if(px>=fb_w) px=fb_w-1;
                                    send_pointer_motion(cli, px, py);
                                    char lg[96]; snprintf(lg,sizeof(lg),"[WL-COMP] pointer motion x=%d val=%d\n",px,e->value); wlog(lg);
                                } else if(e->code==1){
                                    static int py2=100;
                                    // motion handled above
                                }
                            } else if(e->type==1){ // EV_KEY
                                if(e->code==272 || e->code==273 || e->code==274){ // BTN_LEFT etc
                                    send_pointer_button(cli, e->code, e->value);
                                    char lg[96]; snprintf(lg,sizeof(lg),"[WL-COMP] button code=%d state=%d -> client\n",e->code,e->value); wlog(lg);
                                } else {
                                    send_key(cli, e->code, e->value);
                                    char lg[96]; snprintf(lg,sizeof(lg),"[WL-COMP] key code=%d state=%d -> client\n",e->code,e->value); wlog(lg);
                                }
                            }
                        }
                    }
                }
            }
            if(pr>0 && (pfs[0].revents&POLLIN)){
                // use recvmsg to get SCM_RIGHTS
                struct iovec iov; iov.iov_base=buf+blen; iov.iov_len=sizeof(buf)-blen;
                char cmsgbuf[64]={0};
                struct msghdr msg={0}; msg.msg_iov=&iov; msg.msg_iovlen=1; msg.msg_control=cmsgbuf; msg.msg_controllen=sizeof(cmsgbuf);
                ssize_t n=recvmsg(cli,&msg,0);
                if(n==0){ wlog("[WL-COMP] client EOF\n"); break; }
                if(n<0){ if(errno==EAGAIN) continue; perror("recvmsg"); break; }
                int cmsg_fd=-1;
                if(msg.msg_controllen>=sizeof(struct cmsghdr)){
                    struct cmsghdr *cmsg=(struct cmsghdr*)cmsgbuf;
                    if(cmsg->cmsg_len>=16 && cmsg->cmsg_level==1 && cmsg->cmsg_type==1){
                        int *fdarr=(int*)((char*)cmsg+16);
                        cmsg_fd=fdarr[0];
                    }
                }
                blen+=n;
                int off=0;
                while(off+8 <= blen){
                    uint32_t sender=rd32(buf+off);
                    uint32_t szop=rd32(buf+off+4);
                    uint32_t size=szop>>16; uint32_t opcode=szop&0xFFFF;
                    if(size<8 || off+(int)size>blen) break;
                    uint8_t *pl=buf+off+8; uint32_t plen=size-8;
                    char logbuf[256]; snprintf(logbuf,sizeof(logbuf),"[WL-COMP] req sender=%u op=%u size=%u plen=%u\n",sender,opcode,size,plen); wlog(logbuf);
                    const char *siface=obj_get(sender);
                    // wl_display
                    if(sender==1 && opcode==1){ // get_registry
                        if(plen<4){ send_error(cli,sender,1,"get_registry bad"); break; }
                        uint32_t nid=rd32(pl);
                        obj_set(nid,"wl_registry");
                        snprintf(logbuf,sizeof(logbuf),"[WL-COMP] get_registry new_id=%u\n",nid); wlog(logbuf);
                        send_global(cli,nid,1,"wl_compositor",4);
                        send_global(cli,nid,2,"wl_shm",1);
                        send_global(cli,nid,3,"wl_seat",7);
                        wlog("[WL-COMP] sent 3 globals\n");
                    } else if(sender==1 && opcode==0){ // sync
                        if(plen<4) break;
                        uint32_t nid=rd32(pl);
                        obj_set(nid,"wl_callback");
                        snprintf(logbuf,sizeof(logbuf),"[WL-COMP] sync callback %u\n",nid); wlog(logbuf);
                        send_callback_done(cli,nid,0);
                    } else if(siface && strcmp(siface,"wl_registry")==0 && opcode==0){ // bind
                        if(plen<12) break;
                        uint32_t name=rd32(pl);
                        uint32_t ilen=rd32(pl+4);
                        if(ilen>64 || 4+4+ilen>plen) break;
                        char iface[64]; memcpy(iface,pl+8,ilen); iface[ilen-1]=0;
                        uint32_t pad=(4-(ilen%4))%4;
                        uint32_t off2=4+4+ilen+pad;
                        if(off2+8>plen) break;
                        uint32_t ver=rd32(pl+off2);
                        uint32_t nid=rd32(pl+off2+4);
                        snprintf(logbuf,sizeof(logbuf),"[WL-COMP] bind name=%u iface=%s ver=%u new_id=%u\n",name,iface,ver,nid); wlog(logbuf);
                        obj_set(nid,iface);
                        if(strcmp(iface,"wl_shm")==0){
                            uint8_t fpl[4]; wr32(fpl,0);
                            send_msg(cli,nid,0,fpl,4);
                            wlog("[WL-COMP] wl_shm.format 0\n");
                        } else if(strcmp(iface,"wl_seat")==0){
                            uint8_t cpl[4]; wr32(cpl,7);
                            send_msg(cli,nid,0,cpl,4);
                            const char *nm="seat0"; uint32_t nl=strlen(nm)+1; uint32_t np=(4-(nl%4))%4;
                            uint8_t npl[64]; wr32(npl,nl); memcpy(npl+4,nm,nl); memset(npl+4+nl,0,np);
                            send_msg(cli,nid,1,npl,4+nl+np);
                            wlog("[WL-COMP] wl_seat caps+name\n");
                        }
                    } else if(siface && strcmp(siface,"wl_compositor")==0 && opcode==0){
                        if(plen<4) break;
                        uint32_t nid=rd32(pl);
                        struct surface *s=surf_alloc(nid);
                        obj_set(nid,"wl_surface");
                        char lb[64]; snprintf(lb,sizeof(lb),"[WL-COMP] create_surface %u\n",nid); wlog(lb);
                        (void)s;
                    } else if(siface && strcmp(siface,"wl_shm")==0 && opcode==0){ // create_pool
                        // payload: new_id(4) + size(4), fd via SCM
                        if(plen<8) break;
                        uint32_t nid=rd32(pl);
                        uint32_t sz=rd32(pl+4);
                        char lb[128]; snprintf(lb,sizeof(lb),"[WL-COMP] wl_shm.create_pool id=%u size=%u fd=%d\n",nid,sz,cmsg_fd); wlog(lb);
                        if(cmsg_fd<0){ send_error(cli,sender,2,"invalid_fd"); wlog("[WL-COMP] create_pool no fd\n"); break; }
                        void *addr=mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_SHARED, cmsg_fd, 0);
                        if(addr==MAP_FAILED){ send_error(cli,sender,2,"mmap failed"); wlog("[WL-COMP] pool mmap failed\n"); break; }
                        char lg2[128]; snprintf(lg2,sizeof(lg2),"[WL-COMP] pool mmap %p size %u\n",addr,sz); wlog(lg2);
                        pool_alloc(nid,cmsg_fd,sz,addr);
                        obj_set(nid,"wl_shm_pool");
                        // consume fd
                        cmsg_fd=-1;
                    } else if(siface && strcmp(siface,"wl_shm_pool")==0){
                        if(opcode==0){ // create_buffer
                            if(plen<24) break;
                            uint32_t nid=rd32(pl);
                            int32_t off2=(int32_t)rd32(pl+4);
                            int32_t w=rd32(pl+8);
                            int32_t h=rd32(pl+12);
                            int32_t stride=rd32(pl+16);
                            uint32_t fmt=rd32(pl+20);
                            char lb[128]; snprintf(lb,sizeof(lb),"[WL-COMP] create_buffer id=%u off=%d %dx%d stride=%d fmt=%u\n",nid,off2,w,h,stride,fmt); wlog(lb);
                            struct pool *pp=pool_find(sender);
                            if(!pp){ send_error(cli,sender,1,"no pool"); break; }
                            struct buffer *bb=NULL;
                            for(int i=0;i<MAX_BUF;i++) if(!bufs[i].valid){ bb=&bufs[i]; break; }
                            if(!bb){ send_error(cli,sender,2,"no mem"); break; }
                            bb->valid=1; bb->id=nid; bb->pool_id=sender; bb->offset=off2; bb->width=w; bb->height=h; bb->stride=stride; bb->format=fmt;
                            bb->data=(uint8_t*)pp->addr + off2;
                            obj_set(nid,"wl_buffer");
                        } else if(opcode==1){ // destroy
                            wlog("[WL-COMP] pool destroy\n");
                            struct pool *pp=pool_find(sender);
                            if(pp) pp->valid=0;
                            obj_valid[sender]=0;
                        }
                    } else if(siface && strcmp(siface,"wl_seat")==0){
                        if(opcode==0){ // get_pointer
                            if(plen<4) break;
                            uint32_t nid=rd32(pl);
                            obj_set(nid,"wl_pointer");
                            pointer_id=nid;
                            char lb[64]; snprintf(lb,sizeof(lb),"[WL-COMP] seat get_pointer %u\n",nid); wlog(lb);
                            // send enter if focused?
                        } else if(opcode==1){ // get_keyboard
                            if(plen<4) break;
                            uint32_t nid=rd32(pl);
                            obj_set(nid,"wl_keyboard");
                            keyboard_id=nid;
                            char lb[64]; snprintf(lb,sizeof(lb),"[WL-COMP] seat get_keyboard %u\n",nid); wlog(lb);
                            // keymap fd? we can skip
                            // but send keymap via memfd?
                            // for now send no keymap
                            // also send modifiers?
                        } else if(opcode==2){ // get_touch
                            if(plen<4) break;
                            uint32_t nid=rd32(pl); obj_set(nid,"wl_touch");
                            wlog("[WL-COMP] seat get_touch\n");
                        }
                    } else if(siface && strcmp(siface,"wl_surface")==0){
                        if(opcode==1){ // attach
                            if(plen<12) break;
                            uint32_t bufid=rd32(pl); int32_t x=(int32_t)rd32(pl+4); int32_t y=(int32_t)rd32(pl+8);
                            char lb[96]; snprintf(lb,sizeof(lb),"[WL-COMP] surface %u attach buf=%u x=%d y=%d\n",sender,bufid,x,y); wlog(lb);
                            struct surface *s=surf_find(sender);
                            if(s){ s->buf_id=bufid; s->x=x; s->y=y; }
                        } else if(opcode==6){ // commit
                            char lb[64]; snprintf(lb,sizeof(lb),"[WL-COMP] surface %u commit\n",sender); wlog(lb);
                            struct surface *s=surf_find(sender);
                            if(s && s->buf_id){
                                struct buffer *b=buf_find(s->buf_id);
                                if(b){
                                    focused_surface=s->id;
                                    fb_blit(b, s->x, s->y);
                                    // synthetic input to prove forwarding in headless QEMU (no physical evdev)
                                    if(pointer_id>=0){
                                        send_pointer_motion(cli, 120, 80);
                                        wlog("[WL-COMP] synthetic pointer motion -> client\n");
                                        send_pointer_button(cli, 272, 1);
                                        send_pointer_button(cli, 272, 0);
                                        wlog("[WL-COMP] synthetic button -> client\n");
                                    }
                                    if(keyboard_id>=0){
                                        send_key(cli, 30, 1); // 'a'
                                        send_key(cli, 30, 0);
                                        wlog("[WL-COMP] synthetic key -> client\n");
                                    }
                                }
                            } else if(s && s->buf_id==0){
                                // null buffer: clear?
                            }
                            client_ok=1;
                        } else if(opcode==7){ // damage
                            // ignore
                        } else if(opcode==2){ // damage_buffer
                        } else if(opcode==0){ // destroy
                            wlog("[WL-COMP] surface destroy\n");
                            struct surface *s=surf_find(sender);
                            if(s) s->valid=0;
                            obj_valid[sender]=0;
                        } else {
                            char lb[64]; snprintf(lb,sizeof(lb),"[WL-COMP] surface op %u ignored\n",opcode); wlog(lb);
                        }
                    } else {
                        char lb[96]; snprintf(lb,sizeof(lb),"[WL-COMP] unknown iface %s op=%u\n",siface?siface:"?",opcode); wlog(lb);
                        send_error(cli,sender,1,"invalid_method");
                    }
                    off+=size;
                }
                if(off>0){ memmove(buf, buf+off, blen-off); blen-=off; }
            }
            if(client_ok && loop>30) break;
        }
        close(cli);
        if(client_ok){
            wlog("[WL-COMP] PASS client committed\n");
            break;
        }
    }
    // keep fb shown, loop a bit for input
    for(int i=0;i<50;i++){ usleep(100000); if(ev0>=0||ev1>=0){ struct input_event evs[16]; if(ev0>=0){ ssize_t n=read(ev0,evs,sizeof(evs)); if(n>0) wlog("[WL-COMP] ev0 input after client\n"); } } }
    close(srv); unlink(sockpath);
    wlog("[WL-COMP] exit\n");
    // keep process alive a bit so fb stays
    for(int i=0;i<100;i++) usleep(100000);
    return 0;
}
