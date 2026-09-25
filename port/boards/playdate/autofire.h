#ifndef PD_AUTOFIRE_H
#define PD_AUTOFIRE_H

/*
 * Call once per frame. a_held: the fire button is down (it owns the fire key).
 * allowed: the player is in control and the crank is out. Holds +attack while
 * cl_autofire is on and an enemy is under the crosshair.
 */
void pdq_autofire_update(int a_held, int allowed);

#endif /* PD_AUTOFIRE_H */
