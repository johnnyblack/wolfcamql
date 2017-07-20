// Copyright (C) 1999-2000 Id Software, Inc.
//
// cg_marks.c -- wall marks

#include "cg_local.h"

#include "cg_localents.h"
#include "cg_main.h"
#include "cg_marks.h"
#include "cg_predict.h"
#include "cg_syscalls.h"

/*
===================================================================

MARK POLYS

===================================================================
*/


static markPoly_t	cg_activeMarkPolys;			// double linked list
static markPoly_t	*cg_freeMarkPolys;			// single linked list
static markPoly_t	cg_markPolys[MAX_MARK_POLYS];
static		int	markTotal;

/*
===================
CG_InitMarkPolys

This is called at startup and for tournement restarts
===================
*/
void	CG_InitMarkPolys( void ) {
	int		i;

	memset( cg_markPolys, 0, sizeof(cg_markPolys) );

	//Com_Printf("^6mark polys: %f MB\n", (float)sizeof(cg_markPolys) / 1024.0 / 1024.0);

	cg_activeMarkPolys.nextMark = &cg_activeMarkPolys;
	cg_activeMarkPolys.prevMark = &cg_activeMarkPolys;
	cg_freeMarkPolys = cg_markPolys;
	for ( i = 0 ; i < MAX_MARK_POLYS - 1 ; i++ ) {
		cg_markPolys[i].nextMark = &cg_markPolys[i+1];
	}
}


/*
==================
CG_FreeMarkPoly
==================
*/
static void CG_FreeMarkPoly( markPoly_t *le ) {
	if ( !le->prevMark  ||  !le->nextMark ) {
		CG_Error( "CG_FreeMarkPoly: not active" );
	}

	// remove from the doubly linked active list
	le->prevMark->nextMark = le->nextMark;
	le->nextMark->prevMark = le->prevMark;

	// the free list is only singly linked
	le->nextMark = cg_freeMarkPolys;
	cg_freeMarkPolys = le;
}

/*
===================
CG_AllocMark

Will allways succeed, even if it requires freeing an old active mark
===================
*/
static markPoly_t *CG_AllocMark( void ) {
	markPoly_t	*le;
	int time;

	if ( !cg_freeMarkPolys ) {
		// no free entities, so free the one at the end of the chain
		// remove the oldest active entity
		time = cg_activeMarkPolys.prevMark->time;
		while (cg_activeMarkPolys.prevMark && time == cg_activeMarkPolys.prevMark->time) {
			CG_FreeMarkPoly( cg_activeMarkPolys.prevMark );
		}
	}

	le = cg_freeMarkPolys;
	cg_freeMarkPolys = cg_freeMarkPolys->nextMark;

	memset( le, 0, sizeof( *le ) );

	// link into the active list
	le->nextMark = cg_activeMarkPolys.nextMark;
	le->prevMark = &cg_activeMarkPolys;
	cg_activeMarkPolys.nextMark->prevMark = le;
	cg_activeMarkPolys.nextMark = le;
	return le;
}



/*
=================
CG_ImpactMark

origin should be a point within a unit of the plane
dir should be the plane normal

temporary marks will not be stored or randomly oriented, but immediately
passed to the renderer.
=================
*/
#define	MAX_MARK_FRAGMENTS	128
#define	MAX_MARK_POINTS		384

void CG_ImpactMark( qhandle_t markShader, const vec3_t origin, const vec3_t dir, 
				   float orientation, float red, float green, float blue, float alpha,
					qboolean alphaFade, float radius, qboolean temporary, qboolean energy, qboolean debug ) {
	vec3_t			axis[3];
	float			texCoordScale;
	vec3_t			originalPoints[4];
	byte			colors[4];
	int				i, j;
	int				numFragments;
	markFragment_t	markFragments[MAX_MARK_FRAGMENTS], *mf;
	vec3_t			markPoints[MAX_MARK_POINTS];
	vec3_t			projection;

	if (debug) {
		VectorCopy(origin, cg.lastImpactOrigin);
	}

	if (cg_debugImpactOrigin.integer  &&  debug) {
		Com_Printf("mark origin: %f %f %f\n", origin[0], origin[1], origin[2]);
	}

	if ( !cg_marks.integer ) {
		return;
	}

	if ( radius <= 0 ) {
		CG_Error( "CG_ImpactMark called with <= 0 radius" );
	}

	//if ( markTotal >= MAX_MARK_POLYS ) {
	//	return;
	//}

	// create the texture axis
	VectorNormalize2( dir, axis[0] );
	PerpendicularVector( axis[1], axis[0] );
	RotatePointAroundVector( axis[2], axis[0], axis[1], orientation );
	CrossProduct( axis[0], axis[2], axis[1] );

	texCoordScale = 0.5 * 1.0 / radius;

	// create the full polygon
	for ( i = 0 ; i < 3 ; i++ ) {
		originalPoints[0][i] = origin[i] - radius * axis[1][i] - radius * axis[2][i];
		originalPoints[1][i] = origin[i] + radius * axis[1][i] - radius * axis[2][i];
		originalPoints[2][i] = origin[i] + radius * axis[1][i] + radius * axis[2][i];
		originalPoints[3][i] = origin[i] - radius * axis[1][i] + radius * axis[2][i];
	}

	// get the fragments
	VectorScale( dir, -20, projection );
	numFragments = trap_CM_MarkFragments( 4, (void *)originalPoints,
					projection, MAX_MARK_POINTS, markPoints[0],
					MAX_MARK_FRAGMENTS, markFragments );

	colors[0] = red * 255;
	colors[1] = green * 255;
	colors[2] = blue * 255;
	colors[3] = alpha * 255;

	for ( i = 0, mf = markFragments ; i < numFragments ; i++, mf++ ) {
		polyVert_t	*v;
		polyVert_t	verts[MAX_VERTS_ON_POLY];
		markPoly_t	*mark;

		// we have an upper limit on the complexity of polygons
		// that we store persistantly
		if ( mf->numPoints > MAX_VERTS_ON_POLY ) {
			mf->numPoints = MAX_VERTS_ON_POLY;
		}
		for ( j = 0, v = verts ; j < mf->numPoints ; j++, v++ ) {
			vec3_t		delta;

			VectorCopy( markPoints[mf->firstPoint + j], v->xyz );

			VectorSubtract( v->xyz, origin, delta );
			v->st[0] = 0.5 + DotProduct( delta, axis[1] ) * texCoordScale;
			v->st[1] = 0.5 + DotProduct( delta, axis[2] ) * texCoordScale;
			*(int *)v->modulate = *(int *)colors;
		}

		// if it is a temporary (shadow) mark, add it immediately and forget about it
		if ( temporary ) {
			trap_R_AddPolyToScene( markShader, mf->numPoints, verts, qfalse );
			continue;
		}

		// otherwise save it persistantly
		mark = CG_AllocMark();
		mark->time = cg.time;
		mark->alphaFade = alphaFade;
		mark->markShader = markShader;
		mark->energy = energy;
		mark->poly.numVerts = mf->numPoints;
		mark->color[0] = red;
		mark->color[1] = green;
		mark->color[2] = blue;
		mark->color[3] = alpha;
		memcpy( mark->verts, verts, mf->numPoints * sizeof( verts[0] ) );
		markTotal++;
	}
}


/*
===============
CG_AddMarks
===============
*/
//#define	MARK_TOTAL_TIME		10000
//#define	MARK_FADE_TIME		1000

void CG_AddMarks( void ) {
	int			j;
	markPoly_t	*mp, *next;
	int			t;
	int			fade;
	int MARK_TOTAL_TIME;
	int MARK_FADE_TIME;

	if ( !cg_marks.integer ) {
		return;
	}

	MARK_TOTAL_TIME = cg_markTime.integer;
	MARK_FADE_TIME = cg_markFadeTime.integer;

	mp = cg_activeMarkPolys.nextMark;
	for ( ; mp != &cg_activeMarkPolys ; mp = next ) {
		// grab next now, so if the local entity is freed we
		// still have it
		next = mp->nextMark;

		// see if it is time to completely remove it
		if ( cg.time > mp->time + MARK_TOTAL_TIME ) {
			CG_FreeMarkPoly( mp );
			continue;
		}

		// fade out the energy bursts
		//if ( mp->markShader == cgs.media.energyMarkShader ) {
		if (mp->energy) {

			fade = 450 - 450 * ( (cg.time - mp->time ) / 3000.0 );
			//Com_Printf("energy fade: %d\n", fade);
			if ( fade < 255 ) {
				if ( fade < 0 ) {
					fade = 0;
				}
				if ( mp->verts[0].modulate[0] != 0 ) {
					for ( j = 0 ; j < mp->poly.numVerts ; j++ ) {
						mp->verts[j].modulate[0] = mp->color[0] * fade;
						mp->verts[j].modulate[1] = mp->color[1] * fade;
						mp->verts[j].modulate[2] = mp->color[2] * fade;
					}
				}
			}
		}

		// fade all marks out with time
		t = mp->time + MARK_TOTAL_TIME - cg.time;
		if ( t < MARK_FADE_TIME ) {
			fade = 255 * t / MARK_FADE_TIME;
			//Com_Printf("mark fade: %d  alphafade:%d\n", fade, mp->alphaFade);
			if ( mp->alphaFade ) {
				for ( j = 0 ; j < mp->poly.numVerts ; j++ ) {
					mp->verts[j].modulate[3] = fade;
				}
			} else {
				for ( j = 0 ; j < mp->poly.numVerts ; j++ ) {
					if (mp->energy) {
						mp->verts[j].modulate[0] *= fade;
						mp->verts[j].modulate[1] *= fade;
						mp->verts[j].modulate[2] *= fade;
					} else {
						mp->verts[j].modulate[0] = mp->color[0] * fade;
						mp->verts[j].modulate[1] = mp->color[1] * fade;
						mp->verts[j].modulate[2] = mp->color[2] * fade;
					}
				}
			}
		}

		trap_R_AddPolyToScene( mp->markShader, mp->poly.numVerts, mp->verts, qfalse );
	}
}

#if 0   // testing
static float   frand(void)
{
	return (rand()&32767)* (1.0/32767);
}

static float   crand(void)
{
	return (rand()&32767)* (2.0/32767) - 1;
}


void CG_Q2RailTrail (const vec3_t start, const vec3_t end)
{
	vec3_t          move;
	vec3_t          vec;
	float           len;
	int                     j;
	cparticle_t     *p;
	float           dec;
	vec3_t          right, up;
	int                     i;
	float           d, c, s;
	vec3_t          dir;
	byte            clr = 0x74;

	VectorCopy (start, move);
	VectorSubtract (end, start, vec);
	len = VectorNormalize (vec);

	MakeNormalVectors (vec, right, up);

	//goto core;

	for (i=0 ; i<len ; i++)
        {
			if (!free_particles)
				return;

			p = free_particles;
			free_particles = p->next;
			p->next = active_particles;
			active_particles = p;

			p->time = cg.time;
			VectorClear (p->accel);
			p->pshader = trap_R_RegisterShader("gfx/misc/tracer");
			p->type = P_SPRITE;
			p->width = 1;
			p->height = 1;
			p->endwidth = 1;
			p->endheight = 1;

			d = i * 0.1;
			c = cos(d);
			s = sin(d);

			VectorScale (right, c, dir);
			VectorMA (dir, s, up, dir);

			p->alpha = 1.0;
			p->alphavel = -1.0 / (1+frand()*0.2);
			p->color = clr + (rand()&7);
			for (j=0 ; j<3 ; j++)
                {
					p->org[j] = move[j] + dir[j]*3;
					p->vel[j] = dir[j]*6;
                }

			VectorAdd (move, vec, move);
        }

//core:

	dec = 0.75;
	VectorScale (vec, dec, vec);
	VectorCopy (start, move);

	while (len > 0)
        {
			len -= dec;

			if (!free_particles)
				return;
			p = free_particles;
			free_particles = p->next;
			p->next = active_particles;
			active_particles = p;

			p->time = cg.time;
			VectorClear (p->accel);
			//p->pshader = trap_R_RegisterShader("gfx/misc/tracer");
			p->pshader = cgs.media.railRingsShader;
			p->type = P_SPRITE;
			p->width = 1;
			p->height = 1;
			p->endwidth = 1;
			p->endheight = 1;

			p->alpha = 1.0;
			p->alphavel = -1.0 / (0.6+frand()*0.2);
			p->color = 0x0 + (rand()&15);

			for (j=0 ; j<3 ; j++)
                {
					p->org[j] = move[j] + crand()*3;
					p->vel[j] = crand()*3;
					p->accel[j] = 0;
                }

			VectorAdd (move, vec, move);
        }
}
#endif
