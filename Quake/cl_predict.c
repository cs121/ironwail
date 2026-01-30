/*
Copyright (C) 2024
*/

#include "quakedef.h"

extern cvar_t sv_maxspeed;
extern cvar_t sv_accelerate;
extern cvar_t sv_friction;
extern cvar_t sv_stopspeed;
extern cvar_t sv_gravity;
extern cvar_t cl_netdebug_parse;
extern cvar_t cl_cmdrate;
extern cvar_t cl_physrate;
extern cvar_t cl_jitter_debug;

typedef struct
{
	int		id;
	vec3_t		last_origin;
	vec3_t		last_angles;
	double		last_time;
	qboolean	valid;
} cl_pred_ground_cache_t;

typedef struct
{
	qboolean	onground;
	int		groundent;
	qboolean	ground_valid;
	float		ground_delta_len;
	float		ground_yaw_delta;
	int		ground_apply_pred;
	int		ground_apply_render;
} cl_pred_ground_debug_t;

typedef struct
{
	vec3_t	origin;
	vec3_t	velocity;
	vec3_t	viewangles;
	qboolean	onground;
	int		groundent;
	qboolean	ground_valid;
	cl_pred_ground_cache_t ground_cache;
} cl_pred_state_t;

typedef struct
{
	usercmd_t	cmds[CMD_RING];
	unsigned int	seq_latest;
	unsigned int	seq_acked;
	qboolean	has_base;
	cl_pred_state_t	base;
	cl_pred_state_t	predicted;
} cl_pred_t;

static cl_pred_t cl_pred;
static qboolean cl_netdbg_predict_ran;
static qboolean cl_pred_warned_solid;
static vec3_t cl_pred_error;
static vec3_t cl_pred_angle_error;
static int cl_pred_steps_this_frame;
static qboolean cl_pred_server_update_this_frame;
static cl_pred_ground_debug_t cl_pred_ground_dbg;

#define CL_PREDICT_MAX_CLIP_PLANES 5
#define CL_PREDICT_STEP_SIZE 18.0f
#define CL_PREDICT_GROUND_EPSILON 2.0f
#define CL_PREDICT_CORRECTION_THRESHOLD 64.0f

qboolean CL_NetDbg_PredictRan (void)
{
	return cl_netdbg_predict_ran;
}

static qboolean CL_ViewEntityOriginIsBad (const vec3_t origin)
{
	const float max_abs = 65536.0f;
	int i;

	if (VectorCompare (origin, vec3_origin))
		return true;
	for (i = 0; i < 3; i++)
	{
		if (!isfinite (origin[i]) || fabsf (origin[i]) > max_abs)
			return true;
	}

	return false;
}

static void CL_EnsureViewEntityOrigin (const char *reason)
{
	entity_t *ent;

	if (!cl_entities || cl.viewentity <= 0 || cl.viewentity >= cl_max_edicts)
		return;
	ent = &cl_entities[cl.viewentity];
	if (!CL_ViewEntityOriginIsBad (ent->origin))
		return;

	// The renderer/camera rely on the viewentity origin; repair it using simorg.
	VectorCopy (cl.simorg, ent->origin);
	VectorCopy (cl.simorg, ent->msg_origins[0]);
	VectorCopy (cl.simorg, ent->msg_origins[1]);
	if (cl_netdebug_parse.value)
	{
		Con_Printf ("NETDBG: viewentity origin repaired (%s): simorg=%f %f %f\n",
			reason ? reason : "unknown", cl.simorg[0], cl.simorg[1], cl.simorg[2]);
	}
}

static qboolean CL_Predict_SeqNewer (unsigned int seq, unsigned int ref)
{
	return NETSEQ_GT (seq, ref);
}

static void CL_Predict_ResetGroundCache (cl_pred_state_t *state)
{
	if (!state)
		return;

	state->groundent = 0;
	state->ground_valid = false;
	state->ground_cache.id = 0;
	state->ground_cache.last_time = 0.0;
	state->ground_cache.valid = false;
	VectorClear (state->ground_cache.last_origin);
	VectorClear (state->ground_cache.last_angles);
}

static void CL_Predict_InvalidateGroundCache (cl_pred_state_t *state)
{
	if (!state)
		return;

	state->ground_valid = false;
	state->ground_cache.id = 0;
	state->ground_cache.last_time = 0.0;
	state->ground_cache.valid = false;
	VectorClear (state->ground_cache.last_origin);
	VectorClear (state->ground_cache.last_angles);
}

static void CL_Predict_ResetGroundDebug (void)
{
	memset (&cl_pred_ground_dbg, 0, sizeof(cl_pred_ground_dbg));
}

static qboolean CL_Predict_IsGroundEntityValid (int groundent)
{
	if (groundent <= 0 || groundent >= cl_max_edicts)
		return false;
	if (!cl_entities)
		return false;
	if (cl.mtime[0] <= 0.0)
		return false;
	if (cl.snapshot_present && !cl.snapshot_present[groundent])
		return false;
	if (cl_entities[groundent].msgtime != cl.mtime[0])
		return false;
	return true;
}

static float CL_Predict_GetStepTime (void)
{
	double cmd_rate = cl_cmdrate.value > 0.0 ? cl_cmdrate.value : 60.0;
	double cmd_dt = 1.0 / cmd_rate;

	if (cl_physrate.value > 0.0)
		return (float)(1.0 / cl_physrate.value);
	return (float)cmd_dt;
}

static float CL_Predict_AngleDelta (float a, float b)
{
	float d = a - b;

	while (d > 180.0f)
		d -= 360.0f;
	while (d < -180.0f)
		d += 360.0f;
	return d;
}

static qboolean CL_Predict_IsEnabled (void)
{
	// Prediction may be gated on world readiness, but command sending must not be.
	if (cls.state != ca_connected)
		return false;
	if (cls.demoplayback)
		return false;
	if (cl.intermission)
		return false;
	if (!cl.has_valid_worldstate)
		return false;
	if (!CL_WorldReady ())
		return false;
	return true;
}

static void CL_Predict_ApplyToClient (void)
{
	vec3_t smooth_origin;
	vec3_t smooth_angles;
	float smooth_ms;
	float decay;
	vec3_t correction;
	vec3_t angle_correction;

	if (!CL_Predict_IsEnabled () || !cl_pred.has_base)
		return;

	smooth_ms = cl_pred_smooth_ms.value;
	if (smooth_ms <= 0.0f)
	{
		VectorClear (cl_pred_error);
		VectorClear (cl_pred_angle_error);
	}

	VectorAdd (cl_pred.predicted.origin, cl_pred_error, smooth_origin);
	VectorAdd (cl_pred.predicted.viewangles, cl_pred_angle_error, smooth_angles);
	VectorCopy (smooth_origin, cl.simorg);
	VectorCopy (smooth_origin, cl_entities[cl.viewentity].origin);
	VectorCopy (smooth_origin, cl_entities[cl.viewentity].msg_origins[0]);
	VectorCopy (smooth_origin, cl_entities[cl.viewentity].msg_origins[1]);
	VectorCopy (smooth_angles, cl.viewangles);
	VectorCopy (cl_pred.predicted.velocity, cl.mvelocity[0]);
	VectorCopy (cl_pred.predicted.velocity, cl.mvelocity[1]);
	cl.onground = cl_pred.predicted.onground;

	if (smooth_ms > 0.0f)
	{
		decay = host_frametime / (smooth_ms * 0.001f);
		decay = CLAMP (0.0f, decay, 1.0f);
		VectorScale (cl_pred_error, decay, correction);
		VectorSubtract (cl_pred_error, correction, cl_pred_error);
		VectorScale (cl_pred_angle_error, decay, angle_correction);
		VectorSubtract (cl_pred_angle_error, angle_correction, cl_pred_angle_error);
		if (cl_netdbg_pred.value > 0.0f)
		{
			Con_Printf ("NETDBG: pred_smooth apply %.2f remaining %.2f\n",
				VectorLength (correction), VectorLength (cl_pred_error));
		}
	}

	CL_EnsureViewEntityOrigin ("predict");
}

static void CL_Predict_Friction (vec3_t velocity, float dt)
{
	float	speed;
	float	newspeed;
	float	control;

	speed = sqrt(velocity[0]*velocity[0] + velocity[1]*velocity[1]);
	if (!speed)
		return;

	control = speed < sv_stopspeed.value ? sv_stopspeed.value : speed;
	newspeed = speed - dt * control * sv_friction.value;
	if (newspeed < 0)
		newspeed = 0;
	newspeed /= speed;

	velocity[0] *= newspeed;
	velocity[1] *= newspeed;
}

static void CL_Predict_Accelerate (vec3_t velocity, const vec3_t wishdir, float wishspeed, float accel, float dt)
{
	float	addspeed;
	float	accelspeed;
	float	currentspeed;
	int		i;

	currentspeed = DotProduct (velocity, wishdir);
	addspeed = wishspeed - currentspeed;
	if (addspeed <= 0)
		return;

	accelspeed = accel * dt * wishspeed;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	for (i=0 ; i<3 ; i++)
		velocity[i] += accelspeed * wishdir[i];
}

static void CL_Predict_AirAccelerate (vec3_t velocity, vec3_t wishvel, float wishspeed, float dt)
{
	float	addspeed;
	float	accelspeed;
	float	currentspeed;
	float	wishspd;
	int		i;

	wishspd = VectorNormalize (wishvel);
	if (wishspd > 30)
		wishspd = 30;
	currentspeed = DotProduct (velocity, wishvel);
	addspeed = wishspd - currentspeed;
	if (addspeed <= 0)
		return;

	accelspeed = sv_accelerate.value * wishspeed * dt;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	for (i=0 ; i<3 ; i++)
		velocity[i] += accelspeed * wishvel[i];
}

static void CL_Predict_GetPlayerBounds (vec3_t mins, vec3_t maxs)
{
	if (cl.worldmodel)
	{
		VectorCopy (cl.worldmodel->hulls[1].clip_mins, mins);
		VectorCopy (cl.worldmodel->hulls[1].clip_maxs, maxs);
		return;
	}

	mins[0] = -16;
	mins[1] = -16;
	mins[2] = -24;
	maxs[0] = 16;
	maxs[1] = 16;
	maxs[2] = 32;
}

static trace_t CL_Predict_TraceBox (const vec3_t start, const vec3_t end, const vec3_t mins, const vec3_t maxs, int typeflags)
{
	trace_t trace;
	vec3_t start_copy;
	vec3_t end_copy;
	vec3_t mins_copy;
	vec3_t maxs_copy;
	qmodel_t *saved;
	qcvm_t *oldvm;

	memset (&trace, 0, sizeof(trace));
	trace.fraction = 1.0f;
	VectorCopy (end, trace.endpos);

	if (!cl.worldmodel)
		return trace;

	VectorCopy (start, start_copy);
	VectorCopy (end, end_copy);
	VectorCopy (mins, mins_copy);
	VectorCopy (maxs, maxs_copy);

	saved = sv.worldmodel;
	sv.worldmodel = cl.worldmodel;
	PR_PushQCVM(&sv.qcvm, &oldvm);
	trace = SV_Move (start_copy, mins_copy, maxs_copy, end_copy, typeflags, NULL);
	// NUM_FOR_EDICT depends on qcvm; compute ent->num before popping sv.qcvm.
	trace.entnum = trace.ent ? NUM_FOR_EDICT (trace.ent) : 0;
	PR_PopQCVM(oldvm);
	sv.worldmodel = saved;

	return trace;
}

static void CL_Predict_LogSolidTrace (const char *context, const trace_t *trace)
{
	if (cl_pred_warned_solid || !cl_netdebug_parse.value)
		return;

	if (trace->startsolid || trace->allsolid)
	{
		Con_Printf ("NETDBG: prediction trace %s startsolid=%d allsolid=%d\n",
			context ? context : "unknown", trace->startsolid, trace->allsolid);
		cl_pred_warned_solid = true;
	}
}

static int CL_Predict_TraceEntNum (const trace_t *trace)
{
	if (!trace->ent)
		return 0;
	return trace->entnum;
}

static qboolean CL_Predict_GetGroundTrace (const vec3_t origin, const vec3_t mins, const vec3_t maxs, int *groundent)
{
	trace_t trace;
	vec3_t end;

	VectorCopy (origin, end);
	end[2] -= CL_PREDICT_GROUND_EPSILON;

	trace = CL_Predict_TraceBox (origin, end, mins, maxs, MOVE_NOMONSTERS);

	if (trace.startsolid || trace.allsolid)
		return false;

	if (trace.fraction < 1.0f && trace.plane.normal[2] > 0.7f)
	{
		if (groundent)
			*groundent = CL_Predict_TraceEntNum (&trace);
		return true;
	}

	return false;
}

static qboolean CL_Predict_GetGroundMotion (cl_pred_state_t *state, int groundent, float dt, vec3_t out_delta, float *out_yaw_delta)
{
	entity_t *ent;
	double msg_dt;
	float scale;
	int i;

	VectorClear (out_delta);
	if (out_yaw_delta)
		*out_yaw_delta = 0.0f;

	if (!CL_Predict_IsGroundEntityValid (groundent))
	{
		if (cl_netdebug_parse.value)
		{
			Con_Printf ("NETDBG: pred ground invalid ent=%d mtime=%.3f\n",
				groundent, cl.mtime[0]);
		}
		return false;
	}

	msg_dt = cl.mtime[0] - cl.mtime[1];
	if (msg_dt <= 0.0)
		return false;

	ent = &cl_entities[groundent];
	if (!state || !state->ground_cache.valid || state->ground_cache.id != groundent || state->ground_cache.last_time != cl.mtime[1])
	{
		if (state)
		{
			state->ground_cache.id = groundent;
			VectorCopy (ent->msg_origins[1], state->ground_cache.last_origin);
			VectorCopy (ent->msg_angles[1], state->ground_cache.last_angles);
			state->ground_cache.last_time = cl.mtime[1];
			state->ground_cache.valid = true;
			state->ground_valid = false;
		}
		return false;
	}

	scale = (float)(dt / msg_dt);
	for (i = 0; i < 3; i++)
		out_delta[i] = (ent->msg_origins[0][i] - state->ground_cache.last_origin[i]) * scale;

	if (out_yaw_delta)
	{
		float yaw_delta = CL_Predict_AngleDelta (ent->msg_angles[0][YAW], state->ground_cache.last_angles[YAW]);
		*out_yaw_delta = yaw_delta * scale;
	}

	return true;
}

static qboolean CL_Predict_GroundDeltaIsValid (const vec3_t delta, float yaw_delta)
{
	int i;

	if (!isfinite (yaw_delta))
		return false;
	for (i = 0; i < 3; i++)
	{
		if (!isfinite (delta[i]))
			return false;
	}
	return true;
}

static qboolean CL_Predict_ApplyGroundMotion (cl_pred_state_t *state, int groundent, float dt, vec3_t out_delta, float *out_yaw_delta)
{
	vec3_t delta;
	float yaw_delta;
	entity_t *ground;
	vec3_t rel;
	float radians;
	float c, s;
	qboolean applied;

	VectorClear (delta);
	yaw_delta = 0.0f;
	applied = false;

	if (!CL_Predict_IsGroundEntityValid (groundent))
	{
		CL_Predict_ResetGroundCache (state);
		goto done;
	}

	if (state->groundent != groundent)
	{
		state->groundent = groundent;
		CL_Predict_InvalidateGroundCache (state);
	}

	if (!CL_Predict_GetGroundMotion (state, groundent, dt, delta, &yaw_delta))
		goto done;

	if (!CL_Predict_GroundDeltaIsValid (delta, yaw_delta))
	{
		state->ground_valid = false;
		goto done;
	}

	state->ground_valid = true;
	applied = true;

	VectorAdd (state->origin, delta, state->origin);

	if (yaw_delta != 0.0f)
	{
		ground = &cl_entities[groundent];
		VectorSubtract (state->origin, ground->msg_origins[0], rel);
		radians = yaw_delta * (float)(M_PI / 180.0f);
		c = cosf (radians);
		s = sinf (radians);
		state->origin[0] = ground->msg_origins[0][0] + rel[0] * c - rel[1] * s;
		state->origin[1] = ground->msg_origins[0][1] + rel[0] * s + rel[1] * c;
		state->origin[2] = ground->msg_origins[0][2] + rel[2];
	}

done:
	if (out_delta)
		VectorCopy (delta, out_delta);
	if (out_yaw_delta)
		*out_yaw_delta = yaw_delta;
	return applied;
}

static int CL_Predict_ClipVelocity (const vec3_t in, const vec3_t normal, vec3_t out, float overbounce)
{
	float backoff;
	float change;
	int i;
	int blocked;

	blocked = 0;
	if (normal[2] > 0)
		blocked |= 1;
	if (!normal[2])
		blocked |= 2;

	backoff = DotProduct (in, normal) * overbounce;

	for (i = 0; i < 3; i++)
	{
		change = normal[i] * backoff;
		out[i] = in[i] - change;
		if (out[i] > -0.1f && out[i] < 0.1f)
			out[i] = 0;
	}

	return blocked;
}

static qboolean CL_Predict_SlideMove (cl_pred_state_t *state, const vec3_t mins, const vec3_t maxs, float dt)
{
	int bumpcount;
	int numbumps;
	int numplanes;
	vec3_t planes[CL_PREDICT_MAX_CLIP_PLANES];
	vec3_t primal_velocity;
	vec3_t original_velocity;
	vec3_t new_velocity;
	vec3_t end;
	vec3_t dir;
	trace_t trace;
	float time_left;
	float d;
	int i;
	int j;
	qboolean blocked;

	numbumps = 4;
	blocked = false;
	numplanes = 0;
	time_left = dt;

	VectorCopy (state->velocity, original_velocity);
	VectorCopy (state->velocity, primal_velocity);

	for (bumpcount = 0; bumpcount < numbumps; bumpcount++)
	{
		if (!state->velocity[0] && !state->velocity[1] && !state->velocity[2])
			break;

		VectorMA (state->origin, time_left, state->velocity, end);
		trace = CL_Predict_TraceBox (state->origin, end, mins, maxs, MOVE_NOMONSTERS);

		if (trace.startsolid || trace.allsolid)
		{
			CL_Predict_LogSolidTrace ("slide", &trace);
			VectorClear (state->velocity);
			state->onground = false;
			return true;
		}

		if (trace.fraction > 0)
		{
			VectorCopy (trace.endpos, state->origin);
			VectorCopy (state->velocity, original_velocity);
			numplanes = 0;
		}

		if (trace.fraction == 1.0f)
			break;

		blocked = true;
		time_left -= time_left * trace.fraction;

		if (numplanes >= CL_PREDICT_MAX_CLIP_PLANES)
		{
			VectorClear (state->velocity);
			return true;
		}

		VectorCopy (trace.plane.normal, planes[numplanes]);
		numplanes++;

		for (i = 0; i < numplanes; i++)
		{
			CL_Predict_ClipVelocity (original_velocity, planes[i], new_velocity, 1);
			for (j = 0; j < numplanes; j++)
			{
				if (j != i && DotProduct (new_velocity, planes[j]) < 0)
					break;
			}
			if (j == numplanes)
				break;
		}

		if (i != numplanes)
		{
			VectorCopy (new_velocity, state->velocity);
		}
		else
		{
			if (numplanes != 2)
			{
				VectorClear (state->velocity);
				return true;
			}
			CrossProduct (planes[0], planes[1], dir);
			d = DotProduct (dir, state->velocity);
			VectorScale (dir, d, state->velocity);
		}

		if (DotProduct (state->velocity, primal_velocity) <= 0)
		{
			VectorClear (state->velocity);
			return true;
		}
	}

	return blocked;
}

static void CL_Predict_StepSlideMove (cl_pred_state_t *state, const vec3_t mins, const vec3_t maxs, float dt)
{
	vec3_t original_origin;
	vec3_t original_velocity;
	vec3_t nostep_origin;
	vec3_t nostep_velocity;
	vec3_t end;
	trace_t trace;
	trace_t downtrace;
	float stepdist;
	float nostepdist;

	VectorCopy (state->origin, original_origin);
	VectorCopy (state->velocity, original_velocity);

	if (!CL_Predict_SlideMove (state, mins, maxs, dt))
		return;

	if (!state->onground)
		return;

	VectorCopy (state->origin, nostep_origin);
	VectorCopy (state->velocity, nostep_velocity);

	VectorCopy (original_origin, state->origin);
	VectorCopy (original_velocity, state->velocity);

	VectorCopy (state->origin, end);
	end[2] += CL_PREDICT_STEP_SIZE;
	trace = CL_Predict_TraceBox (state->origin, end, mins, maxs, MOVE_NOMONSTERS);
	if (trace.startsolid || trace.allsolid)
	{
		CL_Predict_LogSolidTrace ("step-up", &trace);
		VectorCopy (nostep_origin, state->origin);
		VectorCopy (nostep_velocity, state->velocity);
		return;
	}

	VectorCopy (trace.endpos, state->origin);
	state->velocity[2] = 0;

	CL_Predict_SlideMove (state, mins, maxs, dt);

	VectorCopy (state->origin, end);
	end[2] -= CL_PREDICT_STEP_SIZE;
	downtrace = CL_Predict_TraceBox (state->origin, end, mins, maxs, MOVE_NOMONSTERS);
	if (downtrace.startsolid || downtrace.allsolid)
	{
		CL_Predict_LogSolidTrace ("step-down", &downtrace);
		VectorCopy (nostep_origin, state->origin);
		VectorCopy (nostep_velocity, state->velocity);
		return;
	}

	VectorCopy (downtrace.endpos, state->origin);

	if (downtrace.plane.normal[2] < 0.7f)
	{
		VectorCopy (nostep_origin, state->origin);
		VectorCopy (nostep_velocity, state->velocity);
		return;
	}

	stepdist = DistanceSquared (original_origin, state->origin);
	nostepdist = DistanceSquared (original_origin, nostep_origin);
	if (nostepdist > stepdist)
	{
		VectorCopy (nostep_origin, state->origin);
		VectorCopy (nostep_velocity, state->velocity);
	}
}

static void CL_Predict_SimulateCmd (cl_pred_state_t *state, const usercmd_t *cmd, float dt)
{
	vec3_t forward, right, up;
	vec3_t wishvel, wishdir;
	float wishspeed;
	vec3_t mins, maxs;
	int groundent = 0;
	vec3_t ground_delta;
	float ground_yaw_delta;
	qboolean ground_applied;

	cl_pred_steps_this_frame++;
	CL_Predict_GetPlayerBounds (mins, maxs);
	VectorClear (ground_delta);
	ground_yaw_delta = 0.0f;
	ground_applied = false;

	if (!state->onground && state->velocity[2] <= 0)
		state->onground = CL_Predict_GetGroundTrace (state->origin, mins, maxs, &groundent);
	else if (state->onground)
		CL_Predict_GetGroundTrace (state->origin, mins, maxs, &groundent);

	if (state->onground)
		ground_applied = CL_Predict_ApplyGroundMotion (state, groundent, dt, ground_delta, &ground_yaw_delta);
	else
	{
		CL_Predict_ResetGroundCache (state);
		ground_applied = false;
		VectorClear (ground_delta);
		ground_yaw_delta = 0.0f;
	}

	VectorCopy (cmd->viewangles, state->viewangles);

	AngleVectors (cmd->viewangles, forward, right, up);

	wishvel[0] = forward[0] * cmd->forwardmove + right[0] * cmd->sidemove;
	wishvel[1] = forward[1] * cmd->forwardmove + right[1] * cmd->sidemove;
	wishvel[2] = forward[2] * cmd->forwardmove + right[2] * cmd->sidemove;
	wishvel[2] += cmd->upmove;

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize (wishdir);
	if (wishspeed > sv_maxspeed.value)
	{
		VectorScale (wishvel, sv_maxspeed.value / wishspeed, wishvel);
		wishspeed = sv_maxspeed.value;
	}

	if (noclip_anglehack)
	{
		VectorCopy (wishvel, state->velocity);
		VectorMA (state->origin, dt, state->velocity, state->origin);
		state->onground = false;
		return;
	}

	if (state->onground)
	{
		if (cmd->buttons & 2)
		{
			state->velocity[2] = 270;
			state->onground = false;
		}

		CL_Predict_Friction (state->velocity, dt);
		CL_Predict_Accelerate (state->velocity, wishdir, wishspeed, sv_accelerate.value, dt);
	}
	else
	{
		CL_Predict_AirAccelerate (state->velocity, wishvel, wishspeed, dt);
		state->velocity[2] -= sv_gravity.value * dt;
	}

	if (state->onground)
		CL_Predict_StepSlideMove (state, mins, maxs, dt);
	else
		CL_Predict_SlideMove (state, mins, maxs, dt);

	state->onground = CL_Predict_GetGroundTrace (state->origin, mins, maxs, &groundent);
	if (state->onground && state->velocity[2] < 0)
		state->velocity[2] = 0;

	if (state->onground)
		state->groundent = groundent;
	else
	{
		CL_Predict_ResetGroundCache (state);
	}

	cl_pred_ground_dbg.onground = state->onground;
	cl_pred_ground_dbg.groundent = state->groundent;
	cl_pred_ground_dbg.ground_valid = state->ground_valid;
	cl_pred_ground_dbg.ground_delta_len = VectorLength (ground_delta);
	cl_pred_ground_dbg.ground_yaw_delta = ground_yaw_delta;
	if (ground_applied)
		cl_pred_ground_dbg.ground_apply_pred++;
}

void CL_Predict_Clear (void)
{
	memset (&cl_pred, 0, sizeof(cl_pred));
	cl_pred_warned_solid = false;
	VectorClear (cl_pred_error);
	VectorClear (cl_pred_angle_error);
	cl_pred_steps_this_frame = 0;
	cl_pred_server_update_this_frame = false;
	CL_Predict_ResetGroundDebug ();
}

void CL_Predict_ResetGround (void)
{
	if (!cl_pred.has_base)
		return;

	CL_Predict_ResetGroundCache (&cl_pred.base);
	CL_Predict_ResetGroundCache (&cl_pred.predicted);
}

void CL_Predict_BeginFrame (void)
{
	cl_pred_steps_this_frame = 0;
	cl_pred_server_update_this_frame = false;
	CL_Predict_ResetGroundDebug ();
}

void CL_Predict_SetupCmd (usercmd_t *cmd)
{
	float dt;

	cl_netdbg_predict_ran = false;
	cmd->sequence = cl_pred.seq_latest + 1;
	cl_pred.seq_latest = cmd->sequence;
	cl_pred.cmds[cmd->sequence % CMD_RING] = *cmd;

	if (!CL_Predict_IsEnabled () || !cl_pred.has_base)
		return;

	dt = CL_Predict_GetStepTime ();
	CL_Predict_SimulateCmd (&cl_pred.predicted, cmd, dt);
	CL_Predict_ApplyToClient ();
	cl_netdbg_predict_ran = true;
}

qboolean CL_Predict_GetCmd (unsigned int seq, usercmd_t *out)
{
	usercmd_t *cmd = &cl_pred.cmds[seq % CMD_RING];

	if (cmd->sequence != seq)
		return false;

	if (out)
		*out = *cmd;
	return true;
}

void CL_Predict_ServerUpdate (unsigned int ack, const vec3_t origin, const vec3_t velocity, const vec3_t viewangles, qboolean onground)
{
	unsigned int seq;
	float correction;
	vec3_t error;
	vec3_t angle_error;
	float error_len;
	float teleport_dist = cl_pred_teleport_dist.value;
	float dt;
	int i;

	if (cl_pred.has_base && !CL_Predict_SeqNewer (ack, cl_pred.seq_acked))
		return;

	if (cl_pred.has_base && cl_netdebug_parse.value)
	{
		correction = Distance (origin, cl.simorg);
		if (correction > CL_PREDICT_CORRECTION_THRESHOLD)
		{
			Con_Printf ("NETDBG: prediction correction %.1f units (server %f %f %f, client %f %f %f)\n",
				correction,
				origin[0], origin[1], origin[2],
				cl.simorg[0], cl.simorg[1], cl.simorg[2]);
		}
	}

	cl_pred.seq_acked = ack;
	cl_pred_server_update_this_frame = true;
	if (cl_pred.has_base)
	{
		VectorSubtract (origin, cl_pred.predicted.origin, error);
		error_len = VectorLength (error);
		for (i = 0; i < 3; i++)
			angle_error[i] = CL_Predict_AngleDelta (viewangles[i], cl_pred.predicted.viewangles[i]);
		if (cl_netdbg_pred.value > 0.0f)
		{
			Con_Printf ("NETDBG: pred_error %.2f (server %.1f %.1f %.1f client %.1f %.1f %.1f)\n",
				error_len,
				origin[0], origin[1], origin[2],
				cl_pred.predicted.origin[0], cl_pred.predicted.origin[1], cl_pred.predicted.origin[2]);
		}
		if (cl_jitter_debug.value > 0.0f && error_len >= 2.0f)
		{
			Con_Printf ("JITTERDBG ground onground %d groundent %d ground_valid %d ground_delta %.2f ground_yaw %.2f apply_pred %d apply_render %d\n",
				cl_pred_ground_dbg.onground ? 1 : 0,
				cl_pred_ground_dbg.groundent,
				cl_pred_ground_dbg.ground_valid ? 1 : 0,
				cl_pred_ground_dbg.ground_delta_len,
				cl_pred_ground_dbg.ground_yaw_delta,
				cl_pred_ground_dbg.ground_apply_pred,
				cl_pred_ground_dbg.ground_apply_render);
		}
		if (teleport_dist > 0.0f && error_len > teleport_dist)
		{
			VectorClear (cl_pred_error);
			VectorClear (cl_pred_angle_error);
		}
		else
		{
			float deadzone = cl_pred_deadzone.value;
			float angle_deadzone = cl_pred_angle_deadzone.value;

			if (deadzone > 0.0f && error_len < deadzone)
				VectorClear (error);
			if (angle_deadzone > 0.0f)
			{
				for (i = 0; i < 3; i++)
				{
					if (fabsf (angle_error[i]) < angle_deadzone)
						angle_error[i] = 0.0f;
				}
			}
			VectorAdd (cl_pred_error, error, cl_pred_error);
			VectorAdd (cl_pred_angle_error, angle_error, cl_pred_angle_error);
		}
	}
	else
	{
		VectorClear (cl_pred_error);
		VectorClear (cl_pred_angle_error);
	}
	VectorCopy (origin, cl_pred.base.origin);
	VectorCopy (origin, cl.simorg);
	VectorCopy (velocity, cl_pred.base.velocity);
	VectorCopy (viewangles, cl_pred.base.viewangles);
	cl_pred.base.onground = onground;
	cl_pred.predicted = cl_pred.base;
	cl_pred.has_base = true;
	CL_Predict_InvalidateGroundCache (&cl_pred.base);
	CL_Predict_InvalidateGroundCache (&cl_pred.predicted);
	cl_pred.base.groundent = 0;
	cl_pred.predicted.groundent = 0;

	if (onground)
	{
		vec3_t mins, maxs;
		int groundent = 0;

		CL_Predict_GetPlayerBounds (mins, maxs);
		if (CL_Predict_GetGroundTrace (cl_pred.base.origin, mins, maxs, &groundent))
		{
			cl_pred.base.groundent = groundent;
			cl_pred.predicted.groundent = groundent;
		}
	}

	if (!CL_Predict_IsEnabled ())
		return;

	dt = CL_Predict_GetStepTime ();
	for (seq = ack + 1; !CL_Predict_SeqNewer (seq, cl_pred.seq_latest); seq++)
	{
		usercmd_t cmd;

		if (!CL_Predict_GetCmd (seq, &cmd))
			break;
		CL_Predict_SimulateCmd (&cl_pred.predicted, &cmd, dt);
	}

	CL_Predict_ApplyToClient ();
}

void CL_Predict_Reapply (void)
{
	if (!CL_Predict_IsEnabled () || !cl_pred.has_base)
		return;

	CL_Predict_ApplyToClient ();
}

qboolean CL_Predict_GetDebug (cl_pred_debug_t *out)
{
	if (!out)
		return false;

	memset (out, 0, sizeof(*out));
	out->prediction_steps = cl_pred_steps_this_frame;
	out->server_update_applied = cl_pred_server_update_this_frame;
	out->pred_error_len = VectorLength (cl_pred_error);
	out->pred_angle_error_len = VectorLength (cl_pred_angle_error);
	VectorCopy (cl_pred_error, out->pred_error);
	VectorCopy (cl_pred_angle_error, out->pred_angle_error);
	out->onground = cl_pred_ground_dbg.onground;
	out->groundent = cl_pred_ground_dbg.groundent;
	out->ground_valid = cl_pred_ground_dbg.ground_valid;
	out->ground_delta_len = cl_pred_ground_dbg.ground_delta_len;
	out->ground_yaw_delta = cl_pred_ground_dbg.ground_yaw_delta;
	out->ground_apply_pred = cl_pred_ground_dbg.ground_apply_pred;
	out->ground_apply_render = cl_pred_ground_dbg.ground_apply_render;

	if (!cl_pred.has_base)
		return false;

	VectorCopy (cl_pred.base.origin, out->base_origin);
	VectorCopy (cl_pred.base.viewangles, out->base_angles);
	VectorCopy (cl_pred.predicted.origin, out->predicted_origin);
	VectorCopy (cl_pred.predicted.viewangles, out->predicted_angles);
	return true;
}
