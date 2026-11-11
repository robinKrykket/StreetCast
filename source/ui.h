// Look-and-feel helpers: bubbly white buttons, the pulsing blue focus glow, text layout, icons.
#pragma once
#include "gfx.h"

#define COL_BG       0xEEF2F6FF
#define COL_BG_TOP   0xF8FAFCFF
#define COL_WHITE    0xFFFFFFFF
#define COL_BORDER   0xC6CFD9FF
#define COL_SHADOW   0xD8DFE7FF
#define COL_LINE     0xE1E7EEFF
#define COL_TEXT     0x454B53FF
#define COL_TEXT2    0x8A929CFF
#define COL_GLOW     0x36C5F7FF // the Wii-remote-LED blue
#define COL_BLUE     0x1BA4DCFF // darker blue for text/icons on white
#define COL_TRACK    0xDCE3EAFF
#define COL_GREEN    0x4DBB78FF
#define COL_RED      0xE5625CFF

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
