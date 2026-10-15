/* moe moe kyun <3 */
/* moe moe kyun <3 */
/* ♥ kernel stuff ~ aqui é onde o bicho pega de verdade! kyun~
 * arquivo: linker.ld ~ funcoes anotadas: 0
 */
/* ♥ Linker script ~ "Junte tudo, seu montador baka!" */
ENTRY(_start)

SECTIONS
{
    . = 0x08000000;
    
    .multiboot : {
        KEEP(*(.multiboot))
    }
    
    .text : {
        *(.text)
    }
    
    .rodata : {
        *(.rodata)
    }
    
    .data : {
        *(.data)
    }
    
    .paging : {
        *(.paging)
    }
    
    .bss : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }
}

/* ♥ STACK ~ pilha do kernel 16KB */

/* ♥ linker.ld ~ arquivo fofinho do OvsbMkM! kyun~ <3 */
