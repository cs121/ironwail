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
#include "sys_jobs.h"
#include "opengl/gl_lightgrid.h"
#include "renderer/r_dlight_pool.h"
#include "renderer/r_envlight.h"
#include <float.h>
#include <math.h>

extern cvar_t r_flatlightstyles; //johnfitz
extern cvar_t r_lerplightstyles;
extern cvar_t r_clustered_lights;
extern cvar_t r_clustered_light_debug;
extern cvar_t r_clustered_light_radius_scale;
extern cvar_t r_clustered_tilesize;
extern cvar_t r_clustered_zslices;
extern cvar_t r_clustered_zslices_low;
extern cvar_t r_clustered_zslices_low_lights;
extern cvar_t r_clustered_maxindices;
extern cvar_t r_clustered_debug;
extern cvar_t r_clustered_log;
extern cvar_t r_clustered_sanity_debug;
extern cvar_t r_clustered_profile;
extern cvar_t r_clustered_profile_dumpinterval;
extern cvar_t r_clustered_validate;
extern cvar_t r_dbg_clustered_force_fallback;
extern cvar_t r_clustered_clearlists;
extern cvar_t r_clustered_barriers;
extern cvar_t r_clustered_force_empty;
extern cvar_t r_clustered_async;
extern cvar_t r_clustered_workers;
extern cvar_t r_clustered_async_debug;
extern cvar_t r_clustered_force_sync;
extern cvar_t r_lightgrid;
extern cvar_t r_lightgrid_force;
extern cvar_t r_rgblighting_enable;
extern cvar_t r_shadow_sun_dir;
extern cvar_t r_shadow_sun_color;
extern cvar_t r_shadow_sun_intensity;
extern cvar_t r_sun_fallback;
extern cvar_t r_sun_force_fallback;
extern cvar_t r_sun_allow_no_sun;
extern cvar_t r_sun_distance;
extern cvar_t r_sun_debug;

cvar_t r_debug_itemlight = { "r_debug_itemlight", "0", CVAR_NONE };
cvar_t r_minlight_models = { "r_minlight_models", "0.02", CVAR_ARCHIVE };
cvar_t r_model_lightgrid = { "r_model_lightgrid", "1", CVAR_ARCHIVE };

gpulightbuffer_t r_lightbuffer;
float r_lightstyle_framefrac;
dlight_t *r_dlight_sources[DLIGHT_GPU_MAX];

static qboolean R_IsFinite (float v)
{
#if defined(_MSC_VER)
        return _finite(v) != 0;
#else
        return isfinite(v);
#endif
}


static void R_ParseDlightColor (const char *value, vec3_t color)
{
	float r = 1.f, g = 1.f, b = 1.f;
	if (value && sscanf (value, "%f %f %f", &r, &g, &b) == 3)
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
	return value && sscanf (value, "%f %f %f", &origin[0], &origin[1], &origin[2]) == 3;
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
	dl->flags = DLIGHTF_DEFAULT;
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


static qboolean R_EntityHasSunClassname (const char *classname)
{
	return !q_strcasecmp (classname, "light_environment")
		|| !q_strcasecmp (classname, "sun")
		|| !q_strcasecmp (classname, "env_sun");
}

static void R_ParseSunColor (const char *value, vec3_t out_color)
{
	float r = 1.f, g = 1.f, b = 1.f;
	if (value && sscanf (value, "%f %f %f", &r, &g, &b) == 3)
	{
		if (r > 2.f || g > 2.f || b > 2.f)
		{
			r *= 1.f / 255.f;
			g *= 1.f / 255.f;
			b *= 1.f / 255.f;
		}
	}
	out_color[0] = CLAMP (0.f, r, 8.f);
	out_color[1] = CLAMP (0.f, g, 8.f);
	out_color[2] = CLAMP (0.f, b, 8.f);
}

static qboolean R_ParseSunDir (const char *value, vec3_t out_dir)
{
	float x, y, z;
	if (!value || sscanf (value, "%f %f %f", &x, &y, &z) != 3)
		return false;
	VectorSet (out_dir, x, y, z);
	if (VectorNormalize (out_dir) == 0.f)
		return false;
	return true;
}

static qboolean R_ParseSunMangle (const char *value, vec3_t out_dir)
{
	vec3_t angles, forward;
	if (!value || sscanf (value, "%f %f %f", &angles[0], &angles[1], &angles[2]) != 3)
		return false;
	AngleVectors (angles, forward, NULL, NULL);
	VectorCopy (forward, out_dir);
	if (VectorNormalize (out_dir) == 0.f)
		return false;
	return true;
}

static qboolean R_CvarDiffersDefault (const cvar_t *var)
{
	if (!var || !var->string || !*var->string)
		return false;
	if (!var->default_string)
		return true;
	return q_strcasecmp (var->string, var->default_string) != 0;
}

void R_UpdateSunFallback (void)
{
	const char *data;
	qboolean map_has_sun = false;
	qboolean map_explicit_no_sun = false;
	qboolean entity_has_sun = false;
	qboolean cvar_has_sun = false;
	qboolean fallback_allowed;
	qboolean fallback_used = false;
	qboolean use_fallback;
	int entity_index = 0;
	qboolean is_worldspawn_entity = false;
	qboolean map_has_color = false;
	float map_intensity = 1.f;
	vec3_t map_dir = { 0.3f, 0.5f, -1.f };
	vec3_t map_color = { 1.f, 1.f, 1.f };
	vec3_t cvar_dir = { 0.3f, 0.5f, -1.f };
	vec3_t cvar_color = { 1.f, 1.f, 1.f };
	float cvar_intensity = 1.f;

	r_sun.enabled = false;
	VectorSet (r_sun.direction, 0.f, 0.f, -1.f);
	VectorSet (r_sun.color, 1.f, 1.f, 1.f);
	r_sun.intensity = 1.f;
	VectorSet (r_sun.virtual_origin, 0.f, 0.f, 8192.f);
	r_sun.source = SUN_SOURCE_NONE;
	VectorSet (r_sun.dir_viewspace, 0.f, 0.f, -1.f);

	if (VectorNormalize (map_dir) == 0.f)
		VectorSet (map_dir, 0.f, 0.f, -1.f);
	if (VectorNormalize (cvar_dir) == 0.f)
		VectorSet (cvar_dir, 0.f, 0.f, -1.f);

	if (cl.worldmodel && cl.worldmodel->entities)
	{
		data = cl.worldmodel->entities;
		data = COM_Parse (data);
		while (data && com_token[0])
		{
			qboolean entity_is_sun = false;
			is_worldspawn_entity = (entity_index == 0);

			if (com_token[0] != '{')
				break;

			while (1)
			{
				char key[64], value[1024];
				data = COM_Parse (data);
				if (!data || !com_token[0])
					break;
				if (com_token[0] == '}')
					break;

				q_strlcpy (key, com_token, sizeof (key));
				if (key[0] == '_')
					memmove (key, key + 1, strlen (key));

				data = COM_ParseEx (data, CPE_ALLOWTRUNC);
				if (!data)
					break;
				q_strlcpy (value, com_token, sizeof (value));

				if (!q_strcasecmp (key, "classname"))
				{
					if (entity_index == 0)
						is_worldspawn_entity = !q_strcasecmp (value, "worldspawn");
					if (R_EntityHasSunClassname (value))
						entity_is_sun = true;
					continue;
				}

				if (!is_worldspawn_entity)
					continue;

				if (!q_strcasecmp (key, "sunlight"))
				{
					map_intensity = q_max (0.f, (float)atof (value));
					map_has_sun = true;
					if (map_intensity <= 0.f)
						map_explicit_no_sun = true;
					continue;
				}
				if (!q_strcasecmp (key, "sunlight_color"))
				{
					R_ParseSunColor (value, map_color);
					map_has_color = true;
					map_has_sun = true;
					continue;
				}
				if (!q_strcasecmp (key, "sun_mangle"))
				{
					R_ParseSunMangle (value, map_dir);
					map_has_sun = true;
					continue;
				}
			}

			if (entity_is_sun)
				entity_has_sun = true;

			entity_index++;
			data = COM_Parse (data);
		}
	}

	if (R_ParseSunDir (r_shadow_sun_dir.string, cvar_dir)
		|| R_CvarDiffersDefault (&r_shadow_sun_color)
		|| R_CvarDiffersDefault (&r_shadow_sun_intensity))
		cvar_has_sun = true;
	R_ParseSunColor (r_shadow_sun_color.string, cvar_color);
	cvar_intensity = q_max (0.f, r_shadow_sun_intensity.value);

	fallback_allowed = (r_sun_fallback.value > 0.f && r_sun_allow_no_sun.value <= 0.f);
	if (r_sun_force_fallback.value > 0.f)
		use_fallback = (r_sun_fallback.value > 0.f);
	else
		use_fallback = (fallback_allowed && !map_has_sun && !entity_has_sun && !cvar_has_sun);

	if (map_has_sun || entity_has_sun)
	{
		r_sun.enabled = !map_explicit_no_sun;
		VectorCopy (map_dir, r_sun.direction);
		if (map_has_color)
			VectorCopy (map_color, r_sun.color);
		r_sun.intensity = map_intensity;
		r_sun.source = SUN_SOURCE_MAP;
	}
	else if (cvar_has_sun)
	{
		r_sun.enabled = (cvar_intensity > 0.f);
		VectorCopy (cvar_dir, r_sun.direction);
		VectorCopy (cvar_color, r_sun.color);
		r_sun.intensity = cvar_intensity;
		r_sun.source = SUN_SOURCE_CVAR;
	}
	else if (use_fallback)
	{
		VectorSet (r_sun.direction, -0.3f, -0.6f, -0.7f);
		if (VectorNormalize (r_sun.direction) == 0.f)
			VectorSet (r_sun.direction, 0.f, 0.f, -1.f);
		VectorSet (r_sun.color, 1.f, 1.f, 1.f);
		r_sun.intensity = 1.f;
		r_sun.enabled = true;
		r_sun.source = SUN_SOURCE_FALLBACK;
		fallback_used = true;
	}

	if (fallback_used)
		Con_Printf ("Sun: no map sun found, using fallback origin (0 0 8192), dir (-0.3 -0.6 -0.7)\n");

	if (r_sun.enabled && VectorNormalize (r_sun.direction) == 0.f)
	{
		VectorSet (r_sun.direction, 0.f, 0.f, -1.f);
	}

	if (r_sun_debug.value > 0.f)
	{
		const char *source = "NONE";
		if (r_sun.source == SUN_SOURCE_MAP) source = "MAP";
		else if (r_sun.source == SUN_SOURCE_CVAR) source = "CVAR";
		else if (r_sun.source == SUN_SOURCE_FALLBACK) source = "FALLBACK";
		Con_Printf ("SunState: source=%s enabled=%d dir=(%.3f %.3f %.3f) color=(%.3f %.3f %.3f) intensity=%.3f\n",
			source, r_sun.enabled ? 1 : 0,
			r_sun.direction[0], r_sun.direction[1], r_sun.direction[2],
			r_sun.color[0], r_sun.color[1], r_sun.color[2], r_sun.intensity);
	}
}

void R_UpdateSunVirtualOrigin (void)
{
	float dist = q_max (1.f, r_sun_distance.value);
	VectorMA (r_refdef.vieworg, -dist, r_sun.direction, r_sun.virtual_origin);
	r_sun.dir_viewspace[0] = DotProduct (r_sun.direction, vright);
	r_sun.dir_viewspace[1] = DotProduct (r_sun.direction, vup);
	r_sun.dir_viewspace[2] = DotProduct (r_sun.direction, vpn);
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
		else if (r_flatlightstyles.value == 1 || !r_clustered_lights.value)
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

typedef struct clustered_header_s {
	GLuint offset;
	GLuint count;
} clustered_header_t;

typedef struct clustered_light_s {
	vec4_t pos_radius;
	vec4_t color_intensity;
	int flags[4];
} clustered_light_t;

typedef struct clustered_params_s {
	int screen_size[2];
	int grid_xy[2];
	int z_slices;
	int clustered_enabled;
	float near_plane;
	float far_plane;
	float z_log_scale;
	float z_log_bias;
	int _pad0[2];
	float view_matrix[16];
	float proj_matrix[16];
	float inv_proj[16];
	int tile_size;
	int debug_mode;
	int _pad1[2];
} clustered_params_t;

typedef struct cluster_runtime_params_s {
	int tile_size;
	int grid_x;
	int grid_y;
	int z_slices;
	int screen_w;
	int screen_h;
	float z_log_scale;
	float z_log_bias;
	float near_plane;
	float far_plane;
} cluster_runtime_params_t;

COMPILE_TIME_ASSERT (clustered_header_size, sizeof (clustered_header_t) == 8);
COMPILE_TIME_ASSERT (clustered_light_size, sizeof (clustered_light_t) == 48);
COMPILE_TIME_ASSERT (clustered_params_size, sizeof (clustered_params_t) == 256);

static struct {
	GLuint lights_ssbo;
	GLuint headers_ssbo;
	GLuint indices_ssbo;
	GLuint counters_ssbo;
	GLuint temp_counts_ssbo;
	GLuint params_ubo;
	int cluster_count;
	int grid_x;
	int grid_y;
	int z_slices;
	int max_indices;
	int max_lights;
	qboolean has_frame_data;
	qboolean shading_enabled;
	qboolean shading_bound;
	GLuint bound_lights_ssbo;
	GLuint bound_headers_ssbo;
	GLuint bound_indices_ssbo;
	GLuint bound_counters_ssbo;
	GLuint bound_params_ubo;
	GLsizeiptr bound_lights_size;
	GLsizeiptr bound_headers_size;
	GLsizeiptr bound_indices_size;
	GLsizeiptr bound_counters_size;
	GLsizeiptr bound_params_size;
	double last_overflow_log;
	qboolean available;
	int build_calls;
	int upload_calls;
	int bind_calls;
	int last_frame_built;
	int last_frame_uploaded;
	cluster_runtime_params_t params;
	GLuint debug_indices_written_count;
	double build_ms;
	double upload_ms;
	double bind_ms_total;
	int num_glget_calls_in_bind;
	int num_buffer_allocs;
	int num_barriers;
	unsigned int last_upload_light_count;
	qboolean build_ran_this_frame;
	qboolean bind_ran_this_frame;
} r_clustered;

qboolean R_ClusteredShadingActive (void)
{
	return r_clustered.shading_enabled;
}

unsigned int R_ClusteredSubmittedLightCount (void)
{
	return (unsigned int)r_framedata.numlights;
}

static double R_ClusteredNowMS (void)
{
	return Sys_DoubleTime () * 1000.0;
}

static clustered_header_t *r_clustered_headers_cpu;
static int r_clustered_debug_light_cluster_hits[DLIGHT_GPU_MAX];

typedef struct clustered_snapshot_s {
	cluster_runtime_params_t runtime;
	int num_lights;
	clustered_light_t lights[DLIGHT_GPU_MAX];
	float view_matrix[16];
	float proj_matrix[16];
	int debug_mode;
} clustered_snapshot_t;

typedef struct clustered_job_output_s {
	int cluster_begin;
	int cluster_end;
	clustered_header_t *headers;
	GLuint *indices;
	int max_indices;
	int index_count;
	qboolean overflowed;
} clustered_job_output_t;

typedef struct clustered_async_frame_s {
	clustered_snapshot_t snapshot;
	clustered_job_output_t jobs[8];
	int num_jobs;
	int completed_jobs;
	qboolean pending;
	qboolean ready;
	qboolean overflowed;
	double cpu_build_ms;
	int output_bytes;
} clustered_async_frame_t;

static struct {
	sys_job_queue_t *queue;
	SDL_mutex *mutex;
	clustered_async_frame_t frames[3];
	int submit_slot;
	int commit_slot;
	int workers;
	qboolean initialized;
	unsigned int submitted;
	unsigned int completed;
} r_clustered_async_state;

static int r_clustered_debug_cluster_count;
static int r_clustered_debug_index_count;

#define CLUSTER_PERF_RING 16

typedef enum {
	CLCPU_PUSH_TOTAL = 0,
	CLCPU_DLIGHT_FILTER,
	CLCPU_LIGHT_UPLOAD,
	CLCPU_CLUSTER_BUILD_CPU,
	CLCPU_CLUSTER_BIND_BARRIER,
	CLCPU_WORLD,
	CLCPU_ALIAS,
	CLCPU_COUNT
} cluster_cpu_block_t;

typedef enum {
	CLGPU_CLUSTER_CLEAR = 0,
	CLGPU_CLUSTER_BUILD,
	CLGPU_WORLD,
	CLGPU_ALIAS,
	CLGPU_SHADOW_DLIGHT,
	CLGPU_COUNT
} cluster_gpu_block_t;

typedef struct cluster_perf_sample_s {
	double cpu_ms[CLCPU_COUNT];
	GLuint gpu_queries[CLGPU_COUNT];
	qboolean gpu_open[CLGPU_COUNT];
	qboolean gpu_issued[CLGPU_COUNT];
	qboolean valid;
	int frame_index;
} cluster_perf_sample_t;

static struct {
	qboolean inited;
	qboolean gpu_supported;
	int frame_counter;
	int current_slot;
	double cpu_begin[CLCPU_COUNT];
	double gpu_sum_ms[CLGPU_COUNT];
	double gpu_max_ms[CLGPU_COUNT];
	double cpu_sum_ms[CLCPU_COUNT];
	double cpu_max_ms[CLCPU_COUNT];
	int agg_samples;
	cluster_perf_sample_t slots[CLUSTER_PERF_RING];
} r_cluster_perf;

static void R_ClusterPerf_Init (void)
{
	int s, b;
	if (r_cluster_perf.inited)
		return;
	memset (&r_cluster_perf, 0, sizeof (r_cluster_perf));
	r_cluster_perf.inited = true;
	r_cluster_perf.gpu_supported = (GL_GenQueriesFunc != NULL && GL_BeginQueryFunc != NULL && GL_EndQueryFunc != NULL && GL_GetQueryObjectuivFunc != NULL && GL_GetQueryObjectui64vFunc != NULL);
	if (!r_cluster_perf.gpu_supported)
		return;
	for (s = 0; s < CLUSTER_PERF_RING; ++s)
		for (b = 0; b < CLGPU_COUNT; ++b)
			GL_GenQueriesFunc (1, &r_cluster_perf.slots[s].gpu_queries[b]);
}

static void R_ClusterPerf_Destroy (void)
{
	int s, b;
	if (!r_cluster_perf.inited)
		return;
	if (r_cluster_perf.gpu_supported && GL_DeleteQueriesFunc)
	{
		for (s = 0; s < CLUSTER_PERF_RING; ++s)
			for (b = 0; b < CLGPU_COUNT; ++b)
				GL_DeleteQueriesFunc (1, &r_cluster_perf.slots[s].gpu_queries[b]);
	}
	memset (&r_cluster_perf, 0, sizeof (r_cluster_perf));
}

static void R_ClusterPerf_BeginCPU (cluster_cpu_block_t block)
{
	cluster_perf_sample_t *slot;
	if (r_clustered_profile.value <= 0.f)
		return;
	R_ClusterPerf_Init ();
	slot = &r_cluster_perf.slots[r_cluster_perf.current_slot];
	r_cluster_perf.cpu_begin[block] = Sys_DoubleTime ();
}

static void R_ClusterPerf_EndCPU (cluster_cpu_block_t block)
{
	cluster_perf_sample_t *slot;
	if (r_clustered_profile.value <= 0.f)
		return;
	slot = &r_cluster_perf.slots[r_cluster_perf.current_slot];
	slot->cpu_ms[block] += (Sys_DoubleTime () - r_cluster_perf.cpu_begin[block]) * 1000.0;
	if (slot->cpu_ms[block] > r_cluster_perf.cpu_max_ms[block])
		r_cluster_perf.cpu_max_ms[block] = slot->cpu_ms[block];
}

static void R_ClusterPerf_BeginGPU (cluster_gpu_block_t block)
{
	cluster_perf_sample_t *slot;
	if (r_clustered_profile.value <= 0.f)
		return;
	R_ClusterPerf_Init ();
	if (!r_cluster_perf.gpu_supported)
		return;
	slot = &r_cluster_perf.slots[r_cluster_perf.current_slot];
	GL_BeginQueryFunc (GL_TIME_ELAPSED, slot->gpu_queries[block]);
	slot->gpu_open[block] = true;
}

static void R_ClusterPerf_EndGPU (cluster_gpu_block_t block)
{
	cluster_perf_sample_t *slot;
	if (r_clustered_profile.value <= 0.f)
		return;
	if (!r_cluster_perf.gpu_supported)
		return;
	slot = &r_cluster_perf.slots[r_cluster_perf.current_slot];
	if (slot->gpu_open[block])
	{
		GL_EndQueryFunc (GL_TIME_ELAPSED);
		slot->gpu_open[block] = false;
		slot->gpu_issued[block] = true;
	}
}

static void R_ClusterPerf_DumpIfNeeded (void)
{
	int i;
	int interval;
	if (r_clustered_profile.value <= 0.f)
		return;
	interval = q_max (1, (int)r_clustered_profile_dumpinterval.value);
	if (r_cluster_perf.agg_samples <= 0 || (r_cluster_perf.frame_counter % interval) != 0)
		return;
	Con_Printf ("CLPROFILE avg/max over %d frames lights=%u grid=(%d %d %d) tile=%d | CPU(ms): push %.3f/%.3f filter %.3f/%.3f upload %.3f/%.3f build %.3f/%.3f bind+barrier %.3f/%.3f world %.3f/%.3f alias %.3f/%.3f\n",
		r_cluster_perf.agg_samples,
		r_framedata.numlights,
		r_clustered.grid_x,
		r_clustered.grid_y,
		r_clustered.z_slices,
		(int)r_clustered_tilesize.value,
		r_cluster_perf.cpu_sum_ms[CLCPU_PUSH_TOTAL] / r_cluster_perf.agg_samples, r_cluster_perf.cpu_max_ms[CLCPU_PUSH_TOTAL],
		r_cluster_perf.cpu_sum_ms[CLCPU_DLIGHT_FILTER] / r_cluster_perf.agg_samples, r_cluster_perf.cpu_max_ms[CLCPU_DLIGHT_FILTER],
		r_cluster_perf.cpu_sum_ms[CLCPU_LIGHT_UPLOAD] / r_cluster_perf.agg_samples, r_cluster_perf.cpu_max_ms[CLCPU_LIGHT_UPLOAD],
		r_cluster_perf.cpu_sum_ms[CLCPU_CLUSTER_BUILD_CPU] / r_cluster_perf.agg_samples, r_cluster_perf.cpu_max_ms[CLCPU_CLUSTER_BUILD_CPU],
		r_cluster_perf.cpu_sum_ms[CLCPU_CLUSTER_BIND_BARRIER] / r_cluster_perf.agg_samples, r_cluster_perf.cpu_max_ms[CLCPU_CLUSTER_BIND_BARRIER],
		r_cluster_perf.cpu_sum_ms[CLCPU_WORLD] / r_cluster_perf.agg_samples, r_cluster_perf.cpu_max_ms[CLCPU_WORLD],
		r_cluster_perf.cpu_sum_ms[CLCPU_ALIAS] / r_cluster_perf.agg_samples, r_cluster_perf.cpu_max_ms[CLCPU_ALIAS]);
	Con_Printf ("CLPROFILE avg/max over %d frames | GPU(ms): clear %.3f/%.3f build %.3f/%.3f world %.3f/%.3f alias %.3f/%.3f shadow %.3f/%.3f\n",
		r_cluster_perf.agg_samples,
		r_cluster_perf.gpu_sum_ms[CLGPU_CLUSTER_CLEAR] / r_cluster_perf.agg_samples, r_cluster_perf.gpu_max_ms[CLGPU_CLUSTER_CLEAR],
		r_cluster_perf.gpu_sum_ms[CLGPU_CLUSTER_BUILD] / r_cluster_perf.agg_samples, r_cluster_perf.gpu_max_ms[CLGPU_CLUSTER_BUILD],
		r_cluster_perf.gpu_sum_ms[CLGPU_WORLD] / r_cluster_perf.agg_samples, r_cluster_perf.gpu_max_ms[CLGPU_WORLD],
		r_cluster_perf.gpu_sum_ms[CLGPU_ALIAS] / r_cluster_perf.agg_samples, r_cluster_perf.gpu_max_ms[CLGPU_ALIAS],
		r_cluster_perf.gpu_sum_ms[CLGPU_SHADOW_DLIGHT] / r_cluster_perf.agg_samples, r_cluster_perf.gpu_max_ms[CLGPU_SHADOW_DLIGHT]);
	for (i = 0; i < CLCPU_COUNT; ++i)
		r_cluster_perf.cpu_sum_ms[i] = r_cluster_perf.cpu_max_ms[i] = 0.0;
	for (i = 0; i < CLGPU_COUNT; ++i)
		r_cluster_perf.gpu_sum_ms[i] = r_cluster_perf.gpu_max_ms[i] = 0.0;
	r_cluster_perf.agg_samples = 0;
}

static void R_ClusterPerf_Reason (const char *reason)
{
	if (r_clustered_profile.value <= 0.f)
		return;
	Con_Printf ("CLPROFILE reason frame=%d numlights=%u %s\n", r_framecount, r_framedata.numlights, reason);
}

void R_ClusterPerf_BeginFrame (void)
{
	cluster_perf_sample_t *slot;
	int b;
	if (r_clustered_profile.value <= 0.f)
		return;
	R_ClusterPerf_Init ();
	r_cluster_perf.current_slot = r_cluster_perf.frame_counter % CLUSTER_PERF_RING;
	slot = &r_cluster_perf.slots[r_cluster_perf.current_slot];
	memset (slot->cpu_ms, 0, sizeof (slot->cpu_ms));
	memset (slot->gpu_open, 0, sizeof (slot->gpu_open));
	memset (slot->gpu_issued, 0, sizeof (slot->gpu_issued));
	for (b = 0; b < CLGPU_COUNT; ++b)
	{
		slot->gpu_open[b] = false;
		slot->gpu_issued[b] = false;
	}
	slot->valid = false;
	slot->frame_index = r_cluster_perf.frame_counter;
}

void R_ClusterPerf_EndFrame (void)
{
	int resolve_slot;
	cluster_perf_sample_t *slot;
	if (r_clustered_profile.value <= 0.f)
		return;
	slot = &r_cluster_perf.slots[r_cluster_perf.current_slot];
	slot->valid = true;
	r_cluster_perf.frame_counter++;
	resolve_slot = (r_cluster_perf.frame_counter - 4 + CLUSTER_PERF_RING) % CLUSTER_PERF_RING;
	slot = &r_cluster_perf.slots[resolve_slot];
	if (slot->valid)
	{
		int b;
		double gpu_ms[CLGPU_COUNT] = {0};
		qboolean gpu_ready = r_cluster_perf.gpu_supported;
		for (b = 0; b < CLGPU_COUNT; ++b)
		{
			if (!slot->gpu_issued[b])
				continue;
			if (r_cluster_perf.gpu_supported)
			{
				GLuint available = 0;
				GL_GetQueryObjectuivFunc (slot->gpu_queries[b], GL_QUERY_RESULT_AVAILABLE, &available);
				if (!available)
				{
					gpu_ready = false;
					break;
				}
			}
		}
		if (gpu_ready)
		{
			for (b = 0; b < CLGPU_COUNT; ++b)
			{
				if (!slot->gpu_issued[b])
					continue;
				if (r_cluster_perf.gpu_supported)
				{
					GLuint64 ns = 0;
					GL_GetQueryObjectui64vFunc (slot->gpu_queries[b], GL_QUERY_RESULT, &ns);
					gpu_ms[b] = (double)ns * (1.0 / 1000000.0);
				}
			}
			for (b = 0; b < CLCPU_COUNT; ++b)
			{
				r_cluster_perf.cpu_sum_ms[b] += slot->cpu_ms[b];
				if (slot->cpu_ms[b] > r_cluster_perf.cpu_max_ms[b])
					r_cluster_perf.cpu_max_ms[b] = slot->cpu_ms[b];
			}
			for (b = 0; b < CLGPU_COUNT; ++b)
			{
				r_cluster_perf.gpu_sum_ms[b] += gpu_ms[b];
				if (gpu_ms[b] > r_cluster_perf.gpu_max_ms[b])
					r_cluster_perf.gpu_max_ms[b] = gpu_ms[b];
			}
			r_cluster_perf.agg_samples++;
			R_ClusterPerf_DumpIfNeeded ();
			slot->valid = false;
		}
	}
}

void R_ClusterPerf_BeginPush (void) { R_ClusterPerf_BeginCPU (CLCPU_PUSH_TOTAL); }
void R_ClusterPerf_EndPush (void) { R_ClusterPerf_EndCPU (CLCPU_PUSH_TOTAL); }
void R_ClusterPerf_BeginBuild (void) { R_ClusterPerf_BeginCPU (CLCPU_CLUSTER_BUILD_CPU); }
void R_ClusterPerf_EndBuild (void) { R_ClusterPerf_EndCPU (CLCPU_CLUSTER_BUILD_CPU); }
void R_ClusterPerf_BeginWorld (void) { R_ClusterPerf_BeginCPU (CLCPU_WORLD); R_ClusterPerf_BeginGPU (CLGPU_WORLD); }
void R_ClusterPerf_EndWorld (void) { R_ClusterPerf_EndGPU (CLGPU_WORLD); R_ClusterPerf_EndCPU (CLCPU_WORLD); }
void R_ClusterPerf_BeginAlias (void) { R_ClusterPerf_BeginCPU (CLCPU_ALIAS); R_ClusterPerf_BeginGPU (CLGPU_ALIAS); }
void R_ClusterPerf_EndAlias (void) { R_ClusterPerf_EndGPU (CLGPU_ALIAS); R_ClusterPerf_EndCPU (CLCPU_ALIAS); }
void R_ClusterPerf_BeginDebug (void) { }
void R_ClusterPerf_EndDebug (void) { }
void R_ClusterPerf_BeginPost (void) { }
void R_ClusterPerf_EndPost (void) { }
void R_ClusterPerf_BeginLightFilter (void) { R_ClusterPerf_BeginCPU (CLCPU_DLIGHT_FILTER); }
void R_ClusterPerf_EndLightFilter (void) { R_ClusterPerf_EndCPU (CLCPU_DLIGHT_FILTER); }
void R_ClusterPerf_BeginLightUpload (void) { R_ClusterPerf_BeginCPU (CLCPU_LIGHT_UPLOAD); }
void R_ClusterPerf_EndLightUpload (void) { R_ClusterPerf_EndCPU (CLCPU_LIGHT_UPLOAD); }
void R_ClusterPerf_BeginClusterBindBarrier (void) { R_ClusterPerf_BeginCPU (CLCPU_CLUSTER_BIND_BARRIER); }
void R_ClusterPerf_EndClusterBindBarrier (void) { R_ClusterPerf_EndCPU (CLCPU_CLUSTER_BIND_BARRIER); }
void R_ClusterPerf_BeginClusterClearGPU (void) { R_ClusterPerf_BeginGPU (CLGPU_CLUSTER_CLEAR); }
void R_ClusterPerf_EndClusterClearGPU (void) { R_ClusterPerf_EndGPU (CLGPU_CLUSTER_CLEAR); }
void R_ClusterPerf_BeginClusterBuildGPU (void) { R_ClusterPerf_BeginGPU (CLGPU_CLUSTER_BUILD); }
void R_ClusterPerf_EndClusterBuildGPU (void) { R_ClusterPerf_EndGPU (CLGPU_CLUSTER_BUILD); }
void R_ClusterPerf_BeginShadowDlightGPU (void) { R_ClusterPerf_BeginGPU (CLGPU_SHADOW_DLIGHT); }
void R_ClusterPerf_EndShadowDlightGPU (void) { R_ClusterPerf_EndGPU (CLGPU_SHADOW_DLIGHT); }
void R_ClusterPerf_MarkReason (const char *reason) { R_ClusterPerf_Reason (reason); }

static qboolean R_ClusteredShouldLog (void)
{
	return r_clustered_log.value > 0.f || r_clustered_debug.value > 0.f;
}

static void R_ClusteredBarrier (GLbitfield bits)
{
	if (r_clustered_barriers.value <= 0.f && developer.value > 0.f && R_ClusteredShouldLog ())
		R_ClusterPerf_MarkReason ("r_clustered_barriers=0 ignored (memory barriers always enabled)");
	r_clustered.num_barriers++;
	GL_MemoryBarrierFunc (bits);
}

static void R_ClusteredLabelBuffer (GLuint buffer, const char *name)
{
	if (!buffer || !name || !name[0] || !GL_ObjectLabelFunc)
		return;
	GL_ObjectLabelFunc (GL_BUFFER, buffer, -1, name);
}

static void R_ClusteredValidateBinding (GLenum target, GLuint binding, GLuint expected, const char *name)
{
	if (!R_ClusteredShouldLog ())
		return;

	// This validation helper is debug-only. Keep logging lightweight here to
	// avoid calling indexed binding query entry points that may not be declared
	// by all platform GL headers.
	Con_DPrintf ("CLUSTERDBG binding_check target=0x%X binding=%u expected=%u name=%s\n",
		(unsigned)target, binding, expected, name ? name : "<unnamed>");
}

static void R_ClusteredDebugDumpUpload (const clustered_light_t *lights_local, int light_count)
{
	const int ssbo_size = (int)(sizeof (clustered_light_t) * (size_t)r_clustered.max_lights);
	const int ubo_size = sizeof (clustered_params_t);
	int show = q_min (light_count, 3);

	if (!R_ClusteredShouldLog ())
		return;

	Con_Printf ("CLUSTERDBG upload numlights=%d lights_ssbo=%u size=%d bind_ssbo3=%u headers_ssbo=%u bind_ssbo4=%u indices_ssbo=%u bind_ssbo5=%u params_ubo=%u size=%d bind_ubo2=%u\n",
		light_count,
		r_clustered.lights_ssbo,
		ssbo_size,
		r_clustered.lights_ssbo,
		r_clustered.headers_ssbo,
		r_clustered.headers_ssbo,
		r_clustered.indices_ssbo,
		r_clustered.indices_ssbo,
		r_clustered.params_ubo,
		ubo_size,
		r_clustered.params_ubo);

	for (int i = 0; i < show; i++)
	{
		Con_Printf ("CLUSTERDBG cpu_light[%d] org=(%.2f %.2f %.2f) radius=%.2f color=(%.2f %.2f %.2f)\n",
			i,
			lights_local[i].pos_radius[0], lights_local[i].pos_radius[1], lights_local[i].pos_radius[2], lights_local[i].pos_radius[3],
			lights_local[i].color_intensity[0], lights_local[i].color_intensity[1], lights_local[i].color_intensity[2]);
	}

	if (show > 0 && r_clustered_debug.value >= 2.f)
	{
		const clustered_light_t *mapped;
		GL_BindBufferFunc (GL_SHADER_STORAGE_BUFFER, r_clustered.lights_ssbo);
		mapped = (const clustered_light_t *)GL_MapBufferRangeFunc (GL_SHADER_STORAGE_BUFFER, 0,
			(GLsizeiptr)(sizeof (clustered_light_t) * (size_t)show), GL_MAP_READ_BIT);
		if (mapped)
		{
			for (int i = 0; i < show; i++)
			{
				Con_Printf ("CLUSTERDBG gpu_light[%d] org=(%.2f %.2f %.2f) radius=%.2f color=(%.2f %.2f %.2f)\n",
					i,
					mapped[i].pos_radius[0], mapped[i].pos_radius[1], mapped[i].pos_radius[2], mapped[i].pos_radius[3],
					mapped[i].color_intensity[0], mapped[i].color_intensity[1], mapped[i].color_intensity[2]);
			}
			GL_UnmapBufferFunc (GL_SHADER_STORAGE_BUFFER);
		}
	}
}

void R_GetClusterDlightDebugStats (int *out_clusters, int *out_indices, const int **out_light_hits, int *out_max_hits);

typedef struct gpu_cluster_inputs_s {
	int pass_mode;
	int num_lights;
	int _pad0;
	int _pad1;
} gpu_cluster_inputs_t;

static void R_Clustered_Shutdown (void);
static qboolean R_ClusteredShouldLog (void);
static void R_ClusteredAsync_InitIfNeeded (void);
static void R_ClusteredAsync_Shutdown (void);
static void R_ClusteredAsync_EnsureBuffers (void);

static qboolean R_ClusteredEnabled (void)
{
	return r_clustered.available;
}

void R_Clustered_Init (void)
{
	memset (&r_clustered, 0, sizeof (r_clustered));

	if (!glprogs.cluster_lights || !glprogs.cluster_prefix)
		return;
	R_ClusterPerf_Init ();

	GL_GenBuffersFunc (1, &r_clustered.lights_ssbo);
	GL_GenBuffersFunc (1, &r_clustered.headers_ssbo);
	GL_GenBuffersFunc (1, &r_clustered.indices_ssbo);
	GL_GenBuffersFunc (1, &r_clustered.counters_ssbo);
	GL_GenBuffersFunc (1, &r_clustered.temp_counts_ssbo);
	GL_GenBuffersFunc (1, &r_clustered.params_ubo);

	r_clustered.available = (r_clustered.lights_ssbo && r_clustered.headers_ssbo &&
		r_clustered.indices_ssbo && r_clustered.counters_ssbo &&
		r_clustered.temp_counts_ssbo && r_clustered.params_ubo);

	if (!r_clustered.available)
	{
		Con_Printf ("Clustered lighting init failed; clustered shading disabled.\n");
		R_Clustered_Shutdown ();
		return;
	}

	R_ClusteredLabelBuffer (r_clustered.lights_ssbo, "cluster lights");
	R_ClusteredLabelBuffer (r_clustered.headers_ssbo, "cluster headers");
	R_ClusteredLabelBuffer (r_clustered.indices_ssbo, "cluster indices");
	R_ClusteredLabelBuffer (r_clustered.counters_ssbo, "cluster counters");
	R_ClusteredLabelBuffer (r_clustered.temp_counts_ssbo, "cluster temp counts");
	R_ClusteredLabelBuffer (r_clustered.params_ubo, "cluster params");
	r_clustered.shading_bound = false;
	r_clustered.has_frame_data = false;
	r_clustered.shading_enabled = false;
	R_ClusteredAsync_InitIfNeeded ();
}

static void R_ClusteredEnsureCapacity (int grid_x, int grid_y, int z_slices)
{
	const int cluster_count = q_max (1, grid_x * grid_y * z_slices);
	const int max_indices = q_max (1024, (int)r_clustered_maxindices.value);
	const int max_lights = DLIGHT_GPU_MAX;

	if (r_clustered.cluster_count == cluster_count &&
		r_clustered.max_indices == max_indices &&
		r_clustered.max_lights == max_lights)
		return;

	r_clustered.cluster_count = cluster_count;
	r_clustered.grid_x = grid_x;
	r_clustered.grid_y = grid_y;
	r_clustered.z_slices = z_slices;
	r_clustered.max_indices = max_indices;
	r_clustered.max_lights = max_lights;
	r_clustered.shading_bound = false;

	free (r_clustered_headers_cpu);
	r_clustered_headers_cpu = (clustered_header_t *)calloc ((size_t)cluster_count, sizeof (*r_clustered_headers_cpu));

	GL_BindBufferFunc (GL_SHADER_STORAGE_BUFFER, r_clustered.lights_ssbo);
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(sizeof (clustered_light_t) * (size_t)max_lights), NULL, GL_DYNAMIC_DRAW);

	GL_BindBufferFunc (GL_SHADER_STORAGE_BUFFER, r_clustered.headers_ssbo);
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(sizeof (clustered_header_t) * (size_t)cluster_count), NULL, GL_DYNAMIC_DRAW);

	GL_BindBufferFunc (GL_SHADER_STORAGE_BUFFER, r_clustered.indices_ssbo);
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(sizeof (GLuint) * (size_t)max_indices), NULL, GL_DYNAMIC_DRAW);

	GL_BindBufferFunc (GL_SHADER_STORAGE_BUFFER, r_clustered.temp_counts_ssbo);
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(sizeof (GLuint) * (size_t)cluster_count), NULL, GL_DYNAMIC_DRAW);

	GL_BindBufferFunc (GL_SHADER_STORAGE_BUFFER, r_clustered.counters_ssbo);
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(sizeof (GLuint) * 2), NULL, GL_DYNAMIC_DRAW);

	GL_BindBufferFunc (GL_UNIFORM_BUFFER, r_clustered.params_ubo);
	GL_BufferDataFunc (GL_UNIFORM_BUFFER, sizeof (clustered_params_t), NULL, GL_DYNAMIC_DRAW);

	r_clustered.num_buffer_allocs += 6;

	GL_BindBufferFunc (GL_SHADER_STORAGE_BUFFER, 0);
	GL_BindBufferFunc (GL_UNIFORM_BUFFER, 0);
}

void R_Clustered_Shutdown (void)
{
	if (r_clustered.lights_ssbo)
		GL_DeleteBuffersFunc (1, &r_clustered.lights_ssbo);
	if (r_clustered.headers_ssbo)
		GL_DeleteBuffersFunc (1, &r_clustered.headers_ssbo);
	if (r_clustered.indices_ssbo)
		GL_DeleteBuffersFunc (1, &r_clustered.indices_ssbo);
	if (r_clustered.counters_ssbo)
		GL_DeleteBuffersFunc (1, &r_clustered.counters_ssbo);
	if (r_clustered.temp_counts_ssbo)
		GL_DeleteBuffersFunc (1, &r_clustered.temp_counts_ssbo);
	if (r_clustered.params_ubo)
		GL_DeleteBuffersFunc (1, &r_clustered.params_ubo);
	memset (&r_clustered, 0, sizeof (r_clustered));
	R_ClusterPerf_Destroy ();
	free (r_clustered_headers_cpu);
	r_clustered_headers_cpu = NULL;
	memset (r_clustered_debug_light_cluster_hits, 0, sizeof (r_clustered_debug_light_cluster_hits));
	r_clustered_debug_cluster_count = 0;
	r_clustered_debug_index_count = 0;
	R_ClusteredAsync_Shutdown ();
}



static int R_Clustered_SelectZSlices (unsigned int numlights)
{
	int z_slices = CLAMP (4, (int)r_clustered_zslices.value, 128);
	const int low_slices = CLAMP (4, (int)r_clustered_zslices_low.value, 128);
	const int low_light_threshold = CLAMP (0, (int)r_clustered_zslices_low_lights.value, DLIGHT_GPU_MAX);

	if (numlights > 0 && numlights <= (unsigned int)low_light_threshold)
		z_slices = q_min (z_slices, low_slices);

	return z_slices;
}

static void R_ClusteredBindRangeCached (GLenum target, GLuint index, GLuint buffer, GLsizeiptr size, GLuint *cache_buffer, GLsizeiptr *cache_size)
{
	if (*cache_buffer == buffer && *cache_size == size)
		return;
	GL_BindBufferRange (target, index, buffer, 0, size);
	*cache_buffer = buffer;
	*cache_size = size;
}

static void R_ClusteredComputeRuntimeParams (cluster_runtime_params_t *out)
{
	int tile_size;
	if (!out)
		return;

	tile_size = (int)r_clustered_tilesize.value;
	if (tile_size <= 8)
		tile_size = 8;
	else if (tile_size <= 16)
		tile_size = 16;
	else if (tile_size <= 32)
		tile_size = 32;
	else if (tile_size <= 64)
		tile_size = 64;
	else
		tile_size = 128;

	out->tile_size = tile_size;
	out->z_slices = R_Clustered_SelectZSlices (r_framedata.numlights);
	out->screen_w = vid.width;
	out->screen_h = vid.height;
	out->grid_x = q_max (1, (out->screen_w + tile_size - 1) / tile_size);
	out->grid_y = q_max (1, (out->screen_h + tile_size - 1) / tile_size);
	out->z_log_scale = r_framedata.zparams[0];
	out->z_log_bias = r_framedata.zparams[1];
	if (out->z_log_scale != 0.f)
	{
		out->near_plane = exp2f (-out->z_log_bias / out->z_log_scale);
		out->far_plane = exp2f ((float)out->z_slices / out->z_log_scale - out->z_log_bias / out->z_log_scale);
	}
	else
	{
		out->near_plane = 0.5f;
		out->far_plane = 4096.f;
	}
}

static void R_ClusteredFillParamsUBO (clustered_params_t *params, const cluster_runtime_params_t *runtime)
{
	if (!params || !runtime)
		return;
	memset (params, 0, sizeof (*params));
	params->screen_size[0] = runtime->screen_w;
	params->screen_size[1] = runtime->screen_h;
	params->grid_xy[0] = runtime->grid_x;
	params->grid_xy[1] = runtime->grid_y;
	params->z_slices = runtime->z_slices;
	params->clustered_enabled = r_clustered.shading_enabled ? 1 : 0;
	params->near_plane = runtime->near_plane;
	params->far_plane = runtime->far_plane;
	params->z_log_scale = runtime->z_log_scale;
	params->z_log_bias = runtime->z_log_bias;
	memcpy (params->view_matrix, r_matview, sizeof (params->view_matrix));
	memcpy (params->proj_matrix, r_matproj, sizeof (params->proj_matrix));
	memset (params->inv_proj, 0, sizeof (params->inv_proj));
	params->inv_proj[0] = 1.f;
	params->inv_proj[5] = 1.f;
	params->inv_proj[10] = 1.f;
	params->inv_proj[15] = 1.f;
	params->tile_size = runtime->tile_size;
	params->debug_mode = (int)r_clustered_debug.value;
}

static void R_ClusteredResetFrameState (const cluster_runtime_params_t *runtime)
{
	clustered_params_t params;
	double upload_start_ms;

	r_clustered.shading_enabled = false;
	r_clustered.last_upload_light_count = 0;
	R_ClusteredFillParamsUBO (&params, runtime);
	r_clustered.upload_calls++;
	r_clustered.last_frame_uploaded = r_framecount;
	upload_start_ms = R_ClusteredNowMS ();

	GL_BindBufferFunc (GL_UNIFORM_BUFFER, r_clustered.params_ubo);
	GL_BufferSubDataFunc (GL_UNIFORM_BUFFER, 0, sizeof (params), &params);
	r_clustered.upload_ms += R_ClusteredNowMS () - upload_start_ms;

	r_clustered.has_frame_data = false;
	r_clustered_debug_cluster_count = r_clustered.cluster_count;
	r_clustered_debug_index_count = 0;
	memset (r_clustered_debug_light_cluster_hits, 0, sizeof (r_clustered_debug_light_cluster_hits));

	if (R_ClusteredShouldLog ())
	{
		Con_Printf ("CLUSTERDBG summary clusters=%d indices=%u (empty-frame) overflow=0 grid=(%d %d %d) tile=%d calls=(b:%d u:%d n:%d) frame=(b:%d u:%d) cpu_ms=(build:%.3f upload:%.3f bind:%.3f) counters=(glget_bind:%d alloc:%d barrier:%d)\n",
			r_clustered.cluster_count,
			r_clustered.debug_indices_written_count,
			r_clustered.grid_x,
			r_clustered.grid_y,
			r_clustered.z_slices,
			r_clustered.params.tile_size,
				r_clustered.build_calls,
				r_clustered.upload_calls,
				r_clustered.bind_calls,
				r_clustered.last_frame_built,
				r_clustered.last_frame_uploaded,
				r_clustered.build_ms,
				r_clustered.upload_ms,
				r_clustered.bind_ms_total,
				r_clustered.num_glget_calls_in_bind,
				r_clustered.num_buffer_allocs,
				r_clustered.num_barriers);
	}

	R_ClusterPerf_MarkReason ("cluster build skipped (numlights=0)");
}

static void R_ClusteredBuildJob_Run (void *job_data)
{
	clustered_job_output_t *out = (clustered_job_output_t *)job_data;
	clustered_async_frame_t *frame;
	const clustered_snapshot_t *snap;
	int c;

	if (!out)
		return;
	frame = NULL;
	for (c = 0; c < 3; ++c)
	{
		int j;
		for (j = 0; j < r_clustered_async_state.frames[c].num_jobs; ++j)
			if (&r_clustered_async_state.frames[c].jobs[j] == out)
				frame = &r_clustered_async_state.frames[c];
	}
	if (!frame)
		return;
	snap = &frame->snapshot;
	out->index_count = 0;
	out->overflowed = false;
	for (c = out->cluster_begin; c < out->cluster_end; ++c)
	{
		int x = c % snap->runtime.grid_x;
		int y = (c / snap->runtime.grid_x) % snap->runtime.grid_y;
		int z = c / (snap->runtime.grid_x * snap->runtime.grid_y);
		int l;
		clustered_header_t *hdr = &out->headers[c - out->cluster_begin];
		hdr->count = 0;
		hdr->offset = (GLuint)out->index_count;
		for (l = 0; l < snap->num_lights; ++l)
		{
			const clustered_light_t *light = &snap->lights[l];
			float radius = q_max (0.f, light->pos_radius[3]);
			float vx = snap->view_matrix[12] + light->pos_radius[0] * snap->view_matrix[0] + light->pos_radius[1] * snap->view_matrix[4] + light->pos_radius[2] * snap->view_matrix[8];
			float vy = snap->view_matrix[13] + light->pos_radius[0] * snap->view_matrix[1] + light->pos_radius[1] * snap->view_matrix[5] + light->pos_radius[2] * snap->view_matrix[9];
			float vzv = snap->view_matrix[14] + light->pos_radius[0] * snap->view_matrix[2] + light->pos_radius[1] * snap->view_matrix[6] + light->pos_radius[2] * snap->view_matrix[10];
			float vz = q_max (1e-4f, -vzv);
			float clipw = snap->proj_matrix[11] * vzv + snap->proj_matrix[15];
			float clipx = snap->proj_matrix[0] * vx + snap->proj_matrix[8] * vzv + snap->proj_matrix[12];
			float clipy = snap->proj_matrix[5] * vy + snap->proj_matrix[9] * vzv + snap->proj_matrix[13];
			float ndcx, ndcy, pixRadius, centerx, centery;
			int minX, maxX, minY, maxY, minZ, maxZ;
			if (radius <= 0.f || clipw <= 0.0001f)
				continue;
			ndcx = clipx / clipw;
			ndcy = clipy / clipw;
			pixRadius = (radius / q_max (vz, 1e-4f)) * (float)snap->runtime.screen_h * 0.5f;
			centerx = (ndcx * 0.5f + 0.5f) * (float)snap->runtime.screen_w;
			centery = (ndcy * 0.5f + 0.5f) * (float)snap->runtime.screen_h;
			minX = CLAMP (0, (int)floorf ((centerx - pixRadius) / (float)snap->runtime.tile_size), snap->runtime.grid_x - 1);
			maxX = CLAMP (0, (int)floorf ((centerx + pixRadius) / (float)snap->runtime.tile_size), snap->runtime.grid_x - 1);
			minY = CLAMP (0, (int)floorf ((centery - pixRadius) / (float)snap->runtime.tile_size), snap->runtime.grid_y - 1);
			maxY = CLAMP (0, (int)floorf ((centery + pixRadius) / (float)snap->runtime.tile_size), snap->runtime.grid_y - 1);
			minZ = CLAMP (0, (int)floorf (log2f (q_max (1e-4f, vz - radius)) * snap->runtime.z_log_scale + snap->runtime.z_log_bias), snap->runtime.z_slices - 1);
			maxZ = CLAMP (0, (int)floorf (log2f (q_max (1e-4f, vz + radius)) * snap->runtime.z_log_scale + snap->runtime.z_log_bias), snap->runtime.z_slices - 1);
			if (x < minX || x > maxX || y < minY || y > maxY || z < minZ || z > maxZ)
				continue;
			if (out->index_count < out->max_indices)
				out->indices[out->index_count++] = (GLuint)l;
			else
				out->overflowed = true;
			hdr->count++;
		}
	}
	SDL_LockMutex (r_clustered_async_state.mutex);
	frame->completed_jobs++;
	if (out->overflowed)
		frame->overflowed = true;
	if (frame->completed_jobs >= frame->num_jobs)
	{
		frame->ready = true;
		frame->pending = false;
		r_clustered_async_state.completed++;
	}
	SDL_UnlockMutex (r_clustered_async_state.mutex);
}

static void R_ClusteredAsync_EnsureBuffers (void)
{
	int i, j;
	for (i = 0; i < 3; ++i)
		for (j = 0; j < 8; ++j)
		{
			clustered_job_output_t *job = &r_clustered_async_state.frames[i].jobs[j];
			if (job->max_indices != q_max (1, (int)r_clustered_maxindices.value))
			{
				free (job->indices);
				job->indices = NULL;
				job->max_indices = q_max (1, (int)r_clustered_maxindices.value);
			}
			if (!job->headers)
				job->headers = (clustered_header_t *)calloc ((size_t)q_max (1, r_clustered.cluster_count), sizeof (clustered_header_t));
			if (!job->indices)
				job->indices = (GLuint *)calloc ((size_t)job->max_indices, sizeof (GLuint));
		}
}

static void R_ClusteredAsync_InitIfNeeded (void)
{
	int workers = CLAMP (1, (int)r_clustered_workers.value, 8);
	int i, j;
	if (r_clustered_async_state.initialized && workers == r_clustered_async_state.workers)
		return;
	if (r_clustered_async_state.queue)
		Sys_Jobs_DestroyQueue (r_clustered_async_state.queue);
	if (!r_clustered_async_state.mutex)
		r_clustered_async_state.mutex = SDL_CreateMutex ();
	memset (&r_clustered_async_state.frames, 0, sizeof (r_clustered_async_state.frames));
	r_clustered_async_state.queue = Sys_Jobs_CreateQueue ("CPU_PREP", 256, (size_t)workers);
	r_clustered_async_state.workers = workers;
	r_clustered_async_state.initialized = true;
	for (i = 0; i < 3; ++i)
		for (j = 0; j < 8; ++j)
			r_clustered_async_state.frames[i].jobs[j].max_indices = q_max (1, (int)r_clustered_maxindices.value);
	R_ClusteredAsync_EnsureBuffers ();
}

static void R_ClusteredAsync_Shutdown (void)
{
	int i, j;
	if (r_clustered_async_state.queue)
		Sys_Jobs_DestroyQueue (r_clustered_async_state.queue);
	for (i = 0; i < 3; ++i)
		for (j = 0; j < 8; ++j)
		{
			free (r_clustered_async_state.frames[i].jobs[j].headers);
			free (r_clustered_async_state.frames[i].jobs[j].indices);
		}
	if (r_clustered_async_state.mutex)
		SDL_DestroyMutex (r_clustered_async_state.mutex);
	memset (&r_clustered_async_state, 0, sizeof (r_clustered_async_state));
}

static void R_Clustered_CommitLists (const clustered_async_frame_t *frame)
{
	clustered_params_t params;
	GLuint counters[2] = {0u, 0u};
	double upload_start_ms;
	int i, j;
	int global = 0;
	if (!frame)
		return;

	r_clustered.upload_calls++;
	r_clustered.last_upload_light_count = (unsigned int)frame->snapshot.num_lights;
	r_clustered.last_frame_uploaded = r_framecount;
	upload_start_ms = R_ClusteredNowMS ();
	for (i = 0; i < frame->num_jobs; ++i)
	{
		const clustered_job_output_t *job = &frame->jobs[i];
		for (j = job->cluster_begin; j < job->cluster_end; ++j)
		{
			clustered_header_t src = job->headers[j - job->cluster_begin];
			clustered_header_t *dst = &r_clustered_headers_cpu[j];
			int copy_count = (int)src.count;
			if (global >= r_clustered.max_indices)
				copy_count = 0;
			else if (global + copy_count > r_clustered.max_indices)
				copy_count = r_clustered.max_indices - global;
			dst->offset = (GLuint)global;
			dst->count = (GLuint)copy_count;
			if (copy_count > 0)
			{
				GL_BindBufferFunc (GL_SHADER_STORAGE_BUFFER, r_clustered.indices_ssbo);
				GL_BufferSubDataFunc (GL_SHADER_STORAGE_BUFFER, (GLintptr)(global * (int)sizeof (GLuint)), (GLsizeiptr)(copy_count * (int)sizeof (GLuint)), &job->indices[src.offset]);
				global += copy_count;
			}
		}
	}
	R_ClusteredFillParamsUBO (&params, &frame->snapshot.runtime);
	params.clustered_enabled = 1;
	GL_BindBufferFunc (GL_SHADER_STORAGE_BUFFER, r_clustered.headers_ssbo);
	GL_BufferSubDataFunc (GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)(sizeof (clustered_header_t) * (size_t)r_clustered.cluster_count), r_clustered_headers_cpu);
	GL_BindBufferFunc (GL_UNIFORM_BUFFER, r_clustered.params_ubo);
	GL_BufferSubDataFunc (GL_UNIFORM_BUFFER, 0, sizeof (params), &params);
	counters[0] = (GLuint)r_clustered.max_indices;
	counters[1] = frame->overflowed ? 1u : 0u;
	GL_BindBufferFunc (GL_SHADER_STORAGE_BUFFER, r_clustered.counters_ssbo);
	GL_BufferSubDataFunc (GL_SHADER_STORAGE_BUFFER, 0, sizeof (counters), counters);

	// Upload packed light data (positions, radii, colors) so fragment shaders can
	// read from PackedLightsBuffer (binding=3).  This was missing: headers and
	// indices were uploaded but lights_ssbo was left uninitialised, causing every
	// ClusterFetchLight() call to return zero-radius / black lights.
	if (frame->snapshot.num_lights > 0)
	{
		GL_BindBufferFunc (GL_SHADER_STORAGE_BUFFER, r_clustered.lights_ssbo);
		GL_BufferSubDataFunc (GL_SHADER_STORAGE_BUFFER, 0,
			(GLsizeiptr)(sizeof (clustered_light_t) * (size_t)frame->snapshot.num_lights),
			frame->snapshot.lights);
	}

	r_clustered.debug_indices_written_count = (GLuint)global;
	((clustered_async_frame_t *)frame)->output_bytes = (int)(sizeof (clustered_header_t) * (size_t)r_clustered.cluster_count + sizeof (GLuint) * (size_t)global);
	r_clustered.upload_ms += R_ClusteredNowMS () - upload_start_ms;
	r_clustered.shading_enabled = true;
	r_clustered.has_frame_data = true;
}

static void R_Clustered_BuildLists_Snapshot (clustered_snapshot_t *snap)
{
	unsigned int i;
	if (!snap)
		return;
	memset (snap, 0, sizeof (*snap));
	R_ClusteredComputeRuntimeParams (&snap->runtime);
	snap->num_lights = (int)q_min ((unsigned int)DLIGHT_GPU_MAX, r_framedata.numlights);
	for (i = 0; i < (unsigned int)snap->num_lights; ++i)
	{
		const gpulight_t *in = &r_lightbuffer.lights[i];
		clustered_light_t *out = &snap->lights[i];
		out->pos_radius[0] = in->pos[0]; out->pos_radius[1] = in->pos[1]; out->pos_radius[2] = in->pos[2]; out->pos_radius[3] = in->radius;
		out->color_intensity[0] = in->color[0]; out->color_intensity[1] = in->color[1]; out->color_intensity[2] = in->color[2]; out->color_intensity[3] = 1.f;
	}
	memcpy (snap->view_matrix, r_matview, sizeof (snap->view_matrix));
	memcpy (snap->proj_matrix, r_matproj, sizeof (snap->proj_matrix));
	snap->debug_mode = (int)r_clustered_debug.value;
}

void R_Clustered_BuildLists (void)
{
	cluster_runtime_params_t runtime;
	clustered_snapshot_t sync_snap;
	clustered_async_frame_t *slot;
	int i;
	r_clustered.shading_bound = false;
	if (!R_ClusteredEnabled ())
		return;
	R_ClusteredComputeRuntimeParams (&runtime);
	r_clustered.params = runtime;
	R_ClusteredEnsureCapacity (runtime.grid_x, runtime.grid_y, runtime.z_slices);
	R_ClusteredAsync_EnsureBuffers ();
	if (!r_clustered_headers_cpu)
		return;

	r_clustered.build_calls++;
	r_clustered.build_ran_this_frame = true;
	r_clustered.last_frame_built = r_framecount;

	if (r_framedata.numlights == 0)
	{
		r_clustered.debug_indices_written_count = 0u;
		R_ClusteredResetFrameState (&runtime);
		return;
	}

	if (r_clustered_async.value <= 0.f || r_clustered_force_sync.value > 0.f)
	{
		clustered_async_frame_t *sync_frame;
		R_ClusteredAsync_InitIfNeeded ();
		sync_frame = &r_clustered_async_state.frames[0];
		R_Clustered_BuildLists_Snapshot (&sync_snap);
		sync_frame->snapshot = sync_snap;
		sync_frame->num_jobs = 1;
		sync_frame->completed_jobs = 0;
		sync_frame->ready = false;
		sync_frame->pending = true;
		sync_frame->overflowed = false;
		sync_frame->jobs[0].cluster_begin = 0;
		sync_frame->jobs[0].cluster_end = r_clustered.cluster_count;
		R_ClusteredBuildJob_Run (&sync_frame->jobs[0]);
		R_Clustered_CommitLists (sync_frame);
		sync_frame->ready = false;
		sync_frame->pending = false;
		return;
	}

	R_ClusteredAsync_InitIfNeeded ();
	slot = &r_clustered_async_state.frames[r_clustered_async_state.submit_slot % 3];
	if (!slot->pending)
	{
		R_Clustered_BuildLists_Snapshot (&slot->snapshot);
		slot->num_jobs = q_min (r_clustered_async_state.workers, 8);
		slot->completed_jobs = 0;
		slot->ready = false;
		slot->pending = true;
		slot->overflowed = false;
		for (i = 0; i < slot->num_jobs; ++i)
		{
			int begin = (r_clustered.cluster_count * i) / slot->num_jobs;
			int end = (r_clustered.cluster_count * (i + 1)) / slot->num_jobs;
			slot->jobs[i].cluster_begin = begin;
			slot->jobs[i].cluster_end = end;
			if (!Sys_Jobs_Submit (r_clustered_async_state.queue, R_ClusteredBuildJob_Run, &slot->jobs[i]))
				slot->pending = false;
		}
		r_clustered_async_state.submitted++;
		r_clustered_async_state.submit_slot++;
	}

	slot = &r_clustered_async_state.frames[r_clustered_async_state.commit_slot % 3];
	if (slot->ready)
	{
		R_Clustered_CommitLists (slot);
		slot->ready = false;
		r_clustered_async_state.commit_slot++;
	}
	else
	{
		R_ClusteredResetFrameState (&runtime);
	}
	if (r_clustered_async_debug.value > 0.f)
	{
		Con_Printf ("cluster_async: submitted=%u completed=%u bytes=%d latency=1 force_sync=%d\n",
			r_clustered_async_state.submitted,
			r_clustered_async_state.completed,
			slot->output_bytes,
			r_clustered_force_sync.value > 0.f ? 1 : 0);
	}
}

void R_Clustered_BindForShading (void)
{
	double bind_start_ms;

	if (!R_ClusteredEnabled ())
		return;
	if (!r_clustered.shading_enabled)
		return;
	if (r_clustered.shading_bound)
		return;

	bind_start_ms = R_ClusteredNowMS ();
	R_ClusterPerf_BeginClusterBindBarrier ();
	R_ClusteredBindRangeCached (GL_SHADER_STORAGE_BUFFER, 3, r_clustered.lights_ssbo,
		(GLsizeiptr)(sizeof (clustered_light_t) * (size_t)r_clustered.max_lights), &r_clustered.bound_lights_ssbo, &r_clustered.bound_lights_size);
	R_ClusteredBindRangeCached (GL_SHADER_STORAGE_BUFFER, 4, r_clustered.headers_ssbo,
		(GLsizeiptr)(sizeof (clustered_header_t) * (size_t)r_clustered.cluster_count), &r_clustered.bound_headers_ssbo, &r_clustered.bound_headers_size);
	R_ClusteredBindRangeCached (GL_SHADER_STORAGE_BUFFER, 5, r_clustered.indices_ssbo,
		(GLsizeiptr)(sizeof (GLuint) * (size_t)r_clustered.max_indices), &r_clustered.bound_indices_ssbo, &r_clustered.bound_indices_size);
	R_ClusteredBindRangeCached (GL_SHADER_STORAGE_BUFFER, 6, r_clustered.counters_ssbo,
		sizeof (GLuint) * 2, &r_clustered.bound_counters_ssbo, &r_clustered.bound_counters_size);
	R_ClusteredBindRangeCached (GL_UNIFORM_BUFFER, 2, r_clustered.params_ubo,
		sizeof (clustered_params_t), &r_clustered.bound_params_ubo, &r_clustered.bound_params_size);
	R_ClusteredValidateBinding (GL_SHADER_STORAGE_BUFFER_BINDING, 3, r_clustered.lights_ssbo, "PackedLightsBuffer");
	R_ClusteredValidateBinding (GL_SHADER_STORAGE_BUFFER_BINDING, 4, r_clustered.headers_ssbo, "ClusterHeaderBuffer");
	R_ClusteredValidateBinding (GL_SHADER_STORAGE_BUFFER_BINDING, 5, r_clustered.indices_ssbo, "ClusterIndexBuffer");
	R_ClusteredValidateBinding (GL_UNIFORM_BUFFER_BINDING, 2, r_clustered.params_ubo, "ClusterParams");
	R_ClusterPerf_EndClusterBindBarrier ();
	r_clustered.bind_ms_total += R_ClusteredNowMS () - bind_start_ms;
	r_clustered.shading_bound = true;
	r_clustered.bind_ran_this_frame = true;
}


void R_Clustered_RebindForProgram (GLuint program, const char *pass_name)
{
	if (!R_ClusteredEnabled ())
		return;

	R_Clustered_BindForShading ();
	r_clustered.bind_calls++;

	if (R_ClusteredShouldLog ())
	{
		Con_Printf ("CLUSTERDBG bind pass=%s program=%u buffers=ok lights=%u grid=(%d %d %d) tile=%d\n",
			pass_name ? pass_name : "<unknown>",
			(unsigned)program,
			r_framedata.numlights,
			r_clustered.grid_x,
			r_clustered.grid_y,
			r_clustered.z_slices,
			r_clustered.params.tile_size);
	}
}

void R_GetClusterDlightDebugStats (int *out_clusters, int *out_indices, const int **out_light_hits, int *out_max_hits)
{
	if (out_clusters)
		*out_clusters = r_clustered_debug_cluster_count;
	if (out_indices)
		*out_indices = (int)r_clustered.debug_indices_written_count;
	if (out_light_hits)
		*out_light_hits = r_clustered_debug_light_cluster_hits;
	if (out_max_hits)
		*out_max_hits = (int)countof (r_clustered_debug_light_cluster_hits);
}

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
	R_Clustered_Init ();
}

/*
=============
GLLight_DeleteResources
=============
*/
void GLLight_DeleteResources (void)
{
	R_Clustered_Shutdown ();
}

static void R_PushDlightArray (dlight_t *const *lights, int count)
{
		for (int i = 0; i < count; i++)
	{
		dlight_t *l = lights[i];
		gpulight_t *out;
		float radius;

		if (l->spawn > cl.time)
		{
			l->die = 0.f;
			continue;
		}

		if (!CL_DlightIsActive (l))
			continue;

		if (CL_DlightShouldFlicker (l))
			radius = l->baseradius * (1.f + 0.1f * (float) sin (cl.time * 9.0 + l->flicker_seed));
		else
			radius = l->baseradius;
		radius *= q_max (0.f, r_clustered_light_radius_scale.value);
		radius = q_max (radius, 0.f);
		l->radius = radius;

		out = &r_lightbuffer.lights[r_framedata.numlights++];
		r_dlight_sources[r_framedata.numlights - 1] = l;
		const vec3_t *temp = R_GetDynamicLightTemperature (l->type);
		float radiusFactor = q_min (1.f, q_max (radius / 350.f, 0.2f));
		vec3_t finalcolor;
		finalcolor[0] = l->color[0] * (*temp)[0] * radiusFactor;
		finalcolor[1] = l->color[1] * (*temp)[1] * radiusFactor;
		finalcolor[2] = l->color[2] * (*temp)[2] * radiusFactor;
		if (CL_DlightShouldFlicker (l))
		{
			float flicker = 1.f + (float)sin (cl.time * 15.0 + l->key) * 0.1f;
			finalcolor[0] *= flicker;
			finalcolor[1] *= flicker;
			finalcolor[2] *= flicker;
		}
		if (l->type == DLIGHT_TORCH)
		{
			float colorshift = (float)sin (cl.time * 11.0 + l->key) * 0.1f;
			finalcolor[0] *= 1.0f + colorshift;
			finalcolor[1] *= 1.0f - colorshift;
		}
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
	qboolean clustered_enabled;
	dlight_t *submit[DLIGHT_GPU_MAX];
	dlight_filter_debug_t filter_stats;
	int final_filtered_count;
	int upload_count;
	int enable_flag;
	int indices_count;

	r_framedata.numlights = 0;
	r_clustered.last_upload_light_count = 0;
	r_clustered.build_ran_this_frame = false;
	r_clustered.bind_ran_this_frame = false;
	R_PerfStats_SetClusterBuildRan (false);
	memset (r_dlight_sources, 0, sizeof (r_dlight_sources));
	if (r_clustered.last_frame_built != r_framecount)
	{
		r_clustered.build_calls = 0;
		r_clustered.upload_calls = 0;
		r_clustered.bind_calls = 0;
		r_clustered.build_ms = 0.0;
		r_clustered.upload_ms = 0.0;
		r_clustered.bind_ms_total = 0.0;
		r_clustered.num_glget_calls_in_bind = 0;
		r_clustered.num_buffer_allocs = 0;
		r_clustered.num_barriers = 0;
	}
	R_ClusterPerf_BeginPush ();

	if (r_clustered_lights.value > 0.f)
	{
		R_ClusterPerf_BeginLightFilter ();
		const int budget = q_min (q_max (1, DLightPool_GetBudget ()), DLIGHT_GPU_MAX);
		DLightPool_NewFrame (cl.time, r_framecount);
		const int num_submit = DLightPool_CollectForRender (cl.time, r_refdef.vieworg, r_viewleaf, submit, budget);
		R_ClusterPerf_EndLightFilter ();
		if (num_submit > 0)
			R_PushDlightArray (submit, num_submit);
		DLightPool_DebugPrint ();
	}

	DLightPool_GetFilterDebug (&filter_stats, NULL);
	final_filtered_count = filter_stats.final_active_count;
	if (final_filtered_count < 0)
		final_filtered_count = 0;
	if ((unsigned int)final_filtered_count != r_framedata.numlights)
	{
		Con_Printf ("CLUSTERERR desync final_filtered=%d framedata_numlights=%u (R_PushDlights)\n",
			final_filtered_count, r_framedata.numlights);
		if (final_filtered_count > 0)
			r_framedata.numlights = (unsigned int)q_min (final_filtered_count, DLIGHT_GPU_MAX);
	}

	R_ClusterPerf_BeginLightUpload ();
	r_framedata.clustered_light_params[2] = (r_clustered_lights.value > 0.f && r_framedata.numlights > 0) ? 1.f : 0.f;
	R_UploadFrameData ();
	R_ClusterPerf_EndLightUpload ();

	clustered_enabled = (r_clustered.available != 0);
	if (!clustered_enabled)
	{
		R_ClusterPerf_MarkReason ("clustered resources unavailable");
		R_ClusterPerf_EndPush ();
		return;
	}

	if (r_dbg_clustered_force_fallback.value > 0.f)
	{
		if (developer.value > 0.f && R_ClusteredShouldLog ())
			R_ClusterPerf_MarkReason ("r_dbg_clustered_force_fallback=1 (developer fallback)");
		R_ClusterPerf_EndPush ();
		return;
	}

	if (r_clustered_lights.value > 0.f && r_framedata.numlights > 0)
	{
		GL_BeginGroup ("Light clustering");
		if (r_clustered.last_frame_built != r_framecount)
		{
			R_PerfStats_SetClusterBuildRan (true);
			R_ClusterPerf_BeginBuild ();
			R_Clustered_BuildLists ();
			R_ClusterPerf_EndBuild ();
		}
		else if (R_ClusteredShouldLog ())
		{
			R_ClusterPerf_MarkReason ("cluster build reused (already built this frame)");
		}
		R_Clustered_BindForShading ();
		GL_EndGroup ();
	}
	else
	{
		r_clustered.has_frame_data = false;
		r_clustered.shading_enabled = false;
		r_clustered.shading_bound = false;
		R_ClusterPerf_MarkReason (r_clustered_lights.value <= 0.f ? "clustered disabled" : "cluster build skipped (numlights=0)");
	}
	R_ClusterPerf_EndPush ();

	upload_count = (int)r_clustered.last_upload_light_count;
	enable_flag = r_framedata.clustered_light_params[2] > 0.f ? 1 : 0;
	indices_count = (int)r_clustered.debug_indices_written_count;
	if (final_filtered_count > 0 && upload_count == 0)
	{
		Con_Printf ("CLUSTERERR desync final_filtered=%d framedata_numlights=%u upload_count=%d enable=%d build=%d bind=%d indices=%d (R_PushDlights)\n",
			final_filtered_count,
			r_framedata.numlights,
			upload_count,
			enable_flag,
			r_clustered.build_ran_this_frame ? 1 : 0,
			r_clustered.bind_ran_this_frame ? 1 : 0,
			indices_count);
	}
	if (r_clustered_sanity_debug.value > 0.f)
	{
		Con_Printf ("CLSANITY frame=%d created=%d after_merge=%d final=%d framedata_numlights=%u upload_count=%d enable=%d build_ran=%d bind_ran=%d indices=%d\n",
			r_framecount,
			filter_stats.created_count,
			filter_stats.after_merge_count,
			final_filtered_count,
			r_framedata.numlights,
			upload_count,
			enable_flag,
			r_clustered.build_ran_this_frame ? 1 : 0,
			r_clustered.bind_ran_this_frame ? 1 : 0,
			indices_count);
	}

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


static inline int LightStyleValue (unsigned short style)
{
if (style < 256)
return d_lightstylevalue[style];

return d_lightstylevalue[0];
}

static qboolean SampleDeluxemapDir(const qmodel_t *model, const msurface_t *surf, int ds, int dt, vec3_t out_dir)
{
const byte *samples;
int smax, tmax;
int dsfrac = ds & 15, dtfrac = dt & 15;
float fsfrac = dsfrac * (1.f / 16.f);
float ftfrac = dtfrac * (1.f / 16.f);
const float to_signed = 2.f * (1.f / 255.f);

if (!model || !model->lightdirdata || !surf || !surf->luxsamples)
{
VectorClear(out_dir);
return false;
}

samples = surf->luxsamples;
smax = (surf->extents[0] >> 4) + 1;
tmax = (surf->extents[1] >> 4) + 1;

const int s0 = ds >> 4;
const int t0 = dt >> 4;
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
int smax, tmax;
int dsfrac = ds & 15, dtfrac = dt & 15;
        float fsfrac = dsfrac * (1.f / 16.f);
        float ftfrac = dtfrac * (1.f / 16.f);
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

        smax = (surf->extents[0] >> 4) + 1;
        tmax = (surf->extents[1] >> 4) + 1;

        const int s0 = ds >> 4;
        const int t0 = dt >> 4;
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
int R_LightPoint (qmodel_t *model, vec3_t p, float ofs, lightcache_t *cache)
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

qboolean R_EntityStaticLight (entity_t *e, vec3_t out_color255, entity_lightinfo_t *info)
{
        vec3_t lightgrid_color = {0.f, 0.f, 0.f};
        vec3_t lightpoint_color = {0.f, 0.f, 0.f};
        float lightgrid_ao = 1.f;
        qboolean lightgrid_valid = false;
        qboolean used_lightgrid = false;
        qboolean used_lightpoint = false;
        qboolean used_minlight = false;

        VectorClear (out_color255);

        if (R_EnvLight_SampleEntityAmbient (e, lightgrid_color, &lightgrid_ao))
        {
                lightgrid_valid = true;
                VectorScale (lightgrid_color, lightgrid_ao * 255.f, out_color255);
                used_lightgrid = true;
        }

        if (!used_lightgrid)
        {
                qmodel_t *lightmodel = cl.worldmodel ? cl.worldmodel : e->model;
                if (lightmodel)
                {
                        if (!R_LightPointNoGrid (lightmodel, e->origin, 0.f, &e->lightcache, lightpoint_color))
                        {
                                float ofs = e->model ? e->model->maxs[2] * 0.5f : 0.f;
                                R_LightPointNoGrid (lightmodel, e->origin, ofs, &e->lightcache, lightpoint_color);
                        }
                        VectorCopy (lightpoint_color, out_color255);
                        used_lightpoint = VectorLength (lightpoint_color) > 0.f;
                }
        }

        float intensity = (out_color255[0] + out_color255[1] + out_color255[2]) * (1.f / (3.f * 255.f));
        if (intensity <= 0.f && r_minlight_models.value > 0.f && e != &cl.viewent)
        {
                const float minlight = CLAMP (0.f, r_minlight_models.value, 1.f);
                VectorSet (out_color255, minlight * 255.f, minlight * 255.f, minlight * 255.f);
                intensity = minlight;
                used_minlight = true;
        }

        e->lightcache.lightgrid_has_sample = lightgrid_valid;
        e->lightcache.lightgrid_ao = lightgrid_ao;
        VectorCopy (lightgrid_color, e->lightcache.lightgrid_color);

        if (info)
        {
                for (int i = 0; i < 3; i++)
                {
                        float L = out_color255[i] * (1.0f / 256.0f);
                        L = fminf (L, 1.0f);
                        info->static_color[i] = powf (L, 1.0f / 2.2f);
                }
                info->intensity = intensity;
                info->used_lightgrid = used_lightgrid;
                info->lightgrid_valid = lightgrid_valid;
                info->lightgrid_cell_valid = R_LightgridCellForPoint (e->origin, info->lightgrid_cell);
                VectorCopy (lightgrid_color, info->lightgrid_color);
                info->lightgrid_ao = lightgrid_ao;
                info->used_lightpoint = used_lightpoint;
                VectorScale (lightpoint_color, 1.f / 255.f, info->lightpoint_color);
                info->used_minlight = used_minlight;
        }

        return used_lightgrid || used_lightpoint || used_minlight;
}
