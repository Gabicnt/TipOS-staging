// moe moe kyun <3
// moe moe kyun <3
// Keyboard PS/2 driver for TipOS — Linux-like input subsystem (GPL v2)

// ~~ Buffer circular do teclado ~~
// KB_BUF_SIZE = 256 bytes de buffer (mais que suficiente~)
// buf_head: onde escreve o próximo caractere
// buf_tail: onde lê o próximo caractere
// Se head == tail, o buffer tá vazio (e você espera~)
const KB_BUF_SIZE = 256;
var key_buf: [KB_BUF_SIZE]u8 = undefined;
var buf_head: usize = 0;
var buf_tail: usize = 0;

// ~~ RAW_BUF_SIZE ~
// Buffer circular DOS SCAN CODES CRUS (com bit de release 0x80 e
// prefixos 0xE0). O Xorg (driver kbd em modo K_RAW) lê daqui via
// /dev/ttyN. O buffer de chars (key_buf) continua sendo o do shell~
const RAW_BUF_SIZE = 256;
var raw_buf: [RAW_BUF_SIZE]u8 = undefined;
var raw_head: usize = 0;
var raw_tail: usize = 0;

const EVENT_BUF_SIZE = 256;
var event_buf: [EVENT_BUF_SIZE]u8 = undefined;
var event_head: usize = 0;
var event_tail: usize = 0;
var keyboard_trace_count: u32 = 0;

extern fn serial_puts(s: [*:0]const u8) void;
extern fn serial_puthex(value: u32) void;

// ~~ norm / shf ~~
// Tabelas de tradução scan code → caractere ASCII.
// `norm`: teclas normais (shift desligado)
// `shf`: teclas com shift ativo (maiúsculas, símbolos~)
// O scan code do PS/2 é o índice na tabela.
// Ex: scan code 0x10 = 'q' em norm, 'Q' em shf. Simples, né? >_<
const norm = [_]u8{
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,
    9,'q','w','e','r','t','y','u','i','o','p','[',']',10,
    0,'a','s','d','f','g','h','j','k','l',';',39,'`',0,92,
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

const shf = [_]u8{
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+',8,
    9,'Q','W','E','R','T','Y','U','I','O','P','{','}',10,
    0,'A','S','D','F','G','H','J','K','L',':',34,'~',0,'|',
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

// ~~ inb ~~
// Lê um byte de uma porta de I/O.
// Usa a instrução `inb` do x86-64 (assembly inline~)
// Porta 0x60 = dados do teclado, 0x64 = status/command
fn inb(port: u16) u8 {
    return asm volatile ("inb %[port], %[ret]"
        : [ret] "={al}" (-> u8),
        : [port] "N{dx}" (port),
    );
}

// ~~ outb ~~
// Escreve um byte numa porta de I/O.
// Usa a instrução `outb` (o contrário do inb~)
// Pra falar com o controlador PS/2, usa isso aqui~ ☆
fn outb(port: u16, val: u8) void {
    asm volatile ("outb %[val], %[port]"
        :
        : [port] "N{dx}" (port),
          [val] "{al}" (val),
    );
}

// ~~ kbd_push ~~
// Empurra um caractere no buffer circular.
// Se o buffer encher, o caractere é descartado (sorry~).
// Quem não lê, não reclama~
fn kbd_push(ch: u8) void {
    const next = (buf_head + 1) % KB_BUF_SIZE;
    if (next != buf_tail) {
        key_buf[buf_head] = ch;
        buf_head = next;
    }
}

// ~~ raw_push ~~
// Empurra um scan code cru no buffer raw (pro Xorg~)
fn raw_push(b: u8) void {
    const next = (raw_head + 1) % RAW_BUF_SIZE;
    if (next != raw_tail) {
        raw_buf[raw_head] = b;
        raw_head = next;
    }
}

fn event_push(b: u8) void {
    const next = (event_head + 1) % EVENT_BUF_SIZE;
    if (next == event_tail) {
        event_tail = (event_tail + 1) % EVENT_BUF_SIZE;
    }
    event_buf[event_head] = b;
    event_head = next;
}

// ~~ keyboard_init ~~
// Inicializa o estado do teclado: zera o buffer, reseta shift/ctrl.
// Chama uma vez no boot e esquece~ (ou chama de novo se quiser resetar)
export fn keyboard_init() void {
    buf_head = 0;
    buf_tail = 0;
    raw_head = 0;
    raw_tail = 0;
    event_head = 0;
    event_tail = 0;
    shift_pressed = 0;
    ctrl_pressed = 0;
}

// ~~ keyboard_handler ~~
// Handler de IRQ do teclado! Chamado toda vez que uma tecla é
// pressionada ou solta. Lê o scan code da porta 0x60, verifica
// se é press (bit 7 = 0) ou release (bit 7 = 1), traduz pra ASCII
// via tabela norm/shf, e coloca no buffer.
// Se for Ctrl+C, manda ETX (3) pro buffer (pra matar o processo~)
// No final, manda EOI pro PIC (0x20, 0x20) pra liberar a interrupção.
export fn keyboard_handler() void {
    const scancode = inb(0x60);
    raw_push(scancode);
    event_push(scancode);
    if (keyboard_trace_count < 32 and
        (scancode == 0x1F or scancode == 0x9F or scancode == 0xE0 or
         scancode == 0x11 or scancode == 0x91 or scancode == 0x48 or
         scancode == 0xC8 or scancode == 0x50 or scancode == 0xD0))
    {
        keyboard_trace_count += 1;
        serial_puts("[KIRQ] ");
        serial_puthex(scancode);
        serial_puts("\r\n");
    }
    if ((scancode & 0x80) != 0) {
        const released = scancode & 0x7F;
        switch (released) {
            0x2A, 0x36 => shift_pressed = 0,
            0x1D => ctrl_pressed = 0,
            else => {},
        }
    } else {
        // 0xE0 e prefixo extended (setas: E0 48/C8, E0 50/D0, E0 4B/CB, E0 4D/CD;
        // R-ctrl/alt: E0 1D/9D, E0 38/B8). O byte E0 ja foi pushado em raw/event
        // acima; o byte seguinte sera processado na proxima IRQ. Nao pode
        // dar EOI+return sem esperar o segundo byte — o fix antigo descartava
        // o scancode real e o libinput nunca via as setas. Agora apenas
        // ignora a conversao ASCII para E0 e deixa cair no EOI final.
        if (scancode == 0xE0) {
            outb(0x20, 0x20);
            return;
        }
        // Se o byte anterior foi 0xE0, este e o scan extended real.
        // raw/event ja contem [E0, sc2]; para kbd_push/ctrl/shift precisamos
        // tratar sc2 sem conflitar com a tabela norm (0..0x7F).
        // Mantemos shift/ctrl apenas para scancodes nao-extended aqui.
        if (scancode < norm.len) {
            const table = if (shift_pressed != 0) &shf else &norm;
            const ch = table[scancode];
            if (ch != 0) {
                if (ctrl_pressed != 0 and ch == 'c') {
                    kbd_push(3);
                } else {
                    kbd_push(ch);
                }
            }
        }
        switch (scancode) {
            0x2A, 0x36 => shift_pressed = 1,
            0x1D => ctrl_pressed = 1,
            else => {},
        }
    }
    outb(0x20, 0x20);
}

// ~~ keyboard_avail ~~
// Verifica se tem caractere disponível no buffer.
// Retorna 1 se tem, 0 se não tem (polite polling~)
export fn keyboard_avail() i32 {
    if (buf_head != buf_tail) return 1;
    return 0;
}

// ~~ keyboard_read ~~
// Lê um caractere do buffer (bloqueante!).
// Se o buffer tiver vazio, fica em loop com `pause` até ter algo.
// Retorna o caractere mais antigo (FIFO, pq sou justa~)
export fn keyboard_read() u8 {
    while (buf_head == buf_tail) {
        asm volatile ("pause");
    }
    const ch = key_buf[buf_tail];
    buf_tail = (buf_tail + 1) % KB_BUF_SIZE;
    return ch;
}

// ~~ kbd_raw_avail / kbd_raw_read ~~
// Scan codes crus pro Xorg (driver kbd). Não bloqueia: retorna o que
// tiver no buffer e deixa o resto. O Xorg espera via select()~
export fn kbd_raw_avail() i32 {
    if (raw_head != raw_tail) return 1;
    return 0;
}

export fn kbd_raw_read() u8 {
    if (raw_head == raw_tail) return 0;
    const b = raw_buf[raw_tail];
    raw_tail = (raw_tail + 1) % RAW_BUF_SIZE;
    return b;
}

export fn keyboard_event_avail() i32 {
    if (event_head != event_tail) return 1;
    return 0;
}

export fn keyboard_event_clear() void {
    event_tail = event_head;
}

export fn keyboard_event_read() u8 {
    if (event_head == event_tail) return 0;
    const b = event_buf[event_tail];
    event_tail = (event_tail + 1) % EVENT_BUF_SIZE;
    return b;
}

// ~~ shift_pressed / ctrl_pressed ~~
// Estado global das teclas modificadoras (shift e ctrl).
// Exportadas pra outros módulos poderem ver (tipo o disp~)
// 0 = solta, 1 = pressionada. Simples assim >_<
export var shift_pressed: i32 = 0;
export var ctrl_pressed: i32 = 0;