// Host preview: runs the real StreetCast app logic (network, feeds, downloads, UI) with a
// CoreGraphics renderer and a simulated audio player, and writes screenshots of both screens.
#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#include <sys/stat.h>
#include "../source/app.h"
#include "../source/dl.h"
#include "../source/gfx.h"
#include "../source/net.h"
#include "../source/plat.h"
#include "../source/player.h"
#include "../source/store.h"
#include "../source/util.h"

static CGContextRef ctx; // current target
static CGContextRef top_ctx, bot_ctx;

static long verts, max_verts; static const char* max_name = "";
// Any draw call made while no frame is open (ctx == NULL) is a crash on the 3DS: trap it here.
static void need_frame(void) { if (!ctx) { fprintf(stderr, "FATAL: draw call outside a frame\n"); abort(); } }
static void setcol(Color c) { need_frame(); CGContextSetRGBFillColor(ctx, (c >> 24) / 255.0, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0); }
void gfx_rect(float x, float y, float w, float h, Color c) { verts += 6; setcol(c); CGContextSetShouldAntialias(ctx, false); CGContextFillRect(ctx, CGRectMake(x, y, w, h)); }
void gfx_rect_vgrad(float x, float y, float w, float h, Color t, Color b)
{
	for (int i = 0; i < (int)h; i++) {
		float f = (float)i / h; Color c = 0;
		for (int sh = 0; sh < 32; sh += 8) { float a = (t >> sh) & 255, bb = (b >> sh) & 255; c |= (Color)(a + (bb - a) * f) << sh; }
		gfx_rect(x, y + i, w, 1, c);
	}
}
void gfx_circle(float cx, float cy, float r, Color c) { verts += 3 * (r < 5 ? 12 : r < 16 ? 24 : 48); setcol(c); CGContextSetShouldAntialias(ctx, false); CGContextFillEllipseInRect(ctx, CGRectMake(cx - r, cy - r, 2 * r, 2 * r)); }
void gfx_quarter(float cx, float cy, float r, int q, Color c)
{
	static const float a0[4] = {M_PI, 1.5 * M_PI, 0, 0.5 * M_PI};
	verts += 3 * (r < 5 ? 3 : r < 16 ? 6 : 12);
	setcol(c); CGContextSetShouldAntialias(ctx, false); // the 3DS draws these without smoothing
	CGContextBeginPath(ctx); CGContextMoveToPoint(ctx, cx, cy); CGContextAddArc(ctx, cx, cy, r, a0[q & 3], a0[q & 3] + 0.5 * M_PI, 0); CGContextClosePath(ctx); CGContextFillPath(ctx);
}
void gfx_tri(float x0, float y0, float x1, float y1, float x2, float y2, Color c)
{
	verts += 3;
	setcol(c); CGContextSetShouldAntialias(ctx, true);
	CGContextBeginPath(ctx); CGContextMoveToPoint(ctx, x0, y0); CGContextAddLineToPoint(ctx, x1, y1); CGContextAddLineToPoint(ctx, x2, y2); CGContextClosePath(ctx); CGContextFillPath(ctx);
}
static CTLineRef make_line(const char* s, float scale, Color c)
{
	NSString* str = [NSString stringWithUTF8String:s];
	if (!str) str = @"?";
	// The 3DS system font has button glyphs in the private-use area; substitute look-alikes.
	NSDictionary* sub = @{@"": @"Ⓐ", @"": @"Ⓑ", @"": @"Ⓧ", @"": @"Ⓨ", @"": @"Ⓛ", @"": @"Ⓡ"};
	for (NSString* k in sub) str = [str stringByReplacingOccurrencesOfString:k withString:sub[k]];
	NSFont* font = [NSFont fontWithName:@"HelveticaNeue-Medium" size:30 * scale * 0.80];
	NSColor* col = [NSColor colorWithSRGBRed:(c >> 24) / 255.0 green:((c >> 16) & 255) / 255.0 blue:((c >> 8) & 255) / 255.0 alpha:(c & 255) / 255.0];
	NSAttributedString* as = [[NSAttributedString alloc] initWithString:str attributes:@{NSFontAttributeName: font, NSForegroundColorAttributeName: col}];
	return CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
}
void gfx_text(const char* s, float x, float y, float scale, Color c)
{
	need_frame();
	verts += 6 * (long)strlen(s);
	CTLineRef line = make_line(s, scale, c);
	CGContextSetShouldAntialias(ctx, true);
	CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1, -1));
	CGContextSetTextPosition(ctx, x, y + 25 * scale);
	CTLineDraw(line, ctx);
	CFRelease(line);
}
float gfx_text_width(const char* s, float scale)
{
	CTLineRef line = make_line(s, scale, 0xFF);
	double w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
	CFRelease(line);
	return (float)w * 1.04f; // the 3DS font runs slightly wider
}
int sys_battery(void) { return 4; }
bool sys_charging(void) { return false; }
int sys_wifi(void) { return 3; }
void sys_clock(int* h, int* m) { *h = 12; *m = 45; }
static const char* kb_answer = "";
bool sys_keyboard(const char* hint, const char* initial, char* out, int cap) { (void)hint; (void)initial; snprintf(out, cap, "%s", kb_answer); return out[0] != 0; }

// --- simulated player ---------------------------------------------------------------------------
static PlInfo sim;
bool player_init(void) { return true; }
bool player_exit(void) { return true; }
void player_play(const char* src, bool stream, const char* id, const char* title, const char* podcast, double start, int hint)
{
	printf("    [player] play %s stream=%d start=%.0f dur=%d\n      src=%.100s\n", title, stream, start, hint, src);
	memset(&sim, 0, sizeof(sim));
	sim.state = PL_PLAYING; sim.is_stream = stream; sim.pos = start; sim.dur = hint;
	str_copy(sim.id, sizeof(sim.id), id); str_copy(sim.title, sizeof(sim.title), title); str_copy(sim.podcast, sizeof(sim.podcast), podcast);
}
void player_stop(void) { sim.state = PL_STOPPED; }
void player_set_paused(bool p) { if (sim.state == PL_PLAYING || sim.state == PL_PAUSED) sim.state = p ? PL_PAUSED : PL_PLAYING; }
void player_seek(double s) { if (s < 0) s = 0; sim.pos = s; printf("    [player] seek %.0f\n", s); }
void player_get(PlInfo* o) { if (sim.state == PL_PLAYING) sim.pos += 1.0 / 60; *o = sim; }
bool player_active(void) { return sim.state == PL_PLAYING; }

// --- harness ------------------------------------------------------------------------------------
static CGContextRef make_ctx(int w, int h)
{
	CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	CGContextRef c = CGBitmapContextCreate(NULL, w, h, 8, 0, cs, kCGImageAlphaPremultipliedLast);
	CGContextTranslateCTM(c, 0, h); CGContextScaleCTM(c, 1, -1); // y-down like the 3DS
	return c;
}
static int shot_no;
static void snapshot(const char* name)
{
	verts = 0;
	ctx = top_ctx; app_draw_top();
	ctx = bot_ctx; app_draw_bottom();
	ctx = NULL;
	if (verts > max_verts) { max_verts = verts; max_name = name; }
	printf("  %-22s %6ld vertices\n", name, verts);
	int S = 2, W = 400 * S, H = 480 * S + 8;
	CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	CGContextRef out = CGBitmapContextCreate(NULL, W, H, 8, 0, cs, kCGImageAlphaPremultipliedLast);
	CGContextSetRGBFillColor(out, 0.1, 0.1, 0.12, 1); CGContextFillRect(out, CGRectMake(0, 0, W, H));
	CGContextSetInterpolationQuality(out, kCGInterpolationNone);
	CGImageRef ti = CGBitmapContextCreateImage(top_ctx), bi = CGBitmapContextCreateImage(bot_ctx);
	CGContextDrawImage(out, CGRectMake(0, 240 * S + 8, 400 * S, 240 * S), ti);
	CGContextDrawImage(out, CGRectMake(40 * S, 0, 320 * S, 240 * S), bi);
	CGImageRef oi = CGBitmapContextCreateImage(out);
	NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:oi];
	NSString* path = [NSString stringWithFormat:@"tests/out/shots/%02d_%s.png", ++shot_no, name];
	[[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
	printf("  shot %s\n", path.UTF8String);
	CGImageRelease(ti); CGImageRelease(bi); CGImageRelease(oi); CGContextRelease(out);
}
static void frame(unsigned keys)
{
	Input in = {0};
	in.down = in.repeat = in.held = keys;
	app_update(&in);
}
static void press(unsigned k) { frame(k); frame(0); }
static void tap(int x, int y)
{
	Input in = {0};
	in.down = in.held = K_TOUCH; in.tx = x; in.ty = y; app_update(&in);
	in.down = 0; app_update(&in);
	memset(&in, 0, sizeof(in)); in.up = K_TOUCH; app_update(&in);
	frame(0);
}
static void drag(int x, int y0, int y1)
{
	Input in = {0};
	in.down = in.held = K_TOUCH; in.tx = x; in.ty = y0; app_update(&in);
	in.down = 0;
	for (int i = 1; i <= 8; i++) { in.ty = y0 + (y1 - y0) * i / 8; app_update(&in); }
	memset(&in, 0, sizeof(in)); in.up = K_TOUCH; app_update(&in);
	for (int i = 0; i < 20; i++) frame(0);
}
static void idle(int frames) { for (int i = 0; i < frames; i++) frame(0); }
// Run frames until the foreground task finishes (or timeout).
static bool wait_task(int seconds)
{
	for (int i = 0; i < seconds * 50 && app_task_busy(); i++) { frame(0); plat_sleep_ms(20); }
	bool done = !app_task_busy();
	idle(3);
	return done;
}

int main(int argc, char** argv)
{
	@autoreleasepool {
		bool live = !(argc > 1 && !strcmp(argv[1], "--offline"));
		system("rm -rf tests/out/preview tests/out/shots; mkdir -p tests/out/preview tests/out/shots");
		top_ctx = make_ctx(400, 240); bot_ctx = make_ctx(320, 240);
		net_init(); store_init(); dl_init(); app_init(player_init());

		idle(30); snapshot("home");
		press(K_A); idle(10); snapshot("podcasts_empty");
		press(K_B); press(K_RIGHT); idle(10); snapshot("home_add_focused");
		press(K_A); idle(10); snapshot("add");
		if (!live) return 0;

		kb_answer = "darknet diaries";
		press(K_A); idle(8); snapshot("search_busy");
		printf("search finished: %d\n", wait_task(30)); snapshot("search_results");
		press(K_A); idle(6); snapshot("subscribe_busy");
		printf("subscribe finished: %d (subs=%d)\n", wait_task(60), nsubs); idle(5); snapshot("subscribed_toast");

		// A feed by URL, and a bad URL.
		press(K_B); idle(3);
		kb_answer = "https://changelog.com/podcast/feed";
		press(K_DOWN); press(K_A); printf("url subscribe finished: %d (subs=%d)\n", wait_task(60), nsubs);
		kb_answer = "https://example.com/";
		press(K_A); wait_task(30); idle(5); snapshot("bad_feed_error");
		printf("after bad feed subs=%d\n", nsubs);
		press(K_A);
		// feeds.txt import
		FILE* f = fopen("tests/out/preview/feeds.txt", "a");
		fputs("https://feeds.npr.org/510289/podcast.xml\n# comment\nfeed://feeds.feedburner.com/radiolab\n", f); fclose(f);
		press(K_DOWN); press(K_A); idle(4); snapshot("import_busy");
		printf("import finished: %d (subs=%d)\n", wait_task(90), nsubs);
		for (int i = 0; i < nsubs; i++) printf("  sub %d: [%d eps] %s <%s>\n", i, subs[i].count, subs[i].title, subs[i].url);

		press(K_B); idle(3); press(K_LEFT); press(K_A); idle(15); snapshot("podcasts");
		press(K_Y); idle(5); snapshot("podcast_menu"); press(K_B);
		press(K_X); idle(5); printf("update all finished: %d\n", wait_task(120)); idle(5); snapshot("updated");
		press(K_DOWN); press(K_A); idle(15); snapshot("episodes");
		// In-list search: filter, no matches, clear with B (must stay on the screen).
		kb_answer = "postgres"; press(K_Y); idle(10); snapshot("episodes_search");
		kb_answer = "OPEN source"; press(K_Y); idle(10); snapshot("episodes_search_two_words");
		kb_answer = "zzzqqq"; press(K_Y); idle(5); snapshot("episodes_search_none");
		press(K_A); press(K_DOWN); idle(3); // must be harmless on an empty list
		press(K_B); idle(5); snapshot("episodes_search_cleared");
		press(K_DOWN); press(K_DOWN); press(K_DOWN); press(K_DOWN); press(K_DOWN); idle(15); snapshot("episodes_scrolled");
		drag(160, 180, 60); snapshot("episodes_dragged");
		press(K_A); idle(5); snapshot("episode_menu");
		press(K_DOWN); press(K_A); // Download
		for (int i = 0; i < 150; i++) { frame(0); plat_sleep_ms(20); }
		snapshot("episode_downloading");
		press(K_B); idle(2); press(K_B); idle(2); press(K_B); idle(2);
		press(K_DOWN); press(K_A); idle(10); snapshot("downloads_active");
		// wait for the download to finish (bounded)
		for (int i = 0; i < 3000 && dl_busy(); i++) { frame(0); plat_sleep_ms(20); }
		idle(10); printf("download done: busy=%d ndls=%d\n", dl_busy(), ndls);
		snapshot("downloads_done");
		kb_answer = "changelog"; press(K_Y); idle(5); snapshot("downloads_search");
		kb_answer = "nothing like this"; press(K_Y); idle(5); snapshot("downloads_search_none");
		press(K_B); idle(5); snapshot("downloads_search_cleared");
		if (ndls) {
			char path[128]; struct stat st; path_episode(dls[0].id, path, sizeof(path));
			printf("  file %s: %lld bytes (registry %lld)\n", path, stat(path, &st) == 0 ? (long long)st.st_size : -1LL, dls[0].size);
		}
		press(K_A); idle(5); snapshot("download_menu");
		press(K_A); idle(20); snapshot("player_file");
		press(K_R); press(K_R); idle(600); press(K_A); idle(10); snapshot("player_paused");
		press(K_B); idle(5); press(K_B); idle(5); snapshot("home_with_mini");
		press(K_UP); press(K_LEFT); press(K_A); idle(5); press(K_A); idle(10);
		press(K_A); idle(5); snapshot("resume_menu");
		press(K_B);
		press(K_UP); press(K_A); idle(5); press(K_A); idle(30); snapshot("player_stream");
		tap(250, 105); idle(5); snapshot("player_scrubbed");
		press(K_X); idle(5); snapshot("after_stop");
		press(K_B); press(K_B); idle(3);
		press(K_DOWN); press(K_DOWN); press(K_A); idle(10); snapshot("settings");
		press(K_DOWN); press(K_DOWN); press(K_DOWN); press(K_A); idle(5); snapshot("about");
		press(K_A); press(K_B);
		// quit confirm while a download runs
		press(K_UP); press(K_UP); press(K_A); idle(5); press(K_A); idle(5); press(K_DOWN); press(K_DOWN); press(K_A); idle(3); press(K_A); idle(3);
		press(K_DOWN); press(K_A); idle(40);
		// L / R: a tap skips time; holding L 3 s restarts; holding R 5 s plays the next episode.
		printf("-- shoulder keys (expect: seek +30, then play of the next episode; seek -15, then seek 0)\n");
		for (int i = 0; i < 330; i++) { Input h = {0}; h.held = K_R; if (i == 0) h.down = K_R; app_update(&h); plat_sleep_ms(16); }
		frame(0); snapshot("hold_r_next");
		for (int i = 0; i < 120; i++) { Input h = {0}; h.held = K_R; if (i == 0) h.down = K_R; app_update(&h); plat_sleep_ms(16); } // 2 s: too short
		frame(0);
		for (int i = 0; i < 200; i++) { Input h = {0}; h.held = K_L; if (i == 0) h.down = K_L; app_update(&h); plat_sleep_ms(16); }
		frame(0); snapshot("hold_l_restart");
		// START with a download running asks first; START again (confirm box open) must quit.
		Episode big; memset(&big, 0, sizeof(big));
		strcpy(big.id, "00000000000000aa"); strcpy(big.title, "Big test episode");
		strcpy(big.url, "https://cdn.changelog.com/uploads/podcast/689/the-changelog-689.mp3");
		dl_enqueue(&big, "Test"); idle(30);
		Input st = {0}; st.down = st.held = K_START;
		bool r1 = app_update(&st); frame(0); idle(5); snapshot("quit_confirm");
		press(K_A); // "Keep going"
		bool r1b = app_update(&st); frame(0);
		bool r2 = app_update(&st);
		printf("START with download: 1st keeps running=%d (want 1), after declining 1st again=%d (want 1), 2nd=%d (want 0)\n", r1, r1b, r2);
		app_set_closing(); snapshot("closing");
		printf("MAX vertices in a frame: %ld (%s); budget %d\n", max_verts, max_name, 16384 * 6);
		app_shutdown(); dl_exit(); store_flush(); net_exit();
		printf("state files:\n"); system("ls -la tests/out/preview tests/out/preview/episodes tests/out/preview/cache; cat tests/out/preview/state.tsv tests/out/preview/settings.txt 2>/dev/null; cut -c1-150 tests/out/preview/subs.tsv tests/out/preview/downloads.tsv");
	}
	return 0;
}
