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

// r_draw.c

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"	// FIXME: shouldn't need to include this

#define MAXLEFTCLIPEDGES		100

// !!! if these are changed, they must be changed in asm_draw.h too !!!
#define FULLY_CLIPPED_CACHED	0x80000000
#define FRAMECOUNT_MASK			0x7FFFFFFF

unsigned int	cacheoffset;

int			c_faceclip;					// number of faces clipped

zpointdesc_t	r_zpointdesc;

polydesc_t		r_polydesc;



clipplane_t	*entity_clipplanes;
clipplane_t	view_clipplanes[4];
clipplane_t	world_clipplanes[16];

medge_t			*r_pedge;

qboolean		r_leftclipped, r_rightclipped;
static qboolean	makeleftedge, makerightedge;
qboolean		r_nearzionly;

int		sintable[SIN_BUFFER_SIZE];
int		intsintable[SIN_BUFFER_SIZE];

mvertex_t	r_leftenter, r_leftexit;
mvertex_t	r_rightenter, r_rightexit;

typedef struct
{
	float	u,v;
	int		ceilv;
} evert_t;

int				r_emitted;
float			r_nearzi;
float			r_u1, r_v1, r_lzi1;
int				r_ceilv1;

qboolean	r_lastvertvalid;

static inline int FastCeil(float x)
{
	int i = (int)x;
	if ((float)i < x)
		++i;
	return i;
}

#ifdef PD_FAST_FACES
/*
==============================================================================

World face edge emission with the scratch state on the stack

R_RenderFace pushes every edge of a face through R_ClipEdge / R_EmitEdge, which
in the original keep their working state (the last projected vertex, the
clipped/emitted flags, the near 1/z, the cache offset, the free edge pointer...)
in about twenty globals. On the Playdate a store to a global costs ~26 ns per
byte while the stack is fast internal RAM, so that bookkeeping was a large part
of a face's cost. Here the same state lives in an rface_t on the stack and the
free edge pointer is only written back once per face. The edges, surfaces and
cache offsets that come out are identical.
==============================================================================
*/

typedef struct
{
	edge_t		*edge_p;			// next free edge; stored back to edge_p by the caller
	int			surfnum;			// number of the surface being built
	medge_t		*pedge;
	unsigned	cacheoffset;
	float		nearzi;
	float		u1, v1, lzi1;
	int			ceilv1;
	qboolean	emitted, nearzionly, lastvertvalid, leftclipped, rightclipped;
} rface_t;
// r_leftenter etc. stay globals: they are only written when a face crosses a side plane, and
// (as in the original) a face that sets just one of an enter/exit pair reuses the other from
// an earlier face, which changes the picture

static void R_EmitEdgeF (rface_t *f, mvertex_t *pv0, mvertex_t *pv1)
{
	edge_t	*edge, *pcheck;
	int		u_check;
	float	u, u_step;
	vec3_t	local, transformed;
	float	*world;
	int		v, v2, ceilv0;
	float	scale, lzi0, u0, v0;
	int		side;

	if (f->lastvertvalid)
	{
		u0 = f->u1;
		v0 = f->v1;
		lzi0 = f->lzi1;
		ceilv0 = f->ceilv1;
	}
	else
	{
		world = &pv0->position[0];

	// transform and project
		VectorSubtract (world, modelorg, local);
		TransformVector (local, transformed);

		if (transformed[2] < NEAR_CLIP)
			transformed[2] = NEAR_CLIP;

		lzi0 = 1.0f / transformed[2];

		scale = xscale * lzi0;
		u0 = (xcenter + scale*transformed[0]);
		if (u0 < r_refdef.fvrectx_adj)
			u0 = r_refdef.fvrectx_adj;
		if (u0 > r_refdef.fvrectright_adj)
			u0 = r_refdef.fvrectright_adj;

		scale = yscale * lzi0;
		v0 = (ycenter - scale*transformed[1]);
		if (v0 < r_refdef.fvrecty_adj)
			v0 = r_refdef.fvrecty_adj;
		if (v0 > r_refdef.fvrectbottom_adj)
			v0 = r_refdef.fvrectbottom_adj;

		ceilv0 = FastCeil(v0);
	}

	world = &pv1->position[0];

// transform and project
	VectorSubtract (world, modelorg, local);
	TransformVector (local, transformed);

	if (transformed[2] < NEAR_CLIP)
		transformed[2] = NEAR_CLIP;

	f->lzi1 = 1.0f / transformed[2];

	scale = xscale * f->lzi1;
	f->u1 = (xcenter + scale*transformed[0]);
	if (f->u1 < r_refdef.fvrectx_adj)
		f->u1 = r_refdef.fvrectx_adj;
	if (f->u1 > r_refdef.fvrectright_adj)
		f->u1 = r_refdef.fvrectright_adj;

	scale = yscale * f->lzi1;
	f->v1 = (ycenter - scale*transformed[1]);
	if (f->v1 < r_refdef.fvrecty_adj)
		f->v1 = r_refdef.fvrecty_adj;
	if (f->v1 > r_refdef.fvrectbottom_adj)
		f->v1 = r_refdef.fvrectbottom_adj;

	if (f->lzi1 > lzi0)
		lzi0 = f->lzi1;

	if (lzi0 > f->nearzi)	// for mipmap finding
		f->nearzi = lzi0;

// for right edges, all we want is the effect on 1/z
	if (f->nearzionly)
		return;

	f->emitted = 1;

	f->ceilv1 = FastCeil(f->v1);

// create the edge
	if (ceilv0 == f->ceilv1)
	{
	// we cache unclipped horizontal edges as fully clipped
		if (f->cacheoffset != 0x7FFFFFFF)
		{
			f->cacheoffset = FULLY_CLIPPED_CACHED |
					(r_framecount & FRAMECOUNT_MASK);
		}

		return;		// horizontal edge
	}

	side = ceilv0 > f->ceilv1;

	edge = f->edge_p++;

	edge->owner = f->pedge;

	edge->nearzi = lzi0;

	if (side == 0)
	{
	// trailing edge (go from p1 to p2)
		v = ceilv0;
		v2 = f->ceilv1 - 1;

		edge->surfs[0] = f->surfnum;
		edge->surfs[1] = 0;

		u_step = ((f->u1 - u0) / (f->v1 - v0));
		u = u0 + ((float)v - v0) * u_step;
	}
	else
	{
	// leading edge (go from p2 to p1)
		v2 = ceilv0 - 1;
		v = f->ceilv1;

		edge->surfs[0] = 0;
		edge->surfs[1] = f->surfnum;

		u_step = ((u0 - f->u1) / (v0 - f->v1));
		u = f->u1 + ((float)v - f->v1) * u_step;
	}

	edge->u_step = (fixed16_t)(u_step * 0x100000);
	edge->u = ((fixed16_t)(u * 0x100000)) + 0xFFFFF;

// we need to do this to avoid stepping off the edges if a very nearly
// horizontal edge is less than epsilon above a scan, and numeric error causes
// it to incorrectly extend to the scan, and the extension of the line goes off
// the edge of the screen
// FIXME: is this actually needed?
	if (edge->u < r_refdef.vrect_x_adj_shift20)
		edge->u = r_refdef.vrect_x_adj_shift20;
	if (edge->u > r_refdef.vrectright_adj_shift20)
		edge->u = r_refdef.vrectright_adj_shift20;

//
// sort the edge in normally
//
	u_check = edge->u;
	if (edge->surfs[0])
		u_check++;	// sort trailers after leaders

	if (!newedges[v] || newedges[v]->u >= u_check)
	{
		edge->next = newedges[v];
		newedges[v] = edge;
	}
	else
	{
		pcheck = newedges[v];
		while (pcheck->next && pcheck->next->u < u_check)
			pcheck = pcheck->next;
		edge->next = pcheck->next;
		pcheck->next = edge;
	}

	edge->nextremove = removeedges[v2];
	removeedges[v2] = edge;
}


static void R_ClipEdgeF (rface_t *f, mvertex_t *pv0, mvertex_t *pv1, clipplane_t *clip)
{
	float		d0, d1, t;
	mvertex_t	clipvert;

	if (clip)
	{
		do
		{
			d0 = DotProduct (pv0->position, clip->normal) - clip->dist;
			d1 = DotProduct (pv1->position, clip->normal) - clip->dist;

			if (d0 >= 0)
			{
			// point 0 is unclipped
				if (d1 >= 0)
				{
				// both points are unclipped
					continue;
				}

			// only point 1 is clipped

			// we don't cache clipped edges
				f->cacheoffset = 0x7FFFFFFF;

				t = d0 / (d0 - d1);
				clipvert.position[0] = pv0->position[0] +
						t * (pv1->position[0] - pv0->position[0]);
				clipvert.position[1] = pv0->position[1] +
						t * (pv1->position[1] - pv0->position[1]);
				clipvert.position[2] = pv0->position[2] +
						t * (pv1->position[2] - pv0->position[2]);

				if (clip->leftedge)
				{
					f->leftclipped = true;
					r_leftexit = clipvert;
				}
				else if (clip->rightedge)
				{
					f->rightclipped = true;
					r_rightexit = clipvert;
				}

				R_ClipEdgeF (f, pv0, &clipvert, clip->next);
				return;
			}
			else
			{
			// point 0 is clipped
				if (d1 < 0)
				{
				// both points are clipped
				// we do cache fully clipped edges
					if (!f->leftclipped)
						f->cacheoffset = FULLY_CLIPPED_CACHED |
								(r_framecount & FRAMECOUNT_MASK);
					return;
				}

			// only point 0 is clipped
				f->lastvertvalid = false;

			// we don't cache partially clipped edges
				f->cacheoffset = 0x7FFFFFFF;

				t = d0 / (d0 - d1);
				clipvert.position[0] = pv0->position[0] +
						t * (pv1->position[0] - pv0->position[0]);
				clipvert.position[1] = pv0->position[1] +
						t * (pv1->position[1] - pv0->position[1]);
				clipvert.position[2] = pv0->position[2] +
						t * (pv1->position[2] - pv0->position[2]);

				if (clip->leftedge)
				{
					f->leftclipped = true;
					r_leftenter = clipvert;
				}
				else if (clip->rightedge)
				{
					f->rightclipped = true;
					r_rightenter = clipvert;
				}

				R_ClipEdgeF (f, &clipvert, pv1, clip->next);
				return;
			}
		} while ((clip = clip->next) != NULL);
	}

// add the edge
	R_EmitEdgeF (f, pv0, pv1);
}


static void R_EmitCachedEdgeF (rface_t *f)
{
	edge_t		*pedge_t;

	pedge_t = (edge_t *)((uintptr_t)r_edges + f->pedge->cachededgeoffset);

	if (!pedge_t->surfs[0])
		pedge_t->surfs[0] = f->surfnum;
	else
		pedge_t->surfs[1] = f->surfnum;

	if (pedge_t->nearzi > f->nearzi)	// for mipmap finding
		f->nearzi = pedge_t->nearzi;

	f->emitted = 1;
}


// links up the clip planes that clipflags asks for, returns the first
static inline clipplane_t *R_LinkClipPlanes (int clipflags)
{
	clipplane_t	*pclip = NULL;
	unsigned	mask;
	int			i;

	for (i=3, mask = 0x08 ; i>=0 ; i--, mask >>= 1)
	{
		if (clipflags & mask)
		{
		// consecutive faces almost always use the same planes: only store what changes
			if (view_clipplanes[i].next != pclip)
				view_clipplanes[i].next = pclip;
			pclip = &view_clipplanes[i];
		}
	}

	return pclip;
}


// fill in the surface s that the edges of f belong to (the caller counts and advances)
static inline void R_PostSurface (surf_t *s, rface_t *f, msurface_t *fa, int key, qboolean sub)
{
	mplane_t	*pplane;
	float		distinv;
	vec3_t		p_normal;

	s->data = (void *)fa;
	s->nearzi = f->nearzi;
	s->flags = fa->flags;
	s->insubmodel = sub;
	s->spanstate = 0;
	s->entity = currententity;
	s->key = key;
	s->spans = NULL;

	pplane = fa->plane;
// FIXME: cache this?
	TransformVector (pplane->normal, p_normal);
// FIXME: cache this?
	distinv = 1.0f / (pplane->dist - DotProduct (modelorg, pplane->normal));

	s->d_zistepu = p_normal[0] * xscaleinv * distinv;
	s->d_zistepv = -p_normal[1] * yscaleinv * distinv;
	s->d_ziorigin = p_normal[2] * distinv -
			xcenter * s->d_zistepu -
			ycenter * s->d_zistepv;
}


/*
================
R_RenderFaceW

rworld_t holds edge_p, surface_p, r_currentkey and r_polycount for the whole world traversal
in the caller's stack frame: as globals, every face wrote four of them back to slow memory.
================
*/
void R_RenderFaceW (rworld_t *w, msurface_t *fa, int clipflags)
{
	int			i, lindex;
	medge_t		*pedges, tedge;
	clipplane_t	*pclip;
	rface_t		f;
	qboolean	makeleftedge, makerightedge;
	int			*psurfedge;
	mvertex_t	*pvertbase;
	unsigned	framecount;

// skip out if no more surfs
	if ((w->surface_p) >= surf_max)
	{
		r_outofsurfaces++;
		return;
	}

// ditto if not enough edges left, or switch to auxedges if possible
	if ((w->edge_p + fa->numedges + 4) >= edge_max)
	{
		r_outofedges += fa->numedges;
		return;
	}

	if (r_speeds.value)
		c_faceclip++;

// set up clip planes
	pclip = R_LinkClipPlanes (clipflags);

// push the edges through
	f.edge_p = w->edge_p;
	f.surfnum = w->surface_p - surfaces;
	f.emitted = 0;
	f.nearzi = 0;
	f.nearzionly = false;
	f.lastvertvalid = false;
	f.u1 = f.v1 = f.lzi1 = 0;
	f.ceilv1 = 0;
	makeleftedge = makerightedge = false;
	pedges = currententity->model->edges;
	psurfedge = &currententity->model->surfedges[fa->firstedge];
	pvertbase = r_pcurrentvertbase;
	framecount = r_framecount & FRAMECOUNT_MASK;

	for (i=0 ; i<fa->numedges ; i++)
	{
		mvertex_t	*pv0, *pv1;

		lindex = psurfedge[i];

		if (lindex > 0)
		{
			f.pedge = &pedges[lindex];
			pv0 = &pvertbase[f.pedge->v[0]];
			pv1 = &pvertbase[f.pedge->v[1]];
		}
		else
		{
			f.pedge = &pedges[-lindex];
			pv0 = &pvertbase[f.pedge->v[1]];
			pv1 = &pvertbase[f.pedge->v[0]];
		}

	// if the edge is cached, we can just reuse the edge
		if (!insubmodel)
		{
			unsigned	cached = f.pedge->cachededgeoffset;

			if (cached & FULLY_CLIPPED_CACHED)
			{
				if ((cached & FRAMECOUNT_MASK) == framecount)
				{
					f.lastvertvalid = false;
					continue;
				}
			}
			else
			{
			// it's cached if the cached edge is valid and is owned
			// by this medge_t
				if ((((uintptr_t)f.edge_p - (uintptr_t)r_edges) > cached) &&
					(((edge_t *)((uintptr_t)r_edges + cached))->owner == f.pedge))
				{
					R_EmitCachedEdgeF (&f);
					f.lastvertvalid = false;
					continue;
				}
			}
		}

	// assume it's cacheable
		f.cacheoffset = (byte *)f.edge_p - (byte *)r_edges;
		f.leftclipped = f.rightclipped = false;
		R_ClipEdgeF (&f, pv0, pv1, pclip);
		f.pedge->cachededgeoffset = f.cacheoffset;

		if (f.leftclipped)
			makeleftedge = true;
		if (f.rightclipped)
			makerightedge = true;
		f.lastvertvalid = true;
	}

// if there was a clip off the left edge, add that edge too
// FIXME: faster to do in screen space?
// FIXME: share clipped edges?
	if (makeleftedge)
	{
		f.pedge = &tedge;
		f.lastvertvalid = false;
		R_ClipEdgeF (&f, &r_leftexit, &r_leftenter, pclip->next);
	}

// if there was a clip off the right edge, get the right r_nearzi
	if (makerightedge)
	{
		f.pedge = &tedge;
		f.lastvertvalid = false;
		f.nearzionly = true;
		R_ClipEdgeF (&f, &r_rightexit, &r_rightenter, view_clipplanes[1].next);
	}

	w->edge_p = f.edge_p;

// if no edges made it out, return without posting the surface
	if (!f.emitted)
		return;

	R_PostSurface (w->surface_p, &f, fa, w->currentkey++, insubmodel);
	w->polycount++;
	w->surface_p++;
}


/*
================
R_RenderFace
================
*/
void R_RenderFace (msurface_t *fa, int clipflags)
{
	rworld_t	w;

	w.edge_p = edge_p;
	w.surface_p = surface_p;
	w.currentkey = r_currentkey;
	w.polycount = r_polycount;
	R_RenderFaceW (&w, fa, clipflags);
	edge_p = w.edge_p;
	surface_p = w.surface_p;
	r_currentkey = w.currentkey;
	r_polycount = w.polycount;
}


/*
================
R_RenderBmodelFace
================
*/
void R_RenderBmodelFace (bedge_t *pedges, msurface_t *psurf)
{
	medge_t		tedge;
	clipplane_t	*pclip;
	rface_t		f;
	qboolean	makeleftedge, makerightedge;

// skip out if no more surfs
	if (surface_p >= surf_max)
	{
		r_outofsurfaces++;
		return;
	}

// ditto if not enough edges left, or switch to auxedges if possible
	if ((edge_p + psurf->numedges + 4) >= edge_max)
	{
		r_outofedges += psurf->numedges;
		return;
	}

	if (r_speeds.value)
		c_faceclip++;

// set up clip planes
	pclip = R_LinkClipPlanes (r_clipflags);

// push the edges through
	f.edge_p = edge_p;
	f.surfnum = surface_p - surfaces;
	f.emitted = 0;
	f.nearzi = 0;
	f.nearzionly = false;
	f.cacheoffset = 0;
	f.u1 = f.v1 = f.lzi1 = 0;
	f.ceilv1 = 0;
	makeleftedge = makerightedge = false;
// this is a dummy to give the caching mechanism someplace to write to
	f.pedge = &tedge;
// FIXME: keep clipped bmodel edges in clockwise order so last vertex caching
// can be used?
	f.lastvertvalid = false;

	for ( ; pedges ; pedges = pedges->pnext)
	{
		f.leftclipped = f.rightclipped = false;
		R_ClipEdgeF (&f, pedges->v[0], pedges->v[1], pclip);

		if (f.leftclipped)
			makeleftedge = true;
		if (f.rightclipped)
			makerightedge = true;
	}

// if there was a clip off the left edge, add that edge too
// FIXME: faster to do in screen space?
// FIXME: share clipped edges?
	if (makeleftedge)
	{
		f.pedge = &tedge;
		R_ClipEdgeF (&f, &r_leftexit, &r_leftenter, pclip->next);
	}

// if there was a clip off the right edge, get the right r_nearzi
	if (makerightedge)
	{
		f.pedge = &tedge;
		f.nearzionly = true;
		R_ClipEdgeF (&f, &r_rightexit, &r_rightenter, view_clipplanes[1].next);
	}

	edge_p = f.edge_p;

// if no edges made it out, return without posting the surface
	if (!f.emitted)
		return;

	R_PostSurface (surface_p, &f, psurf, r_currentbkey, true);
	r_polycount++;
	surface_p++;
}

#else	// !PD_FAST_FACES

/*
================
R_EmitEdge
================
*/
void R_EmitEdge (mvertex_t *pv0, mvertex_t *pv1)
{
	edge_t	*edge, *pcheck;
	int		u_check;
	float	u, u_step;
	vec3_t	local, transformed;
	float	*world;
	int		v, v2, ceilv0;
	float	scale, lzi0, u0, v0;
	int		side;

	if (r_lastvertvalid)
	{
		u0 = r_u1;
		v0 = r_v1;
		lzi0 = r_lzi1;
		ceilv0 = r_ceilv1;
	}
	else
	{
		world = &pv0->position[0];
	
	// transform and project
		VectorSubtract (world, modelorg, local);
		TransformVector (local, transformed);
	
		if (transformed[2] < NEAR_CLIP)
			transformed[2] = NEAR_CLIP;
	
		lzi0 = 1.0f / transformed[2];
	
	// FIXME: build x/yscale into transform?
		scale = xscale * lzi0;
		u0 = (xcenter + scale*transformed[0]);
		if (u0 < r_refdef.fvrectx_adj)
			u0 = r_refdef.fvrectx_adj;
		if (u0 > r_refdef.fvrectright_adj)
			u0 = r_refdef.fvrectright_adj;
	
		scale = yscale * lzi0;
		v0 = (ycenter - scale*transformed[1]);
		if (v0 < r_refdef.fvrecty_adj)
			v0 = r_refdef.fvrecty_adj;
		if (v0 > r_refdef.fvrectbottom_adj)
			v0 = r_refdef.fvrectbottom_adj;
	
		ceilv0 = FastCeil(v0);
	}

	world = &pv1->position[0];

// transform and project
	VectorSubtract (world, modelorg, local);
	TransformVector (local, transformed);

	if (transformed[2] < NEAR_CLIP)
		transformed[2] = NEAR_CLIP;

	r_lzi1 = 1.0f / transformed[2];

	scale = xscale * r_lzi1;
	r_u1 = (xcenter + scale*transformed[0]);
	if (r_u1 < r_refdef.fvrectx_adj)
		r_u1 = r_refdef.fvrectx_adj;
	if (r_u1 > r_refdef.fvrectright_adj)
		r_u1 = r_refdef.fvrectright_adj;

	scale = yscale * r_lzi1;
	r_v1 = (ycenter - scale*transformed[1]);
	if (r_v1 < r_refdef.fvrecty_adj)
		r_v1 = r_refdef.fvrecty_adj;
	if (r_v1 > r_refdef.fvrectbottom_adj)
		r_v1 = r_refdef.fvrectbottom_adj;

	if (r_lzi1 > lzi0)
		lzi0 = r_lzi1;

	if (lzi0 > r_nearzi)	// for mipmap finding
		r_nearzi = lzi0;

// for right edges, all we want is the effect on 1/z
	if (r_nearzionly)
		return;

	r_emitted = 1;

	r_ceilv1 = FastCeil(r_v1);


// create the edge
	if (ceilv0 == r_ceilv1)
	{
	// we cache unclipped horizontal edges as fully clipped
		if (cacheoffset != 0x7FFFFFFF)
		{
			cacheoffset = FULLY_CLIPPED_CACHED |
					(r_framecount & FRAMECOUNT_MASK);
		}

		return;		// horizontal edge
	}

	side = ceilv0 > r_ceilv1;

	edge = edge_p++;

	edge->owner = r_pedge;

	edge->nearzi = lzi0;

	if (side == 0)
	{
	// trailing edge (go from p1 to p2)
		v = ceilv0;
		v2 = r_ceilv1 - 1;

		edge->surfs[0] = surface_p - surfaces;
		edge->surfs[1] = 0;

		u_step = ((r_u1 - u0) / (r_v1 - v0));
		u = u0 + ((float)v - v0) * u_step;
	}
	else
	{
	// leading edge (go from p2 to p1)
		v2 = ceilv0 - 1;
		v = r_ceilv1;

		edge->surfs[0] = 0;
		edge->surfs[1] = surface_p - surfaces;

		u_step = ((u0 - r_u1) / (v0 - r_v1));
		u = r_u1 + ((float)v - r_v1) * u_step;
	}

	edge->u_step = (fixed16_t)(u_step * 0x100000);
	edge->u = ((fixed16_t)(u * 0x100000)) + 0xFFFFF;

// we need to do this to avoid stepping off the edges if a very nearly
// horizontal edge is less than epsilon above a scan, and numeric error causes
// it to incorrectly extend to the scan, and the extension of the line goes off
// the edge of the screen
// FIXME: is this actually needed?
	if (edge->u < r_refdef.vrect_x_adj_shift20)
		edge->u = r_refdef.vrect_x_adj_shift20;
	if (edge->u > r_refdef.vrectright_adj_shift20)
		edge->u = r_refdef.vrectright_adj_shift20;

//
// sort the edge in normally
//
	u_check = edge->u;
	if (edge->surfs[0])
		u_check++;	// sort trailers after leaders

	if (!newedges[v] || newedges[v]->u >= u_check)
	{
		edge->next = newedges[v];
		newedges[v] = edge;
	}
	else
	{
		pcheck = newedges[v];
		while (pcheck->next && pcheck->next->u < u_check)
			pcheck = pcheck->next;
		edge->next = pcheck->next;
		pcheck->next = edge;
	}

	edge->nextremove = removeedges[v2];
	removeedges[v2] = edge;
}


/*
================
R_ClipEdge
================
*/
void R_ClipEdge (mvertex_t *pv0, mvertex_t *pv1, clipplane_t *clip)
{
	float		d0, d1, f;
	mvertex_t	clipvert;

	if (clip)
	{
		do
		{
			d0 = DotProduct (pv0->position, clip->normal) - clip->dist;
			d1 = DotProduct (pv1->position, clip->normal) - clip->dist;

			if (d0 >= 0)
			{
			// point 0 is unclipped
				if (d1 >= 0)
				{
				// both points are unclipped
					continue;
				}

			// only point 1 is clipped

			// we don't cache clipped edges
				cacheoffset = 0x7FFFFFFF;

				f = d0 / (d0 - d1);
				clipvert.position[0] = pv0->position[0] +
						f * (pv1->position[0] - pv0->position[0]);
				clipvert.position[1] = pv0->position[1] +
						f * (pv1->position[1] - pv0->position[1]);
				clipvert.position[2] = pv0->position[2] +
						f * (pv1->position[2] - pv0->position[2]);

				if (clip->leftedge)
				{
					r_leftclipped = true;
					r_leftexit = clipvert;
				}
				else if (clip->rightedge)
				{
					r_rightclipped = true;
					r_rightexit = clipvert;
				}

				R_ClipEdge (pv0, &clipvert, clip->next);
				return;
			}
			else
			{
			// point 0 is clipped
				if (d1 < 0)
				{
				// both points are clipped
				// we do cache fully clipped edges
					if (!r_leftclipped)
						cacheoffset = FULLY_CLIPPED_CACHED |
								(r_framecount & FRAMECOUNT_MASK);
					return;
				}

			// only point 0 is clipped
				r_lastvertvalid = false;

			// we don't cache partially clipped edges
				cacheoffset = 0x7FFFFFFF;

				f = d0 / (d0 - d1);
				clipvert.position[0] = pv0->position[0] +
						f * (pv1->position[0] - pv0->position[0]);
				clipvert.position[1] = pv0->position[1] +
						f * (pv1->position[1] - pv0->position[1]);
				clipvert.position[2] = pv0->position[2] +
						f * (pv1->position[2] - pv0->position[2]);

				if (clip->leftedge)
				{
					r_leftclipped = true;
					r_leftenter = clipvert;
				}
				else if (clip->rightedge)
				{
					r_rightclipped = true;
					r_rightenter = clipvert;
				}

				R_ClipEdge (&clipvert, pv1, clip->next);
				return;
			}
		} while ((clip = clip->next) != NULL);
	}

// add the edge
	R_EmitEdge (pv0, pv1);
}


/*
================
R_EmitCachedEdge
================
*/
void R_EmitCachedEdge (void)
{
	edge_t		*pedge_t;

	pedge_t = (edge_t *)((uintptr_t)r_edges + r_pedge->cachededgeoffset);

	if (!pedge_t->surfs[0])
		pedge_t->surfs[0] = surface_p - surfaces;
	else
		pedge_t->surfs[1] = surface_p - surfaces;

	if (pedge_t->nearzi > r_nearzi)	// for mipmap finding
		r_nearzi = pedge_t->nearzi;

	r_emitted = 1;
}


/*
================
R_RenderFace
================
*/
void R_RenderFace (msurface_t *fa, int clipflags)
{
	int			i, lindex;
	unsigned	mask;
	mplane_t	*pplane;
	float		distinv;
	vec3_t		p_normal;
	medge_t		*pedges, tedge;
	clipplane_t	*pclip;

// skip out if no more surfs
	if ((surface_p) >= surf_max)
	{
		r_outofsurfaces++;
		return;
	}

// ditto if not enough edges left, or switch to auxedges if possible
	if ((edge_p + fa->numedges + 4) >= edge_max)
	{
		r_outofedges += fa->numedges;
		return;
	}

	c_faceclip++;

// set up clip planes
	pclip = NULL;

	for (i=3, mask = 0x08 ; i>=0 ; i--, mask >>= 1)
	{
		if (clipflags & mask)
		{
			view_clipplanes[i].next = pclip;
			pclip = &view_clipplanes[i];
		}
	}

// push the edges through
	r_emitted = 0;
	r_nearzi = 0;
	r_nearzionly = false;
	makeleftedge = makerightedge = false;
	pedges = currententity->model->edges;
	r_lastvertvalid = false;

	for (i=0 ; i<fa->numedges ; i++)
	{
		lindex = currententity->model->surfedges[fa->firstedge + i];

		if (lindex > 0)
		{
			r_pedge = &pedges[lindex];

		// if the edge is cached, we can just reuse the edge
			if (!insubmodel)
			{
				if (r_pedge->cachededgeoffset & FULLY_CLIPPED_CACHED)
				{
					if ((r_pedge->cachededgeoffset & FRAMECOUNT_MASK) ==
						r_framecount)
					{
						r_lastvertvalid = false;
						continue;
					}
				}
				else
				{
					if ((((uintptr_t)edge_p - (uintptr_t)r_edges) >
						 r_pedge->cachededgeoffset) &&
						(((edge_t *)((uintptr_t)r_edges +
						 r_pedge->cachededgeoffset))->owner == r_pedge))
					{
						R_EmitCachedEdge ();
						r_lastvertvalid = false;
						continue;
					}
				}
			}

		// assume it's cacheable
			cacheoffset = (byte *)edge_p - (byte *)r_edges;
			r_leftclipped = r_rightclipped = false;
			R_ClipEdge (&r_pcurrentvertbase[r_pedge->v[0]],
						&r_pcurrentvertbase[r_pedge->v[1]],
						pclip);
			r_pedge->cachededgeoffset = cacheoffset;

			if (r_leftclipped)
				makeleftedge = true;
			if (r_rightclipped)
				makerightedge = true;
			r_lastvertvalid = true;
		}
		else
		{
			lindex = -lindex;
			r_pedge = &pedges[lindex];
		// if the edge is cached, we can just reuse the edge
			if (!insubmodel)
			{
				if (r_pedge->cachededgeoffset & FULLY_CLIPPED_CACHED)
				{
					if ((r_pedge->cachededgeoffset & FRAMECOUNT_MASK) ==
						r_framecount)
					{
						r_lastvertvalid = false;
						continue;
					}
				}
				else
				{
				// it's cached if the cached edge is valid and is owned
				// by this medge_t
					if ((((uintptr_t)edge_p - (uintptr_t)r_edges) >
						 r_pedge->cachededgeoffset) &&
						(((edge_t *)((uintptr_t)r_edges +
						 r_pedge->cachededgeoffset))->owner == r_pedge))
					{
						R_EmitCachedEdge ();
						r_lastvertvalid = false;
						continue;
					}
				}
			}

		// assume it's cacheable
			cacheoffset = (byte *)edge_p - (byte *)r_edges;
			r_leftclipped = r_rightclipped = false;
			R_ClipEdge (&r_pcurrentvertbase[r_pedge->v[1]],
						&r_pcurrentvertbase[r_pedge->v[0]],
						pclip);
			r_pedge->cachededgeoffset = cacheoffset;

			if (r_leftclipped)
				makeleftedge = true;
			if (r_rightclipped)
				makerightedge = true;
			r_lastvertvalid = true;
		}
	}

// if there was a clip off the left edge, add that edge too
// FIXME: faster to do in screen space?
// FIXME: share clipped edges?
	if (makeleftedge)
	{
		r_pedge = &tedge;
		r_lastvertvalid = false;
		R_ClipEdge (&r_leftexit, &r_leftenter, pclip->next);
	}

// if there was a clip off the right edge, get the right r_nearzi
	if (makerightedge)
	{
		r_pedge = &tedge;
		r_lastvertvalid = false;
		r_nearzionly = true;
		R_ClipEdge (&r_rightexit, &r_rightenter, view_clipplanes[1].next);
	}

// if no edges made it out, return without posting the surface
	if (!r_emitted)
		return;

	r_polycount++;

	surface_p->data = (void *)fa;
	surface_p->nearzi = r_nearzi;
	surface_p->flags = fa->flags;
	surface_p->insubmodel = insubmodel;
	surface_p->spanstate = 0;
	surface_p->entity = currententity;
	surface_p->key = r_currentkey++;
	surface_p->spans = NULL;

	pplane = fa->plane;
// FIXME: cache this?
	TransformVector (pplane->normal, p_normal);
// FIXME: cache this?
	distinv = 1.0f / (pplane->dist - DotProduct (modelorg, pplane->normal));

	surface_p->d_zistepu = p_normal[0] * xscaleinv * distinv;
	surface_p->d_zistepv = -p_normal[1] * yscaleinv * distinv;
	surface_p->d_ziorigin = p_normal[2] * distinv -
			xcenter * surface_p->d_zistepu -
			ycenter * surface_p->d_zistepv;

//JDC	VectorCopy (r_worldmodelorg, surface_p->modelorg);
	surface_p++;
}


/*
================
R_RenderBmodelFace
================
*/
void R_RenderBmodelFace (bedge_t *pedges, msurface_t *psurf)
{
	int			i;
	unsigned	mask;
	mplane_t	*pplane;
	float		distinv;
	vec3_t		p_normal;
	medge_t		tedge;
	clipplane_t	*pclip;

// skip out if no more surfs
	if (surface_p >= surf_max)
	{
		r_outofsurfaces++;
		return;
	}

// ditto if not enough edges left, or switch to auxedges if possible
	if ((edge_p + psurf->numedges + 4) >= edge_max)
	{
		r_outofedges += psurf->numedges;
		return;
	}

	c_faceclip++;

// this is a dummy to give the caching mechanism someplace to write to
	r_pedge = &tedge;

// set up clip planes
	pclip = NULL;

	for (i=3, mask = 0x08 ; i>=0 ; i--, mask >>= 1)
	{
		if (r_clipflags & mask)
		{
			view_clipplanes[i].next = pclip;
			pclip = &view_clipplanes[i];
		}
	}

// push the edges through
	r_emitted = 0;
	r_nearzi = 0;
	r_nearzionly = false;
	makeleftedge = makerightedge = false;
// FIXME: keep clipped bmodel edges in clockwise order so last vertex caching
// can be used?
	r_lastvertvalid = false;

	for ( ; pedges ; pedges = pedges->pnext)
	{
		r_leftclipped = r_rightclipped = false;
		R_ClipEdge (pedges->v[0], pedges->v[1], pclip);

		if (r_leftclipped)
			makeleftedge = true;
		if (r_rightclipped)
			makerightedge = true;
	}

// if there was a clip off the left edge, add that edge too
// FIXME: faster to do in screen space?
// FIXME: share clipped edges?
	if (makeleftedge)
	{
		r_pedge = &tedge;
		R_ClipEdge (&r_leftexit, &r_leftenter, pclip->next);
	}

// if there was a clip off the right edge, get the right r_nearzi
	if (makerightedge)
	{
		r_pedge = &tedge;
		r_nearzionly = true;
		R_ClipEdge (&r_rightexit, &r_rightenter, view_clipplanes[1].next);
	}

// if no edges made it out, return without posting the surface
	if (!r_emitted)
		return;

	r_polycount++;

	surface_p->data = (void *)psurf;
	surface_p->nearzi = r_nearzi;
	surface_p->flags = psurf->flags;
	surface_p->insubmodel = true;
	surface_p->spanstate = 0;
	surface_p->entity = currententity;
	surface_p->key = r_currentbkey;
	surface_p->spans = NULL;

	pplane = psurf->plane;
// FIXME: cache this?
	TransformVector (pplane->normal, p_normal);
// FIXME: cache this?
	distinv = 1.0f / (pplane->dist - DotProduct (modelorg, pplane->normal));

	surface_p->d_zistepu = p_normal[0] * xscaleinv * distinv;
	surface_p->d_zistepv = -p_normal[1] * yscaleinv * distinv;
	surface_p->d_ziorigin = p_normal[2] * distinv -
			xcenter * surface_p->d_zistepu -
			ycenter * surface_p->d_zistepv;

//JDC	VectorCopy (r_worldmodelorg, surface_p->modelorg);
	surface_p++;
}

#endif	// PD_FAST_FACES


/*
================
R_RenderPoly
================
*/
void R_RenderPoly (msurface_t *fa, int clipflags)
{
	int			i, lindex, lnumverts, s_axis, t_axis;
	float		dist, lastdist, lzi, scale, u, v, frac;
	unsigned	mask;
	vec3_t		local, transformed;
	clipplane_t	*pclip;
	medge_t		*pedges;
	mplane_t	*pplane;
	static mvertex_t	verts[2][100];	//FIXME: do real number
	static polyvert_t	pverts[100];	//FIXME: do real number, safely
	int			vertpage, newverts, newpage, lastvert;
	qboolean	visible;

// FIXME: clean this up and make it faster
// FIXME: guard against running out of vertices

	s_axis = t_axis = 0;	// keep compiler happy

// set up clip planes
	pclip = NULL;

	for (i=3, mask = 0x08 ; i>=0 ; i--, mask >>= 1)
	{
		if (clipflags & mask)
		{
			view_clipplanes[i].next = pclip;
			pclip = &view_clipplanes[i];
		}
	}

// reconstruct the polygon
// FIXME: these should be precalculated and loaded off disk
	pedges = currententity->model->edges;
	lnumverts = fa->numedges;
	vertpage = 0;

	for (i=0 ; i<lnumverts ; i++)
	{
		lindex = currententity->model->surfedges[fa->firstedge + i];

		if (lindex > 0)
		{
			r_pedge = &pedges[lindex];
			verts[0][i] = r_pcurrentvertbase[r_pedge->v[0]];
		}
		else
		{
			r_pedge = &pedges[-lindex];
			verts[0][i] = r_pcurrentvertbase[r_pedge->v[1]];
		}
	}

// clip the polygon, done if not visible
	while (pclip)
	{
		lastvert = lnumverts - 1;
		lastdist = DotProduct (verts[vertpage][lastvert].position,
							   pclip->normal) - pclip->dist;

		visible = false;
		newverts = 0;
		newpage = vertpage ^ 1;

		for (i=0 ; i<lnumverts ; i++)
		{
			dist = DotProduct (verts[vertpage][i].position, pclip->normal) -
					pclip->dist;

			if ((lastdist > 0) != (dist > 0))
			{
				frac = dist / (dist - lastdist);
				verts[newpage][newverts].position[0] =
						verts[vertpage][i].position[0] +
						((verts[vertpage][lastvert].position[0] -
						  verts[vertpage][i].position[0]) * frac);
				verts[newpage][newverts].position[1] =
						verts[vertpage][i].position[1] +
						((verts[vertpage][lastvert].position[1] -
						  verts[vertpage][i].position[1]) * frac);
				verts[newpage][newverts].position[2] =
						verts[vertpage][i].position[2] +
						((verts[vertpage][lastvert].position[2] -
						  verts[vertpage][i].position[2]) * frac);
				newverts++;
			}

			if (dist >= 0)
			{
				verts[newpage][newverts] = verts[vertpage][i];
				newverts++;
				visible = true;
			}

			lastvert = i;
			lastdist = dist;
		}

		if (!visible || (newverts < 3))
			return;

		lnumverts = newverts;
		vertpage ^= 1;
		pclip = pclip->next;
	}

// transform and project, remembering the z values at the vertices and
// r_nearzi, and extract the s and t coordinates at the vertices
	pplane = fa->plane;
	switch (pplane->type)
	{
	case PLANE_X:
	case PLANE_ANYX:
		s_axis = 1;
		t_axis = 2;
		break;
	case PLANE_Y:
	case PLANE_ANYY:
		s_axis = 0;
		t_axis = 2;
		break;
	case PLANE_Z:
	case PLANE_ANYZ:
		s_axis = 0;
		t_axis = 1;
		break;
	}

	r_nearzi = 0;

	for (i=0 ; i<lnumverts ; i++)
	{
	// transform and project
		VectorSubtract (verts[vertpage][i].position, modelorg, local);
		TransformVector (local, transformed);

		if (transformed[2] < NEAR_CLIP)
			transformed[2] = NEAR_CLIP;

		lzi = 1.0f / transformed[2];

		if (lzi > r_nearzi)	// for mipmap finding
			r_nearzi = lzi;

	// FIXME: build x/yscale into transform?
		scale = xscale * lzi;
		u = (xcenter + scale*transformed[0]);
		if (u < r_refdef.fvrectx_adj)
			u = r_refdef.fvrectx_adj;
		if (u > r_refdef.fvrectright_adj)
			u = r_refdef.fvrectright_adj;

		scale = yscale * lzi;
		v = (ycenter - scale*transformed[1]);
		if (v < r_refdef.fvrecty_adj)
			v = r_refdef.fvrecty_adj;
		if (v > r_refdef.fvrectbottom_adj)
			v = r_refdef.fvrectbottom_adj;

		pverts[i].u = u;
		pverts[i].v = v;
		pverts[i].zi = lzi;
		pverts[i].s = verts[vertpage][i].position[s_axis];
		pverts[i].t = verts[vertpage][i].position[t_axis];
	}

// build the polygon descriptor, including fa, r_nearzi, and u, v, s, t, and z
// for each vertex
	r_polydesc.numverts = lnumverts;
	r_polydesc.nearzi = r_nearzi;
	r_polydesc.pcurrentface = fa;
	r_polydesc.pverts = pverts;

// draw the polygon
	D_DrawPoly ();
}


/*
================
R_ZDrawSubmodelPolys
================
*/
void R_ZDrawSubmodelPolys (model_t *pmodel)
{
	int			i, numsurfaces;
	msurface_t	*psurf;
	float		dot;
	mplane_t	*pplane;

	psurf = &pmodel->surfaces[pmodel->firstmodelsurface];
	numsurfaces = pmodel->nummodelsurfaces;

	for (i=0 ; i<numsurfaces ; i++, psurf++)
	{
	// find which side of the node we are on
		pplane = psurf->plane;

		dot = DotProduct (modelorg, pplane->normal) - pplane->dist;

	// draw the polygon
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
		// FIXME: use bounding-box-based frustum clipping info?
			R_RenderPoly (psurf, 15);
		}
	}
}

