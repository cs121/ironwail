/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2024 Ironwail contributors

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
#include "cl_postfx.h"

#define POSTFX_MAX_EVENTS 32

typedef struct postfx_event_s
{
	postfx_event_type_t	type;
	double				start_time;
	double				last_update;
	float				duration;
	float				attack;
	float				decay;
	float				intensity;
	float				params[8];
	int					flags;
	qboolean			active;
} postfx_event_t;

static postfx_event_t	postfx_events[POSTFX_MAX_EVENTS];
static double			postfx_last_time;
static int				postfx_contents = CONTENTS_EMPTY;
static qboolean			postfx_underwater;
static float			postfx_underwater_grade;
static float			postfx_underwater_fog;
static int				postfx_powerup_lut = PFX_LUT_NONE;
static float			postfx_powerup_strength;

extern cvar_t r_postfx;
extern cvar_t r_postfx_pickup;
extern cvar_t r_postfx_pickup_exposure;
extern cvar_t r_postfx_pickup_bloom;
extern cvar_t r_postfx_pickup_duration;
extern cvar_t r_postfx_damage;
extern cvar_t r_postfx_damage_vignette;
extern cvar_t r_postfx_damage_vignette_softness;
extern cvar_t r_postfx_damage_desat;
extern cvar_t r_postfx_damage_exposure;
extern cvar_t r_postfx_damage_duration;
extern cvar_t r_postfx_damage_accum_window;
extern cvar_t r_postfx_damage_accum_scale;
extern cvar_t r_postfx_powerup;
extern cvar_t r_postfx_powerup_lut_strength;
extern cvar_t r_postfx_lut_strength_powerup;
extern cvar_t r_postfx_powerup_ramp_in;
extern cvar_t r_postfx_powerup_ramp_out;
extern cvar_t r_postfx_underwater;
extern cvar_t r_postfx_underwater_grade_strength;
extern cvar_t r_postfx_lut_strength_underwater;
extern cvar_t r_postfx_underwater_fog_strength;
extern cvar_t r_postfx_underwater_ramp_in;
extern cvar_t r_postfx_underwater_ramp_out;
extern cvar_t r_postfx_underwater_fog_water_r;
extern cvar_t r_postfx_underwater_fog_water_g;
extern cvar_t r_postfx_underwater_fog_water_b;
extern cvar_t r_postfx_underwater_fog_slime_r;
extern cvar_t r_postfx_underwater_fog_slime_g;
extern cvar_t r_postfx_underwater_fog_slime_b;
extern cvar_t r_postfx_underwater_fog_lava_r;
extern cvar_t r_postfx_underwater_fog_lava_g;
extern cvar_t r_postfx_underwater_fog_lava_b;
extern cvar_t r_postfx_quad;
extern cvar_t r_postfx_quad_emissive_boost;
extern cvar_t r_postfx_quad_bloom_boost;
extern cvar_t r_postfx_quad_pulse_speed;
extern cvar_t r_postfx_quad_pulse_intensity;
extern cvar_t r_postfx_bloom_mode;
extern cvar_t r_postfx_debug;

static float PostFX_Saturate (float value)
{
	if (value < 0.f)
		return 0.f;
	if (value > 1.f)
		return 1.f;
	return value;
}

static float PostFX_SmoothStep (float t)
{
	return t * t * (3.f - 2.f * t);
}

static float PostFX_SmoothedLerp (float current, float target, float dt, float ramp_in, float ramp_out)
{
	float ramp = (target > current) ? ramp_in : ramp_out;
	float safe_ramp = q_max (ramp, 0.001f);
	float alpha = q_min (1.f, q_max (0.f, dt / safe_ramp));
	return current + (target - current) * alpha;
}

static postfx_event_t *PostFX_AllocEvent (void)
{
	int i;
	int oldest_index = 0;
	double oldest_time = postfx_events[0].start_time;

	for (i = 0; i < POSTFX_MAX_EVENTS; ++i)
	{
		if (!postfx_events[i].active)
			return &postfx_events[i];
		if (postfx_events[i].start_time < oldest_time)
		{
			oldest_time = postfx_events[i].start_time;
			oldest_index = i;
		}
	}

	return &postfx_events[oldest_index];
}

static float PostFX_EventWeight (postfx_event_t *event, double now)
{
	float elapsed;
	float duration;
	float remaining;
	float weight;

	if (!event->active)
		return 0.f;

	duration = q_max (event->duration, 0.f);
	elapsed = (float)(now - event->start_time);
	if (elapsed < 0.f || elapsed >= duration || duration <= 0.f)
	{
		event->active = false;
		return 0.f;
	}

	if (event->attack <= 0.f && event->decay <= 0.f)
	{
		remaining = 1.f - (elapsed / duration);
		remaining = PostFX_Saturate (remaining);
		weight = PostFX_SmoothStep (remaining);
		return weight * event->intensity;
	}

	weight = 1.f;
	if (event->attack > 0.f)
	{
		float attack_end = q_min (event->attack, duration);
		if (elapsed < attack_end)
		{
			float t = attack_end > 0.f ? elapsed / attack_end : 1.f;
			weight = PostFX_SmoothStep (PostFX_Saturate (t));
			return weight * event->intensity;
		}
	}
	if (event->decay > 0.f)
	{
		float decay_start = q_max (0.f, duration - event->decay);
		if (elapsed > decay_start)
		{
			float t = (duration - elapsed) / q_max (event->decay, 0.001f);
			weight = PostFX_SmoothStep (PostFX_Saturate (t));
		}
	}

	return weight * event->intensity;
}

static int PostFX_DesiredPowerupLUT (void)
{
	if (cl.items & IT_INVULNERABILITY)
		return PFX_LUT_PENT;
	if (cl.items & IT_INVISIBILITY)
		return PFX_LUT_RING;
	if (cl.items & IT_SUIT)
		return PFX_LUT_SUIT;
	if (cl.items & IT_QUAD)
		return PFX_LUT_QUAD;
	return PFX_LUT_NONE;
}

void CL_PostFX_Init (void)
{
	CL_PostFX_Reset ();
}

void CL_PostFX_Reset (void)
{
	memset (postfx_events, 0, sizeof (postfx_events));
	postfx_last_time = cl.time;
	postfx_contents = CONTENTS_EMPTY;
	postfx_underwater = false;
	postfx_underwater_grade = 0.f;
	postfx_underwater_fog = 0.f;
	postfx_powerup_lut = PFX_LUT_NONE;
	postfx_powerup_strength = 0.f;
}

void CL_PostFX_Frame (void)
{
	double now = cl.time;
	float dt = (float)(now - postfx_last_time);
	int desired_lut;
	float target_strength;
	float grade_strength;
	float fog_strength;
	float ramp_in;
	float ramp_out;

	if (dt < 0.f)
		dt = 0.f;
	if (dt > 0.1f)
		dt = 0.1f;
	postfx_last_time = now;

	if (r_postfx.value <= 0.f)
	{
		postfx_underwater_grade = 0.f;
		postfx_underwater_fog = 0.f;
		postfx_powerup_strength = 0.f;
		return;
	}

	desired_lut = PostFX_DesiredPowerupLUT ();
	target_strength = (r_postfx_powerup.value > 0.f && desired_lut != PFX_LUT_NONE)
		? q_max (0.f, q_max (r_postfx_powerup_lut_strength.value, r_postfx_lut_strength_powerup.value))
		: 0.f;
	ramp_in = q_max (0.f, r_postfx_powerup_ramp_in.value);
	ramp_out = q_max (0.f, r_postfx_powerup_ramp_out.value);
	postfx_powerup_strength = PostFX_SmoothedLerp (postfx_powerup_strength, target_strength, dt, ramp_in, ramp_out);
	if (postfx_powerup_strength > 0.f)
		postfx_powerup_lut = desired_lut;
	else
		postfx_powerup_lut = PFX_LUT_NONE;

	grade_strength = (r_postfx_underwater.value > 0.f && postfx_underwater)
		? q_max (0.f, q_max (r_postfx_underwater_grade_strength.value, r_postfx_lut_strength_underwater.value))
		: 0.f;
	fog_strength = (r_postfx_underwater.value > 0.f && postfx_underwater)
		? q_max (0.f, r_postfx_underwater_fog_strength.value)
		: 0.f;
	ramp_in = q_max (0.f, r_postfx_underwater_ramp_in.value);
	ramp_out = q_max (0.f, r_postfx_underwater_ramp_out.value);
	postfx_underwater_grade = PostFX_SmoothedLerp (postfx_underwater_grade, grade_strength, dt, ramp_in, ramp_out);
	postfx_underwater_fog = PostFX_SmoothedLerp (postfx_underwater_fog, fog_strength, dt, ramp_in, ramp_out);
}

void CL_PostFX_PushPickup (void)
{
	postfx_event_t *event;
	float duration;
	float intensity;

	if (r_postfx.value <= 0.f || r_postfx_pickup.value <= 0.f)
		return;

	duration = q_max (0.05f, r_postfx_pickup_duration.value);
	intensity = PostFX_Saturate (r_postfx_pickup.value);

	event = PostFX_AllocEvent ();
	memset (event, 0, sizeof (*event));
	event->type = PFX_EVENT_PICKUP;
	event->start_time = cl.time;
	event->last_update = cl.time;
	event->duration = duration;
	event->attack = duration * 0.2f;
	event->decay = duration * 0.8f;
	event->intensity = intensity;
	event->params[0] = r_postfx_pickup_exposure.value;
	event->params[1] = r_postfx_pickup_bloom.value;
	event->active = true;
}

void CL_PostFX_PushDamage (float damage_amount)
{
	postfx_event_t *event;
	double now;
	float duration;
	float intensity;
	float accum_window;
	int i;

	if (r_postfx.value <= 0.f || r_postfx_damage.value <= 0.f)
		return;

	intensity = PostFX_Saturate (damage_amount / 100.f);
	intensity = PostFX_Saturate (intensity * r_postfx_damage.value);
	if (intensity <= 0.f)
		return;

	duration = q_max (0.05f, r_postfx_damage_duration.value);
	accum_window = q_max (0.f, r_postfx_damage_accum_window.value);
	now = cl.time;

	for (i = 0; i < POSTFX_MAX_EVENTS; ++i)
	{
		if (!postfx_events[i].active || postfx_events[i].type != PFX_EVENT_DAMAGE)
			continue;
		if ((now - postfx_events[i].last_update) > accum_window)
			continue;

		postfx_events[i].intensity = PostFX_Saturate (postfx_events[i].intensity + intensity * q_max (0.f, r_postfx_damage_accum_scale.value));
		postfx_events[i].duration = q_max (postfx_events[i].duration, (float)(now - postfx_events[i].start_time) + duration);
		postfx_events[i].last_update = now;
		return;
	}

	event = PostFX_AllocEvent ();
	memset (event, 0, sizeof (*event));
	event->type = PFX_EVENT_DAMAGE;
	event->start_time = now;
	event->last_update = now;
	event->duration = duration;
	event->attack = duration * 0.1f;
	event->decay = duration * 0.9f;
	event->intensity = intensity;
	event->params[0] = r_postfx_damage_vignette.value;
	event->params[1] = r_postfx_damage_desat.value;
	event->params[2] = r_postfx_damage_exposure.value;
	event->params[3] = r_postfx_damage_vignette_softness.value;
	event->active = true;
}

void CL_PostFX_SetContents (int contents, qboolean underwater_active)
{
	postfx_contents = contents;
	postfx_underwater = underwater_active;
}

void CL_PostFX_GetState (postfx_state_t *out_state)
{
	int i;
	double now;
	float weight;
	float pulse;
	float bloom_add;
	float vignette;
	float desat;
	float exposure;
	float vignette_softness;
	float fog_r;
	float fog_g;
	float fog_b;

	memset (out_state, 0, sizeof (*out_state));

	if (r_postfx.value <= 0.f)
		return;

	now = cl.time;
	bloom_add = 0.f;
	vignette = 0.f;
	desat = 0.f;
	exposure = 0.f;
	vignette_softness = 0.f;

	for (i = 0; i < POSTFX_MAX_EVENTS; ++i)
	{
		postfx_event_t *event = &postfx_events[i];
		if (!event->active)
			continue;
		weight = PostFX_EventWeight (event, now);
		if (weight <= 0.f)
			continue;

		switch (event->type)
		{
		case PFX_EVENT_PICKUP:
			exposure += weight * event->params[0];
			if (r_postfx_bloom_mode.value > 0.f)
				bloom_add = q_max (bloom_add, weight * event->params[1]);
			else
				bloom_add += weight * event->params[1];
			break;
		case PFX_EVENT_DAMAGE:
			vignette = q_max (vignette, weight * event->params[0]);
			vignette_softness = q_max (vignette_softness, weight * event->params[3]);
			desat = q_max (desat, weight * event->params[1]);
			exposure += weight * event->params[2];
			break;
		}
	}

	out_state->exposure_add_stops = CLAMP (-2.f, exposure, 2.f);
	out_state->bloom_boost = q_max (0.f, bloom_add);
	out_state->vignette = PostFX_Saturate (vignette);
	out_state->vignette_softness = PostFX_Saturate (vignette_softness);
	out_state->desat = PostFX_Saturate (desat);
	out_state->underwater_grade_strength = PostFX_Saturate (postfx_underwater_grade);
	out_state->underwater_fog_strength = PostFX_Saturate (postfx_underwater_fog);

	fog_r = r_postfx_underwater_fog_water_r.value;
	fog_g = r_postfx_underwater_fog_water_g.value;
	fog_b = r_postfx_underwater_fog_water_b.value;
	if (postfx_contents == CONTENTS_SLIME)
	{
		fog_r = r_postfx_underwater_fog_slime_r.value;
		fog_g = r_postfx_underwater_fog_slime_g.value;
		fog_b = r_postfx_underwater_fog_slime_b.value;
	}
	else if (postfx_contents == CONTENTS_LAVA)
	{
		fog_r = r_postfx_underwater_fog_lava_r.value;
		fog_g = r_postfx_underwater_fog_lava_g.value;
		fog_b = r_postfx_underwater_fog_lava_b.value;
	}
	out_state->underwater_fog_color[0] = PostFX_Saturate (fog_r);
	out_state->underwater_fog_color[1] = PostFX_Saturate (fog_g);
	out_state->underwater_fog_color[2] = PostFX_Saturate (fog_b);

	if (postfx_powerup_strength > 0.f)
	{
		out_state->lut_id = postfx_powerup_lut;
		out_state->lut_strength = PostFX_Saturate (postfx_powerup_strength);
	}
	else if (postfx_underwater_grade > 0.f)
	{
		if (postfx_contents == CONTENTS_SLIME)
			out_state->lut_id = PFX_LUT_SLIME;
		else if (postfx_contents == CONTENTS_LAVA)
			out_state->lut_id = PFX_LUT_LAVA;
		else
			out_state->lut_id = PFX_LUT_WATER;
		out_state->lut_strength = PostFX_Saturate (postfx_underwater_grade);
	}
	else
	{
		out_state->lut_id = PFX_LUT_NONE;
		out_state->lut_strength = 0.f;
	}

	if (r_postfx_quad.value > 0.f && (cl.items & IT_QUAD))
	{
		out_state->emissive_boost = q_max (0.f, r_postfx_quad_emissive_boost.value);
		out_state->bloom_boost += q_max (0.f, r_postfx_quad_bloom_boost.value);
		if (r_postfx_quad_pulse_intensity.value > 0.f && r_postfx_quad_pulse_speed.value > 0.f)
		{
			pulse = 0.5f + 0.5f * sinf ((float)cl.time * r_postfx_quad_pulse_speed.value * (float)M_PI * 2.f);
			out_state->emissive_boost += pulse * q_max (0.f, r_postfx_quad_pulse_intensity.value);
		}
		out_state->emissive_boost = q_max (0.f, out_state->emissive_boost);
	}

	if (r_postfx_debug.value > 0.f)
	{
		static int last_printed = -1;
		int now_seconds = (int)floor (cl.time);
		if (now_seconds != last_printed)
		{
			int active_count = 0;
			for (i = 0; i < POSTFX_MAX_EVENTS; ++i)
				if (postfx_events[i].active)
					active_count++;
				Con_Printf ("PostFX: exposure %.2f bloom %.2f vignette %.2f desat %.2f lut %d/%.2f fog %.2f events %d\n",
					out_state->exposure_add_stops,
					out_state->bloom_boost,
					out_state->vignette,
					out_state->desat,
					out_state->lut_id,
					out_state->lut_strength,
					out_state->underwater_fog_strength,
					active_count);
			last_printed = now_seconds;
		}
	}
}
