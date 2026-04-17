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
// r_light.c

#include "quakedef.h"
#include "glquake.h"
#include "../common/lightgrid.h"
#include "gl_lightgrid.h"
#include "r_dlight_pool.h"
#include "r_realtimelight.h"
#include <float.h>
#include <math.h>

#ifdef RENDERER_PLUGIN_BUILD
#define IW_PARSE_SSCANF sscanf_s
#else
#define IW_PARSE_SSCANF q_sscanf
#endif

extern cvar_t r_flatlightstyles; //johnfitz
extern cvar_t r_lerplightstyles;
extern cvar_t r_dynamic;
extern cvar_t r_lightgrid;
extern cvar_t r_lightgrid_force;
extern cvar_t r_rgblighting_enable;

cvar_t r_debug_itemlight = { "r_debug_itemlight", "0", CVAR_NONE };
cvar_t r_minlight_models = { "r_minlight_models", "0.02", CVAR_ARCHIVE };
cvar_t r_model_lightgrid = { "r_model_lightgrid", "1", CVAR_ARCHIVE };
cvar_t r_model_light_multisample = { "r_model_light_multisample", "0", CVAR_ARCHIVE };
cvar_t r_model_light_smooth = { "r_model_light_smooth", "0", CVAR_ARCHIVE };
cvar_t r_model_lightgrid_assist = { "r_model_lightgrid_assist", "0", CVAR_ARCHIVE };
cvar_t r_model_lightgrid_assist_threshold = { "r_model_lightgrid_assist_threshold", "0.03", CVAR_ARCHIVE };
cvar_t r_bmodel_relight = { "r_bmodel_relight", "0", CVAR_ARCHIVE };
cvar_t r_model_light_stats = { "r_model_light_stats", "0", CVAR_NONE };
cvar_t r_model_light_samples_max = { "r_model_light_samples_max", "1024", CVAR_ARCHIVE };

gpulightbuffer_t r_lightbuffer;
float r_lightstyle_framefrac;
dlight_t *r_dlight_sources[DLIGHT_GPU_MAX];

typedef struct model_light_stats_s {
	int frame;
	int entities;
	int multisample_entities;
	int budget_fallback_entities;
	int total_samples;
	double total_ms;
} model_light_stats_t;

static model_light_stats_t r_model_light_frame_stats = {
	-1, 0, 0, 0, 0, 0.0
};

const vec3_t *R_GetDynamicLightTemperature (int type);

static qboolean R_IsFinite (float v)
{
#if defined(_MSC_VER)
        return _finite(v) != 0;
#else
        return isfinite(v);
#endif
}

static float R_ModelLightLuma (const vec3_t color)
{
	return color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
}

void R_EvaluateDLightForRender (const dlight_t *l, float *out_radius, vec3_t out_color)
{
	float radius;
	float radius_scale = 1.f;
	float energy_scale = 1.f;
	const vec3_t *temp;

	if (!l)
	{
		if (out_radius)
			*out_radius = 0.f;
		VectorClear (out_color);
		return;
	}

	if (CL_DlightShouldFlickerRadius (l))
		radius = l->baseradius * (1.f + 0.1f * (float)sin (cl.time * 9.0 + l->flicker_seed));
	else
		radius = l->baseradius;

	switch (l->type)
	{
	case DLIGHT_DEFAULT:
		radius_scale = 1.55f;
		energy_scale = 1.70f;
		break;
	case DLIGHT_LAVA:
		radius_scale = 1.45f;
		energy_scale = 1.45f;
		break;
	case DLIGHT_TORCH:
		radius_scale = 1.25f;
		energy_scale = 1.20f;
		break;
	case DLIGHT_TELEPORT:
		radius_scale = 1.30f;
		energy_scale = 1.25f;
		break;
	case DLIGHT_LIGHTNING:
		radius_scale = 1.20f;
		energy_scale = 1.15f;
		break;
	default:
		break;
	}

	radius = q_max (radius * radius_scale, 0.f);
	temp = R_GetDynamicLightTemperature (l->type);
	out_color[0] = l->color[0] * (*temp)[0] * energy_scale;
	out_color[1] = l->color[1] * (*temp)[1] * energy_scale;
	out_color[2] = l->color[2] * (*temp)[2] * energy_scale;

	if (CL_DlightShouldFlickerColor (l))
	{
		float flicker = 1.f + (float)sin (cl.time * 15.0 + l->key) * 0.1f;
		out_color[0] *= flicker;
		out_color[1] *= flicker;
		out_color[2] *= flicker;
	}
	if (l->type == DLIGHT_TORCH)
	{
		float colorshift = (float)sin (cl.time * 11.0 + l->key) * 0.1f;
		out_color[0] *= 1.0f + colorshift;
		out_color[1] *= 1.0f - colorshift;
	}

	if (out_radius)
		*out_radius = radius;
}

static void R_ModelLightStats_NewFrame (void)
{
	const int frame = r_framecount;

	if (r_model_light_frame_stats.frame == frame)
		return;

	if (r_model_light_frame_stats.frame >= 0 && r_model_light_stats.value > 0.f)
	{
		const int interval = r_model_light_stats.value >= 2.f ? 1 : 30;
		if ((r_model_light_frame_stats.frame % interval) == 0)
		{
			Con_Printf ("r_model_light_stats: frame=%d entities=%d multisample=%d budget_fallback=%d samples=%d cpu_ms=%.3f\n",
				r_model_light_frame_stats.frame,
				r_model_light_frame_stats.entities,
				r_model_light_frame_stats.multisample_entities,
				r_model_light_frame_stats.budget_fallback_entities,
				r_model_light_frame_stats.total_samples,
				r_model_light_frame_stats.total_ms);
		}
	}

	r_model_light_frame_stats.frame = frame;
	r_model_light_frame_stats.entities = 0;
	r_model_light_frame_stats.multisample_entities = 0;
	r_model_light_frame_stats.budget_fallback_entities = 0;
	r_model_light_frame_stats.total_samples = 0;
	r_model_light_frame_stats.total_ms = 0.0;
}

static qboolean R_ModelLightWouldExceedBudget (int requested_samples)
{
	const int max_samples = (int)r_model_light_samples_max.value;

	if (requested_samples <= 0 || max_samples <= 0)
		return false;

	return (r_model_light_frame_stats.total_samples + requested_samples) > max_samples;
}

static void R_ModelLightStats_AddCall (int sample_count, qboolean used_multisample, qboolean budget_fallback, double elapsed_ms)
{
	R_ModelLightStats_NewFrame ();
	r_model_light_frame_stats.entities++;
	if (used_multisample)
		r_model_light_frame_stats.multisample_entities++;
	if (budget_fallback)
		r_model_light_frame_stats.budget_fallback_entities++;
	if (sample_count > 0)
		r_model_light_frame_stats.total_samples += sample_count;
	if (elapsed_ms > 0.0)
		r_model_light_frame_stats.total_ms += elapsed_ms;
}

static int R_AccumulateEntityPPFrameLights (const vec3_t pos, vec3_t out_color, vec3_t out_dir)
{
	int light_count = 0;
	int contributed = 0;
	const rl_light_t *lights;
	vec3_t dir_accum = {0.f, 0.f, 0.f};
	float dir_weight_sum = 0.f;

	if (!r_dynamic.value)
	{
		if (out_dir)
			VectorClear (out_dir);
		return 0;
	}

	lights = R_PPdlights_GetFrameLights (&light_count);
	if (!lights || light_count <= 0)
	{
		if (out_dir)
			VectorClear (out_dir);
		return 0;
	}

	for (int i = 0; i < light_count; ++i)
	{
		const rl_light_t *src = &lights[i];
		vec3_t delta;
		float dist;
		float add;
		vec3_t contrib;

		if ((src->flags & RL_LIGHT_SURFACE_CONTRIB) == 0u)
			continue;
		/* CPU model sampling accepts runtime point dlights only. */
		if (src->type != RL_LIGHT_POINT)
			continue;
		if (src->radius <= 0.f || src->intensity <= 0.f)
			continue;

		VectorSubtract (pos, src->origin, delta);
		dist = VectorLength (delta);
		add = src->radius - dist;
		if (add <= 0.f)
			continue;

		VectorScale (src->color, add * src->intensity, contrib);
		VectorAdd (out_color, contrib, out_color);
		contributed++;

		if (out_dir && dist > 1e-4f)
		{
			vec3_t ldir;
			const float weight = q_max (R_ModelLightLuma (contrib), 0.f);
			VectorScale (delta, -1.f / dist, ldir);
			VectorMA (dir_accum, weight, ldir, dir_accum);
			dir_weight_sum += weight;
		}
	}

	if (out_dir)
	{
		if (dir_weight_sum > 1e-6f && VectorNormalize (dir_accum) > 1e-6f)
			VectorCopy (dir_accum, out_dir);
		else
			VectorClear (out_dir);
	}

	return contributed;
}

void R_AccumulateEntityModelDLights (const vec3_t pos, vec3_t out_color, vec3_t out_dir)
{
	VectorClear (out_color);
	if (out_dir)
		VectorClear (out_dir);

	if (!r_dynamic.value)
		return;

	/* Single-path behavior: model lighting uses the shared per-pixel frame lights. */
	(void)R_AccumulateEntityPPFrameLights (pos, out_color, out_dir);
}


static void R_ParseDlightColor (const char *value, vec3_t color)
{
	float r = 1.f, g = 1.f, b = 1.f;
	if (value && IW_PARSE_SSCANF (value, "%f %f %f", &r, &g, &b) == 3)
	{
		// Accept either 0-1 or 0-255 ranges; normalize to 0-1 for storage.
		if (r > 2.f || g > 2.f || b > 2.f)
		{
			r *= 1.f / 255.f;
			g *= 1.f / 255.f;
			b *= 1.f / 255.f;
		}
	}
	color[0] = r;
	color[1] = g;
	color[2] = b;
}

static qboolean R_ParseDlightOrigin (const char *value, vec3_t origin)
{
	return value && IW_PARSE_SSCANF (value, "%f %f %f", &origin[0], &origin[1], &origin[2]) == 3;
}

static void R_AddEntityDlight (const vec3_t origin, float radius, const vec3_t color, int style, int key)
{
	dlight_t *dl = DLightPool_GetOrCreatePersistent (key, cl.time);

	VectorCopy (origin, dl->origin);
	VectorCopy (color, dl->color);
	dl->baseradius = radius;
	dl->radius = radius;
	dl->spawn = cl.time - 0.001f;
	dl->die = FLT_MAX;
	dl->decay = 0.f;
	dl->minlight = 0.f;
	dl->key = key;
	dl->type = DLIGHT_DEFAULT;
	dl->style = style;
	dl->flicker_seed = (float) rand ();
	dl->kind = DL_PERSISTENT;
	dl->active = true;
}

// Parse BSP entity lump for persistent dynamic lights.
// Supported forms:
//   classname "dlight" (always persistent)
//   classname "light" with either "dynamic" "1" or "dlight" "1"
// Keys:
//   "origin" (vector, required)
//   "radius" or fallback "light" (radius/intensity)
//   "_color" or "color" (RGB, accepts floats 0-1 or bytes 0-255, stored as 0-1)
//   optional "style" / "flicker" / "pulse" (stored for future use)
void R_ParseDlightEntities (void)
{
	const char *data;
	int entity_dlight_count = 0;

	DLightPool_ClearPersistent ();

	if (!cl.worldmodel || !cl.worldmodel->entities)
		return;

	data = cl.worldmodel->entities;
	data = COM_Parse (data);
	while (data && com_token[0])
	{
		vec3_t origin = {0.f, 0.f, 0.f};
		vec3_t color = {1.f, 1.f, 1.f};
		float radius = 0.f;
		int style = 0;
		qboolean classname_is_dlight = false;
		qboolean classname_is_light = false;
		qboolean marked_dynamic = false;
		qboolean parsed_origin = false;

		if (com_token[0] != '{')
			break;

		while (1)
		{
			char key[64], value[1024];
			data = COM_Parse (data);
			if (!data || !com_token[0])
				return;
			if (com_token[0] == '}')
				break;
			q_strlcpy (key, com_token, sizeof (key));
			if (key[0] == '_')
				memmove (key, key + 1, strlen (key));
			data = COM_ParseEx (data, CPE_ALLOWTRUNC);
			if (!data)
				return;
			q_strlcpy (value, com_token, sizeof (value));
			if (!strcmp (key, "classname"))
			{
				classname_is_dlight = !strcmp (value, "dlight");
				classname_is_light = !strcmp (value, "light");
			}
			else if (!strcmp (key, "origin"))
				parsed_origin = R_ParseDlightOrigin (value, origin);
			else if (!strcmp (key, "_color") || !strcmp (key, "color"))
				R_ParseDlightColor (value, color);
			else if (!strcmp (key, "radius"))
				radius = atof (value);
			else if (!strcmp (key, "light") && radius <= 0.f)
				radius = atof (value);
			else if (!strcmp (key, "dynamic") || !strcmp (key, "dlight"))
				marked_dynamic = atoi (value) != 0;
			else if (!strcmp (key, "style") || !strcmp (key, "flicker") || !strcmp (key, "pulse"))
				style = atoi (value);
		}

		if ((classname_is_dlight || (classname_is_light && marked_dynamic)) && radius > 0.f && parsed_origin)
		{
			const int key = -(1000 + ++entity_dlight_count);
			R_AddEntityDlight (origin, radius, color, style, key);
		}

		data = COM_Parse (data);
	}

	if (entity_dlight_count || developer.value)
	{
		int active_count = 0;
		const dlight_t *const *active = DLightPool_GetActiveList (&active_count);
		int shown = 0;

		Con_DPrintf ("Spawned %d entity dlights (showing %d):\n",
				entity_dlight_count, q_min (entity_dlight_count, 5));

		for (int i = 0; i < active_count && shown < 5; i++)
		{
			const dlight_t *dl = active[i];
			if (dl->kind != DL_PERSISTENT)
				continue;
			Con_DPrintf ("  #%d origin %.1f %.1f %.1f radius %.1f color %.2f %.2f %.2f style %d\n", shown,
					dl->origin[0], dl->origin[1], dl->origin[2], dl->baseradius,
					dl->color[0], dl->color[1], dl->color[2], dl->style);
			shown++;
		}
	}
	return;
}

int RecursiveLightPoint (qmodel_t *model, lightcache_t *cache, mnode_t *node, vec3_t rayorg, vec3_t start, vec3_t end, float *maxdist);
static qboolean R_LightgridEnabledInternal (const lightgrid_t *lg)
{
        if (r_lightgrid.value <= 0.f)
                return false;

        if (lg && lg->octree)
                return true;

        return r_lightgrid_force.value > 0.f;
}

/*
==================
R_AnimateLight
==================
*/
void R_AnimateLight (void)
{
	int			i,j,k,n;
	double		f,base;

//
// light animations
// 'm' is normal light, 'a' is no light, 'z' is double bright
	f = cl.time * 10.0;
	base = floor(f);
	i = (int)base;
        f -= base;
        if (!r_lerplightstyles.value)
                f = 0.0;
        r_lightstyle_framefrac = (float)f;

	for (j=0 ; j<MAX_LIGHTSTYLES ; j++)
	{
		if (!cl_lightstyle[j].length)
		{
                        d_lightstylevalue[j] = 256;
                        r_lightbuffer.lightstyles[j * 2 + 0] = 1.f;
                        r_lightbuffer.lightstyles[j * 2 + 1] = 1.f;
                        continue;
                }
		//johnfitz -- r_flatlightstyles
		if (r_flatlightstyles.value == 2)
			k = n = cl_lightstyle[j].peak - 'a';
		else if (r_flatlightstyles.value == 1 || !r_dynamic.value)
			k = n = cl_lightstyle[j].average - 'a';
		else
		{
			k = i % cl_lightstyle[j].length;
			n = k + 1;
			if (n == cl_lightstyle[j].length)
				n = 0;
			k = cl_lightstyle[j].map[k] - 'a';
			n = cl_lightstyle[j].map[n] - 'a';
		}
		// only interpolate abrupt changes (e.g. flickering light in e1m1) if r_lerplightstyles >= 2
		if (r_lerplightstyles.value < 2.f && abs(n - k) >= ('m' - 'a') / 2)
			n = k;
                d_lightstylevalue[j] = (int)(k*22 + (n-k)*22*f);
                r_lightbuffer.lightstyles[j * 2 + 0] = k * (22.f/256.f);
                r_lightbuffer.lightstyles[j * 2 + 1] = n * (22.f/256.f);
                //johnfitz
        }

	if (r_fullbright_cheatsafe)
		r_lightbuffer.lightstyles[0] = 1.f;
}

/*
=============================================================================

DYNAMIC LIGHTS

=============================================================================
*/

const vec3_t *R_GetDynamicLightTemperature (int type)
{
	static const vec3_t temps[DLIGHT_MAX_TYPES] = {
		{ 1.0f, 1.0f, 1.0f }, // default
		{ 1.25f, 0.90f, 0.65f }, // rocket
		{ 1.30f, 1.10f, 1.50f }, // plasma
		{ 0.60f, 0.75f, 1.40f }, // lightning
		{ 1.30f, 1.00f, 0.50f }, // explosion
		{ 1.30f, 1.10f, 0.80f }, // torch
		{ 1.00f, 0.60f, 1.50f }, // teleport
		{ 1.20f, 0.40f, 0.20f }, // lava
	};

	if (type < 0 || type >= DLIGHT_MAX_TYPES)
		type = DLIGHT_DEFAULT;

	return &temps[type];
}

/*
=============
GLLight_CreateResources
=============
*/
void GLLight_CreateResources (void)
{
}

/*
=============
GLLight_DeleteResources
=============
*/
void GLLight_DeleteResources (void)
{
}


static void R_PushDlightArray (dlight_t *const *lights, int count)
{
	int	j;

	for (int i = 0; i < count; i++)
	{
		dlight_t *l = lights[i];
		gpulight_t *out;
		qboolean cull = false;
		float radius;
		vec3_t finalcolor;

		if (!CL_DlightTransientIsLiveAtTime (l, cl.time, NULL))
			continue;

		if (!CL_DlightIsActive (l))
			continue;

		R_EvaluateDLightForRender (l, &radius, finalcolor);
		l->radius = radius;

		for (j = 0; j < 4; j++)
		{
			mplane_t *p = &frustum[j];
			if (DotProduct (p->normal, l->origin) - p->dist + radius < 0.f)
			{
				cull = true;
				break;
			}
		}
		if (cull)
			continue;

		out = &r_lightbuffer.lights[r_framedata.numlights++];
		r_dlight_sources[r_framedata.numlights - 1] = l;
		out->pos[0]   = l->origin[0];
		out->pos[1]   = l->origin[1];
		out->pos[2]   = l->origin[2];
		out->radius   = radius;
		out->color[0] = finalcolor[0];
		out->color[1] = finalcolor[1];
		out->color[2] = finalcolor[2];
		out->minlight = l->minlight;
	}
}

/*
===============
R_PushDlights
===============
*/
void R_PushDlights (void)
{
	dlight_t *submit[DLIGHT_GPU_MAX];

	r_framedata.numlights = 0;
	memset (r_dlight_sources, 0, sizeof (r_dlight_sources));
	if (r_dynamic.value > 0.f)
	{
		const int budget = q_min (DLightPool_GetBudget (), DLIGHT_GPU_MAX);
		DLightPool_NewFrame (cl.time, r_framecount);
		const int num_submit = DLightPool_CollectForRender (cl.time, r_refdef.vieworg, r_viewleaf, submit, budget);
		if (num_submit > 0)
			R_PushDlightArray (submit, num_submit);
	}
	else
		DLightPool_NewFrame (cl.time, r_framecount);

	R_UploadFrameData ();
}



/*
=============================================================================

LIGHT SAMPLING

=============================================================================
*/

vec3_t lightcolor; //johnfitz -- lit support via lordhavoc

/*
==================
R_LightgridEnabled
==================
*/
qboolean R_LightgridEnabled (void)
{
        return R_LightgridEnabledInternal (Lightgrid_Get ());
}

/*
==================
R_LightgridLighting
==================
*/
void R_LightgridLighting (const vec3_t pos, vec3_t out_color, float *out_ao)
{
        const lightgrid_t *lg = Lightgrid_Get ();
        float ao = 1.f;

        if (!R_LightgridEnabledInternal (lg))
        {
                VectorClear (out_color);
                if (out_ao)
                        *out_ao = 1.f;
                return;
        }

        Lightgrid_Sample (pos, out_color, &ao);
        VectorScale (out_color, ao, out_color);
        if (out_ao)
                *out_ao = ao;
}

/*
==================
R_AddDynamicLights_Lightgrid
==================
*/
static void R_ClampSampleColor(vec3_t color);
static int R_LightPoint_Internal (qmodel_t *model, vec3_t p, float ofs, lightcache_t *cache, qboolean include_dynamic);

static int R_AddDynamicLights_Lightgrid_Internal (const vec3_t pos, vec3_t out_lightcolor)
{
	if (!r_dynamic.value)
		return 0;

	int count = 0;
	int contributed = 0;
	const dlight_t *const *active = DLightPool_GetActiveList (&count);
	if (!active || count <= 0)
		return 0;

	for (int i = 0; i < count; ++i)
	{
		const dlight_t *l = active[i];
		vec3_t dist;
		float add;
		float dist_len;
		float radius;
		vec3_t eval_color;
		vec3_t contrib;

		if (!l || !CL_DlightIsActive (l))
			continue;

		R_EvaluateDLightForRender (l, &radius, eval_color);
		if (radius <= 0.f)
			continue;

		VectorSubtract (pos, l->origin, dist);
		dist_len = VectorLength (dist);
		add = radius - dist_len;
		if (add <= l->minlight)
			continue;

		add -= l->minlight;
		VectorScale (eval_color, add, contrib);
		VectorAdd (out_lightcolor, contrib, out_lightcolor);
		contributed++;
	}

	return contributed;
}

void R_AddDynamicLights_Lightgrid (const vec3_t pos, vec3_t out_lightcolor)
{
	(void)R_AddDynamicLights_Lightgrid_Internal (pos, out_lightcolor);
}

static int R_AddDynamicLights_PPFrame (const vec3_t pos, vec3_t out_lightcolor)
{
	int light_count = 0;
	int contributed = 0;
	const rl_light_t *lights;

	if (!r_dynamic.value)
		return 0;

	lights = R_PPdlights_GetFrameLights (&light_count);
	if (!lights || light_count <= 0)
		return 0;

	for (int i = 0; i < light_count; ++i)
	{
		const rl_light_t *src = &lights[i];
		vec3_t delta;
		float dist;
		float add;
		vec3_t contrib;

		if ((src->flags & RL_LIGHT_SURFACE_CONTRIB) == 0u)
			continue;
		if (src->radius <= 0.f)
			continue;

		VectorSubtract (pos, src->origin, delta);
		dist = VectorLength (delta);
		add = src->radius - dist;
		if (add <= 0.f)
			continue;

		VectorScale (src->color, add * src->intensity, contrib);
		VectorAdd (out_lightcolor, contrib, out_lightcolor);
		contributed++;
	}

	return contributed;
}

static void R_AddDynamicLights_Receiver (const vec3_t pos, qboolean lightgrid_has_sample, vec3_t out_lightcolor)
{
	(void)lightgrid_has_sample;

	if (!r_dynamic.value)
		return;

	/* Single-path behavior: receiver dynamic contribution always comes from PP frame lights. */
	(void)R_AddDynamicLights_PPFrame (pos, out_lightcolor);
}

void R_SampleReceiverLighting (const vec3_t pos, vec3_t out_color)
{
	lightcache_t cache;
	vec3_t sample_pos;

	if (!out_color)
		return;

	if (r_fullbright_cheatsafe || r_lightmap_cheatsafe || !cl.worldmodel)
	{
		VectorSet (out_color, 1.f, 1.f, 1.f);
		return;
	}

	memset (&cache, 0, sizeof (cache));
	VectorCopy (pos, sample_pos);
	R_LightPoint_Internal (cl.worldmodel, sample_pos, 0.f, &cache, false);

	R_AddDynamicLights_Receiver (pos, cache.lightgrid_has_sample, lightcolor);

	R_ClampSampleColor (lightcolor);
	VectorScale (lightcolor, 1.f / 255.f, out_color);
}


static inline int LightStyleValue (unsigned short style)
{
if (style < 256)
return d_lightstylevalue[style];

return d_lightstylevalue[0];
}

static qboolean SampleDeluxemapDir(const qmodel_t *model, const msurface_t *surf, int ds, int dt, vec3_t out_dir)
{
	const byte *samples;
	int smax, tmax, lmshift, lmmask;
	int dsfrac, dtfrac;
	float fsfrac, ftfrac;
	const float to_signed = 2.f * (1.f / 255.f);

if (!model || !model->lightdirdata || !surf || !surf->luxsamples)
{
VectorClear(out_dir);
return false;
}

	samples = surf->luxsamples;
	lmshift = q_min ((int)surf->lmshift, 15);
	lmmask = (1 << lmshift) - 1;
	dsfrac = ds & lmmask;
	dtfrac = dt & lmmask;
	fsfrac = dsfrac * (1.f / (float)(1 << lmshift));
	ftfrac = dtfrac * (1.f / (float)(1 << lmshift));
	smax = (surf->extents[0] >> lmshift) + 1;
	tmax = (surf->extents[1] >> lmshift) + 1;

	const int s0 = ds >> lmshift;
	const int t0 = dt >> lmshift;
const int stride = smax * 3;

const byte *row0 = samples + t0 * stride;
const byte *row1 = (t0 + 1 < tmax) ? row0 + stride : row0;
const byte *col00 = row0 + s0 * 3;
const byte *col10 = (s0 + 1 < smax) ? col00 + 3 : col00;
const byte *col01 = row1 + s0 * 3;
const byte *col11 = (s0 + 1 < smax) ? col01 + 3 : col01;

vec3_t lux00 = {col00[0] * to_signed - 1.f, col00[1] * to_signed - 1.f, col00[2] * to_signed - 1.f};
vec3_t lux10 = {col10[0] * to_signed - 1.f, col10[1] * to_signed - 1.f, col10[2] * to_signed - 1.f};
vec3_t lux01 = {col01[0] * to_signed - 1.f, col01[1] * to_signed - 1.f, col01[2] * to_signed - 1.f};
vec3_t lux11 = {col11[0] * to_signed - 1.f, col11[1] * to_signed - 1.f, col11[2] * to_signed - 1.f};

vec3_t top, bottom;
VectorLerp(lux00, lux10, fsfrac, top);
VectorLerp(lux01, lux11, fsfrac, bottom);
VectorLerp(top, bottom, ftfrac, out_dir);

const float len = VectorNormalize(out_dir);
if (len < 1e-6f || !R_IsFinite(len))
{
VectorClear(out_dir);
return false;
}

return true;
}

static void InterpolateLightmap (qmodel_t *model, vec3_t color, msurface_t *surf, int ds, int dt, qboolean use_rgb)
{
	const byte *samples;
	int smax, tmax, lmshift, lmmask;
	int dsfrac, dtfrac;
	float fsfrac, ftfrac;
	        int bytes_per_pixel;

        if (!surf->samples)
        {
                VectorClear(color);
                return;
        }

        samples = surf->samples;
        bytes_per_pixel = (model && (model->flags & MOD_HDRLIGHTING)) ? 4 : 3;

        if (use_rgb && model && model->lightdata && model->lightdata_rgb)
        {
                const int bytes_per_sample = bytes_per_pixel;
                const ptrdiff_t offset = samples - model->lightdata;

                if (bytes_per_sample > 0 && offset >= 0 && (offset % bytes_per_sample) == 0)
                {
                        const size_t sample_offset = (size_t)offset / (size_t)bytes_per_sample;
                        const size_t rgb_offset = sample_offset * 3;

                        if (rgb_offset + 3 <= (size_t)model->lightdata_rgb_size)
                        {
                                samples = model->lightdata_rgb + rgb_offset;
                                bytes_per_pixel = 3;
                        }
                }
        }

	        lmshift = q_min ((int)surf->lmshift, 15);
	        lmmask = (1 << lmshift) - 1;
	        dsfrac = ds & lmmask;
	        dtfrac = dt & lmmask;
	        fsfrac = dsfrac * (1.f / (float)(1 << lmshift));
	        ftfrac = dtfrac * (1.f / (float)(1 << lmshift));
	        smax = (surf->extents[0] >> lmshift) + 1;
	        tmax = (surf->extents[1] >> lmshift) + 1;

	        const int s0 = ds >> lmshift;
	        const int t0 = dt >> lmshift;
        const int stride = smax * bytes_per_pixel;
        const int facesize = smax * tmax * bytes_per_pixel;

        VectorClear(color);

        for (int maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE; maps++)
        {
                const float scale = LightStyleValue(surf->styles[maps]) * (1.f / 256.f);
                const byte *lightmap = samples + facesize * maps;

                const byte *row0 = lightmap + t0 * stride;
                const byte *row1 = (t0 + 1 < tmax) ? row0 + stride : row0;
                const byte *col00 = row0 + s0 * bytes_per_pixel;
                const byte *col01 = (s0 + 1 < smax) ? col00 + bytes_per_pixel : col00;
                const byte *col10 = row1 + s0 * bytes_per_pixel;
                const byte *col11 = (s0 + 1 < smax) ? col10 + bytes_per_pixel : col10;

                float w00 = (1.f - fsfrac) * (1.f - ftfrac);
                float w01 = fsfrac * (1.f - ftfrac);
                float w10 = (1.f - fsfrac) * ftfrac;
                float w11 = fsfrac * ftfrac;

                color[0] += scale * (w00 * col00[0] + w01 * col01[0] + w10 * col10[0] + w11 * col11[0]);
                color[1] += scale * (w00 * col00[1] + w01 * col01[1] + w10 * col10[1] + w11 * col11[1]);
                color[2] += scale * (w00 * col00[2] + w01 * col01[2] + w10 * col10[2] + w11 * col11[2]);
        }
}

static void R_SurfaceFallbackColor(const qmodel_t *model, const msurface_t *surf, vec3_t out)
{
        const texture_t *tex = NULL;

        if (model && surf && surf->texinfo && surf->texinfo->texnum >= 0 && surf->texinfo->texnum < model->numtextures)
                tex = model->textures[surf->texinfo->texnum];

        VectorSet(out, 127.f, 127.f, 127.f);

        if (!tex)
                return;

        switch (tex->type)
        {
        case TEXTYPE_LAVA:
                VectorSet(out, 255.f, 128.f, 32.f);
                break;
        case TEXTYPE_SLIME:
                VectorSet(out, 64.f, 200.f, 96.f);
                break;
        case TEXTYPE_WATER:
                VectorSet(out, 64.f, 96.f, 196.f);
                break;
        default:
                break;
        }
}

static void R_ClampSampleColor(vec3_t color)
{
        for (int i = 0; i < 3; i++)
        {
                if (!R_IsFinite(color[i]) || color[i] < 0.f)
                        color[i] = 0.f;
        }

        if (VectorLength(color) < 1e-6f)
                VectorClear(color);
}

static qboolean R_AdjustPointForLeaf (qmodel_t *model, vec3_t point)
{
        mleaf_t *leaf;

        for (int i = 0; i < 8; i++)
        {
                leaf = Mod_PointInLeaf (point, model);

                if (!leaf || leaf->contents != CONTENTS_SOLID)
                        return true;

                point[2] += 1.f;
        }

        return leaf && leaf->contents != CONTENTS_SOLID;
}

static void R_SamplePointInternal (qmodel_t *model, const vec3_t pos, float ofs, qboolean use_rgblight, lightcache_t *cache, vec3_t out_color)
{
        vec3_t start, end;
        float maxdist = 8192.f;
        lightcache_t local_cache;

        if (!cache)
        {
                memset (&local_cache, 0, sizeof(local_cache));
                cache = &local_cache;
        }

        start[0] = pos[0];
        start[1] = pos[1];
        start[2] = pos[2] + ofs;
        end[0] = start[0];
        end[1] = start[1];
        end[2] = start[2] - maxdist;

        VectorClear (out_color);

        if (cache->surfidx <= 0 // no cache or pitch black
                || (model && cache->surfidx > model->numsurfaces)
                || fabsf (cache->pos[0] - pos[0]) >= 1.f
                || fabsf (cache->pos[1] - pos[1]) >= 1.f
                || fabsf (cache->pos[2] - pos[2]) >= 1.f)
        {
                cache->surfidx = 0;
                VectorCopy (pos, cache->pos);
                RecursiveLightPoint (model, cache, model->nodes, start, start, end, &maxdist);
        }

        if (cache->surfidx > 0)
        {
                InterpolateLightmap (model, out_color, model->surfaces + cache->surfidx - 1, cache->ds, cache->dt, use_rgblight);
                R_ClampSampleColor (out_color);
}
}

static qboolean R_LightgridCellForPoint (const vec3_t pos, int out_cell[3])
{
        const lightgrid_t *lg = Lightgrid_Get ();

        if (!out_cell || !R_LightgridEnabledInternal (lg) || !lg->octree)
                return false;

        const lightgrid_octree_header_t *header = &lg->octree->header;

        for (int i = 0; i < 3; i++)
        {
                float local = (pos[i] - header->grid_mins[i]) / header->grid_dist[i];
                int cell = Q_rint (local);
                if (cell < 0 || cell >= header->grid_size[i])
                        return false;
                out_cell[i] = cell;
        }

        return true;
}

static qboolean R_LightPointNoGrid (qmodel_t *model, vec3_t p, float ofs, lightcache_t *cache, vec3_t out_color)
{
        qboolean use_rgblight = model && model->has_lightdata_rgb && r_rgblighting_enable.value;
        vec3_t sample_pos;
        lightcache_t local_cache;

        if (!cache)
        {
                memset (&local_cache, 0, sizeof (local_cache));
                cache = &local_cache;
        }

        cache->lightgrid_has_sample = false;
        cache->lightgrid_ao = 0.f;
        VectorClear (cache->lightgrid_color);

        if (!model || !model->lightdata)
        {
                VectorSet (out_color, 255.f, 255.f, 255.f);
                return false;
        }

        VectorCopy (p, sample_pos);

        if (!R_AdjustPointForLeaf (model, sample_pos))
        {
                const float ambient = 0.04f * 255.f;
                VectorSet (out_color, ambient, ambient, ambient);
                cache->surfidx = -1;
                VectorCopy (sample_pos, cache->pos);
                R_ClampSampleColor (out_color);
                return false;
        }

        R_SamplePointInternal (model, sample_pos, ofs, use_rgblight, cache, out_color);

        const vec3_t fallback_offsets[] = {
                { 0.f, 0.f, 0.f }, { 2.f, 0.f, 0.f }, { -2.f, 0.f, 0.f }, { 0.f, 2.f, 0.f }, { 0.f, -2.f, 0.f },
                { 0.f, 0.f, 2.f }, { 0.f, 0.f, -2.f }, { 4.f, 4.f, 0.f }, { -4.f, -4.f, 0.f }, { 0.f, 4.f, 4.f }
        };

        float intensity = (out_color[0] + out_color[1] + out_color[2]) * (1.f / (3.f * 255.f));

        if (intensity < 0.001f)
        {
                vec3_t accum = {0, 0, 0};
                int hits = 0;

                for (size_t i = 0; i < sizeof (fallback_offsets) / sizeof (fallback_offsets[0]); i++)
                {
                        vec3_t test;
                        VectorAdd (sample_pos, fallback_offsets[i], test);

                        if (!R_AdjustPointForLeaf (model, test))
                                continue;

                        vec3_t temp_color;
                        lightcache_t temp_cache = {0};
                        R_SamplePointInternal (model, test, ofs, use_rgblight, &temp_cache, temp_color);

                        if (VectorLength (temp_color) > 0.f)
                        {
                                VectorAdd (accum, temp_color, accum);
                                hits++;
                        }
                }

                if (hits > 0)
                {
                        VectorScale (accum, 1.f / (float)hits, out_color);
                        intensity = (out_color[0] + out_color[1] + out_color[2]) * (1.f / (3.f * 255.f));
                }
        }

        if (intensity > 0.f && intensity < 0.015f)
        {
                vec3_t raised;
                VectorCopy (sample_pos, raised);
                raised[2] += 4.f;

                if (R_AdjustPointForLeaf (model, raised))
                {
                        vec3_t above_color;
                        lightcache_t temp_cache = {0};
                        R_SamplePointInternal (model, raised, ofs, use_rgblight, &temp_cache, above_color);
                        VectorMA (out_color, 0.2f, above_color, out_color);
                }
        }

        R_ClampSampleColor (out_color);

	return VectorLength (out_color) > 0.f;
}

typedef struct entity_static_sample_s {
	vec3_t color255;
	vec3_t lightgrid_effective;
	qboolean valid;
	qboolean used_lightgrid;
	qboolean lightgrid_valid;
	qboolean used_lightpoint;
} entity_static_sample_t;

static float R_SampleIntensity255 (const vec3_t color255)
{
	return (color255[0] + color255[1] + color255[2]) * (1.f / (3.f * 255.f));
}

static void R_ModelLightRotateOffset (entity_t *e, vec3_t local_offset, vec3_t out_world_offset)
{
	vec3_t forward, right, up;

	if (!e)
	{
		VectorCopy (local_offset, out_world_offset);
		return;
	}

	AngleVectors (e->angles, forward, right, up);
	out_world_offset[0] = forward[0] * local_offset[0] + right[0] * local_offset[1] + up[0] * local_offset[2];
	out_world_offset[1] = forward[1] * local_offset[0] + right[1] * local_offset[1] + up[1] * local_offset[2];
	out_world_offset[2] = forward[2] * local_offset[0] + right[2] * local_offset[1] + up[2] * local_offset[2];
}

static void R_ModelLightBuildSamplePos (entity_t *e, vec3_t local_offset, vec3_t out_pos)
{
	vec3_t world_offset;
	R_ModelLightRotateOffset (e, local_offset, world_offset);
	VectorAdd (e->origin, world_offset, out_pos);
}

static qboolean R_ModelLightTryGridSample (entity_t *e, qmodel_t *lightmodel, const vec3_t pos, entity_static_sample_t *sample)
{
	const lightgrid_probe_t *probe;
	vec3_t effective;
	float threshold;
	(void)e;

	if (r_model_lightgrid.value <= 0.f || !R_LightgridEnabled ())
		return false;

	probe = R_GetLightgridSample (pos);
	if (!probe)
		return false;

	sample->lightgrid_valid = probe->intensity > 0.f || probe->ao > 0.f;
	if (!sample->lightgrid_valid)
		return false;

	VectorScale (probe->rgb, CLAMP (0.f, probe->ao, 1.f), effective);

	threshold = CLAMP (0.f, r_model_lightgrid_assist_threshold.value, 1.f);
	if (r_model_lightgrid_assist.value > 0.f && threshold > 0.f && R_ModelLightLuma (effective) < threshold)
	{
		static const vec3_t neighbor_offsets[] = {
			{ 8.f, 0.f, 0.f }, { -8.f, 0.f, 0.f }, { 0.f, 8.f, 0.f }, { 0.f, -8.f, 0.f }, { 0.f, 0.f, 8.f }, { 0.f, 0.f, -8.f }
		};
		vec3_t accum;
		float weight = 1.f;

		VectorCopy (effective, accum);

		for (int i = 0; i < (int)countof (neighbor_offsets); i++)
		{
			vec3_t test_pos;
			const lightgrid_probe_t *neighbor;
			float neighbor_ao;
			vec3_t neighbor_effective;

			VectorAdd (pos, neighbor_offsets[i], test_pos);
			neighbor = R_GetLightgridSample (test_pos);
			if (!neighbor)
				continue;

			neighbor_ao = CLAMP (0.f, neighbor->ao, 1.f);
			if (neighbor->intensity <= 0.f && neighbor_ao <= 0.f)
				continue;

			VectorScale (neighbor->rgb, neighbor_ao, neighbor_effective);
			VectorAdd (accum, neighbor_effective, accum);
			weight += 1.f;
		}

		VectorScale (accum, 1.f / weight, effective);

		if (R_ModelLightLuma (effective) < threshold && lightmodel)
		{
			vec3_t point255;
			lightcache_t temp_cache = {0};
			vec3_t point_pos;
			VectorCopy (pos, point_pos);
			if (R_LightPointNoGrid (lightmodel, point_pos, 0.f, &temp_cache, point255))
			{
				const float assist = CLAMP (0.f, (threshold - R_ModelLightLuma (effective)) / q_max (threshold, 0.001f), 1.f) * 0.5f;
				vec3_t grid255;
				VectorScale (effective, 255.f, grid255);
				VectorLerp (grid255, point255, assist, sample->color255);
				VectorScale (sample->color255, 1.f / 255.f, effective);
			}
		}
	}

	if (!sample->color255[0] && !sample->color255[1] && !sample->color255[2])
		VectorScale (effective, 255.f, sample->color255);
	VectorCopy (effective, sample->lightgrid_effective);
	sample->used_lightgrid = true;
	sample->valid = VectorLength (sample->color255) > 0.f;
	return sample->valid;
}

static void R_EntityStaticLightSampleAtPoint (entity_t *e, qmodel_t *lightmodel, const vec3_t pos, float ofs, lightcache_t *cache, entity_static_sample_t *sample)
{
	memset (sample, 0, sizeof (*sample));
	VectorClear (sample->color255);
	VectorClear (sample->lightgrid_effective);

	if (R_ModelLightTryGridSample (e, lightmodel, pos, sample))
		return;

	if (lightmodel)
	{
		vec3_t point_pos;
		VectorCopy (pos, point_pos);
		if (R_LightPointNoGrid (lightmodel, point_pos, ofs, cache, sample->color255))
		{
			sample->used_lightpoint = true;
			sample->valid = true;
		}
	}
}

static qboolean R_SampleLightmapAtPointInternal(const vec3_t pos, vec3_t out_rgb, vec3_t out_dir, qboolean want_dir)
{
qmodel_t *model = cl.worldmodel;
qboolean use_rgblight;
qboolean found = false;
qboolean dir_valid = false;
float best_dist = FLT_MAX;

VectorClear(out_rgb);
if (out_dir)
VectorClear(out_dir);

if (!model || !model->lightdata)
return false;

        use_rgblight = model->has_lightdata_rgb && r_rgblighting_enable.value;

        vec3_t pos_copy;
        VectorCopy(pos, pos_copy);
        mleaf_t *leaf = Mod_PointInLeaf(pos_copy, model);
if (!leaf || leaf->contents == CONTENTS_SOLID)
return false;

for (int i = 0; i < leaf->nummarksurfaces; i++)
{
msurface_t *surf = &model->surfaces[leaf->firstmarksurface[i]];

                if (!surf->texinfo || (surf->flags & SURF_DRAWTILED))
                        continue;

                const float pdist = DotProduct(pos, surf->plane->normal) - surf->plane->dist;
                if (fabsf(pdist) > 8.f)
                        continue;

                int ds = (int)((double)DoublePrecisionDotProduct(pos, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3]);
                int dt = (int)((double)DoublePrecisionDotProduct(pos, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3]);

                if (ds < surf->texturemins[0] || dt < surf->texturemins[1])
                        continue;

                ds -= surf->texturemins[0];
                dt -= surf->texturemins[1];

if (ds > surf->extents[0] || dt > surf->extents[1])
continue;

vec3_t color;
if (surf->samples)
{
InterpolateLightmap(model, color, surf, ds, dt, use_rgblight);
}
else
{
R_SurfaceFallbackColor(model, surf, color);
}

if (!VectorLength(color))
continue;

const float adist = fabsf(pdist);
if (adist < best_dist)
{
VectorCopy(color, out_rgb);
best_dist = adist;
found = true;

if (want_dir && out_dir && surf->luxsamples)
dir_valid = SampleDeluxemapDir(model, surf, ds, dt, out_dir);
}
}

if (found)
{
VectorScale(out_rgb, 1.f / 255.f, out_rgb);
if (want_dir && out_dir && !dir_valid)
VectorClear(out_dir);
return true;
}

R_SurfaceFallbackColor(model, NULL, out_rgb);
VectorScale(out_rgb, 1.f / 255.f, out_rgb);
if (want_dir && out_dir)
VectorClear(out_dir);
return false;
}

qboolean R_SampleLightmapAtPoint(const vec3_t pos, vec3_t out_rgb)
{
return R_SampleLightmapAtPointInternal(pos, out_rgb, NULL, false);
}

qboolean R_SampleLightmapAndDeluxemapAtPoint(const vec3_t pos, vec3_t out_rgb, vec3_t out_dir)
{
return R_SampleLightmapAtPointInternal(pos, out_rgb, out_dir, true);
}

/*
=============
RecursiveLightPoint -- johnfitz -- replaced entire function for lit support via lordhavoc
=============
*/
int RecursiveLightPoint (qmodel_t *model, lightcache_t *cache, mnode_t *node, vec3_t rayorg, vec3_t start, vec3_t end, float *maxdist)
{
	float		front, back, frac;
	vec3_t		mid;

loc0:
	if (node->contents < 0)
		return false;		// didn't hit anything

// calculate mid point
	if (node->plane->type < 3)
	{
		front = start[node->plane->type] - node->plane->dist;
		back = end[node->plane->type] - node->plane->dist;
	}
	else
	{
		front = DotProduct(start, node->plane->normal) - node->plane->dist;
		back = DotProduct(end, node->plane->normal) - node->plane->dist;
	}

	// LordHavoc: optimized recursion
	if ((back < 0) == (front < 0))
//		return RecursiveLightPoint (model, cache, node->children[front < 0], rayorg, start, end, maxdist);
	{
		node = node->children[front < 0];
		goto loc0;
	}

	frac = front / (front-back);
	mid[0] = start[0] + (end[0] - start[0])*frac;
	mid[1] = start[1] + (end[1] - start[1])*frac;
	mid[2] = start[2] + (end[2] - start[2])*frac;

// go down front side
	if (RecursiveLightPoint (model, cache, node->children[front < 0], rayorg, start, mid, maxdist))
		return true;	// hit something
	else
	{
		unsigned int i;
		int ds, dt;
		msurface_t *surf;
	// check for impact on this node

		surf = model->surfaces + node->firstsurface;
		for (i = 0;i < node->numsurfaces;i++, surf++)
		{
			float sfront, sback, dist;
			vec3_t raydelta;

			if (surf->flags & SURF_DRAWTILED)
				continue;	// no lightmaps

		// ericw -- added double casts to force 64-bit precision.
		// Without them the zombie at the start of jam3_ericw.bsp was
		// incorrectly being lit up in SSE builds.
			ds = (int) ((double) DoublePrecisionDotProduct (mid, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3]);
			dt = (int) ((double) DoublePrecisionDotProduct (mid, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3]);

			if (ds < surf->texturemins[0] || dt < surf->texturemins[1])
				continue;

			ds -= surf->texturemins[0];
			dt -= surf->texturemins[1];

			if (ds > surf->extents[0] || dt > surf->extents[1])
				continue;

			if (surf->plane->type < 3)
			{
				sfront = rayorg[surf->plane->type] - surf->plane->dist;
				sback = end[surf->plane->type] - surf->plane->dist;
			}
			else
			{
				sfront = DotProduct(rayorg, surf->plane->normal) - surf->plane->dist;
				sback = DotProduct(end, surf->plane->normal) - surf->plane->dist;
			}
			VectorSubtract(end, rayorg, raydelta);
			dist = sfront / (sfront - sback) * VectorLength(raydelta);

			if (!surf->samples)
			{
				// We hit a surface that is flagged as lightmapped, but doesn't have actual lightmap info.
				// Instead of just returning black, we'll keep looking for nearby surfaces that do have valid samples.
				// This fixes occasional pitch-black models in otherwise well-lit areas in DOTM (e.g. mge1m1, mge4m1)
				// caused by overlapping surfaces with mixed lighting data.
				const float nearby = 8.f;
				dist += nearby;
				*maxdist = q_min(*maxdist, dist);
				continue;
			}

			if (dist < *maxdist)
			{
				cache->surfidx = surf - model->surfaces + 1;
				cache->ds = ds;
				cache->dt = dt;
			}
			else
			{
				cache->surfidx = -1;
			}

			return true; // success
		}

	// go down back side
		return RecursiveLightPoint (model, cache, node->children[front >= 0], rayorg, mid, end, maxdist);
	}
}

/*
=============
R_LightPoint -- johnfitz -- replaced entire function for lit support via lordhavoc
=============
*/
static int R_LightPoint_Internal (qmodel_t *model, vec3_t p, float ofs, lightcache_t *cache, qboolean include_dynamic)
{
        qboolean        use_rgblight = false;
        qboolean        lightgrid_active;

        cache->lightgrid_has_sample = false;
        cache->lightgrid_ao = 0.f;
        VectorClear (cache->lightgrid_color);

        lightgrid_active = R_LightgridEnabled ();

        if (lightgrid_active)
        {
                vec3_t lg_color, lg_color255;
                float lg_ao = 1.f;

                R_LightgridLighting (p, lg_color, &lg_ao);
                VectorScale (lg_color, 255.f, lg_color255);

                VectorCopy (lg_color255, lightcolor);
                if (include_dynamic)
                        R_AddDynamicLights_Lightgrid (p, lightcolor);

                cache->surfidx = 0;
                VectorCopy (p, cache->pos);
                cache->lightgrid_has_sample = true;
                cache->lightgrid_ao = lg_ao;
                VectorCopy (lg_color, cache->lightgrid_color);

                return ((lightcolor[0] + lightcolor[1] + lightcolor[2]) * (1.0f / 3.0f));
        }

        if (model && model->has_lightdata_rgb && r_rgblighting_enable.value)
                use_rgblight = true;

        if (!model || !model->lightdata)
        {
                lightcolor[0] = lightcolor[1] = lightcolor[2] = 255;
                return 255;
        }

        vec3_t sample_pos;
        VectorCopy (p, sample_pos);

        if (!R_AdjustPointForLeaf (model, sample_pos))
        {
                const float ambient = 0.04f * 255.f;
                VectorSet (lightcolor, ambient, ambient, ambient);
                cache->surfidx = -1;
                VectorCopy (sample_pos, cache->pos);
                R_ClampSampleColor (lightcolor);
                return (int)ambient;
        }

        R_SamplePointInternal (model, sample_pos, ofs, use_rgblight, cache, lightcolor);

        const vec3_t fallback_offsets[] = {
                { 0.f, 0.f, 0.f }, { 2.f, 0.f, 0.f }, { -2.f, 0.f, 0.f }, { 0.f, 2.f, 0.f }, { 0.f, -2.f, 0.f },
                { 0.f, 0.f, 2.f }, { 0.f, 0.f, -2.f }, { 4.f, 4.f, 0.f }, { -4.f, -4.f, 0.f }, { 0.f, 4.f, 4.f }
        };

        float intensity = (lightcolor[0] + lightcolor[1] + lightcolor[2]) * (1.f / (3.f * 255.f));

        if (intensity < 0.001f)
        {
                vec3_t accum = {0, 0, 0};
                int hits = 0;

                for (size_t i = 0; i < sizeof(fallback_offsets) / sizeof(fallback_offsets[0]); i++)
                {
                        vec3_t test;
                        VectorAdd (sample_pos, fallback_offsets[i], test);

                        if (!R_AdjustPointForLeaf (model, test))
                                continue;

                        vec3_t temp_color;
                        lightcache_t temp_cache = {0};
                        R_SamplePointInternal (model, test, ofs, use_rgblight, &temp_cache, temp_color);

                        if (VectorLength (temp_color) > 0.f)
                        {
                                VectorAdd (accum, temp_color, accum);
                                hits++;
                        }
                }

                if (hits > 0)
                {
                        VectorScale (accum, 1.f / (float)hits, lightcolor);
                        intensity = (lightcolor[0] + lightcolor[1] + lightcolor[2]) * (1.f / (3.f * 255.f));
                }
        }

        if (intensity > 0.f && intensity < 0.015f)
        {
                vec3_t raised;
                VectorCopy (sample_pos, raised);
                raised[2] += 4.f;

                if (R_AdjustPointForLeaf (model, raised))
                {
                        vec3_t above_color;
                        lightcache_t temp_cache = {0};
                        R_SamplePointInternal (model, raised, ofs, use_rgblight, &temp_cache, above_color);
                        VectorMA (lightcolor, 0.2f, above_color, lightcolor);
                }
        }

        R_ClampSampleColor (lightcolor);

        return ((lightcolor[0] + lightcolor[1] + lightcolor[2]) * (1.0f / 3.0f));
}

int R_LightPoint (qmodel_t *model, vec3_t p, float ofs, lightcache_t *cache)
{
	return R_LightPoint_Internal (model, p, ofs, cache, true);
}

qboolean R_EntityStaticLight (entity_t *e, vec3_t out_color255, entity_lightinfo_t *info)
{
	vec3_t lightgrid_effective = {0.f, 0.f, 0.f};
	vec3_t lightpoint_color = {0.f, 0.f, 0.f};
	vec3_t target_color255 = {0.f, 0.f, 0.f};
	qboolean lightgrid_valid = false;
	qboolean used_lightgrid = false;
	qboolean used_lightpoint = false;
	qboolean used_minlight = false;
	qboolean used_multisample = false;
	qboolean budget_fallback = false;
	int sample_count = 0;
	double start_time = Sys_DoubleTime ();
	const qboolean legacy_compatible = (r_model_light_multisample.value <= 0.f && r_model_lightgrid_assist.value <= 0.f);
	qmodel_t *lightmodel = cl.worldmodel ? cl.worldmodel : e->model;
	entity_static_light_source_t static_source = ENTITY_STATIC_LIGHT_NONE;
	float intensity;

	VectorClear (out_color255);

	if (legacy_compatible)
	{
		float lightgrid_ao = 1.f;
		vec3_t lightgrid_color = {0.f, 0.f, 0.f};

		sample_count = 1;

		if (r_model_lightgrid.value > 0.f && R_LightgridEnabled ())
		{
			vec3_t sample_pos;
			VectorCopy (e->origin, sample_pos);

			for (int attempt = 0; attempt < 2; attempt++)
			{
				const lightgrid_probe_t *probe = R_GetLightgridSample (sample_pos);
				if (!probe)
					break;

				VectorCopy (probe->rgb, lightgrid_color);
				lightgrid_ao = CLAMP (0.f, probe->ao, 1.f);
				lightgrid_valid = probe->intensity > 0.f || lightgrid_ao > 0.f;
				if (lightgrid_valid || attempt == 1 || !e->model)
					break;

				{
					float ofs = e->model ? (e->model->maxs[2] - e->model->mins[2]) * 0.5f : 0.f;
					if (ofs <= 0.f)
						break;
					sample_pos[2] += ofs;
				}
			}

			if (lightgrid_valid)
			{
				VectorScale (lightgrid_color, lightgrid_ao * 255.f, out_color255);
				VectorScale (lightgrid_color, lightgrid_ao, lightgrid_effective);
				used_lightgrid = true;
			}
		}

		if (!used_lightgrid && lightmodel)
		{
			if (!R_LightPointNoGrid (lightmodel, e->origin, 0.f, &e->lightcache, lightpoint_color))
			{
				float ofs = e->model ? (e->model->maxs[2] - e->model->mins[2]) * 0.5f : 0.f;
				R_LightPointNoGrid (lightmodel, e->origin, ofs, &e->lightcache, lightpoint_color);
			}
			VectorCopy (lightpoint_color, out_color255);
			used_lightpoint = VectorLength (lightpoint_color) > 0.f;
		}
	}
	else
	{
		vec3_t sample_pos[8];
		float sample_weight[8];
		int num_samples = 1;
		float total_weight = 0.f;
		vec3_t weighted_color = {0.f, 0.f, 0.f};
		float grid_weight = 0.f;
		float point_weight = 0.f;

		VectorCopy (e->origin, sample_pos[0]);
		sample_weight[0] = 1.f;

		if (r_model_light_multisample.value > 0.f && e->model)
		{
			const float height = q_max (e->model->maxs[2] - e->model->mins[2], 0.f);
			const float half_extent_x = q_max (fabsf (e->model->mins[0]), fabsf (e->model->maxs[0]));
			const float half_extent_y = q_max (fabsf (e->model->mins[1]), fabsf (e->model->maxs[1]));

			num_samples = 0;
			VectorCopy (e->origin, sample_pos[num_samples]);
			sample_weight[num_samples++] = 0.5f;

			if (height > 1.f)
			{
				vec3_t local_offset = {0.f, 0.f, height * 0.25f};
				R_ModelLightBuildSamplePos (e, local_offset, sample_pos[num_samples]);
				sample_weight[num_samples++] = 0.3f;

				VectorSet (local_offset, 0.f, 0.f, height * 0.5f);
				R_ModelLightBuildSamplePos (e, local_offset, sample_pos[num_samples]);
				sample_weight[num_samples++] = 0.2f;
			}

			if ((half_extent_x + half_extent_y) > 48.f)
			{
				const float xofs = CLAMP (8.f, half_extent_x * 0.35f, 32.f);
				const float yofs = CLAMP (8.f, half_extent_y * 0.35f, 32.f);
				vec3_t local_offset;

				VectorSet (local_offset, xofs, 0.f, 0.f);
				R_ModelLightBuildSamplePos (e, local_offset, sample_pos[num_samples]);
				sample_weight[num_samples++] = 0.15f;

				VectorSet (local_offset, -xofs, 0.f, 0.f);
				R_ModelLightBuildSamplePos (e, local_offset, sample_pos[num_samples]);
				sample_weight[num_samples++] = 0.15f;

				VectorSet (local_offset, 0.f, yofs, 0.f);
				R_ModelLightBuildSamplePos (e, local_offset, sample_pos[num_samples]);
				sample_weight[num_samples++] = 0.15f;

				VectorSet (local_offset, 0.f, -yofs, 0.f);
				R_ModelLightBuildSamplePos (e, local_offset, sample_pos[num_samples]);
				sample_weight[num_samples++] = 0.15f;
			}

			used_multisample = num_samples > 1;
		}

		if (R_ModelLightWouldExceedBudget (num_samples))
		{
			num_samples = 1;
			VectorCopy (e->origin, sample_pos[0]);
			sample_weight[0] = 1.f;
			used_multisample = false;
			budget_fallback = true;
		}

		for (int i = 0; i < num_samples; i++)
		{
			entity_static_sample_t sample;
			lightcache_t temp_cache = {0};
			lightcache_t *cache = (i == 0) ? &e->lightcache : &temp_cache;
			const float ofs = (e->model && i > 0) ? (e->model->maxs[2] - e->model->mins[2]) * 0.5f : 0.f;
			const float w = sample_weight[i];

			R_EntityStaticLightSampleAtPoint (e, lightmodel, sample_pos[i], ofs, cache, &sample);
			sample_count++;

			if (!sample.valid)
				continue;

			VectorMA (weighted_color, w, sample.color255, weighted_color);
			total_weight += w;

			if (sample.used_lightgrid)
			{
				VectorMA (lightgrid_effective, w, sample.lightgrid_effective, lightgrid_effective);
				grid_weight += w;
				lightgrid_valid = lightgrid_valid || sample.lightgrid_valid;
				used_lightgrid = true;
			}

			if (sample.used_lightpoint)
			{
				VectorMA (lightpoint_color, w, sample.color255, lightpoint_color);
				point_weight += w;
				used_lightpoint = true;
			}
		}

		if (total_weight > 0.f)
			VectorScale (weighted_color, 1.f / total_weight, out_color255);

		if (grid_weight > 0.f)
			VectorScale (lightgrid_effective, 1.f / grid_weight, lightgrid_effective);

		if (point_weight > 0.f)
			VectorScale (lightpoint_color, 1.f / point_weight, lightpoint_color);
	}

	intensity = R_SampleIntensity255 (out_color255);
	if (intensity <= 0.f && r_minlight_models.value > 0.f && e != &cl.viewent)
	{
		const float minlight = CLAMP (0.f, r_minlight_models.value, 1.f);
		VectorSet (out_color255, minlight * 255.f, minlight * 255.f, minlight * 255.f);
		intensity = minlight;
		used_minlight = true;
	}

	R_ClampSampleColor (out_color255);
	VectorCopy (out_color255, target_color255);
	{
		const float smooth_alpha = CLAMP (0.f, r_model_light_smooth.value, 1.f);
		const qboolean hard_reset = e->forcelink || e->lightcache.static_color_smooth_reset;

		if (smooth_alpha > 0.f && !hard_reset && e->lightcache.static_color_smoothed_valid)
		{
			if (e->lightcache.static_color_smoothed_frame != r_framecount)
			{
				for (int i = 0; i < 3; i++)
					e->lightcache.static_color_smoothed[i] += (target_color255[i] - e->lightcache.static_color_smoothed[i]) * smooth_alpha;
			}
		}
		else
		{
			VectorCopy (target_color255, e->lightcache.static_color_smoothed);
		}

		e->lightcache.static_color_smoothed_valid = true;
		e->lightcache.static_color_smoothed_frame = r_framecount;
		e->lightcache.static_color_smooth_reset = false;
		R_ClampSampleColor (e->lightcache.static_color_smoothed);
		VectorCopy (e->lightcache.static_color_smoothed, out_color255);
	}

	e->lightcache.lightgrid_has_sample = used_lightgrid && lightgrid_valid;
	e->lightcache.lightgrid_ao = e->lightcache.lightgrid_has_sample ? 1.f : 0.f;
	if (e->lightcache.lightgrid_has_sample)
		VectorCopy (lightgrid_effective, e->lightcache.lightgrid_color);
	else
		VectorClear (e->lightcache.lightgrid_color);

	if (used_lightgrid && used_lightpoint)
		static_source = ENTITY_STATIC_LIGHT_MIXED;
	else if (used_lightgrid)
		static_source = ENTITY_STATIC_LIGHT_GRID;
	else if (used_lightpoint)
		static_source = ENTITY_STATIC_LIGHT_POINT;
	else if (used_minlight)
		static_source = ENTITY_STATIC_LIGHT_MINLIGHT;

	if (info)
	{
		for (int i = 0; i < 3; i++)
		{
			float smoothed = out_color255[i] * (1.0f / 256.0f);
			float target = target_color255[i] * (1.0f / 256.0f);
			smoothed = fminf (smoothed, 1.0f);
			target = fminf (target, 1.0f);
			info->static_color[i] = smoothed;
			info->static_target_color[i] = target;
		}
		info->intensity = intensity;
		info->used_lightgrid = used_lightgrid;
		info->lightgrid_valid = lightgrid_valid;
		info->lightgrid_cell_valid = R_LightgridCellForPoint (e->origin, info->lightgrid_cell);
		VectorCopy (e->lightcache.lightgrid_color, info->lightgrid_color);
		info->lightgrid_ao = e->lightcache.lightgrid_ao;
		info->used_lightpoint = used_lightpoint;
		VectorScale (lightpoint_color, 1.f / 255.f, info->lightpoint_color);
		info->used_minlight = used_minlight;
		info->used_multisample = used_multisample;
		info->sample_count = sample_count;
		info->static_source = static_source;
		VectorClear (info->dynamic_color);
		VectorCopy (info->static_color, info->final_color);
	}

	R_ModelLightStats_AddCall (sample_count > 0 ? sample_count : 1, used_multisample, budget_fallback, (Sys_DoubleTime () - start_time) * 1000.0);
	return used_lightgrid || used_lightpoint || used_minlight;
}

const lightgrid_probe_t *R_GetLightgridSample (const vec3_t pos)
{
        static lightgrid_probe_t probe;
        const lightgrid_t *lg = Lightgrid_Get ();

        if (!R_LightgridEnabledInternal (lg))
                return NULL;

        if (!Lightgrid_SampleProbe (lg, pos, &probe))
                return NULL;

        return &probe;
}
