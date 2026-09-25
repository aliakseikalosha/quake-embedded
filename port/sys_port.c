/*
 * Copyright (C) 2020 Shotaro Uchida <fantom@xmaker.mx>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <quakedef.h>
#include <quakembd.h>
#include "pdprof.h"

#ifndef DEFAULT_MEM_SIZE
#define DEFAULT_MEM_SIZE (8 * 1024 * 1024)
#endif
#define DEFAULT_BASEDIR "quakembd"
#define DEFAULT_CACHEDIR "/tmp"

qboolean isDedicated;

cvar_t  sys_linerefresh = {"sys_linerefresh","0"};// set for entity display

// =======================================================================
// Logging
// =======================================================================

#ifdef WINQUAKE_LOGGING_EXTERNAL
#include <stdio.h>
#define DEFAULT_LOG_BUFFER_SIZE 1024

void _Sys_Printf(const char *fmt, ...)
{
	va_list argptr;
	char text[DEFAULT_LOG_BUFFER_SIZE];
	unsigned char *p;

	va_start(argptr, fmt);
	vsprintf(text, fmt, argptr);
	va_end(argptr);

	if (strlen(text) > DEFAULT_LOG_BUFFER_SIZE) {
		qembd_error("memory overwrite in Sys_Printf");
		return;
	}

#ifdef QEMBD_PLAYDATE
	qembd_log(text);
	return;
#endif

	for (p = (unsigned char *) text; *p; p++) {
		*p &= 0x7f;
		if ((*p > 128 || *p < 32) && *p != 10 && *p != 13 && *p != 9)
			fprintf(stdout, "[%02x]", *p);
		else
			putc(*p, stdout);
	}
}
#endif

void Sys_Error(char *error, ...)
{ 
	va_list argptr;
	char string[1024];

	va_start (argptr, error);
	vsprintf (string, error, argptr);
	va_end (argptr);
#ifdef QEMBD_PLAYDATE
	qembd_fatal(string);
#else
	fprintf(stderr, "Error: %s\n", string);

	Host_Shutdown ();
	exit (1);
#endif
}

// =======================================================================
// General routines
// =======================================================================

void Sys_Quit(void)
{
	Host_Shutdown();
#ifdef QEMBD_PLAYDATE
	qembd_quit();
#else
	exit(0);
#endif
}

double Sys_FloatTime(void)
{
	return qembd_get_us_time() / 1000000.0;
}

char *Sys_ConsoleInput(void)
{
	// TODO
	return NULL;
}

void Sys_Sleep(void)
{
}

void Sys_SendKeyEvents(void)
{
	key_event_t e;
	while (qembd_dequeue_key_event(&e) == 0)
		Key_Event(e.keycode, e.state == 1);
}

void Sys_HighFPPrecision(void)
{
}

void Sys_LowFPPrecision(void)
{
}

void Sys_MakeCodeWriteable(unsigned long startaddr, unsigned long length)
{
}

static float oldtime;

int qembd_init(int c, char **v)
{
	quakeparms_t parms = {0};
	int j;

	COM_InitArgv(c, v);
	parms.argc = com_argc;
	parms.argv = com_argv;

	parms.memsize = DEFAULT_MEM_SIZE;
	j = COM_CheckParm("-mem");
	if (j)
		parms.memsize = (int) (Q_atof(com_argv[j+1]) * 1024 * 1024);
	parms.membase = qembd_allocmain(parms.memsize);
#ifdef QEMBD_PLAYDATE
	/* The heap left for a game varies; settle for less rather than fail. */
	while (!parms.membase && !j && parms.memsize > DEFAULT_MIN_MEM_SIZE) {
		parms.memsize -= 512 * 1024;
		parms.membase = qembd_allocmain(parms.memsize);
	}
#endif
	if (!parms.membase) {
		qembd_error("Memory cannot be allocated");
		return -1;
	}
	qembd_info("Quake heap: %d KiB", parms.memsize / 1024);

	parms.basedir = DEFAULT_BASEDIR;
// caching is disabled by default, use -cachedir to enable
//	parms.cachedir = DEFAULT_CACHEDIR;

	Host_Init(&parms);

//	if (COM_CheckParm("-nostdout")) {
//		nostdout = 1;
//	}

	qembd_info("QuakEMBD - Based on WinQuake %0.3f", VERSION);

	oldtime = Sys_FloatTime() - 0.1;
	return 0;
}

void qembd_frame(void)
{
	float time, newtime;

	// find time spent rendering last frame
	newtime = Sys_FloatTime();
	time = newtime - oldtime;

	if (time > sys_ticrate.value*2)
		oldtime = newtime;
	else
		oldtime += time;

	pdprof_frame_begin();
	Host_Frame(time);
	pdprof_frame_end();

#if 0
	// graphic debugging aids
	if (sys_linerefresh.value)
		Sys_LineRefresh ();
#endif
}

int qembd_main(int c, char **v)
{
	int r = qembd_init(c, v);

	if (r)
		return r;
	while (1)
		qembd_frame();
}
