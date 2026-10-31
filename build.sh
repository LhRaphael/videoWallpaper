#!/usr/bin/env bash
# =============================================================================
#  Video Wallpaper (Linux) — script de compilação e instalação
#
#  Uso:
#    ./build.sh                 compila em ./build/videowallpaper
#    ./build.sh debug           compila com símbolos de depuração (-O0 -g)
#    ./build.sh run [args]      compila e executa
#    ./build.sh install         compila e instala em ~/.local (sem sudo)
#    ./build.sh uninstall       remove a instalação
#    ./build.sh deps            mostra o comando para instalar as dependências
#    ./build.sh clean           apaga ./build
#
#  Opções (antes do comando):
#    --no-layer-shell   não usa gtk-layer-shell (sem suporte a Wayland nativo)
#    --no-tray          não usa AppIndicator (sem ícone na bandeja)
#    --prefix=DIR       destino do "install" (padrão: ~/.local; /usr/local requer sudo)
#    CC=clang ./build.sh            escolhe outro compilador
#    CFLAGS=... LDFLAGS=... ./build.sh   flags extras (ex.: -fsanitize=address)
# =============================================================================
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"

APP=videowallpaper
BUILD_DIR=build
PREFIX="${HOME}/.local"
USE_LAYER_SHELL=auto
USE_TRAY=auto
CC="${CC:-cc}"
CMD=build
RUN_ARGS=()

# ---- cores (só se for um terminal) ------------------------------------------
if [ -t 1 ]; then
    B=$'\e[1m'; R=$'\e[31m'; G=$'\e[32m'; Y=$'\e[33m'; N=$'\e[0m'
else
    B=; R=; G=; Y=; N=
fi
info() { echo "${B}==>${N} $*"; }
ok()   { echo "  ${G}✔${N} $*"; }
warn() { echo "  ${Y}!${N} $*"; }
die()  { echo "${R}erro:${N} $*" >&2; exit 1; }

# ---- argumentos -------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --no-layer-shell) USE_LAYER_SHELL=no ;;
        --no-tray)        USE_TRAY=no ;;
        --prefix=*)       PREFIX="${1#--prefix=}" ;;
        build|debug|install|uninstall|clean|deps) CMD="$1" ;;
        run) CMD=run; shift; RUN_ARGS=("$@"); break ;;
        -h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "argumento desconhecido: $1 (use --help)" ;;
    esac
    shift
done

# ---- dependências -------------------------------------------------------------
distro_family() {
    local id="" like=""
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        id="${ID:-}"; like="${ID_LIKE:-}"
    fi
    case " $id $like " in
        *" debian "*|*" ubuntu "*) echo debian ;;
        *" fedora "*|*" rhel "*)   echo fedora ;;
        *" arch "*)                echo arch ;;
        *" suse "*|*" opensuse "*) echo suse ;;
        *) echo unknown ;;
    esac
}

print_deps_command() {
    case "$(distro_family)" in
        debian) echo "sudo apt install build-essential pkg-config libgtk-3-dev libmpv-dev libepoxy-dev libx11-dev libgtk-layer-shell-dev libayatana-appindicator3-dev" ;;
        fedora) echo "sudo dnf install gcc pkgconf-pkg-config gtk3-devel mpv-devel libepoxy-devel libX11-devel gtk-layer-shell-devel libayatana-appindicator-gtk3-devel"
                echo "   (se mpv-devel não existir, habilite o RPM Fusion e use mpv-libs-devel)" ;;
        arch)   echo "sudo pacman -S --needed base-devel pkgconf gtk3 mpv libepoxy libx11 gtk-layer-shell libayatana-appindicator" ;;
        suse)   echo "sudo zypper install gcc pkg-config gtk3-devel mpv-devel libepoxy-devel libX11-devel gtk-layer-shell-devel libayatana-appindicator3-devel" ;;
        *)      echo "Instale os pacotes de desenvolvimento de: GTK 3, libmpv, libepoxy, libX11, gtk-layer-shell (opcional) e libayatana-appindicator3 (opcional)." ;;
    esac
}

if [ "$CMD" = deps ]; then
    print_deps_command
    exit 0
fi

if [ "$CMD" = clean ]; then
    rm -rf "$BUILD_DIR"
    ok "build/ removido"
    exit 0
fi

if [ "$CMD" = uninstall ]; then
    info "Removendo de $PREFIX"
    rm -fv "$PREFIX/bin/$APP" \
           "$PREFIX/share/applications/$APP.desktop" \
           "$PREFIX/share/icons/hicolor/256x256/apps/$APP.png"
    rm -fv "${XDG_CONFIG_HOME:-$HOME/.config}/autostart/$APP.desktop"
    command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true
    ok "removido (as configurações ficam em ${XDG_CONFIG_HOME:-$HOME/.config}/videowallpaper)"
    exit 0
fi

info "Verificando dependências"
command -v pkg-config >/dev/null || { warn "pkg-config não encontrado"; echo; print_deps_command; exit 1; }
command -v "$CC" >/dev/null || { warn "compilador '$CC' não encontrado"; echo; print_deps_command; exit 1; }

REQUIRED=(gtk+-3.0 mpv epoxy x11)
MISSING=()
for pkg in "${REQUIRED[@]}"; do
    if pkg-config --exists "$pkg"; then
        ok "$pkg $(pkg-config --modversion "$pkg")"
    else
        MISSING+=("$pkg")
    fi
done
if [ ${#MISSING[@]} -gt 0 ]; then
    echo
    warn "faltando: ${MISSING[*]}"
    echo "    Instale com:"
    print_deps_command | sed 's/^/    /'
    exit 1
fi

PKGS=("${REQUIRED[@]}")
DEFS=()

if [ "$USE_LAYER_SHELL" != no ]; then
    if pkg-config --atleast-version=0.6 gtk-layer-shell-0 2>/dev/null; then
        PKGS+=(gtk-layer-shell-0); DEFS+=(-DHAVE_LAYER_SHELL)
        ok "gtk-layer-shell $(pkg-config --modversion gtk-layer-shell-0) (Wayland nativo)"
    else
        warn "gtk-layer-shell >= 0.6 não encontrado: no Wayland o app usará o XWayland"
    fi
fi

if [ "$USE_TRAY" != no ]; then
    if pkg-config --exists ayatana-appindicator3-0.1; then
        PKGS+=(ayatana-appindicator3-0.1); DEFS+=(-DHAVE_APPINDICATOR)
        ok "ayatana-appindicator3 (ícone na bandeja)"
    else
        warn "libayatana-appindicator3 não encontrado: sem ícone na bandeja"
    fi
fi

# ---- compilação --------------------------------------------------------------
case "$CMD" in
    debug) OPT=(-O0 -g3) ;;
    *)     OPT=(-O2 -DNDEBUG) ;;
esac

CFLAGS_ALL=(-std=gnu11 -Wall -Wextra -Wno-missing-field-initializers "${OPT[@]}" "${DEFS[@]}")
read -r -a USER_CFLAGS <<< "${CFLAGS:-}"
CFLAGS_ALL+=("${USER_CFLAGS[@]}")
read -r -a PKG_CFLAGS <<< "$(pkg-config --cflags "${PKGS[@]}")"
read -r -a PKG_LIBS   <<< "$(pkg-config --libs "${PKGS[@]}")"

SOURCES=(src/main.c src/gui.c src/wallpaper.c src/player.c src/config.c)
mkdir -p "$BUILD_DIR/obj"

info "Compilando com $($CC --version | head -n1)"
OBJS=()
for src in "${SOURCES[@]}"; do
    obj="$BUILD_DIR/obj/$(basename "${src%.c}").o"
    echo "  CC  $src"
    "$CC" "${CFLAGS_ALL[@]}" "${PKG_CFLAGS[@]}" -c "$src" -o "$obj"
    OBJS+=("$obj")
done

echo "  LD  $BUILD_DIR/$APP"
"$CC" "${OBJS[@]}" -o "$BUILD_DIR/$APP" "${PKG_LIBS[@]}" ${LDFLAGS:-}
cp -f data/videowallpaper.png "$BUILD_DIR/"
ok "pronto: $BUILD_DIR/$APP"

# ---- instalar / executar ------------------------------------------------------
if [ "$CMD" = install ]; then
    info "Instalando em $PREFIX"
    install -Dm755 "$BUILD_DIR/$APP"           "$PREFIX/bin/$APP"
    install -Dm644 data/videowallpaper.png     "$PREFIX/share/icons/hicolor/256x256/apps/$APP.png"
    mkdir -p "$PREFIX/share/applications"
    sed "s|@BINDIR@|$PREFIX/bin|g" data/videowallpaper.desktop.in > "$PREFIX/share/applications/$APP.desktop"
    command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true
    command -v update-desktop-database >/dev/null && update-desktop-database -q "$PREFIX/share/applications" 2>/dev/null || true
    ok "instalado: $PREFIX/bin/$APP (procure \"Video Wallpaper\" no menu de aplicativos)"
    case ":$PATH:" in
        *":$PREFIX/bin:"*) ;;
        *) warn "$PREFIX/bin não está no PATH" ;;
    esac
elif [ "$CMD" = run ]; then
    info "Executando"
    exec "./$BUILD_DIR/$APP" "${RUN_ARGS[@]}"
fi
