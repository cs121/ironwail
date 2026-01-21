/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

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
// cl_main.c  -- client main loop

#include "quakedef.h"
#include "cl_postfx.h"
#include "bgmusic.h"
#include "../common/lightgrid.h"
#include "r_dlight_pool.h"

// we need to declare some mouse variables here, because the menu system
// references them even when on a unix system.

// these two are not intended to be set directly
cvar_t	cl_name = {"_cl_name", "player", CVAR_ARCHIVE};
cvar_t	cl_color = {"_cl_color", "0", CVAR_ARCHIVE};

cvar_t	cl_shownet = {"cl_shownet","0",CVAR_NONE};	// can be 0, 1, or 2
cvar_t	cl_netdebug_parse = {"cl_netdebug_parse", "0", CVAR_NONE};
cvar_t	cl_netdebug_hexdump = {"cl_netdebug_hexdump", "0", CVAR_NONE};
cvar_t	cl_netdebug_dropbad = {"cl_netdebug_dropbad", "1", CVAR_NONE};
cvar_t	cl_netdebug_maxdump = {"cl_netdebug_maxdump", "256", CVAR_NONE};
cvar_t	cl_signon_chunk_debug = {"cl_signon_chunk_debug", "0", CVAR_NONE};
cvar_t	cl_signon_debug = {"cl_signon_debug", "0", CVAR_NONE};
cvar_t	cl_nolerp = {"cl_nolerp","0",CVAR_NONE};
cvar_t	cl_packedents = {"cl_packedents", "1", CVAR_ARCHIVE};
cvar_t	cl_snap_debug = {"cl_snap_debug", "0", CVAR_NONE};
cvar_t	cl_delta_reject_debug = {"cl_delta_reject_debug", "0", CVAR_NONE};
cvar_t	cl_full_reasm_debug = {"cl_full_reasm_debug", "0", CVAR_NONE};
cvar_t	cl_full_reasm_timeout_ms = {"cl_full_reasm_timeout_ms", "1000", CVAR_NONE};
cvar_t	cl_test_drop = {"cl_test_drop", "0", CVAR_NONE};
cvar_t	cl_entity_timeout_ms = {"cl_entity_timeout_ms", "750", CVAR_NONE};
cvar_t	cl_lerp_ms = {"cl_lerp_ms", "120", CVAR_NONE};
cvar_t	cl_lerp_max_gap_ms = {"cl_lerp_max_gap_ms", "250", CVAR_NONE};
cvar_t	cl_lerp_debug = {"cl_lerp_debug", "0", CVAR_NONE};

cvar_t	cfg_unbindall = {"cfg_unbindall", "1", CVAR_ARCHIVE};

cvar_t	freelook = {"freelook","1", CVAR_ARCHIVE};
cvar_t	lookspring = {"lookspring","0", CVAR_ARCHIVE};
cvar_t	lookstrafe = {"lookstrafe","0", CVAR_ARCHIVE};
cvar_t	sensitivity = {"sensitivity","3", CVAR_ARCHIVE};

cvar_t	m_pitch = {"m_pitch","0.022", CVAR_ARCHIVE};
cvar_t	m_yaw = {"m_yaw","0.022", CVAR_ARCHIVE};
cvar_t	m_forward = {"m_forward","1", CVAR_ARCHIVE};
cvar_t	m_side = {"m_side","0.8", CVAR_ARCHIVE};

cvar_t	cl_maxpitch = {"cl_maxpitch", "90", CVAR_ARCHIVE}; //johnfitz -- variable pitch clamping
cvar_t	cl_minpitch = {"cl_minpitch", "-90", CVAR_ARCHIVE}; //johnfitz -- variable pitch clamping

cvar_t	cl_mwheelpitch = {"cl_mwheelpitch", "5", CVAR_ARCHIVE};

cvar_t	cl_startdemos = {"cl_startdemos", "1", CVAR_ARCHIVE};
cvar_t	cl_confirmquit = {"cl_confirmquit", "0", CVAR_ARCHIVE};

client_static_t	cls;
client_state_t	cl;
// FIXME: put these on hunk?
entity_t		cl_static_entities[MAX_STATIC_ENTITIES];
lightstyle_t	cl_lightstyle[MAX_LIGHTSTYLES];
int					cl_max_lightstyles = MAX_LIGHTSTYLES;

entity_t		*cl_entities; //johnfitz -- was a static array, now on hunk
int				cl_max_edicts; //johnfitz -- only changes when new map loads

int				cl_numvisedicts;
entity_t		*cl_visedicts[MAX_VISEDICTS];

extern cvar_t	r_lerpmodels, r_lerpmove; //johnfitz
extern float	host_netinterval;	//Spike

extern vec3_t	v_punchangles[2];

#define CL_PLAYER_SNAP_HISTORY 64
#define CL_PLAYER_SNAP_MASK_WORDS ((MAX_SCOREBOARD + 31) / 32)

typedef struct cl_player_snap_s
{
	double		t;
	int			servertime;
	vec3_t		org[MAX_SCOREBOARD];
	vec3_t		ang[MAX_SCOREBOARD];
	uint32_t	valid_mask_words[CL_PLAYER_SNAP_MASK_WORDS];
} cl_player_snap_t;

static cl_player_snap_t	cl_psnaps[CL_PLAYER_SNAP_HISTORY];
static int				cl_psnap_head = 0;
static int				cl_psnap_count = 0;

static int				cl_lerp_miss_pairs = 0;
static int				cl_lerp_hold_newest = 0;
static int				cl_lerp_hold_oldest = 0;
static int				cl_lerp_gap_holds = 0;
static double			cl_lerp_debug_next_time = 0.0;

static inline void CL_SetValidBit (cl_player_snap_t *snap, int idx)
{
	int word = idx >> 5;
	int bit = idx & 31;

	if (word < 0 || word >= CL_PLAYER_SNAP_MASK_WORDS)
		return;
	snap->valid_mask_words[word] |= (1u << bit);
}

static inline qboolean CL_IsValidBit (const cl_player_snap_t *snap, int idx)
{
	int word = idx >> 5;
	int bit = idx & 31;

	if (word < 0 || word >= CL_PLAYER_SNAP_MASK_WORDS)
		return false;
	return (snap->valid_mask_words[word] & (1u << bit)) != 0;
}

static void CL_ClearPlayerSnaps (void)
{
	memset (cl_psnaps, 0, sizeof(cl_psnaps));
	cl_psnap_head = 0;
	cl_psnap_count = 0;
	cl_lerp_miss_pairs = 0;
	cl_lerp_hold_newest = 0;
	cl_lerp_hold_oldest = 0;
	cl_lerp_gap_holds = 0;
	cl_lerp_debug_next_time = 0.0;
}

void CL_ResetPlayerSnaps (void)
{
	CL_ClearPlayerSnaps ();
}

float cl_lerpfrac = 1.0f;
qboolean cl_viewent_needs_init = true;

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

void CL_RecordPlayerSnap (void)
{
	cl_player_snap_t *snap;
	int i;

	if (cl.maxclients <= 0)
		return;

	snap = &cl_psnaps[cl_psnap_head];
	memset (snap, 0, sizeof(*snap));
	snap->t = Sys_DoubleTime ();
	snap->servertime = 0;

	for (i = 0; i < cl.maxclients && i < MAX_SCOREBOARD; i++)
	{
		int entnum = i + 1;
		entity_t *ent;

		if (entnum == cl.viewentity)
			continue;
		if (!cl.snapshot_present || !cl.snapshot_present[entnum])
			continue;
		ent = &cl_entities[entnum];
		if (!ent->model)
			continue;
		VectorCopy (ent->msg_origins[0], snap->org[i]);
		VectorCopy (ent->msg_angles[0], snap->ang[i]);
		CL_SetValidBit (snap, i);
	}

	cl_psnap_head = (cl_psnap_head + 1) % CL_PLAYER_SNAP_HISTORY;
	cl_psnap_count = q_min (cl_psnap_count + 1, CL_PLAYER_SNAP_HISTORY);
}

qboolean CL_WorldReady (void)
{
	entity_t *view;

	// Strict mapload gating: only allow prediction/input when the viewentity has a model.
	if (cls.signon != SIGNONS)
		return false;
	if (!cl.worldmodel)
		return false;
	if (cl.viewentity <= 0 || cl.viewentity >= cl_max_edicts)
		return false;
	if (!cl_entities)
		return false;
	view = &cl_entities[cl.viewentity];
	if (!view->model)
		return false;

	return true;
}

static float CL_LerpAngle (float a, float b, float f)
{
	float d = b - a;

	while (d > 180.0f)
		d -= 360.0f;
	while (d < -180.0f)
		d += 360.0f;
	return a + f * d;
}

static qboolean CL_GetInterpolatedPlayer (int player, vec3_t out_org, vec3_t out_ang)
{
	double now;
	double render_t;
	double max_gap_s;
	cl_player_snap_t *newest;
	cl_player_snap_t *oldest = NULL;
	cl_player_snap_t *snap;
	cl_player_snap_t *prev;
	int idx;
	int count;

	if (cl_lerp_ms.value <= 0.0f)
		return false;
	if (player < 0 || player >= MAX_SCOREBOARD)
		return false;
	if (cl_psnap_count <= 0)
		return false;

	now = Sys_DoubleTime ();
	render_t = now - (cl_lerp_ms.value * 0.001);
	max_gap_s = cl_lerp_max_gap_ms.value * 0.001;

	idx = (cl_psnap_head - 1 + CL_PLAYER_SNAP_HISTORY) % CL_PLAYER_SNAP_HISTORY;
	newest = &cl_psnaps[idx];

	if (render_t >= newest->t)
	{
		if (!CL_IsValidBit (newest, player))
			return false;
		VectorCopy (newest->org[player], out_org);
		VectorCopy (newest->ang[player], out_ang);
		cl_lerp_hold_newest++;
		return true;
	}

	prev = newest;
	for (count = 0; count < cl_psnap_count; count++)
	{
		snap = &cl_psnaps[idx];
		oldest = snap;
		if (snap->t <= render_t)
		{
			if (!CL_IsValidBit (snap, player) || !CL_IsValidBit (prev, player))
				break;
			if (prev->t - snap->t > max_gap_s)
			{
				VectorCopy (snap->org[player], out_org);
				VectorCopy (snap->ang[player], out_ang);
				cl_lerp_gap_holds++;
				return true;
			}
			if (prev->t <= snap->t)
				break;
			{
				float f = (float)((render_t - snap->t) / (prev->t - snap->t));
				int axis;
				f = CLAMP (0.0f, f, 1.0f);
				for (axis = 0; axis < 3; axis++)
				{
					out_org[axis] = snap->org[player][axis] + f * (prev->org[player][axis] - snap->org[player][axis]);
					out_ang[axis] = CL_LerpAngle (snap->ang[player][axis], prev->ang[player][axis], f);
				}
				return true;
			}
		}
		prev = snap;
		idx = (idx - 1 + CL_PLAYER_SNAP_HISTORY) % CL_PLAYER_SNAP_HISTORY;
	}

	if (oldest && render_t < oldest->t && CL_IsValidBit (oldest, player))
	{
		VectorCopy (oldest->org[player], out_org);
		VectorCopy (oldest->ang[player], out_ang);
		cl_lerp_hold_oldest++;
		return true;
	}

	cl_lerp_miss_pairs++;
	return false;
}

static qboolean CL_GetInterpolatedEntity (int entnum, vec3_t out_org, vec3_t out_ang)
{
	double render_t;
	double max_gap_s;
	double max_extrap_s;
	cl_entity_snap_t *snaps;
	cl_entity_snap_t *newest;
	cl_entity_snap_t *oldest = NULL;
	cl_entity_snap_t *snap;
	cl_entity_snap_t *prev;
	int snap_count;
	int head;
	int idx;
	int axis;

	if (cl_lerp_ms.value <= 0.0f)
		return false;
	if (!cl.entity_snapshots || !cl.entity_snap_head || !cl.entity_snap_count)
		return false;
	if (entnum <= 0 || entnum >= cl_max_edicts)
		return false;

	snap_count = cl.entity_snap_count[entnum];
	if (snap_count <= 0)
		return false;

	render_t = cl.time - (cl_lerp_ms.value * 0.001);
	max_gap_s = cl_lerp_max_gap_ms.value * 0.001;
	max_extrap_s = 0.05;
	if (max_gap_s > 0.0 && max_gap_s < max_extrap_s)
		max_extrap_s = max_gap_s;

	snaps = &cl.entity_snapshots[entnum * CL_ENTITY_SNAP_HISTORY];
	head = cl.entity_snap_head[entnum];
	newest = &snaps[head];

	if (snap_count == 1)
	{
		VectorCopy (newest->origin, out_org);
		VectorCopy (newest->angles, out_ang);
		return true;
	}

	if (render_t >= newest->servertime)
	{
		int prev_idx = (head - 1 + CL_ENTITY_SNAP_HISTORY) % CL_ENTITY_SNAP_HISTORY;
		prev = &snaps[prev_idx];
		if (prev->servertime > 0.0 && newest->servertime > prev->servertime)
		{
			double dt = newest->servertime - prev->servertime;
			double extrap_t = render_t - newest->servertime;

			if ((max_gap_s <= 0.0 || dt <= max_gap_s) && dt > 0.0)
			{
				float f;

				if (extrap_t < 0.0)
					extrap_t = 0.0;
				if (extrap_t > max_extrap_s)
					extrap_t = max_extrap_s;

				for (axis = 0; axis < 3; axis++)
				{
					float delta = newest->origin[axis] - prev->origin[axis];
					if (delta > 100.0f || delta < -100.0f)
					{
						VectorCopy (newest->origin, out_org);
						VectorCopy (newest->angles, out_ang);
						return true;
					}
				}

				f = (float)((dt + extrap_t) / dt);
				if (f < 0.0f || f > 1.0f)
					f = 1.0f;
				for (axis = 0; axis < 3; axis++)
				{
					out_org[axis] = prev->origin[axis] + f * (newest->origin[axis] - prev->origin[axis]);
					out_ang[axis] = CL_LerpAngle (prev->angles[axis], newest->angles[axis], f);
				}
				return true;
			}
		}
		VectorCopy (newest->origin, out_org);
		VectorCopy (newest->angles, out_ang);
		return true;
	}

	prev = newest;
	idx = head;
	for (idx = 0; idx < snap_count; idx++)
	{
		snap = &snaps[head];
		oldest = snap;
		if (snap->servertime <= render_t)
		{
			double span = prev->servertime - snap->servertime;
			float f;

			if (span <= 0.0)
			{
				VectorCopy (prev->origin, out_org);
				VectorCopy (prev->angles, out_ang);
				return true;
			}
			if (max_gap_s > 0.0 && span > max_gap_s)
			{
				VectorCopy (snap->origin, out_org);
				VectorCopy (snap->angles, out_ang);
				return true;
			}

			for (axis = 0; axis < 3; axis++)
			{
				float delta = prev->origin[axis] - snap->origin[axis];
				if (delta > 100.0f || delta < -100.0f)
				{
					VectorCopy (prev->origin, out_org);
					VectorCopy (prev->angles, out_ang);
					return true;
				}
			}

			f = (float)((render_t - snap->servertime) / span);
			f = CLAMP (0.0f, f, 1.0f);
			for (axis = 0; axis < 3; axis++)
			{
				out_org[axis] = snap->origin[axis] + f * (prev->origin[axis] - snap->origin[axis]);
				out_ang[axis] = CL_LerpAngle (snap->angles[axis], prev->angles[axis], f);
			}
			return true;
		}
		prev = snap;
		head = (head - 1 + CL_ENTITY_SNAP_HISTORY) % CL_ENTITY_SNAP_HISTORY;
	}

	if (oldest && render_t < oldest->servertime)
	{
		VectorCopy (oldest->origin, out_org);
		VectorCopy (oldest->angles, out_ang);
		return true;
	}

	return false;
}

void CL_FreeState(void)
{
        int i;
        for (i = 0; i < MAX_CL_STATS; i++)
                free (cl.statss[i]);
        if (cl.signon_frag_buf)
                Z_Free (cl.signon_frag_buf);
        Lightgrid_Free(cl.lightgrid);
        PR_ClearProgs (&cl.qcvm);
        memset (&cl, 0, sizeof(cl));
}

/*
=====================
CL_ClearState

=====================
*/
void CL_ClearState (void)
{
	if (cl.qcvm.extfuncs.CSQC_Shutdown)
	{
		PR_SwitchQCVM(&cl.qcvm);
		PR_ExecuteProgram(qcvm->extfuncs.CSQC_Shutdown);
		qcvm->extfuncs.CSQC_Shutdown = 0;
		PR_SwitchQCVM(NULL);
	}

	if (!sv.active)
		Host_ClearMemory ();

// wipe the entire cl structure
	CL_FreeState ();
	CL_ClearPlayerSnaps ();

	SZ_Clear (&cls.message);

// clear other arrays
	DLightPool_Clear ();
	memset (cl_lightstyle, 0, sizeof(cl_lightstyle));
	cl_max_lightstyles = MAX_LIGHTSTYLES;
	memset (cl_temp_entities, 0, sizeof(cl_temp_entities));
	memset (cl_beams, 0, sizeof(cl_beams));

	//johnfitz -- cl_entities is now dynamically allocated
	cl_max_edicts = CLAMP (MIN_EDICTS,(int)max_edicts.value,MAX_EDICTS);
	cl_entities = (entity_t *) Hunk_AllocName (cl_max_edicts*sizeof(entity_t), "cl_entities");
	cl.snapshot_baseline = (snapshot_state_t *) Hunk_AllocName (cl_max_edicts*sizeof(snapshot_state_t), "cl_snap_base");
	cl.snapshot_present = (byte *) Hunk_AllocName (cl_max_edicts*sizeof(byte), "cl_snap_present");
	cl.snapshot_active = (byte *) Hunk_AllocName (cl_max_edicts*sizeof(byte), "cl_snap_active");
	cl.snapshot_last_update_time = (double *) Hunk_AllocName (cl_max_edicts*sizeof(double), "cl_snap_time");
	cl.snapshot_chunk = (snapshot_state_t *) Hunk_AllocName (cl_max_edicts*sizeof(snapshot_state_t), "cl_snap_chunk");
	cl.snapshot_chunk_present = (byte *) Hunk_AllocName (cl_max_edicts*sizeof(byte), "cl_snap_chunk_present");
	cl.snapshot_stage = (snapshot_state_t *) Hunk_AllocName (cl_max_edicts*sizeof(snapshot_state_t), "cl_snap_stage");
	cl.snapshot_stage_present = (byte *) Hunk_AllocName (cl_max_edicts*sizeof(byte), "cl_snap_stage_present");
	cl.snapshot_stage_remove = (byte *) Hunk_AllocName (cl_max_edicts*sizeof(byte), "cl_snap_stage_remove");
	cl.entity_snapshots = (cl_entity_snap_t *) Hunk_AllocName (cl_max_edicts * CL_ENTITY_SNAP_HISTORY * sizeof(cl_entity_snap_t), "cl_ent_snaps");
	cl.entity_snap_head = (byte *) Hunk_AllocName (cl_max_edicts * sizeof(byte), "cl_ent_snap_head");
	cl.entity_snap_count = (byte *) Hunk_AllocName (cl_max_edicts * sizeof(byte), "cl_ent_snap_count");
	//johnfitz
	if (cl_entities)
		memset (cl_entities, 0, cl_max_edicts * sizeof(entity_t));
	if (cl.snapshot_present)
		memset (cl.snapshot_present, 0, cl_max_edicts * sizeof(byte));
	if (cl.snapshot_active)
		memset (cl.snapshot_active, 0, cl_max_edicts * sizeof(byte));
	if (cl.snapshot_last_update_time)
		memset (cl.snapshot_last_update_time, 0, cl_max_edicts * sizeof(double));
	if (cl.snapshot_chunk_present)
		memset (cl.snapshot_chunk_present, 0, cl_max_edicts * sizeof(byte));
	if (cl.snapshot_stage_present)
		memset (cl.snapshot_stage_present, 0, cl_max_edicts * sizeof(byte));
	if (cl.snapshot_stage_remove)
		memset (cl.snapshot_stage_remove, 0, cl_max_edicts * sizeof(byte));
	if (cl.entity_snapshots)
		memset (cl.entity_snapshots, 0, cl_max_edicts * CL_ENTITY_SNAP_HISTORY * sizeof(cl_entity_snap_t));
	if (cl.entity_snap_head)
		memset (cl.entity_snap_head, 0, cl_max_edicts * sizeof(byte));
	if (cl.entity_snap_count)
		memset (cl.entity_snap_count, 0, cl_max_edicts * sizeof(byte));
	cl.mtime[0] = 0.0;
	cl.mtime[1] = 0.0;
	cl.time = 0.0;
	cl.oldtime = 0.0;
	cl_lerpfrac = 1.0f;
	cl_viewent_needs_init = true;
	cl.snapshot_chunk_active = false;
	cl.snapshot_chunk_seq = 0;
	cl.snapshot_chunk_start_time = 0;
	cl.snapshot_chunk_packets = 0;
	cl.snap_last_applied_seq = 0;
	cl.snap_last_complete_seq = 0;
	cl.snap_last_incomplete_seq = 0;
	cl.snap_last_incomplete = false;
	cl.has_full_snapshot = false;
	cl.need_full_snapshot = true;
	cl.has_valid_worldstate = false;
	cl.snap_parse_errors = 0;
	cl.snap_delta_mismatch = 0;
	cl.snap_incomplete_count = 0;
	cl.snap_rem0_seq = 0;
	cl.snap_rem0_base = 0;
	cl.snap_rem0_count = 0;
	cl.snapshot_chunk_expected_total = 0;
	cl.snapshot_chunk_received = 0;
	cl.snapshot_chunk_remaining = 0;

	memset (v_punchangles, 0, sizeof (v_punchangles));

	CL_Predict_Clear ();
}

/*
=====================
CL_Disconnect

Sends a disconnect message to the server
This is also called on Host_Error, so it shouldn't cause any errors
=====================
*/
void CL_Disconnect (void)
{
	if (key_dest == key_message)
		Key_EndChat ();	// don't get stuck in chat mode

// stop sounds (especially looping!)
	S_StopAllSounds (true);
	BGM_Pause ();

// if running a local server, shut it down
	if (cls.demoplayback)
		CL_StopPlayback ();
	else if (cls.state == ca_connected)
	{
		if (cls.demorecording)
			CL_Stop_f ();

		Con_DPrintf ("Sending clc_disconnect\n");
		SZ_Clear (&cls.message);
		MSG_WriteByte (&cls.message, clc_disconnect);
		NET_SendUnreliableMessage (cls.netcon, &cls.message);
		SZ_Clear (&cls.message);
		NET_Close (cls.netcon);

		cls.state = ca_disconnected;
		if (sv.active)
			Host_ShutdownServer(false);
	}

	cls.demoplayback = cls.timedemo = false;
	cls.demopaused = false;
	cl.intermission = 0;
	cl.sendprespawn = false;
	CL_ClearSignons ();

	V_ResetEffects ();
	DLightPool_Clear ();
}

void CL_Disconnect_f (void)
{
	CL_Disconnect ();
	BGM_Stop ();
	CDAudio_Stop ();
	if (sv.active)
		Host_ShutdownServer (false);
}


/*
=====================
CL_EstablishConnection

Host should be either "local" or a net address to be passed on
=====================
*/
void CL_EstablishConnection (const char *host)
{
	if (cls.state == ca_dedicated)
		return;

	if (cls.demoplayback)
		return;

	CL_Disconnect ();

	cls.netcon = NET_Connect (host);
	if (!cls.netcon)
		Host_Error ("CL_Connect: connect failed");
	Con_DPrintf ("CL_EstablishConnection: connected to %s\n", host);

	cls.demonum = -1;			// not in the demo loop now
	cls.state = ca_connected;
	CL_ClearSignons ();			// need all the signon messages before playing
	MSG_WriteByte (&cls.message, clc_nop);	// NAT Fix from ProQuake
}

/*
=====================
CL_SignonReply

An svc_signonnum has been received, perform a client side setup
=====================
*/
void CL_SignonReply (void)
{
	char 	str[8192];

	Con_DPrintf ("CL_SignonReply: %i\n", cls.signon);

	switch (cls.signon)
	{
	case 1:
		cl.sendprespawn = true;
		break;

	case 2:
		MSG_WriteByte (&cls.message, clc_stringcmd);
		MSG_WriteString (&cls.message, va("name \"%s\"\n", cl_name.string));

		MSG_WriteByte (&cls.message, clc_stringcmd);
		MSG_WriteString (&cls.message, va("color %i %i\n", ((int)cl_color.value)>>4, ((int)cl_color.value)&15));

		if (cl_packedents.value)
		{
			MSG_WriteByte (&cls.message, clc_stringcmd);
			MSG_WriteString (&cls.message, "packedents 1");
		}

		MSG_WriteByte (&cls.message, clc_stringcmd);
		sprintf (str, "spawn %s", cls.spawnparms);
		MSG_WriteString (&cls.message, str);
		break;

	case 3:
		MSG_WriteByte (&cls.message, clc_stringcmd);
		MSG_WriteString (&cls.message, "begin");
		Cache_Report ();		// print remaining memory
		break;

	case 4:
		cl.spawntime = cl.mtime[0];
		SCR_EndLoadingPlaque ();		// allow normal screen updates
		break;
	}

}

/*
=====================
CL_NextDemo

Called to play the next demo in the demo loop
=====================
*/
void CL_NextDemo (void)
{
	char	str[1024];

	if (cls.demonum == -1)
		return;		// don't play demos

	if (!cls.demos[cls.demonum][0] || cls.demonum == MAX_DEMOS)
	{
		cls.demonum = 0;
		if (!cls.demos[cls.demonum][0])
		{
			Con_Printf ("No demos listed with startdemos\n");
			cls.demonum = -1;
			CL_Disconnect();
			return;
		}
	}

	SCR_BeginLoadingPlaque ();

	sprintf (str,"playdemo %s 1\n", cls.demos[cls.demonum]);
	Cbuf_InsertText (str);
	cls.demonum++;
}

/*
==============
CL_PrintEntities_f
==============
*/
void CL_PrintEntities_f (void)
{
	entity_t	*ent;
	int			i;

	if (cls.state != ca_connected)
		return;

	for (i=0,ent=cl_entities ; i<cl.num_entities ; i++,ent++)
	{
		Con_Printf ("%3i:",i);
		if (!ent->model)
		{
			Con_Printf ("EMPTY\n");
			continue;
		}
		Con_Printf ("%s:%2i  (%5.1f,%5.1f,%5.1f) [%5.1f %5.1f %5.1f]\n"
		,ent->model->name,ent->frame, ent->origin[0], ent->origin[1], ent->origin[2], ent->angles[0], ent->angles[1], ent->angles[2]);
	}
}

/*
===============
CL_IsPlayerEnt
===============
*/
qboolean CL_IsPlayerEnt (const entity_t *ent)
{
	return PTR_IN_RANGE (ent, cl_entities + 1, cl_entities + 1 + cl.maxclients);
}

/*
===============
CL_SetLightstyle

Sets the lightstyle map and updates the length, peak, and average values
===============
*/
void CL_SetLightstyle (int i, const char *str)
{
	q_strlcpy (cl_lightstyle[i].map, str, MAX_STYLESTRING);
	cl_lightstyle[i].length = Q_strlen(cl_lightstyle[i].map);
	//johnfitz -- save extra info
	if (cl_lightstyle[i].length)
	{
		int j, total = 0;
		cl_lightstyle[i].peak = 'a';
		for (j=0; j<cl_lightstyle[i].length; j++)
		{
			total += cl_lightstyle[i].map[j] - 'a';
			cl_lightstyle[i].peak = q_max(cl_lightstyle[i].peak, cl_lightstyle[i].map[j]);
		}
		cl_lightstyle[i].average = total / cl_lightstyle[i].length + 'a';
	}
	else
		cl_lightstyle[i].average = cl_lightstyle[i].peak = 'm';
	//johnfitz
}

/*
===============
CL_AllocDlight

===============
*/
dlight_t *CL_AllocDlight (int key)
{
	dlight_t *dl = DLightPool_AllocTransientByKey (key, cl.time);

	dl->key = key;
	dl->color[0] = dl->color[1] = dl->color[2] = 1; //johnfitz -- lit support via lordhavoc
	dl->type = DLIGHT_DEFAULT;
	dl->spawn = cl.time - 0.001;
	dl->flicker_seed = (float) rand ();
	dl->active = true;
	dl->kind = DL_TRANSIENT;
	return dl;
}


/*
===============
CL_DecayLights

===============
*/
/*
===============
CL_DecayLights
===============
*/
void CL_DecayLights (void)
{
	float time;

	time = cl.time - cl.oldtime;
	DLightPool_Decay (time, cl.time);
}


/*
===============
CL_LerpPoint}
}


/*
===============
CL_LerpPoint

Determines the fraction between the last two messages that the objects
should be put at.
===============
*/
float	CL_LerpPoint (void)
{
	float	f, frac;

	f = cl.mtime[0] - cl.mtime[1];

	if (f <= 0.0f || cls.timedemo || (sv.active && !host_netinterval))
	{
		cl.time = cl.mtime[0];
		cl.mtime[1] = cl.mtime[0];
		return 1;
	}

	if (f > 0.1) // dropped packet, or start of demo
	{
		cl.mtime[1] = cl.mtime[0] - 0.1;
		f = 0.1;
	}

	frac = (cl.time - cl.mtime[1]) / f;

	if (frac < 0.0f || frac > 1.0f)
	{
		cl.time = cl.mtime[0];
		cl.mtime[1] = cl.mtime[0];
		frac = 1.0f;
	}

	//johnfitz -- better nolerp behavior
	if (cl_nolerp.value)
		return 1;
	//johnfitz

	return frac;
}

/*
===============
CL_ResetTrail
===============
*/
static void CL_ResetTrail (entity_t *ent)
{
	ent->traildelay = 1.f / 72.f;
	VectorCopy (ent->origin, ent->trailorg);
}

/*
===============
CL_RocketTrail

Rate-limiting wrapper over R_RocketTrail
===============
*/
static void CL_RocketTrail (entity_t *ent, int type)
{
	ent->traildelay -= cl.time - cl.oldtime;
	if (ent->traildelay > 0.f)
		return;
	R_RocketTrail (ent->trailorg, ent->origin, type);
	CL_ResetTrail (ent);
}

/*
===============
			CL_SetDlightColorForEntity

Tint dynamic lights to better match their source.
===============
*/
static void CL_SetDlightColorForEntity (dlight_t *dl, const entity_t *ent)
{
        const char *name;

        if (!dl || !ent || !ent->model)
                return;

        name = ent->model->name;

        if (ent->model->flags & EF_TRACER3)
        {
                dl->color[0] = 0.60f; dl->color[1] = 0.25f; dl->color[2] = 0.80f;
                return;
        }

        if (ent->model->flags & EF_TRACER2)
        {
                dl->color[0] = 0.95f; dl->color[1] = 0.55f; dl->color[2] = 0.20f;
                return;
        }

        if (ent->model->flags & EF_TRACER)
        {
                dl->color[0] = 0.30f; dl->color[1] = 0.85f; dl->color[2] = 0.30f;
                return;
        }

        if (name && (q_strcasestr (name, "acid") || q_strcasestr (name, "spit")))
        {
                dl->color[0] = 0.20f; dl->color[1] = 0.95f; dl->color[2] = 0.20f;
                return;
        }

        if (name && (q_strcasestr (name, "fire") || q_strcasestr (name, "flame") || q_strcasestr (name, "torch")))
        {
                dl->type = DLIGHT_TORCH;
                return;
        }

        if (name && q_strcasestr (name, "lava"))
        {
                dl->color[0] = 1.00f; dl->color[1] = 0.15f; dl->color[2] = 0.05f;
                dl->type = DLIGHT_LAVA;
                return;
        }

        if (name && q_strcasestr (name, "wiz"))
        {
                dl->color[0] = 0.20f; dl->color[1] = 0.95f; dl->color[2] = 0.20f;
                return;
        }

        if (name && (q_strcasestr (name, "missile") || q_strcasestr (name, "rocket")))
        {
                dl->color[0] = 1.00f; dl->color[1] = 0.70f; dl->color[2] = 0.30f;
                dl->type = DLIGHT_ROCKET;
                return;
        }
}

/*
===============
CL_RelinkEntities
===============
*/
void CL_RelinkEntities (void)
{
	entity_t	*ent;
	int			i, j;
	float		frac, f, d;
	vec3_t		delta;
	float		bobjrotate;
	dlight_t	*dl;
	qboolean		viewentity_teleported = false;

// determine partial update time
	frac = CL_LerpPoint ();
	cl_lerpfrac = frac;

	cl_numvisedicts = 0;

//
// interpolate player info
//
	for (i=0 ; i<3 ; i++)
		cl.velocity[i] = cl.mvelocity[1][i] +
			frac * (cl.mvelocity[0][i] - cl.mvelocity[1][i]);

	SCR_UpdateZoom ();

	if (cls.demoplayback)
	{
	// interpolate the angles
		for (j=0 ; j<3 ; j++)
		{
			d = cl.mviewangles[0][j] - cl.mviewangles[1][j];
			if (d > 180)
				d -= 360;
			else if (d < -180)
				d += 360;
			cl.viewangles[j] = cl.mviewangles[1][j] + frac*d;
		}
	}

	bobjrotate = anglemod(100*cl.time);

// start on the entity after the world
	for (i=1,ent=cl_entities+1 ; i<cl.num_entities ; i++,ent++)
	{
		qboolean teleported = false;

		if (!ent->model)
		{	// empty slot
			
			// ericw -- efrags are only used for static entities in GLQuake
			// ent can't be static, so this is a no-op.
			//if (ent->forcelink)
			//	R_RemoveEfrags (ent);	// just became empty
			continue;
		}

// if the object wasn't included in the last packet, remove it (or hold briefly)
		if (ent->msgtime != cl.mtime[0])
		{
			qboolean keepalive = false;
			double timeout_s = cl_entity_timeout_ms.value / 1000.0;

			if (cl.snap_last_incomplete)
			{
				keepalive = true;
			}

			if (timeout_s > 0.0 && cl.snapshot_active && cl.snapshot_active[i] && cl.snapshot_last_update_time)
			{
				if (cl.time - cl.snapshot_last_update_time[i] <= timeout_s)
				{
					keepalive = true;
				}
			}
			else
			{
				keepalive = true;
			}

			if (keepalive)
			{
				ent->msgtime = cl.mtime[0];
			}
			else
			{
				ent->model = NULL;
				ent->lerpflags |= LERP_RESETMOVE|LERP_RESETANIM; //johnfitz -- next time this entity slot is reused, the lerp will need to be reset
				if (cl.snapshot_active)
					cl.snapshot_active[i] = 0;
				continue;
			}
		}

		if (ent->forcelink)
		{	// the entity was not updated in the last message
			// so move to the final spot
			VectorCopy (ent->msg_origins[0], ent->origin);
			VectorCopy (ent->msg_angles[0], ent->angles);
		}
		else
		{	// if the delta is large, assume a teleport and don't lerp
			f = frac;
			if (ent == &cl_entities[cl.viewentity] && cl_viewent_needs_init)
			{
				f = 1.0f;
				ent->lerpflags |= LERP_RESETMOVE;
			}
			for (j=0 ; j<3 ; j++)
			{
				delta[j] = ent->msg_origins[0][j] - ent->msg_origins[1][j];
				if (delta[j] > 100 || delta[j] < -100)
				{
					f = 1;		// assume a teleportation, not a motion
					ent->lerpflags |= LERP_RESETMOVE; //johnfitz -- don't lerp teleports
					teleported = true;
				}
			}

			//johnfitz -- don't cl_lerp entities that will be r_lerped
			if (r_lerpmove.value && (ent->lerpflags & LERP_MOVESTEP))
				f = 1;
			//johnfitz

		// interpolate the origin and angles
			for (j=0 ; j<3 ; j++)
			{
				ent->origin[j] = ent->msg_origins[1][j] + f*delta[j];

				d = ent->msg_angles[0][j] - ent->msg_angles[1][j];
				if (d > 180)
					d -= 360;
				else if (d < -180)
					d += 360;
				ent->angles[j] = ent->msg_angles[1][j] + f*d;
			}
		}

		if (i <= cl.maxclients && i != cl.viewentity)
		{
			vec3_t lerp_org;
			vec3_t lerp_ang;

			if (CL_GetInterpolatedPlayer (i - 1, lerp_org, lerp_ang))
			{
				VectorCopy (lerp_org, ent->origin);
				VectorCopy (lerp_ang, ent->angles);
			}
		}
		else if (i > cl.maxclients)
		{
			vec3_t lerp_org;
			vec3_t lerp_ang;

			if (CL_GetInterpolatedEntity (i, lerp_org, lerp_ang))
			{
				VectorCopy (lerp_org, ent->origin);
				VectorCopy (lerp_ang, ent->angles);
			}
		}

		if (ent->forcelink || ent->lerpflags & LERP_RESETMOVE)
			CL_ResetTrail (ent);

		if (ent == &cl_entities[cl.viewentity] && teleported)
			viewentity_teleported = true;

// rotate binary objects locally
		if (ent->model->flags & EF_ROTATE)
			ent->angles[1] = bobjrotate;

		if (ent->effects & EF_BRIGHTFIELD)
			R_EntityParticles (ent);

		if (ent->effects & EF_MUZZLEFLASH)
		{
			vec3_t		fv, rv, uv;

			dl = CL_AllocDlight (i);
			VectorCopy (ent->origin,  dl->origin);
			dl->origin[2] += 16;
			AngleVectors (ent->angles, fv, rv, uv);

			VectorMA (dl->origin, 18, fv, dl->origin);
			dl->radius = 200 + (rand()&31);
			dl->baseradius = dl->radius;
			dl->minlight = 32;
			dl->die = cl.time + 0.1;
			dl->color[0] = 1.00f; dl->color[1] = 0.70f; dl->color[2] = 0.30f;
			// Give muzzle flashes a per-shot color flicker for a punchier look.
			{
				float flicker = (float) sin (cl.time * 30.0 + dl->flicker_seed);
				dl->color[0] *= 1.0f + flicker * 0.05f;
				dl->color[1] *= 1.0f - flicker * 0.03f;
			}
			CL_SetDlightColorForEntity (dl, ent);

			//johnfitz -- assume muzzle flash accompanied by muzzle flare, which looks bad when lerped
			if (r_lerpmodels.value != 2)
			{
			if (ent == &cl_entities[cl.viewentity])
				cl.viewent.lerpflags |= LERP_RESETANIM|LERP_RESETANIM2; //no lerping for two frames
			else
				ent->lerpflags |= LERP_RESETANIM|LERP_RESETANIM2; //no lerping for two frames
			}
			//johnfitz
		}
                if (ent->effects & EF_BRIGHTLIGHT)
                {
                        dl = CL_AllocDlight (i);
                        VectorCopy (ent->origin,  dl->origin);
                        dl->origin[2] += 16;
                        dl->radius = 400 + (rand()&31);
                        dl->baseradius = dl->radius;
                        dl->die = cl.time + 0.001;
                        CL_SetDlightColorForEntity (dl, ent);
                }
                if (ent->effects & EF_DIMLIGHT)
                {
                        dl = CL_AllocDlight (i);
                        VectorCopy (ent->origin,  dl->origin);
                        dl->radius = 200 + (rand()&31);
                        dl->baseradius = dl->radius;
                        dl->die = cl.time + 0.001;
                        CL_SetDlightColorForEntity (dl, ent);
                }
                if (ent->effects & EF_QEX_QUADLIGHT)
                {
                        dl = CL_AllocDlight (i);
                        VectorCopy (ent->origin,  dl->origin);
                        dl->radius = 200 + (rand()&31);
                        dl->baseradius = dl->radius;
                        dl->die = cl.time + 0.001;
                        dl->color[0] = 0.25f;
                        dl->color[1] = 0.25f;
                        dl->color[2] = 1.0f;
                }
                if (ent->effects & EF_QEX_PENTALIGHT)
                {
                        dl = CL_AllocDlight (i);
                        VectorCopy (ent->origin,  dl->origin);
                        dl->radius = 200 + (rand()&31);
                        dl->baseradius = dl->radius;
                        dl->die = cl.time + 0.001;
                        dl->color[0] = 1.0f;
                        dl->color[1] = 0.25f;
                        dl->color[2] = 0.25f;
                }

		if (ent->model->flags & EF_GIB)
			CL_RocketTrail (ent, 2);
		else if (ent->model->flags & EF_ZOMGIB)
			CL_RocketTrail (ent, 4);
		else if (ent->model->flags & EF_TRACER)
			CL_RocketTrail (ent, 3);
		else if (ent->model->flags & EF_TRACER2)
			CL_RocketTrail (ent, 5);
                else if (ent->model->flags & EF_ROCKET)
                {
                        CL_RocketTrail (ent, 0);
                        dl = CL_AllocDlight (i);
                        VectorCopy (ent->origin, dl->origin);
                        dl->radius = 200;
                        dl->baseradius = dl->radius;
                        dl->die = cl.time + 0.01;
                        dl->type = DLIGHT_ROCKET;
                        CL_SetDlightColorForEntity (dl, ent);
                }
                else if (ent->model->flags & EF_GRENADE)
			CL_RocketTrail (ent, 1);
		else if (ent->model->flags & EF_TRACER3)
			CL_RocketTrail (ent, 6);
		else
			CL_ResetTrail (ent);

		ent->forcelink = false;

		if (i == cl.viewentity && !chase_active.value)
			continue;

		if (cl_numvisedicts < MAX_VISEDICTS)
		{
			cl_visedicts[cl_numvisedicts] = ent;
			cl_numvisedicts++;
		}
	}

	if (viewentity_teleported)
		cl.teleport_fx_time = cl.time;

	if (cl_lerp_debug.value > 0.0f)
	{
		double now = Sys_DoubleTime ();

		if (now >= cl_lerp_debug_next_time)
		{
			Con_Printf ("lerp: miss_pairs %d hold_newest %d hold_oldest %d gap_holds %d\n",
				cl_lerp_miss_pairs, cl_lerp_hold_newest, cl_lerp_hold_oldest, cl_lerp_gap_holds);
			cl_lerp_miss_pairs = 0;
			cl_lerp_hold_newest = 0;
			cl_lerp_hold_oldest = 0;
			cl_lerp_gap_holds = 0;
			cl_lerp_debug_next_time = now + 1.0;
		}
	}
}


/*
===============
CL_ReadFromServer

Read all incoming data from the server
===============
*/
int CL_ReadFromServer (void)
{
	int			ret;
	extern int	num_temp_entities; //johnfitz
	int			num_beams = 0; //johnfitz
	int			num_dlights = 0; //johnfitz
	beam_t		*b; //johnfitz
	int			i; //johnfitz

	CL_AdvanceTime ();

	do
	{
		ret = CL_GetMessage ();
		if (ret == -1)
			Host_Error ("CL_ReadFromServer: lost server connection");
		if (!ret)
			break;

		cl.last_received_message = realtime;
		CL_ParseServerMessage ();
	} while (ret && cls.state == ca_connected);

	if (cl_shownet.value)
		Con_Printf ("\n");

	CL_EnsureViewEntityOrigin ("render");
	CL_RelinkEntities ();
	CL_UpdateTEnts ();

//johnfitz -- devstats

	//visedicts
	if (cl_numvisedicts > 256 && dev_peakstats.visedicts <= 256)
		Con_DWarning ("%i visedicts exceeds standard limit of 256 (max = %d).\n", cl_numvisedicts, MAX_VISEDICTS);
	dev_stats.visedicts = cl_numvisedicts;
	dev_peakstats.visedicts = q_max(cl_numvisedicts, dev_peakstats.visedicts);

	//temp entities
	if (num_temp_entities > 64 && dev_peakstats.tempents <= 64)
		Con_DWarning ("%i tempentities exceeds standard limit of 64 (max = %d).\n", num_temp_entities, MAX_TEMP_ENTITIES);
	dev_stats.tempents = num_temp_entities;
	dev_peakstats.tempents = q_max(num_temp_entities, dev_peakstats.tempents);

	//beams
	for (i=0, b=cl_beams ; i< MAX_BEAMS ; i++, b++)
		if (b->model && b->starttime <= cl.time && b->endtime >= cl.time)
			num_beams++;
	if (num_beams > 24 && dev_peakstats.beams <= 24)
		Con_DWarning ("%i beams exceeded standard limit of 24 (max = %d).\n", num_beams, MAX_BEAMS);
	dev_stats.beams = num_beams;
	dev_peakstats.beams = q_max(num_beams, dev_peakstats.beams);

	//dlights
	{
		dlight_pool_stats_t stats;
		DLightPool_GetStats (&stats);
		num_dlights = stats.active;
	}
	if (num_dlights > 32 && dev_peakstats.dlights <= 32)
		Con_DWarning ("%i dlights exceeded standard limit of 32 (max = %d).\n", num_dlights, DLIGHT_GPU_MAX);
	dev_stats.dlights = num_dlights;
	dev_peakstats.dlights = q_max(num_dlights, dev_peakstats.dlights);

//johnfitz

//
// bring the links up to date
//
	return 0;
}

/*
=================
CL_UpdateViewAngles

Spike: split from CL_SendCmd, to do clientside viewangle changes separately from outgoing packets.
=================
*/
void CL_AccumulateCmd (void)
{
	if (cls.signon == SIGNONS)
	{
		//basic keyboard looking
		CL_AdjustAngles ();

		//accumulate movement from other devices
		IN_Move (&cl.pendingcmd);
	}
}

/*
=================
CL_SendCmd
=================
*/
void CL_SendCmd (void)
{
	usercmd_t		cmd;

	if (cls.state != ca_connected)
		return;

	if (cls.signon == SIGNONS)
	{
	// get basic movement from keyboard
		CL_BaseMove (&cmd);

	// allow mice or other external controllers to add to the move
		cmd.forwardmove	+= cl.pendingcmd.forwardmove;
		cmd.sidemove	+= cl.pendingcmd.sidemove;
		cmd.upmove		+= cl.pendingcmd.upmove;

		VectorCopy (cl.viewangles, cmd.viewangles);
		cmd.buttons = 0;
		if ( in_attack.state & 3 )
			cmd.buttons |= 1;
		in_attack.state &= ~2;

		if (in_jump.state & 3)
		cmd.buttons |= 2;
		in_jump.state &= ~2;

		cmd.impulse = in_impulse;
		in_impulse = 0;

		// Freeze local movement until we have a ready viewentity to avoid void spawns.
		if (!CL_WorldReady ())
		{
			cmd.forwardmove = 0;
			cmd.sidemove = 0;
			cmd.upmove = 0;
			cmd.buttons = 0;
			cmd.impulse = 0;
		}

		CL_Predict_SetupCmd (&cmd);

	// send the unreliable message
		CL_SendMove (&cmd);
	}
	else
		CL_SendMove (NULL);
	memset(&cl.pendingcmd, 0, sizeof(cl.pendingcmd));

	if (cls.demoplayback)
	{
		SZ_Clear (&cls.message);
		return;
	}

// send the reliable message
	if (!cls.message.cursize)
		return;		// no message at all

	if (!NET_CanSendMessage (cls.netcon))
	{
		Con_DPrintf ("CL_SendCmd: can't send\n");
		return;
	}

	if (NET_SendMessage (cls.netcon, &cls.message) == -1)
		Host_Error ("CL_SendCmd: lost server connection");

	SZ_Clear (&cls.message);
}

/*
=============
CL_Tracepos_f -- johnfitz

display impact point of trace along VPN
=============
*/
void CL_Tracepos_f (void)
{
	vec3_t	v, w;

	if (cls.state != ca_connected)
		return;

	VectorMA(r_refdef.vieworg, 8192.0, vpn, v);
	TraceLine(r_refdef.vieworg, v, w);

	if (VectorLength(w) == 0)
		Con_Printf ("Tracepos: trace didn't hit anything\n");
	else
		Con_Printf ("Tracepos: (%i %i %i)\n", (int)w[0], (int)w[1], (int)w[2]);
}

/*
=============
CL_Viewpos_f -- johnfitz

display client's position and angles
=============
*/
void CL_Viewpos_f (void)
{
	char buf[256];
	if (cls.state != ca_connected)
		return;

	// player position
	q_snprintf (buf, sizeof (buf),
		"(%.0f %.0f %.0f) %.0f %.0f %.0f",
		cl_entities[cl.viewentity].origin[0],
		cl_entities[cl.viewentity].origin[1],
		cl_entities[cl.viewentity].origin[2],
		cl.viewangles[PITCH],
		cl.viewangles[YAW],
		cl.viewangles[ROLL]
	);
	Con_SafePrintf ("Player pos: %s\n", buf);

	if (Cmd_Argc () >= 2 && !q_strcasecmp (Cmd_Argv (1), "copy"))
		if (SDL_SetClipboardText (buf) < 0)
			Con_SafePrintf ("Clipboard copy failed: %s\n", SDL_GetError ());

	// camera position
	Con_SafePrintf ("Camera pos: (%.0f %.0f %.0f) %.0f %.0f %.0f\n",
		r_refdef.vieworg[0],
		r_refdef.vieworg[1],
		r_refdef.vieworg[2],
		r_refdef.viewangles[PITCH],
		r_refdef.viewangles[YAW],
		r_refdef.viewangles[ROLL]
	);

	// sun mangle
	Con_SafePrintf ("Sun mangle: %.0f %.0f %.0f\n",
		NormalizeAngle (r_refdef.viewangles[YAW]) + 180.f,
		r_refdef.viewangles[PITCH],
		r_refdef.viewangles[ROLL]
	);
}

/*
===============
CL_Viewpos_Completion_f -- tab completion for the viewpos command
===============
*/
static void CL_Viewpos_Completion_f (const char *partial)
{
	if (Cmd_Argc () != 2)
		return;
	Con_AddToTabList ("copy", partial, NULL);
}

/*
=============
CL_SetStat_f
=============
*/
void CL_SetStat_f (void)
{
	int i, argc, stnum;
	double value;

	for (i = 1, argc = Cmd_Argc (); i + 1 < argc; i += 2)
	{
		stnum = atoi (Cmd_Argv (i));
		if (stnum < 0 || stnum >= MAX_CL_STATS)
			Host_Error ("CL_SetStat_f: stnum(%d) >= MAX_CL_STATS\n", stnum);

		value = atof (Cmd_Argv (i + 1));
		cl.statsf[stnum] = (float)value;
		cl.stats[stnum] = (int)value;
	}
}

/*
=============
CL_SetStatString_f
=============
*/
void CL_SetStatString_f (void)
{
	int i, argc, stnum;

	for (i = 1, argc = Cmd_Argc (); i + 1 < argc; i += 2)
	{
		stnum = atoi (Cmd_Argv (i));
		if (stnum < 0 || stnum >= MAX_CL_STATS)
			Host_Error ("CL_SetStatString_f: stnum(%d) >= MAX_CL_STATS\n", stnum);

		free (cl.statss[stnum]);
		cl.statss[stnum] = strdup (Cmd_Argv (i + 1));
	}
}

/*
=================
V_Water_f
=================
*/
void V_Water_f (void)
{
	if (Cmd_Argc () < 2)
		return;
	cl.forceunderwater = atoi (Cmd_Argv (1));
}

/*
=================
CL_Init
=================
*/
void CL_Init (void)
{
	cmd_function_t *cmd;

	SZ_Alloc (&cls.message, 1024);

	CL_InitInput ();
	CL_InitTEnts ();
	DLightPool_Init ();
	CL_PostFX_Init ();

	Cvar_RegisterVariable (&cl_name);
	Cvar_RegisterVariable (&cl_color);
	Cvar_RegisterVariable (&cl_upspeed);
	Cvar_RegisterVariable (&cl_forwardspeed);
	Cvar_RegisterVariable (&cl_backspeed);
	Cvar_RegisterVariable (&cl_sidespeed);
	Cvar_RegisterVariable (&cl_movespeedkey);
	Cvar_RegisterVariable (&cl_yawspeed);
	Cvar_RegisterVariable (&cl_pitchspeed);
	Cvar_RegisterVariable (&cl_anglespeedkey);
	Cvar_RegisterVariable (&cl_shownet);
	Cvar_RegisterVariable (&cl_netdebug_parse);
	Cvar_RegisterVariable (&cl_netdebug_hexdump);
	Cvar_RegisterVariable (&cl_netdebug_dropbad);
	Cvar_RegisterVariable (&cl_netdebug_maxdump);
	Cvar_RegisterVariable (&cl_signon_chunk_debug);
	Cvar_RegisterVariable (&cl_signon_debug);
	Cvar_RegisterVariable (&cl_nolerp);
	Cvar_RegisterVariable (&cl_packedents);
	Cvar_RegisterVariable (&cl_snap_debug);
	Cvar_RegisterVariable (&cl_delta_reject_debug);
	Cvar_RegisterVariable (&cl_full_reasm_debug);
	Cvar_RegisterVariable (&cl_full_reasm_timeout_ms);
	Cvar_RegisterVariable (&cl_test_drop);
	Cvar_RegisterVariable (&cl_entity_timeout_ms);
	Cvar_RegisterVariable (&cl_lerp_ms);
	Cvar_RegisterVariable (&cl_lerp_max_gap_ms);
	Cvar_RegisterVariable (&cl_lerp_debug);
	Cvar_RegisterVariable (&freelook);
	Cvar_RegisterVariable (&lookspring);
	Cvar_RegisterVariable (&lookstrafe);
	Cvar_RegisterVariable (&sensitivity);
	
	Cvar_RegisterVariable (&cl_alwaysrun);

	Cvar_RegisterVariable (&m_pitch);
	Cvar_RegisterVariable (&m_yaw);
	Cvar_RegisterVariable (&m_forward);
	Cvar_RegisterVariable (&m_side);

	Cvar_RegisterVariable (&cfg_unbindall);

	Cvar_RegisterVariable (&cl_maxpitch); //johnfitz -- variable pitch clamping
	Cvar_RegisterVariable (&cl_minpitch); //johnfitz -- variable pitch clamping

	Cvar_RegisterVariable (&cl_mwheelpitch);

	Cvar_RegisterVariable (&cl_startdemos);
	Cvar_RegisterVariable (&cl_confirmquit);

	Cmd_AddCommand ("entities", CL_PrintEntities_f);
	Cmd_AddCommand ("disconnect", CL_Disconnect_f);
	Cmd_AddCommand ("record", CL_Record_f);
	Cmd_AddCommand ("stop", CL_Stop_f);
	Cmd_AddCommand ("playdemo", CL_PlayDemo_f);
	Cmd_AddCommand ("timedemo", CL_TimeDemo_f);

	Cmd_AddCommand ("tracepos", CL_Tracepos_f); //johnfitz
	cmd = Cmd_AddCommand ("viewpos", CL_Viewpos_f); //johnfitz
	if (cmd)
		cmd->completion = CL_Viewpos_Completion_f;

	Cmd_AddCommand_ServerCommand ("st", CL_SetStat_f);
	Cmd_AddCommand_ServerCommand ("sts", CL_SetStatString_f);

	Cmd_AddCommand_ServerCommand ("v_water", V_Water_f);
}
