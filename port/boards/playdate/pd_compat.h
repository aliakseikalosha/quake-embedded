/*
 * Force-included into every C file of a Playdate build.
 *
 * The Playdate has no working libc file I/O or console: files go through
 * playdate->file and text through playdate->system->logToConsole. WinQuake
 * uses stdio directly (config, savegames, demos), so redirect it here rather
 * than patching every call site.
 */
#ifndef PD_COMPAT_H
#define PD_COMPAT_H

#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>

FILE *pdq_fopen(const char *path, const char *mode);
int pdq_fclose(FILE *f);
size_t pdq_fread(void *buf, size_t size, size_t n, FILE *f);
size_t pdq_fwrite(const void *buf, size_t size, size_t n, FILE *f);
int pdq_fseek(FILE *f, long off, int whence);
int pdq_fgetc(FILE *f);
int pdq_feof(FILE *f);
int pdq_fflush(FILE *f);
int pdq_fprintf(FILE *f, const char *fmt, ...);
int pdq_fscanf(FILE *f, const char *fmt, ...);
int pdq_printf(const char *fmt, ...);
int pdq_unlink(const char *path);

#undef getc
#undef feof
#define fopen pdq_fopen
#define fclose pdq_fclose
#define fread pdq_fread
#define fwrite pdq_fwrite
#define fseek pdq_fseek
#define fgetc pdq_fgetc
#define getc pdq_fgetc
#define feof pdq_feof
#define fflush pdq_fflush
#define fprintf pdq_fprintf
#define fscanf pdq_fscanf
#define printf pdq_printf
#define unlink pdq_unlink

/* quakembd.h logging goes to the Playdate console */
#define QEMBD_PRINTF pdq_printf

#endif /* PD_COMPAT_H */
