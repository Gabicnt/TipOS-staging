#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>

int main(void) {
    printf("FB_EVDEV test start\n");
    fflush(stdout);
    int fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) {
        perror("open /dev/fb0");
        return 1;
    }
    printf("open /dev/fb0 OK fd=%d\n", fb);
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    memset(&vinfo, 0, sizeof(vinfo));
    memset(&finfo, 0, sizeof(finfo));
    if (ioctl(fb, FBIOGET_VSCREENINFO, &vinfo)==0) {
        printf("FB V: %dx%d bpp=%d\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
        printf("  red off=%d len=%d green off=%d len=%d blue off=%d len=%d\n",
               vinfo.red.offset, vinfo.red.length,
               vinfo.green.offset, vinfo.green.length,
               vinfo.blue.offset, vinfo.blue.length);
    } else {
        perror("FBIOGET_VSCREENINFO");
    }
    if (ioctl(fb, FBIOGET_FSCREENINFO, &finfo)==0) {
        printf("FB F: id=%.16s smem_len=%u line_len=%u smem_start=0x%lx\n",
               finfo.id, finfo.smem_len, finfo.line_length, (unsigned long)finfo.smem_start);
    } else {
        perror("FBIOGET_FSCREENINFO");
    }
    size_t size = finfo.smem_len;
    if (size==0) size = (size_t)vinfo.xres * vinfo.yres * 4;
    if (size==0) size = 1024*768*4;
    void *map = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fb, 0);
    if (map == MAP_FAILED) {
        perror("mmap fb0");
        return 1;
    }
    printf("FB mmap OK at %p size %zu (write-back, no flush needed)\n", map, size);
    fflush(stdout);
    int W = vinfo.xres ? (int)vinfo.xres : 1024;
    int H = vinfo.yres ? (int)vinfo.yres : 768;
    int pitch = finfo.line_length ? (int)(finfo.line_length/4) : W;
    uint32_t *fb32 = (uint32_t*)map;
    int sq = 200;
    int x0 = W/2 - sq/2;
    int y0 = H/2 - sq/2;
    if (x0<0) x0=0;
    if (y0<0) y0=0;
    uint32_t red = 0xFFFF0000u;
    for (int y=0; y<sq; y++) {
        for (int x=0; x<sq; x++) {
            fb32[(y0+y)*pitch + (x0+x)] = red;
        }
    }
    printf("Drew red square %dx%d at %d,%d color 0x%08x\n", sq,sq,x0,y0, (unsigned)red);
    uint32_t pix = fb32[y0*pitch + x0];
    printf("Pixel check at %d,%d: 0x%08x %s\n", x0, y0, (unsigned)pix, (pix==red) ? "RED OK" : "FAIL");
    uint32_t pix2 = fb32[(y0+sq/2)*pitch + (x0+sq/2)];
    printf("Pixel center: 0x%08x %s\n", (unsigned)pix2, (pix2==red) ? "RED OK" : "FAIL");
    fflush(stdout);

    int ev0 = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (ev0<0) ev0 = open("/dev/input/event0", O_RDWR | O_NONBLOCK);
    if (ev0>=0) printf("event0 open OK fd=%d (mouse)\n", ev0); else perror("event0");
    int ev1 = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (ev1<0) ev1 = open("/dev/input/event1", O_RDWR | O_NONBLOCK);
    if (ev1>=0) printf("event1 open OK fd=%d (kbd)\n", ev1); else perror("event1");
    fflush(stdout);

    struct pollfd pfd[2];
    int nfds=0;
    if (ev0>=0) { pfd[nfds].fd=ev0; pfd[nfds].events=POLLIN; nfds++; }
    if (ev1>=0) { pfd[nfds].fd=ev1; pfd[nfds].events=POLLIN; nfds++; }
    if (nfds==0) {
        printf("No evdev fds, skip poll\n");
        return 0;
    }
    printf("Polling events for 5s (move mouse / press keys)...\n");
    fflush(stdout);
    for (int iter=0; iter<50; iter++) {
        int r = poll(pfd, nfds, 100);
        if (r>0) {
            for (int i=0;i<nfds;i++) if (pfd[i].revents & POLLIN) {
                struct input_event ev[32];
                ssize_t n = read(pfd[i].fd, ev, sizeof(ev));
                if (n>0) {
                    int cnt = (int)(n / sizeof(struct input_event));
                    for (int k=0;k<cnt;k++) {
                        printf("ev%d: type=%d code=%d value=%d\n", i, ev[k].type, ev[k].code, ev[k].value);
                    }
                    fflush(stdout);
                }
            }
        }
    }
    printf("FB_EVDEV test done (square should stay on screen)\n");
    fflush(stdout);
    return 0;
}
