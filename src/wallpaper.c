/*
 * wallpaper.c — ver wallpaper.h.
 */
#include "wallpaper.h"
#include "debuglog.h"

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

/* Mensagem não documentada que pede ao Progman para separar a camada do
 * papel de parede (cria o WorkerW). Usada por Lively, Wallpaper Engine etc. */
#define PROGMAN_SPAWN_WORKERW 0x052C

static const wchar_t WALL_CLASS[] = L"VideoWallpaper.Surface";
static BOOL g_testMode;

void Wallpaper_SetTestMode(BOOL enable) { g_testMode = enable; }

/* ---- Funções de DPI carregadas dinamicamente (Windows 10 1607+) -------- */

typedef void *VW_DPI_CTX;
typedef VW_DPI_CTX (WINAPI *PFN_GetWindowDpiAwarenessContext)(HWND);
typedef VW_DPI_CTX (WINAPI *PFN_SetThreadDpiAwarenessContext)(VW_DPI_CTX);

static PFN_GetWindowDpiAwarenessContext pGetWindowDpiAwarenessContext;
static PFN_SetThreadDpiAwarenessContext pSetThreadDpiAwarenessContext;
static BOOL g_dpiLoaded;

static void LoadDpiFunctions(void)
{
    if (g_dpiLoaded) return;
    g_dpiLoaded = TRUE;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    pGetWindowDpiAwarenessContext = (PFN_GetWindowDpiAwarenessContext)(void *)
        GetProcAddress(user32, "GetWindowDpiAwarenessContext");
    pSetThreadDpiAwarenessContext = (PFN_SetThreadDpiAwarenessContext)(void *)
        GetProcAddress(user32, "SetThreadDpiAwarenessContext");
}

/* Faz a thread usar o mesmo modo de DPI da janela informada. Isso evita
 * falhas de SetParent entre janelas com DPI awareness diferente e mantém
 * coordenadas coerentes com as do Explorer. Retorna o contexto anterior. */
static VW_DPI_CTX EnterDpiContextOf(HWND ref)
{
    LoadDpiFunctions();
    if (!ref || !pGetWindowDpiAwarenessContext || !pSetThreadDpiAwarenessContext)
        return NULL;
    VW_DPI_CTX ctx = pGetWindowDpiAwarenessContext(ref);
    return ctx ? pSetThreadDpiAwarenessContext(ctx) : NULL;
}

static void LeaveDpiContext(VW_DPI_CTX old)
{
    if (old && pSetThreadDpiAwarenessContext)
        pSetThreadDpiAwarenessContext(old);
}

/* ---- Janela de vídeo -------------------------------------------------- */

static LRESULT CALLBACK SurfaceProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST:
        if (g_testMode) break;
        return HTTRANSPARENT;          /* nunca captura o mouse */
    case WM_MOUSEACTIVATE:
        if (g_testMode) break;
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;                      /* o Direct3D pinta tudo */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL RegisterSurfaceClass(HINSTANCE inst)
{
    static BOOL done;
    if (done) return TRUE;
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = SurfaceProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WALL_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return FALSE;
    done = TRUE;
    return TRUE;
}

/* ---- Descoberta do layout da área de trabalho ------------------------- */

typedef struct DesktopLayout {
    HWND progman;
    HWND defview;    /* SHELLDLL_DefView                          */
    HWND parent;     /* onde nossa janela deve ser filha           */
    BOOL raised;
    BOOL layered;
} DesktopLayout;

static BOOL IsOwnedBy(HWND h, DWORD pid)
{
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    return p == pid;
}

/* Procura um WorkerW do Explorer (sem ícones, de tamanho plausível) que
 * venha depois de 'after' entre os filhos de 'parent' (NULL = nível superior). */
static HWND FindWallpaperWorkerW(HWND parent, HWND after, DWORD pid)
{
    HWND w = after;
    while ((w = FindWindowExW(parent, w, L"WorkerW", NULL)) != NULL) {
        if (!IsOwnedBy(w, pid)) continue;
        if (FindWindowExW(w, NULL, L"SHELLDLL_DefView", NULL)) continue;
        RECT r;
        if (!GetWindowRect(w, &r)) continue;
        if (r.right - r.left < 200 || r.bottom - r.top < 200) continue;
        return w;
    }
    return NULL;
}

typedef struct { DWORD pid; HWND host; } HostSearch;

static BOOL CALLBACK FindIconHostProc(HWND top, LPARAM lp)
{
    HostSearch *s = (HostSearch *)lp;
    if (IsOwnedBy(top, s->pid) && FindWindowExW(top, NULL, L"SHELLDLL_DefView", NULL)) {
        s->host = top;
        return FALSE;
    }
    return TRUE;
}

/* allowNoWorkerW: aceita o layout 24H2 mesmo sem achar o WorkerW filho. */
static BOOL DetectLayout(DesktopLayout *L, BOOL allowNoWorkerW)
{
    ZeroMemory(L, sizeof(*L));
    L->progman = FindWindowW(L"Progman", NULL);
    if (!L->progman) return FALSE;

    DWORD pid = 0;
    GetWindowThreadProcessId(L->progman, &pid);
    LONG_PTR ex = GetWindowLongPtrW(L->progman, GWL_EXSTYLE);

    /* Layout 24H2+: ícones e WorkerW são filhos diretos do Progman. */
    HWND dvInProgman = FindWindowExW(L->progman, NULL, L"SHELLDLL_DefView", NULL);
    if (dvInProgman) {
        HWND worker = FindWallpaperWorkerW(L->progman, NULL, pid);
        BOOL raisedStyle = (ex & WS_EX_NOREDIRECTIONBITMAP) != 0;
        if (worker || (raisedStyle && allowNoWorkerW)) {
            L->raised  = TRUE;
            L->layered = raisedStyle;
            L->defview = dvInProgman;
            L->parent  = L->progman;
            return TRUE;
        }
    }

    /* Layout clássico: o host dos ícones é um WorkerW de nível superior e
     * o WorkerW do papel de parede é o próximo irmão dele. */
    HostSearch s = { pid, NULL };
    EnumWindows(FindIconHostProc, (LPARAM)&s);
    if (s.host) {
        HWND worker = FindWallpaperWorkerW(NULL, s.host, pid);
        if (worker) {
            L->parent = worker;
            return TRUE;
        }
    }
    return FALSE;
}

static void SpawnWorkerW(HWND progman, WPARAM wp, LPARAM lp)
{
    DWORD_PTR result = 0;
    SendMessageTimeoutW(progman, PROGMAN_SPAWN_WORKERW, wp, lp,
                        SMTO_NORMAL, 1000, &result);
}

static BOOL WaitForLayout(DesktopLayout *L, BOOL allowNoWorkerW)
{
    for (int i = 0; i < 20; i++) {
        if (DetectLayout(L, allowNoWorkerW)) return TRUE;
        Sleep(50);
    }
    return FALSE;
}

static BOOL FindDesktopLayout(DesktopLayout *L)
{
    if (DetectLayout(L, FALSE)) return TRUE;

    HWND progman = FindWindowW(L"Progman", NULL);
    if (!progman) return FALSE;

    /* (0xD, 1) é o que funciona no Windows 11; (0, 0) é a variante antiga.
     * Só mandamos a segunda se a primeira não criou nada, porque o
     * Explorer conta referências dessa divisão. */
    SpawnWorkerW(progman, 0xD, 1);
    if (WaitForLayout(L, FALSE)) return TRUE;

    SpawnWorkerW(progman, 0, 0);
    if (WaitForLayout(L, FALSE)) return TRUE;

    /* Último recurso: 24H2 sem WorkerW visível — ainda dá para ficar
     * entre os ícones e o fundo do Progman. */
    return DetectLayout(L, TRUE);
}

/* ---- API pública ------------------------------------------------------ */

BOOL Wallpaper_Attach(HINSTANCE inst, WallTarget *out)
{
    ZeroMemory(out, sizeof(*out));
    if (!RegisterSurfaceClass(inst)) return FALSE;

    if (g_testMode) {
        out->hwnd = CreateWindowExW(0, WALL_CLASS, L"Video Wallpaper \u2014 janela de teste",
                                    WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                    960, 540, NULL, NULL, inst, NULL);
        return out->hwnd != NULL;
    }

    DesktopLayout L;
    if (!FindDesktopLayout(&L)) {
        DbgLog("Attach: layout da area de trabalho NAO encontrado\n");
        return FALSE;
    }
    DbgLog("Attach: layout %s, camadas=%d, parent=%p, defview=%p\n",
           L.raised ? "24H2+ (raised)" : "classico (WorkerW)", L.layered,
           (void *)L.parent, (void *)L.defview);

    VW_DPI_CTX oldCtx = EnterDpiContextOf(L.parent);

    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (L.layered) ex |= WS_EX_LAYERED;

    /* Criada como popup e convertida em filha antes do SetParent. */
    HWND hwnd = CreateWindowExW(ex, WALL_CLASS, L"VideoWallpaper",
                                WS_POPUP, 0, 0, 1, 1, NULL, NULL, inst, NULL);
    if (!hwnd) {
        LeaveDpiContext(oldCtx);
        return FALSE;
    }

    /* No desktop 24H2 o DWM só compõe filhos em camadas totalmente opacos.
     * O alfa precisa ser definido ANTES do SetParent. */
    if (L.layered)
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    SetWindowLongPtrW(hwnd, GWL_STYLE,
                      WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_DISABLED);
    SetParent(hwnd, L.parent);

    if (GetAncestor(hwnd, GA_PARENT) != L.parent) {
        DbgLog("Attach: SetParent falhou (erro %lu)\n", GetLastError());
        DestroyWindow(hwnd);
        LeaveDpiContext(oldCtx);
        return FALSE;
    }

    RECT rc;
    GetClientRect(L.parent, &rc);
    /* 24H2: inserir logo abaixo dos ícones. Clássico: topo do WorkerW. */
    HWND after = L.raised ? L.defview : HWND_TOP;
    SetWindowPos(hwnd, after, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    LeaveDpiContext(oldCtx);
    DbgLog("Attach: ok, janela %ldx%ld\n", rc.right - rc.left, rc.bottom - rc.top);

    out->hwnd    = hwnd;
    out->parent  = L.parent;
    out->defview = L.defview;
    out->raised  = L.raised;
    out->layered = L.layered;
    return TRUE;
}

/* Faz o Explorer repintar o papel de parede original. */
static void RefreshStaticWallpaper(void)
{
    wchar_t path[MAX_PATH * 2] = L"";
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(path), path, 0))
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, path, 0);
}

void Wallpaper_Detach(WallTarget *t)
{
    if (!t) return;
    BOOL hadWindow = t->hwnd != NULL;
    BOOL raised = t->raised;
    HWND parent = t->parent;

    if (t->hwnd && IsWindow(t->hwnd))
        DestroyWindow(t->hwnd);
    ZeroMemory(t, sizeof(*t));

    if (!hadWindow) return;
    if (raised) {
        /* No 24H2 o WorkerW do Explorer continua embaixo; basta repintar. */
        if (parent && IsWindow(parent))
            RedrawWindow(parent, NULL, NULL,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    } else {
        /* No layout clássico o WorkerW pode ficar com o último quadro. */
        RefreshStaticWallpaper();
    }
}

BOOL Wallpaper_Maintain(WallTarget *t, BOOL *resized)
{
    if (resized) *resized = FALSE;
    if (g_testMode) return t->hwnd && IsWindow(t->hwnd);
    if (!t->hwnd || !IsWindow(t->hwnd) || !IsWindow(t->parent)) return FALSE;
    if (GetAncestor(t->hwnd, GA_PARENT) != t->parent) return FALSE;
    if (t->raised && !IsWindow(t->defview)) return FALSE;

    VW_DPI_CTX oldCtx = EnterDpiContextOf(t->hwnd);

    UINT flags = SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE;
    HWND after = NULL;
    if (t->raised && GetWindow(t->hwnd, GW_HWNDPREV) != t->defview) {
        after = t->defview;              /* o Explorer reordenou os filhos */
        flags &= ~(UINT)SWP_NOZORDER;
    } else {
        flags |= SWP_NOZORDER;
    }

    RECT pr, wr;
    GetClientRect(t->parent, &pr);
    GetWindowRect(t->hwnd, &wr);
    MapWindowPoints(HWND_DESKTOP, t->parent, (POINT *)&wr, 2);
    BOOL sizeWrong = wr.left != 0 || wr.top != 0 ||
                     (wr.right - wr.left) != (pr.right - pr.left) ||
                     (wr.bottom - wr.top) != (pr.bottom - pr.top);
    if (sizeWrong) {
        flags &= ~(UINT)(SWP_NOMOVE | SWP_NOSIZE);
        if (resized) *resized = TRUE;
    }

    if (after || sizeWrong)
        SetWindowPos(t->hwnd, after, 0, 0, pr.right - pr.left, pr.bottom - pr.top, flags);

    LeaveDpiContext(oldCtx);
    return TRUE;
}

typedef struct { const WallTarget *t; WallGeometry *g; } MonEnum;

static BOOL CALLBACK MonitorProc(HMONITOR mon, HDC dc, LPRECT rcMon, LPARAM lp)
{
    (void)mon; (void)dc;
    MonEnum *e = (MonEnum *)lp;
    if (e->g->monitorCount >= WALL_MAX_MONITORS) return FALSE;

    RECT r = *rcMon;
    MapWindowPoints(HWND_DESKTOP, e->t->hwnd, (POINT *)&r, 2);
    RECT client = { 0, 0, (LONG)e->g->width, (LONG)e->g->height };
    RECT clipped;
    if (IntersectRect(&clipped, &r, &client))
        e->g->monitors[e->g->monitorCount++] = clipped;
    return TRUE;
}

void Wallpaper_GetGeometry(const WallTarget *t, WallGeometry *g)
{
    ZeroMemory(g, sizeof(*g));
    if (!t->hwnd) return;

    VW_DPI_CTX oldCtx = EnterDpiContextOf(t->hwnd);

    RECT rc;
    GetClientRect(t->hwnd, &rc);
    g->width  = (UINT)(rc.right - rc.left);
    g->height = (UINT)(rc.bottom - rc.top);

    MonEnum e = { t, g };
    if (!g_testMode)
        EnumDisplayMonitors(NULL, NULL, MonitorProc, (LPARAM)&e);

    if (g->monitorCount == 0 && g->width && g->height) {
        g->monitors[0] = rc;
        g->monitorCount = 1;
    }
    LeaveDpiContext(oldCtx);
}
