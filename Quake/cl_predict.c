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
extern cvar_t cl_cmd_maxbatch;

typedef struct
{
	vec3_t	origin;
	vec3_t	velocity;
	vec3_t	viewangles;
	qboolean	onground;
} cl_pred_state_t;

typedef struct
{
	usercmd_t	cmds[CMD_RING];
	double		cmd_dt[CMD_RING];
	double		pred_dt[CMD_RING];
	double		accum_phase[CMD_RING];
	unsigned int	seq_latest;
	unsigned int	seq_acked;
	double		last_ack_time;
	qboolean	has_base;
	cl_pred_state_t	base;
	cl_pred_state_t	predicted;
} cl_pred_t;

static cl_pred_t cl_pred;
static qboolean cl_netdbg_predict_ran;
static qboolean cl_pred_warned_solid;
static vec3_t cl_pred_error;
static vec3_t cl_pred_vel_error;

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

static qboolean CL_Predict_IsEnabled (void)
{
	// Prediction may be gated on world readiness, but command sending must not be.
	if (cls.state != ca_connected)
		return false;
	if (cls.demoplayback)
		return false;
	if (cl.intermission)
		return false;
	if (cls.conn_gen_parse != 0 && cls.conn_gen_parse != cls.conn_gen)
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
	vec3_t smooth_velocity;
	float corr_ms;
	float decay;
	vec3_t correction;
	vec3_t vel_correction;
	float corr_len = 0.0f;

	if (!CL_Predict_IsEnabled () || !cl_pred.has_base)
		return;

	corr_ms = cl_pred_corr_ms.value;
	if (corr_ms <= 0.0f)
		corr_ms = cl_pred_smooth_ms.value;
	if (corr_ms <= 0.0f)
	{
		VectorClear (cl_pred_error);
		VectorClear (cl_pred_vel_error);
	}

	VectorAdd (cl_pred.predicted.origin, cl_pred_error, smooth_origin);
	if (cl_pred_corr_vel.value != 0.0f)
		VectorAdd (cl_pred.predicted.velocity, cl_pred_vel_error, smooth_velocity);
	else
		VectorCopy (cl_pred.predicted.velocity, smooth_velocity);
	VectorCopy (smooth_origin, cl.simorg);
	VectorCopy (smooth_origin, cl_entities[cl.viewentity].origin);
	VectorCopy (smooth_origin, cl_entities[cl.viewentity].msg_origins[0]);
	VectorCopy (smooth_origin, cl_entities[cl.viewentity].msg_origins[1]);
	VectorCopy (smooth_velocity, cl.mvelocity[0]);
	VectorCopy (smooth_velocity, cl.mvelocity[1]);
	cl.onground = cl_pred.predicted.onground;

	if (corr_ms > 0.0f)
	{
		decay = host_frametime / (corr_ms * 0.001f);
		decay = CLAMP (0.0f, decay, 1.0f);
		VectorScale (cl_pred_error, decay, correction);
		VectorSubtract (cl_pred_error, correction, cl_pred_error);
		corr_len = VectorLength (correction);
		if (cl_pred_corr_vel.value != 0.0f)
		{
			VectorScale (cl_pred_vel_error, decay, vel_correction);
			VectorSubtract (cl_pred_vel_error, vel_correction, cl_pred_vel_error);
		}
		if (cl_netdbg_pred.value > 0.0f)
		{
			Con_Printf ("NETDBG: pred_corr apply %.2f remaining %.2f window %.1fms\n",
				corr_len, VectorLength (cl_pred_error), corr_ms);
		}
	}

	cl.pred_correction_applied += corr_len;
	cl.pred_correction_remaining = VectorLength (cl_pred_error);
	cl.pred_error_mag = cl.pred_correction_remaining;
	cl.pred_snapshot_age = (cl.snap_last_arrival_time > 0.0) ? (realtime - cl.snap_last_arrival_time) : 0.0;
	cl.pred_cmd_age = (cl_pred.last_ack_time > 0.0) ? (realtime - cl_pred.last_ack_time) : 0.0;

	CL_EnsureViewEntityOrigin ("predict");
}

static void CL_Predict_Friction (vec3_t velocity)
{
	float	speed;
	float	newspeed;
	float	control;

	speed = sqrt(velocity[0]*velocity[0] + velocity[1]*velocity[1]);
	if (!speed)
		return;

	control = speed < sv_stopspeed.value ? sv_stopspeed.value : speed;
	newspeed = speed - host_frametime * control * sv_friction.value;
	if (newspeed < 0)
		newspeed = 0;
	newspeed /= speed;

	velocity[0] *= newspeed;
	velocity[1] *= newspeed;
}

static void CL_Predict_Accelerate (vec3_t velocity, const vec3_t wishdir, float wishspeed, float accel)
{
	float	addspeed;
	float	accelspeed;
	float	currentspeed;
	int		i;

	currentspeed = DotProduct (velocity, wishdir);
	addspeed = wishspeed - currentspeed;
	if (addspeed <= 0)
		return;

	accelspeed = accel * host_frametime * wishspeed;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	for (i=0 ; i<3 ; i++)
		velocity[i] += accelspeed * wishdir[i];
}

static void CL_Predict_AirAccelerate (vec3_t velocity, vec3_t wishvel, float wishspeed)
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

	accelspeed = sv_accelerate.value * wishspeed * host_frametime;
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

static qboolean CL_Predict_CheckGroundTrace (const vec3_t origin, const vec3_t mins, const vec3_t maxs)
{
	trace_t trace;
	vec3_t end;

	VectorCopy (origin, end);
	end[2] -= CL_PREDICT_GROUND_EPSILON;

	trace = CL_Predict_TraceBox (origin, end, mins, maxs, MOVE_NOMONSTERS);

	if (trace.startsolid || trace.allsolid)
		return false;

	if (trace.fraction < 1.0f && trace.plane.normal[2] > 0.7f)
		return true;

	return false;
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

static void CL_Predict_SimulateCmd (cl_pred_state_t *state, const usercmd_t *cmd)
{
	vec3_t forward, right, up;
	vec3_t wishvel, wishdir;
	float wishspeed;
	vec3_t mins, maxs;

	CL_Predict_GetPlayerBounds (mins, maxs);

	if (!state->onground && state->velocity[2] <= 0)
		state->onground = CL_Predict_CheckGroundTrace (state->origin, mins, maxs);

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
		VectorMA (state->origin, host_frametime, state->velocity, state->origin);
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

		CL_Predict_Friction (state->velocity);
		CL_Predict_Accelerate (state->velocity, wishdir, wishspeed, sv_accelerate.value);
	}
	else
	{
		CL_Predict_AirAccelerate (state->velocity, wishvel, wishspeed);
		state->velocity[2] -= sv_gravity.value * host_frametime;
	}

	if (state->onground)
		CL_Predict_StepSlideMove (state, mins, maxs, host_frametime);
	else
		CL_Predict_SlideMove (state, mins, maxs, host_frametime);

	state->onground = CL_Predict_CheckGroundTrace (state->origin, mins, maxs);
	if (state->onground && state->velocity[2] < 0)
		state->velocity[2] = 0;
}

static int CL_Predict_RunCommand (cl_pred_state_t *state, const usercmd_t *cmd, double cmd_dt, double pred_dt, int max_steps)
{
	double accum;
	double max_accum;
	int steps = 0;

	if (cmd_dt <= 0.0)
		cmd_dt = host_frametime > 0.0 ? host_frametime : 0.015;
	if (pred_dt <= 0.0)
		pred_dt = cmd_dt;
	pred_dt = CLAMP (0.001, pred_dt, 0.1);
	if (max_steps < 1)
		max_steps = 1;
	max_accum = pred_dt * (double)max_steps;
	accum = cl.pred_accumulator + cmd_dt;
	if (accum > max_accum)
		accum = max_accum;

	while (accum >= pred_dt && steps < max_steps)
	{
		host_frametime = (float)pred_dt;
		CL_Predict_SimulateCmd (state, cmd);
		accum -= pred_dt;
		steps++;
	}

	if (steps == 0 && accum > 0.0)
	{
		host_frametime = (float)accum;
		CL_Predict_SimulateCmd (state, cmd);
		accum = 0.0;
		steps = 1;
	}

	cl.pred_accumulator = accum;
	return steps;
}

void CL_Predict_Clear (void)
{
	memset (&cl_pred, 0, sizeof(cl_pred));
	cl_pred_warned_solid = false;
	VectorClear (cl_pred_error);
	VectorClear (cl_pred_vel_error);
	cl.pred_accumulator = 0.0;
	cl.pred_steps = 0;
}

void CL_Predict_SetupCmd (usercmd_t *cmd, double cmd_dt, double pred_dt)
{
	int max_steps;
	int steps;
	float prev_frametime = host_frametime;

	cl_netdbg_predict_ran = false;
	cmd->sequence = cl_pred.seq_latest + 1;
	cl_pred.seq_latest = cmd->sequence;
	cl_pred.cmds[cmd->sequence % CMD_RING] = *cmd;
	if (cmd_dt <= 0.0)
		cmd_dt = prev_frametime > 0.0f ? prev_frametime : 0.015;
	if (pred_dt <= 0.0)
		pred_dt = cmd_dt;
	pred_dt = CLAMP (0.001, pred_dt, 0.1);
	cl_pred.cmd_dt[cmd->sequence % CMD_RING] = cmd_dt;
	cl_pred.pred_dt[cmd->sequence % CMD_RING] = pred_dt;

	if (!CL_Predict_IsEnabled () || !cl_pred.has_base)
	{
		cl.pred_accumulator = 0.0;
		cl_pred.accum_phase[cmd->sequence % CMD_RING] = cl.pred_accumulator;
		return;
	}

	max_steps = (int)cl_cmd_maxbatch.value;
	if (max_steps < 1)
		max_steps = 1;
	max_steps *= 4;
	steps = CL_Predict_RunCommand (&cl_pred.predicted, cmd, cmd_dt, pred_dt, max_steps);
	cl.pred_steps += (unsigned int)steps;
	cl_pred.accum_phase[cmd->sequence % CMD_RING] = cl.pred_accumulator;
	host_frametime = (float)cmd_dt;
	CL_Predict_ApplyToClient ();
	host_frametime = prev_frametime;
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
	vec3_t vel_error;
	float error_len = 0.0f;
	float teleport_dist = cl_pred_teleport_dist.value;
	float epsilon = q_max (0.0f, cl_pred_corr_eps.value);
	float corr_ms = cl_pred_corr_ms.value > 0.0f ? cl_pred_corr_ms.value : cl_pred_smooth_ms.value;
	qboolean apply_correction = false;
	qboolean teleported = false;
	double cmd_dt;
	double step_dt;
	int max_steps;
	int steps;
	float prev_frametime = host_frametime;

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
	cl_pred.last_ack_time = realtime;
	cl.snap_last_arrival_time = realtime;
	cl.pred_snapshot_age = 0.0;
	if (cl_pred_corr_vel.value == 0.0f)
		VectorClear (cl_pred_vel_error);
	if (cl_pred.has_base)
	{
		VectorSubtract (origin, cl_pred.predicted.origin, error);
		error_len = VectorLength (error);
		cl.pred_error_raw_mag = error_len;
		apply_correction = (error_len > epsilon && corr_ms > 0.0f);
		teleported = (teleport_dist > 0.0f && error_len > teleport_dist);
		if (cl_netdbg_pred.value > 0.0f)
		{
			Con_Printf ("NETDBG: pred_error %.2f (server %.1f %.1f %.1f client %.1f %.1f %.1f)\n",
				error_len,
				origin[0], origin[1], origin[2],
				cl_pred.predicted.origin[0], cl_pred.predicted.origin[1], cl_pred.predicted.origin[2]);
		}
		if (teleported)
		{
			VectorClear (cl_pred_error);
			VectorClear (cl_pred_vel_error);
			cl.pred_accumulator = 0.0;
			if (cl_netdbg_pred.value > 0.0f)
			{
				Con_Printf ("NETDBG: pred teleport reset err %.2f dist %.2f\n",
					error_len, teleport_dist);
			}
		}
		else if (apply_correction)
		{
			VectorAdd (cl_pred_error, error, cl_pred_error);
			if (cl_pred_corr_vel.value != 0.0f)
			{
				VectorSubtract (velocity, cl_pred.predicted.velocity, vel_error);
				VectorAdd (cl_pred_vel_error, vel_error, cl_pred_vel_error);
			}
			if (cl_netdbg_pred.value > 0.0f)
			{
				Con_Printf ("NETDBG: pred correction queued err %.2f eps %.2f window %.1fms\n",
					error_len, epsilon, corr_ms);
			}
		}
	}
	else
	{
		VectorClear (cl_pred_error);
		VectorClear (cl_pred_vel_error);
		cl.pred_error_raw_mag = 0.0f;
	}
	VectorCopy (origin, cl_pred.base.origin);
	VectorCopy (origin, cl.simorg);
	VectorCopy (velocity, cl_pred.base.velocity);
	VectorCopy (viewangles, cl_pred.base.viewangles);
	cl_pred.base.onground = onground;
	cl_pred.predicted = cl_pred.base;
	cl_pred.has_base = true;

	if (!CL_Predict_IsEnabled ())
		return;

	max_steps = (int)cl_cmd_maxbatch.value;
	if (max_steps < 1)
		max_steps = 1;
	max_steps *= 4;
	cl.pred_accumulator = 0.0;
	if (!teleported && !CL_Predict_SeqNewer (ack, cl_pred.seq_latest))
	{
		usercmd_t *ack_cmd = &cl_pred.cmds[ack % CMD_RING];

		if (ack_cmd->sequence == ack)
			cl.pred_accumulator = cl_pred.accum_phase[ack % CMD_RING];
	}

	for (seq = ack + 1; !CL_Predict_SeqNewer (seq, cl_pred.seq_latest); seq++)
	{
		usercmd_t cmd;

		if (!CL_Predict_GetCmd (seq, &cmd))
			break;
		cmd_dt = cl_pred.cmd_dt[seq % CMD_RING];
		step_dt = cl_pred.pred_dt[seq % CMD_RING];
		if (cmd_dt <= 0.0)
			cmd_dt = cl.pred_frame_dt_clamped > 0.0 ? cl.pred_frame_dt_clamped : prev_frametime;
		if (step_dt <= 0.0)
			step_dt = cl.pred_fixed_dt > 0.0 ? cl.pred_fixed_dt : cmd_dt;
		steps = CL_Predict_RunCommand (&cl_pred.predicted, &cmd, cmd_dt, step_dt, max_steps);
		cl.pred_steps += (unsigned int)steps;
		cl_pred.accum_phase[seq % CMD_RING] = cl.pred_accumulator;
	}

	host_frametime = prev_frametime;
	CL_Predict_ApplyToClient ();
	host_frametime = prev_frametime;
}

void CL_Predict_Reapply (void)
{
	if (!CL_Predict_IsEnabled () || !cl_pred.has_base)
		return;

	CL_Predict_ApplyToClient ();
}
