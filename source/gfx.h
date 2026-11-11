// Minimal drawing + system layer. The app draws only through this, so the same UI code runs on
// the 3DS (citro2d, gfx_3ds.c) and in the host preview harness (tests/).
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t Color; // 0xRRGGBBAA

#define TOP_W 400
#define BOT_W 320
#define SCREEN_H 240

void gfx_rect(float x, float y, float w, float h, Color c);
void gfx_rect_vgrad(float x, float y, float w, float h, Color top, Color bottom);
void gfx_circle(float cx, float cy, float r, Color c);
// Quarter disc for rounded corners. q: 0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left.
void gfx_quarter(float cx, float cy, float r, int q, Color c);
void gfx_tri(float x0, float y0, float x1, float y1, float x2, float y2, Color c);
// Text uses the system font: scale 1.0 is a 30 px line. y is the top of the line.
void gfx_text(const char* utf8, float x, float y, float scale, Color c);
float gfx_text_width(const char* utf8, float scale);

// Input, in terms the app uses.
enum {
	K_A = 1 << 0, K_B = 1 << 1, K_X = 1 << 2, K_Y = 1 << 3, K_L = 1 << 4, K_R = 1 << 5,
	K_UP = 1 << 6, K_DOWN = 1 << 7, K_LEFT = 1 << 8, K_RIGHT = 1 << 9, K_START = 1 << 10,
	K_SELECT = 1 << 11, K_TOUCH = 1 << 12,
};

typedef struct {
	unsigned down, held, repeat, up;
	int tx, ty; // touch position while K_TOUCH is held
} Input;

// System facts for the status bar.
int sys_battery(void);   // 0..5
bool sys_charging(void);
int sys_wifi(void);      // 0..3 bars
void sys_clock(int* hour, int* minute);
// On-screen keyboard. Returns false if cancelled.
bool sys_keyboard(const char* hint, const char* initial, char* out, int cap);

// Glyphs for the console's buttons in the system font.
#define G_A "\xEE\x80\x80"
#define G_B "\xEE\x80\x81"
#define G_X "\xEE\x80\x82"
#define G_Y "\xEE\x80\x83"
#define G_L "\xEE\x80\x84"
#define G_R "\xEE\x80\x85"
#define G_DPAD "\xEE\x80\x86"
