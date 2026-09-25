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
// r_edge.c

#include "quakedef.h"
#include "r_local.h"
#include "pdprof.h"

#if 0
// FIXME
the complex cases add new polys on most lines, so dont optimize for keeping them the same
have multiple free span lists to try to get better coherence?
low depth complexity -- 1 to 3 or so

this breaks spans at every edge, even hidden ones (bad)

have a sentinal at both ends?
#endif


edge_t	*auxedges;
edge_t	*r_edges, *edge_p, *edge_max;

surf_t	*surfaces, *surface_p, *surf_max;

// surfaces are generated in back to front order by the bsp, so if a surf
// pointer is greater than another one, it should be drawn in front
// surfaces[1] is the background, and is used as the active surface stack

edge_t	*newedges[MAXHEIGHT];
edge_t	*removeedges[MAXHEIGHT];

espan_t	*span_p, *max_span_p;

int		r_currentkey;

extern	int	screenwidth;

int	current_iv;

int	edge_head_u_shift20, edge_tail_u_shift20;

static void (*pdrawfunc)(void);

edge_t	edge_head;
edge_t	edge_tail;
edge_t	edge_aftertail;
edge_t	edge_sentinel;

float	fv;

void R_GenerateSpans (void);
void R_GenerateSpansBackward (void);

void R_LeadingEdge (edge_t *edge);
void R_LeadingEdgeBackwards (edge_t *edge);
void R_TrailingEdge (surf_t *surf, edge_t *edge);


//=============================================================================


/*
==============
R_DrawCulledPolys
==============
*/
void R_DrawCulledPolys (void)
{
	surf_t			*s;
	msurface_t		*pface;

	currententity = &cl_entities[0];

	if (r_worldpolysbacktofront)
	{
		for (s=surface_p-1 ; s>&surfaces[1] ; s--)
		{
			if (!s->spans)
				continue;

			if (!(s->flags & SURF_DRAWBACKGROUND))
			{
				pface = (msurface_t *)s->data;
				R_RenderPoly (pface, 15);
			}
		}
	}
	else
	{
		for (s = &surfaces[1] ; s<surface_p ; s++)
		{
			if (!s->spans)
				continue;

			if (!(s->flags & SURF_DRAWBACKGROUND))
			{
				pface = (msurface_t *)s->data;
				R_RenderPoly (pface, 15);
			}
		}
	}
}


/*
==============
R_BeginEdgeFrame
==============
*/
void R_BeginEdgeFrame (void)
{
	int		v;

	edge_p = r_edges;
	edge_max = &r_edges[r_numallocatededges];

	surface_p = &surfaces[2];	// background is surface 1,
								//  surface 0 is a dummy
	surfaces[1].spans = NULL;	// no background spans yet
	surfaces[1].flags = SURF_DRAWBACKGROUND;

// put the background behind everything in the world
	if (r_draworder.value)
	{
		pdrawfunc = R_GenerateSpansBackward;
		surfaces[1].key = 0;
		r_currentkey = 1;
	}
	else
	{
		pdrawfunc = R_GenerateSpans;
		surfaces[1].key = 0x7FFFFFFF;
		r_currentkey = 0;
	}

// FIXME: set with memset
	for (v=r_refdef.vrect.y ; v<r_refdef.vrectbottom ; v++)
	{
		newedges[v] = removeedges[v] = NULL;
	}
}

/*
==============
R_InsertNewEdges

Adds the edges in the linked list edgestoadd, adding them to the edges in the
linked list edgelist.  edgestoadd is assumed to be sorted on u, and non-empty (this is actually newedges[v]).  edgelist is assumed to be sorted on u, with a
sentinel at the end (actually, this is the active edge table starting at
edge_head.next).
==============
*/
void R_InsertNewEdges (edge_t *edgestoadd, edge_t *edgelist)
{
	edge_t	*next_edge;

	do
	{
		next_edge = edgestoadd->next;
edgesearch:
		if (edgelist->u >= edgestoadd->u)
			goto addedge;
		edgelist=edgelist->next;
		if (edgelist->u >= edgestoadd->u)
			goto addedge;
		edgelist=edgelist->next;
		if (edgelist->u >= edgestoadd->u)
			goto addedge;
		edgelist=edgelist->next;
		if (edgelist->u >= edgestoadd->u)
			goto addedge;
		edgelist=edgelist->next;
		goto edgesearch;

	// insert edgestoadd before edgelist
addedge:
		edgestoadd->next = edgelist;
		edgestoadd->prev = edgelist->prev;
		edgelist->prev->next = edgestoadd;
		edgelist->prev = edgestoadd;
	} while ((edgestoadd = next_edge) != NULL);
}

/*
==============
R_RemoveEdges
==============
*/
void R_RemoveEdges (edge_t *pedge)
{

	do
	{
		pedge->next->prev = pedge->prev;
		pedge->prev->next = pedge->next;
	} while ((pedge = pedge->nextremove) != NULL);
}

/*
==============
R_StepActiveU
==============
*/
void R_StepActiveU (edge_t *pedge)
{
	edge_t		*pnext_edge, *pwedge;

	while (1)
	{
nextedge:
		pedge->u += pedge->u_step;
		if (pedge->u < pedge->prev->u)
			goto pushback;
		pedge = pedge->next;
			
		pedge->u += pedge->u_step;
		if (pedge->u < pedge->prev->u)
			goto pushback;
		pedge = pedge->next;
			
		pedge->u += pedge->u_step;
		if (pedge->u < pedge->prev->u)
			goto pushback;
		pedge = pedge->next;
			
		pedge->u += pedge->u_step;
		if (pedge->u < pedge->prev->u)
			goto pushback;
		pedge = pedge->next;
			
		goto nextedge;		
		
pushback:
		if (pedge == &edge_aftertail)
			return;
			
	// push it back to keep it sorted		
		pnext_edge = pedge->next;

	// pull the edge out of the edge list
		pedge->next->prev = pedge->prev;
		pedge->prev->next = pedge->next;

	// find out where the edge goes in the edge list
		pwedge = pedge->prev->prev;

		while (pwedge->u > pedge->u)
		{
			pwedge = pwedge->prev;
		}

	// put the edge back into the edge list
		pedge->next = pwedge->next;
		pedge->prev = pwedge;
		pedge->next->prev = pedge;
		pwedge->next = pedge;

		pedge = pnext_edge;
		if (pedge == &edge_tail)
			return;
	}
}

/*
==============
R_CleanupSpan
==============
*/
void R_CleanupSpan ()
{
	surf_t	*surf;
	int		iu;
	espan_t	*span;

// now that we've reached the right edge of the screen, we're done with any
// unfinished surfaces, so emit a span for whatever's on top
	surf = surfaces[1].next;
	iu = edge_tail_u_shift20;
	if (iu > surf->last_u)
	{
		span = span_p++;
		span->u = surf->last_u;
		span->count = iu - span->u;
		span->v = current_iv;
		span->pnext = surf->spans;
		surf->spans = span;
	}

// reset spanstate for all surfaces in the surface stack
	do
	{
		surf->spanstate = 0;
		surf = surf->next;
	} while (surf != &surfaces[1]);
}


/*
==============
R_LeadingEdgeBackwards
==============
*/
void R_LeadingEdgeBackwards (edge_t *edge)
{
	espan_t			*span;
	surf_t			*surf, *surf2;
	int				iu;

// it's adding a new surface in, so find the correct place
	surf = &surfaces[edge->surfs[1]];

// don't start a span if this is an inverted span, with the end
// edge preceding the start edge (that is, we've already seen the
// end edge)
	if (++surf->spanstate == 1)
	{
		surf2 = surfaces[1].next;

		if (surf->key > surf2->key)
			goto newtop;

	// if it's two surfaces on the same plane, the one that's already
	// active is in front, so keep going unless it's a bmodel
		if (surf->insubmodel && (surf->key == surf2->key))
		{
		// must be two bmodels in the same leaf; don't care, because they'll
		// never be farthest anyway
			goto newtop;
		}

continue_search:

		do
		{
			surf2 = surf2->next;
		} while (surf->key < surf2->key);

		if (surf->key == surf2->key)
		{
		// if it's two surfaces on the same plane, the one that's already
		// active is in front, so keep going unless it's a bmodel
			if (!surf->insubmodel)
				goto continue_search;

		// must be two bmodels in the same leaf; don't care which is really
		// in front, because they'll never be farthest anyway
		}

		goto gotposition;

newtop:
	// emit a span (obscures current top)
		iu = edge->u >> 20;

		if (iu > surf2->last_u)
		{
			span = span_p++;
			span->u = surf2->last_u;
			span->count = iu - span->u;
			span->v = current_iv;
			span->pnext = surf2->spans;
			surf2->spans = span;
		}

		// set last_u on the new span
		surf->last_u = iu;
				
gotposition:
	// insert before surf2
		surf->next = surf2;
		surf->prev = surf2->prev;
		surf2->prev->next = surf;
		surf2->prev = surf;
	}
}


/*
==============
R_TrailingEdge
==============
*/
void R_TrailingEdge (surf_t *surf, edge_t *edge)
{
	espan_t			*span;
	int				iu;

// don't generate a span if this is an inverted span, with the end
// edge preceding the start edge (that is, we haven't seen the
// start edge yet)
	if (--surf->spanstate == 0)
	{
		if (surf->insubmodel)
			r_bmodelactive--;

		if (surf == surfaces[1].next)
		{
		// emit a span (current top going away)
			iu = edge->u >> 20;
			if (iu > surf->last_u)
			{
				span = span_p++;
				span->u = surf->last_u;
				span->count = iu - span->u;
				span->v = current_iv;
				span->pnext = surf->spans;
				surf->spans = span;
			}

		// set last_u on the surface below
			surf->next->last_u = iu;
		}

		surf->prev->next = surf->next;
		surf->next->prev = surf->prev;
	}
}

/*
==============
R_LeadingEdge
==============
*/
void R_LeadingEdge (edge_t *edge)
{
	espan_t			*span;
	surf_t			*surf, *surf2;
	int				iu;
	float			fu, newzi, testzi, newzitop, newzibottom;

	if (edge->surfs[1])
	{
	// it's adding a new surface in, so find the correct place
		surf = &surfaces[edge->surfs[1]];

	// don't start a span if this is an inverted span, with the end
	// edge preceding the start edge (that is, we've already seen the
	// end edge)
		if (++surf->spanstate == 1)
		{
			if (surf->insubmodel)
				r_bmodelactive++;

			surf2 = surfaces[1].next;

			if (surf->key < surf2->key)
				goto newtop;

		// if it's two surfaces on the same plane, the one that's already
		// active is in front, so keep going unless it's a bmodel
			if (surf->insubmodel && (surf->key == surf2->key))
			{
			// must be two bmodels in the same leaf; sort on 1/z
				fu = (float)(edge->u - 0xFFFFF) * (1.0f / 0x100000);
				newzi = surf->d_ziorigin + fv*surf->d_zistepv +
						fu*surf->d_zistepu;
				newzibottom = newzi * 0.99f;

				testzi = surf2->d_ziorigin + fv*surf2->d_zistepv +
						fu*surf2->d_zistepu;

				if (newzibottom >= testzi)
				{
					goto newtop;
				}

				newzitop = newzi * 1.01f;
				if (newzitop >= testzi)
				{
					if (surf->d_zistepu >= surf2->d_zistepu)
					{
						goto newtop;
					}
				}
			}

continue_search:

			do
			{
				surf2 = surf2->next;
			} while (surf->key > surf2->key);

			if (surf->key == surf2->key)
			{
			// if it's two surfaces on the same plane, the one that's already
			// active is in front, so keep going unless it's a bmodel
				if (!surf->insubmodel)
					goto continue_search;

			// must be two bmodels in the same leaf; sort on 1/z
				fu = (float)(edge->u - 0xFFFFF) * (1.0f / 0x100000);
				newzi = surf->d_ziorigin + fv*surf->d_zistepv +
						fu*surf->d_zistepu;
				newzibottom = newzi * 0.99f;

				testzi = surf2->d_ziorigin + fv*surf2->d_zistepv +
						fu*surf2->d_zistepu;

				if (newzibottom >= testzi)
				{
					goto gotposition;
				}

				newzitop = newzi * 1.01f;
				if (newzitop >= testzi)
				{
					if (surf->d_zistepu >= surf2->d_zistepu)
					{
						goto gotposition;
					}
				}

				goto continue_search;
			}

			goto gotposition;

newtop:
		// emit a span (obscures current top)
			iu = edge->u >> 20;

			if (iu > surf2->last_u)
			{
				span = span_p++;
				span->u = surf2->last_u;
				span->count = iu - span->u;
				span->v = current_iv;
				span->pnext = surf2->spans;
				surf2->spans = span;
			}

			// set last_u on the new span
			surf->last_u = iu;
				
gotposition:
		// insert before surf2
			surf->next = surf2;
			surf->prev = surf2->prev;
			surf2->prev->next = surf;
			surf2->prev = surf;
		}
	}
}


/*
==============
R_GenerateSpans
==============
*/
void R_GenerateSpans (void)
{
	edge_t			*edge;
	surf_t			*surf;

	r_bmodelactive = 0;

// clear active surfaces to just the background surface
	surfaces[1].next = surfaces[1].prev = &surfaces[1];
	surfaces[1].last_u = edge_head_u_shift20;

// generate spans
	for (edge=edge_head.next ; edge != &edge_tail; edge=edge->next)
	{			
		if (edge->surfs[0])
		{
		// it has a left surface, so a surface is going away for this span
			surf = &surfaces[edge->surfs[0]];

			R_TrailingEdge (surf, edge);

			if (!edge->surfs[1])
				continue;
		}

		R_LeadingEdge (edge);
	}

	R_CleanupSpan ();
}

/*
==============
R_GenerateSpansBackward
==============
*/
void R_GenerateSpansBackward (void)
{
	edge_t			*edge;

	r_bmodelactive = 0;

// clear active surfaces to just the background surface
	surfaces[1].next = surfaces[1].prev = &surfaces[1];
	surfaces[1].last_u = edge_head_u_shift20;

// generate spans
	for (edge=edge_head.next ; edge != &edge_tail; edge=edge->next)
	{			
		if (edge->surfs[0])
			R_TrailingEdge (&surfaces[edge->surfs[0]], edge);

		if (edge->surfs[1])
			R_LeadingEdgeBackwards (edge);
	}

	R_CleanupSpan ();
}


/*
==============
R_ScanEdges

Input: 
newedges[] array
	this has links to edges, which have links to surfaces

Output:
Each surface has a linked list of its visible spans
==============
*/
static byte	basespans[MAXSPANS*sizeof(espan_t)+CACHE_SIZE];

#ifdef PD_FAST_EDGES
static void R_ScanEdgesList (void)
#else
void R_ScanEdges (void)
#endif
{
	int		iv, bottom;
	espan_t	*basespan_p;
	surf_t	*s;

	basespan_p = (espan_t *)
			((uintptr_t)(basespans + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));
	max_span_p = &basespan_p[MAXSPANS - r_refdef.vrect.width];

	span_p = basespan_p;

// clear active edges to just the background edges around the whole screen
// FIXME: most of this only needs to be set up once
	edge_head.u = r_refdef.vrect.x << 20;
	edge_head_u_shift20 = edge_head.u >> 20;
	edge_head.u_step = 0;
	edge_head.prev = NULL;
	edge_head.next = &edge_tail;
	edge_head.surfs[0] = 0;
	edge_head.surfs[1] = 1;
	
	edge_tail.u = (r_refdef.vrectright << 20) + 0xFFFFF;
	edge_tail_u_shift20 = edge_tail.u >> 20;
	edge_tail.u_step = 0;
	edge_tail.prev = &edge_head;
	edge_tail.next = &edge_aftertail;
	edge_tail.surfs[0] = 1;
	edge_tail.surfs[1] = 0;
	
	edge_aftertail.u = -1;		// force a move
	edge_aftertail.u_step = 0;
	edge_aftertail.next = &edge_sentinel;
	edge_aftertail.prev = &edge_tail;

// FIXME: do we need this now that we clamp x in r_draw.c?
	edge_sentinel.u = 100 << 24;		// make sure nothing sorts past this
	edge_sentinel.prev = &edge_aftertail;

//	
// process all scan lines
//
	bottom = r_refdef.vrectbottom - 1;

	for (iv=r_refdef.vrect.y ; iv<bottom ; iv++)
	{
		current_iv = iv;
		fv = (float)iv;

	// mark that the head (background start) span is pre-included
		surfaces[1].spanstate = 1;

		if (newedges[iv])
		{
			PROF_BEGINF(P_SEINS);
			R_InsertNewEdges (newedges[iv], edge_head.next);
			PROF_ENDF(P_SEINS);
		}

		PROF_BEGINF(P_SEGEN);
		(*pdrawfunc) ();
		PROF_ENDF(P_SEGEN);

	// flush the span list if we can't be sure we have enough spans left for
	// the next scan
		if (span_p >= max_span_p)
		{
			VID_UnlockBuffer ();
			S_ExtraUpdate ();	// don't let sound get messed up if going slow
			VID_LockBuffer ();
		
			if (r_drawculledpolys)
			{
				R_DrawCulledPolys ();
			}
			else
			{
				D_DrawSurfaces ();
			}

		// clear the surface span pointers
			for (s = &surfaces[1] ; s<surface_p ; s++)
				s->spans = NULL;

			span_p = basespan_p;
		}

		PROF_BEGINF(P_SEREM);
		if (removeedges[iv])
			R_RemoveEdges (removeedges[iv]);
		PROF_ENDF(P_SEREM);

		PROF_BEGINF(P_SESTEP);
		if (edge_head.next != &edge_tail)
			R_StepActiveU (edge_head.next);
		PROF_ENDF(P_SESTEP);
	}

// do the last scan (no need to step or sort or remove on the last scan)

	current_iv = iv;
	fv = (float)iv;

// mark that the head (background start) span is pre-included
	surfaces[1].spanstate = 1;

	if (newedges[iv])
		R_InsertNewEdges (newedges[iv], edge_head.next);

	(*pdrawfunc) ();

// draw whatever's left in the span list
	if (r_drawculledpolys)
		R_DrawCulledPolys ();
	else
		D_DrawSurfaces ();
}

#ifdef PD_FAST_EDGES
/*
==============================================================================

Array-based edge scan

R_ScanEdgesList keeps the active edge table and the active surface stack as
linked lists whose pointers live in edge_t and surf_t, so every edge crossing
rewrites a dozen words of heap memory. On the Playdate a store to the heap
costs ~100 ns (the data cache is write-through and the heap is slow), while the
stack is fast internal RAM. Both structures are small (a few dozen entries), so
this version keeps them as arrays on the stack and only reads the heap. The
spans it produces are identical: same insertion order, same tie-breaking, same
span lists per surface (see the differential test in tools/hostcheck).

Falls back to the list version for r_draworder (debug view).
==============================================================================
*/

#define FE_ACTIVE_STACK	96
#define FE_SURF_STACK	32

typedef struct
{
	int				u;
	int				u_step;
	unsigned short	s0, s1;		// surfs[0], surfs[1]
	edge_t			*e;
} aedge_t;

typedef struct
{
	surf_t	*s;
	int		key;
	int		last_u;
} sentry_t;

// used only when a scan line has more active edges / surfaces than the stack arrays hold;
// allocated on first use so they do not take up (and shift) static data
static aedge_t	*fe_active_heap;
static sentry_t	*fe_surf_heap;

/*
The head of each surface's span list used to live in surf_t: one isolated store to slow memory
per span (~0.7 us, a thousand a frame). While the scan runs the heads are kept in an array on the
stack instead, and D_DrawSurfaces picks them up from r_spanheads (NULL: use surf_t.spans, as the
list scan does). Only used when there are few enough surfaces for that array to be small.
*/
espan_t	**r_spanheads;
#define FE_MAXHEADS	512

#define FE_EMIT(sf, ustart, uend) \
	do { \
		espan_t	*sp_ = sp++; \
		sp_->u = (ustart); \
		sp_->count = (uend) - sp_->u; \
		sp_->v = iv; \
		if (heads_on) \
		{ \
			sp_->pnext = heads[(sf) - surfaces]; \
			heads[(sf) - surfaces] = sp_; \
		} \
		else \
		{ \
			sp_->pnext = (sf)->spans; \
			(sf)->spans = sp_; \
		} \
	} while (0)

// sort two bmodel surfaces that share a key on 1/z at the current edge
#define FE_ZTEST(surf_, surf2_, edge_u_, action_newer_front_) \
	do { \
		fu = (float)((edge_u_) - 0xFFFFF) * (1.0f / 0x100000); \
		newzi = (surf_)->d_ziorigin + fv*(surf_)->d_zistepv + fu*(surf_)->d_zistepu; \
		newzibottom = newzi * 0.99f; \
		testzi = (surf2_)->d_ziorigin + fv*(surf2_)->d_zistepv + fu*(surf2_)->d_zistepu; \
		if (newzibottom >= testzi) \
		{ \
			action_newer_front_; \
		} \
		newzitop = newzi * 1.01f; \
		if (newzitop >= testzi) \
		{ \
			if ((surf_)->d_zistepu >= (surf2_)->d_zistepu) \
			{ \
				action_newer_front_; \
			} \
		} \
	} while (0)

void R_ScanEdges (void)
{
	aedge_t		act_stack[FE_ACTIVE_STACK];
	sentry_t	stk_stack[FE_SURF_STACK];
	aedge_t		*act = act_stack;
	sentry_t	*stk = stk_stack;
	int			act_cap = FE_ACTIVE_STACK, stk_cap = FE_SURF_STACK;
	int			nact, nstk;
	int			iv, bottom, i, j, k, p, iu;
	int			head_u, tail_u;
	signed char	state[surface_p - surfaces];	// spanstate per surface, 0 = not in span, -1 = in inverted span
	int			heads_on = (surface_p - surfaces) <= FE_MAXHEADS;
	espan_t		*heads[heads_on ? surface_p - surfaces : 1];	// span list head per surface
	espan_t		*basespan_p, *sp;
	edge_t		*ne, *next_edge;
	surf_t		*s, *surf, *surf2;
	float		fv, fu, newzi, testzi, newzitop, newzibottom;

	PROF_STK(K_SCAN);
	if (r_draworder.value)
	{
		R_ScanEdgesList ();
		return;
	}

	basespan_p = (espan_t *)
			((uintptr_t)(basespans + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));
	max_span_p = &basespan_p[MAXSPANS - r_refdef.vrect.width];
	sp = basespan_p;

	memset (state, 0, sizeof(state));
	if (heads_on)
	{
		memset (heads, 0, sizeof(heads));
		r_spanheads = heads;
	}

// the active edge table: left screen edge, the active edges sorted on u, right screen edge
	head_u = r_refdef.vrect.x << 20;
	tail_u = (r_refdef.vrectright << 20) + 0xFFFFF;
	edge_head_u_shift20 = head_u >> 20;
	edge_tail_u_shift20 = tail_u >> 20;

	act[0].u = head_u;
	act[0].u_step = 0;
	act[0].s0 = 0;
	act[0].s1 = 1;
	act[0].e = &edge_head;
	act[1].u = tail_u;
	act[1].u_step = 0;
	act[1].s0 = 1;
	act[1].s1 = 0;
	act[1].e = &edge_tail;
	nact = 2;

// process all scan lines
	bottom = r_refdef.vrectbottom - 1;

	for (iv=r_refdef.vrect.y ; iv<=bottom ; iv++)
	{
		fv = (float)iv;

	// mark that the head (background start) span is pre-included
		state[1] = 1;

		PROF_BEGINF(P_SEINS);
		if (newedges[iv])
		{
		// merge the new edges (sorted on u) into the active table: each goes before
		// the first active edge with u >= its own, searching on from the last insertion
			j = 1;
			ne = newedges[iv];
			do
			{
				next_edge = ne->next;
				iu = ne->u;

				while (act[j].u < iu)
					j++;

				if (nact == act_cap)
				{
					if (act == act_stack)
					{
						if (!fe_active_heap)
							fe_active_heap = malloc ((NUMSTACKEDGES + 2) * sizeof(aedge_t));
						if (!fe_active_heap)
							Sys_Error ("R_ScanEdges: out of memory");
						memcpy (fe_active_heap, act_stack, nact * sizeof(aedge_t));
						act = fe_active_heap;
						act_cap = NUMSTACKEDGES + 2;
					}
					else
						Sys_Error ("R_ScanEdges: too many active edges");
				}
				for (k=nact ; k>j ; k--)
					act[k] = act[k-1];
				act[j].u = iu;
				act[j].u_step = ne->u_step;
				act[j].s0 = ne->surfs[0];
				act[j].s1 = ne->surfs[1];
				act[j].e = ne;
				nact++;
				j++;
			} while ((ne = next_edge) != NULL);
		}
		PROF_ENDF(P_SEINS);

		PROF_BEGINF(P_SEGEN);
	// generate spans: the active surface stack is stk[0] (nearest) ... the background
		nstk = 1;
		stk[0].s = &surfaces[1];
		stk[0].key = surfaces[1].key;
		stk[0].last_u = edge_head_u_shift20;
		r_bmodelactive = 0;

		for (i=1 ; act[i].e != &edge_tail ; i++)
		{
			aedge_t	*a = &act[i];

			if (a->s0)
			{
			// it has a left surface, so a surface is going away for this span
				surf = &surfaces[a->s0];

				if (--state[a->s0] == 0)
				{
					if (surf->insubmodel)
						r_bmodelactive--;

					if (surf == stk[0].s)
					{
					// emit a span (current top going away)
						iu = a->u >> 20;
						if (iu > stk[0].last_u)
							FE_EMIT (surf, stk[0].last_u, iu);

					// set last_u on the surface below
						stk[1].last_u = iu;
					}

					for (k=0 ; stk[k].s != surf ; k++)
						;
					nstk--;
					for ( ; k<nstk ; k++)
						stk[k] = stk[k+1];
				}

				if (!a->s1)
					continue;
			}

			if (!a->s1)
				continue;

		// it's adding a new surface in, so find the correct place
			surf = &surfaces[a->s1];

			if (++state[a->s1] != 1)
				continue;

			if (surf->insubmodel)
				r_bmodelactive++;

			{
				int		key = surf->key;

				p = 0;
				surf2 = stk[0].s;

				if (key < stk[0].key)
					goto newtop;

			// if it's two surfaces on the same plane, the one that's already
			// active is in front, so keep going unless it's a bmodel
				if (surf->insubmodel && key == stk[0].key)
				{
				// must be two bmodels in the same leaf; sort on 1/z
					FE_ZTEST (surf, surf2, a->u, goto newtop);
				}

continue_search:
				do
				{
					p++;
				} while (key > stk[p].key);

				surf2 = stk[p].s;

				if (key == stk[p].key)
				{
					if (!surf->insubmodel)
						goto continue_search;

				// must be two bmodels in the same leaf; sort on 1/z
					FE_ZTEST (surf, surf2, a->u, goto gotposition);

					goto continue_search;
				}

				goto gotposition;

newtop:
			// emit a span (obscures current top)
				iu = a->u >> 20;

				if (iu > stk[0].last_u)
					FE_EMIT (stk[0].s, stk[0].last_u, iu);

gotposition:
			// insert before stk[p]; a surface put on top starts its span here
				if (nstk == stk_cap)
				{
					if (stk == stk_stack)
					{
						if (!fe_surf_heap)
							fe_surf_heap = malloc ((NUMSTACKSURFACES + 1) * sizeof(sentry_t));
						if (!fe_surf_heap)
							Sys_Error ("R_ScanEdges: out of memory");
						memcpy (fe_surf_heap, stk_stack, nstk * sizeof(sentry_t));
						stk = fe_surf_heap;
						stk_cap = NUMSTACKSURFACES + 1;
					}
					else
						Sys_Error ("R_ScanEdges: surface stack overflow");
				}
				for (k=nstk ; k>p ; k--)
					stk[k] = stk[k-1];
				stk[p].s = surf;
				stk[p].key = key;
				stk[p].last_u = (p == 0) ? (a->u >> 20) : 0;
				nstk++;
			}
		}

	// now that we've reached the right edge of the screen, we're done with any
	// unfinished surfaces, so emit a span for whatever's on top
		iu = edge_tail_u_shift20;
		if (iu > stk[0].last_u)
			FE_EMIT (stk[0].s, stk[0].last_u, iu);

	// reset spanstate for all surfaces in the surface stack (not the background)
		for (k=0 ; k<nstk ; k++)
			state[stk[k].s - surfaces] = 0;
		PROF_ENDF(P_SEGEN);

		if (iv == bottom)
			break;

	// flush the span list if we can't be sure we have enough spans left for
	// the next scan
		if (sp >= max_span_p)
		{
			span_p = sp;

			VID_UnlockBuffer ();
			S_ExtraUpdate ();	// don't let sound get messed up if going slow
			VID_LockBuffer ();

			if (r_drawculledpolys)
			{
				if (heads_on)
					for (s = &surfaces[1] ; s<surface_p ; s++)
						s->spans = heads[s - surfaces];
				R_DrawCulledPolys ();
			}
			else
			{
				D_DrawSurfaces ();
			}

		// clear the surface span pointers
			if (heads_on)
				memset (heads, 0, sizeof(heads));
			else
				for (s = &surfaces[1] ; s<surface_p ; s++)
					s->spans = NULL;

			sp = basespan_p;
		}

		PROF_BEGINF(P_SEREM);
		if (removeedges[iv])
		{
			for (ne = removeedges[iv] ; ne ; ne = ne->nextremove)
			{
				for (k=1 ; act[k].e != ne ; k++)
					;
				nact--;
				for ( ; k<nact ; k++)
					act[k] = act[k+1];
			}
		}
		PROF_ENDF(P_SEREM);

		PROF_BEGINF(P_SESTEP);

	// step the active edges and keep them sorted: an edge that ends up left of
	// its predecessor is moved back to where it belongs
		if (nact > 2)
		{
			for (i=1 ; i<nact ; i++)
			{
				aedge_t	moved;

				act[i].u += act[i].u_step;
				if (act[i].u >= act[i-1].u)
					continue;

				moved = act[i];
				for (j=i-2 ; j >= 0 && act[j].u > moved.u ; j--)
					;
				for (k=i ; k>j+1 ; k--)
					act[k] = act[k-1];
				act[j+1] = moved;
			}
		}
		PROF_ENDF(P_SESTEP);
	}

// draw whatever's left in the span list
	span_p = sp;
	if (r_drawculledpolys)
	{
		if (heads_on)
			for (s = &surfaces[1] ; s<surface_p ; s++)
				s->spans = heads[s - surfaces];
		R_DrawCulledPolys ();
	}
	else
		D_DrawSurfaces ();
	r_spanheads = NULL;
}
#endif	// PD_FAST_EDGES
