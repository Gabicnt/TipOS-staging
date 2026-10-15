/* moe moe kyun <3 */
/* moe moe kyun <3 */
/* ♥ kernel stuff ~ aqui é onde o bicho pega de verdade! kyun~
 * arquivo: memory.c ~ funcoes anotadas: 31
 */
/* ♥ memory.c ~ feito com carinho (e gambiarras) pela equipe TipOS! ♥
 * Se quebrar, a culpa é sua~ <3
 *~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*/

/* ♥ SPINLOCK ~ Lock atomico! "__sync_lock_test_and_set protege tudo~"
 * mem_lock/mem_unlock com __sync_lock_test_and_set + pause
 * Protege todas as operacoes do buddy allocator e SLUB!
 * Single-core safe ~ se travar, culpe o scheduler! moe~ */

/* ♥ Per-CPU pools ~ Fast paths sem lock pra alloc comum!
 * pcpu_pages: pool de paginas soltas (order 0) pra alloc rapida
 * pcpu_objects[cache]: pool de objetos pre-free pra cada kmem_cache
 * Quando o pool enche, drena pro slab. Quando esvazia, recarrega do slab.
 * Tudo single-core por enquanto, mas a estrutura é SMP-ready! >_< */

#include "memory.h"
#include "serial.h"
#include "process.h"
#include <stdint.h>

#define HEAP_PHYS    BUDDY_HEAP_PHYS
#define HEAP_SIZE    BUDDY_HEAP_SIZE
#define FRAME_SIZE   4096u
#define FRAME_SHIFT  12u
#define FRAME_COUNT  (HEAP_SIZE / FRAME_SIZE)
#define MAX_ORDER    16u

#define FRAME_ALLOC  0x80u
#define FRAME_USER   0x40u
#define FRAME_ORDER  0x3Fu

#define SLAB_MAGIC   0x534C4142u
#define CACHE_COUNT  8u

/* Per-CPU pool constants */
#define PCPU_PAGE_POOL   32
#define PCPU_OBJ_CACHE   16
#define PCPU_OBJ_REFILL  8

static uint8_t frames[FRAME_COUNT];

typedef struct free_block {
    struct free_block *next;
    struct free_block *prev;
} free_block_t;

static free_block_t *free_area[MAX_ORDER + 1];

typedef struct kmem_cache kmem_cache_t;

typedef struct slab {
    uint32_t      magic;
    uint32_t      in_use;
    uint16_t      total;
    void         *freelist;
    struct slab  *next;
    struct slab  *prev;
    kmem_cache_t *cache;
} slab_t;

struct kmem_cache {
    size_t  obj_size;
    slab_t *slabs;
    int     obj_per_slab;
    int     id;
};

static kmem_cache_t kmalloc_caches[CACHE_COUNT];

/* Per-CPU page pool */
static struct {
    void *pages[PCPU_PAGE_POOL];
    int count;
} pcpu_pages;

/* Per-CPU object freelists for each cache */
static struct {
    void *objs[PCPU_OBJ_CACHE];
    int count;
} pcpu_objects[CACHE_COUNT];

static volatile int memlock = 0;

/* ~ essa demorou pra debugar, respeita ~ */
static inline void mem_lock(void) {
    while (__sync_lock_test_and_set(&memlock, 1))
        __asm__ volatile("pause");
    __asm__ volatile("" ::: "memory");
}

/* ~ essa demorou pra debugar, respeita ~ */
static inline void mem_unlock(void) {
    __asm__ volatile("" ::: "memory");
    __sync_lock_release(&memlock);
}

/* ~ essa demorou pra debugar, respeita ~ */
static inline size_t addr_to_frame(uint64_t addr) {
    return (addr - HEAP_PHYS) >> FRAME_SHIFT;
}

/* ~~ watchpoints de frame (fase 7): PAs rastreados. Setados por
 * pml4_map_phys/clone_identity_tables quando criam o PD da região
 * 3–4GB. Qualquer alloc/free desses frames imprime [watch] + caller. ~~ */
uint64_t g_watch_frames[8];
int g_watch_count = 0;
void watch_add(uint64_t pa) {
    for (int i = 0; i < g_watch_count; i++)
        if (g_watch_frames[i] == pa) return;
    if (g_watch_count < 8) g_watch_frames[g_watch_count++] = pa;
}
static inline void watch_hit(const char *tag, void *ptr) {
    int hit = 0;
    for (int i = 0; i < g_watch_count; i++)
        if (g_watch_frames[i] == (uint64_t)ptr) { hit = 1; break; }
    if (!hit) return;
    extern void serial_puts(const char *s);
    extern void serial_puthex(uint32_t v);
    serial_puts(tag);
    serial_puthex((uint32_t)((uint64_t)ptr >> 32));
    serial_puthex((uint32_t)(uint64_t)ptr);
    serial_puts(" ret=");
    serial_puthex((uint32_t)(uint64_t)__builtin_return_address(1));
    serial_puts("\r\n");
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
static inline uint64_t frame_to_addr(size_t idx) {
    return HEAP_PHYS + (idx << FRAME_SHIFT);
}

/* FASE5-DBG temporário: anel forense do buddy (quem alocou/liberou o
 * quê). Descarregado no EXC# de ring 0. Sem serial no caminho quente. */
uint64_t dbg_target_pa = 0;
#define BUDDY_RING_N 2048
struct buddy_ev { uint64_t addr; uint64_t ret; uint8_t op; uint8_t ord; };
static struct buddy_ev buddy_ring[BUDDY_RING_N];
static unsigned buddy_ring_i = 0;
static inline void buddy_log(uint8_t op, int ord, uint64_t addr) {
    unsigned i = buddy_ring_i++ & (BUDDY_RING_N - 1);
    buddy_ring[i].op = op;
    buddy_ring[i].ord = (uint8_t)ord;
    buddy_ring[i].addr = addr;
    buddy_ring[i].ret = (uint64_t)__builtin_return_address(0);
}
void buddy_dump_ring(void) {
    extern void serial_puts(const char *s);
    extern void serial_puthex(uint32_t v);
    extern free_block_t *free_area[];
    serial_puts("[bring] ultimas 48 ops (op 1=alloc 0=free):\r\n");
    unsigned n = buddy_ring_i < 48 ? buddy_ring_i : 48;
    for (unsigned k = 0; k < n; k++) {
        unsigned i = (buddy_ring_i - 1 - k) & (BUDDY_RING_N - 1);
        serial_puts(k == 0 ? "  +" : "  -");
        serial_puthex(k);
        serial_puts(k ? " " : " ");
        serial_puts(buddy_ring[i].op ? "A" : "F");
        serial_puthex(buddy_ring[i].ord);
        serial_puts(" @"); serial_puthex((uint32_t)buddy_ring[i].addr);
        serial_puts(" ret="); serial_puthex((uint32_t)buddy_ring[i].ret);
        serial_puts("\r\n");
    }
    serial_puts("[bring] free_area heads:\r\n");
    for (int o = 0; o <= 16; o++) {
        free_block_t *h = free_area[o];
        if (!h) continue;
        serial_puts("  ord="); serial_puthex((uint32_t)o);
        serial_puts(" head="); serial_puthex((uint32_t)(uintptr_t)h);
        serial_puts(" next="); serial_puthex(h->next ? (uint32_t)(uintptr_t)h->next : 0);
        serial_puts(" prev="); serial_puthex(h->prev ? (uint32_t)(uintptr_t)h->prev : 0);
        serial_puts("\r\n");
    }
    /* FASE5-DBG: quem mapeia o frame 0 (0xA00000)? varre as tabelas do
     * CR3 atual procurando folha == 0xA00000 com U/S. */
    {
        extern uint64_t dbg_target_pa;
        uint64_t target = dbg_target_pa ? dbg_target_pa : 0xA00000ULL;
        serial_puts("[bring] scan pa="); serial_puthex((uint32_t)target);
        serial_puts("\r\n");
        /* quadros 0..15 do bitmap + conteúdo do frame 0 */
        {
            extern uint8_t frames[];
            for (int fi = 0; fi < 8; fi++) {
                serial_puts(" f"); serial_puthex((uint32_t)fi);
                serial_puts("="); serial_puthex((uint32_t)frames[fi]);
            }
            serial_puts("\r\n");
            volatile uint64_t *fz = (volatile uint64_t *)(uintptr_t)target;
            serial_puts(" frame0:"); serial_puthex((uint32_t)fz[0]);
            serial_puts(" "); serial_puthex((uint32_t)fz[1]);
            serial_puts(" "); serial_puthex((uint32_t)fz[2]);
            serial_puts(" "); serial_puthex((uint32_t)fz[3]);
            serial_puts("\r\n");
        }
        /* lista ord-14 inteira (cabeca + 7) */
        {
            int shown = 0;
            free_block_t *nd = free_area[14];
            while (nd && shown < 8) {
                serial_puts(" nd="); serial_puthex((uint32_t)(uintptr_t)nd);
                /* link valido? dentro da RAM e alinhado */
                uint64_t nn = (uint64_t)(uintptr_t)nd->next;
                uint64_t np = (uint64_t)(uintptr_t)nd->prev;
                if (nn < BUDDY_HEAP_PHYS || nn >= (uint64_t)BUDDY_HEAP_PHYS + BUDDY_HEAP_SIZE || (nn & 0xFFFULL)) break;
                if (np < BUDDY_HEAP_PHYS || np >= (uint64_t)BUDDY_HEAP_PHYS + BUDDY_HEAP_SIZE || (np & 0xFFFULL)) {
                    serial_puts(" POISON");
                    serial_puts("\r\n");
                    break;
                }
                nd = nd->next;
                shown++;
            }
            serial_puts(" lista-fim\r\n");
        }
        /* varre TODOS os processos (não só o CR3 atual) */
        for (int ps = 0; ps < MAX_PROC; ps++) {
            if (pcb_table[ps].state == PROC_EMPTY) continue;
            uint64_t *p4 = (uint64_t *)(uintptr_t)pcb_table[ps].pml4;
            if (!p4) continue;
            serial_puts(" pid="); serial_puthex((uint32_t)pcb_table[ps].pid);
            serial_puts("\r\n");
            for (int i4 = 0; i4 < 512; i4++) {
            uint64_t e4 = p4[i4];
            if (!(e4 & 1)) continue;
            uint64_t *p3 = (uint64_t *)(uintptr_t)(e4 & 0x000FFFFFFFFFF000ULL);
            for (int i3 = 0; i3 < 512; i3++) {
                uint64_t e3 = p3[i3];
                if (!(e3 & 1) || (e3 & 0x80)) continue;
                uint64_t *p2 = (uint64_t *)(uintptr_t)(e3 & 0x000FFFFFFFFFF000ULL);
                for (int i2 = 0; i2 < 512; i2++) {
                    uint64_t e2 = p2[i2];
                    if (!(e2 & 1)) continue;
                    if (e2 & 0x80) {
                        if ((e2 & 0x000FFFFFE00000ULL) <= target &&
                            target < ((e2 & 0x000FFFFFE00000ULL) + 0x200000)) {
                            uint64_t va = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) | ((uint64_t)i2 << 21);
                            serial_puts("  HUGE va="); serial_puthex((uint32_t)va);
                            serial_puts(" e="); serial_puthex((uint32_t)e2);
                            serial_puts("\r\n");
                        }
                        continue;
                    }
                    uint64_t *p1 = (uint64_t *)(uintptr_t)(e2 & 0x000FFFFFFFFFF000ULL);
                    for (int i1 = 0; i1 < 512; i1++) {
                        uint64_t e1 = p1[i1];
                        if ((e1 & 1) && (e1 & 4) &&
                            (e1 & 0x000FFFFFFFFFF000ULL) == target) {
                            uint64_t va = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                          ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);
                            serial_puts("  USE va="); serial_puthex((uint32_t)va);
                            serial_puts(" e="); serial_puthex((uint32_t)e1);
                            serial_puts("\r\n");
                        }
                    }
                    }
                }
            }
        }
        serial_puts("[bring] scan fim\r\n");
    }
}

/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
static void list_add(free_block_t **head, free_block_t *blk) {
    blk->next = *head;
    blk->prev = NULL;
    if (*head) (*head)->prev = blk;
    *head = blk;
}

/* ~ cuidado que essa aqui morde ~ */
static void list_remove(free_block_t **head, free_block_t *blk) {
    if (blk->prev)
        blk->prev->next = blk->next;
    else
        *head = blk->next;
    if (blk->next)
        blk->next->prev = blk->prev;
}

/* FASE5: bloco 100% livre? (sem ALLOC nem USER em nenhum frame).
 * Sub-frames de blocos vivos sempre carregam ALLOC (ver alloc
 * abaixo) — sem isso um vizinho "velho" parecia livre e o split
 * anotava/linkava por cima de memória viva. */
static int block_is_free(size_t block_idx, int order) {
    size_t n = 1UL << order;
    if (block_idx + n > FRAME_COUNT) return 0;
    for (size_t k = 0; k < n; k++) {
        uint8_t f = frames[block_idx + k];
        if (f & (FRAME_ALLOC | FRAME_USER)) return 0;
    }
    return 1;
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
static void *buddy_alloc(int order, int user) {
    if (order < 0 || order > MAX_ORDER) return NULL;
    mem_lock();
    int found_order = -1;
    free_block_t *block = NULL;
    size_t block_idx = 0;
    /* FASE5: pega o primeiro bloco da ordem que estiver 100% livre e
     * alinhado (pula links podres / blocos contaminados em vez de
     * confiar cegamente na cabeça da lista). */
    for (int o = order; o <= MAX_ORDER; o++) {
        int scanned = 0;
        free_block_t *b = free_area[o];
        while (b) {
            if (++scanned > 8192) break;   /* lista podre: desiste */
            uint64_t ba = (uint64_t)b;
            /* ponteiro fora do heap/alinhamento = lista podre além
             * daqui: PARA (ler b->next disso daria #PF em ring 0). */
            if (ba < HEAP_PHYS || ba >= HEAP_PHYS + HEAP_SIZE ||
                (ba & (FRAME_SIZE - 1))) break;
            /* links precisam ser sãos (cabeça: prev NULL + next NULL
             * ou frame alinhado do heap): list_remove por link podre
             * escreve em endereço selvagem (#GP). */
            uint64_t bn = (uint64_t)(uintptr_t)b->next;
            uint64_t bp = (uint64_t)(uintptr_t)b->prev;
            int link_ok = !bp &&
                (!bn || (bn >= HEAP_PHYS && bn < HEAP_PHYS + HEAP_SIZE && !(bn & 0xFFFULL)));
            size_t bi = addr_to_frame(ba);
            if (link_ok && ((bi >> o) << o) == bi && block_is_free(bi, o)) {
                found_order = o;
                block = b;
                block_idx = bi;
                break;
            }
            if (!link_ok) break;   /* resto da lista incoerente: próxima ordem */
            b = b->next;
        }
        if (block) break;
    }
    if (!block) { serial_puts("ba: no free\r\n"); mem_unlock(); buddy_log(1, order, 0); return NULL; }
    list_remove(&free_area[found_order], block);

    for (int o = found_order; o > order; o--) {
        size_t buddy_idx = block_idx ^ (1UL << (o - 1));
        free_block_t *buddy = (free_block_t *)frame_to_addr(buddy_idx);
        frames[buddy_idx] = (uint8_t)(o - 1);
        list_add(&free_area[o - 1], buddy);
    }

    frames[block_idx] = (uint8_t)(order | FRAME_ALLOC);
    /* FASE5: marca ALLOC|USER em TODOS os frames do bloco (ordem só na
     * cabeça). Antes os sub-frames ficavam com order velha sem ALLOC:
     * pareciam livres pra coalescência/split e o buddy anotava por
     * cima de memória viva. (frame_is_user continua valendo p/ USER.) */
    for (size_t k = 0; k < (1UL << order); k++)
        frames[block_idx + k] = (uint8_t)(FRAME_ALLOC | FRAME_USER);
    frames[block_idx] = (uint8_t)(order | FRAME_ALLOC | FRAME_USER);
    if (!user) {
        for (size_t k = 0; k < (1UL << order); k++)
            frames[block_idx + k] &= (uint8_t)~FRAME_USER;
    }
    mem_unlock();
    {
        void *res = (void *)frame_to_addr(block_idx);
        watch_hit("[watch] buddy-alloc ", res);
        buddy_log(1, order, (uint64_t)(uintptr_t)res);
        return res;
    }
}

/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
static void buddy_free(void *addr, int order) {
    if (!addr || order < 0 || order > MAX_ORDER) return;
    uint64_t phys = (uint64_t)addr;
    watch_hit("[watch] buddy-free ", addr);
    if (phys < HEAP_PHYS || phys >= HEAP_PHYS + HEAP_SIZE) return;
    if (phys & (FRAME_SIZE - 1)) return;
    buddy_log(0, order, (uint64_t)phys);
    /* ~~ debug fase 7: devolvendo pra buddy um frame que ainda é de
     * SLAB ou está no pool pcpu? = double-free → a página vira kstack
     * por cima de slab vivo e a freelist vira lixo (0x23). ~~ */
    {
        static int dfc;
        if (dfc < 8) {
            int hit = 0;
            for (unsigned ci = 0; ci < CACHE_COUNT && !hit; ci++)
                for (slab_t *sl = kmalloc_caches[ci].slabs; sl && !hit; sl = sl->next)
                    if ((uint64_t)sl == phys) hit = 1;
            if (!hit)
                for (int pi = 0; pi < pcpu_pages.count && !hit; pi++)
                    if ((uint64_t)pcpu_pages.pages[pi] == phys) hit = 1;
            if (hit) {
                extern void serial_puthex(uint32_t v);
                dfc++;
                serial_puts("[BUD-FREE-LIVE] pa="); serial_puthex((uint32_t)phys);
                serial_puts(" ord="); serial_puthex((uint32_t)order);
                serial_puts(" ret="); serial_puthex((uint32_t)(uint64_t)__builtin_return_address(0));
                serial_puts("\r\n");
            }
        }
    }
    mem_lock();
    size_t idx = addr_to_frame(phys);
    frames[idx] = (uint8_t)order;
    /* FASE5: limpa as marcas dos sub-frames do bloco (ALLOC|USER de
     * quando estava vivo). Sem isso eles pareciam "vivos" pra sempre:
     * nada voltava a coalescer e o pool fragmentava até o primeiro
     * alloc grande falhar (kmalloc de 8MB do FOOT travava o boot). */
    {
        size_t n = 1UL << order;
        if (idx + n <= FRAME_COUNT)
            for (size_t k = 1; k < n; k++) frames[idx + k] = 0;
    }
    while (order < MAX_ORDER) {
        size_t buddy_idx = idx ^ (1UL << order);
        /* FASE5: o vizinho precisa estar LIVRE de verdade (sem ALLOC e
         * sem USER, ordem igual). Sub-frames de blocos vivos carregam
         * order velha + USER (o alloc só marca a cabeça com ALLOC) —
         * sem o gate USER, o merge engolia página viva pra freelist
         * (munmap em massa do fontconfig no foot) e o próximo alloc
         * grande tropeçava em links com bytes de usuário (#GP). */
        if ((frames[buddy_idx] & (FRAME_ALLOC | FRAME_USER | FRAME_ORDER)) == (uint8_t)order) {
            free_block_t *buddy = (free_block_t *)frame_to_addr(buddy_idx);
            list_remove(&free_area[order], buddy);
            idx = (idx < buddy_idx) ? idx : buddy_idx;
            order++;
            frames[idx] = (uint8_t)order;
        } else {
            break;
        }
    }
    free_block_t *block = (free_block_t *)frame_to_addr(idx);
    list_add(&free_area[order], block);
    mem_unlock();
}

/* ~ cuidado que essa aqui morde ~ */
static void *slab_alloc(kmem_cache_t *cache) {
    if (!cache) return NULL;
    slab_t *s = cache->slabs;
    while (s) {
        if (s->freelist) {
            void *obj = s->freelist;
            /* ~~ debug fase 7: freelist aponta pra FORA da página do slab? ~~ */
            if (((uint64_t)obj & ~0xFFFULL) != (uint64_t)s) {
                extern void serial_puts(const char *s);
                extern void serial_puthex(uint32_t v);
                serial_puts("[SLAB-BAD] slab="); serial_puthex((uint32_t)s);
                serial_puts(" free="); serial_puthex((uint32_t)obj);
                serial_puts(" cid="); serial_puthex((uint32_t)cache->id);
                serial_puts(" ret0="); serial_puthex((uint32_t)(uint64_t)__builtin_return_address(0));
                serial_puts(" ret1="); serial_puthex((uint32_t)(uint64_t)__builtin_return_address(1));
                serial_puts("\r\n  chain:");
                { int n = 0; slab_t *w = cache->slabs;
                  while (w && n < 16) {
                      serial_puts(" "); serial_puthex((uint32_t)w);
                      serial_puts(":"); serial_puthex(w->magic);
                      w = w->next; n++; }
                  if (w) serial_puts(" ...LOOP/OVF");
                }
                serial_puts("\r\n");
                s->freelist = NULL;
                s = s->next;
                continue;
            }
            s->freelist = *(void **)obj;
            s->in_use++;
            return obj;
        }
        s = s->next;
    }
    void *page = buddy_alloc(0, 0);
    if (!page) return NULL;
    s = (slab_t *)page;
    s->magic   = SLAB_MAGIC;
    s->in_use  = 0;
    s->total   = cache->obj_per_slab;
    s->cache   = cache;
    s->next    = cache->slabs;
    s->prev    = NULL;
    if (cache->slabs) cache->slabs->prev = s;
    cache->slabs = s;
    char *data = (char *)s + sizeof(slab_t);
    char *end  = (char *)s + FRAME_SIZE;
    void *head = NULL;
    int count = 0;
    while (data + cache->obj_size <= end) {
        *(void **)data = head;
        head = data;
        data += cache->obj_size;
        count++;
    }
    s->freelist = head;
    s->total = count;
    void *obj = s->freelist;
    s->freelist = *(void **)obj;
    s->in_use = 1;
    return obj;
}

/* ~ cuidado que essa aqui morde ~ */
static void slab_free(void *ptr) {
    if (!ptr) return;
    slab_t *s = (slab_t *)((uint64_t)ptr & ~(FRAME_SIZE - 1));
    if (s->magic != SLAB_MAGIC) return;
    *(void **)ptr = s->freelist;
    s->freelist = ptr;
    s->in_use--;
    if (s->in_use == 0) {
        kmem_cache_t *cache = s->cache;
        if (s->next) s->next->prev = s->prev;
        if (s->prev) s->prev->next = s->next;
        else if (cache) cache->slabs = s->next;
        buddy_free(s, 0);
    }
}

/* ~~ Per-CPU page pool: fast path para alloc/free de pages ~~ */

/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
static void *pcpu_page_alloc(void) {
    if (pcpu_pages.count > 0) {
        void *p = pcpu_pages.pages[--pcpu_pages.count];
        watch_hit("[watch] pcpu-alloc ", p);
        return p;
    }
    void *p = buddy_alloc(0, 0);
    watch_hit("[watch] pcpu-alloc(buddy) ", p);
    return p;
}

/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
static void pcpu_page_free(void *p) {
    watch_hit("[watch] pcpu-free ", p);
    if (pcpu_pages.count < PCPU_PAGE_POOL)
        pcpu_pages.pages[pcpu_pages.count++] = p;
    else
        buddy_free(p, 0);
}

/* ~~ Per-CPU slab cache: refill e drain ~~ */

/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
static void *pcpu_slab_alloc(kmem_cache_t *cache) {
    int id = cache->id;
    if (pcpu_objects[id].count > 0) {
        void *o = pcpu_objects[id].objs[--pcpu_objects[id].count];
        /* ~~ debug fase 7: objeto do pcpu cuja página não é mais slab? ~~ */
        {
            slab_t *os = (slab_t *)((uint64_t)o & ~(FRAME_SIZE - 1));
            if (os->magic != SLAB_MAGIC || os->cache != cache) {
                extern void serial_puts(const char *s);
                extern void serial_puthex(uint32_t v);
                serial_puts("[PCPU-POISON] obj="); serial_puthex((uint32_t)o);
                serial_puts(" magic="); serial_puthex(os->magic);
                serial_puts(" id="); serial_puthex(id);
                serial_puts(" ret="); serial_puthex((uint32_t)(uint64_t)__builtin_return_address(0));
                serial_puts("\r\n");
            }
        }
        return o;
    }
    void *obj = slab_alloc(cache);
    if (!obj) return NULL;
    /* refill: grab extra objects from same slab */
    slab_t *s = (slab_t *)((uint64_t)obj & ~(FRAME_SIZE - 1));
    int want = PCPU_OBJ_REFILL;
    while (want > 0 && s->freelist) {
        void *extra = s->freelist;
        s->freelist = *(void **)extra;
        s->in_use++;
        if (pcpu_objects[id].count < PCPU_OBJ_CACHE)
            pcpu_objects[id].objs[pcpu_objects[id].count++] = extra;
        else
            break;
        want--;
    }
    return obj;
}

/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
static void pcpu_slab_free(void *ptr, slab_t *s) {
    kmem_cache_t *cache = s->cache;
    int id = cache->id;
    if (pcpu_objects[id].count < PCPU_OBJ_CACHE) {
        pcpu_objects[id].objs[pcpu_objects[id].count++] = ptr;
        return;
    }
    /* drain all per-CPU objects back to their respective slabs */
    while (pcpu_objects[id].count > 0) {
        void *obj = pcpu_objects[id].objs[--pcpu_objects[id].count];
        slab_t *os = (slab_t *)((uint64_t)obj & ~(FRAME_SIZE - 1));
        *(void **)obj = os->freelist;
        os->freelist = obj;
        os->in_use--;
        if (os->in_use == 0) {
            kmem_cache_t *oc = os->cache;
            if (os->next) os->next->prev = os->prev;
            if (os->prev) os->prev->next = os->next;
            else if (oc) oc->slabs = os->next;
            buddy_free(os, 0);
        }
    }
    /* free current object directly to its slab */
    *(void **)ptr = s->freelist;
    s->freelist = ptr;
    s->in_use--;
    if (s->in_use == 0) {
        if (s->next) s->next->prev = s->prev;
        if (s->prev) s->prev->next = s->next;
        else if (cache) cache->slabs = s->next;
        buddy_free(s, 0);
    }
}

/* ~~ free_range ~ adiciona o intervalo [start,end) (índices de frame) na
 * buddy em blocos alinhados à maior potência de 2 que couber. O
 * memory_init usa isso pra esculpir a imagem do kernel FORA do heap ~~ */
static void free_range(size_t start, size_t end) {
    while (start < end) {
        int order = 0;
        for (int o = MAX_ORDER; o >= 0; o--) {
            size_t sz = 1UL << o;
            if (sz <= end - start && (start & (sz - 1)) == 0) {
                order = o;
                break;
            }
        }
        size_t sz = 1UL << order;
        frames[start] = (uint8_t)order;
        free_block_t *blk = (free_block_t *)frame_to_addr(start);
        blk->next = NULL;
        blk->prev = NULL;
        list_add(&free_area[order], blk);
        start += sz;
    }
}

/* ~~ Inicializando~~ torce pra não dar panic! */
/* ~ cuidado que essa aqui morde ~ */
void memory_init(void) {
    mem_lock();
    for (int i = 0; i <= MAX_ORDER; i++)
        free_area[i] = NULL;
    for (size_t i = 0; i < FRAME_COUNT; i++)
        frames[i] = 0;
    /* ~~ RESERVA A IMAGEM DO KERNEL ~~
     * O heap buddy [0xA00000..0x1FE00000) COBRE o kernel em 0x08000000!
     * Sem essa reserva, estourados os ~118MB baixos (Xorg+fvwm2+xterm+
     * fork eager), o buddy entregava frames do PRÓPRIO .text: o
     * buddy_alloc gravou a freelist em 0x08004000 — por cima das
     * instruções que estava executando → PF em loop (RIP=0x0800400E). ~~ */
    {
        extern char __bss_end[];
        uint64_t kstart = 0x08000000;
        uint64_t kend = ((uint64_t)(uintptr_t)__bss_end + 0xFFFULL) & ~0xFFFULL;
        size_t k0 = (size_t)((kstart - HEAP_PHYS) >> FRAME_SHIFT);
        size_t k1 = (size_t)((kend - HEAP_PHYS) >> FRAME_SHIFT);
        if (k1 > FRAME_COUNT) k1 = FRAME_COUNT;
        for (size_t ki = k0; ki < k1; ki++)
            frames[ki] = (uint8_t)(FRAME_ALLOC | 0x20); /* ordem 32 > MAX: buddy_free recusa */
        free_range(0, k0);
        free_range(k1, FRAME_COUNT);
    }
    static const size_t sizes[CACHE_COUNT] = {
        16, 32, 64, 128, 256, 512, 1024, 2048
    };
    for (int i = 0; i < CACHE_COUNT; i++) {
        kmalloc_caches[i].obj_size    = sizes[i];
        kmalloc_caches[i].slabs       = NULL;
        kmalloc_caches[i].obj_per_slab =
            (int)((FRAME_SIZE - sizeof(slab_t)) / sizes[i]);
        kmalloc_caches[i].id          = i;
    }
    pcpu_pages.count = 0;
    for (int i = 0; i < CACHE_COUNT; i++)
        pcpu_objects[i].count = 0;
    mem_unlock();
}

/* ~ essa demorou pra debugar, respeita ~ */
void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    if (size < 16) size = 16;
    if (size >= 2048) {
        size_t pages = (size + FRAME_SIZE - 1) / FRAME_SIZE;
        int order = 0;
        while ((1UL << order) < pages) order++;
        if (order == 0) return pcpu_page_alloc();
        return buddy_alloc(order, 0);
    }
    for (int i = 0; i < CACHE_COUNT; i++) {
        if (kmalloc_caches[i].obj_size >= size)
            return pcpu_slab_alloc(&kmalloc_caches[i]);
    }
    return NULL;
}

/* ~~ Liberando memória~~ não esquece de pagar o aluguel! */
/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
void kfree(void *ptr) {
    if (!ptr) return;
    uint64_t addr = (uint64_t)ptr;
    if (addr < HEAP_PHYS || addr >= HEAP_PHYS + HEAP_SIZE) return;
    slab_t *s = (slab_t *)(addr & ~(FRAME_SIZE - 1));
    if (s->magic == SLAB_MAGIC) {
        if (s->cache) {
            /* ~~ debug fase 7: valida o cache antes de indexar pcpu_objects
             * (id fora de 0..7 escrevia FORA do array e corrompia o BSS) ~~ */
            kmem_cache_t *cchk = s->cache;
            if ((uint64_t)cchk < (uint64_t)kmalloc_caches ||
                (uint64_t)cchk >= (uint64_t)&kmalloc_caches[CACHE_COUNT]) {
                extern void serial_puthex(uint32_t v);
                serial_puts("[SLAB-BADID] ptr="); serial_puthex((uint32_t)ptr);
                serial_puts(" cache="); serial_puthex((uint32_t)(uint64_t)cchk);
                serial_puts(" ret="); serial_puthex((uint32_t)(uint64_t)__builtin_return_address(0));
                serial_puts("\r\n");
                return;
            }
            int id = cchk->id;
            if (id < 0 || id >= (int)CACHE_COUNT) {
                extern void serial_puthex(uint32_t v);
                serial_puts("[SLAB-BADID] ptr="); serial_puthex((uint32_t)ptr);
                serial_puts(" cache="); serial_puthex((uint32_t)(uint64_t)s->cache);
                serial_puts(" id="); serial_puthex((uint32_t)id);
                serial_puts(" ret="); serial_puthex((uint32_t)(uint64_t)__builtin_return_address(0));
                serial_puts("\r\n");
                return;
            }
            /* dup free no pcpu? (dois donos do mesmo objeto → alias) */
            for (int di = 0; di < pcpu_objects[id].count; di++) {
                if (pcpu_objects[id].objs[di] == ptr) {
                    extern void serial_puthex(uint32_t v);
                    serial_puts("[KFREE-DUP] ptr="); serial_puthex((uint32_t)ptr);
                    serial_puts(" id="); serial_puthex((uint32_t)id);
                    serial_puts(" ret="); serial_puthex((uint32_t)(uint64_t)__builtin_return_address(0));
                    serial_puts("\r\n");
                    return;
                }
            }
            pcpu_slab_free(ptr, s);
            return;
        }
        slab_free(ptr);
        return;
    }
    if (addr & (FRAME_SIZE - 1)) return;
    size_t idx = addr_to_frame(addr);
    if (idx >= FRAME_COUNT) return;
    if (!(frames[idx] & FRAME_ALLOC)) return;
    int order = frames[idx] & FRAME_ORDER;
    if (order == 0) { pcpu_page_free(ptr); return; }
    buddy_free(ptr, order);
}

/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
void *kcalloc(size_t count, size_t size) {
    size_t total = count * size;
    void *p = kmalloc(total);
    if (p) {
        size_t alloc_size = total;
        if (total < 2048) {
            for (int i = 0; i < CACHE_COUNT; i++) {
                if (kmalloc_caches[i].obj_size >= total) {
                    alloc_size = kmalloc_caches[i].obj_size;
                    break;
                }
            }
        } else {
            size_t pages = (total + FRAME_SIZE - 1) / FRAME_SIZE;
            int order = 0;
            while ((1UL << order) < pages) order++;
            alloc_size = (1UL << order) * FRAME_SIZE;
        }
        for (size_t i = 0; i < alloc_size; i++)
            ((uint8_t *)p)[i] = 0;
    }
    return p;
}

/* ~~ page_frame_kernel ~~
 * Frame pra TABELA de página (PD/PDP/PDPT/PT). NÃO é memória de
 * usuário: se saísse do mmap_user (FRAME_USER), o fork copiava a
 * tabela como se fosse dado e REBINDAVA o PTE do filho pro frame
 * cópia — o kernel passava a editar uma sombra enquanto o hardware
 * caminhava na tabela original → heap/slab divergiam e o kmalloc
 * engolia lixo (freelist 0x23 → #GP em ring 0 no xterm). ~~ */
void *page_frame_kernel(void) {
    uint8_t *p = (uint8_t *)pcpu_page_alloc();
    /* ~~ ZERA: antes usávamos mmap_user (que zera). Sem isso as tabelas
     * herdavam lixo do pool — new_pdp pulava entradas ausentes com
     * `continue` e o filho ficava com PDP sujo → triple fault no fork. ~~ */
    if (p) for (size_t i = 0; i < 4096; i++) p[i] = 0;
    return p;
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
void *mmap_user(void *addr, size_t length, int prot, int flags) {
    (void)addr; (void)prot; (void)flags;
    size_t pages = (length + FRAME_SIZE - 1) / FRAME_SIZE;
    if (pages == 0) pages = 1;
    int order = 0;
    while ((1UL << order) < pages) order++;
    void *result = buddy_alloc(order, 1);
    if (result) {
        /* ~~ POSIX: MAP_ANONYMOUS retorna memória ZERADA!~~
         * Sem isso o musl mallocng lê lixo de alocações antigas
         * e o Xorg crasha no enframe() durante init de screen~ */
        uint8_t *p = (uint8_t *)result;
        size_t total = pages * FRAME_SIZE;
        for (size_t i = 0; i < total; i++) p[i] = 0;
    }
    return result;
}

/* ~~ frame_is_user ~~ 1 se o frame veio de alocação de USUÁRIO
 * (mmap_user). O fork copia só esses: memória de kernel (heap kmalloc,
 * kstack, tabelas) é idêntica nos dois processos de verdade. */
int frame_is_user(uint64_t pa) {
    /* FASE5: o índice precisa descontar HEAP_PHYS como addr_to_frame
     * faz (buddy gerencia [HEAP_PHYS, HEAP_PHYS+HEAP_SIZE)). Sem isso
     * líamos o slot de outro frame (+10MB de deslocamento): página de
     * usuário virava "kernel" aleatoriamente → o fork COMPARTILHAVA a
     * stack em vez de copiar → pai e filho pisavam na mesma pilha
     * (*** stack smashing detected *** no fork do foot/FORK_TEST). */
    if (pa < HEAP_PHYS) return 0;
    size_t idx = (size_t)((pa - HEAP_PHYS) >> FRAME_SHIFT);
    if (idx >= FRAME_COUNT) return 0;
    return (frames[idx] & FRAME_USER) ? 1 : 0;
    /* ~~ O back-scan de 16 frames foi REMOVIDO: ele marcava como
     * "user" qualquer frame vizinho de uma alocação user (slab logo
     * após uma página mmap_user) — o fork copiava PÁGINA DE HEAP DO
     * KERNEL e rebindava o PTE do filho pro frame sombra → corrupção
     * da freelist do kmalloc (xterm morria com #GP). buddy_alloc já
     * marca todos os frames do bloco, então o back-scan é inútil. ~~ */
}

/* ~~ munmap_user ~~ */
/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
int munmap_user(void *addr, size_t length) {
    (void)length;
    if (!addr) return 0;
    uint64_t a = (uint64_t)addr;
    watch_hit("[watch] munmap-user ", addr);
    /* FASE5: cap de log (era 1 linha por página liberada — junto com
     * [CLR] era 97% do serial do boot). */
    {
        static int nmu = 0;
        if (nmu < 30) {
            nmu++;
            extern void serial_puts(const char *s);
            extern void serial_puthex(uint32_t v);
            serial_puts("[munmap-user] pa="); serial_puthex((uint32_t)a);
            serial_puts(" ret=");
            serial_puthex((uint32_t)(uint64_t)__builtin_return_address(0));
            serial_puts("\r\n");
        }
    }
    if (a & (FRAME_SIZE - 1)) return -1;
    size_t idx = addr_to_frame(a);
    if (idx >= FRAME_COUNT) return -1;
    if (!(frames[idx] & FRAME_ALLOC)) return 0;
    int order = frames[idx] & FRAME_ORDER;
    buddy_free(addr, order);
    return 0;
}

/* ~~ munmap_all_user ~~ */
/* ~ essa demorou pra debugar, respeita ~ */
void munmap_all_user(void) {
    mem_lock();
    for (size_t i = 0; i < FRAME_COUNT; ) {
        uint8_t f = frames[i];
        if ((f & (FRAME_ALLOC | FRAME_USER)) == (FRAME_ALLOC | FRAME_USER)) {
            int order = f & FRAME_ORDER;
            frames[i] = (uint8_t)order;
            size_t idx = i;
            int ord = order;
            while (ord < MAX_ORDER) {
                size_t buddy_idx = idx ^ (1UL << ord);
                if ((frames[buddy_idx] & (FRAME_ALLOC | FRAME_ORDER)) == (uint8_t)ord) {
                    free_block_t *buddy = (free_block_t *)frame_to_addr(buddy_idx);
                    list_remove(&free_area[ord], buddy);
                    idx = (idx < buddy_idx) ? idx : buddy_idx;
                    ord++;
                    frames[idx] = (uint8_t)ord;
                } else {
                    break;
                }
            }
            free_block_t *blk = (free_block_t *)frame_to_addr(idx);
            list_add(&free_area[ord], blk);
            i += (1UL << order);
        } else {
            i++;
        }
    }
    mem_unlock();
}

static void *page_alloc(void) { return pcpu_page_alloc(); }
static void page_free(void *p) { pcpu_page_free(p); }

/* ~~ Pegando o valor~ só confia que ta certo~ */
/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
uint64_t pml4_get_current(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/* ~~ Criando coisa nova~~ que emocionante! */
/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
uint64_t pml4_create(void) {
    uint64_t *current = (uint64_t *)pml4_get_current();
    uint64_t *new_pml4 = page_alloc();
    if (!new_pml4) return 0;
    for (int i = 0; i < 512; i++)
        new_pml4[i] = current[i];
    return (uint64_t)(uintptr_t)new_pml4;
}

/* ~~ pml4_load ~~ */
/* ~ cuidado que essa aqui morde ~ */
void pml4_load(uint64_t pml4_pa) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_pa) : "memory");
}

/* ~~ pml4_restore ~~ */
void pml4_restore(uint64_t pml4_pa) { pml4_load(pml4_pa); }

/* ~~ pml4_add_user ~ Add U/S bit to a PD entry for user access ~~ */
/* ~ kyun~ mais uma funcao pra fazer o kernel n morrer */
int pml4_add_user(uint64_t pml4_pa, uint64_t vaddr) {
    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_pa;
    int pml4_idx = (vaddr >> 39) & 0x1FF;
    int pdpt_idx = (vaddr >> 30) & 0x1FF;
    int pd_idx   = (vaddr >> 21) & 0x1FF;
    if (!(pml4[pml4_idx] & 1)) return -1;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[pml4_idx] & ~0xFFFULL);
    if (!(pdpt[pdpt_idx] & 1)) return -1;
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFFULL);
    if (!(pd[pd_idx] & 1)) {
        uint64_t phys = vaddr & ~0x1FFFFFULL;
        pd[pd_idx] = phys | 0x87;
    } else {
        pd[pd_idx] |= 0x04;
        /* ~~ PDE dividido em 4KB: U/S só no ponteiro da PT não basta —
         * o acesso user depende também do U/S de CADA PTE. Marca os 512
         * (mesma semântica de quando a entrada era 2MB huge). Sem isso
         * o stack do execve em VA identity dava PF em ring 3. ~~ */
        if (!(pd[pd_idx] & 0x80)) {
            uint64_t *pt = (uint64_t *)(uintptr_t)(pd[pd_idx] & ~0xFFFULL);
            for (int i2 = 0; i2 < 512; i2++)
                if (pt[i2] & 1) pt[i2] |= 0x04;
        }
    }
    return 0;
}

/* Split a 2MB PD entry (huge page) into 512 × 4KB PT entries.
 * Returns 0 on success, -1 on failure.
 * After split, each 4KB entry preserves the same physical addr + flags
 * as the original 2MB entry, but as 4KB pages. */
/* ~ essa demorou pra debugar, respeita ~ */
int split_2mb_pde(uint64_t pml4_pa, uint64_t vaddr) {
    int pml4_idx = (vaddr >> 39) & 0x1FF;
    int pdpt_idx = (vaddr >> 30) & 0x1FF;
    int pd_idx   = (vaddr >> 21) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_pa;
    if (!(pml4[pml4_idx] & 1)) return -1;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[pml4_idx] & ~0xFFFULL);
    if (!(pdpt[pdpt_idx] & 1)) return -1;
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFFULL);

    uint64_t pde = pd[pd_idx];
    if (!(pde & 1)) return -1;
    if (!(pde & 0x80)) return 0;  // Already 4KB pages, nothing to do

    /* Allocate a 4KB page table */
    uint64_t *pt = page_frame_kernel();
    if (!pt) return -1;

    uint64_t phys_base = pde & ~0x1FFFFFULL;
    uint64_t flags = pde & 0x1FF;
    flags &= ~0x80;  // Clear huge-page bit
    for (int i = 0; i < 512; i++)
        pt[i] = (phys_base + (uint64_t)i * 0x1000) | flags;

    pd[pd_idx] = (uint64_t)(uintptr_t)pt | 0x07;
    __sync_synchronize();
    {
        uint64_t _chk;
        __asm__ volatile("movq %1, %0" : "=r"(_chk) : "m"(pd[pd_idx]));
        (void)_chk;
    }
    return 0;
}

    /* Fine-grained: set U/S on a single 4KB page within a PD entry.
     * Splits the 2MB huge page first if necessary. */
/* ~ essa demorou pra debugar, respeita ~ */
/* ~~ pml4_add_user_range ~ marca U/S só nas PÁGINAS de 4KB em
 * [lo,hi). Diferente do pml4_add_user (janela INTEIRA de 2MB), não
 * expõe os objetos de kernel que dividem a mesma janela identity
 * (tabelas de página e frames do buddy moram junto com o stack do
 * user): o Xterm escrevia ~42KB abaixo da própria pilha e
 * SMASHEAVA o page directory do próprio processo (fork-bad). ~~ */
int pml4_add_user_range(uint64_t pml4_pa, uint64_t lo, uint64_t hi) {
    if (hi <= lo) return -1;
    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_pa;
    for (uint64_t va = lo & ~0xFFFULL; va < hi; va += 0x1000) {
        int i4 = (va >> 39) & 0x1FF;
        int i3 = (va >> 30) & 0x1FF;
        int i2 = (va >> 21) & 0x1FF;
        int i1 = (va >> 12) & 0x1FF;
        if (!(pml4[i4] & 1)) return -1;
        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[i4] & ~0xFFFULL);
        if (!(pdpt[i3] & 1)) return -1;
        if (split_2mb_pde(pml4_pa, va) < 0) return -1;
        uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[i3] & ~0xFFFULL);
        if (!(pd[i2] & 1)) return -1;
        pd[i2] |= 0x04;                  /* U/S no nível de tabela (walk) */
        uint64_t *pt = (uint64_t *)(uintptr_t)(pd[i2] & ~0xFFFULL);
        pt[i1] |= 0x04;                  /* U/S só na página pedida */
    }
    __asm__ volatile("invlpg (%0)" :: "r"(lo) : "memory");
    return 0;
}

int pml4_add_user_4kb(uint64_t pml4_pa, uint64_t vaddr) {
    if (split_2mb_pde(pml4_pa, vaddr & ~0x1FFFFFULL) < 0) return -1;
    int pml4_idx = (vaddr >> 39) & 0x1FF;
    int pdpt_idx = (vaddr >> 30) & 0x1FF;
    int pd_idx   = (vaddr >> 21) & 0x1FF;
    int pt_idx   = (vaddr >> 12) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_pa;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[pml4_idx] & ~0xFFFULL);
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFFULL);
    uint64_t *pt = (uint64_t *)(uintptr_t)(pd[pd_idx] & ~0xFFFULL);
    pt[pt_idx] |= 0x04;
    __sync_synchronize();
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    return 0;
}

/* ~~ map_user_4kb ~ Mapeia 1 página de 4KB (frame novo) num espaço de processo ~~
 * Cria as tabelas (pml4/pdpt/pd/PT) que faltarem e aloca um frame físico
 * novo pro endereço, com U/S + RW (0x07). Se a página 4KB já estiver
 * mapeada, não mexe em nada e retorna 0. Se cair numa huge page de
 * KERNEL (identity supervisor herdada), divide via split_2mb_pde e o
 * PTE pedido ganha frame fresco (Fase 4d: brk do foot passou de 2MB)~
 * (pro brk do musl crescer o heap — antes o handler só mudava o valor
 *  e o processo tomava PF ao escrever nas páginas novas~ rssrsrs) */
int map_user_4kb(uint64_t pml4_pa, uint64_t va) {
    int pml4_idx = (va >> 39) & 0x1FF;
    int pdpt_idx = (va >> 30) & 0x1FF;
    int pd_idx   = (va >> 21) & 0x1FF;
    int pt_idx   = (va >> 12) & 0x1FF;

    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_pa;
    if (!(pml4[pml4_idx] & 1)) {
        uint64_t *pdpt = page_frame_kernel();
        if (!pdpt) return -1;
        for (int i = 0; i < 512; i++) pdpt[i] = 0;
        pml4[pml4_idx] = (uint64_t)(uintptr_t)pdpt | 0x07;
    }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[pml4_idx] & ~0xFFFULL);
    if (!(pdpt[pdpt_idx] & 1)) {
        uint64_t *pd = page_frame_kernel();
        if (!pd) return -1;
        for (int i = 0; i < 512; i++) pd[i] = 0;
        pdpt[pdpt_idx] = (uint64_t)(uintptr_t)pd | 0x07;
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFFULL);
    if (pd[pd_idx] & 0x80) {
        /* ~~ huge page no caminho do brk/mmap (Fase 4d: foot cresceu o
         * heap até 0xC15000 e morreu em wayl_flush+0x72 — PDE 6 era a
         * identity supervisor 0xC00083 herdada do mapa low do kernel,
         * e o PF foi negado pelo gate do USFIX no idt.c).
         * Huge que já é de usuário: mantém (conteúdo é do processo).
         * Huge de kernel: divide (split_2mb_pde, padrão do fork); os
         * PTEs pedidos ganham frame FRESCO no teste abaixo — nunca
         * U/S em RAM de kernel. ~~ */
        if (pd[pd_idx] & 0x04) return 0;        /* huge de user: mantém~ */
        if (split_2mb_pde(pml4_pa, va) < 0) return -1;
        pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFFULL);
    }
    if (!(pd[pd_idx] & 1)) {
        uint64_t *pt = page_frame_kernel();
        if (!pt) return -1;
        for (int i = 0; i < 512; i++) pt[i] = 0;
        pd[pd_idx] = (uint64_t)(uintptr_t)pt | 0x07;
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)(pd[pd_idx] & ~0xFFFULL);
    /* ~~ PTE presente MAS supervisor (= resto identity pós-split, que
     * ring 3 nunca conseguiu ler) também ganha frame fresco: só PTE
     * com U/S é "já mapeada" de verdade. ~~ */
    if ((pt[pt_idx] & 1) && (pt[pt_idx] & 0x04)) return 0;
    uint64_t *frame = mmap_user(0, 4096, 3, 0);
    if (!frame) return -1;
    pt[pt_idx] = (uint64_t)(uintptr_t)frame | 0x07;
    __sync_synchronize();
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    return 0;
}

/* ~~ pml4_destroy ~~ */
/* ~ cuidado que essa aqui morde ~ */
void pml4_destroy(uint64_t pml4_pa) {
    page_free((void *)(uintptr_t)pml4_pa);
}

/* ~~ pml4_map_phys ~~ */
int pml4_map_phys(uint64_t pml4_pa, uint64_t virt_addr, uint64_t phys_addr,
                  size_t size, int writable) {
    uint64_t entry_flags = 0x81;  /* P | PS (2MB page) */
    entry_flags |= 0x04;          /* U/S — mapeamento de userland */
    if (writable) entry_flags |= 0x02;  /* R/W controlado pelo parametro */
    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_pa;
    for (uint64_t offset = 0; offset < size; offset += 0x200000) {
        uint64_t va = virt_addr + offset;
        uint64_t pa = phys_addr + offset;
        int pml4_idx = (va >> 39) & 0x1FF;
        int pdpt_idx = (va >> 30) & 0x1FF;
        int pd_idx   = (va >> 21) & 0x1FF;
        if (!(pml4[pml4_idx] & 1)) {
            uint64_t *pdpt = page_frame_kernel();
            if (!pdpt) return -1;
            for (int i = 0; i < 512; i++) pdpt[i] = 0;
            pml4[pml4_idx] = (uint64_t)(uintptr_t)pdpt | 0x07;
        }
        uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[pml4_idx] & ~0xFFF);
        if (!(pdpt[pdpt_idx] & 1)) {
            uint64_t *pd = page_frame_kernel();
            if (!pd) return -1;
            for (int i = 0; i < 512; i++) pd[i] = 0;
            pdpt[pdpt_idx] = (uint64_t)(uintptr_t)pd | 0x07;
            if (pdpt_idx == 3) {
                extern void watch_add(uint64_t pa);
                watch_add((uint64_t)(uintptr_t)pd);
                extern void serial_puts(const char *s);
                extern void serial_puthex(uint32_t v);
                serial_puts("[map-phys] pdpt3 va="); serial_puthex((uint32_t)va);
                serial_puts(" pd="); serial_puthex((uint32_t)(uint64_t)pd);
                serial_puts("\r\n");
            }
        }
        uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFF);
        pd[pd_idx] = pa | entry_flags;
    }
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_pa) : "memory");
    return 0;
}

/* ~~ User high-address allocator ~~
 * Allocates physical pages and maps them at a high virtual address
 * (starting at 0x70000000) with full user-accessible flags.
 * This avoids QEMU TCG issues with identity-map 2MB→4KB splits. */
#define USER_VA_BASE    0x70000000ULL
#define USER_VA_END     0x7F000000ULL
static uint64_t user_va_next = USER_VA_BASE;

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
static int ensure_pml4_entry(uint64_t *pml4, int idx) {
    if (pml4[idx] & 1) return 0;
    uint64_t *page = page_frame_kernel();
    if (!page) return -1;
    for (int i = 0; i < 512; i++) page[i] = 0;
    pml4[idx] = (uint64_t)(uintptr_t)page | 0x07;
    return 0;
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
static int map_one_page(uint64_t va, uint64_t pa, uint64_t cr3) {
    uint64_t *pml4 = (uint64_t *)(uintptr_t)cr3;
    int pml4_idx = (va >> 39) & 0x1FF;
    int pdpt_idx = (va >> 30) & 0x1FF;
    int pd_idx   = (va >> 21) & 0x1FF;
    int pt_idx   = (va >> 12) & 0x1FF;
    if (ensure_pml4_entry(pml4, pml4_idx) < 0) return -1;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[pml4_idx] & ~0xFFFULL);
    if (!(pdpt[pdpt_idx] & 1)) {
        uint64_t *pd = page_frame_kernel();
        if (!pd) return -1;
        for (int i = 0; i < 512; i++) pd[i] = 0;
        pdpt[pdpt_idx] = (uint64_t)(uintptr_t)pd | 0x07;
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFFULL);
    if (!(pd[pd_idx] & 1)) {
        uint64_t *pt = page_frame_kernel();
        if (!pt) return -1;
        for (int i = 0; i < 512; i++) pt[i] = 0;
        pd[pd_idx] = (uint64_t)(uintptr_t)pt | 0x07;
    } else if (pd[pd_idx] & 0x80) {
        uint64_t pde = pd[pd_idx];
        uint64_t *pt = page_frame_kernel();
        if (!pt) return -1;
        uint64_t phys_base = pde & ~0x1FFFFFULL;
        uint64_t flags = pde & 0x1FF;
        flags &= ~0x80;
        for (int i = 0; i < 512; i++)
            pt[i] = (phys_base + (uint64_t)i * 0x1000) | flags;
        pd[pd_idx] = (uint64_t)(uintptr_t)pt | 0x07;
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)(pd[pd_idx] & ~0xFFFULL);
    pt[pt_idx] = pa | 0x67;
    __sync_synchronize();
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    return 0;
}

/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
void *mmap_user_high(size_t length) {
    serial_puts("mh: len="); serial_puthex((uint32_t)length); serial_puts("\r\n");
    size_t pages = (length + FRAME_SIZE - 1) / FRAME_SIZE;
    if (pages < 64) pages = 64;
    serial_puts("mh: pages="); serial_puthex((uint32_t)pages); serial_puts("\r\n");
    int order = 0;
    while ((1UL << order) < pages) order++;
    serial_puts("mh: order="); serial_puthex((uint32_t)order); serial_puts("\r\n");
    uint64_t n_total = (1UL << order);
    serial_puts("mh: n_total="); serial_puthex((uint32_t)n_total); serial_puts("\r\n");
    serial_puts("mh: alloc...\r\n");
    void *phys = buddy_alloc(order, 1);
    serial_puts("mh: alloc done\r\n");
    if (!phys) { serial_puts("mh: alloc FAIL\r\n"); return NULL; }
    uint64_t pa = (uint64_t)(uintptr_t)phys;
#define N_GUARD 4
    uint64_t *guard_page = buddy_alloc(0, 1);
    if (!guard_page) { buddy_free(phys, order); serial_puts("mh: guard FAIL\r\n"); return NULL; }
    uint64_t guard_pa = (uint64_t)(uintptr_t)guard_page;
    void *guard2 = buddy_alloc(0, 1);
    if (!guard2) { buddy_free(phys, order); buddy_free(guard_page, 0); serial_puts("mh: guard2 FAIL\r\n"); return NULL; }
    uint64_t guard2_pa = (uint64_t)(uintptr_t)guard2;
    void *guard3 = buddy_alloc(0, 1);
    if (!guard3) { buddy_free(phys, order); buddy_free(guard_page, 0); buddy_free(guard2, 0); serial_puts("mh: guard3 FAIL\r\n"); return NULL; }
    uint64_t guard3_pa = (uint64_t)(uintptr_t)guard3;
    void *guard4 = buddy_alloc(0, 1);
    if (!guard4) { buddy_free(phys, order); buddy_free(guard_page, 0); buddy_free(guard2, 0); buddy_free(guard3, 0); serial_puts("mh: guard4 FAIL\r\n"); return NULL; }
    uint64_t guard4_pa = (uint64_t)(uintptr_t)guard4;
    uint64_t va = user_va_next;
    uint64_t total_size = (n_total + N_GUARD) * FRAME_SIZE;
    if (va + total_size > USER_VA_END) {
        buddy_free(phys, order);
        buddy_free(guard_page, 0);
        buddy_free(guard2, 0);
        buddy_free(guard3, 0);
        buddy_free(guard4, 0);
        return NULL;
    }
    uint64_t cr3 = pml4_get_current();
    for (uint64_t i = 0; i < n_total; i++) {
        if (map_one_page(va + i * FRAME_SIZE, pa + i * FRAME_SIZE, cr3) < 0) {
            buddy_free(phys, order);
            buddy_free(guard_page, 0);
            buddy_free(guard2, 0);
            buddy_free(guard3, 0);
            buddy_free(guard4, 0);
            return NULL;
        }
    }
    if (map_one_page(va + n_total * FRAME_SIZE, guard_pa, cr3) < 0) goto guard_fail;
    if (map_one_page(va + (n_total + 1) * FRAME_SIZE, guard2_pa, cr3) < 0) goto guard_fail;
    if (map_one_page(va + (n_total + 2) * FRAME_SIZE, guard3_pa, cr3) < 0) goto guard_fail;
    if (map_one_page(va + (n_total + 3) * FRAME_SIZE, guard4_pa, cr3) < 0) goto guard_fail;
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    serial_puts("mh: done guard va="); serial_puthex((uint32_t)(va + n_total * FRAME_SIZE)); serial_puts("\r\n");
    user_va_next = va + (n_total + N_GUARD) * FRAME_SIZE;
    return (void *)va;
guard_fail:
    buddy_free(phys, order);
    buddy_free(guard_page, 0);
    buddy_free(guard2, 0);
    buddy_free(guard3, 0);
    buddy_free(guard4, 0);
    return NULL;
}

/* ♥ memory.c ~ feito com carinho (e uma raiva controlada) ~ kyun! */
