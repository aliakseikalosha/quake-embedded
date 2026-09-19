/*
 * Playdate video: Quake's 8-bit paletted frame -> 400x240 1-bit LCD.
 *
 * Every pixel is turned into a luminance, contrast-stretched
 * (Quake is dark and a 1-bit panel loses shadow detail), and then ordered-
 * dithered with a 4x4 Bayer matrix. When Quake renders below the panel size
 * the image is stretched with nearest-neighbour lookup tables.
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

/* Quake is dark and low-contrast; stretch [BLACK_POINT, WHITE_POINT] to the
 * full range before a mild gamma so surfaces separate after dithering. */
#define BLACK_POINT 4.0f
#define WHITE_POINT 120.0f
#define GAMMA 0.8f

static uint16_t xmap[LCD_COLUMNS];	/* LCD column -> Quake column */
static uint16_t ymap[LCD_ROWS];		/* LCD row -> Quake row */
static uint8_t gamma_lut[256];
static uint8_t threshold[4][4];

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

	for (int x = 0; x < LCD_COLUMNS; x++)
		xmap[x] = x * PD_RENDER_WIDTH / LCD_COLUMNS;
	for (int y = 0; y < LCD_ROWS; y++)
		ymap[y] = y * PD_RENDER_HEIGHT / LCD_ROWS;
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
	uint8_t lum[256];
	uint8_t *frame = qembd_pd->graphics->getFrame();

	/* Palette (0x00RRGGBB) -> gamma-corrected luminance */
	for (int i = 0; i < 256; i++) {
		uint32_t c = clut[i];
		uint32_t l = (77 * ((c >> 16) & 0xff) + 151 * ((c >> 8) & 0xff) +
		              28 * (c & 0xff)) >> 8;
		lum[i] = gamma_lut[l];
	}

	/* Quake rect -> LCD rect, widened to whole bytes; Quake keeps the full
	 * frame in src so redrawing a little extra is harmless. */
	int y0 = y * LCD_ROWS / PD_RENDER_HEIGHT;
	int y1 = ((y + ysize) * LCD_ROWS + PD_RENDER_HEIGHT - 1) / PD_RENDER_HEIGHT;
	int b0 = (x * LCD_COLUMNS / PD_RENDER_WIDTH) >> 3;
	int b1 = (((x + xsize) * LCD_COLUMNS + PD_RENDER_WIDTH - 1) / PD_RENDER_WIDTH + 7) >> 3;

	if (y1 > LCD_ROWS)
		y1 = LCD_ROWS;
	if (b1 > LCD_COLUMNS / 8)
		b1 = LCD_COLUMNS / 8;

	for (int py = y0; py < y1; py++) {
		const uint8_t *srow = src + ymap[py] * PD_RENDER_WIDTH;
		const uint8_t *thr = threshold[py & 3];
		uint8_t *dst = frame + py * LCD_ROWSIZE;

		for (int bx = b0; bx < b1; bx++) {
			const uint16_t *xm = &xmap[bx * 8];
			unsigned bits = 0;

			/* MSB = leftmost pixel, 1 = white */
			for (int k = 0; k < 8; k++)
				bits = (bits << 1) | (lum[srow[xm[k]]] > thr[k & 3]);
			dst[bx] = (uint8_t) bits;
		}
	}
}

void qembd_refresh()
{
	qembd_pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
}
