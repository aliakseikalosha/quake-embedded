#ifndef PD_KEYQUEUE_H
#define PD_KEYQUEUE_H

#include <stdint.h>

/* Queue a key press (down = 1) or release (down = 0) for Quake to read */
void pdq_push_key(uint32_t key, int down);

#endif /* PD_KEYQUEUE_H */
