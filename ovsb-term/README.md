# ovsb.term — terminal Wayland nativo (v1.0 em progresso)

Implementação do blueprint em `ARCHITECTURE.md`. C11, cliente Wayland
(`libwayland-client` + `xdg_shell`), PTY + shell, parser VT100/xterm,
grid de células, render via `wl_shm` (fcft + pixman), input XKB.

## Estado atual (2026-10-13)

- [x] Todos os 21 módulos v1 implementados (~5.500 linhas)
- [x] `make check` — 30/30 asserts (grid, escape, scrollback)
- [x] `make` — binário `ovsb.term` linka limpo, zero warnings
- [x] Headless e2e: PTY → shell → escape_feed → grid → render → exit 0
- [ ] Teste com compositor real (precisa de Wayland rodando)
- [ ] `make static` (binário 100% estático p/ OvsbMk)
- [ ] v2 (abas, sixel, clipboard wl_data_device, etc.)

## Uso

```bash
make            # binário host p/ dev/teste
make check      # testes unitários (30 asserts)
./ovsb.term --headless-test -c config/ovsb.term.conf
make static     # best-effort estático (blueprint)
make install    # ::/BIN/OVSB.TERM no disk.img + terminfo
```

## Desvios do blueprint

| Item | Blueprint | Implementado | Motivo |
|---|---|---|---|
| loop | `epoll` | `poll()` | TipOS tem handler real p/ `poll`; `epoll` é só mapa de nº |
| `signal.c/h` | esse nome | `sigpipe.c/h` | `src/signal.h` sombreava `<signal.h>` com `-Isrc` |
| link default | `-static` total | dinâmico host | estático total quebra `fontconfig` (dlopen); `make static` mantido p/ OvsbMk |
| SGR `:` | `4:0-4:5` | subparâmetro genérico | sem tocar `term.h` (`COLON_BIT` interno) |

## Layout

```
src/term.h        contrato central (structs + protótipos de tudo)
src/main.c        entry point + term_cleanup
src/grid.c        buffer de células + dirty tracking + palette xterm-256
src/escape.c      parser VT100/xterm (CSI/SGR/OSC/DCS/UTF-8)
src/pty.c         posix_openpt + fork + termios + TIOCSWINSZ
src/input.c       XKB → bytes p/ PTY + mouse SGR/X10 + seleção
src/wl_client.c   registry/xdg_shell/seat (padrão de OvsbMk/tests/compositor)
src/loop.c        poll(wl, pty, signal, timer)
src/render.c      dirty rows → ARGB (fcft/pixman ou fallback manual)
src/font.c        fcft 4 variantes + cache LRU (fallback 8x16 sem fcft)
src/shm.c         memfd + wl_shm_pool (double buffer)
src/scrollback.c  ring buffer | src/selection.c | src/clipboard.c (OSC 52)
src/timer.c       timerfd blink | src/sigpipe.c self-pipe
src/config.c      INI | src/log.c | src/util.c (wcwidth/utf8/base64/cor)
src/generated/    xdg-shell gerado via wayland-scanner
config/ovsb.term.conf   terminfo/ovsb.term   tests/test_{grid,escape,scrollback}.c
```
