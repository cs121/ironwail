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
extern cvar_t r_shadow_log;

extern cvar_t r_sun_light;
extern qboolean R_WorldHasSun (void);

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
		for (f = 0; f < SHADOW_DLIGHT_FACES; ++f)
			memset (state->dlight_frustum[i][f], 0, sizeof (state->dlight_frustum[i][f]));
	}
}

void R_Shadow_SelectDlights (shadow_runtime_t *state)
{
	int i, slot;
	int limit = CLAMP (0, (int)Q_rint (r_shadow_dlight_max.value), SHADOW_DLIGHT_MAX);
	float best_score[SHADOW_DLIGHT_MAX];

	if (!state)
		return;

	state->num_dlights = 0;
	for (slot = 0; slot < SHADOW_DLIGHT_MAX; ++slot)
	{
		state->dlight_indices[slot] = -1;
		VectorSet (state->dlight_pos_radius[slot], 0.f, 0.f, 0.f);
		state->dlight_pos_radius[slot][3] = 1.f;
		best_score[slot] = -FLT_MAX;
	}

	if (!R_Shadow_DlightEnabled () || limit <= 0)
		return;

	for (i = 0; i < (int)r_framedata.numlights; ++i)
	{
		const gpulight_t *l = &r_lightbuffer.lights[i];
		vec3_t to_light;
		float dist, luminance, score;

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
		if (idx < 0 || idx >= (int)r_framedata.numlights)
			continue;
		VectorCopy (r_lightbuffer.lights[idx].pos, state->dlight_pos_radius[slot]);
		state->dlight_pos_radius[slot][3] = q_max (r_lightbuffer.lights[idx].radius, 16.f);
		state->num_dlights++;
	}
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
