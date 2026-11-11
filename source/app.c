#include "app.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dl.h"
#include "feed.h"
#include "net.h"
#include "plat.h"
#include "player.h"
#include "store.h"
#include "ui.h"
#include "util.h"

#define APP_VERSION "1.0"

typedef enum { SCR_HOME, SCR_PODCASTS, SCR_ADD, SCR_SEARCH, SCR_EPISODES, SCR_DOWNLOADS, SCR_PLAYER, SCR_SETTINGS, SCR_COUNT } Screen;

// Bottom-screen layout.
#define HEADER_H 28
#define FOOTER_Y 216
#define LIST_TOP 37
#define LIST_VIEW 172
#define ROW_H 38
#define ROW_PITCH 44
#define ROW_X 10
#define ROW_W 294

static Screen stack[12];
static int depth;
static struct { int sel; float scroll, target; } ls[SCR_COUNT];
static bool want_quit, sound_ok;
static Input in;

// Touch gesture tracking.
static bool t_active, t_drag, t_tap;
static int t_sx, t_sy, t_lx, t_ly, tap_x, tap_y;
static float t_scroll0;

// Data shown by the screens.
static Feed cur_feed;
static int cur_sub = -1;
static SearchResult results[25];
static int nresults;
static PlInfo pi;
static PlState last_state = PL_STOPPED;
static char last_id[17];
static int player_focus = 1;
static bool scrubbing;
static float scrub_frac;

static char toast_text[160];
static int toast_until;
static bool closing; // START was pressed: show "Closing" while threads wind down

// In-list search: the Episodes and Downloads screens show a filtered view of their data.
static char ep_filter[64], dl_filter[64];
static int ep_view[MAX_FEED_ITEMS], ep_nview; // indices into cur_feed.eps
static int dl_view[MAX_DLS], dl_nview;        // indices into dls

// ------------------------------------------------------------------------------------------------
// Small helpers
// ------------------------------------------------------------------------------------------------

static Screen cur_screen(void) { return stack[depth]; }

static void push(Screen s)
{
	if (cur_screen() == s) return;
	if (depth < 11) stack[++depth] = s;
}

static void back(void)
{
	if (depth > 0) depth--;
}

static void toast(const char* fmt, const char* arg)
{
	char a[100]; // keep the argument short and cut on a character boundary
	str_copy(a, sizeof(a), arg ? arg : "");
	snprintf(toast_text, sizeof(toast_text), fmt, a);
	toast_until = ui_frame + 210;
}

static bool tapped(float x, float y, float w, float h)
{
	return t_tap && tap_x >= x && tap_x < x + w && tap_y >= y && tap_y < y + h;
}

static bool has_media(void)
{
	return pi.state == PL_PLAYING || pi.state == PL_PAUSED || pi.state == PL_LOADING;
}

static void save_position(void)
{
	player_get(&pi);
	if (pi.id[0] && has_media()) st_set_pos(pi.id, pi.pos > 10 ? (int)pi.pos : 0);
}

static bool ci_contains(const char* hay, const char* needle)
{
	size_t n = strlen(needle);
	if (!n) return true;
	for (; *hay; hay++)
		if (!strncasecmp(hay, needle, n)) return true;
	return false;
}

// Every word of the filter must appear in a or b (case-insensitive). An empty filter matches.
static bool filter_match(const char* filter, const char* a, const char* b)
{
	char buf[64];
	str_copy(buf, sizeof(buf), filter);
	for (char* w = strtok(buf, " "); w; w = strtok(NULL, " "))
		if (!ci_contains(a, w) && !ci_contains(b, w)) return false;
	return true;
}

static void ep_view_rebuild(void)
{
	ep_nview = 0;
	for (int i = 0; i < cur_feed.count && ep_nview < MAX_FEED_ITEMS; i++)
		if (filter_match(ep_filter, cur_feed.eps[i].title, cur_feed.eps[i].desc)) ep_view[ep_nview++] = i;
}

static void dl_view_rebuild(void)
{
	dl_nview = 0;
	for (int i = 0; i < ndls; i++)
		if (filter_match(dl_filter, dls[i].title, dls[i].podcast)) dl_view[dl_nview++] = i;
}

// Ask for search words and apply them to a list. Returns true if the filter changed.
static bool ask_filter(char* filter, size_t cap, Screen scr)
{
	char buf[64];
	if (!sys_keyboard("Search this list", filter, buf, sizeof(buf))) return false;
	str_sanitize(buf);
	str_copy(filter, cap, buf);
	ls[scr].sel = 0;
	ls[scr].scroll = ls[scr].target = 0;
	return true;
}

static void clear_filter(char* filter, Screen scr)
{
	filter[0] = 0;
	ls[scr].sel = 0;
	ls[scr].scroll = ls[scr].target = 0;
}

// ------------------------------------------------------------------------------------------------
// Modal menu / message box
// ------------------------------------------------------------------------------------------------

#define MENU_MAX 6
static struct {
	bool open;
	char title[96];
	char body[220];
	char items[MENU_MAX][44];
	int ids[MENU_MAX];
	int n, sel;
	void (*on)(int id);
} menu;

static void menu_begin(const char* title, const char* body, void (*on)(int))
{
	memset(&menu, 0, sizeof(menu));
	str_copy(menu.title, sizeof(menu.title), title);
	if (body) str_copy(menu.body, sizeof(menu.body), body);
	menu.on = on;
	menu.open = true;
}

static void menu_add(const char* label, int id)
{
	if (menu.n == MENU_MAX) return;
	str_copy(menu.items[menu.n], sizeof(menu.items[0]), label);
	menu.ids[menu.n++] = id;
}

static void msg(const char* title, const char* body)
{
	menu_begin(title, body, NULL);
	menu_add("OK", 0);
}

static void menu_layout(float* px, float* py, float* pw, float* ph, int* body_lines)
{
	// This also runs during input handling, outside a GPU frame, so it must only measure.
	*body_lines = menu.body[0] ? ui_text_lines(menu.body, 252, TS_SMALL, 5) : 0;
	*pw = 280;
	*ph = 12 + 20 + (*body_lines ? *body_lines * 14 + 8 : 2) + menu.n * 34 + 6;
	*px = (BOT_W - *pw) / 2;
	*py = (SCREEN_H - *ph) / 2;
}

static void menu_input(void)
{
	float px, py, pw, ph;
	int bl;
	menu_layout(&px, &py, &pw, &ph, &bl);
	float iy = py + 12 + 20 + (bl ? bl * 14 + 8 : 2);
	int chosen = -1;
	if (in.repeat & K_DOWN) menu.sel = (menu.sel + 1) % menu.n;
	if (in.repeat & K_UP) menu.sel = (menu.sel + menu.n - 1) % menu.n;
	if (in.down & K_A) chosen = menu.sel;
	for (int i = 0; i < menu.n; i++)
		if (tapped(px + 14, iy + i * 34, pw - 28, 28)) chosen = i;
	if (in.down & K_B) {
		menu.open = false;
		return;
	}
	if (chosen >= 0) {
		void (*on)(int) = menu.on;
		int id = menu.ids[chosen];
		menu.open = false;
		if (on) on(id);
	}
}

static void menu_draw(void)
{
	float px, py, pw, ph;
	int bl;
	menu_layout(&px, &py, &pw, &ph, &bl);
	gfx_rect(0, 0, BOT_W, SCREEN_H, 0x26323F8C);
	ui_rrect(px - 1, py - 1, pw + 2, ph + 2, 15, COL_BORDER);
	ui_rrect(px, py, pw, ph, 14, COL_BG_TOP);
	ui_text_fit(menu.title, BOT_W / 2, py + 10, pw - 28, TS_BODY, COL_TEXT, 1);
	float y = py + 12 + 20;
	if (bl) {
		ui_text_wrap(menu.body, px + 14, y, 252, TS_SMALL, COL_TEXT2, 5, 14, true);
		y += bl * 14 + 8;
	} else {
		y += 2;
	}
	for (int i = 0; i < menu.n; i++) {
		bool f = i == menu.sel;
		ui_bubble(px + 14, y + i * 34, pw - 28, 28, 14, f, COL_BG_TOP);
		ui_text_fit(menu.items[i], BOT_W / 2, y + i * 34 + 6, pw - 48, TS_BODY, f ? COL_BLUE : COL_TEXT, 1);
	}
}

// ------------------------------------------------------------------------------------------------
// Foreground network tasks (run on a worker thread behind a "busy" overlay)
// ------------------------------------------------------------------------------------------------

static struct {
	bool running;
	volatile bool done;
	PlatThread thread;
	NetProgress prog;
	char label[80];
	void (*work)(void);
	void (*finish)(void);
} task;

static void task_thread(void* arg)
{
	(void)arg;
	task.work();
	task.done = true;
}

static void task_start(const char* label, void (*work)(void), void (*finish)(void))
{
	if (task.running) return;
	memset(&task.prog, 0, sizeof(task.prog));
	str_copy(task.label, sizeof(task.label), label);
	task.work = work;
	task.finish = finish;
	task.done = false;
	if (!plat_thread_start(&task.thread, task_thread, NULL, 96 * 1024, 1)) {
		msg("Out of memory", "Couldn't start the network task.");
		return;
	}
	task.running = true;
}

bool app_task_busy(void) { return task.running; }

// --- refresh / subscribe -------------------------------------------------------------------------

static struct {
	int n;
	char url[MAX_SUBS][512];
	char id[MAX_SUBS][17];
	char title[MAX_SUBS][128];
	int count[MAX_SUBS];
	bool ok[MAX_SUBS];
	char err[160];
	bool is_subscribe; // a brand-new subscription: undo it if the feed can't be loaded
	bool open_after;
} rf;

static void open_podcast(int i, bool allow_fetch);

static bool fetch_feed(const char* url, const char* id, char* title, int* count, char* err, size_t errcap)
{
	char tmp[128], cache[128];
	path_feed_tmp(tmp, sizeof(tmp));
	task.prog.now = task.prog.total = 0;
	if (net_download(url, tmp, false, true, &task.prog, err, errcap) != 0) {
		remove(tmp);
		return false;
	}
	Feed f;
	int rc = feed_parse_file(tmp, &f, MAX_FEED_ITEMS);
	remove(tmp);
	if (rc == -2 || (rc == 0 && f.count == 0 && !f.title[0])) {
		if (rc == 0) feed_free(&f);
		snprintf(err, errcap, "That address doesn't look like a podcast feed.");
		return false;
	}
	if (rc != 0) {
		snprintf(err, errcap, "Couldn't read the feed from the SD card.");
		return false;
	}
	path_feed_cache(id, cache, sizeof(cache));
	bool ok = feed_save_cache(&f, cache) == 0;
	if (!ok) snprintf(err, errcap, "Couldn't save to the SD card (is it full?).");
	str_copy(title, 128, f.title);
	*count = f.count;
	feed_free(&f);
	return ok;
}

static void work_refresh(void)
{
	for (int k = 0; k < rf.n; k++) {
		if (task.prog.cancel) break;
		if (rf.n > 1) snprintf(task.label, sizeof(task.label), "Updating %d of %d", k + 1, rf.n);
		rf.ok[k] = fetch_feed(rf.url[k], rf.id[k], rf.title[k], &rf.count[k], rf.err, sizeof(rf.err));
	}
}

static int sub_by_id(const char* id)
{
	for (int i = 0; i < nsubs; i++)
		if (!strcmp(subs[i].id, id)) return i;
	return -1;
}

static void finish_refresh(void)
{
	int failed = 0, first_ok = -1;
	bool cancelled = task.prog.cancel;
	for (int k = 0; k < rf.n; k++) {
		int i = sub_by_id(rf.id[k]);
		if (i < 0) continue;
		if (rf.ok[k]) {
			if (rf.title[k][0]) str_copy(subs[i].title, sizeof(subs[i].title), rf.title[k]);
			subs[i].count = rf.count[k];
			if (first_ok < 0) first_ok = i;
		} else {
			failed++;
		}
	}
	subs_touch();

	if (rf.is_subscribe) {
		// Remove subscriptions whose feed never loaded.
		for (int k = 0; k < rf.n; k++) {
			int i = sub_by_id(rf.id[k]);
			if (i >= 0 && !rf.ok[k] && subs[i].count < 0) subs_remove(i);
		}
	}
	store_flush();

	if (failed && cancelled) toast("Cancelled", NULL);
	else if (failed && rf.is_subscribe) msg(rf.n > 1 ? "Some feeds couldn't be added" : "Couldn't subscribe", rf.err);
	else if (failed) msg(rf.n > 1 ? "Some podcasts couldn't update" : "Couldn't update", rf.err);
	else if (rf.is_subscribe && rf.n == 1 && first_ok >= 0) toast("Subscribed to %s", subs[first_ok].title);
	else if (rf.is_subscribe) toast("Podcasts added", NULL);
	else if (!rf.open_after) toast("Up to date", NULL);

	if (rf.open_after && first_ok >= 0) {
		open_podcast(first_ok, false);
	} else if (cur_screen() == SCR_EPISODES && cur_sub >= 0 && cur_sub < nsubs) {
		// Reload the list we're looking at.
		char path[128];
		Feed f;
		path_feed_cache(subs[cur_sub].id, path, sizeof(path));
		if (feed_load_cache(&f, path) == 0) {
			feed_free(&cur_feed);
			cur_feed = f;
			ep_view_rebuild();
		}
	}
}

// Refresh the given subscriptions (indices). n < 0 means all.
static void start_refresh(const int* idx, int n, bool is_subscribe, bool open_after)
{
	memset(&rf, 0, sizeof(rf));
	if (n < 0) {
		for (int i = 0; i < nsubs; i++) {
			str_copy(rf.url[rf.n], 512, subs[i].url);
			str_copy(rf.id[rf.n++], 17, subs[i].id);
		}
	} else {
		for (int k = 0; k < n; k++) {
			str_copy(rf.url[rf.n], 512, subs[idx[k]].url);
			str_copy(rf.id[rf.n++], 17, subs[idx[k]].id);
		}
	}
	if (rf.n == 0) return;
	rf.is_subscribe = is_subscribe;
	rf.open_after = open_after;
	task_start(is_subscribe ? "Loading podcast" : "Updating", work_refresh, finish_refresh);
}

static void subscribe(const char* url)
{
	int before = nsubs;
	int i = subs_add(url, NULL);
	if (i < 0) {
		msg("Can't add that", nsubs >= MAX_SUBS ? "You've reached the subscription limit." : "That doesn't look like a web address.");
		return;
	}
	if (i < before && subs[i].count >= 0) {
		toast("Already subscribed", NULL);
		return;
	}
	start_refresh(&i, 1, true, false);
}

// --- search --------------------------------------------------------------------------------------

static char search_term[96];
static char search_err[160];

static void work_search(void)
{
	char enc[300], url[400];
	url_encode(search_term, enc, sizeof(enc));
	snprintf(url, sizeof(url), "https://itunes.apple.com/search?media=podcast&limit=25&term=%s", enc);
	char* body = NULL;
	nresults = 0;
	search_err[0] = 0;
	if (net_fetch_mem(url, &body, 1024 * 1024, &task.prog, search_err, sizeof(search_err)) == 0) {
		nresults = search_parse(body, results, 25);
		free(body);
	}
}

static void finish_search(void)
{
	if (task.prog.cancel) return;
	if (search_err[0]) msg("Search failed", search_err);
	else if (nresults == 0) msg("No podcasts found", "Try a different name, or add the feed address directly.");
	else {
		ls[SCR_SEARCH].sel = 0;
		ls[SCR_SEARCH].scroll = ls[SCR_SEARCH].target = 0;
		push(SCR_SEARCH);
	}
}

// ------------------------------------------------------------------------------------------------
// Actions
// ------------------------------------------------------------------------------------------------

static void open_podcast(int i, bool allow_fetch)
{
	if (i < 0 || i >= nsubs) return;
	char path[128];
	Feed f;
	path_feed_cache(subs[i].id, path, sizeof(path));
	if (feed_load_cache(&f, path) == 0) {
		feed_free(&cur_feed);
		cur_feed = f;
		cur_sub = i;
		ep_filter[0] = 0;
		ep_view_rebuild();
		ls[SCR_EPISODES].sel = 0;
		ls[SCR_EPISODES].scroll = ls[SCR_EPISODES].target = 0;
		push(SCR_EPISODES);
	} else if (allow_fetch) {
		start_refresh(&i, 1, false, true);
	} else {
		msg("Couldn't open", "The episode list couldn't be loaded.");
	}
}

static void start_play(const char* id, const char* title, const char* podcast, const char* url, int duration, bool from_start)
{
	if (!sound_ok) {
		msg("Sound isn't available", "DSP firmware is missing. Open the Rosalina menu (L+Down+Select), choose Miscellaneous options > Dump DSP firmware, then restart StreetCast.");
		return;
	}
	save_position();
	double start = from_start ? 0 : st_pos(id);
	if (dls_find(id) >= 0) {
		char path[128];
		path_episode(id, path, sizeof(path));
		player_play(path, false, id, title, podcast, start, duration);
	} else if (url && url[0]) {
		player_play(url, true, id, title, podcast, start, duration);
	} else {
		msg("Can't play", "This episode isn't downloaded.");
		return;
	}
	player_get(&pi);
	last_state = pi.state;
	str_copy(last_id, sizeof(last_id), pi.id);
	player_focus = 1;
	push(SCR_PLAYER);
}

static bool playing_file(const char* id)
{
	return has_media() && !pi.is_stream && !strcmp(pi.id, id);
}

static void delete_download(const char* id)
{
	if (playing_file(id)) {
		msg("Still playing", "Stop playback before deleting this episode.");
		return;
	}
	dls_remove(dls_find(id));
	store_flush();
	toast("Download deleted", NULL);
}

enum { M_PLAY, M_PLAY_START, M_DOWNLOAD, M_CANCEL_DL, M_RETRY_DL, M_DELETE, M_MARK, M_REFRESH, M_UNSUB, M_YES, M_NO };

// The episode a menu is about.
static Episode m_ep;
static char m_podcast[128];
static int m_sub;

static void on_episode_menu(int id)
{
	switch (id) {
	case M_PLAY: start_play(m_ep.id, m_ep.title, m_podcast, m_ep.url, m_ep.duration, false); break;
	case M_PLAY_START: start_play(m_ep.id, m_ep.title, m_podcast, m_ep.url, m_ep.duration, true); break;
	case M_DOWNLOAD:
		if (dl_enqueue(&m_ep, m_podcast)) toast("Downloading %s", m_ep.title);
		else msg("Download queue is full", "Wait for some downloads to finish first.");
		break;
	case M_RETRY_DL: dl_retry(m_ep.id); break;
	case M_CANCEL_DL: dl_cancel(m_ep.id); toast("Download cancelled", NULL); break;
	case M_DELETE: delete_download(m_ep.id); break;
	case M_MARK: {
		bool played = !st_played(m_ep.id);
		st_set_played(m_ep.id, played);
		if (played) st_set_pos(m_ep.id, 0);
		store_flush();
		break;
	}
	}
}

static void episode_menu(const Episode* ep, const char* podcast)
{
	m_ep = *ep;
	str_copy(m_podcast, sizeof(m_podcast), podcast);
	bool downloaded = dls_find(ep->id) >= 0;
	if (ep->kind == EP_UNSUPPORTED && !downloaded) {
		msg("Not an MP3", "This episode is in a format StreetCast can't play (it only supports MP3).");
		return;
	}
	char label[44], t[16];
	int pos = st_pos(ep->id);
	DlJob job;
	bool queued = dl_find(ep->id, &job) >= 0;
	menu_begin(ep->title, NULL, on_episode_menu);
	if (pos > 0) {
		fmt_time(pos, t, sizeof(t));
		snprintf(label, sizeof(label), "Resume from %s", t);
		menu_add(label, M_PLAY);
		menu_add(downloaded ? "Play from start" : "Stream from start", M_PLAY_START);
	} else {
		menu_add(downloaded ? "Play" : "Stream now", M_PLAY);
	}
	if (downloaded) menu_add("Delete download", M_DELETE);
	else if (queued && job.state == DJ_FAILED) {
		menu_add("Retry download", M_RETRY_DL);
		menu_add("Remove from queue", M_CANCEL_DL);
	} else if (queued) menu_add("Cancel download", M_CANCEL_DL);
	else menu_add("Download", M_DOWNLOAD);
	menu_add(st_played(ep->id) ? "Mark as unplayed" : "Mark as played", M_MARK);
}

static void on_unsub_confirm(int id)
{
	if (id == M_YES && m_sub >= 0 && m_sub < nsubs) {
		subs_remove(m_sub);
		store_flush();
		toast("Unsubscribed", NULL);
	}
}

static void on_podcast_menu(int id)
{
	if (id == M_REFRESH) start_refresh(&m_sub, 1, false, false);
	else if (id == M_UNSUB) {
		menu_begin("Unsubscribe?", "Downloaded episodes are kept.", on_unsub_confirm);
		menu_add("Unsubscribe", M_YES);
		menu_add("Cancel", M_NO);
		menu.sel = 1;
	}
}

static void on_quit_confirm(int id)
{
	if (id == M_YES) want_quit = true;
}

// ------------------------------------------------------------------------------------------------
// List widget
// ------------------------------------------------------------------------------------------------

static float list_max_scroll(int count)
{
	float m = (float)count * ROW_PITCH - (ROW_PITCH - ROW_H) - LIST_VIEW;
	return m > 0 ? m : 0;
}

static void list_show_sel(Screen s)
{
	float top = (float)ls[s].sel * ROW_PITCH - ls[s].target;
	if (top < 0) ls[s].target = (float)ls[s].sel * ROW_PITCH;
	else if (top + ROW_H > LIST_VIEW) ls[s].target = (float)ls[s].sel * ROW_PITCH + ROW_H - LIST_VIEW;
}

// Handles navigation; returns the activated row or -1.
static int list_input(Screen s, int count)
{
	if (count <= 0) {
		ls[s].sel = 0;
		ls[s].scroll = ls[s].target = 0;
		return -1;
	}
	if (ls[s].sel >= count) ls[s].sel = count - 1;
	int old = ls[s].sel;
	if (in.repeat & K_DOWN) {
		if (ls[s].sel + 1 < count) ls[s].sel++;
		else if (in.down & K_DOWN) ls[s].sel = 0;
	}
	if (in.repeat & K_UP) {
		if (ls[s].sel > 0) ls[s].sel--;
		else if (in.down & K_UP) ls[s].sel = count - 1;
	}
	if (in.repeat & K_RIGHT) ls[s].sel = ls[s].sel + 4 < count ? ls[s].sel + 4 : count - 1;
	if (in.repeat & K_LEFT) ls[s].sel = ls[s].sel >= 4 ? ls[s].sel - 4 : 0;
	if (ls[s].sel != old) list_show_sel(s);

	float maxs = list_max_scroll(count);
	if (t_active && t_drag && t_sy >= HEADER_H && t_sy < FOOTER_Y) {
		ls[s].target = t_scroll0 - (float)(t_ly - t_sy);
		if (ls[s].target < 0) ls[s].target = 0;
		if (ls[s].target > maxs) ls[s].target = maxs;
		ls[s].scroll = ls[s].target;
	}
	if (ls[s].target > maxs) ls[s].target = maxs;

	if (t_tap && tap_y >= HEADER_H && tap_y < FOOTER_Y) {
		float rel = (float)(tap_y - LIST_TOP) + ls[s].scroll;
		int idx = (int)floorf(rel / ROW_PITCH);
		if (rel >= 0 && idx < count && rel - (float)idx * ROW_PITCH < ROW_H) {
			ls[s].sel = idx;
			list_show_sel(s);
			return idx;
		}
	}
	if (in.down & K_A) return ls[s].sel;
	return -1;
}

// Draws the row background and returns its y, or a negative number if it's off-screen.
static float row_begin(Screen s, int i)
{
	float y = LIST_TOP + (float)i * ROW_PITCH - ls[s].scroll;
	if (y < HEADER_H - ROW_H - 8 || y > FOOTER_Y + 8) return -1000;
	ui_bubble(ROW_X, y, ROW_W, ROW_H, 13, i == ls[s].sel, COL_BG);
	return y;
}

static void list_scrollbar(Screen s, int count)
{
	float maxs = list_max_scroll(count);
	if (maxs <= 0) return;
	float track = FOOTER_Y - HEADER_H - 12, total = maxs + LIST_VIEW;
	float h = track * LIST_VIEW / total;
	if (h < 16) h = 16;
	float y = HEADER_H + 6 + (track - h) * (ls[s].scroll / maxs);
	ui_rrect(311, HEADER_H + 6, 4, track, 2, COL_LINE);
	ui_rrect(311, y, 4, h, 2, COL_BORDER);
}

static void empty_state(Icon ic, const char* title, const char* hint)
{
	ui_icon(ic, BOT_W / 2, 92, 40, COL_BORDER, COL_BG);
	ui_text_center(title, BOT_W / 2, 124, TS_BODY, COL_TEXT);
	ui_text_wrap(hint, 30, 146, 260, TS_SMALL, COL_TEXT2, 3, 14, true);
}

// ------------------------------------------------------------------------------------------------
// Screens: input
// ------------------------------------------------------------------------------------------------

static const struct { float x, y, w, h; } home_btn[5] = {
	{12, 12, 143, 80}, {165, 12, 143, 80}, {12, 100, 143, 80}, {165, 100, 143, 80}, {100, 187, 120, 23},
};

static void home_activate(int i)
{
	switch (i) {
	case 0: push(SCR_PODCASTS); break;
	case 1: push(SCR_ADD); break;
	case 2: push(SCR_DOWNLOADS); break;
	case 3: push(SCR_PLAYER); break;
	case 4: push(SCR_SETTINGS); break;
	}
}

static void home_input(void)
{
	int* sel = &ls[SCR_HOME].sel;
	if (in.down & K_RIGHT) { if (*sel == 0 || *sel == 2) (*sel)++; }
	if (in.down & K_LEFT) { if (*sel == 1 || *sel == 3) (*sel)--; }
	if (in.down & K_DOWN) { if (*sel < 2) *sel += 2; else *sel = 4; }
	if (in.down & K_UP) { if (*sel == 4) *sel = 2; else if (*sel >= 2) *sel -= 2; }
	for (int i = 0; i < 5; i++)
		if (tapped(home_btn[i].x, home_btn[i].y, home_btn[i].w, home_btn[i].h)) {
			*sel = i;
			home_activate(i);
			return;
		}
	if (in.down & K_A) home_activate(*sel);
}

static void podcasts_input(void)
{
	int act = list_input(SCR_PODCASTS, nsubs);
	if (nsubs == 0) {
		if (in.down & K_A) push(SCR_ADD);
		return;
	}
	if (act >= 0) open_podcast(act, true);
	else if (in.down & K_X) start_refresh(NULL, -1, false, false);
	else if (in.down & K_Y) {
		m_sub = ls[SCR_PODCASTS].sel;
		menu_begin(subs[m_sub].title[0] ? subs[m_sub].title : subs[m_sub].url, NULL, on_podcast_menu);
		menu_add("Check for new episodes", M_REFRESH);
		menu_add("Unsubscribe", M_UNSUB);
	}
}

static void add_input(void)
{
	int act = list_input(SCR_ADD, 3);
	char buf[512];
	if (act == 0) {
		if (sys_keyboard("Podcast name", "", buf, 90)) {
			str_copy(search_term, sizeof(search_term), buf);
			str_sanitize(search_term);
			if (search_term[0]) task_start("Searching", work_search, finish_search);
		}
	} else if (act == 1) {
		if (sys_keyboard("Feed address (RSS URL)", "https://", buf, sizeof(buf))) {
			str_sanitize(buf);
			if (buf[0] && strcmp(buf, "https://")) subscribe(buf);
		}
	} else if (act == 2) {
		int before = nsubs;
		int added = subs_import_txt();
		if (added < 0) msg("feeds.txt not found", "Put one feed address per line in /3ds/StreetCast/feeds.txt on the SD card.");
		else if (added == 0) msg("Nothing new", "Every address in /3ds/StreetCast/feeds.txt is already subscribed (or the file is empty).");
		else {
			int idx[MAX_SUBS], n = 0;
			for (int i = before; i < nsubs; i++) idx[n++] = i;
			start_refresh(idx, n, true, false);
		}
	}
}

static void search_input(void)
{
	int act = list_input(SCR_SEARCH, nresults);
	if (act >= 0) subscribe(results[act].url);
}

static void episodes_input(void)
{
	int act = list_input(SCR_EPISODES, ep_nview);
	if ((in.down & K_Y) || tapped(BOT_W - 64, 0, 64, HEADER_H)) {
		if (ask_filter(ep_filter, sizeof(ep_filter), SCR_EPISODES)) ep_view_rebuild();
		return;
	}
	if (in.down & K_X) {
		if (cur_sub >= 0 && cur_sub < nsubs) start_refresh(&cur_sub, 1, false, false);
		return;
	}
	if (act >= 0 && act < ep_nview) episode_menu(&cur_feed.eps[ep_view[act]], cur_feed.title);
}

static int g_njobs; // download jobs listed above the finished downloads this frame

static void on_download_menu(int id)
{
	if (id == M_PLAY || id == M_PLAY_START) start_play(m_ep.id, m_ep.title, m_podcast, NULL, m_ep.duration, id == M_PLAY_START);
	else on_episode_menu(id);
}

// Recompute what the Downloads screen lists. Called before input and before drawing, because
// downloads finish (and get deleted) at any time.
static void downloads_sync(void)
{
	dl_view_rebuild();
	// While searching, only finished downloads are listed (not the queue).
	g_njobs = dl_filter[0] ? 0 : dl_count();
}

static void downloads_input(void)
{
	downloads_sync();
	int act = list_input(SCR_DOWNLOADS, g_njobs + dl_nview);
	if ((in.down & K_Y) || tapped(BOT_W - 64, 0, 64, HEADER_H)) {
		ask_filter(dl_filter, sizeof(dl_filter), SCR_DOWNLOADS);
		return;
	}
	if (act < 0 || act >= g_njobs + dl_nview) return;
	if (act < g_njobs) {
		DlJob j;
		if (!dl_get(act, &j)) return;
		m_ep = j.ep;
		str_copy(m_podcast, sizeof(m_podcast), j.podcast);
		menu_begin(j.ep.title, j.state == DJ_FAILED ? j.err : NULL, on_episode_menu);
		if (j.state == DJ_FAILED) menu_add("Retry download", M_RETRY_DL);
		menu_add(j.state == DJ_FAILED ? "Remove from queue" : "Cancel download", M_CANCEL_DL);
		menu_add("Close", M_NO);
	} else {
		DlEntry* d = &dls[dl_view[act - g_njobs]];
		memset(&m_ep, 0, sizeof(m_ep));
		str_copy(m_ep.id, sizeof(m_ep.id), d->id);
		str_copy(m_ep.title, sizeof(m_ep.title), d->title);
		m_ep.duration = d->duration;
		str_copy(m_podcast, sizeof(m_podcast), d->podcast);
		char label[44], t[16];
		int pos = st_pos(d->id);
		menu_begin(d->title, NULL, on_download_menu);
		if (pos > 0) {
			fmt_time(pos, t, sizeof(t));
			snprintf(label, sizeof(label), "Resume from %s", t);
			menu_add(label, M_PLAY);
			menu_add("Play from start", M_PLAY_START);
		} else {
			menu_add("Play", M_PLAY);
		}
		menu_add("Delete download", M_DELETE);
	}
}

static void toggle_pause(void)
{
	if (pi.state == PL_PAUSED) player_set_paused(false);
	else if (pi.state == PL_PLAYING || pi.state == PL_LOADING) {
		player_set_paused(true);
	}
}

static void seek_rel(int delta)
{
	if (!has_media()) return;
	player_get(&pi);
	player_seek(pi.pos + delta);
}

static void stop_playback(void)
{
	save_position();
	player_stop();
	store_flush();
}

#define SCRUB_X 24
#define SCRUB_W 272
#define SCRUB_Y 98

static void player_input(void)
{
	if (!has_media()) {
		scrubbing = false;
		return;
	}
	if (in.down & K_LEFT) player_focus = player_focus > 0 ? player_focus - 1 : 0;
	if (in.down & K_RIGHT) player_focus = player_focus < 2 ? player_focus + 1 : 2;
	int act = -1;
	if (in.down & K_A) act = player_focus;
	if (tapped(30, 140, 72, 54)) act = player_focus = 0;
	if (tapped(120, 134, 80, 66)) act = player_focus = 1;
	if (tapped(218, 140, 72, 54)) act = player_focus = 2;
	if (act == 0) seek_rel(-settings.skip_back);
	else if (act == 1) toggle_pause();
	else if (act == 2) seek_rel(settings.skip_fwd);

	// Drag (or tap) on the progress bar to scrub.
	if (t_active && t_sy >= SCRUB_Y - 14 && t_sy <= SCRUB_Y + 22) {
		scrubbing = true;
		scrub_frac = ((float)t_lx - SCRUB_X) / SCRUB_W;
		if (scrub_frac < 0) scrub_frac = 0;
		if (scrub_frac > 1) scrub_frac = 1;
	} else if (scrubbing) {
		scrubbing = false;
		if (pi.dur > 0) player_seek(scrub_frac * pi.dur);
	}

	if (in.down & K_X) {
		stop_playback();
		back();
	}
}

static const int skip_back_opts[] = {10, 15, 30};
static const int skip_fwd_opts[] = {15, 30, 45, 60};

static int next_opt(const int* opts, int n, int cur)
{
	for (int i = 0; i < n; i++)
		if (opts[i] == cur) return opts[(i + 1) % n];
	return opts[0];
}

static void settings_input(void)
{
	int act = list_input(SCR_SETTINGS, 4);
	if (act == 0) {
		settings.verify_tls = !settings.verify_tls;
		net_verify_tls = settings.verify_tls;
	} else if (act == 1) settings.skip_back = next_opt(skip_back_opts, 3, settings.skip_back);
	else if (act == 2) settings.skip_fwd = next_opt(skip_fwd_opts, 4, settings.skip_fwd);
	else if (act == 3)
		msg("StreetCast " APP_VERSION, "A podcast player for Nintendo 3DS. Your podcasts and downloads live in /3ds/StreetCast on the SD card.");
	if (act >= 0 && act < 3) {
		settings_touch();
		store_flush();
	}
}

// ------------------------------------------------------------------------------------------------
// Frame update
// ------------------------------------------------------------------------------------------------

static void track_touch(void)
{
	t_tap = false;
	if (in.down & K_TOUCH) {
		t_active = true;
		t_drag = false;
		t_sx = t_lx = in.tx;
		t_sy = t_ly = in.ty;
		t_scroll0 = ls[cur_screen()].target;
	} else if ((in.held & K_TOUCH) && t_active) {
		t_lx = in.tx;
		t_ly = in.ty;
		if (abs(t_ly - t_sy) > 8 || abs(t_lx - t_sx) > 14) t_drag = true;
	} else if (t_active) {
		t_active = false;
		if (!t_drag) {
			t_tap = true;
			tap_x = t_lx;
			tap_y = t_ly;
		}
	}
}

static void housekeeping(void)
{
	// Finished downloads become library entries.
	DlJob j;
	while (dl_take_finished(&j)) {
		dls_add(&j.ep, j.podcast, j.now);
		store_flush();
		toast("Downloaded %s", j.ep.title);
	}

	player_get(&pi);
	if (pi.state != last_state || strcmp(pi.id, last_id)) {
		if (pi.state == PL_ENDED) {
			st_set_played(pi.id, true);
			st_set_pos(pi.id, 0);
			store_flush();
		} else if (pi.state == PL_ERROR) {
			msg("Can't play this episode", pi.err);
		} else if (pi.state == PL_PAUSED) {
			save_position();
			store_flush();
		}
		last_state = pi.state;
		str_copy(last_id, sizeof(last_id), pi.id);
	}
	// Remember the position every 10 seconds so a dead battery loses very little.
	if (ui_frame % 600 == 0) {
		if (pi.state == PL_PLAYING) save_position();
		store_flush();
	}
}

bool app_update(const Input* input)
{
	in = *input;
	ui_frame++;
	track_touch();
	housekeeping();

	for (int s = 0; s < SCR_COUNT; s++) ls[s].scroll += (ls[s].target - ls[s].scroll) * 0.35f;

	// START always closes the app, whatever is on screen (a message box, a busy overlay...).
	// The one exception: with downloads running it asks first; START again confirms.
	if (in.down & K_START) {
		bool confirming = menu.open && menu.on == on_quit_confirm;
		if (dl_busy() && !confirming && !task.running) {
			menu_begin("Quit StreetCast?", "Downloads in progress will stop (they resume next time).", on_quit_confirm);
			menu_add("Quit", M_YES);
			menu_add("Keep going", M_NO);
			menu.sel = 1;
			return true;
		}
		if (task.running) task.prog.cancel = true;
		want_quit = true;
		return false;
	}

	if (task.running) {
		if (task.done) {
			plat_thread_join(&task.thread);
			task.running = false;
			if (task.finish) task.finish();
		} else if (in.down & K_B) {
			task.prog.cancel = true;
		}
		return !want_quit;
	}
	if (menu.open) {
		menu_input();
		return !want_quit;
	}

	// Keys that work everywhere.
	if ((in.down & K_SELECT) && has_media()) push(SCR_PLAYER);
	if (in.down & K_L) seek_rel(-settings.skip_back);
	if (in.down & K_R) seek_rel(settings.skip_fwd);

	Screen s = cur_screen();
	if (depth > 0 && ((in.down & K_B) || tapped(0, 0, 64, HEADER_H))) {
		if (s == SCR_EPISODES && ep_filter[0]) {
			clear_filter(ep_filter, SCR_EPISODES);
			ep_view_rebuild();
		} else if (s == SCR_DOWNLOADS && dl_filter[0]) {
			clear_filter(dl_filter, SCR_DOWNLOADS);
		} else {
			back();
		}
		return true;
	}
	switch (s) {
	case SCR_HOME: home_input(); break;
	case SCR_PODCASTS: podcasts_input(); break;
	case SCR_ADD: add_input(); break;
	case SCR_SEARCH: search_input(); break;
	case SCR_EPISODES: episodes_input(); break;
	case SCR_DOWNLOADS: downloads_input(); break;
	case SCR_PLAYER: player_input(); break;
	case SCR_SETTINGS: settings_input(); break;
	default: break;
	}
	return !want_quit;
}

void app_init(bool sound)
{
	sound_ok = sound;
	net_verify_tls = settings.verify_tls;
	stack[0] = SCR_HOME;
	depth = 0;
	player_get(&pi);
}

void app_set_closing(void) { closing = true; }

bool app_shutdown(void)
{
	// Save first: nothing below may cost the user their place.
	save_position();
	store_flush();
	if (task.running) {
		task.prog.cancel = true;
		if (!plat_thread_join_timeout(&task.thread, 4000)) return false; // still using cur_feed's files; leave it
		task.running = false;
	}
	feed_free(&cur_feed);
	return true;
}

// ------------------------------------------------------------------------------------------------
// Drawing: bottom screen
// ------------------------------------------------------------------------------------------------

static void draw_header(const char* title)
{
	gfx_rect(0, 0, BOT_W, HEADER_H, COL_WHITE);
	gfx_rect(0, HEADER_H - 1, BOT_W, 1, COL_LINE);
	if (depth > 0) {
		ui_rrect(6, 5, 52, 18, 9, COL_BG);
		ui_text(G_B " Back", 13, 7, TS_SMALL, COL_TEXT2);
	}
	ui_text_fit(title, BOT_W / 2, 5, 180, 0.55f, COL_TEXT, 1);
}

// Header for a searchable list: shows the search words when a filter is active.
static void draw_search_header(const char* title, const char* filter, int matches)
{
	char t[128];
	if (filter[0]) snprintf(t, sizeof(t), "\"%s\"  \xC2\xB7  %d", filter, matches);
	draw_header(filter[0] ? t : title);
	ui_rrect(BOT_W - 58, 5, 52, 18, 9, filter[0] ? COL_GLOW : COL_BG);
	ui_icon(IC_SEARCH, BOT_W - 44, 14, 11, filter[0] ? COL_WHITE : COL_TEXT2, filter[0] ? COL_GLOW : COL_BG);
	ui_text(G_Y, BOT_W - 31, 7, TS_SMALL, filter[0] ? COL_WHITE : COL_TEXT2);
}

static void draw_footer(const char* hints)
{
	gfx_rect(0, FOOTER_Y, BOT_W, SCREEN_H - FOOTER_Y, COL_WHITE);
	gfx_rect(0, FOOTER_Y, BOT_W, 1, COL_LINE);
	ui_text_fit(hints, BOT_W / 2, FOOTER_Y + 6, BOT_W - 12, TS_SMALL, COL_TEXT2, 1);
}

static void home_draw(void)
{
	static const char* labels[5] = {"My Podcasts", "Add Podcast", "Downloads", "Now Playing", "Settings"};
	static const Icon icons[4] = {IC_LIBRARY, IC_PLUS, IC_DOWNLOAD, IC_PLAY};
	char sub[4][48];
	snprintf(sub[0], 48, nsubs == 1 ? "%d show" : "%d shows", nsubs);
	snprintf(sub[1], 48, "Search or enter a feed");
	int nj = dl_count();
	if (nj) snprintf(sub[2], 48, "%d in progress", nj);
	else snprintf(sub[2], 48, ndls == 1 ? "%d episode" : "%d episodes", ndls);
	snprintf(sub[3], 48, "%s", pi.state == PL_PLAYING ? "Playing" : pi.state == PL_PAUSED ? "Paused" : pi.state == PL_LOADING ? "Loading" : "Nothing playing");

	for (int i = 0; i < 4; i++) {
		bool f = ls[SCR_HOME].sel == i;
		float x = home_btn[i].x, y = home_btn[i].y, w = home_btn[i].w, h = home_btn[i].h;
		ui_bubble(x, y, w, h, 18, f, COL_BG);
		Icon ic = icons[i];
		if (i == 3 && pi.state == PL_PLAYING) ic = IC_PAUSE;
		ui_icon(ic, x + w / 2, y + 24, 28, f ? COL_GLOW : COL_BLUE, COL_WHITE);
		ui_text_center(labels[i], x + w / 2, y + 42, TS_BODY, COL_TEXT);
		ui_text_fit(sub[i], x + w / 2, y + 60, w - 16, TS_SMALL - 0.02f, COL_TEXT2, 1);
	}
	bool f = ls[SCR_HOME].sel == 4;
	ui_bubble(home_btn[4].x, home_btn[4].y, home_btn[4].w, home_btn[4].h, 11.5f, f, COL_BG);
	ui_icon(IC_SETTINGS, home_btn[4].x + 26, home_btn[4].y + 11.5f, 13, f ? COL_GLOW : COL_TEXT2, COL_WHITE);
	ui_text_center(labels[4], home_btn[4].x + 66, home_btn[4].y + 5, TS_SMALL, COL_TEXT);
	draw_footer(G_A " Select    START Quit");
}

static void podcasts_draw(void)
{
	if (nsubs == 0) empty_state(IC_LIBRARY, "No podcasts yet", "Press " G_A " to search for a show or add a feed address.");
	for (int i = 0; i < nsubs; i++) {
		float y = row_begin(SCR_PODCASTS, i);
		if (y < -500) continue;
		bool f = i == ls[SCR_PODCASTS].sel;
		ui_icon(IC_WAVES, ROW_X + 21, y + ROW_H / 2, 26, f ? COL_GLOW : COL_BLUE, COL_WHITE);
		ui_text_fit(subs[i].title[0] ? subs[i].title : subs[i].url, ROW_X + 42, y + 4, ROW_W - 54, TS_BODY, COL_TEXT, 0);
		char sub[48];
		if (subs[i].count < 0) snprintf(sub, sizeof(sub), "Not loaded yet");
		else snprintf(sub, sizeof(sub), subs[i].count == 1 ? "%d episode" : "%d episodes", subs[i].count);
		ui_text(sub, ROW_X + 42, y + 21, TS_SMALL - 0.02f, COL_TEXT2);
	}
	list_scrollbar(SCR_PODCASTS, nsubs);
	draw_header("My Podcasts");
	draw_footer(nsubs ? G_A " Open   " G_X " Update all   " G_Y " Options" : G_A " Add a podcast   " G_B " Back");
}

static void add_draw(void)
{
	static const char* titles[3] = {"Search by name", "Enter a feed address", "Import feeds.txt"};
	static const char* subsx[3] = {"Find a show in the podcast directory", "Type or paste an RSS URL", "Add every address in /3ds/StreetCast/feeds.txt"};
	static const Icon icons[3] = {IC_SEARCH, IC_LINK, IC_FILE};
	for (int i = 0; i < 3; i++) {
		float y = row_begin(SCR_ADD, i);
		if (y < -500) continue;
		bool f = i == ls[SCR_ADD].sel;
		ui_icon(icons[i], ROW_X + 21, y + ROW_H / 2, 22, f ? COL_GLOW : COL_BLUE, COL_WHITE);
		ui_text(titles[i], ROW_X + 42, y + 4, TS_BODY, COL_TEXT);
		ui_text_fit(subsx[i], ROW_X + 42, y + 21, ROW_W - 54, TS_SMALL - 0.02f, COL_TEXT2, 0);
	}
	draw_header("Add Podcast");
	draw_footer(G_A " Select   " G_B " Back");
}

static void search_draw(void)
{
	for (int i = 0; i < nresults; i++) {
		float y = row_begin(SCR_SEARCH, i);
		if (y < -500) continue;
		bool have = subs_find(results[i].url) >= 0;
		ui_text_fit(results[i].name, ROW_X + 14, y + 4, ROW_W - (have ? 60 : 28), TS_BODY, COL_TEXT, 0);
		ui_text_fit(results[i].author, ROW_X + 14, y + 21, ROW_W - 28, TS_SMALL - 0.02f, COL_TEXT2, 0);
		if (have) ui_icon(IC_CHECK, ROW_X + ROW_W - 22, y + 13, 16, COL_GREEN, COL_WHITE);
	}
	list_scrollbar(SCR_SEARCH, nresults);
	draw_header("Search Results");
	draw_footer(G_A " Subscribe   " G_B " Back");
}

// Short status for an episode row; returns its colour.
static Color episode_status(const Episode* e, char* out, size_t cap, float* progress)
{
	DlJob j;
	*progress = -1;
	out[0] = 0;
	if (dl_find(e->id, &j) >= 0) {
		if (j.state == DJ_FAILED) {
			snprintf(out, cap, "Download failed");
			return COL_RED;
		}
		if (j.state == DJ_ACTIVE && j.total > 0) {
			*progress = (float)((double)j.now / (double)j.total);
			snprintf(out, cap, "%d%%", (int)(*progress * 100));
		} else {
			snprintf(out, cap, j.state == DJ_ACTIVE ? "Starting" : "Queued");
		}
		return COL_BLUE;
	}
	if (dls_find(e->id) >= 0) {
		snprintf(out, cap, "Downloaded");
		return COL_GREEN;
	}
	if (e->kind == EP_UNSUPPORTED) {
		snprintf(out, cap, "Not MP3");
		return COL_TEXT2;
	}
	return COL_TEXT2;
}

static void episodes_draw(void)
{
	if (cur_feed.count == 0) empty_state(IC_LIBRARY, "No episodes", "Press " G_X " to check for new episodes.");
	else if (ep_nview == 0) empty_state(IC_SEARCH, "No matching episodes", "Press " G_Y " to search again or " G_B " to clear the search.");
	for (int i = 0; i < ep_nview; i++) {
		float y = row_begin(SCR_EPISODES, i);
		if (y < -500) continue;
		Episode* e = &cur_feed.eps[ep_view[i]];
		bool played = st_played(e->id);
		int pos = st_pos(e->id);
		bool now = has_media() && !strcmp(pi.id, e->id);
		// Unplayed marker: solid blue dot; in progress: ring; played: faint ring.
		float dx = ROW_X + 14, dy = y + ROW_H / 2;
		if (now) ui_icon(pi.state == PL_PAUSED ? IC_PAUSE : IC_PLAY, dx, dy, 11, COL_GLOW, COL_WHITE);
		else {
			gfx_circle(dx, dy, 4.5f, played ? COL_TRACK : COL_GLOW);
			if (played || pos > 0) gfx_circle(dx, dy, 2.5f, COL_WHITE);
		}
		ui_text_fit(e->title, ROW_X + 27, y + 4, ROW_W - 39, TS_BODY, played ? COL_TEXT2 : COL_TEXT, 0);

		char meta[64], dur[16], status[32];
		float prog;
		Color sc = episode_status(e, status, sizeof(status), &prog);
		if (e->duration > 0) {
			fmt_time(e->duration, dur, sizeof(dur));
			snprintf(meta, sizeof(meta), "%s%s%s", e->date, e->date[0] ? "  \xC2\xB7  " : "", dur);
		} else {
			snprintf(meta, sizeof(meta), "%s", e->date);
		}
		if (!status[0] && pos > 0 && !played) {
			char t[16];
			fmt_time(pos, t, sizeof(t));
			snprintf(status, sizeof(status), "%s in", t);
			sc = COL_BLUE;
		} else if (!status[0] && played) {
			snprintf(status, sizeof(status), "Played");
		}
		ui_text(meta, ROW_X + 27, y + 21, TS_SMALL - 0.02f, COL_TEXT2);
		if (status[0]) ui_text_right(status, ROW_X + ROW_W - 12, y + 21, TS_SMALL - 0.02f, sc);
		if (prog >= 0) ui_progress(ROW_X + ROW_W - 92, y + 33, 80, 3, prog, false, COL_WHITE);
	}
	list_scrollbar(SCR_EPISODES, ep_nview);
	draw_search_header(cur_feed.title[0] ? cur_feed.title : "Episodes", ep_filter, ep_nview);
	draw_footer(ep_filter[0] ? G_A " Options   " G_Y " Search   " G_B " Clear search" : G_A " Options   " G_X " Update   " G_Y " Search");
}

static void downloads_draw(void)
{
	int nj = g_njobs;
	int total = nj + dl_nview;
	if (total == 0 && dl_filter[0]) empty_state(IC_SEARCH, "No matching downloads", "Press " G_Y " to search again or " G_B " to clear the search.");
	else if (total == 0) empty_state(IC_DOWNLOAD, "No downloads", "Download episodes to listen without Wi-Fi.");
	for (int i = 0; i < total; i++) {
		float y = row_begin(SCR_DOWNLOADS, i);
		if (y < -500) continue;
		bool f = i == ls[SCR_DOWNLOADS].sel;
		char sub[96], a[24], b[24];
		if (i < nj) {
			DlJob j;
			if (!dl_get(i, &j)) continue;
			ui_icon(IC_DOWNLOAD, ROW_X + 19, y + ROW_H / 2, 18, j.state == DJ_FAILED ? COL_RED : (f ? COL_GLOW : COL_BLUE), COL_WHITE);
			ui_text_fit(j.ep.title, ROW_X + 38, y + 4, ROW_W - 50, TS_BODY, COL_TEXT, 0);
			if (j.state == DJ_FAILED) snprintf(sub, sizeof(sub), "Failed \xC2\xB7 press " G_A " to retry");
			else if (j.state == DJ_QUEUED) snprintf(sub, sizeof(sub), "Waiting");
			else if (j.total > 0) {
				fmt_size(j.now, a, sizeof(a));
				fmt_size(j.total, b, sizeof(b));
				snprintf(sub, sizeof(sub), "%s of %s", a, b);
			} else {
				fmt_size(j.now, a, sizeof(a));
				snprintf(sub, sizeof(sub), "%s", a);
			}
			ui_text(sub, ROW_X + 38, y + 21, TS_SMALL - 0.02f, j.state == DJ_FAILED ? COL_RED : COL_TEXT2);
			if (j.state == DJ_ACTIVE && j.total > 0)
				ui_progress(ROW_X + ROW_W - 102, y + 26, 90, 4, (float)((double)j.now / (double)j.total), false, COL_WHITE);
		} else {
			DlEntry* d = &dls[dl_view[i - nj]];
			bool played = st_played(d->id);
			ui_icon(IC_CHECK, ROW_X + 19, y + ROW_H / 2 - 2, 16, played ? COL_TRACK : COL_GREEN, COL_WHITE);
			ui_text_fit(d->title, ROW_X + 38, y + 4, ROW_W - 50, TS_BODY, played ? COL_TEXT2 : COL_TEXT, 0);
			fmt_size(d->size, a, sizeof(a));
			snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s", a, d->podcast);
			ui_text_fit(sub, ROW_X + 38, y + 21, ROW_W - 50, TS_SMALL - 0.02f, COL_TEXT2, 0);
		}
	}
	list_scrollbar(SCR_DOWNLOADS, total);
	draw_search_header("Downloads", dl_filter, dl_nview);
	draw_footer(dl_filter[0] ? G_A " Options   " G_Y " Search   " G_B " Clear search" : G_A " Options   " G_Y " Search   " G_B " Back");
}

static const char* state_label(void)
{
	if (pi.state == PL_LOADING) return pi.is_stream ? "Connecting\xE2\x80\xA6" : "Loading\xE2\x80\xA6";
	if (pi.buffering) return "Buffering\xE2\x80\xA6";
	if (pi.state == PL_PAUSED) return "Paused";
	return pi.is_stream ? "Streaming" : "Downloaded";
}

static void player_draw(void)
{
	if (!has_media()) {
		const char* t = pi.state == PL_ENDED ? "Episode finished" : "Nothing playing";
		empty_state(IC_PLAY, t, "Pick an episode from My Podcasts or Downloads.");
		draw_header("Now Playing");
		draw_footer(G_B " Back");
		return;
	}
	ui_text_fit(pi.podcast, BOT_W / 2, 34, 280, TS_SMALL, COL_BLUE, 1);
	ui_text_wrap(pi.title, 20, 49, 280, TS_BODY, COL_TEXT, 2, 16, true);

	float frac = pi.dur > 0 ? (float)(pi.pos / pi.dur) : 0;
	double shown = pi.pos;
	if (scrubbing) {
		frac = scrub_frac;
		shown = scrub_frac * pi.dur;
	}
	ui_progress(SCRUB_X, SCRUB_Y, SCRUB_W, 6, frac, true, COL_BG);
	char a[16], b[20];
	fmt_time((int)shown, a, sizeof(a));
	ui_text(a, SCRUB_X, SCRUB_Y + 14, TS_SMALL, COL_TEXT2);
	if (pi.dur > 0) {
		char r[16];
		fmt_time((int)(pi.dur - shown), r, sizeof(r));
		snprintf(b, sizeof(b), "-%s", r);
	} else {
		snprintf(b, sizeof(b), "--:--");
	}
	ui_text_right(b, SCRUB_X + SCRUB_W, SCRUB_Y + 14, TS_SMALL, COL_TEXT2);
	ui_text_center(state_label(), BOT_W / 2, SCRUB_Y + 14, TS_SMALL, pi.buffering || pi.state == PL_LOADING ? COL_BLUE : COL_TEXT2);

	char lab[12];
	bool f0 = player_focus == 0, f1 = player_focus == 1, f2 = player_focus == 2;
	ui_bubble(30, 140, 72, 54, 20, f0, COL_BG);
	ui_icon(IC_BACK15, 66, 160, 22, f0 ? COL_GLOW : COL_BLUE, COL_WHITE);
	snprintf(lab, sizeof(lab), "%ds", settings.skip_back);
	ui_text_center(lab, 66, 174, TS_SMALL, COL_TEXT2);

	ui_bubble(120, 134, 80, 66, 33, f1, COL_BG);
	bool show_pause = pi.state == PL_PLAYING || pi.state == PL_LOADING;
	if (pi.state == PL_LOADING || pi.buffering) ui_spinner(160, 167, 15, COL_WHITE);
	else ui_icon(show_pause ? IC_PAUSE : IC_PLAY, 160, 167, 30, f1 ? COL_GLOW : COL_BLUE, COL_WHITE);

	ui_bubble(218, 140, 72, 54, 20, f2, COL_BG);
	ui_icon(IC_FWD30, 254, 160, 22, f2 ? COL_GLOW : COL_BLUE, COL_WHITE);
	snprintf(lab, sizeof(lab), "%ds", settings.skip_fwd);
	ui_text_center(lab, 254, 174, TS_SMALL, COL_TEXT2);

	draw_header("Now Playing");
	draw_footer(G_A " Select   " G_L G_R " Skip   " G_X " Stop   " G_B " Back");
}

static void settings_draw(void)
{
	char val[4][24];
	snprintf(val[0], 24, "%s", settings.verify_tls ? "On" : "Off");
	snprintf(val[1], 24, "%d seconds", settings.skip_back);
	snprintf(val[2], 24, "%d seconds", settings.skip_fwd);
	snprintf(val[3], 24, "v" APP_VERSION);
	static const char* names[4] = {"Verify HTTPS", "Skip back (" G_L ")", "Skip forward (" G_R ")", "About StreetCast"};
	static const char* notes[4] = {"Turn off only if feeds fail with a security error", "How far the rewind button jumps", "How far the forward button jumps", "Version and where your files are kept"};
	for (int i = 0; i < 4; i++) {
		float y = row_begin(SCR_SETTINGS, i);
		if (y < -500) continue;
		ui_text(names[i], ROW_X + 14, y + 4, TS_BODY, COL_TEXT);
		ui_text_fit(notes[i], ROW_X + 14, y + 21, ROW_W - 28, TS_SMALL - 0.02f, COL_TEXT2, 0);
		ui_text_right(val[i], ROW_X + ROW_W - 14, y + 5, TS_BODY - 0.04f, COL_BLUE);
	}
	draw_header("Settings");
	draw_footer(G_A " Change   " G_B " Back");
}

static void busy_draw(void)
{
	gfx_rect(0, 0, BOT_W, SCREEN_H, 0x26323F8C);
	float w = 220, h = 104, x = (BOT_W - w) / 2, y = (SCREEN_H - h) / 2;
	ui_rrect(x - 1, y - 1, w + 2, h + 2, 17, COL_BORDER);
	ui_rrect(x, y, w, h, 16, COL_BG_TOP);
	ui_spinner(BOT_W / 2, y + 32, 14, COL_BG_TOP);
	char label[100];
	snprintf(label, sizeof(label), "%s\xE2\x80\xA6", task.label);
	ui_text_fit(label, BOT_W / 2, y + 56, w - 24, TS_BODY, COL_TEXT, 1);
	char sub[48] = "";
	if (task.prog.cancel) snprintf(sub, sizeof(sub), "Cancelling");
	else if (task.prog.now > 0) {
		char sz[24];
		fmt_size(task.prog.now, sz, sizeof(sz));
		snprintf(sub, sizeof(sub), "%s   \xC2\xB7   " G_B " Cancel", sz);
	} else snprintf(sub, sizeof(sub), G_B " Cancel");
	ui_text_center(sub, BOT_W / 2, y + 78, TS_SMALL, COL_TEXT2);
}

void app_draw_bottom(void)
{
	gfx_rect(0, 0, BOT_W, SCREEN_H, COL_BG);
	switch (cur_screen()) {
	case SCR_HOME: home_draw(); break;
	case SCR_PODCASTS: podcasts_draw(); break;
	case SCR_ADD: add_draw(); break;
	case SCR_SEARCH: search_draw(); break;
	case SCR_EPISODES: episodes_draw(); break;
	case SCR_DOWNLOADS: downloads_draw(); break;
	case SCR_PLAYER: player_draw(); break;
	case SCR_SETTINGS: settings_draw(); break;
	default: break;
	}
	if (closing) {
		gfx_rect(0, 0, BOT_W, SCREEN_H, 0x26323F8C);
		ui_rrect(59, 91, 202, 58, 17, COL_BORDER);
		ui_rrect(60, 92, 200, 56, 16, COL_BG_TOP);
		ui_text_center("Closing StreetCast\xE2\x80\xA6", BOT_W / 2, 112, TS_BODY, COL_TEXT);
		return;
	}
	if (menu.open) menu_draw();
	if (task.running) busy_draw();
}

// ------------------------------------------------------------------------------------------------
// Drawing: top screen
// ------------------------------------------------------------------------------------------------

static void draw_wordmark(float x, float y, float scale, bool icon)
{
	float xx = x;
	if (icon) {
		ui_icon(IC_WAVES, x + 15 * scale, y + 15 * scale, 30 * scale, COL_GLOW, COL_WHITE);
		xx += 36 * scale;
	}
	ui_text("Street", xx, y, scale, COL_TEXT);
	ui_text("Cast", xx + gfx_text_width("Street", scale), y, scale, COL_BLUE);
}

static float wordmark_width(float scale, bool icon)
{
	return (icon ? 36 * scale : 0) + gfx_text_width("StreetCast", scale);
}

static void draw_status_bar(void)
{
	gfx_rect(0, 0, TOP_W, 24, COL_WHITE);
	gfx_rect(0, 24, TOP_W, 1, COL_LINE);
	ui_icon(IC_WAVES, 17, 12, 17, COL_GLOW, COL_WHITE);
	ui_text("Street", 30, 4, TS_BODY, COL_TEXT);
	ui_text("Cast", 30 + gfx_text_width("Street", TS_BODY), 4, TS_BODY, COL_BLUE);

	// Battery
	float bx = TOP_W - 34, by = 7;
	int lvl = sys_battery();
	ui_rrect(bx, by, 22, 11, 3, COL_TEXT2);
	ui_rrect(bx + 1.5f, by + 1.5f, 19, 8, 2, COL_WHITE);
	gfx_rect(bx + 22, by + 3, 2, 5, COL_TEXT2);
	if (lvl > 0) ui_rrect(bx + 3, by + 3, 16.0f * (float)lvl / 5.0f, 5, 1, sys_charging() ? COL_GREEN : (lvl <= 1 ? COL_RED : COL_GLOW));
	// Wi-Fi bars
	int wifi = sys_wifi();
	for (int i = 0; i < 3; i++) {
		float h = 4 + (float)i * 3.5f;
		ui_rrect(TOP_W - 58 + (float)i * 5, 18 - h, 3.5f, h, 1, i < wifi ? COL_GLOW : COL_TRACK);
	}
	int hh, mm;
	char clk[12];
	sys_clock(&hh, &mm);
	snprintf(clk, sizeof(clk), "%d:%02d", hh, mm);
	ui_text_right(clk, TOP_W - 68, 5, TS_SMALL + 0.04f, COL_TEXT2);
}

static void draw_backdrop(void)
{
	gfx_rect_vgrad(0, 0, TOP_W, SCREEN_H, COL_BG_TOP, COL_BG);
	// Slow drifting bubbles.
	static const struct { float x, y, r, sp; } b[6] = {
		{50, 200, 46, 0.011f}, {345, 70, 60, 0.008f}, {210, 215, 30, 0.015f}, {120, 60, 22, 0.012f}, {300, 190, 18, 0.018f}, {20, 90, 14, 0.02f},
	};
	for (int i = 0; i < 6; i++) {
		float dy = sinf((float)ui_frame * b[i].sp + (float)i) * 6;
		float cy = b[i].y + dy;
		float t = cy / SCREEN_H; // approximate the gradient behind the bubble
		Color under = ui_mix(COL_BG_TOP, COL_BG, t);
		gfx_circle(b[i].x, cy, b[i].r, ui_mix(under, COL_GLOW, 0.10f));
		gfx_circle(b[i].x, cy, b[i].r - 2, ui_mix(under, COL_WHITE, 0.55f));
	}
}

static void card(float x, float y, float w, float h)
{
	ui_rrect(x - 1, y + 1, w + 2, h + 2, 17, COL_SHADOW);
	ui_rrect(x - 1, y - 1, w + 2, h + 2, 17, COL_BORDER);
	ui_rrect(x, y, w, h, 16, COL_WHITE);
}

static void draw_now_playing_card(void)
{
	card(14, 36, 372, 190);
	ui_icon(IC_WAVES, 44, 64, 34, COL_GLOW, COL_WHITE);
	ui_text("NOW PLAYING", 70, 48, TS_SMALL - 0.03f, COL_BLUE);
	ui_text_fit(pi.podcast, 70, 62, 300, TS_BODY, COL_TEXT2, 0);
	ui_text_wrap(pi.title, 30, 92, 340, 0.6f, COL_TEXT, 3, 20, false);

	float frac = pi.dur > 0 ? (float)(pi.pos / pi.dur) : 0;
	ui_progress(30, 180, 340, 6, frac, false, COL_WHITE);
	char a[16], b[16];
	fmt_time((int)pi.pos, a, sizeof(a));
	ui_text(a, 30, 194, TS_SMALL, COL_TEXT2);
	if (pi.dur > 0) {
		fmt_time((int)pi.dur, b, sizeof(b));
		ui_text_right(b, 370, 194, TS_SMALL, COL_TEXT2);
	}
	ui_text_center(state_label(), TOP_W / 2, 194, TS_SMALL, pi.buffering || pi.state == PL_LOADING ? COL_BLUE : COL_TEXT2);
}

static void draw_mini_player(void)
{
	float x = 14, y = 192, w = 372, h = 36;
	card(x, y, w, h);
	if (pi.state == PL_LOADING || pi.buffering) ui_spinner(x + 20, y + 18, 8, COL_WHITE);
	else ui_icon(pi.state == PL_PAUSED ? IC_PAUSE : IC_PLAY, x + 20, y + 18, 14, COL_GLOW, COL_WHITE);
	ui_text_fit(pi.title, x + 38, y + 5, w - 110, TS_SMALL + 0.02f, COL_TEXT, 0);
	char a[16], b[16], t[40];
	fmt_time((int)pi.pos, a, sizeof(a));
	if (pi.dur > 0) {
		fmt_time((int)pi.dur, b, sizeof(b));
		snprintf(t, sizeof(t), "%s / %s", a, b);
	} else {
		snprintf(t, sizeof(t), "%s", a);
	}
	ui_text_right(t, x + w - 12, y + 6, TS_SMALL - 0.03f, COL_TEXT2);
	ui_progress(x + 38, y + 25, w - 50, 3, pi.dur > 0 ? (float)(pi.pos / pi.dur) : 0, false, COL_WHITE);
}

static void draw_episode_card(const char* title, const char* meta, const char* desc, float h)
{
	card(14, 36, 372, h);
	int lines = ui_text_wrap(title, 30, 46, 340, 0.6f, COL_TEXT, 2, 20, false);
	float y = 46 + (float)lines * 20 + 2;
	ui_text_fit(meta, 30, y, 340, TS_SMALL, COL_BLUE, 0);
	y += 18;
	int room = (int)((36 + h - 8 - y) / 14);
	if (desc && desc[0] && room > 0) ui_text_wrap(desc, 30, y, 340, TS_SMALL, COL_TEXT2, room, 14, false);
}

static void draw_hero(bool with_tip)
{
	float sc = 1.15f;
	float w = wordmark_width(sc, true);
	float y = has_media() ? 62 : 78;
	draw_wordmark((TOP_W - w) / 2, y, sc, true);
	ui_text_center("Podcasts for your Nintendo 3DS", TOP_W / 2, y + 42, TS_BODY, COL_TEXT2);
	if (with_tip && !has_media())
		ui_text_center("Plug in headphones and close the lid to keep listening.", TOP_W / 2, 204, TS_SMALL, COL_TEXT2);
}

void app_draw_top(void)
{
	draw_backdrop();
	Screen s = cur_screen();
	if (s == SCR_DOWNLOADS) downloads_sync();
	bool media = has_media();
	bool mini = media;
	float card_h = media ? 148 : 190;

	if (s == SCR_PLAYER && media) {
		draw_now_playing_card();
		mini = false;
	} else if (s == SCR_EPISODES && ep_nview > 0) {
		Episode* e = &cur_feed.eps[ep_view[ls[SCR_EPISODES].sel < ep_nview ? ls[SCR_EPISODES].sel : 0]];
		char meta[96], dur[16], size[24];
		snprintf(meta, sizeof(meta), "%s", e->date);
		if (e->duration > 0) {
			fmt_time(e->duration, dur, sizeof(dur));
			snprintf(meta + strlen(meta), sizeof(meta) - strlen(meta), "%s%s", meta[0] ? "  \xC2\xB7  " : "", dur);
		}
		if (e->size > 1024 * 1024) {
			fmt_size(e->size, size, sizeof(size));
			snprintf(meta + strlen(meta), sizeof(meta) - strlen(meta), "%s%s", meta[0] ? "  \xC2\xB7  " : "", size);
		}
		draw_episode_card(e->title, meta, e->desc, card_h);
	} else if (s == SCR_DOWNLOADS && ls[SCR_DOWNLOADS].sel >= g_njobs && ls[SCR_DOWNLOADS].sel - g_njobs < dl_nview) {
		DlEntry* d = &dls[dl_view[ls[SCR_DOWNLOADS].sel - g_njobs]];
		char meta[96], dur[16], size[24];
		fmt_size(d->size, size, sizeof(size));
		if (d->duration > 0) {
			fmt_time(d->duration, dur, sizeof(dur));
			snprintf(meta, sizeof(meta), "%s  \xC2\xB7  %s", dur, size);
		} else {
			snprintf(meta, sizeof(meta), "%s", size);
		}
		draw_episode_card(d->title, meta, d->podcast, 96);
	} else if (s == SCR_SEARCH && nresults > 0) {
		SearchResult* r = &results[ls[SCR_SEARCH].sel < nresults ? ls[SCR_SEARCH].sel : 0];
		draw_episode_card(r->name, r->author, subs_find(r->url) >= 0 ? "You're subscribed to this podcast." : "Press \xEE\x80\x80 to subscribe.", 96);
	} else if (s == SCR_PODCASTS && nsubs > 0) {
		Sub* p = &subs[ls[SCR_PODCASTS].sel < nsubs ? ls[SCR_PODCASTS].sel : 0];
		char meta[64];
		if (p->count < 0) snprintf(meta, sizeof(meta), "Not loaded yet");
		else snprintf(meta, sizeof(meta), p->count == 1 ? "%d episode" : "%d episodes", p->count);
		draw_episode_card(p->title[0] ? p->title : "New podcast", meta, p->url, 96);
	} else {
		draw_hero(true);
	}

	if (mini) draw_mini_player();
	draw_status_bar();

	if (ui_frame < toast_until && toast_text[0]) {
		float tw = gfx_text_width(toast_text, TS_SMALL + 0.02f);
		if (tw > 340) tw = 340;
		float w = tw + 32, x = (TOP_W - w) / 2, y = mini ? 158 : 196;
		ui_rrect(x - 2, y - 2, w + 4, 30, 15, COL_GLOW);
		ui_rrect(x, y, w, 26, 13, COL_WHITE);
		ui_text_fit(toast_text, TOP_W / 2, y + 5, 340, TS_SMALL + 0.02f, COL_TEXT, 1);
	}
}
