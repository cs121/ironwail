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

static froxel_state_t r_froxel;
static froxel_gpu_light_t r_froxel_gpu_lights[MAX_FROXEL_GPU_LIGHTS];

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

	if (!r_froxel.valid || r_fogvol_froxel_sun.value <= 0.f)
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
}

void R_Froxel_InjectDlights (void)
{
	int active_count = 0;
	const dlight_t *const *active = NULL;
	float intensity_scale;

	if (!r_froxel.valid || r_fogvol_light.value <= 0.f)
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
