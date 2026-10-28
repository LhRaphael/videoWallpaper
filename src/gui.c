/*
 * gui.c — painel de controle do Video Wallpaper (Win32 puro).
 */
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <mferror.h>
#include <wchar.h>
#include <stdio.h>

#include "gui.h"
#include "engine.h"
#include "config.h"
#include "resource.h"

#define WM_APP_TRAY       (WM_APP + 201)
#define TIMER_AUTOPAUSE   1
#define TIMER_REFRESH     2
#define TIMER_TRAYRETRY   3
#define AUTOPAUSE_MS      1500

/* IDs dos controles e do menu da bandeja */
enum {
    IDC_PATH = 100, IDC_BROWSE, IDC_APPLY, IDC_PAUSE, IDC_STOP,
    IDC_MUTE, IDC_VOLUME, IDC_VOLVALUE, IDC_SCALE,
    IDC_AUTOSTART, IDC_RESUME, IDC_PAUSEFULL, IDC_PAUSEMAX, IDC_TRAY,
    IDC_STATUS,
    IDM_SHOW = 300, IDM_PAUSE, IDM_STOP, IDM_EXIT
};

static struct {
    HINSTANCE inst;
    HWND  hwnd;
    HFONT font;
    UINT  dpi;
    HICON iconBig, iconSmall;

    HWND grpVideo, editPath, btnBrowse;
    HWND grpPlay, btnApply, btnPause, btnStop;
    HWND chkMute, lblVol, trkVol, lblVolValue, lblScale, cmbScale;
    HWND grpOpt, chkAutostart, chkResume, chkFull, chkMax, chkTray;
    HWND lblStatus;

    Config      cfg;
    EngineState state;
    DWORD       vidW, vidH;
    BOOL        userPaused;
    BOOL        autoPaused;
    wchar_t     errorText[512];

    UINT msgTaskbarCreated;
    BOOL trayAdded;
    BOOL engineOk;
    int  trayRetries;
} G;

/* ======================================================================
 * DPI
 * ====================================================================== */

typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
typedef BOOL (WINAPI *PFN_AdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);

static UINT DpiOf(HWND hwnd)
{
    static PFN_GetDpiForWindow p;
    static BOOL loaded;
    if (!loaded) {
        loaded = TRUE;
        p = (PFN_GetDpiForWindow)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    }
    if (p && hwnd) {
        UINT d = p(hwnd);
        if (d) return d;
    }
    HDC dc = GetDC(NULL);
    UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    return d ? d : 96;
}

static void AdjustRectForDpi(RECT *r, DWORD style, DWORD exStyle, UINT dpi)
{
    static PFN_AdjustWindowRectExForDpi p;
    static BOOL loaded;
    if (!loaded) {
        loaded = TRUE;
        p = (PFN_AdjustWindowRectExForDpi)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                                 "AdjustWindowRectExForDpi");
    }
    if (p) p(r, style, FALSE, exStyle, dpi);
    else   AdjustWindowRectEx(r, style, FALSE, exStyle);
}

static int S(int v) { return MulDiv(v, (int)G.dpi, 96); }

/* ======================================================================
 * Layout
 * ====================================================================== */

#define CLIENT_W 500
#define CLIENT_H 436
#define MAIN_STYLE (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)

typedef struct { HWND *h; int x, y, w, hgt; } Slot;

static void ApplyFont(void)
{
    if (G.font) DeleteObject(G.font);
    G.font = CreateFontW(-MulDiv(9, (int)G.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static void Layout(void)
{
    const Slot slots[] = {
        { &G.grpVideo,     12,   8, 476,  62 },
        { &G.editPath,     24,  32, 350,  24 },
        { &G.btnBrowse,   382,  31,  94,  26 },

        { &G.grpPlay,      12,  78, 476, 138 },
        { &G.btnApply,     24, 102, 146,  30 },
        { &G.btnPause,    177, 102, 146,  30 },
        { &G.btnStop,     330, 102, 146,  30 },
        { &G.chkMute,      24, 145,  92,  22 },
        { &G.lblVol,      122, 148,  50,  20 },
        { &G.trkVol,      172, 142, 256,  28 },
        { &G.lblVolValue, 432, 148,  44,  20 },
        { &G.lblScale,     24, 183,  52,  20 },
        { &G.cmbScale,     80, 179, 396, 200 },

        { &G.grpOpt,       12, 224, 476, 154 },
        { &G.chkAutostart, 24, 248, 452,  22 },
        { &G.chkResume,    24, 272, 452,  22 },
        { &G.chkFull,      24, 296, 452,  22 },
        { &G.chkMax,       24, 320, 452,  22 },
        { &G.chkTray,      24, 344, 452,  22 },

        { &G.lblStatus,    14, 388, 472,  44 },
    };
    HDWP dwp = BeginDeferWindowPos((int)(sizeof(slots) / sizeof(slots[0])));
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        HWND h = *slots[i].h;
        SendMessageW(h, WM_SETFONT, (WPARAM)G.font, FALSE);
        if (dwp)
            dwp = DeferWindowPos(dwp, h, NULL, S(slots[i].x), S(slots[i].y),
                                 S(slots[i].w), S(slots[i].hgt), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (dwp) EndDeferWindowPos(dwp);
    InvalidateRect(G.hwnd, NULL, TRUE);
}

static void ResizeToClient(void)
{
    RECT r = { 0, 0, S(CLIENT_W), S(CLIENT_H) };
    AdjustRectForDpi(&r, MAIN_STYLE, 0, G.dpi);
    SetWindowPos(G.hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static HWND MakeControl(DWORD exStyle, const wchar_t *cls, const wchar_t *text, DWORD style, int id)
{
    return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                           0, 0, 10, 10, G.hwnd, (HMENU)(INT_PTR)id, G.inst, NULL);
}

static void CreateControls(void)
{
    G.grpVideo  = MakeControl(0, L"BUTTON", L"Vídeo", BS_GROUPBOX, -1);
    G.editPath  = MakeControl(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY, IDC_PATH);
    G.btnBrowse = MakeControl(0, L"BUTTON", L"Procurar...", WS_TABSTOP | BS_PUSHBUTTON, IDC_BROWSE);

    G.grpPlay  = MakeControl(0, L"BUTTON", L"Reprodução", BS_GROUPBOX, -1);
    G.btnApply = MakeControl(0, L"BUTTON", L"Aplicar", WS_TABSTOP | BS_DEFPUSHBUTTON, IDC_APPLY);
    G.btnPause = MakeControl(0, L"BUTTON", L"Pausar", WS_TABSTOP | BS_PUSHBUTTON, IDC_PAUSE);
    G.btnStop  = MakeControl(0, L"BUTTON", L"Parar", WS_TABSTOP | BS_PUSHBUTTON, IDC_STOP);
    G.chkMute  = MakeControl(0, L"BUTTON", L"Sem som", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_MUTE);
    G.lblVol   = MakeControl(0, L"STATIC", L"Volume:", SS_LEFT, -1);
    G.trkVol   = MakeControl(0, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, IDC_VOLUME);
    G.lblVolValue = MakeControl(0, L"STATIC", L"", SS_RIGHT, IDC_VOLVALUE);
    G.lblScale = MakeControl(0, L"STATIC", L"Ajuste:", SS_LEFT, -1);
    G.cmbScale = MakeControl(0, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_SCALE);

    G.grpOpt       = MakeControl(0, L"BUTTON", L"Opções", BS_GROUPBOX, -1);
    G.chkAutostart = MakeControl(0, L"BUTTON", L"Iniciar junto com o Windows",
                                 WS_TABSTOP | BS_AUTOCHECKBOX, IDC_AUTOSTART);
    G.chkResume    = MakeControl(0, L"BUTTON", L"Retomar o último vídeo ao abrir o aplicativo",
                                 WS_TABSTOP | BS_AUTOCHECKBOX, IDC_RESUME);
    G.chkFull      = MakeControl(0, L"BUTTON", L"Pausar quando um jogo ou aplicativo estiver em tela cheia",
                                 WS_TABSTOP | BS_AUTOCHECKBOX, IDC_PAUSEFULL);
    G.chkMax       = MakeControl(0, L"BUTTON", L"Pausar quando a janela em foco estiver maximizada",
                                 WS_TABSTOP | BS_AUTOCHECKBOX, IDC_PAUSEMAX);
    G.chkTray      = MakeControl(0, L"BUTTON", L"Ao fechar, continuar rodando na bandeja do sistema",
                                 WS_TABSTOP | BS_AUTOCHECKBOX, IDC_TRAY);

    G.lblStatus = MakeControl(0, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_STATUS);

    SendMessageW(G.trkVol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(G.trkVol, TBM_SETPAGESIZE, 0, 10);

    ComboBox_AddString(G.cmbScale, L"Preencher a tela (corta as bordas se precisar)");
    ComboBox_AddString(G.cmbScale, L"Ajustar à tela (vídeo inteiro, com barras pretas)");
    ComboBox_AddString(G.cmbScale, L"Tamanho original (centralizado)");
}

/* ======================================================================
 * Estado → interface
 * ====================================================================== */

static const wchar_t *BaseName(const wchar_t *p)
{
    const wchar_t *a = wcsrchr(p, L'\\');
    const wchar_t *b = wcsrchr(p, L'/');
    if (b > a) a = b;
    return a ? a + 1 : p;
}

static BOOL EngineIsActive(void)
{
    return G.state == ENGINE_LOADING || G.state == ENGINE_PLAYING ||
           G.state == ENGINE_PAUSED  || G.state == ENGINE_AUTOPAUSED;
}

static void Tray_Update(const wchar_t *tip);

static void RefreshUi(void)
{
    wchar_t buf[640];
    const wchar_t *name = BaseName(G.cfg.videoPath);

    switch (G.state) {
    case ENGINE_LOADING:
        _snwprintf(buf, 640, L"Carregando “%s”...", name);
        break;
    case ENGINE_PLAYING:
        _snwprintf(buf, 640, L"Reproduzindo “%s” (%lu×%lu) em loop na área de trabalho.",
                   name, (unsigned long)G.vidW, (unsigned long)G.vidH);
        break;
    case ENGINE_PAUSED:
        _snwprintf(buf, 640, L"Pausado — “%s”.", name);
        break;
    case ENGINE_AUTOPAUSED:
        _snwprintf(buf, 640, L"Pausado automaticamente enquanto há um aplicativo em tela cheia "
                             L"ou uma janela maximizada em foco.");
        break;
    case ENGINE_ERROR:
        _snwprintf(buf, 640, L"%s", G.errorText);
        break;
    case ENGINE_IDLE:
    default:
        _snwprintf(buf, 640, G.cfg.videoPath[0]
                   ? L"Parado. Clique em Aplicar para usar o vídeo como papel de parede."
                   : L"Escolha um vídeo (ou arraste um arquivo para esta janela).");
        break;
    }
    buf[639] = 0;
    SetWindowTextW(G.lblStatus, buf);

    BOOL active = EngineIsActive();
    EnableWindow(G.btnApply, G.cfg.videoPath[0] != 0);
    EnableWindow(G.btnPause, active);
    EnableWindow(G.btnStop, active);
    SetWindowTextW(G.btnPause, G.userPaused ? L"Continuar" : L"Pausar");
    EnableWindow(G.trkVol, !G.cfg.muted);

    wchar_t tip[128];
    if (active)
        _snwprintf(tip, 128, L"Video Wallpaper — %s%s",
                   G.state == ENGINE_PLAYING ? L"" : L"(pausado) ", name);
    else
        _snwprintf(tip, 128, L"Video Wallpaper");
    tip[127] = 0;
    Tray_Update(tip);
}

static void LoadConfigIntoUi(void)
{
    SetWindowTextW(G.editPath, G.cfg.videoPath);
    Button_SetCheck(G.chkMute, G.cfg.muted ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(G.trkVol, TBM_SETPOS, TRUE, G.cfg.volume);
    wchar_t v[16];
    _snwprintf(v, 16, L"%d%%", G.cfg.volume);
    v[15] = 0;
    SetWindowTextW(G.lblVolValue, v);
    ComboBox_SetCurSel(G.cmbScale, G.cfg.scaleMode);
    Button_SetCheck(G.chkAutostart, Autostart_IsEnabled() ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(G.chkResume, G.cfg.resumeOnStart ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(G.chkFull, G.cfg.pauseFullscreen ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(G.chkMax, G.cfg.pauseMaximized ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(G.chkTray, G.cfg.closeToTray ? BST_CHECKED : BST_UNCHECKED);
}

static void DescribeError(int meErr, HRESULT hr)
{
    const wchar_t *msg;
    if (meErr == 4 || hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE || hr == MF_E_TOPO_CODEC_NOT_FOUND ||
        hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_UNSUPPORTED_FORMAT) {
        msg = L"Formato ou codec não suportado pelo Windows. MP4 com H.264 sempre funciona; "
              L"para HEVC, VP9 ou AV1 instale a extensão de vídeo correspondente da Microsoft Store.";
    } else if (meErr == 3) {
        msg = L"Erro ao decodificar o vídeo (arquivo corrompido ou codec ausente).";
    } else if (meErr == 2 || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
               hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) ||
               hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ||
               hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) {
        msg = L"Não foi possível ler o arquivo de vídeo (ele existe e não está bloqueado?).";
    } else if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        msg = L"Não foi possível encontrar a camada do papel de parede da área de trabalho. "
              L"O Explorer do Windows está em execução?";
    } else {
        _snwprintf(G.errorText, 512, L"Erro ao reproduzir o vídeo (código 0x%08lX, %d).",
                   (unsigned long)hr, meErr);
        G.errorText[511] = 0;
        return;
    }
    _snwprintf(G.errorText, 512, L"%s", msg);
    G.errorText[511] = 0;
}

/* ======================================================================
 * Bandeja do sistema
 * ====================================================================== */

static void Tray_Add(void)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = G.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = G.iconSmall;
    wcscpy(nid.szTip, L"Video Wallpaper");
    G.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid);
}

static void Tray_Remove(void)
{
    if (!G.trayAdded) return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = G.hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    G.trayAdded = FALSE;
}

static void Tray_Update(const wchar_t *tip)
{
    if (!G.trayAdded) return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = G.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    wcsncpy(nid.szTip, tip, sizeof(nid.szTip) / sizeof(nid.szTip[0]) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void Tray_Balloon(const wchar_t *title, const wchar_t *text)
{
    if (!G.trayAdded) return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = G.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy(nid.szInfoTitle, title, sizeof(nid.szInfoTitle) / sizeof(nid.szInfoTitle[0]) - 1);
    wcsncpy(nid.szInfo, text, sizeof(nid.szInfo) / sizeof(nid.szInfo[0]) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void ShowMainWindow(void)
{
    if (IsIconic(G.hwnd)) ShowWindow(G.hwnd, SW_RESTORE);
    else ShowWindow(G.hwnd, SW_SHOW);
    SetForegroundWindow(G.hwnd);
}

static void Tray_Menu(void)
{
    HMENU m = CreatePopupMenu();
    if (!m) return;
    BOOL active = EngineIsActive();
    AppendMenuW(m, MF_STRING, IDM_SHOW, L"Abrir painel");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (active ? 0 : MF_GRAYED), IDM_PAUSE,
                G.userPaused ? L"Continuar" : L"Pausar");
    AppendMenuW(m, MF_STRING | (active ? 0 : MF_GRAYED), IDM_STOP, L"Parar e restaurar papel de parede");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Sair");
    SetMenuDefaultItem(m, IDM_SHOW, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(G.hwnd);     /* necessário para o menu fechar direito */
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, G.hwnd, NULL);
    PostMessageW(G.hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ======================================================================
 * Ações
 * ====================================================================== */

static void Apply(void)
{
    if (!G.cfg.videoPath[0]) return;
    if (GetFileAttributesW(G.cfg.videoPath) == INVALID_FILE_ATTRIBUTES) {
        _snwprintf(G.errorText, 512, L"Arquivo não encontrado: %s", G.cfg.videoPath);
        G.errorText[511] = 0;
        G.state = ENGINE_ERROR;
        RefreshUi();
        return;
    }
    G.userPaused = FALSE;
    G.state = ENGINE_LOADING;
    Engine_Open(G.cfg.videoPath, G.cfg.muted, G.cfg.volume, (ScaleMode)G.cfg.scaleMode);
    G.cfg.wasPlaying = TRUE;
    Config_Save(&G.cfg);
    RefreshUi();
}

static void SetVideoPath(const wchar_t *path)
{
    wcsncpy(G.cfg.videoPath, path, CONFIG_PATH_MAX - 1);
    G.cfg.videoPath[CONFIG_PATH_MAX - 1] = 0;
    SetWindowTextW(G.editPath, G.cfg.videoPath);
    Apply();
}

static void Browse(void)
{
    wchar_t file[CONFIG_PATH_MAX];
    wcsncpy(file, G.cfg.videoPath, CONFIG_PATH_MAX - 1);
    file[CONFIG_PATH_MAX - 1] = 0;

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = G.hwnd;
    ofn.lpstrFilter = L"Vídeos (*.mp4;*.m4v;*.mkv;*.mov;*.webm;*.wmv;*.avi)\0"
                      L"*.mp4;*.m4v;*.mkv;*.mov;*.webm;*.wmv;*.avi\0"
                      L"Todos os arquivos (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = CONFIG_PATH_MAX;
    ofn.lpstrTitle  = L"Escolha o vídeo para o papel de parede";
    ofn.Flags       = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn))
        SetVideoPath(file);
}

static void TogglePause(void)
{
    if (!EngineIsActive()) return;
    G.userPaused = !G.userPaused;
    Engine_SetUserPaused(G.userPaused);
    RefreshUi();
}

static void Stop(void)
{
    G.userPaused = FALSE;
    Engine_Stop();
    G.state = ENGINE_IDLE;
    G.cfg.wasPlaying = FALSE;
    Config_Save(&G.cfg);
    RefreshUi();
}

static BOOL IsShellWindow(HWND h)
{
    wchar_t cls[64];
    if (!GetClassNameW(h, cls, 64)) return FALSE;
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
           wcscmp(cls, L"Shell_TrayWnd") == 0;
}

static void CheckAutoPause(void)
{
    BOOL want = FALSE;
    if (G.cfg.pauseFullscreen) {
        QUERY_USER_NOTIFICATION_STATE q;
        if (SUCCEEDED(SHQueryUserNotificationState(&q)))
            want = q == QUNS_BUSY || q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE;
    }
    if (!want && G.cfg.pauseMaximized) {
        HWND fg = GetForegroundWindow();
        if (fg && fg != G.hwnd && IsWindowVisible(fg) && IsZoomed(fg) && !IsShellWindow(fg))
            want = TRUE;
    }
    if (want != G.autoPaused) {
        G.autoPaused = want;
        Engine_SetAutoPaused(want);
    }
}

static void Quit(void)
{
    DestroyWindow(G.hwnd);
}

/* ======================================================================
 * Janela principal
 * ====================================================================== */

static void OnCommand(int id, int code)
{
    switch (id) {
    case IDC_BROWSE: Browse(); break;
    case IDC_APPLY:
        if (!G.cfg.videoPath[0]) Browse(); else Apply();
        break;
    case IDC_PAUSE:  TogglePause(); break;
    case IDC_STOP:   Stop(); break;

    case IDC_MUTE:
        G.cfg.muted = Button_GetCheck(G.chkMute) == BST_CHECKED;
        Engine_SetMuted(G.cfg.muted);
        Config_Save(&G.cfg);
        RefreshUi();
        break;

    case IDC_SCALE:
        if (code == CBN_SELCHANGE) {
            int sel = ComboBox_GetCurSel(G.cmbScale);
            if (sel >= 0 && sel < SCALE_COUNT) {
                G.cfg.scaleMode = sel;
                Engine_SetScaleMode((ScaleMode)sel);
                Config_Save(&G.cfg);
            }
        }
        break;

    case IDC_AUTOSTART: {
        BOOL on = Button_GetCheck(G.chkAutostart) == BST_CHECKED;
        if (!Autostart_Set(on)) {
            Button_SetCheck(G.chkAutostart, on ? BST_UNCHECKED : BST_CHECKED);
            MessageBoxW(G.hwnd, L"Não foi possível alterar a inicialização automática.",
                        L"Video Wallpaper", MB_ICONWARNING);
        }
        break;
    }
    case IDC_RESUME:
        G.cfg.resumeOnStart = Button_GetCheck(G.chkResume) == BST_CHECKED;
        Config_Save(&G.cfg);
        break;
    case IDC_PAUSEFULL:
        G.cfg.pauseFullscreen = Button_GetCheck(G.chkFull) == BST_CHECKED;
        Config_Save(&G.cfg);
        CheckAutoPause();
        break;
    case IDC_PAUSEMAX:
        G.cfg.pauseMaximized = Button_GetCheck(G.chkMax) == BST_CHECKED;
        Config_Save(&G.cfg);
        CheckAutoPause();
        break;
    case IDC_TRAY:
        G.cfg.closeToTray = Button_GetCheck(G.chkTray) == BST_CHECKED;
        Config_Save(&G.cfg);
        break;

    case IDM_SHOW:  ShowMainWindow(); break;
    case IDM_PAUSE: TogglePause(); break;
    case IDM_STOP:  Stop(); break;
    case IDM_EXIT:  Quit(); break;
    }
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (G.msgTaskbarCreated && msg == G.msgTaskbarCreated) {
        /* Explorer reiniciou: recoloca o ícone e reencaixa o vídeo. */
        G.trayAdded = FALSE;
        Tray_Add();
        if (G.trayAdded) KillTimer(hwnd, TIMER_TRAYRETRY);
        RefreshUi();
        Engine_Reattach();
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        G.hwnd = hwnd;
        G.dpi = DpiOf(hwnd);
        ApplyFont();
        CreateControls();
        Layout();
        DragAcceptFiles(hwnd, TRUE);
        return 0;

    case WM_COMMAND:
        OnCommand(LOWORD(wp), HIWORD(wp));
        return 0;

    case WM_HSCROLL:
        if ((HWND)lp == G.trkVol) {
            G.cfg.volume = (int)SendMessageW(G.trkVol, TBM_GETPOS, 0, 0);
            wchar_t v[16];
            _snwprintf(v, 16, L"%d%%", G.cfg.volume);
            v[15] = 0;
            SetWindowTextW(G.lblVolValue, v);
            Engine_SetVolume(G.cfg.volume);
            if (LOWORD(wp) == TB_ENDTRACK) Config_Save(&G.cfg);
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_AUTOPAUSE) {
            CheckAutoPause();
        } else if (wp == TIMER_REFRESH) {
            KillTimer(hwnd, TIMER_REFRESH);
            Engine_RefreshLayout();
        } else if (wp == TIMER_TRAYRETRY) {
            /* Iniciado com o Windows antes da barra de tarefas existir */
            if (!G.trayAdded) Tray_Add();
            if (G.trayAdded) {
                KillTimer(hwnd, TIMER_TRAYRETRY);
                RefreshUi();
            } else if (++G.trayRetries >= 15) {
                KillTimer(hwnd, TIMER_TRAYRETRY);
                ShowMainWindow();
            }
        }
        return 0;

    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        wchar_t file[CONFIG_PATH_MAX];
        if (DragQueryFileW(drop, 0, file, CONFIG_PATH_MAX))
            SetVideoPath(file);
        DragFinish(drop);
        return 0;
    }

    case WM_APP_ENGINE_STATE:
        G.state = (EngineState)wp;
        if (G.state == ENGINE_IDLE) G.userPaused = FALSE;
        RefreshUi();
        return 0;

    case WM_APP_ENGINE_VIDEO:
        G.vidW = (DWORD)wp;
        G.vidH = (DWORD)lp;
        RefreshUi();
        return 0;

    case WM_APP_ENGINE_ERROR:
        DescribeError((int)wp, (HRESULT)lp);
        G.state = ENGINE_ERROR;
        G.userPaused = FALSE;
        G.cfg.wasPlaying = FALSE;
        Config_Save(&G.cfg);
        RefreshUi();
        if (!IsWindowVisible(hwnd))
            Tray_Balloon(L"Video Wallpaper", G.errorText);
        return 0;

    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONUP:
            ShowMainWindow();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            Tray_Menu();
            break;
        }
        return 0;

    case WM_APP_SHOWGUI:
        ShowMainWindow();
        return 0;

    case WM_DISPLAYCHANGE:
        Engine_RefreshLayout();
        SetTimer(hwnd, TIMER_REFRESH, 1500, NULL);   /* o Explorer ajusta com atraso */
        return 0;

    case WM_DPICHANGED: {
        G.dpi = HIWORD(wp);
        ApplyFont();
        Layout();
        const RECT *r = (const RECT *)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ResizeToClient();
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_CLOSE:
        if (G.cfg.closeToTray && G.trayAdded) {
            ShowWindow(hwnd, SW_HIDE);
            if (!G.cfg.trayHintShown) {
                Tray_Balloon(L"Video Wallpaper continua rodando",
                             L"O vídeo segue na área de trabalho. Use o ícone da bandeja para "
                             L"abrir o painel ou sair.");
                G.cfg.trayHintShown = TRUE;
                Config_Save(&G.cfg);
            }
        } else {
            Quit();
        }
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wp) {
            G.cfg.wasPlaying = EngineIsActive();
            Config_Save(&G.cfg);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_AUTOPAUSE);
        KillTimer(hwnd, TIMER_REFRESH);
        KillTimer(hwnd, TIMER_TRAYRETRY);
        if (G.engineOk) {
            G.cfg.wasPlaying = EngineIsActive();
            Config_Save(&G.cfg);
        }
        Engine_Shutdown();           /* restaura o papel de parede original */
        Tray_Remove();
        if (G.font) { DeleteObject(G.font); G.font = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL Gui_PreTranslateMessage(MSG *msg)
{
    return G.hwnd && IsDialogMessageW(G.hwnd, msg);
}

BOOL Gui_Create(HINSTANCE inst, BOOL startHidden)
{
    ZeroMemory(&G, sizeof(G));
    G.inst = inst;
    G.state = ENGINE_IDLE;
    Config_Load(&G.cfg);

    G.iconBig = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    G.iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!G.iconBig) G.iconBig = LoadIconW(NULL, IDI_APPLICATION);
    if (!G.iconSmall) G.iconSmall = G.iconBig;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MainProc;
    wc.hInstance     = inst;
    wc.hIcon         = G.iconBig;
    wc.hIconSm       = G.iconSmall;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = GUI_WINDOW_CLASS;
    if (!RegisterClassExW(&wc)) return FALSE;

    HWND hwnd = CreateWindowExW(0, GUI_WINDOW_CLASS, L"Video Wallpaper", MAIN_STYLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 520, 480,
                                NULL, NULL, inst, NULL);
    if (!hwnd) return FALSE;
    ResizeToClient();

    G.msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    Tray_Add();

    if (!Engine_Start(hwnd)) {
        wchar_t msg[256];
        _snwprintf(msg, 256,
                   L"Não foi possível inicializar o Direct3D 11 / Media Foundation (0x%08lX).\n\n"
                   L"O Video Wallpaper requer Windows 10 ou 11. Em edições “N” do Windows, "
                   L"instale o Media Feature Pack.",
                   (unsigned long)GetLastError());
        msg[255] = 0;
        MessageBoxW(hwnd, msg, L"Video Wallpaper", MB_ICONERROR);
        DestroyWindow(hwnd);
        return FALSE;
    }

    G.engineOk = TRUE;
    LoadConfigIntoUi();
    RefreshUi();
    SetTimer(hwnd, TIMER_AUTOPAUSE, AUTOPAUSE_MS, NULL);
    CheckAutoPause();

    if (G.cfg.resumeOnStart && G.cfg.wasPlaying && G.cfg.videoPath[0])
        Apply();

    if (!startHidden) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        UpdateWindow(hwnd);
    } else if (!G.trayAdded) {
        SetTimer(hwnd, TIMER_TRAYRETRY, 2000, NULL);
    }
    return TRUE;
}
