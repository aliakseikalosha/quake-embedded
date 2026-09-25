/*
 * Playdate video: Quake's 8-bit paletted frame -> 400x240 1-bit LCD.
 *
 * Every pixel is turned into a luminance, contrast-stretched
 * (Quake is dark and a 1-bit panel loses shadow detail), and then compared
 * against a 4x4 Bayer matrix (ordered dithering).
 * Quake renders at PD_RENDER_WIDTH x PD_RENDER_HEIGHT (320x240 by default) and is drawn 1:1, centred on the
 * panel; the border stays black.
 */

#include <math.h>
#include <string.h>
#include <quakembd.h>
#include "pd_port.h"

#ifndef PD_RENDER_WIDTH
#define PD_RENDER_WIDTH 320
#endif
#ifndef PD_RENDER_HEIGHT
#define PD_RENDER_HEIGHT 200
#endif

_Static_assert(PD_RENDER_WIDTH >= 320 && PD_RENDER_HEIGHT >= 200,
			   "Quake's menus need at least 320x200");
_Static_assert(PD_RENDER_WIDTH <= LCD_COLUMNS && PD_RENDER_HEIGHT <= LCD_ROWS,
			   "Rendering above the panel resolution is pointless");

/* Top-left of the image on the LCD. X must stay byte-aligned. */
#define X_OFF ((LCD_COLUMNS - PD_RENDER_WIDTH) / 2)
#define Y_OFF ((LCD_ROWS - PD_RENDER_HEIGHT) / 2)

_Static_assert(X_OFF % 8 == 0 && PD_RENDER_WIDTH % 8 == 0,
			   "Width and centring offset must be byte aligned");

/* Quake is dark and low-contrast; stretch [BLACK_POINT, WHITE_POINT] to the
 * full range before a mild gamma so surfaces separate after dithering. */
#define BLACK_POINT 4.0f
#define WHITE_POINT 120.0f
#define GAMMA 0.8f

#ifdef PD_LOWRES_3D
/*
 * The 3D view is rendered at half resolution and pixel-doubled by the engine
 * (D_UpscaleScreen). Inside that rectangle every uniform 2x2 block is drawn
 * as one of 5 fixed patterns instead of being dithered per pixel:
 *   0: 00/00   1: 10/00   2: 10/01   3: 10/11   4: 11/11   (top/bottom, 1 = white)
 * Levels 1-3 have equally bright variants:
 *   1: the white pixel in any of the 4 corners
 *   2: diagonal 10/01, anti-diagonal 01/10, vertical 10/10, horizontal 00/11
 *   3: the black pixel in any of the 4 corners
 * One is picked per palette colour ramp (index >> 4) so different surfaces of
 * the same brightness stay distinguishable.
 * Blocks that are not uniform (overlays drawn over the view) fall back to the
 * 4x4 Bayer dither.
 */
_Static_assert(Y_OFF % 2 == 0 && X_OFF % 2 == 0, "2x2 patterns need an even image origin");

extern int qembd_lowres_rect[4]; /* x, y, w, h in Quake pixels */

/* The half-resolution view, not yet expanded into the Quake buffer (see D_UpscaleScreen).
 * Row pair p (Quake rows 2p, 2p+1) is dithered straight from row p of qembd_lowres_src
 * while qembd_lowres_pending[p] is set; otherwise the expanded rows in the Quake buffer are used. */
extern int qembd_lowres_active;
extern const uint8_t *qembd_lowres_src;
extern int qembd_lowres_stride;
extern uint8_t qembd_lowres_pending[];

static const uint8_t var_n[5] = {1, 4, 4, 4, 1};	/* variants per level */
static const uint8_t var_top[5][4] = {
	{0x0},
	{0x2, 0x1, 0x0, 0x0},	/* white at TL, TR, BL, BR */
	{0x2, 0x1, 0x2, 0x0},	/* diagonal, anti-diagonal, vertical, horizontal */
	{0x1, 0x2, 0x3, 0x3},	/* black at TL, TR, BL, BR */
	{0x3},
};
static const uint8_t var_bot[5][4] = {
	{0x0},
	{0x0, 0x0, 0x2, 0x1},
	{0x1, 0x2, 0x2, 0x3},
	{0x3, 0x3, 0x1, 0x2},
	{0x3},
};
static uint8_t ptop[256], pbot[256]; /* palette index -> 2-bit pattern for the top/bottom row */
static uint16_t pat2[256];			 /* ptop | pbot << 8, one lookup for both rows */
#endif

static uint8_t gamma_lut[256];
static uint8_t threshold[4][4];
static uint32_t lum_clut[256]; /* palette lum[] was built from */
static uint8_t lum[256];	   /* palette index -> dithering luminance */
static int lum_valid;

int qembd_get_width()
{
	return PD_RENDER_WIDTH;
}

int qembd_get_height()
{
	return PD_RENDER_HEIGHT;
}

void qembd_vidinit()
{
	static const uint8_t bayer4[4][4] = {
		{0, 8, 2, 10},
		{12, 4, 14, 6},
		{3, 11, 1, 9},
		{15, 7, 13, 5},
	};

	for (int i = 0; i < 256; i++)
	{
		float v = (i - BLACK_POINT) / (WHITE_POINT - BLACK_POINT);

		v = v < 0 ? 0 : (v > 1 ? 1 : v);
		gamma_lut[i] = (uint8_t)(255.0f * powf(v, GAMMA) + 0.5f);
	}

	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
			threshold[y][x] = bayer4[y][x] * 16 + 8;

	memset(qembd_pd->graphics->getFrame(), 0, LCD_ROWSIZE * LCD_ROWS);
}

/* Bayer-dither 8 Quake pixels into one LCD byte (MSB = leftmost, 1 = white).
 * The 4-wide matrix repeats twice per byte (X_OFF is a multiple of 8, so the
 * phase matches the Quake column). */
static inline uint8_t dither8(const uint8_t *sp, unsigned t0, unsigned t1, unsigned t2, unsigned t3)
{
	return (uint8_t)((lum[sp[0]] > t0) << 7 |
					 (lum[sp[1]] > t1) << 6 |
					 (lum[sp[2]] > t2) << 5 |
					 (lum[sp[3]] > t3) << 4 |
					 (lum[sp[4]] > t0) << 3 |
					 (lum[sp[5]] > t1) << 2 |
					 (lum[sp[6]] > t2) << 1 |
					 (lum[sp[7]] > t3));
}

/* Dither LCD bytes [b0, b1) of one Quake row. srow is offset so that
 * srow + bx * 8 is the first Quake pixel of byte bx. */
static inline void dither_span(uint8_t *dst, const uint8_t *srow, int b0, int b1, const uint8_t *thr)
{
	const unsigned t0 = thr[0], t1 = thr[1], t2 = thr[2], t3 = thr[3];

	for (int bx = b0; bx < b1; bx++)
		dst[bx] = dither8(srow + bx * 8, t0, t1, t2, t3);
}

#ifdef PD_LOWRES_3D
/* Unaligned-safe 32-bit load (a single LDR on Cortex-M7). Pixel order is
 * little endian: byte 0 is the leftmost pixel. */
static inline uint32_t ld32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof v);
	return v;
}

/* One LCD byte (4 blocks) of a row inside the low-res view: uniform 2x2 blocks
 * become a pattern, anything else (overlays) is dithered per pixel. sp is this
 * row, op the other row of the 2x2 blocks. */
static inline uint8_t lowres_byte(const uint8_t *sp, const uint8_t *op, const uint8_t *pat, const uint8_t *thr)
{
	unsigned bits = 0;

	for (int i = 0; i < 8; i += 2)
	{
		uint8_t a = sp[i];
		unsigned p;

		if (a == sp[i + 1] && a == op[i] && a == op[i + 1])
			p = pat[a];
		else
			p = (lum[a] > thr[i & 3]) << 1 | (lum[sp[i + 1]] > thr[(i + 1) & 3]);
		bits = bits << 2 | p;
	}
	return (uint8_t)bits;
}

/*
 * LCD bytes [b0, b1) of the row pair (top, bottom = top + 1) inside the
 * low-res view. Almost every byte is four uniform blocks, so test that with
 * a few word operations and produce both rows from one lookup per block.
 */
static void lowres_pair(uint8_t *dt, uint8_t *db, const uint8_t *st, const uint8_t *sb,
						int b0, int b1, const uint8_t *thr_t, const uint8_t *thr_b)
{
	for (int bx = b0; bx < b1; bx++)
	{
		const uint8_t *t = st + bx * 8;
		const uint8_t *b = sb + bx * 8;
		const uint32_t t0 = ld32(t), t1 = ld32(t + 4);

		/* both rows identical, and every pixel equal to its right neighbour
		 * inside its block (bytes 0-1 and 2-3 of each word) */
		if (t0 == ld32(b) && t1 == ld32(b + 4) &&
			!(((t0 ^ (t0 >> 8)) | (t1 ^ (t1 >> 8))) & 0x00ff00ffu))
		{
			/* each pat2 entry: top pattern in bits 0-1, bottom in bits 8-9 */
			const uint32_t v = (uint32_t)pat2[t0 & 0xff] << 6 |
							   (uint32_t)pat2[(t0 >> 16) & 0xff] << 4 |
							   (uint32_t)pat2[t1 & 0xff] << 2 |
							   (uint32_t)pat2[(t1 >> 16) & 0xff];

			dt[bx] = (uint8_t)v;
			db[bx] = (uint8_t)(v >> 8);
		}
		else
		{
			dt[bx] = lowres_byte(t, b, ptop, thr_t);
			db[bx] = lowres_byte(b, t, pbot, thr_b);
		}
	}
}

/*
 * LCD bytes [b0, b1) of a row pair straight from one half-resolution row.
 * lrow is offset so that lrow + bx * 4 is the first source pixel of byte bx.
 * Every 2x2 block is uniform here, so this produces exactly what lowres_pair
 * produces from the expanded rows.
 */
static void lowres_direct(uint8_t *dt, uint8_t *db, const uint8_t *lrow, int b0, int b1)
{
	for (int bx = b0; bx < b1; bx++)
	{
		const uint32_t w = ld32(lrow + bx * 4);
		const uint32_t v = (uint32_t)pat2[w & 0xff] << 6 |
						   (uint32_t)pat2[(w >> 8) & 0xff] << 4 |
						   (uint32_t)pat2[(w >> 16) & 0xff] << 2 |
						   (uint32_t)pat2[w >> 24];

		dt[bx] = (uint8_t)v;
		db[bx] = (uint8_t)(v >> 8);
	}
}
#endif

void qembd_fillrect(uint8_t *src, uint32_t *clut,
					uint16_t x, uint16_t y, uint16_t xsize, uint16_t ysize)
{
	uint8_t *frame = qembd_pd->graphics->getFrame();

	/* Palette (0x00RRGGBB) -> gamma-corrected luminance. The palette rarely
	 * changes (only on flashes/gamma), so rebuild only when it differs. */
	if (!lum_valid || memcmp(lum_clut, clut, sizeof(lum_clut)) != 0)
	{
		for (int i = 0; i < 256; i++)
		{
			uint32_t c = clut[i];
			uint32_t l = (77 * ((c >> 16) & 0xff) + 151 * ((c >> 8) & 0xff) +
						  28 * (c & 0xff)) >>
						 8;
			lum[i] = gamma_lut[l];
		}
#ifdef PD_LOWRES_3D
		for (int i = 0; i < 256; i++)
		{
			int lv = (lum[i] * 4 + 128) >> 8;

			int v = (i >> 4) % var_n[lv];

			ptop[i] = var_top[lv][v];
			pbot[i] = var_bot[lv][v];
			pat2[i] = (uint16_t)(ptop[i] | pbot[i] << 8);
		}
#endif
		memcpy(lum_clut, clut, sizeof(lum_clut));
		lum_valid = 1;
	}

	/* Quake rect -> LCD bytes. X_OFF and the width are byte aligned, so
	 * widening to whole bytes only redraws pixels inside the image. */
	int b0 = (X_OFF + x) >> 3;
	int b1 = (X_OFF + x + xsize + 7) >> 3;
	int bmax = (X_OFF + PD_RENDER_WIDTH) >> 3;

	if (b1 > bmax)
		b1 = bmax;
	if (y + ysize > PD_RENDER_HEIGHT)
		ysize = PD_RENDER_HEIGHT - y;

#ifdef PD_LOWRES_3D
	/* Patterns span two rows: refresh whole row pairs. */
	if (ysize)
	{
		int y1 = (y + ysize + 1) & ~1;

		y &= ~1;
		ysize = y1 - y;
		if (y + ysize > PD_RENDER_HEIGHT)
			ysize = PD_RENDER_HEIGHT - y;
	}

	int rb0 = 0, rb1 = 0, ry0 = 0, ry1 = 0;

	if (qembd_lowres_rect[2] > 0)
	{
		rb0 = (X_OFF + qembd_lowres_rect[0] + 7) >> 3;
		rb1 = (X_OFF + qembd_lowres_rect[0] + qembd_lowres_rect[2]) >> 3;
		ry0 = qembd_lowres_rect[1];
		ry1 = ry0 + qembd_lowres_rect[3];
	}
#endif

	for (int qy = y; qy < y + ysize;)
	{
		const uint8_t *srow = src + qy * PD_RENDER_WIDTH - X_OFF;
		uint8_t *dst = frame + (Y_OFF + qy) * LCD_ROWSIZE;

#ifdef PD_LOWRES_3D
		if (qy >= ry0 && qy < ry1)
		{
			/* qy is even here: y, ry0 and ry1 are, and rows before ry0 step by one */
			const uint8_t *sbot = srow + PD_RENDER_WIDTH;
			uint8_t *dbot = dst + LCD_ROWSIZE;
			const uint8_t *thr_t = threshold[(Y_OFF + qy) & 3];
			const uint8_t *thr_b = threshold[(Y_OFF + qy + 1) & 3];
			int f0 = rb0 < b0 ? b0 : (rb0 > b1 ? b1 : rb0);	/* view columns: [f0, f1) */
			int f1 = rb1 < f0 ? f0 : (rb1 > b1 ? b1 : rb1);

			dither_span(dst, srow, b0, f0, thr_t);
			dither_span(dbot, sbot, b0, f0, thr_b);
			if (qembd_lowres_active && qembd_lowres_pending[qy >> 1])
				lowres_direct(dst, dbot, qembd_lowres_src + (qy >> 1) * qembd_lowres_stride - (X_OFF >> 1),
							  f0, f1);
			else
				lowres_pair(dst, dbot, srow, sbot, f0, f1, thr_t, thr_b);
			dither_span(dst, srow, f1, b1, thr_t);
			dither_span(dbot, sbot, f1, b1, thr_b);
			qy += 2;
			continue;
		}
#endif

		dither_span(dst, srow, b0, b1, threshold[(Y_OFF + qy) & 3]);
		qy++;
	}
}

void qembd_refresh()
{
	qembd_pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
}
