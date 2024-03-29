#pragma once

#include <stdbool.h>
#include "banner.h"

typedef struct _cairo_surface cairo_surface_t;
typedef struct _cairo cairo_t;
struct modules;
struct display;

// draw
struct ui;

struct ui* ui_create(struct modules*);
void ui_destroy(struct ui*);
void ui_set_display(struct ui*, struct display*);

// Draws the ui on the given cairo surface, with the given context for it.
// If (banner == banner_none) will draw the dashboard, otherwise the
// specified banner type.
void ui_draw(struct ui*, cairo_t*, unsigned width, unsigned height, enum banner);

// Passes the given pressed key to the ui.
// The key is a linux key code. Returns whether the dashboard should
// be closed.
bool ui_key(struct ui*, unsigned);

