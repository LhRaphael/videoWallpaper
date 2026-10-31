/*
 * wallpaper.h — cria uma superfície de vídeo atrás das janelas em cada
 * monitor e controla os reprodutores.
 *
 * Backends:
 *   - X11: janela _NET_WM_WINDOW_TYPE_DESKTOP, sempre abaixo, em todas as
 *     áreas de trabalho, sem foco e com clique passando direto.
 *   - Wayland: protocolo wlr-layer-shell, camada BOTTOM (KDE Plasma,
 *     Sway, Hyprland, river, Wayfire, labwc, COSMIC...).
 *   - Janela comum (--windowed): para testes.
 */
#ifndef VW_WALLPAPER_H
#define VW_WALLPAPER_H

#include <gtk/gtk.h>
#include "player.h"

typedef enum {
    WP_IDLE = 0,
    WP_LOADING,
    WP_PLAYING,
    WP_PAUSED,        /* pausado pelo usuário                  */
    WP_AUTOPAUSED,    /* pausado automaticamente (tela cheia)   */
    WP_ERROR
} WpState;

typedef enum {
    WP_BACKEND_NONE = 0,
    WP_BACKEND_X11,
    WP_BACKEND_LAYER_SHELL,
    WP_BACKEND_WINDOWED
} WpBackend;

/* message: texto do erro (WP_ERROR) ou NULL */
typedef void (*WpStateFunc)(WpState state, const char *message, gpointer user_data);

void        wp_init(gboolean windowed, WpStateFunc cb, gpointer user_data);
WpBackend   wp_get_backend(void);
const char *wp_backend_description(void);

/* Retorna FALSE (e preenche *error) se não for possível começar. */
gboolean wp_start(const char *path, gboolean muted, int volume, ScaleMode scale, GError **error);
void     wp_stop(void);
void     wp_shutdown(void);

void wp_set_user_paused(gboolean paused);
void wp_set_auto_paused(gboolean paused);
void wp_set_muted(gboolean muted);
void wp_set_volume(int volume);
void wp_set_scale(ScaleMode scale);

WpState  wp_get_state(void);
gboolean wp_is_active(void);
void     wp_get_video_size(int *w, int *h);

/* TRUE se a janela X11 pertence a uma das nossas superfícies */
gboolean wp_owns_xid(unsigned long xid);

#endif
