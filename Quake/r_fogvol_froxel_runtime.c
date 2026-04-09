#include "quakedef.h"
#include "draw.h"
#include "r_fogvol.h"
#include "r_fogvol_internal.h"
#include "r_dlight_pool.h"
#include "r_realtimelight.h"
#include <math.h>
#include <string.h>

extern cvar_t gl_farclip;
extern cvar_t r_dlight_entities;

typedef struct froxel_gpu_light_s
{
	float pos_rad[4];
	float color_intensity[4];
	uint32_t type;
	uint32_t _pad[3];
} froxel_gpu_light_t;

COMPILE_TIME_ASSERT (froxel_gpu_light_std430_size, sizeof (froxel_gpu_light_t) == 48);

typedef struct froxel_state_s
{
	GLuint light_tex;
	GLuint history_tex;
	GLuint light_ssbo;
	int dims[3];
	int light_count;
	float near_clip;
	float far_clip;
	float tan_half_fov_x;
	float tan_half_fov_y;
	float log_far_near;
	vec3_t prev_vieworg;
	qboolean prev_valid;
	qboolean valid;
	int prev_mode;
} froxel_state_t;

#define MAX_FROXEL_GPU_LIGHTS 32
#define MAX_FROXEL_DEBUG_LIGHTS 5
#define MAX_FROXEL_LAVA_LIGHTS 4

static froxel_state_t r_froxel;
static froxel_gpu_light_t r_froxel_gpu_lights[MAX_FROXEL_GPU_LIGHTS];

typedef struct froxel_ppdlight_debug_stats_s
{
	int source_count;
	int fog_eligible_count;
	int injected_count;
	int rejected_nonvolumetric;
	int rejected_distance;
	int rejected_local_budget;
	int rejected_budget;
	int gi_candidates;
	int gi_injected_count;
	int gi_rejected_distance;
	int gi_rejected_budget;
	float injected_radiance;
	float gi_radiance;
} froxel_ppdlight_debug_stats_t;

static froxel_ppdlight_debug_stats_t r_froxel_ppd_stats;

typedef struct froxel_debug_state_s
{
	double next_refresh_time;
	int random_light_count;
	int phase;
	uint32_t grid_seed;
	float dlight_scale;
	float sun_scale;
	float emissive_scale;
	float sun_intensity;
	vec3_t sun_dir;
	vec3_t sun_color;
	froxel_gpu_light_t random_lights[MAX_FROXEL_DEBUG_LIGHTS];
	qboolean active;
} froxel_debug_state_t;

static froxel_debug_state_t r_froxel_debug;
static float r_froxel_debug_random_slice[192 * 128 * 4];

static qboolean R_Froxel_ShouldPrintStats (void)
{
	if (r_fogvol_stats.value <= 0.f)
		return false;
	/* Keep stats readable: one sample per second at 60fps. */
	return (r_framecount % 60) == 0;
}

typedef struct froxel_lava_candidate_s
{
	vec3_t center;
	float extent_radius;
	float score;
} froxel_lava_candidate_t;

static void R_Froxel_AddLight (const vec3_t origin, float radius, const vec3_t color, float intensity, uint32_t type);
static void R_Froxel_InjectPPDLights (void);
static void R_Froxel_InjectPPDGIHelper (const rl_light_t *lights, int light_count);

static float R_Froxel_DlightRadiusForFog (float radius, uint32_t type)
{
	float scale = 0.70f;
	float cap;

	switch ((dlighttype_t)type)
	{
	case DLIGHT_EXPLOSION:
		scale = 0.85f;
		break;
	case DLIGHT_ROCKET:
	case DLIGHT_PLASMA:
	case DLIGHT_LIGHTNING:
	case DLIGHT_TELEPORT:
		scale = 0.76f;
		break;
	case DLIGHT_TORCH:
		scale = 0.62f;
		break;
	case DLIGHT_LAVA:
		scale = 0.68f;
		break;
	case DLIGHT_DEFAULT:
	default:
		scale = 0.70f;
		break;
	}

	/* Fog receives a more compact dlight footprint than opaque shading.
	 * Keep a hard cap so farclip cannot inflate local fog lighting excessively. */
	cap = q_max (192.f, q_min (640.f, r_froxel.far_clip * 0.24f));
	return CLAMP (32.f, radius * scale, cap);
}

static qboolean R_Froxel_IsViewMuzzleDlight (const dlight_t *dl)
{
	if (!dl)
		return false;
	if (dl->type != DLIGHT_DEFAULT)
		return false;
	if (dl->kind != DL_TRANSIENT)
		return false;
	if (dl->key != cl.viewentity)
		return false;
	if (dl->minlight <= 0.f)
		return false;
	/* Keep the match tight to short-lived muzzle flashes only. */
	if ((dl->die - dl->spawn) > 0.25f)
		return false;
	return true;
}

static qboolean R_Froxel_IsViewMuzzlePPDLight (const rl_light_t *src, uint32_t fog_type)
{
	if (!src)
		return false;
	if ((rl_light_type_t)src->type != RL_LIGHT_POINT)
		return false;
	if (fog_type != (uint32_t)DLIGHT_DEFAULT)
		return false;
	return src->source_id == (unsigned int)cl.viewentity;
}

static void R_Froxel_ApplyViewMuzzleFogClamp (const vec3_t origin, float *radius, vec3_t color, float *intensity)
{
	vec3_t to_cam;
	float dist;
	float proximity;
	float radius_scale;
	float energy_scale;

	if (!origin || !radius || !color || !intensity)
		return;

	VectorSubtract (origin, r_refdef.vieworg, to_cam);
	dist = VectorLength (to_cam);
	/* Very close muzzle lights can blow out local fog scattering.
	 * Fade suppression out by ~240u so only first-person flashes are affected. */
	proximity = CLAMP (0.f, (dist - 48.f) * (1.f / 192.f), 1.f);
	radius_scale = 0.38f + 0.62f * proximity;
	energy_scale = 0.22f + 0.78f * proximity;

	*radius = q_max (24.f, *radius * radius_scale);
	VectorScale (color, energy_scale, color);
	*intensity *= energy_scale;
}

static qboolean R_Froxel_SurfaceTextureIsLava (const qmodel_t *world, const msurface_t *surf)
{
	const texture_t *tex;
	int texnum;

	if (!world || !surf || !surf->texinfo)
		return false;

	texnum = surf->texinfo->texnum;
	if (texnum < 0 || texnum >= world->numtextures)
		return false;

	tex = world->textures[texnum];
	if (!tex)
		return false;
	if (tex->type == TEXTYPE_LAVA)
		return true;
	if (tex->name[0] && q_strcasestr (tex->name, "lava"))
		return true;
	if (tex->material_map && tex->material_map[0] && q_strcasestr (tex->material_map, "lava"))
		return true;
	return false;
}

static qboolean R_Froxel_PointInLavaLeaf (qmodel_t *world, const vec3_t point)
{
	mleaf_t *leaf;
	vec3_t probe;

	if (!world)
		return false;

	VectorCopy (point, probe);
	leaf = Mod_PointInLeaf (probe, world);
	return leaf && leaf->contents == CONTENTS_LAVA;
}

static qboolean R_Froxel_SurfaceIsLava (qmodel_t *world, const msurface_t *surf, const vec3_t center)
{
	vec3_t probe;

	if (!world || !surf || !center)
		return false;

	if (R_Froxel_SurfaceTextureIsLava (world, surf))
		return true;
	if (surf->flags & SURF_DRAWLAVA)
		return true;
	/* Fallback for maps/materials where lava flagging is missing:
	 * sample nearby leaf contents on both sides of the surface plane. */
	if ((surf->flags & SURF_DRAWTURB) == 0 || !surf->plane)
		return false;

	VectorMA (center, 2.f, surf->plane->normal, probe);
	if (R_Froxel_PointInLavaLeaf (world, probe))
		return true;
	VectorMA (center, -2.f, surf->plane->normal, probe);
	if (R_Froxel_PointInLavaLeaf (world, probe))
		return true;

	return false;
}

static int R_Froxel_InjectLavaProbeFallbackLights (qmodel_t *world, float lava_emissive)
{
	static const vec3_t probe_offsets[] = {
		{   0.f,   0.f, -48.f }, {   0.f,   0.f, -112.f },
		{  96.f,   0.f, -64.f }, { -96.f,   0.f, -64.f },
		{   0.f,  96.f, -64.f }, {   0.f, -96.f, -64.f },
		{ 128.f, 128.f, -72.f }, { 128.f, -128.f, -72.f },
		{-128.f, 128.f, -72.f }, {-128.f, -128.f, -72.f }
	};
	int injected = 0;

	if (!world || lava_emissive <= 0.f)
		return 0;

	for (int i = 0; i < (int)countof (probe_offsets) && r_froxel.light_count < MAX_FROXEL_GPU_LIGHTS; ++i)
	{
		vec3_t probe;
		vec3_t color;

		VectorAdd (r_refdef.vieworg, probe_offsets[i], probe);
		if (!R_Froxel_PointInLavaLeaf (world, probe))
			continue;

		VectorSet (color, 1.75f, 0.42f, 0.10f);
		R_Froxel_AddLight (probe, 240.f, color, lava_emissive * 2.2f, (uint32_t)DLIGHT_LAVA);
		injected++;
	}

	return injected;
}

static int R_Froxel_DebugMode (void)
{
	return CLAMP (0, (int)Q_rint (r_fogvol_debug_froxel_random.value), 4);
}

static qboolean R_Froxel_DebugEnabled (void)
{
	return (R_Froxel_DebugMode () > 0);
}

static float R_Froxel_DebugRand01 (void)
{
	return (float)(rand () & 0x7fff) * (1.f / 32767.f);
}

static float R_Froxel_DebugRandRange (float lo, float hi)
{
	return lo + (hi - lo) * R_Froxel_DebugRand01 ();
}

static float R_Froxel_DebugHash01 (int x, int y, int z, uint32_t seed)
{
	uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u ^ seed;
	h ^= h >> 13;
	h *= 1274126177u;
	h ^= h >> 16;
	return (float)(h & 0x00ffffffu) * (1.f / 16777215.f);
}

static void R_Froxel_DebugRandomUnitVector (vec3_t out)
{
	float len2 = 0.f;
	int attempt = 0;

	do
	{
		out[0] = R_Froxel_DebugRandRange (-1.f, 1.f);
		out[1] = R_Froxel_DebugRandRange (-1.f, 1.f);
		out[2] = R_Froxel_DebugRandRange (-1.f, 1.f);
		len2 = DotProduct (out, out);
	}
	while (len2 < 1e-5f && ++attempt < 8);

	if (len2 < 1e-5f)
	{
		VectorSet (out, 0.f, 0.f, -1.f);
		return;
	}

	VectorScale (out, 1.f / sqrtf (len2), out);
}

static void R_Froxel_DebugUploadRandomGrid (void)
{
	const int nx = r_froxel.dims[0];
	const int ny = r_froxel.dims[1];
	const int nz = r_froxel.dims[2];
	const int debug_mode = R_Froxel_DebugMode ();
	const int slice_pixels = nx * ny;
	const int block_x = (debug_mode <= 1) ? q_max (3, nx / 11) : q_max (2, nx / 7);
	const int block_y = (debug_mode <= 1) ? q_max (3, ny / 10) : q_max (2, ny / 6);
	const int block_z = (debug_mode <= 1) ? q_max (2, nz / 9) : q_max (1, nz / 6);
	const float debug_strength = (debug_mode <= 1) ? 0.38f : 1.0f;
	float *slice = r_froxel_debug_random_slice;

	if (!r_froxel.valid || !r_froxel.light_tex || !r_froxel.history_tex)
		return;
	if (nx <= 0 || ny <= 0 || nz <= 0 || slice_pixels <= 0)
		return;
	if (nx > 192 || ny > 128)
		return;

	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_3D, r_froxel.light_tex);
	for (int z = 0; z < nz; ++z)
	{
		for (int i = 0; i < slice_pixels; ++i)
		{
			const int x = i % nx;
			const int y = i / nx;
			const int cx = x / block_x;
			const int cy = y / block_y;
			const int cz = z / block_z;
			const float n0 = R_Froxel_DebugHash01 (cx, cy, cz, r_froxel_debug.grid_seed);
			const float n1 = R_Froxel_DebugHash01 (cx + 11, cy + 7, cz + 3, r_froxel_debug.grid_seed ^ 0x9e3779b9u);
			const float n2 = R_Froxel_DebugHash01 (cx + 19, cy + 13, cz + 5, r_froxel_debug.grid_seed ^ 0x85ebca6bu);
			const float e = R_Froxel_DebugHash01 (cx + 3, cy + 23, cz + 29, r_froxel_debug.grid_seed ^ 0xc2b2ae35u);
			const float band = ((cz + r_froxel_debug.phase) & 1) ? 0.58f : 1.35f;
			const float energy = ((0.06f + e * e * 2.9f) * band) * debug_strength;
			slice[i * 4 + 0] = energy * (0.18f + n0 * 2.2f);
			slice[i * 4 + 1] = energy * (0.18f + n1 * 2.2f);
			slice[i * 4 + 2] = energy * (0.18f + n2 * 2.2f);
			slice[i * 4 + 3] = 1.f;
		}
		GL_TexSubImage3DFunc (GL_TEXTURE_3D, 0, 0, 0, z, nx, ny, 1, GL_RGBA, GL_FLOAT, slice);
	}

	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_3D, r_froxel.history_tex);
	for (int z = 0; z < nz; ++z)
	{
		for (int i = 0; i < slice_pixels; ++i)
		{
			const int x = i % nx;
			const int y = i / nx;
			const int cx = x / block_x;
			const int cy = y / block_y;
			const int cz = z / block_z;
			const float n0 = R_Froxel_DebugHash01 (cx + 31, cy + 5, cz + 17, r_froxel_debug.grid_seed ^ 0x27d4eb2du);
			const float n1 = R_Froxel_DebugHash01 (cx + 2, cy + 37, cz + 9, r_froxel_debug.grid_seed ^ 0x165667b1u);
			const float n2 = R_Froxel_DebugHash01 (cx + 43, cy + 3, cz + 21, r_froxel_debug.grid_seed ^ 0x7feb352du);
			const float e = R_Froxel_DebugHash01 (cx + 29, cy + 11, cz + 13, r_froxel_debug.grid_seed ^ 0x846ca68bu);
			const float band = ((cz + r_froxel_debug.phase + 1) & 1) ? 0.62f : 1.25f;
			const float energy = ((0.06f + e * e * 2.9f) * band) * debug_strength;
			slice[i * 4 + 0] = energy * (0.18f + n0 * 2.2f);
			slice[i * 4 + 1] = energy * (0.18f + n1 * 2.2f);
			slice[i * 4 + 2] = energy * (0.18f + n2 * 2.2f);
			slice[i * 4 + 3] = 1.f;
		}
		GL_TexSubImage3DFunc (GL_TEXTURE_3D, 0, 0, 0, z, nx, ny, 1, GL_RGBA, GL_FLOAT, slice);
	}
}

static void R_Froxel_DebugBuildRandomLights (void)
{
	const float max_dist = q_max (96.f, q_min (384.f, r_froxel.far_clip * 0.22f));
	const float max_radius = q_max (64.f, q_min (176.f, r_froxel.far_clip * 0.14f));

	r_froxel_debug.random_light_count = 1 + (rand () % MAX_FROXEL_DEBUG_LIGHTS);
	for (int i = 0; i < r_froxel_debug.random_light_count; ++i)
	{
		froxel_gpu_light_t *out = &r_froxel_debug.random_lights[i];
		vec3_t dir;
		vec3_t origin;

		R_Froxel_DebugRandomUnitVector (dir);
		VectorMA (r_refdef.vieworg, R_Froxel_DebugRandRange (28.f, max_dist), dir, origin);

		out->pos_rad[0] = origin[0];
		out->pos_rad[1] = origin[1];
		out->pos_rad[2] = origin[2];
		out->pos_rad[3] = R_Froxel_DebugRandRange (36.f, max_radius);
		out->color_intensity[0] = R_Froxel_DebugRandRange (0.25f, 2.2f);
		out->color_intensity[1] = R_Froxel_DebugRandRange (0.25f, 2.2f);
		out->color_intensity[2] = R_Froxel_DebugRandRange (0.25f, 2.2f);
		out->color_intensity[3] = R_Froxel_DebugRandRange (1.2f, 4.0f);
		out->type = (uint32_t)DLIGHT_DEFAULT;
		out->_pad[0] = out->_pad[1] = out->_pad[2] = 0;
	}
}

static void R_Froxel_DebugRefreshState (void)
{
	if (!R_Froxel_DebugEnabled ())
	{
		r_froxel_debug.active = false;
		return;
	}
	if (!r_froxel.valid)
		return;
	if (r_froxel_debug.active && realtime < r_froxel_debug.next_refresh_time)
		return;

	r_froxel_debug.active = true;
	r_froxel_debug.next_refresh_time = realtime + 2.0;
	r_froxel_debug.phase = rand () & 3;
	r_froxel_debug.grid_seed = (uint32_t)rand () ^ ((uint32_t)(r_framecount * 1103515245u));
	switch (r_froxel_debug.phase)
	{
	default:
	case 0: /* dlight focus */
		r_froxel_debug.dlight_scale = R_Froxel_DebugRandRange (2.4f, 5.6f);
		r_froxel_debug.sun_scale = R_Froxel_DebugRandRange (0.35f, 0.9f);
		r_froxel_debug.emissive_scale = R_Froxel_DebugRandRange (0.10f, 0.45f);
		break;
	case 1: /* sun/shadow focus */
		r_froxel_debug.dlight_scale = R_Froxel_DebugRandRange (0.8f, 1.7f);
		r_froxel_debug.sun_scale = R_Froxel_DebugRandRange (1.9f, 3.8f);
		r_froxel_debug.emissive_scale = R_Froxel_DebugRandRange (0.10f, 0.40f);
		break;
	case 2: /* emissive focus */
		r_froxel_debug.dlight_scale = R_Froxel_DebugRandRange (0.7f, 1.5f);
		r_froxel_debug.sun_scale = R_Froxel_DebugRandRange (0.3f, 0.9f);
		r_froxel_debug.emissive_scale = R_Froxel_DebugRandRange (0.9f, 2.4f);
		break;
	case 3: /* mixed stress test */
		r_froxel_debug.dlight_scale = R_Froxel_DebugRandRange (1.8f, 4.2f);
		r_froxel_debug.sun_scale = R_Froxel_DebugRandRange (1.0f, 2.6f);
		r_froxel_debug.emissive_scale = R_Froxel_DebugRandRange (0.5f, 1.6f);
		break;
	}
	r_froxel_debug.sun_intensity = R_Froxel_DebugRandRange (0.9f, 3.2f);
	R_Froxel_DebugRandomUnitVector (r_froxel_debug.sun_dir);
	r_froxel_debug.sun_dir[2] = -fabsf (r_froxel_debug.sun_dir[2]);
	VectorNormalize (r_froxel_debug.sun_dir);
	r_froxel_debug.sun_color[0] = R_Froxel_DebugRandRange (0.3f, 1.6f);
	r_froxel_debug.sun_color[1] = R_Froxel_DebugRandRange (0.3f, 1.6f);
	r_froxel_debug.sun_color[2] = R_Froxel_DebugRandRange (0.3f, 1.6f);
	R_Froxel_DebugBuildRandomLights ();
	R_Froxel_DebugUploadRandomGrid ();
	if (R_Froxel_ShouldPrintStats () || r_fogvol_debug.value >= 8.f)
	{
		Con_DPrintf ("fogvol_debug_froxel: phase=%d d=%.2f sun=%.2f em=%.2f lights=%d\n",
			r_froxel_debug.phase, r_froxel_debug.dlight_scale, r_froxel_debug.sun_scale,
			r_froxel_debug.emissive_scale, r_froxel_debug.random_light_count);
	}
}

static qboolean R_Froxel_EnsureResources (int nx, int ny, int nz)
{
	if (nx <= 0 || ny <= 0 || nz <= 0)
		return false;

	if (r_froxel.dims[0] != nx || r_froxel.dims[1] != ny || r_froxel.dims[2] != nz)
	{
		if (r_froxel.light_tex)
			glDeleteTextures (1, &r_froxel.light_tex);
		if (r_froxel.history_tex)
			glDeleteTextures (1, &r_froxel.history_tex);
		r_froxel.light_tex = 0;
		r_froxel.history_tex = 0;
		r_froxel.dims[0] = nx;
		r_froxel.dims[1] = ny;
		r_froxel.dims[2] = nz;
		r_froxel.prev_valid = false;
	}

	if (!r_froxel.light_tex)
		glGenTextures (1, &r_froxel.light_tex);
	if (!r_froxel.history_tex)
		glGenTextures (1, &r_froxel.history_tex);
	if (!r_froxel.light_ssbo)
		GL_GenBuffersFunc (1, &r_froxel.light_ssbo);

	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_3D, r_froxel.light_tex);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	GL_TexImage3DFunc (GL_TEXTURE_3D, 0, GL_RGBA16F, nx, ny, nz, 0, GL_RGBA, GL_FLOAT, NULL);

	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_3D, r_froxel.history_tex);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	GL_TexImage3DFunc (GL_TEXTURE_3D, 0, GL_RGBA16F, nx, ny, nz, 0, GL_RGBA, GL_FLOAT, NULL);

	return true;
}

static void R_Froxel_AddLight (const vec3_t origin, float radius, const vec3_t color, float intensity, uint32_t type)
{
	froxel_gpu_light_t *out;

	if (!r_froxel.valid || r_froxel.light_count >= MAX_FROXEL_GPU_LIGHTS)
		return;
	if (radius <= 1.f || intensity <= 0.f)
		return;

	out = &r_froxel_gpu_lights[r_froxel.light_count++];
	out->pos_rad[0] = origin[0];
	out->pos_rad[1] = origin[1];
	out->pos_rad[2] = origin[2];
	out->pos_rad[3] = radius;
	out->color_intensity[0] = q_max (0.f, color[0]);
	out->color_intensity[1] = q_max (0.f, color[1]);
	out->color_intensity[2] = q_max (0.f, color[2]);
	out->color_intensity[3] = intensity;
	out->type = type;
	out->_pad[0] = out->_pad[1] = out->_pad[2] = 0;
}

static uint32_t R_Froxel_DlightTypeFromRealtimeLight (const rl_light_t *src)
{
	if (!src)
		return (uint32_t)DLIGHT_DEFAULT;

	switch ((rl_light_type_t)src->type)
	{
	case RL_LIGHT_EMISSIVE_PROXY:
		return (uint32_t)DLIGHT_TORCH;
	case RL_LIGHT_GI_PROXY:
		return (uint32_t)DLIGHT_DEFAULT;
	case RL_LIGHT_POINT:
		/* Keep original dlight subtype (lava/torch/etc.) when available. */
		if (src->reserved < (unsigned int)DLIGHT_MAX_TYPES)
			return src->reserved;
		return (uint32_t)DLIGHT_DEFAULT;
	default:
		return (uint32_t)DLIGHT_DEFAULT;
	}
}

static void R_Froxel_InjectPPDGIHelper (const rl_light_t *lights, int light_count)
{
	int i;
	int gi_budget;
	float gi_scale;

	if (!lights || light_count <= 0)
		return;
	if (r_ppdlights_gi.value <= 0.f)
		return;

	/* Conservative helper: broad, low-energy secondary bounce proxies for fog ambience. */
	gi_scale = CLAMP (0.f, r_ppdlights_gi.value, 2.f);
	gi_budget = CLAMP (0, (int)Q_rint (r_ppdlights_gi_budget.value), MAX_FROXEL_GPU_LIGHTS);
	for (i = 0; i < light_count; ++i)
	{
		const rl_light_t *src = &lights[i];
		vec3_t to_src;
		float src_dist2;
		float luma;
		float gi_intensity;
		float gi_radius;
		float gi_max_dist;
		int before_count;
		vec3_t gi_color;

		if ((src->flags & RL_LIGHT_VOLUMETRIC_CONTRIB) == 0u)
			continue;
		if (src->intensity <= 0.f || src->radius <= 1.f)
			continue;

		r_froxel_ppd_stats.gi_candidates++;
		if (r_froxel_ppd_stats.gi_injected_count >= gi_budget || r_froxel.light_count >= MAX_FROXEL_GPU_LIGHTS)
		{
			r_froxel_ppd_stats.gi_rejected_budget++;
			continue;
		}

		luma = q_max (0.f, src->color[0] * 0.2126f + src->color[1] * 0.7152f + src->color[2] * 0.0722f);
		/* Subtle by default: small fraction of source energy as wide bounce fill. */
		gi_intensity = q_max (0.f, src->intensity) * (0.12f * gi_scale);
		gi_radius = CLAMP (96.f, src->radius * (1.7f + 0.2f * gi_scale), q_max (384.f, r_froxel.far_clip * 0.55f));
		gi_max_dist = q_max (96.f, r_froxel.far_clip + gi_radius);
		VectorSubtract (src->origin, r_refdef.vieworg, to_src);
		src_dist2 = DotProduct (to_src, to_src);
		if (src_dist2 > gi_max_dist * gi_max_dist)
		{
			r_froxel_ppd_stats.gi_rejected_distance++;
			continue;
		}
		gi_color[0] = q_max (0.f, src->color[0] * 0.35f + luma * 0.65f);
		gi_color[1] = q_max (0.f, src->color[1] * 0.35f + luma * 0.65f);
		gi_color[2] = q_max (0.f, src->color[2] * 0.35f + luma * 0.65f);

		before_count = r_froxel.light_count;
		R_Froxel_AddLight (src->origin, gi_radius, gi_color, gi_intensity, (uint32_t)DLIGHT_DEFAULT);
		if (r_froxel.light_count > before_count)
		{
			r_froxel_ppd_stats.gi_injected_count++;
			r_froxel_ppd_stats.gi_radiance +=
				(gi_color[0] * 0.2126f + gi_color[1] * 0.7152f + gi_color[2] * 0.0722f) * gi_intensity;
		}
		else
		{
			r_froxel_ppd_stats.gi_rejected_budget++;
		}
	}
}

static void R_Froxel_InjectPPDLights (void)
{
	const rl_light_t *lights;
	const int fog_budget = CLAMP (0, (int)Q_rint (r_ppdlights_fog_budget.value), MAX_FROXEL_GPU_LIGHTS);
	int light_count = 0;
	int i;

	/*
	 * Shared-light architecture:
	 * - Reads the frame list produced by R_PPdlights_CollectFrame.
	 * - Consumes only volumetric-flagged lights for froxel fog injection.
	 * - Optionally derives broad GI helper lights from the same source list.
	 * World/model forward passes consume their own subsets independently.
	 */
	memset (&r_froxel_ppd_stats, 0, sizeof (r_froxel_ppd_stats));
	if (!r_froxel.valid)
		return;

	lights = R_PPdlights_GetFrameLights (&light_count);
	r_froxel_ppd_stats.source_count = q_max (0, light_count);
	if (!lights || light_count <= 0)
		return;

	for (i = 0; i < light_count; ++i)
	{
		const rl_light_t *src = &lights[i];
		uint32_t fog_type;
		float fog_radius;
		float fog_intensity = 1.f;
		int before_count;
		vec3_t scaled_color;
		R_PPdlights_RecordConsumerConsidered (RL_CONSUMER_FOG, src->source_id);

		if ((src->flags & RL_LIGHT_VOLUMETRIC_CONTRIB) == 0u)
		{
			r_froxel_ppd_stats.rejected_nonvolumetric++;
			R_PPdlights_RecordConsumerReject (RL_CONSUMER_FOG, src->source_id, RL_REJECT_NON_CONTRIB);
			continue;
		}
		r_froxel_ppd_stats.fog_eligible_count++;
		if (r_froxel_ppd_stats.injected_count >= fog_budget)
		{
			r_froxel_ppd_stats.rejected_local_budget++;
			R_PPdlights_RecordConsumerReject (RL_CONSUMER_FOG, src->source_id, RL_REJECT_LOCAL_BUDGET);
			continue;
		}
		before_count = r_froxel.light_count;
		fog_type = R_Froxel_DlightTypeFromRealtimeLight (src);
		fog_radius = R_Froxel_DlightRadiusForFog (src->radius, fog_type);
		VectorScale (src->color, q_max (0.f, src->intensity), scaled_color);
		if (R_Froxel_IsViewMuzzlePPDLight (src, fog_type))
			R_Froxel_ApplyViewMuzzleFogClamp (src->origin, &fog_radius, scaled_color, &fog_intensity);
		R_Froxel_AddLight (src->origin, fog_radius, scaled_color, fog_intensity, fog_type);
		if (r_froxel.light_count > before_count)
		{
			float energy;
			r_froxel_ppd_stats.injected_count++;
			r_froxel_ppd_stats.injected_radiance +=
				scaled_color[0] * 0.2126f + scaled_color[1] * 0.7152f + scaled_color[2] * 0.0722f;
			energy = scaled_color[0] * 0.2126f + scaled_color[1] * 0.7152f + scaled_color[2] * 0.0722f;
			R_PPdlights_RecordConsumerAccept (RL_CONSUMER_FOG, src->source_id, energy);
		}
		else
		{
			r_froxel_ppd_stats.rejected_budget++;
			R_PPdlights_RecordConsumerReject (RL_CONSUMER_FOG, src->source_id, RL_REJECT_HW_BUDGET);
		}
	}
	R_Froxel_InjectPPDGIHelper (lights, light_count);

	if ((r_ppdlights_fog_debug.value > 0.f || r_ppdlights_gi_debug.value > 0.f) && (r_framecount % 60) == 0)
	{
		rl_consumer_stats_t consumer_stats;
		Con_DPrintf ("r_ppdlights_fog: src=%d eligible=%d injected=%d reject(nonvol=%d distance=%d local_budget=%d hw_budget=%d) radiance=%.3f gi(candidates=%d injected=%d distance=%d budget=%d radiance=%.3f) caps(fog=%d gi=%d)\n",
			r_froxel_ppd_stats.source_count,
			r_froxel_ppd_stats.fog_eligible_count,
			r_froxel_ppd_stats.injected_count,
			r_froxel_ppd_stats.rejected_nonvolumetric,
			r_froxel_ppd_stats.rejected_distance,
			r_froxel_ppd_stats.rejected_local_budget,
			r_froxel_ppd_stats.rejected_budget,
			r_froxel_ppd_stats.injected_radiance,
			r_froxel_ppd_stats.gi_candidates,
			r_froxel_ppd_stats.gi_injected_count,
			r_froxel_ppd_stats.gi_rejected_distance,
			r_froxel_ppd_stats.gi_rejected_budget,
			r_froxel_ppd_stats.gi_radiance,
			fog_budget,
			CLAMP (0, (int)Q_rint (r_ppdlights_gi_budget.value), MAX_FROXEL_GPU_LIGHTS));
		if (R_PPdlights_GetConsumerStats (RL_CONSUMER_FOG, &consumer_stats))
		{
			Con_DPrintf ("r_ppdlights_fog_consumer: considered=%d accepted=%d energy=%.3f reject(non_contrib=%d distance=%d local_budget=%d hw_budget=%d)\n",
				consumer_stats.considered,
				consumer_stats.accepted,
				consumer_stats.accepted_energy,
				consumer_stats.rejected[RL_REJECT_NON_CONTRIB],
				consumer_stats.rejected[RL_REJECT_DISTANCE],
				consumer_stats.rejected[RL_REJECT_LOCAL_BUDGET],
				consumer_stats.rejected[RL_REJECT_HW_BUDGET]);
		}
	}
}

static void R_Froxel_InsertLavaCandidate (froxel_lava_candidate_t *candidates, int *candidate_count, int max_candidates,
	const vec3_t center, float extent_radius, float score)
{
	int i;
	int replace_idx = -1;
	float min_score;

	if (!candidates || !candidate_count || max_candidates <= 0)
		return;
	if (score <= 0.f)
		return;

	if (*candidate_count < max_candidates)
	{
		replace_idx = (*candidate_count)++;
	}
	else
	{
		min_score = candidates[0].score;
		replace_idx = 0;
		for (i = 1; i < max_candidates; ++i)
		{
			if (candidates[i].score < min_score)
			{
				min_score = candidates[i].score;
				replace_idx = i;
			}
		}
		if (score <= min_score)
			return;
	}

	VectorCopy (center, candidates[replace_idx].center);
	candidates[replace_idx].extent_radius = extent_radius;
	candidates[replace_idx].score = score;
}

static void R_Froxel_InjectLavaSurfaceLights (void)
{
	froxel_lava_candidate_t candidates[MAX_FROXEL_LAVA_LIGHTS];
	qmodel_t *world = cl.worldmodel;
	float lava_emissive_cvar = r_fogvol_lava_emissive.value;
	float lava_emissive = (lava_emissive_cvar > 0.f) ? lava_emissive_cvar : 2.0f;
	float max_dist = q_max (384.f, q_min (r_froxel.far_clip * 0.85f, 3072.f));
	int max_lava_lights = q_min (MAX_FROXEL_LAVA_LIGHTS, q_max (0, MAX_FROXEL_GPU_LIGHTS - r_froxel.light_count));
	int candidate_count = 0;
	int liquid_surface_count = 0;
	int lava_surface_count = 0;
	int probe_injected = 0;
	int i;

	if (!r_froxel.valid)
		return;
	if (!world || !world->surfaces || world->numsurfaces <= 0)
		return;
	if (lava_emissive_cvar < 0.f)
	{
		if (R_Froxel_ShouldPrintStats () || r_fogvol_debug.value >= 8.f)
			Con_DPrintf ("fogvol_lava: disabled (r_fogvol_lava_emissive=%.2f)\n", lava_emissive_cvar);
		return;
	}
	if (max_lava_lights <= 0)
		return;

	memset (candidates, 0, sizeof (candidates));

	for (i = 0; i < world->numsurfaces; ++i)
	{
		const msurface_t *surf = &world->surfaces[i];
		vec3_t center;
		vec3_t half_extent;
		vec3_t delta;
		float extent_radius;
		float dist2;
		float dist;
		float score;

		/* Restrict expensive checks unless surface already looks lava-like. */
		if ((surf->flags & (SURF_DRAWTURB | SURF_DRAWLAVA)) == 0
			&& !R_Froxel_SurfaceTextureIsLava (world, surf))
			continue;
		liquid_surface_count++;

		for (int a = 0; a < 3; ++a)
		{
			center[a] = 0.5f * (surf->mins[a] + surf->maxs[a]);
			half_extent[a] = 0.5f * q_max (0.f, surf->maxs[a] - surf->mins[a]);
		}

		extent_radius = VectorLength (half_extent);
		if (extent_radius <= 1.f)
			continue;
		if (!R_Froxel_SurfaceIsLava (world, surf, center))
			continue;
		lava_surface_count++;

		VectorSubtract (center, r_refdef.vieworg, delta);
		dist2 = DotProduct (delta, delta);
		dist = sqrtf (q_max (dist2, 0.f));
		if (dist > max_dist + extent_radius)
			continue;

		/* Prefer broader/closer lava patches for stable, low-count proxies. */
		score = ((extent_radius + 32.f) * (extent_radius + 32.f)) / (dist2 + 4096.f);
		R_Froxel_InsertLavaCandidate (candidates, &candidate_count, max_lava_lights, center, extent_radius, score);
	}

	for (i = 0; i < candidate_count && r_froxel.light_count < MAX_FROXEL_GPU_LIGHTS; ++i)
	{
		vec3_t light_color;
		float light_radius;
		float light_intensity;

		/* Keep lava contribution clearly visible in fog: warm spectrum + stronger baseline energy. */
		VectorSet (light_color, 1.80f, 0.45f, 0.12f);
		light_radius = CLAMP (160.f, candidates[i].extent_radius * 4.0f, q_max (224.f, r_froxel.far_clip * 0.65f));
		light_intensity = lava_emissive * CLAMP (1.8f + candidates[i].extent_radius / 64.f, 1.8f, 6.0f);

		R_Froxel_AddLight (candidates[i].center, light_radius, light_color, light_intensity, (uint32_t)DLIGHT_LAVA);
	}
	if (candidate_count <= 0)
		probe_injected = R_Froxel_InjectLavaProbeFallbackLights (world, lava_emissive);

	if (R_Froxel_ShouldPrintStats () || r_fogvol_debug.value >= 8.f)
	{
		Con_DPrintf ("fogvol_lava: injected=%d probe=%d lava=%d liquid=%d emissive=%.2f(cvar=%.2f) max_dist=%.0f\n",
			candidate_count, probe_injected, lava_surface_count, liquid_surface_count, lava_emissive, lava_emissive_cvar, max_dist);
	}
}

static void R_Froxel_InjectSun (void)
{
	const sun_t *sun;
	vec3_t inject_dir;
	vec3_t inject_origin;
	float radius;
	float intensity;

	if (!r_froxel.valid)
		return;
	if (R_Froxel_DebugEnabled ())
	{
		R_Froxel_DebugRefreshState ();
		if (!r_froxel_debug.active)
			return;
		radius = q_max (384.f, r_froxel.far_clip * 0.55f);
		VectorMA (r_refdef.vieworg, r_froxel.far_clip * 0.62f, r_froxel_debug.sun_dir, inject_origin);
		R_Froxel_AddLight (inject_origin, radius, r_froxel_debug.sun_color,
			q_max (0.1f, r_froxel_debug.sun_intensity * q_max (0.f, r_froxel_debug.sun_scale)), (uint32_t)DLIGHT_DEFAULT);
		return;
	}
	if (r_fogvol_froxel_sun.value <= 0.f)
		return;
	if (!R_WorldHasSun ())
		return;

	sun = R_GetSun ();
	if (!sun)
		return;
	intensity = q_max (0.f, sun->intensity) * q_max (0.f, r_fogvol_froxel_sun.value);
	if (intensity <= 0.f)
		return;

	VectorScale (sun->dir, -1.f, inject_dir);
	if (VectorNormalize (inject_dir) <= 0.f)
		return;

	radius = q_max (384.f, r_froxel.far_clip * 0.40f);
	VectorMA (r_refdef.vieworg, r_froxel.far_clip * 0.55f, inject_dir, inject_origin);
	R_Froxel_AddLight (inject_origin, radius, sun->color, intensity, (uint32_t)DLIGHT_DEFAULT);
}

void R_Froxel_ResetResources (void)
{
	if (r_froxel.light_tex)
		glDeleteTextures (1, &r_froxel.light_tex);
	if (r_froxel.history_tex)
		glDeleteTextures (1, &r_froxel.history_tex);
	if (r_froxel.light_ssbo)
		GL_DeleteBuffersFunc (1, &r_froxel.light_ssbo);
	memset (&r_froxel, 0, sizeof (r_froxel));
	r_froxel.prev_mode = -1;
	memset (&r_froxel_debug, 0, sizeof (r_froxel_debug));
}

void R_Froxel_BeginFrame (float near_clip, float far_clip)
{
	int nx, ny, nz;
	int debug_mode = R_Froxel_DebugMode ();
	int mode = CLAMP (0, (int)Q_rint (r_fogvol.value), 2);

	r_froxel.valid = false;
	if (mode <= 0)
		return;

	{
		const int scene_w = q_max (1, R_GetSceneRenderWidth ());
		const int scene_h = q_max (1, R_GetSceneRenderHeight ());
		nx = CLAMP (16, (scene_w + 15) / 16, 192);
		ny = CLAMP (12, (scene_h + 15) / 16, 128);
	}
	nz = 32;

	r_froxel.near_clip = q_max (near_clip, 1.f);
	r_froxel.far_clip = q_max (far_clip, r_froxel.near_clip + 1.f);
	r_froxel.tan_half_fov_x = tanf (DEG2RAD (r_refdef.fov_x) * 0.5f);
	r_froxel.tan_half_fov_y = tanf (DEG2RAD (r_refdef.fov_y) * 0.5f);
	r_froxel.log_far_near = logf (r_froxel.far_clip / r_froxel.near_clip);

	if (!R_Froxel_EnsureResources (nx, ny, nz))
		return;

	if (r_froxel.prev_mode != mode)
		r_froxel.prev_valid = false;
	r_froxel.prev_mode = mode;
	r_froxel.light_count = 0;
	memset (&r_froxel_ppd_stats, 0, sizeof (r_froxel_ppd_stats));
	r_froxel.valid = true;
	/* Debug mode keeps froxel structure crisp; normal mode stays filtered. */
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_3D, r_froxel.light_tex);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, (debug_mode >= 2) ? GL_NEAREST : GL_LINEAR);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, (debug_mode >= 2) ? GL_NEAREST : GL_LINEAR);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_3D, r_froxel.history_tex);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, (debug_mode >= 2) ? GL_NEAREST : GL_LINEAR);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, (debug_mode >= 2) ? GL_NEAREST : GL_LINEAR);
	R_Froxel_DebugRefreshState ();
}

void R_Froxel_InjectDlights (void)
{
	int active_count = 0;
	const dlight_t *const *active = NULL;
	const int dlight_source_mode = CLAMP (0, (int)Q_rint (r_fogvol_dlight_source.value), 2);
	const qboolean pp_fog_enabled = (r_ppdlights.value > 0.f && r_ppdlights_fog.value > 0.f);
	const qboolean pp_collect_enabled = (r_ppdlights.value > 0.f);
	const qboolean allow_legacy_fallback = (r_fogvol_dlight_legacy_fallback.value > 0.f);
	const qboolean allow_lava_emissive = (r_fogvol_lava_emissive.value >= 0.f);
	const qboolean fog_light_enabled = (r_fogvol_light.value > 0.f);
	qboolean use_pp_path = false;
	float intensity_scale;

	if (!r_froxel.valid)
		return;
	/*
	 * Keep lava emissive parity between legacy and pp-fog paths.
	 * Previously pp-fog returned before this injection, causing visible
	 * differences whenever lava was in view.
	 */
	if (allow_lava_emissive)
		R_Froxel_InjectLavaSurfaceLights ();

	/* Fog dlight source selection:
	 * 0 = legacy-compatible behavior (pp only when r_ppdlights_fog=1)
	 * 1 = prefer shared pp frame lights (default), fallback to legacy when pp contributes none
	 * 2 = force legacy pool path */
	if (dlight_source_mode == 1)
		use_pp_path = pp_collect_enabled;
	else if (dlight_source_mode == 2)
		use_pp_path = false;
	else
		use_pp_path = pp_fog_enabled;

	if (use_pp_path)
	{
		const int light_count_before_pp = r_froxel.light_count;
		R_Froxel_InjectPPDLights ();
		if (!(dlight_source_mode == 1
			&& allow_legacy_fallback
			&& r_froxel.light_count == light_count_before_pp))
		{
			return;
		}
	}
	if (!fog_light_enabled && !allow_lava_emissive)
		return;

	/* Debug lights are additive and do not replace real dlights. */
	if (fog_light_enabled && R_Froxel_DebugEnabled ())
	{
		R_Froxel_DebugRefreshState ();
		for (int i = 0; i < r_froxel_debug.random_light_count && r_froxel.light_count < MAX_FROXEL_GPU_LIGHTS; ++i)
		{
			const froxel_gpu_light_t *light = &r_froxel_debug.random_lights[i];
			vec3_t color;
			float radius_scale = CLAMP (0.8f, 0.7f + r_froxel_debug.dlight_scale * 0.2f, 2.2f);
			float intensity = light->color_intensity[3] * q_max (0.f, r_froxel_debug.dlight_scale);

			VectorSet (color, light->color_intensity[0], light->color_intensity[1], light->color_intensity[2]);
			R_Froxel_AddLight (light->pos_rad, light->pos_rad[3] * radius_scale, color, intensity, light->type);
		}
		/* No early return here: real dlights are injected below as well. */
	}

	if (!fog_light_enabled)
		return;
	/* r_fogvol_dlightscale is applied in fogvol.frag via FogLightSourceScales.x.
	 * Do not bake it into froxel injection as well, otherwise dlights are scaled
	 * twice (effectively squared). */
	intensity_scale = 1.f;
	if (q_max (0.f, r_fogvol_dlightscale.value) <= 0.f)
		return;

	active = DLightPool_GetActiveList (&active_count);
	if (!active || active_count <= 0)
		return;

	for (int i = 0; i < active_count && r_froxel.light_count < MAX_FROXEL_GPU_LIGHTS; ++i)
	{
		const dlight_t *dl = active[i];
		float eval_radius = 0.f;
		float fog_radius = 0.f;
		float fog_intensity = intensity_scale;
		vec3_t eval_color;

		if (!dl)
			continue;
		if (!CL_DlightIsActive (dl))
			continue;
		if (dl->kind == DL_PERSISTENT && r_dlight_entities.value <= 0.f)
			continue;
		if (!CL_DlightTransientIsLiveAtTime (dl, cl.time, NULL))
			continue;

		R_EvaluateDLightForRender (dl, &eval_radius, eval_color);
		if (eval_radius <= 1.f)
			continue;
		if (eval_color[0] <= 0.f && eval_color[1] <= 0.f && eval_color[2] <= 0.f)
			continue;
		fog_radius = R_Froxel_DlightRadiusForFog (eval_radius, (uint32_t)dl->type);
		if (R_Froxel_IsViewMuzzleDlight (dl))
			R_Froxel_ApplyViewMuzzleFogClamp (dl->origin, &fog_radius, eval_color, &fog_intensity);

		R_Froxel_AddLight (dl->origin, fog_radius, eval_color, fog_intensity, (uint32_t)dl->type);
	}
}

void R_Froxel_EndFrame (void)
{
	float inv_view[16];
	vec3_t view_delta;
	int groups_x, groups_y, groups_z;
	GLuint tmp_tex;
	int upload_count;
	GLsizeiptr upload_bytes;
	float temporal_alpha = 0.f;
	float temporal_reject_threshold = 1.f;
	float temporal_camera_delta = 0.f;
	float temporal_prev_valid = 0.f;
	const qboolean debug_mode = R_Froxel_DebugEnabled ();

	if (!r_froxel.valid || !r_froxel.light_tex || !r_froxel.history_tex || !r_froxel.light_ssbo || !glprogs.fogvol_froxel_inject)
		return;
	if (!Mat4_Inverse (r_matview, inv_view))
		return;

	R_Froxel_InjectSun ();
	if (r_froxel.light_count <= 0 && !r_froxel.prev_valid)
	{
		VectorCopy (r_refdef.vieworg, r_froxel.prev_vieworg);
		return;
	}

	groups_x = (r_froxel.dims[0] + 3) / 4;
	groups_y = (r_froxel.dims[1] + 3) / 4;
	groups_z = (r_froxel.dims[2] + 3) / 4;

	/* Ping-pong before dispatch so output becomes the texture sampled this frame. */
	tmp_tex = r_froxel.light_tex;
	r_froxel.light_tex = r_froxel.history_tex;
	r_froxel.history_tex = tmp_tex;

	if (debug_mode)
	{
		VectorSubtract (r_refdef.vieworg, r_froxel.prev_vieworg, view_delta);
		temporal_alpha = 0.95f;
		temporal_reject_threshold = 192.f;
		temporal_camera_delta = VectorLength (view_delta);
		temporal_prev_valid = r_froxel.prev_valid ? 1.f : 0.f;
	}

	upload_count = q_max (r_froxel.light_count, 1);
	upload_bytes = (GLsizeiptr)(sizeof (froxel_gpu_light_t) * upload_count);
	GL_UseProgram (glprogs.fogvol_froxel_inject);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, r_froxel.light_ssbo, 0, upload_bytes);
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, upload_bytes, r_froxel_gpu_lights, GL_STREAM_DRAW);
	/* image3D uses binding=1 and history sampler uses binding=2. */
	GL_BindImageTextureFunc (1, r_froxel.light_tex, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
	GL_BindNative (GL_TEXTURE2, GL_TEXTURE_3D, r_froxel.history_tex);
	GL_Uniform4fFunc (0, (float)r_froxel.dims[0], (float)r_froxel.dims[1], (float)r_froxel.dims[2], (float)r_froxel.light_count);
	GL_Uniform4fFunc (1, r_froxel.near_clip, r_froxel.far_clip, r_froxel.tan_half_fov_x, r_froxel.tan_half_fov_y);
	GL_Uniform4fFunc (2, r_froxel.log_far_near, 0.f, 0.f, 0.f);
	GL_UniformMatrix4fvFunc (3, 1, GL_FALSE, inv_view);
	GL_Uniform4fFunc (7, temporal_alpha, temporal_reject_threshold, temporal_camera_delta, temporal_prev_valid);
	GL_DispatchComputeFunc (groups_x, groups_y, groups_z);
	GL_MemoryBarrierFunc (GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

	VectorCopy (r_refdef.vieworg, r_froxel.prev_vieworg);
	r_froxel.prev_valid = true;
}

qboolean R_Froxel_GetDebugScales (float *out_dlight_scale, float *out_sun_scale, float *out_emissive_scale)
{
	if (!R_Froxel_DebugEnabled () || !r_froxel_debug.active)
		return false;

	if (out_dlight_scale)
		*out_dlight_scale = q_max (0.f, r_froxel_debug.dlight_scale);
	if (out_sun_scale)
		*out_sun_scale = q_max (0.f, r_froxel_debug.sun_scale);
	if (out_emissive_scale)
		*out_emissive_scale = q_max (0.f, r_froxel_debug.emissive_scale);

	return true;
}

qboolean R_Froxel_GetShaderState (GLuint *out_light_tex, int *out_light_count, float params0[4], float params1[4])
{
	if (out_light_tex)
		*out_light_tex = r_froxel.valid ? r_froxel.light_tex : 0;
	if (out_light_count)
		*out_light_count = r_froxel.valid ? r_froxel.light_count : 0;
	if (params0)
	{
		params0[0] = r_froxel.near_clip;
		params0[1] = r_froxel.far_clip;
		params0[2] = r_froxel.tan_half_fov_x;
		params0[3] = r_froxel.tan_half_fov_y;
	}
	if (params1)
	{
		params1[0] = r_froxel.log_far_near;
		params1[1] = (float)q_max (1, r_froxel.dims[0]);
		params1[2] = (float)q_max (1, r_froxel.dims[1]);
		params1[3] = (float)q_max (1, r_froxel.dims[2]);
	}
	return (r_froxel.valid && r_froxel.light_tex && r_froxel.light_count > 0);
}
