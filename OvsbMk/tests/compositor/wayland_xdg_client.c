#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#ifndef SYS_memfd_create
#define SYS_memfd_create 319
#endif
#define MFD_CLOEXEC 0x0001
static int my_memfd_create(const char *n, unsigned f){ return syscall(SYS_memfd_create,n,f); }
static void c_log(const char *s){ write(1,s,strlen(s)); }

static struct wl_compositor *g_comp=NULL;
static struct wl_shm *g_shm=NULL;
static struct xdg_wm_base *g_wm=NULL;
static struct wl_surface *g_surface=NULL;
static struct xdg_surface *g_xdg_surface=NULL;
static struct xdg_toplevel *g_toplevel=NULL;
static struct wl_shm_pool *g_pool=NULL;
static struct wl_buffer *g_buffer=NULL;
static int g_configured=0;
static int g_got_input=0;

static void xdg_surface_handle_configure(void *data, struct xdg_surface *xs, uint32_t serial){
    char tmp[128]; snprintf(tmp,sizeof(tmp),"[XDG-CLIENT] xdg_surface configure serial=%u\n",serial); c_log(tmp); printf("[XDG-CLIENT] xdg_surface configure serial=%u\n",serial);
    xdg_surface_ack_configure(xs, serial);
    g_configured=1;
}
static const struct xdg_surface_listener xdg_surface_listener={
    .configure=xdg_surface_handle_configure,
};
static void toplevel_handle_configure(void *data, struct xdg_toplevel *tl, int32_t w,int32_t h, struct wl_array *states){
    char tmp[128]; snprintf(tmp,sizeof(tmp),"[XDG-CLIENT] toplevel configure %dx%d\n",w,h); c_log(tmp);
}
static void toplevel_handle_close(void *data, struct xdg_toplevel *tl){ c_log("[XDG-CLIENT] toplevel close\n"); }
static void toplevel_handle_configure_bounds(void *data, struct xdg_toplevel *tl, int32_t w,int32_t h){ }
static void toplevel_handle_wm_capabilities(void *data, struct xdg_toplevel *tl, struct wl_array *caps){ }
static const struct xdg_toplevel_listener toplevel_listener={
    .configure=toplevel_handle_configure,
    .close=toplevel_handle_close,
    .configure_bounds=toplevel_handle_configure_bounds,
    .wm_capabilities=toplevel_handle_wm_capabilities,
};
static void wm_handle_ping(void *data, struct xdg_wm_base *wm, uint32_t serial){
    char tmp[64]; snprintf(tmp,sizeof(tmp),"[XDG-CLIENT] ping %u pong\n",serial); c_log(tmp);
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_listener={ .ping=wm_handle_ping };

static void seat_handle_capabilities(void *d, struct wl_seat *s, uint32_t caps){
    char tmp[64]; snprintf(tmp,sizeof(tmp),"[XDG-CLIENT] seat caps %u\n",caps); c_log(tmp);
}
static void seat_handle_name(void *d, struct wl_seat *s, const char *name){ }
static const struct wl_seat_listener seat_listener={ .capabilities=seat_handle_capabilities, .name=seat_handle_name };

static void pointer_handle_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy){ c_log("[XDG-CLIENT] pointer enter\n"); g_got_input=1; }
static void pointer_handle_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *surf){ c_log("[XDG-CLIENT] pointer leave\n"); }
static void pointer_handle_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t sx, wl_fixed_t sy){
    char tmp[128]; snprintf(tmp,sizeof(tmp),"[XDG-CLIENT] pointer motion t=%u x=%.2f y=%.2f\n",time,wl_fixed_to_double(sx),wl_fixed_to_double(sy)); c_log(tmp); printf("[XDG-CLIENT] pointer motion t=%u x=%.2f y=%.2f\n",time,wl_fixed_to_double(sx),wl_fixed_to_double(sy)); g_got_input=1;
}
static void pointer_handle_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t btn, uint32_t st){
    char tmp[128]; snprintf(tmp,sizeof(tmp),"[XDG-CLIENT] pointer button s=%u t=%u btn=%u st=%u\n",serial,time,btn,st); c_log(tmp); g_got_input=1;
}
static void pointer_handle_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t v){}
static void pointer_handle_frame(void *d, struct wl_pointer *p){}
static void pointer_handle_axis_source(void *d, struct wl_pointer *p, uint32_t s){}
static void pointer_handle_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a){}
static void pointer_handle_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t d2){}
static void pointer_handle_axis_value120(void *d, struct wl_pointer *p, uint32_t a, int32_t v){}
static void pointer_handle_axis_relative_direction(void *d, struct wl_pointer *p, uint32_t a, uint32_t dir){}
static const struct wl_pointer_listener pointer_listener={
    .enter=pointer_handle_enter,.leave=pointer_handle_leave,.motion=pointer_handle_motion,.button=pointer_handle_button,
    .axis=pointer_handle_axis,.frame=pointer_handle_frame,.axis_source=pointer_handle_axis_source,.axis_stop=pointer_handle_axis_stop,
    .axis_discrete=pointer_handle_axis_discrete,.axis_value120=pointer_handle_axis_value120,.axis_relative_direction=pointer_handle_axis_relative_direction,
};
static void keyboard_handle_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int fd, uint32_t sz){}
static void keyboard_handle_enter(void *d, struct wl_keyboard *k, uint32_t ser, struct wl_surface *surf, struct wl_array *keys){}
static void keyboard_handle_leave(void *d, struct wl_keyboard *k, uint32_t ser, struct wl_surface *surf){}
static void keyboard_handle_key(void *d, struct wl_keyboard *k, uint32_t ser, uint32_t time, uint32_t key, uint32_t st){
    char tmp[128]; snprintf(tmp,sizeof(tmp),"[XDG-CLIENT] keyboard key s=%u t=%u key=%u st=%u\n",ser,time,key,st); c_log(tmp); g_got_input=1;
}
static void keyboard_handle_modifiers(void *d, struct wl_keyboard *k, uint32_t ser, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp){}
static void keyboard_handle_repeat_info(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay){}
static const struct wl_keyboard_listener keyboard_listener={
    .keymap=keyboard_handle_keymap,.enter=keyboard_handle_enter,.leave=keyboard_handle_leave,.key=keyboard_handle_key,.modifiers=keyboard_handle_modifiers,.repeat_info=keyboard_handle_repeat_info,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver){
    char tmp[160]; snprintf(tmp,sizeof(tmp),"[XDG-CLIENT] global %u %s v%u\n",name,iface,ver); c_log(tmp); printf("[XDG-CLIENT] global %u %s v%u\n",name,iface,ver);
    if(strcmp(iface,"wl_compositor")==0) g_comp=wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if(strcmp(iface,"wl_shm")==0) g_shm=wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if(strcmp(iface,"wl_seat")==0){
        struct wl_seat *seat=wl_registry_bind(reg, name, &wl_seat_interface, 7);
        wl_seat_add_listener(seat, &seat_listener, NULL);
        // get pointer/keyboard
        struct wl_pointer *p=wl_seat_get_pointer(seat);
        wl_pointer_add_listener(p, &pointer_listener, NULL);
        struct wl_keyboard *k=wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(k, &keyboard_listener, NULL);
    }
    else if(strcmp(iface,"xdg_wm_base")==0){
        g_wm=wl_registry_bind(reg, name, &xdg_wm_base_interface, 3);
        xdg_wm_base_add_listener(g_wm, &wm_listener, NULL);
    }
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n){}
static const struct wl_registry_listener reg_listener={ .global=registry_global, .global_remove=registry_global_remove };

int main(void){
    c_log("[XDG-CLIENT] start\n"); printf("[XDG-CLIENT] start\n");
    struct wl_display *d=NULL;
    for(int tries=0; tries<50; tries++){
        d=wl_display_connect(NULL);
        if(d) break;
        d=wl_display_connect("wayland-0");
        if(d) break;
        const char *xd=getenv("XDG_RUNTIME_DIR");
        char path[128];
        if(xd&&xd[0]) snprintf(path,sizeof(path),"%s/wayland-0",xd); else strcpy(path,"/tmp/wayland-0");
        d=wl_display_connect(path);
        if(d) break;
        usleep(100000);
        if(tries==0){ char tmp[160]; snprintf(tmp,sizeof(tmp),"[XDG-CLIENT] connect retry %s\n",path); c_log(tmp); }
    }
    if(!d){ perror("wl_display_connect"); c_log("[XDG-CLIENT] connect FAIL\n"); return 1; }
    c_log("[XDG-CLIENT] connected\n");
    struct wl_registry *reg=wl_display_get_registry(d);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(d);
    wl_display_roundtrip(d);
    if(!g_comp||!g_shm||!g_wm){ c_log("[XDG-CLIENT] missing globals\n"); printf("[XDG-CLIENT] missing globals comp=%p shm=%p wm=%p\n",g_comp,g_shm,g_wm); return 1; }
    c_log("[XDG-CLIENT] registry OK\n");
    g_surface=wl_compositor_create_surface(g_comp);
    g_xdg_surface=xdg_wm_base_get_xdg_surface(g_wm, g_surface);
    xdg_surface_add_listener(g_xdg_surface, &xdg_surface_listener, NULL);
    g_toplevel=xdg_surface_get_toplevel(g_xdg_surface);
    xdg_toplevel_add_listener(g_toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(g_toplevel, "test");
    wl_surface_commit(g_surface);
    // wait for configure
    c_log("[XDG-CLIENT] waiting configure\n");
    int it=0;
    while(!g_configured && it<100){
        wl_display_dispatch(d);
        usleep(10000);
        it++;
        if(it%20==0) wl_display_roundtrip(d);
    }
    if(!g_configured){ c_log("[XDG-CLIENT] no configure FAIL\n"); return 1; }
    c_log("[XDG-CLIENT] configured, creating shm buffer\n");
    int W=200,H=100;
    int stride=W*4;
    int sz=stride*H;
    int mfd=my_memfd_create("xdg_shm", MFD_CLOEXEC);
    if(mfd<0){ perror("memfd_create"); return 1; }
    if(ftruncate(mfd,sz)<0){ perror("ftruncate"); return 1; }
    void *addr=mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_SHARED, mfd, 0);
    if(addr==MAP_FAILED){ perror("mmap"); return 1; }
    char tmp[128]; snprintf(tmp,sizeof(tmp),"[XDG-CLIENT] pool mmap %p size %d\n",addr,sz); c_log(tmp);
    uint32_t *pix=(uint32_t*)addr;
    for(int y=0;y<H;y++) for(int x=0;x<W;x++){
        uint32_t col;
        if(x<4||x>=W-4||y<4||y>=H-4) col=0xFFFF0000u;
        else if((x/20+y/20)%2==0) col=0xFF00FF00u;
        else col=0xFF0000FFu;
        pix[y*W+x]=col;
    }
    c_log("[XDG-CLIENT] drew rectangle\n");
    g_pool=wl_shm_create_pool(g_shm, mfd, sz);
    g_buffer=wl_shm_pool_create_buffer(g_pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(g_pool);
    wl_surface_attach(g_surface, g_buffer, 0, 0);
    wl_surface_commit(g_surface);
    wl_display_flush(d);
    c_log("[XDG-CLIENT] attach+commit\n");
    // poll input for 10s
    c_log("[XDG-CLIENT] polling input 10s\n");
    for(int i=0;i<100;i++){
        wl_display_dispatch_pending(d);
        wl_display_flush(d);
        struct pollfd pf={wl_display_get_fd(d), POLLIN, 0};
        poll(&pf,1,100);
        if(pf.revents&POLLIN) wl_display_dispatch(d);
        usleep(10000);
    }
    char tmp2[128]; snprintf(tmp2,sizeof(tmp2),"[XDG-CLIENT] input seen=%d done\n",g_got_input); c_log(tmp2); printf("[XDG-CLIENT] input seen=%d\n",g_got_input);
    c_log("[XDG-CLIENT] PASS\n");
    sleep(1);
    wl_buffer_destroy(g_buffer);
    munmap(addr,sz);
    close(mfd);
    xdg_toplevel_destroy(g_toplevel);
    xdg_surface_destroy(g_xdg_surface);
    wl_surface_destroy(g_surface);
    wl_display_disconnect(d);
    return 0;
}
