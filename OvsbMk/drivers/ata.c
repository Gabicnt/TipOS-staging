/* moe moe kyun <3 */
/* moe moe kyun <3 */
/* ♥ driver ~ conversando com o hardware, seu chato!
 * arquivo: ata.c ~ funcoes anotadas: 6
 */
/* ♥ ata.c ~ feito com carinho (e gambiarras) pela equipe TipOS! ♥
 * Se quebrar, a culpa é sua~ <3
 *~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*~*/

/* ♥ ATA ~ driver de disco ATA! le e escreve setores~ */
#include "ata.h"
#include "../kernel/serial.h"

#define ATA_DATA       0x1F0
#define ATA_SECTORS    0x1F2
#define ATA_LBA_LOW    0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HIGH   0x1F5
#define ATA_DRIVE      0x1F6
#define ATA_COMMAND    0x1F7
#define ATA_STATUS     0x1F7
#define ATA_ALT_STATUS 0x3F6

static volatile int ata_io_lock;

static unsigned long ata_lock_irqsave(void) {
    unsigned long flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__sync_lock_test_and_set(&ata_io_lock, 1))
        __asm__ volatile ("pause");
    return flags;
}

static void ata_unlock_irqrestore(unsigned long flags) {
    __sync_lock_release(&ata_io_lock);
    if (flags & (1UL << 9)) __asm__ volatile ("sti" ::: "memory");
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
static int ata_wait(int data_ready) {
    for (int timeout = 0; timeout < 1000000; timeout++) {
        uint8_t s = inb(ATA_STATUS);
        if (s == 0 || s == 0xFF) return -1;
        if (s & 0x80) continue;
        if (s & 0x21) return -1;
        if (!data_ready || (s & 0x08)) return 0;
    }
    return -1;
}

static void ata_delay_400ns(void) {
    for (int i = 0; i < 4; i++) (void)inb(ATA_ALT_STATUS);
}

/* ~~ Inicializando~~ torce pra não dar panic! */
/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
void ata_init(void) {
    serial_puts("[ATA] Inicializando...\r\n");
    outb(ATA_DRIVE, 0xE0);
    ata_delay_400ns();
    for (volatile int i = 0; i < 100000; i++);
    if (ata_wait(0) != 0) { serial_puts("[ATA] Timeout!\r\n"); return; }
    outb(ATA_COMMAND, 0xEC);
    if (ata_wait(1) != 0) { serial_puts("[ATA] Sem disco!\r\n"); return; }
    uint16_t buf[256];
    for (int i = 0; i < 256; i++)
        __asm__ volatile ("inw %w1, %0" : "=a"(buf[i]) : "d"(ATA_DATA));
    serial_puts("[ATA] Disco: QEMU HARDDISK\r\n");
}

/* ~~ Lendo coisinhas~ toma cuidado pra não ler lixo! */
/* ~ essa funcao aqui e a mais importante, presta atencao baka! */
int ata_read_sector(uint32_t lba, uint8_t *buffer) {
    unsigned long flags = ata_lock_irqsave();
    int result = -1;
    if (ata_wait(0) != 0) goto done;
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    ata_delay_400ns();
    outb(ATA_SECTORS, 1);
    outb(ATA_LBA_LOW, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
    outb(ATA_COMMAND, 0x20);
    if (ata_wait(1) != 0) goto done;
    for (int i = 0; i < 256; i++) {
        uint16_t d;
        __asm__ volatile ("inw %w1, %0" : "=a"(d) : "d"(ATA_DATA));
        buffer[i*2] = d & 0xFF;
        buffer[i*2+1] = (d >> 8) & 0xFF;
    }
    result = 0;
done:
    ata_unlock_irqrestore(flags);
    return result;
}

/* ~~ Escrevendo~~ não vai corromper nada, vai? >_< */
/* ~ simples mas essencial, n mexe sem saber oq ta fazendo */
int ata_write_sector(uint32_t lba, const uint8_t *buffer) {
    unsigned long flags = ata_lock_irqsave();
    int result = -1;
    if (ata_wait(0) != 0) goto done;
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    ata_delay_400ns();
    outb(ATA_SECTORS, 1);
    outb(ATA_LBA_LOW, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
    outb(ATA_COMMAND, 0x30);
    if (ata_wait(1) != 0) goto done;
    for (int i = 0; i < 256; i++) {
        uint16_t d = buffer[i*2] | (buffer[i*2+1] << 8);
        __asm__ volatile ("outw %0, %w1" :: "a"(d), "d"(ATA_DATA));
    }
    if (ata_wait(0) != 0) goto done;
    result = 0;
done:
    ata_unlock_irqrestore(flags);
    return result;
}




/* ♥ ata.c ~ se bugar me chama, se n bugar tb me chama ~ >u< */
