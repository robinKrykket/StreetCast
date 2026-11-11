// StreetCast - a podcast player for Nintendo 3DS homebrew.
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>

#include "app.h"
#include "dl.h"
#include "gfx.h"
#include "gfx_3ds.h"
#include "net.h"
#include "player.h"
#include "store.h"

// curl + mbedTLS and big feeds want more than the 32 KB default main stack.
u32 __stacksize__ = 128 * 1024;

// ------------------------------------------------------------------------------------------------
// Lid-closed listening.
//
// The built-in Nintendo 3DS Sound app keeps playing with the lid shut only through headphones
// (the speakers are muted when the console is closed). Homebrew gets the same result by refusing
// the system's sleep request while audio is playing through headphones. With sleep refused the
// console does NOT turn its screens off by itself, so we switch the backlights off ourselves,
// and we stop the network manager from handing Wi-Fi over to StreetPass (which it otherwise does
// when the lid closes) so streams and downloads keep flowing.
// ------------------------------------------------------------------------------------------------

static bool have_gsplcd, have_ndmu, have_ptmu;
static bool awake_mode;   // sleep currently refused
static bool screens_dark; // we turned the backlights off
static bool ndm_locked;

static bool shell_open(void)
{
	u8 state = 1;
	if (!have_ptmu || R_FAILED(PTMU_GetShellState(&state))) return true;
	return state != 0;
}

static void screens_set_dark(bool dark)
{
	if (dark == screens_dark || !have_gsplcd) return;
	if (dark) GSPLCD_PowerOffAllBacklights();
	else GSPLCD_PowerOnAllBacklights();
	screens_dark = dark;
}

static void awake_set(bool on)
{
	if (on == awake_mode) return;
	if (on) {
		aptSetSleepAllowed(false);
		if (have_ndmu && R_SUCCEEDED(NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE))) {
			NDMU_LockState();
			ndm_locked = true;
		}
	} else {
		// Light the screens before allowing sleep: if the lid is shut, allowing sleep puts
		// the console to sleep immediately.
		screens_set_dark(false);
		if (ndm_locked) {
			NDMU_UnlockState();
			NDMU_LeaveExclusiveState();
			ndm_locked = false;
		}
		aptSetSleepAllowed(true);
	}
	awake_mode = on;
}

static void power_update(bool open)
{
	bool phones = osIsHeadsetConnected();
	// Lid shut without headphones: nothing would be audible, so pause (and let the console sleep).
	if (!open && !phones && player_active()) player_set_paused(true);

	bool want_awake = (player_active() && (open || phones)) || dl_busy() || app_task_busy();
	awake_set(want_awake);
	if (awake_mode) screens_set_dark(!open);
}

static void apt_hook(APT_HookType type, void* param)
{
	(void)param;
	// Never leave the screens dark when the system takes over (HOME menu, sleep, exit).
	if (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP || type == APTHOOK_ONEXIT) screens_set_dark(false);
}

// ------------------------------------------------------------------------------------------------

static unsigned map_keys(u32 k)
{
	unsigned o = 0;
	if (k & KEY_A) o |= K_A;
	if (k & KEY_B) o |= K_B;
	if (k & KEY_X) o |= K_X;
	if (k & KEY_Y) o |= K_Y;
	if (k & (KEY_L | KEY_ZL)) o |= K_L;
	if (k & (KEY_R | KEY_ZR)) o |= K_R;
	if (k & KEY_UP) o |= K_UP;
	if (k & KEY_DOWN) o |= K_DOWN;
	if (k & KEY_LEFT) o |= K_LEFT;
	if (k & KEY_RIGHT) o |= K_RIGHT;
	if (k & KEY_START) o |= K_START;
	if (k & KEY_SELECT) o |= K_SELECT;
	if (k & KEY_TOUCH) o |= K_TOUCH;
	return o;
}

static void render(C3D_RenderTarget* top, C3D_RenderTarget* bottom)
{
	if (!C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) return;
	gfx3ds_frame();
	C2D_TargetClear(top, C2D_Color32(0xEE, 0xF2, 0xF6, 0xFF));
	C2D_SceneBegin(top);
	app_draw_top();
	C2D_TargetClear(bottom, C2D_Color32(0xEE, 0xF2, 0xF6, 0xFF));
	C2D_SceneBegin(bottom);
	app_draw_bottom();
	gfx3ds_frame_end();
	C3D_FrameEnd(0);
}

int main(void)
{
	gfxInitDefault();
	gfxSet3D(false);
	romfsInit();
	// Four times citro3d's default command buffer, and room for ~16k quads of vertices:
	// the default command buffer overflowed (a svcBreak crash) on busy frames.
	C3D_Init(4 * C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(16384);
	C2D_Prepare();
	C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	gfx3ds_init();

	osSetSpeedupEnable(true); // New 3DS: use the faster CPU mode for decoding
	have_ptmu = R_SUCCEEDED(ptmuInit());
	have_gsplcd = R_SUCCEEDED(gspLcdInit());
	have_ndmu = R_SUCCEEDED(ndmuInit());
	hidSetRepeatParameters(22, 6);

	bool net_ok = net_init() == 0;
	store_init();
	// Start the DSP once, up front: starting it later with the lid already shut can stay silent.
	bool sound_ok = player_init();
	dl_init();
	app_init(sound_ok);
	(void)net_ok; // without networking, requests simply fail with a readable error

	aptHookCookie cookie;
	aptHook(&cookie, apt_hook, NULL);

	while (aptMainLoop()) {
		hidScanInput();
		bool open = shell_open();
		Input in = {0};
		u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp(), rep = hidKeysDownRepeat();
		if (!open) {
			// Lid shut: only the shoulder buttons are reachable (skip back / forward).
			u32 mask = KEY_L | KEY_R | KEY_ZL | KEY_ZR;
			down &= mask; held &= mask; up &= mask; rep &= mask;
		}
		in.down = map_keys(down);
		in.held = map_keys(held);
		in.up = map_keys(up);
		in.repeat = map_keys(rep);
		if (held & KEY_TOUCH) {
			touchPosition tp;
			hidTouchRead(&tp);
			in.tx = tp.px;
			in.ty = tp.py;
		}

		if (!app_update(&in)) break;
		power_update(open);

		if (screens_dark) {
			// Nothing to draw; idle gently to save battery while audio keeps playing.
			svcSleepThread(50 * 1000000LL);
			continue;
		}
		render(top, bottom);
	}

	// Closing: say so on screen straight away, then stop the worker threads. A thread stuck in a
	// network call (a dead connection can block for many seconds) must never keep the app open,
	// so each wait is bounded; anything still running is abandoned and dies with the process.
	aptUnhook(&cookie);
	awake_set(false);
	screens_set_dark(false);
	app_set_closing();
	render(top, bottom);
	render(top, bottom); // both framebuffers

	bool clean = app_shutdown(); // saves positions and settings before anything else
	clean = dl_exit() && clean;
	clean = player_exit() && clean;
	store_flush();
	if (clean) net_exit(); // otherwise a live thread may still be inside a socket call

	if (have_ndmu) ndmuExit();
	if (have_gsplcd) gspLcdExit();
	if (have_ptmu) ptmuExit();
	gfx3ds_exit();
	C2D_Fini();
	C3D_Fini();
	romfsExit();
	gfxExit();
	return 0;
}
