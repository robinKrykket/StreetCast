// citro2d implementation of gfx.h plus the system queries the status bar needs.
#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "gfx.h"
#include "gfx_3ds.h"

static C2D_TextBuf text_buf;
static float text_scale = 1.0f; // system font cell -> 30 px line
static void fan_init(void);
// Drawing outside C3D_FrameBegin/End writes GPU commands with no buffer bound, which is an
// instant svcBreak crash. Every draw call checks this.
static bool in_frame;

void gfx3ds_init(void)
{
	text_buf = C2D_TextBufNew(8192);
	fan_init();
	fontEnsureMapped();
	TGLP_s* tglp = fontGetGlyphInfo(NULL);
	if (tglp && tglp->cellHeight) text_scale = 30.0f / (float)tglp->cellHeight;
}

void gfx3ds_frame(void)
{
	C2D_TextBufClear(text_buf);
	in_frame = true;
}

void gfx3ds_frame_end(void)
{
	in_frame = false;
}

void gfx3ds_exit(void)
{
	C2D_TextBufDelete(text_buf);
}

static inline u32 conv(Color c)
{
	return C2D_Color32((u8)(c >> 24), (u8)(c >> 16), (u8)(c >> 8), (u8)c);
}

void gfx_rect(float x, float y, float w, float h, Color c)
{
	if (!in_frame) return;
	C2D_DrawRectSolid(x, y, 0, w, h, conv(c));
}

void gfx_rect_vgrad(float x, float y, float w, float h, Color top, Color bottom)
{
	if (!in_frame) return;
	u32 t = conv(top), b = conv(bottom);
	C2D_DrawRectangle(x, y, 0, w, h, t, t, b, b);
}

// Circles are drawn as triangle fans rather than with citro2d's ellipse primitive. The ellipse
// uses a different GPU mode, and every switch between it and plain shapes ends a draw batch;
// a UI made of rounded rectangles switched hundreds of times per frame and overflowed the GPU
// command buffer. Fans keep every shape in one batch.
#define FAN_STEPS 48 // per full turn
static float fan_cos[FAN_STEPS + 1], fan_sin[FAN_STEPS + 1];

static void fan_init(void)
{
	for (int i = 0; i <= FAN_STEPS; i++) {
		float a = (float)i * (6.2831853f / FAN_STEPS);
		fan_cos[i] = cosf(a);
		fan_sin[i] = sinf(a);
	}
}

// Fan from table index `from` to `to`, using every `stride`-th point.
static void fan(float cx, float cy, float r, int from, int to, int stride, u32 col)
{
	float px = cx + fan_cos[from] * r, py = cy + fan_sin[from] * r;
	for (int i = from + stride; i <= to; i += stride) {
		float x = cx + fan_cos[i] * r, y = cy + fan_sin[i] * r;
		C2D_DrawTriangle(cx, cy, col, px, py, col, x, y, col, 0);
		px = x;
		py = y;
	}
}

void gfx_circle(float cx, float cy, float r, Color c)
{
	if (r <= 0 || !in_frame) return;
	fan(cx, cy, r, 0, FAN_STEPS, r < 5 ? 4 : (r < 16 ? 2 : 1), conv(c));
}

void gfx_quarter(float cx, float cy, float r, int q, Color c)
{
	if (r <= 0 || !in_frame) return;
	// Screen y points down, so angle 0..90 degrees is the bottom-right quadrant.
	static const int start[4] = {FAN_STEPS / 2, FAN_STEPS * 3 / 4, 0, FAN_STEPS / 4};
	fan(cx, cy, r, start[q & 3], start[q & 3] + FAN_STEPS / 4, r < 5 ? 4 : (r < 16 ? 2 : 1), conv(c));
}

void gfx_tri(float x0, float y0, float x1, float y1, float x2, float y2, Color c)
{
	if (!in_frame) return;
	u32 col = conv(c);
	C2D_DrawTriangle(x0, y0, col, x1, y1, col, x2, y2, col, 0);
}

void gfx_text(const char* utf8, float x, float y, float scale, Color c)
{
	if (!in_frame) return;
	C2D_Text t;
	if (!C2D_TextParse(&t, text_buf, utf8)) return; // out of glyph space this frame
	C2D_DrawText(&t, C2D_WithColor, x, y, 0, scale, scale, conv(c));
}

float gfx_text_width(const char* utf8, float scale)
{
	// Same arithmetic citro2d uses when laying out the system font, without touching the text buffer.
	float w = 0;
	const uint8_t* p = (const uint8_t*)utf8;
	while (*p) {
		uint32_t cp;
		ssize_t n = decode_utf8(&cp, p);
		if (n <= 0) {
			p++;
			continue;
		}
		p += n;
		charWidthInfo_s* cw = fontGetCharWidthInfo(NULL, fontGlyphIndexFromCodePoint(NULL, cp));
		if (cw) w += (float)cw->charWidth;
	}
	return w * text_scale * scale;
}

int sys_battery(void)
{
	u8 level = 5;
	if (R_FAILED(PTMU_GetBatteryLevel(&level))) return 5;
	return level > 5 ? 5 : level;
}

bool sys_charging(void)
{
	u8 charging = 0;
	return R_SUCCEEDED(PTMU_GetBatteryChargeState(&charging)) && charging;
}

int sys_wifi(void)
{
	return osGetWifiStrength();
}

void sys_clock(int* hour, int* minute)
{
	time_t now = time(NULL);
	struct tm* tm = localtime(&now);
	*hour = tm ? tm->tm_hour : 0;
	*minute = tm ? tm->tm_min : 0;
}

bool sys_keyboard(const char* hint, const char* initial, char* out, int cap)
{
	SwkbdState kb;
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, cap - 1);
	swkbdSetHintText(&kb, hint);
	if (initial && initial[0]) swkbdSetInitialText(&kb, initial);
	swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, "Cancel", false);
	swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "OK", true);
	swkbdSetFeatures(&kb, SWKBD_DEFAULT_QWERTY);
	out[0] = 0;
	SwkbdButton b = swkbdInputText(&kb, out, (size_t)cap);
	return b == SWKBD_BUTTON_RIGHT && out[0];
}
