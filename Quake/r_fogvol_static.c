#include "quakedef.h"
#include "r_fogvol_internal.h"

#define MAX_FOG_STATIC_LIGHTS 192
#define FOGVOL_GPU_LOCAL_SIZE 64

extern cvar_t r_lightmap_colorspace;

typedef struct fog_light_gpu_s
{
	float pos_rad[4];
	float col_int[4];
} fog_light_gpu_t;

typedef struct fogvol_static_field_s
{
	int dims[3];
	vec3_t mins;
	vec3_t maxs;
	vec3_t inv_size;
	float *rgb;
	float *weight;
	qboolean valid;
	qboolean build_complete;
	int next_surface;
	int total_samples;
} fogvol_static_field_t;

static int r_num_fog_static_lights = 0;
static fog_light_gpu_t r_fog_static_lights[MAX_FOG_STATIC_LIGHTS];
static fogvol_static_field_t r_fog_static_field;

static GLuint r_fogvol_static_rgb_ssbo = 0;
static GLuint r_fogvol_static_weight_ssbo = 0;
static GLuint r_fogvol_static_out_ssbo = 0;
static int r_fogvol_static_capacity_cells = 0;

static const int fogvol_static_probe_mode_fixed = 1;
static const int fogvol_static_probe_grid_fixed = 24;
static const float fogvol_static_probe_density_fixed = 1.f;

static float R_FogVol_SRGBToLinear (float c)
{
	if (c <= 0.04045f)
		return c / 12.92f;
	return powf ((c + 0.055f) / 1.055f, 2.4f);
}

static qboolean R_FogVol_SurfaceIsLava (const qmodel_t *model, const msurface_t *surf)
{
	texture_t *tex;
	int texnum;

	if (!surf)
		return false;
	if (surf->flags & SURF_DRAWLAVA)
		return true;
	if (!model || !surf->texinfo)
		return false;
	texnum = surf->texinfo->texnum;
	if (texnum < 0 || texnum >= model->numtextures)
		return false;
	tex = model->textures[texnum];
	return tex && tex->type == TEXTYPE_LAVA;
}

static qboolean R_FogVol_TryGetSurfaceLightSampleAtTexel (const qmodel_t *model, const msurface_t *surf, int sample_s, int sample_t, vec3_t out_rgb)
{
	const byte *samples;
	int bytes_per_pixel;
	int smax;
	int tmax;
	int facesize;
	float accum[3] = {0.f, 0.f, 0.f};
	float accum_weight = 0.f;
	const qboolean is_lava = R_FogVol_SurfaceIsLava (model, surf);
	const qboolean use_rgb = (model && model->has_lightdata_rgb && model->lightdata && model->lightdata_rgb);

	if (!model || !surf)
		return false;
	if (!surf->samples && !is_lava)
		return false;

	VectorClear (out_rgb);

	if (surf->samples)
	{
		samples = surf->samples;
		bytes_per_pixel = (model->flags & MOD_HDRLIGHTING) ? 4 : 3;
		if (use_rgb)
		{
			const ptrdiff_t offset = samples - model->lightdata;
			if (offset >= 0 && bytes_per_pixel > 0 && (offset % bytes_per_pixel) == 0)
			{
				const size_t sample_offset = (size_t)offset / (size_t)bytes_per_pixel;
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
		if (smax <= 0 || tmax <= 0)
			return false;
		facesize = smax * tmax * bytes_per_pixel;
		sample_s = CLAMP (0, sample_s, smax - 1);
		sample_t = CLAMP (0, sample_t, tmax - 1);

		for (int map = 0; map < MAXLIGHTMAPS && surf->styles[map] != INVALID_LIGHTSTYLE; ++map)
		{
			const float style_scale = ((surf->styles[map] < 256) ? d_lightstylevalue[surf->styles[map]] : d_lightstylevalue[0]) * (1.f / 256.f);
			const byte *lightmap = samples + facesize * map;
			const byte *texel = lightmap + (sample_t * smax + sample_s) * bytes_per_pixel;
			accum[0] += style_scale * texel[0];
			accum[1] += style_scale * texel[1];
			accum[2] += style_scale * texel[2];
			accum_weight += style_scale;
		}

		if (accum_weight > 0.f)
		{
			out_rgb[0] = q_max (0.f, accum[0] * (1.f / 255.f));
			out_rgb[1] = q_max (0.f, accum[1] * (1.f / 255.f));
			out_rgb[2] = q_max (0.f, accum[2] * (1.f / 255.f));
			if (!q_strcasecmp (r_lightmap_colorspace.string, "srgb"))
			{
				out_rgb[0] = R_FogVol_SRGBToLinear (out_rgb[0]);
				out_rgb[1] = R_FogVol_SRGBToLinear (out_rgb[1]);
				out_rgb[2] = R_FogVol_SRGBToLinear (out_rgb[2]);
			}
		}
	}

	if (is_lava && r_fogvol_emissive.value > 0.f)
	{
		const float lava = CLAMP (0.f, r_fogvol_lava_emissive.value, 8.f);
		if (lava > 0.f)
		{
			out_rgb[0] = q_max (out_rgb[0], 1.00f * lava);
			out_rgb[1] = q_max (out_rgb[1], 0.22f * lava);
			out_rgb[2] = q_max (out_rgb[2], 0.06f * lava);
		}
	}

	return (out_rgb[0] > 0.f || out_rgb[1] > 0.f || out_rgb[2] > 0.f);
}

static void R_FogVol_GPUStaticBuildReset (void)
{
	if (GL_DeleteBuffersFunc)
	{
		if (r_fogvol_static_rgb_ssbo)
			GL_DeleteBuffersFunc (1, &r_fogvol_static_rgb_ssbo);
		if (r_fogvol_static_weight_ssbo)
			GL_DeleteBuffersFunc (1, &r_fogvol_static_weight_ssbo);
		if (r_fogvol_static_out_ssbo)
			GL_DeleteBuffersFunc (1, &r_fogvol_static_out_ssbo);
	}

	r_fogvol_static_rgb_ssbo = 0;
	r_fogvol_static_weight_ssbo = 0;
	r_fogvol_static_out_ssbo = 0;
	r_fogvol_static_capacity_cells = 0;
}

void R_FogVol_ResetStaticLights (void)
{
	r_num_fog_static_lights = 0;
}

void R_FogVol_FreeStaticField (void)
{
	if (r_fog_static_field.rgb)
		q_free (r_fog_static_field.rgb);
	if (r_fog_static_field.weight)
		q_free (r_fog_static_field.weight);
	R_FogVol_GPUStaticBuildReset ();
	memset (&r_fog_static_field, 0, sizeof (r_fog_static_field));
}

static qboolean R_FogVol_GPUStaticBuildAvailable (void)
{
	return GL_DispatchComputeFunc
		&& GL_MemoryBarrierFunc
		&& GL_MapBufferRangeFunc
		&& GL_UnmapBufferFunc
		&& glprogs.fogvol_static_build;
}

static qboolean R_FogVol_GPUStaticBuildEnsureBuffers (int total_cells)
{
	if (!R_FogVol_GPUStaticBuildAvailable () || total_cells <= 0)
		return false;

	if (!r_fogvol_static_rgb_ssbo)
		GL_GenBuffersFunc (1, &r_fogvol_static_rgb_ssbo);
	if (!r_fogvol_static_weight_ssbo)
		GL_GenBuffersFunc (1, &r_fogvol_static_weight_ssbo);
	if (!r_fogvol_static_out_ssbo)
		GL_GenBuffersFunc (1, &r_fogvol_static_out_ssbo);
	if (!r_fogvol_static_rgb_ssbo || !r_fogvol_static_weight_ssbo || !r_fogvol_static_out_ssbo)
		return false;

	if (r_fogvol_static_capacity_cells < total_cells)
	{
		r_fogvol_static_capacity_cells = total_cells;
		GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fogvol_static_rgb_ssbo);
		GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, sizeof (float) * 3 * r_fogvol_static_capacity_cells, NULL, GL_DYNAMIC_DRAW);
		GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fogvol_static_weight_ssbo);
		GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, sizeof (float) * r_fogvol_static_capacity_cells, NULL, GL_DYNAMIC_DRAW);
	}

	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fogvol_static_out_ssbo);
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, sizeof (fog_light_gpu_t) * MAX_FOG_STATIC_LIGHTS, NULL, GL_DYNAMIC_DRAW);
	return true;
}

static qboolean R_FogVol_SummarizeStaticLightsFromFieldGPU (int nx, int ny, int nz, int total, float sx, float sy, float sz, float radius, int stride)
{
	const int out_slots = q_min (MAX_FOG_STATIC_LIGHTS, (total + stride - 1) / stride);
	const int groups = (out_slots + (FOGVOL_GPU_LOCAL_SIZE - 1)) / FOGVOL_GPU_LOCAL_SIZE;
	fog_light_gpu_t gpu_out[MAX_FOG_STATIC_LIGHTS];
	void *mapped;

	if (out_slots <= 0)
		return false;
	if (!R_FogVol_GPUStaticBuildEnsureBuffers (total))
		return false;

	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fogvol_static_rgb_ssbo);
	GL_BufferSubDataFunc (GL_SHADER_STORAGE_BUFFER, 0, sizeof (float) * 3 * total, r_fog_static_field.rgb);
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fogvol_static_weight_ssbo);
	GL_BufferSubDataFunc (GL_SHADER_STORAGE_BUFFER, 0, sizeof (float) * total, r_fog_static_field.weight);

	GL_UseProgram (glprogs.fogvol_static_build);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, r_fogvol_static_rgb_ssbo, 0, sizeof (float) * 3 * q_max (1, total));
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, r_fogvol_static_weight_ssbo, 0, sizeof (float) * q_max (1, total));
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, r_fogvol_static_out_ssbo, 0, sizeof (fog_light_gpu_t) * MAX_FOG_STATIC_LIGHTS);
	GL_Uniform4fFunc (0, (float)total, (float)stride, (float)out_slots, (float)nx);
	GL_Uniform4fFunc (1, (float)ny, (float)nz, 0.f, 0.f);
	GL_Uniform4fFunc (2, r_fog_static_field.mins[0], r_fog_static_field.mins[1], r_fog_static_field.mins[2], 0.f);
	GL_Uniform4fFunc (3, sx, sy, sz, radius);
	GL_Uniform4fFunc (4, 0.65f, 0.f, 0.f, 0.f);
	GL_DispatchComputeFunc (groups, 1, 1);
	GL_MemoryBarrierFunc (GL_SHADER_STORAGE_BARRIER_BIT);

	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fogvol_static_out_ssbo);
	mapped = GL_MapBufferRangeFunc (GL_SHADER_STORAGE_BUFFER, 0, sizeof (fog_light_gpu_t) * out_slots, GL_MAP_READ_BIT);
	if (!mapped)
		return false;
	memcpy (gpu_out, mapped, sizeof (fog_light_gpu_t) * out_slots);
	if (!GL_UnmapBufferFunc (GL_SHADER_STORAGE_BUFFER))
		return false;

	r_num_fog_static_lights = 0;
	for (int i = 0; i < out_slots && r_num_fog_static_lights < MAX_FOG_STATIC_LIGHTS; ++i)
	{
		if (gpu_out[i].col_int[3] <= 0.f || gpu_out[i].pos_rad[3] <= 0.f)
			continue;
		r_fog_static_lights[r_num_fog_static_lights++] = gpu_out[i];
	}

	return true;
}

static qboolean R_FogVol_WorldToStaticFieldCell (const vec3_t pos, int out_cell[3])
{
	for (int axis = 0; axis < 3; ++axis)
	{
		float f;
		if (!r_fog_static_field.valid || r_fog_static_field.dims[axis] <= 0)
			return false;
		f = (pos[axis] - r_fog_static_field.mins[axis]) * r_fog_static_field.inv_size[axis] * (float)r_fog_static_field.dims[axis];
		out_cell[axis] = CLAMP (0, (int)f, r_fog_static_field.dims[axis] - 1);
	}
	return true;
}

static qboolean R_FogVol_SurfaceTexelToWorld (const msurface_t *surf, float sample_s, float sample_t, vec3_t out_pos)
{
	vec3_t row0, row1, row2;
	vec3_t rhs;
	vec3_t cross12, cross20, cross01;
	float det;

	if (!surf || !surf->texinfo || !surf->plane)
		return false;

	VectorSet (row0, surf->texinfo->vecs[0][0], surf->texinfo->vecs[0][1], surf->texinfo->vecs[0][2]);
	VectorSet (row1, surf->texinfo->vecs[1][0], surf->texinfo->vecs[1][1], surf->texinfo->vecs[1][2]);
	VectorCopy (surf->plane->normal, row2);
	VectorSet (rhs,
		(float)surf->texturemins[0] + sample_s * 16.f - surf->texinfo->vecs[0][3],
		(float)surf->texturemins[1] + sample_t * 16.f - surf->texinfo->vecs[1][3],
		surf->plane->dist);

	CrossProduct (row1, row2, cross12);
	det = DotProduct (row0, cross12);
	if (fabsf (det) < 1e-6f)
		return false;
	CrossProduct (rhs, row2, cross20);
	CrossProduct (row1, rhs, cross01);
	out_pos[0] = DotProduct (rhs, cross12) / det;
	out_pos[1] = DotProduct (row0, cross20) / det;
	out_pos[2] = DotProduct (row0, cross01) / det;
	return true;
}

void R_FogVol_SummarizeStaticLightsFromField (void)
{
	const int nx = r_fog_static_field.dims[0];
	const int ny = r_fog_static_field.dims[1];
	const int nz = r_fog_static_field.dims[2];
	const int total = nx * ny * nz;
	const float sx = (r_fog_static_field.maxs[0] - r_fog_static_field.mins[0]) / (float)q_max (1, nx);
	const float sy = (r_fog_static_field.maxs[1] - r_fog_static_field.mins[1]) / (float)q_max (1, ny);
	const float sz = (r_fog_static_field.maxs[2] - r_fog_static_field.mins[2]) / (float)q_max (1, nz);
	const float radius = CLAMP (32.f, 1.25f * q_max (sx, q_max (sy, sz)), 320.f);
	struct { int idx; float score; } best[MAX_FOG_STATIC_LIGHTS];
	int best_count = 0;
	int best_min_slot = 0;
	float best_min_score = 0.f;

	r_num_fog_static_lights = 0;
	if (!r_fog_static_field.valid || total <= 0)
		return;
	if (r_fogvol_gpu_static_build.value > 0.f)
	{
		if (R_FogVol_SummarizeStaticLightsFromFieldGPU (nx, ny, nz, total, sx, sy, sz, radius, q_max (1, total / MAX_FOG_STATIC_LIGHTS)))
			return;
	}

	for (int i = 0; i < total; ++i)
	{
		const float w = r_fog_static_field.weight[i];
		const float e0 = q_max (0.f, r_fog_static_field.rgb[i * 3 + 0]);
		const float e1 = q_max (0.f, r_fog_static_field.rgb[i * 3 + 1]);
		const float e2 = q_max (0.f, r_fog_static_field.rgb[i * 3 + 2]);
		const float score = e0 + e1 + e2;
		if (w <= 0.f || score <= 0.f)
			continue;

		if (best_count < MAX_FOG_STATIC_LIGHTS)
		{
			best[best_count].idx = i;
			best[best_count].score = score;
			if (best_count == 0 || score < best_min_score)
			{
				best_min_score = score;
				best_min_slot = best_count;
			}
			best_count++;
		}
		else if (score > best_min_score)
		{
			best[best_min_slot].idx = i;
			best[best_min_slot].score = score;
			best_min_score = best[0].score;
			best_min_slot = 0;
			for (int k = 1; k < best_count; ++k)
			{
				if (best[k].score < best_min_score)
				{
					best_min_score = best[k].score;
					best_min_slot = k;
				}
			}
		}
	}

	for (int n = 0; n < best_count; ++n)
	{
		const int i = best[n].idx;
		const float w = r_fog_static_field.weight[i];
		const int x = i % nx;
		const int y = (i / nx) % ny;
		const int z = i / (nx * ny);
		if (w <= 0.f)
			continue;
		r_fog_static_lights[r_num_fog_static_lights].pos_rad[0] = r_fog_static_field.mins[0] + ((float)x + 0.5f) * sx;
		r_fog_static_lights[r_num_fog_static_lights].pos_rad[1] = r_fog_static_field.mins[1] + ((float)y + 0.5f) * sy;
		r_fog_static_lights[r_num_fog_static_lights].pos_rad[2] = r_fog_static_field.mins[2] + ((float)z + 0.5f) * sz;
		r_fog_static_lights[r_num_fog_static_lights].pos_rad[3] = radius;
		r_fog_static_lights[r_num_fog_static_lights].col_int[0] = q_max (0.f, r_fog_static_field.rgb[i * 3 + 0] / w) * 0.65f;
		r_fog_static_lights[r_num_fog_static_lights].col_int[1] = q_max (0.f, r_fog_static_field.rgb[i * 3 + 1] / w) * 0.65f;
		r_fog_static_lights[r_num_fog_static_lights].col_int[2] = q_max (0.f, r_fog_static_field.rgb[i * 3 + 2] / w) * 0.65f;
		r_fog_static_lights[r_num_fog_static_lights].col_int[3] = 1.f;
		r_num_fog_static_lights++;
	}
}

void R_FogVol_AccumulateStaticLightsAtPoint (const vec3_t p, vec3_t out_rgb)
{
	for (int i = 0; i < r_num_fog_static_lights; ++i)
	{
		const float radius = q_max (r_fog_static_lights[i].pos_rad[3], 1e-3f);
		const float dx = r_fog_static_lights[i].pos_rad[0] - p[0];
		const float dy = r_fog_static_lights[i].pos_rad[1] - p[1];
		const float dz = r_fog_static_lights[i].pos_rad[2] - p[2];
		const float dist2 = dx * dx + dy * dy + dz * dz;
		const float radius2 = radius * radius;
		float normDist2;
		float x;
		float window;
		float invSq;
		float atten;
		float energy;

		if (dist2 >= radius2)
			continue;

		normDist2 = dist2 / q_max (radius2, 1e-6f);
		x = CLAMP (0.f, 1.f, 1.f - normDist2);
		window = x * x * (3.f - 2.f * x);
		invSq = 1.f / (1.f + normDist2 * 6.f);
		atten = window * invSq;
		if (atten <= 1e-5f)
			continue;

		energy = q_max (0.f, r_fog_static_lights[i].col_int[3]) * atten;
		out_rgb[0] += q_max (0.f, r_fog_static_lights[i].col_int[0]) * energy;
		out_rgb[1] += q_max (0.f, r_fog_static_lights[i].col_int[1]) * energy;
		out_rgb[2] += q_max (0.f, r_fog_static_lights[i].col_int[2]) * energy;
	}
}

void R_FogVol_ContinueStaticFieldBuild (int sample_budget)
{
	qmodel_t *model = cl.worldmodel;
	const float density = q_max (0.25f, fogvol_static_probe_density_fixed);
	const int texel_step = CLAMP (1, (int)Q_rint (2.5f / density), 8);

	if (!r_fog_static_field.valid || r_fog_static_field.build_complete)
		return;
	if (!model || model->type != mod_brush || !model->surfaces)
		return;

	while (r_fog_static_field.next_surface < model->numsurfaces && sample_budget > 0)
	{
		msurface_t *surf = &model->surfaces[r_fog_static_field.next_surface++];
		const qboolean is_lava = R_FogVol_SurfaceIsLava (model, surf);
		vec3_t sample_rgb;
		vec3_t sample_pos;
		int smax;
		int tmax;

		if (!surf->samples && !is_lava)
			continue;
		if (!surf->texinfo || surf->texinfo->texnum < 0 || surf->texinfo->texnum >= model->numtextures)
			continue;
		if (!model->textures[surf->texinfo->texnum])
			continue;
		if ((surf->texinfo->flags & TEX_SPECIAL) && !is_lava)
			continue;

		smax = (surf->extents[0] >> 4) + 1;
		tmax = (surf->extents[1] >> 4) + 1;
		if (smax <= 0 || tmax <= 0)
			continue;

		for (int t = 0; t < tmax && sample_budget > 0; t += texel_step)
		for (int s = 0; s < smax && sample_budget > 0; s += texel_step)
		{
			int cell[3];
			int idx;
			if (!R_FogVol_TryGetSurfaceLightSampleAtTexel (model, surf, s, t, sample_rgb))
			{
				sample_budget--;
				continue;
			}
			if (!R_FogVol_SurfaceTexelToWorld (surf, (float)s, (float)t, sample_pos))
			{
				sample_budget--;
				continue;
			}
			if (!R_FogVol_WorldToStaticFieldCell (sample_pos, cell))
			{
				sample_budget--;
				continue;
			}
			idx = cell[0] + r_fog_static_field.dims[0] * (cell[1] + r_fog_static_field.dims[1] * cell[2]);
			r_fog_static_field.rgb[idx * 3 + 0] += sample_rgb[0];
			r_fog_static_field.rgb[idx * 3 + 1] += sample_rgb[1];
			r_fog_static_field.rgb[idx * 3 + 2] += sample_rgb[2];
			r_fog_static_field.weight[idx] += 1.f;
			r_fog_static_field.total_samples++;
			sample_budget--;
		}
	}

	if (r_fog_static_field.next_surface >= model->numsurfaces)
	{
		r_fog_static_field.build_complete = true;
		R_FogVol_SummarizeStaticLightsFromField ();
	}
}

void R_FogVol_BuildStaticLightInjection (void)
{
	qmodel_t *model = cl.worldmodel;
	int dims;
	size_t total_cells;

	r_num_fog_static_lights = 0;
	R_FogVol_FreeStaticField ();
	if (!model || model->type != mod_brush || model->numsurfaces <= 0)
	{
		R_FogVol_CommitStaticBuildConfig ();
		return;
	}

	dims = CLAMP (8, fogvol_static_probe_grid_fixed, 64);
	total_cells = (size_t)dims * (size_t)dims * (size_t)dims;
	r_fog_static_field.rgb = (float *)q_calloc(total_cells * 3u, sizeof (float));
	r_fog_static_field.weight = (float *)q_calloc(total_cells, sizeof (float));
	if (!r_fog_static_field.rgb || !r_fog_static_field.weight)
	{
		R_FogVol_FreeStaticField ();
		R_FogVol_CommitStaticBuildConfig ();
		return;
	}

	r_fog_static_field.dims[0] = dims;
	r_fog_static_field.dims[1] = dims;
	r_fog_static_field.dims[2] = dims;
	VectorCopy (model->mins, r_fog_static_field.mins);
	VectorCopy (model->maxs, r_fog_static_field.maxs);
	for (int axis = 0; axis < 3; ++axis)
	{
		const float extent = r_fog_static_field.maxs[axis] - r_fog_static_field.mins[axis];
		r_fog_static_field.inv_size[axis] = (extent > 1e-6f) ? (1.f / extent) : 1.f;
	}
	r_fog_static_field.valid = true;
	r_fog_static_field.build_complete = false;
	r_fog_static_field.next_surface = 0;
	r_fog_static_field.total_samples = 0;

	R_FogVol_ContinueStaticFieldBuild (q_max (1, (int)Q_rint (r_fogvol_static_probe_budget_load.value)));

	if (developer.value > 0.f)
	{
		Con_DPrintf ("FogVol: static probes mode=%d lights=%d samples=%d complete=%d\n",
			fogvol_static_probe_mode_fixed, r_num_fog_static_lights,
			r_fog_static_field.total_samples, r_fog_static_field.build_complete ? 1 : 0);
	}
	R_FogVol_CommitStaticBuildConfig ();
}

qboolean R_FogVol_StaticFieldNeedsBuildContinuation (void)
{
	return (r_fog_static_field.valid && !r_fog_static_field.build_complete);
}

int R_FogVol_GetStaticFieldTotalSamples (void)
{
	return r_fog_static_field.total_samples;
}

int R_FogVol_GetStaticLightCount (void)
{
	return r_num_fog_static_lights;
}

qboolean R_FogVol_GetStaticLight (int idx, vec4_t pos_rad, vec4_t col_int)
{
	if (idx < 0 || idx >= r_num_fog_static_lights)
		return false;
	if (pos_rad)
	{
		pos_rad[0] = r_fog_static_lights[idx].pos_rad[0];
		pos_rad[1] = r_fog_static_lights[idx].pos_rad[1];
		pos_rad[2] = r_fog_static_lights[idx].pos_rad[2];
		pos_rad[3] = r_fog_static_lights[idx].pos_rad[3];
	}
	if (col_int)
	{
		col_int[0] = r_fog_static_lights[idx].col_int[0];
		col_int[1] = r_fog_static_lights[idx].col_int[1];
		col_int[2] = r_fog_static_lights[idx].col_int[2];
		col_int[3] = r_fog_static_lights[idx].col_int[3];
	}
	return true;
}
