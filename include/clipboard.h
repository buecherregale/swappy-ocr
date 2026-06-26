#pragma once

#include "swappy.h"

bool clipboard_copy_drawing_area_to_selection(struct swappy_state *state);
bool clipboard_copy_text_to_gdk_clipboard(char *text);
