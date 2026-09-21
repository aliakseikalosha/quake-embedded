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

static uint8_t gamma_lut[256];
static uint8_t threshold[4][4];
static uint32_t lum_clut[256];		/* palette lum[] was built from */
static uint8_t lum[256];		/* palette index -> dithering luminance */
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
		{ 0,  8,  2, 10},
		{12,  4, 14,  6},
		{ 3, 11,  1,  9},
		{15,  7, 13,  5},
	};

	for (int i = 0; i < 256; i++) {
		float v = (i - BLACK_POINT) / (WHITE_POINT - BLACK_POINT);

		v = v < 0 ? 0 : (v > 1 ? 1 : v);
		gamma_lut[i] = (uint8_t) (255.0f * powf(v, GAMMA) + 0.5f);
	}

	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
			threshold[y][x] = bayer4[y][x] * 16 + 8;

	memset(qembd_pd->graphics->getFrame(), 0, LCD_ROWSIZE * LCD_ROWS);
}

void qembd_fillrect(uint8_t *src, uint32_t *clut,
                    uint16_t x, uint16_t y, uint16_t xsize, uint16_t ysize)
{
	uint8_t *frame = qembd_pd->graphics->getFrame();

	/* Palette (0x00RRGGBB) -> gamma-corrected luminance. The palette rarely
	 * changes (only on flashes/gamma), so rebuild only when it differs. */
	if (!lum_valid || memcmp(lum_clut, clut, sizeof(lum_clut)) != 0) {
		for (int i = 0; i < 256; i++) {
			uint32_t c = clut[i];
			uint32_t l = (77 * ((c >> 16) & 0xff) + 151 * ((c >> 8) & 0xff) +
			              28 * (c & 0xff)) >> 8;
			lum[i] = gamma_lut[l];
		}
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

	for (int qy = y; qy < y + ysize; qy++) {
		int py = Y_OFF + qy;
		const uint8_t *srow = src + qy * PD_RENDER_WIDTH - X_OFF;
		const uint8_t *thr = threshold[py & 3];
		uint8_t *dst = frame + py * LCD_ROWSIZE;
		const unsigned t0 = thr[0], t1 = thr[1], t2 = thr[2], t3 = thr[3];

		for (int bx = b0; bx < b1; bx++) {
			const uint8_t *sp = srow + bx * 8;

			/* MSB = leftmost pixel, 1 = white; the 4-wide matrix repeats
			 * twice per byte (X_OFF is a multiple of 8, so the phase
			 * matches the Quake column). */
			dst[bx] = (uint8_t) ((lum[sp[0]] > t0) << 7 |
			                     (lum[sp[1]] > t1) << 6 |
			                     (lum[sp[2]] > t2) << 5 |
			                     (lum[sp[3]] > t3) << 4 |
			                     (lum[sp[4]] > t0) << 3 |
			                     (lum[sp[5]] > t1) << 2 |
			                     (lum[sp[6]] > t2) << 1 |
			                     (lum[sp[7]] > t3));
		}
	}
}

void qembd_refresh()
{
	qembd_pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
}
