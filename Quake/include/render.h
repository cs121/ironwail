/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

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

#ifndef _QUAKE_RENDER_H
#define _QUAKE_RENDER_H

// refresh.h -- public interface to refresh functions

#define	MAXCLIPPLANES	11

#define	TOP_RANGE		16			// soldier uniform colors
#define	BOTTOM_RANGE	96

//=============================================================================

typedef struct lightcell_s lightcell_t;
typedef lightcell_t lightgrid_probe_t;
typedef struct lightgrid_s lightgrid_t;

typedef struct lightcache_s {
	int					surfidx; // < 0: black surface; == 0: no cache; > 0: 1+index of surface
	vec3_t				pos;
	short				ds;
	short				dt;
	vec3_t				ambientcolor;
	vec3_t				dlightcolor;
	vec3_t				dlightdir;
	vec3_t				staticlightdir;
	vec3_t				lightgrid_color;
	float				lightgrid_ao;
	qboolean		lightgrid_has_sample;
	vec3_t				static_color_smoothed;
	int					static_color_smoothed_frame;
	qboolean			static_color_smoothed_valid;
	qboolean			static_color_smooth_reset;
} lightcache_t;

typedef enum entity_static_light_source_e {
	ENTITY_STATIC_LIGHT_NONE = 0,
	ENTITY_STATIC_LIGHT_GRID,
	ENTITY_STATIC_LIGHT_POINT,
	ENTITY_STATIC_LIGHT_MINLIGHT,
	ENTITY_STATIC_LIGHT_MIXED
} entity_static_light_source_t;

typedef struct entity_lightinfo_s {
	vec3_t				static_color;
	vec3_t				static_target_color;
	vec3_t				dynamic_color;
	vec3_t				final_color;
	float				intensity;
	qboolean			used_lightgrid;
	qboolean			lightgrid_valid;
	qboolean			lightgrid_cell_valid;
	int					lightgrid_cell[3];
	vec3_t				lightgrid_color;
	float				lightgrid_ao;
	qboolean			used_lightpoint;
	vec3_t				lightpoint_color;
	qboolean			used_minlight;
	qboolean			used_multisample;
	int					sample_count;
	entity_static_light_source_t	static_source;
} entity_lightinfo_t;

//johnfitz -- for lerping
#define LERP_MOVESTEP	(1<<0) //this is a MOVETYPE_STEP entity, enable movement lerp
#define LERP_RESETANIM	(1<<1) //disable anim lerping until next anim frame
#define LERP_RESETANIM2	(1<<2) //set this and previous flag to disable anim lerping for two anim frames
#define LERP_RESETMOVE	(1<<3) //disable movement lerping until next origin/angles change
#define LERP_FINISH		(1<<4) //use lerpfinish time from server update instead of assuming interval of 0.1
//johnfitz

typedef struct entity_s
{
	qboolean				forcelink;		// model changed

	int						update_type;

	entity_state_t			baseline;		// to fill in defaults in updates

	double					msgtime;		// time of last update
	vec3_t					msg_origins[2];	// last two updates (0 is newest)
	vec3_t					origin;
	vec3_t					msg_angles[2];	// last two updates (0 is newest)
	vec3_t					angles;
	struct qmodel_s			*model;			// NULL = no model
	int						frame;
	float					syncbase;		// for client-side animations
	byte					*colormap;
	int						effects;		// light, particles, etc
	int						skinnum;		// for Alias models

	int						firstleaf;		// for sorting static entities

// FIXME: could turn these into a union

	byte					alpha;			//johnfitz -- alpha
	byte					scale;
	byte					lerpflags;		//johnfitz -- lerping
	float					lerpstart;		//johnfitz -- animation lerping
	float					lerptime;		//johnfitz -- animation lerping
	float					lerpfinish;		//johnfitz -- lerping -- server sent us a more accurate interval, use it instead of 0.1
	short					previouspose;	//johnfitz -- animation lerping
	short					currentpose;	//johnfitz -- animation lerping
//	short					futurepose;		//johnfitz -- animation lerping
	float					movelerpstart;	//johnfitz -- transform lerping
	vec3_t					previousorigin;	//johnfitz -- transform lerping
	vec3_t					currentorigin;	//johnfitz -- transform lerping
	vec3_t					previousangles;	//johnfitz -- transform lerping
	vec3_t					currentangles;	//johnfitz -- transform lerping

	// Cached transform from the previous rendered frame for motion blur.
	vec3_t					motion_blur_prev_origin;
	vec3_t					motion_blur_prev_angles;
	int					motion_blur_prev_frame;
	qboolean			motion_blur_prev_valid;

	lightcache_t			lightcache;		// alias light trace cache

	float					traildelay;		// time left until next particle trail update
	vec3_t					trailorg;		// previous particle trail point
} entity_t;

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct refdef_s
{
	vrect_t		vrect;				// subwindow in video for refresh
									// FIXME: not need vrect next field here?
	vec3_t		vieworg;
	vec3_t		viewangles;

	float		basefov;
	float		fov_x, fov_y;
	int			scale;
} refdef_t;


//
// refresh
//

extern	refdef_t	r_refdef;
extern vec3_t	r_origin, vpn, vright, vup;


void R_Init (void);
void R_RenderView (void);		// must set r_refdef first
void R_ClearEfrags (void);
void R_CheckEfrags (void); //johnfitz
void R_AddEfrags (entity_t *ent);
void R_AddStaticModels (const byte *vis);

void R_NewMap (void);
void R_ParseDlightEntities (void);


void R_ParseParticleEffect (void);
void R_RunParticleEffect (vec3_t org, vec3_t dir, int color, int count);
void R_RocketTrail (vec3_t start, vec3_t end, int type);
void R_EntityParticles (entity_t *ent);
void R_BlobExplosion (vec3_t org);
void R_ParticleExplosion (vec3_t org);
void R_ParticleExplosion2 (vec3_t org, int colorStart, int colorLength);
void R_LavaSplash (vec3_t org);
void R_TeleportSplash (vec3_t org);
const lightgrid_probe_t *R_GetLightgridSample (const vec3_t pos);


void R_InitDecals (void);
void R_ClearDecals (void);
void R_ReloadDecals (void);
void R_UpdateDecals (void);
void R_DrawDecals (void);
void R_Decals_RegisterFrameGraphPasses (void);
void R_RegisterFrameGraphPasses (void);
void R_SpawnImpactDecalEx (const char *category, const vec3_t origin, const vec3_t normal, const vec3_t hit_dir, qboolean heavy_blood);
void R_SpawnImpactDecal (const char *category, const vec3_t origin, const vec3_t normal);

void R_PushDlights (void);

#endif	/* _QUAKE_RENDER_H */
