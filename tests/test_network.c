// Unit tests for network.c's pure logic (signal-strength icon/glyph
// thresholds, throughput formatting) -- no GTK init, no Wayland, no nmcli
// process. #includes the plugin source directly to reach its `static`
// functions without changing their visibility for production code; this
// file supplies its own main() (network.c has none), so nothing conflicts.
//
// parse_state/read_throughput/update_bar aren't unit-testable this way:
// parse_state unconditionally calls read_throughput (real /proc/net/dev
// I/O, a hardcoded path like sysmon's /proc parsing) and update_bar (real
// GTK widget touching), so the JSON^H^H^Hpipe-parsing logic here isn't
// cleanly separated from those side effects.
#include "../src/network.c"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
	else { printf("ok - %s\n", msg); } \
} while (0)
#define CHECK_STR(a, b, msg) CHECK(strcmp((a), (b)) == 0, msg)

int main(void) {
	// wifi_icon_name: 3-tier bar-icon threshold
	CHECK_STR(wifi_icon_name(0), "wifi1.svg", "wifi_icon_name(0) is the weakest icon");
	CHECK_STR(wifi_icon_name(33), "wifi1.svg", "wifi_icon_name(33) is still the weakest icon (boundary)");
	CHECK_STR(wifi_icon_name(34), "wifi2.svg", "wifi_icon_name(34) crosses into the mid icon (boundary)");
	CHECK_STR(wifi_icon_name(66), "wifi2.svg", "wifi_icon_name(66) is still the mid icon (boundary)");
	CHECK_STR(wifi_icon_name(67), "wifi3.svg", "wifi_icon_name(67) crosses into the strongest icon (boundary)");
	CHECK_STR(wifi_icon_name(100), "wifi3.svg", "wifi_icon_name(100) is the strongest icon");

	// wifi_glyph: 5-tier popover glyph threshold (including the "off" sentinel)
	CHECK_STR(wifi_glyph(-1), IC_OFF, "wifi_glyph(-1) is the off glyph (disconnected sentinel)");
	CHECK_STR(wifi_glyph(0), "\xf3\xb0\xa4\x9f", "wifi_glyph(0) is strength-1");
	CHECK_STR(wifi_glyph(24), "\xf3\xb0\xa4\x9f", "wifi_glyph(24) is still strength-1 (boundary)");
	CHECK_STR(wifi_glyph(25), "\xf3\xb0\xa4\xa2", "wifi_glyph(25) crosses into strength-2 (boundary)");
	CHECK_STR(wifi_glyph(50), "\xf3\xb0\xa4\xa5", "wifi_glyph(50) crosses into strength-3 (boundary)");
	CHECK_STR(wifi_glyph(75), "\xf3\xb0\xa4\xa8", "wifi_glyph(75) crosses into strength-4 (boundary)");
	CHECK_STR(wifi_glyph(100), "\xf3\xb0\xa4\xa8", "wifi_glyph(100) is still strength-4");

	// fmt_rate: byte-rate unit scaling
	char buf[24];
	fmt_rate(500, buf, sizeof buf);
	CHECK_STR(buf, "500 B/s", "fmt_rate(500) stays in bytes/s");
	fmt_rate(1023, buf, sizeof buf);
	CHECK_STR(buf, "1023 B/s", "fmt_rate(1023) is still bytes/s (just under the KB boundary)");
	fmt_rate(1024, buf, sizeof buf);
	CHECK_STR(buf, "1.0 KB/s", "fmt_rate(1024) crosses into KB/s");
	fmt_rate(1536, buf, sizeof buf);
	CHECK_STR(buf, "1.5 KB/s", "fmt_rate(1536) formats fractional KB/s");
	fmt_rate(1048576, buf, sizeof buf);
	CHECK_STR(buf, "1.0 MB/s", "fmt_rate(1048576) crosses into MB/s");
	fmt_rate(1073741824.0, buf, sizeof buf);
	CHECK_STR(buf, "1.0 GB/s", "fmt_rate(1073741824) crosses into GB/s");

	printf("----\n%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
