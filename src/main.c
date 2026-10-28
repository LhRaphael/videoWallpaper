/*
 * Video Wallpaper — exibe um vídeo em loop como papel de parede do Windows.
 *
 * main.c — ponto de entrada: instância única, COM, controles comuns e o
 * laço de mensagens da interface.
 *
 * Uso:  VideoWallpaper.exe [/tray]
 *       /tray      inicia escondido na bandeja (usado pela inicialização automática)
 *       /windowed  depuração: mostra o vídeo numa janela comum, não na área de trabalho
 */
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <wchar.h>

#include "gui.h"
#include "wallpaper.h"

static BOOL HasArg(const wchar_t *cmdLine, const wchar_t *arg)
{
    return cmdLine && (wcsstr(cmdLine, arg) != NULL);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdLine, int show)
{
    (void)prev; (void)show;

    /* Só uma instância: a segunda apenas traz a primeira para a frente. */
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\VideoWallpaper.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(GUI_WINDOW_CLASS, NULL);
        if (other) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(other, WM_APP_SHOWGUI, 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    Wallpaper_SetTestMode(HasArg(cmdLine, L"/windowed"));

    int exitCode = 1;
    if (Gui_Create(inst, HasArg(cmdLine, L"/tray"))) {
        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (Gui_PreTranslateMessage(&msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exitCode = (int)msg.wParam;
    }

    if (SUCCEEDED(hrCom)) CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return exitCode;
}
