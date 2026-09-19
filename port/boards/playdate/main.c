/*
 * Playdate entry point: event handler, frame loop, input and timing.
 *
 * Controls
 *   D-pad up/down   walk forward / back      (menus: move)
 *   D-pad left/right turn                    (menus: change value)
 *   A               fire                     (menus: select)
 *   B               jump                     (menus: back)
 *   Crank           next / previous weapon
 *   System menu     "Quake Menu" opens Quake's own menu; "Always Run" toggles
 *                   running; "Show FPS" draws the frame rate.
 * While a demo is playing (title screen) A and B open Quake's menu.
 */

#include <setjmp.h>
#include <string.h>
#include <quakembd.h>
#include <quakedef.h>
#include "pd_port.h"

PlaydateAPI *qembd_pd;

enum { ST_SPLASH, ST_INIT, ST_RUN, ST_STOPPED };

#define CRANK_STEP 40.0f	/* degrees of crank per weapon change */
#define RUN_SPEED 400
#define WALK_SPEED 200

static int state = ST_SPLASH;
static jmp_buf frame_jmp;
static int in_frame;

static int menu_requested;
static int always_run = 1;
static int show_fps;
static PDMenuItem *run_item, *fps_item;

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
	return qembd_pd->system->realloc(NULL, size);
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

static void poll_input(void)
{
	static PDButtons prev;
	static int sent[NUM_BUTTONS];	/* key sent on press, released with the same */
	static float crank;
	PDButtons cur;
	int ui = key_dest != key_game;

	qembd_pd->system->getButtonState(&cur, NULL, NULL);

	for (int i = 0; i < NUM_BUTTONS; i++) {
		int down = (cur & buttons[i].mask) != 0;
		int was = (prev & buttons[i].mask) != 0;

		if (down && !was) {
			int key = ui ? buttons[i].ui_key : buttons[i].game_key;

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

	if (menu_requested) {
		menu_requested = 0;
		push_key(K_ESCAPE, 1);
		push_key(K_ESCAPE, 0);
	}

	/* Crank cycles weapons while playing */
	crank += qembd_pd->system->getCrankChange();
	if (key_dest != key_game || cls.state != ca_connected || cls.demoplayback) {
		crank = 0;
	} else {
		while (crank >= CRANK_STEP) {
			Cbuf_AddText("impulse 10\n");	/* next weapon */
			crank -= CRANK_STEP;
		}
		while (crank <= -CRANK_STEP) {
			Cbuf_AddText("impulse 12\n");	/* previous weapon */
			crank += CRANK_STEP;
		}
	}
}

static void apply_run(void)
{
	Cvar_SetValue("cl_forwardspeed", always_run ? RUN_SPEED : WALK_SPEED);
	Cvar_SetValue("cl_backspeed", always_run ? RUN_SPEED : WALK_SPEED);
}

static void menu_quake(void *ud)
{
	menu_requested = 1;
}

static void menu_run(void *ud)
{
	always_run = qembd_pd->system->getMenuItemValue(run_item);
	if (state == ST_RUN)
		apply_run();
}

static void menu_fps(void *ud)
{
	show_fps = qembd_pd->system->getMenuItemValue(fps_item);
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
		if (show_fps)
			qembd_pd->system->drawFPS(0, 0);
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

	if (event == kEventInit) {
		qembd_pd = playdate;
		playdate->display->setRefreshRate(30);
		playdate->system->addMenuItem("Quake Menu", menu_quake, NULL);
		run_item = playdate->system->addCheckmarkMenuItem("Always Run", always_run,
		                                                  menu_run, NULL);
		fps_item = playdate->system->addCheckmarkMenuItem("Show FPS", show_fps, menu_fps, NULL);
		/* Setting an update callback tells the system this is a pure C game */
		playdate->system->setUpdateCallback(update, NULL);
	}
	return 0;
}
