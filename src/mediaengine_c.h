/*
 * mediaengine_c.h
 *
 * Declarações em C puro das interfaces COM do Media Foundation Media Engine
 * (IMFMediaEngine, IMFMediaEngineEx, IMFMediaEngineClassFactory e
 * IMFMediaEngineNotify).
 *
 * O MinGW-w64 não distribui <mfmediaengine.h>, e o header do Windows SDK
 * é pensado para C++. Para o projeto compilar igual no MinGW e no MSVC,
 * declaramos aqui só o que usamos, com nomes próprios (prefixo VW_) para
 * não colidir com nada do SDK.
 *
 * A ORDEM DOS MÉTODOS NA VTABLE É CRÍTICA: ela segue exatamente o
 * mfmediaengine.idl oficial. Não reordene nem remova entradas.
 */
#ifndef VW_MEDIAENGINE_C_H
#define VW_MEDIAENGINE_C_H

#include <windows.h>
#include <unknwn.h>
#include <oleauto.h>
#include <mfobjects.h>   /* IMFAttributes, IMFByteStream, MFARGB */

#ifdef GetCurrentTime
#undef GetCurrentTime    /* macro antiga do windows.h colide com o método */
#endif

/* ---- GUIDs ------------------------------------------------------------ */

static const GUID VW_CLSID_MFMediaEngineClassFactory =
    { 0xb44392da, 0x499b, 0x446b, { 0xa4, 0xcb, 0x00, 0x5f, 0xea, 0xd0, 0xe6, 0xd5 } };
static const GUID VW_IID_IMFMediaEngineClassFactory =
    { 0x4d645ace, 0x26aa, 0x4688, { 0x9b, 0xe1, 0xdf, 0x35, 0x16, 0x99, 0x0b, 0x93 } };
static const GUID VW_IID_IMFMediaEngine =
    { 0x98a1b0bb, 0x03eb, 0x4935, { 0xae, 0x7c, 0x93, 0xc1, 0xfa, 0x0e, 0x1c, 0x93 } };
static const GUID VW_IID_IMFMediaEngineEx =
    { 0x83015ead, 0xb1e6, 0x40d0, { 0xa9, 0x8a, 0x37, 0x14, 0x5f, 0xfe, 0x1a, 0xd1 } };
static const GUID VW_IID_IMFMediaEngineNotify =
    { 0xfee7c112, 0xe776, 0x42b5, { 0x9b, 0xbf, 0x00, 0x48, 0x52, 0x4e, 0x2b, 0xd5 } };

/* Atributos passados para IMFMediaEngineClassFactory::CreateInstance */
static const GUID VW_MF_MEDIA_ENGINE_CALLBACK =
    { 0xc60381b8, 0x83a4, 0x41f8, { 0xa3, 0xd0, 0xde, 0x05, 0x07, 0x68, 0x49, 0xa9 } };
static const GUID VW_MF_MEDIA_ENGINE_DXGI_MANAGER =
    { 0x065702da, 0x1094, 0x486d, { 0x86, 0x17, 0xee, 0x7c, 0xc4, 0xee, 0x46, 0x48 } };
static const GUID VW_MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT =
    { 0x5066893c, 0x8cf9, 0x42bc, { 0x8b, 0x8a, 0x47, 0x22, 0x12, 0xe5, 0x27, 0x26 } };

/* ---- Constantes --------------------------------------------------------- */

/* MF_MEDIA_ENGINE_EVENT (subconjunto) */
#define VW_ME_EVENT_LOADSTART        1
#define VW_ME_EVENT_ERROR            5
#define VW_ME_EVENT_LOADEDMETADATA  10
#define VW_ME_EVENT_LOADEDDATA      11
#define VW_ME_EVENT_PLAYING         13
#define VW_ME_EVENT_CANPLAY         14
#define VW_ME_EVENT_ENDED           19
#define VW_ME_EVENT_FORMATCHANGE  1000

/* MF_MEDIA_ENGINE_ERR */
#define VW_ME_ERR_NOERROR            0
#define VW_ME_ERR_ABORTED            1
#define VW_ME_ERR_NETWORK            2
#define VW_ME_ERR_DECODE             3
#define VW_ME_ERR_SRC_NOT_SUPPORTED  4
#define VW_ME_ERR_ENCRYPTED          5

/* MFVideoNormalizedRect (mesma disposição de memória do SDK) */
typedef struct VW_NormRect {
    float left, top, right, bottom;
} VW_NormRect;

/* ---- IMFMediaEngine / IMFMediaEngineEx ---------------------------------- */

typedef struct VW_MediaEngine VW_MediaEngine;

typedef struct VW_MediaEngineVtbl {
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(VW_MediaEngine *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(VW_MediaEngine *This);
    ULONG   (STDMETHODCALLTYPE *Release)(VW_MediaEngine *This);

    /* IMFMediaEngine */
    HRESULT (STDMETHODCALLTYPE *GetError)(VW_MediaEngine *This, IUnknown **error);
    HRESULT (STDMETHODCALLTYPE *SetErrorCode)(VW_MediaEngine *This, int error);
    HRESULT (STDMETHODCALLTYPE *SetSourceElements)(VW_MediaEngine *This, IUnknown *elements);
    HRESULT (STDMETHODCALLTYPE *SetSource)(VW_MediaEngine *This, BSTR url);
    HRESULT (STDMETHODCALLTYPE *GetCurrentSource)(VW_MediaEngine *This, BSTR *url);
    USHORT  (STDMETHODCALLTYPE *GetNetworkState)(VW_MediaEngine *This);
    int     (STDMETHODCALLTYPE *GetPreload)(VW_MediaEngine *This);
    HRESULT (STDMETHODCALLTYPE *SetPreload)(VW_MediaEngine *This, int preload);
    HRESULT (STDMETHODCALLTYPE *GetBuffered)(VW_MediaEngine *This, IUnknown **buffered);
    HRESULT (STDMETHODCALLTYPE *Load)(VW_MediaEngine *This);
    HRESULT (STDMETHODCALLTYPE *CanPlayType)(VW_MediaEngine *This, BSTR type, int *answer);
    USHORT  (STDMETHODCALLTYPE *GetReadyState)(VW_MediaEngine *This);
    BOOL    (STDMETHODCALLTYPE *IsSeeking)(VW_MediaEngine *This);
    double  (STDMETHODCALLTYPE *GetCurrentTime)(VW_MediaEngine *This);
    HRESULT (STDMETHODCALLTYPE *SetCurrentTime)(VW_MediaEngine *This, double time);
    double  (STDMETHODCALLTYPE *GetStartTime)(VW_MediaEngine *This);
    double  (STDMETHODCALLTYPE *GetDuration)(VW_MediaEngine *This);
    BOOL    (STDMETHODCALLTYPE *IsPaused)(VW_MediaEngine *This);
    double  (STDMETHODCALLTYPE *GetDefaultPlaybackRate)(VW_MediaEngine *This);
    HRESULT (STDMETHODCALLTYPE *SetDefaultPlaybackRate)(VW_MediaEngine *This, double rate);
    double  (STDMETHODCALLTYPE *GetPlaybackRate)(VW_MediaEngine *This);
    HRESULT (STDMETHODCALLTYPE *SetPlaybackRate)(VW_MediaEngine *This, double rate);
    HRESULT (STDMETHODCALLTYPE *GetPlayed)(VW_MediaEngine *This, IUnknown **played);
    HRESULT (STDMETHODCALLTYPE *GetSeekable)(VW_MediaEngine *This, IUnknown **seekable);
    BOOL    (STDMETHODCALLTYPE *IsEnded)(VW_MediaEngine *This);
    BOOL    (STDMETHODCALLTYPE *GetAutoPlay)(VW_MediaEngine *This);
    HRESULT (STDMETHODCALLTYPE *SetAutoPlay)(VW_MediaEngine *This, BOOL autoplay);
    BOOL    (STDMETHODCALLTYPE *GetLoop)(VW_MediaEngine *This);
    HRESULT (STDMETHODCALLTYPE *SetLoop)(VW_MediaEngine *This, BOOL loop);
    HRESULT (STDMETHODCALLTYPE *Play)(VW_MediaEngine *This);
    HRESULT (STDMETHODCALLTYPE *Pause)(VW_MediaEngine *This);
    BOOL    (STDMETHODCALLTYPE *GetMuted)(VW_MediaEngine *This);
    HRESULT (STDMETHODCALLTYPE *SetMuted)(VW_MediaEngine *This, BOOL muted);
    double  (STDMETHODCALLTYPE *GetVolume)(VW_MediaEngine *This);
    HRESULT (STDMETHODCALLTYPE *SetVolume)(VW_MediaEngine *This, double volume);
    BOOL    (STDMETHODCALLTYPE *HasVideo)(VW_MediaEngine *This);
    BOOL    (STDMETHODCALLTYPE *HasAudio)(VW_MediaEngine *This);
    HRESULT (STDMETHODCALLTYPE *GetNativeVideoSize)(VW_MediaEngine *This, DWORD *cx, DWORD *cy);
    HRESULT (STDMETHODCALLTYPE *GetVideoAspectRatio)(VW_MediaEngine *This, DWORD *cx, DWORD *cy);
    HRESULT (STDMETHODCALLTYPE *Shutdown)(VW_MediaEngine *This);
    HRESULT (STDMETHODCALLTYPE *TransferVideoFrame)(VW_MediaEngine *This, IUnknown *surface,
                                                    const VW_NormRect *src, const RECT *dst,
                                                    const MFARGB *borderColor);
    HRESULT (STDMETHODCALLTYPE *OnVideoStreamTick)(VW_MediaEngine *This, LONGLONG *time);

    /* IMFMediaEngineEx — SÓ é válido em ponteiros obtidos via
     * QueryInterface(VW_IID_IMFMediaEngineEx). Usamos apenas o 1º método. */
    HRESULT (STDMETHODCALLTYPE *SetSourceFromByteStream)(VW_MediaEngine *This,
                                                         IMFByteStream *byteStream, BSTR url);
} VW_MediaEngineVtbl;

struct VW_MediaEngine {
    const VW_MediaEngineVtbl *lpVtbl;
};

/* ---- IMFMediaEngineClassFactory ----------------------------------------- */

typedef struct VW_MediaEngineClassFactory VW_MediaEngineClassFactory;

typedef struct VW_MediaEngineClassFactoryVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(VW_MediaEngineClassFactory *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(VW_MediaEngineClassFactory *This);
    ULONG   (STDMETHODCALLTYPE *Release)(VW_MediaEngineClassFactory *This);
    HRESULT (STDMETHODCALLTYPE *CreateInstance)(VW_MediaEngineClassFactory *This, DWORD flags,
                                                IMFAttributes *attributes, VW_MediaEngine **engine);
    HRESULT (STDMETHODCALLTYPE *CreateTimeRange)(VW_MediaEngineClassFactory *This, IUnknown **range);
    HRESULT (STDMETHODCALLTYPE *CreateError)(VW_MediaEngineClassFactory *This, IUnknown **error);
} VW_MediaEngineClassFactoryVtbl;

struct VW_MediaEngineClassFactory {
    const VW_MediaEngineClassFactoryVtbl *lpVtbl;
};

/* ---- IMFMediaEngineNotify (implementado por nós) ------------------------ */

typedef struct VW_MediaEngineNotify VW_MediaEngineNotify;

typedef struct VW_MediaEngineNotifyVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(VW_MediaEngineNotify *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(VW_MediaEngineNotify *This);
    ULONG   (STDMETHODCALLTYPE *Release)(VW_MediaEngineNotify *This);
    HRESULT (STDMETHODCALLTYPE *EventNotify)(VW_MediaEngineNotify *This, DWORD event,
                                             DWORD_PTR param1, DWORD param2);
} VW_MediaEngineNotifyVtbl;

struct VW_MediaEngineNotify {
    const VW_MediaEngineNotifyVtbl *lpVtbl;
};

#endif /* VW_MEDIAENGINE_C_H */
