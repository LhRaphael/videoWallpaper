/*
 * config.h — configurações em $XDG_CONFIG_HOME/videowallpaper/config.ini e
 * inicialização automática via $XDG_CONFIG_HOME/autostart (padrão XDG,
 * funciona em GNOME, KDE, XFCE, Cinnamon, MATE, LXQt...).
 */
#ifndef VW_CONFIG_H
#define VW_CONFIG_H

#include <glib.h>

typedef struct {
    gchar   *videoPath;
    gboolean muted;
    int      volume;            /* 0..100 */
    int      scaleMode;         /* ScaleMode */
    gboolean resumeOnStart;
    gboolean wasPlaying;
    gboolean pauseFullscreen;
    gboolean pauseMaximized;
    gboolean closeToBackground;
    gboolean backgroundHintShown;
} Config;

void config_load(Config *c);
void config_save(const Config *c);
void config_clear(Config *c);

gboolean autostart_is_enabled(void);
gboolean autostart_set(gboolean enable, GError **error);

#endif
