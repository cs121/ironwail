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

#ifndef _CLIENT_H_
#define _CLIENT_H_

// client.h

#include "../common/lightgrid.h"

// snapshot baseline history
#define CL_SNAPSHOT_BASELINE_HISTORY 4

typedef struct
{
	int		length;
	char	map[MAX_STYLESTRING];
	char	average; //johnfitz
	char	peak; //johnfitz
	vec3_t	colours; // FTE extended lightstyles
	int		colourkey;
} lightstyle_t;

typedef struct
{
	char	name[MAX_SCOREBOARDNAME];
	float	entertime;
	int		frags;
	int		colors;			// two 4 bit fields
	byte	translations[VID_GRADES*256];
} scoreboard_t;

typedef struct
{
	int		destcolor[3];
	float	percent;		// 0-256
} cshift_t;

extern cshift_t		cshift_empty;

#define	CSHIFT_CONTENTS	0
#define	CSHIFT_DAMAGE	1
#define	CSHIFT_BONUS	2
#define	CSHIFT_POWERUP	3
#define	NUM_CSHIFTS		4

#define	NAME_LENGTH	64


//
// client_state_t should hold all pieces of the client state
//

#define	SIGNONS		4			// signon messages to receive before connected

#define CL_ENTITY_SNAP_HISTORY 3

typedef struct cl_entity_snap_s
{
	double		servertime;
	vec3_t		origin;
	vec3_t		angles;
} cl_entity_snap_t;

typedef enum
{
	DLIGHT_DEFAULT = 0,
	DLIGHT_ROCKET,
	DLIGHT_PLASMA,
	DLIGHT_LIGHTNING,
	DLIGHT_EXPLOSION,
	DLIGHT_TORCH,
	DLIGHT_TELEPORT,
	DLIGHT_LAVA,
	DLIGHT_MAX_TYPES
} dlighttype_t;
typedef enum
{
	DL_TRANSIENT = 0,
	DL_PERSISTENT
} dlight_kind_t;
typedef struct dlight_s
{
	vec3_t	origin;
	float	radius;
	float	baseradius;
	float	spawn;				// stop lighting before this time
	float	die;				// stop lighting after this time
	float	decay;				// drop this each second
	float	minlight;			// don't add when contributing less
	int		key;
	vec3_t	color;				//johnfitz -- lit support via lordhavoc
	float	flicker_seed;
	dlighttype_t type;
	int		style;			// optional light style/flicker index (entity dlights)
	int		id;
	dlight_kind_t kind;
	qboolean active;
	float	spawn_time;
	float	last_score;
	int		flags;
	int		last_frame_touched;
} dlight_t;

#define	MAX_BEAMS	32 //johnfitz -- was 24
typedef struct
{
	int		entity;
	struct qmodel_s	*model;
	float	starttime;
	float	endtime;
	vec3_t	start, end;
} beam_t;

#define	MAX_MAPSTRING	2048
#define	MAX_DEMOS		8
#define	MAX_DEMONAME	16

typedef enum {
ca_dedicated, 		// a dedicated server with no ability to start a client
ca_disconnected, 	// full screen console with no connection
ca_connected		// valid netcon, talking to a server
} cactive_t;

//
// the client_static_t structure is persistant through an arbitrary number
// of server connections
//
typedef struct
{
	cactive_t	state;

// personalization data sent to server
	char		spawnparms[MAX_MAPSTRING];	// to restart a level

// demo loop control
	qboolean	demoloop;
	int			demonum;		// -1 = don't play demos
	char		demos[MAX_DEMOS][MAX_DEMONAME];	// when not playing

// demo recording info must be here, because record is started before
// entering a map (and clearing client_state_t)
	qboolean	demorecording;
	qboolean	demoplayback;

// did the user pause demo playback? (separate from cl.paused because we don't
// want a svc_setpause inside the demo to actually pause demo playback).
	qboolean	demopaused;

// demo playback speed (<0 = rewinding; 0 = paused; >0 = forward)
	float		demospeed;

// base demo speed; different from demospeed when fast-forwarding/rewinding or when playback is paused
// (we want to be able to set playback speed to 1/2x, pause, and then resume playback at 1/2x not 1x)
	float		basedemospeed;

	qboolean	timedemo;
	int		forcetrack;		// -1 = use normal cd track
	char		demofilename[MAX_OSPATH];
	FILE		*demofile;
	qfileofs_t	demofilestart;	// for demos in pak files
	qfileofs_t	demofilesize;
	int		td_lastframe;		// to meter out one message a frame
	int		td_startframe;		// host_framecount at start
	float		td_starttime;		// realtime at second frame of timedemo

// connection information
	int		signon;			// 0 to SIGNONS
	struct qsocket_s	*netcon;
	sizebuf_t	message;		// writing buffer to send to server

} client_static_t;

extern client_static_t	cls;

//
// the client_state_t structure is wiped completely at every
// server signon
//
typedef struct
{
	int			movemessages;	// since connecting to this server
								// throw out the first couple, so the player
								// doesn't accidentally do something the
								// first frame
	usercmd_t	cmd;			// last command sent to the server
	usercmd_t	pendingcmd;		// accumulated state from mice+joysticks.
	unsigned int	last_cmd_ack;		// last command acknowledged by the server
	unsigned int	last_cmd_ack_echo;	// last command ack echoed back by the server

// information for local display
	int			stats[MAX_CL_STATS];	// health, etc
	float		statsf[MAX_CL_STATS];
	char		*statss[MAX_CL_STATS];
	int			items;			// inventory bit flags
	float	item_gettime[32];	// cl.time of aquiring item, for blinking
	float		faceanimtime;	// use anim frame if cl.time < this

	cshift_t	cshifts[NUM_CSHIFTS];	// color shifts for damage, powerups
	cshift_t	prev_cshifts[NUM_CSHIFTS];	// and content types

// the client maintains its own idea of view angles, which are
// sent to the server each frame.  The server sets punchangle when
// the view is temporarliy offset, and an angle reset commands at the start
// of each level and after teleporting.
	vec3_t		mviewangles[2];	// during demo playback viewangles is lerped
								// between these
	vec3_t		viewangles;
	vec3_t		simorg;			// last simulated origin for viewentity

	vec3_t		mvelocity[2];	// update by server, used for lean+bob
								// (0 is newest)
	vec3_t		velocity;		// lerped between mvelocity[0] and [1]

	vec3_t		punchangle;		// temporary offset
	double		punchtime;

// pitch drifting vars
	float		idealpitch;
	float		pitchvel;
	qboolean	nodrift;
	float		driftmove;
	double		laststop;
	double		lastcenterstart;	// last time centerview was called

	float		wheel_pitch;	// for looking up/down using the mouse wheel

	float		viewheight;
	float		crouch;			// local amount for smoothing stepups
	double		teleport_fx_time;		// time when the last teleport effect should start

	qboolean	paused;			// send over by server
	qboolean	onground;
	qboolean	inwater;
	qboolean	fixangle;		// freeze view angle until next server frame

	int			intermission;	// don't change view angle, full screen, etc
	int			completed_time;	// latched at intermission start

	double		mtime[2];		// the timestamp of last two messages
	double		time;			// clients view of time, should be between
								// servertime and oldservertime to generate
								// a lerp point for other data
	double		oldtime;		// previous cl.time, time-oldtime is used
								// to decay light values and smooth step ups


	float		last_received_message;	// (realtime) for net trouble icon
	float		spawntime;		// time when signon 4 was received

//
// information that is static for the entire time connected to a server
//
	struct qmodel_s		*model_precache[MAX_MODELS];
	struct sfx_s		*sound_precache[MAX_SOUNDS];

	char		mapname[128];
	char		levelname[128];	// for display on solo scoreboard //johnfitz -- was 40.
	int			viewentity;		// cl_entitites[cl.viewentity] = player
	int			maxclients;
	int			gametype;

// refresh related state
	struct qmodel_s	*worldmodel;	// cl_entitites[0].model
	lightgrid_t	*lightgrid;
	int			num_efrags;
	int			num_entities;	// held in cl_entities array
	int			num_statics;	// held in cl_staticentities array
	entity_t	viewent;			// the gun model
	unsigned int snapshot_baseline_seq;
	unsigned short snap_last_applied_seq;
	unsigned short snap_last_complete_seq;
	unsigned short snap_last_incomplete_seq;
	qboolean	snap_last_incomplete;
	qboolean	has_full_snapshot;
	qboolean	need_full_snapshot;
	qboolean	has_valid_worldstate;
	int			snap_parse_errors;
	int			snap_parse_consecutive;
	int			snap_delta_mismatch;
	int			snap_incomplete_count;
	unsigned int snap_rem0_seq;
	unsigned int snap_rem0_base;
	int			snap_rem0_count;
	int			snapshot_baseline_head;
	int			snapshot_baseline_index;
	snapshot_state_t *snapshot_baseline;
	snapshot_state_t *snapshot_baselines[CL_SNAPSHOT_BASELINE_HISTORY];
	byte		*snapshot_present;
	byte		*snapshot_baseline_present[CL_SNAPSHOT_BASELINE_HISTORY];
	unsigned int snapshot_baseline_seqs[CL_SNAPSHOT_BASELINE_HISTORY];
	byte		snapshot_baseline_valid[CL_SNAPSHOT_BASELINE_HISTORY];
	byte		*snapshot_active;
	double		*snapshot_last_update_time;
	qboolean	snapshot_chunk_active;
	unsigned int snapshot_chunk_seq;
	unsigned int snapshot_chunk_expected_total;
	unsigned int snapshot_chunk_received;
	unsigned short snapshot_chunk_remaining;
	double		snapshot_chunk_start_time;
	int			snapshot_chunk_packets;
	snapshot_state_t *snapshot_chunk;
	byte		*snapshot_chunk_present;
	snapshot_state_t *snapshot_stage;
	byte		*snapshot_stage_present;
	byte		*snapshot_stage_remove;
	cl_entity_snap_t *entity_snapshots;
	byte		*entity_snap_head;
	byte		*entity_snap_count;

	int			cdtrack, looptrack;	// cd audio

// frag scoreboard
	scoreboard_t	*scores;		// [cl.maxclients]

	unsigned	protocol; //johnfitz
	unsigned	protocolflags;
	qboolean	signon_chunking;
	byte		signon_chunk_stage;
	unsigned short	signon_chunk_next_seq;
	unsigned short	signon_frag_id;
	unsigned short	signon_frag_total;
	unsigned short	signon_frag_offset;
	byte		*signon_frag_buf;

	qboolean	sendprespawn;

	char		stuffcmdbuf[1024];	//comment-extensions are a thing with certain servers, make sure we can handle them properly without further hacks/breakages. there's also some server->client only console commands that we might as well try to handle a bit better, like reconnect

	qcvm_t		qcvm;	//for csqc.

	float		zoom;
	float		zoomdir;

	qboolean	forceunderwater;	// force underwater warping/sound distortion even when camera is not submerged (e.g. alk1.2 liquidbrush)
} client_state_t;


//
// cvars
//
extern	cvar_t	cl_name;
extern	cvar_t	cl_color;

extern	cvar_t	cl_upspeed;
extern	cvar_t	cl_forwardspeed;
extern	cvar_t	cl_backspeed;
extern	cvar_t	cl_sidespeed;

extern	cvar_t	cl_movespeedkey;

extern	cvar_t	cl_yawspeed;
extern	cvar_t	cl_pitchspeed;

extern	cvar_t	cl_anglespeedkey;

extern	cvar_t	cl_alwaysrun; // QuakeSpasm

extern	cvar_t	cl_autofire;

extern	cvar_t	cl_shownet;
extern	cvar_t	cl_nolerp;

extern	cvar_t	cfg_unbindall;

extern	cvar_t	cl_pitchdriftspeed;
extern	cvar_t	freelook;
extern	cvar_t	lookspring;
extern	cvar_t	lookstrafe;
extern	cvar_t	sensitivity;

extern	cvar_t	m_pitch;
extern	cvar_t	m_yaw;
extern	cvar_t	m_forward;
extern	cvar_t	m_side;

extern	cvar_t	cl_startdemos;
extern	cvar_t	cl_confirmquit;
extern	cvar_t	cl_snap_debug;
extern	cvar_t	cl_snapshot_debug;
extern	cvar_t	cl_delta_reject_debug;
extern	cvar_t	cl_full_reasm_debug;
extern	cvar_t	cl_full_reasm_timeout_ms;
extern	cvar_t	cl_test_drop;
extern	cvar_t	cl_entity_timeout_ms;
extern	cvar_t	cl_lerp_ms;
extern	cvar_t	cl_lerp_max_gap_ms;
extern	cvar_t	cl_lerp_debug;
extern	float	cl_lerpfrac;


#define	MAX_TEMP_ENTITIES	256		//johnfitz -- was 64
#define	MAX_STATIC_ENTITIES	4096	//ericw -- was 512	//johnfitz -- was 128
#define	MAX_VISEDICTS		16384	// larger, now we support BSP2

extern	client_state_t	cl;

// FIXME, allocate dynamically
extern	entity_t		cl_static_entities[MAX_STATIC_ENTITIES];
extern	int			cl_max_lightstyles;
extern	lightstyle_t	cl_lightstyle[MAX_LIGHTSTYLES];
extern	entity_t		cl_temp_entities[MAX_TEMP_ENTITIES];
extern	beam_t			cl_beams[MAX_BEAMS];
extern	entity_t		*cl_visedicts[MAX_VISEDICTS];
extern	int				cl_numvisedicts;

static inline qboolean CL_DlightIsActive (const dlight_t *dl)
{
	return dl && dl->active && dl->die >= cl.time && dl->spawn <= cl.time && dl->baseradius > 0;
}

static inline qboolean CL_DlightShouldFlicker (const dlight_t *dl)
{
	return dl && (dl->type == DLIGHT_EXPLOSION || dl->type == DLIGHT_TORCH || dl->type == DLIGHT_LAVA);
}

extern	entity_t		*cl_entities; //johnfitz -- was a static array, now on hunk
extern	int				cl_max_edicts; //johnfitz -- only changes when new map loads

//=============================================================================

//
// cl_main
//
dlight_t *CL_AllocDlight (int key);
void	CL_DecayLights (void);
void	CL_SetLightstyle (int i, const char *str);

void CL_Init (void);

void CL_EstablishConnection (const char *host);

void CL_Disconnect (void);
void CL_Disconnect_f (void);
void CL_NextDemo (void);

//
// cl_input
//
typedef struct
{
	int		down[2];		// key nums holding it down
	int		state;			// low bit is down state
} kbutton_t;

extern	kbutton_t	in_mlook, in_klook;
extern 	kbutton_t 	in_strafe;
extern 	kbutton_t 	in_speed;
extern	kbutton_t	in_attack;
extern	kbutton_t	in_jump;
extern	int			in_impulse;

void CL_InitInput (void);
void CL_AccumulateCmd (void);
void CL_SendCmd (void);
void CL_SendMove (const usercmd_t *cmd);
void CL_Predict_Clear (void);
void CL_Predict_SetupCmd (usercmd_t *cmd);
void CL_Predict_ServerUpdate (unsigned int ack, const vec3_t origin, const vec3_t velocity, const vec3_t viewangles, qboolean onground);
void CL_Predict_Reapply (void);
qboolean CL_Predict_GetCmd (unsigned int seq, usercmd_t *out);
int  CL_ReadFromServer (void);
void CL_AdjustAngles (void);
void CL_BaseMove (usercmd_t *cmd);
qboolean CL_InCutscene (void);

qboolean CL_IsPlayerEnt (const entity_t *ent);

void CL_ParseTEnt (void);
void CL_UpdateTEnts (void);
void CL_ResetPlayerSnaps (void);

void CL_FreeState(void);
void CL_ClearState (void);

//
// cl_demo.c
//
void CL_StopPlayback (void);
int CL_GetMessage (void);
void CL_ClearSignons (void);
void CL_AdvanceTime (void);
void CL_FinishDemoFrame (void);
void CL_AddDemoRewindSound (int entnum, int channel, sfx_t *sfx, vec3_t pos, int vol, float atten);

void CL_Stop_f (void);
void CL_Record_f (void);
void CL_PlayDemo_f (void);
void CL_TimeDemo_f (void);

//
// cl_parse.c
//
void CL_ParseServerMessage (void);
void CL_NewTranslation (int slot);
void CL_RecordPlayerSnap (void);
qboolean CL_WorldReady (void);

//
// view
//
void V_StartPitchDrift (void);
void V_StopPitchDrift (void);

void V_RenderView (void);
void V_ParseDamage (void);
void V_AddExplosionVibration (const vec3_t origin);
void V_SetContentsColor (int contents);

//
// cl_tent
//
void CL_InitTEnts (void);
void CL_SignonReply (void);

//
// chase
//
extern	cvar_t	chase_active;

void Chase_Init (void);
void TraceLine (vec3_t start, vec3_t end, vec3_t impact);
void Chase_UpdateForClient (void);	//johnfitz
void Chase_UpdateForDrawing (void);	//johnfitz

#endif	/* _CLIENT_H_ */
