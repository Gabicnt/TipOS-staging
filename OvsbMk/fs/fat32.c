/* moe moe kyun <3 */
/* moe moe kyun <3 */
/* ♥ filesystem ~ onde os byte moram organizadinho (ou n) ~
 * arquivo: fat32.c ~ funcoes anotadas: 26
 */
/* ♥ fat32.c ~ feito com carinho (e gambiarras) pela equipe TipOS! ♥
 * Se quebrar, a culpa é sua~ <3
 *~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*/

/* ♥ FAT32 ~ sistema de arquivos gordinho mas funcional! */
/* ♥ FAT32 - Driver de sistema de arquivos ~ "Gordão mas eficiente!"
 * Dica: FAT32 = File Allocation Table 32 bits~
 * A Tabela FAT guarda a cadeia de clusters de cada arquivo~
 * Se perder a FAT, perdeu tudo~ entao nao mexe nela! >_<
 * Cada entrada de diretorio tem 32 bytes com nome 8.3~
 * Nomes Longos (LFN) sao ignorados aqui~ preguiça~ kyun!
 *
 * Bugs corrigidos (da versao TipOS):
 * 1. name_to_83() nao respeitava terminador nulo~
 *    Nomes curtos tipo "X" corrompiam o buffer com lixo da pilha~
 * 2. read_chain/write_chain: divisao to_read/512 truncava <512 pra 0~
 *    Arquivo HELLO com 180 bytes era lido como 0 bytes~ hihi burro~
 * 3. create_entry_at() retornava -1 generico; agora FAT_ERR_IO/NOESPACE
 * 4. fat32_mkdir() tinha bytes nulos no lugar de espacos em '.' e '..'~ */

#include "fat32.h"
#include <stdint.h>
#include "../kernel/utils.h"
#include "../kernel/console.h"
#include "../drivers/ata.h"
#include "../kernel/memory.h"
#include "../kernel/serial.h"

/* Cores pro list_dir */
#define C_DIR  0xFF60FF60
#define C_FILE 0xFFC0C0C0
#define C_OUT 0xFFC0C0C0

static fat32_boot_t boot;
static uint32_t first_data_sector;
static uint32_t sectors_per_cluster;
static uint32_t current_dir_cluster;
static uint32_t fat_start_sector;
static uint32_t fat_cache_sector = 0xFFFFFFFF;
static uint8_t fat_cache[512];
#define FAT_READ_CACHE_CLUSTERS 65536
static uint32_t read_cache_clusters[FAT_READ_CACHE_CLUSTERS];
static char read_cache_path[256];
static uint32_t read_cache_first_cluster;
static uint32_t read_cache_file_size;
static uint32_t read_cache_count;
static volatile int fat32_path_lock;

static void fat32_path_lock_acquire(void) {
    while (__sync_lock_test_and_set(&fat32_path_lock, 1))
        __asm__ volatile("pause");
}

static void fat32_path_lock_release(void) {
    __sync_lock_release(&fat32_path_lock);
}

/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
static uint8_t name_to_83(const char *name, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    if (!name || !name[0]) return 0;
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
        int i = 0;
        while (name[i] && i < 11) { out[i] = name[i]; i++; }
        return 0;
    }
    const char *src = name;
    while (*src == '.') src++;
    if (!*src) {
        const char *p = name;
        while (*p && p - name < 11) {
            int index = (int)(p - name);
            out[index] = *p;
            p++;
        }
        return 0;
    }
    const char *dot = 0;
    const char *p = src;
    while (*p) {
        if (*p == '.') { dot = p; break; }
        p++;
    }
    int pos = 0;
    while (src != dot && *src && pos < 8) {
        char c = *src++;
        if (c >= 'a' && c <= 'z') c -= 0x20;
        out[pos++] = c;
    }
    if (dot) {
        dot++;
        pos = 8;
        while (*dot && pos < 11) {
            char c = *dot++;
            if (c >= 'a' && c <= 'z') c -= 0x20;
            out[pos++] = c;
        }
    }
    return 0;
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
static uint32_t cluster_to_sector(uint32_t cluster) {
    return first_data_sector + (cluster - 2) * sectors_per_cluster;
}

/* ~ cuidado que essa aqui morde ~ */
static uint32_t fat_read_entry(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_sector + (fat_offset / 512);
    uint32_t ent_offset = fat_offset % 512;
    if (fat_cache_sector != fat_sector) {
        if (ata_read_sector(fat_sector, fat_cache) != 0) {
            fat_cache_sector = 0xFFFFFFFF;
            return 0;
        }
        fat_cache_sector = fat_sector;
    }
    return *(uint32_t *)(fat_cache + ent_offset) & 0x0FFFFFFF;
}

/* ~ cuidado que essa aqui morde ~ */
static int fat_write_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_sector + (fat_offset / 512);
    uint32_t ent_offset = fat_offset % 512;
    uint8_t sector[512];
    if (ata_read_sector(fat_sector, sector) != 0) return -1;
    *(uint32_t *)(sector + ent_offset) =
        (*(uint32_t *)(sector + ent_offset) & 0xF0000000) | (value & 0x0FFFFFFF);
    if (ata_write_sector(fat_sector, sector) != 0) return -1;
    return 0;
}

/* ~ cuidado que essa aqui morde ~ */
static uint32_t fat_find_free(void) {
    uint32_t total_clusters =
        (boot.total_sectors_32 - first_data_sector) / sectors_per_cluster;
    for (uint32_t c = 2; c < total_clusters + 2; c++) {
        if (fat_read_entry(c) == 0) return c;
    }
    return 0;
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
static uint32_t fat_chain_last(uint32_t start) {
    uint32_t c = start;
    while (1) {
        uint32_t next = fat_read_entry(c);
        if (next >= 0x0FFFFFF8) return c;
        if (next == 0) return c;
        c = next;
    }
}

/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
static int read_chain(uint32_t start_cluster, uint8_t *buffer, uint32_t size) {
    uint32_t cluster = start_cluster;
    uint32_t offset = 0;
    uint32_t cluster_bytes = sectors_per_cluster * 512;

    while (cluster >= 2 && cluster < 0x0FFFFFF8 && offset < size) {
        uint32_t sector = cluster_to_sector(cluster);
        uint32_t to_read = size - offset;
        if (to_read > cluster_bytes) to_read = cluster_bytes;

        uint32_t num_sectors = to_read / 512;
        for (uint32_t s = 0; s < num_sectors; s++) {
            if (ata_read_sector(sector + s, buffer + offset + s * 512) != 0)
                return -1;
        }
        uint32_t remainder = to_read % 512;
        if (remainder > 0) {
            uint8_t tmp[512];
            if (ata_read_sector(sector + num_sectors, tmp) != 0) return -1;
            for (uint32_t b = 0; b < remainder; b++)
                buffer[offset + num_sectors * 512 + b] = tmp[b];
        }
        offset += to_read;
        cluster = fat_read_entry(cluster);
    }
    return offset;
}

/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
static int write_chain(uint32_t start_cluster, uint8_t *buffer, uint32_t size) {
    uint32_t cluster = start_cluster;
    uint32_t offset = 0;
    uint32_t cluster_bytes = sectors_per_cluster * 512;

    while (size > 0) {
        uint32_t sector = cluster_to_sector(cluster);
        uint32_t to_write = size < cluster_bytes ? size : cluster_bytes;
        uint8_t sector_buf[512];

        uint32_t num_sectors = to_write / 512;
        for (uint32_t s = 0; s < num_sectors; s++) {
            for (uint32_t b = 0; b < 512; b++)
                sector_buf[b] = buffer[offset + s * 512 + b];
            if (ata_write_sector(sector + s, sector_buf) != 0) return -1;
        }
        uint32_t remainder = to_write % 512;
        if (remainder > 0) {
            uint8_t tmp[512];
            if (ata_read_sector(sector + num_sectors, tmp) != 0) return -1;
            for (uint32_t b = 0; b < remainder; b++)
                tmp[b] = buffer[offset + num_sectors * 512 + b];
            if (ata_write_sector(sector + num_sectors, tmp) != 0) return -1;
        }
        offset += to_write;
        size -= to_write;

        if (size > 0) {
            uint32_t next = fat_read_entry(cluster);
            if (next == 0 || next >= 0x0FFFFFF8) {
                uint32_t free = fat_find_free();
                if (free == 0) return -1;
                if (fat_write_entry(cluster, free) != 0) return -1;
                if (fat_write_entry(free, 0x0FFFFFFF) != 0) return -1;
                cluster = free;
            } else {
                cluster = next;
            }
        }
    }
    return offset;
}

/* ~ cuidado que essa aqui morde ~ */
static int alloc_clusters(uint32_t start, uint32_t count) {
    uint32_t prev = start;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t free = fat_find_free();
        if (free == 0) return -1;
        if (fat_write_entry(prev, free) != 0) return -1;
        if (fat_write_entry(free, 0x0FFFFFFF) != 0) return -1;
        prev = free;
    }
    return 0;
}

/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
static uint32_t first_cluster_of(fat32_dir_entry_t *e) {
    return ((uint32_t)e->first_cluster_high << 16) | e->first_cluster_low;
}

/* ~~ LFN: compara nome longo montado com o pedido (case-insensitive) ~~ */
static int lfn_match(const char *lfn, int len, const char *name) {
    int i;
    for (i = 0; i < len; i++) {
        char lc = lfn[i], nc = name[i];
        if (lc >= 'a' && lc <= 'z') lc -= 0x20;
        if (nc >= 'a' && nc <= 'z') nc -= 0x20;
        if (nc == '\0' || lc != nc) return 0;
    }
    return name[len] == '\0';
}

static int find_entry(uint32_t dir_cluster, const char *name,
                      fat32_dir_entry_t *out) {
    uint8_t name83[11];
    name_to_83(name, name83);

    /* ~~ Long File Names: entradas attr 0x0F guardam o nome em UTF-16~~
     * 13 chars por entrada, em ordem DECRESCENTE antes da entrada curta~
     * Sem isso o kernel não enxerga fonts.dir/6x13.pcf.gz/etc~ */
    char lfn[264];
    int lfn_len = 0;
    int expect_seq = 0;
    static const int lofs[13] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t sector = cluster_to_sector(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            uint8_t buf[512];
            if (ata_read_sector(sector + s, buf) != 0) return -1;
            fat32_dir_entry_t *entries = (fat32_dir_entry_t *)buf;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == 0x00) return -1;
                if (entries[i].name[0] == 0xE5) { lfn_len = 0; continue; }
                if (entries[i].attr == 0x0F) {
                    int seq = entries[i].name[0];
                    if (seq & 0x40) { lfn_len = 0; expect_seq = seq & 0x3F; }
                    if (expect_seq >= 1 && (seq & 0x3F) == expect_seq &&
                        (expect_seq - 1) * 13 + 13 < (int)sizeof(lfn)) {
                        int pos = (expect_seq - 1) * 13;
                        const uint8_t *c = entries[i].name;
                        for (int k = 0; k < 13; k++) {
                            uint16_t u = c[lofs[k]] | (uint16_t)(c[lofs[k]+1] << 8);
                            if (u == 0x0000 || u == 0xFFFF) break;
                            lfn[pos + k] = (char)(u & 0xFF);
                            if (pos + k + 1 > lfn_len) lfn_len = pos + k + 1;
                        }
                        expect_seq--;
                    } else {
                        lfn_len = 0; expect_seq = 0;
                    }
                    continue;
                }
                /* entrada normal: tenta LFN montado, depois 8.3~ */
                if (lfn_len > 0 && lfn_match(lfn, lfn_len, name)) {
                    if (out) *out = entries[i];
                    return cluster_to_sector(cluster) + s;
                }
                /* ~~ DEBUG temporario: mostra busca vs nome real ~~ */
                {
                    extern void serial_puts(const char *);
                    static int dbg = 0;
                    if (dbg < 60 && (name[0]=='k'||name[0]=='K'||name[0]=='m'||name[0]=='M')) {
                        dbg++;
                        serial_puts("[FE] want='"); serial_puts(name);
                        serial_puts("' lfn='");
                        for (int z=0; z<lfn_len && z<40; z++) { char b[2]={lfn[z],0}; serial_puts(b); }
                        serial_puts("'\r\n");
                    }
                }
                lfn_len = 0;
                int match = 1;
                for (int j = 0; j < 11; j++) {
                    if (entries[i].name[j] != name83[j]) { match = 0; break; }
                }
                if (match) {
                    if (out) *out = entries[i];
                    return cluster_to_sector(cluster) + s;
                }
            }
        }
        cluster = fat_read_entry(cluster);
    }
    return -1;
}

/* ~ cuidado que essa aqui morde ~ */
static int list_dir_at(uint32_t dir_cluster) {
    uint32_t cluster = dir_cluster;
    int count = 0;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t sector = cluster_to_sector(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            uint8_t buf[512];
            if (ata_read_sector(sector + s, buf) != 0) return -1;
            fat32_dir_entry_t *entries = (fat32_dir_entry_t *)buf;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == 0x00) goto done;
                if (entries[i].name[0] == 0xE5) continue;
                if (entries[i].attr == 0x0F) continue;
                if (entries[i].attr & 0x10) console_set_fg(C_DIR);
                else console_set_fg(C_FILE);
                for (int j = 0; j < 11; j++) {
                    char c = entries[i].name[j];
                    if (c != ' ') console_putchar(c);
                }
                if (entries[i].attr & 0x10) console_putchar('/');
                console_set_fg(C_OUT);
                uint16_t mt = entries[i].modification_time;
                uint16_t md = entries[i].modification_date;
                if (md) {
                    int hh = (mt >> 11) & 0x1F;
                    int mm = (mt >> 5) & 0x3F;
                    int mo = (md >> 5) & 0x0F;
                    int dd = md & 0x1F;
                    console_putchar(' ');
                    console_putchar('0' + mo/10); console_putchar('0' + mo%10);
                    console_putchar('-');
                    console_putchar('0' + dd/10); console_putchar('0' + dd%10);
                    console_putchar(' ');
                    console_putchar('0' + hh/10); console_putchar('0' + hh%10);
                    console_putchar(':');
                    console_putchar('0' + mm/10); console_putchar('0' + mm%10);
                }
                console_putchar('\n');
                count++;
            }
        }
        cluster = fat_read_entry(cluster);
    }
done:
    return count;
}

/* ~~ fat32_match_prefix ~~ */
int fat32_match_prefix(const char *prefix, uint32_t dir_cluster,
                       fat32_dirent_t *entries, int max_entries) {
    int count = 0, plen = 0;
    while (prefix[plen]) plen++;
    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < 0x0FFFFFF8 && count < max_entries) {
        uint32_t sector = cluster_to_sector(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster && count < max_entries; s++) {
            uint8_t buf[512];
            if (ata_read_sector(sector + s, buf) != 0) return count;
            fat32_dir_entry_t *e = (fat32_dir_entry_t *)buf;
            for (int i = 0; i < 16 && count < max_entries; i++) {
                if (e[i].name[0] == 0x00) return count;
                if (e[i].name[0] == 0xE5) continue;
                if (e[i].attr == 0x0F) continue;
                char name[64]; int len = 0;
                for (int j = 0; j < 8 && e[i].name[j] != ' '; j++) name[len++] = e[i].name[j];
                int has_ext = 0;
                for (int j = 8; j < 11; j++) if (e[i].name[j] != ' ') has_ext = 1;
                if (has_ext && len > 0) {
                    name[len++] = '.';
                    for (int j = 8; j < 11 && e[i].name[j] != ' '; j++) name[len++] = e[i].name[j];
                }
                name[len] = '\0';
                if (plen == 0 || strncmp(name, prefix, plen) == 0) {
                    int k; for (k = 0; k < len && k < 63; k++) entries[count].name[k] = name[k];
                    entries[count].name[k] = '\0';
                    entries[count].attr = e[i].attr;
                    count++;
                }
            }
        }
        cluster = fat_read_entry(cluster);
    }
    return count;
}

static int fat32_readdir_unlocked(const char *path, uint64_t cookie,
                                  fat32_vfs_dirent_t *entries, uint32_t capacity) {
    if (!path || !entries || capacity == 0) return FAT_ERR_IO;
    uint32_t saved = current_dir_cluster;
    char directory_path[512];
    int path_length = 0;
    while (path[path_length] && path_length < (int)sizeof(directory_path) - 1) {
        directory_path[path_length] = path[path_length];
        path_length++;
    }
    if (path[path_length]) return FAT_ERR_NOTFOUND;
    directory_path[path_length] = '\0';
    if (directory_path[0] == '/') current_dir_cluster = boot.root_cluster;
    char *component = directory_path;
    while (*component == '/') component++;
    while (*component) {
        char *next = component;
        while (*next && *next != '/') next++;
        char separator = *next;
        *next = '\0';
        if (component[0] && fat32_change_dir(component) < 0) {
            serial_puts("[FAT-READDIR-FAIL] path=");
            serial_puts(path);
            serial_puts(" component=");
            serial_puts(component);
            serial_puts(" cwd=");
            serial_puthex(current_dir_cluster);
            serial_puts("\r\n");
            current_dir_cluster = saved;
            return FAT_ERR_NOTFOUND;
        }
        if (!separator) break;
        component = next + 1;
        while (*component == '/') component++;
    }

    uint32_t ordinal = 0;
    uint32_t count = 0;
    uint32_t cluster = current_dir_cluster;
    char long_name[264];
    int long_name_len = 0;
    int expected_lfn_sequence = 0;
    static const int lfn_offsets[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    while (cluster >= 2 && cluster < 0x0FFFFFF8 && count < capacity) {
        uint32_t sector = cluster_to_sector(cluster);
        for (uint32_t sector_index = 0;
             sector_index < sectors_per_cluster && count < capacity;
             sector_index++) {
            uint8_t buffer[512];
            if (ata_read_sector(sector + sector_index, buffer) != 0) {
                current_dir_cluster = saved;
                return FAT_ERR_IO;
            }
            fat32_dir_entry_t *dir = (fat32_dir_entry_t *)buffer;
            for (int entry_index = 0; entry_index < 16 && count < capacity;
                 entry_index++, ordinal++) {
                fat32_dir_entry_t *entry = &dir[entry_index];
                if (entry->name[0] == 0x00) {
                    current_dir_cluster = saved;
                    return (int)count;
                }
                if (entry->name[0] == 0xE5) {
                    long_name_len = 0;
                    expected_lfn_sequence = 0;
                    continue;
                }
                if (entry->attr == 0x0F) {
                    int sequence = entry->name[0];
                    if (sequence & 0x40) {
                        long_name_len = 0;
                        expected_lfn_sequence = sequence & 0x3F;
                    }
                    if (expected_lfn_sequence >= 1 &&
                        (sequence & 0x3F) == expected_lfn_sequence &&
                        (expected_lfn_sequence - 1) * 13 + 13 < (int)sizeof(long_name)) {
                        int base = (expected_lfn_sequence - 1) * 13;
                        const uint8_t *chars = entry->name;
                        for (int char_index = 0; char_index < 13; char_index++) {
                            uint16_t value = chars[lfn_offsets[char_index]] |
                                (uint16_t)(chars[lfn_offsets[char_index] + 1] << 8);
                            if (value == 0 || value == 0xFFFF) break;
                            long_name[base + char_index] = (char)value;
                            if (base + char_index + 1 > long_name_len)
                                long_name_len = base + char_index + 1;
                        }
                        expected_lfn_sequence--;
                    } else {
                        long_name_len = 0;
                        expected_lfn_sequence = 0;
                    }
                    continue;
                }
                if (ordinal < cookie) {
                    long_name_len = 0;
                    expected_lfn_sequence = 0;
                    continue;
                }

                fat32_vfs_dirent_t *out = &entries[count];
                int length = 0;
                if (long_name_len > 0) {
                    for (; length < long_name_len && length < (int)sizeof(out->name) - 1;
                         length++)
                        out->name[length] = long_name[length];
                } else {
                    /* ~~ sem LFN: emite o 8.3 em minusculas, como o Xorg/Unix
                     * espera (ex: KBD_DRV.SO -> kbd_drv.so) ~~ */
                    for (int i = 0; i < 8 && entry->name[i] != ' '; i++) {
                        char c = (char)entry->name[i];
                        if (c >= 'A' && c <= 'Z') c += 0x20;
                        out->name[length++] = c;
                    }
                    int has_extension = 0;
                    for (int i = 8; i < 11; i++)
                        if (entry->name[i] != ' ') has_extension = 1;
                    if (has_extension && length > 0) out->name[length++] = '.';
                    if (has_extension) {
                        for (int i = 8; i < 11 && entry->name[i] != ' '; i++) {
                            char c = (char)entry->name[i];
                            if (c >= 'A' && c <= 'Z') c += 0x20;
                            out->name[length++] = c;
                        }
                    }
                }
                long_name_len = 0;
                expected_lfn_sequence = 0;
                if (length == 0) continue;
                out->name[length] = '\0';
                out->name_len = (uint16_t)length;
                out->type = (entry->attr & 0x10) ? 4 : 8;
                out->ino = ((uint64_t)current_dir_cluster << 32) |
                           (uint64_t)(ordinal + 1);
                out->next_cookie = (uint64_t)ordinal + 1;
                count++;
            }
        }
        cluster = fat_read_entry(cluster);
    }
    current_dir_cluster = saved;
    return (int)count;
}

static int create_entry_at(uint32_t dir_cluster, const char *name,
                           uint8_t attr) {
    uint8_t name83[11];
    name_to_83(name, name83);

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t sector_num = cluster_to_sector(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            uint8_t buf[512];
            if (ata_read_sector(sector_num + s, buf) != 0) return FAT_ERR_IO;
            fat32_dir_entry_t *entries = (fat32_dir_entry_t *)buf;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == 0x00 ||
                    entries[i].name[0] == 0xE5) {
                    for (int j = 0; j < 11; j++)
                        entries[i].name[j] = name83[j];
                    entries[i].attr = attr;
                    entries[i].size = 0;
                    entries[i].first_cluster_low = 0;
                    entries[i].first_cluster_high = 0;
                    if (ata_write_sector(sector_num + s, buf) != 0)
                        return FAT_ERR_IO;
                    return 0;
                }
            }
        }
        cluster = fat_read_entry(cluster);
    }
    return FAT_ERR_NOSPACE;
}

/* ~ essa demorou pra debugar, respeita ~ */
static int delete_entry_at(uint32_t dir_cluster, const char *name) {
    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t sector_num = cluster_to_sector(cluster);
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            uint8_t buf[512];
            if (ata_read_sector(sector_num + s, buf) != 0) return -1;
            fat32_dir_entry_t *entries = (fat32_dir_entry_t *)buf;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == 0x00) return -1;
                if (entries[i].name[0] == 0xE5) continue;
                if (entries[i].attr == 0x0F) continue;
                uint8_t name83[11];
                name_to_83(name, name83);
                int match = 1;
                for (int j = 0; j < 11; j++) {
                    if (entries[i].name[j] != name83[j]) { match = 0; break; }
                }
                if (match) {
                    entries[i].name[0] = 0xE5;
                    if (ata_write_sector(sector_num + s, buf) != 0) return -1;
                    return 0;
                }
            }
        }
        cluster = fat_read_entry(cluster);
    }
    return -1;
}

/* ~~ Inicializando~~ torce pra não dar panic! */
/* ~ essa demorou pra debugar, respeita ~ */
int fat32_init(void) {
    uint8_t sector[512];
    if (ata_read_sector(0, sector) != 0) return -1;
    for (int i = 0; i < sizeof(fat32_boot_t); i++)
        ((uint8_t *)&boot)[i] = sector[i];
    if (boot.boot_sector_signature != 0xAA55) return -1;

    sectors_per_cluster = boot.sectors_per_cluster;
    first_data_sector =
        boot.reserved_sectors + boot.num_fats * boot.sectors_per_fat_32;
    fat_start_sector = boot.reserved_sectors;

    current_dir_cluster = boot.root_cluster;
    return 0;
}

/* ~~ Criando coisa nova~~ que emocionante! */
/* ~ essa demorou pra debugar, respeita ~ */
int fat32_create_file(const char *name) {
    int result = create_entry_at(current_dir_cluster, name, 0x20);
    serial_puts("[fat32_create_file] name='");
    serial_puts(name ? name : "(null)");
    serial_puts("' ret=");
    serial_puthex((uint32_t)result);
    serial_puts("\r\n");
    return result;
}

/* ~~ Deletando~~ vou sentir saudades~ brinks! */
/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
int fat32_delete_file(const char *name) {
    return delete_entry_at(current_dir_cluster, name);
}

/* ~~ Lendo coisinhas~ toma cuidado pra não ler lixo! */
/* ~ essa demorou pra debugar, respeita ~ */
/* ~~ fat32_walk ~ resolve caminho COM diretórios (absoluto ou relativo) ~~
 * Deixa o cwd no dir final e devolve o nome-base em out.
 * O caller DEVE restaurar current_dir_cluster depois (salve antes!)~
 * Sem isso "/share/X11/xkb/rules/evdev" era procurado DENTRO de /BIN! */
static int fat32_walk(const char *path, char *out, int outlen) {
    if (!path || !path[0]) return -1;
    char tmp[512];
    int ti = 0;
    while (path[ti] && ti < 511) { tmp[ti] = path[ti]; ti++; }
    tmp[ti] = '\0';
    char *cur = tmp;
    if (*cur == '/') { current_dir_cluster = boot.root_cluster; cur++; }
    char *last = 0, *s = cur;
    while (*s) { if (*s == '/') last = s; s++; }
    char *file = cur;
    if (last) {
        *last = '\0';
        file = last + 1;
        /* ~~ caminha componente a componente (cada um é uma entrada!) ~~ */
        char *w = cur;
        while (w && *w) {
            char *n = w;
            while (*n && *n != '/') n++;
            int end = (*n == '\0');
            if (*n) *n = '\0';
            if (w[0] && !(w[0] == '.' && w[1] == '\0')) {
                if (fat32_change_dir(w) < 0) return -1;
            }
            if (end) break;
            w = n + 1;
        }
    }
    if (!file[0]) return -1;
    int j = 0;
    while (file[j] && j < outlen - 1) { out[j] = file[j]; j++; }
    out[j] = '\0';
    return 0;
}

int fat32_read_file(const char *name, uint8_t *buffer, uint32_t size) {
    fat32_dir_entry_t entry;
    uint32_t saved = current_dir_cluster;
    char base[256];
    if (fat32_walk(name, base, sizeof(base)) < 0) {
        current_dir_cluster = saved; return -1;
    }
    int r = find_entry(current_dir_cluster, base, &entry);
    current_dir_cluster = saved;
    if (r < 0) return -1;
    uint32_t cluster = first_cluster_of(&entry);
    if (cluster == 0) return 0;
    uint32_t to_read = size < entry.size ? size : entry.size;
    return read_chain(cluster, buffer, to_read);
}

static void fat_read_at_error(const char *stage, uint64_t offset, uint32_t size,
                              uint32_t index, uint32_t count, uint32_t value) {
    static uint32_t logged_errors;
    if (logged_errors >= 8) return;
    logged_errors++;
    serial_puts("[FAT-RDFAIL] "); serial_puts(stage);
    serial_puts(" off="); serial_puthex(offset);
    serial_puts(" size="); serial_puthex(size);
    serial_puts(" idx="); serial_puthex(index);
    serial_puts(" count="); serial_puthex(count);
    serial_puts(" val="); serial_puthex(value);
    serial_puts("\r\n");
}

static int fat32_read_at_unlocked(const char *name, uint8_t *buffer, uint32_t size,
                                  uint64_t offset) {
    fat32_dir_entry_t entry;
    uint32_t saved = current_dir_cluster;
    char base[256];
    if (fat32_walk(name, base, sizeof(base)) < 0) {
        current_dir_cluster = saved;
        fat_read_at_error("walk", offset, size, 0, 0, 0);
        return -1;
    }
    int found = find_entry(current_dir_cluster, base, &entry);
    current_dir_cluster = saved;
    if (found < 0) {
        fat_read_at_error("find", offset, size, 0, 0, 0);
        return -1;
    }
    if (offset > 0xFFFFFFFFULL) return FAT_ERR_IO;
    if (offset >= entry.size || size == 0) return 0;
    uint32_t offset32 = (uint32_t)offset;
    if (size > entry.size - offset32) size = entry.size - offset32;

    uint32_t cluster_bytes = sectors_per_cluster * 512;
    uint32_t first_cluster = first_cluster_of(&entry);
    int cache_hit = read_cache_count > 0 &&
                    read_cache_first_cluster == first_cluster &&
                    read_cache_file_size == entry.size;
    if (cache_hit) {
        uint32_t i = 0;
        while (name[i] && read_cache_path[i] == name[i]) i++;
        cache_hit = name[i] == read_cache_path[i];
    }
    if (!cache_hit) {
        int path_i = 0;
        while (name[path_i] && path_i < (int)sizeof(read_cache_path) - 1) {
            read_cache_path[path_i] = name[path_i];
            path_i++;
        }
        read_cache_path[path_i] = '\0';
        read_cache_first_cluster = first_cluster;
        read_cache_file_size = entry.size;
        read_cache_count = 0;
        uint32_t needed = (entry.size + cluster_bytes - 1) / cluster_bytes;
        if (needed > FAT_READ_CACHE_CLUSTERS) {
            fat_read_at_error("cache-cap", offset, size, 0, needed, first_cluster);
            return -1;
        }
        uint32_t cluster = first_cluster;
        while (read_cache_count < needed) {
            if (cluster < 2 || cluster >= 0x0FFFFFF8) {
                fat_read_at_error("bad-chain", offset, size, read_cache_count, needed, cluster);
                read_cache_count = 0;
                return -1;
            }
            read_cache_clusters[read_cache_count++] = cluster;
            if (read_cache_count < needed) {
                uint32_t previous = cluster;
                cluster = fat_read_entry(previous);
                if (cluster == 0) {
                    serial_puts("[FAT-CHAIN] prev=");
                    serial_puthex(previous);
                    serial_puts(" fatsec=");
                    serial_puthex(fat_start_sector + (previous * 4) / 512);
                    serial_puts(" entoff=");
                    serial_puthex((previous * 4) % 512);
                    serial_puts("\r\n");
                }
            }
        }
    }

    uint32_t cluster_index = offset32 / cluster_bytes;
    uint32_t in_cluster = offset32 % cluster_bytes;
    if (cluster_index >= read_cache_count) {
        fat_read_at_error("index", offset, size, cluster_index, read_cache_count, entry.size);
        return -1;
    }
    uint32_t cluster = read_cache_clusters[cluster_index];

    uint32_t copied = 0;
    while (copied < size && cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t sector = cluster_to_sector(cluster) + in_cluster / 512;
        uint32_t sector_offset = in_cluster % 512;
        uint32_t sectors_left = sectors_per_cluster - in_cluster / 512;
        while (sectors_left-- > 0 && copied < size) {
            uint32_t amount = 512 - sector_offset;
            if (amount > size - copied) amount = size - copied;
            if (sector_offset == 0 && amount == 512) {
                if (ata_read_sector(sector, buffer + copied) != 0) {
                    fat_read_at_error("ata-full", offset, size, cluster_index, copied, sector);
                    return -1;
                }
            } else {
                uint8_t sector_buffer[512];
                if (ata_read_sector(sector, sector_buffer) != 0) {
                    fat_read_at_error("ata-part", offset, size, cluster_index, copied, sector);
                    return -1;
                }
                for (uint32_t i = 0; i < amount; i++)
                    buffer[copied + i] = sector_buffer[sector_offset + i];
            }
            copied += amount;
            sector++;
            sector_offset = 0;
        }
        if (copied < size) {
            cluster_index++;
            if (cluster_index >= read_cache_count) {
                fat_read_at_error("short-chain", offset, size, cluster_index, read_cache_count, entry.size);
                return -1;
            }
            cluster = read_cache_clusters[cluster_index];
        }
        in_cluster = 0;
    }
    return (int)copied;
}

static int fat32_write_at_unlocked(const char *name, const uint8_t *buffer,
                                   uint32_t size, uint64_t offset) {
    if (!name || (size && !buffer) || offset > 0xFFFFFFFFULL)
        return FAT_ERR_IO;
    uint32_t offset32 = (uint32_t)offset;
    fat32_dir_entry_t entry;
    uint32_t saved = current_dir_cluster;
    char base[256];
    if (fat32_walk(name, base, sizeof(base)) < 0) {
        current_dir_cluster = saved;
        return FAT_ERR_NOTFOUND;
    }
    if (find_entry(current_dir_cluster, base, &entry) < 0) {
        current_dir_cluster = saved;
        return FAT_ERR_NOTFOUND;
    }
    if (entry.attr & 0x10 || size > 0xFFFFFFFFU - offset32) {
        current_dir_cluster = saved;
        return FAT_ERR_IO;
    }
    uint32_t old_size = entry.size;
    uint32_t new_size = old_size > offset32 ? old_size : offset32;
    if (size > 0xFFFFFFFFU - new_size) {
        current_dir_cluster = saved;
        return FAT_ERR_NOSPACE;
    }
    new_size += size;
    uint8_t *combined = (uint8_t *)kmalloc(new_size ? new_size : 1);
    if (!combined) {
        current_dir_cluster = saved;
        return FAT_ERR_NOSPACE;
    }
    for (uint32_t index = 0; index < new_size; index++) combined[index] = 0;
    uint32_t old_cluster = first_cluster_of(&entry);
    if (old_size && old_cluster && read_chain(old_cluster, combined, old_size) < 0) {
        kfree(combined);
        current_dir_cluster = saved;
        return FAT_ERR_IO;
    }
    for (uint32_t index = 0; index < size; index++)
        combined[offset32 + index] = buffer[index];
    int result = fat32_write_file(base, combined, new_size);
    kfree(combined);
    current_dir_cluster = saved;
    return result < 0 ? FAT_ERR_IO : (int)size;
}

/* ~~ Escrevendo~~ não vai corromper nada, vai? >_< */
/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
int fat32_write_file(const char *name, const uint8_t *buffer, uint32_t size) {
    fat32_dir_entry_t entry;
    if (find_entry(current_dir_cluster, name, &entry) < 0) return -1;
    uint32_t cluster = first_cluster_of(&entry);

    if (cluster == 0) {
        cluster = fat_find_free();
        if (cluster == 0) return -1;
        if (fat_write_entry(cluster, 0x0FFFFFFF) != 0) return -1;
        entry.first_cluster_low = cluster & 0xFFFF;
        entry.first_cluster_high = (cluster >> 16) & 0xFFFF;
    }

    int result = write_chain(cluster, (uint8_t *)buffer, size);
    if (result < 0) return result;

    uint8_t name83[11];
    name_to_83(name, name83);
    uint32_t c = current_dir_cluster;
    while (c >= 2 && c < 0x0FFFFFF8) {
        uint32_t s = cluster_to_sector(c);
        for (uint32_t si = 0; si < sectors_per_cluster; si++) {
            uint8_t buf[512];
            if (ata_read_sector(s + si, buf) != 0) return result;
            fat32_dir_entry_t *entries = (fat32_dir_entry_t *)buf;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == 0xE5) continue;
                if (entries[i].attr == 0x0F) continue;
                int match = 1;
                for (int j = 0; j < 11; j++)
                    if (entries[i].name[j] != name83[j]) { match = 0; break; }
                if (match) {
                    entries[i].size = result;
                    entries[i].first_cluster_low = cluster & 0xFFFF;
                    entries[i].first_cluster_high = (cluster >> 16) & 0xFFFF;
                    if (ata_write_sector(s + si, buf) != 0) return -1;
                    return result;
                }
            }
        }
        c = fat_read_entry(c);
    }
    return result;
}

/* ~~ fat32_list_dir ~~ */
/* ~ cuidado que essa aqui morde ~ */
int fat32_list_dir(void) {
    return list_dir_at(current_dir_cluster);
}

/* ~~ fat32_change_dir ~~ */
/* ~ cuidado que essa aqui morde ~ */
int fat32_change_dir(const char *name) {
    if (name[0] == '\0' || (name[0] == '/' && name[1] == '\0')) {
        current_dir_cluster = boot.root_cluster;
        return 0;
    }
    if (name[0] == '/') {
        current_dir_cluster = boot.root_cluster;
        name++;
        if (name[0] == '\0') return 0;
    }

    fat32_dir_entry_t entry;
    if (find_entry(current_dir_cluster, name, &entry) < 0) {
        return FAT_ERR_NOTFOUND;
    }
    if (!(entry.attr & 0x10)) {
        return FAT_ERR_NOTDIR;
    }
    uint32_t new_cluster = first_cluster_of(&entry);
    if (new_cluster == 0) new_cluster = boot.root_cluster;
    current_dir_cluster = new_cluster;
    return 0;
}

/* ~~ Pegando o valor~ só confia que ta certo~ */
/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
void fat32_get_cwd_name(char *out, int maxlen) {
    if (current_dir_cluster == boot.root_cluster) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }

    char stack[16][11];
    int depth = 0;
    uint32_t c = current_dir_cluster;

    while (c != boot.root_cluster) {
        uint32_t parent_cluster = boot.root_cluster;
        uint32_t sector_num = cluster_to_sector(c);
        uint8_t buf[512];
        if (ata_read_sector(sector_num, buf) == 0) {
            fat32_dir_entry_t *entries = (fat32_dir_entry_t *)buf;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == 0xE5) continue;
                if (entries[i].attr != 0x0F &&
                    entries[i].name[0] == '.' &&
                    entries[i].name[1] == '.' &&
                    entries[i].name[2] == ' ') {
                    parent_cluster = first_cluster_of(&entries[i]);
                    if (parent_cluster == 0) parent_cluster = boot.root_cluster;
                    break;
                }
            }
        }

        uint8_t found = 0;
        uint32_t pc = parent_cluster;
        while (pc >= 2 && pc < 0x0FFFFFF8 && !found) {
            uint32_t ps = cluster_to_sector(pc);
            for (uint32_t si = 0; si < sectors_per_cluster && !found; si++) {
                uint8_t pb[512];
                if (ata_read_sector(ps + si, pb) != 0) break;
                fat32_dir_entry_t *pe = (fat32_dir_entry_t *)pb;
                for (int i = 0; i < 16 && !found; i++) {
                    if (pe[i].name[0] == 0x00 ||
                        pe[i].name[0] == 0xE5) continue;
                    if (pe[i].attr == 0x0F) continue;
                    uint32_t ec = first_cluster_of(&pe[i]);
                    if (ec == c) {
                        for (int j = 0; j < 11; j++)
                            stack[depth][j] = pe[i].name[j];
                        depth++;
                        found = 1;
                    }
                }
            }
            pc = fat_read_entry(pc);
        }
        c = parent_cluster;
    }

    int pos = 0;
    out[pos++] = '/';
    for (int i = depth - 1; i >= 0; i--) {
        for (int j = 0; j < 11 && stack[i][j] != ' '; j++) {
            if (pos < maxlen - 1) out[pos++] = stack[i][j];
        }
        if (i > 0 && pos < maxlen - 1) out[pos++] = '/';
    }
    out[pos] = '\0';
}

/* ~~ Pegando o valor~ só confia que ta certo~ */
/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
uint32_t fat32_get_cwd(void) {
    return current_dir_cluster;
}

/* ~~ Setando~~ espero que saiba o que ta fazendo! */
/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
void fat32_set_cwd(uint32_t cluster) {
    current_dir_cluster = cluster;
}

/* ~~ fat32_mkdir ~~ */
/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
int fat32_mkdir(const char *name) {
    uint32_t new_cluster = fat_find_free();
    if (new_cluster == 0) return FAT_ERR_NOSPACE;

    fat32_dir_entry_t entry;
    if (find_entry(current_dir_cluster, name, &entry) >= 0)
        return FAT_ERR_EXISTS;

    int r = create_entry_at(current_dir_cluster, name, 0x10);
    if (r != 0) return r;

    uint8_t name83[11];
    name_to_83(name, name83);
    uint32_t c = current_dir_cluster;
    while (c >= 2 && c < 0x0FFFFFF8) {
        uint32_t s = cluster_to_sector(c);
        for (uint32_t si = 0; si < sectors_per_cluster; si++) {
            uint8_t buf[512];
            if (ata_read_sector(s + si, buf) != 0) return FAT_ERR_IO;
            fat32_dir_entry_t *entries = (fat32_dir_entry_t *)buf;
            for (int i = 0; i < 16; i++) {
                if (entries[i].name[0] == 0xE5) continue;
                if (entries[i].attr == 0x0F) continue;
                int match = 1;
                for (int j = 0; j < 11; j++)
                    if (entries[i].name[j] != name83[j]) { match = 0; break; }
                if (match && (entries[i].attr & 0x10)) {
                    entries[i].first_cluster_low = new_cluster & 0xFFFF;
                    entries[i].first_cluster_high =
                        (new_cluster >> 16) & 0xFFFF;
                    if (ata_write_sector(s + si, buf) != 0) return FAT_ERR_IO;

                    uint8_t dirbuf[512] = {0};
                    fat32_dir_entry_t *de = (fat32_dir_entry_t *)dirbuf;
                    de[0].name[0] = '.';
                    de[0].attr = 0x10;
                    for (int j = 1; j < 11; j++) de[0].name[j] = ' ';
                    de[0].first_cluster_low = new_cluster & 0xFFFF;
                    de[0].first_cluster_high =
                        (new_cluster >> 16) & 0xFFFF;
                    de[1].name[0] = '.';
                    de[1].name[1] = '.';
                    de[1].attr = 0x10;
                    for (int j = 2; j < 11; j++) de[1].name[j] = ' ';
                    de[1].first_cluster_low = current_dir_cluster & 0xFFFF;
                    de[1].first_cluster_high =
                        (current_dir_cluster >> 16) & 0xFFFF;
                    uint32_t ds = cluster_to_sector(new_cluster);
                    if (fat_write_entry(new_cluster, 0x0FFFFFFF) != 0) return FAT_ERR_IO;
                    if (ata_write_sector(ds, dirbuf) != 0) return FAT_ERR_IO;
                    return 0;
                }
            }
        }
        c = fat_read_entry(c);
    }
    return -1;
}

/* ~~ fat32_rename ~~ */
/* ~ essa demorou pra debugar, respeita ~ */
int fat32_rename(const char *oldname, const char *newname) {
    uint8_t old83[11], new83[11];
    name_to_83(oldname, old83);
    name_to_83(newname, new83);
    uint32_t c = current_dir_cluster;
    while (c >= 2 && c < 0x0FFFFFF8) {
        uint32_t s = cluster_to_sector(c);
        for (uint32_t si = 0; si < sectors_per_cluster; si++) {
            uint8_t buf[512];
            if (ata_read_sector(s + si, buf) != 0) return FAT_ERR_IO;
            fat32_dir_entry_t *e = (fat32_dir_entry_t *)buf;
            for (int i = 0; i < 16; i++) {
                if (e[i].name[0] == 0x00 || e[i].name[0] == 0xE5) continue;
                if (e[i].attr == 0x0F) continue;
                int match = 1;
                for (int j = 0; j < 11; j++)
                    if (e[i].name[j] != old83[j]) { match = 0; break; }
                if (!match) continue;
                for (int j = 0; j < 11; j++) e[i].name[j] = new83[j];
                if (ata_write_sector(s + si, buf) != 0) return FAT_ERR_IO;
                return 0;
            }
        }
        c = fat_read_entry(c);
    }
    return FAT_ERR_NOTFOUND;
}

/* ~~ fat32_rmdir ~~ */
/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
int fat32_rmdir(const char *name) {
    uint8_t name83[11];
    name_to_83(name, name83);
    uint32_t c = current_dir_cluster;
    while (c >= 2 && c < 0x0FFFFFF8) {
        uint32_t s = cluster_to_sector(c);
        for (uint32_t si = 0; si < sectors_per_cluster; si++) {
            uint8_t buf[512];
            if (ata_read_sector(s + si, buf) != 0) return FAT_ERR_IO;
            fat32_dir_entry_t *e = (fat32_dir_entry_t *)buf;
            for (int i = 0; i < 16; i++) {
                if (e[i].name[0] == 0x00 || e[i].name[0] == 0xE5) continue;
                if (e[i].attr == 0x0F) continue;
                int match = 1;
                for (int j = 0; j < 11; j++)
                    if (e[i].name[j] != name83[j]) { match = 0; break; }
                if (!match) continue;
                if (!(e[i].attr & 0x10)) return FAT_ERR_NOTDIR;
                uint32_t cl = ((uint32_t)e[i].first_cluster_high << 16) | e[i].first_cluster_low;
                if (cl != 0) {
                    uint8_t db[512];
                    if (ata_read_sector(cluster_to_sector(cl), db) == 0) {
                        fat32_dir_entry_t *de = (fat32_dir_entry_t *)db;
                        int empty = 1;
                        for (int ei = 2; ei < 16; ei++) {
                            if (de[ei].name[0] != 0x00 && de[ei].name[0] != 0xE5)
                                { empty = 0; break; }
                        }
                        if (!empty) return FAT_ERR_NOTEMPTY;
                    }
                    uint32_t next;
                    do {
                        next = fat_read_entry(cl);
                        fat_write_entry(cl, 0);
                        cl = next;
                    } while (cl >= 2 && cl < 0x0FFFFFF8);
                }
                e[i].name[0] = 0xE5;
                if (ata_write_sector(s + si, buf) != 0) return FAT_ERR_IO;
                return 0;
            }
        }
        c = fat_read_entry(c);
    }
    return FAT_ERR_NOTFOUND;
}

/* ~~ fat32_stat ~~ */
static int fat32_stat_unlocked(const char *name, uint32_t *size, uint8_t *attr,
                               uint16_t *mtime, uint16_t *mdate) {
    fat32_dir_entry_t e;
    uint32_t saved = current_dir_cluster;
    char base[256];
    if (fat32_walk(name, base, sizeof(base)) < 0) {
        current_dir_cluster = saved; return FAT_ERR_NOTFOUND;
    }
    int r = find_entry(current_dir_cluster, base, &e);
    current_dir_cluster = saved;
    if (r < 0) return FAT_ERR_NOTFOUND;
    if (size) *size = e.size;
    if (attr) *attr = e.attr;
    if (mtime) *mtime = e.modification_time;
    if (mdate) *mdate = e.modification_date;
    return 0;
}

/* ~~ fat32_sync ~~ */
/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
void fat32_sync(void) {
}

static int fat32_truncate_unlocked(const char *name, uint32_t new_size) {
    if (!name) return FAT_ERR_IO;
    fat32_dir_entry_t e;
    uint32_t saved = current_dir_cluster;
    char base[256];
    if (fat32_walk(name, base, sizeof(base)) < 0) {
        current_dir_cluster = saved; return FAT_ERR_NOTFOUND;
    }
    if (find_entry(current_dir_cluster, base, &e) < 0) {
        current_dir_cluster = saved; return FAT_ERR_NOTFOUND;
    }
    if (e.attr & 0x10) { current_dir_cluster = saved; return FAT_ERR_IO; }
    uint32_t old_size = e.size;
    if (new_size == old_size) { current_dir_cluster = saved; return 0; }
    uint32_t cluster = first_cluster_of(&e);
    uint32_t cbytes = sectors_per_cluster * 512;
    if (new_size > old_size) {
        /* grow: alocar clusters e zerar extensão */
        if (cluster == 0) {
            cluster = fat_find_free();
            if (cluster == 0) { current_dir_cluster = saved; return FAT_ERR_NOSPACE; }
            if (fat_write_entry(cluster, 0x0FFFFFFF) != 0) { current_dir_cluster = saved; return FAT_ERR_IO; }
            e.first_cluster_low = cluster & 0xFFFF;
            e.first_cluster_high = (cluster >> 16) & 0xFFFF;
            /* atualizar entry no diretório para refletir novo cluster inicial */
            {
                uint8_t name83[11]; name_to_83(base, name83);
                uint32_t c = current_dir_cluster;
                while (c >= 2 && c < 0x0FFFFFF8) {
                    uint32_t s = cluster_to_sector(c);
                    for (uint32_t si = 0; si < sectors_per_cluster; si++) {
                        uint8_t buf[512]; if (ata_read_sector(s + si, buf) != 0) { current_dir_cluster = saved; return FAT_ERR_IO; }
                        fat32_dir_entry_t *ents = (fat32_dir_entry_t *)buf;
                        for (int i = 0; i < 16; i++) {
                            if (ents[i].name[0] == 0xE5 || ents[i].attr == 0x0F) continue;
                            int match = 1; for (int j = 0; j < 11; j++) if (ents[i].name[j] != name83[j]) { match = 0; break; }
                            if (match) {
                                ents[i].first_cluster_low = e.first_cluster_low;
                                ents[i].first_cluster_high = e.first_cluster_high;
                                if (ata_write_sector(s + si, buf) != 0) { current_dir_cluster = saved; return FAT_ERR_IO; }
                                goto grow_clusters;
                            }
                        }
                    }
                    c = fat_read_entry(c);
                }
            }
        }
grow_clusters: ;
        uint32_t old_clusters = (old_size + cbytes - 1) / cbytes;
        uint32_t new_clusters = (new_size + cbytes - 1) / cbytes;
        if (new_clusters > old_clusters) {
            uint32_t cur = cluster;
            /* caminhar até o último cluster atual */
            for (uint32_t i = 1; i < old_clusters; i++) {
                uint32_t nxt = fat_read_entry(cur);
                if (nxt < 2 || nxt >= 0x0FFFFFF8) break;
                cur = nxt;
            }
            if (old_size == 0) old_clusters = 1; /* já temos 1 cluster alocado acima */
            for (uint32_t i = old_clusters; i < new_clusters; i++) {
                uint32_t free = fat_find_free();
                if (free == 0) { current_dir_cluster = saved; return FAT_ERR_NOSPACE; }
                if (fat_write_entry(cur, free) != 0) { current_dir_cluster = saved; return FAT_ERR_IO; }
                if (fat_write_entry(free, 0x0FFFFFFF) != 0) { current_dir_cluster = saved; return FAT_ERR_IO; }
                cur = free;
            }
        }
        /* zerar extensão no disco */
        {
            uint8_t zero[512]; for (int i = 0; i < 512; i++) zero[i] = 0;
            uint32_t off = old_size;
            while (off < new_size) {
                uint32_t clust_idx = off / cbytes;
                uint32_t in_clust = off % cbytes;
                uint32_t cl = cluster;
                for (uint32_t i = 0; i < clust_idx; i++) {
                    uint32_t nxt = fat_read_entry(cl);
                    if (nxt < 2 || nxt >= 0x0FFFFFF8) break;
                    cl = nxt;
                }
                uint32_t sec = cluster_to_sector(cl) + in_clust / 512;
                uint32_t sec_off = in_clust % 512;
                uint32_t remain = cbytes - in_clust;
                uint32_t to_zero = new_size - off < remain ? new_size - off : remain;
                /* tratar setor parcial */
                if (sec_off || (to_zero % 512)) {
                    uint8_t buf[512];
                    uint32_t cur_sec = sec;
                    uint32_t cur_off = sec_off;
                    uint32_t left = to_zero;
                    while (left) {
                        if (ata_read_sector(cur_sec, buf) != 0) { current_dir_cluster = saved; return FAT_ERR_IO; }
                        uint32_t n = left < 512 - cur_off ? left : 512 - cur_off;
                        for (uint32_t b = 0; b < n; b++) buf[cur_off + b] = 0;
                        if (ata_write_sector(cur_sec, buf) != 0) { current_dir_cluster = saved; return FAT_ERR_IO; }
                        left -= n; cur_sec++; cur_off = 0;
                    }
                } else {
                    uint32_t secs = to_zero / 512;
                    for (uint32_t s = 0; s < secs; s++) {
                        if (ata_write_sector(sec + s, zero) != 0) { current_dir_cluster = saved; return FAT_ERR_IO; }
                    }
                }
                off += to_zero;
            }
        }
    } else {
        /* shrink: truncar chain e liberar clusters excedentes */
        if (new_size == 0) {
            if (cluster) {
                uint32_t cl = cluster;
                while (cl >= 2 && cl < 0x0FFFFFF8) {
                    uint32_t nxt = fat_read_entry(cl);
                    fat_write_entry(cl, 0);
                    if (nxt >= 0x0FFFFFF8 || nxt < 2) break;
                    cl = nxt;
                }
            }
            e.first_cluster_low = 0;
            e.first_cluster_high = 0;
        } else {
            uint32_t keep_clusters = (new_size + cbytes - 1) / cbytes;
            uint32_t cl = cluster;
            for (uint32_t i = 1; i < keep_clusters; i++) {
                uint32_t nxt = fat_read_entry(cl);
                if (nxt < 2 || nxt >= 0x0FFFFFF8) break;
                cl = nxt;
            }
            uint32_t to_free = fat_read_entry(cl);
            fat_write_entry(cl, 0x0FFFFFFF);
            while (to_free >= 2 && to_free < 0x0FFFFFF8) {
                uint32_t nxt = fat_read_entry(to_free);
                fat_write_entry(to_free, 0);
                to_free = nxt;
            }
            /* zerar cauda dentro do último cluster mantido */
            uint32_t tail_off = new_size % cbytes;
            if (tail_off) {
                uint32_t cl2 = cluster;
                for (uint32_t i = 1; i < keep_clusters; i++) {
                    uint32_t nxt = fat_read_entry(cl2);
                    if (nxt < 2 || nxt >= 0x0FFFFFF8) break;
                    cl2 = nxt;
                }
                uint32_t sec = cluster_to_sector(cl2) + tail_off / 512;
                uint32_t sec_off = tail_off % 512;
                uint8_t buf[512];
                if (ata_read_sector(sec, buf) == 0) {
                    for (uint32_t b = sec_off; b < 512; b++) buf[b] = 0;
                    ata_write_sector(sec, buf);
                }
            }
        }
    }
    /* atualizar size no diretório */
    {
        uint8_t name83[11]; name_to_83(base, name83);
        uint32_t c = current_dir_cluster;
        while (c >= 2 && c < 0x0FFFFFF8) {
            uint32_t s = cluster_to_sector(c);
            for (uint32_t si = 0; si < sectors_per_cluster; si++) {
                uint8_t buf[512]; if (ata_read_sector(s + si, buf) != 0) { current_dir_cluster = saved; return FAT_ERR_IO; }
                fat32_dir_entry_t *ents = (fat32_dir_entry_t *)buf;
                for (int i = 0; i < 16; i++) {
                    if (ents[i].name[0] == 0xE5 || ents[i].attr == 0x0F) continue;
                    int match = 1; for (int j = 0; j < 11; j++) if (ents[i].name[j] != name83[j]) { match = 0; break; }
                    if (match) {
                        ents[i].size = new_size;
                        ents[i].first_cluster_low = e.first_cluster_low;
                        ents[i].first_cluster_high = e.first_cluster_high;
                        if (ata_write_sector(s + si, buf) != 0) { current_dir_cluster = saved; return FAT_ERR_IO; }
                        current_dir_cluster = saved;
                        return 0;
                    }
                }
            }
            c = fat_read_entry(c);
        }
    }
    current_dir_cluster = saved;
    return FAT_ERR_IO;
}

int fat32_readdir(const char *path, uint64_t cookie,
                  fat32_vfs_dirent_t *entries, uint32_t capacity) {
    fat32_path_lock_acquire();
    int result = fat32_readdir_unlocked(path, cookie, entries, capacity);
    fat32_path_lock_release();
    return result;
}

int fat32_read_at(const char *name, uint8_t *buffer, uint32_t size, uint64_t offset) {
    fat32_path_lock_acquire();
    int result = fat32_read_at_unlocked(name, buffer, size, offset);
    fat32_path_lock_release();
    return result;
}

int fat32_write_at(const char *name, const uint8_t *buffer,
                   uint32_t size, uint64_t offset) {
    fat32_path_lock_acquire();
    int result = fat32_write_at_unlocked(name, buffer, size, offset);
    fat32_path_lock_release();
    return result;
}

int fat32_stat(const char *name, uint32_t *size, uint8_t *attr,
               uint16_t *mtime, uint16_t *mdate) {
    fat32_path_lock_acquire();
    int result = fat32_stat_unlocked(name, size, attr, mtime, mdate);
    fat32_path_lock_release();
    return result;
}

int fat32_truncate(const char *name, uint32_t new_size) {
    fat32_path_lock_acquire();
    int result = fat32_truncate_unlocked(name, new_size);
    fat32_path_lock_release();
    return result;
}




/* ♥ fat32.c ~ se bugar me chama, se n bugar tb me chama ~ >u< */
