/* LAYER_SHELL_TEST — FASE 8.0a: valida wlr-layer-shell no compositor.
 * Protocolo raw (mesmo padrão do wayland_shm_client.c, sem libwayland):
 * cria layer background (wallpaper fullscreen) + layer top (barra 24px),
 * verifica configure/ack, imprime PASS. Roda no TipOS (static). */
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
#ifndef SYS_memfd_create
#define SYS_memfd_create 319
#endif
#define MFD_CLOEXEC 0x0001
static int my_memfd_create(const char *n, unsigned f){ return syscall(SYS_memfd_create,n,f); }
static uint32_t rd32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void wr32(uint8_t *p,uint32_t v){p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF;}
static int send_msg(int fd, uint32_t sender, uint32_t opcode, const uint8_t *payload, uint32_t plen){
    uint32_t size = 8 + plen;
    uint8_t msg[2048];
    if(size>sizeof(msg)) return -1;
    wr32(msg, sender);
    wr32(msg+4, (size<<16)|(opcode&0xFFFF));
    if(plen) memcpy(msg+8,payload,plen);
    ssize_t n = write(fd, msg, size);
    return (n==(ssize_t)size)?0:-1;
}
static int send_msg_fd(int fd, uint32_t sender, uint32_t opcode, const uint8_t *payload, uint32_t plen, int cfd){
    uint32_t size = 8 + plen;
    uint8_t msg[2048];
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
/* bind helper: registry(bind): name + "iface" + version + new_id */
static void do_bind(int fd, uint32_t reg, uint32_t name, const char *iface, uint32_t ver, uint32_t nid){
    uint32_t ilen=strlen(iface)+1, pad=(4-(ilen%4))%4;
    uint8_t b[128]; int o=0;
    wr32(b+o,name);o+=4; wr32(b+o,ilen);o+=4; memcpy(b+o,iface,ilen);o+=ilen;
    memset(b+o,0,pad);o+=pad; wr32(b+o,ver);o+=4; wr32(b+o,nid);o+=4;
    send_msg(fd,reg,0,b,o);
}
/* monta pool shm solido via memfd+SCM_RIGHTS; retorna addr (mmap) */
static void *make_pool(int fd, uint32_t shm_id, uint32_t pool_id, int W, int H, uint32_t color, int *sz_out){
    char l[128];
    int stride=W*4, sz=stride*H;
    int mfd=my_memfd_create("layer_shm", MFD_CLOEXEC);
    snprintf(l,sizeof(l),"[LAYER-TEST] memfd=%d (fd=%d)\n",mfd,fd); c_log(l);
    if(mfd<0){ snprintf(l,sizeof(l),"[LAYER-TEST] memfd FAIL errno=%d\n",errno); c_log(l); return NULL; }
    if(ftruncate(mfd,sz)<0){ snprintf(l,sizeof(l),"[LAYER-TEST] ftruncate FAIL errno=%d\n",errno); c_log(l); close(mfd); return NULL; }
    void *addr=mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_SHARED, mfd, 0);
    snprintf(l,sizeof(l),"[LAYER-TEST] mmap=%p\n",addr); c_log(l);
    if(addr==MAP_FAILED||addr==NULL){ snprintf(l,sizeof(l),"[LAYER-TEST] mmap FAIL errno=%d\n",errno); c_log(l); close(mfd); return NULL; }
    uint32_t *pix=(uint32_t*)addr;
    for(int i=0;i<W*H;i++) pix[i]=color;
    uint8_t b[8]; wr32(b,pool_id); wr32(b+4,sz);
    if(send_msg_fd(fd,shm_id,0,b,8,mfd)!=0){ printf("create_pool fail\n"); return NULL; }
    *sz_out=sz;
    return addr;
}
int main(void){
    setvbuf(stdout, NULL, _IONBF, 0); // serial: sem buffer (forense)
    c_log("[LAYER-TEST] start\n");
    c_log("[LAYER-TEST] go\n");
    const char *xdg=getenv("XDG_RUNTIME_DIR");
    char path[128];
    if(xdg&&xdg[0]) snprintf(path,sizeof(path),"%s/wayland-0",xdg);
    else strcpy(path,"/tmp/wayland-0");
    int fd=socket(AF_UNIX,SOCK_STREAM,0);
    if(fd<0){perror("socket");return 1;}
    struct sockaddr_un sa; memset(&sa,0,sizeof(sa)); sa.sun_family=AF_UNIX;
    strncpy(sa.sun_path,path,sizeof(sa.sun_path)-1);
    int tries=0; while(connect(fd,(struct sockaddr*)&sa,sizeof(sa))<0){
        if(tries++>=100){perror("connect");return 1;} usleep(100000);
    }
    c_log("[LAYER-TEST] connected\n");
    uint8_t pl[4]; wr32(pl,2); send_msg(fd,1,1,pl,4); // get_registry -> 2
    wr32(pl,3); send_msg(fd,1,0,pl,4); // sync -> 3
    uint8_t buf[8192]; int blen=0;
    int n_comp=0,n_shm=0,n_layer=0,n_output=0,v_layer=0,cb_done=0,got_error=0;
    for(int iter=0; iter<80 && !(cb_done&&n_comp&&n_shm&&n_layer&&n_output); iter++){
        struct pollfd pf={fd,POLLIN,0};
        if(poll(&pf,1,100)>0 && (pf.revents&POLLIN)){
            ssize_t n=read(fd, buf+blen, sizeof(buf)-blen);
            if(n<=0) break;
            blen+=n;
            int off=0;
            while(off+8<=blen){
                uint32_t sender=rd32(buf+off), szop=rd32(buf+off+4);
                uint32_t size=szop>>16, op=szop&0xFFFF;
                if(size<8||off+(int)size>blen) break;
                uint8_t *pay=buf+off+8; uint32_t plen=size-8;
                if(sender==2 && op==0 && plen>=12){
                    uint32_t name=rd32(pay), ilen=rd32(pay+4);
                    if(ilen<64 && 8+ilen<=plen){
                        char iface[64]; memcpy(iface,pay+8,ilen); iface[ilen-1]=0;
                        uint32_t pad=(4-(ilen%4))%4;
                        uint32_t ver=rd32(pay+8+ilen+pad);
                        printf("[LAYER-TEST] global %u %s v%u\n",name,iface,ver);
                        if(!strcmp(iface,"wl_compositor")) n_comp=name;
                        else if(!strcmp(iface,"wl_shm")) n_shm=name;
                        else if(!strcmp(iface,"wl_output")) n_output=name;
                        else if(!strcmp(iface,"zwlr_layer_shell_v1")){ n_layer=name; v_layer=ver; }
                    }
                } else if(sender==1 && op==0){ printf("[LAYER-TEST] display error\n"); got_error=1; }
                else if(sender==3 && op==0){ cb_done=1; }
                off+=size;
            }
            if(off>0){ memmove(buf,buf+off,blen-off); blen-=off; }
        }
        if(got_error) break;
    }
    if(got_error) return 1;
    if(!n_comp||!n_shm||!n_layer||!n_output){ printf("[LAYER-TEST] missing globals c=%d shm=%d layer=%d out=%d\n",n_comp,n_shm,n_layer,n_output); return 1; }
    printf("[LAYER-TEST] registry OK layer v%u\n",v_layer);
    uint32_t lv = v_layer>4?4:v_layer;
    do_bind(fd,2,n_comp,"wl_compositor",4,4);
    do_bind(fd,2,n_shm,"wl_shm",1,5);
    do_bind(fd,2,n_output,"wl_output",2,6);
    do_bind(fd,2,n_layer,"zwlr_layer_shell_v1",lv,7);
    /* --- background: surface 8 + layer 9 --- */
    { uint8_t b[4]; wr32(b,8); send_msg(fd,4,0,b,4); } // create_surface
    {
        const char *ns="wallpaper"; uint32_t nl=strlen(ns)+1, pad=(4-(nl%4))%4;
        uint8_t b[64]; int o=0;
        wr32(b+o,9);o+=4; wr32(b+o,8);o+=4; wr32(b+o,6);o+=4; wr32(b+o,0);o+=4; // layer=BACKGROUND
        wr32(b+o,nl);o+=4; memcpy(b+o,ns,nl);o+=nl; memset(b+o,0,pad);o+=pad;
        send_msg(fd,7,0,b,o); printf("[LAYER-TEST] get_layer_surface bg\n");
    }
    { uint8_t b[8]; wr32(b,0); wr32(b+4,0); send_msg(fd,9,0,b,8); } // set_size 0x0
    { uint8_t b[4]; wr32(b,15); send_msg(fd,9,1,b,4); } // set_anchor all
    int W=1280,H=800,sz=0; // fb do QEMU (ver logs do compositor)
    void *bg=make_pool(fd,5,10,W,H,0xFF1E1E1Eu,&sz);
    { char l[96]; snprintf(l,sizeof(l),"[LAYER-TEST] bg pool=%p sz=%d\n",bg,sz); c_log(l); }
    if(!bg) return 1;
    { uint8_t b[24]; wr32(b,11); wr32(b+4,0); wr32(b+8,W); wr32(b+12,H); wr32(b+16,W*4); wr32(b+20,1); send_msg(fd,10,0,b,24); } // create_buffer XRGB
    { uint8_t b[12]; wr32(b,11); wr32(b+4,0); wr32(b+8,0); send_msg(fd,8,1,b,12); } // attach
    send_msg(fd,8,6,NULL,0); // commit
    printf("[LAYER-TEST] bg committed %dx%d\n",W,H);
    /* --- top bar: surface 12 + layer 13 --- */
    { uint8_t b[4]; wr32(b,12); send_msg(fd,4,0,b,4); }
    {
        const char *ns="bar"; uint32_t nl=strlen(ns)+1, pad=(4-(nl%4))%4;
        uint8_t b[64]; int o=0;
        wr32(b+o,13);o+=4; wr32(b+o,12);o+=4; wr32(b+o,6);o+=4; wr32(b+o,2);o+=4; // layer=TOP
        wr32(b+o,nl);o+=4; memcpy(b+o,ns,nl);o+=nl; memset(b+o,0,pad);o+=pad;
        send_msg(fd,7,0,b,o); printf("[LAYER-TEST] get_layer_surface bar\n");
    }
    { uint8_t b[8]; wr32(b,0); wr32(b+4,24); send_msg(fd,13,0,b,8); } // set_size 0x24
    { uint8_t b[4]; wr32(b,13); send_msg(fd,13,1,b,4); } // anchor TOP|LEFT|RIGHT
    { uint8_t b[4]; wr32(b,24); send_msg(fd,13,2,b,4); } // exclusive_zone 24
    int sz2=0;
    void *bar=make_pool(fd,5,14,W,24,0xFF2F2F2Fu,&sz2);
    { char l[96]; snprintf(l,sizeof(l),"[LAYER-TEST] bar pool=%p sz=%d\n",bar,sz2); c_log(l); }
    if(!bar) return 1;
    { uint8_t b[24]; wr32(b,15); wr32(b+4,0); wr32(b+8,W); wr32(b+12,24); wr32(b+16,W*4); wr32(b+20,1); send_msg(fd,14,0,b,24); }
    { uint8_t b[12]; wr32(b,15); wr32(b+4,0); wr32(b+8,0); send_msg(fd,12,1,b,12); }
    send_msg(fd,12,6,NULL,0);
    printf("[LAYER-TEST] bar committed %dx24\n",W);
    /* --- loop: espera configures, responde ack, conta --- */
    int cfg_bg=0,cfg_bar=0,errs=0;
    for(int iter=0; iter<100; iter++){
        struct pollfd pf={fd,POLLIN,0};
        if(poll(&pf,1,100)>0 && (pf.revents&POLLIN)){
            ssize_t n=read(fd, buf+blen, sizeof(buf)-blen);
            if(n<=0) break;
            blen+=n;
            int off=0;
            while(off+8<=blen){
                uint32_t sender=rd32(buf+off), szop=rd32(buf+off+4);
                uint32_t size=szop>>16, op=szop&0xFFFF;
                if(size<8||off+(int)size>blen) break;
                uint8_t *pay=buf+off+8; uint32_t plen=size-8;
                if(sender==1 && op==0){ printf("[LAYER-TEST] display error!\n"); errs++; }
                else if((sender==9||sender==13) && op==0 && plen>=12){
                    uint32_t serial=rd32(pay), w=rd32(pay+4), h=rd32(pay+8);
                    printf("[LAYER-TEST] configure layer=%d serial=%u %ux%u\n",sender==9?0:2,serial,w,h);
                    uint8_t b[4]; wr32(b,serial); send_msg(fd,sender,6,b,4); // ack_configure
                    send_msg(fd,sender==9?8:12,6,NULL,0); // commit
                    if(sender==9) cfg_bg++; else cfg_bar++;
                } else if((sender==9||sender==13) && op==1){ printf("[LAYER-TEST] layer closed!\n"); errs++; }
                off+=size;
            }
            if(off>0){ memmove(buf,buf+off,blen-off); blen-=off; }
        }
        if(cfg_bg&&cfg_bar&&iter>30) break;
    }
    printf("[LAYER-TEST] cfg_bg=%d cfg_bar=%d errs=%d\n",cfg_bg,cfg_bar,errs);
    sleep(3); // deixa o compositor expor na tela p/ screenshot
    if(cfg_bg&&cfg_bar&&!errs) printf("[LAYER-TEST] PASS\n");
    else printf("[LAYER-TEST] FAIL\n");
    close(fd);
    return (cfg_bg&&cfg_bar&&!errs)?0:1;
}
