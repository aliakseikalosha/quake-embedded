/* On-device profiler; see winquake/pdprof.h. Only built with -DPD_PROFILE=ON. */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <quakedef.h>
#include <quakembd.h>
#include <r_local.h>
#include "pd_port.h"
#include "pdprof.h"

uint32_t pdprof_acc[P_NSECT];
uint32_t pdprof_t0[P_NSECT];
uint32_t pdprof_cnt[C_NCNT];
uint32_t pdprof_calls[P_NSECT];
static uint32_t call_ticks; /* cost of one timer call, in ticks (measured at init) */
float (*pdprof_elapsed)(void);
uintptr_t pdprof_minsp[K_NSTK] = {~(uintptr_t)0, ~(uintptr_t)0, ~(uintptr_t)0, ~(uintptr_t)0, ~(uintptr_t)0,
	~(uintptr_t)0, ~(uintptr_t)0, ~(uintptr_t)0, ~(uintptr_t)0, ~(uintptr_t)0};
static uintptr_t base_sp, reported_sp[K_NSTK];
static const char *const stk_names[K_NSTK] = {"world", "scan", "dsurf", "cache", "rsurf", "alias", "poly", "light", "qc", "con"};
extern particle_t *active_particles;

#define TICKS_PER_US 168u
#define US(c) ((c) / TICKS_PER_US)

static const char *const sect_names[P_NSECT] = {
	"frame", "input", "server", "client", "scr", "setup", "world", "bent", "scan",
	"dsurf", "cache", "spans", "zspan", "other",
	"ent", "view", "part", "upscale", "hud", "vid", "snd", "pal",
	"face", "se_ins", "se_gen", "se_rem", "se_step", "grad", "scalloc", "light", "blocks",
	"sv_run", "sv_phys", "sv_send", "qc", "alias", "atrans", "apoly", "lpt", "abbox", "wmark", "wefrag", "wsurfs"
};


/* Enclosing section of each section, for removing the timer's own cost */
static const signed char parent_of[P_NSECT] = {
	[P_FRAME] = -1, [P_INPUT] = P_FRAME, [P_SERVER] = P_FRAME, [P_CLIENT] = P_FRAME, [P_SCR] = P_FRAME,
	[P_SND] = P_FRAME,
	[P_SETUP] = P_SCR, [P_WORLD] = P_SCR, [P_BENT] = P_SCR, [P_SCAN] = P_SCR, [P_ENT] = P_SCR,
	[P_VIEW] = P_SCR, [P_PART] = P_SCR, [P_UPSCALE] = P_SCR, [P_HUD] = P_SCR, [P_VID] = P_SCR,
	[P_PAL] = P_SCR,
	[P_DSURF] = P_SCAN, [P_SEINS] = P_SCAN, [P_SEGEN] = P_SCAN, [P_SEREM] = P_SCAN, [P_SESTEP] = P_SCAN,
	[P_CACHE] = P_DSURF, [P_SPANS] = P_DSURF, [P_ZSPAN] = P_DSURF, [P_OTHER] = P_DSURF, [P_GRAD] = P_DSURF,
	[P_FACE] = P_WSURFS,
	[P_SCALLOC] = P_CACHE, [P_LIGHT] = P_CACHE, [P_BLOCKS] = P_CACHE,
	[P_SVRUN] = P_SERVER, [P_SVPHYS] = P_SERVER, [P_SVSEND] = P_SERVER, [P_QC] = P_SERVER,
	[P_ALIAS] = P_ENT, [P_ATRANS] = P_ALIAS, [P_APOLY] = P_ALIAS, [P_LPT] = P_ENT, [P_ABBOX] = P_ENT, [P_WMARK] = P_WORLD, [P_WEFRAG] = P_WORLD, [P_WSURFS] = P_WORLD,
};
static const char *const cnt_names[C_NCNT] = {"n_spans", "n_pixels", "n_cbuild", "n_ctexels", "n_drawn",
	"n_nodes", "n_leaves", "n_marks", "n_nsurfs", "n_faces", "n_qcops", "n_qccalls", "n_amodels", "n_averts", "n_atris", "n_lpq", "n_lphit", "n_apix", "b_new", "b_dlight", "b_anim", "b_clear", "t_new", "t_dlight", "t_anim", "t_clear"};

static SDFile *pf;
static uint32_t last_entry, entry_ms, last_hostframe, base_ms;
static char batch[3072];
static int blen, bcount;
static char last_map[64];
static int last_demo = -2;

static void pf_write(const char *buf, int len)
{
	if (pf && len > 0) {
		qembd_pd->file->write(pf, buf, len);
		qembd_pd->file->flush(pf);
	}
}

static void pf_line(const char *fmt, ...)
{
	char b[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b) - 1, fmt, ap);
	va_end(ap);
	if (n > 0) {
		b[n++] = '\n';
		pf_write(b, n);
	}
}

static void flush(void)
{
	if (blen)
		pf_write(batch, blen);
	blen = bcount = 0;
}

void pdprof_open(void)
{
	pdprof_elapsed = qembd_pd->system->getElapsedTime;
	if (!pf)
		pf = qembd_pd->file->open("prof.csv", kFileWrite);
	if (!pf)
		qembd_pd->system->logToConsole("PROF: cannot open prof.csv: %s", qembd_pd->file->geterr());
}

void pdprof_stage(const char *text)
{
	pdprof_open();
	pf_line("STAGE,%u,%s", (unsigned)qembd_pd->system->getCurrentTimeMilliseconds(), text);
}

void pdprof_note(const char *text)
{
	if (pf)
		pf_line("L,%u,%s", (unsigned)(qembd_pd->system->getCurrentTimeMilliseconds() - base_ms), text);
}


/*
 * CPU and memory microbenchmarks, logged as MICRO lines: they show what the
 * core and the memory behind the Quake heap can actually do.
 */
static volatile uint32_t micro_sink;
static uint8_t micro_bss[256 * 1024] __attribute__((aligned(32)));

/* Sequential 32-bit reads and dependent random reads over growing sizes */
static void micro_sweep(const char *name, uint8_t *b, int max)
{
	int size, i, r, iters;
	uint32_t sum = 0, x;
	float t0, t1;
	char line[200];
	int n;

	for (i = 0; i < max; i++)
		b[i] = (uint8_t)(i * 7);
	for (size = 1024; size <= max; size *= 2) {
		iters = (2 * 1024 * 1024) / size;
		if (iters < 1)
			iters = 1;
		qembd_pd->system->resetElapsedTime();
		t0 = pdprof_elapsed();
		for (r = 0; r < iters; r++)
			for (i = 0; i < size / 4; i++)
				sum += ((volatile uint32_t *)b)[i];
		t1 = pdprof_elapsed();
		n = snprintf(line, sizeof(line), "MICRO,%s,%dKB,seq %.0f MB/s", name, size / 1024,
			(double)iters * size / ((double)(t1 - t0) * 1e6));

		x = 1;
		qembd_pd->system->resetElapsedTime();
		t0 = pdprof_elapsed();
		for (i = 0; i < 20000; i++)
			x = x * 1664525u + 1013904223u + b[(x >> 8) & (size - 1)];
		t1 = pdprof_elapsed();
		snprintf(line + n, sizeof(line) - n, ",rand %.0f ns", (double)(t1 - t0) * 1e9 / 20000);
		pf_line("%s", line);
		micro_sink = x + sum;
	}
}


/* Store cost by width and region size: is a store to a cached line cheap? */
static void micro_write(uint8_t *buf, int max)
{
	static const int sizes[] = {2048, 16384, 65536, 524288};
	int si, r, i;
	float t0, t1;

	for (si = 0; si < 4 && sizes[si] <= max; si++) {
		int size = sizes[si];
		int reps = (2 * 1024 * 1024) / size;
		double nstores;
		uint8_t *b8 = buf;
		uint16_t *b16 = (uint16_t *)buf;
		uint32_t *b32 = (uint32_t *)buf;
		char line[240];
		int n;

		if (reps < 1)
			reps = 1;
		memset(buf, 0, size);	/* touch the region once */

		qembd_pd->system->resetElapsedTime();
		t0 = pdprof_elapsed();
		for (r = 0; r < reps; r++)
			for (i = 0; i < size; i++)
				((volatile uint8_t *)b8)[i] = (uint8_t)i;
		t1 = pdprof_elapsed();
		nstores = (double)reps * size;
		n = snprintf(line, sizeof(line), "MICRO,write %dKB,byte %.0f ns/store", size / 1024, (double)(t1 - t0) * 1e9 / nstores);

		qembd_pd->system->resetElapsedTime();
		t0 = pdprof_elapsed();
		for (r = 0; r < reps; r++)
			for (i = 0; i < size / 2; i++)
				((volatile uint16_t *)b16)[i] = (uint16_t)i;
		t1 = pdprof_elapsed();
		nstores = (double)reps * size / 2;
		n += snprintf(line + n, sizeof(line) - n, ",half %.0f", (double)(t1 - t0) * 1e9 / nstores);

		qembd_pd->system->resetElapsedTime();
		t0 = pdprof_elapsed();
		for (r = 0; r < reps; r++)
			for (i = 0; i < size / 4; i++)
				((volatile uint32_t *)b32)[i] = (uint32_t)i;
		t1 = pdprof_elapsed();
		nstores = (double)reps * size / 4;
		n += snprintf(line + n, sizeof(line) - n, ",word %.0f", (double)(t1 - t0) * 1e9 / nstores);

		/* 8-word store-multiple bursts */
		qembd_pd->system->resetElapsedTime();
		t0 = pdprof_elapsed();
		for (r = 0; r < reps; r++)
			for (i = 0; i < size / 4; i += 8) {
				uint32_t *d = b32 + i;
				asm volatile("stmia %0, {r4-r11}" : : "r"(d) : "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "memory");
			}
		t1 = pdprof_elapsed();
		nstores = (double)reps * size / 32;
		n += snprintf(line + n, sizeof(line) - n, ",stm8 %.0f ns/burst", (double)(t1 - t0) * 1e9 / nstores);

		/* read-modify-write bytes */
		qembd_pd->system->resetElapsedTime();
		t0 = pdprof_elapsed();
		for (r = 0; r < reps; r++)
			for (i = 0; i < size; i++)
				((volatile uint8_t *)b8)[i] += 1;
		t1 = pdprof_elapsed();
		nstores = (double)reps * size;
		snprintf(line + n, sizeof(line) - n, ",rmw byte %.0f", (double)(t1 - t0) * 1e9 / nstores);
		pf_line("%s", line);
	}
}


/*
 * Two questions that decide how to restructure the renderer:
 *  1. does a store to an uncached line allocate it (is a freshly written line cheap to read back)?
 *  2. can a cache miss overlap with other work or with other misses (can an early load act as a prefetch)?
 */
static void micro_mem2(uint8_t *b, int size)
{
	enum { NLINES = 8192, ALU_UNITS = 7 };
	volatile uint32_t *v = (volatile uint32_t *)b;
	uint32_t sum = 0, x = 1;
	double ts = 0, tr1 = 0, tr2 = 0;
	float t0, t1;
	int r, i, k;
	char line[240];
	int n;

	if (size < NLINES * 32)
		return;

	/* 1. write-allocate: evict with a big read, store one byte per line of an 8 KB region, read it back */
	for (r = 0; r < 20; r++) {
		for (i = 128 * 1024; i < 192 * 1024; i += 32)
			sum += v[i / 4];
		qembd_pd->system->resetElapsedTime();
		t0 = pdprof_elapsed();
		for (i = 0; i < 8192; i += 32)
			b[i] = (uint8_t)r;
		t1 = pdprof_elapsed();
		ts += t1 - t0;
		t0 = pdprof_elapsed();
		for (i = 0; i < 8192; i += 32)
			sum += v[(i + 8) / 4];
		t1 = pdprof_elapsed();
		tr1 += t1 - t0;
		t0 = pdprof_elapsed();
		for (i = 0; i < 8192; i += 32)
			sum += v[(i + 16) / 4];
		t1 = pdprof_elapsed();
		tr2 += t1 - t0;
	}
	pf_line("MICRO,write-allocate,store %.0f ns/line,first read after %.0f,second read %.0f",
		ts * 1e9 / (20 * 256), tr1 * 1e9 / (20 * 256), tr2 * 1e9 / (20 * 256));

	/* 2. overlapping misses: 1, then 4 independent loads in flight, then a dependent chain */
	n = 0;
	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (i = 0; i < NLINES; i++)
		sum += v[((i * 40503u) & (NLINES - 1)) * 8];
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, "MICRO,miss-overlap,one at a time %.0f ns", (double)(t1 - t0) * 1e9 / NLINES);

	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (i = 0; i < NLINES; i += 4) {
		uint32_t a0 = v[((i * 40503u) & (NLINES - 1)) * 8];
		uint32_t a1 = v[(((i + 1) * 40503u) & (NLINES - 1)) * 8];
		uint32_t a2 = v[(((i + 2) * 40503u) & (NLINES - 1)) * 8];
		uint32_t a3 = v[(((i + 3) * 40503u) & (NLINES - 1)) * 8];
		sum += a0 + a1 + a2 + a3;
	}
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, ",four per group %.0f", (double)(t1 - t0) * 1e9 / NLINES);

	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (i = 0; i < NLINES; i++)
		x = (x + v[((x * 40503u + i) & (NLINES - 1)) * 8]) & 0xffff;
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, ",dependent chain %.0f", (double)(t1 - t0) * 1e9 / NLINES);
	pf_line("%s", line);

	/* 3. early load as prefetch: ~0.8 us of ALU work per line, load used after (early) or before (late) the work */
	n = 0;
	for (k = 0; k < 2; k++) {
		uint32_t y, w = 1;

		qembd_pd->system->resetElapsedTime();
		t0 = pdprof_elapsed();
		for (i = 0; i < 2048; i++) {
			int j;

			if (k == 0) {
				y = v[((i * 40503u) & (NLINES - 1)) * 8];	/* load, work, then use */
				for (j = 0; j < ALU_UNITS; j++)
					asm volatile("add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
						"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
						"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
						"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n" : "+r"(w));
				sum += y;
			} else {
				for (j = 0; j < ALU_UNITS; j++)
					asm volatile("add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
						"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
						"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
						"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n" : "+r"(w));
				y = v[((i * 40503u) & (NLINES - 1)) * 8];	/* work, then load and use */
				sum += y;
			}
		}
		t1 = pdprof_elapsed();
		n += snprintf(line + n, sizeof(line) - n, "%s%s %.0f ns", k ? ",work then load" : "MICRO,prefetch,load then work",
			k ? "" : "", (double)(t1 - t0) * 1e9 / 2048);
		sum += w;
	}
	{
		uint32_t w = 1;

		qembd_pd->system->resetElapsedTime();
		t0 = pdprof_elapsed();
		for (i = 0; i < 2048; i++) {
			int j;

			for (j = 0; j < ALU_UNITS; j++)
				asm volatile("add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
					"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
					"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
					"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n" : "+r"(w));
		}
		t1 = pdprof_elapsed();
		n += snprintf(line + n, sizeof(line) - n, ",work alone %.0f", (double)(t1 - t0) * 1e9 / 2048);
		sum += w;
	}
	pf_line("%s", line);
	micro_sink = sum + x;
}


#define MB_WORK(w) asm volatile("add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n" \
	"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n" \
	"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n" \
	"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n" : "+r"(w))

/* What does a scattered store cost when compute sits between the stores (as in real code)? */
static void micro_store_spacing(uint8_t *b)
{
	enum { N = 2000 };
	volatile uint32_t *v = (volatile uint32_t *)b;
	uint32_t w = 1;
	float t0, t1;
	int i, u, k;
	char line[300];
	int n = 0;
	static const int units[] = {0, 1, 2, 4, 8};

	n += snprintf(line + n, sizeof(line) - n, "MICRO,store-spacing (ns per store, minus the work alone), 64 hot lines");
	for (k = 0; k < 5; k++) {
		int wu = units[k];
		double base, withst;

		/* work alone */
		qembd_pd->system->resetElapsedTime();
		t0 = pdprof_elapsed();
		for (i = 0; i < N; i++)
			for (u = 0; u < wu; u++)
				MB_WORK(w);
		t1 = pdprof_elapsed();
		base = t1 - t0;
		/* store to a rotating line, then work */
		qembd_pd->system->resetElapsedTime();
		t0 = pdprof_elapsed();
		for (i = 0; i < N; i++) {
			v[(i & 63) * 8] = (uint32_t)i;
			for (u = 0; u < wu; u++)
				MB_WORK(w);
		}
		t1 = pdprof_elapsed();
		withst = t1 - t0;
		n += snprintf(line + n, sizeof(line) - n, ",work %dns:%.0f", wu * 117, (withst - base) * 1e9 / N);
	}
	pf_line("%s", line);

	/* two store streams alternating (view byte + z halfword per pixel) versus one stream at a time */
	{
		volatile uint8_t *A = b + 65536;
		volatile uint16_t *B = (volatile uint16_t *)(b + 131072);
		double ti, ts;

		qembd_pd->system->resetElapsedTime();
		t0 = pdprof_elapsed();
		for (k = 0; k < 8; k++)
			for (i = 0; i < 2048; i++) {
				A[i] = (uint8_t)i;
				B[i] = (uint16_t)i;
			}
		t1 = pdprof_elapsed();
		ti = (t1 - t0) * 1e9 / (8 * 2048);
		qembd_pd->system->resetElapsedTime();
		t0 = pdprof_elapsed();
		for (k = 0; k < 8; k++) {
			for (i = 0; i < 2048; i++)
				A[i] = (uint8_t)i;
			for (i = 0; i < 2048; i++)
				B[i] = (uint16_t)i;
		}
		t1 = pdprof_elapsed();
		ts = (t1 - t0) * 1e9 / (8 * 2048);
		pf_line("MICRO,two-streams,interleaved byte+half %.0f ns per pixel,separate passes %.0f ns per pixel", ti, ts);
	}
	micro_sink = w;
}


/*
 * Does an early load (a "prefetch by touch") overlap with the kind of work the renderer does
 * while waiting: cached loads, and stores to slow memory?  Each case is timed with the cold miss
 * (issued first, its result used after the work) and without it (work alone).
 */
static void micro_overlap_kinds(uint8_t *b)
{
	enum { N = 1024, NL = 8192 };
	volatile uint32_t *cold = (volatile uint32_t *)b;			/* 256 KB: every line a miss */
	volatile uint8_t *hot = b + 262144 - 4096;					/* 2 KB read, 2 KB written */
	uint32_t sum = 0, w = 1;
	float t0, t1;
	int i, k, kind, with;
	static const char *const names[] = {"alu", "32 cached loads", "24 byte stores", "16 loads+16 stores"};
	double res[4][2];
	char line[400];
	int n = 0;

	for (kind = 0; kind < 4; kind++)
		for (with = 0; with < 2; with++) {
			qembd_pd->system->resetElapsedTime();
			t0 = pdprof_elapsed();
			for (i = 0; i < N; i++) {
				uint32_t y = 0;

				if (with)
					y = cold[((i * 40503u) & (NL - 1)) * 8];
				switch (kind) {
				case 0:
					for (k = 0; k < 7; k++)
						MB_WORK(w);
					break;
				case 1:
					for (k = 0; k < 32; k++)
						sum += hot[(k * 61 + i) & 2047];
					break;
				case 2:
					for (k = 0; k < 24; k++)
						hot[2048 + ((i * 8 + k) & 2047)] = (uint8_t)k;
					break;
				default:
					for (k = 0; k < 16; k++) {
						sum += hot[(k * 61 + i) & 2047];
						hot[2048 + ((i * 8 + k) & 2047)] = (uint8_t)k;
					}
					break;
				}
				sum += y;
			}
			t1 = pdprof_elapsed();
			res[kind][with] = (t1 - t0) * 1e9 / N;
		}
	n += snprintf(line + n, sizeof(line) - n, "MICRO,overlap (ns per iteration: work alone / work + one early cold load / cold load alone ~1750):");
	for (kind = 0; kind < 4; kind++)
		n += snprintf(line + n, sizeof(line) - n, " [%s: %.0f / %.0f]", names[kind], res[kind][0], res[kind][1]);
	pf_line("%s", line);
	micro_sink = sum + w;
}


/* Store patterns of the renderer: 16-byte segments down a column of rows versus whole rows */
static void micro_store_patterns(uint8_t *b)
{
	enum { W = 128, H = 32, REPS = 40 };
	volatile uint8_t *d = b + 32768;
	uint32_t src = 0x01020304u;
	float t0, t1;
	int r, x, y, k;
	char line[400];
	int n = 0;
	double bytes = (double)REPS * W * H;

	n += snprintf(line + n, sizeof(line) - n, "MICRO,store-patterns (ns per byte), %dx%d region:", W, H);

	/* whole rows, byte stores */
	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (r = 0; r < REPS; r++)
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++)
				d[y * W + x] = (uint8_t)x;
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, " rows/byte %.1f", (t1 - t0) * 1e9 / bytes);

	/* 16-byte segments, one row at a time down 16 rows, then the next segment column (block builder order) */
	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (r = 0; r < REPS; r++)
		for (x = 0; x < W; x += 16)
			for (y = 0; y < H; y++)
				for (k = 0; k < 16; k++)
					d[y * W + x + k] = (uint8_t)k;
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, ", 16B segments/byte %.1f", (t1 - t0) * 1e9 / bytes);

	/* 16-byte segments as four word stores */
	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (r = 0; r < REPS; r++)
		for (x = 0; x < W; x += 16)
			for (y = 0; y < H; y++) {
				volatile uint32_t *p = (volatile uint32_t *)(d + y * W + x);
				p[0] = src; p[1] = src; p[2] = src; p[3] = src;
			}
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, ", 16B segments/word %.1f", (t1 - t0) * 1e9 / bytes);

	/* whole rows as word stores */
	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (r = 0; r < REPS; r++)
		for (y = 0; y < H; y++) {
			volatile uint32_t *p = (volatile uint32_t *)(d + y * W);
			for (x = 0; x < W / 4; x++)
				p[x] = src;
		}
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, ", rows/word %.1f", (t1 - t0) * 1e9 / bytes);

	/* 18-byte spans at scattered rows (one span per row, like textured spans) */
	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (r = 0; r < REPS; r++)
		for (y = 0; y < H; y++)
			for (k = 0; k < 18; k++)
				d[y * W + 5 + k] = (uint8_t)k;
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, ", 18B span per row %.1f (per byte)", (t1 - t0) * 1e9 / ((double)REPS * H * 18));
	pf_line("%s", line);
	micro_sink = src;
}


/* libm cost: the FPU is single precision, so anything that goes through double is software */
static void micro_math(void)
{
	enum { N = 2000 };
	volatile float in = 0.37f;
	volatile double ind = 1.7;
	float t0, t1, acc = 0;
	double accd = 0;
	int i;
	char line[240];
	int n = 0;

	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (i = 0; i < N; i++)
		acc += sinf(in + i * 0.01f);
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, "MICRO,math,sinf %.0f ns", (double)(t1 - t0) * 1e9 / N);

	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (i = 0; i < N; i++)
		acc += cosf(in + i * 0.01f);
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, ",cosf %.0f", (double)(t1 - t0) * 1e9 / N);

	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (i = 0; i < N; i++)
		acc += sqrtf(in + i);
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, ",sqrtf %.0f", (double)(t1 - t0) * 1e9 / N);

	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (i = 0; i < N; i++)
		accd += sqrt(ind + i);
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, ",sqrt(double) %.0f", (double)(t1 - t0) * 1e9 / N);

	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (i = 0; i < N; i++)
		acc += 1.0f / (in + i);
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, ",fdiv %.0f", (double)(t1 - t0) * 1e9 / N);

	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (i = 0; i < N; i++)
		accd += ind * (i + 1.0) + ind / (i + 1.0);
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, ",double mul+div %.0f", (double)(t1 - t0) * 1e9 / N);

	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (i = 0; i < N; i++)
		acc += floorf(in + i * 0.37f) + fabsf(in - i);
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, ",floorf+fabsf %.0f", (double)(t1 - t0) * 1e9 / N);

	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (i = 0; i < N; i++)
		acc += atan2f(in + i, 3.0f);
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, ",atan2f %.0f", (double)(t1 - t0) * 1e9 / N);
	pf_line("%s", line);
	micro_sink = (uint32_t)(int)acc + (uint32_t)(int)accd;
}


/* Same store/load loops on a small stack array (internal SRAM?) */
static void micro_stack(void)
{
	uint32_t st[512]; /* 2 KB */
	volatile uint32_t *v = st;
	uint32_t r, i, sum = 0;
	float t0, t1;
	char line[200];
	int n;

	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (r = 0; r < 4000; r++)
		for (i = 0; i < 512; i++)
			v[i] = i;
	t1 = pdprof_elapsed();
	n = snprintf(line, sizeof(line), "MICRO,stack 2KB,word store %.1f ns", (double)(t1 - t0) * 1e9 / (4000.0 * 512));

	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (r = 0; r < 4000; r++)
		for (i = 0; i < 512; i++)
			((volatile uint8_t *)st)[i] = (uint8_t)i;
	t1 = pdprof_elapsed();
	n += snprintf(line + n, sizeof(line) - n, ",byte store %.1f", (double)(t1 - t0) * 1e9 / (4000.0 * 512));

	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (r = 0; r < 4000; r++)
		for (i = 0; i < 512; i++)
			sum += v[i];
	t1 = pdprof_elapsed();
	snprintf(line + n, sizeof(line) - n, ",word load %.1f ns", (double)(t1 - t0) * 1e9 / (4000.0 * 512));
	micro_sink = sum;
	pf_line("%s", line);
}


static void micro(void)
{
	enum { ALU_N = 100000, BUF = 512 * 1024 };
	uint8_t *buf = qembd_pd->system->realloc(NULL, BUF);
	uint8_t stk[64];
	uint32_t x = 1, i;
	float t0, t1;

	/* 16 dependent single-cycle adds per iteration: ns/iter ~ 16 cycles */
	qembd_pd->system->resetElapsedTime();
	t0 = pdprof_elapsed();
	for (i = 0; i < ALU_N; i++) {
		asm volatile(
			"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
			"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
			"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
			"add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n add %0,%0,#1\n"
			: "+r"(x));
	}
	t1 = pdprof_elapsed();
	pf_line("MICRO,alu,%u ns per 16 dependent adds,~%u MHz", (unsigned)((t1 - t0) * 1e9f / ALU_N),
		(unsigned)(16.0f * ALU_N / ((t1 - t0) * 1e6f)));
	micro_sink = x;

	if (buf) {
		micro_sweep("heap", buf, BUF);
		micro_write(buf, BUF);
		/* write speed */
		qembd_pd->system->resetElapsedTime();
		t0 = pdprof_elapsed();
		for (i = 0; i < 8; i++)
			memset(buf, (int)i, BUF);
		t1 = pdprof_elapsed();
		pf_line("MICRO,heap,memset 512KB,%.0f MB/s", 8.0 * BUF / ((double)(t1 - t0) * 1e6));
		qembd_pd->system->realloc(buf, 0);
	}
	micro_stack();
	micro_math();
	micro_overlap_kinds(micro_bss);
	micro_store_patterns(micro_bss);
	micro_store_spacing(micro_bss);
	micro_mem2(micro_bss, sizeof(micro_bss));
	micro_sweep("bss", micro_bss, sizeof(micro_bss));
	micro_write(micro_bss, sizeof(micro_bss));
	pf_line("MICRO,addresses,stack=%p bss=%p heapbuf=%p text=%p", (void *)stk, (void *)micro_bss, (void *)buf, (void *)pdprof_init);
}

void pdprof_init(void)
{
	char hdr[768];
	int n, i;
	uint32_t ms0, ms1, calls = 0;
	float t, prev, mind = 1.0f;

	pdprof_open();

	/* Timer self-test: cost per call and smallest step */
	qembd_pd->system->resetElapsedTime();
	ms0 = qembd_pd->system->getCurrentTimeMilliseconds();
	prev = pdprof_elapsed();
	for (i = 0; i < 20000; i++) {
		t = pdprof_elapsed();
		if (t > prev && t - prev < mind)
			mind = t - prev;
		prev = t;
		calls++;
	}
	ms1 = qembd_pd->system->getCurrentTimeMilliseconds();
	call_ticks = (uint32_t)((double)(ms1 - ms0) * 1e-3 / calls * 168e6);
	pf_line("CAL,calls=%u,ms=%u,min_step_ns=%u,call_ticks=%u", (unsigned)calls, (unsigned)(ms1 - ms0), (unsigned)(mind * 1e9f), (unsigned)call_ticks);

	/* Column names for the P lines (times in us, then counters, then scene stats) */
	n = snprintf(hdr, sizeof(hdr), "H,ms,period");
	for (i = 0; i < P_NSECT; i++)
		n += snprintf(hdr + n, sizeof(hdr) - n, ",%s", sect_names[i]);
	for (i = 0; i < C_NCNT; i++)
		n += snprintf(hdr + n, sizeof(hdr) - n, ",%s", cnt_names[i]);
	snprintf(hdr + n, sizeof(hdr) - n, ",edicts,amodels,parts,edges,surfs,cltime,keygame,demo,svedicts");
	pf_line("%s", hdr);
	micro();
	base_ms = qembd_pd->system->getCurrentTimeMilliseconds();
}

/* Per-builtin QuakeC call cost, cumulative; written out now and then as BI lines */
#define BI_MAX 128
static uint32_t bi_ticks[BI_MAX], bi_n[BI_MAX];

void pdprof_bi_add(int builtin, uint32_t ticks)
{
	if (builtin < 0 || builtin >= BI_MAX)
		return;
	bi_ticks[builtin] += ticks > call_ticks ? ticks - call_ticks : 0;
	bi_n[builtin]++;
}

static void bi_report(void)
{
	char line[900];
	int n = snprintf(line, sizeof(line), "BI,%u", (unsigned)(qembd_pd->system->getCurrentTimeMilliseconds() - base_ms));
	int i;

	for (i = 0; i < BI_MAX && n < (int)sizeof(line) - 40; i++)
		if (bi_n[i])
			n += snprintf(line + n, sizeof(line) - n, ",%d:%u:%u", i, (unsigned)bi_n[i], (unsigned)(bi_ticks[i] / TICKS_PER_US));
	pf_line("%s", line);
}

void pdprof_count_spans(void *espans)
{
	espan_t *s;

	for (s = espans; s; s = s->pnext) {
		pdprof_cnt[C_SPANS]++;
		pdprof_cnt[C_PIXELS] += s->count;
	}
	pdprof_cnt[C_DRAWN]++;
}

#ifdef PD_BENCH_GAMEPLAY
/*
 * Scripted play instead of demos: start e1m1, walk forward, turn one way and the
 * other, fire now and then. Real-time (not frame-exact), so use it for
 * per-section averages including the server/QuakeLC cost that demos do not have.
 */
static int gp_started, gp_frame0;

static void gameplay_tick(void)
{
	int f;

	if (!gp_started) {
		if (host_framecount > 60) {
			cls.demonum = -1;
			Cbuf_AddText("map e1m1\n");
			gp_started = 1;
			gp_frame0 = 0;
			pf_line("BENCH,%u,start,gameplay", (unsigned)(qembd_pd->system->getCurrentTimeMilliseconds() - base_ms));
		}
		return;
	}
	if (cls.state != ca_connected || cls.signon != SIGNONS || cls.demoplayback) {
		return;
	}
	if (!gp_frame0)
		gp_frame0 = host_framecount;
	key_dest = key_game;
	f = host_framecount - gp_frame0;
	if (f == 30)
		Cbuf_AddText("+forward\n");
	if (f % 240 == 60)
		Cbuf_AddText("-right\n+left\n");
	if (f % 240 == 180)
		Cbuf_AddText("-left\n+right\n");
	if (f % 120 == 20)
		Cbuf_AddText("+attack\n");
	if (f % 120 == 40)
		Cbuf_AddText("-attack\n");
}
#endif

#ifdef PD_BENCH
/*
 * Play the three attract-mode demos back with `timedemo` (every demo frame is
 * rendered, none skipped), so each build renders exactly the same scenes and
 * can be compared frame by frame through cl.time.
 */
static const char *const bench_demos[] = {"demo1", "demo2", "demo3"};
#ifndef PD_BENCH_COUNT
#define PD_BENCH_COUNT 3
#endif
#define NUM_BENCH PD_BENCH_COUNT
static int bench_i = -1, bench_running;

static void bench_start(int i)
{
	char cmd[32];

#ifdef PD_BENCH_CMDS
	Cbuf_AddText(PD_BENCH_CMDS "\n");
#endif
	cls.demonum = -1; /* no attract-mode chaining */
	snprintf(cmd, sizeof(cmd), "timedemo %s\n", bench_demos[i]);
	Cbuf_AddText(cmd);
	bench_i = i;
	bench_running = 0;
	pf_line("BENCH,%u,start,%s", (unsigned)(qembd_pd->system->getCurrentTimeMilliseconds() - base_ms), bench_demos[i]);
}

static void bench_tick(void)
{
	if (bench_i < 0) {
		if (host_framecount > 60)
			bench_start(0);
		return;
	}
	if (bench_i >= NUM_BENCH)
		return;
	if (cls.timedemo) {
		bench_running = 1;
		key_dest = key_game; /* keep the console off the view */
	} else if (bench_running) {
		pf_line("BENCH,%u,done,%s", (unsigned)(qembd_pd->system->getCurrentTimeMilliseconds() - base_ms), bench_demos[bench_i]);
		if (bench_i + 1 < NUM_BENCH)
			bench_start(bench_i + 1);
		else {
			bench_i = NUM_BENCH;
			pf_line("BENCH,%u,all-done", (unsigned)(qembd_pd->system->getCurrentTimeMilliseconds() - base_ms));
			flush();
		}
	}
}
#endif

void pdprof_frame_begin(void)
{
	memset(pdprof_acc, 0, sizeof(pdprof_acc));
	memset(pdprof_cnt, 0, sizeof(pdprof_cnt));
	memset(pdprof_calls, 0, sizeof(pdprof_calls));
	{
		uintptr_t sp;

		__asm__ volatile("mov %0, sp" : "=r"(sp));
		base_sp = sp;
	}
	entry_ms = qembd_pd->system->getCurrentTimeMilliseconds();
	qembd_pd->system->resetElapsedTime();
	PROF_BEGIN(P_FRAME);
}

void pdprof_frame_end(void)
{
	uint32_t period, ms;
	int nparts = 0, n, i;
	particle_t *p;

	PROF_END(P_FRAME);
	{
		int k, changed = 0;
		char line[200];
		int len = snprintf(line, sizeof(line), "STK");

		for (k = 0; k < K_NSTK; k++) {
			if (pdprof_minsp[k] != ~(uintptr_t)0 && (reported_sp[k] == 0 || pdprof_minsp[k] < reported_sp[k])) {
				reported_sp[k] = pdprof_minsp[k];
				changed = 1;
			}
			if (reported_sp[k])
				len += snprintf(line + len, sizeof(line) - len, ",%s=%u", stk_names[k], (unsigned)(base_sp - reported_sp[k]));
		}
		if (changed) {
			flush();
			pf_line("%s", line);
		}
	}
	{
		/* memory: log the hunk and the surface cache when the map changes and whenever the cache starts thrashing */
		extern int hunk_size, hunk_low_used, hunk_high_used, sc_size;
		extern qboolean r_cache_thrash;
		static int last_free = -1, last_thrash = -1;
		int freeb = hunk_size - hunk_low_used - hunk_high_used;

		if (freeb != last_free || (int)r_cache_thrash != last_thrash) {
			last_free = freeb;
			last_thrash = (int)r_cache_thrash;
			flush();
			pf_line("MEM,%u,hunk %d KB, free %d KB, low %d KB, high %d KB, surfcache %d KB, thrash %d", (unsigned)(entry_ms - base_ms),
				hunk_size / 1024, freeb / 1024, hunk_low_used / 1024, hunk_high_used / 1024, sc_size / 1024, (int)r_cache_thrash);
		}
	}
	if (host_framecount == last_hostframe) { /* Host_FilterTime skipped it */
		last_entry = entry_ms;
		return;
	}
	last_hostframe = host_framecount;
	period = last_entry ? (entry_ms - last_entry) * 1000u : 0;
	last_entry = entry_ms;
	ms = entry_ms - base_ms;

	for (p = active_particles; p; p = p->next)
		nparts++;

	if (cl.worldmodel && strcmp(last_map, cl.worldmodel->name) != 0) {
		flush();
		strncpy(last_map, cl.worldmodel->name, sizeof(last_map) - 1);
		pf_line("PM,%u,%s", (unsigned)ms, last_map);
	}
	if (cls.demonum != last_demo) {
		flush();
		last_demo = cls.demonum;
		pf_line("PD,%u,demonum=%d,playback=%d", (unsigned)ms, cls.demonum, cls.demoplayback);
	}

	/* Remove the cost of the timer calls: a bracket's own interval includes one call,
	 * and every enclosing section also contains both calls of each nested bracket. */
	for (i = 0; i < P_NSECT; i++) {
		uint32_t own = pdprof_calls[i] * call_ticks, nested = pdprof_calls[i] * 2 * call_ticks;
		int a;

		pdprof_acc[i] = pdprof_acc[i] > own ? pdprof_acc[i] - own : 0;
		for (a = parent_of[i]; a >= 0; a = parent_of[a])
			pdprof_acc[a] = pdprof_acc[a] > nested ? pdprof_acc[a] - nested : 0;
	}

	n = snprintf(batch + blen, sizeof(batch) - blen, "P,%u,%u", (unsigned)ms, (unsigned)period);
	for (i = 0; i < P_NSECT; i++)
		n += snprintf(batch + blen + n, sizeof(batch) - blen - n, ",%u", (unsigned)US(pdprof_acc[i]));
	for (i = 0; i < C_NCNT; i++)
		n += snprintf(batch + blen + n, sizeof(batch) - blen - n, ",%u", (unsigned)pdprof_cnt[i]);
	n += snprintf(batch + blen + n, sizeof(batch) - blen - n, ",%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
		cl_numvisedicts, r_amodels_drawn, nparts,
		(int)(edge_p - r_edges), (int)(surface_p - surfaces),
		(int)(cl.time * 1000), key_dest == key_game, cls.demoplayback, sv.active ? sv.num_edicts : 0);
	if (n > 0 && n < (int)sizeof(batch) - blen)
		blen += n;
	if ((host_framecount & 255) == 0)
		bi_report();
	if (++bcount >= 12 || blen > (int)sizeof(batch) - 384)
		flush();

#if defined(PD_BENCH_GAMEPLAY)
	gameplay_tick();
#elif defined(PD_BENCH)
	bench_tick();
#endif
}
