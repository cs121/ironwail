#include "quakedef.h"
#include "draw.h"
#include "r_fogvol.h"
#include "r_fogvol_internal.h"
#include "r_dlight_pool.h"
#include <math.h>
#include <string.h>

extern cvar_t gl_farclip;

typedef struct froxel_gpu_light_s
{
	float pos_rad[4];
	float color_intensity[4];
	uint32_t type;
	uint32_t _pad[3];
} froxel_gpu_light_t;

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

static froxel_state_t r_froxel;
static froxel_gpu_light_t r_froxel_gpu_lights[MAX_FROXEL_GPU_LIGHTS];

typedef struct froxel_debug_state_s
{
	double next_refresh_time;
	int random_light_count;
	int phase;
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

static qboolean R_Froxel_DebugEnabled (void)
{
	return (r_fogvol_debug_froxel_random.value > 0.f);
}

static float R_Froxel_DebugRand01 (void)
{
	return (float)(rand () & 0x7fff) * (1.f / 32767.f);
}

static float R_Froxel_DebugRandRange (float lo, float hi)
{
	return lo + (hi - lo) * R_Froxel_DebugRand01 ();
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
	const int slice_pixels = nx * ny;
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
			const float base = R_Froxel_DebugRandRange (0.02f, 1.35f);
			slice[i * 4 + 0] = base * R_Froxel_DebugRandRange (0.12f, 2.4f);
			slice[i * 4 + 1] = base * R_Froxel_DebugRandRange (0.12f, 2.4f);
			slice[i * 4 + 2] = base * R_Froxel_DebugRandRange (0.12f, 2.4f);
			slice[i * 4 + 3] = 1.f;
		}
		GL_TexSubImage3DFunc (GL_TEXTURE_3D, 0, 0, 0, z, nx, ny, 1, GL_RGBA, GL_FLOAT, slice);
	}

	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_3D, r_froxel.history_tex);
	for (int z = 0; z < nz; ++z)
	{
		for (int i = 0; i < slice_pixels; ++i)
		{
			const float base = R_Froxel_DebugRandRange (0.02f, 1.35f);
			slice[i * 4 + 0] = base * R_Froxel_DebugRandRange (0.12f, 2.4f);
			slice[i * 4 + 1] = base * R_Froxel_DebugRandRange (0.12f, 2.4f);
			slice[i * 4 + 2] = base * R_Froxel_DebugRandRange (0.12f, 2.4f);
			slice[i * 4 + 3] = 1.f;
		}
		GL_TexSubImage3DFunc (GL_TEXTURE_3D, 0, 0, 0, z, nx, ny, 1, GL_RGBA, GL_FLOAT, slice);
	}
}

static void R_Froxel_DebugBuildRandomLights (void)
{
	const float max_dist = q_max (160.f, r_froxel.far_clip * 0.45f);
	const float max_radius = q_max (128.f, r_froxel.far_clip * 0.30f);

	r_froxel_debug.random_light_count = 1 + (rand () % MAX_FROXEL_DEBUG_LIGHTS);
	for (int i = 0; i < r_froxel_debug.random_light_count; ++i)
	{
		froxel_gpu_light_t *out = &r_froxel_debug.random_lights[i];
		vec3_t dir;
		vec3_t origin;

		R_Froxel_DebugRandomUnitVector (dir);
		VectorMA (r_refdef.vieworg, R_Froxel_DebugRandRange (48.f, max_dist), dir, origin);

		out->pos_rad[0] = origin[0];
		out->pos_rad[1] = origin[1];
		out->pos_rad[2] = origin[2];
		out->pos_rad[3] = R_Froxel_DebugRandRange (96.f, max_radius);
		out->color_intensity[0] = R_Froxel_DebugRandRange (0.35f, 1.8f);
		out->color_intensity[1] = R_Froxel_DebugRandRange (0.35f, 1.8f);
		out->color_intensity[2] = R_Froxel_DebugRandRange (0.35f, 1.8f);
		out->color_intensity[3] = R_Froxel_DebugRandRange (0.6f, 2.5f);
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
	if (r_fogvol_stats.value > 0.f || r_fogvol_debug.value >= 8.f)
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
	int mode = CLAMP (0, (int)Q_rint (r_fogvol.value), 2);

	r_froxel.valid = false;
	if (mode <= 0)
		return;

	nx = CLAMP (16, (r_refdef.vrect.width + 15) / 16, 192);
	ny = CLAMP (12, (r_refdef.vrect.height + 15) / 16, 128);
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
	r_froxel.valid = true;
	R_Froxel_DebugRefreshState ();
}

void R_Froxel_InjectDlights (void)
{
	int active_count = 0;
	const dlight_t *const *active = NULL;
	float intensity_scale;

	if (!r_froxel.valid)
		return;
	if (R_Froxel_DebugEnabled ())
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
		return;
	}
	if (r_fogvol_light.value <= 0.f)
		return;
	intensity_scale = q_max (0.f, r_fogvol_dlightscale.value);
	if (intensity_scale <= 0.f)
		return;

	active = DLightPool_GetActiveList (&active_count);
	if (!active || active_count <= 0)
		return;

	for (int i = 0; i < active_count && r_froxel.light_count < MAX_FROXEL_GPU_LIGHTS; ++i)
	{
		const dlight_t *dl = active[i];
		if (!dl || !dl->active)
			continue;
		if (dl->die > 0.f && dl->die < cl.time)
			continue;
		if (dl->radius <= 1.f)
			continue;
		R_Froxel_AddLight (dl->origin, dl->radius, dl->color, intensity_scale, (uint32_t)dl->type);
	}
}

void R_Froxel_EndFrame (void)
{
	float inv_view[16];
	int groups_x, groups_y, groups_z;
	GLuint tmp_tex;

	if (!r_froxel.valid || !r_froxel.light_tex || !r_froxel.history_tex || !r_froxel.light_ssbo || !glprogs.fogvol_froxel_inject)
		return;
	if (!Mat4_Inverse (r_matview, inv_view))
		return;

	R_Froxel_InjectSun ();

	groups_x = (r_froxel.dims[0] + 3) / 4;
	groups_y = (r_froxel.dims[1] + 3) / 4;
	groups_z = (r_froxel.dims[2] + 3) / 4;

	GL_UseProgram (glprogs.fogvol_froxel_inject);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, r_froxel.light_ssbo, 0, sizeof (froxel_gpu_light_t) * MAX_FROXEL_GPU_LIGHTS);
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, sizeof (froxel_gpu_light_t) * MAX_FROXEL_GPU_LIGHTS, r_froxel_gpu_lights, GL_STREAM_DRAW);
	GL_BindImageTextureFunc (0, r_froxel.light_tex, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
	GL_BindNative (GL_TEXTURE1, GL_TEXTURE_3D, r_froxel.history_tex);
	GL_Uniform4fFunc (0, (float)r_froxel.dims[0], (float)r_froxel.dims[1], (float)r_froxel.dims[2], (float)r_froxel.light_count);
	GL_Uniform4fFunc (1, r_froxel.near_clip, r_froxel.far_clip, r_froxel.tan_half_fov_x, r_froxel.tan_half_fov_y);
	GL_Uniform4fFunc (2, r_froxel.log_far_near, 0.f, 0.f, 0.f);
	GL_UniformMatrix4fvFunc (3, 1, GL_FALSE, inv_view);
	GL_Uniform4fFunc (7, 0.f, 1.f, 0.f, 0.f);
	GL_DispatchComputeFunc (groups_x, groups_y, groups_z);
	GL_MemoryBarrierFunc (GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

	tmp_tex = r_froxel.light_tex;
	r_froxel.light_tex = r_froxel.history_tex;
	r_froxel.history_tex = tmp_tex;
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
