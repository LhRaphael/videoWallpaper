/*
 * gui.h — janela principal (painel de controle) e ícone da bandeja.
 */
#ifndef VW_GUI_H
#define VW_GUI_H

#include <windows.h>

#define GUI_WINDOW_CLASS L"VideoWallpaper.Main"
#define WM_APP_SHOWGUI   (WM_APP + 200)   /* enviada por uma 2ª instância */

BOOL Gui_Create(HINSTANCE inst, BOOL startHidden);
BOOL Gui_PreTranslateMessage(MSG *msg);   /* navegação com Tab */

#endif
