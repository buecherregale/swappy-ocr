#pragma once

#include "swappy.h"

bool ocr_find_text(GtkWidget *clicked, struct swappy_state *state);
void ocr_remove_overlay_buttons(struct swappy_state *state);
void ocr_cleanup(struct swappy_state *state);
