/* dynlink.c — linker dinâmico no kernel (Fase 4 TipOS)
 * Quando não existe ld.so no FS, o kernel carrega as DT_NEEDED,
 * aplica RELA (RELATIVE/GLOB_DAT/JUMP_SLOT/COPY/IRELATIVE) e o
 * binário principal roda direto no entry dele. ~ kyun */
#include <stdint.h>
#include "memory.h"
#include "serial.h"
#include "../fs/vfs.h"

extern void *elf64_load_at(unsigned char *data, unsigned int len,
                           uint64_t pml4, uint64_t force_base);

#define DL_MAX_OBJS   8
#define DL_LIB_BASE   0x48000000ULL   /* entre PIE(0x40000000) e interp(0x60000000) */
#define DL_LIB_STEP   0x02000000ULL   /* 32MB por .so */

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} dl_ehdr_t;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} dl_phdr_t;

typedef struct { uint64_t d_tag, d_val; } dl_dyn_t;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } dl_rela_t;
typedef struct {
    uint32_t st_name;
    unsigned char st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} dl_sym_t;

typedef struct {
    uint64_t base;
    uint8_t *buf;
    uint32_t size;
    dl_phdr_t *ph;
    int phnum;
    long symtab_off, strtab_off;
    uint32_t strsz, syment, nsyms;
} dl_obj_t;

static long dl_vaddr_off(dl_phdr_t *ph, int phnum, uint64_t vaddr) {
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != 1) continue;
        if (vaddr >= ph[i].p_vaddr && vaddr < ph[i].p_vaddr + ph[i].p_filesz)
            return (long)(ph[i].p_offset + (vaddr - ph[i].p_vaddr));
    }
    return -1;
}

/* VA no PML4 do filho → endereço físico (identity no kernel) */
static uint64_t dl_va_to_pa(uint64_t pml4_pa, uint64_t va) {
    uint64_t *pml4 = (uint64_t *)pml4_pa;
    uint64_t e = pml4[(va >> 39) & 0x1FF];
    if (!(e & 1)) return 0;
    uint64_t *pdpt = (uint64_t *)(e & 0x000FFFFFFFFFF000ULL);
    uint64_t e2 = pdpt[(va >> 30) & 0x1FF];
    if (!(e2 & 1)) return 0;
    if (e2 & 0x80) return (e2 & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFFULL);
    uint64_t *pd = (uint64_t *)(e2 & 0x000FFFFFFFFFF000ULL);
    uint64_t e3 = pd[(va >> 21) & 0x1FF];
    if (!(e3 & 1)) return 0;
    if (e3 & 0x80) return (e3 & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFULL);
    uint64_t *pt = (uint64_t *)(e3 & 0x000FFFFFFFFFF000ULL);
    uint64_t e4 = pt[(va >> 12) & 0x1FF];
    if (!(e4 & 1)) return 0;
    return (e4 & 0x000FFFFFFFFFF000ULL) | (va & 0xFFFULL);
}

static int dl_write64(uint64_t pml4, uint64_t va, uint64_t val) {
    uint64_t pa = dl_va_to_pa(pml4, va);
    if (!pa) {
        serial_puts("[dynl] write sem pa va=");
        serial_puthex((uint32_t)va);
        serial_puts("\r\n");
        return -1;
    }
    *(volatile uint64_t *)pa = val;
    return 0;
}

/* conta .dynsym: DT_HASH (nchain) ou DT_GNU_HASH (varre buckets) */
static uint32_t dl_sym_count(uint8_t *buf, long hash_off, long gnu_off) {
    if (hash_off >= 0) {
        uint32_t *h = (uint32_t *)(buf + hash_off);
        return h[1]; /* nchain */
    }
    if (gnu_off >= 0) {
        uint32_t *h = (uint32_t *)(buf + gnu_off);
        uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2];
        uint64_t *bloom = (uint64_t *)(h + 4);
        uint32_t *buckets = (uint32_t *)(bloom + bloom_size);
        uint32_t *chains = buckets + nbuckets;
        uint32_t max = symoffset;
        for (uint32_t i = 0; i < nbuckets; i++) {
            uint32_t idx = buckets[i];
            if (idx < symoffset) continue;
            while (1) {
                uint32_t c = chains[idx - symoffset];
                idx++;
                if (c & 1) break;
            }
            if (idx > max) max = idx;
        }
        return max;
    }
    return 0;
}

/* procura símbolo DEFINIDO em todos os objetos (ordem de carga) */
static uint64_t dl_lookup(dl_obj_t *objs, int nobj, const char *name,
                          uint64_t *size_out) {
    for (int o = 0; o < nobj; o++) {
        dl_obj_t *ob = &objs[o];
        if (ob->symtab_off < 0 || ob->strtab_off < 0) continue;
        for (uint32_t i = 0; i < ob->nsyms; i++) {
            dl_sym_t *s = (dl_sym_t *)(ob->buf + ob->symtab_off +
                                       (uint64_t)i * ob->syment);
            if (s->st_shndx == 0 || s->st_name == 0) continue;
            if (s->st_name >= ob->strsz) continue;
            const char *sn = (const char *)(ob->buf + ob->strtab_off + s->st_name);
            int match = 1;
            for (int k = 0; name[k] || sn[k]; k++)
                if (name[k] != sn[k]) { match = 0; break; }
            if (match) {
                if (size_out) *size_out = s->st_size;
                return ob->base + s->st_value;
            }
        }
    }
    return 0;
}

static int dl_open_lib(const char *name, const char *runpath,
                       uint8_t **out, uint32_t *outsize) {
    char path[256];
    uint32_t sz;
    uint8_t attr;

    if (name[0] == '/') {
        if (vfs_stat_size_attr(name, &sz, &attr) == 0 && sz >= 64) {
            uint8_t *b = kmalloc(sz + 1);
            if (b && vfs_read_file(name, b, sz) >= 0) { *out = b; *outsize = sz; return 0; }
            if (b) kfree(b);
        }
        return -1;
    }
    /* DT_RUNPATH/DT_RPATH primeiro */
    if (runpath && runpath[0]) {
        const char *p = runpath;
        while (*p) {
            int n = 0;
            while (p[n] && p[n] != ':') n++;
            if (n > 0 && n < 200) {
                int i = 0;
                for (; i < n; i++) path[i] = p[i];
                path[i] = '/'; i++;
                int j = 0;
                while (name[j] && i < 240) path[i++] = name[j++];
                path[i] = 0;
                if (vfs_stat_size_attr(path, &sz, &attr) == 0 && sz >= 64) {
                    uint8_t *b = kmalloc(sz + 1);
                    if (b && vfs_read_file(path, b, sz) >= 0) { *out = b; *outsize = sz; return 0; }
                    if (b) kfree(b);
                }
            }
            p += n;
            if (*p == ':') p++;
        }
    }
    static const char *const dirs[] = { "/lib/", "/usr/lib/", "/" };
    for (int d = 0; d < 3; d++) {
        int i = 0;
        const char *pre = dirs[d];
        while (pre[i] && i < 200) { path[i] = pre[i]; i++; }
        int j = 0;
        while (name[j] && i < 240) path[i++] = name[j++];
        path[i] = 0;
        if (vfs_stat_size_attr(path, &sz, &attr) == 0 && sz >= 64) {
            uint8_t *b = kmalloc(sz + 1);
            if (b && vfs_read_file(path, b, sz) >= 0) {
                serial_puts("[dynl] lib ");
                serial_puts(path);
                serial_puts("\r\n");
                *out = b; *outsize = sz; return 0;
            }
            if (b) kfree(b);
        }
    }
    return -1;
}

static int dl_apply_relas(dl_obj_t *objs, int nobj, dl_obj_t *o,
                          uint64_t pml4, uint64_t rela_va, uint64_t rela_sz,
                          int *unsupported) {
    long off = dl_vaddr_off(o->ph, o->phnum, rela_va);
    if (off < 0 || off + rela_sz > o->size) return -1;
    uint64_t count = rela_sz / 24;
    for (uint64_t i = 0; i < count; i++) {
        dl_rela_t *r = (dl_rela_t *)(o->buf + off + i * 24);
        uint32_t type = (uint32_t)(r->r_info & 0xFFFFFFFF);
        uint32_t sym = (uint32_t)(r->r_info >> 32);
        uint64_t loc = o->base + r->r_offset;

        if (type == 0) continue;                       /* NONE */
        if (type == 8) {                               /* RELATIVE */
            if (dl_write64(pml4, loc, o->base + (uint64_t)r->r_addend) < 0)
                return -1;
            continue;
        }
        if (type == 37) {                              /* IRELATIVE: chama o resolver */
            uint64_t cur;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cur));
            __asm__ volatile("mov %0, %%cr3" :: "r"(pml4) : "memory");
            uint64_t (*fn)(void) = (uint64_t (*)(void))(o->base + (uint64_t)r->r_addend);
            uint64_t v = fn();
            __asm__ volatile("mov %0, %%cr3" :: "r"(cur) : "memory");
            if (dl_write64(pml4, loc, v) < 0) return -1;
            continue;
        }
        /* tipos que precisam de símbolo: 1=64, 5=COPY, 6=GLOB_DAT, 7=JUMP_SLOT */
        if (type != 1 && type != 5 && type != 6 && type != 7) {
            (*unsupported)++;
            continue;
        }
        if (sym == 0 || sym >= o->nsyms) { (*unsupported)++; continue; }
        dl_sym_t *s = (dl_sym_t *)(o->buf + o->symtab_off +
                                   (uint64_t)sym * o->syment);
        if (s->st_name == 0 || s->st_name >= o->strsz) { (*unsupported)++; continue; }
        const char *name = (const char *)(o->buf + o->strtab_off + s->st_name);
        uint64_t sz = 0;
        uint64_t S = dl_lookup(objs, nobj, name, &sz);
        if (!S) {
            serial_puts("[dynl] sym missing: ");
            serial_puts(name);
            serial_puts("\r\n");
            return -1;
        }
        if (type == 5) {                               /* COPY */
            uint64_t spa = dl_va_to_pa(pml4, S);
            uint64_t dpa = dl_va_to_pa(pml4, loc);
            if (!spa || !dpa) return -1;
            uint8_t *src = (uint8_t *)spa, *dst = (uint8_t *)dpa;
            for (uint64_t k = 0; k < s->st_size; k++) dst[k] = src[k];
            continue;
        }
        uint64_t val = (type == 1) ? (S + (uint64_t)r->r_addend) : S;
        if (dl_write64(pml4, loc, val) < 0) return -1;
    }
    return 0;
}

/* Retorna: 0 = dinâmico linkado, 1 = binário estático, -1 = erro */
int dynlink_process(uint64_t pml4, uint8_t *buf, uint32_t fsize,
                    uint64_t main_base) {
    if (fsize < sizeof(dl_ehdr_t)) return 1;
    if (buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F')
        return 1;
    dl_ehdr_t *eh = (dl_ehdr_t *)buf;
    if (eh->e_phentsize != sizeof(dl_phdr_t) || eh->e_phnum == 0) return 1;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > fsize) return 1;
    dl_phdr_t *ph = (dl_phdr_t *)(buf + eh->e_phoff);

    long dyn_off = -1;
    uint64_t dyn_sz = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == 2) {
            dyn_off = (long)ph[i].p_offset;
            dyn_sz = ph[i].p_filesz ? ph[i].p_filesz : ph[i].p_memsz;
        }
    }
    if (dyn_off < 0 || dyn_off + (uint64_t)sizeof(dl_dyn_t) > fsize)
        return 1; /* estático */

    uint64_t strtab_va = 0, symtab_va = 0, hash_va = 0, gnu_hash_va = 0;
    uint64_t rela_va = 0, rela_sz = 0, jmprel_va = 0, pltrelsz = 0;
    uint64_t syment = 24;
    uint64_t needed[DL_MAX_OBJS];
    int nneeded = 0;
    long runpath_off = -1;

    for (uint64_t o = 0; o + sizeof(dl_dyn_t) <= dyn_sz; o += sizeof(dl_dyn_t)) {
        dl_dyn_t *d = (dl_dyn_t *)(buf + dyn_off + o);
        if (d->d_tag == 0) break;
        switch (d->d_tag) {
        case 1:  if (nneeded < DL_MAX_OBJS) needed[nneeded++] = d->d_val; break;
        case 4:  hash_va = d->d_val; break;
        case 5:  strtab_va = d->d_val; break;
        case 6:  symtab_va = d->d_val; break;
        case 7:  rela_va = d->d_val; break;
        case 8:  rela_sz = d->d_val; break;
        case 10: break; /* STRSZ */
        case 11: syment = d->d_val; break;
        case 15: runpath_off = (long)d->d_val; break;
        case 23: jmprel_va = d->d_val; break;
        case 2:  pltrelsz = d->d_val; break;
        case 29: runpath_off = (long)d->d_val; break; /* DT_RUNPATH */
        case 0x6ffffef5: gnu_hash_va = d->d_val; break;
        default: break;
        }
    }
    if (!symtab_va || !strtab_va) {
        serial_puts("[dynl] dynamic sem symtab\r\n");
        return -1;
    }
    (void)rela_va; (void)rela_sz; (void)jmprel_va; (void)pltrelsz;

    char runpath[256];
    runpath[0] = 0;
    long strtab_main = dl_vaddr_off(ph, eh->e_phnum, strtab_va);
    if (runpath_off >= 0 && strtab_main >= 0) {
        int i = 0;
        const char *s = (const char *)(buf + strtab_main + runpath_off);
        while (s[i] && i < 255) { runpath[i] = s[i]; i++; }
        runpath[i] = 0;
    }

    dl_obj_t objs[DL_MAX_OBJS];
    int nobj = 0;

    /* objeto principal */
    objs[0].base = main_base;
    objs[0].buf = buf;
    objs[0].size = fsize;
    objs[0].ph = ph;
    objs[0].phnum = eh->e_phnum;
    objs[0].symtab_off = dl_vaddr_off(ph, eh->e_phnum, symtab_va);
    objs[0].strtab_off = strtab_main;
    objs[0].strsz = fsize;
    objs[0].syment = (uint32_t)(syment ? syment : 24);
    objs[0].nsyms = dl_sym_count(buf, hash_va ? dl_vaddr_off(ph, eh->e_phnum, hash_va) : -1,
                                 gnu_hash_va ? dl_vaddr_off(ph, eh->e_phnum, gnu_hash_va) : -1);
    nobj = 1;

    /* DT_NEEDED */
    const char *strtab_main_p = strtab_main >= 0 ? (const char *)(buf + strtab_main) : "";
    for (int n = 0; n < nneeded; n++) {
        if (nobj >= DL_MAX_OBJS) break;
        const char *name = strtab_main_p + needed[n];
        uint8_t *lib = NULL;
        uint32_t libsz = 0;
        if (dl_open_lib(name, runpath, &lib, &libsz) < 0) {
            serial_puts("[dynl] lib nao encontrada: ");
            serial_puts(name);
            serial_puts("\r\n");
            return -1;
        }
        uint64_t base = DL_LIB_BASE + (uint64_t)(nobj - 1) * DL_LIB_STEP;
        void *ientry = elf64_load_at(lib, libsz, pml4, base);
        if (!ientry) {
            serial_puts("[dynl] load falhou: ");
            serial_puts(name);
            serial_puts("\r\n");
            kfree(lib);
            return -1;
        }
        dl_ehdr_t *le = (dl_ehdr_t *)lib;
        dl_phdr_t *lph = (dl_phdr_t *)(lib + le->e_phoff);
        uint64_t l_strtab = 0, l_symtab = 0, l_hash = 0, l_gnu = 0;
        long l_dyn = -1;
        uint64_t l_dynsz = 0;
        for (int i = 0; i < le->e_phnum; i++) {
            if (lph[i].p_type == 2) {
                l_dyn = (long)lph[i].p_offset;
                l_dynsz = lph[i].p_filesz ? lph[i].p_filesz : lph[i].p_memsz;
            }
        }
        if (l_dyn < 0) { kfree(lib); return -1; }
        for (uint64_t o = 0; o + sizeof(dl_dyn_t) <= l_dynsz; o += sizeof(dl_dyn_t)) {
            dl_dyn_t *d = (dl_dyn_t *)(lib + l_dyn + o);
            if (d->d_tag == 0) break;
            if (d->d_tag == 5) l_strtab = d->d_val;
            else if (d->d_tag == 6) l_symtab = d->d_val;
            else if (d->d_tag == 4) l_hash = d->d_val;
            else if (d->d_tag == 0x6ffffef5) l_gnu = d->d_val;
        }
        objs[nobj].base = base;
        objs[nobj].buf = lib;
        objs[nobj].size = libsz;
        objs[nobj].ph = lph;
        objs[nobj].phnum = le->e_phnum;
        objs[nobj].symtab_off = l_symtab ? dl_vaddr_off(lph, le->e_phnum, l_symtab) : -1;
        objs[nobj].strtab_off = l_strtab ? dl_vaddr_off(lph, le->e_phnum, l_strtab) : -1;
        objs[nobj].strsz = libsz;
        objs[nobj].syment = 24;
        objs[nobj].nsyms = dl_sym_count(lib,
            l_hash ? dl_vaddr_off(lph, le->e_phnum, l_hash) : -1,
            l_gnu ? dl_vaddr_off(lph, le->e_phnum, l_gnu) : -1);
        serial_puts("[dynl] loaded ");
        serial_puts(name);
        serial_puts(" base=");
        serial_puthex((uint32_t)base);
        serial_puts(" nsyms=");
        serial_puthex(objs[nobj].nsyms);
        serial_puts("\r\n");
        nobj++;
    }

    /* relocações de cada objeto */
    int unsupported = 0;
    for (int i = 0; i < nobj; i++) {
        dl_obj_t *o = &objs[i];
        uint64_t o_rela = 0, o_relasz = 0, o_jmprel = 0, o_pltrelsz = 0;
        long o_dyn = -1;
        uint64_t o_dynsz = 0;
        for (int k = 0; k < o->phnum; k++)
            if (o->ph[k].p_type == 2) {
                o_dyn = (long)o->ph[k].p_offset;
                o_dynsz = o->ph[k].p_filesz ? o->ph[k].p_filesz : o->ph[k].p_memsz;
            }
        if (o_dyn < 0) continue;
        for (uint64_t k = 0; k + sizeof(dl_dyn_t) <= o_dynsz; k += sizeof(dl_dyn_t)) {
            dl_dyn_t *d = (dl_dyn_t *)(o->buf + o_dyn + k);
            if (d->d_tag == 0) break;
            if (d->d_tag == 7) o_rela = d->d_val;
            else if (d->d_tag == 8) o_relasz = d->d_val;
            else if (d->d_tag == 23) o_jmprel = d->d_val;
            else if (d->d_tag == 2) o_pltrelsz = d->d_val;
        }
        if (o_rela && o_relasz)
            if (dl_apply_relas(objs, nobj, o, pml4, o_rela, o_relasz, &unsupported) < 0)
                return -1;
        if (o_jmprel && o_pltrelsz)
            if (dl_apply_relas(objs, nobj, o, pml4, o_jmprel, o_pltrelsz, &unsupported) < 0)
                return -1;
        serial_puts("[dynl] relocs ok obj=");
        serial_puthex((uint32_t)i);
        serial_puts("\r\n");
    }
    if (unsupported) {
        serial_puts("[dynl] unsupported types=");
        serial_puthex((uint32_t)unsupported);
        serial_puts("\r\n");
    }

    /* libs ficam carregadas; buffers dos libs são mantidos (não dá pra
     * kfree: os segmentos já foram copiados pro PML4, mas os buffers de
     * leitura não são mais necessários) */
    for (int i = 1; i < nobj; i++) kfree(objs[i].buf);
    serial_puts("[dynl] OK objs=");
    serial_puthex((uint32_t)nobj);
    serial_puts("\r\n");
    return 0;
}
