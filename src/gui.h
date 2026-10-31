/*
 * gui.h — painel de controle (GTK3) e ícone da bandeja (AppIndicator,
 * opcional).
 */
#ifndef VW_GUI_H
#define VW_GUI_H

#include <gtk/gtk.h>

void gui_create(GtkApplication *app, gboolean windowed, gboolean startHidden);
void gui_present(void);
void gui_shutdown(void);

#endif
