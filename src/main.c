/*
 * Video Wallpaper (Linux) — exibe um vídeo em loop como papel de parede.
 *
 * main.c — GtkApplication (instância única via D-Bus), opções de linha de
 * comando e escolha do backend gráfico.
 *
 * Uso: videowallpaper [--hidden] [--windowed]
 *   --hidden    inicia sem mostrar o painel (usado pelo autostart)
 *   --windowed  depuração: vídeo numa janela comum, não no fundo
 */
#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#ifdef HAVE_LAYER_SHELL
#include <gtk-layer-shell.h>
#endif

#include "gui.h"

#define APP_ID      "io.github.VideoWallpaper"
#define APP_VERSION "1.0.0"

#if GLIB_CHECK_VERSION(2, 74, 0)
#define VW_APP_FLAGS G_APPLICATION_DEFAULT_FLAGS
#else
#define VW_APP_FLAGS G_APPLICATION_FLAGS_NONE
#endif

static gboolean optHidden;
static gboolean optWindowed;
static gboolean optVersion;
static gboolean created;

static GOptionEntry entries[] = {
    { "hidden",   0,  0, G_OPTION_ARG_NONE, &optHidden,   "Inicia sem mostrar o painel", NULL },
    { "windowed", 0,  0, G_OPTION_ARG_NONE, &optWindowed, "Depuração: vídeo numa janela comum", NULL },
    { "version",  'v', 0, G_OPTION_ARG_NONE, &optVersion, "Mostra a versão", NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL }
};

/*
 * No Wayland só dá para ficar no fundo com o protocolo wlr-layer-shell.
 * O GNOME (Mutter) não o implementa; nesse caso o programa se reinicia
 * pelo XWayland (GDK_BACKEND=x11), onde a janela do tipo "desktop"
 * funciona. Defina VW_NO_REEXEC=1 para desativar esse comportamento.
 */
static void maybe_reexec_under_xwayland(char **argv)
{
    if (g_getenv("VW_NO_REEXEC") || g_getenv("GDK_BACKEND")) return;
    if (!g_getenv("WAYLAND_DISPLAY") || !g_getenv("DISPLAY")) return;

    gboolean needX11 = TRUE;
#if defined(HAVE_LAYER_SHELL) && defined(GDK_WINDOWING_WAYLAND)
    if (gtk_init_check(NULL, NULL)) {
        GdkDisplay *d = gdk_display_get_default();
        needX11 = !(d && GDK_IS_WAYLAND_DISPLAY(d) && gtk_layer_is_supported());
    }
#endif
    if (!needX11) return;

    /* exec pelo caminho real (via /proc/self/exe o processo apareceria
     * com o nome "exe" no monitor do sistema) */
    gchar *exe = g_file_read_link("/proc/self/exe", NULL);
    g_setenv("GDK_BACKEND", "x11", TRUE);
    g_setenv("VW_NO_REEXEC", "1", TRUE);
    execv(exe ? exe : "/proc/self/exe", argv);
    g_free(exe);
    /* se o exec falhar, segue no Wayland e a interface explica o problema */
    g_unsetenv("GDK_BACKEND");
}

static gint on_handle_local_options(GApplication *app, GVariantDict *opts, gpointer data)
{
    (void)app; (void)opts; (void)data;
    if (optVersion) {
        g_print("Video Wallpaper %s\n", APP_VERSION);
        return 0;
    }
    return -1;   /* continua normalmente */
}

static void on_startup(GApplication *app, gpointer data)
{
    (void)app; (void)data;
    /* O GTK aplica o locale do sistema; o mpv precisa de LC_NUMERIC=C. */
    setlocale(LC_NUMERIC, "C");
}

static void on_activate(GApplication *app, gpointer data)
{
    (void)data;
    if (!created) {
        created = TRUE;
        gui_create(GTK_APPLICATION(app), optWindowed, optHidden);
    } else {
        gui_present();   /* executado de novo: só mostra o painel */
    }
}

static void on_shutdown(GApplication *app, gpointer data)
{
    (void)app; (void)data;
    if (created) gui_shutdown();
}

int main(int argc, char **argv)
{
    /* opções que precisam ser vistas antes de abrir o display */
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--windowed") == 0) optWindowed = TRUE;
        if (g_strcmp0(argv[i], "--version") == 0 || g_strcmp0(argv[i], "-v") == 0) optVersion = TRUE;
    }
    if (!optWindowed && !optVersion)
        maybe_reexec_under_xwayland(argv);

    GtkApplication *app = gtk_application_new(APP_ID, VW_APP_FLAGS);
    g_application_add_main_option_entries(G_APPLICATION(app), entries);
    g_application_set_option_context_summary(G_APPLICATION(app),
        "Exibe um vídeo em loop como papel de parede da área de trabalho.");
    g_signal_connect(app, "handle-local-options", G_CALLBACK(on_handle_local_options), NULL);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
