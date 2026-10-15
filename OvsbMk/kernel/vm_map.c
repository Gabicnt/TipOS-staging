/* moe moe kyun <3 */
/* moe moe kyun <3 */
/* ♥ kernel stuff ~ aqui é onde o bicho pega de verdade! kyun~
 * arquivo: vm_map.c ~ funcoes anotadas: 19
 */
#include "vm_map.h"
#include "memory.h"
#include "serial.h"
#include "process.h"
#include <stdint.h>
#include <stddef.h>

/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
static uint64_t vm_alloc_page(void) {
    void *p = mmap_user(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON);
    return p ? (uint64_t)(uintptr_t)p : 0;
}

/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
static void vm_free_page(uint64_t pa) {
    munmap_user((void *)(uintptr_t)pa, 4096);
}

/* FASE5: contagem de donos por frame (objetos compartilhados).
 * Entradas partidas (split no munmap) e mremap dividem os MESMOS
 * frames entre dois vm_object_t. Soltar o frame no primeiro munmap
 * deixava a irmã mapeando frame livre (use-after-free → bytes de
 * usuário na freelist do buddy → EXC#0x0D no mmap do foot).
 * refcnt do objeto não bastava (protegia a struct, não as páginas).
 * Tabela global simples: sem ciclo de vida acoplado. */
#define OBJREF_MAX BUDDY_FRAME_COUNT
static uint16_t objref_tab[OBJREF_MAX];
static inline size_t objref_idx(uint64_t pa) {
    if (pa < BUDDY_HEAP_PHYS || pa >= (uint64_t)BUDDY_HEAP_PHYS + BUDDY_HEAP_SIZE) return (size_t)-1;
    return (size_t)((pa - BUDDY_HEAP_PHYS) >> 12);
}
/* registra UM dono novo (chamado ao instalar página nova) */
void objref_claim(uint64_t pa) {
    size_t i = objref_idx(pa);
    if (i < OBJREF_MAX && objref_tab[i] < 0xFFFF) objref_tab[i]++;
}
/* devolve UMA posse; retorna 1 se foi o último dono (pode liberar) */
static inline int objref_release(uint64_t pa) {
    size_t i = objref_idx(pa);
    if (i >= OBJREF_MAX || objref_tab[i] == 0) return 1;
    return --objref_tab[i] == 0;
}

/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
static vm_map_entry_t *entry_alloc(void) {
    return (vm_map_entry_t *)kmalloc(sizeof(vm_map_entry_t));
}

/* ~ essa demorou pra debugar, respeita ~ */
static void entry_free(vm_map_entry_t *e) {
    kfree(e);
}

/* ~ essa demorou pra debugar, respeita ~ */
vm_object_t *vm_object_create(size_t size) {
    vm_object_t *obj = (vm_object_t *)kmalloc(sizeof(vm_object_t));
    if (!obj) return NULL;
    int npages = (size + 0xFFF) >> 12;
    if (npages == 0) npages = 1;
    obj->type = VM_OBJ_ANON;
    obj->refcnt = 1;
    obj->size = (size_t)npages << 12;
    obj->pages = (uint64_t *)kcalloc(npages, sizeof(uint64_t));
    obj->page_count = npages;
    if (!obj->pages) { kfree(obj); return NULL; }
    for (int i = 0; i < npages; i++)
        obj->pages[i] = 0;
    return obj;
}

/* FASE5: parte um objeto em dois (split de entrada no munmap/fixed).
 * O pedaço [a0, a0+n) (índices ABSOLUTOS de página, como o wire usa)
 * MUDA de dono: sai do array velho (zerado lá) e vai pro objeto novo.
 * Sem compartilhar array (o share antigo dividia os mesmos slots e o
 * munmap de um lado liberava frames ainda mapeados do outro). */
static vm_object_t *vm_object_split(vm_object_t *obj, size_t a0, size_t n) {
    if (!obj) return NULL;
    vm_object_t *nobj = (vm_object_t *)kmalloc(sizeof(vm_object_t));
    if (!nobj) return NULL;
    nobj->type = obj->type;
    nobj->refcnt = 1;
    nobj->size = (size_t)n << 12;
    nobj->page_count = (int)n;
    nobj->pages = NULL;
    if (n > 0) {
        nobj->pages = (uint64_t *)kcalloc(n, sizeof(uint64_t));
        if (!nobj->pages) { kfree(nobj); return NULL; }
        if (obj->pages) {
            for (size_t j = 0; j < n; j++) {
                size_t src = a0 + j;
                uint64_t pa = (src < (size_t)obj->page_count) ? obj->pages[src] : 0;
                nobj->pages[j] = pa;
                if (src < (size_t)obj->page_count) obj->pages[src] = 0;
                /* posse muda de dono: contagem igual (sem bump) */
            }
        }
    }
    return nobj;
}

/* ~ cuidado que essa aqui morde ~ */
vm_object_t *vm_object_share(vm_object_t *obj) {
    if (!obj) return NULL;
    /* FASE5-shm: snapshot PRIVADO (array próprio + reclama os frames).
     * NÃO mexe no refcnt do original: cada objeto solta só o dele no
     * destroy (frame cai no último dono via objref). O COMPARTILHAMENTO
     * real (MAP_SHARED: wl_shm/foot) usa vm_object_alias abaixo — antes
     * tudo virava snapshot e o compositor lia zeros (tela preta). */
    vm_object_t *nobj = (vm_object_t *)kmalloc(sizeof(vm_object_t));
    if (!nobj) { return NULL; }
    nobj->type = obj->type;
    nobj->refcnt = 1;
    nobj->size = obj->size;
    nobj->page_count = obj->page_count;
    nobj->pages = NULL;
    if (obj->page_count > 0 && obj->pages) {
        nobj->pages = (uint64_t *)kcalloc(obj->page_count, sizeof(uint64_t));
        if (!nobj->pages) { kfree(nobj); return NULL; }
        for (int i = 0; i < obj->page_count; i++) {
            nobj->pages[i] = obj->pages[i];
            if (obj->pages[i]) objref_claim(obj->pages[i]);
        }
    }
    return nobj;
}

/* ~~ FASE5-shm: apelido (MESMO objeto, +1 dono). Só pra MAP_SHARED:
 * as duas entradas enxergam as escritas uma da outra (wl_shm pool).
 * Quem MUTAR array compartilhado (split/release/mremap-shrink) tem
 * que passar no vm_object_unalias antes. ~~ */
vm_object_t *vm_object_alias(vm_object_t *obj) {
    if (!obj) return NULL;
    /* FASE6-flake: RMW atômico (lock-prefixed, sem libatomic). Fecha
     * lost-update entre apelidos concorrentes (free prematuro). */
    __sync_fetch_and_add(&obj->refcnt, 1);
    return obj;
}

/* ~~ FASE5-shm: desfaz apelido antes de mutar (COW): refcnt==1 volta
 * o próprio; senão solta 1 posse e devolve cópia privada. ~~ */
vm_object_t *vm_object_unalias(vm_object_t *obj) {
    if (!obj) return NULL;
    /* solta 1 posse atomicamente; se éramos o último dono (old==1),
     * desfaz e volta o próprio (old<=0 = estado corrupto: não piora) */
    if (__sync_fetch_and_sub(&obj->refcnt, 1) <= 1) {
        __sync_fetch_and_add(&obj->refcnt, 1);
        return obj;
    }
    return vm_object_share(obj);
}

/* solta UMA página do objeto (munmap parcial): só libera o frame
 * quando o último dono soltar. Retorna 1 se liberou. */
int vm_object_release_page(vm_object_t *obj, size_t pg) {
    if (!obj || !obj->pages || pg >= (size_t)obj->page_count) return 0;
    uint64_t pa = obj->pages[pg];
    obj->pages[pg] = 0;
    if (!pa) return 0;
    if (objref_release(pa)) { vm_free_page(pa); return 1; }
    return 0;
}

/* ~ essa demorou pra debugar, respeita ~ */
void vm_object_destroy(vm_object_t *obj) {
    if (!obj) return;
    /* FASE5-shm: apelido (MAP_SHARED) — só o último dono libera.
     * Snapshot tem refcnt==1 e cai no fluxo antigo (array privado,
     * frame cai no último dono via objref).
     * FASE6-flake: decremento atômico; quem levou de 1->0 libera.
     * (old<=0 = double-destroy: recusa em vez de liberar 2x.) */
    if (__sync_fetch_and_sub(&obj->refcnt, 1) != 1) return;
    /* destroy = "solto todas as MINHAS posses": snapshot tem array
     * privado; apelido (refcnt>1) já retornou acima. O frame cai no
     * último dono via objref. */
    if (obj->pages) {
        for (int i = 0; i < obj->page_count; i++) {
            uint64_t pa = obj->pages[i];
            obj->pages[i] = 0;
            if (pa && objref_release(pa)) vm_free_page(pa);
        }
        kfree(obj->pages);
    }
    kfree(obj);
}

/* ~ cuidado que essa aqui morde ~ */
static uint64_t obj_get_page(vm_object_t *obj, size_t idx) {
    if (idx >= (size_t)obj->page_count) return 0;
    if (!obj->pages[idx]) {
        uint64_t pa = vm_alloc_page();
        if (!pa) return 0;
        /* ~~ zera: a bss do libc via MAP_FIXED precisa ler 0 ~~
         * mmap_user devolve frame reciclado com lixo~ */
        uint64_t *q = (uint64_t *)(uintptr_t)pa;
        for (int i = 0; i < 512; i++) q[i] = 0;
        obj->pages[idx] = pa;
        objref_claim(pa);   /* FASE5: primeira posse é deste objeto */
    }
    return obj->pages[idx];
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
void vm_map_init(vm_map_t *map, uint64_t min, uint64_t max) {
    map->head = NULL;
    map->nentries = 0;
    map->min_offset = min;
    map->max_offset = max;
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
static int entry_covers(vm_map_entry_t *e, uint64_t start, uint64_t end) {
    return e->start <= start && e->end >= end;
}

/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
vm_map_entry_t *vm_map_find_entry(vm_map_t *map, uint64_t addr) {
    vm_map_entry_t *e = map->head;
    while (e) {
        if (addr >= e->start && addr < e->end) return e;
        e = e->next;
    }
    return NULL;
}

int vm_map_find(vm_map_t *map, vm_object_t *obj, uint64_t offset,
                uint64_t *addr, size_t size, int prot, int flags) {
    size = (size + 0xFFF) & ~0xFFFULL;
    if (size == 0) return -1;

    uint64_t hint = *addr;
    uint64_t base = hint ? hint : 0x70000000ULL;
    if (base < map->min_offset) base = map->min_offset;
    base = (base + 0xFFF) & ~0xFFFULL;

    /* FASE5: anda pelos BURACOS entre entradas (lista ordenada) em vez
     * de chutar 500 candidatos de 64KB a partir de 0x70000000. O chute
     * cobria só [0x70000000, 0x72000000): com o mapa fragmentado pelo
     * fontconfig, o 4º thread-stack do foot (2MB) falhava mesmo com
     * 1.8GB livres acima (pthread_create → ENOENT → sem render). */
    for (int pass = 0; pass < 2; pass++) {
        uint64_t cursor = (pass == 0) ? base : map->min_offset;
        uint64_t stop = (pass == 0) ? map->max_offset : base;
        for (vm_map_entry_t *e = map->head; e; e = e->next) {
            if (e->end <= cursor) continue;
            if (e->start >= stop) break;
            if (e->start > cursor) {
                if (e->start - cursor >= size &&
                    cursor + size <= map->max_offset) {
                    *addr = cursor;
                    return vm_map_fixed(map, obj, offset, cursor, size, prot, flags);
                }
                cursor = e->end;
                if (cursor >= stop) break;
            } else if (e->end > cursor) {
                cursor = e->end;
                if (cursor >= stop) break;
            }
        }
        if (stop > cursor && stop - cursor >= size &&
            cursor + size <= map->max_offset) {
            *addr = cursor;
            return vm_map_fixed(map, obj, offset, cursor, size, prot, flags);
        }
        if (!hint) break;   /* sem hint a 1ª passada já viu tudo */
    }
    return -1;
}

int vm_map_fixed(vm_map_t *map, vm_object_t *obj, uint64_t offset,
                 uint64_t addr, size_t size, int prot, int flags) {
    size = (size + 0xFFF) & ~0xFFFULL;
    if (size == 0 || (addr & 0xFFF) != 0) return -1;

    /* ~~ MAP_FIXED = SUBSTITUI (semântica Linux) ~~
     * O ld.so do glibc faz mmap(MAP_FIXED) por cima das reservas/
     * segmentos vizinhos do libc — antes o overlap devolvia -1 e o
     * loader morria com "failed to map segment from shared object".
     * Agora: remove o que estiver coberto e encurta o que encosta~ */
    {
        uint64_t new_end = addr + size;
        vm_map_entry_t *e = map->head;
        while (e) {
            vm_map_entry_t *next = e->next;
            if (e->start < new_end && e->end > addr) {
                if (e->start >= addr && e->end <= new_end) {
                    /* coberto inteiro: remove */
                    if (e->prev) e->prev->next = e->next;
                    else map->head = e->next;
                    if (e->next) e->next->prev = e->prev;
                    if (e->object) vm_object_destroy(e->object);
                    entry_free(e);
                    map->nentries--;
                } else if (e->start < addr && e->end > new_end) {
                    /* engole o range inteiro: parte em duas (com
                     * TRANSFERÊNCIA de fatia — vide vm_munmap). */
                    vm_map_entry_t *t = entry_alloc();
                    if (t) {
                        size_t a_we = (new_end - e->start + e->offset) >> 12;
                        size_t a_end = (e->end - e->start + e->offset) >> 12;
                        t->start = new_end;
                        t->end = e->end;
                        t->offset = e->offset + (new_end - e->start);
                        t->prot = e->prot;
                        t->flags = e->flags;
                        /* FASE5-shm: split MUTA o array (zera a fatia
                         * movida) — des-apelida antes p/ não arrancar
                         * páginas de entradas irmãs MAP_SHARED. */
                        if (e->object) e->object = vm_object_unalias(e->object);
                        t->object = e->object ? vm_object_split(e->object, a_we, a_end - a_we) : NULL;
                        t->prev = e;
                        t->next = e->next;
                        if (e->next) e->next->prev = t;
                        e->next = t;
                        map->nentries++;
                    }
                    e->end = addr;
                } else if (e->start < addr) {
                    e->end = addr;           /* encurta por baixo */
                } else {
                    e->start = new_end;      /* encurta por cima */
                }
            }
            e = next;
        }
    }

    vm_map_entry_t *e = entry_alloc();
    if (!e) return -1;
    e->start = addr;
    e->end = addr + size;
    /* FASE5-shm: MAP_SHARED (bit0, ex. wl_shm pool do foot) apelida o
     * MESMO objeto (escritas visíveis pros dois lados); o resto ganha
     * snapshot privado (fork/mmap anon). */
    e->object = obj ? (((flags & MAP_SHARED) ? vm_object_alias(obj) : vm_object_share(obj))) : NULL;
    e->offset = offset;
    e->prot = prot;
    e->flags = flags;

    vm_map_entry_t **pp = &map->head;
    vm_map_entry_t *prev = NULL;
    while (*pp && (*pp)->start < addr) {
        prev = *pp;
        pp = &(*pp)->next;
    }
    e->next = *pp;
    e->prev = prev;
    if (*pp) {
        (*pp)->prev = e;
    }
    *pp = e;
    map->nentries++;
    return 0;
}

/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
static int g_in_vm_remove_log = 0;
int vm_map_remove(vm_map_t *map, uint64_t start, uint64_t end) {
    if (!g_in_vm_remove_log && start < 0x40600000ULL && end > 0x40000000ULL) {
        extern int process_current_pid(void);
        g_in_vm_remove_log = 1;
        serial_puts("[RM-TEXT] pid=");
        serial_puthex((uint32_t)process_current_pid());
        serial_puts(" ");
        serial_puthex((uint32_t)(start >> 12));
        serial_puts("-");
        serial_puthex((uint32_t)(end >> 12));
        serial_puts("\r\n");
        g_in_vm_remove_log = 0;
    }
    (void)end;
    vm_map_entry_t *e = map->head;
    while (e) {
        vm_map_entry_t *next = e->next;
        if (e->start >= start && e->end <= end) {
            if (e->prev) e->prev->next = e->next;
            else map->head = e->next;
            if (e->next) e->next->prev = e->prev;
            if (e->object) vm_object_destroy(e->object);
            entry_free(e);
            map->nentries--;
        }
        e = next;
    }
    return 0;
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
int vm_map_protect(vm_map_t *map, uint64_t start, uint64_t end, int prot) {
    vm_map_entry_t *e = map->head;
    while (e) {
        if (e->start < end && e->end > start) {
            e->prot = prot;
        }
        e = e->next;
    }
    return 0;
}

/* ~ essa demorou pra debugar, respeita ~ */
int vm_map_wire(vm_map_t *map, uint64_t start, size_t size, uint64_t pml4) {
    uint64_t end = start + size;
    vm_map_entry_t *e = map->head;
    while (e) {
        if (e->start < end && e->end > start) {
            uint64_t ws = e->start > start ? e->start : start;
            uint64_t we = e->end < end ? e->end : end;
            vm_object_t *obj = e->object;
            if (!obj) { // file-backed: aloca ANON pages p/ fill em ring0 sem PF supervisor
                // was_file mantém file content via fill loop após wire
                obj = vm_object_create(e->end - e->start);
                if (!obj) return -1;
                e->object = obj;
            }
            if (obj) {
                uint64_t a_start = ws & ~0x1FFFFFULL;
                uint64_t a_end   = (we + 0x1FFFFF) & ~0x1FFFFFULL;
                for (uint64_t chunk = a_start; chunk < a_end; chunk += 0x200000) {
                    int pml4_idx = (chunk >> 39) & 0x1FF;
                    int pdpt_idx = (chunk >> 30) & 0x1FF;
                    int pd_idx   = (chunk >> 21) & 0x1FF;
                    uint64_t *p4 = (uint64_t *)(uintptr_t)pml4;
                    if (!(p4[pml4_idx] & 1)) {
                        uint64_t *p = page_frame_kernel(); if(!p) return -1;
                        serial_puts("[wt] pml4t="); serial_puthex((uint32_t)(uintptr_t)p); serial_puts("\r\n");
                        for(int i=0;i<512;i++) p[i]=0;
                        p4[pml4_idx] = (uint64_t)(uintptr_t)p | 0x07;
                    }
                    uint64_t *pdpt = (uint64_t *)(uintptr_t)(p4[pml4_idx] & ~0xFFFULL);
                    if (!(pdpt[pdpt_idx] & 1)) {
                        uint64_t *p = page_frame_kernel(); if(!p) return -1;
                        serial_puts("[wt] pdpt="); serial_puthex((uint32_t)(uintptr_t)p); serial_puts("\r\n");
                        for(int i=0;i<512;i++) p[i]=0;
                        pdpt[pdpt_idx] = (uint64_t)(uintptr_t)p | 0x07;
                    }
                    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFFULL);
                    uint64_t old = pd[pd_idx];
                    uint64_t *pt;
                    if ((old & 1) && (old & 0x80)) {
                        /* ~~ Huge page: divide em 4KB SEM derrubar o resto! ~~
                         * (antes as paginas fora do range pedido recebiam
                         *  pt[i]=0 — o MAP_FIXED do musl desmapeava o proprio
                         *  codigo/dados/BSS do processo! o XORG tomava PF em
                         *  0x4057E878 depois do mmap brk~ rssrsrs) */
                        uint64_t old_phys = old & ~0x1FFFFFULL;
                        uint64_t old_flags = old & 0x1FF;
                        old_flags &= ~0x80;
                        pt = page_frame_kernel(); if(!pt) return -1;
                        for (int i = 0; i < 512; i++) {
                            uint64_t va = chunk + (uint64_t)i * 0x1000;
                            if (va >= ws && va < we) {
                                size_t pg = (va - e->start + e->offset) >> 12;
                                uint64_t pa = obj_get_page(obj, pg);
                                if (!pa) return -1;
                                pt[i] = pa | 0x07;
                            } else {
                                pt[i] = (old_phys + (uint64_t)i * 0x1000) | old_flags;
                            }
                        }
                        pd[pd_idx] = (uint64_t)(uintptr_t)pt | 0x07;
                        __sync_synchronize();
                    } else {
                        if (!(old & 1)) {
                            pt = page_frame_kernel(); if(!pt) return -1;
                            serial_puts("[wt] pt="); serial_puthex((uint32_t)(uintptr_t)pt);
                            serial_puts(" pdi="); serial_puthex((uint32_t)pd_idx); serial_puts("\r\n");
                            for (int i = 0; i < 512; i++) pt[i] = 0;
                            pd[pd_idx] = (uint64_t)(uintptr_t)pt | 0x07;
                        } else {
                            pt = (uint64_t *)(uintptr_t)(old & ~0xFFFULL);
                        }
                        for (int i = 0; i < 512; i++) {
                            uint64_t va = chunk + (uint64_t)i * 0x1000;
                            if (va >= ws && va < we && !(pt[i] & 1)) {
                                size_t pg = (va - e->start + e->offset) >> 12;
                                uint64_t pa = obj_get_page(obj, pg);
                                if (!pa) return -1;
                                pt[i] = pa | 0x07;
                            } else if (va >= ws && va < we && (pt[i] & 1)) {
                                serial_puts("[WIRE-skip] va=");
                                serial_puthex((uint32_t)(va >> 12));
                                serial_puts(" pte=");
                                serial_puthex((uint32_t)(pt[i] & 0xFFFFFFFF));
                                serial_puts("\r\n");
                            }
                        }
                    }
                }
            }
        }
        e = e->next;
    }
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4) : "memory");
    return 0;
}

/* ~ essa demorou pra debugar, respeita ~ */
vmspace_t *vmspace_create(void) {
    vmspace_t *vs = (vmspace_t *)kmalloc(sizeof(vmspace_t));
    if (!vs) return NULL;
    vm_map_init(&vs->vm_map, 0x10000, 0x7FFFF000ULL);
    vs->pml4 = pml4_create();
    vs->stack_top = 0;
    return vs;
}

/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
void vmspace_destroy(vmspace_t *vs) {
    if (!vs) return;
    vm_map_remove(&vs->vm_map, vs->vm_map.min_offset, vs->vm_map.max_offset);
    pml4_destroy(vs->pml4);
    kfree(vs);
}

/* ~ cuidado que essa aqui morde ~ */
void vmspace_swap_pml4(vmspace_t *vs, uint64_t new_pml4) {
    vs->pml4 = new_pml4;
}

/* ~~ walk do pml4: aplica funcao nas PTEs de um range ~~
 * split_2mb_pde quebra huge pages, depois cada PTE 4KB é ajustada.
 * Retorna 0 se todas as PTEs do range foram tocadas. */
static int pml4_walk_ptes(uint64_t pml4, uint64_t start, uint64_t end,
                          void (*fn)(uint64_t *pte, uint64_t va, void *ctx),
                          void *ctx) {
    uint64_t *p4 = (uint64_t *)(uintptr_t)pml4;
    for (uint64_t chunk = start & ~0x1FFFFFULL; chunk < end; chunk += 0x200000) {
        int pml4_idx = (chunk >> 39) & 0x1FF;
        int pdpt_idx = (chunk >> 30) & 0x1FF;
        int pd_idx   = (chunk >> 21) & 0x1FF;
        if (!(p4[pml4_idx] & 1)) continue;
        uint64_t *pdpt = (uint64_t *)(uintptr_t)(p4[pml4_idx] & ~0xFFFULL);
        if (!(pdpt[pdpt_idx] & 1)) continue;
        uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFFULL);
        uint64_t pde = pd[pd_idx];
        if (!(pde & 1)) continue;
        if (pde & 0x80) {
            if (split_2mb_pde(pml4, chunk) < 0) return -1;
            pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFFULL);
        }
        uint64_t *pt = (uint64_t *)(uintptr_t)(pd[pd_idx] & ~0xFFFULL);
        for (int i = 0; i < 512; i++) {
            uint64_t va = chunk + (uint64_t)i * 0x1000;
            if (va >= start && va < end && (pt[i] & 1))
                fn(&pt[i], va, ctx);
        }
    }
    return 0;
}

/* ~~ callback: aplica R/W + U/S conforme prot ~~ */
static void pte_set_prot(uint64_t *pte, uint64_t va, void *ctx) {
    (void)va;
    int prot = *(int *)ctx;
    uint64_t f = *pte;
    if (prot & 2) f |= 0x02; else f &= ~0x02ULL;
    f |= 0x04;  /* U/S sempre ligado — userland */
    if (!(prot & 1)) f &= ~0x02ULL;  /* sem READ = sem WRITE (Linux: R implica W) */
    /* ~~ NX conservador: só marcar NX se prot NAO tiver EXEC e NAO
     * tiver READ (i.e., PROT_WRITE sem EXEC = dados puro).
     * O Xorg mapeia PROT_READ e depois executa (libffi/JIT glib);
     * marcar NX ali causa #UD (err=0x15, I/D=1) na execucao. ~~ */
    if ((prot & 4) || (prot & 1)) {
        f &= ~0x8000000000000000ULL;   /* EXEC ou READ: sem NX */
    } else {
        f |=  0x8000000000000000ULL;   /* WRITE puro: NX */
    }
    *pte = f;
}

/* ~~ callback: zera a PTE (unmap) ~~ */
static void pte_clear(uint64_t *pte, uint64_t va, void *ctx) {
    (void)ctx;
    /* FASE5: cap de log (era 1 linha por página desmapeada — 97% do
     * volume serial do boot; afogava o fontconfig do foot). */
    if (va >= 0x70000000ULL && va < 0x72000000ULL && (*pte & 1)) {
        static int nclr = 0;
        if (nclr < 30) {
            nclr++;
            serial_puts("[CLR] va=");
            serial_puthex((uint32_t)(va >> 12));
            serial_puts(" pte=");
            serial_puthex((uint32_t)(*pte & 0xFFFFFFFF));
            serial_puts("\r\n");
        }
    }
    *pte = 0;
}

/* ~~ vm_munmap: desmapeia PTEs e remove entries do range ~~ */
int vm_munmap(uint64_t addr, size_t size) {
    pcb_t *cur = process_current();
    if (!cur) return -1;
    /* ~~ quem desmapeia o TEXTO do processo?! (bug do PML4 esvaziado) ~~ */
    if (addr < 0x40600000ULL && addr + size > 0x40000000ULL) {
        serial_puts("[UNM-TEXT] pid=");
        serial_puthex((uint32_t)cur->pid);
        serial_puts(" ");
        serial_puthex((uint32_t)(addr >> 12));
        serial_puts("-");
        serial_puthex((uint32_t)((addr + size) >> 12));
        serial_puts("\r\n");
    }
    vm_map_t *map = (vm_map_t *)cur->vm_map;
    if (!map) return -1;
    size = (size + 0xFFF) & ~0xFFFULL;
    if (size == 0) return 0;
    uint64_t end = addr + size;

    /* ~~ limpa PTEs E LIBERA os frames do objeto~~
     * (antes só dava unlink: cada ciclo mmap/munmap do mallocng
     *  vazava frames até a memória acabar~ rssrsrs) */
    for (vm_map_entry_t *e = map->head; e; ) {
        vm_map_entry_t *next = e->next;
        if (e->start < end && e->end > addr) {
            if (e->object) {
                /* FASE5: solta com guarda (só libera no último dono),
                 * índice ABSOLUTO (como o wire) — o relativo liberava
                 * a página errada quando offset != 0. */
                uint64_t ws = e->start > addr ? e->start : addr;
                uint64_t we = e->end < end ? e->end : end;
                for (uint64_t va = ws & ~0xFFFULL; va < we; va += 0x1000) {
                    size_t pg = (va - e->start + e->offset) >> 12;
                    if (pg < (size_t)e->object->page_count &&
                        e->object->pages[pg]) {
                        vm_object_release_page(e->object, pg);
                    }
                }
                /* FASE5: munmap parcial NÃO destrói o objeto (os frames
                 * fora de [ws,we) continuam mapeados!). Parte a entrada
                 * com TRANSFERÊNCIA de fatia; só o caso total destrói. */
                if (e->start >= addr && e->end <= end) {
                    vm_object_destroy(e->object);
                    e->object = NULL;
                } else if (e->start >= addr) {
                    /* só-cauda [end, e->end): e vira a cauda no lugar,
                     * com a fatia transferida do objeto. */
                    size_t a_we = (end - e->start + e->offset) >> 12;
                    size_t a_end = (e->end - e->start + e->offset) >> 12;
                    vm_object_t *nt = vm_object_split(e->object, a_we, a_end - a_we);
                    vm_object_destroy(e->object);
                    e->object = nt;
                    e->offset = e->offset + (end - e->start);
                    e->start = end;
                } else {
                    /* cabeça [e->start, addr) sobrevive (+ cauda?) */
                    if (e->end > end) {
                        vm_map_entry_t *t = entry_alloc();
                        if (t) {
                            size_t a_we = (end - e->start + e->offset) >> 12;
                            size_t a_end = (e->end - e->start + e->offset) >> 12;
                            t->start = end;
                            t->end = e->end;
                            t->offset = e->offset + (end - e->start);
                            t->prot = e->prot;
                            t->flags = e->flags;
                            t->object = vm_object_split(e->object, a_we, a_end - a_we);
                            t->prev = e;
                            t->next = e->next;
                            if (e->next) e->next->prev = t;
                            e->next = t;
                            map->nentries++;
                        }
                    }
                    e->end = addr;
                }
            }
        }
        e = next;
    }

    pml4_walk_ptes(cur->pml4, addr, end, pte_clear, NULL);
    /* ~~ invlpg em TODAS as páginas! Uma só deixava TLB velha — o
     * processo acessava o frame JÁ LIBERADO sem PF (janela de uso
     * após unmap = corrupção silenciosa que o mallocng detectava
     * com a_crash()/hlt em ring3!) rssrsrs */
    for (uint64_t a = addr; a < end; a += 0x1000)
        __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
    __sync_synchronize();
    vm_map_remove(map, addr, end);
    return 0;
}

/* ~~ vm_mprotect: muda permissoes das PTEs do range ~~ */
int vm_mprotect(uint64_t addr, size_t size, int prot) {
    pcb_t *cur = process_current();
    serial_puts("[mprotect] addr="); serial_puthex((uint32_t)addr);
    serial_puts(" size="); serial_puthex((uint32_t)size);
    serial_puts(" prot="); serial_puthex((uint32_t)prot);
    serial_puts("\r\n");
    if (!cur) return -1;
    vm_map_t *map = (vm_map_t *)cur->vm_map;
    if (!map) return -1;
    size = (size + 0xFFF) & ~0xFFFULL;
    if (size == 0) return 0;
    uint64_t end = addr + size;
    if (vm_map_protect(map, addr, end, prot) < 0) return -1;
    pml4_walk_ptes(cur->pml4, addr, end, pte_set_prot, &prot);
    return 0;
}

/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
/* ~~ o endereço pertence a alguma região mapeada deste processo? ~~ */
int vm_map_covers(void *mapp, uint64_t va) {
    vm_map_t *map = (vm_map_t *)mapp;
    if (!map) return 0;
    for (vm_map_entry_t *e = map->head; e; e = e->next)
        if (va >= e->start && va < e->end) return 1;
    return 0;
}

int vm_map_count_entries(void *m) {
    vm_map_t *map = (vm_map_t *)m;
    if (!map) return -1;
    int n = 0;
    for (vm_map_entry_t *e = map->head; e; e = e->next) n++;
    return n;
}

/* ~~ callback: derruba a PTE (presente→ausente + invlpg) ~~
 * Usado pelo vm_mmap antes do wire: sem isso o wire pula páginas já
 * presentes ("WIRE-skip") e o MAP_FIXED fica com o conteúdo VELHO —
 * a bss do libc via MAP_FIXED lia lixo do arquivo e o ld.so morria ~ */
static void pte_absent(uint64_t *pte, uint64_t va, void *ctx) {
    (void)ctx;
    if (*pte & 1) {
        *pte &= ~1ULL;
        __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    }
}

int vm_mmap(uint64_t *addr, size_t size, int prot, int flags) {
    pcb_t *cur = process_current();
    if (!cur) return -1;
    vm_map_t *map = (vm_map_t *)cur->vm_map;
    if (!map) return -1;

    size = (size + 0xFFF) & ~0xFFFULL;
    if (size == 0) return -1;

    vm_object_t *obj = NULL;
    if (flags & MAP_ANON) {
        obj = vm_object_create(size);
        if (!obj) return -1;
    }

    uint64_t map_addr = *addr;
    int r;
    if (flags & MAP_FIXED) {
        r = vm_map_fixed(map, obj, 0, map_addr, size, prot, flags);
    } else {
        r = vm_map_find(map, obj, 0, &map_addr, size, prot, flags);
    }
    if (r < 0) {
        if (obj) vm_object_destroy(obj);
        return -1;
    }
    /* FASE5-shm: o insert já apelidou (SHARED) ou copiou (PRIVATE) —
     * solta a posse do criador (senão o alias nunca libera). */
    if (obj) vm_object_destroy(obj);

    /* derruba PTEs velhas do range antes de wirear as novas~ */
    pml4_walk_ptes(cur->pml4, map_addr, map_addr + size, pte_absent, NULL);

    if (vm_map_wire(map, map_addr, size, cur->pml4) < 0) {
        vm_map_remove(map, map_addr, map_addr + size);
        return -1;
    }

    *addr = map_addr;
    return 0;
}

/* ♥ vm_map.c ~ arquivo fofinho do OvsbMkM! kyun~ <3 */
