/*
 * Weapon list for the system menu: which weapons can be selected right now.
 */

#include <quakedef.h>
#include "weapons.h"

/* Weapons in impulse order (impulse 1 = axe ... 8 = thunderbolt) */
static const struct
{
	const char *name;
	int item;	   /* IT_* bit that says the weapon is owned */
	int ammo_stat; /* STAT_* holding its ammo, or -1 */
	int ammo_min;  /* ammo needed to select it (Quake refuses otherwise) */
} weapons[] = {
	{"Axe", IT_AXE, -1, 0},
	{"Shotgun", IT_SHOTGUN, STAT_SHELLS, 1},
	{"Dbl Shotgun", IT_SUPER_SHOTGUN, STAT_SHELLS, 2},
	{"Nailgun", IT_NAILGUN, STAT_NAILS, 1},
	{"Super Nailgun", IT_SUPER_NAILGUN, STAT_NAILS, 2},
	{"Grenade", IT_GRENADE_LAUNCHER, STAT_ROCKETS, 1},
	{"Rocket", IT_ROCKET_LAUNCHER, STAT_ROCKETS, 1},
	{"Lightning", IT_LIGHTNING, STAT_CELLS, 1},
};
#define NUM_WEAPONS (int)(sizeof(weapons) / sizeof(weapons[0]))

/* avail_impulse[menu value] = impulse */
const char *pdq_weapon_names[NUM_WEAPONS];
static int avail_impulse[NUM_WEAPONS];
static int num_avail;

int pdq_weapons_scan(int *current)
{
	*current = 0;
	num_avail = 0;
	for (int i = 0; i < NUM_WEAPONS; i++)
	{
		if (!(cl.items & weapons[i].item))
			continue;
		if (weapons[i].ammo_stat >= 0 && cl.stats[weapons[i].ammo_stat] < weapons[i].ammo_min)
			continue;
		if (cl.stats[STAT_ACTIVEWEAPON] == weapons[i].item)
			*current = num_avail;
		pdq_weapon_names[num_avail] = weapons[i].name;
		avail_impulse[num_avail] = i + 1;
		num_avail++;
	}
	return num_avail;
}

void pdq_weapon_select(int index)
{
	if (index >= 0 && index < num_avail)
		Cbuf_AddText(va("impulse %d\n", avail_impulse[index]));
}
