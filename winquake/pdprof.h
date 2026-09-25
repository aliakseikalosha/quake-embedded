/*
 * Optional on-device profiler (cmake -DPD_PROFILE=ON, and -DPD_BENCH=ON to also
 * play the three demos back with `timedemo`). Everything compiles to nothing
 * when PD_PROFILE is not defined.
 *
 * Timing uses system->getElapsedTime(): game code is unprivileged, so the
 * Cortex-M7 DWT cycle counter faults. The timer is reset every frame to keep
 * float precision; ticks are 1/168 us so reports divide by 168.
 * Results are written to prof.csv in the game's Data folder (the USB serial
 * console does not carry logToConsole output).
 */
#ifndef PDPROF_H
#define PDPROF_H

#ifdef PD_PROFILE
#include <stdint.h>

/* Timed sections (microseconds per frame in prof.csv). Sections nest:
 * SCAN contains DSURF, which contains CACHE/SPANS/ZSPAN/OTHER. */
enum {
	P_FRAME, P_INPUT, P_SERVER, P_CLIENT, P_SCR, P_SETUP, P_WORLD, P_BENT, P_SCAN,
	P_DSURF, P_CACHE, P_SPANS, P_ZSPAN, P_OTHER,
	P_ENT, P_VIEW, P_PART, P_UPSCALE, P_HUD, P_VID, P_SND, P_PAL,
	/* finer sections; each is nested in another one (see pdprof.c) */
	P_FACE, P_SEINS, P_SEGEN, P_SEREM, P_SESTEP, P_GRAD, P_SCALLOC, P_LIGHT, P_BLOCKS,
	P_SVRUN, P_SVPHYS, P_SVSEND, P_QC, P_ALIAS, P_ATRANS, P_APOLY, P_LPT, P_ABBOX, P_WMARK, P_WEFRAG, P_WSURFS,
	P_NSECT
};

/* Per-frame counters */
enum { C_SPANS, C_PIXELS, C_CBUILD, C_CTEXELS, C_DRAWN, C_NODES, C_LEAVES, C_MARKS, C_NSURFS, C_FACES, C_QCOPS, C_QCCALLS, C_AMODELS, C_AVERTS, C_ATRIS, C_LPQ, C_LPHIT, C_APIX, C_BNEW, C_BDLIGHT, C_BANIM, C_BCLEAR, C_TNEW, C_TDLIGHT, C_TANIM, C_TCLEAR, C_NCNT };

extern uint32_t pdprof_acc[P_NSECT];
extern uint32_t pdprof_t0[P_NSECT];
extern uint32_t pdprof_cnt[C_NCNT];
extern uint32_t pdprof_calls[P_NSECT];
extern float (*pdprof_elapsed)(void);

/* Lowest stack pointer seen at the probed functions (fast internal RAM is only ~10 KB); logged as STK lines */
enum { K_WORLD, K_SCAN, K_DSURF, K_CACHE, K_RSURF, K_ALIAS, K_POLY, K_LIGHT, K_QC, K_CON, K_NSTK };
extern uintptr_t pdprof_minsp[K_NSTK];
#define PROF_STK(id) do { uintptr_t sp_; __asm__ volatile("mov %0, sp" : "=r"(sp_)); if (sp_ < pdprof_minsp[id]) pdprof_minsp[id] = sp_; } while (0)

#define PROF_CYC() ((uint32_t)(pdprof_elapsed() * 168000000.0f))
#define PROF_BEGIN(s) (pdprof_t0[s] = PROF_CYC())
#define PROF_END(s) (pdprof_acc[s] += PROF_CYC() - pdprof_t0[s], pdprof_calls[s]++)
#define PROF_CNT(c, n) (pdprof_cnt[c] += (n))
#define PROF_SPANS(list) pdprof_count_spans(list)

/* Finer sections (world faces, edge scan parts, surface builds, server): each timer call costs
 * ~3 us, so they only exist with -DPD_PROFILE_FINE=ON to keep the default profile close to a
 * release build. The counters that go with them are also fine-only. */
#ifdef PD_PROFILE_FINE
#define PROF_BEGINF(s) PROF_BEGIN(s)
#define PROF_ENDF(s) PROF_END(s)
#define PROF_CNTF(c, n) PROF_CNT(c, n)
#define PROF_SPANSF(list) pdprof_count_spans(list)
#else
#define PROF_BEGINF(s) ((void)0)
#define PROF_ENDF(s) ((void)0)
#define PROF_CNTF(c, n) ((void)0)
#define PROF_SPANSF(list) ((void)0)
#endif

void pdprof_init(void);
void pdprof_open(void);
void pdprof_stage(const char *text);
void pdprof_note(const char *text);
void pdprof_frame_begin(void);
void pdprof_frame_end(void);
void pdprof_count_spans(void *espans);
void pdprof_bi_add(int builtin, uint32_t ticks);

#else

#define PROF_STK(id) ((void)0)
#define PROF_BEGIN(s) ((void)0)
#define PROF_END(s) ((void)0)
#define PROF_CNT(c, n) ((void)0)
#define PROF_SPANS(list) ((void)0)
#define PROF_BEGINF(s) ((void)0)
#define PROF_ENDF(s) ((void)0)
#define PROF_CNTF(c, n) ((void)0)
#define PROF_SPANSF(list) ((void)0)
#define pdprof_init() ((void)0)
#define pdprof_open() ((void)0)
#define pdprof_stage(t) ((void)0)
#define pdprof_note(t) ((void)0)
#define pdprof_frame_begin() ((void)0)
#define pdprof_frame_end() ((void)0)
#define pdprof_bi_add(i, t) ((void)0)

#endif
#endif
