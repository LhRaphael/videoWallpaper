/*
 * config.h — configurações persistidas em %APPDATA%\VideoWallpaper\config.ini
 * e inicialização automática via HKCU\...\Run.
 */
#ifndef VW_CONFIG_H
#define VW_CONFIG_H

#include <windows.h>

#define CONFIG_PATH_MAX 1024

typedef struct Config {
    wchar_t videoPath[CONFIG_PATH_MAX];
    BOOL    muted;
    int     volume;            /* 0..100 */
    int     scaleMode;         /* ScaleMode */
    BOOL    resumeOnStart;     /* volta a tocar o último vídeo ao abrir */
    BOOL    wasPlaying;        /* estava tocando quando o app fechou    */
    BOOL    pauseFullscreen;   /* pausa com app/jogo em tela cheia      */
    BOOL    pauseMaximized;    /* pausa com janela maximizada em foco   */
    BOOL    closeToTray;       /* X da janela só esconde na bandeja     */
    BOOL    trayHintShown;
} Config;

void Config_Load(Config *c);
void Config_Save(const Config *c);

BOOL Autostart_IsEnabled(void);
BOOL Autostart_Set(BOOL enable);   /* grava o caminho atual do .exe */

#endif
