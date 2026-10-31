/*
 * player.c — ver player.h.
 *
 * A libmpv decodifica o vídeo (com aceleração por hardware quando
 * disponível: VA-API, VDPAU, NVDEC...) e desenha cada quadro no
 * framebuffer de um GtkGLArea através da API de render do mpv.
 *
 * Threads: o mpv chama nossos callbacks a partir das threads dele. Eles
 * só agendam trabalho na thread principal do GTK (g_idle_add); todo o
 * resto acontece na thread principal. O Player tem contagem de
 * referências para que callbacks atrasados nunca usem memória liberada.
 */
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <gtk/gtk.h>
#include <epoxy/gl.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <epoxy/glx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#include <epoxy/egl.h>
#endif

#include "player.h"

#define REPLY_VID 100   /* reply_userdata da consulta assíncrona de "vid" */

/*
 * IMPORTANTE: a thread principal do GTK também é a thread de render do
 * mpv. Por isso, depois de mpv_initialize(), só usamos a API ASSÍNCRONA
 * (mpv_*_async). Uma chamada síncrona aqui pode travar o programa: ela
 * espera o núcleo do mpv, que por sua vez espera esta thread desenhar.
 */

struct Player {
    gint refs;
    gboolean dead;
    gboolean primary;

    mpv_handle *mpv;
    mpv_render_context *rc;
    GtkWidget *area;

    PlayerEventFunc cb;
    gpointer user_data;

    gint update_pending;   /* atômicos: evitam enfileirar idles repetidos */
    gint events_pending;

    gint64 width, height;
    char message[512];
};

static Player *player_ref(Player *p)
{
    g_atomic_int_inc(&p->refs);
    return p;
}

static void player_unref(Player *p)
{
    if (g_atomic_int_dec_and_test(&p->refs))
        g_free(p);
}

static void emit(Player *p, PlayerEventType type, const char *message)
{
    if (!p->primary || !p->cb || p->dead) return;
    PlayerEvent ev = { type, (int)p->width, (int)p->height, message };
    p->cb(&ev, p->user_data);
}

/* ---- OpenGL ------------------------------------------------------------ */

static void *get_proc_address(void *ctx, const char *name)
{
    (void)ctx;
    GdkDisplay *d = gdk_display_get_default();
#ifdef GDK_WINDOWING_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY(d))
        return (void *)eglGetProcAddress(name);
#endif
#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(d))
        return (void *)glXGetProcAddressARB((const GLubyte *)name);
#endif
    (void)d;
    return NULL;
}

/* mpv avisa (thread dele) que há um quadro novo para desenhar. */
static gboolean idle_render(gpointer data)
{
    Player *p = data;
    g_atomic_int_set(&p->update_pending, 0);
    if (!p->dead && p->rc) {
        uint64_t flags = mpv_render_context_update(p->rc);
        if (flags & MPV_RENDER_UPDATE_FRAME)
            gtk_gl_area_queue_render(GTK_GL_AREA(p->area));
    }
    player_unref(p);
    return G_SOURCE_REMOVE;
}

static void on_mpv_render_update(void *ctx)
{
    Player *p = ctx;
    if (g_atomic_int_compare_and_exchange(&p->update_pending, 0, 1)) {
        player_ref(p);
        /* mesma prioridade dos eventos: um render lento (GPU fraca) não
         * pode deixar a interface e os eventos do mpv sem vez */
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, idle_render, p, NULL);
    }
}

static void on_realize(GtkGLArea *area, gpointer data)
{
    Player *p = data;
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area)) {
        emit(p, PLAYER_EVENT_ERROR,
             "Não foi possível criar o contexto OpenGL. Verifique os drivers de vídeo.");
        return;
    }

    mpv_opengl_init_params gl = { get_proc_address, NULL };
    /* API + init + display X11 (opcional) + terminador */
    mpv_render_param params[4];
    int n = 0;
    params[n++] = (mpv_render_param){ MPV_RENDER_PARAM_API_TYPE, (char *)MPV_RENDER_API_TYPE_OPENGL };
    params[n++] = (mpv_render_param){ MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl };

    /* No X11, passar o display habilita VDPAU/VA-API sem cópias.
     * No Wayland NÃO passamos o wl_display de propósito: a libva-wayland
     * depende do protocolo wl_drm, que compositores recentes (Sway,
     * Hyprland...) não oferecem mais, e pode derrubar o processo. Sem ele
     * o mpv abre a VA-API direto pelo /dev/dri/renderD*, com dmabuf. */
    GdkDisplay *d = gtk_widget_get_display(GTK_WIDGET(area));
#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(d))
        params[n++] = (mpv_render_param){ MPV_RENDER_PARAM_X11_DISPLAY, gdk_x11_display_get_xdisplay(d) };
#endif
    (void)d;
    g_assert((size_t)n < G_N_ELEMENTS(params));
    params[n] = (mpv_render_param){ MPV_RENDER_PARAM_INVALID, NULL };

    int err = mpv_render_context_create(&p->rc, p->mpv, params);
    if (err < 0) {
        p->rc = NULL;
        snprintf(p->message, sizeof(p->message),
                 "Falha ao iniciar o desenho do vídeo (OpenGL): %s", mpv_error_string(err));
        emit(p, PLAYER_EVENT_ERROR, p->message);
        return;
    }
    mpv_render_context_set_update_callback(p->rc, on_mpv_render_update, p);
}

static void free_render_context(Player *p)
{
    if (!p->rc) return;
    if (gtk_widget_get_realized(p->area))
        gtk_gl_area_make_current(GTK_GL_AREA(p->area));
    mpv_render_context_set_update_callback(p->rc, NULL, NULL);
    mpv_render_context_free(p->rc);
    p->rc = NULL;
}

static void on_unrealize(GtkGLArea *area, gpointer data)
{
    (void)area;
    free_render_context(data);
}

static gboolean on_render(GtkGLArea *area, GdkGLContext *ctx, gpointer data)
{
    (void)ctx;
    Player *p = data;
    if (!p->rc) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return TRUE;
    }

    GLint fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));

    mpv_opengl_fbo target = {
        .fbo = fbo,
        .w = gtk_widget_get_allocated_width(GTK_WIDGET(area)) * scale,
        .h = gtk_widget_get_allocated_height(GTK_WIDGET(area)) * scale,
        .internal_format = 0
    };
    int flip = 1;
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_OPENGL_FBO, &target },
        { MPV_RENDER_PARAM_FLIP_Y, &flip },
        { MPV_RENDER_PARAM_INVALID, NULL }
    };
    mpv_render_context_render(p->rc, params);
    return TRUE;
}

/* ---- eventos do mpv ------------------------------------------------------ */

static const char *describe_error(int err)
{
    switch (err) {
    case MPV_ERROR_UNKNOWN_FORMAT:
        return "Formato de arquivo não reconhecido.";
    case MPV_ERROR_LOADING_FAILED:
        return "Não foi possível abrir o arquivo (ele existe e você tem permissão de leitura?).";
    case MPV_ERROR_NOTHING_TO_PLAY:
        return "O arquivo não tem nenhuma faixa que possa ser reproduzida.";
    default:
        return NULL;
    }
}

static gboolean idle_events(gpointer data)
{
    Player *p = data;
    g_atomic_int_set(&p->events_pending, 0);

    while (!p->dead && p->mpv) {
        mpv_event *ev = mpv_wait_event(p->mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;

        switch (ev->event_id) {
        case MPV_EVENT_START_FILE:
            p->width = p->height = 0;
            emit(p, PLAYER_EVENT_LOADING, NULL);
            break;

        case MPV_EVENT_FILE_LOADED:
            /* Assíncrono: chamadas síncronas na thread que também desenha
             * podem travar (o núcleo do mpv espera o render e vice-versa). */
            mpv_get_property_async(p->mpv, REPLY_VID, "vid", MPV_FORMAT_STRING);
            break;

        case MPV_EVENT_GET_PROPERTY_REPLY: {
            if (ev->reply_userdata != REPLY_VID) break;
            mpv_event_property *pr = ev->data;
            const char *vid = (ev->error >= 0 && pr->format == MPV_FORMAT_STRING && pr->data)
                              ? *(const char **)pr->data : NULL;
            if (vid && strcmp(vid, "no") != 0) emit(p, PLAYER_EVENT_PLAYING, NULL);
            else emit(p, PLAYER_EVENT_ERROR, "O arquivo não contém vídeo.");
            break;
        }

        case MPV_EVENT_END_FILE: {
            mpv_event_end_file *ef = ev->data;
            if (ef->reason == MPV_END_FILE_REASON_ERROR) {
                const char *msg = describe_error(ef->error);
                if (!msg) {
                    snprintf(p->message, sizeof(p->message),
                             "Erro ao reproduzir o vídeo: %s", mpv_error_string(ef->error));
                    msg = p->message;
                }
                emit(p, PLAYER_EVENT_ERROR, msg);
            }
            break;
        }

        case MPV_EVENT_PROPERTY_CHANGE: {
            mpv_event_property *pr = ev->data;
            if (pr->format != MPV_FORMAT_INT64 || !pr->data) break;
            gint64 v = *(gint64 *)pr->data;
            if (strcmp(pr->name, "video-params/w") == 0) p->width = v;
            else if (strcmp(pr->name, "video-params/h") == 0) p->height = v;
            if (p->width > 0 && p->height > 0)
                emit(p, PLAYER_EVENT_VIDEO_SIZE, NULL);
            break;
        }

        default:
            break;
        }
    }
    player_unref(p);
    return G_SOURCE_REMOVE;
}

static void on_mpv_wakeup(void *ctx)
{
    Player *p = ctx;
    if (g_atomic_int_compare_and_exchange(&p->events_pending, 0, 1)) {
        player_ref(p);
        g_idle_add(idle_events, p);
    }
}

/* ---- API pública --------------------------------------------------------- */

static void set_prop(Player *p, const char *name, const char *value)
{
    if (!p || !p->mpv) return;
    mpv_set_property_async(p->mpv, 0, name, MPV_FORMAT_STRING, &value);
}

static void set_opt(mpv_handle *mpv, const char *name, const char *value)
{
    int err = mpv_set_option_string(mpv, name, value);
    if (err < 0)
        g_debug("mpv: opção %s=%s recusada: %s", name, value, mpv_error_string(err));
}

Player *player_new(gboolean primary, PlayerEventFunc cb, gpointer user_data)
{
    /* O mpv exige ponto como separador decimal; o GTK aplica o locale do
     * sistema (pt_BR usa vírgula). */
    setlocale(LC_NUMERIC, "C");

    mpv_handle *mpv = mpv_create();
    if (!mpv) return NULL;

    gboolean debug = g_getenv("VW_DEBUG") != NULL;
    set_opt(mpv, "vo", "libmpv");
    /* Papel de parede fica rodando o tempo todo: o perfil "fast" usa escala
     * bilinear e desliga etapas caras (dithering, etc.), poupando a GPU. */
    set_opt(mpv, "profile", "fast");
    /* VW_HWDEC=no desliga a decodificação por hardware (diagnóstico) */
    const char *hwdec = g_getenv("VW_HWDEC");
    set_opt(mpv, "hwdec", hwdec && *hwdec ? hwdec : "auto-safe");
    set_opt(mpv, "loop-file", "inf");
    set_opt(mpv, "idle", "yes");
    set_opt(mpv, "config", "no");               /* ignora o mpv.conf do usuário */
    set_opt(mpv, "terminal", debug ? "yes" : "no");
    set_opt(mpv, "msg-level", debug ? "all=v" : "all=warn");
    set_opt(mpv, "input-default-bindings", "no");
    set_opt(mpv, "input-vo-keyboard", "no");
    set_opt(mpv, "osc", "no");
    set_opt(mpv, "osd-level", "0");
    set_opt(mpv, "load-scripts", "no");
    set_opt(mpv, "ytdl", "no");
    set_opt(mpv, "sub-auto", "no");
    set_opt(mpv, "sid", "no");
    set_opt(mpv, "audio-display", "no");
    set_opt(mpv, "keepaspect", "yes");
    set_opt(mpv, "audio-client-name", "Video Wallpaper");
    if (!primary) {
        set_opt(mpv, "aid", "no");               /* só o monitor principal toca som */
        set_opt(mpv, "mute", "yes");
    }

    if (mpv_initialize(mpv) < 0) {
        mpv_terminate_destroy(mpv);
        return NULL;
    }

    Player *p = g_new0(Player, 1);
    p->refs = 1;
    p->primary = primary;
    p->mpv = mpv;
    p->cb = cb;
    p->user_data = user_data;

    if (primary) {
        mpv_observe_property(mpv, 1, "video-params/w", MPV_FORMAT_INT64);
        mpv_observe_property(mpv, 2, "video-params/h", MPV_FORMAT_INT64);
    }
    mpv_set_wakeup_callback(mpv, on_mpv_wakeup, p);

    p->area = g_object_ref_sink(gtk_gl_area_new());
    gtk_gl_area_set_has_alpha(GTK_GL_AREA(p->area), FALSE);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(p->area), FALSE);
    gtk_widget_set_hexpand(p->area, TRUE);
    gtk_widget_set_vexpand(p->area, TRUE);
    g_signal_connect(p->area, "realize", G_CALLBACK(on_realize), p);
    g_signal_connect(p->area, "unrealize", G_CALLBACK(on_unrealize), p);
    g_signal_connect(p->area, "render", G_CALLBACK(on_render), p);
    return p;
}

GtkWidget *player_get_widget(Player *p)
{
    return p->area;
}

void player_load(Player *p, const char *path)
{
    if (!p || !p->mpv) return;
    const char *cmd[] = { "loadfile", path, "replace", NULL };
    mpv_command_async(p->mpv, 0, cmd);
}

void player_set_paused(Player *p, gboolean paused)
{
    set_prop(p, "pause", paused ? "yes" : "no");
}

void player_set_muted(Player *p, gboolean muted)
{
    if (p && p->primary) set_prop(p, "mute", muted ? "yes" : "no");
}

void player_set_volume(Player *p, int volume)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", CLAMP(volume, 0, 100));
    set_prop(p, "volume", buf);
}

void player_set_scale(Player *p, ScaleMode mode)
{
    /* panscan=1 corta para preencher; video-unscaled=yes mostra 1:1
     * (centralizado, cortando se for maior que a tela). */
    set_prop(p, "panscan", mode == SCALE_FILL ? "1.0" : "0.0");
    set_prop(p, "video-unscaled", mode == SCALE_CENTER ? "yes" : "no");
}

void player_free(Player *p)
{
    if (!p) return;
    p->dead = TRUE;
    free_render_context(p);          /* normalmente já liberado no unrealize */
    mpv_set_wakeup_callback(p->mpv, NULL, NULL);
    mpv_terminate_destroy(p->mpv);
    p->mpv = NULL;
    g_signal_handlers_disconnect_by_data(p->area, p);
    g_object_unref(p->area);
    p->area = NULL;
    player_unref(p);
}
