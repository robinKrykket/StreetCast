// StreetCast application logic and screens.
#pragma once
#include <stdbool.h>

#include "gfx.h"

void app_init(bool sound_ok);
// Advance one frame. Returns false when the user asked to quit.
bool app_update(const Input* in);
void app_draw_top(void);
void app_draw_bottom(void);
// Show the "Closing" overlay on the next drawn frame.
void app_set_closing(void);
// Save everything and stop the foreground task. Returns false if that task would not stop.
bool app_shutdown(void);
// True while a foreground network task (refresh, search, subscribe) is running.
bool app_task_busy(void);
