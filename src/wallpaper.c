/*
 * wallpaper.c — ver wallpaper.h.
 */
#include <string.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#ifdef HAVE_LAYER_SHELL
#include <gtk-layer-shell.h>
#endif

#include "wallpaper.h"

typedef struct {
    GtkWidget *window;
    Player    *player;
} Surface;

static struct {
    WpBackend   backend;
    WpStateFunc cb;
    gpointer    user_data;

    GPtrArray *surfaces;     /* Surface* */
    gchar     *path;
    gboolean   muted;
    int        volume;
    ScaleMode  scale;

    gboolean active;
    gboolean userPaused;
    gboolean autoPaused;
    WpState  state;
    int      vidW, vidH;

    guint rebuildId;
    guint errorId;
    gchar *pendingError;
} W;

/* ---- estado -------------------------------------------------------------- */

static void set_state(WpState s, const char *message)
{
    W.state = s;
    if (W.cb) W.cb(s, message, W.user_data);
}

static WpState running_state(void)
{
    if (W.userPaused) return WP_PAUSED;
    if (W.autoPaused) return WP_AUTOPAUSED;
    return WP_PLAYING;
}

static void schedule_rebuild(void);

/* O compositor pode fechar uma superfície por conta própria (ex.: monitor
 * desconectado no Wayland); o GTK então destrói a janela. */
static void on_surface_window_destroyed(GtkWidget *w, gpointer data)
{
    (void)w;
    Surface *s = data;
    s->window = NULL;
    schedule_rebuild();
}

static void destroy_surfaces(void)
{
    if (!W.surfaces) return;
    for (guint i = 0; i < W.surfaces->len; i++) {
        Surface *s = g_ptr_array_index(W.surfaces, i);
        /* destruir a janela primeiro libera o contexto de render (unrealize) */
        if (s->window) {
            g_signal_handlers_disconnect_by_func(s->window, G_CALLBACK(on_surface_window_destroyed), s);
            gtk_widget_destroy(s->window);
        }
        player_free(s->player);
        g_free(s);
    }
    g_ptr_array_set_size(W.surfaces, 0);
}

static gboolean idle_report_error(gpointer data)
{
    (void)data;
    W.errorId = 0;
    destroy_surfaces();
    W.active = FALSE;
    set_state(WP_ERROR, W.pendingError);
    g_clear_pointer(&W.pendingError, g_free);
    return G_SOURCE_REMOVE;
}

static void on_player_event(const PlayerEvent *ev, gpointer data)
{
    (void)data;
    if (!W.active) return;
    switch (ev->type) {
    case PLAYER_EVENT_LOADING:
        set_state(WP_LOADING, NULL);
        break;
    case PLAYER_EVENT_PLAYING:
        set_state(running_state(), NULL);
        break;
    case PLAYER_EVENT_VIDEO_SIZE:
        W.vidW = ev->width;
        W.vidH = ev->height;
        set_state(W.state, NULL);         /* só para a interface atualizar */
        break;
    case PLAYER_EVENT_ERROR:
        /* Adiado: não destruímos o reprodutor de dentro do próprio callback. */
        if (!W.errorId) {
            g_free(W.pendingError);
            W.pendingError = g_strdup(ev->message);
            W.errorId = g_idle_add(idle_report_error, NULL);
        }
        break;
    }
}

/* ---- superfícies --------------------------------------------------------- */

static void apply_settings(Player *p)
{
    player_set_scale(p, W.scale);
    player_set_volume(p, W.volume);
    player_set_muted(p, W.muted);
    player_set_paused(p, W.userPaused || W.autoPaused);
}

static Surface *surface_new(GdkMonitor *monitor, gboolean primary)
{
    Player *player = player_new(primary, on_player_event, NULL);
    if (!player) return NULL;

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWindow *w = GTK_WINDOW(win);
    gtk_window_set_title(w, W.backend == WP_BACKEND_WINDOWED
                            ? "Video Wallpaper — janela de teste"
                            : "Video Wallpaper (fundo)");

    /* fundo preto enquanto o primeiro quadro não chega */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, "window { background-color: #000; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(win), GTK_STYLE_PROVIDER(css),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GdkRectangle geo = { 0, 0, 960, 540 };
    if (monitor) gdk_monitor_get_geometry(monitor, &geo);

    switch (W.backend) {
#ifdef HAVE_LAYER_SHELL
    case WP_BACKEND_LAYER_SHELL:
        gtk_layer_init_for_window(w);
        gtk_layer_set_namespace(w, "videowallpaper");
        /* BOTTOM: acima do papel de parede do próprio sistema (swaybg,
         * hyprpaper, desktop do Plasma), abaixo de janelas e painéis.
         * VW_LAYER=background usa a camada de fundo propriamente dita. */
        gtk_layer_set_layer(w, g_strcmp0(g_getenv("VW_LAYER"), "background") == 0
                                   ? GTK_LAYER_SHELL_LAYER_BACKGROUND
                                   : GTK_LAYER_SHELL_LAYER_BOTTOM);
        gtk_layer_set_monitor(w, monitor);
        gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor(w, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_exclusive_zone(w, -1);           /* cobre por baixo dos painéis */
        gtk_layer_set_keyboard_mode(w, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
        break;
#endif
    case WP_BACKEND_X11:
        gtk_window_set_type_hint(w, GDK_WINDOW_TYPE_HINT_DESKTOP);
        gtk_window_set_decorated(w, FALSE);
        gtk_window_set_skip_taskbar_hint(w, TRUE);
        gtk_window_set_skip_pager_hint(w, TRUE);
        gtk_window_set_keep_below(w, TRUE);
        gtk_window_set_accept_focus(w, FALSE);
        gtk_window_set_focus_on_map(w, FALSE);
        gtk_window_stick(w);
        gtk_window_move(w, geo.x, geo.y);
        gtk_window_set_default_size(w, geo.width, geo.height);
        break;
    case WP_BACKEND_WINDOWED:
    default:
        gtk_window_set_default_size(w, 960, 540);
        break;
    }

    gtk_container_add(GTK_CONTAINER(win), player_get_widget(player));

    if (W.backend != WP_BACKEND_WINDOWED) {
        /* região de entrada vazia: cliques atravessam para a área de trabalho */
        cairo_region_t *empty = cairo_region_create();
        gtk_widget_input_shape_combine_region(win, empty);
        cairo_region_destroy(empty);
    }

    gtk_widget_show_all(win);
    if (W.backend == WP_BACKEND_X11) {
        gtk_window_move(w, geo.x, geo.y);
        gtk_window_resize(w, geo.width, geo.height);
    }

    Surface *s = g_new0(Surface, 1);
    s->window = win;
    s->player = player;
    g_signal_connect(win, "destroy", G_CALLBACK(on_surface_window_destroyed), s);
    apply_settings(player);
    player_load(player, W.path);
    return s;
}

static gboolean build_surfaces(GError **error)
{
    destroy_surfaces();
    W.vidW = W.vidH = 0;

    if (W.backend == WP_BACKEND_WINDOWED) {
        Surface *s = surface_new(NULL, TRUE);
        if (s) g_ptr_array_add(W.surfaces, s);
    } else {
        GdkDisplay *d = gdk_display_get_default();
        int n = gdk_display_get_n_monitors(d);
        GdkMonitor *primary = gdk_display_get_primary_monitor(d);
        if (!primary && n > 0) primary = gdk_display_get_monitor(d, 0);

        /* o monitor principal primeiro: ele toca o áudio */
        if (primary) {
            Surface *s = surface_new(primary, TRUE);
            if (s) g_ptr_array_add(W.surfaces, s);
        }
        for (int i = 0; i < n; i++) {
            GdkMonitor *m = gdk_display_get_monitor(d, i);
            if (m == primary) continue;
            Surface *s = surface_new(m, FALSE);
            if (s) g_ptr_array_add(W.surfaces, s);
        }
    }

    if (W.surfaces->len == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Não foi possível iniciar o reprodutor de vídeo (libmpv).");
        return FALSE;
    }
    return TRUE;
}

/* Monitores conectados/desconectados ou resolução alterada. */
static gboolean rebuild_timeout(gpointer data)
{
    (void)data;
    W.rebuildId = 0;
    if (!W.active) return G_SOURCE_REMOVE;
    GError *err = NULL;
    if (!build_surfaces(&err)) {
        W.active = FALSE;
        set_state(WP_ERROR, err->message);
        g_error_free(err);
    } else {
        set_state(WP_LOADING, NULL);
    }
    return G_SOURCE_REMOVE;
}

static void schedule_rebuild(void)
{
    if (!W.active || W.backend == WP_BACKEND_WINDOWED) return;
    if (W.rebuildId) g_source_remove(W.rebuildId);
    W.rebuildId = g_timeout_add(800, rebuild_timeout, NULL);
}

static void on_monitors_changed(GdkDisplay *d, GdkMonitor *m, gpointer data)
{
    (void)d; (void)m; (void)data;
    schedule_rebuild();
}

static void on_screen_size_changed(GdkScreen *screen, gpointer data)
{
    (void)screen; (void)data;
    if (W.backend == WP_BACKEND_X11) schedule_rebuild();
}

/* ---- API pública --------------------------------------------------------- */

static WpBackend detect_backend(gboolean windowed)
{
    if (windowed) return WP_BACKEND_WINDOWED;
    GdkDisplay *d = gdk_display_get_default();
#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(d)) return WP_BACKEND_X11;
#endif
#if defined(HAVE_LAYER_SHELL) && defined(GDK_WINDOWING_WAYLAND)
    if (GDK_IS_WAYLAND_DISPLAY(d) && gtk_layer_is_supported()) return WP_BACKEND_LAYER_SHELL;
#endif
    (void)d;
    return WP_BACKEND_NONE;
}

void wp_init(gboolean windowed, WpStateFunc cb, gpointer user_data)
{
    memset(&W, 0, sizeof(W));
    W.cb = cb;
    W.user_data = user_data;
    W.volume = 50;
    W.surfaces = g_ptr_array_new();
    W.backend = detect_backend(windowed);

    GdkDisplay *d = gdk_display_get_default();
    g_signal_connect(d, "monitor-added", G_CALLBACK(on_monitors_changed), NULL);
    g_signal_connect(d, "monitor-removed", G_CALLBACK(on_monitors_changed), NULL);
    g_signal_connect(gdk_screen_get_default(), "size-changed", G_CALLBACK(on_screen_size_changed), NULL);
}

WpBackend wp_get_backend(void)
{
    return W.backend;
}

const char *wp_backend_description(void)
{
    switch (W.backend) {
    case WP_BACKEND_X11:         return "X11 (janela de fundo da área de trabalho)";
    case WP_BACKEND_LAYER_SHELL: return "Wayland (layer-shell, abaixo das janelas)";
    case WP_BACKEND_WINDOWED:    return "Janela de teste (--windowed)";
    default:                     return "sem suporte neste ambiente";
    }
}

gboolean wp_start(const char *path, gboolean muted, int volume, ScaleMode scale, GError **error)
{
    if (W.backend == WP_BACKEND_NONE) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
            "Este ambiente gráfico não permite colocar um vídeo no fundo da área de trabalho. "
            "No Wayland é preciso um compositor com wlr-layer-shell (KDE Plasma, Sway, Hyprland...); "
            "no GNOME, inicie o aplicativo pelo XWayland (padrão) ou use uma sessão X11.");
        return FALSE;
    }
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Arquivo não encontrado: %s", path);
        return FALSE;
    }

    if (W.errorId) { g_source_remove(W.errorId); W.errorId = 0; }
    g_free(W.path);
    W.path = g_strdup(path);
    W.muted = muted;
    W.volume = volume;
    W.scale = scale;
    W.userPaused = FALSE;
    W.vidW = W.vidH = 0;

    /* Já tocando: só troca o arquivo, sem recriar as janelas. */
    if (W.active && W.surfaces->len > 0) {
        for (guint i = 0; i < W.surfaces->len; i++) {
            Surface *s = g_ptr_array_index(W.surfaces, i);
            apply_settings(s->player);
            player_load(s->player, W.path);
        }
        set_state(WP_LOADING, NULL);
        return TRUE;
    }

    W.active = TRUE;
    if (!build_surfaces(error)) {
        W.active = FALSE;
        return FALSE;
    }
    set_state(WP_LOADING, NULL);
    return TRUE;
}

void wp_stop(void)
{
    if (W.rebuildId) { g_source_remove(W.rebuildId); W.rebuildId = 0; }
    if (W.errorId) { g_source_remove(W.errorId); W.errorId = 0; }
    destroy_surfaces();
    W.active = FALSE;
    W.userPaused = FALSE;
    W.vidW = W.vidH = 0;
    set_state(WP_IDLE, NULL);
}

void wp_shutdown(void)
{
    W.cb = NULL;
    wp_stop();
    g_clear_pointer(&W.path, g_free);
    g_clear_pointer(&W.pendingError, g_free);
}

static void apply_pause(void)
{
    gboolean paused = W.userPaused || W.autoPaused;
    for (guint i = 0; i < W.surfaces->len; i++)
        player_set_paused(((Surface *)g_ptr_array_index(W.surfaces, i))->player, paused);
    if (W.active && (W.state == WP_PLAYING || W.state == WP_PAUSED || W.state == WP_AUTOPAUSED))
        set_state(running_state(), NULL);
}

void wp_set_user_paused(gboolean paused) { W.userPaused = paused; apply_pause(); }
void wp_set_auto_paused(gboolean paused) { W.autoPaused = paused; apply_pause(); }

void wp_set_muted(gboolean muted)
{
    W.muted = muted;
    for (guint i = 0; i < W.surfaces->len; i++)
        player_set_muted(((Surface *)g_ptr_array_index(W.surfaces, i))->player, muted);
}

void wp_set_volume(int volume)
{
    W.volume = volume;
    for (guint i = 0; i < W.surfaces->len; i++)
        player_set_volume(((Surface *)g_ptr_array_index(W.surfaces, i))->player, volume);
}

void wp_set_scale(ScaleMode scale)
{
    W.scale = scale;
    for (guint i = 0; i < W.surfaces->len; i++)
        player_set_scale(((Surface *)g_ptr_array_index(W.surfaces, i))->player, scale);
}

WpState  wp_get_state(void) { return W.state; }
gboolean wp_is_active(void) { return W.active; }

void wp_get_video_size(int *w, int *h)
{
    if (w) *w = W.vidW;
    if (h) *h = W.vidH;
}

gboolean wp_owns_xid(unsigned long xid)
{
#ifdef GDK_WINDOWING_X11
    for (guint i = 0; W.surfaces && i < W.surfaces->len; i++) {
        Surface *s = g_ptr_array_index(W.surfaces, i);
        GdkWindow *gw = s->window ? gtk_widget_get_window(s->window) : NULL;
        if (gw && GDK_IS_X11_WINDOW(gw) && gdk_x11_window_get_xid(gw) == xid)
            return TRUE;
    }
#else
    (void)xid;
#endif
    return FALSE;
}
