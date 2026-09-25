/*
 * Key event queue handed to Quake (qembd_dequeue_key_event), plus the mouse
 * hooks, which have nothing to report on this device.
 */

#include <stdbool.h>
#include <quakembd.h>
#include "keyqueue.h"

#define QUEUE_SIZE 32

static key_event_t queue[QUEUE_SIZE];
static int q_head, q_tail;

void pdq_push_key(uint32_t key, int down)
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
