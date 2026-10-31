/*
 * player.h — um reprodutor libmpv desenhando num GtkGLArea.
 *
 * Cada monitor recebe um Player próprio (uma superfície de papel de
 * parede por saída de vídeo). Só o Player "principal" toca áudio e
 * reporta eventos para a interface.
 */
#ifndef VW_PLAYER_H
#define VW_PLAYER_H

#include <gtk/gtk.h>

typedef enum {
    SCALE_FILL = 0,   /* preenche a tela, cortando as sobras */
    SCALE_FIT,        /* vídeo inteiro, com barras pretas    */
    SCALE_CENTER,     /* tamanho original, centralizado       */
    SCALE_COUNT
} ScaleMode;

typedef enum {
    PLAYER_EVENT_LOADING,
    PLAYER_EVENT_PLAYING,      /* arquivo aberto e tocando        */
    PLAYER_EVENT_VIDEO_SIZE,   /* largura/altura conhecidas        */
    PLAYER_EVENT_ERROR         /* message = descrição do erro      */
} PlayerEventType;

typedef struct {
    PlayerEventType type;
    int width, height;
    const char *message;
} PlayerEvent;

typedef void (*PlayerEventFunc)(const PlayerEvent *ev, gpointer user_data);

typedef struct Player Player;

/* Cria o reprodutor. primary=TRUE: toca áudio e envia eventos. */
Player    *player_new(gboolean primary, PlayerEventFunc cb, gpointer user_data);

/* Widget (GtkGLArea) a ser colocado na janela de fundo. */
GtkWidget *player_get_widget(Player *p);

void player_load(Player *p, const char *path);
void player_set_paused(Player *p, gboolean paused);
void player_set_muted(Player *p, gboolean muted);
void player_set_volume(Player *p, int volume0to100);
void player_set_scale(Player *p, ScaleMode mode);

/* Encerra o mpv. Chame DEPOIS de destruir a janela que contém o widget. */
void player_free(Player *p);

#endif
