/* moe moe kyun <3 */
/* ~~ vfs.c ~ a fundação da #70 ~~
 * Backend único na raiz (ext2 preferido, fat32 de fallback) + resolução
 * de caminhos absolutos/relativos. Os handlers de syscall chamam SÓ
 * essas funções — nenhum deles sabe mais qual FS tá por trás~ ☆ */

#include "vfs.h"
#include "../kernel/serial.h"
#include "../kernel/drivers/tty.h"
#include <stdint.h>
#include <stdbool.h>
#define NULL ((void *)0)

/* ~~ backends ~~
 * FAT32: fs/fat32.c (C) — legacy, 8.3+LFN-read, cwd global (só leitura aqui)
 * EXT2:  fs/ext2.zig (Zig) — POSIX tree, caminhos absolutos nativos~ */
extern int fat32_read_file(const char *name, uint8_t *buffer, uint32_t size);
extern int fat32_read_at(const char *name, uint8_t *buffer, uint32_t size, uint64_t offset);
extern int fat32_write_at(const char *name, const uint8_t *buffer, uint32_t size, uint64_t offset);
extern int fat32_init(void);
extern int fat32_stat(const char *name, uint32_t *size, uint8_t *attr,
                      uint16_t *mtime, uint16_t *mdate);
extern int fat32_write_file(const char *name, const uint8_t *buffer, uint32_t size);
extern int fat32_create_file(const char *name);
extern int fat32_delete_file(const char *name);
extern int fat32_mkdir(const char *name);
extern int fat32_readdir(const char *path, uint64_t cookie,
                         void *entries, uint32_t capacity);

extern int ext2new_mount(void);
extern int ext2new_read_file(const char *path, unsigned char *buffer, unsigned int size);
extern int ext2new_stat(const char *path, unsigned int *size, bool *is_dir);
extern int ext2new_read_at(const char *path, unsigned char *buffer, unsigned int size, uint64_t offset);
extern void ext2new_sync(void);
extern int ext2_create_file(const char *path);
extern int ext2new_write_file(const char *path, const unsigned char *buffer,
                              unsigned int size, uint64_t offset);
extern int ext2new_readdir(const char *path, uint64_t cookie,
                           vfs_dirent_t *entries, uint32_t capacity);

int g_vfs_backend = -1; /* 0=ext2 · 1=fat32 · -1=nada */

static int backend_error(int rc) {
    if (rc >= 0) return rc;
    if (g_vfs_backend == 0) {
        switch (-rc) {
            case 2: return -VFS_ENOENT;
            case 5: return -VFS_EIO;
            case 20: return -VFS_ENOTDIR;
            case 28: return -VFS_ENOSPC;
            case 100: return -VFS_ENODEV;
            default: return -VFS_EIO;
        }
    }
    switch (-rc) {
        case 1: return -VFS_ENOENT;
        case 2: return -VFS_EEXIST;
        case 3: return -VFS_EIO;
        case 4: return -VFS_ENOSPC;
        case 5: return -VFS_ENOTDIR;
        case 6: return -VFS_ENOTEMPTY;
        default: return -VFS_EIO;
    }
}

int vfs_mount(void) {
    if (ext2new_mount() == 0) {
        g_vfs_backend = 0;
        serial_puts("[VFS] backend = EXT2\r\n");
    } else {
        if (fat32_init() != 0) {
            g_vfs_backend = -1;
            serial_puts("[VFS] FAT32 mount failed\r\n");
            return -1;
        }
        g_vfs_backend = 1;
        serial_puts("[VFS] backend = FAT32\r\n");
    }
    return g_vfs_backend;
}

/* ~~ vfs_abs_path ~ junta cwd+path e normaliza / e .. ~~
 * Regras: path absoluto substitui; relativo concatena; ".." sobe um~
 * Não usa libc — freestanding, tudo na mão mesmo~ kyun! */
int vfs_abs_path(const char *cwd, const char *path, char *out, int outlen) {
    /* ~~ debug fase 7: qual processo ta abrindo? (pid = campo 0 do
     * PCB) — pra rastrear o "segundo Xorg" que sobe sem execve ~~ */
    {
        extern void *process_current(void);
        extern void serial_puthex(uint32_t v);
        int *pcb = (int *)process_current();
        serial_puts("[vfs_abs] pid=");
        serial_puthex(pcb ? (uint32_t)pcb[0] : 0xFFFFFFFF);
    }
    serial_puts(" cwd='");
    serial_puts(cwd ? cwd : "(null)");
    serial_puts("' path='");
    serial_puts(path ? path : "(null)");
    serial_puts("'\r\n");
    if (!path || !out) return -VFS_EFAULT;
    if (!path[0]) return -VFS_ENOENT;
    if (outlen < 2) return -VFS_EINVAL;

    char base[VFS_MAX_PATH];
    int base_len = 0;
    if (path[0] != '/') {
        const char *base_cwd = cwd ? cwd : "/";
        if (base_cwd[0] != '/') return -VFS_EINVAL;
        while (base_cwd[base_len]) {
            if (base_len >= VFS_MAX_PATH - 1) return -VFS_ENAMETOOLONG;
            base[base_len] = base_cwd[base_len];
            base_len++;
        }
        if (base_len == 0 || base[base_len - 1] != '/') {
            if (base_len >= VFS_MAX_PATH - 1) return -VFS_ENAMETOOLONG;
            base[base_len++] = '/';
        }
    }
    for (int i = 0; path[i]; i++) {
        if (base_len >= VFS_MAX_PATH - 1) return -VFS_ENAMETOOLONG;
        base[base_len++] = path[i];
    }
    base[base_len] = '\0';

    int component_starts[VFS_MAX_PATH / 2];
    int component_count = 0;
    int pos = 1;
    out[0] = '/';
    for (int i = 0; i <= base_len;) {
        while (base[i] == '/') i++;
        if (i >= base_len) break;
        int start = i;
        while (base[i] && base[i] != '/') i++;
        int component_len = i - start;

        if (component_len == 1 && base[start] == '.') continue;
        if (component_len == 2 && base[start] == '.' && base[start + 1] == '.') {
            if (component_count > 0) pos = component_starts[--component_count];
            continue;
        }
        if (component_len > 255) return -VFS_ENAMETOOLONG;
        if (component_count >= (int)(sizeof(component_starts) / sizeof(component_starts[0])))
            return -VFS_ENAMETOOLONG;
        int component_start = pos;
        if (pos > 1) {
            if (pos >= outlen - 1) return -VFS_ENAMETOOLONG;
            out[pos++] = '/';
        }
        if (component_len > outlen - pos - 1) return -VFS_ENAMETOOLONG;
        component_starts[component_count++] = component_start;
        for (int j = 0; j < component_len; j++) out[pos++] = base[start + j];
    }
    out[pos] = '\0';
    return 0;
}

/* ~~ dispatch genérico~~ */
int vfs_read_file(const char *abs, uint8_t *buf, uint32_t count) {
    return vfs_read_at(abs, buf, count, 0);
}

int vfs_read_at(const char *abs, uint8_t *buf, uint32_t count, uint64_t offset) {
    if (!abs || (count && !buf)) return -VFS_EFAULT;
    if (g_vfs_backend < 0) return -VFS_ENODEV;
    {
        static int kml = 0;
        if (kml < 10 && abs) {
            int has = 0;
            for (const char *q = abs; q[0]; q++)
                if (q[0]=='x' && q[1]=='k' && q[2]=='m') { has = 1; break; }
            if (has) {
                kml++;
                serial_puts("[vfsrd] ");
                serial_puts(abs);
                serial_puts(" off=");
                serial_puthex(offset);
                serial_puts(" cnt=");
                serial_puthex(count);
                serial_puts("\r\n");
            }
        }
    }
    if (g_vfs_backend == 0)
        return backend_error(ext2new_read_at(abs, buf, count, offset));
    return backend_error(fat32_read_at(abs, buf, count, offset));
}

int vfs_stat_size_attr(const char *abs, uint32_t *size, uint8_t *attr) {
    if (!abs) return -VFS_EFAULT;
    /* FASE5: a raiz "/" não tem entrada de diretório no FAT32
     * (fat32_stat procura "" e volta NOTFOUND) → chdir("/") falhava
     * e o fork-pty do foot morria antes do exec. Atalho aqui vale
     * pros dois backends (fat32.c é intocável). */
    if (abs[0] == '/' && abs[1] == '\0') {
        if (size) *size = 0;
        if (attr) *attr = 0x10;
        return 0;
    }
    if ((abs[0] == '/' && abs[1] == 'd' && abs[2] == 'e' && abs[3] == 'v' &&
         abs[4] == '\0') ||
        (abs[0] == '/' && abs[1] == 'd' && abs[2] == 'e' && abs[3] == 'v' &&
         abs[4] == '/' && abs[5] == 'p' && abs[6] == 't' && abs[7] == 's' &&
         abs[8] == '\0')) {
        if (size) *size = 4096;
        if (attr) *attr = 0x10;
        return 0;
    }
    if (g_vfs_backend < 0) return -VFS_ENODEV;
    if (g_vfs_backend == 0) {
        unsigned int sz = 0;
        bool is_dir = false;
        int rc = backend_error(ext2new_stat(abs, &sz, &is_dir));
        if (rc < 0) return rc;
        if (size) *size = sz;
        if (attr) *attr = is_dir ? 0x10 : 0x20;
        return 0;
    }
    return backend_error(fat32_stat(abs, size, attr, NULL, NULL));
}

int vfs_readdir(const char *abs_dir, uint64_t cookie,
                vfs_dirent_t *entries, uint32_t capacity) {
    if (!abs_dir || !entries || capacity == 0) return -VFS_EFAULT;
    if (abs_dir[0] == '/' && abs_dir[1] == 'd' && abs_dir[2] == 'e' &&
        abs_dir[3] == 'v' && abs_dir[4] == '/' && abs_dir[5] == 'p' &&
        abs_dir[6] == 't' && abs_dir[7] == 's' && abs_dir[8] == '\0')
        return tty_pair_readdir(cookie, entries, capacity);
    if (g_vfs_backend < 0) return -VFS_ENODEV;
    if (g_vfs_backend == 0)
        return backend_error(ext2new_readdir(abs_dir, cookie, entries, capacity));
    return backend_error(fat32_readdir(abs_dir, cookie, entries, capacity));
}

int vfs_write_at(const char *abs, const uint8_t *buf, uint32_t count, uint64_t offset) {
    if (!abs || (count && !buf)) return -VFS_EFAULT;
    if (g_vfs_backend < 0) return -VFS_ENODEV;
    if (g_vfs_backend == 0)
        return backend_error(ext2new_write_file(abs, buf, count, offset));
    return backend_error(fat32_write_at(abs, buf, count, offset));
}

int vfs_create_file(const char *abs) {
    if (!abs) return -VFS_EFAULT;
    if (g_vfs_backend < 0) return -VFS_ENODEV;
    if (g_vfs_backend == 0) return backend_error(ext2_create_file(abs));
    return backend_error(fat32_create_file(abs));
}

int vfs_unlink(const char *abs) {
    if (!abs) return -VFS_EFAULT;
    if (g_vfs_backend < 0) return -VFS_ENODEV;
    if (g_vfs_backend == 0) return -VFS_EROFS;
    extern int fat32_delete_file(const char *name);
    return backend_error(fat32_delete_file(abs));
}

int vfs_mkdir(const char *abs) {
    if (!abs) return -VFS_EFAULT;
    if (g_vfs_backend < 0) return -VFS_ENODEV;
    if (g_vfs_backend == 0) return -VFS_EROFS;
    extern int fat32_mkdir(const char *name);
    return backend_error(fat32_mkdir(abs));
}

int vfs_rmdir(const char *abs) {
    if (!abs) return -VFS_EFAULT;
    if (g_vfs_backend < 0) return -VFS_ENODEV;
    if (g_vfs_backend == 0) return -VFS_EROFS;
    extern int fat32_rmdir(const char *name);
    return backend_error(fat32_rmdir(abs));
}

int vfs_chdir_isdir(const char *abs) {
    uint32_t sz = 0; uint8_t at = 0;
    if (vfs_stat_size_attr(abs, &sz, &at) != 0) return 0;
    if (g_vfs_backend == 0) return 1; /* ext2 stat só dá size hoje — assume dir navegável */
    return (at & 0x10) ? 1 : 0; /* attr FAT32 bit diretório */
}

int vfs_truncate(const char *abs, uint32_t new_size) {
    if (!abs) return -VFS_EFAULT;
    if (g_vfs_backend < 0) return -VFS_ENODEV;
    if (g_vfs_backend == 0) return -VFS_EROFS; /* ext2 write não suporta truncate ainda */
    extern int fat32_truncate(const char *name, uint32_t new_size);
    return backend_error(fat32_truncate(abs, new_size));
}

int vfs_fsync(const char *abs) {
    (void)abs;
    if (g_vfs_backend < 0) return -VFS_ENODEV;
    /* FAT32 flusha no write/truncate; ext2 sem dirty ainda — no-op */
    return 0;
}
