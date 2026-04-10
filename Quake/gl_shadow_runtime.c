#include "quakedef.h"

#include "gl_shadow.h"
#include "gl_shadow_runtime.h"

#include <float.h>
#include <math.h>

extern cvar_t r_shadow;
extern cvar_t r_shadow_sun;
extern cvar_t r_shadow_dlight;
extern cvar_t r_shadow_dlight_max;
extern cvar_t r_shadow_sun_distance;
extern cvar_t r_shadow_sun_size;
extern cvar_t r_shadow_dlight_size;
extern cvar_t r_shadow_sun_bias;
extern cvar_t r_shadow_dlight_bias;
extern cvar_t r_shadow_sun_pcf;
extern cvar_t r_shadow_dlight_pcf;
extern cvar_t r_shadow_sun_snap;
extern cvar_t r_shadow_sun_split1;
extern cvar_t r_shadow_sun_split2;
extern cvar_t r_shadow_sun_split3;
extern cvar_t r_shadow_sun_cascades;
extern cvar_t r_shadow_sun_split_mode;
extern cvar_t r_shadow_sun_split_lambda;
extern cvar_t r_shadow_mark_mode;
extern cvar_t r_shadow_profile;
extern cvar_t r_shadow_cull_vis;
extern cvar_t r_shadow_cull_backface;
extern cvar_t r_shadow_cull_frustum;
extern cvar_t r_shadow_cull_sphere;
extern cvar_t r_shadow_receiver_bias;
extern cvar_t r_shadow_debug;
extern cvar_t r_shadow_log;
extern cvar_t r_rimlight;
extern cvar_t r_rimlight_world;
extern cvar_t r_rimlight_models;
extern cvar_t r_rimlight_intensity;
extern cvar_t r_rimlight_power;
extern cvar_t r_rimlight_sun;
extern cvar_t r_rimlight_dlight;
extern cvar_t r_rimlight_shadow;
extern cvar_t r_drawworld;

extern cvar_t r_sun_light;
extern qboolean R_WorldHasSun (void);
extern int r_visframecount;
extern int r_framecount;
extern mleaf_t *r_viewleaf;
extern qboolean r_drawworld_cheatsafe;

typedef struct shadow_draw_context_s
{
	entity_t	**brush_ents;
	int		brush_count;
	entity_t	**alias_ents;
	int		alias_count;
} shadow_draw_context_t;

typedef struct shadow_draw_cache_s
{
	qboolean	valid;
	int		visframecount;
	int		numshadowedicts;
	qboolean	include_world;
	qmodel_t	*worldmodel;
	mleaf_t		*viewleaf;
	entity_t	*first_shadow_ent;
	entity_t	*last_shadow_ent;
	int		brush_count;
	int		alias_count;
	entity_t	*brush_ents[MAX_VISEDICTS + 1];
	entity_t	*alias_ents[MAX_VISEDICTS];
} shadow_draw_cache_t;

typedef struct shadow_profile_stats_s
{
	double	cpu_sun_ms;
	double	cpu_dlight_ms;
	double	cpu_total_ms;
	double	gpu_sun_ms;
	double	gpu_dlight_ms;
} shadow_profile_stats_t;

typedef struct shadow_receiver_uniforms_s
{
	GLuint	program;
	GLint	sun_viewproj;
	GLint	sun_splits;
	GLint	sun_cascade_count;
	GLint	enables_debug;
	GLint	dlight_indices;
	GLint	bias_counts;
	GLint	pcf_texel;
	GLint	rim_params0;
	GLint	rim_params1;
} shadow_receiver_uniforms_t;

typedef struct shadow_caster_uniforms_s
{
	GLuint	program;
	GLint	viewproj;
	GLint	lightpos_far;
	GLint	mode;
} shadow_caster_uniforms_t;

static const float r_shadow_identity_mat4[16] = {
	1.f, 0.f, 0.f, 0.f,
	0.f, 1.f, 0.f, 0.f,
	0.f, 0.f, 1.f, 0.f,
	0.f, 0.f, 0.f, 1.f
};

static shadow_runtime_t r_shadow_state;
static shadow_draw_context_t r_shadow_draw_ctx;
static shadow_draw_cache_t r_shadow_draw_cache;
static shadow_profile_stats_t r_shadow_profile_stats;
static shadow_receiver_uniforms_t r_shadow_receiver_uniforms[16];
static int r_shadow_receiver_uniforms_count = 0;
static shadow_caster_uniforms_t r_shadow_caster_uniforms[8];
static int r_shadow_caster_uniforms_count = 0;
static entity_t *r_shadow_brush_ents[MAX_VISEDICTS + 1];
static entity_t *r_shadow_alias_ents[MAX_VISEDICTS];
static entity_t *r_shadow_filtered_brush_ents[MAX_VISEDICTS + 1];
static entity_t *r_shadow_filtered_alias_ents[MAX_VISEDICTS];
static int r_shadow_log_last_frame = -1024;
static int r_shadow_profile_last_frame = -1024;

static void R_Shadow_BuildSplitRatios (int cascade_count, float sun_dist, float out_ratio[SHADOW_SUN_CASCADE_MAX])
{
	int mode = CLAMP (0, (int)Q_rint (r_shadow_sun_split_mode.value), 3);
	float lambda = CLAMP (0.f, r_shadow_sun_split_lambda.value, 1.f);
	int i;

	for (i = 0; i < SHADOW_SUN_CASCADE_MAX; ++i)
		out_ratio[i] = 1.f;

	if (mode == 0)
	{
		out_ratio[0] = CLAMP (0.05f, r_shadow_sun_split1.value, 0.95f);
		out_ratio[1] = CLAMP (out_ratio[0] + 0.02f, r_shadow_sun_split2.value, 0.97f);
		out_ratio[2] = CLAMP (out_ratio[1] + 0.02f, r_shadow_sun_split3.value, 0.99f);
		out_ratio[3] = 1.f;
		return;
	}

	for (i = 0; i < cascade_count; ++i)
	{
		float u = (float)(i + 1) / (float)cascade_count;
		float uniform_ratio = u;
		float log_ratio = 1.f;

		if (sun_dist > 1.f)
			log_ratio = powf (sun_dist, u - 1.f);

		if (mode == 1)
			out_ratio[i] = uniform_ratio;
		else if (mode == 2)
			out_ratio[i] = log_ratio;
		else
			out_ratio[i] = uniform_ratio * (1.f - lambda) + log_ratio * lambda;

		out_ratio[i] = CLAMP (0.01f, out_ratio[i], 1.f);
	}

	for (i = 1; i < cascade_count; ++i)
		out_ratio[i] = q_max (out_ratio[i], out_ratio[i - 1] + 0.02f);
	out_ratio[cascade_count - 1] = 1.f;
}

qboolean R_Shadow_Enabled (void)
{
	return r_shadow.value > 0.f;
}

qboolean R_Shadow_SunEnabled (void)
{
	return R_Shadow_Enabled () && r_shadow_sun.value > 0.f && R_WorldHasSun () && r_sun_light.value > 0.f;
}

qboolean R_Shadow_DlightEnabled (void)
{
	return R_Shadow_Enabled () && r_shadow_dlight.value > 0.f && r_framedata.numlights > 0;
}

void R_Shadow_NormalizeSettings (void)
{
	int ivalue;
	float fvalue;

	ivalue = (r_shadow_mark_mode.value > 0.f) ? 1 : 0;
	if (ivalue != (int)r_shadow_mark_mode.value)
		Cvar_SetValueQuick (&r_shadow_mark_mode, (float)ivalue);
	ivalue = (r_shadow_profile.value > 0.f) ? 1 : 0;
	if (ivalue != (int)r_shadow_profile.value)
		Cvar_SetValueQuick (&r_shadow_profile, (float)ivalue);
	ivalue = (r_shadow_cull_vis.value > 0.f) ? 1 : 0;
	if (ivalue != (int)r_shadow_cull_vis.value)
		Cvar_SetValueQuick (&r_shadow_cull_vis, (float)ivalue);
	ivalue = (r_shadow_cull_backface.value > 0.f) ? 1 : 0;
	if (ivalue != (int)r_shadow_cull_backface.value)
		Cvar_SetValueQuick (&r_shadow_cull_backface, (float)ivalue);
	ivalue = (r_shadow_cull_frustum.value > 0.f) ? 1 : 0;
	if (ivalue != (int)r_shadow_cull_frustum.value)
		Cvar_SetValueQuick (&r_shadow_cull_frustum, (float)ivalue);
	ivalue = (r_shadow_cull_sphere.value > 0.f) ? 1 : 0;
	if (ivalue != (int)r_shadow_cull_sphere.value)
		Cvar_SetValueQuick (&r_shadow_cull_sphere, (float)ivalue);

	fvalue = CLAMP (0.f, r_shadow_receiver_bias.value, 16.f);
	if (fvalue != r_shadow_receiver_bias.value)
		Cvar_SetValueQuick (&r_shadow_receiver_bias, fvalue);
	ivalue = CLAMP (1, (int)Q_rint (r_shadow_sun_cascades.value), SHADOW_SUN_CASCADE_MAX);
	if (ivalue != (int)r_shadow_sun_cascades.value)
		Cvar_SetValueQuick (&r_shadow_sun_cascades, (float)ivalue);
	ivalue = CLAMP (0, (int)Q_rint (r_shadow_sun_split_mode.value), 3);
	if (ivalue != (int)r_shadow_sun_split_mode.value)
		Cvar_SetValueQuick (&r_shadow_sun_split_mode, (float)ivalue);
	fvalue = CLAMP (0.f, r_shadow_sun_split_lambda.value, 1.f);
	if (fvalue != r_shadow_sun_split_lambda.value)
		Cvar_SetValueQuick (&r_shadow_sun_split_lambda, fvalue);

	fvalue = CLAMP (0.05f, r_shadow_sun_split1.value, 0.95f);
	if (fvalue != r_shadow_sun_split1.value)
		Cvar_SetValueQuick (&r_shadow_sun_split1, fvalue);
	fvalue = CLAMP (r_shadow_sun_split1.value + 0.05f, r_shadow_sun_split2.value, 0.97f);
	if (fvalue != r_shadow_sun_split2.value)
		Cvar_SetValueQuick (&r_shadow_sun_split2, fvalue);
	fvalue = CLAMP (r_shadow_sun_split2.value + 0.05f, r_shadow_sun_split3.value, 0.99f);
	if (fvalue != r_shadow_sun_split3.value)
		Cvar_SetValueQuick (&r_shadow_sun_split3, fvalue);
}

void R_Shadow_ResetRuntime (shadow_runtime_t *state)
{
	int i, f, c;

	if (!state)
		return;

	state->valid = false;
	state->sun_cascade_count = 1;
	state->num_dlights = 0;
	for (c = 0; c < SHADOW_SUN_CASCADE_MAX; ++c)
	{
		state->sun_split_dist[c] = 0.f;
		R_Shadow_MatrixIdentity (state->sun_viewproj[c]);
		VectorClear (state->sun_eye[c]);
		memset (state->sun_frustum[c], 0, sizeof (state->sun_frustum[c]));
	}
	for (i = 0; i < SHADOW_DLIGHT_MAX; ++i)
	{
		state->dlight_indices[i] = -1;
		state->dlight_sources[i] = NULL;
		for (f = 0; f < SHADOW_DLIGHT_FACES; ++f)
			memset (state->dlight_frustum[i][f], 0, sizeof (state->dlight_frustum[i][f]));
	}
}

void R_Shadow_SelectDlights (shadow_runtime_t *state)
{
	int i, slot;
	int limit = CLAMP (0, (int)Q_rint (r_shadow_dlight_max.value), SHADOW_DLIGHT_MAX);
	float best_score[SHADOW_DLIGHT_MAX];
	const gpulight_t *lights = r_lightbuffer.lights;
	dlight_t *const *sources = r_dlight_sources;
	int light_count = (int)r_framedata.numlights;

	if (!state)
		return;

	state->num_dlights = 0;
	for (slot = 0; slot < SHADOW_DLIGHT_MAX; ++slot)
	{
		state->dlight_indices[slot] = -1;
		state->dlight_sources[slot] = NULL;
		VectorSet (state->dlight_pos_radius[slot], 0.f, 0.f, 0.f);
		state->dlight_pos_radius[slot][3] = 1.f;
		best_score[slot] = -FLT_MAX;
	}

	if (!R_Shadow_DlightEnabled () || limit <= 0)
		return;

	for (i = 0; i < light_count; ++i)
	{
		const gpulight_t *l = &lights[i];
		vec3_t to_light;
		float dist, luminance, score;

		/* Keep shadow-slot ownership on concrete dlight sources only. */
		if (!sources[i])
			continue;

		VectorSubtract (l->pos, r_refdef.vieworg, to_light);
		dist = VectorLength (to_light);
		luminance = l->color[0] * 0.299f + l->color[1] * 0.587f + l->color[2] * 0.114f;
		score = l->radius * (0.35f + luminance) / (1.f + dist * 0.0025f);

		for (slot = 0; slot < limit; ++slot)
		{
			if (score > best_score[slot] || (score == best_score[slot] && i < state->dlight_indices[slot]))
			{
				int k;
				for (k = limit - 1; k > slot; --k)
				{
					best_score[k] = best_score[k - 1];
					state->dlight_indices[k] = state->dlight_indices[k - 1];
				}
				best_score[slot] = score;
				state->dlight_indices[slot] = i;
				break;
			}
		}
	}

	for (slot = 0; slot < limit; ++slot)
	{
		int idx = state->dlight_indices[slot];
		if (idx < 0 || idx >= light_count)
			continue;
		VectorCopy (lights[idx].pos, state->dlight_pos_radius[slot]);
		state->dlight_pos_radius[slot][3] = q_max (lights[idx].radius, 16.f);
		state->dlight_sources[slot] = sources[idx];
		state->num_dlights++;
	}
}

void R_Shadow_ClearProgramUniformCaches (void)
{
	r_shadow_receiver_uniforms_count = 0;
	r_shadow_caster_uniforms_count = 0;
	memset (r_shadow_receiver_uniforms, 0, sizeof (r_shadow_receiver_uniforms));
	memset (r_shadow_caster_uniforms, 0, sizeof (r_shadow_caster_uniforms));
}

void R_Shadow_UpdateSunMatrix (shadow_runtime_t *state)
{
	const sun_t *sun = R_GetSun ();
	float sun_dist = q_max (128.f, r_shadow_sun_distance.value);
	float znear = 1.f;
	float zfar = sun_dist * 2.5f;
	float split_ratio[SHADOW_SUN_CASCADE_MAX] = { 1.f, 1.f, 1.f, 1.f };
	int cascade_count = CLAMP (1, (int)Q_rint (r_shadow_sun_cascades.value), SHADOW_SUN_CASCADE_MAX);
	int c;
	vec3_t forward, up;
	float view[16], proj[16];
	float center_ls[4];
	float texel = 0.f;

	if (!state || !sun)
		return;

	VectorScale (sun->dir, -1.f, forward);
	VectorSet (up, 0.f, 0.f, 1.f);
	R_Shadow_BuildSplitRatios (cascade_count, sun_dist, split_ratio);
	state->sun_cascade_count = cascade_count;

	for (c = 0; c < cascade_count; ++c)
	{
		const float near_dist = (c == 0) ? 1.f : (sun_dist * split_ratio[c - 1]);
		const float far_dist = sun_dist * split_ratio[c];
		const float mid_dist = (near_dist + far_dist) * 0.5f;
		const float radius = q_max (64.f, far_dist * 0.75f);
		vec3_t center, eye;

		VectorMA (r_refdef.vieworg, mid_dist, vpn, center);
		VectorMA (center, -sun_dist, forward, eye);
		VectorCopy (eye, state->sun_eye[c]);

		R_Shadow_MatrixLook (view, eye, forward, up);
		R_Shadow_MatrixOrtho (proj, -radius, radius, -radius, radius, znear, zfar);

		if (r_shadow_sun_snap.value > 0.f && framebufs.shadow.sun_size > 0)
		{
			texel = (radius * 2.f) / q_max (1.f, (float)framebufs.shadow.sun_size);
			center_ls[0] = view[0] * center[0] + view[4] * center[1] + view[8] * center[2] + view[12];
			center_ls[1] = view[1] * center[0] + view[5] * center[1] + view[9] * center[2] + view[13];
			center_ls[2] = view[2] * center[0] + view[6] * center[1] + view[10] * center[2] + view[14];
			center_ls[0] = floorf (center_ls[0] / texel + 0.5f) * texel;
			center_ls[1] = floorf (center_ls[1] / texel + 0.5f) * texel;
			view[12] += center_ls[0] - (view[0] * center[0] + view[4] * center[1] + view[8] * center[2] + view[12]);
			view[13] += center_ls[1] - (view[1] * center[0] + view[5] * center[1] + view[9] * center[2] + view[13]);
		}

		R_Shadow_MatrixMultiply (state->sun_viewproj[c], proj, view);
		R_Shadow_ExtractFrustum (state->sun_viewproj[c], state->sun_frustum[c]);
		state->sun_split_dist[c] = far_dist;
	}

	if (r_shadow_log.value > 1.f && (r_framecount % 60) == 0)
	{
		Con_Printf ("Shadow sun cascades: count=%d mode=%d lambda=%.2f dist=%.1f splits=(%.2f %.2f %.2f)\n",
			cascade_count,
			CLAMP (0, (int)Q_rint (r_shadow_sun_split_mode.value), 3),
			CLAMP (0.f, r_shadow_sun_split_lambda.value, 1.f),
			sun_dist,
			split_ratio[0], split_ratio[1], split_ratio[2]);
	}
}

void R_Shadow_UpdateDlightMatrices (shadow_runtime_t *state)
{
	static const vec3_t face_dirs[SHADOW_DLIGHT_FACES] = {
		{ 1.f, 0.f, 0.f }, { -1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f },
		{ 0.f, -1.f, 0.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, -1.f }
	};
	static const vec3_t face_ups[SHADOW_DLIGHT_FACES] = {
		{ 0.f, -1.f, 0.f }, { 0.f, -1.f, 0.f }, { 0.f, 0.f, 1.f },
		{ 0.f, 0.f, -1.f }, { 0.f, -1.f, 0.f }, { 0.f, -1.f, 0.f }
	};
	int slot, face;

	if (!state)
		return;

	for (slot = 0; slot < state->num_dlights; ++slot)
	{
		const vec4_t *light = &state->dlight_pos_radius[slot];
		float zfar = q_max ((*light)[3], 16.f);
		float znear = q_max (1.f, zfar * 0.02f);
		float proj[16], view[16];

		R_Shadow_MatrixPerspective (proj, 90.f, 1.f, znear, zfar);
		for (face = 0; face < SHADOW_DLIGHT_FACES; ++face)
		{
			R_Shadow_MatrixLook (view, *light, face_dirs[face], face_ups[face]);
			R_Shadow_MatrixMultiply (state->dlight_viewproj[slot][face], proj, view);
			R_Shadow_ExtractFrustum (state->dlight_viewproj[slot][face], state->dlight_frustum[slot][face]);
		}
	}
}

void R_Shadow_SetCasterState (shadow_runtime_t *state, const float viewproj[16], qboolean dlight, const vec3_t lightpos, float far_plane)
{
	if (!state || !viewproj || !lightpos)
		return;

	memcpy (state->caster_viewproj, viewproj, sizeof (state->caster_viewproj));
	VectorCopy (lightpos, state->caster_lightpos_far_mode);
	state->caster_lightpos_far_mode[3] = q_max (far_plane, 1.f);
	state->caster_mode = dlight ? 1 : 0;
}

static shadow_receiver_uniforms_t *R_Shadow_GetReceiverUniforms (GLuint program)
{
	int i;

	for (i = 0; i < r_shadow_receiver_uniforms_count; ++i)
	{
		if (r_shadow_receiver_uniforms[i].program == program)
			return &r_shadow_receiver_uniforms[i];
	}

	if (r_shadow_receiver_uniforms_count >= (int)countof (r_shadow_receiver_uniforms))
	{
		r_shadow_receiver_uniforms_count = 0;
		memset (r_shadow_receiver_uniforms, 0, sizeof (r_shadow_receiver_uniforms));
	}

	{
		shadow_receiver_uniforms_t *u = &r_shadow_receiver_uniforms[r_shadow_receiver_uniforms_count++];
		u->program = program;
		u->sun_viewproj = GL_GetUniformLocationFunc (program, "ShadowSunViewProj[0]");
		u->sun_splits = GL_GetUniformLocationFunc (program, "ShadowSunSplits");
		u->sun_cascade_count = GL_GetUniformLocationFunc (program, "ShadowSunCascadeCount");
		u->enables_debug = GL_GetUniformLocationFunc (program, "ShadowEnableDebug");
		u->dlight_indices = GL_GetUniformLocationFunc (program, "ShadowDLightIndices");
		u->bias_counts = GL_GetUniformLocationFunc (program, "ShadowBiasCounts");
		u->pcf_texel = GL_GetUniformLocationFunc (program, "ShadowPCFTexel");
		u->rim_params0 = GL_GetUniformLocationFunc (program, "RimLightParams0");
		u->rim_params1 = GL_GetUniformLocationFunc (program, "RimLightParams1");
		/* Rim uniforms use explicit locations in GLSL (45/46). On some drivers,
		 * querying explicit uniform locations may spuriously return -1, which
		 * leaves rim light permanently disabled. Fall back to the fixed slots. */
		if (u->rim_params0 < 0)
			u->rim_params0 = 45;
		if (u->rim_params1 < 0)
			u->rim_params1 = 46;
		return u;
	}
}

static shadow_caster_uniforms_t *R_Shadow_GetCasterUniforms (GLuint program)
{
	int i;

	for (i = 0; i < r_shadow_caster_uniforms_count; ++i)
	{
		if (r_shadow_caster_uniforms[i].program == program)
			return &r_shadow_caster_uniforms[i];
	}

	if (r_shadow_caster_uniforms_count >= (int)countof (r_shadow_caster_uniforms))
	{
		r_shadow_caster_uniforms_count = 0;
		memset (r_shadow_caster_uniforms, 0, sizeof (r_shadow_caster_uniforms));
	}

	{
		shadow_caster_uniforms_t *u = &r_shadow_caster_uniforms[r_shadow_caster_uniforms_count++];
		u->program = program;
		u->viewproj = GL_GetUniformLocationFunc (program, "ShadowViewProj");
		u->lightpos_far = GL_GetUniformLocationFunc (program, "ShadowLightPosFar");
		u->mode = GL_GetUniformLocationFunc (program, "ShadowMode");
		return u;
	}
}

void R_Shadow_CreateFrameBuffers (void)
{
	GLenum status;
	GLfloat border[] = { 1.f, 1.f, 1.f, 1.f };
	int sun_size = R_Shadow_ClampMapSize (r_shadow_sun_size.value);
	int dlight_size = R_Shadow_ClampMapSize (r_shadow_dlight_size.value);

	framebufs.shadow.sun_depth_tex = 0;
	framebufs.shadow.sun_fbo = 0;
	framebufs.shadow.dlight_depth_tex = 0;
	framebufs.shadow.dlight_fbo = 0;
	framebufs.shadow.available = false;
	framebufs.shadow.sun_size = sun_size;
	framebufs.shadow.dlight_size = dlight_size;

	if (!R_Shadow_Enabled ())
		return;

	if (!GL_TexStorage3DFunc || !GL_FramebufferTextureLayerFunc)
	{
		Con_Warning ("GL texture-array shadow APIs unavailable, disabling shadow maps.\n");
		return;
	}

	glGenTextures (1, &framebufs.shadow.sun_depth_tex);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D_ARRAY, framebufs.shadow.sun_depth_tex);
	GL_ObjectLabelFunc (GL_TEXTURE, framebufs.shadow.sun_depth_tex, -1, "shadow sun depth");
	GL_TexStorage3DFunc (GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH_COMPONENT24, sun_size, sun_size, SHADOW_SUN_CASCADE_MAX);
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	glTexParameterfv (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border);
	glTexParameteri (GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);

	GL_GenFramebuffersFunc (1, &framebufs.shadow.sun_fbo);
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.shadow.sun_fbo);
	GL_ObjectLabelFunc (GL_FRAMEBUFFER, framebufs.shadow.sun_fbo, -1, "shadow sun fbo");
	GL_FramebufferTextureLayerFunc (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, framebufs.shadow.sun_depth_tex, 0, 0);
	glDrawBuffer (GL_NONE);
	glReadBuffer (GL_NONE);
	status = GL_CheckFramebufferStatusFunc (GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		Con_Warning ("Shadow sun framebuffer incomplete (0x%X), disabling sun+dlight shadows.\n", status);
		GL_DeleteFramebuffersFunc (1, &framebufs.shadow.sun_fbo);
		GL_DeleteNativeTexture (framebufs.shadow.sun_depth_tex);
		framebufs.shadow.sun_fbo = 0;
		framebufs.shadow.sun_depth_tex = 0;
		return;
	}
	if (r_shadow_log.value > 0.f)
		Con_Printf ("Shadow: sun depth array created (%dx%d, cascades=%d)\n",
			sun_size, sun_size, SHADOW_SUN_CASCADE_MAX);

	glGenTextures (1, &framebufs.shadow.dlight_depth_tex);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_CUBE_MAP_ARRAY, framebufs.shadow.dlight_depth_tex);
	GL_ObjectLabelFunc (GL_TEXTURE, framebufs.shadow.dlight_depth_tex, -1, "shadow dlight depth cubearray");
	GL_TexStorage3DFunc (GL_TEXTURE_CUBE_MAP_ARRAY, 1, GL_DEPTH_COMPONENT24, dlight_size, dlight_size, SHADOW_DLIGHT_MAX * SHADOW_DLIGHT_FACES);
	glTexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	glTexParameteri (GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);

	GL_GenFramebuffersFunc (1, &framebufs.shadow.dlight_fbo);
	GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.shadow.dlight_fbo);
	GL_ObjectLabelFunc (GL_FRAMEBUFFER, framebufs.shadow.dlight_fbo, -1, "shadow dlight fbo");
	GL_FramebufferTextureLayerFunc (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, framebufs.shadow.dlight_depth_tex, 0, 0);
	glDrawBuffer (GL_NONE);
	glReadBuffer (GL_NONE);
	status = GL_CheckFramebufferStatusFunc (GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		Con_Warning ("Shadow dlight framebuffer incomplete (0x%X), disabling dlight shadows (sun shadows stay enabled).\n", status);
		framebufs.shadow.dlight_fbo = 0;
		GL_DeleteNativeTexture (framebufs.shadow.dlight_depth_tex);
		framebufs.shadow.dlight_depth_tex = 0;
		framebufs.shadow.available = true;
		return;
	}
	if (r_shadow_log.value > 0.f)
		Con_Printf ("Shadow: dlight cube-array atlas created (%dx%d, layers=%d)\n",
			dlight_size, dlight_size, SHADOW_DLIGHT_MAX * SHADOW_DLIGHT_FACES);

	framebufs.shadow.available = true;
}

void R_Shadow_DeleteFrameBuffers (void)
{
	GL_DeleteFramebuffersFunc (1, &framebufs.shadow.sun_fbo);
	GL_DeleteFramebuffersFunc (1, &framebufs.shadow.dlight_fbo);
	GL_DeleteNativeTexture (framebufs.shadow.sun_depth_tex);
	GL_DeleteNativeTexture (framebufs.shadow.dlight_depth_tex);
	framebufs.shadow.sun_depth_tex = 0;
	framebufs.shadow.sun_fbo = 0;
	framebufs.shadow.dlight_depth_tex = 0;
	framebufs.shadow.dlight_fbo = 0;
	framebufs.shadow.available = false;
	r_shadow_draw_cache.valid = false;
}

void R_Shadow_ReconcileResources (void)
{
	static int retry_after_frame = 0;
	const int sun_size = R_Shadow_ClampMapSize (r_shadow_sun_size.value);
	const int dlight_size = R_Shadow_ClampMapSize (r_shadow_dlight_size.value);
	const qboolean want_shadows = R_Shadow_Enabled ();
	const qboolean have_any_resources =
		framebufs.shadow.available
		|| framebufs.shadow.sun_depth_tex != 0
		|| framebufs.shadow.sun_fbo != 0
		|| framebufs.shadow.dlight_depth_tex != 0
		|| framebufs.shadow.dlight_fbo != 0;

	if (!want_shadows)
	{
		if (have_any_resources)
			R_Shadow_DeleteFrameBuffers ();
		framebufs.shadow.sun_size = sun_size;
		framebufs.shadow.dlight_size = dlight_size;
		retry_after_frame = 0;
		return;
	}

	if (framebufs.shadow.available
		&& framebufs.shadow.sun_size == sun_size
		&& framebufs.shadow.dlight_size == dlight_size)
	{
		return;
	}

	if (have_any_resources)
		R_Shadow_DeleteFrameBuffers ();

	if (r_framecount < retry_after_frame)
		return;

	R_Shadow_CreateFrameBuffers ();
	if (!framebufs.shadow.available)
		retry_after_frame = r_framecount + 300;
	else
		retry_after_frame = 0;
}

static int R_Shadow_FindCurrentIndexForSlot (const dlight_t *source)
{
	int i;
	int light_count = (int)r_framedata.numlights;

	if (!source)
		return -1;

	for (i = 0; i < light_count; ++i)
	{
		if (r_dlight_sources[i] == source)
			return i;
	}

	return -1;
}

static void R_Shadow_GetSelectedIndices (int outidx[SHADOW_DLIGHT_MAX])
{
	int i;

	for (i = 0; i < SHADOW_DLIGHT_MAX; ++i)
	{
		if (i >= r_shadow_state.num_dlights)
		{
			outidx[i] = -1;
			continue;
		}

		outidx[i] = R_Shadow_FindCurrentIndexForSlot (r_shadow_state.dlight_sources[i]);
	}
}

qboolean R_Shadow_GetSunOcclusionData (float out_viewproj[16], float *out_bias, float *out_pcf_uv)
{
	qboolean enabled = (framebufs.shadow.available && R_Shadow_Enabled () && r_shadow_state.valid
		&& R_Shadow_SunEnabled () && framebufs.shadow.sun_depth_tex);

	if (out_viewproj)
	{
		if (enabled)
			memcpy (out_viewproj, r_shadow_state.sun_viewproj[0], sizeof (r_shadow_state.sun_viewproj[0]));
		else
			memcpy (out_viewproj, r_shadow_identity_mat4, sizeof (r_shadow_identity_mat4));
	}
	if (out_bias)
		*out_bias = enabled ? q_max (0.f, r_shadow_sun_bias.value) : 0.f;
	if (out_pcf_uv)
	{
		float texel = (enabled && framebufs.shadow.sun_size > 0)
			? (1.f / (float)framebufs.shadow.sun_size)
			: 0.f;
		*out_pcf_uv = enabled ? (q_max (0.f, r_shadow_sun_pcf.value) * texel) : 0.f;
	}

	return enabled;
}

qboolean R_Shadow_GetSunCascadeData (float out_viewproj[4][16], float out_splits[4], int *out_count, float *out_bias, float *out_pcf_uv)
{
	int i;
	qboolean enabled = (framebufs.shadow.available && R_Shadow_Enabled () && r_shadow_state.valid
		&& R_Shadow_SunEnabled () && framebufs.shadow.sun_depth_tex);

	if (out_viewproj)
	{
		for (i = 0; i < 4; ++i)
		{
			if (enabled && i < r_shadow_state.sun_cascade_count)
				memcpy (out_viewproj[i], r_shadow_state.sun_viewproj[i], sizeof (r_shadow_state.sun_viewproj[i]));
			else
				memcpy (out_viewproj[i], r_shadow_identity_mat4, sizeof (r_shadow_identity_mat4));
		}
	}
	if (out_splits)
	{
		for (i = 0; i < 4; ++i)
			out_splits[i] = (enabled && i < r_shadow_state.sun_cascade_count) ? r_shadow_state.sun_split_dist[i] : 0.f;
	}
	if (out_count)
		*out_count = enabled ? r_shadow_state.sun_cascade_count : 0;
	if (out_bias)
		*out_bias = enabled ? q_max (0.f, r_shadow_sun_bias.value) : 0.f;
	if (out_pcf_uv)
	{
		float texel = (enabled && framebufs.shadow.sun_size > 0)
			? (1.f / (float)framebufs.shadow.sun_size)
			: 0.f;
		*out_pcf_uv = enabled ? (q_max (0.f, r_shadow_sun_pcf.value) * texel) : 0.f;
	}

	return enabled;
}

void R_Shadow_ApplyWorldReceiverUniforms (GLuint program)
{
	int idx[SHADOW_DLIGHT_MAX];
	float enabled, sun_enabled, dlight_enabled;
	float rim_master, rim_world, rim_models;
	float rim_intensity, rim_power, rim_sun, rim_dlight, rim_shadow;
	GLuint suntex = 0;
	GLuint dlighttex = 0;
	shadow_receiver_uniforms_t *u = R_Shadow_GetReceiverUniforms (program);
	if (!u)
		return;

	enabled = (framebufs.shadow.available && R_Shadow_Enabled () && r_shadow_state.valid) ? 1.f : 0.f;
	sun_enabled = (enabled > 0.f && R_Shadow_SunEnabled () && framebufs.shadow.sun_depth_tex) ? 1.f : 0.f;
	dlight_enabled = (enabled > 0.f && r_shadow_state.num_dlights > 0 && framebufs.shadow.dlight_depth_tex) ? 1.f : 0.f;
	R_Shadow_GetSelectedIndices (idx);

	if (sun_enabled > 0.f)
		suntex = framebufs.shadow.sun_depth_tex;
	if (dlight_enabled > 0.f)
		dlighttex = framebufs.shadow.dlight_depth_tex;

	GL_BindNative (GL_TEXTURE7, GL_TEXTURE_2D_ARRAY, suntex);
	GL_BindNative (GL_TEXTURE8, GL_TEXTURE_CUBE_MAP_ARRAY, dlighttex);

	if (u->sun_viewproj >= 0)
		GL_UniformMatrix4fvFunc (u->sun_viewproj, r_shadow_state.sun_cascade_count, GL_FALSE, &r_shadow_state.sun_viewproj[0][0]);
	if (u->sun_splits >= 0)
		GL_Uniform4fFunc (u->sun_splits, r_shadow_state.sun_split_dist[0], r_shadow_state.sun_split_dist[1], r_shadow_state.sun_split_dist[2], r_shadow_state.sun_split_dist[3]);
	if (u->sun_cascade_count >= 0)
		GL_Uniform1iFunc (u->sun_cascade_count, r_shadow_state.sun_cascade_count);
	if (u->enables_debug >= 0)
		GL_Uniform4fFunc (u->enables_debug, enabled, sun_enabled, dlight_enabled, CLAMP (0.f, r_shadow_debug.value, 5.f));
	if (u->dlight_indices >= 0)
		GL_Uniform4fFunc (u->dlight_indices, (float)idx[0], (float)idx[1], (float)idx[2], (float)idx[3]);
	if (u->bias_counts >= 0)
		GL_Uniform4fFunc (u->bias_counts, (float)r_shadow_state.num_dlights,
			q_max (0.f, r_shadow_sun_bias.value),
			q_max (0.f, r_shadow_dlight_bias.value),
			q_max (0.f, r_shadow_receiver_bias.value));
	if (u->pcf_texel >= 0)
		GL_Uniform4fFunc (u->pcf_texel,
			q_max (0.f, r_shadow_sun_pcf.value),
			q_max (0.f, r_shadow_dlight_pcf.value),
			framebufs.shadow.sun_size > 0 ? 1.f / (float)framebufs.shadow.sun_size : 0.f,
			framebufs.shadow.dlight_size > 0 ? 1.f / (float)framebufs.shadow.dlight_size : 0.f);
	rim_master = (r_rimlight.value > 0.f) ? 1.f : 0.f;
	rim_world = (r_rimlight_world.value > 0.f) ? 1.f : 0.f;
	rim_models = (r_rimlight_models.value > 0.f) ? 1.f : 0.f;
	rim_intensity = q_max (0.f, r_rimlight_intensity.value);
	rim_power = q_max (0.5f, r_rimlight_power.value);
	rim_sun = q_max (0.f, r_rimlight_sun.value);
	rim_dlight = q_max (0.f, r_rimlight_dlight.value);
	rim_shadow = (r_rimlight_shadow.value > 0.f) ? 1.f : 0.f;
	if (u->rim_params0 >= 0)
		GL_Uniform4fFunc (u->rim_params0, rim_master, rim_intensity, rim_power, rim_shadow);
	if (u->rim_params1 >= 0)
		GL_Uniform4fFunc (u->rim_params1, rim_world, rim_models, rim_sun, rim_dlight);
}

void R_Shadow_ApplyAliasReceiverUniforms (GLuint program)
{
	R_Shadow_ApplyWorldReceiverUniforms (program);
	GL_Uniform4fFunc (38, r_framedata.sun_dir_enabled[0], r_framedata.sun_dir_enabled[1], r_framedata.sun_dir_enabled[2], r_framedata.sun_dir_enabled[3]);
	GL_Uniform4fFunc (39, r_framedata.sun_color_intensity[0], r_framedata.sun_color_intensity[1], r_framedata.sun_color_intensity[2], r_framedata.sun_color_intensity[3]);
	GL_Uniform1fFunc (40, r_framedata.shader_params[2]);
	GL_Uniform1fFunc (41, (float)r_framedata.numlights);
	GL_Uniform4fFunc (42, 1.f, 1.f, 3.f, 2.2f);
	GL_Uniform4fFunc (43, 0.75f, 6.f, 1.5f, 0.2f);
	GL_Uniform4fFunc (44, 0.1f, 0.f, 0.f, 0.f);
}

void R_Shadow_ApplyWorldCasterUniforms (GLuint program)
{
	shadow_caster_uniforms_t *u = R_Shadow_GetCasterUniforms (program);
	if (!u)
		return;

	if (u->viewproj >= 0)
		GL_UniformMatrix4fvFunc (u->viewproj, 1, GL_FALSE, r_shadow_state.caster_viewproj);
	if (u->lightpos_far >= 0)
		GL_Uniform4fFunc (u->lightpos_far,
			r_shadow_state.caster_lightpos_far_mode[0],
			r_shadow_state.caster_lightpos_far_mode[1],
			r_shadow_state.caster_lightpos_far_mode[2],
			r_shadow_state.caster_lightpos_far_mode[3]);
	if (u->mode >= 0)
		GL_Uniform1iFunc (u->mode, r_shadow_state.caster_mode);
}

void R_Shadow_ApplyAliasCasterUniforms (GLuint program)
{
	R_Shadow_ApplyWorldCasterUniforms (program);
}

static void R_Shadow_ClearDrawContext (void)
{
	memset (&r_shadow_draw_ctx, 0, sizeof (r_shadow_draw_ctx));
}

static qboolean R_Shadow_CanReuseDrawCache (qboolean include_world, entity_t **shadow_visedicts, int numshadowedicts)
{
	if (!r_shadow_draw_cache.valid)
		return false;
	if (r_shadow_draw_cache.visframecount != r_visframecount)
		return false;
	if (r_shadow_draw_cache.numshadowedicts != numshadowedicts)
		return false;
	if (r_shadow_draw_cache.include_world != include_world)
		return false;
	if (r_shadow_draw_cache.worldmodel != cl.worldmodel)
		return false;
	if (r_shadow_draw_cache.viewleaf != r_viewleaf)
		return false;
	if (numshadowedicts > 0)
	{
		if (r_shadow_draw_cache.first_shadow_ent != shadow_visedicts[0])
			return false;
		if (r_shadow_draw_cache.last_shadow_ent != shadow_visedicts[numshadowedicts - 1])
			return false;
	}
	else if (r_shadow_draw_cache.first_shadow_ent || r_shadow_draw_cache.last_shadow_ent)
	{
		return false;
	}

	return true;
}

static void R_Shadow_LoadDrawCache (void)
{
	r_shadow_draw_ctx.brush_count = r_shadow_draw_cache.brush_count;
	r_shadow_draw_ctx.alias_count = r_shadow_draw_cache.alias_count;
	if (r_shadow_draw_ctx.brush_count > 0)
		memcpy (r_shadow_brush_ents, r_shadow_draw_cache.brush_ents, sizeof (entity_t *) * r_shadow_draw_ctx.brush_count);
	if (r_shadow_draw_ctx.alias_count > 0)
		memcpy (r_shadow_alias_ents, r_shadow_draw_cache.alias_ents, sizeof (entity_t *) * r_shadow_draw_ctx.alias_count);
}

static void R_Shadow_SaveDrawCache (qboolean include_world, entity_t **shadow_visedicts, int numshadowedicts)
{
	r_shadow_draw_cache.valid = true;
	r_shadow_draw_cache.visframecount = r_visframecount;
	r_shadow_draw_cache.numshadowedicts = numshadowedicts;
	r_shadow_draw_cache.include_world = include_world;
	r_shadow_draw_cache.worldmodel = cl.worldmodel;
	r_shadow_draw_cache.viewleaf = r_viewleaf;
	r_shadow_draw_cache.first_shadow_ent = (numshadowedicts > 0) ? shadow_visedicts[0] : NULL;
	r_shadow_draw_cache.last_shadow_ent = (numshadowedicts > 0) ? shadow_visedicts[numshadowedicts - 1] : NULL;
	r_shadow_draw_cache.brush_count = r_shadow_draw_ctx.brush_count;
	r_shadow_draw_cache.alias_count = r_shadow_draw_ctx.alias_count;
	if (r_shadow_draw_cache.brush_count > 0)
		memcpy (r_shadow_draw_cache.brush_ents, r_shadow_brush_ents, sizeof (entity_t *) * r_shadow_draw_cache.brush_count);
	if (r_shadow_draw_cache.alias_count > 0)
		memcpy (r_shadow_draw_cache.alias_ents, r_shadow_alias_ents, sizeof (entity_t *) * r_shadow_draw_cache.alias_count);
}

static void R_Shadow_BuildDrawContext (entity_t **shadow_visedicts, int numshadowedicts)
{
	int i;
	qboolean include_world = (r_drawworld.value && cl.worldmodel) ? true : false;

	r_shadow_draw_ctx.brush_ents = r_shadow_brush_ents;
	r_shadow_draw_ctx.alias_ents = r_shadow_alias_ents;
	r_shadow_draw_ctx.brush_count = 0;
	r_shadow_draw_ctx.alias_count = 0;

	if (R_Shadow_CanReuseDrawCache (include_world, shadow_visedicts, numshadowedicts))
	{
		R_Shadow_LoadDrawCache ();
		return;
	}

	if (include_world)
		r_shadow_brush_ents[r_shadow_draw_ctx.brush_count++] = &cl_entities[0];

	for (i = 0; i < numshadowedicts; ++i)
	{
		entity_t *ent = shadow_visedicts[i];

		if (!ent || !ent->model || !ENTALPHA_OPAQUE (ent->alpha))
			continue;
		if (ent == &cl_entities[0] || ent == &cl.viewent)
			continue;

		if (ent->model->type == mod_brush)
		{
			if (r_shadow_draw_ctx.brush_count < (int)countof (r_shadow_brush_ents))
				r_shadow_brush_ents[r_shadow_draw_ctx.brush_count++] = ent;
		}
		else if (ent->model->type == mod_alias)
		{
			if (r_shadow_draw_ctx.alias_count < (int)countof (r_shadow_alias_ents))
				r_shadow_alias_ents[r_shadow_draw_ctx.alias_count++] = ent;
		}
	}

	R_Shadow_SaveDrawCache (include_world, shadow_visedicts, numshadowedicts);
}

static qboolean R_Shadow_AABBOutsideFrustum (const vec3_t mins, const vec3_t maxs, const mplane_t planes[4])
{
	int i;
	vec3_t center, extents;

	center[0] = (mins[0] + maxs[0]) * 0.5f;
	center[1] = (mins[1] + maxs[1]) * 0.5f;
	center[2] = (mins[2] + maxs[2]) * 0.5f;
	extents[0] = (maxs[0] - mins[0]) * 0.5f;
	extents[1] = (maxs[1] - mins[1]) * 0.5f;
	extents[2] = (maxs[2] - mins[2]) * 0.5f;

	for (i = 0; i < 4; ++i)
	{
		const mplane_t *plane = &planes[i];
		float signed_distance = DotProduct (plane->normal, center) - plane->dist;
		float radius =
			fabsf (plane->normal[0]) * extents[0]
			+ fabsf (plane->normal[1]) * extents[1]
			+ fabsf (plane->normal[2]) * extents[2];
		if (signed_distance < -radius)
			return true;
	}

	return false;
}

static qboolean R_Shadow_EntityOutsideSphere (entity_t *ent, const vec4_t sphere)
{
	vec3_t mins, maxs, center, extents, to_center;
	float dist;
	float surf_radius;

	if (!ent || sphere[3] <= 0.f)
		return false;

	R_GetEntityBounds (ent, mins, maxs);
	center[0] = (mins[0] + maxs[0]) * 0.5f;
	center[1] = (mins[1] + maxs[1]) * 0.5f;
	center[2] = (mins[2] + maxs[2]) * 0.5f;
	extents[0] = (maxs[0] - mins[0]) * 0.5f;
	extents[1] = (maxs[1] - mins[1]) * 0.5f;
	extents[2] = (maxs[2] - mins[2]) * 0.5f;
	surf_radius = VectorLength (extents);

	VectorSubtract (center, sphere, to_center);
	dist = VectorLength (to_center);
	return dist > sphere[3] + surf_radius;
}

static qboolean R_Shadow_EntityPassesCull (entity_t *ent, const vec4_t *sphere, const mplane_t frustum4[4])
{
	vec3_t mins, maxs;

	if (!ent || !ent->model)
		return false;
	if (ent == &cl_entities[0])
		return true;
	if (sphere && r_shadow_cull_sphere.value > 0.f && R_Shadow_EntityOutsideSphere (ent, *sphere))
		return false;
	if (frustum4 && r_shadow_cull_frustum.value > 0.f)
	{
		R_GetEntityBounds (ent, mins, maxs);
		if (R_Shadow_AABBOutsideFrustum (mins, maxs, frustum4))
			return false;
	}
	return true;
}

static void R_Shadow_FilterDrawContext (const shadow_draw_context_t *src, shadow_draw_context_t *dst, const vec4_t *sphere, const mplane_t frustum4[4])
{
	int i;

	dst->brush_ents = r_shadow_filtered_brush_ents;
	dst->alias_ents = r_shadow_filtered_alias_ents;
	dst->brush_count = 0;
	dst->alias_count = 0;

	for (i = 0; i < src->brush_count; ++i)
	{
		entity_t *ent = src->brush_ents[i];
		if (!ent)
			continue;
		if (!R_Shadow_EntityPassesCull (ent, sphere, frustum4))
			continue;
		if (dst->brush_count < (int)countof (r_shadow_filtered_brush_ents))
			dst->brush_ents[dst->brush_count++] = ent;
	}

	for (i = 0; i < src->alias_count; ++i)
	{
		entity_t *ent = src->alias_ents[i];
		if (!ent)
			continue;
		if (!R_Shadow_EntityPassesCull (ent, sphere, frustum4))
			continue;
		if (dst->alias_count < (int)countof (r_shadow_filtered_alias_ents))
			dst->alias_ents[dst->alias_count++] = ent;
	}
}

static void R_RenderSunShadowMap (void)
{
	int cascade;
	vec3_t dummy = { 0.f, 0.f, 0.f };
	qboolean shadow_mark = (r_shadow_mark_mode.value > 0.f) && (r_shadow_draw_ctx.brush_count > 0);

	if (!R_Shadow_SunEnabled () || !framebufs.shadow.sun_fbo || !framebufs.shadow.sun_depth_tex)
		return;
	if (r_shadow_draw_ctx.brush_count <= 0 && r_shadow_draw_ctx.alias_count <= 0)
		return;

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.shadow.sun_fbo);
	for (cascade = 0; cascade < r_shadow_state.sun_cascade_count; ++cascade)
	{
		shadow_draw_context_t pass_ctx;
		const shadow_draw_context_t *ctx = &r_shadow_draw_ctx;

		if (r_shadow_cull_frustum.value > 0.f)
		{
			R_Shadow_FilterDrawContext (&r_shadow_draw_ctx, &pass_ctx, NULL, r_shadow_state.sun_frustum[cascade]);
			ctx = &pass_ctx;
		}
		if (ctx->brush_count <= 0 && ctx->alias_count <= 0)
			continue;

		if (shadow_mark)
		{
			R_MarkSurfaces_Shadow (
				r_shadow_state.sun_eye[cascade],
				r_shadow_state.sun_frustum[cascade],
				NULL,
				r_shadow_cull_vis.value > 0.f,
				r_shadow_cull_backface.value > 0.f,
				r_shadow_cull_frustum.value > 0.f);
		}

		GL_FramebufferTextureLayerFunc (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, framebufs.shadow.sun_depth_tex, 0, cascade);
		glViewport (0, 0, framebufs.shadow.sun_size, framebufs.shadow.sun_size);
		glClear (GL_DEPTH_BUFFER_BIT);

		R_Shadow_SetCasterState (&r_shadow_state, r_shadow_state.sun_viewproj[cascade], false, dummy, 1.f);
		if (ctx->brush_count > 0)
			R_DrawBrushModels_Shadow (ctx->brush_ents, ctx->brush_count, false);
		if (ctx->alias_count > 0)
			R_DrawAliasModels_Shadow (ctx->alias_ents, ctx->alias_count, false);
	}
}

static void R_RenderDLightShadowMaps (void)
{
	int slot, face;
	qboolean shadow_mark = (r_shadow_mark_mode.value > 0.f) && (r_shadow_draw_ctx.brush_count > 0);

	if (!R_Shadow_DlightEnabled () || !framebufs.shadow.dlight_fbo || !framebufs.shadow.dlight_depth_tex)
		return;
	if (r_shadow_state.num_dlights <= 0)
		return;
	if (r_shadow_draw_ctx.brush_count <= 0 && r_shadow_draw_ctx.alias_count <= 0)
		return;

	GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.shadow.dlight_fbo);
	for (slot = 0; slot < r_shadow_state.num_dlights; ++slot)
	{
		const vec4_t *light = &r_shadow_state.dlight_pos_radius[slot];
		for (face = 0; face < SHADOW_DLIGHT_FACES; ++face)
		{
			shadow_draw_context_t pass_ctx;
			const shadow_draw_context_t *ctx = &r_shadow_draw_ctx;
			vec4_t sphere;
			int layer = slot * SHADOW_DLIGHT_FACES + face;

			if (r_shadow_cull_frustum.value > 0.f || r_shadow_cull_sphere.value > 0.f)
			{
				if (r_shadow_cull_sphere.value > 0.f)
				{
					sphere[0] = (*light)[0];
					sphere[1] = (*light)[1];
					sphere[2] = (*light)[2];
					sphere[3] = (*light)[3];
				}
				else
				{
					sphere[0] = sphere[1] = sphere[2] = sphere[3] = 0.f;
				}
				R_Shadow_FilterDrawContext (&r_shadow_draw_ctx, &pass_ctx,
					(r_shadow_cull_sphere.value > 0.f) ? &sphere : NULL,
					(r_shadow_cull_frustum.value > 0.f) ? r_shadow_state.dlight_frustum[slot][face] : NULL);
				ctx = &pass_ctx;
			}
			if (ctx->brush_count <= 0 && ctx->alias_count <= 0)
				continue;

			if (shadow_mark)
			{
				sphere[0] = (*light)[0];
				sphere[1] = (*light)[1];
				sphere[2] = (*light)[2];
				sphere[3] = (*light)[3];
				R_MarkSurfaces_Shadow (
					*light,
					r_shadow_state.dlight_frustum[slot][face],
					(r_shadow_cull_sphere.value > 0.f) ? &sphere : NULL,
					r_shadow_cull_vis.value > 0.f,
					r_shadow_cull_backface.value > 0.f,
					r_shadow_cull_frustum.value > 0.f);
			}

			GL_FramebufferTextureLayerFunc (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, framebufs.shadow.dlight_depth_tex, 0, layer);
			glViewport (0, 0, framebufs.shadow.dlight_size, framebufs.shadow.dlight_size);
			glClear (GL_DEPTH_BUFFER_BIT);

			R_Shadow_SetCasterState (&r_shadow_state, r_shadow_state.dlight_viewproj[slot][face], true, *light, (*light)[3]);
			if (ctx->brush_count > 0)
				R_DrawBrushModels_Shadow (ctx->brush_ents, ctx->brush_count, true);
			if (ctx->alias_count > 0)
				R_DrawAliasModels_Shadow (ctx->alias_ents, ctx->alias_count, true);
		}
	}
}

void R_Shadow_RenderMaps (entity_t **shadow_visedicts, int numshadowedicts)
{
	unsigned saved_state = glstate;
	GLuint sun_query = 0;
	GLuint dlight_query = 0;
	double cpu_begin = 0.0;
	double cpu_after_sun = 0.0;
	double cpu_after_dlight = 0.0;
	double cpu_end = 0.0;
	double sun_gpu_ms = 0.0;
	double dlight_gpu_ms = 0.0;
	qboolean profile_enabled = false;
	qboolean clip_depth_mode_changed = false;
	qboolean want_sun_shadow = false;
	qboolean want_dlight_shadows = false;
	qboolean marked_for_shadow = false;
	qboolean have_query_api = false;

	R_Shadow_ResetRuntime (&r_shadow_state);
	R_Shadow_ClearDrawContext ();
	R_Shadow_ReconcileResources ();
	R_Shadow_NormalizeSettings ();

	if (!framebufs.shadow.available || !R_Shadow_Enabled ())
		return;
	if (!cl.worldmodel || !r_drawworld_cheatsafe)
		return;

	R_Shadow_BuildDrawContext (shadow_visedicts, numshadowedicts);
	if (r_shadow_draw_ctx.brush_count <= 0 && r_shadow_draw_ctx.alias_count <= 0)
		return;

	R_Shadow_SelectDlights (&r_shadow_state);
	want_sun_shadow = (R_Shadow_SunEnabled () && framebufs.shadow.sun_fbo && framebufs.shadow.sun_depth_tex);
	want_dlight_shadows = (r_shadow_state.num_dlights > 0 && R_Shadow_DlightEnabled () && framebufs.shadow.dlight_fbo && framebufs.shadow.dlight_depth_tex);
	if (want_sun_shadow)
		R_Shadow_UpdateSunMatrix (&r_shadow_state);
	if (want_dlight_shadows)
		R_Shadow_UpdateDlightMatrices (&r_shadow_state);
	if (!want_sun_shadow && !want_dlight_shadows)
		return;
	marked_for_shadow = (r_shadow_draw_ctx.brush_count > 0 && r_shadow_mark_mode.value > 0.f);

	have_query_api = (GL_GenQueriesFunc && GL_BeginQueryFunc && GL_EndQueryFunc
		&& GL_GetQueryObjectui64vFunc && GL_DeleteQueriesFunc);
	profile_enabled = (r_shadow_profile.value > 0.f);
	if (profile_enabled && !have_query_api && r_framecount >= r_shadow_profile_last_frame + 300)
	{
		Con_Printf ("Shadow profile: GPU timer queries unavailable, reporting CPU timings only.\n");
		r_shadow_profile_last_frame = r_framecount;
	}
	if (profile_enabled)
		cpu_begin = Sys_DoubleTime ();
	if (r_shadow_log.value > 0.f && r_framecount >= r_shadow_log_last_frame + 60)
	{
		Con_Printf ("Shadow frame: sun=%d dlight=%d cascades=%d casters(brush=%d alias=%d) selected_dlights=%d\n",
			want_sun_shadow ? 1 : 0,
			want_dlight_shadows ? 1 : 0,
			r_shadow_state.sun_cascade_count,
			r_shadow_draw_ctx.brush_count,
			r_shadow_draw_ctx.alias_count,
			r_shadow_state.num_dlights);
		r_shadow_log_last_frame = r_framecount;
	}

	if (gl_clipcontrol_able && GL_ClipControlFunc)
	{
		GL_ClipControlFunc (GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
		clip_depth_mode_changed = true;
	}
	glDepthFunc (GL_LEQUAL);
	glClearDepth (1.0);
	glDepthMask (GL_TRUE);
	glColorMask (GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

	GL_BeginGroup ("Shadow maps");
	if (profile_enabled && have_query_api && want_sun_shadow)
	{
		GL_GenQueriesFunc (1, &sun_query);
		GL_BeginQueryFunc (GL_TIME_ELAPSED, sun_query);
	}
	R_RenderSunShadowMap ();
	if (profile_enabled && have_query_api && sun_query)
		GL_EndQueryFunc (GL_TIME_ELAPSED);
	if (profile_enabled)
		cpu_after_sun = Sys_DoubleTime ();

	if (profile_enabled && have_query_api && want_dlight_shadows)
	{
		GL_GenQueriesFunc (1, &dlight_query);
		GL_BeginQueryFunc (GL_TIME_ELAPSED, dlight_query);
	}
	R_RenderDLightShadowMaps ();
	if (profile_enabled && have_query_api && dlight_query)
		GL_EndQueryFunc (GL_TIME_ELAPSED);
	GL_EndGroup ();

	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	R_Backend_SetPipelineState (saved_state);
	if (clip_depth_mode_changed)
		GL_ClipControlFunc (GL_LOWER_LEFT, GL_ZERO_TO_ONE);
	if (gl_clipcontrol_able)
	{
		glDepthFunc (GL_GEQUAL);
		glClearDepth (0.0);
	}
	else
	{
		glDepthFunc (GL_LEQUAL);
		glClearDepth (1.0);
	}
	if (marked_for_shadow)
		R_RestoreMarkedSurfaces ();

	if (profile_enabled)
	{
		cpu_after_dlight = Sys_DoubleTime ();
		cpu_end = cpu_after_dlight;

		if (have_query_api)
		{
			if (sun_query)
			{
				GLuint64 ns = 0;
				GL_GetQueryObjectui64vFunc (sun_query, GL_QUERY_RESULT, &ns);
				sun_gpu_ms = (double)ns / 1000000.0;
				GL_DeleteQueriesFunc (1, &sun_query);
			}
			if (dlight_query)
			{
				GLuint64 ns = 0;
				GL_GetQueryObjectui64vFunc (dlight_query, GL_QUERY_RESULT, &ns);
				dlight_gpu_ms = (double)ns / 1000000.0;
				GL_DeleteQueriesFunc (1, &dlight_query);
			}
		}

		{
			const double cpu_sun_ms = (cpu_after_sun - cpu_begin) * 1000.0;
			const double cpu_dlight_ms = (cpu_after_dlight - cpu_after_sun) * 1000.0;
			const double cpu_total_ms = (cpu_end - cpu_begin) * 1000.0;
			const double alpha = 0.2;

			if (r_shadow_profile_stats.cpu_total_ms <= 0.0)
			{
				r_shadow_profile_stats.cpu_sun_ms = cpu_sun_ms;
				r_shadow_profile_stats.cpu_dlight_ms = cpu_dlight_ms;
				r_shadow_profile_stats.cpu_total_ms = cpu_total_ms;
				r_shadow_profile_stats.gpu_sun_ms = sun_gpu_ms;
				r_shadow_profile_stats.gpu_dlight_ms = dlight_gpu_ms;
			}
			else
			{
				r_shadow_profile_stats.cpu_sun_ms = r_shadow_profile_stats.cpu_sun_ms * (1.0 - alpha) + cpu_sun_ms * alpha;
				r_shadow_profile_stats.cpu_dlight_ms = r_shadow_profile_stats.cpu_dlight_ms * (1.0 - alpha) + cpu_dlight_ms * alpha;
				r_shadow_profile_stats.cpu_total_ms = r_shadow_profile_stats.cpu_total_ms * (1.0 - alpha) + cpu_total_ms * alpha;
				r_shadow_profile_stats.gpu_sun_ms = r_shadow_profile_stats.gpu_sun_ms * (1.0 - alpha) + sun_gpu_ms * alpha;
				r_shadow_profile_stats.gpu_dlight_ms = r_shadow_profile_stats.gpu_dlight_ms * (1.0 - alpha) + dlight_gpu_ms * alpha;
			}
		}

		if (r_framecount >= r_shadow_profile_last_frame + 60)
		{
			Con_Printf (
				"Shadow profile: CPU(ms) sun=%.3f dlight=%.3f total=%.3f | GPU(ms) sun=%.3f dlight=%.3f | dlights=%d\n",
				r_shadow_profile_stats.cpu_sun_ms,
				r_shadow_profile_stats.cpu_dlight_ms,
				r_shadow_profile_stats.cpu_total_ms,
				r_shadow_profile_stats.gpu_sun_ms,
				r_shadow_profile_stats.gpu_dlight_ms,
				r_shadow_state.num_dlights);
			r_shadow_profile_last_frame = r_framecount;
		}
	}

	r_shadow_state.valid = (want_sun_shadow || want_dlight_shadows);
}

