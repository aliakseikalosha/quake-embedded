/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// d_scan.c
//
// Portable C scan-level rasterization code, all pixel depths.

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"
#include "pdprof.h"

unsigned char	*r_turb_pbase, *r_turb_pdest;
fixed16_t		r_turb_s, r_turb_t, r_turb_sstep, r_turb_tstep;
int				*r_turb_turb;
int				r_turb_spancount;

/*
=============
D_WarpScreen

// this performs a slight compression of the screen at the same time as
// the sine warp, to keep the edges from wrapping
=============
*/
void D_WarpScreen (void)
{
	int		w, h, src_w, src_h;
	int		u,v;
	byte	*dest;
	int		*turb;
	int		*col;
	byte	**row;
	static byte	*rowptr[MAXHEIGHT+(AMP2*2)];	// static: keep big buffers off the small device stack
	static int	column[MAXWIDTH+(AMP2*2)];
	float	wratio, hratio;
	unsigned int	_vid_rowbytes;

	w = r_refdef.vrect.width;
	h = r_refdef.vrect.height;
	src_w = scr_vrect.width;
	src_h = scr_vrect.height;

	wratio = w / (float)src_w;
	hratio = h / (float)src_h;

	for (v=0 ; v<src_h+AMP2*2 ; v++)
	{
		rowptr[v] = d_viewbuffer + (r_refdef.vrect.y * screenwidth) +
				 (screenwidth * (int)((float)v * hratio * h / (h + AMP2 * 2)));
	}

	for (u=0 ; u<src_w+AMP2*2 ; u++)
	{
		column[u] = r_refdef.vrect.x +
				(int)((float)u * wratio * w / (w + AMP2 * 2));
	}

	_vid_rowbytes = vid.rowbytes;
	turb = intsintable + ((int)(cl.time*SPEED)&(CYCLE-1));
	dest = vid.buffer + scr_vrect.y * _vid_rowbytes + scr_vrect.x;

	for (v=0 ; v<src_h ; v++, dest += _vid_rowbytes)
	{
		col = &column[turb[v]];
		row = &rowptr[v];

		for (u=0 ; u<src_w ; u+=4)
		{
			dest[u+0] = row[turb[u+0]][col[u+0]];
			dest[u+1] = row[turb[u+1]][col[u+1]];
			dest[u+2] = row[turb[u+2]][col[u+2]];
			dest[u+3] = row[turb[u+3]][col[u+3]];
		}
	}
}

#ifdef PD_LOWRES_3D
/* View rectangle (in full-resolution screen pixels) that holds the pixel-doubled
 * 3D view. The display layer expands 2x2 blocks in it into 5-level patterns. */
int	qembd_lowres_rect[4];

/*
 * The 3D view is rendered at half resolution into r_warpbuffer. Expanding it
 * into vid.buffer costs 96 KB of writes (and the display pass then reads them
 * back), which on this device is dominated by cache misses. Instead each row
 * pair of the view is marked pending, and the display layer dithers pending
 * pairs straight from the half-resolution buffer. Only row pairs that
 * something draws over (console, menu, HUD text; see DRAW_TOUCH in draw.h)
 * are expanded into vid.buffer, so the overlay has its background.
 */
int			qembd_lowres_active;			// a low-res view is waiting for the display
const byte	*qembd_lowres_src;				// half-resolution buffer (row 0)
int			qembd_lowres_stride;
byte		qembd_lowres_pending[PD_RENDER_HEIGHT / 2];	// per full-res row pair

// Expand one half-resolution row into the two rows of its pair in vid.buffer
static void D_UpscaleRow (int p)
{
	int		u;
	int		w = qembd_lowres_rect[2] >> 1;
	const byte	*src = qembd_lowres_src + p * qembd_lowres_stride + (qembd_lowres_rect[0] >> 1);
	byte	*dest = vid.buffer + (p * 2) * vid.rowbytes + qembd_lowres_rect[0];
	unsigned int	*d0 = (unsigned int *)dest;
	unsigned int	*d1 = (unsigned int *)(dest + vid.rowbytes);

	// four source pixels -> two words of doubled pixels, on both rows
	for (u=0 ; u+4<=w ; u+=4)
	{
		unsigned int	s, lo, hi;

		memcpy (&s, src + u, sizeof(s));
		lo = s & 0xffff;
		hi = s >> 16;
		lo = ((lo | (lo << 8)) & 0x00ff00ff) * 0x101;	// p0 p0 p1 p1
		hi = ((hi | (hi << 8)) & 0x00ff00ff) * 0x101;	// p2 p2 p3 p3
		d0[0] = d1[0] = lo;
		d0[1] = d1[1] = hi;
		d0 += 2;
		d1 += 2;
	}

	for ( ; u<w ; u++)
	{
		unsigned short	*e0 = (unsigned short *)d0;
		unsigned short	*e1 = (unsigned short *)d1;

		e0[0] = e1[0] = (unsigned short)(src[u] * 0x0101);
		d0 = (unsigned int *)(e0 + 1);
		d1 = (unsigned int *)(e1 + 1);
	}
}

/*
=============
D_LowresTouch

Called before anything draws into screen rows [y0, y1): expands the pending
view row pairs among them, once.
=============
*/
void D_LowresTouch (int y0, int y1)
{
	int		p, p0, p1;

	if (y0 < qembd_lowres_rect[1])
		y0 = qembd_lowres_rect[1];
	if (y1 > qembd_lowres_rect[1] + qembd_lowres_rect[3])
		y1 = qembd_lowres_rect[1] + qembd_lowres_rect[3];
	if (y0 >= y1)
		return;

	p0 = y0 >> 1;
	p1 = (y1 + 1) >> 1;
	for (p=p0 ; p<p1 ; p++)
	{
		if (qembd_lowres_pending[p])
		{
			D_UpscaleRow (p);
			qembd_lowres_pending[p] = 0;
		}
	}
}

/*
=============
D_LowresEndFrame

The display has consumed the frame; nothing is pending any more.
=============
*/
void D_LowresEndFrame (void)
{
	qembd_lowres_active = 0;
}

/*
=============
D_UpscaleScreen

Hands the half-resolution view in r_warpbuffer to the display layer (see above).
=============
*/
void D_UpscaleScreen (void)
{
	int		p;

	qembd_lowres_rect[0] = r_refdef.vrect.x * 2;
	qembd_lowres_rect[1] = r_refdef.vrect.y * 2;
	qembd_lowres_rect[2] = r_refdef.vrect.width * 2;
	qembd_lowres_rect[3] = r_refdef.vrect.height * 2;

	qembd_lowres_src = d_viewbuffer;
	qembd_lowres_stride = screenwidth;
	memset (qembd_lowres_pending, 0, sizeof(qembd_lowres_pending));
	for (p=r_refdef.vrect.y ; p<r_refdef.vrect.y + r_refdef.vrect.height ; p++)
		qembd_lowres_pending[p] = 1;
	qembd_lowres_active = 1;
}
#endif

/*
=============
D_DrawTurbulent8Span
=============
*/
static inline void D_DrawTurbulent8Span (void)
{
	int		sturb, tturb;

	// Preload global variables.
	fixed16_t _r_turb_s = r_turb_s;
	fixed16_t _r_turb_t = r_turb_t;
	const fixed16_t _r_turb_sstep = r_turb_sstep;
	fixed16_t _r_turb_tstep = r_turb_tstep;
	int _r_turb_spancount = r_turb_spancount;
	int* _r_turb_turb = r_turb_turb;
	unsigned char* _r_turb_pbase = r_turb_pbase;
	unsigned char* _r_turb_pdest = r_turb_pdest;

	do
	{
		sturb = ((_r_turb_s + _r_turb_turb[(_r_turb_t>>16)&(CYCLE-1)])>>16)&63;
		tturb = ((_r_turb_t + _r_turb_turb[(_r_turb_s>>16)&(CYCLE-1)])>>16)&63;
		*_r_turb_pdest++ = *(_r_turb_pbase + (tturb<<6) + sturb);
		_r_turb_s += _r_turb_sstep;
		_r_turb_t += _r_turb_tstep;
	} while (--_r_turb_spancount > 0);

	// Update global variables.
	r_turb_pbase = _r_turb_pbase;
	r_turb_pdest = _r_turb_pdest;
	r_turb_s = _r_turb_s;
	r_turb_t = _r_turb_t;
	r_turb_spancount = _r_turb_spancount;
}

/*
=============
Turbulent8
=============
*/
void Turbulent8 (espan_t *pspan)
{
	int				count;
	fixed16_t		snext, tnext;
	float			sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float			sdivz16stepu, tdivz16stepu, zi16stepu;
	
	r_turb_turb = sintable + ((int)(cl.time*SPEED)&(CYCLE-1));

	r_turb_sstep = 0;	// keep compiler happy
	r_turb_tstep = 0;	// ditto

	r_turb_pbase = (unsigned char *)cacheblock;

	sdivz16stepu = d_sdivzstepu * 16;
	tdivz16stepu = d_tdivzstepu * 16;
	zi16stepu = d_zistepu * 16;

	do
	{
		r_turb_pdest = (unsigned char *)((byte *)d_viewbuffer +
				(screenwidth * pspan->v) + pspan->u);

		count = pspan->count;

	// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

		r_turb_s = (int)(sdivz * z) + sadjust;
		if (r_turb_s > bbextents)
			r_turb_s = bbextents;
		else if (r_turb_s < 0)
			r_turb_s = 0;

		r_turb_t = (int)(tdivz * z) + tadjust;
		if (r_turb_t > bbextentt)
			r_turb_t = bbextentt;
		else if (r_turb_t < 0)
			r_turb_t = 0;

		do
		{
		// calculate s and t at the far end of the span
			if (count >= 16)
				r_turb_spancount = 16;
			else
				r_turb_spancount = count;

			count -= r_turb_spancount;

			if (count)
			{
			// calculate s/z, t/z, zi->fixed s and t at far end of span,
			// calculate s and t steps across span by shifting
				sdivz += sdivz16stepu;
				tdivz += tdivz16stepu;
				zi += zi16stepu;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 16)
					snext = 16;	// prevent round-off error on <0 steps from
								//  from causing overstepping & running off the
								//  edge of the texture

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 16)
					tnext = 16;	// guard against round-off error on <0 steps

				r_turb_sstep = (snext - r_turb_s) >> 4;
				r_turb_tstep = (tnext - r_turb_t) >> 4;
			}
			else
			{
			// calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
			// can't step off polygon), clamp, calculate s and t steps across
			// span by division, biasing steps low so we don't run off the
			// texture
				spancountminus1 = (float)(r_turb_spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 16)
					snext = 16;	// prevent round-off error on <0 steps from
								//  from causing overstepping & running off the
								//  edge of the texture

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 16)
					tnext = 16;	// guard against round-off error on <0 steps

				if (r_turb_spancount > 1)
				{
					r_turb_sstep = (snext - r_turb_s) / (r_turb_spancount - 1);
					r_turb_tstep = (tnext - r_turb_t) / (r_turb_spancount - 1);
				}
			}

			r_turb_s = r_turb_s & ((CYCLE<<16)-1);
			r_turb_t = r_turb_t & ((CYCLE<<16)-1);

			D_DrawTurbulent8Span ();

			r_turb_s = snext;
			r_turb_t = tnext;

		} while (count > 0);

	} while ((pspan = pspan->pnext) != NULL);
}

/*
=============
D_DrawSpans8
Note: This function is the top CPU consumer. Optimize it as far as possible!
=============
*/
#if defined(PD_FAST_SURFACES) && defined(__arm__)
/*
Span records are written by the edge scan (stores do not allocate cache lines) and read back
here, so reading each one is a cache miss. The FP set-up of a span does no loads, and an early
load overlaps with that (measured on the device: ~70% of a miss is hidden behind ALU work), so
the next span's record is requested before this span's set-up and only used after its pixels.
*/
#define SPAN_TOUCH(next, sink)	do { if (next) __asm__ volatile("ldr %0, [%1]" : "=r"(sink) : "r"(next)); } while (0)
#define SPAN_TOUCH_DONE(sink)	__asm__ volatile("" : : "r"(sink))
#else
#define SPAN_TOUCH(next, sink)	((void)0)
#define SPAN_TOUCH_DONE(sink)	((void)0)
#endif

void D_DrawSpans8 (espan_t *pspan)
{
	int		count, spancount;
	unsigned	touched = 0;
	espan_t		*pnext;
	unsigned char	*pbase, *pdest;
	fixed16_t	s, t, snext, tnext, sstep = 0, tstep = 0;
	float		sdivz, tdivz, zi, z, du, dv, spancountminus1;

	pbase = (unsigned char *)cacheblock;

	byte *viewbuffer = (byte *)d_viewbuffer;
	int _screenwidth = screenwidth;
	float _d_sdivzorigin = d_sdivzorigin;
	float _d_sdivzstepv = d_sdivzstepv;
	float _d_sdivzstepu = d_sdivzstepu;
	float _d_tdivzorigin = d_tdivzorigin;
	float _d_tdivzstepv = d_tdivzstepv;
	float _d_tdivzstepu = d_tdivzstepu;
	float _d_ziorigin = d_ziorigin;
	float _d_zistepv = d_zistepv;
	float _d_zistepu = d_zistepu;
	fixed16_t _sadjust = sadjust;
	fixed16_t _tadjust = tadjust;
	fixed16_t _bbextents = bbextents;
	fixed16_t _bbextentt = bbextentt;
	int _cachewidth = cachewidth;

	float sdivzstepu = _d_sdivzstepu * 16;
	float tdivzstepu = _d_tdivzstepu * 16;
	float zistepu = _d_zistepu * 16;

	do
	{
		pnext = pspan->pnext;
		SPAN_TOUCH (pnext, touched);
		pdest = (unsigned char *)&viewbuffer[(_screenwidth * pspan->v) + pspan->u];
		count = pspan->count >> 4;
		spancount = pspan->count % 16;

		// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = _d_sdivzorigin + dv*_d_sdivzstepv + du*_d_sdivzstepu;
		tdivz = _d_tdivzorigin + dv*_d_tdivzstepv + du*_d_tdivzstepu;
		zi = _d_ziorigin + dv*_d_zistepv + du*_d_zistepu;
		z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

		// prevent round-off error on <0 steps from causing overstepping
		// and running off the edge of the texture.
		s = bound(0, (int) (sdivz * z) + _sadjust, _bbextents);
		t = bound(0, (int) (tdivz * z) + _tadjust, _bbextentt);

		while (count-- > 0)
		{
			// calculate s/z, t/z, zi->fixed s and t at far end of span,
			// calculate s and t steps across span by shifting
			sdivz += sdivzstepu;
			tdivz += tdivzstepu;
			zi += zistepu;
			z = (float)0x10000 / zi;   // prescale to 16.16 fixed-point

			snext = bound(16, (int) (sdivz * z) + _sadjust, _bbextents);
			tnext = bound(16, (int) (tdivz * z) + _tadjust, _bbextentt);

			sstep = (snext - s) >> 4;
			tstep = (tnext - t) >> 4;
			pdest += 16;

#define WRITEPDEST(i) \
	{ pdest[i] = *(pbase + (s >> 16) + (t >> 16) * _cachewidth); s += sstep; t += tstep; }

			WRITEPDEST(-16);
			WRITEPDEST(-15);
			WRITEPDEST(-14);
			WRITEPDEST(-13);
			WRITEPDEST(-12);
			WRITEPDEST(-11);
			WRITEPDEST(-10);
			WRITEPDEST(-9);
			WRITEPDEST(-8);
			WRITEPDEST(-7);
			WRITEPDEST(-6);
			WRITEPDEST(-5);
			WRITEPDEST(-4);
			WRITEPDEST(-3);
			WRITEPDEST(-2);
			WRITEPDEST(-1);

			s = snext;
			t = tnext;
		}

		// calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
		// can't step off polygon), clamp, calculate s and t steps across
		// span by division, biasing steps low so we don't run off the
		// texture
		if (spancount > 0)
		{
			spancountminus1 = (float)(spancount - 1);
			sdivz += d_sdivzstepu * spancountminus1;
			tdivz += d_tdivzstepu * spancountminus1;
			zi += d_zistepu * spancountminus1;
			z = (float)0x10000 / zi;   // prescale to 16.16 fixed-point

			snext = bound(16, (int)(sdivz * z) + sadjust, bbextents);
			tnext = bound(16, (int)(tdivz * z) + tadjust, bbextentt);

			if (spancount > 1)
			{
				sstep = (snext - s) / (spancount - 1);
				tstep = (tnext - t) / (spancount - 1);
			}

			pdest += spancount;

			switch (spancount)
			{
			case 16:
				WRITEPDEST(-16);
			case 15:
				WRITEPDEST(-15);
			case 14:
				WRITEPDEST(-14);
			case 13:
				WRITEPDEST(-13);
			case 12:
				WRITEPDEST(-12);
			case 11:
				WRITEPDEST(-11);
			case 10:
				WRITEPDEST(-10);
			case  9:
				WRITEPDEST(-9);
			case  8:
				WRITEPDEST(-8);
			case  7:
				WRITEPDEST(-7);
			case  6:
				WRITEPDEST(-6);
			case  5:
				WRITEPDEST(-5);
			case  4:
				WRITEPDEST(-4);
			case  3:
				WRITEPDEST(-3);
			case  2:
				WRITEPDEST(-2);
			case  1:
				WRITEPDEST(-1);
				break;
			}
		}
		SPAN_TOUCH_DONE (touched);
	}
	while ((pspan = pnext));
}

/*
=============
D_DrawZSpans
=============
*/
void D_DrawZSpans (espan_t *pspan)
{
	int				count, doublecount, izistep;
	int				izi;
	short			*pdest;
	unsigned		ltemp;
	float			zi;
	float			du, dv;

	PROF_BEGINF(P_ZSPAN);
// FIXME: check for clamping/range problems
// we count on FP exceptions being turned off to avoid range problems
	izistep = (int)(d_zistepu * 0x8000 * 0x10000);

	short *_d_pzbuffer = d_pzbuffer;
	unsigned int _d_zwidth= d_zwidth;
	float _d_ziorigin = d_ziorigin;
	float _d_zistepu = d_zistepu;
	float _d_zistepv = d_zistepv;

	do
	{
		pdest = _d_pzbuffer + (_d_zwidth * pspan->v) + pspan->u;

		count = pspan->count;

	// calculate the initial 1/z
		du = (float)pspan->u;
		dv = (float)pspan->v;

		zi = _d_ziorigin + dv*_d_zistepv + du*_d_zistepu;
	// we count on FP exceptions being turned off to avoid range problems
		izi = (int)(zi * 0x8000 * 0x10000);

		if ((uintptr_t)pdest & 0x02)
		{
			*pdest++ = (short)(izi >> 16);
			izi += izistep;
			count--;
		}

		if ((doublecount = count >> 1) > 0)
		{
			do
			{
				ltemp = izi >> 16;
				izi += izistep;
				ltemp |= izi & 0xFFFF0000;
				izi += izistep;
				*(int *)pdest = ltemp;
				pdest += 2;
			} while (--doublecount > 0);
		}

		if (count & 1)
			*pdest = (short)(izi >> 16);

	} while ((pspan = pspan->pnext) != NULL);
	PROF_ENDF(P_ZSPAN);
}
