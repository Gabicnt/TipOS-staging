#define _GNU_SOURCE /* memfd_create */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "xdg-shell-server-protocol.h"

static void wlog(const char *s){ write(1,s,strlen(s)); }

// fb
static void *fb_addr=NULL;
static int fb_w=0, fb_h=0, fb_pitch=0, fb_fd=-1;
static int fb_init(void){
    fb_fd=open("/dev/fb0", O_RDWR);
    if(fb_fd<0){ wlog("[WL-LIB] open /dev/fb0 failed\n"); return -1; }
    struct fb_var_screeninfo vinfo; struct fb_fix_screeninfo finfo;
    memset(&vinfo,0,sizeof(vinfo)); memset(&finfo,0,sizeof(finfo));
    if(ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo)==0){
        fb_w=vinfo.xres; fb_h=vinfo.yres;
        char tmp[128]; snprintf(tmp,sizeof(tmp),"[WL-LIB] fb %dx%d bpp=%d\n",fb_w,fb_h,vinfo.bits_per_pixel); wlog(tmp);
    }
    if(ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo)==0){
        fb_pitch=finfo.line_length ? finfo.line_length/4 : fb_w;
        char tmp[128]; snprintf(tmp,sizeof(tmp),"[WL-LIB] fb pitch=%d smem=%u\n",fb_pitch,finfo.smem_len); wlog(tmp);
    }
    size_t sz=finfo.smem_len ? finfo.smem_len : (size_t)fb_w*fb_h*4;
    if(!sz) sz=1024*768*4;
    fb_addr=mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if(fb_addr==MAP_FAILED){ wlog("[WL-LIB] mmap fb failed\n"); return -1; }
    wlog("[WL-LIB] fb mmap ok\n");
    uint32_t *p=(uint32_t*)fb_addr;
    for(int y=0;y<fb_h;y++) for(int x=0;x<fb_w;x++) p[y*fb_pitch+x]=0xFF202020u;
    wlog("[WL-LIB] fb cleared 0xFF202020\n");
    return 0;
}
// ---- splash "Ovsb.K carregando" (feedback no boot lento; TCG!) ----
// Fonte 8x8 própria (sem freetype no compositor), escala 2x.
// Cobre só os glifos do texto + pontos.
static const uint8_t splash_glyph(char c, int row){
    switch(c){
    case 'O': { static const uint8_t g[8]={0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}; return g[row]; }
    case 'v': { static const uint8_t g[8]={0x00,0xC6,0xC6,0xC6,0xC6,0x7C,0x38,0x00}; return g[row]; }
    case 's': { static const uint8_t g[8]={0x00,0x7C,0xC0,0x7C,0x06,0xC6,0x7C,0x00}; return g[row]; }
    case 'b': { static const uint8_t g[8]={0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xFC,0x00}; return g[row]; }
    case 'K': { static const uint8_t g[8]={0xC6,0xCC,0xE0,0xF0,0xE0,0xCC,0xC6,0x00}; return g[row]; }
    case 'c': { static const uint8_t g[8]={0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0x7C,0x00}; return g[row]; }
    case 'a': { static const uint8_t g[8]={0x00,0x7C,0x06,0x7F,0xC6,0xC6,0x7F,0x00}; return g[row]; }
    case 'r': { static const uint8_t g[8]={0x00,0xDC,0xFC,0xC0,0xC0,0xC0,0xC0,0x00}; return g[row]; }
    case 'e': { static const uint8_t g[8]={0x00,0x7C,0xC6,0xFE,0xC0,0xC6,0x7C,0x00}; return g[row]; }
    case 'g': { static const uint8_t g[8]={0x00,0x7E,0xC6,0xC6,0x7E,0x06,0x7C,0x00}; return g[row]; }
    case 'n': { static const uint8_t g[8]={0x00,0xDC,0xFC,0xC6,0xC6,0xC6,0xC6,0x00}; return g[row]; }
    case 'd': { static const uint8_t g[8]={0x06,0x06,0x7E,0xC6,0xC6,0xC6,0x7E,0x00}; return g[row]; }
    case 'o': { static const uint8_t g[8]={0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}; return g[row]; }
    case '.': { static const uint8_t g[8]={0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18}; return g[row]; }
    default: return 0x00;
    }
}
static int splash_on=1, splash_tick=0;
static struct wl_event_source *splash_timer=NULL;
static void splash_draw(int ndots){
    if(!fb_addr) return;
    const char *t1="Ovsb.K carregando";
    uint32_t *dst=(uint32_t*)fb_addr;
    int x0=(fb_w-17*16)/2, y0=fb_h/2-16;
    if(x0<0) x0=0; if(y0<0) y0=0;
    for(int i=0;i<17 && t1[i];i++){
        for(int r=0;r<8;r++){
            uint8_t bits=splash_glyph(t1[i],r);
            for(int c=0;c<8;c++){
                if(!(bits & (0x80>>c))) continue;
                for(int dy=0;dy<2;dy++) for(int dx=0;dx<2;dx++){
                    int x=x0+i*16+c*2+dx, y=y0+r*2+dy;
                    if(x>=0&&x<fb_w&&y>=0&&y<fb_h) dst[y*fb_pitch+x]=0xFFFFFFFFu;
                }
            }
        }
    }
    for(int i=0;i<ndots && i<4;i++){
        for(int r=0;r<8;r++){
            uint8_t bits=splash_glyph('.',r);
            for(int c=0;c<8;c++){
                if(!(bits & (0x80>>c))) continue;
                for(int dy=0;dy<2;dy++) for(int dx=0;dx<2;dx++){
                    int x=x0+i*16+c*2+dx, y=y0+24+r*2+dy;
                    if(x>=0&&x<fb_w&&y>=0&&y<fb_h) dst[y*fb_pitch+x]=0xFFFFFFFFu;
                }
            }
        }
    }
}
static int splash_timer_cb(void *data){
    (void)data;
    if(!splash_on) return 0;
    splash_tick++;
    // limpa a faixa do splash e redesenha com ndots atual
    if(fb_addr){
        uint32_t *dst=(uint32_t*)fb_addr;
        int x0=(fb_w-17*16)/2, y0=fb_h/2-16;
        if(x0<0) x0=0; if(y0<0) y0=0;
        for(int y=y0;y<y0+48 && y<fb_h;y++)
            for(int x=x0;x<x0+17*16 && x<fb_w;x++) dst[y*fb_pitch+x]=0xFF202020u;
    }
    splash_draw((splash_tick%4)+1);
    if(splash_tick<=5){ char b[64]; snprintf(b,sizeof(b),"[WL-LIB] splash tick %d\n",splash_tick); wlog(b); }
    if(splash_timer) wl_event_source_timer_update(splash_timer, 500);
    return 0;
}
static int cursor_x=120, cursor_y=80; // mesma origem do pointer enter
static const char *cursor_bits[15]={
    "X..........",
    "XX.........",
    "XXX........",
    "XXXX.......",
    "XXXXX......",
    "XXXXXX.....",
    "XXXXXXX....",
    "XXXXXXXX...",
    "XXXXXXXXX..",
    "XXXXXXXXXX.",
    "XXXXXX.....",
    "XXXX.XX....",
    "XXX...XX...",
    "XX.....XX..",
    "X.......XX.",
};
static void draw_cursor(void){
    if(!fb_addr) return;
    uint32_t *dst=(uint32_t*)fb_addr;
    for(int r=0;r<15;r++){
        for(int c=0;c<11;c++){
            if(cursor_bits[r][c]!='X') continue;
            int x0=cursor_x+c, y0=cursor_y+r;
            // sombra 1px pra baixo-direita, depois corpo
            int sx=x0+1, sy=y0+1;
            if(sx>=0&&sx<fb_w&&sy>=0&&sy<fb_h) dst[sy*fb_pitch+sx]=0xFF000000u;
            if(x0>=0&&x0<fb_w&&y0>=0&&y0<fb_h) dst[y0*fb_pitch+x0]=0xFFFFFFFFu;
        }
    }
}
static void redraw_all(void); // definida após struct surf
static void fb_blit_shm(struct wl_shm_buffer *shmb, int dst_x, int dst_y){
    if(!fb_addr || !shmb) return;
    int32_t w = wl_shm_buffer_get_width(shmb);
    int32_t h = wl_shm_buffer_get_height(shmb);
    int32_t stride = wl_shm_buffer_get_stride(shmb);
    uint32_t fmt = wl_shm_buffer_get_format(shmb);
    (void)fmt;
    void *data = wl_shm_buffer_get_data(shmb);
    if(!data) return;
    // assume ARGB8888/XRGB8888
    int s_stride = stride/4;
    uint32_t *src=(uint32_t*)data;
    uint32_t *dst=(uint32_t*)fb_addr;
    wl_shm_buffer_begin_access(shmb);
    for(int y=0;y<h;y++){
        int fy=dst_y+y;
        if(fy<0||fy>=fb_h) continue;
        for(int x=0;x<w;x++){
            int fx=dst_x+x;
            if(fx<0||fx>=fb_w) continue;
            dst[fy*fb_pitch+fx]=src[y*s_stride+x];
        }
    }
    wl_shm_buffer_end_access(shmb);
    char tmp[128]; snprintf(tmp,sizeof(tmp),"[WL-LIB] blit %dx%d at %d,%d\n",w,h,dst_x,dst_y); wlog(tmp);
    int cx=dst_x+w/2, cy=dst_y+h/2;
    if(cx>=0&&cx<fb_w&&cy>=0&&cy<fb_h){
        uint32_t pix=dst[cy*fb_pitch+cx];
        snprintf(tmp,sizeof(tmp),"[WL-LIB] fb pixel %d,%d = 0x%08x\n",cx,cy,pix); wlog(tmp);
    }
    draw_cursor(); // cursor server-side sempre por cima
}

// surface tracking
// FASE7-WM: struct surf == janela (window_t). Ordem da lista `surfs`:
// head->...->tail == fundo->frente (redraw_all desenha nessa ordem;
// a focada é sempre a última = topo).
struct surf {
    struct wl_resource *res;
    struct wl_resource *buffer;
    struct wl_resource *xdg_surface;
    struct wl_resource *toplevel;
    struct wl_client *client; // FASE8.2-fix: dono (purge sem tocar resources)
    int mapped;
    uint32_t serial;
    struct wl_list link;
    struct subsurf *subsurface; // Fase 4d: NULL = janela principal
    int entered; // Fase 4d: pointer enter já enviado?
    int kbd_entered; // FASE5: keyboard enter já enviado?
    int output_entered; // FASE6: wl_surface.enter (output) já enviado?
    /* FASE7-WM: geometria + estado */
    int wx, wy;          // posição no framebuffer
    int ww, wh;          // último tamanho conhecido (do shm buffer)
    char title[128];
    int focused;         // tem o foco de teclado
    int minimized;
    int maximized;
    int sx, sy, sw, sh;  // FASE8.2: geometria salva p/ un-maximize
    /* FASE8.0a: wlr-layer-shell (wallpaper/barra: sem titlebar/foco) */
    int is_layer;        // 1 = layer surface (não xdg)
    int layer;           // 0bg 1bottom 2top 3overlay
    int anchor;          // T1 B2 L4 R8
    int excl_zone;
    int kbd_interactive;
    int margin_t, margin_r, margin_b, margin_l;
    int layer_w, layer_h; // set_size (0 = pelo buffer)
    uint32_t layer_serial, layer_acked;
};
/* FASE5: keymap XKB-US real (gerado no host via libxkbcommon). */
#include "us_keymap_bin.c"
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
static int kbd_keymap_fd = -1;
static int kbd_keymap_send(struct wl_resource *kr) {
    if (kbd_keymap_fd < 0) {
#ifdef MFD_CLOEXEC
        kbd_keymap_fd = memfd_create("xkb-keymap", MFD_CLOEXEC);
#endif
        if (kbd_keymap_fd < 0) {
            /* fallback: arquivo normal (o keymap precisa de fd com seek) */
            kbd_keymap_fd = open("/tmp/kbd-keymap", O_RDWR | O_CREAT | O_TRUNC, 0600);
        }
        if (kbd_keymap_fd < 0) return -1;
        const char *p = kbd_keymap_str;
        unsigned left = kbd_keymap_len;
        while (left) {
            ssize_t w = write(kbd_keymap_fd, p, left);
            if (w <= 0) return -1;
            p += w;
            left -= (unsigned)w;
        }
    }
    wl_keyboard_send_keymap(kr, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                            kbd_keymap_fd, kbd_keymap_len);
    return 0;
}
// Fase 4d: subsuperficie (foot exige wl_subcompositor; fatal sem)
struct subsurf {
    struct wl_resource *res;
    struct surf *parent;
    struct wl_client *client; // FASE8.2-fix: dono (purge)
    int32_t x, y;
    struct wl_list link;
};
static struct wl_list subsurfs;
static struct wl_list surfs;
static struct wl_display *g_display;
static struct wl_global *seat_global;
static struct wl_list seats; // not used
static struct wl_resource *seat_pointer_res=NULL;
static struct wl_resource *seat_keyboard_res=NULL;
static struct wl_resource *output_res=NULL;
static struct wl_client *focused_client=NULL;
static uint32_t g_serial=1;

/* ---- FASE7-WM: foco + z-order ---- */
// seat objects são por-cliente (cada foot tem seu wl_keyboard/
// wl_pointer); a tabela resolve cliente -> resource p/ rotear.
#define WM_MAX_CLIENTS 16
static struct { struct wl_client *client; struct wl_resource *kbd; struct wl_resource *ptr; } wm_seats[WM_MAX_CLIENTS];
static int wm_nseats=0;
static struct surf *focused_surf=NULL;
static void wm_send_enter(struct surf *s);
static void redraw_all(void);
static void wm_layer_place(struct surf *s); // FASE8.0a (def. no bloco layer)
static uint32_t kbd_mods_depressed; // definida no input evdev (uso atual aqui)
static int wm_top_zone, wm_bottom_zone; // FASE8.0a: exclusive zones (def. no bloco layer)
/* FASE7-move: drag Alt+botão-esquerdo (janela segue o cursor) */
static struct surf *drag_surf=NULL;
static int drag_offx=0, drag_offy=0;
/* FASE8.2: titlebar (defines cedo p/ wm_pick usar) */
#define WM_TB_H 24
#define WM_BTN_W 28
/* FASE7-resize: Alt+botão-direito + arrastar (âncora = canto superior-
 * esquerdo; cresce/encolhe pelo delta do cursor; cliente re-renderiza
 * via xdg configure). */
static struct { struct surf *s; int cx0, cy0, w0, h0, sent_w, sent_h; } rs={0};
#define WM_MIN_W 100
#define WM_MIN_H 60
static void wm_resize_begin(struct surf *s){
    rs.s=s; rs.cx0=cursor_x; rs.cy0=cursor_y;
    rs.w0=s->ww>0?s->ww:640; rs.h0=s->wh>0?s->wh:480;
    rs.sent_w=rs.w0; rs.sent_h=rs.h0;
    char lg[96]; snprintf(lg,sizeof(lg),"[WM] resize start surf=%u %dx%d\n",
        wl_resource_get_id(s->res), rs.w0, rs.h0); wlog(lg);
}
static void wm_resize_send(int w, int h){
    if(!rs.s || !rs.s->toplevel || !rs.s->xdg_surface) return;
    if(w<WM_MIN_W) w=WM_MIN_W; if(h<WM_MIN_H) h=WM_MIN_H;
    if(w>fb_w) w=fb_w; if(h>fb_h) h=fb_h;
    if(w==rs.sent_w && h==rs.sent_h) return; // sem spam de configure
    rs.sent_w=w; rs.sent_h=h;
    struct wl_array states; wl_array_init(&states);
    xdg_toplevel_send_configure(rs.s->toplevel, w, h, &states);
    wl_array_release(&states);
    xdg_surface_send_configure(rs.s->xdg_surface, g_serial++);
    char lg[96]; snprintf(lg,sizeof(lg),"[WM] resize configure %dx%d\n",w,h); wlog(lg);
}
static void wm_bind_res(struct wl_client *c, struct wl_resource *r, int is_kbd){
    for(int i=0;i<wm_nseats;i++){
        if(wm_seats[i].client==c){
            if(is_kbd) wm_seats[i].kbd=r; else wm_seats[i].ptr=r;
            return;
        }
    }
    if(wm_nseats<WM_MAX_CLIENTS){
        wm_seats[wm_nseats].client=c;
        wm_seats[wm_nseats].kbd=is_kbd?r:NULL;
        wm_seats[wm_nseats].ptr=is_kbd?NULL:r;
        wm_nseats++;
    }
}
static struct wl_resource *wm_kbd_res(struct wl_client *c){
    for(int i=0;i<wm_nseats;i++) if(wm_seats[i].client==c) return wm_seats[i].kbd;
    return NULL;
}
static struct wl_resource *wm_ptr_res(struct wl_client *c){
    for(int i=0;i<wm_nseats;i++) if(wm_seats[i].client==c) return wm_seats[i].ptr;
    return NULL;
}
// topmost (última da lista) contendo x,y; NULL se nenhuma
static struct surf *wm_pick(int x, int y){
    struct surf *s, *found=NULL;
    wl_list_for_each(s, &surfs, link){
        if(s->subsurface || s->is_layer || !s->mapped || !s->buffer || s->minimized) continue;
        if(s->ww<=0 || s->wh<=0) continue;
        int tb=s->toplevel?WM_TB_H:0; // titlebar também é clicável
        if(x>=s->wx && y>=s->wy && x<s->wx+s->ww && y<s->wy+tb+s->wh) found=s;
    }
    return found;
}
static void wm_send_leave(struct surf *s){
    if(!s || s->subsurface) return;
    struct wl_client *c=wl_resource_get_client(s->res);
    if(s->kbd_entered){
        struct wl_resource *kr=wm_kbd_res(c);
        if(kr) wl_keyboard_send_leave(kr, g_serial++, s->res);
        s->kbd_entered=0;
    }
    if(s->entered){
        struct wl_resource *pr=wm_ptr_res(c);
        if(pr){ wl_pointer_send_leave(pr, g_serial++, s->res); wl_pointer_send_frame(pr); }
        s->entered=0;
    }
    if(s->output_entered){
        /* wl_surface.leave existe no protocolo; sem ele o foot pode
         * achar que continua visível — envia se o cliente bound >=v1
         * (sempre). Re-enter no refocus rearma output_entered. */
        wl_surface_send_leave(s->res, output_res);
        s->output_entered=0;
    }
    s->focused=0;
}
static void wm_focus(struct surf *s){
    if(!s || s->subsurface) return;
    if(focused_surf==s && s->focused) return;
    if(focused_surf && focused_surf!=s) wm_send_leave(focused_surf);
    /* traz p/ frente: remove e reinsere no tail (topo do z-order) */
    wl_list_remove(&s->link);
    wl_list_insert(surfs.prev, &s->link);
    focused_surf=s;
    s->minimized=0; // FASE8.2: focar restaura minimizada
    focused_client=wl_resource_get_client(s->res);
    wm_send_enter(s);
    redraw_all(); // reempilha (fundo->frente) + cursor por cima
    char tmp[96]; snprintf(tmp,sizeof(tmp),"[WM] focus surf=%u title=%s\n",
        wl_resource_get_id(s->res), s->title[0]?s->title:"?"); wlog(tmp);
}
// Alt+Tab: próxima janela mapeada (ordem da lista, com wrap)
static void wm_focus_next(void){
    if(!focused_surf){ // nenhuma: foca a primeira mapeada
        struct surf *s;
        wl_list_for_each(s, &surfs, link){
            if(!s->subsurface && !s->is_layer && s->mapped && s->buffer){ wm_focus(s); return; }
        }
        return;
    }
    struct surf *s=focused_surf;
    // conta janelas candidatas p/ não girar em falso com 1 só
    int n=0; struct surf *q;
    wl_list_for_each(q, &surfs, link){ if(!q->subsurface && !q->is_layer && q->mapped && q->buffer) n++; }
    if(n<2) return;
    // próxima após a focada (do tail volta pro head)
    struct wl_list *it=s->link.next;
    for(int i=0;i<n;i++){
        if(it==&surfs) it=it->next;
        if(it==&surfs) break;
        q=wl_container_of(it, q, link);
        it=it->next;
        if(!q->subsurface && !q->is_layer && q->mapped && q->buffer && !q->minimized){ wm_focus(q); return; }
    }
}

/* ---- FASE8.2: pointer em layers (top/overlay) ----
 * Hierarquia: overlay > top > xdg > bottom > background.
 * top/overlay recebem enter/motion/button (sem roubar foco de teclado);
 * bottom/background: ignorados. ptr_focus rastreia quem tem o pointer
 * (xdg ou layer) p/ enter/leave corretos no hover. */
static struct surf *ptr_focus=NULL;
static struct surf *wm_pick_layer(int x, int y){
    struct surf *s, *found=NULL;
    wl_list_for_each(s, &surfs, link){
        if(!s->is_layer || !s->mapped || !s->buffer) continue;
        if(s->layer!=2 && s->layer!=3) continue; // só top/overlay
        if(s->ww<=0 || s->wh<=0) continue;
        if(x>=s->wx && y>=s->wy && x<s->wx+s->ww && y<s->wy+s->wh) found=s;
    }
    return found;
}
static struct wl_resource *wm_client_ptr(struct surf *s){
    if(!s) return NULL;
    struct wl_client *c=s->client?s->client:wl_resource_get_client(s->res);
    struct wl_resource *pr=c?wm_ptr_res(c):NULL;
    return pr?pr:seat_pointer_res; // fallback: boot de 1 cliente
}
static void wm_layer_enter(struct surf *s){
    if(!s || !s->is_layer) return;
    struct wl_resource *pr=wm_client_ptr(s);
    if(pr && !s->entered){
        wl_pointer_send_enter(pr, g_serial++, s->res,
            wl_fixed_from_int(cursor_x-s->wx), wl_fixed_from_int(cursor_y-s->wy));
        wl_pointer_send_frame(pr);
        s->entered=1;
        ptr_focus=s;
        wlog("[WM] layer pointer enter\n");
    }
}
/* leave SÓ de pointer (não mexe em kbd/output/foco) */
static void wm_ptr_leave(struct surf *s){
    if(!s || !s->entered) return;
    struct wl_resource *pr=wm_client_ptr(s);
    if(pr){ wl_pointer_send_leave(pr, g_serial++, s->res); wl_pointer_send_frame(pr); }
    s->entered=0;
    if(ptr_focus==s) ptr_focus=NULL;
    wlog("[WM] pointer leave\n");
}

/* ---- FASE8.2-fix: purge-on-disconnect ----
 * Sem isso, cliente morto deixa surfs/seats pendurados e o redraw
 * ou o roteamento acessam wl_resource liberada (UAF). O notify NÃO
 * toca em resources (podem já estar liberadas) — só nos ponteiros
 * `client` guardados nas structs. */
static struct wl_client *seat_pointer_client=NULL;
static struct wl_client *seat_keyboard_client=NULL;
static struct wl_client *output_client=NULL;
static void wm_client_gone(struct wl_listener *listener, void *data){
    struct wl_client *c=(struct wl_client*)data;
    (void)listener;
    char tmp[80]; snprintf(tmp,sizeof(tmp),"[WM] client gone, purging\n"); wlog(tmp);
    struct surf *s, *sn;
    wl_list_for_each_safe(s, sn, &surfs, link){
        if(s->client!=c) continue;
        if(focused_surf==s) focused_surf=NULL;
        if(drag_surf==s) drag_surf=NULL;
        if(ptr_focus==s) ptr_focus=NULL; // FASE8.2
        if(rs.s==s) rs.s=NULL;
        wl_list_remove(&s->link);
        free(s);
    }
    struct subsurf *ss, *ssn;
    wl_list_for_each_safe(ss, ssn, &subsurfs, link){
        if(ss->client!=c) continue;
        wl_list_remove(&ss->link);
        free(ss);
    }
    for(int i=0;i<wm_nseats;i++){
        if(wm_seats[i].client==c){ wm_seats[i].client=NULL; wm_seats[i].kbd=NULL; wm_seats[i].ptr=NULL; }
    }
    if(seat_pointer_client==c){ seat_pointer_client=NULL; seat_pointer_res=NULL; }
    if(seat_keyboard_client==c){ seat_keyboard_client=NULL; seat_keyboard_res=NULL; }
    if(output_client==c){ output_client=NULL; output_res=NULL; }
    if(focused_client==c) focused_client=NULL;
    // refoca o topo sobrevivente (se houver)
    struct surf *q;
    wl_list_for_each_reverse(q, &surfs, link){
        if(!q->subsurface && !q->is_layer && q->mapped && q->buffer && !q->minimized){ wm_focus(q); break; }
    }
    redraw_all();
}
#define WM_MAX_WATCH 16
static struct { struct wl_client *client; struct wl_listener listener; } wm_watch[WM_MAX_WATCH];
static int wm_nwatch=0;
static void wm_watch_client(struct wl_client *c){
    if(!c) return;
    for(int i=0;i<wm_nwatch;i++) if(wm_watch[i].client==c) return;
    if(wm_nwatch>=WM_MAX_WATCH) return;
    wm_watch[wm_nwatch].client=c;
    wm_watch[wm_nwatch].listener.notify=wm_client_gone;
    wl_client_add_destroy_listener(c, &wm_watch[wm_nwatch].listener);
    wm_nwatch++;
}

/* ---- FASE8.2: titlebar server-side (decoração no compositor) ----
 * Barra de 24px no topo: título (fonte 8x8 própria) + 3 botões
 * [_][□][X]. Conteúdo blita 24px abaixo. */
static const uint8_t wm_font[96][8]={
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 20 space */
{0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, /* 21 ! */
{0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, /* 22 " */
{0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* 23 # */
{0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, /* 24 $ */
{0xC6,0xCC,0x18,0x30,0x60,0x66,0xC6,0x00}, /* 25 % */
{0x38,0x6C,0x6C,0x38,0x6D,0x66,0x3B,0x00}, /* 26 & */
{0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* 27 ' */
{0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* 28 ( */
{0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* 29 ) */
{0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 2A * */
{0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* 2B + */
{0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* 2C , */
{0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* 2D - */
{0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* 2E . */
{0x06,0x0C,0x0C,0x18,0x30,0x30,0x60,0x00}, /* 2F / */
{0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00}, /* 30 0 */
{0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 31 1 */
{0x7C,0xC6,0x06,0x0C,0x30,0x60,0xFE,0x00}, /* 32 2 */
{0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, /* 33 3 */
{0x0C,0x1C,0x3C,0x6C,0xFE,0x0C,0x0C,0x00}, /* 34 4 */
{0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, /* 35 5 */
{0x3C,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, /* 36 6 */
{0xFE,0x06,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 37 7 */
{0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, /* 38 8 */
{0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, /* 39 9 */
{0x00,0x00,0x18,0x00,0x00,0x18,0x18,0x00}, /* 3A : */
{0x00,0x00,0x18,0x00,0x00,0x18,0x18,0x30}, /* 3B ; */
{0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, /* 3C < */
{0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, /* 3D = */
{0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, /* 3E > */
{0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, /* 3F ? */
{0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x7E,0x00}, /* 40 @ */
{0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, /* 41 A */
{0xFC,0xC6,0xC6,0xFC,0xC6,0xC6,0xFC,0x00}, /* 42 B */
{0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00}, /* 43 C */
{0xF8,0xCC,0xC6,0xC6,0xC6,0xCC,0xF8,0x00}, /* 44 D */
{0xFE,0xC0,0xC0,0xF8,0xC0,0xC0,0xFE,0x00}, /* 45 E */
{0xFE,0xC0,0xC0,0xF8,0xC0,0xC0,0xC0,0x00}, /* 46 F */
{0x7C,0xC6,0xC0,0xDE,0xC6,0xC6,0x7E,0x00}, /* 47 G */
{0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, /* 48 H */
{0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 49 I */
{0x06,0x06,0x06,0x06,0x06,0xC6,0x7C,0x00}, /* 4A J */
{0xC6,0xCC,0xD8,0xF0,0xD8,0xCC,0xC6,0x00}, /* 4B K */
{0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xFE,0x00}, /* 4C L */
{0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, /* 4D M */
{0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* 4E N */
{0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* 4F O */
{0xFC,0xC6,0xC6,0xFC,0xC0,0xC0,0xC0,0x00}, /* 50 P */
{0x7C,0xC6,0xC6,0xC6,0xDA,0xCC,0x76,0x00}, /* 51 Q */
{0xFC,0xC6,0xC6,0xFC,0xD8,0xCC,0xC6,0x00}, /* 52 R */
{0x7C,0xC6,0xC0,0x7C,0x06,0xC6,0x7C,0x00}, /* 53 S */
{0xFE,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* 54 T */
{0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* 55 U */
{0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x38,0x00}, /* 56 V */
{0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, /* 57 W */
{0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, /* 58 X */
{0xC6,0xC6,0xC6,0x7C,0x18,0x18,0x18,0x00}, /* 59 Y */
{0xFE,0x06,0x0C,0x18,0x30,0x60,0xFE,0x00}, /* 5A Z */
{0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* 5B [ */
{0x60,0x30,0x30,0x18,0x0C,0x0C,0x06,0x00}, /* 5C backslash */
{0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* 5D ] */
{0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, /* 5E ^ */
{0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00}, /* 5F _ */
{0x18,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* 60 ` */
{0x00,0x00,0x7C,0x06,0x7E,0xC6,0x7E,0x00}, /* 61 a */
{0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xFC,0x00}, /* 62 b */
{0x00,0x00,0x7C,0xC6,0xC0,0xC0,0x7C,0x00}, /* 63 c */
{0x06,0x06,0x7E,0xC6,0xC6,0xC6,0x7E,0x00}, /* 64 d */
{0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, /* 65 e */
{0x1C,0x36,0x30,0x78,0x30,0x30,0x30,0x00}, /* 66 f */
{0x00,0x00,0x7E,0xC6,0xC6,0x7E,0x06,0x7C,0x00}, /* 67 g */
{0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x00}, /* 68 h */
{0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* 69 i */
{0x06,0x00,0x0E,0x06,0x06,0x06,0x66,0x3C,0x00}, /* 6A j */
{0xC0,0xC0,0xCC,0xD8,0xF0,0xCC,0xC6,0x00}, /* 6B k */
{0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 6C l */
{0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0x00}, /* 6D m */
{0x00,0x00,0xDC,0xFC,0xC6,0xC6,0xC6,0x00}, /* 6E n */
{0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, /* 6F o */
{0x00,0x00,0xFC,0xC6,0xC6,0xFC,0xC0,0xC0,0x00}, /* 70 p */
{0x00,0x00,0x7E,0xC6,0xC6,0x7E,0x06,0x06,0x00}, /* 71 q */
{0x00,0x00,0xDC,0xFC,0xC0,0xC0,0xC0,0x00}, /* 72 r */
{0x00,0x00,0x7E,0xC0,0x7C,0x06,0x7C,0x00}, /* 73 s */
{0x30,0x30,0x7C,0x30,0x30,0x34,0x18,0x00}, /* 74 t */
{0x00,0x00,0xC6,0xC6,0xC6,0xC6,0x7E,0x00}, /* 75 u */
{0x00,0x00,0xC6,0xC6,0xC6,0x7C,0x38,0x00}, /* 76 v */
{0x00,0x00,0xC6,0xD6,0xFE,0xEE,0x44,0x00}, /* 77 w */
{0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, /* 78 x */
{0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0x7C,0x00}, /* 79 y */
{0x00,0x00,0xFE,0x0C,0x18,0x30,0xFE,0x00}, /* 7A z */
{0x0C,0x18,0x18,0x70,0x18,0x18,0x0C,0x00}, /* 7B { */
{0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* 7C | */
{0x30,0x18,0x18,0x0E,0x18,0x18,0x30,0x00}, /* 7D } */
{0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00}, /* 7E ~ */
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 7F DEL=blank */
};
static void wm_px(int x, int y, uint32_t color){
    if(!fb_addr) return;
    if(x<0||x>=fb_w||y<0||y>=fb_h) return;
    ((uint32_t*)fb_addr)[y*fb_pitch+x]=color;
}
static void wm_text(int x, int y, const char *t, uint32_t color, int maxpx){
    if(!fb_addr || !t) return;
    int maxch=maxpx/8;
    for(int i=0;t[i] && i<maxch;i++){
        unsigned char c=(unsigned char)t[i];
        if(c<0x20||c>0x7E) continue;
        const uint8_t *g=wm_font[c-0x20];
        for(int r=0;r<8;r++) for(int col=0;col<8;col++)
            if(g[r]&(0x80u>>col)) wm_px(x+i*8+col, y+r, color);
    }
}
// idx 0=min 1=max 2=close (da direita p/ esquerda)
static void wm_btn_rect(struct surf *s, int idx, int *x0, int *y0, int *x1, int *y1){
    *x1=s->wx+s->ww-(idx*WM_BTN_W); *x0=*x1-WM_BTN_W;
    *y0=s->wy; *y1=s->wy+WM_TB_H;
}
static void wm_draw_titlebar(struct surf *s){
    if(!fb_addr || s->ww<=0) return;
    uint32_t *dst=(uint32_t*)fb_addr;
    uint32_t bg=s->focused?0xFF2F5D8Au:0xFF4A4A4Au;
    for(int y=s->wy;y<s->wy+WM_TB_H;y++){
        if(y<0||y>=fb_h) continue;
        for(int x=s->wx;x<s->wx+s->ww;x++){
            if(x<0||x>=fb_w) continue;
            dst[y*fb_pitch+x]=bg;
        }
    }
    wm_text(s->wx+8, s->wy+8, s->title[0]?s->title:"?", 0xFFFFFFFFu,
            s->ww-3*WM_BTN_W-16);
    for(int i=0;i<3;i++){
        int x0,y0,x1,y1; wm_btn_rect(s,i,&x0,&y0,&x1,&y1);
        uint32_t bbg=(i==2)?0xFFB03030u:0xFF5A5A5Au;
        for(int y=y0+3;y<y1-3;y++) for(int x=x0+3;x<x1-3;x++) wm_px(x,y,bbg);
        /* símbolos vetoriais (sem fonte): _ □ X */
        int ix0=x0+8, ix1=x1-8, iy0=y0+7, iy1=y1-7;
        uint32_t fg=0xFFFFFFFFu;
        if(i==0){ for(int x=ix0;x<ix1;x++) wm_px(x,iy1-2,fg); }
        else if(i==1){
            for(int x=ix0;x<ix1;x++){ wm_px(x,iy0,fg); wm_px(x,iy1-1,fg); }
            for(int y=iy0;y<iy1;y++){ wm_px(ix0,y,fg); wm_px(ix1-1,y,fg); }
        } else {
            int n=ix1-ix0; int m=iy1-iy0; if(m<n) n=m;
            for(int k=0;k<n;k++){ wm_px(ix0+k,iy0+k,fg); wm_px(ix1-1-k,iy0+k,fg); }
        }
    }
}
// blit unificado: titlebar + conteúdo (conteúdo 24px abaixo).
// Minimizada não desenha nada.
static void wm_blit_win(struct surf *s){
    if(!s || s->minimized) return;
    if(s->mapped && !s->subsurface && s->toplevel) wm_draw_titlebar(s);
    if(s->mapped && s->buffer){
        struct wl_shm_buffer *shmb=wl_shm_buffer_get(s->buffer);
        if(shmb){
            int ox=s->subsurface?s->subsurface->x:s->wx;
            int oy=s->subsurface?s->subsurface->y:(s->wy+(s->toplevel?WM_TB_H:0));
            fb_blit_shm(shmb,ox,oy);
        }
    }
}
static void wm_toggle_max(struct surf *s){
    if(!s || !s->toplevel || !s->xdg_surface) return;
    struct wl_array states; wl_array_init(&states);
    if(!s->maximized){
        s->sx=s->wx; s->sy=s->wy;
        s->sw=s->ww>0?s->ww:640; s->sh=s->wh>0?s->wh:480;
        s->maximized=1; s->wx=0; s->wy=0;
        uint32_t *p=wl_array_add(&states,sizeof(*p));
        if(p) *p=XDG_TOPLEVEL_STATE_MAXIMIZED;
        xdg_toplevel_send_configure(s->toplevel, fb_w, fb_h-WM_TB_H-wm_top_zone-wm_bottom_zone, &states);
        wlog("[WM] maximize\n");
    } else {
        s->maximized=0; s->wx=s->sx; s->wy=s->sy;
        xdg_toplevel_send_configure(s->toplevel, s->sw, s->sh, &states);
        wlog("[WM] un-maximize\n");
    }
    wl_array_release(&states);
    xdg_surface_send_configure(s->xdg_surface, g_serial++);
    redraw_all();
}

static void seat_send_caps(struct wl_resource *r){
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER|WL_SEAT_CAPABILITY_KEYBOARD);
    wl_seat_send_name(r, "seat0");
}

// wl_compositor
static void comp_create_surface(struct wl_client *client, struct wl_resource *res, uint32_t id){
    struct surf *s=calloc(1,sizeof(*s));
    s->client=client; // FASE8.2-fix
    s->res = wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(res), id);
    wl_list_insert(&surfs, &s->link);
    // wl_surface handlers
    // we use implementation via dispatcher: simple approach set implementation with callbacks struct
    // Actually we need to set implementation for wl_surface; we use wl_resource_set_implementation with our struct
    // Define inline handlers below via separate functions and wl_surface_interface impl
    // For brevity we use dispatcher: set implementation after creation
    // We'll set via wl_resource_set_implementation with surface_impl
    extern const struct wl_surface_interface surface_impl;
    wl_resource_set_implementation(s->res, &surface_impl, s, NULL);
    char tmp[64]; snprintf(tmp,sizeof(tmp),"[WL-LIB] create_surface %u\n",id); wlog(tmp);
}
static const struct wl_region_interface region_impl;
static void comp_create_region(struct wl_client *client, struct wl_resource *res, uint32_t id){
    wlog("[WL-LIB] TRACE create_region\n");
    struct wl_resource *r=wl_resource_create(client, &wl_region_interface, wl_resource_get_version(res), id);
    wl_resource_set_implementation(r, &region_impl, NULL, NULL);
    // no-op
}
// Fase 4d: wl_region com impl real (impl NULL + request = segfault
// no SERVIDOR via wl_closure_invoke/ffi — foot chama add/subtract)
static void region_destroy(struct wl_client *c, struct wl_resource *r){ wl_resource_destroy(r); }
static void region_add(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h){}
static void region_subtract(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h){}
static const struct wl_region_interface region_impl={
    .destroy=region_destroy,
    .add=region_add,
    .subtract=region_subtract,
};
static const struct wl_compositor_interface compositor_impl={
    .create_surface=comp_create_surface,
    .create_region=comp_create_region,
};
static void bind_compositor(struct wl_client *client, void *data, uint32_t version, uint32_t id){
    struct wl_resource *r=wl_resource_create(client, &wl_compositor_interface, version, id);
    wl_resource_set_implementation(r, &compositor_impl, NULL, NULL);
    wm_watch_client(client); // FASE8.2-fix: purge-on-disconnect
    wlog("[WL-LIB] bind wl_compositor\n");
}

// wl_surface impl
static void surf_destroy(struct wl_client *c, struct wl_resource *r){
    wlog("[WL-LIB] surface destroy\n");
    /* FASE7-WM: deslista a janela; se era a focada, passa o foco p/
     * a próxima (topo do z-order). Sem isso a lista apodrece e o
     * foco aponta p/ janela morta. */
    struct surf *s=wl_resource_get_user_data(r);
    if(!s){ struct surf *q; wl_list_for_each(q, &surfs, link){ if(q->res==r){ s=q; break; } } }
    if(!s || s->subsurface) return;
    int was_focused=(focused_surf==s);
    if(drag_surf==s) drag_surf=NULL; // FASE7-move: janela morreu no drag
    if(ptr_focus==s) ptr_focus=NULL; // FASE8.2
    if(rs.s==s) rs.s=NULL; // FASE7-resize: janela morreu no resize
    wl_list_remove(&s->link);
    free(s);
    if(was_focused){
        focused_surf=NULL;
        struct surf *q;
        wl_list_for_each_reverse(q, &surfs, link){
            if(!q->subsurface && !q->is_layer && q->mapped && q->buffer){ wm_focus(q); break; }
        }
        if(!focused_surf) focused_client=NULL;
    }
}
static void surf_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buf, int32_t x, int32_t y){
    struct surf *s=wl_resource_get_user_data(r);
    if(s) { s->buffer=buf; }
    char tmp[96]; snprintf(tmp,sizeof(tmp),"[WL-LIB] attach buf=%p x=%d y=%d\n",buf,x,y); wlog(tmp);
}
static void surf_damage(struct wl_client *c, struct wl_resource *r, int32_t x,int32_t y,int32_t w,int32_t h){ wlog("[WL-LIB] TRACE damage\n"); }
static void surf_frame(struct wl_client *c, struct wl_resource *r, uint32_t cb){
    wlog("[WL-LIB] TRACE frame\n");
    struct wl_resource *cr=wl_resource_create(c, &wl_callback_interface, 1, cb);
    wl_callback_send_done(cr, 0);
    wl_resource_destroy(cr);
}
static void surf_set_opaque(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg){}
static void surf_set_input(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg){}
static void surf_commit(struct wl_client *c, struct wl_resource *r){
    struct surf *s=wl_resource_get_user_data(r);
    if(!s) return;
    wlog("[WL-LIB] surface commit\n");
    /* splash sai no 1º commit (cliente vivo): desarma o timer e
     * repinta tudo (apaga o texto). */
    if(splash_on){
        splash_on=0;
        if(splash_timer) wl_event_source_timer_update(splash_timer, 0);
        wlog("[WL-LIB] splash off (first commit)\n");
        redraw_all();
    }
    /* FASE7-WM: foco. Primeira janela mapeada ganha foco; nas demais,
     * enter só se for a focada (commit retenta até o send dar OK).
     * FASE8.0a: layers nunca focam. */
    if(!s->subsurface && !s->is_layer){
        if(!focused_surf) wm_focus(s);
        else if(s==focused_surf) wm_send_enter(s);
    }
    // FASE8.0a: commit de layer = mapeia + posiciona + repinta em ordem
    if(s->is_layer){
        s->mapped=1;
        if(s->buffer){
            struct wl_shm_buffer *shmb=wl_shm_buffer_get(s->buffer);
            if(shmb){
                s->ww=wl_shm_buffer_get_width(shmb);
                s->wh=wl_shm_buffer_get_height(shmb);
                wm_layer_place(s);
                char tmp[96]; snprintf(tmp,sizeof(tmp),"[LAYER] commit layer=%d %dx%d @%d,%d\n",
                    s->layer, s->ww, s->wh, s->wx, s->wy); wlog(tmp);
            }
        }
        redraw_all();
    }
    // if xdg mapped and buffer present, blit
    if(!s->is_layer && s->mapped && s->buffer){
        struct wl_shm_buffer *shmb=wl_shm_buffer_get(s->buffer);
        if(shmb){
            /* FASE7-WM: geometria p/ hit-test (wm_pick) e z-order */
            s->ww=wl_shm_buffer_get_width(shmb);
            s->wh=wl_shm_buffer_get_height(shmb);
            wm_blit_win(s); // FASE8.2: titlebar + conteúdo deslocado
        } else {
            wlog("[WL-LIB] commit no shm buffer\n");
        }
    } else if(s->buffer){
        // not yet mapped (waiting configure ack) – still store
        wlog("[WL-LIB] commit with buffer but not mapped yet\n");
    }
}
static void surf_set_buffer_scale(struct wl_client *c, struct wl_resource *r, int32_t s){}
static void surf_damage_buffer(struct wl_client *c, struct wl_resource *r, int32_t x,int32_t y,int32_t w,int32_t h){}
static void surf_set_buffer_transform(struct wl_client *c, struct wl_resource *r, int32_t t){}
static void surf_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y){}
// FASE7-WM: enter unificado (keymap+enter+repeat+mods, output, pointer).
// Só na janela focada; flags só após send OK (commit retenta).
static void wm_send_enter(struct surf *s){
    if(!s || s->subsurface) return;
    struct wl_client *c=wl_resource_get_client(s->res);
    struct wl_resource *kr=wm_kbd_res(c);
    if(!kr) kr=seat_keyboard_res; // fallback: boot de 1 cliente
    if(kr && !s->kbd_entered){
        if(kbd_keymap_send(kr) == 0)
            wlog("[WL-LIB] keyboard keymap -> client\n");
        struct wl_array keys;
        wl_array_init(&keys);
        wl_keyboard_send_enter(kr, g_serial++, s->res, &keys);
        wl_keyboard_send_repeat_info(kr, 25, 250);
        wl_keyboard_send_modifiers(kr, g_serial++, kbd_mods_depressed, 0, 0, 0);
        s->kbd_entered=1;
        wlog("[WL-LIB] keyboard enter -> client\n");
    }
    if(output_res && !s->output_entered){
        wl_surface_send_enter(s->res, output_res);
        s->output_entered=1;
        wlog("[WL-LIB] output enter -> client\n");
    }
    struct wl_resource *pr=wm_ptr_res(c);
    if(!pr) pr=seat_pointer_res; // fallback
    if(pr && !s->entered){
        wl_pointer_send_enter(pr, g_serial++, s->res,
                              wl_fixed_from_int(cursor_x), wl_fixed_from_int(cursor_y));
        wl_pointer_send_frame(pr);
        s->entered=1;
        wlog("[WL-LIB] pointer enter -> client\n");
    }
    s->focused=(s==focused_surf);
}
const struct wl_surface_interface surface_impl={
    .destroy=surf_destroy,
    .attach=surf_attach,
    .damage=surf_damage,
    .frame=surf_frame,
    .set_opaque_region=surf_set_opaque,
    .set_input_region=surf_set_input,
    .commit=surf_commit,
    .set_buffer_transform=surf_set_buffer_transform,
    .set_buffer_scale=surf_set_buffer_scale,
    .damage_buffer=surf_damage_buffer,
    .offset=surf_offset,
};

// wl_seat impl
// Fase 4d: pointer/keyboard com impl real (foot chama set_cursor; impl
// NULL geraria protocol error e derrubaria o CLIENTE)
static void ptr_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial, struct wl_resource *surface, int32_t hx, int32_t hy){
    char tmp[96]; snprintf(tmp,sizeof(tmp),"[WL-LIB] pointer set_cursor surf=%u hx=%d hy=%d\n", surface?wl_resource_get_id(surface):0, hx, hy); wlog(tmp);
}
static void ptr_release(struct wl_client *c, struct wl_resource *r){ wl_resource_destroy(r); }
static const struct wl_pointer_interface pointer_impl={
    .set_cursor=ptr_set_cursor,
    .release=ptr_release,
};
static void kbd_release(struct wl_client *c, struct wl_resource *r){ wl_resource_destroy(r); }
static const struct wl_keyboard_interface keyboard_impl={
    .release=kbd_release,
};
static void seat_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id){
    struct wl_resource *pr=wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(r), id);
    seat_pointer_res=pr;
    seat_pointer_client=c; // FASE8.2-fix: dono (purge)
    focused_client=c;
    wm_bind_res(c, pr, 0); // FASE7-WM: roteamento por cliente
    wl_resource_set_implementation(pr, &pointer_impl, NULL, NULL);
    char tmp[64]; snprintf(tmp,sizeof(tmp),"[WL-LIB] seat get_pointer %u\n",id); wlog(tmp);
    // send enter if mapped surf
    // not yet
}
static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r, uint32_t id){
    struct wl_resource *kr=wl_resource_create(c, &wl_keyboard_interface, wl_resource_get_version(r), id);
    seat_keyboard_res=kr;
    seat_keyboard_client=c; // FASE8.2-fix: dono (purge)
    wm_bind_res(c, kr, 1); // FASE7-WM: roteamento por cliente
    wl_resource_set_implementation(kr, &keyboard_impl, NULL, NULL);
    char tmp[64]; snprintf(tmp,sizeof(tmp),"[WL-LIB] seat get_keyboard %u\n",id); wlog(tmp);
}
static const struct wl_touch_interface touch_impl;
static void seat_get_touch(struct wl_client *c, struct wl_resource *r, uint32_t id){
    wlog("[WL-LIB] TRACE get_touch\n");
    struct wl_resource *tr=wl_resource_create(c, &wl_touch_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(tr, &touch_impl, NULL, NULL);
}
// Fase 4d: wl_touch com impl real (mesmo motivo do region)
static void touch_release(struct wl_client *c, struct wl_resource *r){ wl_resource_destroy(r); }
static const struct wl_touch_interface touch_impl={
    .release=touch_release,
};
static void seat_release(struct wl_client *c, struct wl_resource *r){ wl_resource_destroy(r); }
static const struct wl_seat_interface seat_impl={
    .get_pointer=seat_get_pointer,
    .get_keyboard=seat_get_keyboard,
    .get_touch=seat_get_touch,
    .release=seat_release,
};
static void bind_seat(struct wl_client *c, void *d, uint32_t ver, uint32_t id){
    struct wl_resource *r=wl_resource_create(c, &wl_seat_interface, ver, id);
    wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
    wm_watch_client(c); // FASE8.2-fix
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER|WL_SEAT_CAPABILITY_KEYBOARD);
    wl_seat_send_name(r, "seat0");
    wlog("[WL-LIB] bind wl_seat\n");
}

// xdg_wm_base / xdg_surface / toplevel
static void xdg_surface_destroy(struct wl_client *c, struct wl_resource *r){ wlog("[WL-LIB] xdg_surface destroy\n"); /* FASE8.2-fix: despendura */ { struct surf *s; wl_list_for_each(s, &surfs, link){ if(s->xdg_surface==r){ s->xdg_surface=NULL; break; } } } wl_resource_destroy(r); }
static void xdg_surface_get_toplevel(struct wl_client *c, struct wl_resource *r, uint32_t id);
static void xdg_surface_get_popup(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *parent, struct wl_resource *pos){}
static void xdg_surface_set_window_geometry(struct wl_client *c, struct wl_resource *r, int32_t x,int32_t y,int32_t w,int32_t h){}
static void xdg_surface_ack_configure(struct wl_client *c, struct wl_resource *r, uint32_t serial){
    wlog("[WL-LIB] xdg_surface ack_configure\n");
    // mark surf mapped
    // find surf by xdg_surface resource
    struct surf *s;
    wl_list_for_each(s, &surfs, link){
        if(s->xdg_surface==r){ s->mapped=1; wlog("[WL-LIB] surface mapped\n"); wm_blit_win(s); /* FASE7-WM: janela nova ganha foco (rouba-foco simples; refinar depois) */ if(!s->subsurface && !s->is_layer) wm_focus(s); break; }
    }
}
static const struct xdg_surface_interface xdg_surface_impl={
    .destroy=xdg_surface_destroy,
    .get_toplevel=xdg_surface_get_toplevel,
    .get_popup=xdg_surface_get_popup,
    .set_window_geometry=xdg_surface_set_window_geometry,
    .ack_configure=xdg_surface_ack_configure,
};
static void toplevel_destroy(struct wl_client *c, struct wl_resource *r){ /* FASE8.2-fix: despendura */ { struct surf *s; wl_list_for_each(s, &surfs, link){ if(s->toplevel==r){ s->toplevel=NULL; break; } } } wl_resource_destroy(r); }
static void toplevel_set_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *p){}
static void toplevel_set_title(struct wl_client *c, struct wl_resource *r, const char *t){ char b[128]; snprintf(b,sizeof(b),"[WL-LIB] toplevel set_title %s\n",t?t:""); wlog(b); /* FASE7-WM: guarda p/ log de foco */ if(t){ struct surf *s; wl_list_for_each(s, &surfs, link){ if(s->toplevel==r){ snprintf(s->title,sizeof(s->title),"%s",t); break; } } } }
static void toplevel_set_app_id(struct wl_client *c, struct wl_resource *r, const char *a){ char b[128]; snprintf(b,sizeof(b),"[WL-LIB] TRACE set_app_id %s\n",a?a:""); wlog(b); }
static void toplevel_show_window_menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, int32_t x,int32_t y){}
static void toplevel_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial){}
static void toplevel_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, uint32_t edges){}
static void toplevel_set_max_size(struct wl_client *c, struct wl_resource *r, int32_t w,int32_t h){}
static void toplevel_set_min_size(struct wl_client *c, struct wl_resource *r, int32_t w,int32_t h){}
static void toplevel_set_maximized(struct wl_client *c, struct wl_resource *r){}
static void toplevel_unset_maximized(struct wl_client *c, struct wl_resource *r){}
static void toplevel_set_fullscreen(struct wl_client *c, struct wl_resource *r, struct wl_resource *out){}
static void toplevel_unset_fullscreen(struct wl_client *c, struct wl_resource *r){}
static void toplevel_set_minimized(struct wl_client *c, struct wl_resource *r){}
static const struct xdg_toplevel_interface toplevel_impl={
    .destroy=toplevel_destroy,
    .set_parent=toplevel_set_parent,
    .set_title=toplevel_set_title,
    .set_app_id=toplevel_set_app_id,
    .show_window_menu=toplevel_show_window_menu,
    .move=toplevel_move,
    .resize=toplevel_resize,
    .set_max_size=toplevel_set_max_size,
    .set_min_size=toplevel_set_min_size,
    .set_maximized=toplevel_set_maximized,
    .unset_maximized=toplevel_unset_maximized,
    .set_fullscreen=toplevel_set_fullscreen,
    .unset_fullscreen=toplevel_unset_fullscreen,
    .set_minimized=toplevel_set_minimized,
};
static void xdg_surface_get_toplevel(struct wl_client *c, struct wl_resource *r, uint32_t id){
    struct wl_resource *tr=wl_resource_create(c, &xdg_toplevel_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(tr, &toplevel_impl, NULL, NULL);
    // link to surf
    struct surf *s;
    wl_list_for_each(s, &surfs, link){ if(s->xdg_surface==r){ s->toplevel=tr; break; } }
    char tmp[64]; snprintf(tmp,sizeof(tmp),"[WL-LIB] get_toplevel %u\n",id); wlog(tmp);
    // send configure for toplevel + xdg_surface (empty states array, not NULL)
    struct wl_array states;
    wl_array_init(&states);
    xdg_toplevel_send_configure(tr, 0, 0, &states);
    wl_array_release(&states);
    uint32_t ser=g_serial++;
    xdg_surface_send_configure(r, ser);
    wlog("[WL-LIB] sent configure\n");
}
static void wm_destroy(struct wl_client *c, struct wl_resource *r){ wl_resource_destroy(r); }
static const struct xdg_positioner_interface positioner_impl;
static void wm_create_positioner(struct wl_client *c, struct wl_resource *r, uint32_t id){
    wlog("[WL-LIB] TRACE create_positioner\n");
    struct wl_resource *pr=wl_resource_create(c, &xdg_positioner_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(pr, &positioner_impl, NULL, NULL);
}
// Fase 4d: xdg_positioner com impl real (foot usa em popups; NULL = crash)
static void pos_destroy(struct wl_client *c, struct wl_resource *r){ wl_resource_destroy(r); }
static void pos_set_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h){}
static void pos_set_anchor_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h){}
static void pos_set_anchor(struct wl_client *c, struct wl_resource *r, uint32_t a){}
static void pos_set_gravity(struct wl_client *c, struct wl_resource *r, uint32_t g){}
static void pos_set_constraint_adjustment(struct wl_client *c, struct wl_resource *r, uint32_t adj){}
static void pos_set_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y){}
static void pos_set_reactive(struct wl_client *c, struct wl_resource *r){}
static void pos_set_parent_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h){}
static void pos_set_parent_configure(struct wl_client *c, struct wl_resource *r, uint32_t serial){}
static const struct xdg_positioner_interface positioner_impl={
    .destroy=pos_destroy,
    .set_size=pos_set_size,
    .set_anchor_rect=pos_set_anchor_rect,
    .set_anchor=pos_set_anchor,
    .set_gravity=pos_set_gravity,
    .set_constraint_adjustment=pos_set_constraint_adjustment,
    .set_offset=pos_set_offset,
    .set_reactive=pos_set_reactive,
    .set_parent_size=pos_set_parent_size,
    .set_parent_configure=pos_set_parent_configure,
};
static void wm_get_xdg_surface(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surf_res){
    struct wl_resource *xr=wl_resource_create(c, &xdg_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(xr, &xdg_surface_impl, NULL, NULL);
    // associate with surf
    struct wl_resource *surf_data = surf_res;
    void *ud = wl_resource_get_user_data(surf_res);
    struct surf *s = (struct surf*)ud;
    // fallback search if ud not set (wl_surface user_data is surf)
    if(!s){
        wl_list_for_each(s, &surfs, link){ if(s->res==surf_res) break; }
    } else {
        // s already
    }
    if(s) s->xdg_surface=xr;
    char tmp[64]; snprintf(tmp,sizeof(tmp),"[WL-LIB] get_xdg_surface %u for surface %u\n",id, wl_resource_get_id(surf_res)); wlog(tmp);
}
static void wm_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial){ wlog("[WL-LIB] pong\n"); }
static const struct xdg_wm_base_interface wm_impl={
    .destroy=wm_destroy,
    .create_positioner=wm_create_positioner,
    .get_xdg_surface=wm_get_xdg_surface,
    .pong=wm_pong,
};
static void bind_xdg(struct wl_client *c, void *d, uint32_t ver, uint32_t id){
    struct wl_resource *r=wl_resource_create(c, &xdg_wm_base_interface, ver, id);
    wl_resource_set_implementation(r, &wm_impl, NULL, NULL);
    wm_watch_client(c); // FASE8.2-fix
    wlog("[WL-LIB] bind xdg_wm_base\n");
}

// ---- Fase 4d: wl_subcompositor (foot dá LOG_ERR fatal sem) ----
static void subcomp_destroy(struct wl_client *c, struct wl_resource *r){ wl_resource_destroy(r); }
static void subsurf_destroy(struct wl_client *c, struct wl_resource *r){
    struct subsurf *ss=wl_resource_get_user_data(r);
    if(ss){ wl_list_remove(&ss->link); free(ss); }
    wl_resource_destroy(r);
    wlog("[WL-LIB] subsurface destroy\n");
}
static void subsurf_set_position(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y){
    struct subsurf *ss=wl_resource_get_user_data(r);
    if(ss){ ss->x=x; ss->y=y; }
}
static void subsurf_place_above(struct wl_client *c, struct wl_resource *r, struct wl_resource *sib){}
static void subsurf_place_below(struct wl_client *c, struct wl_resource *r, struct wl_resource *sib){}
static void subsurf_set_sync(struct wl_client *c, struct wl_resource *r){}
static void subsurf_set_desync(struct wl_client *c, struct wl_resource *r){}
static const struct wl_subsurface_interface subsurf_impl={
    .destroy=subsurf_destroy,
    .set_position=subsurf_set_position,
    .place_above=subsurf_place_above,
    .place_below=subsurf_place_below,
    .set_sync=subsurf_set_sync,
    .set_desync=subsurf_set_desync,
};
static void subcomp_get_subsurface(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface, struct wl_resource *parent){
    struct subsurf *ss=calloc(1,sizeof(*ss));
    ss->client=c; // FASE8.2-fix
    ss->res=wl_resource_create(c, &wl_subsurface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(ss->res, &subsurf_impl, ss, NULL);
    ss->parent=(struct surf*)wl_resource_get_user_data(parent);
    wl_list_insert(&subsurfs, &ss->link);
    // acha o surf filho e ancora o offset nele (commit desenha em x,y)
    struct surf *s=(struct surf*)wl_resource_get_user_data(surface);
    if(!s){ struct surf *q; wl_list_for_each(q, &surfs, link){ if(q->res==surface){ s=q; break; } } }
    if(s) s->subsurface=ss;
    char tmp[96]; snprintf(tmp,sizeof(tmp),"[WL-LIB] get_subsurface %u\n",id); wlog(tmp);
}
static const struct wl_subcompositor_interface subcomp_impl={
    .destroy=subcomp_destroy,
    .get_subsurface=subcomp_get_subsurface,
};
static void bind_subcomp(struct wl_client *c, void *d, uint32_t ver, uint32_t id){
    struct wl_resource *r=wl_resource_create(c, &wl_subcompositor_interface, ver, id);
    wl_resource_set_implementation(r, &subcomp_impl, NULL, NULL);
    wm_watch_client(c); // FASE8.2-fix
    wlog("[WL-LIB] bind wl_subcompositor\n");
}

// ---- Fase 4d: wl_output (foot dá LOG_ERR "no monitors" sem) ----
static void output_release(struct wl_client *c, struct wl_resource *r){ wl_resource_destroy(r); }
static const struct wl_output_interface output_impl={
    .release=output_release,
};
static void bind_output(struct wl_client *c, void *d, uint32_t ver, uint32_t id){
    struct wl_resource *r=wl_resource_create(c, &wl_output_interface, ver, id);
    wl_resource_set_implementation(r, &output_impl, NULL, NULL);
    output_res=r;
    output_client=c; // FASE8.2-fix: dono (purge)
    wm_watch_client(c); // FASE8.2-fix
    wl_output_send_geometry(r, 0, 0, 310, 170, WL_OUTPUT_SUBPIXEL_UNKNOWN, "TipOS", "QEMU", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT|WL_OUTPUT_MODE_PREFERRED, fb_w, fb_h, 60000);
    if(ver>=2) wl_output_send_scale(r, 1);
    wl_output_send_done(r);
    wlog("[WL-LIB] bind wl_output\n");
}

// ---- Fase 4d: wl_data_device_manager (foot dá LOG_ERR "no clipboard" sem) ----
static void ds_offer(struct wl_client *c, struct wl_resource *r, const char *mime){ char b[128]; snprintf(b,sizeof(b),"[WL-LIB] data_source offer %s\n",mime?mime:""); wlog(b); }
static void ds_destroy(struct wl_client *c, struct wl_resource *r){ wl_resource_destroy(r); }
static void ds_set_actions(struct wl_client *c, struct wl_resource *r, uint32_t dnd_actions){}
static const struct wl_data_source_interface ds_impl={
    .offer=ds_offer,
    .destroy=ds_destroy,
    .set_actions=ds_set_actions,
};
static void dd_start_drag(struct wl_client *c, struct wl_resource *r, struct wl_resource *src, struct wl_resource *origin, struct wl_resource *icon, uint32_t serial){ wlog("[WL-LIB] data_device start_drag (noop)\n"); }
static void dd_set_selection(struct wl_client *c, struct wl_resource *r, struct wl_resource *src, uint32_t serial){ wlog("[WL-LIB] data_device set_selection\n"); }
static void dd_release(struct wl_client *c, struct wl_resource *r){ wl_resource_destroy(r); }
static const struct wl_data_device_interface dd_impl={
    .start_drag=dd_start_drag,
    .set_selection=dd_set_selection,
    .release=dd_release,
};
static void mgr_create_data_source(struct wl_client *c, struct wl_resource *r, uint32_t id){
    struct wl_resource *sr=wl_resource_create(c, &wl_data_source_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(sr, &ds_impl, NULL, NULL);
    wlog("[WL-LIB] create_data_source\n");
}
static void mgr_get_data_device(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat){
    struct wl_resource *dr=wl_resource_create(c, &wl_data_device_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(dr, &dd_impl, NULL, NULL);
    wlog("[WL-LIB] get_data_device\n");
}
static const struct wl_data_device_manager_interface mgr_impl={
    .create_data_source=mgr_create_data_source,
    .get_data_device=mgr_get_data_device,
};
static void bind_ddm(struct wl_client *c, void *d, uint32_t ver, uint32_t id){
    struct wl_resource *r=wl_resource_create(c, &wl_data_device_manager_interface, ver, id);
    wl_resource_set_implementation(r, &mgr_impl, NULL, NULL);
    wm_watch_client(c); // FASE8.2-fix
    wlog("[WL-LIB] bind wl_data_device_manager\n");
}

/* ---- FASE8.0a: zwlr_layer_shell_v1 (server, hand-rolled) ----
 * Opcodes/assinaturas do protocolo estável wlr-layer-shell-unstable-v1
 * (sem wayland-scanner no TipOS: tabelas manuais). */
static const struct wl_interface zwlr_layer_surface_v1_interface;
static void layer_shell_get_layer_surface(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface, struct wl_resource *output, uint32_t layer, const char *ns);
static void layer_shell_destroy(struct wl_client *c, struct wl_resource *r){ wl_resource_destroy(r); }
static const struct wl_interface *zwlr_layer_shell_v1_types[]={
    &zwlr_layer_surface_v1_interface, &wl_surface_interface, &wl_output_interface,
};
static const struct wl_message zwlr_layer_shell_v1_requests[]={
    { "get_layer_surface", "noous", zwlr_layer_shell_v1_types },
    { "destroy", "", NULL },
};
static const struct wl_interface zwlr_layer_shell_v1_interface={
    "zwlr_layer_shell_v1", 4, 2, zwlr_layer_shell_v1_requests, 0, NULL,
};
struct zwlr_layer_shell_v1_interface {
    void (*get_layer_surface)(struct wl_client*, struct wl_resource*, uint32_t, struct wl_resource*, struct wl_resource*, uint32_t, const char*);
    void (*destroy)(struct wl_client*, struct wl_resource*);
};
static void layer_surface_set_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h){
    struct surf *s=wl_resource_get_user_data(r);
    if(s){ s->layer_w=w; s->layer_h=h; }
    char b[96]; snprintf(b,sizeof(b),"[LAYER] set_size %dx%d\n",w,h); wlog(b);
}
static void layer_surface_set_anchor(struct wl_client *c, struct wl_resource *r, uint32_t a){
    struct surf *s=wl_resource_get_user_data(r);
    if(s){ s->anchor=(int)a; }
    char b[64]; snprintf(b,sizeof(b),"[LAYER] set_anchor %u\n",a); wlog(b);
}
static int wm_top_zone=0, wm_bottom_zone=0; // exclusive zones (1 saída)
static void layer_surface_set_exclusive_zone(struct wl_client *c, struct wl_resource *r, int32_t z){
    struct surf *s=wl_resource_get_user_data(r);
    if(s){
        s->excl_zone=z;
        if(z>0){
            if((s->anchor&1) && z>wm_top_zone) wm_top_zone=z;
            if((s->anchor&2) && z>wm_bottom_zone) wm_bottom_zone=z;
        }
    }
    char b[64]; snprintf(b,sizeof(b),"[LAYER] exclusive_zone %d (top=%d bottom=%d)\n",z,wm_top_zone,wm_bottom_zone); wlog(b);
}
static void layer_surface_set_margin(struct wl_client *c, struct wl_resource *r, int32_t t, int32_t ri, int32_t b, int32_t l){
    struct surf *s=wl_resource_get_user_data(r);
    if(s){ s->margin_t=t; s->margin_r=ri; s->margin_b=b; s->margin_l=l; }
}
static void layer_surface_set_kbd(struct wl_client *c, struct wl_resource *r, uint32_t v){
    struct surf *s=wl_resource_get_user_data(r);
    if(s){ s->kbd_interactive=(int)v; }
    char b[64]; snprintf(b,sizeof(b),"[LAYER] kbd_interactive %u\n",v); wlog(b);
}
static void layer_surface_get_popup(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *parent){
    wlog("[LAYER] get_popup (noop)\n"); // popups: fora do escopo 8.0a
}
static void layer_surface_ack_configure(struct wl_client *c, struct wl_resource *r, uint32_t serial){
    struct surf *s=wl_resource_get_user_data(r);
    if(s){ s->layer_acked=serial; }
    char b[64]; snprintf(b,sizeof(b),"[LAYER] ack_configure %u\n",serial); wlog(b);
}
static void layer_surface_destroy(struct wl_client *c, struct wl_resource *r){
    struct surf *s=wl_resource_get_user_data(r);
    if(s){ s->is_layer=0; s->mapped=0; }
    wlog("[LAYER] layer_surface destroy\n");
    wl_resource_destroy(r);
    redraw_all();
}
static void layer_surface_set_layer(struct wl_client *c, struct wl_resource *r, uint32_t layer){
    struct surf *s=wl_resource_get_user_data(r);
    if(s && layer<=3){ s->layer=(int)layer; }
    char b[64]; snprintf(b,sizeof(b),"[LAYER] set_layer %u\n",layer); wlog(b);
    redraw_all();
}
static const struct wl_message zwlr_layer_surface_v1_requests[]={
    { "set_size", "uu", NULL },
    { "set_anchor", "u", NULL },
    { "set_exclusive_zone", "i", NULL },
    { "set_margin", "iiii", NULL },
    { "set_keyboard_interactivity", "u", NULL },
    { "get_popup", "oo", NULL }, // tipos: sem xdg_popup aqui; noop (nunca chamado no 8.0a)
    { "ack_configure", "u", NULL },
    { "destroy", "", NULL },
    { "set_layer", "u", NULL },
};
static const struct wl_message zwlr_layer_surface_v1_events[]={
    { "configure", "uuu", NULL },
    { "closed", "", NULL },
};
static const struct wl_interface zwlr_layer_surface_v1_interface={
    "zwlr_layer_surface_v1", 4, 9, zwlr_layer_surface_v1_requests, 2, zwlr_layer_surface_v1_events,
};
struct zwlr_layer_surface_v1_interface {
    void (*set_size)(struct wl_client*, struct wl_resource*, int32_t, int32_t);
    void (*set_anchor)(struct wl_client*, struct wl_resource*, uint32_t);
    void (*set_exclusive_zone)(struct wl_client*, struct wl_resource*, int32_t);
    void (*set_margin)(struct wl_client*, struct wl_resource*, int32_t, int32_t, int32_t, int32_t);
    void (*set_keyboard_interactivity)(struct wl_client*, struct wl_resource*, uint32_t);
    void (*get_popup)(struct wl_client*, struct wl_resource*, uint32_t, struct wl_resource*);
    void (*ack_configure)(struct wl_client*, struct wl_resource*, uint32_t);
    void (*destroy)(struct wl_client*, struct wl_resource*);
    void (*set_layer)(struct wl_client*, struct wl_resource*, uint32_t);
};
static const struct zwlr_layer_shell_v1_interface layer_shell_impl={
    .get_layer_surface=layer_shell_get_layer_surface,
    .destroy=layer_shell_destroy,
};
static const struct zwlr_layer_surface_v1_interface layer_surface_impl={
    .set_size=layer_surface_set_size,
    .set_anchor=layer_surface_set_anchor,
    .set_exclusive_zone=layer_surface_set_exclusive_zone,
    .set_margin=layer_surface_set_margin,
    .set_keyboard_interactivity=layer_surface_set_kbd,
    .get_popup=layer_surface_get_popup,
    .ack_configure=layer_surface_ack_configure,
    .destroy=layer_surface_destroy,
    .set_layer=layer_surface_set_layer,
};
static void layer_shell_get_layer_surface(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface, struct wl_resource *output, uint32_t layer, const char *ns){
    struct wl_resource *lr=wl_resource_create(c, &zwlr_layer_surface_v1_interface, wl_resource_get_version(r), id);
    struct surf *s=(struct surf*)wl_resource_get_user_data(surface);
    if(!s){ struct surf *q; wl_list_for_each(q, &surfs, link){ if(q->res==surface){ s=q; break; } } }
    if(s){
        s->is_layer=1;
        s->layer=layer>3?3:(int)layer;
        wl_resource_set_implementation(lr, &layer_surface_impl, s, NULL);
        s->layer_serial=g_serial++;
        wl_resource_post_event(lr, 0, s->layer_serial, 0, 0); // configure inicial (0x0 = cliente decide)
    } else {
        wl_resource_set_implementation(lr, &layer_surface_impl, NULL, NULL);
    }
    char b[128]; snprintf(b,sizeof(b),"[LAYER] get_layer_surface layer=%u ns=%s serial=%u\n",
        layer, ns?ns:"?", s?s->layer_serial:0); wlog(b);
}
static void bind_layer_shell(struct wl_client *c, void *d, uint32_t ver, uint32_t id){
    struct wl_resource *r=wl_resource_create(c, &zwlr_layer_shell_v1_interface, ver, id);
    wl_resource_set_implementation(r, &layer_shell_impl, NULL, NULL);
    wm_watch_client(c);
    wlog("[WL-LIB] bind zwlr_layer_shell_v1\n");
}
// posição pela âncora (+margens); size 0 = tamanho do buffer
static void wm_layer_place(struct surf *s){
    int w=s->layer_w>0?s->layer_w:s->ww;
    int h=s->layer_h>0?s->layer_h:s->wh;
    if(w<=0) w=s->ww; if(h<=0) h=s->wh;
    int a=s->anchor, x, y;
    if(a&4) x=s->margin_l;
    else if(a&8) x=fb_w-w-s->margin_r;
    else x=(fb_w-w)/2;
    if(a&1) y=s->margin_t;
    else if(a&2) y=fb_h-h-s->margin_b;
    else y=(fb_h-h)/2;
    if(x<0) x=0; if(y<0) y=0;
    s->wx=x; s->wy=y;
}
static void wm_blit_layer(struct surf *s){
    if(!s || !s->mapped || !s->buffer) return;
    struct wl_shm_buffer *shmb=wl_shm_buffer_get(s->buffer);
    if(!shmb) return;
    s->ww=wl_shm_buffer_get_width(shmb);
    s->wh=wl_shm_buffer_get_height(shmb);
    wm_layer_place(s);
    fb_blit_shm(shmb, s->wx, s->wy);
}

// cursor repinta via idle (coalesce): N motions num dispatch geram 1
// repaint, e o socket do cliente sempre intercala (sem isso, flood
// de motion saturava o loop e o handshake do foot travava).
static struct wl_event_loop *g_loop=NULL;
static int cursor_dirty=0, cursor_idle_on=0;
static void redraw_all(void); // definida abaixo (usa surfs)
static void cursor_idle_cb(void *data){
    (void)data;
    cursor_idle_on=0;
    if(!cursor_dirty) return;
    cursor_dirty=0;
    redraw_all();
}
// repaint total: FUNDO + mapped + cursor por cima. O fill apaga o
// cursor da posição antiga MESMO fora de qualquer superfície (só
// repintar surfaces deixava rastro no cinza de fundo).
static void redraw_all(void){
    if(fb_addr){
        uint32_t *dst=(uint32_t*)fb_addr;
        for(int y=0;y<fb_h;y++) for(int x=0;x<fb_w;x++) dst[y*fb_pitch+x]=0xFF202020u;
    }
    struct surf *s;
    // FASE8.0a: fundo primeiro (background/bottom), xdg no meio,
    // top/overlay por último. Cursor sempre por cima.
    wl_list_for_each(s, &surfs, link){
        if(s->is_layer && (s->layer==0||s->layer==1)) wm_blit_layer(s);
    }
    wl_list_for_each(s, &surfs, link){
        if(s->is_layer) continue;
        wm_blit_win(s); // FASE8.2: titlebar + conteúdo (pula minimizada)
    }
    wl_list_for_each(s, &surfs, link){
        if(s->is_layer && (s->layer==2||s->layer==3)) wm_blit_layer(s);
    }
    draw_cursor();
}
// ---- input real via evdev (espelha wayland_compositor_shm.c) ----
static uint32_t ev_time=1000;
static uint32_t kbd_mods_depressed=0; // máscara xkb p/ wl_keyboard_modifiers
static void kbd_track_mods(uint32_t code, int pressed){
    uint32_t bit=0;
    switch(code){
    case 42: case 54: bit=0x01; break; /* Shift */
    case 29: case 97: bit=0x04; break; /* Control */
    case 56: case 100: bit=0x08; break; /* Alt/Mod1 */
    case 125: case 126: bit=0x40; break; /* Super/Mod4 */
    default: return;
    }
    if(pressed) kbd_mods_depressed|=bit; else kbd_mods_depressed&=~bit;
    if(seat_keyboard_res){
        wl_keyboard_send_modifiers(seat_keyboard_res, g_serial++,
            kbd_mods_depressed, 0, 0, 0);
    }
}
static int evdev_fd_cb(int fd, uint32_t mask, void *data){
    (void)data;
    if(!(mask & WL_EVENT_READABLE)) return 0;
    struct input_event evs[16];
    ssize_t n=read(fd, evs, sizeof(evs));
    if(n<=0) return 0;
    int cnt=n/(int)sizeof(struct input_event);
    int moved=0;
    for(int k=0;k<cnt;k++){
        struct input_event *e=&evs[k];
        if(e->type==EV_REL){
            if(e->code==REL_X){ cursor_x+=e->value; moved=1; }
            else if(e->code==REL_Y){ cursor_y+=e->value; moved=1; }
        } else if(e->type==EV_KEY){
            if(e->code==BTN_LEFT||e->code==BTN_RIGHT||e->code==BTN_MIDDLE){
                /* FASE8.2: layer top/overlay primeiro (sem Alt): enter se
                 * preciso + encaminha p/ o cliente da layer, consome
                 * (xdg embaixo não vê). Com Alt, cai no drag xdg normal. */
                if(!(kbd_mods_depressed & 0x08)){
                    struct surf *lh=wm_pick_layer(cursor_x, cursor_y);
                    if(lh){
                        if(ptr_focus && ptr_focus!=lh) wm_ptr_leave(ptr_focus);
                        wm_layer_enter(lh);
                        struct wl_resource *pr=wm_client_ptr(lh);
                        if(pr){
                            wl_pointer_send_button(pr, g_serial++, ev_time++,
                                (uint32_t)e->code, e->value ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
                            wl_pointer_send_frame(pr);
                            char lg[96]; snprintf(lg,sizeof(lg),"[WM] layer button code=%d state=%d\n",e->code,e->value); wlog(lg);
                        }
                        continue; // consome (layer)
                    }
                }
                /* FASE7-move: Alt+clique-esquerdo inicia drag (consome o
                 * botão — senão o foot abre seleção de texto junto).
                 * Release com drag ativo termina (também consumido). */
                if(e->code==BTN_LEFT && e->value==1 && (kbd_mods_depressed & 0x08)){
                    struct surf *hit=wm_pick(cursor_x, cursor_y);
                    if(hit){
                        if(hit!=focused_surf) wm_focus(hit);
                        drag_surf=hit;
                        drag_offx=cursor_x-hit->wx;
                        drag_offy=cursor_y-hit->wy;
                        char lg[96]; snprintf(lg,sizeof(lg),"[WM] drag start surf=%u off=%d,%d\n",
                            wl_resource_get_id(hit->res), drag_offx, drag_offy); wlog(lg);
                    } else {
                        wlog("[WM] drag start: sem janela (swallow)\n");
                    }
                    continue; // consome press
                }
                if(e->code==BTN_LEFT && e->value==0 && drag_surf){
                    char lg[64]; snprintf(lg,sizeof(lg),"[WM] drag end @%d,%d\n",
                        drag_surf->wx, drag_surf->wy); wlog(lg);
                    drag_surf=NULL;
                    continue; // consome release
                }
                /* FASE7-resize: Alt+botão-direito inicia/termina (consome;
                 * senão o foot estende seleção junto). */
                if(e->code==BTN_RIGHT && e->value==1 && (kbd_mods_depressed & 0x08)){
                    struct surf *hit=wm_pick(cursor_x, cursor_y);
                    if(hit){
                        if(hit!=focused_surf) wm_focus(hit);
                        wm_resize_begin(hit);
                    } else {
                        wlog("[WM] resize start: sem janela (swallow)\n");
                    }
                    continue; // consome press
                }
                if(e->code==BTN_RIGHT && e->value==0 && rs.s){
                    /* garante o tamanho final (throttle pode ter segurado) */
                    wm_resize_send(rs.sent_w, rs.sent_h);
                    char lg[64]; snprintf(lg,sizeof(lg),"[WM] resize end %dx%d\n",
                        rs.sent_w, rs.sent_h); wlog(lg);
                    rs.s=NULL;
                    continue; // consome release
                }
                /* FASE8.2: clique na titlebar — botões ou arrasto-move.
                 * X fecha via protocolo; _ minimiza; □ max/unmax;
                 * resto da barra = move sem precisar de Alt. */
                if(e->code==BTN_LEFT && e->value==1){
                    struct surf *hit=wm_pick(cursor_x, cursor_y);
                    if(hit && hit->toplevel && cursor_y < hit->wy+WM_TB_H){
                        int b=-1;
                        for(int i=0;i<3;i++){
                            int x0,y0,x1,y1; wm_btn_rect(hit,i,&x0,&y0,&x1,&y1);
                            if(cursor_x>=x0&&cursor_x<x1&&cursor_y>=y0&&cursor_y<y1){ b=i; break; }
                        }
                        if(b==2){
                            if(hit!=focused_surf) wm_focus(hit);
                            if(hit->toplevel) xdg_toplevel_send_close(hit->toplevel);
                            wlog("[WM] close button -> client\n");
                            continue;
                        } else if(b==0){
                            hit->minimized=1;
                            wlog("[WM] minimize button\n");
                            redraw_all();
                            continue;
                        } else if(b==1){
                            if(hit!=focused_surf) wm_focus(hit);
                            wm_toggle_max(hit);
                            continue;
                        } else {
                            if(hit!=focused_surf) wm_focus(hit);
                            drag_surf=hit;
                            drag_offx=cursor_x-hit->wx;
                            drag_offy=cursor_y-hit->wy;
                            wlog("[WM] titlebar drag start\n");
                            continue;
                        }
                    }
                    if(hit && hit!=focused_surf) wm_focus(hit);
                }
                /* FASE8.2-fix: botão SÓ com pointer-enter prévio (o
                 * foot aborta em 'term != NULL' sem enter). Sem focada
                 * ou sem enter, descarta em vez de encaminhar. */
                struct wl_resource *pr=NULL;
                if(focused_surf && focused_surf->entered)
                    pr=wm_ptr_res(wl_resource_get_client(focused_surf->res));
                if(!pr){
                    char lg[96]; snprintf(lg,sizeof(lg),"[WM] button dropped (no enter) code=%d\n",e->code); wlog(lg);
                } else {
                    wl_pointer_send_button(pr, g_serial++, ev_time++,
                        (uint32_t)e->code, e->value ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
                    wl_pointer_send_frame(pr);
                    char lg[96]; snprintf(lg,sizeof(lg),"[WL-LIB] button code=%d state=%d -> client\n",e->code,e->value); wlog(lg);
                }
            } else {
                /* FASE7-WM: máscara primeiro (Alt do Tab já conta),
                 * Alt+Tab alterna foco e é consumido (não vai p/ app). */
                kbd_track_mods((uint32_t)e->code, e->value ? 1 : 0);
                if(e->code==15 /*KEY_TAB*/ && (kbd_mods_depressed & 0x08)){
                    if(e->value==1) wm_focus_next();
                    wlog("[WM] Alt+Tab (swallow)\n");
                } else {
                    struct wl_resource *kr=NULL;
                    if(focused_surf) kr=wm_kbd_res(wl_resource_get_client(focused_surf->res));
                    if(!kr) kr=seat_keyboard_res; // fallback
                    if(kr){
                        wl_keyboard_send_key(kr, g_serial++, ev_time++,
                            (uint32_t)e->code, e->value ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
                        char lg[96]; snprintf(lg,sizeof(lg),"[WL-LIB] key code=%d state=%d -> client\n",e->code,e->value); wlog(lg);
                    }
                }
            }
        }
    }
    if(moved){
        if(cursor_x<0) cursor_x=0; if(cursor_x>=fb_w) cursor_x=fb_w-1;
        if(cursor_y<0) cursor_y=0; if(cursor_y>=fb_h) cursor_y=fb_h-1;
        /* FASE7-move: janela segue o cursor (grab offset). Clamp: deixa
         * sempre uma faixa de 20px agarrável dentro da tela. O repaint
         * já é coalescido abaixo (cursor_dirty/idle) — sem custo extra. */
        if(drag_surf){
            int nx=cursor_x-drag_offx, ny=cursor_y-drag_offy;
            if(nx<-(drag_surf->ww-20)) nx=-(drag_surf->ww-20);
            if(ny<0) ny=0;
            if(nx>fb_w-20) nx=fb_w-20;
            if(ny>fb_h-20) ny=fb_h-20;
            drag_surf->wx=nx; drag_surf->wy=ny;
        }
        /* FASE7-resize: delta do cursor vira tamanho; throttle de 8px
         * evita flood de configure (cada um = re-render no foot). */
        if(rs.s){
            int w=rs.w0+(cursor_x-rs.cx0), h=rs.h0+(cursor_y-rs.cy0);
            int dw=w>rs.sent_w?w-rs.sent_w:rs.sent_w-w;
            int dh=h>rs.sent_h?h-rs.sent_h:rs.sent_h-h;
            if(dw>=8 || dh>=8) wm_resize_send(w, h);
        }
        // repinta no idle (coalesce), nunca aqui: motion em rajada
        // não pode segurar o dispatch do socket do cliente
        cursor_dirty=1;
        if(g_loop && !cursor_idle_on){
            cursor_idle_on=1;
            wl_event_loop_add_idle(g_loop, cursor_idle_cb, NULL);
        }
        /* FASE8.2: motion roteado — layer top/overlay (coords locais) ou
         * janela focada (cliente dela, não global). Hover em layer dá
         * leave na xdg; voltar p/ xdg re-arme o enter via wm_send_enter
         * (só pointer é reenviado: kbd/output seguem marcados). */
        {
            struct surf *lh=wm_pick_layer(cursor_x, cursor_y);
            struct wl_resource *pr=NULL;
            if(lh){
                if(ptr_focus && ptr_focus!=lh) wm_ptr_leave(ptr_focus);
                wm_layer_enter(lh);
                pr=wm_client_ptr(lh);
                if(pr){
                    wl_pointer_send_motion(pr, ev_time++,
                        wl_fixed_from_int(cursor_x-lh->wx), wl_fixed_from_int(cursor_y-lh->wy));
                    wl_pointer_send_frame(pr);
                }
            } else {
                if(ptr_focus && ptr_focus->is_layer) wm_ptr_leave(ptr_focus);
                if(focused_surf && !focused_surf->is_layer){
                    if(!focused_surf->entered) wm_send_enter(focused_surf);
                    ptr_focus=focused_surf;
                    pr=wm_client_ptr(focused_surf);
                }
                if(!pr) pr=seat_pointer_res; // fallback
                if(pr){
                    wl_pointer_send_motion(pr, ev_time++,
                        wl_fixed_from_int(cursor_x), wl_fixed_from_int(cursor_y));
                    wl_pointer_send_frame(pr);
                }
            }
        }
        char lg[96]; snprintf(lg,sizeof(lg),"[WL-LIB] pointer motion %d,%d\n",cursor_x,cursor_y); wlog(lg);
    }
    return 0;
}

// wl_display terminate: eventfd handler is internal, we just need loop to run

int main(void){
    wlog("[WL-LIB] compositor lib starting\n");
    /* FASE6-flake tripwire: aborta com diagnóstico glibc na PRIMEIRA
     * corrupção de heap detectada em malloc/free (vs. walk silencioso
     * que só explode depois em wl_event_loop_dispatch). */
    mallopt(M_CHECK_ACTION, 3);
    setenv("XDG_RUNTIME_DIR","/tmp",1);
    if(fb_init()<0) wlog("[WL-LIB] fb init fail\n");
    // evdev is polled via wl_event_loop_add_fd below

    g_display=wl_display_create();
    if(!g_display){ wlog("[WL-LIB] display create fail\n"); return 1; }
    wl_list_init(&surfs);
    wl_list_init(&subsurfs);

    if(wl_display_init_shm(g_display)<0) wlog("[WL-LIB] init_shm fail\n"); else wlog("[WL-LIB] shm ok\n");

    wl_global_create(g_display, &wl_compositor_interface, 4, NULL, bind_compositor);
    wl_global_create(g_display, &wl_seat_interface, 7, NULL, bind_seat);
    wl_global_create(g_display, &xdg_wm_base_interface, 3, NULL, bind_xdg);
    // Fase 4d: globals que o foot exige (fatais sem)
    wl_global_create(g_display, &wl_subcompositor_interface, 1, NULL, bind_subcomp);
    wl_global_create(g_display, &wl_output_interface, 2, NULL, bind_output);
    wl_global_create(g_display, &wl_data_device_manager_interface, 3, NULL, bind_ddm);
    wl_global_create(g_display, &zwlr_layer_shell_v1_interface, 4, NULL, bind_layer_shell); // FASE8.0a
    wlog("[WL-LIB] globals created\n");

    if(wl_display_add_socket(g_display, "wayland-0")<0){
        wlog("[WL-LIB] add_socket wayland-0 fail\n");
        perror("add_socket");
        return 1;
    }
    wlog("[WL-LIB] listening /tmp/wayland-0\n");

    struct wl_event_loop *loop=wl_display_get_event_loop(g_display);
    g_loop=loop; // p/ cursor_idle_cb (defer do motion)
    // evdev via event loop (input real: motion/button/key -> cliente)
    int ev0=open("/dev/input/event0", O_RDONLY|O_NONBLOCK);
    int ev1=open("/dev/input/event1", O_RDONLY|O_NONBLOCK);
    char tmp[128]; snprintf(tmp,sizeof(tmp),"[WL-LIB] evdev ev0=%d ev1=%d\n",ev0,ev1); wlog(tmp);
    if(ev0>=0) wl_event_loop_add_fd(loop, ev0, WL_EVENT_READABLE, evdev_fd_cb, NULL);
    if(ev1>=0) wl_event_loop_add_fd(loop, ev1, WL_EVENT_READABLE, evdev_fd_cb, NULL);
    // splash até o 1º commit do cliente (feedback no boot lento)
    splash_draw(1);
    wlog("[WL-LIB] splash armed\n");
    splash_timer=wl_event_loop_add_timer(loop, splash_timer_cb, NULL);
    { char b[128]; snprintf(b,sizeof(b),"[WL-LIB] splash timer=%p update=%d\n",
        (void*)splash_timer, splash_timer?wl_event_source_timer_update(splash_timer, 500):-99); wlog(b); }

    // run
    wlog("[WL-LIB] running\n");
    wl_display_run(g_display);

    wlog("[WL-LIB] exit\n");
    wl_display_destroy(g_display);
    return 0;
}
