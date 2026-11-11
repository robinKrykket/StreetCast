// Look-and-feel helpers: bubbly white buttons, the pulsing blue focus glow, text layout, icons.
#pragma once
#include "gfx.h"

// Colours come from the active palette so the whole UI can switch between light and dark.
typedef struct {
	Color bg, bg_top; // screen background (bottom / top of the gradient)
	Color surface;    // buttons, cards, bars: white in light mode, black in dark mode
	Color border, shadow, line;
	Color text, text2;
	Color glow;       // the Wii-remote-LED blue focus glow
	Color blue;       // blue for text/icons on a surface
	Color track;
	Color green, red;
} Palette;

extern Palette ui_pal;
void ui_set_dark(bool dark);
bool ui_is_dark(void);

#define COL_BG       (ui_pal.bg)
#define COL_BG_TOP   (ui_pal.bg_top)
#define COL_WHITE    (ui_pal.surface)
#define COL_BORDER   (ui_pal.border)
#define COL_SHADOW   (ui_pal.shadow)
#define COL_LINE     (ui_pal.line)
#define COL_TEXT     (ui_pal.text)
#define COL_TEXT2    (ui_pal.text2)
#define COL_GLOW     (ui_pal.glow)
#define COL_BLUE     (ui_pal.blue)
#define COL_TRACK    (ui_pal.track)
#define COL_GREEN    (ui_pal.green)
#define COL_RED      (ui_pal.red)

#define TS_BIG   0.62f
#define TS_BODY  0.5f
#define TS_SMALL 0.42f

extern int ui_frame; // frame counter driving animations

Color ui_mix(Color a, Color b, float t);
float ui_pulse(void); // 0..1, slow breathing
void ui_rrect(float x, float y, float w, float h, float r, Color c);
// A white rounded button. When focused it gets the glowing blue outline; `under` is the colour
// of whatever is behind it (the glow is blended towards that).
void ui_bubble(float x, float y, float w, float h, float r, bool focus, Color under);
void ui_line(float x0, float y0, float x1, float y1, float th, Color c);

void ui_text(const char* s, float x, float y, float scale, Color c);
void ui_text_center(const char* s, float cx, float y, float scale, Color c);
void ui_text_right(const char* s, float xr, float y, float scale, Color c);
// One line, shortened with an ellipsis to fit maxw. align: 0 left, 1 centre, 2 right (x is the anchor).
void ui_text_fit(const char* s, float x, float y, float maxw, float scale, Color c, int align);
// Word-wrapped block; returns the number of lines drawn.
int ui_text_wrap(const char* s, float x, float y, float w, float scale, Color c, int max_lines, float line_h, bool center);
// How many lines ui_text_wrap would use. Draws nothing, so it is safe outside a frame.
int ui_text_lines(const char* s, float w, float scale, int max_lines);

void ui_progress(float x, float y, float w, float h, float frac, bool knob, Color under);
void ui_spinner(float cx, float cy, float r, Color under);

typedef enum {
	IC_LIBRARY, IC_PLUS, IC_DOWNLOAD, IC_PLAY, IC_PAUSE, IC_SETTINGS, IC_SEARCH, IC_LINK, IC_FILE,
	IC_BACK15, IC_FWD30, IC_CHECK, IC_WAVES,
} Icon;
void ui_icon(Icon ic, float cx, float cy, float size, Color c, Color under);
