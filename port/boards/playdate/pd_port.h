#ifndef PD_PORT_H
#define PD_PORT_H

#include <stddef.h>
#include "pd_api.h"

/* Set once in eventHandler(kEventInit) */
extern PlaydateAPI *qembd_pd;

/* Console line output (used by qembd_log) */
void pdq_log_line(const char *text);

/* Quake builds paths like ".//id1/pak0.pak"; the Playdate wants "id1/pak0.pak" */
const char *pdq_path(const char *path, char *out, size_t size);

/* Create every directory leading up to the file in path (in the Data folder) */
void pdq_mkdirs(const char *path);

#endif /* PD_PORT_H */
