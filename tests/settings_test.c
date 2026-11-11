// Checks that every setting survives closing and reopening the app: run once with "write"
// (changes every setting and saves), then again with "read" (a fresh process loads them).
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../source/store.h"

int main(int argc, char** argv)
{
	mkdir(APP_DIR, 0755);
	store_init();
	if (argc > 1 && !strcmp(argv[1], "write")) {
		if (settings.theme != THEME_LIGHT || settings.lid_buttons || !settings.verify_tls || settings.skip_back != 15 || settings.skip_fwd != 30) {
			printf("FAIL: unexpected defaults\n");
			return 1;
		}
		settings.theme = THEME_AUTO;
		settings.lid_buttons = true;
		settings.verify_tls = false;
		settings.skip_back = 30;
		settings.skip_fwd = 60;
		settings_touch();
		store_flush();
		printf("wrote settings\n");
		return 0;
	}
	bool ok = settings.theme == THEME_AUTO && settings.lid_buttons && !settings.verify_tls && settings.skip_back == 30 && settings.skip_fwd == 60;
	printf("%s: theme=%d lid_buttons=%d verify_tls=%d skip_back=%d skip_fwd=%d\n", ok ? "PASS" : "FAIL", settings.theme, settings.lid_buttons,
	       settings.verify_tls, settings.skip_back, settings.skip_fwd);
	return ok ? 0 : 1;
}
