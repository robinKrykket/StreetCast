#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

int ui_frame;

static const Palette PAL_LIGHT = {
	.bg = 0xEEF2F6FF, .bg_top = 0xF8FAFCFF, .surface = 0xFFFFFFFF,
	.border = 0xC6CFD9FF, .shadow = 0xD8DFE7FF, .line = 0xE1E7EEFF,
	.text = 0x454B53FF, .text2 = 0x8A929CFF,
	.glow = 0x36C5F7FF, .blue = 0x1BA4DCFF, .track = 0xDCE3EAFF,
	.green = 0x4DBB78FF, .red = 0xE5625CFF,
};

// Dark mode: every white surface becomes black, on a charcoal background, with the same glow.
static const Palette PAL_DARK = {
	.bg = 0x1C1F24FF, .bg_top = 0x24282EFF, .surface = 0x000000FF,
	.border = 0x3B424CFF, .shadow = 0x121417FF, .line = 0x2B3037FF,
	.text = 0xE8ECF0FF, .text2 = 0x8E97A2FF,
	.glow = 0x36C5F7FF, .blue = 0x52CDF9FF, .track = 0x30363EFF,
	.green = 0x62CC8CFF, .red = 0xF0746EFF,
};

Palette ui_pal = {
	.bg = 0xEEF2F6FF, .bg_top = 0xF8FAFCFF, .surface = 0xFFFFFFFF,
	.border = 0xC6CFD9FF, .shadow = 0xD8DFE7FF, .line = 0xE1E7EEFF,
	.text = 0x454B53FF, .text2 = 0x8A929CFF,
	.glow = 0x36C5F7FF, .blue = 0x1BA4DCFF, .track = 0xDCE3EAFF,
	.green = 0x4DBB78FF, .red = 0xE5625CFF,
};
static bool dark_now;

void ui_set_dark(bool dark)
{
	dark_now = dark;
	ui_pal = dark ? PAL_DARK : PAL_LIGHT;
}

bool ui_is_dark(void) { return dark_now; }

Color ui_mix(Color a, Color b, float t)
{
	if (t < 0) t = 0;
	if (t > 1) t = 1;
	Color out = 0;
	for (int sh = 0; sh < 32; sh += 8) {
		float ca = (float)((a >> sh) & 0xFF), cb = (float)((b >> sh) & 0xFF);
		out |= (Color)(ca + (cb - ca) * t + 0.5f) << sh;
	}
	return out;
}

float ui_pulse(void)
{
	return 0.5f + 0.5f * sinf((float)ui_frame * 0.07f);
}

void ui_rrect(float x, float y, float w, float h, float r, Color c)
{
	if (w <= 0 || h <= 0) return;
	if (r > h / 2) r = h / 2;
	if (r > w / 2) r = w / 2;
	if (r < 0.5f) {
		gfx_rect(x, y, w, h, c);
		return;
	}
	gfx_rect(x + r, y, w - 2 * r, h, c);
	gfx_rect(x, y + r, r, h - 2 * r, c);
	gfx_rect(x + w - r, y + r, r, h - 2 * r, c);
	gfx_quarter(x + r, y + r, r, 0, c);
	gfx_quarter(x + w - r, y + r, r, 1, c);
	gfx_quarter(x + w - r, y + h - r, r, 2, c);
	gfx_quarter(x + r, y + h - r, r, 3, c);
}

void ui_bubble(float x, float y, float w, float h, float r, bool focus, Color under)
{
	if (focus) {
		// Soft halo: opaque layers blended towards the background, so overlapping pieces of a
		// rounded rect never double up the way translucent ones would.
		float p = 0.55f + 0.45f * ui_pulse();
		for (int i = 6; i >= 1; i--) {
			float t = 1.0f - (float)i / 7.0f;
			ui_rrect(x - i - 1, y - i - 1, w + 2 * i + 2, h + 2 * i + 2, r + i + 1, ui_mix(under, COL_GLOW, t * t * p));
		}
		ui_rrect(x - 2, y - 2, w + 4, h + 4, r + 2, COL_GLOW);
	} else {
		ui_rrect(x - 1, y + 1, w + 2, h + 2, r + 1, ui_mix(under, COL_SHADOW, 0.9f));
		ui_rrect(x - 1, y - 1, w + 2, h + 2, r + 1, COL_BORDER);
	}
	ui_rrect(x, y, w, h, r, COL_WHITE);
}

void ui_line(float x0, float y0, float x1, float y1, float th, Color c)
{
	float dx = x1 - x0, dy = y1 - y0;
	float len = sqrtf(dx * dx + dy * dy);
	if (len < 0.001f) return;
	float nx = -dy / len * th / 2, ny = dx / len * th / 2;
	gfx_tri(x0 + nx, y0 + ny, x0 - nx, y0 - ny, x1 + nx, y1 + ny, c);
	gfx_tri(x1 + nx, y1 + ny, x0 - nx, y0 - ny, x1 - nx, y1 - ny, c);
	gfx_circle(x0, y0, th / 2, c);
	gfx_circle(x1, y1, th / 2, c);
}

static bool measure_only; // lay text out without drawing anything

void ui_text(const char* s, float x, float y, float scale, Color c)
{
	if (s && s[0] && !measure_only) gfx_text(s, x, y, scale, c);
}

int ui_text_lines(const char* s, float w, float scale, int max_lines)
{
	measure_only = true;
	int n = ui_text_wrap(s, 0, 0, w, scale, 0, max_lines, 0, false);
	measure_only = false;
	return n;
}

void ui_text_center(const char* s, float cx, float y, float scale, Color c)
{
	ui_text(s, cx - gfx_text_width(s, scale) / 2, y, scale, c);
}

void ui_text_right(const char* s, float xr, float y, float scale, Color c)
{
	ui_text(s, xr - gfx_text_width(s, scale), y, scale, c);
}

static int utf8_len(unsigned char lead)
{
	if (lead < 0x80) return 1;
	if ((lead & 0xE0) == 0xC0) return 2;
	if ((lead & 0xF0) == 0xE0) return 3;
	if ((lead & 0xF8) == 0xF0) return 4;
	return 1;
}

#define ELLIPSIS "\xE2\x80\xA6"

// Copy as much of s as fits in maxw into out (adding an ellipsis if it was cut).
static void fit_string(const char* s, float maxw, float scale, char* out, size_t cap)
{
	if (gfx_text_width(s, scale) <= maxw) {
		snprintf(out, cap, "%s", s);
		if (strlen(s) < cap) return; // fitted and not truncated by the buffer
	}
	float limit = maxw - gfx_text_width(ELLIPSIS, scale);
	float acc = 0;
	size_t o = 0;
	const char* p = s;
	while (*p) {
		int n = utf8_len((unsigned char)*p);
		char ch[5] = {0};
		for (int i = 0; i < n && p[i]; i++) ch[i] = p[i];
		float cw = gfx_text_width(ch, scale);
		if (acc + cw > limit || o + (size_t)n + 4 >= cap) break;
		memcpy(out + o, ch, strlen(ch));
		o += strlen(ch);
		acc += cw;
		p += strlen(ch);
	}
	while (o > 0 && out[o - 1] == ' ') o--;
	memcpy(out + o, ELLIPSIS, 4); // includes the terminator
}

void ui_text_fit(const char* s, float x, float y, float maxw, float scale, Color c, int align)
{
	if (!s || !s[0]) return;
	char buf[320];
	fit_string(s, maxw, scale, buf, sizeof(buf));
	if (align == 1) ui_text_center(buf, x, y, scale, c);
	else if (align == 2) ui_text_right(buf, x, y, scale, c);
	else ui_text(buf, x, y, scale, c);
}

int ui_text_wrap(const char* s, float x, float y, float w, float scale, Color c, int max_lines, float line_h, bool center)
{
	if (!s) return 0;
	char line[320] = "";
	int lines = 0;
	const char* p = s;
	while (*p == ' ') p++;
	while (*p && lines < max_lines) {
		// Take the next word.
		const char* e = p;
		while (*e && *e != ' ') e++;
		size_t cur = strlen(line), wl = (size_t)(e - p);
		if (cur + wl + 2 >= sizeof(line)) wl = sizeof(line) - cur - 2;
		char cand[320];
		memcpy(cand, line, cur);
		size_t o = cur;
		if (cur) cand[o++] = ' ';
		memcpy(cand + o, p, wl);
		cand[o + wl] = 0;

		if (gfx_text_width(cand, scale) <= w || cur == 0) {
			if (lines == max_lines - 1 && gfx_text_width(cand, scale) > w) {
				// A single over-long word on the last line.
				ui_text_fit(cand, center ? x + w / 2 : x, y + lines * line_h, w, scale, c, center ? 1 : 0);
				return lines + 1;
			}
			strcpy(line, cand);
			p = e;
			while (*p == ' ') p++;
			if (gfx_text_width(line, scale) > w) {
				// Over-long word on its own line: cut it.
				ui_text_fit(line, center ? x + w / 2 : x, y + lines * line_h, w, scale, c, center ? 1 : 0);
				line[0] = 0;
				lines++;
			}
			continue;
		}
		// The word doesn't fit: flush the current line.
		if (lines == max_lines - 1) {
			// Last allowed line: show as much of the remainder as fits, with an ellipsis.
			char rest[320];
			snprintf(rest, sizeof(rest), "%s %s", line, p);
			ui_text_fit(rest, center ? x + w / 2 : x, y + lines * line_h, w, scale, c, center ? 1 : 0);
			return lines + 1;
		}
		if (center) ui_text_center(line, x + w / 2, y + lines * line_h, scale, c);
		else ui_text(line, x, y + lines * line_h, scale, c);
		lines++;
		line[0] = 0;
	}
	if (line[0] && lines < max_lines) {
		if (center) ui_text_center(line, x + w / 2, y + lines * line_h, scale, c);
		else ui_text(line, x, y + lines * line_h, scale, c);
		lines++;
	}
	return lines;
}

void ui_progress(float x, float y, float w, float h, float frac, bool knob, Color under)
{
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	ui_rrect(x, y, w, h, h / 2, COL_TRACK);
	float fw = w * frac;
	if (fw < h) fw = h;
	if (frac > 0 || knob) ui_rrect(x, y, fw, h, h / 2, COL_GLOW);
	if (knob) {
		float kx = x + w * frac, ky = y + h / 2;
		gfx_circle(kx, ky, h + 4, ui_mix(under, COL_GLOW, 0.35f));
		gfx_circle(kx, ky, h + 2, COL_GLOW);
		gfx_circle(kx, ky, h - 1, COL_WHITE);
	}
}

void ui_spinner(float cx, float cy, float r, Color under)
{
	int head = (ui_frame / 5) % 8;
	for (int i = 0; i < 8; i++) {
		float a = (float)i * 0.785398f;
		int age = (head - i + 8) % 8;
		gfx_circle(cx + cosf(a) * r, cy + sinf(a) * r, r * 0.24f, ui_mix(COL_GLOW, under, (float)age / 8.0f));
	}
}

void ui_icon(Icon ic, float cx, float cy, float s, Color c, Color under)
{
	switch (ic) {
	case IC_LIBRARY:
		for (int i = -1; i <= 1; i++) {
			float yy = cy + (float)i * 0.3f * s;
			gfx_circle(cx - 0.36f * s, yy, 0.08f * s, c);
			ui_rrect(cx - 0.2f * s, yy - 0.065f * s, 0.6f * s, 0.13f * s, 0.065f * s, c);
		}
		break;
	case IC_PLUS:
		ui_rrect(cx - 0.4f * s, cy - 0.09f * s, 0.8f * s, 0.18f * s, 0.09f * s, c);
		ui_rrect(cx - 0.09f * s, cy - 0.4f * s, 0.18f * s, 0.8f * s, 0.09f * s, c);
		break;
	case IC_DOWNLOAD:
		ui_rrect(cx - 0.09f * s, cy - 0.44f * s, 0.18f * s, 0.44f * s, 0.05f * s, c);
		gfx_tri(cx - 0.3f * s, cy - 0.08f * s, cx + 0.3f * s, cy - 0.08f * s, cx, cy + 0.24f * s, c);
		ui_rrect(cx - 0.38f * s, cy + 0.32f * s, 0.76f * s, 0.12f * s, 0.06f * s, c);
		break;
	case IC_PLAY:
		gfx_tri(cx - 0.26f * s, cy - 0.38f * s, cx - 0.26f * s, cy + 0.38f * s, cx + 0.42f * s, cy, c);
		break;
	case IC_PAUSE:
		ui_rrect(cx - 0.3f * s, cy - 0.36f * s, 0.22f * s, 0.72f * s, 0.06f * s, c);
		ui_rrect(cx + 0.08f * s, cy - 0.36f * s, 0.22f * s, 0.72f * s, 0.06f * s, c);
		break;
	case IC_SETTINGS: {
		static const float knob[3] = {-0.16f, 0.2f, -0.02f};
		for (int i = -1; i <= 1; i++) {
			float yy = cy + (float)i * 0.28f * s;
			ui_rrect(cx - 0.4f * s, yy - 0.04f * s, 0.8f * s, 0.08f * s, 0.04f * s, c);
			gfx_circle(cx + knob[i + 1] * s, yy, 0.12f * s, c);
			gfx_circle(cx + knob[i + 1] * s, yy, 0.055f * s, under);
		}
		break;
	}
	case IC_SEARCH:
		ui_line(cx + 0.12f * s, cy + 0.12f * s, cx + 0.38f * s, cy + 0.38f * s, 0.14f * s, c);
		gfx_circle(cx - 0.08f * s, cy - 0.08f * s, 0.32f * s, c);
		gfx_circle(cx - 0.08f * s, cy - 0.08f * s, 0.2f * s, under);
		break;
	case IC_LINK:
		ui_rrect(cx - 0.44f * s, cy - 0.17f * s, 0.5f * s, 0.34f * s, 0.17f * s, c);
		ui_rrect(cx - 0.34f * s, cy - 0.07f * s, 0.3f * s, 0.14f * s, 0.07f * s, under);
		ui_rrect(cx - 0.06f * s, cy - 0.17f * s, 0.5f * s, 0.34f * s, 0.17f * s, c);
		ui_rrect(cx + 0.04f * s, cy - 0.07f * s, 0.3f * s, 0.14f * s, 0.07f * s, under);
		ui_rrect(cx - 0.2f * s, cy - 0.05f * s, 0.4f * s, 0.1f * s, 0.05f * s, c);
		break;
	case IC_FILE:
		ui_rrect(cx - 0.3f * s, cy - 0.42f * s, 0.6f * s, 0.84f * s, 0.08f * s, c);
		for (int i = 0; i < 3; i++) ui_rrect(cx - 0.18f * s, cy - 0.22f * s + (float)i * 0.2f * s, 0.36f * s, 0.07f * s, 0.03f * s, under);
		break;
	case IC_BACK15:
		gfx_tri(cx - 0.02f * s, cy - 0.3f * s, cx - 0.02f * s, cy + 0.3f * s, cx - 0.46f * s, cy, c);
		gfx_tri(cx + 0.42f * s, cy - 0.3f * s, cx + 0.42f * s, cy + 0.3f * s, cx - 0.02f * s, cy, c);
		break;
	case IC_FWD30:
		gfx_tri(cx + 0.02f * s, cy - 0.3f * s, cx + 0.02f * s, cy + 0.3f * s, cx + 0.46f * s, cy, c);
		gfx_tri(cx - 0.42f * s, cy - 0.3f * s, cx - 0.42f * s, cy + 0.3f * s, cx + 0.02f * s, cy, c);
		break;
	case IC_CHECK:
		ui_line(cx - 0.34f * s, cy + 0.02f * s, cx - 0.1f * s, cy + 0.26f * s, 0.15f * s, c);
		ui_line(cx - 0.1f * s, cy + 0.26f * s, cx + 0.36f * s, cy - 0.24f * s, 0.15f * s, c);
		break;
	case IC_WAVES:
		// Signal ripples, a nod to StreetPass.
		gfx_circle(cx, cy, 0.5f * s, ui_mix(under, c, 0.3f));
		gfx_circle(cx, cy, 0.41f * s, under);
		gfx_circle(cx, cy, 0.33f * s, ui_mix(under, c, 0.6f));
		gfx_circle(cx, cy, 0.24f * s, under);
		gfx_circle(cx, cy, 0.15f * s, c);
		break;
	}
}
