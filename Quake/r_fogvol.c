#include "quakedef.h"
#include "draw.h"
#include "r_fogvol.h"
#include "r_fogvol_internal.h"
#include "r_dlight_pool.h"
#include "r_realtimelight.h"
#include "r_sanitize.h"
#include "r_skyvis.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern cvar_t gl_farclip;

typedef struct fog_volume_gpu_s
{
	float mins[4];
	float maxs[4];
	float sphere[4];
	float color_density[4];
	float noise_params[4];
	float velocity_windspeed[4];
	float wind_turbulence[4];
	float misc[4];
	float extra[4];
	float params2[4];
} fog_volume_gpu_t;

typedef struct fog_light_gpu_s
{
	float pos_rad[4];
	float col_int[4];
} fog_light_gpu_t;

typedef struct fog_light_list_gpu_s
{
	int offset_count[4];
} fog_light_list_gpu_t;

typedef struct fog_light_lists_gpu_s
{
	fog_light_list_gpu_t volumes[MAX_FOGVOLUMES];
	fog_light_gpu_t lights[MAX_FOGVOLUMES * MAX_FOGLIGHTS];
} fog_light_lists_gpu_t;

COMPILE_TIME_ASSERT (fog_volume_gpu_std430_size, sizeof (fog_volume_gpu_t) == 160);
COMPILE_TIME_ASSERT (fog_light_gpu_std430_size, sizeof (fog_light_gpu_t) == 32);
COMPILE_TIME_ASSERT (fog_light_list_gpu_std430_size, sizeof (fog_light_list_gpu_t) == 16);

/* Public CVars (simplified 0/1/2 dispatch + shared controls). */
cvar_t r_fogvol = { "r_fogvol", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_steps = { "r_fogvol_steps", "32", CVAR_ARCHIVE };
cvar_t r_fogvol_halfres = { "r_fogvol_halfres", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_noise = { "r_fogvol_noise", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_noisemode = { "r_fogvol_noisemode", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_noise_scale = { "r_fogvol_noise_scale", "0.05", CVAR_ARCHIVE };
cvar_t r_fogvol_noise_amount = { "r_fogvol_noise_amount", "0.5", CVAR_ARCHIVE };
cvar_t r_fogvol_noise_bias = { "r_fogvol_noise_bias", "0.0", CVAR_ARCHIVE };
cvar_t r_fogvol_jitter = { "r_fogvol_jitter", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_blendmode = { "r_fogvol_blendmode", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_emissive = { "r_fogvol_emissive", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_lava_emissive = { "r_fogvol_lava_emissive", "2.0", CVAR_ARCHIVE };
cvar_t r_fogvol_light = { "r_fogvol_light", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_dlightscale = { "r_fogvol_dlightscale", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_dlight_source = { "r_fogvol_dlight_source", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_dlight_legacy_fallback = { "r_fogvol_dlight_legacy_fallback", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_froxel_sun = { "r_fogvol_froxel_sun", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_light_dlight_boost = { "r_fogvol_light_dlight_boost", "1.8", CVAR_ARCHIVE };
cvar_t r_fogvol_light_sun_boost = { "r_fogvol_light_sun_boost", "1.35", CVAR_ARCHIVE };
cvar_t r_fogvol_light_emissive_boost = { "r_fogvol_light_emissive_boost", "1.25", CVAR_ARCHIVE };
cvar_t r_fogvol_light_ambient = { "r_fogvol_light_ambient", "0.055", CVAR_ARCHIVE };
cvar_t r_fogvol_light_contrast = { "r_fogvol_light_contrast", "1.65", CVAR_ARCHIVE };
cvar_t r_fogvol_shadow_contrast = { "r_fogvol_shadow_contrast", "1.45", CVAR_ARCHIVE };
cvar_t r_fogvol_light_extinction_relief = { "r_fogvol_light_extinction_relief", "0.55", CVAR_ARCHIVE };
cvar_t r_fogvol_shadow = { "r_fogvol_shadow", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_shadow_strength = { "r_fogvol_shadow_strength", "0.8", CVAR_ARCHIVE };
cvar_t r_fogvol_density_scale = { "r_fogvol_density_scale", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_global = { "r_fogvol_global", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_global_color = { "r_fogvol_global_color", "0.3 0.3 0.3", CVAR_ARCHIVE };
cvar_t r_fogvol_global_density_scale = { "r_fogvol_global_density_scale", "0.06", CVAR_ARCHIVE };
cvar_t r_fogvol_global_falloff = { "r_fogvol_global_falloff", "64", CVAR_ARCHIVE };
cvar_t r_fogvol_global_noise_scale = { "r_fogvol_global_noise_scale", "0.014", CVAR_ARCHIVE };
cvar_t r_fogvol_global_noise_amount = { "r_fogvol_global_noise_amount", "0.78", CVAR_ARCHIVE };
cvar_t r_fogvol_global_noise_bias = { "r_fogvol_global_noise_bias", "0.0", CVAR_ARCHIVE };
cvar_t r_fogvol_global_velocity_x = { "r_fogvol_global_velocity_x", "1.25", CVAR_ARCHIVE };
cvar_t r_fogvol_global_velocity_y = { "r_fogvol_global_velocity_y", "0.65", CVAR_ARCHIVE };
cvar_t r_fogvol_global_velocity_z = { "r_fogvol_global_velocity_z", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_global_height = { "r_fogvol_global_height", "56", CVAR_ARCHIVE };
cvar_t r_fogvol_global_height_scale = { "r_fogvol_global_height_scale", "0.0020", CVAR_ARCHIVE };
cvar_t r_fogvol_height_mist_strength = { "r_fogvol_height_mist_strength", "0.3", CVAR_ARCHIVE };
cvar_t r_fogvol_global_priority = { "r_fogvol_global_priority", "-1", CVAR_ARCHIVE };
cvar_t r_fogvol_debug = { "r_fogvol_debug", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_debug_froxel_random = { "r_fogvol_debug_froxel_random", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_stats = { "r_fogvol_stats", "0", CVAR_NONE };
cvar_t r_fogvol_debug_dump = { "r_fogvol_debug_dump", "0", CVAR_NONE };

typedef struct fogvol_cvar_reg_s
{
	cvar_t *var;
} fogvol_cvar_reg_t;

static const fogvol_cvar_reg_t fogvol_cvar_table[] = {
	{&r_fogvol},
	{&r_fogvol_steps},
	{&r_fogvol_halfres},
	{&r_fogvol_noise},
	{&r_fogvol_noisemode},
	{&r_fogvol_noise_scale},
	{&r_fogvol_noise_amount},
	{&r_fogvol_noise_bias},
	{&r_fogvol_jitter},
	{&r_fogvol_blendmode},
	{&r_fogvol_emissive},
	{&r_fogvol_lava_emissive},
	{&r_fogvol_light},
	{&r_fogvol_dlightscale},
	{&r_fogvol_dlight_source},
	{&r_fogvol_dlight_legacy_fallback},
	{&r_fogvol_froxel_sun},
	{&r_fogvol_light_dlight_boost},
	{&r_fogvol_light_sun_boost},
	{&r_fogvol_light_emissive_boost},
	{&r_fogvol_light_ambient},
	{&r_fogvol_light_contrast},
	{&r_fogvol_shadow_contrast},
	{&r_fogvol_light_extinction_relief},
	{&r_fogvol_shadow},
	{&r_fogvol_shadow_strength},
	{&r_fogvol_density_scale},
	{&r_fogvol_global},
	{&r_fogvol_global_color},
	{&r_fogvol_global_density_scale},
	{&r_fogvol_global_falloff},
	{&r_fogvol_global_noise_scale},
	{&r_fogvol_global_noise_amount},
	{&r_fogvol_global_noise_bias},
	{&r_fogvol_global_velocity_x},
	{&r_fogvol_global_velocity_y},
	{&r_fogvol_global_velocity_z},
	{&r_fogvol_global_height},
	{&r_fogvol_global_height_scale},
	{&r_fogvol_height_mist_strength},
	{&r_fogvol_global_priority},
	{&r_fogvol_debug},
	{&r_fogvol_debug_froxel_random},
	{&r_fogvol_stats},
	{&r_fogvol_debug_dump},
};

enum
{
	FOGVOL_U_STEPS = 0,
	FOGVOL_U_NOISE_ENABLED = 1,
	FOGVOL_U_DEBUG_MODE = 2,
	FOGVOL_U_VOLUME_INDEX = 3,
	FOGVOL_U_INV_VIEWPROJ = 4,
	FOGVOL_U_NOISE_MODE = 5,
	FOGVOL_U_PHYS_BLEND = 6,
	FOGVOL_U_JITTER_ENABLED = 7,
	FOGVOL_U_CAMERA_POS_WS = 8,
	FOGVOL_U_VIEWPORT_PARAMS = 9,
	FOGVOL_U_DEPTH_SCALE = 10,
	FOGVOL_U_VIEW_PARAMS = 11,
	FOGVOL_U_DEPTH_PARAMS = 12,
	FOGVOL_U_DENSITY_PARAMS = 13,
	FOGVOL_U_EMISSIVE_ENABLED = 14,
	FOGVOL_U_BLEND_MODE_DEFAULT = 15,
	FOGVOL_U_LIGHT_ENABLED = 16,
	FOGVOL_U_SHADOW_ENABLED = 17,
	FOGVOL_U_SHADOW_SAMPLES = 18,
	FOGVOL_U_SHADOW_STRENGTH = 19,
	FOGVOL_U_SHADOW_JITTER = 20,
	FOGVOL_U_SHADOW_DIR = 21,
	FOGVOL_U_LIGHTGRID_ENABLED = 22,
	FOGVOL_U_FRAME_INDEX = 23,
	FOGVOL_U_NOISE_SUBSAMPLE = 24,
	FOGVOL_U_NOISE_LOD_SWITCH = 25,
	FOGVOL_U_DOMAINWARP_DIST = 26,
	FOGVOL_U_NOISE_DETAIL_STRENGTH = 27,
	FOGVOL_U_DLIGHT_SCALE = 28,
	FOGVOL_U_LIGHT_SCISSOR = 29,
	FOGVOL_U_LIGHT_SOURCE_SCALES = 39,
	FOGVOL_U_LIGHTING_MODE = 40,
	FOGVOL_U_GODRAY_COUPLING = 41,
	FOGVOL_U_LOCAL_OCCLUSION_MODE = 42,
	FOGVOL_U_CHECKERBOARD = 47,
	FOGVOL_U_FROXEL_ENABLED = 34,
	FOGVOL_U_FROXEL_PARAMS0 = 35,
	FOGVOL_U_FROXEL_PARAMS1 = 36,
	FOGVOL_U_FROXEL_DEBUG = 37,
	FOGVOL_U_FROXEL_PARITY_MODE = 38,
	FOGVOL_U_FROXEL_TEMPORAL_PARAMS = 43,
	FOGVOL_U_CLUSTER_PARAMS = 49,
	FOGVOL_U_SUN_SHADOW_VIEWPROJ = 50,
	FOGVOL_U_SUN_SHADOW_PARAMS = 54,
	FOGVOL_U_SUN_SHADOW_SPLITS = 55,
	FOGVOL_U_SUN_SHADOW_CASCADE_COUNT = 56,
	FOGVOL_U_COLOR_SCALE = 57,
	FOGVOL_U_DEPTH_BIAS = 58,
	FOGVOL_U_COLOR_BIAS = 59,
	FOGVOL_U_DEPTH_UV_SCALE = 60
};

static fog_volume_t r_fogvolumes[MAX_FOGVOLUMES];
static int r_fogvolume_count = 0;
static fog_volume_t r_fogvolume_entities[MAX_FOGVOLUMES];
static int r_fogvolume_entity_count = 0;
static fog_volume_t r_fogvol_global_volume;
static qboolean r_fogvol_global_active = false;
static qboolean r_fogvol_composite_valid = false;
static GLuint r_fogvol_composite_tex = 0;
static fog_light_lists_gpu_t r_fogvol_empty_lights;

static GLuint r_fogvol_godray_shafts_tex = 0;
static GLuint r_fogvol_godray_mask_tex = 0;
static GLuint r_fogvol_godray_source_tex = 0;
static qboolean r_fogvol_godray_ready = false;

#define FOGVOL_COORD_LIMIT 1048576.f
#define FOGVOL_RADIUS_MAX 262144.f
#define FOGVOL_WIND_LIMIT 4096.f

static qboolean FogVol_ShouldPrintStats (void)
{
	if (r_fogvol_stats.value <= 0.f)
		return false;
	/* Keep stats readable: one sample per second at 60fps. */
	return (r_framecount % 60) == 0;
}

static qboolean FogVol_ShouldPrintDebugDump (void)
{
	static double next_dump_time = 0.0;
	double now;

	if (r_fogvol_debug_dump.value <= 0.f)
		return false;

	now = Sys_DoubleTime ();
	if (now < next_dump_time)
		return false;
	next_dump_time = now + 1.0;
	return true;
}

static qboolean FogVol_DebugReadDepthAtPixel (GLuint fbo, int x, int y, float *out_depth)
{
	GLint prev_read_fbo = 0;
	GLint prev_draw_fbo = 0;
	float depth = 0.f;

	if (!out_depth || !GL_BindFramebufferFunc)
		return false;

	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
	GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, fbo);
	glReadPixels (x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
	GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);
	GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw_fbo);

	*out_depth = depth;
	return true;
}

static qboolean FogVol_DebugReconstructWorldPos (const float inv_viewproj[16], float view_u, float view_v, float depth, vec3_t out_world)
{
	float ndc_x = view_u * 2.f - 1.f;
	float ndc_y = view_v * 2.f - 1.f;
	float ndc_z = gl_clipcontrol_able ? depth : (depth * 2.f - 1.f);
	float clip[4] = { ndc_x, ndc_y, ndc_z, 1.f };
	float world[4];
	float inv_w;

	world[0] = inv_viewproj[0] * clip[0] + inv_viewproj[4] * clip[1] + inv_viewproj[8] * clip[2] + inv_viewproj[12] * clip[3];
	world[1] = inv_viewproj[1] * clip[0] + inv_viewproj[5] * clip[1] + inv_viewproj[9] * clip[2] + inv_viewproj[13] * clip[3];
	world[2] = inv_viewproj[2] * clip[0] + inv_viewproj[6] * clip[1] + inv_viewproj[10] * clip[2] + inv_viewproj[14] * clip[3];
	world[3] = inv_viewproj[3] * clip[0] + inv_viewproj[7] * clip[1] + inv_viewproj[11] * clip[2] + inv_viewproj[15] * clip[3];

	if (fabsf (world[3]) <= 1e-6f)
		return false;

	inv_w = 1.f / world[3];
	out_world[0] = world[0] * inv_w;
	out_world[1] = world[1] * inv_w;
	out_world[2] = world[2] * inv_w;
	return true;
}

static void FogVol_DumpDepthProbe (const float inv_viewproj[16], float view_x, float view_y, float view_w, float view_h)
{
	static const float probe_uv[5][2] = {
		{0.5f, 0.5f},
		{0.1f, 0.1f},
		{0.9f, 0.1f},
		{0.1f, 0.9f},
		{0.9f, 0.9f}
	};
	for (int i = 0; i < 5; ++i)
	{
		float u = probe_uv[i][0];
		float v = probe_uv[i][1];
		int px = (int)floorf (view_x + u * view_w);
		int py = (int)floorf (view_y + v * view_h);
		float depth = 0.f;
		vec3_t world;
		float dist = -1.f;
		qboolean ok_depth;
		qboolean ok_world = false;

		px = CLAMP (0, px, q_max (0, R_GetNativeRenderWidth () - 1));
		py = CLAMP (0, py, q_max (0, R_GetNativeRenderHeight () - 1));

		ok_depth = FogVol_DebugReadDepthAtPixel (framebufs.composite.fbo, px, py, &depth);
		if (ok_depth)
		{
			float view_u = (view_w > 0.f) ? ((float)px + 0.5f - view_x) / view_w : 0.f;
			float view_v = (view_h > 0.f) ? ((float)py + 0.5f - view_y) / view_h : 0.f;
			view_u = CLAMP (0.f, view_u, 1.f);
			view_v = CLAMP (0.f, view_v, 1.f);
			ok_world = FogVol_DebugReconstructWorldPos (inv_viewproj, view_u, view_v, depth, world);
			if (ok_world)
			{
				vec3_t delta;
				VectorSubtract (world, r_refdef.vieworg, delta);
				dist = sqrtf (DotProduct (delta, delta));
			}
		}

		Con_DPrintf ("fogvol_dump probe[%d] uv=(%.2f %.2f) px=(%d %d) depth=%.9f world_ok=%d dist=%.3f\n",
			i, u, v, px, py, depth, ok_world ? 1 : 0, dist);
	}
}

static qboolean FogVol_QueryTexture2DSize (GLuint tex, int *out_w, int *out_h)
{
	GLint prev_active_tex = 0;
	GLint prev_bound_tex = 0;
	GLint width = 0;
	GLint height = 0;

	if (!tex || !out_w || !out_h)
		return false;

	glGetIntegerv (GL_ACTIVE_TEXTURE, &prev_active_tex);
	GL_ActiveTextureFunc (GL_TEXTURE0);
	glGetIntegerv (GL_TEXTURE_BINDING_2D, &prev_bound_tex);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, tex);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, (GLuint)prev_bound_tex);
	GL_ActiveTextureFunc ((GLenum)prev_active_tex);

	if (width <= 0 || height <= 0)
		return false;

	*out_w = (int)width;
	*out_h = (int)height;
	return true;
}

static void FogVol_DumpAllCvars (void)
{
	Con_DPrintf ("fogvol_dump cvars_begin\n");
	for (size_t i = 0; i < countof (fogvol_cvar_table); ++i)
	{
		cvar_t *var = fogvol_cvar_table[i].var;
		if (!var)
			continue;
		Con_DPrintf ("fogvol_dump cvar %s=%s (%.6f)\n",
			var->name ? var->name : "<null>",
			var->string ? var->string : "<null>",
			var->value);
	}
	Con_DPrintf ("fogvol_dump cvars_end\n");
}

static int FogVol_NormalizeShape (int shape)
{
	return (shape == FOGVOL_SHAPE_SPHERE) ? FOGVOL_SHAPE_SPHERE : FOGVOL_SHAPE_BOX;
}

static void FogVol_ParseColor (const char *value, vec3_t color)
{
	float r = 1.f, g = 1.f, b = 1.f;
	if (value && sscanf (value, "%f %f %f", &r, &g, &b) == 3)
	{
		r = R_SanitizeFloatRange (r, 1.f, 0.f, 65535.f);
		g = R_SanitizeFloatRange (g, 1.f, 0.f, 65535.f);
		b = R_SanitizeFloatRange (b, 1.f, 0.f, 65535.f);
		if (r > 1.f || g > 1.f || b > 1.f)
		{
			r *= 1.f / 255.f;
			g *= 1.f / 255.f;
			b *= 1.f / 255.f;
		}
	}
	color[0] = R_SanitizeFloatRange (r, 1.f, 0.f, 8.f);
	color[1] = R_SanitizeFloatRange (g, 1.f, 0.f, 8.f);
	color[2] = R_SanitizeFloatRange (b, 1.f, 0.f, 8.f);
}

static void FogVol_ClampVolume (fog_volume_t *v)
{
	if (!v)
		return;
	v->shape = FogVol_NormalizeShape (v->shape);
	for (int a = 0; a < 3; ++a)
	{
		v->mins[a] = R_SanitizeFloatRange (v->mins[a], -1024.f, -FOGVOL_COORD_LIMIT, FOGVOL_COORD_LIMIT);
		v->maxs[a] = R_SanitizeFloatRange (v->maxs[a], 1024.f, -FOGVOL_COORD_LIMIT, FOGVOL_COORD_LIMIT);
		if (v->mins[a] > v->maxs[a])
		{
			float t = v->mins[a];
			v->mins[a] = v->maxs[a];
			v->maxs[a] = t;
		}
		v->sphereCenter[a] = R_SanitizeFloatRange (v->sphereCenter[a], 0.f, -FOGVOL_COORD_LIMIT, FOGVOL_COORD_LIMIT);
		v->velocity[a] = R_SanitizeFloatRange (v->velocity[a], 0.f, -FOGVOL_WIND_LIMIT, FOGVOL_WIND_LIMIT);
		v->windDir[a] = R_SanitizeFloatRange (v->windDir[a], 0.f, -1.f, 1.f);
		v->color[a] = R_SanitizeFloatRange (v->color[a], 0.f, 0.f, 8.f);
	}
	v->sphereRadius = R_SanitizeFloatRange (v->sphereRadius, 1024.f, 1.f, FOGVOL_RADIUS_MAX);
	v->density = R_SanitizeFloatRange (v->density, 0.f, 0.f, 8.f);
	v->falloff = R_SanitizeFloatRange (v->falloff, 0.f, 0.f, 4096.f);
	v->noiseScale = R_SanitizeFloatRange (v->noiseScale, 0.f, 0.f, 1.f);
	v->noiseAmount = R_SanitizeFloatRange (v->noiseAmount, 0.f, 0.f, 2.f);
	v->noiseBias = R_SanitizeFloatRange (v->noiseBias, 0.f, -2.f, 2.f);
	v->turbulence = R_SanitizeFloatRange (v->turbulence, 0.f, 0.f, 32.f);
	v->windSpeed = R_SanitizeFloatRange (v->windSpeed, 0.f, 0.f, FOGVOL_WIND_LIMIT);
	v->maxDistance = R_SanitizeFloatRange (v->maxDistance, 16384.f, 1.f, FOGVOL_COORD_LIMIT);
	v->emissiveStrength = R_SanitizeFloatRange (v->emissiveStrength, 0.f, 0.f, 32.f);
	v->height = R_SanitizeFloatRange (v->height, 0.f, -FOGVOL_COORD_LIMIT, FOGVOL_COORD_LIMIT);
	v->heightScale = R_SanitizeFloatRange (v->heightScale, 0.f, 0.f, 1.f);
	v->edgeSoftness = R_SanitizeFloatRange (v->edgeSoftness, 0.f, 0.f, 1.f);
	v->blendMode = (v->blendMode < 0) ? -1 : CLAMP (0, v->blendMode, 1);
	if (v->shape == FOGVOL_SHAPE_SPHERE)
		v->sphereRadius = q_max (1.f, v->sphereRadius);
	v->enabled = v->enabled ? 1 : 0;
}

static int FogVol_ComparePriority (const void *a, const void *b)
{
	const fog_volume_t *va = (const fog_volume_t *)a;
	const fog_volume_t *vb = (const fog_volume_t *)b;
	return (vb->priority - va->priority);
}

static float FogVol_DebugRand01 (void)
{
	return (float)(rand () & 0x7fff) * (1.f / 32767.f);
}

static float FogVol_DebugRandRange (float min_value, float max_value)
{
	return min_value + (max_value - min_value) * FogVol_DebugRand01 ();
}

static void FogVol_DebugGetMapBounds (vec3_t mins, vec3_t maxs)
{
	VectorSet (mins, -1024.f, -1024.f, -256.f);
	VectorSet (maxs, 1024.f, 1024.f, 512.f);

	if (!cl.worldmodel)
		return;

	VectorCopy (cl.worldmodel->mins, mins);
	VectorCopy (cl.worldmodel->maxs, maxs);
	for (int a = 0; a < 3; ++a)
	{
		if (maxs[a] - mins[a] < 128.f)
		{
			mins[a] -= 64.f;
			maxs[a] += 64.f;
		}
	}
}

static void FogVol_DebugBuildRandomVolume (fog_volume_t *v, const vec3_t map_mins, const vec3_t map_maxs)
{
	vec3_t center;
	vec3_t extents;

	memset (v, 0, sizeof (*v));
	for (int a = 0; a < 3; ++a)
	{
		center[a] = FogVol_DebugRandRange (map_mins[a], map_maxs[a]);
		extents[a] = FogVol_DebugRandRange (48.f, 320.f);
		v->mins[a] = center[a] - extents[a];
		v->maxs[a] = center[a] + extents[a];
		v->velocity[a] = FogVol_DebugRandRange (-32.f, 32.f);
		v->windDir[a] = FogVol_DebugRandRange (-1.f, 1.f);
		v->color[a] = FogVol_DebugRandRange (0.2f, 1.f);
	}

	v->shape = FOGVOL_SHAPE_BOX;
	v->sphereRadius = q_max (extents[0], q_max (extents[1], extents[2]));
	VectorCopy (center, v->sphereCenter);
	v->density = FogVol_DebugRandRange (0.008f, 0.095f);
	v->falloff = FogVol_DebugRandRange (24.f, 192.f);
	v->mode = 1;
	v->blendMode = (FogVol_DebugRand01 () > 0.5f) ? 1 : 0;
	v->emissiveStrength = FogVol_DebugRandRange (0.f, 1.75f);
	v->noiseScale = FogVol_DebugRandRange (0.01f, 0.16f);
	v->noiseAmount = FogVol_DebugRandRange (0.1f, 1.0f);
	v->noiseBias = FogVol_DebugRandRange (-0.5f, 0.5f);
	v->turbulence = FogVol_DebugRandRange (0.f, 3.f);
	v->windSpeed = FogVol_DebugRandRange (0.f, 48.f);
	v->maxDistance = FogVol_DebugRandRange (512.f, 8192.f);
	v->priority = 16 + (int)Q_rint (FogVol_DebugRandRange (0.f, 32.f));
	v->enabled = 1;
	v->height = FogVol_DebugRandRange (map_mins[2], map_maxs[2]);
	v->heightScale = FogVol_DebugRandRange (0.f, 0.02f);
	v->edgeSoftness = FogVol_DebugRandRange (0.05f, 0.8f);
}

static void FogVol_DebugBuildOriginVolume (fog_volume_t *v)
{
	memset (v, 0, sizeof (*v));
	VectorSet (v->mins, -96.f, -96.f, -64.f);
	VectorSet (v->maxs, 96.f, 96.f, 160.f);
	VectorSet (v->sphereCenter, 0.f, 0.f, 0.f);
	VectorSet (v->color, 0.35f, 0.75f, 1.f);
	VectorSet (v->velocity, 0.f, 0.f, 0.f);
	VectorSet (v->windDir, 0.f, 0.f, 1.f);
	v->shape = FOGVOL_SHAPE_BOX;
	v->sphereRadius = 128.f;
	v->density = 0.055f;
	v->falloff = 72.f;
	v->mode = 1;
	v->blendMode = 0;
	v->emissiveStrength = 0.25f;
	v->noiseScale = 0.06f;
	v->noiseAmount = 0.35f;
	v->noiseBias = 0.f;
	v->turbulence = 0.5f;
	v->windSpeed = 0.f;
	v->maxDistance = 4096.f;
	v->priority = 512;
	v->enabled = 1;
	v->height = 0.f;
	v->heightScale = 0.005f;
	v->edgeSoftness = 0.2f;
}

static qboolean FogVol_BuildGlobalVolume (fog_volume_t *out)
{
	const float world_density_scale = 1.f / 64.f;
	fog_volume_t v;
	vec3_t color;
	float radius;
	float density;

	if (!out || r_fogvol_global.value <= 0.f)
		return false;

	density = q_max (0.f, r_fogvol_global_density_scale.value);
	if (density <= 0.f)
		return false;

	memset (&v, 0, sizeof (v));
	FogVol_ParseColor (r_fogvol_global_color.string, color);
	VectorCopy (color, v.color);
	/* Global fog density cvars are authored in world-space units; convert once
	 * to extinction units so default values remain stable and usable. */
	v.density = density * world_density_scale;
	v.falloff = q_max (0.f, r_fogvol_global_falloff.value);
	v.noiseScale = q_max (0.f, r_fogvol_global_noise_scale.value);
	v.noiseAmount = CLAMP (0.f, r_fogvol_global_noise_amount.value, 2.f);
	v.noiseBias = CLAMP (-2.f, r_fogvol_global_noise_bias.value, 2.f);
	VectorSet (v.velocity, r_fogvol_global_velocity_x.value, r_fogvol_global_velocity_y.value, r_fogvol_global_velocity_z.value);
	v.height = r_fogvol_global_height.value;
	v.heightScale = q_max (0.f, r_fogvol_global_height_scale.value);
	v.maxDistance = 16384.f;
	v.priority = (int)Q_rint (r_fogvol_global_priority.value);
	v.blendMode = -1;
	/* Use a sphere for camera-following global fog to avoid visible box-shaped
	 * boundaries/artifacts around the player when the fallback volume moves. */
	v.shape = FOGVOL_SHAPE_SPHERE;
	v.enabled = 1;

	radius = R_SanitizeFloatRange (q_max (1024.f, q_max (gl_farclip.value, 2048.f)),
		4096.f, 1024.f, FOGVOL_RADIUS_MAX);
	for (int a = 0; a < 3; ++a)
	{
		float center = R_SanitizeFloatRange (r_refdef.vieworg[a], 0.f, -FOGVOL_COORD_LIMIT, FOGVOL_COORD_LIMIT);
		v.mins[a] = center - radius;
		v.maxs[a] = center + radius;
		v.sphereCenter[a] = center;
	}
	v.sphereRadius = radius;

	FogVol_ClampVolume (&v);
	*out = v;
	return true;
}

void R_FogVol_RegisterCvars (void)
{
	for (size_t i = 0; i < countof (fogvol_cvar_table); ++i)
		Cvar_RegisterVariable (fogvol_cvar_table[i].var);
}

void R_FogVol_SetGodrayCouplingTextures (GLuint shafts_tex, GLuint mask_tex, GLuint source_tex, qboolean ready)
{
	r_fogvol_godray_shafts_tex = shafts_tex;
	r_fogvol_godray_mask_tex = mask_tex;
	r_fogvol_godray_source_tex = source_tex;
	r_fogvol_godray_ready = ready;
}

void R_FogVol_ClearEntities (void)
{
	r_fogvolume_entity_count = 0;
}

void R_FogVol_AddEntityVolume (const fog_volume_t *volume)
{
	fog_volume_t v;

	if (!volume || r_fogvolume_entity_count >= MAX_FOGVOLUMES)
		return;
	v = *volume;
	FogVol_ClampVolume (&v);
	r_fogvolume_entities[r_fogvolume_entity_count++] = v;
}

void R_FogVol_CommitStaticBuildConfig (void)
{
}

void R_FogVol_Init (void)
{
	memset (&r_fogvol_empty_lights, 0, sizeof (r_fogvol_empty_lights));
	R_FogVol_Clear (); 
}

void R_FogVol_Clear (void)
{
	r_fogvolume_count = 0;
	r_fogvol_global_active = false;
	r_fogvol_composite_valid = false;
	r_fogvol_composite_tex = 0;
}

void R_FogVol_BuildList (void)
{
	const float local_density_scale = q_max (0.f, r_fogvol_density_scale.value);
	const qboolean debug_spawn_mode = (r_fogvol.value >= 3.f);

	r_fogvolume_count = 0;
	r_fogvol_global_active = FogVol_BuildGlobalVolume (&r_fogvol_global_volume);

	if (r_fogvol_global_active && r_fogvolume_count < MAX_FOGVOLUMES)
		r_fogvolumes[r_fogvolume_count++] = r_fogvol_global_volume;

	for (int i = 0; i < r_fogvolume_entity_count && r_fogvolume_count < MAX_FOGVOLUMES; ++i)
	{
		fog_volume_t v = r_fogvolume_entities[i];
		v.density *= local_density_scale;
		FogVol_ClampVolume (&v);
		/* Density-scaled local volumes with no medium contribution are effectively
		 * no-ops and can force unnecessary extra passes/halfres routing. */
		if (v.density <= 1e-6f)
			continue;
		r_fogvolumes[r_fogvolume_count++] = v;
	}

	if (debug_spawn_mode)
	{
		vec3_t map_mins;
		vec3_t map_maxs;

		FogVol_DebugGetMapBounds (map_mins, map_maxs);
		for (int i = 0; i < 20 && r_fogvolume_count < MAX_FOGVOLUMES; ++i)
		{
			fog_volume_t v;

			FogVol_DebugBuildRandomVolume (&v, map_mins, map_maxs);
			FogVol_ClampVolume (&v);
			if (v.density <= 1e-6f)
				continue;
			r_fogvolumes[r_fogvolume_count++] = v;
		}

		if (r_fogvolume_count < MAX_FOGVOLUMES)
		{
			fog_volume_t v;

			FogVol_DebugBuildOriginVolume (&v);
			FogVol_ClampVolume (&v);
			if (v.density > 1e-6f)
				r_fogvolumes[r_fogvolume_count++] = v;
		}
	}

	if (r_fogvolume_count > 1)
		qsort (r_fogvolumes, r_fogvolume_count, sizeof (r_fogvolumes[0]), FogVol_ComparePriority);

	if (FogVol_ShouldPrintStats ())
	{
		Con_DPrintf ("fogvol_list: global=%d entity=%d total=%d\n",
			r_fogvol_global_active ? 1 : 0, r_fogvolume_entity_count, r_fogvolume_count);
	}
}

qboolean R_FogVol_IsEnabledForFrame (void)
{
	const int mode = CLAMP (0, (int)Q_rint (r_fogvol.value), 2);
	if (mode <= 0)
		return false;
	return (glprogs.fogvol != 0);
}

qboolean R_FogVol_HasRenderableContent (void)
{
	if (r_fogvolume_count > 0)
		return true;
	if (r_fogvolume_entity_count > 0)
		return true;
	if (r_fogvol_global.value > 0.f)
	{
		float density = q_max (0.f, r_fogvol_global_density_scale.value);
		if (density > 0.f)
			return true;
	}
	return false;
}

qboolean R_FogVol_HasValidComposite (void)
{
	return r_fogvol_composite_valid;
}

qboolean R_FogVol_ShouldAffectPostFX (void)
{
	return R_FogVol_IsEnabledForFrame () && R_FogVol_HasRenderableContent ();
}

qboolean R_FogVol_CanRenderGlobal (void)
{
	if (r_fogvol_global_active)
		return true;
	if (r_fogvol_global.value <= 0.f)
		return false;
	return q_max (0.f, r_fogvol_global_density_scale.value) > 0.f;
}

qboolean R_FogVol_GetGlobalFogState (vec3_t color, float *density)
{
	const float world_density_scale = 1.f / 64.f;

	if (!r_fogvol_global_active)
	{
		float fallback_density = q_max (0.f, r_fogvol_global_density_scale.value);
		if (r_fogvol_global.value <= 0.f || fallback_density <= 0.f)
			return false;
		if (color)
			FogVol_ParseColor (r_fogvol_global_color.string, color);
		if (density)
			*density = fallback_density * world_density_scale;
		return true;
	}
	if (color)
		VectorCopy (r_fogvol_global_volume.color, color);
	if (density)
		*density = r_fogvol_global_volume.density;
	return true;
}

GLuint R_FogVol_GetCompositeTex (void)
{
	return r_fogvol_composite_valid ? r_fogvol_composite_tex : 0;
}

void R_FogVol_ClearHistory (void)
{
	r_fogvol_composite_valid = false;
	r_fogvol_composite_tex = 0;
}

static qboolean FogVol_ContainsViewOrigin (const fog_volume_t *v)
{
	if (!v)
		return false;

	if (v->shape == FOGVOL_SHAPE_SPHERE)
	{
		vec3_t delta;
		float radius = q_max (v->sphereRadius, 1.f);
		VectorSubtract (r_refdef.vieworg, v->sphereCenter, delta);
		return DotProduct (delta, delta) <= radius * radius;
	}

	for (int a = 0; a < 3; ++a)
	{
		if (r_refdef.vieworg[a] < v->mins[a] || r_refdef.vieworg[a] > v->maxs[a])
			return false;
	}
	return true;
}

static qboolean FogVol_ProjectPointToScreen (const vec3_t p,
	float view_x, float view_y, float view_w, float view_h,
	float *out_x, float *out_y, qboolean *out_behind)
{
	float clip_x = r_matviewproj[0] * p[0] + r_matviewproj[4] * p[1] + r_matviewproj[8] * p[2] + r_matviewproj[12];
	float clip_y = r_matviewproj[1] * p[0] + r_matviewproj[5] * p[1] + r_matviewproj[9] * p[2] + r_matviewproj[13];
	float clip_w = r_matviewproj[3] * p[0] + r_matviewproj[7] * p[1] + r_matviewproj[11] * p[2] + r_matviewproj[15];
	float inv_w;
	float ndc_x;
	float ndc_y;

	if (clip_w <= 1e-5f)
	{
		if (out_behind)
			*out_behind = true;
		return false;
	}

	inv_w = 1.f / clip_w;
	ndc_x = clip_x * inv_w;
	ndc_y = clip_y * inv_w;
	if (out_x)
		*out_x = view_x + (ndc_x * 0.5f + 0.5f) * view_w;
	if (out_y)
		*out_y = view_y + (ndc_y * 0.5f + 0.5f) * view_h;
	return true;
}

qboolean R_FogVol_ProjectAABBToScreenRect (const fog_volume_t *v, int *x0, int *y0, int *x1, int *y1, qboolean fullres)
{
	vec3_t bmin;
	vec3_t bmax;
	float view_x;
	float view_y;
	float view_w;
	float view_h;
	float min_x = 1e30f;
	float min_y = 1e30f;
	float max_x = -1e30f;
	float max_y = -1e30f;
	qboolean any_projected = false;
	qboolean any_behind = false;
	const int native_w = R_GetNativeRenderWidth ();
	const int native_h = R_GetNativeRenderHeight ();
	int target_w = fullres ? native_w : framebufs.fogvol.width;
	int target_h = fullres ? native_h : framebufs.fogvol.height;
	int out_x0;
	int out_y0;
	int out_x1;
	int out_y1;

	if (!v || !v->enabled || !x0 || !y0 || !x1 || !y1)
		return false;
	if (target_w <= 0 || target_h <= 0)
		return false;

	if (FogVol_ContainsViewOrigin (v))
	{
		*x0 = 0;
		*y0 = 0;
		*x1 = target_w;
		*y1 = target_h;
		return true;
	}

	if (v->shape == FOGVOL_SHAPE_SPHERE)
	{
		for (int a = 0; a < 3; ++a)
		{
			bmin[a] = v->sphereCenter[a] - v->sphereRadius;
			bmax[a] = v->sphereCenter[a] + v->sphereRadius;
		}
	}
	else
	{
		VectorCopy (v->mins, bmin);
		VectorCopy (v->maxs, bmax);
	}

	view_x = (float)(glx + r_refdef.vrect.x);
	view_y = (float)(gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height);
	view_w = (float)q_max (1, r_refdef.vrect.width);
	view_h = (float)q_max (1, r_refdef.vrect.height);

	for (int i = 0; i < 8; ++i)
	{
		vec3_t corner;
		float sx;
		float sy;

		corner[0] = (i & 1) ? bmax[0] : bmin[0];
		corner[1] = (i & 2) ? bmax[1] : bmin[1];
		corner[2] = (i & 4) ? bmax[2] : bmin[2];

		if (!FogVol_ProjectPointToScreen (corner, view_x, view_y, view_w, view_h, &sx, &sy, &any_behind))
			continue;

		any_projected = true;
		min_x = q_min (min_x, sx);
		min_y = q_min (min_y, sy);
		max_x = q_max (max_x, sx);
		max_y = q_max (max_y, sy);
	}

	if (!any_projected)
		return false;
	if (any_behind)
	{
		*x0 = 0;
		*y0 = 0;
		*x1 = target_w;
		*y1 = target_h;
		return true;
	}

	out_x0 = (int)floorf (CLAMP (view_x, min_x, view_x + view_w));
	out_y0 = (int)floorf (CLAMP (view_y, min_y, view_y + view_h));
	out_x1 = (int)ceilf (CLAMP (view_x, max_x, view_x + view_w));
	out_y1 = (int)ceilf (CLAMP (view_y, max_y, view_y + view_h));

	if (!fullres)
	{
		float scale_x = (float)target_w / q_max ((float)native_w, 1.f);
		float scale_y = (float)target_h / q_max ((float)native_h, 1.f);
		out_x0 = (int)floorf ((float)out_x0 * scale_x);
		out_y0 = (int)floorf ((float)out_y0 * scale_y);
		out_x1 = (int)ceilf ((float)out_x1 * scale_x);
		out_y1 = (int)ceilf ((float)out_y1 * scale_y);
	}

	out_x0 = CLAMP (0, out_x0, target_w);
	out_y0 = CLAMP (0, out_y0, target_h);
	out_x1 = CLAMP (0, out_x1, target_w);
	out_y1 = CLAMP (0, out_y1, target_h);
	if (out_x1 <= out_x0 || out_y1 <= out_y0)
		return false;

	*x0 = out_x0;
	*y0 = out_y0;
	*x1 = out_x1;
	*y1 = out_y1;
	return true;
}

static void R_FogVol_FillGPUVolume (const fog_volume_t *v, fog_volume_gpu_t *gpu)
{
	fog_volume_t safe;

	if (!v || !gpu)
		return;
	safe = *v;
	FogVol_ClampVolume (&safe);
	memset (gpu, 0, sizeof (*gpu));
	if (!safe.enabled)
		return;
	gpu->mins[0] = safe.mins[0]; gpu->mins[1] = safe.mins[1]; gpu->mins[2] = safe.mins[2];
	gpu->maxs[0] = safe.maxs[0]; gpu->maxs[1] = safe.maxs[1]; gpu->maxs[2] = safe.maxs[2];
	gpu->sphere[0] = safe.sphereCenter[0]; gpu->sphere[1] = safe.sphereCenter[1]; gpu->sphere[2] = safe.sphereCenter[2]; gpu->sphere[3] = safe.sphereRadius;
	gpu->color_density[0] = safe.color[0]; gpu->color_density[1] = safe.color[1]; gpu->color_density[2] = safe.color[2]; gpu->color_density[3] = safe.density;
	gpu->noise_params[0] = safe.noiseScale;
	gpu->noise_params[1] = safe.noiseAmount;
	gpu->noise_params[2] = safe.noiseBias;
	gpu->noise_params[3] = safe.turbulence;
	gpu->velocity_windspeed[0] = safe.velocity[0];
	gpu->velocity_windspeed[1] = safe.velocity[1];
	gpu->velocity_windspeed[2] = safe.velocity[2];
	gpu->velocity_windspeed[3] = safe.windSpeed;
	gpu->wind_turbulence[0] = safe.windDir[0];
	gpu->wind_turbulence[1] = safe.windDir[1];
	gpu->wind_turbulence[2] = safe.windDir[2];
	gpu->wind_turbulence[3] = safe.windSpeed;
	gpu->misc[0] = (float)safe.mode;
	gpu->misc[1] = (float)safe.shape;
	gpu->misc[2] = (float)safe.blendMode;
	gpu->misc[3] = safe.emissiveStrength;
	gpu->extra[0] = safe.maxDistance;
	gpu->extra[1] = (float)safe.priority;
	/* extra[2] is consumed as enabled-flag in fogvol.frag. */
	gpu->extra[2] = (float)(safe.enabled ? 1 : 0);
	gpu->extra[3] = safe.height;
	gpu->params2[0] = safe.edgeSoftness;
	gpu->params2[1] = safe.heightScale;
	gpu->params2[2] = safe.falloff;
	gpu->params2[3] = 0.f;
	if (r_skyvis.value > 0.f && R_SkyVis_Active ())
		gpu->params2[3] = CLAMP (0.f, R_SkyVis_Sample (safe.sphereCenter), 1.f);
}

static void R_FogVol_UploadVolumeRange (const fog_volume_t *volumes, int count)
{
	GLuint buf;
	GLbyte *ofs;
	fog_volume_gpu_t gpu_volumes[MAX_FOGVOLUMES];

	if (!volumes || count <= 0)
		return;
	count = q_min (count, MAX_FOGVOLUMES);
	memset (gpu_volumes, 0, sizeof (gpu_volumes));
	for (int i = 0; i < count; ++i)
		R_FogVol_FillGPUVolume (&volumes[i], &gpu_volumes[i]);

	GL_Upload (GL_SHADER_STORAGE_BUFFER, gpu_volumes, sizeof (fog_volume_gpu_t) * count, &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, buf, (GLintptr)ofs, sizeof (fog_volume_gpu_t) * count);
}

static void R_FogVol_SetShaderUniforms (int steps, int mode, qboolean use_halfres,
	int fog_width, int fog_height,
	float depth_scale_x, float depth_scale_y,
	float depth_bias_x, float depth_bias_y,
	float color_bias_x, float color_bias_y,
	float color_scale_x, float color_scale_y,
	float depth_uv_scale_x, float depth_uv_scale_y,
	const float inv_viewproj[16],
	float view_x, float view_y, float view_w, float view_h,
	float depth_near, float depth_far, float depth_sky_cutoff)
{
	vec3_t shadow_dir = { 0.f, 0.f, -1.f };
	GLuint froxel_tex = 0;
	int froxel_light_count = 0;
	float froxel_params0[4] = {0.f, 0.f, 0.f, 0.f};
	float froxel_params1[4] = {0.f, 1.f, 1.f, 1.f};
	int froxel_debug_random_mode = CLAMP (0, (int)Q_rint (r_fogvol_debug_froxel_random.value), 4);
	int froxel_debug_mode = 0;
	qboolean force_froxel_debug = false;
	qboolean froxel_ready = R_Froxel_GetShaderState (&froxel_tex, &froxel_light_count, froxel_params0, froxel_params1);
	qboolean shadow_enabled = (mode == 2 && r_fogvol_shadow.value > 0.f);
	const sun_t *sun = R_GetSun ();
	float fog_density = 1.f;
	float sigma_max = 4.f;
	float noise_scale = q_max (0.f, r_fogvol_noise_scale.value);
	float noise_amount = CLAMP (0.f, r_fogvol_noise_amount.value, 2.f);
	float noise_bias = CLAMP (-2.f, r_fogvol_noise_bias.value, 2.f);
	float reverse_z_flag = gl_clipcontrol_able ? 1.f : 0.f;
	int lighting_mode = (mode > 0) ? 2 : 0;
	float sun_shadow_viewproj[4][16];
	float sun_shadow_splits[4] = { 0.f, 0.f, 0.f, 0.f };
	int sun_shadow_cascades = 0;
	float sun_shadow_bias = 0.f;
	float sun_shadow_pcf = 0.f;
	float light_scale_dlight = q_max (0.f, r_fogvol_dlightscale.value) * q_max (0.f, r_fogvol_light_dlight_boost.value);
	/* Sun scatter scale stays tied to both sun controls. */
	float light_scale_sun = q_max (0.f, r_fogvol_froxel_sun.value) * q_max (0.f, r_fogvol_light_sun_boost.value);
	float light_scale_emissive = q_max (0.f, r_fogvol_light_emissive_boost.value);
	float light_contrast = CLAMP (0.5f, r_fogvol_light_contrast.value, 4.f);
	float light_ambient = CLAMP (0.f, r_fogvol_light_ambient.value, 1.f);
	/* Ambient is additionally clamped in shader to keep lit terms visible. */
	float shadow_contrast = CLAMP (0.5f, r_fogvol_shadow_contrast.value, 4.f);
	float extinction_relief = CLAMP (0.f, r_fogvol_light_extinction_relief.value, 0.95f);
	float emissive_floor = 0.f;
	qboolean light_enabled = false;
	qboolean light_has_froxel = false;
	qboolean light_has_sun = false;
	qboolean light_has_emissive = false;
	float debug_dlight_scale = 1.f;
	float debug_sun_scale = 0.f;
	float debug_emissive_scale = 0.f;
	qboolean sun_shadow_map_enabled = R_Shadow_GetSunCascadeData (sun_shadow_viewproj, sun_shadow_splits, &sun_shadow_cascades, &sun_shadow_bias, &sun_shadow_pcf);
	const int native_w = R_GetNativeRenderWidth ();
	const int native_h = R_GetNativeRenderHeight ();

	if (R_Froxel_GetDebugScales (&debug_dlight_scale, &debug_sun_scale, &debug_emissive_scale))
	{
		light_scale_dlight *= q_max (0.f, debug_dlight_scale);
		light_scale_sun = q_max (light_scale_sun, q_max (0.f, debug_sun_scale));
		emissive_floor = q_max (0.f, debug_emissive_scale);
		light_contrast = q_max (light_contrast, 2.0f);
		light_ambient = q_min (light_ambient, 0.03f);
		shadow_contrast = q_max (shadow_contrast, 1.75f);
		extinction_relief = q_max (extinction_relief, 0.78f);
		shadow_enabled = true;
	}

	if (r_fogvol_light.value <= 0.f)
	{
		if (froxel_debug_random_mode <= 0)
		{
			/* Honor explicit dlight controls: do not force a dlight fallback here. */
			light_scale_dlight = 0.f;
			light_scale_sun = 0.f;
			if (r_fogvol_emissive.value <= 0.f)
			{
				light_scale_emissive = 0.f;
				emissive_floor = 0.f;
			}
		}
	}

	if (sun && R_WorldHasSun ())
	{
		VectorScale (sun->dir, -1.f, shadow_dir);
		if (VectorNormalize (shadow_dir) <= 0.f)
			VectorSet (shadow_dir, 0.f, 0.f, -1.f);
	}

	if (froxel_debug_random_mode > 0)
	{
		force_froxel_debug = true;
		if (froxel_debug_random_mode >= 2)
			froxel_debug_mode = froxel_debug_random_mode - 1;
		else
			froxel_debug_mode = 1;
	}
	else if (r_fogvol_debug.value >= 9.f)
	{
		froxel_debug_mode = 1;
		force_froxel_debug = true;
	}

	if (force_froxel_debug)
	{
		/* Make injected random froxel colors clearly visible in final fog shading. */
		light_ambient = 0.f;
		light_scale_dlight = q_max (light_scale_dlight, 3.0f);
		light_contrast = q_max (light_contrast, 1.0f);
	}
	lighting_mode = CLAMP (0, mode, 2);
	light_has_froxel = (mode > 0 && (froxel_ready || force_froxel_debug) && light_scale_dlight > 0.f);
	light_has_sun = (mode > 0 && shadow_enabled && light_scale_sun > 0.f);
	light_has_emissive = (mode > 1 && r_fogvol_emissive.value > 0.f && light_scale_emissive > 0.f);
	if (lighting_mode == 1)
		light_enabled = (light_has_froxel || light_has_sun);
	else if (lighting_mode >= 2)
		light_enabled = (light_has_froxel || light_has_sun || light_has_emissive);

	GL_Uniform1iFunc (FOGVOL_U_STEPS, q_max (8, steps));
	GL_Uniform1iFunc (FOGVOL_U_NOISE_ENABLED, r_fogvol_noise.value > 0.f ? 1 : 0);
	GL_Uniform1iFunc (FOGVOL_U_DEBUG_MODE, CLAMP (0, (int)Q_rint (r_fogvol_debug.value), 16));
	GL_UniformMatrix4fvFunc (FOGVOL_U_INV_VIEWPROJ, 1, GL_FALSE, inv_viewproj);
	GL_Uniform1iFunc (FOGVOL_U_NOISE_MODE, CLAMP (0, (int)Q_rint (r_fogvol_noisemode.value), 2));
	GL_Uniform1iFunc (FOGVOL_U_PHYS_BLEND, 1);
	GL_Uniform1iFunc (FOGVOL_U_JITTER_ENABLED, r_fogvol_jitter.value > 0.f ? 1 : 0);
	GL_Uniform3fFunc (FOGVOL_U_CAMERA_POS_WS, r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2]);
	GL_Uniform4fFunc (FOGVOL_U_VIEWPORT_PARAMS, (float)native_w, (float)native_h, 1.f / q_max (1.f, (float)native_w), 1.f / q_max (1.f, (float)native_h));
	GL_Uniform2fFunc (FOGVOL_U_DEPTH_SCALE, depth_scale_x, depth_scale_y);
	GL_Uniform2fFunc (FOGVOL_U_DEPTH_BIAS, depth_bias_x, depth_bias_y);
	GL_Uniform2fFunc (FOGVOL_U_COLOR_SCALE, color_scale_x, color_scale_y);
	GL_Uniform2fFunc (FOGVOL_U_COLOR_BIAS, color_bias_x, color_bias_y);
	GL_Uniform2fFunc (FOGVOL_U_DEPTH_UV_SCALE, depth_uv_scale_x, depth_uv_scale_y);
	GL_Uniform4fFunc (FOGVOL_U_VIEW_PARAMS, view_x, view_y, 1.f / q_max (1.f, view_w), 1.f / q_max (1.f, view_h));
	GL_Uniform4fFunc (FOGVOL_U_DEPTH_PARAMS, depth_near, depth_far, reverse_z_flag, depth_sky_cutoff);
	GL_Uniform4fFunc (FOGVOL_U_DENSITY_PARAMS, fog_density, sigma_max, noise_scale, noise_bias);
	GL_Uniform1iFunc (FOGVOL_U_EMISSIVE_ENABLED, r_fogvol_emissive.value > 0.f ? 1 : 0);
	GL_Uniform1iFunc (FOGVOL_U_BLEND_MODE_DEFAULT, CLAMP (0, (int)Q_rint (r_fogvol_blendmode.value), 1));
	GL_Uniform1iFunc (FOGVOL_U_LIGHT_ENABLED, light_enabled ? 1 : 0);
	GL_Uniform1iFunc (FOGVOL_U_SHADOW_ENABLED, shadow_enabled ? 1 : 0);
	GL_Uniform1iFunc (FOGVOL_U_SHADOW_SAMPLES, shadow_enabled ? 1 : 0);
	GL_Uniform1fFunc (FOGVOL_U_SHADOW_STRENGTH, CLAMP (0.f, r_fogvol_shadow_strength.value, 4.f));
	GL_Uniform1fFunc (FOGVOL_U_SHADOW_JITTER, (shadow_enabled && r_fogvol_jitter.value > 0.f) ? 1.f : 0.f);
	GL_Uniform3fFunc (FOGVOL_U_SHADOW_DIR, shadow_dir[0], shadow_dir[1], shadow_dir[2]);
	GL_Uniform1iFunc (FOGVOL_U_LIGHTGRID_ENABLED, 0);
	GL_Uniform1iFunc (FOGVOL_U_FRAME_INDEX, r_framecount);
	GL_Uniform1fFunc (FOGVOL_U_NOISE_SUBSAMPLE, 1.f);
	GL_Uniform1fFunc (FOGVOL_U_NOISE_LOD_SWITCH, 64.f);
	GL_Uniform1fFunc (FOGVOL_U_DOMAINWARP_DIST, 128.f);
	GL_Uniform1fFunc (FOGVOL_U_NOISE_DETAIL_STRENGTH, noise_amount);
	GL_Uniform1fFunc (FOGVOL_U_DLIGHT_SCALE, light_scale_dlight);
	GL_Uniform4fFunc (FOGVOL_U_LIGHT_SCISSOR, 0.f, 0.f, 0.f, 0.f);
	GL_Uniform4fFunc (FOGVOL_U_LIGHT_SOURCE_SCALES, light_scale_dlight, light_scale_sun, light_scale_emissive, light_contrast);
	GL_Uniform1iFunc (FOGVOL_U_LIGHTING_MODE, lighting_mode);
	/* Safety fallback: disable Godray->Fog coupling to avoid translucent band artifacts. */
	GL_Uniform1iFunc (FOGVOL_U_GODRAY_COUPLING, 0);
	GL_Uniform1iFunc (FOGVOL_U_LOCAL_OCCLUSION_MODE, 0);
	GL_Uniform1iFunc (FOGVOL_U_FROXEL_ENABLED, (mode > 0 && (froxel_ready || force_froxel_debug)) ? 1 : 0);
	GL_Uniform4fFunc (FOGVOL_U_FROXEL_PARAMS0, froxel_params0[0], froxel_params0[1], froxel_params0[2], froxel_params0[3]);
	GL_Uniform4fFunc (FOGVOL_U_FROXEL_PARAMS1, froxel_params1[0], froxel_params1[1], froxel_params1[2], froxel_params1[3]);
	GL_Uniform1iFunc (FOGVOL_U_FROXEL_DEBUG, froxel_debug_mode);
	GL_Uniform1iFunc (FOGVOL_U_FROXEL_PARITY_MODE, 0);
	GL_Uniform4fFunc (FOGVOL_U_FROXEL_TEMPORAL_PARAMS, 0.f, 0.f, 0.f, 0.f);
	GL_Uniform1iFunc (FOGVOL_U_CHECKERBOARD, 0);
	GL_Uniform4fFunc (FOGVOL_U_CLUSTER_PARAMS, light_ambient, shadow_contrast, emissive_floor, extinction_relief);
	GL_UniformMatrix4fvFunc (FOGVOL_U_SUN_SHADOW_VIEWPROJ, q_max (1, sun_shadow_cascades), GL_FALSE, &sun_shadow_viewproj[0][0]);
	GL_Uniform4fFunc (FOGVOL_U_SUN_SHADOW_PARAMS,
		(shadow_enabled && sun_shadow_map_enabled) ? 1.f : 0.f,
		sun_shadow_bias, sun_shadow_pcf, 0.f);
	GL_Uniform4fFunc (FOGVOL_U_SUN_SHADOW_SPLITS, sun_shadow_splits[0], sun_shadow_splits[1], sun_shadow_splits[2], sun_shadow_splits[3]);
	GL_Uniform1iFunc (FOGVOL_U_SUN_SHADOW_CASCADE_COUNT, (shadow_enabled && sun_shadow_map_enabled) ? q_max (1, sun_shadow_cascades) : 0);
}

static void R_FogVol_RestoreRenderState (void)
{
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glViewport (glx, gly, glwidth, glheight);
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.composite.fbo);
	GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, framebufs.composite.fbo);
	GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, framebufs.composite.fbo);
	glReadBuffer (GL_COLOR_ATTACHMENT0);
	glDrawBuffer (GL_COLOR_ATTACHMENT0);
	GL_SetState (GLS_DEFAULT_STATE);
	GL_SetScissorEnabled (false);
	GL_UseProgram (0);
}

void R_FogVol_Render (void)
{
	float inv_viewproj[16];
	int mode;
	qboolean use_halfres;
	qboolean use_native_pingpong = false;
	int fog_width, fog_height;
	float view_x, view_y, view_w, view_h;
	float depth_view_x, depth_view_y, depth_view_w, depth_view_h;
	float depth_scale_x, depth_scale_y;
	float depth_bias_x = 0.f, depth_bias_y = 0.f;
	float color_bias_x = 0.f, color_bias_y = 0.f;
	float color_scale_x, color_scale_y;
	float depth_uv_scale_x = 0.f, depth_uv_scale_y = 0.f;
	float depth_near, depth_far, depth_sky_cutoff;
	int steps;
	GLuint src_tex, src_fbo;
	GLuint depth_tex;
	int fog_src = 0;
	qboolean has_drawn = false;
	GLuint final_tex, final_fbo;
	GLuint froxel_tex = 0;
	int froxel_light_count = 0;
	float froxel_params0[4] = {0.f, 0.f, 0.f, 0.f};
	float froxel_params1[4] = {0.f, 1.f, 1.f, 1.f};
	GLuint buf;
	GLbyte *ofs;
	int scissor_x0[MAX_FOGVOLUMES];
	int scissor_y0[MAX_FOGVOLUMES];
	int scissor_x1[MAX_FOGVOLUMES];
	int scissor_y1[MAX_FOGVOLUMES];
	qboolean scissor_valid[MAX_FOGVOLUMES];
	const int native_w = R_GetNativeRenderWidth ();
	const int native_h = R_GetNativeRenderHeight ();
	int depth_tex_w = native_w;
	int depth_tex_h = native_h;
	int color_tex_w = native_w;
	int color_tex_h = native_h;
	float native_to_depth_x = 1.f;
	float native_to_depth_y = 1.f;
	float native_to_color_x = 1.f;
	float native_to_color_y = 1.f;
	const int expected_half_w = q_max (1, native_w / 2);
	const int expected_half_h = q_max (1, native_h / 2);
	static int last_size_mismatch_frame = -1;
	qboolean debug_dump = FogVol_ShouldPrintDebugDump ();

	r_fogvol_composite_valid = false;
	r_fogvol_composite_tex = 0;

	if (debug_dump)
		FogVol_DumpAllCvars ();

	if (!R_FogVol_IsEnabledForFrame ())
	{
		if (debug_dump)
			Con_DPrintf ("fogvol_dump skip: disabled_for_frame mode=%d glprogs.fogvol=%u\n",
				CLAMP (0, (int)Q_rint (r_fogvol.value), 2), glprogs.fogvol);
		if (FogVol_ShouldPrintStats ())
			Con_DPrintf ("fogvol_render: skipped (disabled or shader missing)\n");
		return;
	}
	if (r_fogvolume_count <= 0)
	{
		if (debug_dump)
			Con_DPrintf ("fogvol_dump skip: no_volumes entity=%d global_active=%d\n",
				r_fogvolume_entity_count, r_fogvol_global_active ? 1 : 0);
		if (FogVol_ShouldPrintStats ())
			Con_DPrintf ("fogvol_render: skipped (no volumes)\n");
		return;
	}
	if (!Mat4_Inverse (r_matviewproj, inv_viewproj))
	{
		if (debug_dump)
			Con_DPrintf ("fogvol_dump skip: inv_viewproj_failed\n");
		if (FogVol_ShouldPrintStats ())
			Con_DPrintf ("fogvol_render: skipped (inverse viewproj failed)\n");
		return;
	}

	mode = CLAMP (0, (int)Q_rint (r_fogvol.value), 2);
	/* Halfres is controlled exclusively by r_fogvol_halfres. */
	use_halfres = (r_fogvol_halfres.value > 0.f);
	if (r_fogvol_debug_froxel_random.value > 0.f)
		use_halfres = false;

	/* Global (camera-following) fog always covers the full view. Rendering any
	 * frame that includes this path in halfres makes the baseline medium read
	 * like a post-process overlay instead of volumetric depth. Keep all global
	 * fog frames native-res by ping-ponging between composite and finalcopy. */
	if (use_halfres && r_fogvol_global_active)
	{
		use_halfres = false;
		use_native_pingpong = true;
	}

	/* Keep runtime behavior consistent with currently allocated fogvol targets.
	 * This avoids quadrant artifacts when r_fogvol_halfres is toggled without
	 * a full framebuffer rebuild. */
	if (!use_native_pingpong && use_halfres)
	{
		if (framebufs.fogvol.width != expected_half_w || framebufs.fogvol.height != expected_half_h)
		{
			if (framebufs.fogvol.width == native_w && framebufs.fogvol.height == native_h)
				use_halfres = false;
			else if (r_framecount != last_size_mismatch_frame)
			{
				last_size_mismatch_frame = r_framecount;
				Con_DPrintf ("fogvol: size mismatch (requested halfres=%d, allocated=%dx%d, expected half=%dx%d/full=%dx%d)\n",
					1, framebufs.fogvol.width, framebufs.fogvol.height, expected_half_w, expected_half_h, native_w, native_h);
			}
		}
	}
	else if (!use_native_pingpong)
	{
		if (framebufs.fogvol.width == expected_half_w && framebufs.fogvol.height == expected_half_h)
			use_halfres = true;
	}

	fog_width = use_halfres ? framebufs.fogvol.width : native_w;
	fog_height = use_halfres ? framebufs.fogvol.height : native_h;
	view_x = (float)(glx + r_refdef.vrect.x);
	view_y = (float)(gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height);
	view_w = (float)r_refdef.vrect.width;
	view_h = (float)r_refdef.vrect.height;
	depth_view_x = view_x;
	depth_view_y = view_y;
	depth_view_w = view_w;
	depth_view_h = view_h;
	depth_tex = framebufs.composite.depth_stencil_tex;

	if (!depth_tex)
	{
		if (debug_dump)
			Con_DPrintf ("fogvol_dump skip: missing_depth_tex composite.depth=%u\n", framebufs.composite.depth_stencil_tex);
		if (FogVol_ShouldPrintStats ())
			Con_DPrintf ("fogvol_render: skipped (missing depth texture)\n");
		return;
	}

	if (fog_width <= 0 || fog_height <= 0 || view_w <= 0.f || view_h <= 0.f)
		return;

	(void)FogVol_QueryTexture2DSize (depth_tex, &depth_tex_w, &depth_tex_h);
	(void)FogVol_QueryTexture2DSize (framebufs.composite.color_tex, &color_tex_w, &color_tex_h);
	if (depth_tex_w <= 0 || depth_tex_h <= 0 || color_tex_w <= 0 || color_tex_h <= 0)
		return;

	native_to_depth_x = (float)depth_tex_w / q_max (1.f, (float)native_w);
	native_to_depth_y = (float)depth_tex_h / q_max (1.f, (float)native_h);
	native_to_color_x = (float)color_tex_w / q_max (1.f, (float)native_w);
	native_to_color_y = (float)color_tex_h / q_max (1.f, (float)native_h);
	depth_uv_scale_x = 1.f / q_max (1.f, (float)depth_tex_w);
	depth_uv_scale_y = 1.f / q_max (1.f, (float)depth_tex_h);
	if (debug_dump && (depth_tex_w != native_w || depth_tex_h != native_h || color_tex_w != native_w || color_tex_h != native_h))
	{
		Con_DPrintf ("fogvol_dump alignment: native=%dx%d depth=%dx%d color=%dx%d ratios depth=(%.4f %.4f) color=(%.4f %.4f)\n",
			native_w, native_h, depth_tex_w, depth_tex_h, color_tex_w, color_tex_h,
			native_to_depth_x, native_to_depth_y, native_to_color_x, native_to_color_y);
	}

	color_scale_x = (float)color_tex_w / (float)q_max (1, fog_width);
	color_scale_y = (float)color_tex_h / (float)q_max (1, fog_height);
	if (use_halfres)
	{
		/* Half-res fog viewport starts at (0,0); rebase to actual view rect in full-res
		 * scene/composite space so color taps and world reconstruction stay aligned. */
		color_bias_x = view_x * native_to_color_x;
		color_bias_y = view_y * native_to_color_y;
	}
	if (use_halfres)
	{
		/* In half-res path gl_FragCoord is local to fog target; map into the
		 * on-screen view rect before depth fetch to avoid fullscreen overlay artifacts. */
		depth_scale_x = (depth_view_w * native_to_depth_x) / (float)q_max (1, fog_width);
		depth_scale_y = (depth_view_h * native_to_depth_y) / (float)q_max (1, fog_height);
		depth_bias_x = depth_view_x * native_to_depth_x;
		depth_bias_y = depth_view_y * native_to_depth_y;
	}
	else
	{
		/* Keep full-res fog reconstruction in composite-depth space. */
		depth_scale_x = (float)depth_tex_w / (float)q_max (1, fog_width);
		depth_scale_y = (float)depth_tex_h / (float)q_max (1, fog_height);
		depth_bias_x = 0.f;
		depth_bias_y = 0.f;
	}
	depth_near = R_GetViewZNear ();
	depth_far = R_GetViewZFar ();
	/* Treat only clear-depth as sky. A broad cutoff (e.g. 0.001 in reverse-Z)
	 * classifies distant world geometry as sky and collapses volumetric depth
	 * into a near-uniform fullscreen tint. */
	depth_sky_cutoff = gl_clipcontrol_able ? 1e-6f : (1.f - 1e-6f);

	steps = CLAMP (8, (int)Q_rint (r_fogvol_steps.value), 128);
	if (r_fogvol_global.value > 0.f)
		steps = q_max (24, steps);
	/* Halfres mode controls step reduction; keep full-res step budget unchanged. */
	if (use_halfres)
		steps = q_max (16, (steps * 3) / 4);

	R_Froxel_BeginFrame (depth_near, depth_far);
	R_Froxel_InjectDlights ();
	R_Froxel_EndFrame ();
	R_Froxel_GetShaderState (&froxel_tex, &froxel_light_count, froxel_params0, froxel_params1);

	R_FogVol_UploadVolumeRange (r_fogvolumes, r_fogvolume_count);
	GL_Upload (GL_SHADER_STORAGE_BUFFER, &r_fogvol_empty_lights, sizeof (r_fogvol_empty_lights), &buf, &ofs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 4, buf, (GLintptr)ofs, sizeof (r_fogvol_empty_lights));

	GL_UseProgram (glprogs.fogvol);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_SetScissorEnabled (false);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, depth_tex);
	GL_BindNative (GL_TEXTURE6, GL_TEXTURE_3D, froxel_tex);
	GL_BindNative (GL_TEXTURE8, GL_TEXTURE_2D_ARRAY, framebufs.shadow.sun_depth_tex);
	R_FogVol_SetShaderUniforms (steps, mode, use_halfres, fog_width, fog_height,
		depth_scale_x, depth_scale_y, depth_bias_x, depth_bias_y, color_bias_x, color_bias_y, color_scale_x, color_scale_y,
		depth_uv_scale_x, depth_uv_scale_y,
		inv_viewproj, depth_view_x, depth_view_y, depth_view_w, depth_view_h,
		depth_near, depth_far, depth_sky_cutoff);

	if (FogVol_ShouldPrintStats ())
	{
		Con_DPrintf ("fogvol_depth: source=composite depth=%dx%d color=%dx%d fog=%dx%d bias=(%.2f %.2f)\n",
			depth_tex_w, depth_tex_h, color_tex_w, color_tex_h, fog_width, fog_height, depth_bias_x, depth_bias_y);
	}

	src_tex = framebufs.composite.color_tex;
	src_fbo = framebufs.composite.fbo;
	for (int i = 0; i < r_fogvolume_count; ++i)
	{
		const fog_volume_t *v = &r_fogvolumes[i];
		scissor_valid[i] = R_FogVol_ProjectAABBToScreenRect (v, &scissor_x0[i], &scissor_y0[i], &scissor_x1[i], &scissor_y1[i], !use_halfres);
	}

	if (debug_dump)
	{
		Con_DPrintf ("fogvol_dump runtime frame=%d mode=%d count=%d entity=%d global_active=%d halfres=%d native_pingpong=%d\n",
			r_framecount, mode, r_fogvolume_count, r_fogvolume_entity_count, r_fogvol_global_active ? 1 : 0,
			use_halfres ? 1 : 0, use_native_pingpong ? 1 : 0);
		Con_DPrintf ("fogvol_dump dims native=%dx%d fog=%dx%d view=(%.1f %.1f %.1f %.1f)\n",
			native_w, native_h, fog_width, fog_height, view_x, view_y, view_w, view_h);
		Con_DPrintf ("fogvol_dump depth tex=%u size=%dx%d scale=(%.6f %.6f) bias=(%.6f %.6f) znear=%.6f zfar=%.6f skyCutoff=%.9f clipcontrol=%d\n",
			depth_tex, depth_tex_w, depth_tex_h, depth_scale_x, depth_scale_y, depth_bias_x, depth_bias_y,
			depth_near, depth_far, depth_sky_cutoff, gl_clipcontrol_able ? 1 : 0);
		Con_DPrintf ("fogvol_dump color source=%u size=%dx%d scale=(%.6f %.6f) bias=(%.6f %.6f) steps=%d froxelTex=%u froxelLights=%d\n",
			framebufs.composite.color_tex, color_tex_w, color_tex_h, color_scale_x, color_scale_y, color_bias_x, color_bias_y,
			steps, froxel_tex, froxel_light_count);
		for (int i = 0; i < r_fogvolume_count; ++i)
		{
			const fog_volume_t *v = &r_fogvolumes[i];
			Con_DPrintf ("fogvol_dump vol[%d] en=%d shape=%d mode=%d pri=%d blend=%d dens=%.6f emissive=%.6f falloff=%.6f maxDist=%.1f\n",
				i, v->enabled ? 1 : 0, v->shape, v->mode, v->priority, v->blendMode,
				v->density, v->emissiveStrength, v->falloff, v->maxDistance);
			Con_DPrintf ("fogvol_dump vol[%d] mins=(%.1f %.1f %.1f) maxs=(%.1f %.1f %.1f) sphere=(%.1f %.1f %.1f r=%.1f)\n",
				i, v->mins[0], v->mins[1], v->mins[2], v->maxs[0], v->maxs[1], v->maxs[2],
				v->sphereCenter[0], v->sphereCenter[1], v->sphereCenter[2], v->sphereRadius);
			Con_DPrintf ("fogvol_dump vol[%d] scissor valid=%d rect=(%d %d %d %d)\n",
				i, scissor_valid[i] ? 1 : 0, scissor_x0[i], scissor_y0[i], scissor_x1[i], scissor_y1[i]);
		}
		FogVol_DumpDepthProbe (inv_viewproj, view_x, view_y, view_w, view_h);
	}
	GL_SetScissorEnabled (true);

	for (int i = 0; i < r_fogvolume_count; ++i)
	{
		const fog_volume_t *v = &r_fogvolumes[i];
		int x0;
		int y0;
		int x1;
		int y1;
		int fog_dst = 0;
		GLuint dst_tex;
		GLuint dst_fbo;
		const int pass_w = use_halfres ? fog_width : native_w;
		const int pass_h = use_halfres ? fog_height : native_h;

		if (!v->enabled || !scissor_valid[i])
			continue;
		x0 = scissor_x0[i];
		y0 = scissor_y0[i];
		x1 = scissor_x1[i];
		y1 = scissor_y1[i];

		if (use_native_pingpong)
		{
			if (!has_drawn || src_tex == framebufs.composite.color_tex)
			{
				dst_tex = framebufs.fogvol.finalcopy_tex;
				dst_fbo = framebufs.fogvol.finalcopy_fbo;
			}
			else
			{
				dst_tex = framebufs.composite.color_tex;
				dst_fbo = framebufs.composite.fbo;
			}
		}
		else
		{
			fog_dst = has_drawn ? (1 - fog_src) : 0;
			dst_tex = framebufs.fogvol.color_tex[fog_dst];
			dst_fbo = framebufs.fogvol.fbo[fog_dst];
		}

		if (x0 > 0 || y0 > 0 || x1 < pass_w || y1 < pass_h)
		{
			GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, src_fbo);
			GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, dst_fbo);
			glReadBuffer (GL_COLOR_ATTACHMENT0);
			glDrawBuffer (GL_COLOR_ATTACHMENT0);
			GL_BlitFramebufferFunc (0, 0, pass_w, pass_h,
				0, 0, pass_w, pass_h,
				GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}

		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, src_tex);
		GL_BindFramebufferFunc (GL_FRAMEBUFFER, dst_fbo);
		if (use_halfres)
			glViewport (0, 0, fog_width, fog_height);
		else
			glViewport ((int)view_x, (int)view_y, (int)view_w, (int)view_h);
		glScissor (x0, y0, q_max (1, x1 - x0), q_max (1, y1 - y0));

		GL_Uniform1iFunc (FOGVOL_U_VOLUME_INDEX, i);
		GL_Uniform4fFunc (FOGVOL_U_LIGHT_SCISSOR, (float)x0, (float)y0, (float)x1, (float)y1);
		glDrawArrays (GL_TRIANGLES, 0, 3);

		src_tex = dst_tex;
		src_fbo = dst_fbo;
		if (!use_native_pingpong)
			fog_src = fog_dst;
		has_drawn = true;
	}
	GL_SetScissorEnabled (false);

	if (!has_drawn)
	{
		if (debug_dump)
			Con_DPrintf ("fogvol_dump skip: no_drawn_volumes (all disabled or offscreen)\n");
		if (FogVol_ShouldPrintStats ())
			Con_DPrintf ("fogvol_render: skipped (all volumes disabled)\n");
		R_FogVol_RestoreRenderState ();
		return;
	}

	final_tex = src_tex;
	final_fbo = src_fbo;

	if (use_halfres)
	{
		GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, final_fbo);
		GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, framebufs.fogvol.finalcopy_fbo);
		GL_BlitFramebufferFunc (0, 0, fog_width, fog_height,
			0, 0, native_w, native_h,
			GL_COLOR_BUFFER_BIT, GL_LINEAR);
		final_tex = framebufs.fogvol.finalcopy_tex;
		final_fbo = framebufs.fogvol.finalcopy_fbo;
	}

	if (final_fbo != framebufs.composite.fbo)
	{
		GL_BindFramebufferFunc (GL_READ_FRAMEBUFFER, final_fbo);
		GL_BindFramebufferFunc (GL_DRAW_FRAMEBUFFER, framebufs.composite.fbo);
		glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
		if (use_halfres)
		{
			GL_BlitFramebufferFunc (0, 0, native_w, native_h,
				0, 0, native_w, native_h,
				GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}
		else
		{
			GL_BlitFramebufferFunc ((int)view_x, (int)view_y, (int)(view_x + view_w), (int)(view_y + view_h),
				(int)view_x, (int)view_y, (int)(view_x + view_w), (int)(view_y + view_h),
				GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}
		glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	}
	/* Keep subsequent passes deterministic: fogvol uses custom FBO/read targets
	 * and fullscreen state, so restore a sane default baseline afterwards. */
	R_FogVol_RestoreRenderState ();

	r_fogvol_composite_valid = true;
	r_fogvol_composite_tex = final_tex;

	if (debug_dump)
	{
		Con_DPrintf ("fogvol_dump composite valid=%d tex=%u final_fbo=%u composite_fbo=%u\n",
			r_fogvol_composite_valid ? 1 : 0, r_fogvol_composite_tex, final_fbo, framebufs.composite.fbo);
	}

	if (FogVol_ShouldPrintStats ())
	{
		Con_DPrintf ("fogvol_stats: mode=%d volumes=%d froxelLights=%d steps=%d halfres=%d\n",
			mode, r_fogvolume_count, froxel_light_count, steps, use_halfres ? 1 : 0);
	}
}

void R_FogVol_DrawDebug2D (void)
{
	vec3_t wire_color;

	if (r_fogvol_debug.value <= 0.f)
		return;
	VectorSet (wire_color, 1.f, 1.f, 1.f);
	for (int i = 0; i < r_fogvolume_count; ++i)
	{
		const fog_volume_t *v = &r_fogvolumes[i];
		if (!v->enabled)
			continue;
		R_DebugDrawWireBox (v->mins, v->maxs, wire_color, true);
	}
	R_DebugFlushGeometry ();
}

void R_FogVol_LogEndFrameState (void)
{
}
