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
	unsigned int	seq_latest;
	unsigned int	seq_acked;
	qboolean	has_base;
	cl_pred_state_t	base;
	cl_pred_state_t	predicted;
} cl_pred_t;

static cl_pred_t cl_pred;
static qboolean cl_netdbg_predict_ran;

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
	return (int)(seq - ref) > 0;
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
	if (!CL_Predict_IsEnabled () || !cl_pred.has_base)
		return;

	VectorCopy (cl_pred.predicted.origin, cl.simorg);
	VectorCopy (cl_pred.predicted.origin, cl_entities[cl.viewentity].origin);
	VectorCopy (cl_pred.predicted.origin, cl_entities[cl.viewentity].msg_origins[0]);
	VectorCopy (cl_pred.predicted.origin, cl_entities[cl.viewentity].msg_origins[1]);
	VectorCopy (cl_pred.predicted.velocity, cl.mvelocity[0]);
	VectorCopy (cl_pred.predicted.velocity, cl.mvelocity[1]);
	cl.onground = cl_pred.predicted.onground;

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

static qboolean CL_Predict_CheckGround (const vec3_t origin, const vec3_t mins, const vec3_t maxs)
{
	vec3_t point;
	qmodel_t *saved;
	int contents;
	int x;
	int y;
	float sample_z;

	if (!cl.worldmodel)
		return false;

	saved = sv.worldmodel;
	if (!sv.worldmodel)
		sv.worldmodel = cl.worldmodel;

	sample_z = origin[2] + mins[2] - 1.0f;

	VectorCopy (origin, point);
	point[2] = sample_z;
	contents = SV_PointContents (point);
	if (contents == CONTENTS_SOLID)
		goto done;

	for (x = 0; x <= 1; x++)
	{
		for (y = 0; y <= 1; y++)
		{
			point[0] = origin[0] + (x ? maxs[0] : mins[0]);
			point[1] = origin[1] + (y ? maxs[1] : mins[1]);
			point[2] = sample_z;

			contents = SV_PointContents (point);
			if (contents == CONTENTS_SOLID)
				goto done;
		}
	}

done:

	sv.worldmodel = saved;

	return contents == CONTENTS_SOLID;
}

static void CL_Predict_SimulateCmd (cl_pred_state_t *state, const usercmd_t *cmd)
{
	vec3_t forward, right, up;
	vec3_t wishvel, wishdir;
	float wishspeed;
	vec3_t mins, maxs;

	CL_Predict_GetPlayerBounds (mins, maxs);

	if (!state->onground && state->velocity[2] <= 0)
	{
		if (CL_Predict_CheckGround (state->origin, mins, maxs))
		{
			state->onground = true;
			state->velocity[2] = 0;
		}
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

	VectorMA (state->origin, host_frametime, state->velocity, state->origin);

	if (!state->onground && state->velocity[2] <= 0)
	{
		if (CL_Predict_CheckGround (state->origin, mins, maxs))
		{
			state->onground = true;
			state->velocity[2] = 0;
		}
	}
}

void CL_Predict_Clear (void)
{
	memset (&cl_pred, 0, sizeof(cl_pred));
}

void CL_Predict_SetupCmd (usercmd_t *cmd)
{
	cl_netdbg_predict_ran = false;
	cmd->sequence = cl_pred.seq_latest + 1;
	cl_pred.seq_latest = cmd->sequence;
	cl_pred.cmds[cmd->sequence % CMD_RING] = *cmd;

	if (!CL_Predict_IsEnabled () || !cl_pred.has_base)
		return;

	CL_Predict_SimulateCmd (&cl_pred.predicted, cmd);
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

	if (cl_pred.has_base && !CL_Predict_SeqNewer (ack, cl_pred.seq_acked))
		return;

	cl_pred.seq_acked = ack;
	VectorCopy (origin, cl_pred.base.origin);
	VectorCopy (origin, cl.simorg);
	VectorCopy (velocity, cl_pred.base.velocity);
	VectorCopy (viewangles, cl_pred.base.viewangles);
	cl_pred.base.onground = onground;
	cl_pred.predicted = cl_pred.base;
	cl_pred.has_base = true;

	if (!CL_Predict_IsEnabled ())
		return;

	for (seq = ack + 1; !CL_Predict_SeqNewer (seq, cl_pred.seq_latest); seq++)
	{
		usercmd_t cmd;

		if (!CL_Predict_GetCmd (seq, &cmd))
			break;
		CL_Predict_SimulateCmd (&cl_pred.predicted, &cmd);
	}

	CL_Predict_ApplyToClient ();
}

void CL_Predict_Reapply (void)
{
	if (!CL_Predict_IsEnabled () || !cl_pred.has_base)
		return;

	CL_Predict_ApplyToClient ();
}
