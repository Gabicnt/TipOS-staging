/* moe moe kyun <3 */
/* ~~ VFS mínima do TipOS ~ camada fina entre syscalls e backends ~~
 * Fase 1 da #70: dispatch por backend + caminhos ABSOLUTOS.
 * Fase 2 (com o ext2 write): vnodes de verdade e cwd por processo~ */

#ifndef VFS_H
#define VFS_H
#include <stdint.h>

#define VFS_MAX_PATH 512

#define VFS_EPERM          1
#define VFS_ENOENT         2
#define VFS_EIO            5
#define VFS_EBADF          9
#define VFS_ENOMEM        12
#define VFS_EACCES        13
#define VFS_EFAULT        14
#define VFS_EEXIST        17
#define VFS_ENODEV        19
#define VFS_ENOTDIR       20
#define VFS_EISDIR        21
#define VFS_EINVAL        22
#define VFS_ENOSPC        28
#define VFS_EROFS         30
#define VFS_ENAMETOOLONG  36
#define VFS_ENOTEMPTY     39

typedef struct {
	uint64_t ino;
	uint64_t next_cookie;
	uint8_t type;
	uint16_t name_len;
	char name[256];
} vfs_dirent_t;

/* ~~ monta o backend: ext2 se der, senão fat32 continua~~
 * Chamar UMA vez no boot depois do ata_init. Retorna o backend ativo:
 * 0 = ext2 · 1 = fat32 · -1 = nenhum montou (chora)~ */
int vfs_mount(void);

/* Resolve e normaliza um caminho absoluto/relativo ao cwd. Retorna 0 ou -errno. */
int vfs_abs_path(const char *cwd, const char *path, char *out, int outlen);

/* Operações comuns; erros são retornados como -errno. */
int vfs_read_file(const char *abs, uint8_t *buf, uint32_t count);
int vfs_read_at(const char *abs, uint8_t *buf, uint32_t count, uint64_t offset);
int vfs_write_at(const char *abs, const uint8_t *buf, uint32_t count, uint64_t offset);
int vfs_stat_size_attr(const char *abs, uint32_t *size, uint8_t *attr);
int vfs_readdir(const char *abs_dir, uint64_t cookie,
				vfs_dirent_t *entries, uint32_t capacity);
int vfs_create_file(const char *abs);
int vfs_unlink(const char *abs);
int vfs_mkdir(const char *abs);
int vfs_rmdir(const char *abs);
int vfs_chdir_isdir(const char *abs); /* 1 se é diretório navegável */
int vfs_truncate(const char *abs, uint32_t new_size);
int vfs_fsync(const char *abs);

#endif /* VFS_H */
