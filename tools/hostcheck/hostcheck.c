/*
 * Host-side checks for the Playdate port: the real engine and the real
 * port/boards/playdate/display.c, run headless on the development machine.
 *
 *  golden mode (HGOLD=<file>):  plays demo1..3 back with `timedemo` and writes one hash of the
 *      1-bit LCD image per frame, plus one of the 8-bit render buffer and z buffer. Build the tree before and after an optimisation and compare
 *      the two files (tools/hostcheck/golden-compare.py): equal hashes = identical pictures.
 *
 *  menu mode (HMENU=1):  keys through the options menu, see menu_test().
 *
 *  default mode:  scripted play (walking, console, menus, HUD, view sizes, demos) that, on every
 *      screen update, also checks the lazy low-res upscale (see D_UpscaleScreen in d_scan.c):
 *        1. no view row pair that is still "pending" may differ from the snapshot taken when the
 *           half-resolution view was handed over, i.e. no draw primitive wrote into it without
 *           calling DRAW_TOUCH first;
 *        2. the LCD image produced by dithering pending rows straight from the half-resolution
 *           buffer is byte-identical to the one produced the old way (expand the view into the
 *           frame buffer, then dither).
 *      Built with -DNO_LAZY_CHECK for trees that predate the lazy upscale (golden mode only).
 */
#include <stdio.h>
#include "pd_api.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <quakedef.h>
#include <quakembd.h>
#include <d_local.h>

#define W 400
#define H 240
#define FRAME_BYTES (LCD_ROWSIZE * LCD_ROWS)

PlaydateAPI *qembd_pd;
static struct playdate_graphics gfx;
static PlaydateAPI api;
static uint8_t frame_new[FRAME_BYTES], frame_old[FRAME_BYTES];
static uint8_t *cur_frame;
static long frame_no;
static int cur_scn;
static const char *phase = "";
long checks, pending_pairs_checked, flushed_pairs, mismatches, hook_violations;

static uint8_t *get_frame(void) { return cur_frame; }
static void mark_rows(int a, int b) { (void)a; (void)b; }

/* display.c is compiled twice, as tree_* (the tree under test) and old_* (expand-then-dither) */
void tree_fillrect(uint8_t *, uint32_t *, uint16_t, uint16_t, uint16_t, uint16_t);
void tree_vidinit(void);
int tree_get_width(void);
int tree_get_height(void);

int qembd_get_width(void) { return tree_get_width(); }
int qembd_get_height(void) { return tree_get_height(); }
void qembd_refresh(void) {}

#ifndef NO_LAZY_CHECK
void old_fillrect(uint8_t *, uint32_t *, uint16_t, uint16_t, uint16_t, uint16_t);
void old_vidinit(void);
void real_D_UpscaleScreen(void);
extern int qembd_lowres_rect[4];
extern int qembd_lowres_active;
extern uint8_t qembd_lowres_pending[];
static uint8_t snapshot[W * H];
static int snap_valid;

void D_UpscaleScreen(void)
{
	real_D_UpscaleScreen();
	memcpy(snapshot, vid.buffer, W * H);
	snap_valid = 1;
}

void qembd_vidinit(void)
{
	cur_frame = frame_new;
	tree_vidinit();
	cur_frame = frame_old;
	old_vidinit();
}

void qembd_fillrect(uint8_t *src, uint32_t *clut, uint16_t x, uint16_t y, uint16_t xs, uint16_t ys)
{
	checks++;
	if (qembd_lowres_active && snap_valid) {
		int p, ry0 = qembd_lowres_rect[1], ry1 = ry0 + qembd_lowres_rect[3], rx0 = qembd_lowres_rect[0], rw = qembd_lowres_rect[2];

		for (p = ry0 >> 1; p < ry1 >> 1; p++) {
			if (!qembd_lowres_pending[p]) {
				flushed_pairs++;
				continue;
			}
			pending_pairs_checked++;
			if (memcmp(vid.buffer + 2 * p * W + rx0, snapshot + 2 * p * W + rx0, rw) ||
				memcmp(vid.buffer + (2 * p + 1) * W + rx0, snapshot + (2 * p + 1) * W + rx0, rw)) {
				if (++hook_violations < 6)
					printf("HOOK VIOLATION scn=%d %s frame %ld: pending pair %d was written without DRAW_TOUCH\n", cur_scn, phase, frame_no, p);
			}
		}
	}

	/* new path: pending rows dithered straight from the half-resolution buffer */
	cur_frame = frame_new;
	tree_fillrect(src, clut, x, y, xs, ys);

	/* old path: expand every still-pending row pair into the buffer, then dither it */
	if (qembd_lowres_active)
		D_LowresTouch(0, H);
	cur_frame = frame_old;
	old_fillrect(src, clut, x, y, xs, ys);

	if (memcmp(frame_new, frame_old, FRAME_BYTES)) {
		int i;

		for (i = 0; i < FRAME_BYTES && frame_new[i] == frame_old[i]; i++)
			;
		if (++mismatches < 6)
			printf("LCD MISMATCH scn=%d %s frame %ld byte %d (row %d col %d): new %02x old %02x\n", cur_scn, phase, frame_no, i,
				   i / LCD_ROWSIZE, i % LCD_ROWSIZE, frame_new[i], frame_old[i]);
		memcpy(frame_old, frame_new, FRAME_BYTES); /* resync so one bug is reported once */
	}
}
#else
void qembd_vidinit(void)
{
	cur_frame = frame_new;
	tree_vidinit();
}

void qembd_fillrect(uint8_t *src, uint32_t *clut, uint16_t x, uint16_t y, uint16_t xs, uint16_t ys)
{
	cur_frame = frame_new;
	tree_fillrect(src, clut, x, y, xs, ys);
}
#endif

/* ------------------------------------------------------------------ stubs */
static uint64_t fake_us;
uint64_t qembd_get_us_time(void) { return fake_us; }
void qembd_udelay(uint32_t us) { fake_us += us; }
void *qembd_allocmain(size_t size)
{
	static byte pool[8 * 1024 * 1024] __attribute__((aligned(16)));
	return size <= sizeof(pool) ? pool : NULL;
}
void qembd_log(const char *t) { if (getenv("HLOG")) fputs(t, stdout); }
void qembd_fatal(const char *m) { printf("FATAL: %s\n", m); exit(2); }
void qembd_quit(void) { exit(0); }
int qembd_dequeue_key_event(key_event_t *e) { (void)e; return -1; }
int qembd_get_mouse_movement(mouse_movement_t *m) { (void)m; return -1; }
void qembd_set_relative_mode(bool e) { (void)e; }

cvar_t bgmvolume = {"bgmvolume", "1", true};
cvar_t volume = {"volume", "0.7", true};
vec_t sound_nominal_clip_dist = 1000.0;
void S_Init(void) {}
void S_Startup(void) {}
void S_Shutdown(void) {}
void S_StartSound(int a, int b, sfx_t *s, vec3_t o, float v, float at) {}
void S_StaticSound(sfx_t *s, vec3_t o, float v, float a) {}
void S_StopSound(int a, int b) {}
void S_StopAllSounds(qboolean c) {}
void S_ClearBuffer(void) {}
void S_Update(vec3_t o, vec3_t f, vec3_t r, vec3_t u) {}
void S_ExtraUpdate(void) {}
sfx_t *S_PrecacheSound(char *s) { return NULL; }
void S_TouchSound(char *s) {}
void S_ClearPrecache(void) {}
void S_BeginPrecaching(void) {}
void S_EndPrecaching(void) {}
void S_LocalSound(char *s) {}
void S_AmbientOff(void) {}
void S_AmbientOn(void) {}

/* ------------------------------------------------------------------ driver */
static void cmd(const char *s) { Cbuf_AddText((char *)s); }

static void run(int n)
{
	while (n-- > 0) {
		fake_us += 33000;
		qembd_frame();
		frame_no++;
	}
}

static void step(const char *name, const char *c, int n)
{
	phase = name;
	if (c)
		cmd(c);
	run(n);
}

static uint32_t hash_frame(void)
{
	uint32_t h = 2166136261u;
	int k;

	for (k = 0; k < FRAME_BYTES; k++)
		h = (h ^ frame_new[k]) * 16777619u;
	return h;
}

/* the 8-bit render buffer and the z buffer, which see differences the 1-bit dither hides */
static uint32_t hash_render(void)
{
	uint32_t h = 2166136261u;
	const uint8_t *z = (const uint8_t *)d_pzbuffer;
	int k, n = vid.width * vid.height * 2;

	for (k = 0; k < W * H; k++)
		h = (h ^ vid.buffer[k]) * 16777619u;
	for (k = 0; k < n; k++)
		h = (h ^ z[k]) * 16777619u;
	return h;
}


static int golden(const char *path)
{
	FILE *g = fopen(path, "w");
	int d;

	for (d = 1; d <= 3; d++) {
		int started = 0, i;
		char c[32];

		snprintf(c, sizeof(c), "timedemo demo%d\n", d);
		cmd(c);
		for (i = 0; i < 5000; i++) {
			run(1);
			if (cls.timedemo) {
				started = 1;
				key_dest = key_game; /* keep the console off the view */
			} else if (started)
				break;
			fprintf(g, "demo%d %d %08x %08x\n", d, i, hash_frame(), hash_render());
		}
		fprintf(g, "demo%d done after %d frames\n", d, i);
	}
	fclose(g);
	printf("golden: %ld frames written to %s\n", frame_no, path);
	return 0;
}

/*
 * HMENU=1: the options menu. Presses real keys (Key_Event) and checks navigation, the Texture
 * detail row (d_mipcap), that leaving the menu writes config.cfg with the archived cvars but no key
 * bindings, that the file reloads, and that "Reset to defaults" puts the option back.
 */
extern int options_cursor;
extern cvar_t d_mipcap;

static int menu_fail;
#define MCHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); menu_fail++; } else printf("ok:   %s\n", what); } while (0)

static void press(int key)
{
	Key_Event(key, true);
	Key_Event(key, false);
	run(2);
}

static char *slurp(const char *path)
{
	static char buf[16384];
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return NULL;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = 0;
	fclose(f);
	return buf;
}

/* HMENUPIC=<prefix> also dumps the 1-bit LCD as <prefix>-<name>.pbm (viewable / convertible) */
static void dump_lcd(const char *name)
{
	const char *prefix = getenv("HMENUPIC");
	char path[512];
	FILE *f;
	int y, x;

	if (!prefix)
		return;
	snprintf(path, sizeof(path), "%s-%s.pbm", prefix, name);
	f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P4\n%d %d\n", W, H);
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x += 8) {
			uint8_t b = frame_new[y * LCD_ROWSIZE + x / 8];

			fputc((uint8_t)~b, f);	/* LCD: 1 = white; PBM: 1 = black */
		}
	fclose(f);
}

static int menu_test(void)
{
	char path[256];
	char *cfg;
	int i;

	snprintf(path, sizeof(path), "%s/config.cfg", com_gamedir);
	remove(path);
	cmd("map e1m1\n");
	run(30);
	cmd("menu_options\n");
	run(3);
	MCHECK(key_dest == key_menu, "options menu opens");
	MCHECK(options_cursor == 0, "cursor starts on the first row");

	for (i = 0; i < 13; i++)
		press(K_DOWNARROW);
	MCHECK(options_cursor == 13, "13 x down reaches the Texture detail row");
	MCHECK(d_mipcap.value == 0, "texture detail starts on high (d_mipcap 0)");
	MCHECK(!host_options_dirty, "nothing changed yet");

	dump_lcd("high");
	press(K_RIGHTARROW);
	dump_lcd("low");
	MCHECK(d_mipcap.value == 1, "right: low (d_mipcap 1)");
	MCHECK(host_options_dirty, "the change marks the options dirty");
	press(K_LEFTARROW);
	MCHECK(d_mipcap.value == 0, "left: high again");
	press(K_ENTER);
	MCHECK(d_mipcap.value == 1, "enter toggles too");

	press(K_DOWNARROW);
	MCHECK(options_cursor == 0, "down from the last row wraps to the first (no Video Options row here)");
	press(K_UPARROW);
	MCHECK(options_cursor == 13, "up from the first row lands on Texture detail");

	MCHECK(slurp(path) == NULL, "no config.cfg before leaving the menu");
	press(K_ESCAPE);
	MCHECK(!host_options_dirty, "leaving the menu clears the dirty flag");
	cfg = slurp(path);
	MCHECK(cfg != NULL, "leaving the menu writes config.cfg");
	MCHECK(cfg && strstr(cfg, "d_mipcap \"1"), "config.cfg holds d_mipcap 1");
	MCHECK(cfg && strstr(cfg, "cl_autofire"), "config.cfg holds the other archived options");
	MCHECK(cfg && !strstr(cfg, "bind "), "config.cfg holds no key bindings");

	Cvar_SetValue("d_mipcap", 0);
	cmd("exec config.cfg\n");
	run(3);
	MCHECK(d_mipcap.value == 1, "config.cfg restores d_mipcap 1 (what the next launch does)");

	/* Reset to defaults puts it back */
	cmd("menu_options\n");
	run(3);
	for (i = 0; i < 20 && options_cursor != 2; i++)	/* the cursor keeps its row when the menu reopens */
		press(K_DOWNARROW);
	MCHECK(options_cursor == 2, "cursor on Reset to defaults");
	press(K_ENTER);
	run(3);
	MCHECK(d_mipcap.value == 0, "reset to defaults sets texture detail back to high");
	press(K_ESCAPE);
	cfg = slurp(path);
	MCHECK(cfg && strstr(cfg, "d_mipcap \"0"), "and the saved file says so");

	remove(path);
	printf("%s\n", menu_fail ? "MENU TEST FAILED" : "menu test passed");
	return menu_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
	static char a0[] = "quake";
	static char *av[] = {a0, NULL};
	int frames = argc > 1 ? atoi(argv[1]) : 150;

	gfx.getFrame = get_frame;
	gfx.markUpdatedRows = mark_rows;
	api.graphics = &gfx;
	qembd_pd = &api;
	cur_frame = frame_new;
	if (qembd_init(1, av) != 0)
		return 1;

	if (getenv("HGOLD"))
		return golden(getenv("HGOLD"));
	if (getenv("HMENU"))
		return menu_test();

	/* scenario 1: a level, walking, then overlays and view sizes */
	cur_scn = 1;
	cmd("map e1m1\n");
	step("load", NULL, 60);
	step("walk", "+forward\n+left\n", frames);
	step("fps counter", "scr_showfps 1\n", 40);
	step("notify lines", "echo hello notify line one\necho two\n", 30);
	step("notify decay", NULL, 200);
	step("console open", "toggleconsole\n", 40);
	step("console closed", "toggleconsole\n", 40);
	step("menu main", "menu_main\n", 30);
	step("menu options", "menu_options\n", 30);
	step("menu back to main", "togglemenu\n", 20);
	step("menu closed", "togglemenu\n", 30);
	step("viewsize 60", "viewsize 60\n", 40);
	step("viewsize 80", "viewsize 80\n", 30);
	step("viewsize 110", "viewsize 110\n", 40);
	step("viewsize 100", "viewsize 100\n", 30);
	step("pause", "pause\n", 10);
	step("unpause", "pause\n", 10);
	step("fov 120", "fov 120\n", 20);
	step("impulse 9 (weapons)", "impulse 9\n", 40);
	step("menu help", "menu_help\n", 20);
	step("menu help close", "togglemenu\n", 10);
	step("menu setup (translated pic)", "menu_setup\n", 20);
	step("menu setup close", "togglemenu\ntogglemenu\n", 10);
	step("menu keys", "menu_keys\n", 20);
	step("menu keys close", "togglemenu\ntogglemenu\n", 10);
	step("menu save", "menu_save\n", 20);
	step("menu load", "menu_load\n", 20);
	step("menu load close", "togglemenu\ntogglemenu\n", 10);
	step("menu quit", "menu_quit\n", 20);
	step("menu quit close", "togglemenu\ntogglemenu\n", 10);
	step("scoreboard", "+showscores\n", 30);
	step("scoreboard off", "-showscores\n", 10);
	step("crosshair", "crosshair 1\n", 20);
	step("stop", "-forward\n-left\n", 10);

	/* scenario 2: the attract-mode demos (HUD, weapons, monsters, effects) */
	cur_scn = 2;
	step("demo1", "playdemo demo1\n", frames * 3);
	step("demo2", "playdemo demo2\n", frames * 3);
	step("demo3", "playdemo demo3\n", frames * 3);
	step("demo1 fps counter", "playdemo demo1\nscr_showfps 1\n", frames * 2);

	printf("frames %ld | screen updates %ld | pending row pairs verified %ld | pairs expanded for overlays %ld\n",
		   frame_no, checks, pending_pairs_checked, flushed_pairs);
	printf("hook violations: %ld | LCD mismatches: %ld\n", hook_violations, mismatches);
	return (hook_violations || mismatches) ? 1 : 0;
}
