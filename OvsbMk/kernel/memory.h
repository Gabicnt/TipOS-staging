/* moe moe kyun <3 */
/* moe moe kyun <3 */
/* ♥ kernel stuff ~ aqui é onde o bicho pega de verdade! kyun~
 * arquivo: memory.h ~ funcoes anotadas: 0
 */
/* ♥ memory.h ~ feito com carinho (e gambiarras) pela equipe TipOS! ♥
 * Se quebrar, a culpa é sua~ <3
 *~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*/

/* ♥ MEMORY HEADER ~ "AQUI MORA O ALLOCADOR MAIS LINDO DO KERNEL!"
 * Dica: Agora com buddy allocator + SLUB caches~ tudo reciclavel!
 * PROT_READ=1, PROT_WRITE=2, PROT_EXEC=4 ~ ta na cara, baka!
 * kyun~ nunca mais vai faltar memoria! >_< */
#ifndef MEMORY_H
#define MEMORY_H
#include <stdint.h>
#include <stddef.h>

#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_PRIVATE 2
#define MAP_ANON    0x1000

/* FASE5: pool do buddy em constantes públicas (vm_map.c usa pros
 * guards de refcount). QEMU tem 512MB; o buddy usa [10MB,510MB). */
#define BUDDY_HEAP_PHYS  0xA00000u
#define BUDDY_HEAP_SIZE  (500u * 1024u * 1024u)
#define BUDDY_FRAME_COUNT (BUDDY_HEAP_SIZE / 4096u)

void memory_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);
void *kcalloc(size_t count, size_t size);
void *mmap_user(void *addr, size_t length, int prot, int flags);
/* frame de tabela de página (não é memória de usuário — fork não copia) */
void *page_frame_kernel(void);
void *mmap_user_high(size_t length);
int munmap_user(void *addr, size_t length);
int frame_is_user(uint64_t pa);
void munmap_all_user(void);
/* FASE5-DBG temporário: anel forense do buddy, despejado no EXC# */
void buddy_dump_ring(void);

uint64_t pml4_get_current(void);
uint64_t pml4_create(void);
void pml4_load(uint64_t pml4_pa);
void pml4_restore(uint64_t pml4_pa);
void pml4_destroy(uint64_t pml4_pa);
/* ~~ pml4_map_phys ~~ */
int pml4_map_phys(uint64_t pml4_pa, uint64_t virt_addr, uint64_t phys_addr,
                  size_t size, int writable);
/* ~~ pml4_add_user ~ Add U/S bit to PD entry for user access ~~ */
int pml4_add_user(uint64_t pml4_pa, uint64_t vaddr);
/* ~~ pml4_add_user_4kb ~ Add U/S on a single 4KB page (splits huge page) ~~ */
int pml4_add_user_4kb(uint64_t pml4_pa, uint64_t vaddr);
/* ~~ pml4_add_user_range ~ U/S só nas páginas 4KB de [lo,hi) ~~ */
int pml4_add_user_range(uint64_t pml4_pa, uint64_t lo, uint64_t hi);
/* ~~ split_2mb_pde ~ Split a 2MB huge PD entry into 512 × 4KB PT entries ~~ */
int split_2mb_pde(uint64_t pml4_pa, uint64_t vaddr);
/* ~~ map_user_4kb ~ Map a fresh 4KB page (new frame) into a process space ~~ */
int map_user_4kb(uint64_t pml4_pa, uint64_t vaddr);

#endif




/* ♥ memory.h ~ se bugar me chama, se n bugar tb me chama ~ >u< */
