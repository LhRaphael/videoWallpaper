# Video Wallpaper (Linux)

Versão Linux do Video Wallpaper: exibe um vídeo em loop como papel de parede,
**atrás de todas as janelas**, com painel de controle gráfico. Escrito em C,
usando GTK 3 na interface, libmpv para decodificar (com aceleração por hardware)
e OpenGL para desenhar.

## Recursos

- Escolher o vídeo pelo botão **Procurar...** ou arrastando o arquivo para a janela
- Loop contínuo, com decodificação na GPU quando disponível (VA-API, VDPAU, NVDEC)
- Vários monitores, com suporte a conectar e desconectar telas com o app aberto
- Três modos de ajuste: **Preencher** (corta as bordas), **Ajustar** (barras
  pretas) e **Tamanho original** (centralizado)
- Som opcional (começa mudo), com volume
- Pausa automática quando um jogo/app está em tela cheia, ou quando a janela em
  foco está maximizada (só no X11; veja abaixo)
- Ícone na bandeja do sistema (AppIndicator), com pausar, parar e sair
- Iniciar junto com a sessão e retomar o último vídeo
- Instância única: abrir o app de novo só traz o painel de volta
- Cliques atravessam o vídeo: o menu da área de trabalho continua funcionando

## Compatibilidade

| Ambiente | Como funciona | Situação |
|---|---|---|
| **X11** (XFCE, Cinnamon, MATE, KDE X11, GNOME X11, i3, Openbox...) | janela `_NET_WM_WINDOW_TYPE_DESKTOP` sempre abaixo | ✅ |
| **Wayland com layer-shell** (KDE Plasma, Sway, Hyprland, river, Wayfire, labwc, COSMIC) | superfície `wlr-layer-shell` na camada BOTTOM | ✅ |
| **GNOME no Wayland** | o GNOME não implementa layer-shell; o app se reinicia sozinho pelo **XWayland** | ⚠️ funciona como no X11 |

**Ícones da área de trabalho:** em ambientes que desenham ícones no fundo (XFCE,
Cinnamon/Nemo, KDE, GNOME com a extensão de ícones), o vídeo pode ficar por
cima dos ícones. Os cliques ainda passam para a área de trabalho. É a mesma
limitação de outros apps de papel de parede animado no Linux.

**Pausa automática no Wayland:** o Wayland não informa a outros aplicativos
qual janela está em tela cheia, então essas opções ficam desativadas lá.

## Compilação

### 1. Instale as dependências

O próprio script mostra o comando certo para a sua distro:

```sh
./build.sh deps
```

| Distro | Comando |
|---|---|
| Ubuntu / Debian / Mint / Pop!_OS | `sudo apt install build-essential pkg-config libgtk-3-dev libmpv-dev libepoxy-dev libx11-dev libgtk-layer-shell-dev libayatana-appindicator3-dev` |
| Fedora | `sudo dnf install gcc pkgconf-pkg-config gtk3-devel mpv-devel libepoxy-devel libX11-devel gtk-layer-shell-devel libayatana-appindicator-gtk3-devel` |
| Arch / Manjaro / EndeavourOS | `sudo pacman -S --needed base-devel pkgconf gtk3 mpv libepoxy libx11 gtk-layer-shell libayatana-appindicator` |
| openSUSE | `sudo zypper install gcc pkg-config gtk3-devel mpv-devel libepoxy-devel libX11-devel gtk-layer-shell-devel libayatana-appindicator3-devel` |

`gtk-layer-shell` e `libayatana-appindicator3` são opcionais. Sem eles, o script
compila mesmo assim, sem Wayland nativo e sem ícone na bandeja.

### 2. Compile e execute

```sh
chmod +x build.sh
./build.sh          # gera build/videowallpaper
./build.sh run      # compila e executa
```

### 3. (Opcional) Instale no menu de aplicativos

```sh
./build.sh install      # em ~/.local, sem sudo
./build.sh uninstall    # remove
```

Para instalar para todos os usuários: `sudo ./build.sh --prefix=/usr/local install`.

### Todas as opções do script

| Comando | Efeito |
|---|---|
| `./build.sh` | compila em modo release (`-O2`) |
| `./build.sh debug` | compila com símbolos de depuração (`-O0 -g3`) |
| `./build.sh run [args]` | compila e executa |
| `./build.sh install` / `uninstall` | instala ou remove em `~/.local` (ou `--prefix=DIR`) |
| `./build.sh deps` | mostra o comando de dependências da sua distro |
| `./build.sh clean` | apaga `build/` |
| `--no-layer-shell` / `--no-tray` | desliga os recursos opcionais |
| `CC=clang`, `CFLAGS=...`, `LDFLAGS=...` | compilador e flags extras |

## Uso

1. Abra o **Video Wallpaper** (pelo menu ou com `./build/videowallpaper`).
2. Clique em **Procurar...** e escolha o vídeo, ou arraste o arquivo para a janela.
   Ele começa a tocar no fundo na hora.
3. Ao fechar a janela, o vídeo continua rodando. Para reabrir o painel, abra o
   app de novo ou use o ícone da bandeja. Para encerrar: **Sair** no ícone da
   bandeja, ou desmarque "continuar rodando em segundo plano" e feche.

Linha de comando:

| Opção | Efeito |
|---|---|
| `--hidden` | inicia sem mostrar o painel (usado pelo autostart) |
| `--windowed` | depuração: vídeo numa janela comum em vez do fundo |
| `--version` | mostra a versão |

Variáveis de ambiente para diagnóstico:

| Variável | Efeito |
|---|---|
| `VW_DEBUG=1` | log detalhado do mpv no terminal |
| `VW_HWDEC=no` | desliga a decodificação por hardware |
| `VW_LAYER=background` | Wayland: usa a camada de fundo em vez da BOTTOM |
| `VW_NO_REEXEC=1` | não reinicia pelo XWayland quando não há layer-shell |

Arquivos:

- configurações: `~/.config/videowallpaper/config.ini`
- autostart: `~/.config/autostart/videowallpaper.desktop` (padrão XDG,
  funciona em todos os ambientes)

## Como funciona

```
 arquivo de vídeo
       │  libmpv (FFmpeg + VA-API/VDPAU/NVDEC, loop-file=inf)
       ▼
 API de render OpenGL do mpv ──── um reprodutor por monitor
       │  desenha no framebuffer de um GtkGLArea
       ▼
 janela de fundo por monitor
   ├─ X11:     _NET_WM_WINDOW_TYPE_DESKTOP + BELOW + sticky + entrada vazia
   └─ Wayland: gtk-layer-shell, camada BOTTOM, ancorada nas 4 bordas
```

| Arquivo | Conteúdo |
|---|---|
| `src/main.c` | GtkApplication (instância única), opções e fallback para XWayland |
| `src/gui.c` | Painel de controle, bandeja e pausa automática (EWMH no X11) |
| `src/wallpaper.c` | Uma superfície de fundo por monitor (X11 / layer-shell) e hotplug |
| `src/player.c` | libmpv + GtkGLArea, eventos e controle do reprodutor |
| `src/config.c` | Configurações (GKeyFile) e autostart XDG |
| `build.sh` | Compilação, verificação de dependências, instalação |

**Decisões de projeto:**

- **Só API assíncrona da libmpv depois de iniciar.** A thread do GTK também é
  a thread de render do mpv. Uma chamada síncrona (`mpv_command`,
  `mpv_set_property`) feita dela pode travar o programa: o núcleo do mpv
  espera o render, e o render espera a chamada.
- **Camada BOTTOM no Wayland.** Fica acima do papel de parede do próprio
  sistema (swaybg, hyprpaper, desktop do Plasma), que também ocupa a camada
  BACKGROUND, e abaixo de janelas e painéis.
- **O `wl_display` não é passado ao mpv.** A libva-wayland depende do protocolo
  `wl_drm`, que compositores recentes não oferecem, e pode derrubar o processo.
  Sem ele, o mpv usa a VA-API direto pelo `/dev/dri/renderD*`.
- **Perfil `fast` do mpv.** Escala bilinear e menos etapas de pós-processamento:
  num papel de parede, que fica rodando o tempo todo, economia de GPU vale mais
  que qualidade máxima de escala.

## Solução de problemas

- **Nada aparece no fundo:** rode com `--windowed`. Se o vídeo tocar na janela
  de teste, o problema está no encaixe no fundo do seu ambiente. Rode no
  terminal e veja a linha "Modo:" no rodapé do painel.
- **Uso alto de CPU:** a decodificação por hardware não está ativa. Rode com
  `VW_DEBUG=1` e procure as linhas `hwdec`. Instale o driver VA-API da sua GPU:
  `intel-media-va-driver` / `mesa-va-drivers` / `nvidia-vaapi-driver`.
- **Sway/Hyprland: o vídeo ficou por cima de uma dock:** use
  `VW_LAYER=background` e desative o swaybg/hyprpaper.
- **Sem ícone na bandeja no GNOME:** instale a extensão "AppIndicator and
  KStatusNotifierItem Support" (o Ubuntu já traz).
