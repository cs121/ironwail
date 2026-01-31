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
extern cvar_t cl_predict;
extern cvar_t cl_cmdrate;
extern cvar_t cl_physrate;
extern cvar_t cl_jitter_debug;
extern cvar_t cl_pred_substeps;
extern cvar_t cl_pred_max_substeps;
extern cvar_t cl_pred_step_hz;
extern cvar_t cl_pred_accum_debug;

static cvar_t cl_pred_debug = {"cl_pred_debug", "0", CVAR_NONE};

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
	int		ground_valid_reason;
	float		ground_trace_fraction;
	float		ground_trace_normal_z;
	int		ground_trace_ent;
	int		ground_trace_startsolid;
	int		ground_trace_allsolid;
	int		ground_trace_fallback;
	float		wishspeed;
	float		wishvel_z;
	float		cmd_frametime;
	int		flags;
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
	int		ground_transition_count;
	qboolean	ground_transition_state;
	int		pred_ground_ent;
	vec3_t		pred_ground_offset;
	float		pred_ground_yaw_delta;
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
static qboolean cl_pred_warned_no_snapshot;
static vec3_t cl_pred_error;
static vec3_t cl_pred_angle_error;
static int cl_pred_steps_this_frame;
static qboolean cl_pred_server_update_this_frame;
static qboolean cl_pred_debug_registered;
static qboolean cl_pred_prev_enabled;
static cl_pred_ground_debug_t cl_pred_ground_dbg;
static float cl_pred_last_trace_fraction;
static float cl_pred_last_trace_normal_z;
static int cl_pred_last_trace_ent;
static int cl_pred_last_trace_startsolid;
static int cl_pred_last_trace_allsolid;
static int cl_pred_last_trace_fallback;
static double cl_pred_render_accum;
static usercmd_t cl_pred_render_cmd;
static qboolean cl_pred_render_cmd_valid;
static int cl_pred_last_substeps;
static float cl_pred_last_substep_dt;
static int cl_pred_nullcmd_injected;
static int cl_pred_angles_normalized;
static int cl_pred_apply_pred_reason;
static int cl_pred_apply_render_reason;

static float CL_Predict_GetStepTime (void);
static void CL_Predict_SimulateCmd (cl_pred_state_t *state, const usercmd_t *cmd, float dt, qboolean is_render);

#define CL_PREDICT_MAX_CLIP_PLANES 5
#define CL_PREDICT_STEP_SIZE 18.0f
#define CL_PREDICT_GROUND_EPSILON 2.0f
#define CL_PREDICT_CORRECTION_THRESHOLD 64.0f
#define CL_PREDICT_SNAP_THRESHOLD 8.0f
#define CL_PREDICT_GROUND_STABLE_FRAMES 2
#define CL_PREDICT_GROUND_KEEP_FRAMES 2

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

static void CL_Predict_RegisterDebugCvars (void)
{
	if (cl_pred_debug_registered)
		return;

	Cvar_RegisterVariable (&cl_pred_debug);
	cl_pred_debug_registered = true;
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
	state->ground_transition_count = 0;
	state->ground_transition_state = false;
	state->pred_ground_ent = 0;
	VectorClear (state->pred_ground_offset);
	state->pred_ground_yaw_delta = 0.0f;
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
	cl_pred_last_trace_fraction = 1.0f;
	cl_pred_last_trace_normal_z = 0.0f;
	cl_pred_last_trace_ent = 0;
	cl_pred_last_trace_startsolid = 0;
	cl_pred_last_trace_allsolid = 0;
	cl_pred_last_trace_fallback = 0;
}

void CL_Predict_SetNullCmdInjected (qboolean injected)
{
	cl_pred_nullcmd_injected = injected ? 1 : 0;
}

void CL_Predict_ForceNullCmd (void)
{
	if (!cl_pred_render_cmd_valid)
		return;

	cl_pred_render_cmd.forwardmove = 0;
	cl_pred_render_cmd.sidemove = 0;
	cl_pred_render_cmd.upmove = 0;
	cl_pred_render_cmd.buttons = 0;
	cl_pred_render_cmd.impulse = 0;
}

static float CL_Predict_GetCmdStepTime (const usercmd_t *cmd)
{
	(void)cmd;
	return CL_Predict_GetStepTime ();
}

static qboolean CL_Predict_IsLocalListenServer (void)
{
	return (sv.active && cls.state == ca_connected && !cls.demoplayback);
}

static void CL_Predict_DebugLogCmd (const char *tag, const usercmd_t *cmd, float dt)
{
	vec3_t error;
	float error_len;
	const vec3_t *base_origin;
	int is_local;

	if (cl_pred_debug.value <= 0.0f || !cmd || !cl_pred.has_base)
		return;

	base_origin = &cl_pred.base.origin;
	VectorSubtract (cl_pred.predicted.origin, *base_origin, error);
	error_len = VectorLength (error);
	is_local = CL_Predict_IsLocalListenServer () ? 1 : 0;

	Con_Printf ("PREDDBG: %s seq=%u dt=%.4f pred=%.2f %.2f %.2f base=%.2f %.2f %.2f err=%.3f local=%d\n",
		tag ? tag : "cmd",
		cmd->sequence,
		dt,
		cl_pred.predicted.origin[0],
		cl_pred.predicted.origin[1],
		cl_pred.predicted.origin[2],
		(*base_origin)[0],
		(*base_origin)[1],
		(*base_origin)[2],
		error_len,
		is_local);
}

static void CL_Predict_HardResetToBase (const char *reason)
{
	if (!cl_pred.has_base)
		return;

	cl_pred.predicted = cl_pred.base;
	VectorClear (cl_pred_error);
	VectorClear (cl_pred_angle_error);
	CL_Predict_InvalidateGroundCache (&cl_pred.base);
	CL_Predict_ResetGroundCache (&cl_pred.predicted);
	cl_pred.predicted.onground = cl_pred.base.onground;
	cl_pred.predicted.groundent = cl_pred.base.groundent;

	if (cl_pred_debug.value > 0.0f)
	{
		Con_Printf ("PREDDBG: HardReset reason=%s base=%.2f %.2f %.2f\n",
			reason ? reason : "unknown",
			cl_pred.base.origin[0],
			cl_pred.base.origin[1],
			cl_pred.base.origin[2]);
	}
}

static void CL_Predict_ApplyGroundTransition (cl_pred_state_t *state, qboolean trace_onground, int trace_groundent)
{
	if (!state)
		return;

	if (trace_onground == state->onground)
	{
		state->ground_transition_count = 0;
		state->ground_transition_state = state->onground;
		if (trace_onground)
			state->groundent = trace_groundent;
		return;
	}

	if (trace_onground)
	{
		// Accept onground transitions immediately; trace normal check already enforces safe planes.
		state->onground = true;
		state->groundent = trace_groundent;
		state->ground_transition_count = 0;
		state->ground_transition_state = state->onground;
		return;
	}

	// Leaving ground requires a brief stable window to avoid flapping.
	if (state->ground_transition_state != trace_onground)
	{
		state->ground_transition_state = trace_onground;
		state->ground_transition_count = 1;
	}
	else
	{
		state->ground_transition_count++;
	}

	if (state->ground_transition_count >= CL_PREDICT_GROUND_STABLE_FRAMES)
	{
		state->onground = false;
		state->groundent = 0;
		state->ground_transition_count = 0;
	}
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

static void CL_Predict_GetSubstepInfo (float cmd_dt, float host_dt, int *substeps, float *dt_sub)
{
	int steps = 1;
	float dt = cmd_dt;

	if (cl_pred_substeps.value > 0.0f)
	{
		// Fixed-step substepping for client prediction.
		float step_hz = cl_pred_step_hz.value > 0.0f ? cl_pred_step_hz.value : 60.0f;
		float step_dt = step_hz > 0.0f ? (1.0f / step_hz) : cmd_dt;
		int max_steps = (int)cl_pred_max_substeps.value;

		if (step_dt <= 0.0f)
			step_dt = cmd_dt;
		steps = (int)ceilf (cmd_dt / step_dt);
		if (steps < 1)
			steps = 1;
		if (max_steps < 1)
			max_steps = 1;
		if (steps > max_steps)
			steps = max_steps;
		dt = cmd_dt / (float)steps;
	}
	else
	{
		float ratio;

		if (host_dt < 0.001f || host_dt > 0.1f)
			host_dt = cmd_dt;
		if (host_dt > 0.0f)
		{
			ratio = cmd_dt / host_dt;
			if (ratio > 1.25f)
				steps = (int)floorf (ratio + 0.5f);
		}
		if (steps < 1)
			steps = 1;
		if (steps > 4)
			steps = 4;
		dt = cmd_dt / (float)steps;
	}

	if (substeps)
		*substeps = steps;
	if (dt_sub)
		*dt_sub = dt;
	cl_pred_last_substeps = steps;
	cl_pred_last_substep_dt = dt;
}

static float CL_Predict_AngleDelta (float a, float b)
{
	return AngleDeltaShortest (b, a);
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
	if (!cl_predict.value)
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
	cl_pred_state_t render_state;
	const cl_pred_state_t *state = &cl_pred.predicted;

	if (!CL_Predict_IsEnabled () || !cl_pred.has_base)
	{
		cl_pred_apply_pred_reason = !CL_Predict_IsEnabled () ? CL_PRED_APPLY_SKIP_DISABLED : CL_PRED_APPLY_SKIP_NO_BASE;
		return;
	}

	if (cl_netdebug_parse.value && !cl_pred_warned_no_snapshot
		&& (!cl.has_full_snapshot || !cl.snapshot_present || cl.mtime[0] <= 0.0))
	{
		Con_Printf ("NETDBG: prediction apply without valid snapshot (full %d present %d mtime %.3f)\n",
			cl.has_full_snapshot ? 1 : 0,
			cl.snapshot_present ? 1 : 0,
			cl.mtime[0]);
		cl_pred_warned_no_snapshot = true;
	}

	smooth_ms = cl_pred_smooth_ms.value;
	if (smooth_ms <= 0.0f)
	{
		VectorClear (cl_pred_error);
		VectorClear (cl_pred_angle_error);
	}

	if (cl_pred_substeps.value > 0.0f && cl_pred_render_cmd_valid && cl_pred_render_accum > 0.0)
	{
		float render_dt = (float)cl_pred_render_accum;
		float cmd_dt = CL_Predict_GetCmdStepTime (&cl_pred_render_cmd);

		if (cmd_dt > 0.0f && render_dt > cmd_dt)
			render_dt = cmd_dt;
		render_state = cl_pred.predicted;
		// Render-only substep to avoid frame holds between command ticks.
		CL_Predict_SimulateCmd (&render_state, &cl_pred_render_cmd, render_dt, true);
		state = &render_state;
	}

	VectorAdd (state->origin, cl_pred_error, smooth_origin);
	VectorAdd (state->viewangles, cl_pred_angle_error, smooth_angles);
	smooth_angles[0] = NormalizeAngle180 (smooth_angles[0]);
	smooth_angles[1] = NormalizeAngle180 (smooth_angles[1]);
	smooth_angles[2] = NormalizeAngle180 (smooth_angles[2]);
	VectorCopy (smooth_origin, cl.simorg);
	VectorCopy (smooth_origin, cl_entities[cl.viewentity].origin);
	VectorCopy (smooth_origin, cl_entities[cl.viewentity].msg_origins[0]);
	VectorCopy (smooth_origin, cl_entities[cl.viewentity].msg_origins[1]);
	VectorCopy (smooth_angles, cl.viewangles);
	VectorCopy (state->velocity, cl.mvelocity[0]);
	VectorCopy (state->velocity, cl.mvelocity[1]);
	cl.onground = state->onground;

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
	cl_pred_apply_render_reason = CL_PRED_APPLY_OK;
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

	cl_pred_last_trace_fraction = trace.fraction;
	cl_pred_last_trace_normal_z = trace.plane.normal[2];
	cl_pred_last_trace_ent = CL_Predict_TraceEntNum (&trace);
	cl_pred_last_trace_startsolid = trace.startsolid ? 1 : 0;
	cl_pred_last_trace_allsolid = trace.allsolid ? 1 : 0;
	cl_pred_last_trace_fallback = 0;

	if (trace.startsolid || trace.allsolid)
		return false;

	if (trace.fraction < 1.0f && trace.plane.normal[2] > 0.7f)
	{
		if (groundent)
			*groundent = CL_Predict_TraceEntNum (&trace);
		return true;
	}

	if (trace.fraction == 1.0f)
	{
		vec3_t lower_end;
		trace_t lower_trace;

		VectorCopy (origin, lower_end);
		lower_end[2] -= (CL_PREDICT_GROUND_EPSILON + 2.0f);
		lower_trace = CL_Predict_TraceBox (origin, lower_end, mins, maxs, MOVE_NOMONSTERS);
		if (!lower_trace.startsolid && !lower_trace.allsolid
			&& lower_trace.fraction < 1.0f && lower_trace.plane.normal[2] > 0.7f)
		{
			cl_pred_last_trace_fraction = lower_trace.fraction;
			cl_pred_last_trace_normal_z = lower_trace.plane.normal[2];
			cl_pred_last_trace_ent = CL_Predict_TraceEntNum (&lower_trace);
			cl_pred_last_trace_startsolid = lower_trace.startsolid ? 1 : 0;
			cl_pred_last_trace_allsolid = lower_trace.allsolid ? 1 : 0;
			cl_pred_last_trace_fallback = 1;
			if (groundent)
				*groundent = CL_Predict_TraceEntNum (&lower_trace);
			return true;
		}
	}

	return false;
}

static qboolean CL_Predict_GetGroundMotion (cl_pred_state_t *state, int groundent, float dt_step, vec3_t out_delta, float *out_yaw_delta)
{
	entity_t *ent;
	double msg_dt;
	float yaw_raw;
	int i;
	vec3_t delta_raw;
	vec3_t vel;

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
	for (i = 0; i < 3; i++)
		delta_raw[i] = ent->msg_origins[0][i] - ent->msg_origins[1][i];
	// TODO: If msg_origins[0] == msg_origins[1] consistently, the server may not be sending pusher samples.
	VectorScale (delta_raw, (float)(1.0 / msg_dt), vel);
	VectorScale (vel, dt_step, out_delta);

	yaw_raw = CL_Predict_AngleDelta (ent->msg_angles[0][YAW], ent->msg_angles[1][YAW]);
	if (out_yaw_delta)
		*out_yaw_delta = yaw_raw * (float)(dt_step / msg_dt);

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
	qboolean got_motion;

	VectorClear (delta);
	yaw_delta = 0.0f;
	applied = false;
	got_motion = false;

	if (!CL_Predict_IsGroundEntityValid (groundent))
	{
		if (cl_netdebug_parse.value)
		{
			Con_Printf ("NETDBG: pred groundent invalid ent=%d full=%d mtime=%.3f\n",
				groundent, cl.has_full_snapshot ? 1 : 0, cl.mtime[0]);
		}
		if (groundent <= 0 || groundent >= cl_max_edicts)
			SDL_assert (!"prediction groundent out of range");
		CL_Predict_ResetGroundCache (state);
		goto done;
	}

	if (state->groundent != groundent)
	{
		state->groundent = groundent;
		CL_Predict_InvalidateGroundCache (state);
	}

	if (state->pred_ground_ent != groundent)
	{
		state->pred_ground_ent = groundent;
		VectorClear (state->pred_ground_offset);
		state->pred_ground_yaw_delta = 0.0f;
	}

	if (CL_Predict_GetGroundMotion (state, groundent, dt, delta, &yaw_delta)
		&& CL_Predict_GroundDeltaIsValid (delta, yaw_delta))
	{
		got_motion = true;
		state->ground_valid = true;
		VectorCopy (delta, state->pred_ground_offset);
		state->pred_ground_yaw_delta = yaw_delta;

		if (cl_jitter_debug.value > 0.0f && groundent > 0)
		{
			entity_t *ent = &cl_entities[groundent];
			double msg_dt = cl.mtime[0] - cl.mtime[1];
			vec3_t delta_raw;
			float yaw_raw = CL_Predict_AngleDelta (ent->msg_angles[0][YAW], ent->msg_angles[1][YAW]);
			int i;

			for (i = 0; i < 3; i++)
				delta_raw[i] = ent->msg_origins[0][i] - ent->msg_origins[1][i];

			Con_Printf ("JITTERDBG groundent %d msg_origins0 %.2f %.2f %.2f msg_origins1 %.2f %.2f %.2f msg_dt %.4f yaw0 %.2f yaw1 %.2f yaw_raw %.2f platform_delta_raw %.3f %.3f %.3f ground_delta %.3f %.3f %.3f mouse_applied %d\n",
				groundent,
				ent->msg_origins[0][0], ent->msg_origins[0][1], ent->msg_origins[0][2],
				ent->msg_origins[1][0], ent->msg_origins[1][1], ent->msg_origins[1][2],
				msg_dt,
				ent->msg_angles[0][YAW], ent->msg_angles[1][YAW], yaw_raw,
				delta_raw[0], delta_raw[1], delta_raw[2],
				state->pred_ground_offset[0], state->pred_ground_offset[1], state->pred_ground_offset[2],
				IN_DidApplyMouseDelta () ? 1 : 0);
		}
	}
	else
	{
		state->ground_valid = false;
		VectorClear (state->pred_ground_offset);
		state->pred_ground_yaw_delta = 0.0f;
	}

	if (state->pred_ground_ent == groundent && got_motion)
		applied = true;

	if (got_motion)
	{
		VectorAdd (state->origin, state->pred_ground_offset, state->origin);

		if (state->pred_ground_yaw_delta != 0.0f)
		{
			ground = &cl_entities[groundent];
			VectorSubtract (state->origin, ground->msg_origins[0], rel);
			radians = state->pred_ground_yaw_delta * (float)(M_PI / 180.0f);
			c = cosf (radians);
			s = sinf (radians);
			state->origin[0] = ground->msg_origins[0][0] + rel[0] * c - rel[1] * s;
			state->origin[1] = ground->msg_origins[0][1] + rel[0] * s + rel[1] * c;
			state->origin[2] = ground->msg_origins[0][2] + rel[2];
		}
	}

done:
	if (out_delta)
		VectorCopy (state->pred_ground_offset, out_delta);
	if (out_yaw_delta)
		*out_yaw_delta = state->pred_ground_yaw_delta;
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

static void CL_Predict_SimulateCmd (cl_pred_state_t *state, const usercmd_t *cmd, float dt, qboolean is_render)
{
	vec3_t forward, right, up;
	vec3_t wishvel, wishdir;
	float wishspeed;
	vec3_t mins, maxs;
	int groundent = 0;
	vec3_t ground_delta;
	float ground_yaw_delta;
	qboolean ground_applied;
	qboolean ground_entity;
	int ground_reason = CL_GROUND_REASON_OK;

	if (!is_render)
		cl_pred_steps_this_frame++;
	CL_Predict_GetPlayerBounds (mins, maxs);
	VectorClear (ground_delta);
	ground_yaw_delta = 0.0f;
	ground_applied = false;

	if (!state->onground && state->velocity[2] <= 0)
	{
		qboolean trace_onground = CL_Predict_GetGroundTrace (state->origin, mins, maxs, &groundent);
		if (!trace_onground)
		{
			if (cl_pred_last_trace_startsolid || cl_pred_last_trace_allsolid)
				ground_reason = CL_GROUND_REASON_TRACE_SOLID;
			else if (cl_pred_last_trace_normal_z <= 0.7f && cl_pred_last_trace_fraction < 1.0f)
				ground_reason = CL_GROUND_REASON_BAD_PLANE;
			else
				ground_reason = CL_GROUND_REASON_TRACE_MISS;
		}
		CL_Predict_ApplyGroundTransition (state, trace_onground, groundent);
	}
	else if (state->onground)
	{
		qboolean trace_onground = CL_Predict_GetGroundTrace (state->origin, mins, maxs, &groundent);
		if (!trace_onground)
		{
			if (cl_pred_last_trace_startsolid || cl_pred_last_trace_allsolid)
				ground_reason = CL_GROUND_REASON_TRACE_SOLID;
			else if (cl_pred_last_trace_normal_z <= 0.7f && cl_pred_last_trace_fraction < 1.0f)
				ground_reason = CL_GROUND_REASON_BAD_PLANE;
			else
				ground_reason = CL_GROUND_REASON_TRACE_MISS;
		}
		CL_Predict_ApplyGroundTransition (state, trace_onground, groundent);
	}
	groundent = state->onground ? state->groundent : 0;

	if (state->onground && groundent == 0 && cl_netdebug_parse.value)
	{
		Con_Printf ("NETDBG: pred onground without ground entity (groundent 0)\n");
	}

	ground_entity = (state->onground && groundent > 0);
	if (ground_entity)
		ground_applied = CL_Predict_ApplyGroundMotion (state, groundent, dt, ground_delta, &ground_yaw_delta);
	else
	{
		CL_Predict_ResetGroundCache (state);
		ground_applied = false;
		VectorClear (ground_delta);
		ground_yaw_delta = 0.0f;
	}
	if (groundent <= 0)
	{
		state->ground_valid = false;
		VectorClear (ground_delta);
		ground_yaw_delta = 0.0f;
	}
	if (!state->ground_valid && state->onground && state->groundent > 0
		&& state->ground_transition_count < CL_PREDICT_GROUND_KEEP_FRAMES
		&& ground_reason != CL_GROUND_REASON_TRACE_SOLID)
	{
		state->ground_valid = true;
		ground_reason = CL_GROUND_REASON_OK;
	}
	if (cl_netdebug_parse.value && ground_entity && !state->ground_valid)
	{
		Con_Printf ("NETDBG: pred ground missing ent=%d mtime=%.3f\n",
			groundent, cl.mtime[0]);
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

	{
		qboolean trace_onground = CL_Predict_GetGroundTrace (state->origin, mins, maxs, &groundent);
		if (!trace_onground && state->onground)
		{
			if (cl_pred_last_trace_startsolid || cl_pred_last_trace_allsolid)
				ground_reason = CL_GROUND_REASON_TRACE_SOLID;
			else if (cl_pred_last_trace_normal_z <= 0.7f && cl_pred_last_trace_fraction < 1.0f)
				ground_reason = CL_GROUND_REASON_BAD_PLANE;
			else
				ground_reason = CL_GROUND_REASON_TRACE_MISS;
		}
		CL_Predict_ApplyGroundTransition (state, trace_onground, groundent);
	}
	groundent = state->onground ? state->groundent : 0;
	if (state->onground && state->velocity[2] < 0)
		state->velocity[2] = 0;

	if (state->onground)
	{
		if (state->groundent <= 0)
		{
			state->ground_valid = false;
			ground_reason = CL_GROUND_REASON_INVALID_ENTITY;
			if (cl_netdebug_parse.value)
			{
				Con_Printf ("NETDBG: pred onground without ground entity (post-move)\n");
			}
		}
	}
	else
	{
		CL_Predict_ResetGroundCache (state);
	}

	cl_pred_ground_dbg.onground = (state->onground && state->groundent > 0 && state->ground_valid);
	cl_pred_ground_dbg.groundent = state->groundent;
	cl_pred_ground_dbg.ground_valid = state->ground_valid;
	cl_pred_ground_dbg.ground_valid_reason = ground_reason;
	cl_pred_ground_dbg.ground_trace_fraction = cl_pred_last_trace_fraction;
	cl_pred_ground_dbg.ground_trace_normal_z = cl_pred_last_trace_normal_z;
	cl_pred_ground_dbg.ground_trace_ent = cl_pred_last_trace_ent;
	cl_pred_ground_dbg.ground_trace_startsolid = cl_pred_last_trace_startsolid;
	cl_pred_ground_dbg.ground_trace_allsolid = cl_pred_last_trace_allsolid;
	cl_pred_ground_dbg.ground_trace_fallback = cl_pred_last_trace_fallback;
	cl_pred_ground_dbg.wishspeed = wishspeed;
	cl_pred_ground_dbg.wishvel_z = wishvel[2];
	cl_pred_ground_dbg.cmd_frametime = dt;
	cl_pred_ground_dbg.flags = state->onground ? FL_ONGROUND : 0;
	cl_pred_ground_dbg.ground_delta_len = VectorLength (ground_delta);
	cl_pred_ground_dbg.ground_yaw_delta = ground_yaw_delta;
	if (ground_applied)
	{
		if (is_render)
			cl_pred_ground_dbg.ground_apply_render++;
		else
			cl_pred_ground_dbg.ground_apply_pred++;
	}
}

void CL_Predict_Clear (void)
{
	CL_Predict_RegisterDebugCvars ();
	memset (&cl_pred, 0, sizeof(cl_pred));
	cl_pred_warned_solid = false;
	cl_pred_warned_no_snapshot = false;
	VectorClear (cl_pred_error);
	VectorClear (cl_pred_angle_error);
	cl_pred_steps_this_frame = 0;
	cl_pred_server_update_this_frame = false;
	cl_pred_prev_enabled = false;
	CL_Predict_ResetGroundDebug ();
	cl_pred_render_accum = 0.0;
	cl_pred_render_cmd_valid = false;
	cl_pred_last_substeps = 0;
	cl_pred_last_substep_dt = 0.0f;
	cl_pred_nullcmd_injected = 0;
	cl_pred_angles_normalized = 0;
	cl_pred_apply_pred_reason = CL_PRED_APPLY_SKIP_DISABLED;
	cl_pred_apply_render_reason = CL_PRED_APPLY_SKIP_DISABLED;
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
	qboolean enabled;

	CL_Predict_RegisterDebugCvars ();
	cl_pred_steps_this_frame = 0;
	cl_pred_server_update_this_frame = false;
	CL_Predict_ResetGroundDebug ();
	cl_pred_nullcmd_injected = 0;
	cl_pred_angles_normalized = 0;

	enabled = CL_Predict_IsEnabled ();
	if (enabled && !cl_pred_prev_enabled && cl_pred.has_base)
		CL_Predict_HardResetToBase ("predict reenabled");
	cl_pred_prev_enabled = enabled;

	if (enabled && cl_pred_render_cmd_valid)
	{
		float cmd_dt = CL_Predict_GetCmdStepTime (&cl_pred_render_cmd);
		float max_accum = cmd_dt;
		if (cl_pred_substeps.value > 0.0f)
		{
			float step_hz = cl_pred_step_hz.value > 0.0f ? cl_pred_step_hz.value : 60.0f;
			float step_dt = step_hz > 0.0f ? (1.0f / step_hz) : cmd_dt;
			int max_steps = (int)cl_pred_max_substeps.value;

			if (max_steps < 1)
				max_steps = 1;
			max_accum = step_dt * (float)max_steps;
		}
		cl_pred_render_accum += host_frametime;
		if (cl_pred_render_accum < 0.0)
			cl_pred_render_accum = 0.0;
		if (max_accum > 0.0f && cl_pred_render_accum > max_accum)
			cl_pred_render_accum = max_accum;
	}
	else
	{
		cl_pred_render_accum = 0.0;
	}

	if (cl_pred_accum_debug.value > 0.0f)
	{
		Con_Printf ("PREDACCUM dt %.4f accum %.4f step_dt %.4f\n",
			host_frametime, cl_pred_render_accum, cl_pred_last_substep_dt);
	}

	if (!enabled)
		cl_pred_apply_pred_reason = CL_PRED_APPLY_SKIP_DISABLED;
	else if (!cl_pred.has_base)
		cl_pred_apply_pred_reason = CL_PRED_APPLY_SKIP_NO_BASE;
	else
		cl_pred_apply_pred_reason = CL_PRED_APPLY_SKIP_ACCUM;

	if (!enabled)
		cl_pred_apply_render_reason = CL_PRED_APPLY_SKIP_DISABLED;
	else if (!cl_pred.has_base)
		cl_pred_apply_render_reason = CL_PRED_APPLY_SKIP_NO_BASE;
	else
		cl_pred_apply_render_reason = CL_PRED_APPLY_SKIP_ACCUM;
}

void CL_Predict_SetupCmd (usercmd_t *cmd)
{
	float cmd_dt;
	float dt_sub;
	float host_dt;
	int substeps;
	int i;

	CL_Predict_RegisterDebugCvars ();
	cl_netdbg_predict_ran = false;
	cmd->sequence = cl_pred.seq_latest + 1;
	cl_pred.seq_latest = cmd->sequence;
	cl_pred.cmds[cmd->sequence % CMD_RING] = *cmd;
	cl_pred_angles_normalized = 1;

	if (!CL_Predict_IsEnabled () || !cl_pred.has_base)
		return;

	cmd_dt = CL_Predict_GetCmdStepTime (cmd);
	host_dt = host_frametime;
	CL_Predict_GetSubstepInfo (cmd_dt, host_dt, &substeps, &dt_sub);
	if (cl_jitter_debug.value > 0.0f)
	{
		Con_Printf ("JITTERDBG setup seq %u cmd_dt %.4f host_dt %.4f substeps %d dt_sub %.4f mouse_applied %d\n",
			cmd->sequence, cmd_dt, host_dt, substeps, dt_sub, IN_DidApplyMouseDelta () ? 1 : 0);
	}
	for (i = 0; i < substeps; i++)
		CL_Predict_SimulateCmd (&cl_pred.predicted, cmd, dt_sub, false);
	CL_Predict_DebugLogCmd ("setup", cmd, cmd_dt);
	cl_pred_render_cmd = *cmd;
	cl_pred_render_cmd_valid = true;
	cl_pred_render_accum = 0.0;
	cl_pred_apply_pred_reason = CL_PRED_APPLY_OK;
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
	float error_len = 0.0f;
	float teleport_dist = cl_pred_teleport_dist.value;
	float cmd_dt;
	float dt_sub;
	float host_dt;
	int substeps;
	int i;
	int prev_pred_ground_ent = 0;
	vec3_t prev_pred_ground_offset;
	float prev_pred_ground_yaw_delta = 0.0f;
	qboolean had_base;
	qboolean resim_cmd_valid = false;
	usercmd_t resim_cmd;

	CL_Predict_RegisterDebugCvars ();
	Q_memset (&resim_cmd, 0, sizeof(resim_cmd));
	if (cl_pred.has_base && !CL_Predict_SeqNewer (ack, cl_pred.seq_acked))
		return;

	had_base = cl_pred.has_base;
	VectorClear (prev_pred_ground_offset);
	if (cl_pred.has_base)
	{
		prev_pred_ground_ent = cl_pred.predicted.pred_ground_ent;
		VectorCopy (cl_pred.predicted.pred_ground_offset, prev_pred_ground_offset);
		prev_pred_ground_yaw_delta = cl_pred.predicted.pred_ground_yaw_delta;
	}

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
			Con_Printf ("JITTERDBG ground onground %d groundent %d ground_valid %d reason %d trace_frac %.2f trace_nz %.2f trace_ent %d trace_solid %d/%d trace_fallback %d wishspeed %.1f wishvel_z %.1f dt %.4f flags %d ground_delta %.2f ground_yaw %.2f apply_pred %d apply_render %d mouse_applied %d\n",
				cl_pred_ground_dbg.onground ? 1 : 0,
				cl_pred_ground_dbg.groundent,
				cl_pred_ground_dbg.ground_valid ? 1 : 0,
				cl_pred_ground_dbg.ground_valid_reason,
				cl_pred_ground_dbg.ground_trace_fraction,
				cl_pred_ground_dbg.ground_trace_normal_z,
				cl_pred_ground_dbg.ground_trace_ent,
				cl_pred_ground_dbg.ground_trace_startsolid,
				cl_pred_ground_dbg.ground_trace_allsolid,
				cl_pred_ground_dbg.ground_trace_fallback,
				cl_pred_ground_dbg.wishspeed,
				cl_pred_ground_dbg.wishvel_z,
				cl_pred_ground_dbg.cmd_frametime,
				cl_pred_ground_dbg.flags,
				cl_pred_ground_dbg.ground_delta_len,
				cl_pred_ground_dbg.ground_yaw_delta,
				cl_pred_ground_dbg.ground_apply_pred,
				cl_pred_ground_dbg.ground_apply_render,
				IN_DidApplyMouseDelta () ? 1 : 0);
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
	VectorCopy (velocity, cl_pred.base.velocity);
	cl_pred.base.viewangles[0] = NormalizeAngle180 (viewangles[0]);
	cl_pred.base.viewangles[1] = NormalizeAngle180 (viewangles[1]);
	cl_pred.base.viewangles[2] = NormalizeAngle180 (viewangles[2]);
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
		qboolean ground_trace;

		CL_Predict_GetPlayerBounds (mins, maxs);
		ground_trace = CL_Predict_GetGroundTrace (cl_pred.base.origin, mins, maxs, &groundent);
		if (ground_trace)
		{
			cl_pred.base.groundent = groundent;
			cl_pred.predicted.groundent = groundent;
		}
		if (groundent == 0 && cl_netdebug_parse.value)
		{
			Con_Printf ("NETDBG: server onground without ground entity (groundent 0)\n");
		}
		if (groundent > 0)
		{
			if (prev_pred_ground_ent == groundent)
			{
				cl_pred.base.pred_ground_ent = groundent;
				VectorCopy (prev_pred_ground_offset, cl_pred.base.pred_ground_offset);
				cl_pred.base.pred_ground_yaw_delta = prev_pred_ground_yaw_delta;
			}
			else
			{
				cl_pred.base.pred_ground_ent = groundent;
				VectorClear (cl_pred.base.pred_ground_offset);
				cl_pred.base.pred_ground_yaw_delta = 0.0f;
			}
			cl_pred.predicted.pred_ground_ent = cl_pred.base.pred_ground_ent;
			VectorCopy (cl_pred.base.pred_ground_offset, cl_pred.predicted.pred_ground_offset);
			cl_pred.predicted.pred_ground_yaw_delta = cl_pred.base.pred_ground_yaw_delta;
		}
		else
		{
			cl_pred.base.pred_ground_ent = 0;
			VectorClear (cl_pred.base.pred_ground_offset);
			cl_pred.base.pred_ground_yaw_delta = 0.0f;
			cl_pred.predicted.pred_ground_ent = 0;
			VectorClear (cl_pred.predicted.pred_ground_offset);
			cl_pred.predicted.pred_ground_yaw_delta = 0.0f;
		}
	}
	else
	{
		cl_pred.base.pred_ground_ent = 0;
		VectorClear (cl_pred.base.pred_ground_offset);
		cl_pred.base.pred_ground_yaw_delta = 0.0f;
		cl_pred.predicted.pred_ground_ent = 0;
		VectorClear (cl_pred.predicted.pred_ground_offset);
		cl_pred.predicted.pred_ground_yaw_delta = 0.0f;
	}

	if (!had_base)
		CL_Predict_HardResetToBase ("new base");

	if (!CL_Predict_IsEnabled ())
	{
		VectorCopy (origin, cl.simorg);
		return;
	}

	if (error_len > CL_PREDICT_SNAP_THRESHOLD)
	{
		VectorCopy (origin, cl.simorg);
		VectorClear (cl_pred_error);
		VectorClear (cl_pred_angle_error);
	}

	if (CL_Predict_SeqNewer (ack, cl_pred.seq_latest))
	{
		CL_Predict_HardResetToBase ("ack ahead of latest");
		CL_Predict_ApplyToClient ();
		return;
	}

	for (seq = ack + 1; !CL_Predict_SeqNewer (seq, cl_pred.seq_latest); seq++)
	{
		usercmd_t cmd;

		if (!CL_Predict_GetCmd (seq, &cmd))
		{
			CL_Predict_HardResetToBase ("cmd chain broken");
			CL_Predict_ApplyToClient ();
			return;
		}
		cmd_dt = CL_Predict_GetCmdStepTime (&cmd);
		host_dt = host_frametime;
		CL_Predict_GetSubstepInfo (cmd_dt, host_dt, &substeps, &dt_sub);
		if (cl_jitter_debug.value > 0.0f)
		{
			Con_Printf ("JITTERDBG resim seq %u cmd_dt %.4f host_dt %.4f substeps %d dt_sub %.4f mouse_applied %d\n",
				cmd.sequence, cmd_dt, host_dt, substeps, dt_sub, IN_DidApplyMouseDelta () ? 1 : 0);
		}
		for (i = 0; i < substeps; i++)
			CL_Predict_SimulateCmd (&cl_pred.predicted, &cmd, dt_sub, false);
		resim_cmd = cmd;
		resim_cmd_valid = true;
		CL_Predict_DebugLogCmd ("resim", &cmd, cmd_dt);
	}

	if (resim_cmd_valid)
	{
		cl_pred_render_cmd = resim_cmd;
		cl_pred_render_cmd_valid = true;
		cl_pred_render_accum = 0.0;
	}
	cl_pred_apply_pred_reason = CL_PRED_APPLY_OK;
	CL_Predict_ApplyToClient ();
}

void CL_Predict_Reapply (void)
{
	if (!CL_Predict_IsEnabled () || !cl_pred.has_base)
	{
		cl_pred_apply_render_reason = CL_PRED_APPLY_SKIP_DISABLED;
		if (cl_pred.has_base && !CL_Predict_IsEnabled ())
			cl_pred_apply_render_reason = CL_PRED_APPLY_SKIP_DISABLED;
		else if (!cl_pred.has_base)
			cl_pred_apply_render_reason = CL_PRED_APPLY_SKIP_NO_BASE;
		return;
	}

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
	out->ground_valid_reason = cl_pred_ground_dbg.ground_valid_reason;
	out->ground_trace_fraction = cl_pred_ground_dbg.ground_trace_fraction;
	out->ground_trace_normal_z = cl_pred_ground_dbg.ground_trace_normal_z;
	out->ground_trace_ent = cl_pred_ground_dbg.ground_trace_ent;
	out->ground_trace_startsolid = cl_pred_ground_dbg.ground_trace_startsolid;
	out->ground_trace_allsolid = cl_pred_ground_dbg.ground_trace_allsolid;
	out->ground_trace_fallback = cl_pred_ground_dbg.ground_trace_fallback;
	out->wishspeed = cl_pred_ground_dbg.wishspeed;
	out->wishvel_z = cl_pred_ground_dbg.wishvel_z;
	out->cmd_frametime = cl_pred_ground_dbg.cmd_frametime;
	out->flags = cl_pred_ground_dbg.flags;
	out->ground_delta_len = cl_pred_ground_dbg.ground_delta_len;
	out->ground_yaw_delta = cl_pred_ground_dbg.ground_yaw_delta;
	out->ground_apply_pred = cl_pred_ground_dbg.ground_apply_pred;
	out->ground_apply_render = cl_pred_ground_dbg.ground_apply_render;
	out->pred_accum_time = (float)cl_pred_render_accum;
	out->pred_step_dt = cl_pred_last_substep_dt;
	out->pred_substeps = cl_pred_last_substeps;
	out->pred_max_substeps = (int)cl_pred_max_substeps.value;
	out->pred_nullcmd = cl_pred_nullcmd_injected;
	out->pred_angles_normalized = cl_pred_angles_normalized;
	out->pred_apply_pred_reason = cl_pred_apply_pred_reason;
	out->pred_apply_render_reason = cl_pred_apply_render_reason;

	if (!cl_pred.has_base)
		return false;

	VectorCopy (cl_pred.base.origin, out->base_origin);
	VectorCopy (cl_pred.base.viewangles, out->base_angles);
	VectorCopy (cl_pred.predicted.origin, out->predicted_origin);
	VectorCopy (cl_pred.predicted.viewangles, out->predicted_angles);
	out->pred_angle_delta_shortest[0] = AngleDeltaShortest (cl_pred.base.viewangles[0], cl_pred.predicted.viewangles[0]);
	out->pred_angle_delta_shortest[1] = AngleDeltaShortest (cl_pred.base.viewangles[1], cl_pred.predicted.viewangles[1]);
	out->pred_angle_delta_shortest[2] = AngleDeltaShortest (cl_pred.base.viewangles[2], cl_pred.predicted.viewangles[2]);
	return true;
}
