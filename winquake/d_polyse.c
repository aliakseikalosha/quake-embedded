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
// d_polyset.c: routines for drawing sets of polygons sharing the same
// texture (used for Alias models)

#include "quakedef.h"
#include "pdprof.h"
#include "r_local.h"
#include "d_local.h"

// TODO: put in span spilling to shrink list size
// !!! if this is changed, it must be changed in d_polysa.s too !!!
#define DPS_MAXSPANS			MAXHEIGHT+1	
									// 1 extra for spanpackage that marks end

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct {
	void			*pdest;
	short			*pz;
	int				count;
	byte			*ptex;
	int				sfrac, tfrac, light, zi;
} spanpackage_t;

typedef struct {
	int		isflattop;
	int		numleftedges;
	int		*pleftedgevert0;
	int		*pleftedgevert1;
	int		*pleftedgevert2;
	int		numrightedges;
	int		*prightedgevert0;
	int		*prightedgevert1;
	int		*prightedgevert2;
} edgetable;

int	r_p0[6], r_p1[6], r_p2[6];

byte		*d_pcolormap;

int			d_aflatcolor;
int			d_xdenom;

edgetable	*pedgetable;

edgetable	edgetables[12] = {
	{0, 1, r_p0, r_p2, NULL, 2, r_p0, r_p1, r_p2 },
	{0, 2, r_p1, r_p0, r_p2,   1, r_p1, r_p2, NULL},
	{1, 1, r_p0, r_p2, NULL, 1, r_p1, r_p2, NULL},
	{0, 1, r_p1, r_p0, NULL, 2, r_p1, r_p2, r_p0 },
	{0, 2, r_p0, r_p2, r_p1,   1, r_p0, r_p1, NULL},
	{0, 1, r_p2, r_p1, NULL, 1, r_p2, r_p0, NULL},
	{0, 1, r_p2, r_p1, NULL, 2, r_p2, r_p0, r_p1 },
	{0, 2, r_p2, r_p1, r_p0,   1, r_p2, r_p0, NULL},
	{0, 1, r_p1, r_p0, NULL, 1, r_p1, r_p2, NULL},
	{1, 1, r_p2, r_p1, NULL, 1, r_p0, r_p1, NULL},
	{1, 1, r_p1, r_p0, NULL, 1, r_p2, r_p0, NULL},
	{0, 1, r_p0, r_p2, NULL, 1, r_p0, r_p1, NULL},
};

// FIXME: some of these can become statics
int				a_sstepxfrac, a_tstepxfrac, r_lstepx, a_ststepxwhole;
int				r_sstepx, r_tstepx, r_lstepy, r_sstepy, r_tstepy;
int				r_zistepx, r_zistepy;
int				d_aspancount, d_countextrastep;

spanpackage_t			*a_spans;
spanpackage_t			*d_pedgespanpackage;
static int				ystart;
byte					*d_pdest, *d_ptex;
short					*d_pz;
int						d_sfrac, d_tfrac, d_light, d_zi;
int						d_ptexextrastep, d_sfracextrastep;
int						d_tfracextrastep, d_lightextrastep, d_pdestextrastep;
int						d_lightbasestep, d_pdestbasestep, d_ptexbasestep;
int						d_sfracbasestep, d_tfracbasestep;
int						d_ziextrastep, d_zibasestep;
int						d_pzextrastep, d_pzbasestep;

byte	*skintable[MAX_LBM_HEIGHT];
int		skinwidth;
byte	*skinstart;

void D_PolysetDrawSpans8 (spanpackage_t *pspanpackage);
void D_PolysetCalcGradients (int skinwidth);
void D_DrawSubdiv (void);
void D_DrawNonSubdiv (void);
void D_PolysetRecursiveTriangle (int *p1, int *p2, int *p3);
void D_PolysetSetEdgeTable (void);
void D_RasterizeAliasPolySmooth (void);
void D_PolysetScanLeftEdge (int height);

/*
================
D_PolysetDraw
================
*/
void D_PolysetDraw (void)
{
#ifndef PD_FAST_ALIAS
	static spanpackage_t	spans[DPS_MAXSPANS + 1 +
			((CACHE_SIZE - 1) / sizeof(spanpackage_t)) + 1];
						// one extra because of cache line pretouching

	a_spans = (spanpackage_t *)
			(((uintptr_t)&spans[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));
#endif

	if (r_affinetridesc.drawtype)
	{
		D_DrawSubdiv ();
	}
	else
	{
		D_DrawNonSubdiv ();
	}
}


/*
================
D_PolysetDrawFinalVerts
================
*/
void D_PolysetDrawFinalVerts (finalvert_t *fv, int numverts)
{
	int		i, z;
	short	*zbuf;

	for (i=0 ; i<numverts ; i++, fv++)
	{
	// valid triangle coordinates for filling can include the bottom and
	// right clip edges, due to the fill rule; these shouldn't be drawn
		if ((fv->v[0] < r_refdef.vrectright) &&
			(fv->v[1] < r_refdef.vrectbottom))
		{
			z = fv->v[5]>>16;
			zbuf = zspantable[fv->v[1]] + fv->v[0];
			if (z >= *zbuf)
			{
				int		pix;
				
				*zbuf = z;
				pix = skintable[fv->v[3]>>16][fv->v[2]>>16];
				pix = ((byte *)acolormap)[pix + (fv->v[4] & 0xFF00) ];
				d_viewbuffer[d_scantable[fv->v[1]] + fv->v[0]] = pix;
			}
		}
	}
}


/*
================
D_DrawSubdiv
================
*/
void D_DrawSubdiv (void)
{
	mtriangle_t		*ptri;
	finalvert_t		*pfv, *index0, *index1, *index2;
	int				i;
	int				lnumtriangles;

	pfv = r_affinetridesc.pfinalverts;
	ptri = r_affinetridesc.ptriangles;
	lnumtriangles = r_affinetridesc.numtriangles;

	for (i=0 ; i<lnumtriangles ; i++)
	{
		index0 = pfv + ptri[i].vertindex[0];
		index1 = pfv + ptri[i].vertindex[1];
		index2 = pfv + ptri[i].vertindex[2];

		if (((index0->v[1]-index1->v[1]) *
			 (index0->v[0]-index2->v[0]) -
			 (index0->v[0]-index1->v[0]) * 
			 (index0->v[1]-index2->v[1])) >= 0)
		{
			continue;
		}

		d_pcolormap = &((byte *)acolormap)[index0->v[4] & 0xFF00];

		if (ptri[i].facesfront)
		{
			D_PolysetRecursiveTriangle(index0->v, index1->v, index2->v);
		}
		else
		{
			int		s0, s1, s2;

			s0 = index0->v[2];
			s1 = index1->v[2];
			s2 = index2->v[2];

			if (index0->flags & ALIAS_ONSEAM)
				index0->v[2] += r_affinetridesc.seamfixupX16;
			if (index1->flags & ALIAS_ONSEAM)
				index1->v[2] += r_affinetridesc.seamfixupX16;
			if (index2->flags & ALIAS_ONSEAM)
				index2->v[2] += r_affinetridesc.seamfixupX16;

			D_PolysetRecursiveTriangle(index0->v, index1->v, index2->v);

			index0->v[2] = s0;
			index1->v[2] = s1;
			index2->v[2] = s2;
		}
	}
}


#ifndef PD_FAST_ALIAS
/*
================
D_DrawNonSubdiv
================
*/
void D_DrawNonSubdiv (void)
{
	mtriangle_t		*ptri;
	finalvert_t		*pfv, *index0, *index1, *index2;
	int				i;
	int				lnumtriangles;

	pfv = r_affinetridesc.pfinalverts;
	ptri = r_affinetridesc.ptriangles;
	lnumtriangles = r_affinetridesc.numtriangles;

	for (i=0 ; i<lnumtriangles ; i++, ptri++)
	{
		index0 = pfv + ptri->vertindex[0];
		index1 = pfv + ptri->vertindex[1];
		index2 = pfv + ptri->vertindex[2];

		d_xdenom = (index0->v[1]-index1->v[1]) *
				(index0->v[0]-index2->v[0]) -
				(index0->v[0]-index1->v[0])*(index0->v[1]-index2->v[1]);

		if (d_xdenom >= 0)
		{
			continue;
		}

		r_p0[0] = index0->v[0];		// u
		r_p0[1] = index0->v[1];		// v
		r_p0[2] = index0->v[2];		// s
		r_p0[3] = index0->v[3];		// t
		r_p0[4] = index0->v[4];		// light
		r_p0[5] = index0->v[5];		// iz

		r_p1[0] = index1->v[0];
		r_p1[1] = index1->v[1];
		r_p1[2] = index1->v[2];
		r_p1[3] = index1->v[3];
		r_p1[4] = index1->v[4];
		r_p1[5] = index1->v[5];

		r_p2[0] = index2->v[0];
		r_p2[1] = index2->v[1];
		r_p2[2] = index2->v[2];
		r_p2[3] = index2->v[3];
		r_p2[4] = index2->v[4];
		r_p2[5] = index2->v[5];

		if (!ptri->facesfront)
		{
			if (index0->flags & ALIAS_ONSEAM)
				r_p0[2] += r_affinetridesc.seamfixupX16;
			if (index1->flags & ALIAS_ONSEAM)
				r_p1[2] += r_affinetridesc.seamfixupX16;
			if (index2->flags & ALIAS_ONSEAM)
				r_p2[2] += r_affinetridesc.seamfixupX16;
		}

		D_PolysetSetEdgeTable ();
		D_RasterizeAliasPolySmooth ();
	}
}
#endif	// !PD_FAST_ALIAS


/*
================
D_PolysetRecursiveTriangle
================
*/
void D_PolysetRecursiveTriangle (int *lp1, int *lp2, int *lp3)
{
	int		*temp;
	int		d;
	int		new[6];
	int		z;
	short	*zbuf;

	d = lp2[0] - lp1[0];
	if (d < -1 || d > 1)
		goto split;
	d = lp2[1] - lp1[1];
	if (d < -1 || d > 1)
		goto split;

	d = lp3[0] - lp2[0];
	if (d < -1 || d > 1)
		goto split2;
	d = lp3[1] - lp2[1];
	if (d < -1 || d > 1)
		goto split2;

	d = lp1[0] - lp3[0];
	if (d < -1 || d > 1)
		goto split3;
	d = lp1[1] - lp3[1];
	if (d < -1 || d > 1)
	{
split3:
		temp = lp1;
		lp1 = lp3;
		lp3 = lp2;
		lp2 = temp;

		goto split;
	}

	return;			// entire tri is filled

split2:
	temp = lp1;
	lp1 = lp2;
	lp2 = lp3;
	lp3 = temp;

split:
// split this edge
	new[0] = (lp1[0] + lp2[0]) >> 1;
	new[1] = (lp1[1] + lp2[1]) >> 1;
	new[2] = (lp1[2] + lp2[2]) >> 1;
	new[3] = (lp1[3] + lp2[3]) >> 1;
	new[5] = (lp1[5] + lp2[5]) >> 1;

// draw the point if splitting a leading edge
	if (lp2[1] > lp1[1])
		goto nodraw;
	if ((lp2[1] == lp1[1]) && (lp2[0] < lp1[0]))
		goto nodraw;


	z = new[5]>>16;
	zbuf = zspantable[new[1]] + new[0];
	if (z >= *zbuf)
	{
		int		pix;
		
		*zbuf = z;
		pix = d_pcolormap[skintable[new[3]>>16][new[2]>>16]];
		d_viewbuffer[d_scantable[new[1]] + new[0]] = pix;
	}

nodraw:
// recursively continue
	D_PolysetRecursiveTriangle (lp3, lp1, new);
	D_PolysetRecursiveTriangle (lp3, new, lp2);
}

/*
================
D_PolysetUpdateTables
================
*/
void D_PolysetUpdateTables (void)
{
	int		i;
	byte	*s;
	
	if (r_affinetridesc.skinwidth != skinwidth ||
		r_affinetridesc.pskin != skinstart)
	{
		skinwidth = r_affinetridesc.skinwidth;
		skinstart = r_affinetridesc.pskin;
		s = skinstart;
		for (i=0 ; i<MAX_LBM_HEIGHT ; i++, s+=skinwidth)
			skintable[i] = s;
	}
}

/*
===================
D_PolysetScanLeftEdge
====================
*/
void D_PolysetScanLeftEdge (int height)
{

	do
	{
		d_pedgespanpackage->pdest = d_pdest;
		d_pedgespanpackage->pz = d_pz;
		d_pedgespanpackage->count = d_aspancount;
		d_pedgespanpackage->ptex = d_ptex;

		d_pedgespanpackage->sfrac = d_sfrac;
		d_pedgespanpackage->tfrac = d_tfrac;

	// FIXME: need to clamp l, s, t, at both ends?
		d_pedgespanpackage->light = d_light;
		d_pedgespanpackage->zi = d_zi;

		d_pedgespanpackage++;

		errorterm += erroradjustup;
		if (errorterm >= 0)
		{
			d_pdest += d_pdestextrastep;
			d_pz += d_pzextrastep;
			d_aspancount += d_countextrastep;
			d_ptex += d_ptexextrastep;
			d_sfrac += d_sfracextrastep;
			d_ptex += d_sfrac >> 16;

			d_sfrac &= 0xFFFF;
			d_tfrac += d_tfracextrastep;
			if (d_tfrac & 0x10000)
			{
				d_ptex += r_affinetridesc.skinwidth;
				d_tfrac &= 0xFFFF;
			}
			d_light += d_lightextrastep;
			d_zi += d_ziextrastep;
			errorterm -= erroradjustdown;
		}
		else
		{
			d_pdest += d_pdestbasestep;
			d_pz += d_pzbasestep;
			d_aspancount += ubasestep;
			d_ptex += d_ptexbasestep;
			d_sfrac += d_sfracbasestep;
			d_ptex += d_sfrac >> 16;
			d_sfrac &= 0xFFFF;
			d_tfrac += d_tfracbasestep;
			if (d_tfrac & 0x10000)
			{
				d_ptex += r_affinetridesc.skinwidth;
				d_tfrac &= 0xFFFF;
			}
			d_light += d_lightbasestep;
			d_zi += d_zibasestep;
		}
	} while (--height);
}

/*
===================
FloorDivMod

Returns mathematically correct (floor-based) quotient and remainder for
numer and denom, both of which should contain no fractional part. The
quotient must fit in 32 bits.
====================
*/
static inline void
FloorDivMod (int numer, int denom, int *quotient, int *rem)
{
        int n = (numer >= 0) ? numer : -numer;
        int q = n / denom;
        int r = n - q * denom;

        // For the negative case, fix mod to make floor-based
        if (numer < 0)
        {
                q = -q;
                if (r != 0)
                {
                        q--;
                        r = denom - r;
                }
        }

        *quotient = q;
        *rem = r;
}

/*
===================
D_PolysetSetUpForLineScan
====================
*/
static void
D_PolysetSetUpForLineScan(fixed8_t startvertu, fixed8_t startvertv,
		fixed8_t endvertu, fixed8_t endvertv)
{
	fixed8_t	tm, tn;

	errorterm = -1;

	tm = endvertu - startvertu;
	tn = endvertv - startvertv;

	FloorDivMod (tm, tn, &ubasestep, &erroradjustup);

	erroradjustdown = tn;
}

/*
================
D_PolysetCalcGradients
================
*/
void D_PolysetCalcGradients (int skinwidth)
{
	float	xstepdenominv, ystepdenominv, t0, t1;
	float	p01_minus_p21, p11_minus_p21, p00_minus_p20, p10_minus_p20;

	p00_minus_p20 = r_p0[0] - r_p2[0];
	p01_minus_p21 = r_p0[1] - r_p2[1];
	p10_minus_p20 = r_p1[0] - r_p2[0];
	p11_minus_p21 = r_p1[1] - r_p2[1];

	xstepdenominv = 1.0f / (float)d_xdenom;

	ystepdenominv = -xstepdenominv;

// ceilf () for light so positive steps are exaggerated, negative steps
// diminished,  pushing us away from underflow toward overflow. Underflow is
// very visible, overflow is very unlikely, because of ambient lighting
	t0 = r_p0[4] - r_p2[4];
	t1 = r_p1[4] - r_p2[4];
	r_lstepx = (int)
			ceilf((t1 * p01_minus_p21 - t0 * p11_minus_p21) * xstepdenominv);
	r_lstepy = (int)
			ceilf((t1 * p00_minus_p20 - t0 * p10_minus_p20) * ystepdenominv);

	t0 = r_p0[2] - r_p2[2];
	t1 = r_p1[2] - r_p2[2];
	r_sstepx = (int)((t1 * p01_minus_p21 - t0 * p11_minus_p21) *
			xstepdenominv);
	r_sstepy = (int)((t1 * p00_minus_p20 - t0* p10_minus_p20) *
			ystepdenominv);

	t0 = r_p0[3] - r_p2[3];
	t1 = r_p1[3] - r_p2[3];
	r_tstepx = (int)((t1 * p01_minus_p21 - t0 * p11_minus_p21) *
			xstepdenominv);
	r_tstepy = (int)((t1 * p00_minus_p20 - t0 * p10_minus_p20) *
			ystepdenominv);

	t0 = r_p0[5] - r_p2[5];
	t1 = r_p1[5] - r_p2[5];
	r_zistepx = (int)((t1 * p01_minus_p21 - t0 * p11_minus_p21) *
			xstepdenominv);
	r_zistepy = (int)((t1 * p00_minus_p20 - t0 * p10_minus_p20) *
			ystepdenominv);

	a_sstepxfrac = r_sstepx & 0xFFFF;
	a_tstepxfrac = r_tstepx & 0xFFFF;

	a_ststepxwhole = skinwidth * (r_tstepx >> 16) + (r_sstepx >> 16);
}

#if 0
byte gelmap[256];
void InitGel (byte *palette)
{
	int		i;
	int		r;

	for (i=0 ; i<256 ; i++)
	{
//		r = (palette[i*3]>>4);
		r = (palette[i*3] + palette[i*3+1] + palette[i*3+2])/(16*3);
		gelmap[i] = /* 64 */ 0 + r;
	}
}
#endif

#ifdef PD_FAST_ALIAS
/*
==============================================================================

Fused alias triangle rasterizer

The original rasterizer scans the left edge of a triangle into an array of
span packages (32 bytes per row, in a static buffer), then walks the right edge
and draws each span; on top of that every triangle copies its vertices into
r_p0..r_p2 and keeps its gradients and edge steppers in ~50 globals. On the
Playdate every byte stored to a global costs ~26 ns, so that bookkeeping costs
more than most of the pixels. This version steps the left and right edges
together, row by row, with all state in locals (registers and the fast stack),
and draws each span straight away. The arithmetic is exactly that of
D_PolysetSetEdgeTable / D_PolysetCalcGradients / D_RasterizeAliasPolySmooth /
D_PolysetDrawSpans8, so the pixels are identical.
==============================================================================
*/

// which vertex (0..2 = p0..p2) each edge of the table runs from and to
typedef struct
{
	signed char	numleft, left[3];
	signed char	numright, right[3];
} fedgetable_t;

static const fedgetable_t fedgetables[12] = {
	{1, {0, 2, -1}, 2, {0, 1, 2}},
	{2, {1, 0, 2}, 1, {1, 2, -1}},
	{1, {0, 2, -1}, 1, {1, 2, -1}},
	{1, {1, 0, -1}, 2, {1, 2, 0}},
	{2, {0, 2, 1}, 1, {0, 1, -1}},
	{1, {2, 1, -1}, 1, {2, 0, -1}},
	{1, {2, 1, -1}, 2, {2, 0, 1}},
	{2, {2, 1, 0}, 1, {2, 0, -1}},
	{1, {1, 0, -1}, 1, {1, 2, -1}},
	{1, {2, 1, -1}, 1, {0, 1, -1}},
	{1, {1, 0, -1}, 1, {2, 0, -1}},
	{1, {0, 2, -1}, 1, {0, 1, -1}},
};

typedef struct
{
	byte	*pdest, *ptex;
	short	*pz;
	int		sfrac, tfrac, light, zi, aspancount;
	int		errorterm, erroradjustup, erroradjustdown, ubasestep, countextrastep;
	int		pdestbasestep, pdestextrastep, pzbasestep, pzextrastep;
	int		ptexbasestep, ptexextrastep, sfracbasestep, sfracextrastep;
	int		tfracbasestep, tfracextrastep, lightbasestep, lightextrastep;
	int		zibasestep, ziextrastep;
} fleft_t;

typedef struct
{
	int		aspancount, errorterm, erroradjustup, erroradjustdown, ubasestep, countextrastep;
} fright_t;

typedef struct
{
	int		lstepx, lstepy, sstepx, sstepy, tstepx, tstepy, zistepx, zistepy;
	int		skinwidth;
	byte	*pskin;
} fgrad_t;

// start a left edge segment at vertex "top" and step it towards "bot" over "height" rows;
// "first" is the top segment of the edge, whose fractions carry over from the vertex
static inline void
D_FastLeftSegment (fleft_t *l, const fgrad_t *g, int *top, int topS, int *bot,
		int height, int rightTopU, qboolean first)
{
	int		working_lstepx, tm, tn;

	l->aspancount = top[0] - rightTopU;
	l->ptex = g->pskin + (topS >> 16) + (top[3] >> 16) * g->skinwidth;
	l->sfrac = first ? (topS & 0xFFFF) : 0;
	l->tfrac = first ? (top[3] & 0xFFFF) : 0;
	l->light = top[4];
	l->zi = top[5];
	l->pdest = (byte *)d_viewbuffer + top[1] * screenwidth + top[0];
	l->pz = d_pzbuffer + top[1] * d_zwidth + top[0];

	if (height == 1)
		return;

	tm = bot[0] - top[0];
	tn = bot[1] - top[1];
	FloorDivMod (tm, tn, &l->ubasestep, &l->erroradjustup);
	l->erroradjustdown = tn;
	l->errorterm = -1;

	l->pzbasestep = d_zwidth + l->ubasestep;
	l->pzextrastep = l->pzbasestep + 1;
	l->pdestbasestep = screenwidth + l->ubasestep;
	l->pdestextrastep = l->pdestbasestep + 1;

// for negative steps in x along left edge, bias toward overflow rather than
// underflow (sort of turning the floor () we did in the gradient calcs into
// ceil (), but plus a little bit)
	if (l->ubasestep < 0)
		working_lstepx = g->lstepx - 1;
	else
		working_lstepx = g->lstepx;

	l->countextrastep = l->ubasestep + 1;
	l->ptexbasestep = ((g->sstepy + g->sstepx * l->ubasestep) >> 16) +
			((g->tstepy + g->tstepx * l->ubasestep) >> 16) * g->skinwidth;
	l->sfracbasestep = (g->sstepy + g->sstepx * l->ubasestep) & 0xFFFF;
	l->tfracbasestep = (g->tstepy + g->tstepx * l->ubasestep) & 0xFFFF;
	l->lightbasestep = g->lstepy + working_lstepx * l->ubasestep;
	l->zibasestep = g->zistepy + g->zistepx * l->ubasestep;

	l->ptexextrastep = ((g->sstepy + g->sstepx * l->countextrastep) >> 16) +
			((g->tstepy + g->tstepx * l->countextrastep) >> 16) * g->skinwidth;
	l->sfracextrastep = (g->sstepy + g->sstepx * l->countextrastep) & 0xFFFF;
	l->tfracextrastep = (g->tstepy + g->tstepx * l->countextrastep) & 0xFFFF;
	l->lightextrastep = l->lightbasestep + working_lstepx;
	l->ziextrastep = l->zibasestep + g->zistepx;
}

static inline void
D_FastRightSegment (fright_t *r, int *top, int *bot, int startcount)
{
	int		tm = bot[0] - top[0];
	int		tn = bot[1] - top[1];

	r->errorterm = -1;
	FloorDivMod (tm, tn, &r->ubasestep, &r->erroradjustup);
	r->erroradjustdown = tn;
	r->aspancount = startcount;
	r->countextrastep = r->ubasestep + 1;
}

// pv[i] = u, v, s, t, l, 1/z of vertex i (s is passed separately: it may carry the seam fix-up)
static void
D_FastRasterizeTriangle (int *pv[3], const int sv[3], int xdenom)
{
	fgrad_t		g;
	fleft_t		l;
	fright_t	r;
	const fedgetable_t	*et;
	float		xstepdenominv, ystepdenominv, t0, t1;
	float		p01_minus_p21, p11_minus_p21, p00_minus_p20, p10_minus_p20;
	int			*ltop, *lbot, *rtop, *rbot;
	int			lrem, rrem, lseg, rseg;
	int			i, k;
	byte		*cmap = (byte *)acolormap;
	int			_r_zistepx, _r_lstepx, _a_ststepxwhole, _a_sstepxfrac, _a_tstepxfrac, _skinwidth;
	int			edgetableindex;
#ifdef PD_PROFILE_FINE
	int			npix = 0;
#endif
	PROF_STK(K_POLY);

// pick the edge table (see D_PolysetSetEdgeTable)
	edgetableindex = 0;
	if (pv[0][1] >= pv[1][1])
	{
		if (pv[0][1] == pv[1][1])
		{
			et = &fedgetables[(pv[0][1] < pv[2][1]) ? 2 : 5];
			goto have_table;
		}
		edgetableindex = 1;
	}
	if (pv[0][1] == pv[2][1])
	{
		et = &fedgetables[edgetableindex ? 8 : 9];
		goto have_table;
	}
	else if (pv[1][1] == pv[2][1])
	{
		et = &fedgetables[edgetableindex ? 10 : 11];
		goto have_table;
	}
	if (pv[0][1] > pv[2][1])
		edgetableindex += 2;
	if (pv[1][1] > pv[2][1])
		edgetableindex += 4;
	et = &fedgetables[edgetableindex];
have_table:

// gradients (see D_PolysetCalcGradients)
	g.skinwidth = r_affinetridesc.skinwidth;
	g.pskin = (byte *)r_affinetridesc.pskin;

	p00_minus_p20 = pv[0][0] - pv[2][0];
	p01_minus_p21 = pv[0][1] - pv[2][1];
	p10_minus_p20 = pv[1][0] - pv[2][0];
	p11_minus_p21 = pv[1][1] - pv[2][1];

	xstepdenominv = 1.0f / (float)xdenom;
	ystepdenominv = -xstepdenominv;

	t0 = pv[0][4] - pv[2][4];
	t1 = pv[1][4] - pv[2][4];
	g.lstepx = (int)
			ceilf((t1 * p01_minus_p21 - t0 * p11_minus_p21) * xstepdenominv);
	g.lstepy = (int)
			ceilf((t1 * p00_minus_p20 - t0 * p10_minus_p20) * ystepdenominv);

	t0 = sv[0] - sv[2];
	t1 = sv[1] - sv[2];
	g.sstepx = (int)((t1 * p01_minus_p21 - t0 * p11_minus_p21) * xstepdenominv);
	g.sstepy = (int)((t1 * p00_minus_p20 - t0 * p10_minus_p20) * ystepdenominv);

	t0 = pv[0][3] - pv[2][3];
	t1 = pv[1][3] - pv[2][3];
	g.tstepx = (int)((t1 * p01_minus_p21 - t0 * p11_minus_p21) * xstepdenominv);
	g.tstepy = (int)((t1 * p00_minus_p20 - t0 * p10_minus_p20) * ystepdenominv);

	t0 = pv[0][5] - pv[2][5];
	t1 = pv[1][5] - pv[2][5];
	g.zistepx = (int)((t1 * p01_minus_p21 - t0 * p11_minus_p21) * xstepdenominv);
	g.zistepy = (int)((t1 * p00_minus_p20 - t0 * p10_minus_p20) * ystepdenominv);

	_r_zistepx = g.zistepx;
	_r_lstepx = g.lstepx;
	_a_sstepxfrac = g.sstepx & 0xFFFF;
	_a_tstepxfrac = g.tstepx & 0xFFFF;
	_skinwidth = g.skinwidth;
	_a_ststepxwhole = _skinwidth * (g.tstepx >> 16) + (g.sstepx >> 16);

// left edge, top segment; the right edge starts at its own top vertex
	ltop = pv[et->left[0]];
	lbot = pv[et->left[1]];
	rtop = pv[et->right[0]];
	rbot = pv[et->right[1]];

	lrem = lbot[1] - ltop[1];
	rrem = rbot[1] - rtop[1];
	lseg = rseg = 1;

	D_FastLeftSegment (&l, &g, ltop, sv[et->left[0]], lbot, lrem, rtop[0], true);
	D_FastRightSegment (&r, rtop, rbot, 0);

	for (;;)
	{
		int		lcount;

		lcount = r.aspancount - l.aspancount;

		r.errorterm += r.erroradjustup;
		if (r.errorterm >= 0)
		{
			r.aspancount += r.countextrastep;
			r.errorterm -= r.erroradjustdown;
		}
		else
		{
			r.aspancount += r.ubasestep;
		}

		if (lcount)
		{
			byte	*lpdest = l.pdest;
			byte	*lptex = l.ptex;
			short	*lpz = l.pz;
			int		lsfrac = l.sfrac;
			int		ltfrac = l.tfrac;
			int		llight = l.light;
			int		lzi = l.zi;
			int		cnt = lcount;

#ifdef PD_PROFILE_FINE
			npix += lcount;
#endif

		// pass 1: the pixels (interleaving stores to two buffers costs about twice as
		// much on this memory as writing each buffer in its own run)
			do
			{
				if ((lzi >> 16) >= *lpz)
					*lpdest = cmap[*lptex + (llight & 0xFF00)];
				lpdest++;
				lzi += _r_zistepx;
				lpz++;
				llight += _r_lstepx;
				lptex += _a_ststepxwhole;
				lsfrac += _a_sstepxfrac;
				lptex += lsfrac >> 16;
				lsfrac &= 0xFFFF;
				ltfrac += _a_tstepxfrac;
				if (ltfrac & 0x10000)
				{
					lptex += _skinwidth;
					ltfrac &= 0xFFFF;
				}
			} while (--cnt);

		// pass 2: the z values of the pixels that passed (the z buffer is unchanged since pass 1)
			lpz = l.pz;
			lzi = l.zi;
			cnt = lcount;
			do
			{
				if ((lzi >> 16) >= *lpz)
					*lpz = lzi >> 16;
				lzi += _r_zistepx;
				lpz++;
			} while (--cnt);
		}

	// step the left edge to the next row
		if (lrem > 1)
		{
			l.errorterm += l.erroradjustup;
			if (l.errorterm >= 0)
			{
				l.pdest += l.pdestextrastep;
				l.pz += l.pzextrastep;
				l.aspancount += l.countextrastep;
				l.ptex += l.ptexextrastep;
				l.sfrac += l.sfracextrastep;
				l.ptex += l.sfrac >> 16;
				l.sfrac &= 0xFFFF;
				l.tfrac += l.tfracextrastep;
				if (l.tfrac & 0x10000)
				{
					l.ptex += _skinwidth;
					l.tfrac &= 0xFFFF;
				}
				l.light += l.lightextrastep;
				l.zi += l.ziextrastep;
				l.errorterm -= l.erroradjustdown;
			}
			else
			{
				l.pdest += l.pdestbasestep;
				l.pz += l.pzbasestep;
				l.aspancount += l.ubasestep;
				l.ptex += l.ptexbasestep;
				l.sfrac += l.sfracbasestep;
				l.ptex += l.sfrac >> 16;
				l.sfrac &= 0xFFFF;
				l.tfrac += l.tfracbasestep;
				if (l.tfrac & 0x10000)
				{
					l.ptex += _skinwidth;
					l.tfrac &= 0xFFFF;
				}
				l.light += l.lightbasestep;
				l.zi += l.zibasestep;
			}
		}

		lrem--;
		rrem--;

		if (rrem == 0)
		{
			if (rseg == et->numright)
			{
				if (lrem == 0 && lseg == et->numleft)
					break;
			}
			else
			{
			// bottom segment of the right edge; its x is relative to the right top vertex
				int		startcount = rbot[0] - rtop[0];

				rseg = 2;
				rtop = rbot;
				rbot = pv[et->right[2]];
				rrem = rbot[1] - rtop[1];
				D_FastRightSegment (&r, rtop, rbot, startcount);
			}
		}

		if (lrem == 0)
		{
			if (lseg == et->numleft)
				break;

			lseg = 2;
			ltop = lbot;
			lbot = pv[et->left[2]];
			lrem = lbot[1] - ltop[1];
			D_FastLeftSegment (&l, &g, ltop, sv[et->left[1]], lbot, lrem,
					pv[et->right[0]][0], false);
		}
	}
	PROF_CNTF(C_APIX, npix);
}

/*
================
D_DrawNonSubdiv
================
*/
void D_DrawNonSubdiv (void)
{
	mtriangle_t		*ptri;
	finalvert_t		*pfv, *index0, *index1, *index2;
	int				i;
	int				lnumtriangles;
	int				*pv[3];
	int				sv[3];
	int				xdenom;

	pfv = r_affinetridesc.pfinalverts;
	ptri = r_affinetridesc.ptriangles;
	lnumtriangles = r_affinetridesc.numtriangles;

	for (i=0 ; i<lnumtriangles ; i++, ptri++)
	{
		index0 = pfv + ptri->vertindex[0];
		index1 = pfv + ptri->vertindex[1];
		index2 = pfv + ptri->vertindex[2];

		xdenom = (index0->v[1]-index1->v[1]) *
				(index0->v[0]-index2->v[0]) -
				(index0->v[0]-index1->v[0])*(index0->v[1]-index2->v[1]);

		if (xdenom >= 0)
			continue;

		pv[0] = index0->v;
		pv[1] = index1->v;
		pv[2] = index2->v;
		sv[0] = index0->v[2];
		sv[1] = index1->v[2];
		sv[2] = index2->v[2];

		if (!ptri->facesfront)
		{
			if (index0->flags & ALIAS_ONSEAM)
				sv[0] += r_affinetridesc.seamfixupX16;
			if (index1->flags & ALIAS_ONSEAM)
				sv[1] += r_affinetridesc.seamfixupX16;
			if (index2->flags & ALIAS_ONSEAM)
				sv[2] += r_affinetridesc.seamfixupX16;
		}

		D_FastRasterizeTriangle (pv, sv, xdenom);
	}
}
#endif	// PD_FAST_ALIAS

/*
================
D_PolysetDrawSpans8
================
*/
void D_PolysetDrawSpans8 (spanpackage_t *pspanpackage)
{
	int		lcount;
	byte	*lpdest;
	byte	*lptex;
	int		lsfrac, ltfrac;
	int		llight;
	int		lzi;
	short	*lpz;

	byte *cmap = (byte *)acolormap;
	int _r_zistepx = r_zistepx;
	int _r_lstepx = r_lstepx;
	int _a_ststepxwhole = a_ststepxwhole;
	int _a_sstepxfrac = a_sstepxfrac;
	int _a_tstepxfrac = a_tstepxfrac;
	int _skinwidth = r_affinetridesc.skinwidth;
	int _d_aspancount = d_aspancount;
	int _errorterm = errorterm;
	int _erroradjustup = erroradjustup;
	int _erroradjustdown = erroradjustdown;
	int _d_countextrastep = d_countextrastep;
	int _ubasestep = ubasestep;

	do
	{
		lcount = _d_aspancount - pspanpackage->count;

		_errorterm += _erroradjustup;
		if (_errorterm >= 0)
		{
			_d_aspancount += _d_countextrastep;
			_errorterm -= _erroradjustdown;
		}
		else
		{
			_d_aspancount += _ubasestep;
		}

		if (lcount)
		{
			lpdest = pspanpackage->pdest;
			lptex = pspanpackage->ptex;
			lpz = pspanpackage->pz;
			lsfrac = pspanpackage->sfrac;
			ltfrac = pspanpackage->tfrac;
			llight = pspanpackage->light;
			lzi = pspanpackage->zi;

			do
			{
				if ((lzi >> 16) >= *lpz)
				{
					*lpdest = cmap[*lptex + (llight & 0xFF00)];
// gel mapping					*lpdest = gelmap[*lpdest];
					*lpz = lzi >> 16;
				}
				lpdest++;
				lzi += _r_zistepx;
				lpz++;
				llight += _r_lstepx;
				lptex += _a_ststepxwhole;
				lsfrac += _a_sstepxfrac;
				lptex += lsfrac >> 16;
				lsfrac &= 0xFFFF;
				ltfrac += _a_tstepxfrac;
				if (ltfrac & 0x10000)
				{
					lptex += _skinwidth;
					ltfrac &= 0xFFFF;
				}
			} while (--lcount);
		}

		pspanpackage++;
	} while (pspanpackage->count != -999999);

	d_aspancount = _d_aspancount;
	errorterm = _errorterm;
}

/*
================
D_PolysetFillSpans8
================
*/
void D_PolysetFillSpans8 (spanpackage_t *pspanpackage)
{
	int				color;

// FIXME: do z buffering

	color = d_aflatcolor++;

	while (1)
	{
		int		lcount;
		byte	*lpdest;

		lcount = pspanpackage->count;

		if (lcount == -1)
			return;

		if (lcount)
		{
			lpdest = pspanpackage->pdest;

			do
			{
				*lpdest++ = color;
			} while (--lcount);
		}

		pspanpackage++;
	}
}

/*
================
D_RasterizeAliasPolySmooth
================
*/
void D_RasterizeAliasPolySmooth (void)
{
	int				initialleftheight, initialrightheight;
	int				*plefttop, *prighttop, *pleftbottom, *prightbottom;
	int				working_lstepx, originalcount;

	plefttop = pedgetable->pleftedgevert0;
	prighttop = pedgetable->prightedgevert0;

	pleftbottom = pedgetable->pleftedgevert1;
	prightbottom = pedgetable->prightedgevert1;

	initialleftheight = pleftbottom[1] - plefttop[1];
	initialrightheight = prightbottom[1] - prighttop[1];

//
// set the s, t, and light gradients, which are consistent across the triangle
// because being a triangle, things are affine
//
	D_PolysetCalcGradients (r_affinetridesc.skinwidth);

//
// rasterize the polygon
//

//
// scan out the top (and possibly only) part of the left edge
//
	d_pedgespanpackage = a_spans;

	ystart = plefttop[1];
	d_aspancount = plefttop[0] - prighttop[0];

	d_ptex = (byte *)r_affinetridesc.pskin + (plefttop[2] >> 16) +
			(plefttop[3] >> 16) * r_affinetridesc.skinwidth;
	d_sfrac = plefttop[2] & 0xFFFF;
	d_tfrac = plefttop[3] & 0xFFFF;
	d_light = plefttop[4];
	d_zi = plefttop[5];

	d_pdest = (byte *)d_viewbuffer +
			ystart * screenwidth + plefttop[0];
	d_pz = d_pzbuffer + ystart * d_zwidth + plefttop[0];

	if (initialleftheight == 1)
	{
		d_pedgespanpackage->pdest = d_pdest;
		d_pedgespanpackage->pz = d_pz;
		d_pedgespanpackage->count = d_aspancount;
		d_pedgespanpackage->ptex = d_ptex;

		d_pedgespanpackage->sfrac = d_sfrac;
		d_pedgespanpackage->tfrac = d_tfrac;

	// FIXME: need to clamp l, s, t, at both ends?
		d_pedgespanpackage->light = d_light;
		d_pedgespanpackage->zi = d_zi;

		d_pedgespanpackage++;
	}
	else
	{
		D_PolysetSetUpForLineScan(plefttop[0], plefttop[1],
							  pleftbottom[0], pleftbottom[1]);

		d_pzbasestep = d_zwidth + ubasestep;
		d_pzextrastep = d_pzbasestep + 1;

		d_pdestbasestep = screenwidth + ubasestep;
		d_pdestextrastep = d_pdestbasestep + 1;

	// TODO: can reuse partial expressions here

	// for negative steps in x along left edge, bias toward overflow rather than
	// underflow (sort of turning the floor () we did in the gradient calcs into
	// ceil (), but plus a little bit)
		if (ubasestep < 0)
			working_lstepx = r_lstepx - 1;
		else
			working_lstepx = r_lstepx;

		d_countextrastep = ubasestep + 1;
		d_ptexbasestep = ((r_sstepy + r_sstepx * ubasestep) >> 16) +
				((r_tstepy + r_tstepx * ubasestep) >> 16) *
				r_affinetridesc.skinwidth;
		d_sfracbasestep = (r_sstepy + r_sstepx * ubasestep) & 0xFFFF;
		d_tfracbasestep = (r_tstepy + r_tstepx * ubasestep) & 0xFFFF;
		d_lightbasestep = r_lstepy + working_lstepx * ubasestep;
		d_zibasestep = r_zistepy + r_zistepx * ubasestep;

		d_ptexextrastep = ((r_sstepy + r_sstepx * d_countextrastep) >> 16) +
				((r_tstepy + r_tstepx * d_countextrastep) >> 16) *
				r_affinetridesc.skinwidth;
		d_sfracextrastep = (r_sstepy + r_sstepx*d_countextrastep) & 0xFFFF;
		d_tfracextrastep = (r_tstepy + r_tstepx*d_countextrastep) & 0xFFFF;
		d_lightextrastep = d_lightbasestep + working_lstepx;
		d_ziextrastep = d_zibasestep + r_zistepx;

		D_PolysetScanLeftEdge (initialleftheight);
	}

//
// scan out the bottom part of the left edge, if it exists
//
	if (pedgetable->numleftedges == 2)
	{
		int		height;

		plefttop = pleftbottom;
		pleftbottom = pedgetable->pleftedgevert2;

		height = pleftbottom[1] - plefttop[1];

// TODO: make this a function; modularize this function in general

		ystart = plefttop[1];
		d_aspancount = plefttop[0] - prighttop[0];
		d_ptex = (byte *)r_affinetridesc.pskin + (plefttop[2] >> 16) +
				(plefttop[3] >> 16) * r_affinetridesc.skinwidth;
		d_sfrac = 0;
		d_tfrac = 0;
		d_light = plefttop[4];
		d_zi = plefttop[5];

		d_pdest = (byte *)d_viewbuffer + ystart * screenwidth + plefttop[0];
		d_pz = d_pzbuffer + ystart * d_zwidth + plefttop[0];

		if (height == 1)
		{
			d_pedgespanpackage->pdest = d_pdest;
			d_pedgespanpackage->pz = d_pz;
			d_pedgespanpackage->count = d_aspancount;
			d_pedgespanpackage->ptex = d_ptex;

			d_pedgespanpackage->sfrac = d_sfrac;
			d_pedgespanpackage->tfrac = d_tfrac;

		// FIXME: need to clamp l, s, t, at both ends?
			d_pedgespanpackage->light = d_light;
			d_pedgespanpackage->zi = d_zi;

			d_pedgespanpackage++;
		}
		else
		{
			D_PolysetSetUpForLineScan(plefttop[0], plefttop[1],
								  pleftbottom[0], pleftbottom[1]);

			d_pdestbasestep = screenwidth + ubasestep;
			d_pdestextrastep = d_pdestbasestep + 1;

			d_pzbasestep = d_zwidth + ubasestep;
			d_pzextrastep = d_pzbasestep + 1;

			if (ubasestep < 0)
				working_lstepx = r_lstepx - 1;
			else
				working_lstepx = r_lstepx;

			d_countextrastep = ubasestep + 1;
			d_ptexbasestep = ((r_sstepy + r_sstepx * ubasestep) >> 16) +
					((r_tstepy + r_tstepx * ubasestep) >> 16) *
					r_affinetridesc.skinwidth;
			d_sfracbasestep = (r_sstepy + r_sstepx * ubasestep) & 0xFFFF;
			d_tfracbasestep = (r_tstepy + r_tstepx * ubasestep) & 0xFFFF;
			d_lightbasestep = r_lstepy + working_lstepx * ubasestep;
			d_zibasestep = r_zistepy + r_zistepx * ubasestep;

			d_ptexextrastep = ((r_sstepy + r_sstepx * d_countextrastep) >> 16) +
					((r_tstepy + r_tstepx * d_countextrastep) >> 16) *
					r_affinetridesc.skinwidth;
			d_sfracextrastep = (r_sstepy+r_sstepx*d_countextrastep) & 0xFFFF;
			d_tfracextrastep = (r_tstepy+r_tstepx*d_countextrastep) & 0xFFFF;
			d_lightextrastep = d_lightbasestep + working_lstepx;
			d_ziextrastep = d_zibasestep + r_zistepx;

			D_PolysetScanLeftEdge (height);
		}
	}

// scan out the top (and possibly only) part of the right edge, updating the
// count field
	d_pedgespanpackage = a_spans;

	D_PolysetSetUpForLineScan(prighttop[0], prighttop[1],
						  prightbottom[0], prightbottom[1]);
	d_aspancount = 0;
	d_countextrastep = ubasestep + 1;
	originalcount = a_spans[initialrightheight].count;
	a_spans[initialrightheight].count = -999999; // mark end of the spanpackages
	D_PolysetDrawSpans8 (a_spans);

// scan out the bottom part of the right edge, if it exists
	if (pedgetable->numrightedges == 2)
	{
		int				height;
		spanpackage_t	*pstart;

		pstart = a_spans + initialrightheight;
		pstart->count = originalcount;

		d_aspancount = prightbottom[0] - prighttop[0];

		prighttop = prightbottom;
		prightbottom = pedgetable->prightedgevert2;

		height = prightbottom[1] - prighttop[1];

		D_PolysetSetUpForLineScan(prighttop[0], prighttop[1],
							  prightbottom[0], prightbottom[1]);

		d_countextrastep = ubasestep + 1;
		a_spans[initialrightheight + height].count = -999999;
											// mark end of the spanpackages
		D_PolysetDrawSpans8 (pstart);
	}
}


/*
================
D_PolysetSetEdgeTable
================
*/
void D_PolysetSetEdgeTable (void)
{
	int			edgetableindex;

	edgetableindex = 0;	// assume the vertices are already in
						//  top to bottom order

//
// determine which edges are right & left, and the order in which
// to rasterize them
//
	if (r_p0[1] >= r_p1[1])
	{
		if (r_p0[1] == r_p1[1])
		{
			if (r_p0[1] < r_p2[1])
				pedgetable = &edgetables[2];
			else
				pedgetable = &edgetables[5];

			return;
		}
		else
		{
			edgetableindex = 1;
		}
	}

	if (r_p0[1] == r_p2[1])
	{
		if (edgetableindex)
			pedgetable = &edgetables[8];
		else
			pedgetable = &edgetables[9];

		return;
	}
	else if (r_p1[1] == r_p2[1])
	{
		if (edgetableindex)
			pedgetable = &edgetables[10];
		else
			pedgetable = &edgetables[11];

		return;
	}

	if (r_p0[1] > r_p2[1])
		edgetableindex += 2;

	if (r_p1[1] > r_p2[1])
		edgetableindex += 4;

	pedgetable = &edgetables[edgetableindex];
}


#if 0

void D_PolysetRecursiveDrawLine (int *lp1, int *lp2)
{
	int		d;
	int		new[6];
	int 	ofs;
	
	d = lp2[0] - lp1[0];
	if (d < -1 || d > 1)
		goto split;
	d = lp2[1] - lp1[1];
	if (d < -1 || d > 1)
		goto split;

	return;	// line is completed

split:
// split this edge
	new[0] = (lp1[0] + lp2[0]) >> 1;
	new[1] = (lp1[1] + lp2[1]) >> 1;
	new[5] = (lp1[5] + lp2[5]) >> 1;
	new[2] = (lp1[2] + lp2[2]) >> 1;
	new[3] = (lp1[3] + lp2[3]) >> 1;
	new[4] = (lp1[4] + lp2[4]) >> 1;

// draw the point
	ofs = d_scantable[new[1]] + new[0];
	if (new[5] > d_pzbuffer[ofs])
	{
		int		pix;
		
		d_pzbuffer[ofs] = new[5];
		pix = skintable[new[3]>>16][new[2]>>16];
//		pix = ((byte *)acolormap)[pix + (new[4] & 0xFF00)];
		d_viewbuffer[ofs] = pix;
	}

// recursively continue
	D_PolysetRecursiveDrawLine (lp1, new);
	D_PolysetRecursiveDrawLine (new, lp2);
}

void D_PolysetRecursiveTriangle2 (int *lp1, int *lp2, int *lp3)
{
	int		d;
	int		new[4];
	
	d = lp2[0] - lp1[0];
	if (d < -1 || d > 1)
		goto split;
	d = lp2[1] - lp1[1];
	if (d < -1 || d > 1)
		goto split;
	return;

split:
// split this edge
	new[0] = (lp1[0] + lp2[0]) >> 1;
	new[1] = (lp1[1] + lp2[1]) >> 1;
	new[5] = (lp1[5] + lp2[5]) >> 1;
	new[2] = (lp1[2] + lp2[2]) >> 1;
	new[3] = (lp1[3] + lp2[3]) >> 1;
	new[4] = (lp1[4] + lp2[4]) >> 1;

	D_PolysetRecursiveDrawLine (new, lp3);

// recursively continue
	D_PolysetRecursiveTriangle (lp1, new, lp3);
	D_PolysetRecursiveTriangle (new, lp2, lp3);
}

#endif

