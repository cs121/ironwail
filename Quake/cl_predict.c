/*
Copyright (C) 2024
*/

/* Q3MINI PLAN:
 * - Add render-only net smoothing based on authoritative correction error.
 * - Log smoothing decisions behind net_dbg_q3mini.
 */

#include "quakedef.h"
#include <float.h>

extern cvar_t sv_maxspeed;
extern cvar_t sv_accelerate;
extern cvar_t sv_friction;
extern cvar_t sv_stopspeed;
extern cvar_t sv_gravity;
extern cvar_t sv_altnoclip;
extern cvar_t cl_netdebug_parse;
extern cvar_t cl_predict;
extern cvar_t cl_cmdrate;
extern cvar_t cl_physrate;
extern cvar_t cl_jitter_debug;
extern cvar_t cl_pred_substeps;
extern cvar_t cl_pred_max_substeps;
extern cvar_t cl_pred_step_hz;
extern cvar_t cl_pred_accum_debug;
extern cvar_t cl_pred_ultra;
extern cvar_t cl_pred_tick;
extern cvar_t cl_pred_eps;
extern cvar_t cl_pred_smooth;
extern cvar_t cl_pred_smooth_rate;
extern cvar_t cl_pred_snapdist;
// Q3MINI BEGIN
extern cvar_t cl_netsmooth;
extern cvar_t cl_netsmooth_time;
extern cvar_t cl_netsmooth_maxdist;
// Q3MINI END

static cvar_t cl_pred_debug = {"cl_pred_debug", "1", CVAR_NONE};
static cvar_t cl_pred_correct_angles = {"cl_pred_correct_angles", "0", CVAR_NONE};
static cvar_t cl_pred_inherit_ground = {"cl_pred_inherit_ground", "1", CVAR_ARCHIVE};
static cvar_t cl_pred_ground_yaw = {"cl_pred_ground_yaw", "0", CVAR_ARCHIVE};
static cvar_t cl_pred_accum_debug_level = {"cl_pred_accum_debug_level", "0", CVAR_NONE};
static cvar_t cl_pred_accum_maxdt = {"cl_pred_accum_maxdt", "0.25", CVAR_NONE};
static cvar_t cl_pred_accum_unbias = {"cl_pred_accum_unbias", "0", CVAR_NONE};
static cvar_t cl_pred_store_render_cmd_only = {"cl_pred_store_render_cmd_only", "0", CVAR_NONE};

#if defined(_DEBUG) || !defined(NDEBUG)
#define CL_PRED_ASSERT(condition) SDL_assert(condition)
#else
#define CL_PRED_ASSERT(condition) ((void)0)
#endif

#define CL_PRED_FRAME_RING 256

typedef struct
{
	unsigned int	seq;
	vec3_t		origin;
	vec3_t		velocity;
	vec3_t		viewangles;
	qboolean	onground;
	int		groundent;
	byte		waterlevel;
	byte		pm_flags;
	qboolean	valid;
} cl_pred_frame_t;

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
	int		ground_switches;
	vec3_t		ground_origin_now;
	vec3_t		ground_origin_prev;
	int		delta_applied;
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
	int		ground_keep;
	int		last_groundent;
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
	cl_pred_frame_t	frames[CL_PRED_FRAME_RING];
} cl_pred_t;

static cl_pred_t cl_pred;
static qboolean cl_netdbg_predict_ran;
static qboolean cl_pred_warned_solid;
static qboolean cl_pred_warned_no_snapshot;
static vec3_t cl_pred_error;
static vec3_t cl_pred_angle_error;
// Q3MINI BEGIN
static vec3_t cl_q3mini_net_error;
static float cl_q3mini_net_remaining;
static float cl_q3mini_net_duration;
static qboolean cl_q3mini_net_active;
// Q3MINI END
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
static usercmd_t cl_pred_render_cmd;
static qboolean cl_pred_render_cmd_valid;
static int cl_pred_last_substeps;
static float cl_pred_last_substep_dt;
static int cl_pred_nullcmd_injected;
static int cl_pred_angles_normalized;
static int cl_pred_apply_pred_reason;
static int cl_pred_apply_render_reason;
static double cl_pred_frame_accum;
static float cl_pred_frame_dt_last;
static double cl_pred_prev_realtime;
static int cl_pred_last_host_framecount = -1;
static qboolean cl_pred_dt_snapped;
static cl_pred_state_t cl_pred_render_from;
static cl_pred_state_t cl_pred_render_to;
static qboolean cl_pred_render_interp_valid;
static float cl_pred_render_frac;
static qboolean cl_pred_reset;
static double cl_pred_error_time;
static int cl_pred_replay_count;
static int cl_pred_snap_count;
static int cl_pred_smooth_count;
static float cl_pred_true_error_len;
static qboolean cl_pred_pred_frame_found;
static unsigned int cl_pred_last_ack_seq;
static qboolean cl_pred_warned_pred_miss;
static qboolean cl_pred_warned_overflow;
static qboolean cl_pred_warned_replay;
static double cl_pred_frame_drop_time;

#define CL_PRED_ACCUM_DT_HISTORY 120
typedef struct
{
	double		dt_sum;
	double		accum_sum;
	float		dt_min;
	float		dt_max;
	float		accum_min;
	float		accum_max;
	float		frac_min;
	float		frac_max;
	int		frames;
	int		steps_total;
	int		dropped_events;
	double		dropped_time;
	double		next_report_time;
} cl_pred_accum_stats_t;

static float cl_pred_dt_history[CL_PRED_ACCUM_DT_HISTORY];
static int cl_pred_dt_history_index;
static int cl_pred_dt_history_count;
static cl_pred_accum_stats_t cl_pred_accum_stats;

static float CL_Predict_GetStepTime (void);
static qboolean CL_Predict_IsEnabled (void);
static void CL_Predict_SimulateCmd (cl_pred_state_t *state, const usercmd_t *cmd, float dt, qboolean is_render);
static void CL_Predict_ResetRenderInterp (void);
static qboolean CL_Predict_Vec3IsFinite (const vec3_t vec);
static qboolean CL_Predict_Vec3Sane (const vec3_t v);
static qboolean CL_Predict_AnglesSane (const vec3_t a);
static qboolean CL_Predict_EntityStateSane (const entity_t *e);
static void CL_Predict_ClearFrames (void);
static void CL_Predict_StoreFrame (unsigned int seq, const cl_pred_state_t *state);
static qboolean CL_Predict_GetFrame (unsigned int seq, cl_pred_frame_t *out);
static qboolean CL_Predict_GetLocalMovementState (byte *movetype, byte *waterlevel);
static void CL_Predict_AccumSelftest_f (void);

#define CL_PREDICT_MAX_CLIP_PLANES 5
#define CL_PREDICT_STEP_SIZE 18.0f
#define CL_PREDICT_GROUND_EPSILON 2.0f
#define CL_PREDICT_CORRECTION_THRESHOLD 64.0f
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
		JITTER_LOG ("NETDBG: viewentity origin repaired (%s): simorg=%f %f %f\n",
			reason ? reason : "unknown", cl.simorg[0], cl.simorg[1], cl.simorg[2]);
	}
}

static void CL_Predict_RegisterDebugCvars (void)
{
	if (cl_pred_debug_registered)
		return;

	Cvar_RegisterVariable (&cl_pred_debug);
	Cvar_RegisterVariable (&cl_pred_correct_angles);
	Cvar_RegisterVariable (&cl_pred_inherit_ground);
	Cvar_RegisterVariable (&cl_pred_ground_yaw);
	Cvar_RegisterVariable (&cl_pred_accum_debug_level);
	Cvar_RegisterVariable (&cl_pred_accum_maxdt);
	Cvar_RegisterVariable (&cl_pred_accum_unbias);
	Cvar_RegisterVariable (&cl_pred_store_render_cmd_only);
	Cmd_AddCommand ("pred_accum_selftest", CL_Predict_AccumSelftest_f);
	cl_pred_debug_registered = true;
}

static qboolean CL_Predict_SeqNewer (unsigned int seq, unsigned int ref)
{
	return NETSEQ_GT (seq, ref);
}

static float CL_Predict_GetMaxAccumTime (void)
{
	float max_accum = cl_pred_accum_maxdt.value;

	if (!isfinite (max_accum) || max_accum <= 0.0f)
		max_accum = 0.25f;
	return max_accum;
}

static qboolean CL_Predict_DtSane (float dt, float max_dt)
{
	return isfinite (dt) && dt > 0.0f && dt < max_dt;
}

static qboolean CL_Predict_AccumSummaryEnabled (void)
{
	return (cl_pred_accum_debug_level.value >= 1.0f) || (cl_pred_accum_debug.value > 0.0f);
}

static qboolean CL_Predict_AccumVerboseEnabled (void)
{
	return (cl_pred_accum_debug_level.value >= 2.0f);
}

static void CL_Predict_ResetAccumStats (void)
{
	memset (&cl_pred_accum_stats, 0, sizeof(cl_pred_accum_stats));
	cl_pred_accum_stats.dt_min = FLT_MAX;
	cl_pred_accum_stats.accum_min = FLT_MAX;
	cl_pred_accum_stats.frac_min = FLT_MAX;
	cl_pred_accum_stats.dt_max = 0.0f;
	cl_pred_accum_stats.accum_max = 0.0f;
	cl_pred_accum_stats.frac_max = 0.0f;
	cl_pred_accum_stats.next_report_time = 0.0;
}

static void CL_Predict_UpdateAccumStatsDt (float dt_use)
{
	if (!CL_Predict_AccumSummaryEnabled ())
		return;

	if (dt_use >= 0.0f)
	{
		cl_pred_accum_stats.dt_sum += dt_use;
		if (dt_use < cl_pred_accum_stats.dt_min)
			cl_pred_accum_stats.dt_min = dt_use;
		if (dt_use > cl_pred_accum_stats.dt_max)
			cl_pred_accum_stats.dt_max = dt_use;
	}
}

static void CL_Predict_UpdateAccumStatsFrame (float accum, float render_frac, int steps, double dropped_time)
{
	if (!CL_Predict_AccumSummaryEnabled ())
		return;

	cl_pred_accum_stats.frames++;
	cl_pred_accum_stats.accum_sum += accum;
	if (accum < cl_pred_accum_stats.accum_min)
		cl_pred_accum_stats.accum_min = accum;
	if (accum > cl_pred_accum_stats.accum_max)
		cl_pred_accum_stats.accum_max = accum;
	if (render_frac < cl_pred_accum_stats.frac_min)
		cl_pred_accum_stats.frac_min = render_frac;
	if (render_frac > cl_pred_accum_stats.frac_max)
		cl_pred_accum_stats.frac_max = render_frac;
	cl_pred_accum_stats.steps_total += steps;
	if (dropped_time > 0.0)
	{
		cl_pred_accum_stats.dropped_events++;
		cl_pred_accum_stats.dropped_time += dropped_time;
	}
}

static void CL_Predict_ReportAccumStats (void)
{
	double now;
	double dt_mean;
	double accum_mean;

	if (!CL_Predict_AccumSummaryEnabled ())
		return;
	if (cl_pred_accum_stats.frames <= 0)
		return;

	now = realtime;
	if (cl_pred_accum_stats.next_report_time <= 0.0)
		cl_pred_accum_stats.next_report_time = now + 1.0;
	if (now < cl_pred_accum_stats.next_report_time)
		return;

	dt_mean = cl_pred_accum_stats.dt_sum / (double)cl_pred_accum_stats.frames;
	accum_mean = cl_pred_accum_stats.accum_sum / (double)cl_pred_accum_stats.frames;
	Con_Printf ("PREDACCUM_SUM dt mean %.6f min %.6f max %.6f | accum mean %.6f min %.6f max %.6f | frac min %.3f max %.3f | steps %d drops %d (%.6f)\n",
		dt_mean,
		cl_pred_accum_stats.dt_min == FLT_MAX ? 0.0f : cl_pred_accum_stats.dt_min,
		cl_pred_accum_stats.dt_max,
		accum_mean,
		cl_pred_accum_stats.accum_min == FLT_MAX ? 0.0f : cl_pred_accum_stats.accum_min,
		cl_pred_accum_stats.accum_max,
		cl_pred_accum_stats.frac_min == FLT_MAX ? 0.0f : cl_pred_accum_stats.frac_min,
		cl_pred_accum_stats.frac_max,
		cl_pred_accum_stats.steps_total,
		cl_pred_accum_stats.dropped_events,
		cl_pred_accum_stats.dropped_time);
	JITTER_LOG ("PREDACCUM_SUM dt mean %.6f min %.6f max %.6f | accum mean %.6f min %.6f max %.6f | frac min %.3f max %.3f | steps %d drops %d (%.6f)\n",
		dt_mean,
		cl_pred_accum_stats.dt_min == FLT_MAX ? 0.0f : cl_pred_accum_stats.dt_min,
		cl_pred_accum_stats.dt_max,
		accum_mean,
		cl_pred_accum_stats.accum_min == FLT_MAX ? 0.0f : cl_pred_accum_stats.accum_min,
		cl_pred_accum_stats.accum_max,
		cl_pred_accum_stats.frac_min == FLT_MAX ? 0.0f : cl_pred_accum_stats.frac_min,
		cl_pred_accum_stats.frac_max,
		cl_pred_accum_stats.steps_total,
		cl_pred_accum_stats.dropped_events,
		cl_pred_accum_stats.dropped_time);

	CL_Predict_ResetAccumStats ();
	cl_pred_accum_stats.next_report_time = now + 1.0;
}

static qboolean CL_Predict_DtIsSnapped (float dt, float epsilon, float *out_target)
{
	static const float snap_rates[] = {30.0f, 60.0f, 72.0f, 90.0f, 120.0f, 144.0f, 165.0f, 240.0f};
	size_t i;

	if (dt <= 0.0f)
		return false;

	for (i = 0; i < sizeof(snap_rates) / sizeof(snap_rates[0]); i++)
	{
		float target = 1.0f / snap_rates[i];
		if (fabsf (dt - target) <= epsilon)
		{
			if (out_target)
				*out_target = target;
			return true;
		}
	}
	return false;
}

static void CL_Predict_UpdateDtSnapState (float dt_use)
{
	const float epsilon = 0.00005f;
	int i;
	int snap_count = 0;
	int sample_count;
	qboolean snapped;

	if (dt_use <= 0.0f)
		return;

	cl_pred_dt_history[cl_pred_dt_history_index] = dt_use;
	cl_pred_dt_history_index = (cl_pred_dt_history_index + 1) % CL_PRED_ACCUM_DT_HISTORY;
	if (cl_pred_dt_history_count < CL_PRED_ACCUM_DT_HISTORY)
		cl_pred_dt_history_count++;

	sample_count = cl_pred_dt_history_count;
	for (i = 0; i < sample_count; i++)
	{
		if (CL_Predict_DtIsSnapped (cl_pred_dt_history[i], epsilon, NULL))
			snap_count++;
	}

	snapped = (sample_count >= 10) && ((float)snap_count / (float)sample_count >= 0.8f);
	if (snapped != cl_pred_dt_snapped)
	{
		cl_pred_dt_snapped = snapped;
		if (CL_Predict_AccumSummaryEnabled ())
		{
			Con_Printf ("PREDACCUM snap_detect %s (%d/%d)\n", snapped ? "ON" : "OFF", snap_count, sample_count);
			JITTER_LOG ("PREDACCUM snap_detect %s (%d/%d)\n", snapped ? "ON" : "OFF", snap_count, sample_count);
		}
	}
}

static qboolean CL_Predict_Vec3IsFinite (const vec3_t vec)
{
	int i;

	for (i = 0; i < 3; i++)
	{
		if (!isfinite (vec[i]))
			return false;
	}

	return true;
}

static qboolean CL_Predict_Vec3Sane (const vec3_t v)
{
	const float max_abs = 65536.0f;
	int i;

	for (i = 0; i < 3; i++)
	{
		if (!isfinite (v[i]) || fabsf (v[i]) >= max_abs)
			return false;
	}

	return true;
}

static qboolean CL_Predict_AnglesSane (const vec3_t a)
{
	const float max_abs = 65536.0f;
	int i;

	for (i = 0; i < 3; i++)
	{
		if (!isfinite (a[i]) || fabsf (a[i]) >= max_abs)
			return false;
	}

	return true;
}

static void CL_Predict_ClearFrames (void)
{
	int i;

	for (i = 0; i < CL_PRED_FRAME_RING; i++)
	{
		cl_pred.frames[i].valid = false;
		cl_pred.frames[i].seq = ~0u;
	}
}

static void CL_Predict_StoreFrame (unsigned int seq, const cl_pred_state_t *state)
{
	cl_pred_frame_t *frame = &cl_pred.frames[seq & (CL_PRED_FRAME_RING - 1)];

	frame->seq = seq;
	VectorCopy (state->origin, frame->origin);
	VectorCopy (state->velocity, frame->velocity);
	VectorCopy (state->viewangles, frame->viewangles);
	frame->onground = state->onground;
	frame->groundent = state->groundent;
	frame->pm_flags = state->onground ? SNAP_PM_ONGROUND : 0;
	frame->waterlevel = 0;
	frame->valid = true;
}

static qboolean CL_Predict_GetFrame (unsigned int seq, cl_pred_frame_t *out)
{
	cl_pred_frame_t *frame = &cl_pred.frames[seq & (CL_PRED_FRAME_RING - 1)];

	if (!frame->valid || frame->seq != seq)
	{
		if (cl_pred_debug.value > 0.0f)
		{
			Con_DPrintf ("PredFrame mismatch: slot %u has %u expected %u\n",
				seq % CL_PRED_FRAME_RING, frame->seq, seq);
			JITTER_LOG ("PredFrame mismatch: slot %u has %u expected %u\n",
				seq % CL_PRED_FRAME_RING, frame->seq, seq);
		}
		return false;
	}
	if (out)
		*out = *frame;
	return true;
}

static qboolean CL_Predict_EntityStateSane (const entity_t *e)
{
	if (!e)
		return false;
	if (!CL_Predict_Vec3Sane (e->msg_origins[0]))
		return false;
	if (!CL_Predict_Vec3Sane (e->msg_origins[1]))
		return false;
	if (!CL_Predict_AnglesSane (e->msg_angles[0]))
		return false;
	if (!CL_Predict_AnglesSane (e->msg_angles[1]))
		return false;
	if (e->model == NULL
		&& VectorCompare (e->msg_origins[0], vec3_origin)
		&& VectorCompare (e->msg_origins[1], vec3_origin))
		return false;

	return true;
}

static qboolean CL_Predict_GetLocalMovementState (byte *movetype, byte *waterlevel)
{
	const snapshot_state_t *state;

	if (movetype)
		*movetype = MOVETYPE_WALK;
	if (waterlevel)
		*waterlevel = 0;
	if (!cl.snapshot_baseline || !cl.snapshot_present)
		return false;
	if (cl.viewentity <= 0 || cl.viewentity >= cl_max_edicts)
		return false;
	if (!cl.snapshot_present[cl.viewentity])
		return false;

	state = &cl.snapshot_baseline[cl.viewentity];
	if (movetype)
		*movetype = state->movetype;
	if (waterlevel)
		*waterlevel = state->waterlevel;
	return true;
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
	state->ground_keep = 0;
	state->last_groundent = 0;
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
	state->ground_keep = 0;
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

static qboolean CL_Predict_UseUltra (void)
{
	return (cl_pred_ultra.value > 0.0f);
}

static float CL_Predict_GetUltraStepTime (void)
{
	float tick = cl_pred_tick.value;

	if (tick <= 0.0f)
		tick = 60.0f;
	return 1.0f / tick;
}

static float CL_Predict_GetUltraEps (void)
{
	float eps = cl_pred_eps.value;

	if (eps < 0.0f)
		eps = 0.0f;
	return eps;
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
	// Q3MINI BEGIN
	VectorClear (cl_q3mini_net_error);
	cl_q3mini_net_remaining = 0.0f;
	cl_q3mini_net_duration = 0.0f;
	cl_q3mini_net_active = false;
	// Q3MINI END
	CL_Predict_InvalidateGroundCache (&cl_pred.base);
	CL_Predict_ResetGroundCache (&cl_pred.predicted);
	cl_pred.predicted.onground = cl_pred.base.onground;
	cl_pred.predicted.groundent = cl_pred.base.groundent;
	CL_Predict_ResetRenderInterp ();

	if (cl_pred_debug.value > 0.0f)
	{
		Con_Printf ("PREDDBG: HardReset reason=%s base=%.2f %.2f %.2f\n",
			reason ? reason : "unknown",
			cl_pred.base.origin[0],
			cl_pred.base.origin[1],
			cl_pred.base.origin[2]);
	}
}

static void CL_Predict_ApplyGroundTransition (cl_pred_state_t *state, qboolean trace_onground, int trace_groundent, qboolean allow_leave)
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

	if (!allow_leave)
		return;

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
	entity_t *ent;
	int i;

	if (groundent <= 0 || groundent >= cl_max_edicts)
		return false;

	if (!cl_entities)
		return false;

	if (cl.mtime[0] <= 0.0)
		return false;

	ent = &cl_entities[groundent];

	// Validate origin sanity only.
	for (i = 0; i < 3; i++)
	{
		if (!isfinite (ent->msg_origins[0][i]) || !isfinite (ent->msg_origins[1][i]))
			return false;

		if (fabs (ent->msg_origins[0][i]) > 65536 || fabs (ent->msg_origins[1][i]) > 65536)
			return false;
	}

	return true;
}

static float CL_Predict_GetStepTime (void)
{
	double cmd_rate = cl_cmdrate.value > 0.0 ? cl_cmdrate.value : 60.0;
	double cmd_dt = 1.0 / cmd_rate;

	if (CL_Predict_UseUltra ())
		return CL_Predict_GetUltraStepTime ();
	if (cl_physrate.value > 0.0)
		return (float)(1.0 / cl_physrate.value);
	return (float)cmd_dt;
}

static void CL_Predict_ResetRenderInterp (void)
{
	cl_pred_render_from = cl_pred.predicted;
	cl_pred_render_to = cl_pred.predicted;
	cl_pred_render_interp_valid = true;
	cl_pred_render_frac = 0.0f;
}

static void CL_Predict_LerpState (cl_pred_state_t *out, const cl_pred_state_t *from, const cl_pred_state_t *to, float frac)
{
	int i;

	if (!out || !from || !to)
		return;

	*out = *to;
	for (i = 0; i < 3; i++)
		out->origin[i] = from->origin[i] + (to->origin[i] - from->origin[i]) * frac;
	for (i = 0; i < 3; i++)
	{
		float delta = AngleDeltaShortest (from->viewangles[i], to->viewangles[i]);
		out->viewangles[i] = NormalizeAngle180 (from->viewangles[i] + delta * frac);
	}
}

static void CL_Predict_GetSubstepInfo (float cmd_dt, float host_dt, int *substeps, float *dt_sub)
{
	int steps = 1;
	float dt = cmd_dt;

	if (CL_Predict_UseUltra ())
	{
		dt = CL_Predict_GetUltraStepTime ();
		if (dt <= 0.0f)
			dt = cmd_dt;
		steps = 1;
		if (substeps)
			*substeps = steps;
		if (dt_sub)
			*dt_sub = dt;
		cl_pred_last_substeps = steps;
		cl_pred_last_substep_dt = dt;
		return;
	}

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

static float CL_Predict_GetFrameStepTime (float frame_dt)
{
	float step_dt;

	if (CL_Predict_UseUltra ())
		return CL_Predict_GetUltraStepTime ();
	if (cl_pred_step_hz.value > 0.0f)
	{
		float step_hz = cl_pred_step_hz.value;

		step_dt = step_hz > 0.0f ? (1.0f / step_hz) : 0.0f;
	}
	else
	{
		step_dt = CL_Predict_GetStepTime ();
	}
	if (step_dt <= 0.0f)
		step_dt = frame_dt;
	return step_dt;
}

static void CL_Predict_RunFrameSteps (void)
{
	usercmd_t cmd;
	float step_dt;
	int max_steps;
	int steps = 0;
	const float PRED_EPS = 0.0005f;
	const float max_accum_time = CL_Predict_GetMaxAccumTime ();
	float pre_accum = (float)cl_pred_frame_accum;
	float dropped_time = (float)cl_pred_frame_drop_time;
	qboolean behind = false;
	qboolean store_frame;

	if (!CL_Predict_IsEnabled () || !cl_pred.has_base)
		return;

	if (cl_pred_frame_accum > max_accum_time)
	{
		float drop = (float)(cl_pred_frame_accum - max_accum_time);
		dropped_time += drop;
		cl_pred_frame_accum = max_accum_time;
		if (CL_Predict_AccumSummaryEnabled ())
		{
			Con_Printf ("PREDACCUM clamp_run drop %.6f max %.6f\n", drop, max_accum_time);
			JITTER_LOG ("PREDACCUM clamp_run drop %.6f max %.6f\n", drop, max_accum_time);
		}
	}

	if (cl_pred_render_cmd_valid)
	{
		cmd = cl_pred_render_cmd;
	}
	else
	{
		memset (&cmd, 0, sizeof(cmd));
		VectorCopy (cl_pred.predicted.viewangles, cmd.viewangles);
	}

	if (CL_Predict_UseUltra ())
	{
		step_dt = CL_Predict_GetUltraStepTime ();
		if (step_dt <= 0.0f)
			return;
		max_steps = (int)ceilf (max_accum_time / step_dt);
		if (max_steps < 1)
			max_steps = 1;
		CL_PRED_ASSERT (step_dt > 0.0f);

		if (cl_pred_nullcmd_injected)
		{
			cmd.forwardmove = 0;
			cmd.sidemove = 0;
			cmd.upmove = 0;
			cmd.buttons = 0;
			cmd.impulse = 0;
		}

		while (cl_pred_frame_accum + PRED_EPS >= step_dt && steps < max_steps)
		{
			cl_pred_render_from = cl_pred.predicted;
			CL_Predict_SimulateCmd (&cl_pred.predicted, &cmd, step_dt, false);
			cl_pred_render_to = cl_pred.predicted;
			store_frame = (cmd.sequence > 0)
				&& (cl_pred_store_render_cmd_only.value > 0.0f ? cl_pred_render_cmd_valid : true);
			if (store_frame)
				CL_Predict_StoreFrame (cmd.sequence, &cl_pred.predicted);
			else if (CL_Predict_AccumVerboseEnabled ())
			{
				Con_Printf ("PREDACCUM store skip seq=%u render_cmd=%d\n", cmd.sequence, cl_pred_render_cmd_valid ? 1 : 0);
				JITTER_LOG ("PREDACCUM store skip seq=%u render_cmd=%d\n", cmd.sequence, cl_pred_render_cmd_valid ? 1 : 0);
			}
			cl_pred_frame_accum -= step_dt;
			if (cl_pred_frame_accum < 0.0)
				cl_pred_frame_accum = 0.0;
			steps++;
		}
	}
	else
	{
		step_dt = CL_Predict_GetFrameStepTime (cl_pred_frame_dt_last);
		if (step_dt <= 0.0f)
			return;
		max_steps = (int)ceilf (max_accum_time / step_dt);
		if (cl_pred_max_substeps.value > 0.0f && max_steps > (int)cl_pred_max_substeps.value)
			max_steps = (int)cl_pred_max_substeps.value;
		if (max_steps < 1)
			max_steps = 1;

		if (cl_pred_nullcmd_injected)
		{
			cmd.forwardmove = 0;
			cmd.sidemove = 0;
			cmd.upmove = 0;
			cmd.buttons = 0;
			cmd.impulse = 0;
		}

		while (cl_pred_frame_accum + PRED_EPS >= step_dt && steps < max_steps)
		{
			cl_pred_render_from = cl_pred.predicted;
			CL_Predict_SimulateCmd (&cl_pred.predicted, &cmd, step_dt, false);
			cl_pred_render_to = cl_pred.predicted;
			store_frame = (cmd.sequence > 0)
				&& (cl_pred_store_render_cmd_only.value > 0.0f ? cl_pred_render_cmd_valid : true);
			if (store_frame)
				CL_Predict_StoreFrame (cmd.sequence, &cl_pred.predicted);
			else if (CL_Predict_AccumVerboseEnabled ())
			{
				Con_Printf ("PREDACCUM store skip seq=%u render_cmd=%d\n", cmd.sequence, cl_pred_render_cmd_valid ? 1 : 0);
				JITTER_LOG ("PREDACCUM store skip seq=%u render_cmd=%d\n", cmd.sequence, cl_pred_render_cmd_valid ? 1 : 0);
			}
			cl_pred_frame_accum -= step_dt;
			if (cl_pred_frame_accum < 0.0f)
				cl_pred_frame_accum = 0.0f;
			steps++;
			Con_Printf ("CONT_PRED step executed acc=%f\n", cl_pred_frame_accum);
			JITTER_LOG ("CONT_PRED step executed acc=%f\n", cl_pred_frame_accum);
		}
	}

	cl_pred_last_substeps = steps;
	cl_pred_last_substep_dt = step_dt;
	if (!cl_pred_render_interp_valid)
		CL_Predict_ResetRenderInterp ();
	if (step_dt > 0.0f)
		cl_pred_render_frac = (float)(cl_pred_frame_accum / step_dt);
	if (cl_pred_frame_accum < 0.0)
		cl_pred_frame_accum = 0.0;
	if (step_dt > 0.0f && cl_pred_frame_accum + PRED_EPS >= step_dt)
	{
		behind = true;
		cl_pred_frame_accum = fmodf ((float)cl_pred_frame_accum, step_dt);
		cl_pred_render_frac = (float)(cl_pred_frame_accum / step_dt);
	}
	cl_pred_render_interp_valid = true;
	if (CL_Predict_AccumVerboseEnabled () && step_dt > 0.0f)
	{
		Con_Printf ("PREDACCUM steps=%d step_dt=%.6f pre=%.6f post=%.6f frac=%.3f behind=%d\n",
			steps, step_dt, pre_accum, (float)cl_pred_frame_accum, cl_pred_render_frac, behind ? 1 : 0);
		JITTER_LOG ("PREDACCUM steps=%d step_dt=%.6f pre=%.6f post=%.6f frac=%.3f behind=%d\n",
			steps, step_dt, pre_accum, (float)cl_pred_frame_accum, cl_pred_render_frac, behind ? 1 : 0);
	}
	if (CL_Predict_AccumVerboseEnabled ())
		CL_PRED_ASSERT (step_dt > 0.0f && cl_pred_frame_accum >= 0.0 && cl_pred_frame_accum < step_dt + 0.0001f);

	if (CL_Predict_AccumSummaryEnabled ())
	{
		Con_Printf ("ACCUM=%f RENDER_FRAC=%f\n", cl_pred_frame_accum, cl_pred_render_frac);
		JITTER_LOG ("ACCUM=%f RENDER_FRAC=%f\n", cl_pred_frame_accum, cl_pred_render_frac);
	}

	if (cl_jitter_debug.value > 0.0f)
	{
		Con_Printf ("JITTERDBG pred_accum %.4f step_dt %.4f steps %d render_frac %.3f\n",
			cl_pred_frame_accum, step_dt, steps, cl_pred_render_frac);
		JITTER_LOG ("JITTERDBG pred_accum %.4f step_dt %.4f steps %d render_frac %.3f\n",
			cl_pred_frame_accum, step_dt, steps, cl_pred_render_frac);
	}

	CL_Predict_UpdateAccumStatsFrame ((float)cl_pred_frame_accum, cl_pred_render_frac, steps, dropped_time);
	CL_Predict_ReportAccumStats ();
}

static float CL_Predict_Selftest_PatternDt (int pattern, int frame)
{
	const float dt_60 = 1.0f / 60.0f;
	const float dt_144 = 1.0f / 144.0f;

	switch (pattern)
	{
	case 0:
		return dt_60;
	case 1:
		return dt_144;
	case 2:
		return (frame & 1) ? 0.024f : 0.008f;
	case 3:
	{
		static const float jitter[] = {0.0000f, 0.0003f, -0.0002f, 0.0001f, -0.0001f, 0.0002f, -0.0003f, 0.0001f};
		const float base = dt_60;
		const float offset = jitter[frame % (int)(sizeof(jitter) / sizeof(jitter[0]))];
		return base + offset;
	}
	case 4:
		if (frame == 120)
			return 0.200f;
		if (frame == 240)
			return 0.350f;
		return dt_60;
	case 5:
		return dt_60 + ((frame % 10) == 0 ? 0.000001f : 0.0f);
	default:
		return dt_60;
	}
}

static qboolean CL_Predict_Selftest_RunPattern (const char *name, int pattern, float step_dt, float max_accum_time,
	int frames, qboolean expect_broad_frac, qboolean expect_clamp)
{
	const float PRED_EPS = 0.0005f;
	float accum = 0.0f;
	float frac_min = 1.0f;
	float frac_max = 0.0f;
	double total_time = 0.0;
	int steps_total = 0;
	int clamp_events = 0;
	double dropped_time = 0.0;
	qboolean ok = true;
	int i;

	for (i = 0; i < frames; i++)
	{
		float dt = CL_Predict_Selftest_PatternDt (pattern, i);
		float drop = 0.0f;
		int steps = 0;
		int max_steps = (int)ceilf (max_accum_time / step_dt);

		total_time += dt;
		if (dt > max_accum_time)
		{
			drop += dt - max_accum_time;
			dt = max_accum_time;
		}

		accum += dt;
		if (accum > max_accum_time)
		{
			drop += accum - max_accum_time;
			accum = max_accum_time;
		}
		if (drop > 0.0f)
		{
			clamp_events++;
			dropped_time += drop;
		}

		while (accum + PRED_EPS >= step_dt && steps < max_steps)
		{
			accum -= step_dt;
			if (accum < 0.0f)
				accum = 0.0f;
			steps++;
			steps_total++;
		}

		if (accum < 0.0f)
			accum = 0.0f;
		if (accum + PRED_EPS >= step_dt)
			accum = fmodf (accum, step_dt);

		if (accum < 0.0f || accum >= step_dt + 0.0001f)
			ok = false;

		if (step_dt > 0.0f)
		{
			float frac = accum / step_dt;
			if (frac < frac_min)
				frac_min = frac;
			if (frac > frac_max)
				frac_max = frac;
		}
	}

	{
		double expected_steps = total_time / (double)step_dt;
		int min_steps = (int)floor (expected_steps - 1.5);
		int max_steps = (int)ceil (expected_steps + 1.5);
		if (steps_total < min_steps || steps_total > max_steps)
			ok = false;
	}

	if (expect_broad_frac && (frac_max - frac_min) < 0.4f)
		ok = false;
	if (expect_clamp && clamp_events <= 0)
		ok = false;

	Con_Printf ("PREDACCUM_SELFTEST %s: %s (steps=%d frac=%.3f..%.3f clamp=%d drop=%.6f)\n",
		name, ok ? "PASS" : "FAIL", steps_total, frac_min, frac_max, clamp_events, dropped_time);
	JITTER_LOG ("PREDACCUM_SELFTEST %s: %s (steps=%d frac=%.3f..%.3f clamp=%d drop=%.6f)\n",
		name, ok ? "PASS" : "FAIL", steps_total, frac_min, frac_max, clamp_events, dropped_time);

	return ok;
}

static void CL_Predict_AccumSelftest_f (void)
{
	const float step_dt = 1.0f / 60.0f;
	const float max_accum_time = CL_Predict_GetMaxAccumTime ();
	int frames = 600;
	int pass = 0;
	int total = 0;

	total++;
	if (CL_Predict_Selftest_RunPattern ("60hz", 0, step_dt, max_accum_time, frames, true, false))
		pass++;
	total++;
	if (CL_Predict_Selftest_RunPattern ("144hz", 1, step_dt, max_accum_time, frames, true, false))
		pass++;
	total++;
	if (CL_Predict_Selftest_RunPattern ("alt_8_24ms", 2, step_dt, max_accum_time, frames, true, false))
		pass++;
	total++;
	if (CL_Predict_Selftest_RunPattern ("micro_jitter", 3, step_dt, max_accum_time, frames, true, false))
		pass++;
	total++;
	if (CL_Predict_Selftest_RunPattern ("hitch_200ms", 4, step_dt, max_accum_time, frames, true, true))
		pass++;
	total++;
	if (CL_Predict_Selftest_RunPattern ("vsync_snap", 5, step_dt, max_accum_time, frames, false, false))
		pass++;

	Con_Printf ("PREDACCUM_SELFTEST SUMMARY %d/%d PASS\n", pass, total);
	JITTER_LOG ("PREDACCUM_SELFTEST SUMMARY %d/%d PASS\n", pass, total);
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

qboolean CL_Predict_ShouldBypassInterpolation (void)
{
	if (!CL_Predict_IsEnabled ())
		return false;
	if (!cl_pred.has_base)
		return false;
	return true;
}

static void CL_Predict_ApplyToClient (const cl_pred_state_t *state, qboolean is_render)
{
	vec3_t smooth_origin;
	float smooth_ms;
	float smooth_rate;
	float decay;
	vec3_t correction;
	vec3_t angle_correction;
	float frame_dt;
	qboolean apply_angle_correction;
	qboolean apply_smoothing;
	qboolean use_ultra;

	if (!CL_Predict_IsEnabled () || !cl_pred.has_base)
	{
		if (is_render)
			cl_pred_apply_render_reason = !CL_Predict_IsEnabled () ? CL_PRED_APPLY_SKIP_DISABLED : CL_PRED_APPLY_SKIP_NO_BASE;
		else
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
		JITTER_LOG ("NETDBG: prediction apply without valid snapshot (full %d present %d mtime %.3f)\n",
			cl.has_full_snapshot ? 1 : 0,
			cl.snapshot_present ? 1 : 0,
			cl.mtime[0]);
		cl_pred_warned_no_snapshot = true;
	}

	if (!CL_Predict_Vec3IsFinite (state->origin) || !CL_Predict_Vec3IsFinite (state->velocity))
	{
		Con_Printf ("NETDBG: prediction apply with invalid state origin/velocity\n");
		JITTER_LOG ("NETDBG: prediction apply with invalid state origin/velocity\n");
		VectorClear (cl_pred_error);
		VectorClear (cl_pred_angle_error);
		return;
	}

	use_ultra = CL_Predict_UseUltra ();
	apply_smoothing = (cl_pred_smooth.value > 0.0f);
	if (use_ultra && !is_render)
		apply_smoothing = false;
	smooth_ms = cl_pred_smooth_ms.value;
	if (smooth_ms < 0.0f)
		smooth_ms = 0.0f;
	smooth_rate = cl_pred_smooth_rate.value;
	if (smooth_rate < 0.0f)
		smooth_rate = 0.0f;
	if (!apply_smoothing && (!use_ultra || is_render))
	{
		VectorClear (cl_pred_error);
		VectorClear (cl_pred_angle_error);
	}

	if (!CL_Predict_Vec3IsFinite (cl_pred_error) || !CL_Predict_Vec3IsFinite (cl_pred_angle_error))
	{
		Con_Printf ("NETDBG: prediction apply with invalid error vectors\n");
		JITTER_LOG ("NETDBG: prediction apply with invalid error vectors\n");
		VectorClear (cl_pred_error);
		VectorClear (cl_pred_angle_error);
	}

	frame_dt = (float)host_frametime;
	if (!isfinite (frame_dt) || frame_dt < 0.0f)
	{
		Con_Printf ("NETDBG: prediction apply with invalid host_frametime %.4f\n", frame_dt);
		JITTER_LOG ("NETDBG: prediction apply with invalid host_frametime %.4f\n", frame_dt);
		frame_dt = 0.0f;
	}

	apply_angle_correction = (cl_pred_correct_angles.value > 0.0f);
	if (!apply_angle_correction)
		VectorClear (cl_pred_angle_error);

	if (use_ultra && !is_render)
		VectorCopy (state->origin, smooth_origin);
	else
		VectorAdd (state->origin, cl_pred_error, smooth_origin);
	// Q3MINI BEGIN
	if (!use_ultra && is_render && cl_q3mini_net_active && cl_netsmooth.value > 0.0f)
	{
		float frac = 0.0f;

		if (cl_q3mini_net_duration > 0.0f)
			frac = cl_q3mini_net_remaining / cl_q3mini_net_duration;
		frac = CLAMP (0.0f, frac, 1.0f);
		VectorMA (smooth_origin, frac, cl_q3mini_net_error, smooth_origin);
		if (frame_dt > 0.0f)
			cl_q3mini_net_remaining -= frame_dt;
		if (cl_q3mini_net_remaining <= 0.0f)
		{
			VectorClear (cl_q3mini_net_error);
			cl_q3mini_net_active = false;
			cl_q3mini_net_remaining = 0.0f;
			cl_q3mini_net_duration = 0.0f;
		}
		if (net_dbg_q3mini.value > 0.0f)
		{
			Con_Printf ("NETDBG q3mini smooth_apply frac %.2f remain %.3f\n",
				frac, cl_q3mini_net_remaining);
			JITTER_LOG ("NETDBG q3mini smooth_apply frac %.2f remain %.3f\n",
				frac, cl_q3mini_net_remaining);
		}
	}
	// Q3MINI END
	VectorCopy (smooth_origin, cl.simorg);
	VectorCopy (smooth_origin, cl_entities[cl.viewentity].origin);
	VectorCopy (smooth_origin, cl_entities[cl.viewentity].msg_origins[0]);
	VectorCopy (smooth_origin, cl_entities[cl.viewentity].msg_origins[1]);
	if (apply_angle_correction)
	{
		vec3_t smooth_angles;

		VectorAdd (state->viewangles, cl_pred_angle_error, smooth_angles);
		VectorCopy (smooth_angles, cl.viewangles);
	}
	VectorCopy (state->velocity, cl.mvelocity[0]);
	VectorCopy (state->velocity, cl.mvelocity[1]);
	cl.onground = state->onground;

	if (apply_smoothing)
	{
		float error_before = VectorLength (cl_pred_error);

		if (smooth_rate <= 0.0f && smooth_ms > 0.0f)
			smooth_rate = 1.0f / (smooth_ms * 0.001f);
		if (smooth_rate > 0.0f)
			decay = expf (-smooth_rate * frame_dt);
		else
			decay = 0.0f;
		decay = CLAMP (0.0f, decay, 1.0f);
		VectorScale (cl_pred_error, decay, correction);
		VectorCopy (correction, cl_pred_error);
		if (apply_angle_correction)
		{
			VectorScale (cl_pred_angle_error, decay, angle_correction);
			VectorCopy (angle_correction, cl_pred_angle_error);
		}
		else
		{
			VectorClear (cl_pred_angle_error);
		}
		if (cl_netdbg_pred.value > 0.0f)
		{
			float error_after = VectorLength (cl_pred_error);

			Con_Printf ("NETDBG: pred_smooth apply %.2f remaining %.2f\n",
				error_before - error_after, error_after);
			JITTER_LOG ("NETDBG: pred_smooth apply %.2f remaining %.2f\n",
				error_before - error_after, error_after);
		}
	}

	CL_EnsureViewEntityOrigin ("predict");
	if (is_render)
		cl_pred_apply_render_reason = CL_PRED_APPLY_OK;
	else
		cl_pred_apply_pred_reason = CL_PRED_APPLY_OK;
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
		JITTER_LOG ("NETDBG: prediction trace %s startsolid=%d allsolid=%d\n",
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

static qboolean CL_Predict_TraceHasValidGroundPlane (const trace_t *trace)
{
	if (trace->fraction <= 0.0f && (trace->startsolid || trace->allsolid || trace->plane.normal[2] <= 0.0f))
		return false;
	if (trace->plane.normal[2] <= 0.7f)
		return false;
	return true;
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

	if (trace.fraction <= 0.0f && (trace.startsolid || trace.allsolid || trace.plane.normal[2] <= 0.0f))
	{
		vec3_t nudge_origin;
		trace_t nudge_trace;

		VectorCopy (origin, nudge_origin);
		nudge_origin[2] += 1.0f;
		nudge_trace = CL_Predict_TraceBox (nudge_origin, end, mins, maxs, MOVE_NOMONSTERS);
		if (CL_Predict_TraceHasValidGroundPlane (&nudge_trace) && nudge_trace.fraction < 1.0f)
		{
			trace = nudge_trace;
			cl_pred_last_trace_fraction = trace.fraction;
			cl_pred_last_trace_normal_z = trace.plane.normal[2];
			cl_pred_last_trace_ent = CL_Predict_TraceEntNum (&trace);
			cl_pred_last_trace_startsolid = trace.startsolid ? 1 : 0;
			cl_pred_last_trace_allsolid = trace.allsolid ? 1 : 0;
			cl_pred_last_trace_fallback = 1;
		}
		else
			return false;
	}

	if (trace.fraction < 1.0f && CL_Predict_TraceHasValidGroundPlane (&trace))
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
		if (lower_trace.fraction < 1.0f && CL_Predict_TraceHasValidGroundPlane (&lower_trace))
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
	vec3_t ground_now;
	vec3_t ground_prev;
	vec3_t delta;
	double t0;
	double t1;
	double sample_t;
	float frac;
	float yaw_now;
	float yaw_prev;
	float yaw_delta;

	VectorClear (out_delta);
	if (out_yaw_delta)
		*out_yaw_delta = 0.0f;
	VectorClear (cl_pred_ground_dbg.ground_origin_now);
	VectorClear (cl_pred_ground_dbg.ground_origin_prev);

	if (!state)
	{
		CL_Predict_ResetGroundCache (state);
		return false;
	}

	if (!CL_Predict_IsGroundEntityValid (groundent))
	{
		if (cl_netdebug_parse.value)
		{
			Con_Printf ("PREDDBG invalid groundent %d cache_id %d\n", groundent, state->ground_cache.id);
		}

		// Only invalidate if entity changed.
		if (state->ground_cache.id != groundent)
			state->ground_cache.valid = false;

		return false;
	}

	t0 = cl.mtime[1];
	t1 = cl.mtime[0];
	if (t1 <= t0)
	{
		CL_Predict_InvalidateGroundCache (state);
		return false;
	}

	ent = &cl_entities[groundent];
	if (state->ground_cache.valid && state->ground_cache.id == groundent && dt_step > 0.0f)
		sample_t = state->ground_cache.last_time + (double)dt_step;
	else
		sample_t = cl.time;

	if (sample_t < t0)
		sample_t = t0;
	if (sample_t > t1)
		sample_t = t1;

	frac = (float)((sample_t - t0) / (t1 - t0));
	frac = CLAMP (0.0f, frac, 1.0f);

	VectorLerp (ent->msg_origins[1], ent->msg_origins[0], frac, ground_now);
	yaw_now = LerpAngleShortest (ent->msg_angles[1][YAW], ent->msg_angles[0][YAW], frac);

	if (!state->ground_cache.valid || state->ground_cache.id != groundent)
	{
		VectorCopy (ground_now, state->ground_cache.last_origin);
		VectorCopy (ground_now, cl_pred_ground_dbg.ground_origin_now);
		VectorCopy (ground_now, cl_pred_ground_dbg.ground_origin_prev);
		state->ground_cache.last_angles[YAW] = yaw_now;
		state->ground_cache.last_time = sample_t;
		state->ground_cache.id = groundent;
		state->ground_cache.valid = true;
		cl_pred_ground_dbg.ground_delta_len = 0.0f;
		cl_pred_ground_dbg.ground_yaw_delta = 0.0f;
		cl_pred_ground_dbg.delta_applied = 0;
		VectorClear (out_delta);
		if (out_yaw_delta)
			*out_yaw_delta = 0.0f;
		return false;
	}

	VectorCopy (state->ground_cache.last_origin, ground_prev);
	yaw_prev = state->ground_cache.last_angles[YAW];
	VectorSubtract (ground_now, ground_prev, delta);
	VectorCopy (delta, out_delta);
	yaw_delta = CL_Predict_AngleDelta (yaw_now, yaw_prev);
	if (out_yaw_delta)
		*out_yaw_delta = yaw_delta;
	VectorCopy (ground_prev, cl_pred_ground_dbg.ground_origin_prev);
	VectorCopy (ground_now, cl_pred_ground_dbg.ground_origin_now);
	cl_pred_ground_dbg.ground_delta_len = VectorLength (delta);
	cl_pred_ground_dbg.ground_yaw_delta = yaw_delta;

	VectorCopy (ground_now, state->ground_cache.last_origin);
	state->ground_cache.last_angles[YAW] = yaw_now;
	state->ground_cache.last_time = sample_t;
	state->ground_cache.id = groundent;
	state->ground_cache.valid = true;

	return true;
}

static qboolean CL_Predict_PostMoveGroundSnap (cl_pred_state_t *state, const vec3_t mins, const vec3_t maxs)
{
	trace_t trace;
	vec3_t end;

	if (!state->onground || state->velocity[2] > 0.0f)
		return false;

	VectorCopy (state->origin, end);
	end[2] -= 2.0f;
	trace = CL_Predict_TraceBox (state->origin, end, mins, maxs, MOVE_NOMONSTERS);
	if (trace.fraction >= 1.0f)
		return false;
	if (!CL_Predict_TraceHasValidGroundPlane (&trace))
		return false;

	VectorCopy (trace.endpos, state->origin);
	state->velocity[2] = 0.0f;
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
	vec3_t origin_no_trans;
	vec3_t rel;
	vec3_t rel_rot;
	vec3_t origin_rot;
	float yaw_delta;
	float radians;
	float c, s;
	qboolean applied;
	qboolean got_motion;

	(void)dt;
	VectorClear (delta);
	yaw_delta = 0.0f;
	applied = false;
	got_motion = false;
	cl_pred_ground_dbg.ground_delta_len = 0.0f;
	cl_pred_ground_dbg.ground_yaw_delta = 0.0f;
	cl_pred_ground_dbg.delta_applied = 0;
	VectorClear (cl_pred_ground_dbg.ground_origin_now);
	VectorClear (cl_pred_ground_dbg.ground_origin_prev);

	if (!CL_Predict_IsGroundEntityValid (groundent))
	{
		if (cl_netdebug_parse.value)
		{
			int present = (cl.snapshot_present && groundent > 0 && groundent < cl_max_edicts) ? (cl.snapshot_present[groundent] ? 1 : 0) : -1;
			double msgtime = (groundent > 0 && groundent < cl_max_edicts) ? cl_entities[groundent].msgtime : 0.0;
			Con_Printf ("NETDBG: pred groundent invalid ent=%d present=%d ent_msg=%.3f mtime0=%.3f mtime1=%.3f sane=%d\n",
				groundent, present, msgtime, cl.mtime[0], cl.mtime[1],
				(groundent > 0 && groundent < cl_max_edicts) ? (CL_Predict_EntityStateSane (&cl_entities[groundent]) ? 1 : 0) : 0);
			JITTER_LOG ("NETDBG: pred groundent invalid ent=%d present=%d ent_msg=%.3f mtime0=%.3f mtime1=%.3f sane=%d\n",
				groundent, present, msgtime, cl.mtime[0], cl.mtime[1],
				(groundent > 0 && groundent < cl_max_edicts) ? (CL_Predict_EntityStateSane (&cl_entities[groundent]) ? 1 : 0) : 0);
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
		cl_pred_ground_dbg.ground_switches++;
	}

	if (CL_Predict_GetGroundMotion (state, groundent, dt, delta, &yaw_delta)
		&& CL_Predict_GroundDeltaIsValid (delta, yaw_delta))
	{
		got_motion = true;
		state->ground_valid = true;
		VectorCopy (delta, state->pred_ground_offset);
		state->pred_ground_yaw_delta = (cl_pred_ground_yaw.value > 0.0f) ? yaw_delta : 0.0f;

		if (cl_jitter_debug.value > 0.0f && groundent > 0)
		{
			Con_Printf ("JITTERDBG groundent %d ground_prev %.3f %.3f %.3f ground_now %.3f %.3f %.3f delta_pos %.3f %.3f %.3f delta_yaw %.3f delta_applied %d\n",
				groundent,
				state->ground_cache.last_origin[0] - state->pred_ground_offset[0],
				state->ground_cache.last_origin[1] - state->pred_ground_offset[1],
				state->ground_cache.last_origin[2] - state->pred_ground_offset[2],
				state->ground_cache.last_origin[0],
				state->ground_cache.last_origin[1],
				state->ground_cache.last_origin[2],
				state->pred_ground_offset[0],
				state->pred_ground_offset[1],
				state->pred_ground_offset[2],
				yaw_delta,
				applied ? 1 : 0);
			JITTER_LOG ("JITTERDBG groundent %d ground_prev %.3f %.3f %.3f ground_now %.3f %.3f %.3f delta_pos %.3f %.3f %.3f delta_yaw %.3f delta_applied %d\n",
				groundent,
				state->ground_cache.last_origin[0] - state->pred_ground_offset[0],
				state->ground_cache.last_origin[1] - state->pred_ground_offset[1],
				state->ground_cache.last_origin[2] - state->pred_ground_offset[2],
				state->ground_cache.last_origin[0],
				state->ground_cache.last_origin[1],
				state->ground_cache.last_origin[2],
				state->pred_ground_offset[0],
				state->pred_ground_offset[1],
				state->pred_ground_offset[2],
				yaw_delta,
				applied ? 1 : 0);
		}
	}
	else
	{
		state->ground_valid = false;
		VectorClear (state->pred_ground_offset);
		state->pred_ground_yaw_delta = 0.0f;
	}

	if (state->pred_ground_ent == groundent && got_motion && cl_pred_inherit_ground.value > 0.0f)
		applied = true;

	if (applied)
	{
		VectorAdd (state->origin, state->pred_ground_offset, state->origin);
		cl_pred_ground_dbg.delta_applied = 1;

		if (state->pred_ground_yaw_delta != 0.0f)
		{
			VectorSubtract (state->origin, state->pred_ground_offset, origin_no_trans);
			VectorSubtract (origin_no_trans, cl_pred_ground_dbg.ground_origin_prev, rel);
			radians = state->pred_ground_yaw_delta * (float)(M_PI / 180.0f);
			c = cosf (radians);
			s = sinf (radians);
			rel_rot[0] = rel[0] * c - rel[1] * s;
			rel_rot[1] = rel[0] * s + rel[1] * c;
			rel_rot[2] = rel[2];
			VectorAdd (cl_pred_ground_dbg.ground_origin_prev, rel_rot, origin_rot);
			VectorAdd (origin_rot, state->pred_ground_offset, state->origin);
		}
	}

	if (!got_motion)
	{
		cl_pred_ground_dbg.ground_delta_len = 0.0f;
		cl_pred_ground_dbg.ground_yaw_delta = 0.0f;
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
	vec3_t moveangles;
	vec3_t wishvel, wishdir;
	float wishspeed;
	vec3_t mins, maxs;
	int groundent = 0;
	vec3_t ground_delta;
	float ground_yaw_delta;
	qboolean ground_applied;
	qboolean ground_entity;
	int ground_reason = CL_GROUND_REASON_OK;
	byte movetype = MOVETYPE_WALK;
	byte waterlevel = 0;
	qboolean use_full_angles = false;
	qboolean use_alt_noclip = false;
	qboolean use_noclip = false;
	qboolean in_water = false;

	if (sys_step_debug.value > 0.0f)
		sys_step_debug_info.cl_pred_steps++;

	if (!is_render)
		cl_pred_steps_this_frame++;
	CL_Predict_GetPlayerBounds (mins, maxs);
	VectorClear (ground_delta);
	ground_yaw_delta = 0.0f;
	ground_applied = false;
	cl_pred_ground_dbg.ground_delta_len = 0.0f;
	cl_pred_ground_dbg.ground_yaw_delta = 0.0f;
	cl_pred_ground_dbg.delta_applied = 0;
	VectorClear (cl_pred_ground_dbg.ground_origin_now);
	VectorClear (cl_pred_ground_dbg.ground_origin_prev);

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
		CL_Predict_ApplyGroundTransition (state, trace_onground, groundent, false);
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
		CL_Predict_ApplyGroundTransition (state, trace_onground, groundent, false);
	}
	groundent = state->onground ? state->groundent : 0;

	if (state->onground && groundent == 0 && cl_netdebug_parse.value)
	{
		Con_Printf ("NETDBG: pred onground without ground entity (groundent 0)\n");
		JITTER_LOG ("NETDBG: pred onground without ground entity (groundent 0)\n");
	}

	ground_entity = (state->onground && groundent > 0);
	if (ground_entity && groundent == state->last_groundent)
		state->ground_keep = CL_PREDICT_GROUND_KEEP_FRAMES;
	else if (ground_entity)
		state->ground_keep = CL_PREDICT_GROUND_KEEP_FRAMES;
	else if (state->ground_keep > 0)
		state->ground_keep--;

	if (ground_entity)
	{
		if (state->last_groundent > 0 && groundent != state->last_groundent)
			CL_Predict_InvalidateGroundCache (state);
		state->last_groundent = groundent;
		ground_applied = CL_Predict_ApplyGroundMotion (state, groundent, dt, ground_delta, &ground_yaw_delta);
	}
	else
	{
		if (state->ground_keep <= 0)
		{
			CL_Predict_ResetGroundCache (state);
			state->last_groundent = 0;
		}
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
		JITTER_LOG ("NETDBG: pred ground missing ent=%d mtime=%.3f\n",
			groundent, cl.mtime[0]);
	}

	VectorCopy (cmd->viewangles, state->viewangles);

	CL_Predict_GetLocalMovementState (&movetype, &waterlevel);
	use_alt_noclip = (movetype == MOVETYPE_NOCLIP && sv_altnoclip.value > 0.0f);
	use_noclip = (movetype == MOVETYPE_NOCLIP || noclip_anglehack);
	in_water = (waterlevel >= 2 && movetype != MOVETYPE_NOCLIP);
	use_full_angles = (movetype == MOVETYPE_NOCLIP || movetype == MOVETYPE_FLY || noclip_anglehack);

	VectorCopy (cmd->viewangles, moveangles);
	if (!use_full_angles)
	{
		moveangles[PITCH] = 0.0f;
		moveangles[ROLL] = 0.0f;
	}
	AngleVectors (moveangles, forward, right, up);

	wishvel[0] = forward[0] * cmd->forwardmove + right[0] * cmd->sidemove;
	wishvel[1] = forward[1] * cmd->forwardmove + right[1] * cmd->sidemove;
	wishvel[2] = 0.0f;
	if (use_full_angles)
		wishvel[2] += forward[2] * cmd->forwardmove + right[2] * cmd->sidemove;
	if (use_full_angles || in_water)
		wishvel[2] += cmd->upmove;
	if (in_water && !cmd->forwardmove && !cmd->sidemove && !cmd->upmove)
		wishvel[2] -= 60.0f;

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize (wishdir);
	if (wishspeed > sv_maxspeed.value)
	{
		VectorScale (wishvel, sv_maxspeed.value / wishspeed, wishvel);
		wishspeed = sv_maxspeed.value;
	}

	if (use_noclip)
	{
		if (use_alt_noclip)
			wishvel[2] += cmd->upmove;
		VectorCopy (wishvel, state->velocity);
		VectorMA (state->origin, dt, state->velocity, state->origin);
		state->onground = false;
		return;
	}

	if (in_water)
	{
		float speed, newspeed, addspeed, accelspeed;

		wishspeed = VectorLength (wishvel);
		if (wishspeed > sv_maxspeed.value)
		{
			VectorScale (wishvel, sv_maxspeed.value / wishspeed, wishvel);
			wishspeed = sv_maxspeed.value;
		}
		wishspeed *= 0.7f;

		speed = VectorLength (state->velocity);
		if (speed)
		{
			newspeed = speed - dt * speed * sv_friction.value;
			if (newspeed < 0.0f)
				newspeed = 0.0f;
			VectorScale (state->velocity, newspeed / speed, state->velocity);
		}
		else
		{
			newspeed = 0.0f;
		}

		if (wishspeed > 0.0f)
		{
			addspeed = wishspeed - newspeed;
			if (addspeed > 0.0f)
			{
				VectorNormalize (wishvel);
				accelspeed = sv_accelerate.value * wishspeed * dt;
				if (accelspeed > addspeed)
					accelspeed = addspeed;
				VectorMA (state->velocity, accelspeed, wishvel, state->velocity);
			}
		}
	}
	else if (state->onground)
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

	CL_Predict_PostMoveGroundSnap (state, mins, maxs);

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
		CL_Predict_ApplyGroundTransition (state, trace_onground, groundent, true);
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
				JITTER_LOG ("NETDBG: pred onground without ground entity (post-move)\n");
			}
		}
	}
	else
	{
		if (state->ground_keep <= 0)
		{
			CL_Predict_ResetGroundCache (state);
			state->last_groundent = 0;
		}
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
	if (!ground_applied)
		cl_pred_ground_dbg.delta_applied = 0;
}

void CL_Predict_Clear (void)
{
	CL_Predict_RegisterDebugCvars ();
	memset (&cl_pred, 0, sizeof(cl_pred));
	CL_Predict_ClearFrames ();
	cl_pred_warned_solid = false;
	cl_pred_warned_no_snapshot = false;
	VectorClear (cl_pred_error);
	VectorClear (cl_pred_angle_error);
	cl_pred_steps_this_frame = 0;
	cl_pred_server_update_this_frame = false;
	cl_pred_prev_enabled = false;
	CL_Predict_ResetGroundDebug ();
	cl_pred_render_cmd_valid = false;
	cl_pred_last_substeps = 0;
	cl_pred_last_substep_dt = 0.0f;
	cl_pred_nullcmd_injected = 0;
	cl_pred_angles_normalized = 0;
	cl_pred_apply_pred_reason = CL_PRED_APPLY_SKIP_DISABLED;
	cl_pred_apply_render_reason = CL_PRED_APPLY_SKIP_DISABLED;
	cl_pred_frame_dt_last = 0.0f;
	cl_pred_frame_accum = 0.0;
	cl_pred_prev_realtime = 0.0;
	cl_pred_last_host_framecount = -1;
	cl_pred_dt_snapped = false;
	cl_pred_dt_history_index = 0;
	cl_pred_dt_history_count = 0;
	CL_Predict_ResetAccumStats ();
	cl_pred_render_interp_valid = false;
	cl_pred_render_frac = 0.0f;
	memset (&cl_pred_render_from, 0, sizeof(cl_pred_render_from));
	memset (&cl_pred_render_to, 0, sizeof(cl_pred_render_to));
	cl_pred_reset = false;
	cl_pred_error_time = 0.0;
	cl_pred_replay_count = 0;
	cl_pred_snap_count = 0;
	cl_pred_smooth_count = 0;
	cl_pred_last_ack_seq = 0;
	cl_pred_warned_pred_miss = false;
	cl_pred_warned_overflow = false;
	cl_pred_warned_replay = false;
	cl_pred_frame_drop_time = 0.0;
}

void CL_Predict_ResetGround (void)
{
	if (!cl_pred.has_base)
		return;

	CL_Predict_ResetGroundCache (&cl_pred.base);
	CL_Predict_ResetGroundCache (&cl_pred.predicted);
}

/*
Prediction accumulator contract:
- Feed exactly once per host frame when prediction is enabled and has base.
- Choose dt in priority order: realtime delta (if sane), raw frame time, host frame time,
  then clamp any outliers to max_accum_time. Sane = finite and 0 < dt < max_accum_time.
- Accumulator increases by dt_use, is clamped to max_accum_time (drop excess with logging),
  then decreased by exact step_dt per simulation step.
- Remainder is normalized to [0, step_dt) and render_frac = remainder / step_dt.
- Snap detection (e.g., exact vsync dt) is recorded; optional micro-unbias is behind a cvar.
*/
void CL_Predict_BeginFrame (void)
{
	qboolean enabled;
	qboolean was_enabled;

	CL_Predict_RegisterDebugCvars ();
	cl_pred_steps_this_frame = 0;
	cl_pred_server_update_this_frame = false;
	CL_Predict_ResetGroundDebug ();
	cl_pred_nullcmd_injected = 0;
	cl_pred_angles_normalized = 0;
	cl_pred_replay_count = 0;
	cl_pred_snap_count = 0;
	cl_pred_smooth_count = 0;
	cl_pred_true_error_len = 0.0f;
	cl_pred_pred_frame_found = false;
	cl_pred_frame_drop_time = 0.0;

	if (host_framecount == cl_pred_last_host_framecount)
	{
		if (CL_Predict_AccumVerboseEnabled ())
		{
			Con_Printf ("PREDACCUM duplicate BeginFrame host_framecount=%d\n", host_framecount);
			JITTER_LOG ("PREDACCUM duplicate BeginFrame host_framecount=%d\n", host_framecount);
		}
		return;
	}
	cl_pred_last_host_framecount = host_framecount;

	was_enabled = cl_pred_prev_enabled;
	enabled = CL_Predict_IsEnabled ();
	if (enabled && !was_enabled && cl_pred.has_base)
		CL_Predict_HardResetToBase ("predict reenabled");

	if (enabled && cl_pred.has_base)
	{
		const float max_accum_time = CL_Predict_GetMaxAccumTime ();
		const float raw_dt = (float)host_rawframetime;
		const float host_dt = (float)host_frametime;
		float real_dt = 0.0f;
		float dt_use = 0.0f;
		float pre_accum = (float)cl_pred_frame_accum;
		float post_accum;
		float step_guess;
		const char *dt_source = "none";
		qboolean dt_real_valid = (cl_pred_prev_realtime > 0.0) && isfinite (realtime) && realtime >= cl_pred_prev_realtime;
		qboolean unbias_applied = false;
		qboolean clamp_applied = false;
		float clamp_drop = 0.0f;

		if (dt_real_valid)
			real_dt = (float)(realtime - cl_pred_prev_realtime);

		if (CL_Predict_DtSane (real_dt, max_accum_time))
		{
			dt_use = real_dt;
			dt_source = "real";
		}
		else if (CL_Predict_DtSane (raw_dt, max_accum_time))
		{
			dt_use = raw_dt;
			dt_source = "raw";
		}
		else if (CL_Predict_DtSane (host_dt, max_accum_time))
		{
			dt_use = host_dt;
			dt_source = "host";
		}
		else
		{
			float fallback = 0.0f;
			if (real_dt > 0.0f && isfinite (real_dt))
				fallback = real_dt;
			else if (raw_dt > 0.0f && isfinite (raw_dt))
				fallback = raw_dt;
			else if (host_dt > 0.0f && isfinite (host_dt))
				fallback = host_dt;
			if (fallback > 0.0f)
			{
				dt_use = fallback;
				dt_source = "clamp";
			}
		}

		if (dt_use > max_accum_time)
		{
			clamp_applied = true;
			clamp_drop = dt_use - max_accum_time;
			dt_use = max_accum_time;
		}
		if (dt_use < 0.0f || !isfinite (dt_use))
			dt_use = 0.0f;

		CL_Predict_UpdateDtSnapState (dt_use);
		step_guess = cl_pred_last_substep_dt > 0.0f ? cl_pred_last_substep_dt : CL_Predict_GetFrameStepTime (dt_use > 0.0f ? dt_use : 0.016f);
		if (cl_pred_accum_unbias.value > 0.0f && cl_pred_dt_snapped && step_guess > 0.0f)
		{
			const float bias = fminf (0.00005f, step_guess * 0.001f);
			if (bias > 0.0f)
			{
				dt_use += bias;
				unbias_applied = true;
			}
		}

		if (raw_dt > 0.0f && host_dt > 0.0f && fabsf (raw_dt - host_dt) > 0.000001f)
		{
			Con_Printf ("JITTERDBG pred_accum dt clamped raw %.6f ft %.6f\n", raw_dt, host_dt);
			JITTER_LOG ("JITTERDBG pred_accum dt clamped raw %.6f ft %.6f\n", raw_dt, host_dt);
		}

		cl_pred_frame_accum += dt_use;
		if (cl_pred_frame_accum < 0.0)
			cl_pred_frame_accum = 0.0;
		if (cl_pred_frame_accum > max_accum_time)
		{
			clamp_drop += (float)(cl_pred_frame_accum - max_accum_time);
			cl_pred_frame_accum = max_accum_time;
			clamp_applied = true;
		}
		post_accum = (float)cl_pred_frame_accum;
		cl_pred_frame_dt_last = dt_use;
		cl_pred_prev_realtime = realtime;
		cl_pred_frame_drop_time = clamp_drop;

		CL_Predict_UpdateAccumStatsDt (dt_use);
		if (CL_Predict_AccumVerboseEnabled ())
		{
			Con_Printf ("PREDACCUM dt_real=%.6f dt_raw=%.6f dt_host=%.6f dt_use=%.6f src=%s pre=%.6f post=%.6f snap=%d clamp=%d drop=%.6f unbias=%d\n",
				real_dt, raw_dt, host_dt, dt_use, dt_source, pre_accum, post_accum,
				cl_pred_dt_snapped ? 1 : 0, clamp_applied ? 1 : 0, clamp_drop, unbias_applied ? 1 : 0);
			JITTER_LOG ("PREDACCUM dt_real=%.6f dt_raw=%.6f dt_host=%.6f dt_use=%.6f src=%s pre=%.6f post=%.6f snap=%d clamp=%d drop=%.6f unbias=%d\n",
				real_dt, raw_dt, host_dt, dt_use, dt_source, pre_accum, post_accum,
				cl_pred_dt_snapped ? 1 : 0, clamp_applied ? 1 : 0, clamp_drop, unbias_applied ? 1 : 0);
		}
		if (clamp_applied && CL_Predict_AccumSummaryEnabled ())
		{
			Con_Printf ("PREDACCUM clamp_begin drop %.6f max %.6f\n", clamp_drop, max_accum_time);
			JITTER_LOG ("PREDACCUM clamp_begin drop %.6f max %.6f\n", clamp_drop, max_accum_time);
		}
	}
	else if (!enabled || !cl_pred.has_base)
	{
		if (was_enabled)
		{
			cl_pred_frame_dt_last = 0.0f;
			cl_pred_frame_accum = 0.0;
			cl_pred_prev_realtime = 0.0;
			CL_Predict_ResetAccumStats ();
		}
		cl_pred_render_interp_valid = false;
	}
	cl_pred_prev_enabled = enabled;

	if (CL_Predict_AccumSummaryEnabled ())
	{
		Con_Printf ("PREDACCUM dt %.4f accum %.4f step_dt %.4f\n",
			host_frametime, cl_pred_frame_accum, cl_pred_last_substep_dt);
		JITTER_LOG ("PREDACCUM dt %.4f accum %.4f step_dt %.4f\n",
			host_frametime, cl_pred_frame_accum, cl_pred_last_substep_dt);
	}

	if (!enabled)
		cl_pred_apply_pred_reason = CL_PRED_APPLY_SKIP_DISABLED;
	else if (!cl_pred.has_base)
		cl_pred_apply_pred_reason = CL_PRED_APPLY_SKIP_NO_BASE;
	else if (!cl_pred_render_cmd_valid)
		cl_pred_apply_pred_reason = CL_PRED_APPLY_SKIP_NO_CMD;
	else
		cl_pred_apply_pred_reason = CL_PRED_APPLY_SKIP_ACCUM;

	if (!enabled)
		cl_pred_apply_render_reason = CL_PRED_APPLY_SKIP_DISABLED;
	else if (!cl_pred.has_base)
		cl_pred_apply_render_reason = CL_PRED_APPLY_SKIP_NO_BASE;
	else if (!cl_pred_render_cmd_valid)
		cl_pred_apply_render_reason = CL_PRED_APPLY_SKIP_NO_CMD;
	else
		cl_pred_apply_render_reason = CL_PRED_APPLY_SKIP_ACCUM;
}

void CL_Predict_SetupCmd (usercmd_t *cmd)
{
	float cmd_dt;
	float dt_sub;
	float host_dt;
	int substeps;

	CL_Predict_RegisterDebugCvars ();
	cl_netdbg_predict_ran = false;
	cmd->sequence = cl_pred.seq_latest + 1;
	cl_pred.seq_latest = cmd->sequence;
	cl_pred.cmds[cmd->sequence % CMD_RING] = *cmd;
	cl_pred_angles_normalized = 1;
	if (cl_jitter_debug.value > 0.0f)
	{
		Con_Printf ("CMD OUT seq=%u\n", cmd->sequence);
		JITTER_LOG ("CMD OUT seq=%u\n", cmd->sequence);
	}

	if (!CL_Predict_IsEnabled () || !cl_pred.has_base)
		return;

	cmd_dt = CL_Predict_GetCmdStepTime (cmd);
	if (cl_jitter_debug.value > 0.0f)
	{
		host_dt = host_frametime;
		CL_Predict_GetSubstepInfo (cmd_dt, host_dt, &substeps, &dt_sub);
		Con_Printf ("JITTERDBG setup seq %u cmd_dt %.4f host_dt %.4f substeps %d dt_sub %.4f mouse_applied %d\n",
			cmd->sequence, cmd_dt, host_dt, substeps, dt_sub, IN_DidApplyMouseDelta () ? 1 : 0);
		JITTER_LOG ("JITTERDBG setup seq %u cmd_dt %.4f host_dt %.4f substeps %d dt_sub %.4f mouse_applied %d\n",
			cmd->sequence, cmd_dt, host_dt, substeps, dt_sub, IN_DidApplyMouseDelta () ? 1 : 0);
	}
	CL_Predict_DebugLogCmd ("setup", cmd, cmd_dt);
	cl_pred_render_cmd = *cmd;
	cl_pred_render_cmd_valid = true;
	CL_Predict_StoreFrame (cmd->sequence, &cl_pred.predicted);
	cl_pred_apply_pred_reason = CL_PRED_APPLY_OK;
	CL_Predict_ApplyToClient (&cl_pred.predicted, false);
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
	float snap_dist = cl_pred_snapdist.value;
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
	qboolean allow_angle_correction;
	cl_pred_frame_t pred_frame;
	qboolean pred_frame_valid = false;
	qboolean snap_correction = false;
	qboolean use_ultra = false;
	qboolean pred_overflow = false;
	unsigned int pred_slot = 0;
	unsigned int pred_stored_seq = 0;

	CL_Predict_RegisterDebugCvars ();
	use_ultra = CL_Predict_UseUltra ();
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
			JITTER_LOG ("NETDBG: prediction correction %.1f units (server %f %f %f, client %f %f %f)\n",
				correction,
				origin[0], origin[1], origin[2],
				cl.simorg[0], cl.simorg[1], cl.simorg[2]);
		}
	}

	cl_pred.seq_acked = ack;
	cl_pred_server_update_this_frame = true;
	cl_pred_last_ack_seq = ack;
	allow_angle_correction = (cl_pred_correct_angles.value > 0.0f);
	if (cl_pred.has_base)
	{
		pred_frame_valid = CL_Predict_GetFrame (ack, &pred_frame);
		if (CL_Predict_SeqNewer (cl_pred.seq_latest, ack))
		{
			unsigned int delta = cl_pred.seq_latest - ack;

			if (delta >= CL_PRED_FRAME_RING)
			{
				pred_overflow = true;
				if (!cl_pred_warned_overflow)
				{
					Con_Printf ("Pred overflow: latest-ack=%u > PRED_RING\n", delta);
					JITTER_LOG ("Pred overflow: latest-ack=%u > PRED_RING\n", delta);
					cl_pred_warned_overflow = true;
				}
			}
		}
		if (pred_overflow)
			pred_frame_valid = false;
		if (use_ultra)
		{
			if (!pred_frame_valid && !cl_pred_warned_pred_miss)
			{
				pred_slot = ack & (CL_PRED_FRAME_RING - 1);
				pred_stored_seq = cl_pred.frames[pred_slot].seq;
				Con_Printf ("PredFrame MISS: ack=%u stored=%u slot=%u latest=%u\n",
					ack, pred_stored_seq, pred_slot, cl_pred.seq_latest);
				JITTER_LOG ("PredFrame MISS: ack=%u stored=%u slot=%u latest=%u\n",
					ack, pred_stored_seq, pred_slot, cl_pred.seq_latest);
				cl_pred_warned_pred_miss = true;
			}
		}
		cl_pred_pred_frame_found = pred_frame_valid ? true : false;
		if (pred_frame_valid)
		{
			VectorSubtract (origin, pred_frame.origin, error);
			error_len = VectorLength (error);
			for (i = 0; i < 3; i++)
				angle_error[i] = CL_Predict_AngleDelta (viewangles[i], pred_frame.viewangles[i]);
		}
		else
		{
			VectorSubtract (origin, cl_pred.predicted.origin, error);
			error_len = VectorLength (error);
			for (i = 0; i < 3; i++)
				angle_error[i] = CL_Predict_AngleDelta (viewangles[i], cl_pred.predicted.viewangles[i]);
		}
		if (cl_netdbg_pred.value > 0.0f)
		{
			const vec3_t *client_origin = pred_frame_valid ? (const vec3_t *)pred_frame.origin
				: (const vec3_t *)cl_pred.predicted.origin;

			Con_Printf ("NETDBG: pred_error %.2f (server %.1f %.1f %.1f client %.1f %.1f %.1f)\n",
				error_len,
				origin[0], origin[1], origin[2],
				(*client_origin)[0], (*client_origin)[1], (*client_origin)[2]);
			JITTER_LOG ("NETDBG: pred_error %.2f (server %.1f %.1f %.1f client %.1f %.1f %.1f)\n",
				error_len,
				origin[0], origin[1], origin[2],
				(*client_origin)[0], (*client_origin)[1], (*client_origin)[2]);
		}
		if (cl_jitter_debug.value > 0.0f && error_len >= 2.0f)
		{
			Con_Printf ("JITTERDBG ground onground %d groundent %d ground_valid %d reason %d trace_frac %.2f trace_nz %.2f trace_ent %d trace_solid %d/%d trace_fallback %d wishspeed %.1f wishvel_z %.1f dt %.4f flags %d ground_delta %.2f ground_yaw %.2f apply_pred %d apply_render %d switches %d gprev %.2f %.2f %.2f gnow %.2f %.2f %.2f delta_applied %d mouse_applied %d\n",
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
				cl_pred_ground_dbg.ground_switches,
				cl_pred_ground_dbg.ground_origin_prev[0], cl_pred_ground_dbg.ground_origin_prev[1], cl_pred_ground_dbg.ground_origin_prev[2],
				cl_pred_ground_dbg.ground_origin_now[0], cl_pred_ground_dbg.ground_origin_now[1], cl_pred_ground_dbg.ground_origin_now[2],
				cl_pred_ground_dbg.delta_applied,
				IN_DidApplyMouseDelta () ? 1 : 0);
			JITTER_LOG ("JITTERDBG ground onground %d groundent %d ground_valid %d reason %d trace_frac %.2f trace_nz %.2f trace_ent %d trace_solid %d/%d trace_fallback %d wishspeed %.1f wishvel_z %.1f dt %.4f flags %d ground_delta %.2f ground_yaw %.2f apply_pred %d apply_render %d switches %d gprev %.2f %.2f %.2f gnow %.2f %.2f %.2f delta_applied %d mouse_applied %d\n",
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
				cl_pred_ground_dbg.ground_switches,
				cl_pred_ground_dbg.ground_origin_prev[0], cl_pred_ground_dbg.ground_origin_prev[1], cl_pred_ground_dbg.ground_origin_prev[2],
				cl_pred_ground_dbg.ground_origin_now[0], cl_pred_ground_dbg.ground_origin_now[1], cl_pred_ground_dbg.ground_origin_now[2],
				cl_pred_ground_dbg.delta_applied,
				IN_DidApplyMouseDelta () ? 1 : 0);
		}
		if ((teleport_dist > 0.0f && error_len > teleport_dist)
			|| (snap_dist > 0.0f && error_len > snap_dist)
			|| (!pred_frame_valid && use_ultra)
			|| pred_overflow)
		{
			VectorClear (cl_pred_error);
			VectorClear (cl_pred_angle_error);
			cl_pred_reset = true;
			cl_pred_snap_count++;
			snap_correction = true;
			CL_Predict_ClearFrames ();
		}
		else if (!use_ultra)
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
			if (allow_angle_correction)
				VectorCopy (angle_error, cl_pred_angle_error);
			else
				VectorClear (cl_pred_angle_error);
			cl_pred_error_time = realtime;
			cl_pred_reset = false;
			cl_pred_smooth_count++;
			if (cl_pred_debug.value > 0.0f)
			{
				float total_error = VectorLength (cl_pred_error);

				Con_Printf ("PREDDBG smooth_add err %.2f total %.2f\n", error_len, total_error);
				JITTER_LOG ("PREDDBG smooth_add err %.2f total %.2f\n", error_len, total_error);
			}
		}
		else
		{
			VectorAdd (cl_pred_error, error, cl_pred_error);
			if (allow_angle_correction)
				VectorCopy (angle_error, cl_pred_angle_error);
			else
				VectorClear (cl_pred_angle_error);
			cl_pred_error_time = realtime;
			cl_pred_reset = false;
			cl_pred_smooth_count++;
		}
		// Q3MINI BEGIN
		if (!use_ultra)
		{
			float smooth_ms = cl_netsmooth_time.value;
			float maxdist = cl_netsmooth_maxdist.value;
			qboolean allow_smooth = (cl_netsmooth.value > 0.0f && smooth_ms > 0.0f && !snap_correction);

			if (maxdist < 0.0f)
				maxdist = 0.0f;
			if (allow_smooth && (maxdist <= 0.0f || error_len <= maxdist))
			{
				VectorCopy (error, cl_q3mini_net_error);
				cl_q3mini_net_duration = smooth_ms * 0.001f;
				cl_q3mini_net_remaining = cl_q3mini_net_duration;
				cl_q3mini_net_active = true;
				if (net_dbg_q3mini.value > 0.0f)
				{
					Con_Printf ("NETDBG q3mini smooth_start err %.2f dur %.0fms onground %d groundent %d\n",
						error_len, smooth_ms, onground ? 1 : 0, cl_pred.base.pred_ground_ent);
					JITTER_LOG ("NETDBG q3mini smooth_start err %.2f dur %.0fms onground %d groundent %d\n",
						error_len, smooth_ms, onground ? 1 : 0, cl_pred.base.pred_ground_ent);
				}
			}
			else
			{
				if (cl_q3mini_net_active && net_dbg_q3mini.value > 0.0f)
				{
					Con_Printf ("NETDBG q3mini smooth_snap err %.2f maxdist %.2f snap %d\n",
						error_len, maxdist, snap_correction ? 1 : 0);
					JITTER_LOG ("NETDBG q3mini smooth_snap err %.2f maxdist %.2f snap %d\n",
						error_len, maxdist, snap_correction ? 1 : 0);
				}
				VectorClear (cl_q3mini_net_error);
				cl_q3mini_net_active = false;
				cl_q3mini_net_remaining = 0.0f;
				cl_q3mini_net_duration = 0.0f;
			}
		}
		// Q3MINI END
	}
	else
	{
		VectorClear (cl_pred_error);
		VectorClear (cl_pred_angle_error);
		cl_pred_reset = false;
	}
	cl_pred_true_error_len = pred_frame_valid ? error_len : 0.0f;
	VectorCopy (origin, cl_pred.base.origin);
	VectorCopy (velocity, cl_pred.base.velocity);
	cl_pred.base.viewangles[0] = NormalizeAngle180 (viewangles[0]);
	cl_pred.base.viewangles[1] = NormalizeAngle180 (viewangles[1]);
	cl_pred.base.viewangles[2] = NormalizeAngle180 (viewangles[2]);
	cl_pred.base.onground = onground;
	cl_pred.predicted = cl_pred.base;
	CL_Predict_ResetRenderInterp ();
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
			JITTER_LOG ("NETDBG: server onground without ground entity (groundent 0)\n");
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

	if (snap_correction)
	{
		VectorCopy (origin, cl.simorg);
		VectorClear (cl_pred_error);
		VectorClear (cl_pred_angle_error);
		cl_pred_true_error_len = 0.0f;
		cl_pred_error_time = 0.0f;
		cl_q3mini_net_active = false;
		cl_q3mini_net_remaining = 0.0f;
		cl_q3mini_net_duration = 0.0f;
		if (cl_jitter_debug.value > 0.0f)
		{
			Con_Printf ("JITTERDBG snap_correction ack %u latest %u accum %.4f steps %d\n",
				ack, cl_pred.seq_latest, cl_pred_frame_accum, cl_pred_steps_this_frame);
			JITTER_LOG ("JITTERDBG snap_correction ack %u latest %u accum %.4f steps %d\n",
				ack, cl_pred.seq_latest, cl_pred_frame_accum, cl_pred_steps_this_frame);
		}
	}
	if (use_ultra && snap_correction)
	{
		cl_pred_apply_pred_reason = CL_PRED_APPLY_OK;
		CL_Predict_ApplyToClient (&cl_pred.predicted, false);
	}

	if (CL_Predict_SeqNewer (ack, cl_pred.seq_latest))
	{
		CL_Predict_HardResetToBase ("ack ahead of latest");
		CL_Predict_ApplyToClient (&cl_pred.predicted, false);
		return;
	}

	for (seq = ack + 1; !CL_Predict_SeqNewer (seq, cl_pred.seq_latest); seq++)
	{
		usercmd_t cmd;

		if (!CL_Predict_GetCmd (seq, &cmd))
		{
			CL_Predict_HardResetToBase ("cmd chain broken");
			CL_Predict_ApplyToClient (&cl_pred.predicted, false);
			return;
		}
		if (use_ultra)
		{
			cmd_dt = CL_Predict_GetUltraStepTime ();
			host_dt = host_frametime;
			substeps = 1;
			dt_sub = cmd_dt;
		}
		else
		{
			cmd_dt = CL_Predict_GetCmdStepTime (&cmd);
			host_dt = host_frametime;
			CL_Predict_GetSubstepInfo (cmd_dt, host_dt, &substeps, &dt_sub);
		}
		if (cl_jitter_debug.value > 0.0f)
		{
			Con_Printf ("JITTERDBG resim seq %u cmd_dt %.4f host_dt %.4f substeps %d dt_sub %.4f mouse_applied %d\n",
				cmd.sequence, cmd_dt, host_dt, substeps, dt_sub, IN_DidApplyMouseDelta () ? 1 : 0);
			JITTER_LOG ("JITTERDBG resim seq %u cmd_dt %.4f host_dt %.4f substeps %d dt_sub %.4f mouse_applied %d\n",
				cmd.sequence, cmd_dt, host_dt, substeps, dt_sub, IN_DidApplyMouseDelta () ? 1 : 0);
		}
		for (i = 0; i < substeps; i++)
			CL_Predict_SimulateCmd (&cl_pred.predicted, &cmd, dt_sub, false);
		CL_Predict_StoreFrame (cmd.sequence, &cl_pred.predicted);
		resim_cmd = cmd;
		resim_cmd_valid = true;
		CL_Predict_DebugLogCmd ("resim", &cmd, cmd_dt);
		cl_pred_replay_count++;
	}
	if (!resim_cmd_valid && CL_Predict_SeqNewer (cl_pred.seq_latest, ack))
	{
		if (!cl_pred_warned_replay)
		{
			Con_Printf ("Replay skipped: ack=%u latest=%u (should be >= ack+1 when cmds pending)\n", ack, cl_pred.seq_latest);
			JITTER_LOG ("Replay skipped: ack=%u latest=%u (should be >= ack+1 when cmds pending)\n", ack, cl_pred.seq_latest);
			cl_pred_warned_replay = true;
		}
	}

	if (resim_cmd_valid)
	{
		cl_pred_render_cmd = resim_cmd;
		cl_pred_render_cmd_valid = true;
	}
	if (cl_pred_debug.value > 0.0f)
	{
		Con_Printf ("PREDDBG ack %u pred %u replay %d err %.2f onground %d groundent %d platform_delta %d dt %.4f snap %d snaps %d smooth %d\n",
			ack,
			cl_pred.seq_latest,
			cl_pred_replay_count,
			error_len,
			cl_pred.predicted.onground ? 1 : 0,
			cl_pred.predicted.groundent,
			cl_pred_ground_dbg.delta_applied,
			host_frametime,
			snap_correction ? 1 : 0,
			cl_pred_snap_count,
			cl_pred_smooth_count);
	}
	cl_pred_apply_pred_reason = CL_PRED_APPLY_OK;
	CL_Predict_ApplyToClient (&cl_pred.predicted, false);
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

	CL_Predict_RunFrameSteps ();
	{
		cl_pred_state_t render_state;

		render_state = cl_pred.predicted;
		if (cl_pred_render_interp_valid)
			CL_Predict_LerpState (&render_state, &cl_pred_render_from, &cl_pred_render_to, cl_pred_render_frac);
		CL_Predict_ApplyToClient (&render_state, true);
	}
}

qboolean CL_Predict_GetDebug (cl_pred_debug_t *out)
{
	if (!out)
		return false;

	memset (out, 0, sizeof(*out));
	out->prediction_steps = cl_pred_steps_this_frame;
	out->server_update_applied = cl_pred_server_update_this_frame;
	out->pred_error_len = cl_pred_true_error_len;
	out->pred_smooth_error_len = VectorLength (cl_pred_error);
	out->pred_angle_error_len = VectorLength (cl_pred_angle_error);
	VectorCopy (cl_pred_error, out->pred_error);
	VectorCopy (cl_pred_angle_error, out->pred_angle_error);
	out->ack_seq = cl_pred_last_ack_seq;
	out->latest_seq = cl_pred.seq_latest;
	out->pred_frame_found = cl_pred_pred_frame_found;
	out->replay_count = cl_pred_replay_count;
	out->snap_count = cl_pred_snap_count;
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
	out->pred_accum_time = (float)cl_pred_frame_accum;
	out->pred_step_dt = cl_pred_last_substep_dt;
	out->pred_steps = cl_pred_last_substeps;
	out->pred_render_frac = cl_pred_render_frac;
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
