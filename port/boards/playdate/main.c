/*
 * Playdate entry point: event handler, frame loop, input and timing.
 *
 * Controls
 *   D-pad up/down   walk forward / back      (menus: move)
 *   D-pad left/right turn                    (menus: change value)
 *   A               fire                     (menus: select)
 *   B               jump                     (menus: back)
 *   Crank           turn left / right
 *                   (crank out: D-pad left/right strafe instead of turning)
 *   Crank out       autofire while an enemy is under the crosshair
 *   System menu     "Quake Menu" opens Quake's own menu (autofire is toggled in
 *                   its Options); "Weapon" lists the weapons you can use now.
 * While a demo is playing (title screen) A and B open Quake's menu.
 */

#include <setjmp.h>
#include <string.h>
#include <quakembd.h>
#include <quakedef.h>
#include "pd_port.h"

PlaydateAPI *qembd_pd;

enum { ST_SPLASH, ST_INIT, ST_RUN, ST_STOPPED };

#define CRANK_TURN 1.0f		/* degrees of view turn per degree of crank */
#define RUN_SPEED 400
#define AUTOFIRE_RANGE 2048.0f
#define AUTOFIRE_AXE_RANGE 64.0f

static int state = ST_SPLASH;
static jmp_buf frame_jmp;
static int in_frame;

static int menu_requested;
static PDMenuItem *weapon_item;

/* ---------------------------------------------------------------- time */

uint64_t qembd_get_us_time()
{
	static unsigned int base;
	static int have_base;
	unsigned int ms = qembd_pd->system->getCurrentTimeMilliseconds();

	if (!have_base) {
		base = ms;
		have_base = 1;
	}
	return (uint64_t) (ms - base) * 1000;
}

void qembd_udelay(uint32_t us)
{
	uint64_t end = qembd_get_us_time() + us;

	while (qembd_get_us_time() < end)
		;
}

void *qembd_allocmain(size_t size)
{
#ifdef TARGET_SIMULATOR
	/*
	 * QuakeC strings are 32-bit offsets from pr_strings, and Quake computes
	 * them from pointers into static buffers (sv.name, pr_string_temp, ...).
	 * On a 64-bit host a malloc'd heap can be >2GB away from static data,
	 * which truncates those offsets, so keep the heap in static storage.
	 * (The device is 32-bit and allocates from the system heap below.)
	 */
	static byte pool[8 * 1024 * 1024] __attribute__((aligned(16)));

	return size <= sizeof(pool) ? pool : NULL;
#else
	return qembd_pd->system->realloc(NULL, size);
#endif
}

/* ------------------------------------------------------------- messages */

static void show_message(const char *title, const char *msg)
{
	PlaydateAPI *pd = qembd_pd;

	pd->graphics->clear(kColorWhite);
	pd->graphics->drawText(title, strlen(title), kUTF8Encoding, 20, 20);
	pd->graphics->drawTextInRect(msg, strlen(msg), kUTF8Encoding, 20, 50, 360, 170,
	                             kWrapWord, kAlignTextLeft);
}

static void stop_with_message(const char *title, const char *msg)
{
	state = ST_STOPPED;
	show_message(title, msg);
	if (in_frame) {
		in_frame = 0;
		longjmp(frame_jmp, 1);
	}
}

void qembd_log(const char *text)
{
	pdq_log_line(text);
}

/* Sys_Error: report on screen (and console) and stop running Quake */
void qembd_fatal(const char *msg)
{
	qembd_pd->system->logToConsole("Quake error: %s", msg);
	stop_with_message("Quake stopped:", msg);
	/* Not reached from inside a frame, but Sys_Error must not return */
	for (;;)
		;
}

/* Sys_Quit: there is no process to exit, so just park */
void qembd_quit(void)
{
	stop_with_message("Quake has exited.", "Open the system menu to leave the game.");
	for (;;)
		;
}

/* ---------------------------------------------------------------- input */

#define QUEUE_SIZE 32

static key_event_t queue[QUEUE_SIZE];
static int q_head, q_tail;

static void push_key(uint32_t key, int down)
{
	int next = (q_tail + 1) % QUEUE_SIZE;

	if (next == q_head)
		return;
	queue[q_tail].keycode = key;
	queue[q_tail].state = down;
	q_tail = next;
}

int qembd_dequeue_key_event(key_event_t *e)
{
	if (q_head == q_tail)
		return -1;
	*e = queue[q_head];
	q_head = (q_head + 1) % QUEUE_SIZE;
	return 0;
}

int qembd_get_mouse_movement(mouse_movement_t *movement)
{
	movement->x = movement->y = 0;
	return 0;
}

/* Mouse capture is a desktop-emulator concept (called by menu.c) */
void qembd_set_relative_mode(bool enabled)
{
}

static const struct {
	PDButtons mask;
	int game_key;
	int ui_key;
} buttons[] = {
	{kButtonUp,    K_UPARROW,    K_UPARROW},
	{kButtonDown,  K_DOWNARROW,  K_DOWNARROW},
	{kButtonLeft,  K_LEFTARROW,  K_LEFTARROW},
	{kButtonRight, K_RIGHTARROW, K_RIGHTARROW},
	{kButtonA,     K_CTRL,       K_ENTER},	/* +attack; select */
	{kButtonB,     K_SPACE,      K_ESCAPE},	/* +jump; back */
};
#define NUM_BUTTONS (int) (sizeof(buttons) / sizeof(buttons[0]))

extern cvar_t sv_aim;

static int is_live_monster(edict_t *e)
{
	return ((int) e->v.flags & FL_MONSTER) && e->v.takedamage && e->v.health > 0;
}

/*
 * Would firing right now hit a live monster? Mirrors PF_aim (Quake's
 * vertical auto-aim): a monster counts if it is on the crosshair, or inside
 * the sv_aim cone and visible, so height differences don't matter. The axe
 * doesn't auto-aim, so it only counts what is directly in front within reach.
 * (The single-player server is local, so its edicts can be read directly.)
 */
static int enemy_in_sight(void)
{
	vec3_t fwd, right, up, start, end, dir;
	trace_t tr;
	edict_t *check;
	int i, j;

	if (!sv.active || !sv_player)
		return 0;

	AngleVectors(cl.viewangles, fwd, right, up);

	if ((int) sv_player->v.weapon == IT_AXE) {
		VectorAdd(sv_player->v.origin, sv_player->v.view_ofs, start);
		VectorMA(start, AUTOFIRE_AXE_RANGE, fwd, end);
		tr = SV_Move(start, vec3_origin, vec3_origin, end, MOVE_NORMAL, sv_player);
		return tr.ent && is_live_monster(tr.ent);
	}

	VectorCopy(sv_player->v.origin, start);
	start[2] += 20;

	/* Straight shot */
	VectorMA(start, AUTOFIRE_RANGE, fwd, end);
	tr = SV_Move(start, vec3_origin, vec3_origin, end, MOVE_NORMAL, sv_player);
	if (tr.ent && is_live_monster(tr.ent))
		return 1;

	/* Anything auto-aim could turn toward */
	check = NEXT_EDICT(sv.edicts);
	for (i = 1; i < sv.num_edicts; i++, check = NEXT_EDICT(check)) {
		if (check == sv_player || !is_live_monster(check))
			continue;
		for (j = 0; j < 3; j++)
			end[j] = check->v.origin[j] + 0.5f * (check->v.mins[j] + check->v.maxs[j]);
		VectorSubtract(end, start, dir);
		VectorNormalize(dir);
		if (DotProduct(dir, fwd) < sv_aim.value)
			continue;	/* too far to turn */
		tr = SV_Move(start, vec3_origin, vec3_origin, end, MOVE_NORMAL, sv_player);
		if (tr.ent == check)
			return 1;
	}
	return 0;
}

static void poll_input(void)
{
	static PDButtons prev;
	static int sent[NUM_BUTTONS];	/* key sent on press, released with the same */
	static int auto_down;		/* autofire is holding +attack */
	PDButtons cur;
	int ui = key_dest != key_game;
	int playing = !ui && !cls.demoplayback;

	qembd_pd->system->getButtonState(&cur, NULL, NULL);

	for (int i = 0; i < NUM_BUTTONS; i++) {
		int down = (cur & buttons[i].mask) != 0;
		int was = (prev & buttons[i].mask) != 0;

		if (down && !was) {
			int key = ui ? buttons[i].ui_key : buttons[i].game_key;

			/* With the crank out the crank turns, so left/right strafe */
			if (playing && !qembd_pd->system->isCrankDocked()) {
				if (buttons[i].mask == kButtonLeft)
					key = ',';
				else if (buttons[i].mask == kButtonRight)
					key = '.';
			}

			/* Title-screen demo: any button opens the menu */
			if (!ui && cls.demoplayback && i >= 4)
				key = K_ESCAPE;
			sent[i] = key;
			push_key(key, 1);
		} else if (!down && was) {
			push_key(sent[i], 0);
		}
	}
	prev = cur;

	/* Crank out: hold fire while an enemy is under the crosshair */
	if (cur & kButtonA) {
		auto_down = 0;	/* A owns the fire key; its release will let go */
	} else {
		int want = cl_autofire.value && playing && !qembd_pd->system->isCrankDocked() &&
		           enemy_in_sight();

		if (want != auto_down) {
			push_key(K_CTRL, want);
			auto_down = want;
		}
	}

	if (menu_requested) {
		menu_requested = 0;
		push_key(K_ESCAPE, 1);
		push_key(K_ESCAPE, 0);
	}

	/* Crank turns the player while playing (clockwise = right) */
	if (key_dest == key_game && cls.state == ca_connected && !cls.demoplayback)
		cl.viewangles[YAW] -= qembd_pd->system->getCrankChange() * CRANK_TURN;
}

static void apply_run(void)
{
	Cvar_SetValue("cl_forwardspeed", RUN_SPEED);
	Cvar_SetValue("cl_backspeed", RUN_SPEED);
}

static void menu_quake(void *ud)
{
	menu_requested = 1;
}

/* Weapons in impulse order (impulse 1 = axe ... 8 = thunderbolt) */
static const struct {
	const char *name;
	int item;	/* IT_* bit that says the weapon is owned */
	int ammo_stat;	/* STAT_* holding its ammo, or -1 */
	int ammo_min;	/* ammo needed to select it (Quake refuses otherwise) */
} weapons[] = {
	{"Axe",           IT_AXE,              -1,            0},
	{"Shotgun",       IT_SHOTGUN,          STAT_SHELLS,   1},
	{"Dbl Shotgun",   IT_SUPER_SHOTGUN,    STAT_SHELLS,   2},
	{"Nailgun",       IT_NAILGUN,          STAT_NAILS,    1},
	{"Super Nailgun", IT_SUPER_NAILGUN,    STAT_NAILS,    2},
	{"Grenade",       IT_GRENADE_LAUNCHER, STAT_ROCKETS,  1},
	{"Rocket",        IT_ROCKET_LAUNCHER,  STAT_ROCKETS,  1},
	{"Lightning",     IT_LIGHTNING,        STAT_CELLS,    1},
};
#define NUM_WEAPONS (int) (sizeof(weapons) / sizeof(weapons[0]))

/* The menu only lists usable weapons: avail_impulse[menu value] = impulse */
static const char *avail_names[NUM_WEAPONS];
static int avail_impulse[NUM_WEAPONS];
static int num_avail;

static void menu_weapon(void *ud)
{
	int v = qembd_pd->system->getMenuItemValue(weapon_item);

	if (state == ST_RUN && v >= 0 && v < num_avail)
		Cbuf_AddText(va("impulse %d\n", avail_impulse[v]));
}

/*
 * Options can't change once a menu item exists, so rebuild it each time the
 * system menu opens, listing only the weapons that can be selected right now.
 */
static void rebuild_weapon_item(void)
{
	int current = 0;

	if (weapon_item) {
		qembd_pd->system->removeMenuItem(weapon_item);
		weapon_item = NULL;
	}
	if (state != ST_RUN || cls.state != ca_connected || cls.demoplayback)
		return;

	num_avail = 0;
	for (int i = 0; i < NUM_WEAPONS; i++) {
		if (!(cl.items & weapons[i].item))
			continue;
		if (weapons[i].ammo_stat >= 0 && cl.stats[weapons[i].ammo_stat] < weapons[i].ammo_min)
			continue;
		if (cl.stats[STAT_ACTIVEWEAPON] == weapons[i].item)
			current = num_avail;
		avail_names[num_avail] = weapons[i].name;
		avail_impulse[num_avail] = i + 1;
		num_avail++;
	}
	if (num_avail < 2)
		return;		/* nothing to choose between */

	weapon_item = qembd_pd->system->addOptionsMenuItem("Weapon", avail_names, num_avail,
	                                                   menu_weapon, NULL);
	qembd_pd->system->setMenuItemValue(weapon_item, current);
}

/* --------------------------------------------------------------- update */

static int update(void *ud)
{
	switch (state) {
	case ST_SPLASH:
		/* Draw first: loading the pak takes a while */
		show_message("Quake", "Loading...");
		state = ST_INIT;
		return 1;

	case ST_INIT: {
		static char arg0[] = "quake";
		static char *argv[] = {arg0, NULL};

		if (setjmp(frame_jmp))
			return 1;
		in_frame = 1;
		if (qembd_init(1, argv) != 0) {
			in_frame = 0;
			stop_with_message("Quake failed to start",
			                  "Not enough memory. Try a smaller PD_RENDER_WIDTH/HEIGHT.");
			return 1;
		}
		apply_run();
		Key_SetBinding(',', "+moveleft");
		Key_SetBinding('.', "+moveright");
		in_frame = 0;
		state = ST_RUN;
		return 1;
	}

	case ST_RUN:
		if (setjmp(frame_jmp))
			return 1;
		in_frame = 1;
		poll_input();
		qembd_frame();
		in_frame = 0;
		return 1;

	default:
		return 1;
	}
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg)
{
	(void) arg;

	if (event == kEventPause)
		rebuild_weapon_item();

	if (event == kEventInit) {
		qembd_pd = playdate;
		playdate->display->setRefreshRate(30);
		playdate->system->addMenuItem("Quake Menu", menu_quake, NULL);
		/* Setting an update callback tells the system this is a pure C game */
		playdate->system->setUpdateCallback(update, NULL);
	}
	return 0;
}
