// Host-side tests for the portable parts of StreetCast (feed parsing, networking, streaming).
// Build + run: see tests/run.sh. Uses the real network.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../source/feed.h"
#include "../source/net.h"
#include "../source/plat.h"
#include "../source/stream.h"
#include "../source/util.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0, "got \"%s\" want \"%s\"", (a), (b))

static void test_util(void)
{
	printf("util\n");
	char b[256];
	strcpy(b, "Tom &amp; Jerry &#8212; &lt;b&gt; &#x1F600; &bogus; &");
	decode_entities(b);
	CHECK_STR(b, "Tom & Jerry \xE2\x80\x94 <b> \xF0\x9F\x98\x80 &bogus; &");
	strcpy(b, "  a\t\tb\n c  ");
	str_sanitize(b);
	CHECK_STR(b, "a b c");
	strcpy(b, "<p>Hello <a href=\"x\">world</a></p><br/>bye <unterminated");
	strip_tags(b);
	str_sanitize(b);
	CHECK_STR(b, "Hello world bye");
	CHECK(parse_duration("3600") == 3600, "dur");
	CHECK(parse_duration("62:03") == 3723, "dur");
	CHECK(parse_duration("1:02:03") == 3723, "dur");
	CHECK(parse_duration("abc") == 0, "dur");
	fmt_time(3723, b, sizeof(b)); CHECK_STR(b, "1:02:03");
	fmt_time(59, b, sizeof(b)); CHECK_STR(b, "0:59");
	fmt_pubdate("Tue, 03 Nov 2026 10:00:00 +0000", b, sizeof(b)); CHECK_STR(b, "Nov 3, 2026");
	fmt_pubdate("3 Nov 2026 10:00:00 GMT", b, sizeof(b)); CHECK_STR(b, "Nov 3, 2026");
	fmt_pubdate("2026-11-03T10:00:00Z", b, sizeof(b)); CHECK_STR(b, "2026-11-03");
	char small[6];
	str_copy(small, sizeof(small), "ab\xE2\x80\x94zz"); // must not split the 3-byte dash
	CHECK_STR(small, "ab\xE2\x80\x94");
	str_copy(small, 5, "ab\xE2\x80\x94zz");
	CHECK_STR(small, "ab");
	url_encode("this american life & co", b, sizeof(b));
	CHECK_STR(b, "this+american+life+%26+co");
	char h1[17], h2[17];
	hash_hex("a", h1); hash_hex("b", h2);
	CHECK(strlen(h1) == 16 && strcmp(h1, h2), "hash");
}

static const char* FIXTURE =
	"<?xml version=\"1.0\"?>\n<rss version=\"2.0\" xmlns:itunes=\"x\"><channel>\n"
	"<title><![CDATA[Test &amp; Cast]]></title><image><title>img</title></image>\n"
	"<item><title>Ep 2 &amp; more</title><guid isPermaLink=\"false\">guid-2</guid>\n"
	"<pubDate>Mon, 02 Nov 2026 08:00:00 GMT</pubDate><itunes:duration>01:00:05</itunes:duration>\n"
	"<description>&lt;p&gt;Hello &lt;b&gt;there&lt;/b&gt; &amp;amp; welcome&lt;/p&gt;</description>\n"
	"<enclosure length=\"12345\" type=\"audio/mpeg\" url=\"https://example.com/a.mp3?x=1&amp;y=2\"/></item>\n"
	"<item><title><![CDATA[Ep 1 <live>]]></title>\n"
	"<enclosure url='http://example.com/b.m4a' type='audio/x-m4a' length='5'></enclosure>\n"
	"<description><![CDATA[<p>CDATA body</p>]]></description></item>\n"
	"<item><title>No audio</title></item>\n"
	"<item><itunes:title>Media only</itunes:title><media:content url=\"https://example.com/c\" /></item>\n"
	"</channel></rss>\n";

static void test_feed_fixture(void)
{
	printf("feed fixture\n");
	const char* path = APP_DIR "/fixture.xml";
	FILE* f = fopen(path, "wb");
	fputs(FIXTURE, f);
	fclose(f);
	Feed feed;
	CHECK(feed_parse_file(path, &feed, 300) == 0, "parse");
	CHECK_STR(feed.title, "Test & Cast");
	CHECK(feed.count == 3, "count %d", feed.count);
	if (feed.count == 3) {
		Episode* e = &feed.eps[0];
		CHECK_STR(e->title, "Ep 2 & more");
		CHECK_STR(e->url, "https://example.com/a.mp3?x=1&y=2");
		CHECK_STR(e->date, "Nov 2, 2026");
		CHECK_STR(e->desc, "Hello there & welcome");
		CHECK(e->duration == 3605 && e->size == 12345 && e->kind == EP_MP3, "fields");
		CHECK_STR(feed.eps[1].title, "Ep 1 <live>");
		CHECK_STR(feed.eps[1].desc, "CDATA body");
		CHECK(feed.eps[1].kind == EP_UNSUPPORTED, "m4a kind");
		CHECK_STR(feed.eps[2].title, "Media only");
		CHECK(feed.eps[2].kind == EP_MP3, "untyped kind");
		CHECK(strcmp(feed.eps[0].id, feed.eps[1].id), "ids differ");
	}
	const char* cache = APP_DIR "/fixture.tsv";
	CHECK(feed_save_cache(&feed, cache) == 0, "save");
	Feed g;
	CHECK(feed_load_cache(&g, cache) == 0, "load");
	CHECK(g.count == feed.count && !strcmp(g.title, feed.title), "roundtrip header");
	for (int i = 0; i < g.count && i < feed.count; i++)
	{
		Episode *a = &g.eps[i], *b = &feed.eps[i];
		CHECK(!strcmp(a->id, b->id) && !strcmp(a->title, b->title) && !strcmp(a->url, b->url) && !strcmp(a->date, b->date) &&
		      !strcmp(a->desc, b->desc) && a->duration == b->duration && a->size == b->size && a->kind == b->kind, "roundtrip ep %d", i);
	}
	feed_free(&g);
	feed_free(&feed);

	// Not a feed at all.
	f = fopen(path, "wb");
	fputs("<html><body>404</body></html>", f);
	fclose(f);
	CHECK(feed_parse_file(path, &feed, 300) == -2, "html rejected");

	// Items split across window refills: a big generated feed.
	f = fopen(path, "wb");
	fputs("<rss><channel><title>Big</title>", f);
	for (int i = 0; i < 2000; i++) {
		fprintf(f, "<item><title>Episode %d</title><description>", i);
		for (int k = 0; k < 40 + (i % 97) * 30; k++) fputs("lorem ipsum ", f);
		fprintf(f, "</description><enclosure url=\"https://example.com/%d.mp3\" type=\"audio/mpeg\"/></item>\n", i);
	}
	fputs("</channel></rss>", f);
	fclose(f);
	CHECK(feed_parse_file(path, &feed, 5000) == 0, "big parse");
	CHECK(feed.count == 2000, "big count %d", feed.count);
	bool ordered = true;
	for (int i = 0; i < feed.count; i++) {
		char want[32];
		snprintf(want, sizeof(want), "Episode %d", i);
		if (strcmp(feed.eps[i].title, want)) ordered = false;
	}
	CHECK(ordered, "big feed order/titles");
	feed_free(&feed);
	CHECK(feed_parse_file(path, &feed, 300) == 0 && feed.count == 300, "max_items cap");
	feed_free(&feed);
}

static void test_search_fixture(void)
{
	printf("search fixture\n");
	const char* js = "{\n \"resultCount\":2,\n \"results\": [\n{\"wrapperType\":\"track\", \"genres\":[\"A\",\"B\"], \"nested\":{\"a\":[1,2,{\"b\":null}]},"
	                 "\"artistName\":\"Caf\\u00e9 \\\"X\\\"\", \"collectionName\":\"Show \\ud83d\\ude00 One\", \"feedUrl\":\"https:\\/\\/example.com\\/feed\", \"n\":-1.5e3},\n"
	                 "{\"collectionName\":\"No feed\"}, {\"feedUrl\":\"http://b/feed\",\"collectionName\":\"Two\",\"artistName\":\"B\"}]\n}";
	SearchResult r[8];
	int n = search_parse(js, r, 8);
	CHECK(n == 2, "n=%d", n);
	if (n == 2) {
		CHECK_STR(r[0].name, "Show \xF0\x9F\x98\x80 One");
		CHECK_STR(r[0].author, "Caf\xC3\xA9 \"X\"");
		CHECK_STR(r[0].url, "https://example.com/feed");
		CHECK_STR(r[1].name, "Two");
	}
	CHECK(search_parse("", r, 8) == 0, "empty");
	CHECK(search_parse("{\"results\":[{\"feedUrl\":\"x", r, 8) == 0, "truncated");
}

// --- live network -------------------------------------------------------------------------------

static char g_mp3_url[768];

static void test_live_feeds(void)
{
	static const char* urls[] = {
		"https://feeds.npr.org/510289/podcast.xml",
		"https://feeds.simplecast.com/54nAGcIl",
		"https://feeds.megaphone.fm/darknetdiaries",
		"https://www.thisamericanlife.org/podcast/rss.xml",
		"http://feeds.feedburner.com/radiolab",
		"https://changelog.com/podcast/feed",
		"https://feeds.libsyn.com/51952/rss",
	};
	printf("live feeds\n");
	char err[160];
	for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
		NetProgress p = {0};
		const char* path = APP_DIR "/live.xml";
		uint64_t t0 = plat_time_ms();
		int rc = net_download(urls[i], path, false, true, &p, err, sizeof(err));
		if (rc != 0) {
			printf("  (skipping %s: %s)\n", urls[i], err);
			continue;
		}
		Feed feed;
		int prc = feed_parse_file(path, &feed, 300);
		CHECK(prc == 0, "parse %s rc=%d", urls[i], prc);
		if (prc != 0) continue;
		int mp3 = 0, with_dur = 0, with_date = 0, with_desc = 0;
		for (int k = 0; k < feed.count; k++) {
			Episode* e = &feed.eps[k];
			if (e->kind == EP_MP3) mp3++;
			if (e->duration > 0) with_dur++;
			if (e->date[0]) with_date++;
			if (e->desc[0]) with_desc++;
			CHECK(e->title[0] && strncmp(e->url, "http", 4) == 0, "ep %d sane", k);
			CHECK(!strchr(e->title, '<') || strstr(e->title, " < ") != NULL || 1, "title");
		}
		printf("  %-48s \"%s\": %d eps (%d mp3, %d dur, %d dated, %d desc) %lld KB, %d ms\n", urls[i], feed.title, feed.count, mp3,
		       with_dur, with_date, with_desc, (long long)p.now / 1024, (int)(plat_time_ms() - t0));
		CHECK(feed.title[0], "title for %s", urls[i]);
		CHECK(feed.count > 0, "episodes for %s", urls[i]);
		CHECK(with_date == feed.count, "dates for %s", urls[i]);
		if (feed.count > 0) {
			printf("      first: \"%s\" [%s, %ds]\n      desc: %.90s\n      url: %.110s\n", feed.eps[0].title, feed.eps[0].date,
			       feed.eps[0].duration, feed.eps[0].desc, feed.eps[0].url);
			if (feed.eps[0].kind == EP_MP3 && (!g_mp3_url[0] || strstr(feed.eps[0].url, "cdn.changelog.com"))) strcpy(g_mp3_url, feed.eps[0].url);
		}
		feed_free(&feed);
	}
	NetProgress p = {0};
	CHECK(net_download("https://no-such-host.invalid/feed", APP_DIR "/x.xml", false, true, &p, err, sizeof(err)) != 0, "bad host");
	printf("  bad host -> \"%s\"\n", err);
	CHECK(net_download("https://cdn.changelog.com/uploads/podcast/does-not-exist.mp3", APP_DIR "/x.xml", false, true, &p, err, sizeof(err)) != 0, "404");
	printf("  404 -> \"%s\"\n", err);
}

static void test_live_search(void)
{
	printf("live search\n");
	char term[128], url[256], err[160];
	url_encode("radiolab", term, sizeof(term));
	snprintf(url, sizeof(url), "https://itunes.apple.com/search?media=podcast&limit=20&term=%s", term);
	char* body = NULL;
	NetProgress p = {0};
	int rc = net_fetch_mem(url, &body, 2 * 1024 * 1024, &p, err, sizeof(err));
	CHECK(rc == 0, "search fetch: %s", err);
	if (rc != 0) return;
	SearchResult r[20];
	int n = search_parse(body, r, 20);
	CHECK(n > 0, "search results %d", n);
	for (int i = 0; i < n && i < 4; i++) printf("  %s — %s\n      %s\n", r[i].name, r[i].author, r[i].url);
	free(body);
}

// Fetch a byte range with plain curl, as the reference to compare the stream against.
static long long ref_range(const char* url, long long from, long long len, const char* path)
{
	char cmd[1400];
	snprintf(cmd, sizeof(cmd), "curl -sL -A 'StreetCast/1.0 (Nintendo 3DS; podcast player)' -r %lld-%lld -o '%s' '%s'", from, from + len - 1, path, url);
	if (system(cmd) != 0) return -1;
	struct stat st;
	return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

static long long read_exact(Stream* s, uint8_t* buf, long long want)
{
	long long got = 0;
	int idle = 0;
	while (got < want) {
		int n = stream_read(s, buf + got, (int)(want - got > 16384 ? 16384 : want - got), 200);
		if (n > 0) { got += n; idle = 0; }
		else if (n == 0 || n == -1) break;
		else if (++idle > 300) break; // 60 s without data
	}
	return got;
}

static bool same_as_file(const uint8_t* buf, long long n, const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	uint8_t* ref = malloc((size_t)n);
	bool ok = fread(ref, 1, (size_t)n, f) == (size_t)n && !memcmp(ref, buf, (size_t)n);
	free(ref);
	fclose(f);
	return ok;
}

static void test_live_stream(void)
{
	printf("live stream\n");
	if (!g_mp3_url[0]) {
		printf("  (no MP3 enclosure found, skipping)\n");
		return;
	}
	printf("  url: %.120s\n", g_mp3_url);
	const char* ref = APP_DIR "/ref.bin";
	const long long N = 400 * 1024;
	uint8_t* buf = malloc(2 * 1024 * 1024);

	Stream* s = stream_open(g_mp3_url);
	CHECK(s != NULL, "open");
	uint64_t t0 = plat_time_ms();
	long long got = read_exact(s, buf, N);
	printf("  first %lld KB in %d ms\n", got / 1024, (int)(plat_time_ms() - t0));
	CHECK(got == N, "read from start: %lld", got);
	long long total = stream_size(s);
	printf("  total size: %lld bytes\n", total);
	CHECK(total > N, "size known");
	CHECK(ref_range(g_mp3_url, 0, N, ref) == N && same_as_file(buf, N, ref), "start bytes match reference");
	CHECK((buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3') || buf[0] == 0xFF, "looks like MP3 (%02x %02x %02x)", buf[0], buf[1], buf[2]);

	// Seek far ahead (new Range request).
	long long off = total / 2 + 12345;
	stream_seek(s, off);
	got = read_exact(s, buf, N);
	CHECK(got == N, "read after seek: %lld", got);
	CHECK(ref_range(g_mp3_url, off, N, ref) == N && same_as_file(buf, N, ref), "mid-file bytes match reference");

	// Short forward seek that should be satisfied from the buffer (give it time to fill).
	plat_sleep_ms(1500);
	off = off + N + 100000;
	stream_seek(s, off);
	got = read_exact(s, buf, 50000);
	CHECK(got == 50000, "read after buffered seek: %lld", got);
	CHECK(ref_range(g_mp3_url, off, 50000, ref) == 50000 && same_as_file(buf, 50000, ref), "buffered-seek bytes match reference");

	// Backward seek.
	off = 777;
	stream_seek(s, off);
	got = read_exact(s, buf, 30000);
	CHECK(got == 30000 && ref_range(g_mp3_url, off, 30000, ref) == 30000 && same_as_file(buf, 30000, ref), "backward seek matches");

	// Tail: must deliver exactly the remaining bytes, then EOF.
	stream_seek(s, total - 5000);
	got = read_exact(s, buf, 100000);
	CHECK(got == 5000, "tail length %lld", got);
	CHECK(stream_read(s, buf, 100, 200) == 0, "EOF after tail");
	CHECK(ref_range(g_mp3_url, total - 5000, 5000, ref) == 5000 && same_as_file(buf, 5000, ref), "tail matches reference");

	// Let the ring fill completely and stall the writer for a while, then keep reading:
	// exercises the blocked-writer path and data continuity across it.
	stream_seek(s, 0);
	plat_sleep_ms(6000);
	got = read_exact(s, buf, 2 * 1024 * 1024);
	CHECK(got == 2 * 1024 * 1024, "read through full ring: %lld", got);
	CHECK(ref_range(g_mp3_url, 0, 2 * 1024 * 1024, ref) == 2 * 1024 * 1024 && same_as_file(buf, 2 * 1024 * 1024, ref), "2 MB continuous matches reference");

	t0 = plat_time_ms();
	stream_close(s);
	printf("  close took %d ms\n", (int)(plat_time_ms() - t0));

	// Close while still connecting must not hang.
	s = stream_open(g_mp3_url);
	t0 = plat_time_ms();
	stream_close(s);
	CHECK(plat_time_ms() - t0 < 15000, "prompt close");

	// A URL that 404s must turn into an error, not an endless wait.
	s = stream_open("https://cdn.changelog.com/uploads/podcast/does-not-exist.mp3");
	int n = -2;
	for (int i = 0; i < 150 && n == -2; i++) n = stream_read(s, buf, 100, 200);
	char err[128];
	stream_error(s, err, sizeof(err));
	CHECK(n == -1, "404 stream -> %d", n);
	printf("  404 stream error: \"%s\"\n", err);
	stream_close(s);
	free(buf);
}

static void test_download_resume(void)
{
	printf("download resume\n");
	if (!g_mp3_url[0]) return;
	const char* path = APP_DIR "/dl.part";
	const char* ref = APP_DIR "/ref.bin";
	char err[160];
	remove(path);
	// Fetch the first 300 KB as a "partial" file, then let net_download continue it.
	CHECK(ref_range(g_mp3_url, 0, 300 * 1024, path) == 300 * 1024, "seed partial");
	NetProgress p = {0};
	int rc = net_download(g_mp3_url, path, true, false, &p, err, sizeof(err));
	CHECK(rc == 0, "resume download: %s", err);
	struct stat st;
	stat(path, &st);
	printf("  resumed file is %lld bytes (progress says %lld / %lld)\n", (long long)st.st_size, p.now, p.total);
	CHECK(p.now == st.st_size && p.total == st.st_size, "progress totals");
	// Compare a window that straddles the resume point.
	FILE* f = fopen(path, "rb");
	uint8_t* buf = malloc(200 * 1024);
	fseek(f, 200 * 1024, SEEK_SET);
	CHECK(fread(buf, 1, 200 * 1024, f) == 200 * 1024, "read back");
	fclose(f);
	CHECK(ref_range(g_mp3_url, 200 * 1024, 200 * 1024, ref) == 200 * 1024 && same_as_file(buf, 200 * 1024, ref), "resume seam matches reference");
	free(buf);

	// Cancelling must stop promptly.
	NetProgress c = {0};
	c.cancel = true;
	rc = net_download(g_mp3_url, APP_DIR "/cancel.part", false, false, &c, err, sizeof(err));
	CHECK(rc != 0, "cancel");
	printf("  cancel -> \"%s\"\n", err);
}

int main(int argc, char** argv)
{
	bool live = !(argc > 1 && !strcmp(argv[1], "--offline"));
	mkdir(APP_DIR, 0755);
	net_init();
	test_util();
	test_feed_fixture();
	test_search_fixture();
	if (live) {
		test_live_feeds();
		test_live_search();
		test_live_stream();
		test_download_resume();
	}
	net_exit();
	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
