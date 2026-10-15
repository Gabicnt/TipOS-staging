#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
DOOM_DIR="$ROOT/src/userland/progs/doom"
DISK="$ROOT/disk.img"
WAD_SOURCE="${DOOM_WAD:-}"
RUN_QEMU=0

usage() {
    printf 'Uso: %s [--wad CAMINHO/DOOM1.WAD] [--run]\n' "$0"
    printf 'Sem --wad, preserva e verifica o WAD que ja esta em disk.img.\n'
}

while (($#)); do
    case "$1" in
        --wad)
            if (($# < 2)); then
                printf 'Erro: --wad precisa de um caminho.\n' >&2
                exit 2
            fi
            WAD_SOURCE="$2"
            shift 2
            ;;
        --run)
            RUN_QEMU=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'Opcao desconhecida: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

for tool in make mcopy mdir mmd; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'Erro: ferramenta necessaria nao encontrada: %s\n' "$tool" >&2
        exit 1
    fi
done

printf '\n==> Compilando kernel e atualizando ISO\n'
make -C "$ROOT" all

printf '\n==> Compilando Doom ELF\n'
make -B -C "$DOOM_DIR" DOOM.ELF

printf '\n==> Garantindo a imagem FAT\n'
make -C "$ROOT" disk.img
mmd -i "$DISK" ::/APPS >/dev/null 2>&1 || true

if [[ -n "$WAD_SOURCE" ]]; then
    if [[ ! -f "$WAD_SOURCE" ]]; then
        printf 'Erro: WAD nao encontrado: %s\n' "$WAD_SOURCE" >&2
        exit 1
    fi
    printf '\n==> Instalando WAD em /APPS/DOOM1.WAD\n'
    mcopy -o -i "$DISK" "$WAD_SOURCE" ::/APPS/DOOM1.WAD
elif ! mdir -i "$DISK" ::/APPS/DOOM1.WAD >/dev/null 2>&1; then
    printf 'Erro: /APPS/DOOM1.WAD nao existe na imagem. Passe --wad CAMINHO/DOOM1.WAD.\n' >&2
    exit 1
fi

printf '\n==> Instalando Doom em /APPS/DOOM.ELF\n'
mcopy -o -i "$DISK" "$DOOM_DIR/DOOM.ELF" ::/APPS/DOOM.ELF

printf '\n==> Conteudo de /APPS na imagem\n'
mdir -i "$DISK" ::/APPS
printf '\nPronto. No TipOS, execute: exec /APPS/DOOM.ELF\n'

if ((RUN_QEMU)); then
    printf '\n==> Iniciando TipOS no QEMU\n'
    make -C "$ROOT" run
fi