/*
 * wallpaper.h — encaixe de uma janela nossa atrás dos ícones da área de
 * trabalho (técnica "WorkerW").
 *
 * Suporta os dois layouts do Explorer:
 *   - Clássico (Windows 10 e Windows 11 até 23H2): a janela é filha do
 *     WorkerW de nível superior que fica logo abaixo do host dos ícones.
 *   - "Raised desktop" (Windows 11 24H2+): a janela é uma filha em camadas
 *     (WS_EX_LAYERED, alfa 255) do Progman, posicionada logo abaixo do
 *     SHELLDLL_DefView (ícones) e acima do WorkerW do Explorer.
 *
 * Todas as funções devem ser chamadas pela MESMA thread que criou a janela.
 */
#ifndef VW_WALLPAPER_H
#define VW_WALLPAPER_H

#include <windows.h>

#define WALL_MAX_MONITORS 16

typedef struct WallTarget {
    HWND hwnd;       /* nossa janela de vídeo                          */
    HWND parent;     /* WorkerW (clássico) ou Progman (24H2+)          */
    HWND defview;    /* SHELLDLL_DefView — só usado no layout 24H2+    */
    BOOL raised;     /* TRUE = layout 24H2+ ("raised desktop")          */
    BOOL layered;    /* janela criada com WS_EX_LAYERED                 */
} WallTarget;

typedef struct WallGeometry {
    UINT width, height;                   /* área cliente da nossa janela */
    int  monitorCount;
    RECT monitors[WALL_MAX_MONITORS];     /* em coords. cliente da janela */
} WallGeometry;

/* Modo de teste (/windowed): desenha numa janela comum em vez da área de
 * trabalho. Útil para depurar o pipeline de vídeo. */
void Wallpaper_SetTestMode(BOOL enable);

/* Cria a janela e a encaixa na área de trabalho. */
BOOL Wallpaper_Attach(HINSTANCE inst, WallTarget *out);

/* Destrói a janela e faz o Explorer redesenhar o papel de parede estático. */
void Wallpaper_Detach(WallTarget *t);

/* Verificação periódica. Reposiciona z-order/tamanho se o Explorer mexeu.
 * Retorna FALSE se o encaixe se perdeu (ex.: Explorer reiniciou) e é
 * preciso chamar Wallpaper_Attach de novo. *resized indica mudança de
 * tamanho. */
BOOL Wallpaper_Maintain(WallTarget *t, BOOL *resized);

/* Tamanho da janela e retângulo de cada monitor dentro dela. */
void Wallpaper_GetGeometry(const WallTarget *t, WallGeometry *g);

#endif
