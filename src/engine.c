/*
 * engine.c — motor de reprodução (ver engine.h).
 *
 * Pipeline:
 *   arquivo --> Media Foundation Media Engine (decodificação por hardware,
 *               loop nativo) --> TransferVideoFrame para o back buffer de
 *               uma swap chain DXGI --> janela filha atrás dos ícones.
 *
 * Um mesmo vídeo é desenhado em cada monitor, cada um com seu próprio
 * recorte/escala, numa única swap chain do tamanho da área de trabalho.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>        /* ID3D10Multithread */
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

#include "mediaengine_c.h"
#include "wallpaper.h"
#include "engine.h"
#include "debuglog.h"

#ifndef DXGI_SWAP_EFFECT_FLIP_DISCARD
#define DXGI_SWAP_EFFECT_FLIP_DISCARD ((DXGI_SWAP_EFFECT)4)
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* Mensagens internas da thread do motor */
enum {
    RM_OPEN = WM_APP + 1,
    RM_STOP,
    RM_USERPAUSE,
    RM_AUTOPAUSE,
    RM_MUTE,
    RM_VOLUME,
    RM_SCALE,
    RM_REFRESH,
    RM_REATTACH,
    RM_QUIT,
    RM_ME_EVENT,     /* wParam = evento, lParam = geração            */
    RM_ME_ERROR      /* wParam = (geração << 8) | código, lParam = HRESULT */
};

#define MAINTAIN_INTERVAL_MS   2000
#define MAX_REATTACH_FAILURES  15     /* ~30 s tentando após o Explorer cair */

typedef struct OpenRequest {
    wchar_t  *path;
    BOOL      muted;
    int       volume;
    ScaleMode scale;
} OpenRequest;

static struct {
    /* thread */
    HWND      notify;
    HANDLE    thread;
    DWORD     tid;
    HANDLE    ready;
    HRESULT   initHr;
    HINSTANCE inst;
    BOOL      quit;

    /* Direct3D / DXGI */
    ID3D11Device         *dev;
    ID3D11DeviceContext  *ctx;
    IDXGIFactory2        *factory;
    IDXGIOutput          *output;     /* para sincronizar com o vblank */
    IMFDXGIDeviceManager *dxgiMgr;
    UINT                  resetToken;
    HANDLE                pacingTimer;

    /* Media Engine */
    VW_MediaEngineClassFactory *mef;
    VW_MediaEngine             *me;
    BOOL                        meIsEx;
    UINT                        gen;  /* descarta eventos de instâncias antigas */

    /* alvo de desenho */
    WallTarget               wall;
    WallGeometry             geo;
    IDXGISwapChain1         *sc;
    ID3D11Texture2D         *back;
    ID3D11RenderTargetView  *rtv;
    int                      reattachFailures;

    /* vídeo atual */
    DWORD  vidW, vidH;
    double dar;                 /* proporção de exibição (largura/altura) */
    BOOL   metaReady;
    BOOL   haveFrame;
    BOOL   needRedraw;
    double resumeTime;

    /* configuração/estado */
    wchar_t    *path;
    BOOL        muted;
    int         volume;
    ScaleMode   scale;
    BOOL        active;
    BOOL        userPaused;
    BOOL        autoPaused;
    EngineState state;
} E;

/* ======================================================================
 * IMFMediaEngineNotify — callback do Media Engine (chamado em threads do
 * Media Foundation). Só repassa os eventos para a thread do motor.
 * ====================================================================== */

typedef struct NotifyObj {
    VW_MediaEngineNotify iface;   /* precisa ser o primeiro membro */
    LONG refs;
    UINT gen;
} NotifyObj;

static HRESULT STDMETHODCALLTYPE Notify_QueryInterface(VW_MediaEngineNotify *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &VW_IID_IMFMediaEngineNotify)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Notify_AddRef(VW_MediaEngineNotify *This)
{
    return (ULONG)InterlockedIncrement(&((NotifyObj *)This)->refs);
}

static ULONG STDMETHODCALLTYPE Notify_Release(VW_MediaEngineNotify *This)
{
    NotifyObj *n = (NotifyObj *)This;
    LONG r = InterlockedDecrement(&n->refs);
    if (r == 0) free(n);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE Notify_EventNotify(VW_MediaEngineNotify *This, DWORD event,
                                                    DWORD_PTR param1, DWORD param2)
{
    NotifyObj *n = (NotifyObj *)This;
    switch (event) {
    case VW_ME_EVENT_ERROR:
        PostThreadMessageW(E.tid, RM_ME_ERROR,
                           ((WPARAM)n->gen << 8) | (WPARAM)(param1 & 0xFF),
                           (LPARAM)(LONG)param2);
        break;
    case VW_ME_EVENT_LOADEDMETADATA:
    case VW_ME_EVENT_PLAYING:
    case VW_ME_EVENT_ENDED:
    case VW_ME_EVENT_FORMATCHANGE:
        PostThreadMessageW(E.tid, RM_ME_EVENT, (WPARAM)event, (LPARAM)n->gen);
        break;
    default:
        break;
    }
    return S_OK;
}

static const VW_MediaEngineNotifyVtbl g_notifyVtbl = {
    Notify_QueryInterface, Notify_AddRef, Notify_Release, Notify_EventNotify
};

static NotifyObj *Notify_Create(UINT gen)
{
    NotifyObj *n = (NotifyObj *)calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->iface.lpVtbl = &g_notifyVtbl;
    n->refs = 1;
    n->gen  = gen;
    return n;
}

/* ======================================================================
 * Utilidades
 * ====================================================================== */


#define SAFE_RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

static void SetState(EngineState s)
{
    E.state = s;
    PostMessageW(E.notify, WM_APP_ENGINE_STATE, (WPARAM)s, 0);
}

static void ReportError(int meErr, HRESULT hr)
{
    DbgLog("ERRO: codigo MF=%d hr=0x%08lX\n", meErr, (unsigned long)hr);
    PostMessageW(E.notify, WM_APP_ENGINE_ERROR, (WPARAM)meErr, (LPARAM)hr);
    SetState(ENGINE_ERROR);
}

static EngineState PausedOrPlayingState(void)
{
    if (E.userPaused) return ENGINE_PAUSED;
    if (E.autoPaused) return ENGINE_AUTOPAUSED;
    return ENGINE_PLAYING;
}

/* ======================================================================
 * Direct3D 11
 * ====================================================================== */

static HRESULT CreateDevice(void)
{
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3
    };
    const UINT nLevels = (UINT)(sizeof(levels) / sizeof(levels[0]));
    const UINT base = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    /* Ordem de tentativa: GPU com suporte a vídeo > GPU > WARP (software) */
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                                   base | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                   levels, nLevels, D3D11_SDK_VERSION, &E.dev, NULL, &E.ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, base,
                               levels, nLevels, D3D11_SDK_VERSION, &E.dev, NULL, &E.ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, base,
                               levels, nLevels, D3D11_SDK_VERSION, &E.dev, NULL, &E.ctx);
    if (FAILED(hr)) return hr;

    /* O Media Engine usa o dispositivo a partir das threads dele. */
    ID3D10Multithread *mt = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(E.dev, &IID_ID3D10Multithread, (void **)&mt))) {
        ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
        ID3D10Multithread_Release(mt);
    }

    IDXGIDevice *dxDev = NULL;
    IDXGIAdapter *adapter = NULL;
    hr = ID3D11Device_QueryInterface(E.dev, &IID_IDXGIDevice, (void **)&dxDev);
    if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dxDev, &adapter);
    if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&E.factory);
    if (SUCCEEDED(hr) && FAILED(IDXGIAdapter_EnumOutputs(adapter, 0, &E.output)))
        E.output = NULL;   /* ex.: notebook híbrido; usamos o timer como plano B */
    SAFE_RELEASE(adapter);
    SAFE_RELEASE(dxDev);
    if (FAILED(hr)) return hr;

    hr = MFCreateDXGIDeviceManager(&E.resetToken, &E.dxgiMgr);
    if (SUCCEEDED(hr))
        hr = IMFDXGIDeviceManager_ResetDevice(E.dxgiMgr, (IUnknown *)E.dev, E.resetToken);
    if (FAILED(hr)) return hr;

    return CoCreateInstance(&VW_CLSID_MFMediaEngineClassFactory, NULL, CLSCTX_INPROC_SERVER,
                            &VW_IID_IMFMediaEngineClassFactory, (void **)&E.mef);
}

static void ReleaseDevice(void)
{
    SAFE_RELEASE(E.mef);
    SAFE_RELEASE(E.dxgiMgr);
    SAFE_RELEASE(E.output);
    SAFE_RELEASE(E.factory);
    if (E.ctx) ID3D11DeviceContext_ClearState(E.ctx);
    SAFE_RELEASE(E.ctx);
    SAFE_RELEASE(E.dev);
}

/* ---- swap chain ------------------------------------------------------- */

static void ReleaseTargets(void)
{
    SAFE_RELEASE(E.rtv);
    SAFE_RELEASE(E.back);
}

static void ReleaseSwapChain(void)
{
    ReleaseTargets();
    SAFE_RELEASE(E.sc);
    if (E.ctx) {
        ID3D11DeviceContext_ClearState(E.ctx);
        ID3D11DeviceContext_Flush(E.ctx);
    }
}

static HRESULT AcquireTargets(void)
{
    HRESULT hr = IDXGISwapChain1_GetBuffer(E.sc, 0, &IID_ID3D11Texture2D, (void **)&E.back);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateRenderTargetView(E.dev, (ID3D11Resource *)E.back, NULL, &E.rtv);
    return hr;
}

static HRESULT CreateSwapChain(void)
{
    ReleaseSwapChain();
    Wallpaper_GetGeometry(&E.wall, &E.geo);
    if (!E.geo.width || !E.geo.height) return E_FAIL;

    DXGI_SWAP_CHAIN_DESC1 d;
    ZeroMemory(&d, sizeof(d));
    d.Width            = E.geo.width;
    d.Height           = E.geo.height;
    d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.Scaling          = DXGI_SCALING_STRETCH;
    d.AlphaMode        = DXGI_ALPHA_MODE_UNSPECIFIED;

    HRESULT hr = E_FAIL;
    if (!E.wall.layered) {
        /* Layout clássico: modelo "flip" (mais eficiente). */
        d.BufferCount = 2;
        d.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(E.factory, (IUnknown *)E.dev, E.wall.hwnd,
                                                  &d, NULL, NULL, &E.sc);
        if (FAILED(hr)) {
            d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            hr = IDXGIFactory2_CreateSwapChainForHwnd(E.factory, (IUnknown *)E.dev, E.wall.hwnd,
                                                      &d, NULL, NULL, &E.sc);
        }
    }
    if (FAILED(hr)) {
        /* Windows 11 24H2 (janela em camadas): a Microsoft orienta usar
         * apresentação "blt", que também serve de plano B geral. */
        d.BufferCount = 1;
        d.SwapEffect  = DXGI_SWAP_EFFECT_DISCARD;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(E.factory, (IUnknown *)E.dev, E.wall.hwnd,
                                                  &d, NULL, NULL, &E.sc);
    }
    DbgLog("SwapChain %ux%u, efeito=%d, hr=0x%08lX\n", d.Width, d.Height, (int)d.SwapEffect,
           (unsigned long)hr);
    if (FAILED(hr)) return hr;

    IDXGIFactory2_MakeWindowAssociation(E.factory, E.wall.hwnd,
                                        DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    hr = AcquireTargets();
    if (FAILED(hr)) ReleaseSwapChain();
    E.needRedraw = TRUE;
    return hr;
}

static HRESULT ResizeSwapChain(void)
{
    if (!E.sc) return CreateSwapChain();
    ReleaseTargets();
    ID3D11DeviceContext_ClearState(E.ctx);
    HRESULT hr = IDXGISwapChain1_ResizeBuffers(E.sc, 0, E.geo.width, E.geo.height,
                                               DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr)) hr = AcquireTargets();
    if (FAILED(hr)) hr = CreateSwapChain();
    E.needRedraw = TRUE;
    return hr;
}

/* ======================================================================
 * Encaixe na área de trabalho
 * ====================================================================== */

static void DropWallpaperWindow(BOOL restoreDesktop)
{
    ReleaseSwapChain();
    if (restoreDesktop) {
        Wallpaper_Detach(&E.wall);
    } else {
        if (E.wall.hwnd && IsWindow(E.wall.hwnd)) DestroyWindow(E.wall.hwnd);
        ZeroMemory(&E.wall, sizeof(E.wall));
    }
}

static HRESULT EnsureAttached(void)
{
    if (E.wall.hwnd && IsWindow(E.wall.hwnd) && E.sc) return S_OK;

    DropWallpaperWindow(FALSE);
    if (!Wallpaper_Attach(E.inst, &E.wall))
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

    HRESULT hr = CreateSwapChain();
    if (FAILED(hr)) DropWallpaperWindow(TRUE);
    return hr;
}

static void UpdateGeometry(void)
{
    if (!E.wall.hwnd) return;
    WallGeometry g;
    Wallpaper_GetGeometry(&E.wall, &g);
    if (!g.width || !g.height) return;

    DbgLog("UpdateGeometry: atual %ux%u, nova %ux%u, monitores=%d\n",
           E.geo.width, E.geo.height, g.width, g.height, g.monitorCount);
    if (g.width != E.geo.width || g.height != E.geo.height) {
        E.geo = g;
        if (FAILED(ResizeSwapChain())) DropWallpaperWindow(FALSE);
    } else if (memcmp(&g, &E.geo, sizeof(g)) != 0) {
        E.geo = g;
        E.needRedraw = TRUE;
    }
}

/* Plano B do loop: com SetLoop(TRUE) o Media Engine recomeça sozinho, mas
 * se alguma implementação chegar ao fim mesmo assim, voltamos ao início. */
static void RestartIfEnded(void)
{
    if (!E.me || !E.metaReady || E.userPaused || E.autoPaused) return;
    if (E.me->lpVtbl->IsEnded(E.me)) {
        E.me->lpVtbl->SetCurrentTime(E.me, 0.0);
        E.me->lpVtbl->Play(E.me);
    }
}

/* Chamado a cada MAINTAIN_INTERVAL_MS. */
static void Maintain(void)
{
    if (!E.active || !E.metaReady) return;
    RestartIfEnded();

    BOOL resized = FALSE;
    if (E.wall.hwnd && E.sc && Wallpaper_Maintain(&E.wall, &resized)) {
        E.reattachFailures = 0;
        UpdateGeometry();
        return;
    }

    /* Perdemos o encaixe (normalmente o Explorer reiniciou). */
    DropWallpaperWindow(FALSE);
    if (SUCCEEDED(EnsureAttached())) {
        E.reattachFailures = 0;
    } else if (++E.reattachFailures > MAX_REATTACH_FAILURES) {
        E.reattachFailures = 0;
        E.active = FALSE;
        ReportError(0, HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    }
}

/* ======================================================================
 * Media Engine
 * ====================================================================== */

static void CloseMedia(void)
{
    if (E.me) {
        E.me->lpVtbl->Shutdown(E.me);
        E.me->lpVtbl->Release(E.me);
        E.me = NULL;
    }
    E.meIsEx    = FALSE;
    E.metaReady = FALSE;
    E.haveFrame = FALSE;
}

static void StopAll(void)
{
    CloseMedia();
    DropWallpaperWindow(TRUE);
    E.active = FALSE;
    E.reattachFailures = 0;
}

static HRESULT OpenMedia(void)
{
    /* Mantém a janela atual (com o último quadro) para a troca ser suave. */
    CloseMedia();
    E.gen++;

    IMFAttributes *attr = NULL;
    VW_MediaEngine *base = NULL;
    NotifyObj *notify = Notify_Create(E.gen);
    if (!notify) return E_OUTOFMEMORY;

    HRESULT hr = MFCreateAttributes(&attr, 3);
    if (SUCCEEDED(hr)) hr = IMFAttributes_SetUnknown(attr, &VW_MF_MEDIA_ENGINE_CALLBACK, (IUnknown *)notify);
    if (SUCCEEDED(hr)) hr = IMFAttributes_SetUnknown(attr, &VW_MF_MEDIA_ENGINE_DXGI_MANAGER, (IUnknown *)E.dxgiMgr);
    if (SUCCEEDED(hr)) hr = IMFAttributes_SetUINT32(attr, &VW_MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT,
                                                    DXGI_FORMAT_B8G8R8A8_UNORM);
    if (SUCCEEDED(hr)) hr = E.mef->lpVtbl->CreateInstance(E.mef, 0, attr, &base);
    SAFE_RELEASE(attr);
    notify->iface.lpVtbl->Release(&notify->iface);   /* o engine guarda a dele */
    if (FAILED(hr)) return hr;

    /* Preferimos IMFMediaEngineEx para abrir via byte stream (aceita
     * qualquer caractere no caminho, ex.: '#', '%'). */
    VW_MediaEngine *ex = NULL;
    if (SUCCEEDED(base->lpVtbl->QueryInterface(base, &VW_IID_IMFMediaEngineEx, (void **)&ex))) {
        base->lpVtbl->Release(base);
        E.me = ex;
        E.meIsEx = TRUE;
    } else {
        E.me = base;
    }

    E.me->lpVtbl->SetLoop(E.me, TRUE);
    E.me->lpVtbl->SetMuted(E.me, E.muted);
    E.me->lpVtbl->SetVolume(E.me, E.volume / 100.0);

    BSTR url = SysAllocString(E.path);
    if (!url) return E_OUTOFMEMORY;

    hr = E_FAIL;
    if (E.meIsEx) {
        IMFByteStream *bs = NULL;
        hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST,
                          MF_FILEFLAGS_NONE, E.path, &bs);
        if (SUCCEEDED(hr)) hr = E.me->lpVtbl->SetSourceFromByteStream(E.me, bs, url);
        SAFE_RELEASE(bs);
    }
    if (FAILED(hr)) hr = E.me->lpVtbl->SetSource(E.me, url);
    SysFreeString(url);
    if (FAILED(hr)) return hr;

    return E.me->lpVtbl->Play(E.me);
}

static void ApplyPause(void)
{
    if (!E.me) return;
    if (E.userPaused || E.autoPaused) E.me->lpVtbl->Pause(E.me);
    else                              E.me->lpVtbl->Play(E.me);
    if (E.metaReady) SetState(PausedOrPlayingState());
}

static void OnLoadedMetadata(void)
{
    if (!E.me) return;

    DWORD w = 0, h = 0;
    if (!E.me->lpVtbl->HasVideo(E.me) ||
        FAILED(E.me->lpVtbl->GetNativeVideoSize(E.me, &w, &h)) || !w || !h) {
        StopAll();
        ReportError(VW_ME_ERR_SRC_NOT_SUPPORTED, MF_E_INVALIDMEDIATYPE);
        return;
    }

    E.vidW = w;
    E.vidH = h;
    E.dar  = (double)w / (double)h;
    DWORD ax = 0, ay = 0;   /* proporção da imagem (corrige vídeos anamórficos) */
    if (SUCCEEDED(E.me->lpVtbl->GetVideoAspectRatio(E.me, &ax, &ay)) && ax && ay)
        E.dar = (double)ax / (double)ay;

    E.metaReady = TRUE;
    E.haveFrame = FALSE;
    DbgLog("Video carregado: %lux%lu, proporcao %.3f\n", (unsigned long)w, (unsigned long)h, E.dar);

    if (E.resumeTime > 0.0) {
        E.me->lpVtbl->SetCurrentTime(E.me, E.resumeTime);
        E.resumeTime = 0.0;
    }

    /* Logo após o login o Explorer pode ainda não ter montado a área de
     * trabalho; nesse caso o timer de manutenção tenta de novo por ~30 s. */
    if (FAILED(EnsureAttached()))
        E.reattachFailures = 1;

    PostMessageW(E.notify, WM_APP_ENGINE_VIDEO, (WPARAM)w, (LPARAM)h);
    ApplyPause();
}

/* ======================================================================
 * Desenho
 * ====================================================================== */

/* Calcula recorte da origem (normalizado) e destino para um monitor. */
static void ComputeRects(const RECT *mon, VW_NormRect *src, RECT *dst)
{
    const double mw = (double)(mon->right - mon->left);
    const double mh = (double)(mon->bottom - mon->top);
    const double mar = mw / mh;
    const double dar = E.dar > 0.0 ? E.dar : mar;

    src->left = 0.0f; src->top = 0.0f; src->right = 1.0f; src->bottom = 1.0f;
    *dst = *mon;

    switch (E.scale) {
    case SCALE_FILL:
        /* corta o excesso para cobrir o monitor inteiro */
        if (dar > mar) {
            float cut = (float)((1.0 - mar / dar) / 2.0);
            src->left = cut; src->right = 1.0f - cut;
        } else if (dar < mar) {
            float cut = (float)((1.0 - dar / mar) / 2.0);
            src->top = cut; src->bottom = 1.0f - cut;
        }
        break;

    case SCALE_FIT:
        /* vídeo inteiro, barras pretas onde sobrar */
        if (dar > mar) {
            LONG hh = (LONG)lround(mw / dar);
            dst->top = mon->top + (LONG)((mh - hh) / 2);
            dst->bottom = dst->top + hh;
        } else if (dar < mar) {
            LONG ww = (LONG)lround(mh * dar);
            dst->left = mon->left + (LONG)((mw - ww) / 2);
            dst->right = dst->left + ww;
        }
        break;

    case SCALE_CENTER:
    default: {
        /* 1 pixel do vídeo = 1 pixel da tela; corta se for maior */
        double vw = (double)E.vidW;
        double vh = vw / dar;
        if (vw > mw) { float cut = (float)((1.0 - mw / vw) / 2.0); src->left = cut; src->right = 1.0f - cut; vw = mw; }
        if (vh > mh) { float cut = (float)((1.0 - mh / vh) / 2.0); src->top = cut; src->bottom = 1.0f - cut; vh = mh; }
        dst->left   = mon->left + (LONG)((mw - vw) / 2);
        dst->top    = mon->top  + (LONG)((mh - vh) / 2);
        dst->right  = dst->left + (LONG)lround(vw);
        dst->bottom = dst->top  + (LONG)lround(vh);
        break;
    }
    }
}

static void HandleDeviceLost(void)
{
    double t = (E.me && E.metaReady) ? E.me->lpVtbl->GetCurrentTime(E.me) : 0.0;
    CloseMedia();
    DropWallpaperWindow(FALSE);
    ReleaseDevice();

    HRESULT hr = CreateDevice();
    if (SUCCEEDED(hr)) {
        E.resumeTime = t;
        hr = OpenMedia();
    }
    if (FAILED(hr)) {
        StopAll();
        ReportError(0, hr);
    } else {
        SetState(ENGINE_LOADING);
    }
}

/* force = redesenha o quadro atual mesmo sem quadro novo (após resize etc.) */
static BOOL RenderFrame(BOOL force)
{
    if (!E.me || !E.sc || !E.rtv || !E.metaReady) return FALSE;

    if (!IsWindow(E.wall.hwnd)) {        /* o Explorer destruiu nossa janela */
        DropWallpaperWindow(FALSE);
        return FALSE;
    }

    LONGLONG pts = 0;
    BOOL fresh = E.me->lpVtbl->OnVideoStreamTick(E.me, &pts) == S_OK;
    if (!fresh && !(force && E.haveFrame)) return FALSE;

    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ID3D11DeviceContext_ClearRenderTargetView(E.ctx, E.rtv, black);

    MFARGB border;
    border.rgbBlue = 0; border.rgbGreen = 0; border.rgbRed = 0; border.rgbAlpha = 255;

    for (int i = 0; i < E.geo.monitorCount; i++) {
        VW_NormRect src;
        RECT dst;
        ComputeRects(&E.geo.monitors[i], &src, &dst);
        if (force)
            DbgLog("Render(force) modo=%d mon=(%ld,%ld,%ld,%ld) src=(%.3f,%.3f,%.3f,%.3f) dst=(%ld,%ld,%ld,%ld) dar=%.3f\n",
                   (int)E.scale, E.geo.monitors[i].left, E.geo.monitors[i].top, E.geo.monitors[i].right,
                   E.geo.monitors[i].bottom, src.left, src.top, src.right, src.bottom,
                   dst.left, dst.top, dst.right, dst.bottom, E.dar);
        if (SUCCEEDED(E.me->lpVtbl->TransferVideoFrame(E.me, (IUnknown *)E.back, &src, &dst, &border)))
            E.haveFrame = TRUE;
    }

    HRESULT hr = IDXGISwapChain1_Present(E.sc, 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        HandleDeviceLost();
        return FALSE;
    }
    return TRUE;
}

/* Espera até o próximo quadro de tela (vblank), sem travar mensagens por
 * muito tempo. Se o vblank não estiver disponível (monitor desligado,
 * GPU híbrida), usa um timer de ~4 ms. */
static void WaitForNextFrame(void)
{
    if (E.output) {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        HRESULT hr = IDXGIOutput_WaitForVBlank(E.output);
        QueryPerformanceCounter(&t1);
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        if (SUCCEEDED(hr) && ms >= 1.0) return;
    }
    if (E.pacingTimer) {
        LARGE_INTEGER due;
        due.QuadPart = -40000;   /* 4 ms em unidades de 100 ns */
        SetWaitableTimer(E.pacingTimer, &due, 0, NULL, NULL, FALSE);
        MsgWaitForMultipleObjectsEx(1, &E.pacingTimer, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } else {
        Sleep(4);
    }
}

/* ======================================================================
 * Thread do motor
 * ====================================================================== */

static void HandleCommand(const MSG *m)
{
    switch (m->message) {
    case RM_OPEN: {
        OpenRequest *r = (OpenRequest *)m->lParam;
        free(E.path);
        E.path   = r->path;
        E.muted  = r->muted;
        E.volume = r->volume;
        E.scale  = r->scale;
        free(r);

        E.active = TRUE;
        E.userPaused = FALSE;
        E.resumeTime = 0.0;
        E.reattachFailures = 0;
        SetState(ENGINE_LOADING);
        HRESULT hr = OpenMedia();
        if (FAILED(hr)) {
            StopAll();
            ReportError(0, hr);
        }
        break;
    }
    case RM_STOP:
        StopAll();
        SetState(ENGINE_IDLE);
        break;
    case RM_USERPAUSE:
        E.userPaused = (BOOL)m->wParam;
        ApplyPause();
        break;
    case RM_AUTOPAUSE:
        E.autoPaused = (BOOL)m->wParam;
        ApplyPause();
        break;
    case RM_MUTE:
        E.muted = (BOOL)m->wParam;
        if (E.me) E.me->lpVtbl->SetMuted(E.me, E.muted);
        break;
    case RM_VOLUME:
        E.volume = (int)m->wParam;
        if (E.me) E.me->lpVtbl->SetVolume(E.me, E.volume / 100.0);
        break;
    case RM_SCALE:
        E.scale = (ScaleMode)m->wParam;
        E.needRedraw = TRUE;
        break;
    case RM_REFRESH:
        if (E.active && E.metaReady) {
            BOOL resized;
            if (E.wall.hwnd && Wallpaper_Maintain(&E.wall, &resized)) UpdateGeometry();
            else Maintain();
        }
        break;
    case RM_REATTACH:
        if (E.active && E.metaReady) {
            DropWallpaperWindow(FALSE);
            if (FAILED(EnsureAttached())) E.reattachFailures = 1;  /* o timer tenta de novo */
        }
        break;
    case RM_QUIT:
        E.quit = TRUE;
        break;
    case RM_ME_EVENT:
        if ((UINT)m->lParam != E.gen || !E.me) break;
        switch ((DWORD)m->wParam) {
        case VW_ME_EVENT_LOADEDMETADATA:
            OnLoadedMetadata();
            break;
        case VW_ME_EVENT_PLAYING:
            if (E.metaReady) SetState(PausedOrPlayingState());
            break;
        case VW_ME_EVENT_ENDED:
            RestartIfEnded();
            break;
        case VW_ME_EVENT_FORMATCHANGE:
            if (E.metaReady) {
                DWORD w = 0, h = 0;
                if (SUCCEEDED(E.me->lpVtbl->GetNativeVideoSize(E.me, &w, &h)) && w && h) {
                    E.vidW = w; E.vidH = h; E.dar = (double)w / (double)h;
                    E.needRedraw = TRUE;
                }
            }
            break;
        }
        break;
    case RM_ME_ERROR:
        if ((UINT)(m->wParam >> 8) != (E.gen & 0x00FFFFFFu)) break;
        StopAll();
        ReportError((int)(m->wParam & 0xFF), (HRESULT)m->lParam);
        break;
    case WM_TIMER:
        Maintain();
        break;
    }
}

static DWORD WINAPI EngineThread(void *arg)
{
    (void)arg;
    MSG m;
    PeekMessageW(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE);   /* cria a fila */

    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    BOOL mfOk = SUCCEEDED(hr);
    if (SUCCEEDED(hr)) hr = CreateDevice();

    E.initHr = hr;
    DbgLog("Motor iniciado: hr=0x%08lX, vblank=%s\n", (unsigned long)hr, E.output ? "sim" : "nao");
    SetEvent(E.ready);

    if (SUCCEEDED(hr)) {
        E.pacingTimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_ALL_ACCESS);
        if (!E.pacingTimer)
            E.pacingTimer = CreateWaitableTimerW(NULL, FALSE, NULL);
        UINT_PTR timer = SetTimer(NULL, 0, MAINTAIN_INTERVAL_MS, NULL);

        while (!E.quit) {
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT) { E.quit = TRUE; break; }
                if (m.hwnd == NULL) HandleCommand(&m);
                else { TranslateMessage(&m); DispatchMessageW(&m); }
            }
            if (E.quit) break;

            if (E.needRedraw) {
                RenderFrame(TRUE);
                E.needRedraw = FALSE;
            }

            BOOL animating = E.active && E.me && E.sc && E.metaReady &&
                             !E.userPaused && !E.autoPaused;
            if (animating) {
                RenderFrame(FALSE);
                WaitForNextFrame();
            } else {
                MsgWaitForMultipleObjectsEx(0, NULL, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
        }

        KillTimer(NULL, timer);
        if (E.pacingTimer) { CloseHandle(E.pacingTimer); E.pacingTimer = NULL; }
    }

    StopAll();
    ReleaseDevice();
    free(E.path);
    E.path = NULL;
    if (mfOk) MFShutdown();
    if (SUCCEEDED(hrCom)) CoUninitialize();
    return 0;
}

/* ======================================================================
 * API pública (chamada pela thread da interface)
 * ====================================================================== */

static void Post(UINT msg, WPARAM wp, LPARAM lp)
{
    if (E.tid) PostThreadMessageW(E.tid, msg, wp, lp);
}

BOOL Engine_Start(HWND notifyWindow)
{
    ZeroMemory(&E, sizeof(E));
    E.notify = notifyWindow;
    E.inst   = GetModuleHandleW(NULL);
    E.volume = 50;
    E.ready  = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!E.ready) return FALSE;

    E.thread = CreateThread(NULL, 0, EngineThread, NULL, 0, &E.tid);
    if (!E.thread) {
        CloseHandle(E.ready);
        E.ready = NULL;
        return FALSE;
    }
    WaitForSingleObject(E.ready, INFINITE);
    CloseHandle(E.ready);
    E.ready = NULL;

    if (FAILED(E.initHr)) {
        WaitForSingleObject(E.thread, 5000);
        CloseHandle(E.thread);
        E.thread = NULL;
        E.tid = 0;
        SetLastError((DWORD)E.initHr);
        return FALSE;
    }
    return TRUE;
}

void Engine_Shutdown(void)
{
    if (!E.thread) return;
    Post(RM_QUIT, 0, 0);
    if (WaitForSingleObject(E.thread, 5000) == WAIT_TIMEOUT)
        TerminateThread(E.thread, 1);   /* último recurso: não travar o logoff */
    CloseHandle(E.thread);
    E.thread = NULL;
    E.tid = 0;
}

void Engine_Open(const wchar_t *path, BOOL muted, int volume, ScaleMode scale)
{
    if (!E.tid || !path || !*path) return;
    OpenRequest *r = (OpenRequest *)calloc(1, sizeof(*r));
    if (!r) return;
    r->path = _wcsdup(path);
    r->muted = muted;
    r->volume = volume;
    r->scale = scale;
    if (!r->path || !PostThreadMessageW(E.tid, RM_OPEN, 0, (LPARAM)r)) {
        free(r->path);
        free(r);
    }
}

void Engine_Stop(void)                 { Post(RM_STOP, 0, 0); }
void Engine_SetUserPaused(BOOL paused) { Post(RM_USERPAUSE, (WPARAM)paused, 0); }
void Engine_SetAutoPaused(BOOL paused) { Post(RM_AUTOPAUSE, (WPARAM)paused, 0); }
void Engine_SetMuted(BOOL muted)       { Post(RM_MUTE, (WPARAM)muted, 0); }
void Engine_SetVolume(int volume)      { Post(RM_VOLUME, (WPARAM)volume, 0); }
void Engine_SetScaleMode(ScaleMode s)  { Post(RM_SCALE, (WPARAM)s, 0); }
void Engine_RefreshLayout(void)        { Post(RM_REFRESH, 0, 0); }
void Engine_Reattach(void)             { Post(RM_REATTACH, 0, 0); }
