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

#include "quakedef.h"
#include "glquake.h"
#include "gl_lightgrid.h"
#include "r_entitylight.h"

extern cvar_t gl_overbright_models, gl_fullbrights;
extern cvar_t r_viewmodel_light_boost;
extern cvar_t r_viewmodel_minlight;
extern cvar_t r_debug_itemlight;

extern cvar_t r_lightgrid_debug;

static qboolean r_lightgrid_debug_sample_reported = false;
static const qmodel_t *r_lightgrid_debug_last_world = NULL;
static int r_itemlight_last_frame[MAX_EDICTS];

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

float R_LightgridLuminance (const vec3_t color)
{
	return color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
}

float R_ModelLightLuma (const vec3_t color)
{
	return color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
}

void R_LightgridChroma (const vec3_t color, vec3_t chroma)
{
	const float sum = color[0] + color[1] + color[2];

	if (sum <= 0.f)
	{
		VectorClear (chroma);
		return;
	}

	chroma[0] = color[0] / sum;
	chroma[1] = color[1] / sum;
	chroma[2] = color[2] / sum;
}

void R_ScaleAliasLighting (vec3_t light, vec3_t ambient, vec3_t dlight, float scale)
{
	if (scale == 1.f)
		return;

	if (scale <= 0.f)
	{
		VectorClear (light);
		VectorClear (ambient);
		VectorClear (dlight);
		return;
	}

	for (int i = 0; i < 3; i++)
	{
		light[i] *= scale;
		ambient[i] *= scale;
		dlight[i] *= scale;
	}
}

void R_DefaultStaticLightDir (vec3_t dir)
{
	VectorSet (dir, 0.f, 0.5f, 1.f);
	VectorNormalize (dir);
}

const char *R_StaticSourceName (entity_static_light_source_t source)
{
	switch (source)
	{
	case ENTITY_STATIC_LIGHT_GRID:
		return "grid";
	case ENTITY_STATIC_LIGHT_POINT:
		return "point";
	case ENTITY_STATIC_LIGHT_MINLIGHT:
		return "minlight";
	case ENTITY_STATIC_LIGHT_MIXED:
		return "mixed";
	default:
		return "none";
	}
}

static qboolean R_EntityLightDebugEnabled (const entity_t *e)
{
	if (r_debug_itemlight.value <= 0.f)
		return false;
	if (!e || !e->model)
		return false;
	if (e == &cl.viewent)
		return false;
	return (e->model->flags & EF_ROTATE) != 0;
}

static qboolean R_ModelLightSampleDirAtPoint (const vec3_t pos, vec3_t out_dir)
{
	vec3_t sample_rgb;

	if (!cl.worldmodel || !cl.worldmodel->lightdirdata)
	{
		R_DefaultStaticLightDir (out_dir);
		return false;
	}

	if (!R_SampleLightmapAndDeluxemapAtPoint (pos, sample_rgb, out_dir) || VectorLength (out_dir) <= 1e-6f)
	{
		R_DefaultStaticLightDir (out_dir);
		return false;
	}

	return true;
}

static const char *R_EntityTypeName (const qmodel_t *model)
{
	if (!model)
		return "none";

	switch (model->type)
	{
	case mod_alias:
	{
		aliashdr_t *hdr = (aliashdr_t *)Mod_Extradata ((qmodel_t *)model);
		if (hdr && hdr->poseverttype == PV_IQM)
			return "alias/iqm";
		return "alias";
	}
	case mod_sprite:
		return "sprite";
	case mod_brush:
		return "brush";
	default:
		return "unknown";
	}
}

void R_EntityLightDebugReport (const entity_t *e, const entity_lightinfo_t *info)
{
	int entnum = -1;

	if (!e || !e->model || !info)
		return;
	if (!R_EntityLightDebugEnabled (e))
		return;

	if (e >= cl_entities && e < cl_entities + cl.num_entities)
		entnum = (int)(e - cl_entities);

	if (entnum >= 0 && r_debug_itemlight.value <= 1.f)
	{
		if (r_itemlight_last_frame[entnum] == r_framecount)
			return;
		r_itemlight_last_frame[entnum] = r_framecount;
	}

	Con_Printf ("r_debug_itemlight: %s ent=%d model=%s src=%s used_grid=%s valid=%s "
		"grid_rgb=(%.1f %.1f %.1f) grid_ao=%.2f static=(%.1f %.1f %.1f) dyn=(%.1f %.1f %.1f) final=(%.1f %.1f %.1f)\n",
		R_EntityTypeName (e->model),
		entnum,
		e->model->name,
		R_StaticSourceName (info->static_source),
		info->used_lightgrid ? "yes" : "no",
		info->lightgrid_valid ? "yes" : "no",
		info->lightgrid_color[0], info->lightgrid_color[1], info->lightgrid_color[2],
		info->lightgrid_ao,
		info->static_color[0] * 255.f, info->static_color[1] * 255.f, info->static_color[2] * 255.f,
		info->dynamic_color[0] * 255.f, info->dynamic_color[1] * 255.f, info->dynamic_color[2] * 255.f,
		info->final_color[0] * 255.f, info->final_color[1] * 255.f, info->final_color[2] * 255.f);
}

void R_FinalizeAliasLighting (entity_t *e, vec3_t lightcolor, vec3_t ambientcolor, vec3_t dlightcolor, vec3_t dlightdir, vec3_t staticlightdir, entity_lightinfo_t *lightinfo_ptr)
{
	float add;
	unsigned int i;

	R_ApplyLightgridLighting (e, ambientcolor);

	if (e == &cl.viewent)
	{
		const float light_sum = lightcolor[0] + lightcolor[1] + lightcolor[2];
		const float light_intensity = light_sum * (1.f / (3.f * 255.f));
		const float boost = fmaxf (r_viewmodel_light_boost.value, 1.f);
		const float minlight_raw = fmaxf (r_viewmodel_minlight.value, 0.f);
		const float minlight = minlight_raw > 1.f ? minlight_raw * (1.f / 255.f) : minlight_raw;

		if (light_intensity > 0.f)
		{
			const float boosted = fmaxf (light_intensity * boost, light_intensity + (120.f / (3.f * 255.f)));
			const float target = fmaxf (boosted, minlight);
			R_ScaleAliasLighting (lightcolor, ambientcolor, dlightcolor, target / light_intensity);
		}
		else if (minlight > 0.f)
		{
			const float per_ch = minlight * 255.f;
			VectorSet (lightcolor, per_ch, per_ch, per_ch);
			VectorCopy (lightcolor, ambientcolor);
			VectorClear (dlightcolor);
		}
	}

	if (e > cl_entities && e <= cl_entities + cl.maxclients)
	{
		add = 24.0f - (lightcolor[0] + lightcolor[1] + lightcolor[2]);
		if (add > 0.0f)
		{
			add *= 1.0f / 3.0f;
			lightcolor[0] += add;
			lightcolor[1] += add;
			lightcolor[2] += add;
			ambientcolor[0] += add;
			ambientcolor[1] += add;
			ambientcolor[2] += add;
		}
	}

	if (!gl_overbright_models.value && (e->model->flags & MOD_FBRIGHTHACK) && gl_fullbrights.value)
	{
		lightcolor[0] = 256.0f;
		lightcolor[1] = 256.0f;
		lightcolor[2] = 256.0f;
		VectorCopy (lightcolor, ambientcolor);
		VectorClear (dlightcolor);
		VectorClear (dlightdir);
	}

	{
		vec3_t pre_total;
		vec3_t post_total;
		VectorAdd (ambientcolor, dlightcolor, pre_total);
		for (i = 0; i < 3; i++)
		{
			float L = pre_total[i] * (1.0f / 256.0f);
			L = fminf (L, 1.0f);
			post_total[i] = L;
		}

		for (i = 0; i < 3; i++)
		{
			const float total = pre_total[i];
			const float ambient_ratio = total > 0.0f ? ambientcolor[i] / total : 0.0f;
			const float dlight_ratio = total > 0.0f ? dlightcolor[i] / total : 0.0f;
			ambientcolor[i] = post_total[i] * ambient_ratio;
			dlightcolor[i] = post_total[i] * dlight_ratio;
			lightcolor[i] = post_total[i];
		}
	}

	if (lightinfo_ptr)
	{
		VectorCopy (dlightcolor, lightinfo_ptr->dynamic_color);
		VectorCopy (lightcolor, lightinfo_ptr->final_color);
	}

	if (lightinfo_ptr)
		R_EntityLightDebugReport (e, lightinfo_ptr);

	VectorCopy (ambientcolor, e->lightcache.ambientcolor);
	VectorCopy (dlightcolor, e->lightcache.dlightcolor);
	VectorCopy (dlightdir, e->lightcache.dlightdir);
	VectorCopy (staticlightdir, e->lightcache.staticlightdir);
}

qboolean R_ModelLightWouldExceedBudget (int requested_samples)
{
	const int max_samples = (int)r_model_light_samples_max.value;

	if (requested_samples <= 0 || max_samples <= 0)
		return false;

	return (r_model_light_frame_stats.total_samples + requested_samples) > max_samples;
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

void R_ModelLightStats_AddCall (int sample_count, qboolean used_multisample, qboolean budget_fallback, double elapsed_ms)
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

static qboolean R_LightgridCellForPoint (const vec3_t pos, int out_cell[3])
{
	const lightgrid_t *lg = Lightgrid_Get ();

	if (!out_cell || !R_LightgridEnabled () || !lg || !lg->octree)
		return false;

	{
		const lightgrid_octree_header_t *header = &lg->octree->header;

		for (int i = 0; i < 3; i++)
		{
			float local = (pos[i] - header->grid_mins[i]) / header->grid_dist[i];
			int cell = Q_rint (local);
			if (cell < 0 || cell >= header->grid_size[i])
				return false;
			out_cell[i] = cell;
		}
	}

	return true;
}

static void R_DebugLightgridSample (const entity_t *e, const vec3_t ambient_before, const vec3_t ambient_after, const vec3_t ambient_delta)
{
	if (!r_lightgrid_debug.value)
	{
		r_lightgrid_debug_sample_reported = false;
		return;
	}

	if (cl.worldmodel != r_lightgrid_debug_last_world)
	{
		r_lightgrid_debug_sample_reported = false;
		r_lightgrid_debug_last_world = cl.worldmodel;
	}

	if (r_lightgrid_debug_sample_reported)
		return;

	r_lightgrid_debug_sample_reported = true;

	{
		vec3_t before_chroma;
		vec3_t after_chroma;
		const float before_luminance = R_LightgridLuminance (ambient_before);
		const float after_luminance = R_LightgridLuminance (ambient_after);

		R_LightgridChroma (ambient_before, before_chroma);
		R_LightgridChroma (ambient_after, after_chroma);

		Con_Printf ("r_lightgrid_debug: %s probe rgb=(%.2f %.2f %.2f) ao=%.2f "
			"ambient_delta=(%.1f %.1f %.1f) lum=%.1f->%.1f "
			"chroma=(%.3f %.3f %.3f)->(%.3f %.3f %.3f)\n",
			e->model ? e->model->name : "<no model>",
			e->lightcache.lightgrid_color[0], e->lightcache.lightgrid_color[1], e->lightcache.lightgrid_color[2],
			e->lightcache.lightgrid_ao,
			ambient_delta[0], ambient_delta[1], ambient_delta[2],
			before_luminance, after_luminance,
			before_chroma[0], before_chroma[1], before_chroma[2],
			after_chroma[0], after_chroma[1], after_chroma[2]);
	}
}

void R_ApplyLightgridLighting (const entity_t *e, vec3_t ambientcolor)
{
	vec3_t gridcolor;

	if (!R_LightgridEnabled () || !e->lightcache.lightgrid_has_sample)
		return;

	VectorScale (e->lightcache.lightgrid_color, e->lightcache.lightgrid_ao * 255.f, gridcolor);
	if (gridcolor[0] == 0.f && gridcolor[1] == 0.f && gridcolor[2] == 0.f)
		return;

	{
		vec3_t ambient_before;
		vec3_t ambient_delta;
		float grid_scale = 1.f;

		VectorCopy (ambientcolor, ambient_before);

		for (int i = 0; i < 3; i++)
		{
			const float before = fmaxf (ambientcolor[i], 0.f);
			const float headroom = fmaxf (255.f - before, 0.f);

			if (gridcolor[i] > 0.f)
				grid_scale = fminf (grid_scale, headroom / gridcolor[i]);
		}

		grid_scale = CLAMP (0.f, grid_scale, 1.f);

		for (int i = 0; i < 3; i++)
		{
			const float before = fmaxf (ambientcolor[i], 0.f);
			const float after = CLAMP (0.f, before + gridcolor[i] * grid_scale, 255.f);
			ambientcolor[i] = after;
			ambient_delta[i] = after - before;
		}

		R_DebugLightgridSample (e, ambient_before, ambientcolor, ambient_delta);
	}
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

void R_ClampSampleColor (vec3_t color)
{
	for (int i = 0; i < 3; i++)
	{
		if (!isfinite (color[i]) || color[i] < 0.f)
			color[i] = 0.f;
	}

	if (VectorLength (color) < 1e-6f)
		VectorClear (color);
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

qboolean R_EntityStaticLight (entity_t *e, vec3_t out_color255, entity_lightinfo_t *info)
{
	vec3_t lightgrid_effective = {0.f, 0.f, 0.f};
	vec3_t lightpoint_color = {0.f, 0.f, 0.f};
	vec3_t staticlightdir = {0.f, 0.f, 0.f};
	vec3_t target_color255 = {0.f, 0.f, 0.f};
	qboolean lightgrid_valid = false;
	qboolean used_lightgrid = false;
	qboolean used_lightpoint = false;
	qboolean used_minlight = false;
	qboolean used_multisample = false;
	qboolean budget_fallback = false;
	int sample_count = 0;
	double start_time = Sys_DoubleTime ();
	const qboolean force_viewmodel_multisample = (e == &cl.viewent);
	const qboolean force_alias_multisample = (e->model && e->model->type == mod_alias);
	const qboolean use_multisample = force_alias_multisample || force_viewmodel_multisample || (r_model_light_multisample.value > 0.f && e->model);
	const qboolean legacy_compatible = (!use_multisample && r_model_lightgrid_assist.value <= 0.f);
	qmodel_t *lightmodel = cl.worldmodel ? cl.worldmodel : e->model;
	entity_static_light_source_t static_source = ENTITY_STATIC_LIGHT_NONE;
	float intensity;

	VectorClear (out_color255);
	R_DefaultStaticLightDir (staticlightdir);

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

		R_ModelLightSampleDirAtPoint (e->origin, staticlightdir);
	}
	else
	{
		vec3_t sample_pos[8];
		float sample_weight[8];
		int num_samples = 1;
		float total_weight = 0.f;
		vec3_t weighted_color = {0.f, 0.f, 0.f};
		vec3_t weighted_dir = {0.f, 0.f, 0.f};
		float grid_weight = 0.f;
		float point_weight = 0.f;
		float dir_weight = 0.f;

		VectorCopy (e->origin, sample_pos[0]);
		sample_weight[0] = 1.f;

		if (use_multisample && e->model)
		{
			const float height = q_max (e->model->maxs[2] - e->model->mins[2], 0.f);
			const float half_extent_x = q_max (fabsf (e->model->mins[0]), fabsf (e->model->maxs[0]));
			const float half_extent_y = q_max (fabsf (e->model->mins[1]), fabsf (e->model->maxs[1]));

			num_samples = 0;
			VectorCopy (e->origin, sample_pos[num_samples]);
			sample_weight[num_samples++] = force_viewmodel_multisample ? 0.28f : 0.5f;

			if (force_viewmodel_multisample)
			{
				vec3_t local_offset;
				const float xofs = CLAMP (4.f, half_extent_x * 0.4f, 20.f);
				const float yofs = CLAMP (4.f, half_extent_y * 0.4f, 20.f);
				const float zofs = CLAMP (4.f, q_max (height, 8.f) * 0.25f, 20.f);

				VectorSet (local_offset, xofs, 0.f, 0.f);
				R_ModelLightBuildSamplePos (e, local_offset, sample_pos[num_samples]);
				sample_weight[num_samples++] = 0.12f;

				VectorSet (local_offset, -xofs, 0.f, 0.f);
				R_ModelLightBuildSamplePos (e, local_offset, sample_pos[num_samples]);
				sample_weight[num_samples++] = 0.12f;

				VectorSet (local_offset, 0.f, yofs, 0.f);
				R_ModelLightBuildSamplePos (e, local_offset, sample_pos[num_samples]);
				sample_weight[num_samples++] = 0.12f;

				VectorSet (local_offset, 0.f, -yofs, 0.f);
				R_ModelLightBuildSamplePos (e, local_offset, sample_pos[num_samples]);
				sample_weight[num_samples++] = 0.12f;

				VectorSet (local_offset, 0.f, 0.f, zofs);
				R_ModelLightBuildSamplePos (e, local_offset, sample_pos[num_samples]);
				sample_weight[num_samples++] = 0.12f;

				VectorSet (local_offset, 0.f, 0.f, -zofs);
				R_ModelLightBuildSamplePos (e, local_offset, sample_pos[num_samples]);
				sample_weight[num_samples++] = 0.12f;
			}
			else if (height > 1.f)
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

			{
				vec3_t sample_dir;
				qboolean have_dir = R_ModelLightSampleDirAtPoint (sample_pos[i], sample_dir);

				if (have_dir || force_viewmodel_multisample)
				{
					VectorMA (weighted_dir, w, sample_dir, weighted_dir);
					dir_weight += w;
				}
			}

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

		if (dir_weight > 0.f)
		{
			VectorScale (weighted_dir, 1.f / dir_weight, weighted_dir);
			if (VectorNormalize (weighted_dir) <= 1e-6f)
				R_DefaultStaticLightDir (weighted_dir);
		}
		else
		{
			R_DefaultStaticLightDir (weighted_dir);
		}

		VectorCopy (weighted_dir, staticlightdir);
	}

	intensity = R_SampleIntensity255 (out_color255);
	if (r_minlight_models.value > 0.f && e != &cl.viewent)
	{
		const float minlight = CLAMP (0.f, r_minlight_models.value, 1.f);

		if (intensity < minlight)
		{
			if (intensity > 0.f)
			{
				/* Keep the sample's hue but lift it to the configured floor. */
				VectorScale (out_color255, minlight / intensity, out_color255);
			}
			else
			{
				VectorSet (out_color255, minlight * 255.f, minlight * 255.f, minlight * 255.f);
			}

			intensity = minlight;
			used_minlight = true;
		}
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

	VectorCopy (staticlightdir, e->lightcache.staticlightdir);

	R_ModelLightStats_AddCall (sample_count > 0 ? sample_count : 1, used_multisample, budget_fallback, (Sys_DoubleTime () - start_time) * 1000.0);
	return used_lightgrid || used_lightpoint || used_minlight;
}
