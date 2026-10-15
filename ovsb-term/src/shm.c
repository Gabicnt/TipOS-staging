/* ovsb.term — shm.c — buffers wl_shm via memfd (moe moe kyun <3) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "term.h"
#include "shm.h"

#include <wayland-client.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef SYS_memfd_create
#define SYS_memfd_create 319
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001u
#endif

/* cria fd anônimo: memfd_create(2), fallback via syscall direta */
static int shm_anon_fd(void)
{
#ifdef __linux__
    int fd = memfd_create("ovsb-term-shm", MFD_CLOEXEC);
    if (fd >= 0)
        return fd;
    /* fallback: syscall crua (OvsbMk/libc mínima) */
    fd = (int)syscall((long)SYS_memfd_create, "ovsb-term-shm",
                      (unsigned long)MFD_CLOEXEC);
    if (fd >= 0)
        return fd;
#endif
    return -1;
}

shm_buffer_t *shm_buffer_create(struct wl_shm *shm, int width, int height)
{
    if (!shm || width <= 0 || height <= 0)
        return NULL;

    shm_buffer_t *buf = calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;

    int stride = width * 4;
    size_t size = (size_t)stride * (size_t)height;

    int fd = shm_anon_fd();
    if (fd < 0) {
        free(buf);
        return NULL;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        free(buf);
        return NULL;
    }
    void *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        free(buf);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    if (!pool) {
        munmap(px, size);
        close(fd);
        free(buf);
        return NULL;
    }
    struct wl_buffer *wlb = wl_shm_pool_create_buffer(
        pool, 0, (int32_t)width, (int32_t)height, (int32_t)stride,
        WL_SHM_FORMAT_ARGB8888);
    if (!wlb) {
        wl_shm_pool_destroy(pool);
        munmap(px, size);
        close(fd);
        free(buf);
        return NULL;
    }

    buf->pool = pool;
    buf->buffer = wlb;
    buf->pixels = (uint32_t *)px;
    buf->size = size;
    buf->fd = fd;
    buf->width = width;
    buf->height = height;
    buf->stride = stride;
    buf->busy = false;
    buf->valid = true;
    return buf;
}

void shm_buffer_destroy(shm_buffer_t *buf)
{
    if (!buf)
        return;
    if (buf->buffer) {
        wl_buffer_destroy(buf->buffer);
        buf->buffer = NULL;
    }
    if (buf->pixels && buf->size > 0) {
        munmap(buf->pixels, buf->size);
        buf->pixels = NULL;
    }
    if (buf->fd >= 0) {
        close(buf->fd);
        buf->fd = -1;
    }
    if (buf->pool) {
        wl_shm_pool_destroy(buf->pool);
        buf->pool = NULL;
    }
    buf->valid = false;
    buf->size = 0;
    free(buf);
}
