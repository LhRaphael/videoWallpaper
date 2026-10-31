/*
 * config.c — ver config.h.
 */
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "config.h"

#define GROUP "VideoWallpaper"

static gchar *config_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "videowallpaper", "config.ini", NULL);
}

static gchar *autostart_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "autostart", "videowallpaper.desktop", NULL);
}

static gboolean get_bool(GKeyFile *kf, const char *key, gboolean def)
{
    GError *e = NULL;
    gboolean v = g_key_file_get_boolean(kf, GROUP, key, &e);
    if (e) { g_error_free(e); return def; }
    return v;
}

static int get_int(GKeyFile *kf, const char *key, int def)
{
    GError *e = NULL;
    int v = g_key_file_get_integer(kf, GROUP, key, &e);
    if (e) { g_error_free(e); return def; }
    return v;
}

void config_load(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->muted = TRUE;
    c->volume = 50;
    c->scaleMode = 0;
    c->resumeOnStart = TRUE;
    c->pauseFullscreen = TRUE;
    c->pauseMaximized = FALSE;
    c->closeToBackground = TRUE;

    gchar *file = config_file();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, file, G_KEY_FILE_NONE, NULL)) {
        c->videoPath           = g_key_file_get_string(kf, GROUP, "VideoPath", NULL);
        c->muted               = get_bool(kf, "Muted", c->muted);
        c->volume              = get_int(kf, "Volume", c->volume);
        c->scaleMode           = get_int(kf, "ScaleMode", c->scaleMode);
        c->resumeOnStart       = get_bool(kf, "ResumeOnStart", c->resumeOnStart);
        c->wasPlaying          = get_bool(kf, "WasPlaying", FALSE);
        c->pauseFullscreen     = get_bool(kf, "PauseFullscreen", c->pauseFullscreen);
        c->pauseMaximized      = get_bool(kf, "PauseMaximized", c->pauseMaximized);
        c->closeToBackground   = get_bool(kf, "CloseToBackground", c->closeToBackground);
        c->backgroundHintShown = get_bool(kf, "BackgroundHintShown", FALSE);
    }
    g_key_file_unref(kf);
    g_free(file);

    c->volume = CLAMP(c->volume, 0, 100);
    if (c->scaleMode < 0 || c->scaleMode > 2) c->scaleMode = 0;
}

void config_save(const Config *c)
{
    gchar *file = config_file();
    gchar *dir = g_path_get_dirname(file);
    g_mkdir_with_parents(dir, 0700);

    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, GROUP, "VideoPath", c->videoPath ? c->videoPath : "");
    g_key_file_set_boolean(kf, GROUP, "Muted", c->muted);
    g_key_file_set_integer(kf, GROUP, "Volume", c->volume);
    g_key_file_set_integer(kf, GROUP, "ScaleMode", c->scaleMode);
    g_key_file_set_boolean(kf, GROUP, "ResumeOnStart", c->resumeOnStart);
    g_key_file_set_boolean(kf, GROUP, "WasPlaying", c->wasPlaying);
    g_key_file_set_boolean(kf, GROUP, "PauseFullscreen", c->pauseFullscreen);
    g_key_file_set_boolean(kf, GROUP, "PauseMaximized", c->pauseMaximized);
    g_key_file_set_boolean(kf, GROUP, "CloseToBackground", c->closeToBackground);
    g_key_file_set_boolean(kf, GROUP, "BackgroundHintShown", c->backgroundHintShown);

    GError *err = NULL;
    if (!g_key_file_save_to_file(kf, file, &err)) {
        g_warning("Não foi possível salvar %s: %s", file, err->message);
        g_error_free(err);
    }
    g_key_file_unref(kf);
    g_free(dir);
    g_free(file);
}

void config_clear(Config *c)
{
    g_clear_pointer(&c->videoPath, g_free);
}

gboolean autostart_is_enabled(void)
{
    gchar *f = autostart_file();
    gboolean ok = g_file_test(f, G_FILE_TEST_EXISTS);
    g_free(f);
    return ok;
}

/* Aspas no campo Exec segundo a especificação Desktop Entry. */
static gchar *quote_exec_arg(const char *s)
{
    GString *out = g_string_new("\"");
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '`' || *p == '$' || *p == '\\')
            g_string_append_c(out, '\\');
        if (*p == '%') g_string_append_c(out, '%');
        g_string_append_c(out, *p);
    }
    g_string_append_c(out, '"');
    return g_string_free(out, FALSE);
}

gboolean autostart_set(gboolean enable, GError **error)
{
    gchar *file = autostart_file();
    gboolean ok = TRUE;

    if (!enable) {
        if (g_file_test(file, G_FILE_TEST_EXISTS) && g_unlink(file) != 0) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Não foi possível remover %s", file);
            ok = FALSE;
        }
        g_free(file);
        return ok;
    }

    gchar *exe = g_file_read_link("/proc/self/exe", error);
    if (!exe) { g_free(file); return FALSE; }

    gchar *dir = g_path_get_dirname(file);
    g_mkdir_with_parents(dir, 0700);

    gchar *quoted = quote_exec_arg(exe);
    gchar *content = g_strdup_printf(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Video Wallpaper\n"
        "Comment=Vídeo em loop como papel de parede\n"
        "Exec=%s --hidden\n"
        "Icon=videowallpaper\n"
        "Terminal=false\n"
        "X-GNOME-Autostart-enabled=true\n"
        "X-GNOME-Autostart-Delay=3\n"
        "X-KDE-autostart-after=panel\n",
        quoted);
    ok = g_file_set_contents(file, content, -1, error);

    g_free(content);
    g_free(quoted);
    g_free(dir);
    g_free(exe);
    g_free(file);
    return ok;
}
