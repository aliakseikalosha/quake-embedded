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
// r_light.c

#include "quakedef.h"
#include "r_local.h"
#include "pdprof.h"

int	r_dlightframecount;


/*
==================
R_AnimateLight
==================
*/
void R_AnimateLight (void)
{
	int			i,j,k;
	
//
// light animations
// 'm' is normal light, 'a' is no light, 'z' is double bright
	i = (int)(cl.time*10);
	for (j=0 ; j<MAX_LIGHTSTYLES ; j++)
	{
		if (!cl_lightstyle[j].length)
		{
			d_lightstylevalue[j] = 256;
			continue;
		}
		k = i % cl_lightstyle[j].length;
		k = cl_lightstyle[j].map[k] - 'a';
		k = k*22;
		d_lightstylevalue[j] = k;
	}	
}


/*
=============================================================================

DYNAMIC LIGHTS

=============================================================================
*/

/*
=============
R_MarkLights
=============
*/
void R_MarkLights (dlight_t *light, int bit, mnode_t *node)
{
	mplane_t	*splitplane;
	float		dist;
	msurface_t	*surf;
	int			i;
	
start:
	if (node->contents < 0)
		return;

	splitplane = node->plane;
	dist = DotProduct (light->origin, splitplane->normal) - splitplane->dist;
	
	if (dist > light->radius)
	{
		node = node->children[0];
		goto start;
	}
	if (dist < -light->radius)
	{
		node = node->children[1];
		goto start;
	}
		
// mark the polygons
	surf = cl.worldmodel->surfaces + node->firstsurface;
	for (i=0 ; i<node->numsurfaces ; i++, surf++)
	{
		if (surf->dlightframe != r_dlightframecount)
		{
			surf->dlightbits = 0;
			surf->dlightframe = r_dlightframecount;
		}
		surf->dlightbits |= bit;
	}

	R_MarkLights (light, bit, node->children[0]);
	R_MarkLights (light, bit, node->children[1]);
}


/*
=============
R_PushDlights
=============
*/
void R_PushDlights (void)
{
	int		i;
	dlight_t	*l;

	r_dlightframecount = r_framecount + 1;	// because the count hasn't
											//  advanced yet for this frame
	l = cl_dlights;

	for (i=0 ; i<MAX_DLIGHTS ; i++, l++)
	{
		if (l->die < cl.time || !l->radius)
			continue;
		R_MarkLights ( l, 1<<i, cl.worldmodel->nodes );
	}
}


/*
=============================================================================

LIGHT SAMPLING

=============================================================================
*/

#ifdef PD_FAST_ALIAS
/*
R_LightPoint walks the world BSP from the top down to the entity's feet: ~30 nodes and their
planes, then the surfaces of the node the ray crosses, each a few cache lines that are cold by
the time an entity is drawn (about 200 us per call on the Playdate). What that walk finds -- the
surface and the lightmap texel -- depends only on the point, so it is remembered per exact point;
the light itself (which depends on the light styles) is recomputed from it every time.
*/
typedef struct
{
	const msurface_t	*surf;		// NULL = the ray hit nothing
	int					ds, dt;
} lighthit_t;

static int LightFromHit (const lighthit_t *hit)
{
	const msurface_t *surf = hit->surf;
	const byte *lightmap;
	int maps, lightlevel, ds, dt;

	if (!surf)
		return -1;
	if (!surf->samples)
		return 0;

	ds = hit->ds >> 4;
	dt = hit->dt >> 4;

	/* FIXME: does this account properly for dynamic lights? e.g. rocket */
	lightlevel = 0;
	lightmap = surf->samples + dt * ((surf->extents[0] >> 4) + 1) + ds;
	foreach_surf_lightstyle(surf, maps) {
		const short *size = surf->extents;
		const int surfbytes = ((size[0] >> 4) + 1) * ((size[1] >> 4) + 1);

		lightlevel += *lightmap * d_lightstylevalue[surf->styles[maps]];
		lightmap += surfbytes;
	}

	return lightlevel >> 8;
}

// returns true if something was hit
static qboolean RecursiveLightHit (mnode_t *node, vec3_t start, vec3_t end, lighthit_t *hit)
{
	const mplane_t *plane;
	float		front, back, frac;
	vec3_t		mid;
	int side;

	const msurface_t *surf;
	const mtexinfo_t *tex;
	int			i;

restart:
	if (node->contents < 0)
		return false;		// didn't hit anything

// calculate mid point

// FIXME: optimize for axial
	plane = node->plane;
	front = DotProduct (start, plane->normal) - plane->dist;
	back = DotProduct (end, plane->normal) - plane->dist;
	side = front < 0;

	if ( (back < 0) == side) {
		/* Completely on one side - tail recursion optimization */
		node = node->children[side];
		goto restart;
	}

	frac = front / (front-back);
	mid[0] = start[0] + (end[0] - start[0])*frac;
	mid[1] = start[1] + (end[1] - start[1])*frac;
	mid[2] = start[2] + (end[2] - start[2])*frac;

// go down front side
	if (RecursiveLightHit(node->children[side], start, mid, hit))
		return true;		/* hit something */

	if ( (back < 0) == side )
		return false;		// didn't hit anuthing

// check for impact on this node

	surf = cl.worldmodel->surfaces + node->firstsurface;
	for (i=0 ; i<node->numsurfaces ; i++, surf++)
	{
		int s, t, ds, dt;

		if (surf->flags & SURF_DRAWTILED)
			continue;	// no lightmaps

		tex = surf->texinfo;

		s = DotProduct (mid, tex->vecs[0]) + tex->vecs[0][3];
		t = DotProduct (mid, tex->vecs[1]) + tex->vecs[1][3];;

		if (s < surf->texturemins[0] ||
		t < surf->texturemins[1])
			continue;

		ds = s - surf->texturemins[0];
		dt = t - surf->texturemins[1];

		if ( ds > surf->extents[0] || dt > surf->extents[1] )
			continue;

		hit->surf = surf;
		hit->ds = ds;
		hit->dt = dt;
		return true;
	}

// go down back side
	return RecursiveLightHit (node->children[!side], mid, end, hit);
}

#define LPCACHE	16
static struct
{
	float		org[3];
	lighthit_t	hit;
	qboolean	valid;
} lpcache[LPCACHE];
static int lpcache_next;

void R_LightPointFlush (void)
{
	int		i;

	for (i=0 ; i<LPCACHE ; i++)
		lpcache[i].valid = false;
}

int R_LightPoint (vec3_t p)
{
	vec3_t		end;
	lighthit_t	hit;
	int lightlevel, i;

	if (!cl.worldmodel->lightdata)
		return 255;

	PROF_BEGINF(P_LPT);
	PROF_CNTF(C_LPQ, 1);
	for (i=0 ; i<LPCACHE ; i++)
	{
		if (lpcache[i].valid && lpcache[i].org[0] == p[0] && lpcache[i].org[1] == p[1] &&
				lpcache[i].org[2] == p[2])
			break;
	}

	if (i < LPCACHE)
	{
		PROF_CNTF(C_LPHIT, 1);
		hit = lpcache[i].hit;
	}
	else
	{
		end[0] = p[0];
		end[1] = p[1];
		end[2] = p[2] - 2048;

		hit.surf = NULL;
		RecursiveLightHit (cl.worldmodel->nodes, p, end, &hit);

		i = lpcache_next;
		lpcache_next = (lpcache_next + 1) % LPCACHE;
		lpcache[i].org[0] = p[0];
		lpcache[i].org[1] = p[1];
		lpcache[i].org[2] = p[2];
		lpcache[i].hit = hit;
		lpcache[i].valid = true;
	}

	lightlevel = LightFromHit (&hit);
	PROF_ENDF(P_LPT);

	if (lightlevel == -1)
		lightlevel = 0;

	if (lightlevel < r_refdef.ambientlight)
		lightlevel = r_refdef.ambientlight;

	return lightlevel;
}

#else	// !PD_FAST_ALIAS

int RecursiveLightPoint (mnode_t *node, vec3_t start, vec3_t end)
{
	const mplane_t *plane;
	float		front, back, frac;
	vec3_t		mid;
	int side;

	const msurface_t *surf;
	const mtexinfo_t *tex;
	const byte *lightmap;
	int maps, lightlevel;
	int			i;

	PROF_STK(K_LIGHT);
restart:
	if (node->contents < 0)
		return -1;		// didn't hit anything
	
// calculate mid point

// FIXME: optimize for axial
	plane = node->plane;
	front = DotProduct (start, plane->normal) - plane->dist;
	back = DotProduct (end, plane->normal) - plane->dist;
	side = front < 0;
	
	if ( (back < 0) == side) {
		/* Completely on one side - tail recursion optimization */
		node = node->children[side];
		goto restart;
	}
	
	frac = front / (front-back);
	mid[0] = start[0] + (end[0] - start[0])*frac;
	mid[1] = start[1] + (end[1] - start[1])*frac;
	mid[2] = start[2] + (end[2] - start[2])*frac;
	
// go down front side	
	lightlevel = RecursiveLightPoint(node->children[side], start, mid);
	if (lightlevel >= 0)
		return lightlevel; /* hit something */
		
	if ( (back < 0) == side )
		return -1;		// didn't hit anuthing
		
// check for impact on this node

	surf = cl.worldmodel->surfaces + node->firstsurface;
	for (i=0 ; i<node->numsurfaces ; i++, surf++)
	{
		int s, t, ds, dt;

		if (surf->flags & SURF_DRAWTILED)
			continue;	// no lightmaps

		tex = surf->texinfo;
		
		s = DotProduct (mid, tex->vecs[0]) + tex->vecs[0][3];
		t = DotProduct (mid, tex->vecs[1]) + tex->vecs[1][3];;

		if (s < surf->texturemins[0] ||
		t < surf->texturemins[1])
			continue;
		
		ds = s - surf->texturemins[0];
		dt = t - surf->texturemins[1];
		
		if ( ds > surf->extents[0] || dt > surf->extents[1] )
			continue;

		if (!surf->samples)
			return 0;

		ds >>= 4;
		dt >>= 4;

		/* FIXME: does this account properly for dynamic lights? e.g. rocket */
		lightlevel = 0;
		lightmap = surf->samples + dt * ((surf->extents[0] >> 4) + 1) + ds;
		foreach_surf_lightstyle(surf, maps) {
			const short *size = surf->extents;
			const int surfbytes = ((size[0] >> 4) + 1) * ((size[1] >> 4) + 1);

			lightlevel += *lightmap * d_lightstylevalue[surf->styles[maps]];
			lightmap += surfbytes;
		}
		
		return lightlevel >> 8;
	}

// go down back side
	return RecursiveLightPoint (node->children[!side], mid, end);
}

int R_LightPoint (vec3_t p)
{
	vec3_t		end;
	int lightlevel;
	
	if (!cl.worldmodel->lightdata)
		return 255;
	
	end[0] = p[0];
	end[1] = p[1];
	end[2] = p[2] - 2048;
	
	PROF_BEGINF(P_LPT);
	lightlevel = RecursiveLightPoint(cl.worldmodel->nodes, p, end);
	PROF_ENDF(P_LPT);

	if (lightlevel == -1)
		lightlevel = 0;

	if (lightlevel < r_refdef.ambientlight)
		lightlevel = r_refdef.ambientlight;

	return lightlevel;
}

#endif	// PD_FAST_ALIAS
