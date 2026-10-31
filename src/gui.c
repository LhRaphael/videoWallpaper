/*
 * gui.c — painel de controle do Video Wallpaper (GTK3).
 */
#include <string.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#endif
#ifdef HAVE_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>
#endif

#include "gui.h"
#include "wallpaper.h"
#include "config.h"

#define AUTOPAUSE_INTERVAL_MS 1500

static struct {
    GtkApplication *app;
    GtkWidget *win;
    GtkWidget *entry, *btnBrowse;
    GtkWidget *btnApply, *btnPause, *btnStop;
    GtkWidget *chkMute, *scaleVol, *cmbScale;
    GtkWidget *chkAutostart, *chkResume, *chkFull, *chkMax, *chkBg;
    GtkWidget *lblStatus, *lblBackend;

    Config   cfg;
    WpState  state;
    gboolean userPaused;
    gboolean autoPaused;
    gchar   *errorText;
    gboolean loading;          /* ignora sinais enquanto preenche a tela */
    guint    pollId;
    gchar   *iconPath;

#ifdef HAVE_APPINDICATOR
    AppIndicator *indicator;
    GtkWidget *menuPause, *menuStop;
#endif
} G;

/* ======================================================================
 * Utilidades
 * ====================================================================== */

static gboolean is_active_state(WpState s)
{
    return s == WP_LOADING || s == WP_PLAYING || s == WP_PAUSED || s == WP_AUTOPAUSED;
}

static gboolean autopause_supported(void)
{
#ifdef GDK_WINDOWING_X11
    return GDK_IS_X11_DISPLAY(gdk_display_get_default());
#else
    return FALSE;
#endif
}

/* Procura o ícone ao lado do executável (build local) ou instalado. */
static gchar *find_icon(void)
{
    gchar *exe = g_file_read_link("/proc/self/exe", NULL);
    gchar *dir = exe ? g_path_get_dirname(exe) : g_strdup(".");
    const char *rel[] = {
        "videowallpaper.png",
        "../data/videowallpaper.png",
        "data/videowallpaper.png",
        "../share/icons/hicolor/256x256/apps/videowallpaper.png",
        NULL
    };
    gchar *found = NULL;
    for (int i = 0; rel[i] && !found; i++) {
        gchar *p = g_build_filename(dir, rel[i], NULL);
        if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) found = g_canonicalize_filename(p, NULL);
        g_free(p);
    }
    if (!found) {
        gchar *p = g_build_filename(g_get_user_data_dir(), "icons", "hicolor", "256x256", "apps",
                                    "videowallpaper.png", NULL);
        if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) found = p; else g_free(p);
    }
    g_free(dir);
    g_free(exe);
    return found;
}

/* ======================================================================
 * Estado -> interface
 * ====================================================================== */

static void refresh_ui(void)
{
    gchar *name = G.cfg.videoPath && *G.cfg.videoPath ? g_path_get_basename(G.cfg.videoPath) : NULL;
    gchar *text = NULL;
    int w = 0, h = 0;
    wp_get_video_size(&w, &h);

    switch (G.state) {
    case WP_LOADING:
        text = g_strdup_printf("Carregando “%s”...", name ? name : "");
        break;
    case WP_PLAYING:
        if (w > 0 && h > 0)
            text = g_strdup_printf("Reproduzindo “%s” (%d×%d) em loop na área de trabalho.",
                                   name, w, h);
        else
            text = g_strdup_printf("Reproduzindo “%s” em loop na área de trabalho.", name);
        break;
    case WP_PAUSED:
        text = g_strdup_printf("Pausado — “%s”.", name);
        break;
    case WP_AUTOPAUSED:
        text = g_strdup("Pausado automaticamente enquanto há um aplicativo em tela cheia "
                        "ou uma janela maximizada em foco.");
        break;
    case WP_ERROR:
        text = g_strdup(G.errorText ? G.errorText : "Erro desconhecido.");
        break;
    case WP_IDLE:
    default:
        text = g_strdup(name ? "Parado. Clique em Aplicar para usar o vídeo como papel de parede."
                             : "Escolha um vídeo (ou arraste um arquivo para esta janela).");
        break;
    }
    gtk_label_set_text(GTK_LABEL(G.lblStatus), text);

    gboolean active = is_active_state(G.state);
    gtk_widget_set_sensitive(G.btnApply, G.cfg.videoPath && *G.cfg.videoPath);
    gtk_widget_set_sensitive(G.btnPause, active);
    gtk_widget_set_sensitive(G.btnStop, active);
    gtk_button_set_label(GTK_BUTTON(G.btnPause), G.userPaused ? "Continuar" : "Pausar");
    gtk_widget_set_sensitive(G.scaleVol, !G.cfg.muted);

#ifdef HAVE_APPINDICATOR
    if (G.menuPause) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(G.menuPause), G.userPaused ? "Continuar" : "Pausar");
        gtk_widget_set_sensitive(G.menuPause, active);
        gtk_widget_set_sensitive(G.menuStop, active);
    }
#endif
    g_free(text);
    g_free(name);
}

static void on_wallpaper_state(WpState state, const char *message, gpointer data)
{
    (void)data;
    G.state = state;
    if (state == WP_ERROR) {
        g_free(G.errorText);
        G.errorText = g_strdup(message);
        G.userPaused = FALSE;
        G.cfg.wasPlaying = FALSE;
        config_save(&G.cfg);
    } else if (state == WP_IDLE) {
        G.userPaused = FALSE;
    }
    refresh_ui();
}

static void load_config_into_ui(void)
{
    G.loading = TRUE;
    gtk_entry_set_text(GTK_ENTRY(G.entry), G.cfg.videoPath ? G.cfg.videoPath : "");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(G.chkMute), G.cfg.muted);
    gtk_range_set_value(GTK_RANGE(G.scaleVol), G.cfg.volume);
    gtk_combo_box_set_active(GTK_COMBO_BOX(G.cmbScale), G.cfg.scaleMode);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(G.chkAutostart), autostart_is_enabled());
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(G.chkResume), G.cfg.resumeOnStart);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(G.chkFull), G.cfg.pauseFullscreen);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(G.chkMax), G.cfg.pauseMaximized);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(G.chkBg), G.cfg.closeToBackground);
    G.loading = FALSE;
}

/* ======================================================================
 * Ações
 * ====================================================================== */

static void apply(void)
{
    if (!G.cfg.videoPath || !*G.cfg.videoPath) return;
    GError *err = NULL;
    G.userPaused = FALSE;
    if (wp_start(G.cfg.videoPath, G.cfg.muted, G.cfg.volume, (ScaleMode)G.cfg.scaleMode, &err)) {
        G.cfg.wasPlaying = TRUE;
    } else {
        g_free(G.errorText);
        G.errorText = g_strdup(err->message);
        G.state = WP_ERROR;
        G.cfg.wasPlaying = FALSE;
        g_error_free(err);
    }
    config_save(&G.cfg);
    refresh_ui();
}

static void set_video_path(const char *path)
{
    g_free(G.cfg.videoPath);
    G.cfg.videoPath = g_strdup(path);
    gtk_entry_set_text(GTK_ENTRY(G.entry), path);
    apply();
}

static void browse(void)
{
    GtkFileChooserNative *dlg = gtk_file_chooser_native_new(
        "Escolha o vídeo para o papel de parede", GTK_WINDOW(G.win),
        GTK_FILE_CHOOSER_ACTION_OPEN, "_Abrir", "_Cancelar");
    GtkFileChooser *fc = GTK_FILE_CHOOSER(dlg);

    GtkFileFilter *videos = gtk_file_filter_new();
    gtk_file_filter_set_name(videos, "Vídeos");
    gtk_file_filter_add_mime_type(videos, "video/*");
    const char *patterns[] = { "*.mp4", "*.m4v", "*.mkv", "*.webm", "*.mov", "*.avi", "*.wmv",
                               "*.flv", "*.ogv", "*.gif", NULL };
    for (int i = 0; patterns[i]; i++) gtk_file_filter_add_pattern(videos, patterns[i]);
    gtk_file_chooser_add_filter(fc, videos);

    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "Todos os arquivos");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(fc, all);

    if (G.cfg.videoPath && *G.cfg.videoPath)
        gtk_file_chooser_set_filename(fc, G.cfg.videoPath);

    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        gchar *file = gtk_file_chooser_get_filename(fc);
        if (file) set_video_path(file);
        g_free(file);
    }
    g_object_unref(dlg);
}

static void toggle_pause(void)
{
    if (!is_active_state(G.state)) return;
    G.userPaused = !G.userPaused;
    wp_set_user_paused(G.userPaused);
    refresh_ui();
}

static void stop(void)
{
    G.userPaused = FALSE;
    wp_stop();
    G.cfg.wasPlaying = FALSE;
    config_save(&G.cfg);
    refresh_ui();
}

static void quit_app(void)
{
    G.cfg.wasPlaying = wp_is_active();
    config_save(&G.cfg);
    wp_shutdown();
    g_application_quit(G_APPLICATION(G.app));
}

void gui_present(void)
{
    if (!G.win) return;
    gtk_widget_show(G.win);
    gtk_window_present(GTK_WINDOW(G.win));
}

/* ======================================================================
 * Pausa automática (X11): olha a janela ativa via EWMH
 * ====================================================================== */

#ifdef GDK_WINDOWING_X11
static unsigned long x11_get_window_prop(Display *d, Window w, Atom prop, Atom type,
                                         unsigned char **data)
{
    Atom actualType;
    int actualFormat;
    unsigned long n = 0, after = 0;
    *data = NULL;
    if (XGetWindowProperty(d, w, prop, 0, 64, False, type, &actualType, &actualFormat,
                           &n, &after, data) != Success || actualType != type) {
        if (*data) { XFree(*data); *data = NULL; }
        return 0;
    }
    return n;
}

static gboolean x11_should_autopause(void)
{
    GdkDisplay *gd = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gd)) return FALSE;
    Display *d = GDK_DISPLAY_XDISPLAY(gd);

    Atom aActive = XInternAtom(d, "_NET_ACTIVE_WINDOW", False);
    Atom aState  = XInternAtom(d, "_NET_WM_STATE", False);
    Atom aFull   = XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False);
    Atom aMaxV   = XInternAtom(d, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    Atom aMaxH   = XInternAtom(d, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    Atom aHidden = XInternAtom(d, "_NET_WM_STATE_HIDDEN", False);

    gboolean result = FALSE;
    gdk_x11_display_error_trap_push(gd);

    unsigned char *data = NULL;
    Window active = None;
    if (x11_get_window_prop(d, DefaultRootWindow(d), aActive, XA_WINDOW, &data) >= 1)
        active = (Window)((unsigned long *)data)[0];
    if (data) XFree(data);

    GdkWindow *own = G.win ? gtk_widget_get_window(G.win) : NULL;
    if (active != None && !wp_owns_xid(active) &&
        !(own && gdk_x11_window_get_xid(own) == active)) {
        unsigned long n = x11_get_window_prop(d, active, aState, XA_ATOM, &data);
        gboolean full = FALSE, maxV = FALSE, maxH = FALSE, hidden = FALSE;
        for (unsigned long i = 0; i < n; i++) {
            Atom a = (Atom)((unsigned long *)data)[i];
            if (a == aFull) full = TRUE;
            else if (a == aMaxV) maxV = TRUE;
            else if (a == aMaxH) maxH = TRUE;
            else if (a == aHidden) hidden = TRUE;
        }
        if (data) XFree(data);
        if (!hidden) {
            if (G.cfg.pauseFullscreen && full) result = TRUE;
            if (G.cfg.pauseMaximized && maxV && maxH) result = TRUE;
        }
    }
    gdk_x11_display_error_trap_pop_ignored(gd);
    return result;
}
#endif

static gboolean poll_autopause(gpointer data)
{
    (void)data;
    gboolean want = FALSE;
#ifdef GDK_WINDOWING_X11
    if (G.cfg.pauseFullscreen || G.cfg.pauseMaximized)
        want = x11_should_autopause();
#endif
    if (want != G.autoPaused) {
        G.autoPaused = want;
        wp_set_auto_paused(want);
    }
    return G_SOURCE_CONTINUE;
}

/* ======================================================================
 * Sinais dos controles
 * ====================================================================== */

static void on_browse(GtkButton *b, gpointer d) { (void)b; (void)d; browse(); }
static void on_pause(GtkButton *b, gpointer d)  { (void)b; (void)d; toggle_pause(); }
static void on_stop(GtkButton *b, gpointer d)   { (void)b; (void)d; stop(); }

static void on_apply(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!G.cfg.videoPath || !*G.cfg.videoPath) browse();
    else apply();
}

static void on_mute(GtkToggleButton *t, gpointer d)
{
    (void)d;
    if (G.loading) return;
    G.cfg.muted = gtk_toggle_button_get_active(t);
    wp_set_muted(G.cfg.muted);
    config_save(&G.cfg);
    refresh_ui();
}

static void on_volume(GtkRange *r, gpointer d)
{
    (void)d;
    if (G.loading) return;
    G.cfg.volume = (int)gtk_range_get_value(r);
    wp_set_volume(G.cfg.volume);
}

static gboolean on_volume_released(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w; (void)e; (void)d;
    config_save(&G.cfg);
    return FALSE;
}

static void on_scale_mode(GtkComboBox *c, gpointer d)
{
    (void)d;
    if (G.loading) return;
    int sel = gtk_combo_box_get_active(c);
    if (sel < 0 || sel >= SCALE_COUNT) return;
    G.cfg.scaleMode = sel;
    wp_set_scale((ScaleMode)sel);
    config_save(&G.cfg);
}

static void on_option_toggled(GtkToggleButton *t, gpointer d)
{
    (void)d;
    if (G.loading) return;
    gboolean on = gtk_toggle_button_get_active(t);

    if (GTK_WIDGET(t) == G.chkAutostart) {
        GError *err = NULL;
        if (!autostart_set(on, &err)) {
            G.loading = TRUE;
            gtk_toggle_button_set_active(t, !on);
            G.loading = FALSE;
            GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(G.win), GTK_DIALOG_MODAL,
                GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
                "Não foi possível alterar a inicialização automática:\n%s",
                err ? err->message : "erro desconhecido");
            gtk_dialog_run(GTK_DIALOG(m));
            gtk_widget_destroy(m);
            g_clear_error(&err);
        }
        return;
    }
    if (GTK_WIDGET(t) == G.chkResume) G.cfg.resumeOnStart = on;
    else if (GTK_WIDGET(t) == G.chkFull) G.cfg.pauseFullscreen = on;
    else if (GTK_WIDGET(t) == G.chkMax) G.cfg.pauseMaximized = on;
    else if (GTK_WIDGET(t) == G.chkBg) G.cfg.closeToBackground = on;
    config_save(&G.cfg);
    poll_autopause(NULL);
}

static void on_drag_data(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                         GtkSelectionData *sel, guint info, guint time, gpointer d)
{
    (void)w; (void)ctx; (void)x; (void)y; (void)info; (void)time; (void)d;
    gchar **uris = gtk_selection_data_get_uris(sel);
    if (uris && uris[0]) {
        gchar *path = g_filename_from_uri(uris[0], NULL, NULL);
        if (path) set_video_path(path);
        g_free(path);
    }
    g_strfreev(uris);
}

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)e; (void)d;
    if (!G.cfg.closeToBackground) {
        quit_app();
        return TRUE;
    }
    if (!G.cfg.backgroundHintShown) {
        GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(w), GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
            "O Video Wallpaper continua rodando em segundo plano.");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m),
#ifdef HAVE_APPINDICATOR
            "Para reabrir este painel ou sair, use o ícone na bandeja do sistema "
            "ou abra o aplicativo novamente."
#else
            "Para reabrir este painel, abra o aplicativo novamente. "
            "Para encerrar, desmarque a última opção e feche a janela."
#endif
        );
        gtk_dialog_run(GTK_DIALOG(m));
        gtk_widget_destroy(m);
        G.cfg.backgroundHintShown = TRUE;
        config_save(&G.cfg);
    }
    gtk_widget_hide(w);
    return TRUE;
}

/* ======================================================================
 * Bandeja (AppIndicator / StatusNotifierItem)
 * ====================================================================== */

#ifdef HAVE_APPINDICATOR
static void on_menu_show(GtkMenuItem *i, gpointer d)  { (void)i; (void)d; gui_present(); }
static void on_menu_pause(GtkMenuItem *i, gpointer d) { (void)i; (void)d; toggle_pause(); }
static void on_menu_stop(GtkMenuItem *i, gpointer d)  { (void)i; (void)d; stop(); }
static void on_menu_quit(GtkMenuItem *i, gpointer d)  { (void)i; (void)d; quit_app(); }

static void create_indicator(void)
{
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    G.indicator = app_indicator_new("videowallpaper",
                                    G.iconPath ? G.iconPath : "video-display",
                                    APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_title(G.indicator, "Video Wallpaper");

    GtkWidget *menu = gtk_menu_new();
    GtkWidget *show = gtk_menu_item_new_with_label("Abrir painel");
    G.menuPause = gtk_menu_item_new_with_label("Pausar");
    G.menuStop  = gtk_menu_item_new_with_label("Parar");
    GtkWidget *quit = gtk_menu_item_new_with_label("Sair");
    g_signal_connect(show, "activate", G_CALLBACK(on_menu_show), NULL);
    g_signal_connect(G.menuPause, "activate", G_CALLBACK(on_menu_pause), NULL);
    g_signal_connect(G.menuStop, "activate", G_CALLBACK(on_menu_stop), NULL);
    g_signal_connect(quit, "activate", G_CALLBACK(on_menu_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), show);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), G.menuPause);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), G.menuStop);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);
    gtk_widget_show_all(menu);

    app_indicator_set_menu(G.indicator, GTK_MENU(menu));
    app_indicator_set_secondary_activate_target(G.indicator, show);
    app_indicator_set_status(G.indicator, APP_INDICATOR_STATUS_ACTIVE);
    G_GNUC_END_IGNORE_DEPRECATIONS
}
#endif

/* ======================================================================
 * Construção da janela
 * ====================================================================== */

static GtkWidget *framed(const char *title, GtkWidget *child)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_frame_set_label_widget(GTK_FRAME(frame), label);
    gtk_container_set_border_width(GTK_CONTAINER(child), 10);
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}

static GtkWidget *check(const char *label, const char *tooltip)
{
    GtkWidget *c = gtk_check_button_new_with_label(label);
    if (tooltip) gtk_widget_set_tooltip_text(c, tooltip);
    g_signal_connect(c, "toggled", G_CALLBACK(on_option_toggled), NULL);
    return c;
}

static void build_window(GtkApplication *app)
{
    G.win = gtk_application_window_new(app);
    GtkWindow *win = GTK_WINDOW(G.win);
    gtk_window_set_title(win, "Video Wallpaper");
    gtk_window_set_resizable(win, FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(win), 14);
    if (G.iconPath) gtk_window_set_icon_from_file(win, G.iconPath, NULL);
    else gtk_window_set_icon_name(win, "videowallpaper");

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_add(GTK_CONTAINER(win), root);

    /* --- Vídeo --- */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    G.entry = gtk_entry_new();
    gtk_editable_set_editable(GTK_EDITABLE(G.entry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(G.entry), "Nenhum vídeo escolhido");
    gtk_widget_set_hexpand(G.entry, TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(G.entry), 40);
    G.btnBrowse = gtk_button_new_with_label("Procurar...");
    g_signal_connect(G.btnBrowse, "clicked", G_CALLBACK(on_browse), NULL);
    gtk_box_pack_start(GTK_BOX(row), G.entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), G.btnBrowse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), framed("Vídeo", row), FALSE, FALSE, 0);

    /* --- Reprodução --- */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
    G.btnApply = gtk_button_new_with_label("Aplicar");
    gtk_style_context_add_class(gtk_widget_get_style_context(G.btnApply), "suggested-action");
    G.btnPause = gtk_button_new_with_label("Pausar");
    G.btnStop  = gtk_button_new_with_label("Parar");
    g_signal_connect(G.btnApply, "clicked", G_CALLBACK(on_apply), NULL);
    g_signal_connect(G.btnPause, "clicked", G_CALLBACK(on_pause), NULL);
    g_signal_connect(G.btnStop, "clicked", G_CALLBACK(on_stop), NULL);
    gtk_box_pack_start(GTK_BOX(buttons), G.btnApply, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), G.btnPause, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), G.btnStop, TRUE, TRUE, 0);
    gtk_grid_attach(GTK_GRID(grid), buttons, 0, 0, 3, 1);

    G.chkMute = gtk_check_button_new_with_label("Sem som");
    g_signal_connect(G.chkMute, "toggled", G_CALLBACK(on_mute), NULL);
    GtkWidget *lblVol = gtk_label_new("Volume:");
    G.scaleVol = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_digits(GTK_SCALE(G.scaleVol), 0);
    gtk_scale_set_value_pos(GTK_SCALE(G.scaleVol), GTK_POS_RIGHT);
    gtk_widget_set_hexpand(G.scaleVol, TRUE);
    g_signal_connect(G.scaleVol, "value-changed", G_CALLBACK(on_volume), NULL);
    g_signal_connect(G.scaleVol, "button-release-event", G_CALLBACK(on_volume_released), NULL);
    gtk_grid_attach(GTK_GRID(grid), G.chkMute, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lblVol, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), G.scaleVol, 2, 1, 1, 1);

    GtkWidget *lblScale = gtk_label_new("Ajuste:");
    gtk_widget_set_halign(lblScale, GTK_ALIGN_START);
    G.cmbScale = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(G.cmbScale),
                                   "Preencher a tela (corta as bordas se precisar)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(G.cmbScale),
                                   "Ajustar à tela (vídeo inteiro, com barras pretas)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(G.cmbScale),
                                   "Tamanho original (centralizado)");
    g_signal_connect(G.cmbScale, "changed", G_CALLBACK(on_scale_mode), NULL);
    gtk_grid_attach(GTK_GRID(grid), lblScale, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), G.cmbScale, 1, 2, 2, 1);
    gtk_box_pack_start(GTK_BOX(root), framed("Reprodução", grid), FALSE, FALSE, 0);

    /* --- Opções --- */
    GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gchar *autostartFile = g_build_filename(g_get_user_config_dir(), "autostart",
                                            "videowallpaper.desktop", NULL);
    gchar *tip = g_strdup_printf("Cria %s", autostartFile);
    G.chkAutostart = check("Iniciar junto com a sessão", tip);
    g_free(tip);
    g_free(autostartFile);
    G.chkResume = check("Retomar o último vídeo ao abrir o aplicativo", NULL);
    G.chkFull   = check("Pausar quando um jogo ou aplicativo estiver em tela cheia", NULL);
    G.chkMax    = check("Pausar quando a janela em foco estiver maximizada", NULL);
    G.chkBg     = check("Ao fechar, continuar rodando em segundo plano", NULL);
    if (!autopause_supported()) {
        const char *t = "Disponível apenas no X11: o Wayland não informa a outros "
                        "aplicativos qual janela está em tela cheia.";
        gtk_widget_set_sensitive(G.chkFull, FALSE);
        gtk_widget_set_sensitive(G.chkMax, FALSE);
        gtk_widget_set_tooltip_text(G.chkFull, t);
        gtk_widget_set_tooltip_text(G.chkMax, t);
    }
    gtk_box_pack_start(GTK_BOX(opts), G.chkAutostart, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opts), G.chkResume, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opts), G.chkFull, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opts), G.chkMax, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opts), G.chkBg, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), framed("Opções", opts), FALSE, FALSE, 0);

    /* --- Status --- */
    G.lblStatus = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(G.lblStatus), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(G.lblStatus), 60);
    gtk_label_set_xalign(GTK_LABEL(G.lblStatus), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(G.lblStatus), TRUE);
    gtk_box_pack_start(GTK_BOX(root), G.lblStatus, FALSE, FALSE, 0);

    gchar *backend = g_strdup_printf("Modo: %s", wp_backend_description());
    G.lblBackend = gtk_label_new(backend);
    g_free(backend);
    gtk_label_set_xalign(GTK_LABEL(G.lblBackend), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(G.lblBackend), "dim-label");
    gtk_box_pack_start(GTK_BOX(root), G.lblBackend, FALSE, FALSE, 0);

    /* arrastar e soltar um arquivo de vídeo na janela */
    gtk_drag_dest_set(G.win, GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY);
    gtk_drag_dest_add_uri_targets(G.win);
    g_signal_connect(G.win, "drag-data-received", G_CALLBACK(on_drag_data), NULL);
    g_signal_connect(G.win, "delete-event", G_CALLBACK(on_delete), NULL);

    gtk_widget_show_all(root);
}

/* ======================================================================
 * API pública
 * ====================================================================== */

void gui_create(GtkApplication *app, gboolean windowed, gboolean startHidden)
{
    memset(&G, 0, sizeof(G));
    G.app = app;
    G.state = WP_IDLE;
    config_load(&G.cfg);
    G.iconPath = find_icon();
    if (G.iconPath) gtk_window_set_default_icon_from_file(G.iconPath, NULL);

    wp_init(windowed, on_wallpaper_state, NULL);
    build_window(app);
    load_config_into_ui();
    refresh_ui();

#ifdef HAVE_APPINDICATOR
    create_indicator();
#endif

    if (autopause_supported())
        G.pollId = g_timeout_add(AUTOPAUSE_INTERVAL_MS, poll_autopause, NULL);

    if (G.cfg.resumeOnStart && G.cfg.wasPlaying && G.cfg.videoPath && *G.cfg.videoPath)
        apply();

    if (!startHidden) gui_present();
}

void gui_shutdown(void)
{
    if (G.pollId) { g_source_remove(G.pollId); G.pollId = 0; }
    wp_shutdown();
#ifdef HAVE_APPINDICATOR
    g_clear_object(&G.indicator);
#endif
    config_clear(&G.cfg);
    g_clear_pointer(&G.errorText, g_free);
    g_clear_pointer(&G.iconPath, g_free);
}
