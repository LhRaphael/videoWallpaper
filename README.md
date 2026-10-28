# Video Wallpaper

Aplicativo para Windows, escrito em **C puro**, que exibe um vídeo em loop como
papel de parede da área de trabalho, **atrás dos ícones**. A interface gráfica
usa Win32 nativo, e não há nenhuma dependência externa: decodificação pelo
Media Foundation do próprio Windows e desenho via Direct3D 11.

## Recursos

- Escolher um vídeo pelo botão **Procurar...** ou arrastando o arquivo para a janela
- Loop contínuo, com decodificação por hardware (GPU)
- Suporte a vários monitores: o vídeo aparece em cada tela, ajustado a ela
- Três modos de ajuste: **Preencher** (corta as bordas), **Ajustar** (barras pretas)
  e **Tamanho original** (centralizado)
- Som opcional (começa mudo), com controle de volume
- Pausa automática quando há um jogo/app em tela cheia (e, opcionalmente,
  quando a janela em foco está maximizada), para economizar GPU
- Ícone na bandeja do sistema: pausar, parar e sair
- Iniciar junto com o Windows e retomar o último vídeo
- Interface nítida em qualquer escala (DPI por monitor)
- Ao parar ou sair, o papel de parede original volta sozinho

## Requisitos

- Windows 10 ou 11 (32 ou 64 bits)
- Formatos: **MP4/MOV com H.264** funcionam em qualquer Windows. Para HEVC
  (H.265), VP9/WebM ou AV1, instale a extensão de vídeo correspondente da
  Microsoft Store. Nas edições "N" do Windows é preciso instalar o
  *Media Feature Pack*.

## Como compilar

### Opção 1: MinGW-w64 (recomendado, gratuito)

Com o [MSYS2](https://www.msys2.org/), abra o terminal **UCRT64** e rode:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc make
cd /c/caminho/para/VideoWallpaper
make
```

Se você usa w64devkit/WinLibs no `cmd`, basta rodar `build_mingw.bat`.

### Opção 2: Visual Studio

Abra o **x64 Native Tools Command Prompt for VS** na pasta do projeto e rode:

```bat
build_msvc.bat
```

### Opção 3: CMake

```sh
cmake -S . -B build
cmake --build build --config Release
```

### Cross-compile a partir do Linux

```sh
sudo apt install mingw-w64
make CROSS=x86_64-w64-mingw32-
```

## Como usar

1. Abra `VideoWallpaper.exe`.
2. Clique em **Procurar...** e escolha um vídeo, ou arraste o arquivo para a janela.
   O vídeo já começa a tocar na área de trabalho.
3. Use **Pausar**, **Parar**, o modo de ajuste e o volume como quiser.
4. Ao fechar a janela, o app continua rodando na bandeja (dá para mudar nas opções).
   Para encerrar de vez: clique com o botão direito no ícone da bandeja e escolha **Sair**.

Parâmetros de linha de comando:

| Parâmetro   | Efeito                                                                   |
|-------------|--------------------------------------------------------------------------|
| `/tray`     | Inicia escondido na bandeja (usado pela inicialização automática)        |
| `/windowed` | Depuração: mostra o vídeo numa janela comum em vez da área de trabalho    |

As configurações ficam em `%APPDATA%\VideoWallpaper\config.ini`. A inicialização
automática usa `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, sem
precisar de administrador.

## Como funciona

```
 arquivo de vídeo
       │  MFCreateFile (byte stream)
       ▼
 Media Foundation Media Engine ──── decodificação na GPU, loop nativo
       │  TransferVideoFrame (um recorte por monitor)
       ▼
 Swap chain DXGI (Direct3D 11) ──── do tamanho da área de trabalho virtual
       │  Present
       ▼
 janela filha atrás dos ícones ──── técnica "WorkerW"
```

**Atrás dos ícones.** A janela da área de trabalho (`Progman`) recebe a
mensagem não documentada `0x052C`, que faz o Explorer separar a camada do
papel de parede numa janela `WorkerW`. O app encaixa a própria janela ali.
O Windows 11 24H2 mudou essa estrutura (o chamado *raised desktop*), e o
código trata os dois casos:

| | Windows 10 / 11 até 23H2 | Windows 11 24H2+ |
|---|---|---|
| Pai da nossa janela | WorkerW de nível superior | `Progman` |
| Posição | topo do WorkerW | logo abaixo de `SHELLDLL_DefView` (ícones) |
| Estilo | janela filha comum | filha `WS_EX_LAYERED` com alfa 255 |
| Apresentação | modelo *flip* | modelo *blt* (recomendação da Microsoft) |

**Uma thread só para o vídeo.** Todo o pipeline (janela, Direct3D e Media
Engine) roda numa thread dedicada, sincronizada ao *vblank* do monitor. Por
isso o vídeo não trava quando a interface abre um diálogo ou é arrastada. A
interface manda comandos por mensagens e recebe o estado de volta.

**Robustez.** A cada 2 segundos o app confere se continua encaixado. Se o
Explorer reiniciar ou reorganizar as janelas, ele se reencaixa sozinho. A
perda do dispositivo gráfico (troca de driver, por exemplo) recria tudo e
retoma do mesmo ponto do vídeo.

### Arquivos

| Arquivo | Conteúdo |
|---|---|
| `src/main.c` | Ponto de entrada, instância única, laço de mensagens |
| `src/gui.c` | Painel de controle, bandeja, pausa automática |
| `src/engine.c` | Thread de vídeo: Media Engine + Direct3D 11 + desenho |
| `src/wallpaper.c` | Encaixe atrás dos ícones (WorkerW / 24H2) e geometria dos monitores |
| `src/config.c` | Configurações (INI Unicode) e inicialização automática |
| `src/mediaengine_c.h` | Interfaces COM do Media Engine declaradas em C (o MinGW não traz `mfmediaengine.h`) |
| `src/debuglog.h` | Log opcional de diagnóstico |
| `res/` | Ícone, manifesto (DPI, estilos visuais) e recurso de versão |

## Solução de problemas

- **O painel diz "Reproduzindo", mas o papel de parede não muda:** feche e
  abra o app de novo, ou reinicie o Explorer (Gerenciador de Tarefas →
  "Windows Explorer" → Reiniciar). Para isolar o problema, rode
  `VideoWallpaper.exe /windowed`: se o vídeo aparecer na janela de teste, o
  pipeline de vídeo está ok e a questão é o encaixe na área de trabalho.
- **Gerar um log:** compile com `make DEBUG=1`, `build_mingw.bat debug`
  ou `build_msvc.bat debug`. O log vai para `%TEMP%\videowallpaper_debug.log`
  e mostra o layout detectado (clássico ou 24H2), a swap chain criada e os erros.
- **"Formato ou codec não suportado":** converta para MP4/H.264, por exemplo:
  `ffmpeg -i entrada.mkv -c:v libx264 -crf 20 -an saida.mp4`
- **O SmartScreen avisa ao abrir:** é normal para um executável sem
  assinatura digital. Use "Mais informações → Executar assim mesmo".
