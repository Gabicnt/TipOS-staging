/* ovsb.term — main.c — entrada + term_cleanup (moe moe kyun <3) */
#include "term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OVSBT_VERSION "0.1.0"
#define DEFAULT_CONFIG "config/ovsb.term.conf"
#define FALLBACK_CONFIG_SUFFIX "/.config/ovsb.term.conf"

static void print_help(const char *prog)
{
    printf("Uso: %s [opcoes]\n", prog);
    printf("  --help            mostra esta ajuda\n");
    printf("  --version         mostra a versao\n");
    printf("  -v                modo verboso\n");
    printf("  -c PATH, --config PATH  arquivo de config\n");
    printf("  --headless-test   roda sem wayland (p/ testes)\n");
}

static void print_version(void)
{
    printf("ovsb.term %s\n", OVSBT_VERSION);
}

/* escolhe config: argv > relativo > ~/.config */
static void pick_config(char *out, size_t outsz, const char *argv_path)
{
    const char *home;
    if (argv_path && argv_path[0]) {
        snprintf(out, outsz, "%s", argv_path);
        return;
    }
    if (access(DEFAULT_CONFIG, R_OK) == 0) {
        snprintf(out, outsz, "%s", DEFAULT_CONFIG);
        return;
    }
    home = getenv("HOME");
    if (home && home[0]) {
        snprintf(out, outsz, "%s%s", home, FALLBACK_CONFIG_SUFFIX);
        return;
    }
    snprintf(out, outsz, "%s", DEFAULT_CONFIG);
}

void term_cleanup(term_t *t)
{
    if (!t)
        return;
    t->running = false;
    loop_cleanup(t);
    input_cleanup(t);
    pty_cleanup(t);
    render_cleanup(t);
    wl_cleanup(t);
    font_cleanup(t);
    scrollback_cleanup(t);
    grid_cleanup(t);
    signal_cleanup(t);
    free(t->selection.text);
    t->selection.text = NULL;
    t->selection.text_len = 0;
    free(t->clipboard.data);
    t->clipboard.data = NULL;
    t->clipboard.len = 0;
}

int main(int argc, char **argv)
{
    term_t term;
    char cfgpath[256];
    const char *argv_cfg = NULL;
    bool headless = false;
    int i, cols, rows;

    memset(&term, 0, sizeof(term));
    cfgpath[0] = '\0';

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        } else if (strcmp(argv[i], "-v") == 0) {
            term.verbose = true;
        } else if (strcmp(argv[i], "--headless-test") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--config") == 0 || strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "falta PATH apos %s\n", argv[i]);
                return 2;
            }
            argv_cfg = argv[++i];
        } else {
            fprintf(stderr, "opcao desconhecida: %s\n", argv[i]);
            return 2;
        }
    }

    log_init(term.verbose);
    pick_config(cfgpath, sizeof(cfgpath), argv_cfg);
    snprintf(term.config_path, sizeof(term.config_path), "%s", cfgpath);

    config_defaults(&term.config);
    config_load(&term.config, cfgpath);

    grid_init(&term, 24, 80);
    scrollback_init(&term, term.config.scrollback_lines > 0
                                     ? term.config.scrollback_lines
                                     : 1000);
    parser_init(&term.parser);
    font_init(&term);

    /* recalcula grade pela métrica real da fonte (tela padrão 800x600) */
    if (term.font.ready) {
        int cw = font_cell_width(&term.font);
        int ch = font_cell_height(&term.font);
        if (cw > 0 && ch > 0) {
            cols = 800 / cw;
            rows = 600 / ch;
            if (cols > 0 && rows > 0 && (cols != 80 || rows != 24))
                grid_resize(&term, rows, cols);
        }
    }

    if (!wl_init(&term)) {
        log_err("wl_init falhou");
        if (!headless) {
            term_cleanup(&term);
            return 1;
        }
        log_msg("headless-test: segue sem wayland");
    }

    render_init(&term);
    if (!pty_init(&term)) {
        log_err("pty_init falhou");
        term_cleanup(&term);
        return 1;
    }
    input_init(&term);
    loop_init(&term);

    term.running = true;
    term.exit_code = 0;
    loop_run(&term);

    term_cleanup(&term);
    return term.exit_code;
}
