#ifndef PD_WEAPONS_H
#define PD_WEAPONS_H

/*
 * List the weapons that can be selected right now (owned, with enough ammo)
 * and return how many. Fills the arrays below and *current with the index of
 * the active weapon.
 */
int pdq_weapons_scan(int *current);

/* Names of the weapons found by the last scan, indexed 0..count-1 */
extern const char *pdq_weapon_names[];

/* Send the "impulse" that selects weapon number index of the last scan */
void pdq_weapon_select(int index);

#endif /* PD_WEAPONS_H */
