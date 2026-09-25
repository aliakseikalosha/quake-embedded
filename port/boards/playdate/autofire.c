/*
 * Autofire: hold fire while a live monster is in the line of fire.
 */

#include <quakedef.h>
#include "autofire.h"
#include "keyqueue.h"

#define AUTOFIRE_RANGE 2048.0f
#define AUTOFIRE_AXE_RANGE 64.0f
#define AUTOFIRE_HEAVY_COOLDOWN 2.0f /* seconds between autofired grenades/rockets (Quake's own refire is 0.6/0.8) */

extern cvar_t sv_aim;

static int is_live_monster(edict_t *e)
{
	return ((int)e->v.flags & FL_MONSTER) && e->v.takedamage && e->v.health > 0;
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

	if ((int)sv_player->v.weapon == IT_AXE)
	{
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
	for (i = 1; i < sv.num_edicts; i++, check = NEXT_EDICT(check))
	{
		if (check == sv_player || !is_live_monster(check))
			continue;
		for (j = 0; j < 3; j++)
			end[j] = check->v.origin[j] + 0.5f * (check->v.mins[j] + check->v.maxs[j]);
		VectorSubtract(end, start, dir);
		VectorNormalize(dir);
		if (DotProduct(dir, fwd) < sv_aim.value)
			continue; /* too far to turn */
		tr = SV_Move(start, vec3_origin, vec3_origin, end, MOVE_NORMAL, sv_player);
		if (tr.ent == check)
			return 1;
	}
	return 0;
}

void pdq_autofire_update(int a_held, int allowed)
{
	static int auto_down;	 /* autofire is holding +attack */
	static float auto_ready; /* cl.time when a heavy weapon may autofire again */
	int want;

	if (a_held)
	{
		auto_down = 0; /* A owns the fire key; its release will let go */
		return;
	}

	want = cl_autofire.value && allowed && enemy_in_sight();

	/* Grenade/rocket launchers: one tap per cooldown instead of holding fire */
	if (want && sv_player &&
		((int)sv_player->v.weapon == IT_GRENADE_LAUNCHER ||
		 (int)sv_player->v.weapon == IT_ROCKET_LAUNCHER))
	{
		if (auto_ready > cl.time + AUTOFIRE_HEAVY_COOLDOWN)
			auto_ready = 0; /* clock restarted with a new level */
		if (auto_down || cl.time < auto_ready)
			want = 0; /* release the tap / still cooling down */
		else
			auto_ready = cl.time + AUTOFIRE_HEAVY_COOLDOWN;
	}

	if (want != auto_down)
	{
		pdq_push_key(K_CTRL, want);
		auto_down = want;
	}
}
