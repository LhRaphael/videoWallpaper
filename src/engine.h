/*
 * engine.h — motor de reprodução.
 *
 * Roda numa thread dedicada que é dona de tudo que toca na área de
 * trabalho: a janela de vídeo, o Direct3D 11 e o Media Foundation Media
 * Engine. Assim o vídeo continua rodando mesmo quando a interface está
 * ocupada (diálogo "Abrir arquivo", arrastar a janela etc.).
 *
 * A interface conversa com o motor só por estas funções (que postam
 * mensagens para a thread) e recebe notificações via PostMessage na
 * janela informada em Engine_Start.
 */
#ifndef VW_ENGINE_H
#define VW_ENGINE_H

#include <windows.h>

typedef enum EngineState {
    ENGINE_IDLE = 0,     /* nada tocando, papel de parede original     */
    ENGINE_LOADING,      /* abrindo o arquivo                           */
    ENGINE_PLAYING,
    ENGINE_PAUSED,       /* pausado pelo usuário                         */
    ENGINE_AUTOPAUSED,   /* pausado automaticamente (tela cheia etc.)    */
    ENGINE_ERROR
} EngineState;

typedef enum ScaleMode {
    SCALE_FILL = 0,      /* preenche a tela, cortando as sobras          */
    SCALE_FIT,           /* vídeo inteiro, com barras pretas            */
    SCALE_CENTER,        /* tamanho original, centralizado               */
    SCALE_COUNT
} ScaleMode;

/* Notificações enviadas à janela da interface */
#define WM_APP_ENGINE_STATE  (WM_APP + 100)  /* wParam = EngineState         */
#define WM_APP_ENGINE_VIDEO  (WM_APP + 101)  /* wParam = largura, lParam = altura */
#define WM_APP_ENGINE_ERROR  (WM_APP + 102)  /* wParam = código MF_MEDIA_ENGINE_ERR
                                                 (0 = erro nosso), lParam = HRESULT */

BOOL Engine_Start(HWND notifyWindow);
void Engine_Shutdown(void);          /* para tudo e restaura o papel de parede */

void Engine_Open(const wchar_t *path, BOOL muted, int volume0to100, ScaleMode scale);
void Engine_Stop(void);
void Engine_SetUserPaused(BOOL paused);
void Engine_SetAutoPaused(BOOL paused);
void Engine_SetMuted(BOOL muted);
void Engine_SetVolume(int volume0to100);
void Engine_SetScaleMode(ScaleMode scale);
void Engine_RefreshLayout(void);     /* resolução/monitores mudaram  */
void Engine_Reattach(void);          /* Explorer reiniciou            */

#endif
