/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2016      Spike

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
// sv_main.c -- server main program

#include "quakedef.h"
#include "arch_def.h"
#include "net_sys.h"
#include "net_defs.h"
#include "net_dgrm.h"

server_t	sv;
server_static_t	svs;

static char	localmodels[MAX_MODELS][8];	// inline model names for precache

extern cvar_t nomonsters;

static cvar_t sv_netsort = {"sv_netsort", "1", CVAR_NONE};
static cvar_t sv_snapshotdelta = {"sv_snapshotdelta", "1", CVAR_NONE};
static cvar_t sv_snapshotdebug = {"sv_snapshotdebug", "0", CVAR_NONE};
static cvar_t sv_snapshottimeout = {"sv_snapshottimeout", "1000", CVAR_NONE};
static cvar_t sv_snapshot2 = {"sv_snapshot2", "1", CVAR_NONE};
static cvar_t sv_snap_debug = {"sv_snap_debug", "0", CVAR_NONE};
static cvar_t sv_snap_full_interval = {"sv_snap_full_interval", "2.0", CVAR_NONE};
static cvar_t sv_snap_force_full_after_frames = {"sv_snap_force_full_after_frames", "3", CVAR_NONE};
static cvar_t sv_snap_max_payload = {"sv_snap_max_payload", "1200", CVAR_NONE};
static cvar_t sv_event_max = {"sv_event_max", "0", CVAR_NONE};
cvar_t sv_cmd_ack_slack = {"sv_cmd_ack_slack", "64", CVAR_NONE};
cvar_t sv_cmd_ack_bad_limit = {"sv_cmd_ack_bad_limit", "3", CVAR_NONE};
cvar_t sv_cmd_ack_bad_window = {"sv_cmd_ack_bad_window", "2.0", CVAR_NONE};
cvar_t sv_cmd_ack_debug = {"sv_cmd_ack_debug", "0", CVAR_NONE};
cvar_t sv_mtu = {"sv_mtu", "1400", CVAR_NONE};
static cvar_t sv_mtu_cap = {"sv_mtu_cap", "1400", CVAR_NONE};
cvar_t sv_mtu_debug = {"sv_mtu_debug", "0", CVAR_NONE};
cvar_t sv_minrate = {"sv_minrate", "0", CVAR_NONE};
cvar_t sv_maxrate = {"sv_maxrate", "0", CVAR_NONE};
cvar_t sv_minupdaterate = {"sv_minupdaterate", "0", CVAR_NONE};
cvar_t sv_maxupdaterate = {"sv_maxupdaterate", "0", CVAR_NONE};
static cvar_t sv_signon_chunk_debug = {"sv_signon_chunk_debug", "0", CVAR_NONE};
static cvar_t sv_signon_chunk_window = {"sv_signon_chunk_window", "3", CVAR_NONE};
static cvar_t sv_signon_debug = {"sv_signon_debug", "0", CVAR_NONE};
static cvar_t net_test_drop = {"net_test_drop", "0", CVAR_NONE};
static cvar_t net_snap_tinyvel = {"net_snap_tinyvel", "1", CVAR_NONE};
static cvar_t net_snap_forcefull_frames = {"net_snap_forcefull_frames", "3", CVAR_NONE};
static cvar_t net_snap_debug = {"net_snap_debug", "0", CVAR_NONE};
static cvar_t sv_icull_enable = {"sv_icull_enable", "1", CVAR_NONE};
static cvar_t sv_icull_radius = {"sv_icull_radius", "2048", CVAR_NONE};
static cvar_t sv_icull_radius_players = {"sv_icull_radius_players", "0", CVAR_NONE};
static cvar_t sv_icull_pvs = {"sv_icull_pvs", "1", CVAR_NONE};
static cvar_t sv_icull_debug = {"sv_icull_debug", "0", CVAR_NONE};

static qboolean SV_SignonChunksEnabled (void);
static void SV_SignonStreamSend (client_t *client);

typedef struct
{
	int considered;
	int sent;
	int culled_distance;
	int culled_pvs;
	int forced_sent;
} sv_icull_stats_t;

typedef struct
{
	vec3_t	vieworg;
	float	radius2;
	byte	*pvs;
	qboolean	enabled;
	qboolean	radius_enabled;
	qboolean	pvs_enabled;
	qboolean	cull_players;
} sv_icull_context_t;

static sv_icull_stats_t sv_icull_last_stats[MAX_SCOREBOARD];
static double sv_icull_debug_next_time = 0.0;
static int sv_event_count = 0;

//============================================================================

static void SV_InitClientSnapshotData (client_t *client);
static void SV_ResetClientSnapshot (client_t *client);
static qboolean SV_IsLocalClient (client_t *client);

void SV_CalcStats(client_t *client, int *statsi, float *statsf, const char **statss)
{
	size_t i;
	edict_t *ent = client->edict;
	//FIXME: string stats!

	memset(statsi, 0, sizeof(*statsi)*MAX_CL_STATS);
	memset(statsf, 0, sizeof(*statsf)*MAX_CL_STATS);
	memset((void*)statss, 0, sizeof(*statss)*MAX_CL_STATS);
	statsf[STAT_HEALTH] = ent->v.health;
//	statsf[STAT_FRAGS] = ent->v.frags;	//obsolete
	statsi[STAT_WEAPON] = SV_ModelIndex(PR_GetString(ent->v.weaponmodel));
	//if ((unsigned int)statsi[STAT_WEAPON] >= client->limit_models)
	//	statsi[STAT_WEAPON] = 0;
	statsf[STAT_AMMO] = ent->v.currentammo;
	statsf[STAT_ARMOR] = ent->v.armorvalue;
	statsf[STAT_WEAPONFRAME] = ent->v.weaponframe;
	statsf[STAT_SHELLS] = ent->v.ammo_shells;
	statsf[STAT_NAILS] = ent->v.ammo_nails;
	statsf[STAT_ROCKETS] = ent->v.ammo_rockets;
	statsf[STAT_CELLS] = ent->v.ammo_cells;
	statsf[STAT_ACTIVEWEAPON] = ent->v.weapon;	//sent in a way that does NOT depend upon the current mod...

	//FIXME: add support for clientstat/globalstat qc builtins.

	for (i = 0; i < sv.numcustomstats; i++)
	{
		eval_t *eval = sv.customstats[i].ptr;
		if (!eval)
			eval = GetEdictFieldValue(ent, sv.customstats[i].fld);

		switch(sv.customstats[i].type)
		{
		case ev_ext_integer:
			statsi[sv.customstats[i].idx] = eval->_int;
			break;
		case ev_entity:
			statsi[sv.customstats[i].idx] = NUM_FOR_EDICT(PROG_TO_EDICT(eval->edict));
			break;
		case ev_float:
			statsf[sv.customstats[i].idx] = eval->_float;
			break;
		case ev_vector:
			statsf[sv.customstats[i].idx+0] = eval->vector[0];
			statsf[sv.customstats[i].idx+1] = eval->vector[1];
			statsf[sv.customstats[i].idx+2] = eval->vector[2];
			break;
		case ev_string:		//not supported in this build... send with svcfte_updatestatstring on change, which is annoying.
			statss[sv.customstats[i].idx] = PR_GetString(eval->string);
			break;
		case ev_void:		//nothing...
		case ev_field:		//panic! everyone panic!
		case ev_function:	//doesn't make much sense
		case ev_pointer:	//doesn't make sense
		default:
			break;
		}
	}
}

static void SV_IcullStats_f (void)
{
	int i;

	if (!sv_icull_enable.value)
		Con_Printf ("icull: sv_icull_enable is 0 (showing last recorded stats)\n");

	for (i = 0; i < svs.maxclients && i < MAX_SCOREBOARD; i++)
	{
		client_t *client = &svs.clients[i];

		if (!client->active)
			continue;

		Con_Printf ("icull %s: considered %d sent %d dist %d pvs %d forced %d\n",
			client->name,
			sv_icull_last_stats[i].considered,
			sv_icull_last_stats[i].sent,
			sv_icull_last_stats[i].culled_distance,
			sv_icull_last_stats[i].culled_pvs,
			sv_icull_last_stats[i].forced_sent);
	}
}

/*
===============
SV_Init
===============
*/
void SV_Init (void)
{
	int		i;
	const char	*p;
	extern	cvar_t	sv_maxvelocity;
	extern	cvar_t	sv_gravity;
	extern	cvar_t	sv_nostep;
	extern	cvar_t	sv_freezenonclients;
	extern	cvar_t	sv_friction;
	extern	cvar_t	sv_edgefriction;
	extern	cvar_t	sv_stopspeed;
	extern	cvar_t	sv_maxspeed;
	extern	cvar_t	sv_accelerate;
	extern	cvar_t	sv_idealpitchscale;
	extern	cvar_t	sv_aim;
	extern	cvar_t	sv_altnoclip; //johnfitz
	extern	cvar_t	sv_gameplayfix_random;
	extern	cvar_t	sv_gameplayfix_elevators;
	extern	cvar_t	sv_autoload;
	extern	cvar_t	sv_autosave;
	extern	cvar_t	sv_autosave_interval;

	Cvar_RegisterVariable (&sv_maxvelocity);
	Cvar_RegisterVariable (&sv_gravity);
	Cvar_RegisterVariable (&sv_friction);
	Cvar_SetCallback (&sv_gravity, Host_Callback_Notify);
	Cvar_SetCallback (&sv_friction, Host_Callback_Notify);
	Cvar_RegisterVariable (&sv_edgefriction);
	Cvar_RegisterVariable (&sv_stopspeed);
	Cvar_RegisterVariable (&sv_maxspeed);
	Cvar_SetCallback (&sv_maxspeed, Host_Callback_Notify);
	Cvar_RegisterVariable (&sv_accelerate);
	Cvar_RegisterVariable (&sv_idealpitchscale);
	Cvar_RegisterVariable (&sv_aim);
	Cvar_RegisterVariable (&sv_nostep);
	Cvar_RegisterVariable (&sv_freezenonclients);
	Cvar_RegisterVariable (&pr_checkextension);
	Cvar_RegisterVariable (&sv_altnoclip); //johnfitz
	Cvar_RegisterVariable (&sv_gameplayfix_random);
	Cvar_RegisterVariable (&sv_gameplayfix_elevators);
	Cvar_RegisterVariable (&sv_netsort);
	Cvar_RegisterVariable (&sv_snapshotdelta);
	Cvar_RegisterVariable (&sv_snapshotdebug);
	Cvar_RegisterVariable (&sv_snapshottimeout);
	Cvar_RegisterVariable (&sv_snapshot2);
	Cvar_RegisterVariable (&sv_snap_debug);
	Cvar_RegisterVariable (&sv_snap_full_interval);
	Cvar_RegisterVariable (&sv_snap_force_full_after_frames);
	Cvar_RegisterVariable (&sv_snap_max_payload);
	Cvar_RegisterVariable (&sv_event_max);
	Cvar_RegisterVariable (&sv_cmd_ack_slack);
	Cvar_RegisterVariable (&sv_cmd_ack_bad_limit);
	Cvar_RegisterVariable (&sv_cmd_ack_bad_window);
	Cvar_RegisterVariable (&sv_cmd_ack_debug);
	Cvar_RegisterVariable (&sv_mtu);
	Cvar_RegisterVariable (&sv_mtu_cap);
	Cvar_RegisterVariable (&sv_mtu_debug);
	Cvar_RegisterVariable (&sv_minrate);
	Cvar_RegisterVariable (&sv_maxrate);
	Cvar_RegisterVariable (&sv_minupdaterate);
	Cvar_RegisterVariable (&sv_maxupdaterate);
	Cvar_RegisterVariable (&sv_signon_chunk_debug);
	Cvar_RegisterVariable (&sv_signon_chunk_window);
	Cvar_RegisterVariable (&sv_signon_debug);
	Cvar_RegisterVariable (&net_test_drop);
	Cvar_RegisterVariable (&net_snap_tinyvel);
	Cvar_RegisterVariable (&net_snap_forcefull_frames);
	Cvar_RegisterVariable (&net_snap_debug);
	Cvar_RegisterVariable (&sv_icull_enable);
	Cvar_RegisterVariable (&sv_icull_radius);
	Cvar_RegisterVariable (&sv_icull_radius_players);
	Cvar_RegisterVariable (&sv_icull_pvs);
	Cvar_RegisterVariable (&sv_icull_debug);
	Cvar_RegisterVariable (&sv_autoload);
	Cvar_RegisterVariable (&sv_autosave);
	Cvar_RegisterVariable (&sv_autosave_interval);

	Cmd_AddCommand ("sv_icull_stats", &SV_IcullStats_f);

	for (i=0 ; i<MAX_MODELS ; i++)
		sprintf (localmodels[i], "*%i", i);

	p = "RMQ";
	Sys_Printf ("Server using protocol %i (%s)\n", PROTOCOL_RMQ, p);
}

/*
=============================================================================

EVENT MESSAGES

=============================================================================
*/

/*
==================
SV_StartParticle

Make sure the event gets sent to all clients
==================
*/
void SV_StartParticle (vec3_t org, vec3_t dir, int color, int count)
{
	int		i, v;
	int		event_max = (int)sv_event_max.value;

	if (event_max > 0 && sv_event_count >= event_max)
		return;
	if (sv.datagram.cursize > MAX_DATAGRAM-18)
		return;
	sv_event_count++;
	MSG_WriteByte (&sv.datagram, svc_particle);
	MSG_WriteCoord (&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord (&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord (&sv.datagram, org[2], sv.protocolflags);
	for (i=0 ; i<3 ; i++)
	{
		v = dir[i]*16;
		if (v > 127)
			v = 127;
		else if (v < -128)
			v = -128;
		MSG_WriteChar (&sv.datagram, v);
	}
	MSG_WriteByte (&sv.datagram, count);
	MSG_WriteByte (&sv.datagram, color);
}

/*
==================
SV_StartSound

Each entity can have eight independant sound sources, like voice,
weapon, feet, etc.

Channel 0 is an auto-allocate channel, the others override anything
already running on that entity/channel pair.

An attenuation of 0 will play full volume everywhere in the level.
Larger attenuations will drop off.  (max 4 attenuation)

==================
*/
void SV_StartSound (edict_t *entity, int channel, const char *sample, int volume, float attenuation)
{
	int			sound_num, ent;
	int			i, field_mask;

	if (volume < 0 || volume > 255)
		Host_Error ("SV_StartSound: volume = %i", volume);

	if (attenuation < 0 || attenuation > 4)
		Host_Error ("SV_StartSound: attenuation = %f", attenuation);

	if (channel < 0 || channel > 7)
		Host_Error ("SV_StartSound: channel = %i", channel);

	if (sv.datagram.cursize > MAX_DATAGRAM-21)
		return;

// find precache number for sound
	for (sound_num = 1; sound_num < MAX_SOUNDS && sv.sound_precache[sound_num]; sound_num++)
	{
		if (!strcmp(sample, sv.sound_precache[sound_num]))
			break;
	}

	if (sound_num == MAX_SOUNDS || !sv.sound_precache[sound_num])
	{
		Con_Printf ("SV_StartSound: %s not precached\n", sample);
		return;
	}

	ent = NUM_FOR_EDICT(entity);

	field_mask = 0;
	if (volume != DEFAULT_SOUND_PACKET_VOLUME)
		field_mask |= SND_VOLUME;
	if (attenuation != DEFAULT_SOUND_PACKET_ATTENUATION)
		field_mask |= SND_ATTENUATION;

	if (ent >= 8192)
		field_mask |= SND_LARGEENTITY;
	if (sound_num >= 256 || channel >= 8)
		field_mask |= SND_LARGESOUND;

	if (sv.datagram.cursize > MAX_DATAGRAM-21)
		return;

// directed messages go only to the entity the are targeted on
	MSG_WriteByte (&sv.datagram, svc_sound);
	MSG_WriteByte (&sv.datagram, field_mask);
	if (field_mask & SND_VOLUME)
		MSG_WriteByte (&sv.datagram, volume);
	if (field_mask & SND_ATTENUATION)
		MSG_WriteByte (&sv.datagram, attenuation*64);

	if (field_mask & SND_LARGEENTITY)
	{
		MSG_WriteShort (&sv.datagram, ent);
		MSG_WriteByte (&sv.datagram, channel);
	}
	else
		MSG_WriteShort (&sv.datagram, (ent<<3) | channel);
	if (field_mask & SND_LARGESOUND)
		MSG_WriteShort (&sv.datagram, sound_num);
	else
		MSG_WriteByte (&sv.datagram, sound_num);

	for (i = 0; i < 3; i++)
		MSG_WriteCoord (&sv.datagram, entity->v.origin[i]+0.5*(entity->v.mins[i]+entity->v.maxs[i]), sv.protocolflags);
}

/*
==================
SV_LocalSound - for 2021 rerelease
==================
*/
void SV_LocalSound (client_t *client, const char *sample)
{
	int	sound_num, field_mask;

	for (sound_num = 1; sound_num < MAX_SOUNDS && sv.sound_precache[sound_num]; sound_num++)
	{
		if (!strcmp(sample, sv.sound_precache[sound_num]))
			break;
	}
	if (sound_num == MAX_SOUNDS || !sv.sound_precache[sound_num])
	{
		Con_Printf ("SV_LocalSound: %s not precached\n", sample);
		return;
	}

	field_mask = 0;
	if (sound_num >= 256)
		field_mask = SND_LARGESOUND;

	if (client->message.cursize > client->message.maxsize-4)
		return;

	MSG_WriteByte (&client->message, svc_localsound);
	MSG_WriteByte (&client->message, field_mask);
	if (field_mask & SND_LARGESOUND)
		MSG_WriteShort (&client->message, sound_num);
	else
		MSG_WriteByte (&client->message, sound_num);
}

/*
==============================================================================

CLIENT SPAWNING

==============================================================================
*/

static qboolean SV_IsLocalClient (client_t *client)
{
	return Q_strcmp (NET_QSocketGetAddressString (client->netconnection), "LOCAL") == 0;
}

static int SV_MTUCap (client_t *client)
{
	(void)client;
	int cap = (int)sv_mtu_cap.value;
	int mtu = (int)sv_mtu.value;
	int payload;

	if (cap <= 0)
		cap = mtu;
	else if (mtu > 0)
		cap = q_min (cap, mtu);
	if (cap <= 0)
		return 0;

	payload = cap - NET_UDPIP_HEADER_BYTES - NET_HEADERSIZE;
	if (payload < 1)
		payload = 1;
	return payload;
}

/*
================
SV_SendServerinfo

Sends the first message from the server to a connected client.
This will be sent on the initial connection and upon each server load.
================
*/
void SV_SendServerinfo (client_t *client)
{
	const char		**s;
	char			message[2048];
	int				i; //johnfitz

	SZ_Clear (&client->message);
	client->message.allowoverflow = true;
	client->message.overflowed = false;

	MSG_BEGINSVC (&client->message, svc_print);
	MSG_WriteByte (&client->message, svc_print);
	sprintf (message, "%c\nFITZQUAKE %1.2f SERVER (%i CRC)\n", 2, FITZQUAKE_VERSION, qcvm->crc); //johnfitz -- include fitzquake version
	MSG_WriteString (&client->message,message);

	MSG_BEGINSVC (&client->message, svc_serverinfo);
	MSG_WriteByte (&client->message, svc_serverinfo);
	MSG_WriteLong (&client->message, sv.protocol);
	MSG_WriteLong (&client->message, sv.protocolflags);
	
	MSG_WriteByte (&client->message, svs.maxclients);

	if (!coop.value && deathmatch.value)
		MSG_WriteByte (&client->message, GAME_DEATHMATCH);
	else
		MSG_WriteByte (&client->message, GAME_COOP);

	MSG_WriteString (&client->message, PR_GetString(qcvm->edicts->v.message));

	for (i = 1, s = sv.model_precache+1; *s; s++,i++)
	{
		MSG_DBGAUX (&client->message, i);
		MSG_WriteString (&client->message, *s);
	}
	MSG_WriteByte (&client->message, 0);

	for (i = 1, s = sv.sound_precache+1; *s; s++, i++)
	{
		MSG_DBGAUX (&client->message, i);
		MSG_WriteString (&client->message, *s);
	}
	MSG_WriteByte (&client->message, 0);

// send music
	MSG_BEGINSVC (&client->message, svc_cdtrack);
	MSG_WriteByte (&client->message, svc_cdtrack);
	MSG_WriteByte (&client->message, qcvm->edicts->v.sounds);
	MSG_WriteByte (&client->message, qcvm->edicts->v.sounds);

// set view
	MSG_BEGINSVC (&client->message, svc_setview);
	MSG_WriteByte (&client->message, svc_setview);
	MSG_WriteShort (&client->message, NUM_FOR_EDICT(client->edict));

	MSG_BEGINSVC (&client->message, svc_signonnum);
	MSG_WriteByte (&client->message, svc_signonnum);
	MSG_WriteByte (&client->message, 1);

	client->sendsignon = PRESPAWN_FLUSH;
	client->spawned = false;		// need prespawn, spawn, etc
}

/*
================
SV_ConnectClient

Initializes a client_t for a new net connection.  This will only be called
once for a player each game, not once for each level change.
================
*/
void SV_ConnectClient (int clientnum)
{
	edict_t			*ent;
	client_t		*client;
	int				edictnum;
	struct qsocket_s *netconnection;
	int				i;
	float			spawn_parms[NUM_SPAWN_PARMS];
	snapshot_state_t *snapshot_baselines[SV_SNAPSHOT_BASELINE_HISTORY];
	byte			*snapshot_baseline_present[SV_SNAPSHOT_BASELINE_HISTORY];
	snapshot_state_t *snapshot_pending;
	byte			*snapshot_pending_present;
	byte			*snapshot_pending_relevant;
	byte			*snapshot_pending_flags;

	client = svs.clients + clientnum;
	memcpy (snapshot_baselines, client->snapshot_baselines, sizeof(snapshot_baselines));
	memcpy (snapshot_baseline_present, client->snapshot_baseline_present, sizeof(snapshot_baseline_present));
	snapshot_pending = client->snapshot_pending;
	snapshot_pending_present = client->snapshot_pending_present;
	snapshot_pending_relevant = client->snapshot_pending_relevant;
	snapshot_pending_flags = client->snapshot_pending_flags;

	Con_DPrintf ("Client %s connected\n", NET_QSocketGetAddressString(client->netconnection));

	edictnum = clientnum+1;

	ent = EDICT_NUM(edictnum);

// set up the client_t
	netconnection = client->netconnection;

	if (sv.loadgame)
		memcpy (spawn_parms, client->spawn_parms, sizeof(spawn_parms));
	memset (client, 0, sizeof(*client));
	memcpy (client->snapshot_baselines, snapshot_baselines, sizeof(snapshot_baselines));
	memcpy (client->snapshot_baseline_present, snapshot_baseline_present, sizeof(snapshot_baseline_present));
	client->snapshot_pending = snapshot_pending;
	client->snapshot_pending_present = snapshot_pending_present;
	client->snapshot_pending_relevant = snapshot_pending_relevant;
	client->snapshot_pending_flags = snapshot_pending_flags;
	if (!client->snapshot_baselines[0] || !client->snapshot_baseline_present[0]
		|| !client->snapshot_pending || !client->snapshot_pending_present
		|| !client->snapshot_pending_relevant || !client->snapshot_pending_flags)
		SV_InitClientSnapshotData (client);
	SV_ResetClientSnapshot (client);
	client->netconnection = netconnection;
	client->rate = sv_minrate.value > 0.0f ? (int)sv_minrate.value : 0;
	client->updaterate = sv_minupdaterate.value > 0.0f ? (int)sv_minupdaterate.value : 0;

	strcpy (client->name, "unconnected");
	client->active = true;
	client->spawned = false;
	client->edict = ent;
	client->message.data = client->msgbuf;
	client->message.maxsize = sizeof(client->msgbuf);
	client->message.allowoverflow = true;		// we can catch it
	client->message.dbg_name = "client_message";
	if (SV_IsLocalClient (client))
		client->message.maxsize = NET_MAXMESSAGE - 4;

	if (sv.loadgame)
		memcpy (client->spawn_parms, spawn_parms, sizeof(spawn_parms));
	else
	{
	// call the progs to get default spawn parms for the new client
		PR_ExecuteProgram (pr_global_struct->SetNewParms);
		for (i=0 ; i<NUM_SPAWN_PARMS ; i++)
			client->spawn_parms[i] = (&pr_global_struct->parm1)[i];
	}

	SV_SendServerinfo (client);
}


/*
===================
SV_CheckForNewClients

===================
*/
void SV_CheckForNewClients (void)
{
	struct qsocket_s	*ret;
	int				i;

//
// check for new connections
//
	while (1)
	{
		ret = NET_CheckNewConnections ();
		if (!ret)
			break;

	//
	// init a new client structure
	//
		for (i=0 ; i<svs.maxclients ; i++)
			if (!svs.clients[i].active)
				break;
		if (i == svs.maxclients)
			Sys_Error ("Host_CheckForNewClients: no free clients");

		svs.clients[i].netconnection = ret;
		SV_ConnectClient (i);

		net_activeconnections++;
	}
}


/*
===============================================================================

FRAME UPDATES

===============================================================================
*/

/*
==================
SV_ClearDatagram

==================
*/
void SV_ClearDatagram (void)
{
	SZ_Clear (&sv.datagram);
	sv_event_count = 0;
}

/*
=============================================================================

The PVS must include a small area around the client to allow head bobbing
or other small motion on the client side.  Otherwise, a bob might cause an
entity that should be visible to not show up, especially when the bob
crosses a waterline.

=============================================================================
*/

static int	fatbytes;
static byte	*fatpvs;
static int	fatpvs_capacity;

void SV_AddToFatPVS (vec3_t org, mnode_t *node, qmodel_t *worldmodel) //johnfitz -- added worldmodel as a parameter
{
	int		i;
	byte	*pvs;
	mplane_t	*plane;
	float	d;

	while (1)
	{
	// if this is a leaf, accumulate the pvs bits
		if (node->contents < 0)
		{
			if (node->contents != CONTENTS_SOLID)
			{
				pvs = Mod_LeafPVS ( (mleaf_t *)node, worldmodel); //johnfitz -- worldmodel as a parameter
				for (i=0 ; i<fatbytes ; i++)
					fatpvs[i] |= pvs[i];
			}
			return;
		}

		plane = node->plane;
		d = DotProduct (org, plane->normal) - plane->dist;
		if (d > 8)
			node = node->children[0];
		else if (d < -8)
			node = node->children[1];
		else
		{	// go down both
			SV_AddToFatPVS (org, node->children[0], worldmodel); //johnfitz -- worldmodel as a parameter
			node = node->children[1];
		}
	}
}

/*
=============
SV_FatPVS

Calculates a PVS that is the inclusive or of all leafs within 8 pixels of the
given point.
=============
*/
byte *SV_FatPVS (vec3_t org, qmodel_t *worldmodel) //johnfitz -- added worldmodel as a parameter
{
	fatbytes = (worldmodel->numleafs+7)>>3; // ericw -- was +31, assumed to be a bug/typo
	fatbytes = (fatbytes + VIS_ALIGN_MASK) & ~VIS_ALIGN_MASK; // round up
	if (fatpvs == NULL || fatbytes > fatpvs_capacity)
	{
		fatpvs_capacity = fatbytes;
		fatpvs = (byte *) realloc (fatpvs, fatpvs_capacity);
		if (!fatpvs)
			Sys_Error ("SV_FatPVS: realloc() failed on %d bytes", fatpvs_capacity);
	}
	
	Q_memset (fatpvs, 0, fatbytes);
	SV_AddToFatPVS (org, worldmodel->nodes, worldmodel); //johnfitz -- worldmodel as a parameter
	return fatpvs;
}

/*
=============
SV_EdictInPVS
=============
*/
qboolean SV_EdictInPVS (edict_t *test, byte *pvs)
{
	int i;
	for (i = 0 ; i < test->num_leafs ; i++)
		if (pvs[test->leafnums[i] >> 3] & (1 << (test->leafnums[i] & 7)))
			return true;
	return false;
}

/*
=============
SV_VisibleToClient -- johnfitz

PVS test encapsulated in a nice function
=============
*/
qboolean SV_VisibleToClient (edict_t *client, edict_t *test, qmodel_t *worldmodel)
{
	byte	*pvs;
	vec3_t	org;

	VectorAdd (client->v.origin, client->v.view_ofs, org);
	pvs = SV_FatPVS (org, worldmodel);

	return SV_EdictInPVS (test, pvs);
}

//=============================================================================

#define MAX_NET_EDICTS 65536

static uint16_t		net_edicts[MAX_NET_EDICTS];
static byte			net_edict_dists[MAX_NET_EDICTS];
static int			net_edict_bins[256];
static uint16_t		net_edicts_sorted[MAX_NET_EDICTS];

#define SNAPFLAG_FORCE_ORIGIN	(1u<<0)
#define SNAPFLAG_FORCE_ANGLES	(1u<<1)
#define SNAPFLAG_HIRES_ORIGIN	(1u<<2)
#define SNAPFLAG_HIRES_ANGLES	(1u<<3)
#define SNAPFLAG_NO_DELTA		(1u<<4)

static qboolean SV_SnapshotSeqNewer (unsigned int a, unsigned int b)
{
	return NETSEQ_GT (a, b);
}

static int SV_FindSnapshotBaselineIndex (const client_t *client, unsigned int seq)
{
	int i;

	if (!seq)
		return -1;

	for (i = 0; i < SV_SNAPSHOT_BASELINE_HISTORY; i++)
	{
		if (!client->snapshot_baseline_valid[i])
			continue;
		if (client->snapshot_baseline_seqs[i] == seq)
			return i;
	}

	return -1;
}

static qboolean SV_SelectSnapshotBaseline (const client_t *client, unsigned int *out_seq,
	const snapshot_state_t **out_states, const byte **out_present)
{
	int index = client->snapshot_baseline_index;

	if (index < 0 || index >= SV_SNAPSHOT_BASELINE_HISTORY)
		return false;
	if (!client->snapshot_baseline_valid[index])
		return false;

	if (out_seq)
		*out_seq = client->snapshot_baseline_seqs[index];
	if (out_states)
		*out_states = client->snapshot_baselines[index];
	if (out_present)
		*out_present = client->snapshot_baseline_present[index];
	return true;
}

static void SV_StoreSnapshotBaseline (client_t *client, unsigned int seq,
	const snapshot_state_t *states, const byte *present)
{
	int index = client->snapshot_baseline_head;

	client->snapshot_baseline_head = (client->snapshot_baseline_head + 1) % SV_SNAPSHOT_BASELINE_HISTORY;
	client->snapshot_baseline_seqs[index] = seq;
	client->snapshot_baseline_valid[index] = 1;
	client->snapshot_baseline_index = index;
	memcpy (client->snapshot_baselines[index], states, qcvm->max_edicts * sizeof(snapshot_state_t));
	memcpy (client->snapshot_baseline_present[index], present, qcvm->max_edicts * sizeof(byte));
	client->snapshot_has_valid_base = true;
}

static void SV_InitClientSnapshotData (client_t *client)
{
	int max_edicts = qcvm->max_edicts;
	int i;

	for (i = 0; i < SV_SNAPSHOT_BASELINE_HISTORY; i++)
	{
		client->snapshot_baselines[i] = (snapshot_state_t *) Hunk_AllocName (max_edicts * sizeof(snapshot_state_t), "sv_snap_base");
		client->snapshot_baseline_present[i] = (byte *) Hunk_AllocName (max_edicts * sizeof(byte), "sv_snap_base_present");
	}
	client->snapshot_pending = (snapshot_state_t *) Hunk_AllocName (max_edicts * sizeof(snapshot_state_t), "sv_snap_pending");
	client->snapshot_pending_present = (byte *) Hunk_AllocName (max_edicts * sizeof(byte), "sv_snap_pending_present");
	client->snapshot_pending_relevant = (byte *) Hunk_AllocName (max_edicts * sizeof(byte), "sv_snap_pending_relevant");
	client->snapshot_pending_flags = (byte *) Hunk_AllocName (max_edicts * sizeof(byte), "sv_snap_pending_flags");
}

static void SV_ResetClientSnapshot (client_t *client)
{
	int max_edicts = qcvm->max_edicts;
	int i;

	client->snapshot_next_seq = 1;
	client->snapshot_pending_seq = 0;
	client->snapshot_no_progress_count = 0;
	client->snapshot_pending_baseline_seq = 0;
	client->snapshot_last_sent_seq = 0;
	client->snapshot_last_acked_seq = 0;
	client->snapshot_last_full_seq = 0;
	client->snapshot_last_full_time = 0;
	client->snapshot_last_acked_time = 0;
	client->snapshot_has_valid_base = false;
	client->snapshot_force_full = true;
	client->snapshot_pending_time = 0;
	client->snapshot_pending_is_delta = false;
	client->snapshot_unacked_frames = 0;
	client->snapshot_pending_mandatory = 0;
	client->snapshot_pending_dropped = 0;
	client->snapshot_pending_incomplete = false;
	client->snapshot_no_progress_seq = 0;
	client->snapshot_no_progress_base = 0;
	client->snapshot_no_progress_next_edict = 0;
	client->snapshot_no_progress_bytes = 0;
	client->snapshot_no_progress_remaining = 0;
	client->snapshot_no_progress_count = 0;
	client->snapshot_debug_flags = 0;
	client->snapshot_debug_base = 0;
	client->snapshot_debug_add = 0;
	client->snapshot_debug_update = 0;
	client->snapshot_debug_remove = 0;
	client->snapshot_debug_full_count = 0;
	client->snapshot_stats_full = 0;
	client->snapshot_stats_delta = 0;
	client->snapshot_stats_forced_full = 0;
	client->snapshot_stats_mandatory = 0;
	client->snapshot_stats_dropped = 0;
	client->snapshot_stats_next_time = 0;
	client->snapshot_debug_next_time = 0;
	client->mtu_last_snapshot_size = 0;
	client->mtu_dropped_tier1 = 0;
	client->mtu_dropped_tier2 = 0;
	client->mtu_debug_next_time = 0;
	client->datagram_overflow_next_time = 0;
	client->snapshot_baseline_head = 0;
	client->snapshot_baseline_index = -1;
	for (i = 0; i < SV_SNAPSHOT_BASELINE_HISTORY; i++)
	{
		client->snapshot_baseline_seqs[i] = 0;
		client->snapshot_baseline_valid[i] = 0;
		if (client->snapshot_baseline_present[i])
			memset (client->snapshot_baseline_present[i], 0, max_edicts * sizeof(byte));
	}
	client->entstream.active = false;
	client->entstream.signon_stage = 0;
	client->entstream.next_edict = 1;
	client->entstream.total_edicts = 0;
	client->entstream.base_snapshot = 0;
	client->entstream.total_entities = 0;
	client->entstream.sent_entities = 0;
	client->entstream.next_log_time = 0;
	if (client->snapshot_pending_present)
		memset (client->snapshot_pending_present, 0, max_edicts * sizeof(byte));
	if (client->snapshot_pending_relevant)
		memset (client->snapshot_pending_relevant, 0, max_edicts * sizeof(byte));
	if (client->snapshot_pending_flags)
		memset (client->snapshot_pending_flags, 0, max_edicts * sizeof(byte));
}

static void SV_InitEntityStream (client_t *client, int total_entities, unsigned int seq)
{
	client->entstream.active = true;
	client->entstream.signon_stage = client->sendsignon;
	client->entstream.next_edict = 1;
	client->entstream.total_edicts = qcvm->max_edicts;
	client->entstream.base_snapshot = (int)seq;
	client->entstream.total_entities = total_entities;
	client->entstream.sent_entities = 0;
	client->entstream.next_log_time = 0;
}

static qboolean SV_SnapshotMoverMoving (const edict_t *ent)
{
	if (VectorLengthSquared (ent->v.velocity) > 0.0f)
		return true;
	if (VectorLengthSquared (ent->v.avelocity) > 0.0f)
		return true;
	return false;
}

static qboolean SV_SnapshotTossMoving (const edict_t *ent)
{
	float tinyvel = net_snap_tinyvel.value;

	if (!((int)ent->v.flags & FL_ONGROUND))
		return true;
	return VectorLengthSquared (ent->v.velocity) > (tinyvel * tinyvel);
}

static void SV_InitInterestCullContext (sv_icull_context_t *ctx, const edict_t *clent)
{
	memset (ctx, 0, sizeof(*ctx));
	VectorAdd (clent->v.origin, clent->v.view_ofs, ctx->vieworg);

	ctx->enabled = (sv_icull_enable.value != 0.0f);
	if (!ctx->enabled)
	{
		ctx->pvs = SV_FatPVS (ctx->vieworg, sv.worldmodel);
		return;
	}

	ctx->radius_enabled = (sv_icull_radius.value > 0.0f);
	if (ctx->radius_enabled)
		ctx->radius2 = sv_icull_radius.value * sv_icull_radius.value;
	ctx->pvs_enabled = (sv_icull_pvs.value != 0.0f);
	ctx->cull_players = (sv_icull_radius_players.value != 0.0f);

	if (ctx->pvs_enabled)
	{
		mleaf_t *leaf = Mod_PointInLeaf (ctx->vieworg, sv.worldmodel);
		if (leaf)
			ctx->pvs = Mod_LeafPVS (leaf, sv.worldmodel);
	}
}

static qboolean SV_EntityMustSend (const edict_t *ent, const edict_t *clent)
{
	if (ent == clent)
		return true;
	if ((int)ent->v.movetype == MOVETYPE_PUSH && SV_SnapshotMoverMoving (ent))
		return true;
	if ((int)ent->v.effects & (EF_BRIGHTFIELD | EF_MUZZLEFLASH | EF_BRIGHTLIGHT | EF_DIMLIGHT
		| EF_QEX_QUADLIGHT | EF_QEX_PENTALIGHT | EF_QEX_CANDLELIGHT))
		return true;
	if ((int)ent->v.movetype == MOVETYPE_FLYMISSILE && ent->v.owner == EDICT_TO_PROG ((edict_t *)clent))
		return true;
	if (ent->v.sounds)
		return true;
	return false;
}

static qboolean SV_ShouldSendEntityToClient (const sv_icull_context_t *ctx, const edict_t *clent,
	edict_t *ent, sv_icull_stats_t *stats)
{
	if (stats)
		stats->considered++;

	if (!ent->v.modelindex || !PR_GetString(ent->v.model)[0])
		return false;

	if (!ctx->enabled)
	{
		if (ent->num_leafs < MAX_ENT_LEAFS && !SV_EdictInPVS (ent, ctx->pvs))
			return false;
		if (stats)
			stats->sent++;
		return true;
	}

	if (SV_EntityMustSend (ent, clent))
	{
		if (stats)
		{
			stats->sent++;
			stats->forced_sent++;
		}
		return true;
	}

	if (ctx->radius_enabled && (ctx->cull_players || !((int)ent->v.flags & FL_CLIENT)))
	{
		vec3_t delta;
		VectorSubtract (ent->v.origin, ctx->vieworg, delta);
		if (DotProduct (delta, delta) > ctx->radius2)
		{
			if (stats)
				stats->culled_distance++;
			if (sv_icull_debug.value >= 2.0f && stats && (stats->considered & 127) == 0)
				Con_DPrintf ("icull: ent %d culled by distance\n", NUM_FOR_EDICT(ent));
			return false;
		}
	}

	if (ctx->pvs_enabled && ctx->pvs && ent->num_leafs < MAX_ENT_LEAFS && !SV_EdictInPVS (ent, ctx->pvs))
	{
		if (stats)
			stats->culled_pvs++;
		if (sv_icull_debug.value >= 2.0f && stats && (stats->considered & 127) == 0)
			Con_DPrintf ("icull: ent %d culled by pvs\n", NUM_FOR_EDICT(ent));
		return false;
	}

	if (stats)
		stats->sent++;
	return true;
}

static void SV_IcullStoreStats (const sv_icull_stats_t *stats)
{
	int i;
	int client_index;

	if (!host_client)
		return;

	client_index = (int)(host_client - svs.clients);
	if (client_index < 0 || client_index >= svs.maxclients || client_index >= MAX_SCOREBOARD)
		return;

	sv_icull_last_stats[client_index] = *stats;

	if (sv_icull_debug.value < 1.0f || realtime < sv_icull_debug_next_time)
		return;

	sv_icull_debug_next_time = realtime + 2.0;
	for (i = 0; i < svs.maxclients && i < MAX_SCOREBOARD; i++)
	{
		client_t *client = &svs.clients[i];

		if (!client->active)
			continue;

		Con_Printf ("icull %s: considered %d sent %d dist %d pvs %d forced %d\n",
			client->name,
			sv_icull_last_stats[i].considered,
			sv_icull_last_stats[i].sent,
			sv_icull_last_stats[i].culled_distance,
			sv_icull_last_stats[i].culled_pvs,
			sv_icull_last_stats[i].forced_sent);
	}
}

static byte SV_SnapshotFlagsForEnt (const edict_t *ent)
{
	byte flags = 0;

	// Movers and tossed items jitter most when quantized or skipped.
	// Use higher-precision packing for these classes, and force updates while moving.
	if (ent->v.movetype == MOVETYPE_PUSH)
	{
		flags |= SNAPFLAG_HIRES_ORIGIN | SNAPFLAG_HIRES_ANGLES;
		if (SV_SnapshotMoverMoving (ent))
		{
			flags |= SNAPFLAG_FORCE_ORIGIN | SNAPFLAG_FORCE_ANGLES;
			flags |= SNAPFLAG_NO_DELTA;
		}
	}
	else if (ent->v.movetype == MOVETYPE_TOSS || ent->v.movetype == MOVETYPE_BOUNCE)
	{
		flags |= SNAPFLAG_HIRES_ORIGIN;
		if (SV_SnapshotTossMoving (ent) && ent->v.solid == SOLID_TRIGGER)
		{
			flags |= SNAPFLAG_FORCE_ORIGIN;
			flags |= SNAPFLAG_NO_DELTA;
		}
	}

	return flags;
}

static qboolean SV_SnapshotMandatoryEnt (const edict_t *ent)
{
	if ((int)ent->v.flags & FL_CLIENT)
		return true;
	return false;
}

static qboolean SV_SnapshotNeedsBaseline (const byte *baseline_present, int entnum)
{
	return baseline_present && !baseline_present[entnum];
}

static qboolean SV_SnapshotPriorityMover (const edict_t *ent)
{
	if ((int)ent->v.movetype == MOVETYPE_PUSH)
		return true;
	if ((int)ent->v.modelindex < 0)
		return true;
	return false;
}

static qboolean SV_SnapshotPriorityMissile (const edict_t *ent)
{
	if ((int)ent->v.movetype == MOVETYPE_FLYMISSILE)
		return true;
	if (((int)ent->v.movetype == MOVETYPE_TOSS || (int)ent->v.movetype == MOVETYPE_BOUNCE)
		&& SV_SnapshotTossMoving (ent))
		return true;
	return false;
}

static qboolean SV_SnapshotTier1Ent (const edict_t *ent)
{
	if (SV_SnapshotPriorityMover (ent))
		return true;
	if (SV_SnapshotPriorityMissile (ent))
		return true;
	if ((int)ent->v.solid == SOLID_BSP)
		return true;
	if (ent->v.velocity[0] || ent->v.velocity[1] || ent->v.velocity[2])
		return true;
	return false;
}

static int SV_SnapshotEntitySizeWorst (void)
{
	int coord_size;
	int angle_size;
	int field_size = 0;
	int extra_flags = (sv.protocolflags & PRFL_SNAPSHOT_HIRES) ? 1 : 0;
	int entry_overhead;

	if (sv.protocolflags & PRFL_INT32COORD)
		coord_size = 4;
	else
		coord_size = 2;

	if (sv.protocolflags & PRFL_SHORTANGLE)
		angle_size = 2;
	else
		angle_size = 1;

	coord_size = q_max (coord_size, 4);
	angle_size = q_max (angle_size, 4);

	field_size += 3 * coord_size;
	field_size += 3 * angle_size;
	field_size += 2; // modelindex
	field_size += 2; // frame
	field_size += 1; // colormap
	field_size += 1; // skin
	field_size += 1; // effects
	field_size += 1; // alpha
	field_size += 1; // scale
	field_size += 1; // step

	entry_overhead = q_max (extra_flags, 4); // worst-case delta includes a 32-bit mask
	return 2 + entry_overhead + field_size;
}

static int SV_SnapshotCoordBytes (qboolean hires)
{
	if (hires && (sv.protocolflags & PRFL_SNAPSHOT_HIRES))
		return 4;
	if (sv.protocolflags & PRFL_INT32COORD)
		return 4;
	return 2;
}

static int SV_SnapshotAngleBytes (qboolean hires)
{
	if (hires && (sv.protocolflags & PRFL_SNAPSHOT_HIRES))
		return 4;
	if (sv.protocolflags & PRFL_SHORTANGLE)
		return 2;
	return 1;
}

static int SV_Snapshot2EntitySizeForFlags (byte snapflags)
{
	int coord_size = SV_SnapshotCoordBytes ((snapflags & SNAPFLAG_HIRES_ORIGIN) != 0);
	int angle_size = SV_SnapshotAngleBytes ((snapflags & SNAPFLAG_HIRES_ANGLES) != 0);
	int state_size = 3 * coord_size + 3 * angle_size + 2 + 2 + 1 + 1 + 1 + 1 + 1 + 1;
	int extra_flags = (sv.protocolflags & PRFL_SNAPSHOT_HIRES) ? 1 : 0;

	return 2 + extra_flags + state_size;
}

static int SV_Snapshot2FullSize (const byte *present, const byte *snapflags, int max_edicts, int *out_count)
{
	int entnum;
	int count = 0;
	int size = 1 + 4 + 4 + 1 + 2 + 2;

	for (entnum = 1; entnum < max_edicts; entnum++)
	{
		if (!present[entnum])
			continue;
		count++;
		size += SV_Snapshot2EntitySizeForFlags (snapflags ? snapflags[entnum] : 0);
	}

	if (out_count)
		*out_count = count;

	return size;
}

static int SV_SnapshotHeaderBytes (void)
{
	if (sv_snapshot2.value)
		return 1 + 4 + 4 + 1 + 2 + 2 + 2 + 2;
	return 1 + 4 + 4 + 2 + 2 + 2;
}

static int SV_SnapshotEntityBudgetBytes (byte snapflags)
{
	if (sv_snapshot2.value)
		return SV_Snapshot2EntitySizeForFlags (snapflags);
	return SV_SnapshotEntitySizeWorst ();
}

static int SV_SnapshotBudgetBytes (client_t *client, sizebuf_t *msg,
	int *out_mtu_payload, int *out_current, int *out_remaining, int *out_header)
{
	int header_size = SV_SnapshotHeaderBytes ();
	int payload_cap = (int)sv_snap_max_payload.value;
	int mtu_cap = SV_MTUCap (client);
	int mtu_payload = msg->maxsize;
	int current = msg->cursize;
	int remaining;
	int budget;

	if (mtu_cap > 0)
		mtu_payload = q_min (mtu_payload, mtu_cap);

	remaining = mtu_payload - current;
	if (remaining < 0)
		remaining = 0;

	budget = remaining;
	if (payload_cap > 0)
	{
		int cap = payload_cap;
		if (mtu_cap > 0)
			cap = q_min (cap, mtu_cap);
		if (cap < 1)
			cap = 1;
		if (budget > cap)
			budget = cap;
	}
	if (client->rate > 0 && client->updaterate > 0)
	{
		int per_snapshot = client->rate / client->updaterate;
		if (per_snapshot < header_size + 1)
			per_snapshot = header_size + 1;
		if (budget > per_snapshot)
			budget = per_snapshot;
	}

	if (out_mtu_payload)
		*out_mtu_payload = mtu_payload;
	if (out_current)
		*out_current = current;
	if (out_remaining)
		*out_remaining = remaining;
	if (out_header)
		*out_header = header_size;

	return budget;
}

static int SV_BuildNetEdictsList (edict_t *clent)
{
	int		e, i, numents;
	vec3_t	org, forward, right, up;
	float	dist, size;
	edict_t	*ent;
	sv_icull_context_t icull;
	sv_icull_stats_t icull_stats = {0};

	SV_InitInterestCullContext (&icull, clent);
	VectorCopy (icull.vieworg, org);

	AngleVectors (clent->v.v_angle, forward, right, up);

	memset (net_edict_bins, 0, sizeof (net_edict_bins));

	if (sv_netsort.value)
	{
		net_edicts[0] = NUM_FOR_EDICT (clent);
		net_edict_dists[0] = 0;
		net_edict_bins[0] = 1;
	}
	else
		net_edicts_sorted[0] = NUM_FOR_EDICT (clent);
	numents = 1;
	icull_stats.considered++;
	icull_stats.sent++;
	icull_stats.forced_sent++;

	ent = NEXT_EDICT(qcvm->edicts);
	for (e=1 ; e<qcvm->num_edicts ; e++, ent = NEXT_EDICT(ent))
	{
		if (ent != clent)
		{
			if (!SV_ShouldSendEntityToClient (&icull, clent, ent, &icull_stats))
				continue;

			if (sv_netsort.value)
			{
				dist = size = 0.f;
				for (i=0 ; i<3 ; i++)
				{
					float delta = CLAMP (ent->v.absmin[i], org[i], ent->v.absmax[i]) - org[i];
					dist += delta * delta;
					delta = ent->v.absmax[i] - ent->v.absmin[i];
					size += delta * delta;
				}
				size = q_max (1.f, size);

				dist = 8.f * sqrt (sqrt (dist/size));
				net_edict_dists[numents] = (int) q_min (dist, 255.f);
				net_edicts[numents] = e;

				dist = 0.f;
				for (i=0 ; i<3 ; i++)
					dist += ((forward[i] < 0.f ? ent->v.absmin[i] : ent->v.absmax[i]) - org[i]) * forward[i];
				if (dist < 0.f)
					net_edict_dists[numents] |= 128;

				net_edict_bins[net_edict_dists[numents]]++;
			}
			else
				net_edicts_sorted[numents] = e;

			if (++numents == MAX_NET_EDICTS)
				break;
		}
	}

	if (sv_netsort.value)
	{
		e = 0;
		for (i=0 ; i<countof(net_edict_bins) ; i++)
		{
			int tmp = net_edict_bins[i];
			net_edict_bins[i] = e;
			e += tmp;
		}

		for (e=0 ; e<numents ; e++)
			net_edicts_sorted[net_edict_bins[net_edict_dists[e]]++] = net_edicts[e];
	}

	SV_IcullStoreStats (&icull_stats);

	return numents;
}

static void SV_FillSnapshotState (edict_t *ent, snapshot_state_t *out)
{
	int	i;
	eval_t	*val;

	for (i=0 ; i<3 ; i++)
	{
		out->state.origin[i] = ent->v.origin[i];
		out->state.angles[i] = ent->v.angles[i];
	}
	out->state.modelindex = ent->v.modelindex;
	out->state.frame = ent->v.frame;
	out->state.colormap = ent->v.colormap;
	out->state.skin = ent->v.skin;
	out->state.effects = (int)ent->v.effects & qcvm->effects_mask;

	val = GetEdictFieldValueByName(ent, "alpha");
	if (val)
		ent->alpha = ENTALPHA_ENCODE(val->_float);
	out->state.alpha = ent->alpha;

	val = GetEdictFieldValueByName(ent, "scale");
	if (val)
		ent->scale = ENTSCALE_ENCODE(val->_float);
	else
		ent->scale = ENTSCALE_DEFAULT;
	out->state.scale = ent->scale;

	out->step = (ent->v.movetype == MOVETYPE_STEP);
}

static int SV_BuildClientSnapshot (edict_t *clent, snapshot_state_t *states, byte *present,
	byte *relevant, byte *snapflags, int budget_bytes, int optional_budget_bytes, int header_bytes,
	const byte *baseline_present,
	int *out_mandatory, int *out_mandatory_bytes, int *out_total_bytes, int *out_dropped,
	int *out_dropped_tier1, int *out_dropped_tier2, qboolean debug_frame,
	qboolean *out_continuation, qboolean *out_mandatory_oversize)
{
	int	num, j, numents, count;
	int	k;
	int mandatory_count = 0;
	int mandatory_bytes = 0;
	int total_bytes = 0;
	int dropped_count = 0;
	int dropped_tier1 = 0;
	int dropped_tier2 = 0;
	int entity_budget = budget_bytes - header_bytes;
	int optional_entity_budget = optional_budget_bytes - header_bytes;
	int remaining_budget;
	qboolean continuation = false;
	edict_t	*ent;

	if (entity_budget < 0)
		entity_budget = 0;
	if (optional_entity_budget < 0)
		optional_entity_budget = 0;
	remaining_budget = entity_budget;

	if (out_mandatory_oversize)
		*out_mandatory_oversize = false;

	memset (present, 0, qcvm->max_edicts * sizeof(byte));
	if (relevant)
		memset (relevant, 0, qcvm->max_edicts * sizeof(byte));
	if (snapflags)
		memset (snapflags, 0, qcvm->max_edicts * sizeof(byte));

	numents = SV_BuildNetEdictsList (clent);
	count = 0;

	for (j=0 ; j<numents ; j++)
	{
		num = net_edicts_sorted[j];
		if (relevant)
			relevant[num] = 1;
	}

	for (j=0 ; j<numents ; j++)
	{
		num = net_edicts_sorted[j];
		ent = EDICT_NUM (num);

		if (!SV_SnapshotMandatoryEnt (ent) && !SV_SnapshotNeedsBaseline (baseline_present, num))
			continue;

		SV_FillSnapshotState (ent, &states[num]);

		if (states[num].state.alpha == ENTALPHA_ZERO && !((int)ent->v.effects & qcvm->effects_mask))
			continue;

		present[num] = 1;
		if (snapflags)
			snapflags[num] = SV_SnapshotFlagsForEnt (ent);
		{
			int ent_size = SV_SnapshotEntityBudgetBytes (snapflags ? snapflags[num] : 0);
			total_bytes += ent_size;
			mandatory_bytes += ent_size;
		}
		count++;
		mandatory_count++;

		if (debug_frame && net_snap_debug.value)
			Con_DPrintf ("snapdbg %s ent %d mandatory hires_o %d hires_a %d\n",
				PR_GetString (clent->v.netname), num,
				(snapflags[num] & SNAPFLAG_HIRES_ORIGIN) != 0,
				(snapflags[num] & SNAPFLAG_HIRES_ANGLES) != 0);
	}

	remaining_budget = entity_budget - mandatory_bytes;
	if (remaining_budget < 0)
	{
		if (out_mandatory_oversize)
			*out_mandatory_oversize = true;
		remaining_budget = 0;
	}
	if (optional_entity_budget - mandatory_bytes < remaining_budget)
	{
		remaining_budget = optional_entity_budget - mandatory_bytes;
		if (remaining_budget < 0)
			remaining_budget = 0;
	}

	for (j=0 ; j<numents ; j++)
	{
		num = net_edicts_sorted[j];
		ent = EDICT_NUM (num);

		if (present[num] || !SV_SnapshotTier1Ent (ent))
			continue;

		if (remaining_budget <= 0)
		{
			continuation = true;
			for (k = j; k < numents; k++)
			{
				int pending = net_edicts_sorted[k];
				edict_t *pending_ent = EDICT_NUM (pending);
				if (present[pending])
					continue;
				if (!SV_SnapshotTier1Ent (pending_ent))
					continue;
				dropped_tier1++;
			}
			break;
		}

		SV_FillSnapshotState (ent, &states[num]);

		if (states[num].state.alpha == ENTALPHA_ZERO && !((int)ent->v.effects & qcvm->effects_mask))
			continue;

		{
			byte snapflag = snapflags ? SV_SnapshotFlagsForEnt (ent) : 0;
			int ent_size = SV_SnapshotEntityBudgetBytes (snapflag);
			if (ent_size > remaining_budget)
			{
				continuation = true;
				for (k = j; k < numents; k++)
				{
					int pending = net_edicts_sorted[k];
					edict_t *pending_ent = EDICT_NUM (pending);
					if (present[pending])
						continue;
					if (!SV_SnapshotTier1Ent (pending_ent))
						continue;
					dropped_tier1++;
				}
				break;
			}
			present[num] = 1;
			if (snapflags)
				snapflags[num] = snapflag;
			total_bytes += ent_size;
			remaining_budget -= ent_size;
		}
		count++;
	}

	if (!continuation)
	{
		for (j=0 ; j<numents ; j++)
		{
			num = net_edicts_sorted[j];
			ent = EDICT_NUM (num);

			if (present[num])
				continue;

			if (remaining_budget <= 0)
			{
				continuation = true;
				for (k = j; k < numents; k++)
				{
					int pending = net_edicts_sorted[k];
					edict_t *pending_ent = EDICT_NUM (pending);
					if (present[pending])
						continue;
					if (SV_SnapshotTier1Ent (pending_ent))
						continue;
					dropped_tier2++;
				}
				break;
			}

			SV_FillSnapshotState (ent, &states[num]);

			if (states[num].state.alpha == ENTALPHA_ZERO && !((int)ent->v.effects & qcvm->effects_mask))
				continue;

			{
				byte snapflag = snapflags ? SV_SnapshotFlagsForEnt (ent) : 0;
				int ent_size = SV_SnapshotEntityBudgetBytes (snapflag);
				if (ent_size > remaining_budget)
				{
					continuation = true;
					for (k = j; k < numents; k++)
					{
						int pending = net_edicts_sorted[k];
						edict_t *pending_ent = EDICT_NUM (pending);
						if (present[pending])
							continue;
						if (SV_SnapshotTier1Ent (pending_ent))
							continue;
						dropped_tier2++;
					}
					break;
				}
				present[num] = 1;
				if (snapflags)
					snapflags[num] = snapflag;
				total_bytes += ent_size;
				remaining_budget -= ent_size;
			}
			count++;

			if (debug_frame && net_snap_debug.value)
				Con_DPrintf ("snapdbg %s ent %d included hires_o %d hires_a %d\n",
					PR_GetString (clent->v.netname), num,
					(snapflags[num] & SNAPFLAG_HIRES_ORIGIN) != 0,
					(snapflags[num] & SNAPFLAG_HIRES_ANGLES) != 0);
		}
	}

	if (out_mandatory)
		*out_mandatory = mandatory_count;
	if (out_mandatory_bytes)
		*out_mandatory_bytes = mandatory_bytes;
	if (out_total_bytes)
		*out_total_bytes = total_bytes;
	dropped_count = dropped_tier1 + dropped_tier2;
	if (out_dropped)
		*out_dropped = dropped_count;
	if (out_dropped_tier1)
		*out_dropped_tier1 = dropped_tier1;
	if (out_dropped_tier2)
		*out_dropped_tier2 = dropped_tier2;
	if (out_continuation)
		*out_continuation = continuation;

	return count;
}

static void SV_WriteSnapshotCoord (sizebuf_t *msg, float value, qboolean hires)
{
	if (hires && (sv.protocolflags & PRFL_SNAPSHOT_HIRES))
		MSG_WriteCoord32f (msg, value);
	else
		MSG_WriteCoord (msg, value, sv.protocolflags);
}

static void SV_WriteSnapshotAngle (sizebuf_t *msg, float value, qboolean hires)
{
	if (hires && (sv.protocolflags & PRFL_SNAPSHOT_HIRES))
		MSG_WriteFloat (msg, value);
	else
		MSG_WriteAngle (msg, value, sv.protocolflags);
}

static void SV_WriteSnapshotState (sizebuf_t *msg, const snapshot_state_t *state, byte snapflags)
{
	int i;
	qboolean hires_origin = (snapflags & SNAPFLAG_HIRES_ORIGIN) != 0;
	qboolean hires_angles = (snapflags & SNAPFLAG_HIRES_ANGLES) != 0;

	for (i=0 ; i<3 ; i++)
	{
		SV_WriteSnapshotCoord (msg, state->state.origin[i], hires_origin);
		SV_WriteSnapshotAngle (msg, state->state.angles[i], hires_angles);
	}
	MSG_WriteShort (msg, state->state.modelindex);
	MSG_WriteShort (msg, state->state.frame);
	MSG_WriteByte (msg, state->state.colormap);
	MSG_WriteByte (msg, state->state.skin);
	MSG_WriteByte (msg, state->state.effects);
	MSG_WriteByte (msg, state->state.alpha);
	MSG_WriteByte (msg, state->state.scale);
	MSG_WriteByte (msg, state->step);
}

static void SV_WriteSnapshot2Header (sizebuf_t *msg, unsigned int seq, unsigned int base_seq,
	unsigned int flags, unsigned short num_entities, unsigned short num_removed)
{
	MSG_BEGINSVC (msg, svc_snapshot2);
	MSG_WriteByte (msg, svc_snapshot2);
	MSG_WriteLong (msg, (int)seq);
	MSG_WriteLong (msg, (int)base_seq);
	MSG_WriteByte (msg, (byte)flags);
	MSG_WriteShort (msg, (short)num_entities);
	MSG_WriteShort (msg, (short)num_removed);
}

static qboolean SV_WriteSnapshot2FullChunked (client_t *client, sizebuf_t *msg,
	const byte *present, const byte *snapflags, unsigned int extra_flags,
	int *out_remaining, unsigned int *out_flags)
{
	int header_size = 1 + 4 + 4 + 1 + 2 + 2;
	int available = msg->maxsize - msg->cursize;
	int entnum;
	int chunk_count = 0;
	int chunk_size = header_size;
	int last_sent = 0;
	qboolean more = false;
	unsigned int flags = SNAPSHOT_FLAG_FULL | (extra_flags & ~SNAPSHOT_FLAG_INCOMPLETE);
	int remaining_entities = 0;

	if (available < header_size)
		return false;

	for (entnum = client->entstream.next_edict; entnum < qcvm->max_edicts; entnum++)
	{
		int ent_size;

		if (!present[entnum])
			continue;

		ent_size = SV_Snapshot2EntitySizeForFlags (snapflags ? snapflags[entnum] : 0);
		if (chunk_size + ent_size > available)
		{
			more = true;
			break;
		}

		chunk_size += ent_size;
		chunk_count++;
		last_sent = entnum;
	}

	if (chunk_count == 0 && more)
		return false;

	remaining_entities = client->entstream.total_entities - (client->entstream.sent_entities + chunk_count);
	if (remaining_entities < 0)
		remaining_entities = 0;

	if (remaining_entities > 0)
		flags |= SNAPSHOT_FLAG_CONTINUE | SNAPSHOT_FLAG_INCOMPLETE;
	else
		flags &= ~(SNAPSHOT_FLAG_CONTINUE | SNAPSHOT_FLAG_INCOMPLETE);

	if (!MSG_CanWrite (msg, chunk_size))
		return false;

	SV_WriteSnapshot2Header (msg, client->snapshot_pending_seq,
		0xFFFFFFFFu, flags, (unsigned short)chunk_count, (unsigned short)remaining_entities);

	if (chunk_count)
	{
		int remaining = chunk_count;

		for (entnum = client->entstream.next_edict; entnum < qcvm->max_edicts; entnum++)
		{
			snapshot_state_t state;
			byte snapflag = 0;

			if (!present[entnum])
				continue;

			if (snapflags)
				snapflag = snapflags[entnum];

			state = client->snapshot_pending[entnum];
			MSG_WriteShort (msg, entnum);
			if (sv.protocolflags & PRFL_SNAPSHOT_HIRES)
				MSG_WriteByte (msg, snapflag);
			SV_WriteSnapshotState (msg, &state, snapflag);

			remaining--;
			if (remaining == 0)
				break;
		}
	}

	client->entstream.sent_entities += chunk_count;

	if (remaining_entities > 0)
	{
		client->entstream.next_edict = last_sent + 1;
		msg->write_locked = true;
		if (realtime >= client->entstream.next_log_time)
		{
			Con_Printf ("snapshot %s seq %u: sent %d/%d entities, continuing\n",
				client->name,
				client->snapshot_pending_seq,
				client->entstream.sent_entities,
				client->entstream.total_entities);
			client->entstream.next_log_time = realtime + 1.0;
		}
	}
	else
	{
		client->entstream.active = false;
		client->entstream.next_edict = 1;
		client->entstream.base_snapshot = 0;
	}

	if (out_remaining)
		*out_remaining = remaining_entities;
	if (out_flags)
		*out_flags = flags;
	NET_DebugLogEvent (true,
		"NETDBG time %.3f snap_chunk_flags %s seq %u rem %d flags 0x%x\n",
		realtime, client->name, client->snapshot_pending_seq, remaining_entities, flags);
	if (net_snap_debug.value)
		NET_DebugLogEvent (true,
			"NETDBG time %.3f snap_chunk_rem %s seq %u rem %d flags 0x%x\n",
			realtime, client->name, client->snapshot_pending_seq, remaining_entities, flags);
	if (net_snap_debug.value && remaining_entities == 0)
		NET_DebugLogEvent (true,
			"NETDBG time %.3f snap_chunk_complete %s seq %u\n",
			realtime, client->name, client->snapshot_pending_seq);

	return true;
}

static unsigned int SV_SnapshotFieldMask (const snapshot_state_t *next, const snapshot_state_t *base, byte snapflags)
{
	unsigned int mask = 0;

	if (next->state.origin[0] != base->state.origin[0])
		mask |= SNAP_ORIGIN1;
	if (next->state.origin[1] != base->state.origin[1])
		mask |= SNAP_ORIGIN2;
	if (next->state.origin[2] != base->state.origin[2])
		mask |= SNAP_ORIGIN3;
	if (next->state.angles[0] != base->state.angles[0])
		mask |= SNAP_ANGLE1;
	if (next->state.angles[1] != base->state.angles[1])
		mask |= SNAP_ANGLE2;
	if (next->state.angles[2] != base->state.angles[2])
		mask |= SNAP_ANGLE3;
	if (next->state.modelindex != base->state.modelindex)
		mask |= SNAP_MODEL;
	if (next->state.frame != base->state.frame)
		mask |= SNAP_FRAME;
	if (next->state.colormap != base->state.colormap)
		mask |= SNAP_COLORMAP;
	if (next->state.skin != base->state.skin)
		mask |= SNAP_SKIN;
	if (next->state.effects != base->state.effects)
		mask |= SNAP_EFFECTS;
	if (next->state.alpha != base->state.alpha)
		mask |= SNAP_ALPHA;
	if (next->state.scale != base->state.scale)
		mask |= SNAP_SCALE;
	if (next->step != base->step)
		mask |= SNAP_STEP;

	if (snapflags & SNAPFLAG_FORCE_ORIGIN)
		mask |= SNAP_ORIGIN1 | SNAP_ORIGIN2 | SNAP_ORIGIN3;
	if (snapflags & SNAPFLAG_FORCE_ANGLES)
		mask |= SNAP_ANGLE1 | SNAP_ANGLE2 | SNAP_ANGLE3;

	if ((snapflags & SNAPFLAG_HIRES_ORIGIN) && (mask & (SNAP_ORIGIN1 | SNAP_ORIGIN2 | SNAP_ORIGIN3))
		&& (sv.protocolflags & PRFL_SNAPSHOT_HIRES))
		mask |= SNAP_HIRES_ORIGIN;
	if ((snapflags & SNAPFLAG_HIRES_ANGLES) && (mask & (SNAP_ANGLE1 | SNAP_ANGLE2 | SNAP_ANGLE3))
		&& (sv.protocolflags & PRFL_SNAPSHOT_HIRES))
		mask |= SNAP_HIRES_ANGLES;

	return mask;
}

static int SV_SnapshotDeltaFieldSize (unsigned int mask)
{
	int size = 0;
	int coord_size;
	int angle_size;
	qboolean hires_origin = (mask & SNAP_HIRES_ORIGIN) != 0;
	qboolean hires_angles = (mask & SNAP_HIRES_ANGLES) != 0;

	coord_size = SV_SnapshotCoordBytes (hires_origin);
	angle_size = SV_SnapshotAngleBytes (hires_angles);

	if (mask & SNAP_ORIGIN1)
		size += coord_size;
	if (mask & SNAP_ORIGIN2)
		size += coord_size;
	if (mask & SNAP_ORIGIN3)
		size += coord_size;
	if (mask & SNAP_ANGLE1)
		size += angle_size;
	if (mask & SNAP_ANGLE2)
		size += angle_size;
	if (mask & SNAP_ANGLE3)
		size += angle_size;
	if (mask & SNAP_MODEL)
		size += 2;
	if (mask & SNAP_FRAME)
		size += 2;
	if (mask & SNAP_COLORMAP)
		size += 1;
	if (mask & SNAP_SKIN)
		size += 1;
	if (mask & SNAP_EFFECTS)
		size += 1;
	if (mask & SNAP_ALPHA)
		size += 1;
	if (mask & SNAP_SCALE)
		size += 1;
	if (mask & SNAP_STEP)
		size += 1;

	return size;
}

static void SV_WriteSnapshotFull (sizebuf_t *msg, unsigned int seq, const snapshot_state_t *states,
	const byte *present, const byte *snapflags, int max_edicts, int *out_count)
{
	int count = 0;
	int entnum;

	for (entnum = 1; entnum < max_edicts; entnum++)
	{
		if (present[entnum])
			count++;
	}

	MSG_BEGINSVC (msg, svc_snapshot_full);
	MSG_WriteByte (msg, svc_snapshot_full);
	MSG_WriteLong (msg, (int)seq);
	MSG_WriteShort (msg, count);
	for (entnum = 1; entnum < max_edicts; entnum++)
	{
		if (!present[entnum])
			continue;
		MSG_WriteShort (msg, entnum);
		if (sv.protocolflags & PRFL_SNAPSHOT_HIRES)
			MSG_WriteByte (msg, snapflags ? snapflags[entnum] : 0);
		SV_WriteSnapshotState (msg, &states[entnum], snapflags ? snapflags[entnum] : 0);
	}

	if (out_count)
		*out_count = count;
}

static void SV_WriteSnapshot2Full (sizebuf_t *msg, unsigned int seq, const snapshot_state_t *states,
	const byte *present, const byte *snapflags, int max_edicts, int *out_count, unsigned int extra_flags)
{
	unsigned short count = 0;
	int entnum;

	for (entnum = 1; entnum < max_edicts; entnum++)
	{
		if (present[entnum])
			count++;
	}

	SV_WriteSnapshot2Header (msg, seq, 0xFFFFFFFFu, SNAPSHOT_FLAG_FULL | extra_flags, count, 0);

	for (entnum = 1; entnum < max_edicts; entnum++)
	{
		if (!present[entnum])
			continue;
		MSG_WriteShort (msg, entnum);
		if (sv.protocolflags & PRFL_SNAPSHOT_HIRES)
			MSG_WriteByte (msg, snapflags ? snapflags[entnum] : 0);
		SV_WriteSnapshotState (msg, &states[entnum], snapflags ? snapflags[entnum] : 0);
	}

	if (out_count)
		*out_count = count;
}

static void SV_WriteSnapshotDelta (sizebuf_t *msg, unsigned int seq, unsigned int baseline_seq,
	const snapshot_state_t *states, const byte *present, const byte *relevant,
	const snapshot_state_t *baseline, const byte *baseline_present, const byte *snapflags,
	int max_edicts, int *out_remove, int *out_add, int *out_update)
{
	int entnum;
	int remove_count = 0;
	int add_count = 0;
	int update_count = 0;

	MSG_BEGINSVC (msg, svc_snapshot_delta);
	MSG_WriteByte (msg, svc_snapshot_delta);
	MSG_WriteLong (msg, (int)seq);
	MSG_WriteLong (msg, (int)baseline_seq);

	for (entnum = 1; entnum < max_edicts; entnum++)
		if (baseline_present[entnum] && relevant && !relevant[entnum])
			remove_count++;
	MSG_WriteShort (msg, remove_count);
	for (entnum = 1; entnum < max_edicts; entnum++)
	{
		if (baseline_present[entnum] && relevant && !relevant[entnum])
			MSG_WriteShort (msg, entnum);
	}

	for (entnum = 1; entnum < max_edicts; entnum++)
		if (present[entnum] && (!baseline_present[entnum]
			|| (snapflags && (snapflags[entnum] & SNAPFLAG_NO_DELTA))))
			add_count++;
	MSG_WriteShort (msg, add_count);
	for (entnum = 1; entnum < max_edicts; entnum++)
	{
		if (present[entnum] && (!baseline_present[entnum]
			|| (snapflags && (snapflags[entnum] & SNAPFLAG_NO_DELTA))))
		{
			MSG_WriteShort (msg, entnum);
			if (sv.protocolflags & PRFL_SNAPSHOT_HIRES)
				MSG_WriteByte (msg, snapflags ? snapflags[entnum] : 0);
			SV_WriteSnapshotState (msg, &states[entnum], snapflags ? snapflags[entnum] : 0);
		}
	}

	for (entnum = 1; entnum < max_edicts; entnum++)
	{
		unsigned int mask;

		if (!baseline_present[entnum] || !present[entnum]
			|| (snapflags && (snapflags[entnum] & SNAPFLAG_NO_DELTA)))
			continue;
		mask = SV_SnapshotFieldMask (&states[entnum], &baseline[entnum], snapflags ? snapflags[entnum] : 0);
		if (mask)
			update_count++;
	}

	MSG_WriteShort (msg, update_count);
	for (entnum = 1; entnum < max_edicts; entnum++)
	{
		unsigned int mask;

		if (!baseline_present[entnum] || !present[entnum]
			|| (snapflags && (snapflags[entnum] & SNAPFLAG_NO_DELTA)))
			continue;
		mask = SV_SnapshotFieldMask (&states[entnum], &baseline[entnum], snapflags ? snapflags[entnum] : 0);
		if (!mask)
			continue;
		MSG_WriteShort (msg, entnum);
		MSG_WriteLong (msg, (int)mask);
		if (mask & SNAP_ORIGIN1)
			SV_WriteSnapshotCoord (msg, states[entnum].state.origin[0], (mask & SNAP_HIRES_ORIGIN) != 0);
		if (mask & SNAP_ORIGIN2)
			SV_WriteSnapshotCoord (msg, states[entnum].state.origin[1], (mask & SNAP_HIRES_ORIGIN) != 0);
		if (mask & SNAP_ORIGIN3)
			SV_WriteSnapshotCoord (msg, states[entnum].state.origin[2], (mask & SNAP_HIRES_ORIGIN) != 0);
		if (mask & SNAP_ANGLE1)
			SV_WriteSnapshotAngle (msg, states[entnum].state.angles[0], (mask & SNAP_HIRES_ANGLES) != 0);
		if (mask & SNAP_ANGLE2)
			SV_WriteSnapshotAngle (msg, states[entnum].state.angles[1], (mask & SNAP_HIRES_ANGLES) != 0);
		if (mask & SNAP_ANGLE3)
			SV_WriteSnapshotAngle (msg, states[entnum].state.angles[2], (mask & SNAP_HIRES_ANGLES) != 0);
		if (mask & SNAP_MODEL)
			MSG_WriteShort (msg, states[entnum].state.modelindex);
		if (mask & SNAP_FRAME)
			MSG_WriteShort (msg, states[entnum].state.frame);
		if (mask & SNAP_COLORMAP)
			MSG_WriteByte (msg, states[entnum].state.colormap);
		if (mask & SNAP_SKIN)
			MSG_WriteByte (msg, states[entnum].state.skin);
		if (mask & SNAP_EFFECTS)
			MSG_WriteByte (msg, states[entnum].state.effects);
		if (mask & SNAP_ALPHA)
			MSG_WriteByte (msg, states[entnum].state.alpha);
		if (mask & SNAP_SCALE)
			MSG_WriteByte (msg, states[entnum].state.scale);
		if (mask & SNAP_STEP)
			MSG_WriteByte (msg, states[entnum].step);
	}

	if (out_remove)
		*out_remove = remove_count;
	if (out_add)
		*out_add = add_count;
	if (out_update)
		*out_update = update_count;
}

static void SV_WriteSnapshot2Delta (sizebuf_t *msg, unsigned int seq, unsigned int baseline_seq,
	const snapshot_state_t *states, const byte *present, const byte *relevant,
	const snapshot_state_t *baseline, const byte *baseline_present, const byte *snapflags,
	int max_edicts, int *out_remove, int *out_add, int *out_update, unsigned int extra_flags,
	qboolean *out_incomplete)
{
	int entnum;
	unsigned short remove_count = 0;
	unsigned short add_count = 0;
	unsigned short update_count = 0;
	unsigned short remove_written = 0;
	unsigned short add_written = 0;
	unsigned short update_written = 0;
	unsigned int flags = SNAPSHOT_FLAG_DELTA | extra_flags;
	int header_size = 1 + 4 + 4 + 1 + 2 + 2;
	int available = msg->maxsize - msg->cursize;
	int budget = available - header_size;
	qboolean incomplete = false;

	if (out_incomplete)
		*out_incomplete = false;

	if (available < header_size)
	{
		msg->overflowed = true;
		msg->write_locked = true;
		if (out_incomplete)
			*out_incomplete = true;
		return;
	}

	if (budget < 0)
		budget = 0;

	for (entnum = 1; entnum < max_edicts; entnum++)
		if (baseline_present[entnum] && relevant && !relevant[entnum])
		{
			if (budget < 2)
			{
				incomplete = true;
				break;
			}
			remove_count++;
			budget -= 2;
		}

	for (entnum = 1; entnum < max_edicts; entnum++)
		if (present[entnum] && (!baseline_present[entnum]
			|| (snapflags && (snapflags[entnum] & SNAPFLAG_NO_DELTA))))
		{
			int ent_size = SV_Snapshot2EntitySizeForFlags (snapflags ? snapflags[entnum] : 0);
			if (budget < ent_size)
			{
				incomplete = true;
				break;
			}
			add_count++;
			budget -= ent_size;
		}

	for (entnum = 1; entnum < max_edicts; entnum++)
	{
		unsigned int mask;

		if (!baseline_present[entnum] || !present[entnum]
			|| (snapflags && (snapflags[entnum] & SNAPFLAG_NO_DELTA)))
			continue;
		mask = SV_SnapshotFieldMask (&states[entnum], &baseline[entnum], snapflags ? snapflags[entnum] : 0);
		if (mask)
		{
			int delta_size = 2 + 4 + SV_SnapshotDeltaFieldSize (mask);
			if (budget < delta_size)
			{
				incomplete = true;
				break;
			}
			update_count++;
			budget -= delta_size;
		}
	}

	if (remove_count)
		flags |= SNAPSHOT_FLAG_HAS_REMOVE_LIST;
	if (incomplete)
		flags |= SNAPSHOT_FLAG_INCOMPLETE;
	SV_WriteSnapshot2Header (msg, seq, baseline_seq, flags, add_count + update_count, remove_count);

	for (entnum = 1; entnum < max_edicts; entnum++)
	{
		if (baseline_present[entnum] && relevant && !relevant[entnum])
		{
			if (remove_count == 0)
				break;
			MSG_WriteShort (msg, entnum);
			remove_count--;
			remove_written++;
		}
	}

	MSG_WriteShort (msg, add_count);
	for (entnum = 1; entnum < max_edicts; entnum++)
	{
		if (present[entnum] && (!baseline_present[entnum]
			|| (snapflags && (snapflags[entnum] & SNAPFLAG_NO_DELTA))))
		{
			if (add_count == 0)
				break;
			MSG_WriteShort (msg, entnum);
			if (sv.protocolflags & PRFL_SNAPSHOT_HIRES)
				MSG_WriteByte (msg, snapflags ? snapflags[entnum] : 0);
			SV_WriteSnapshotState (msg, &states[entnum], snapflags ? snapflags[entnum] : 0);
			add_count--;
			add_written++;
		}
	}

	MSG_WriteShort (msg, update_count);
	for (entnum = 1; entnum < max_edicts; entnum++)
	{
		unsigned int mask;

		if (!baseline_present[entnum] || !present[entnum]
			|| (snapflags && (snapflags[entnum] & SNAPFLAG_NO_DELTA)))
			continue;
		mask = SV_SnapshotFieldMask (&states[entnum], &baseline[entnum], snapflags ? snapflags[entnum] : 0);
		if (!mask)
			continue;
		if (update_count == 0)
			break;
		MSG_WriteShort (msg, entnum);
		MSG_WriteLong (msg, (int)mask);
		if (mask & SNAP_ORIGIN1)
			SV_WriteSnapshotCoord (msg, states[entnum].state.origin[0], (mask & SNAP_HIRES_ORIGIN) != 0);
		if (mask & SNAP_ORIGIN2)
			SV_WriteSnapshotCoord (msg, states[entnum].state.origin[1], (mask & SNAP_HIRES_ORIGIN) != 0);
		if (mask & SNAP_ORIGIN3)
			SV_WriteSnapshotCoord (msg, states[entnum].state.origin[2], (mask & SNAP_HIRES_ORIGIN) != 0);
		if (mask & SNAP_ANGLE1)
			SV_WriteSnapshotAngle (msg, states[entnum].state.angles[0], (mask & SNAP_HIRES_ANGLES) != 0);
		if (mask & SNAP_ANGLE2)
			SV_WriteSnapshotAngle (msg, states[entnum].state.angles[1], (mask & SNAP_HIRES_ANGLES) != 0);
		if (mask & SNAP_ANGLE3)
			SV_WriteSnapshotAngle (msg, states[entnum].state.angles[2], (mask & SNAP_HIRES_ANGLES) != 0);
		if (mask & SNAP_MODEL)
			MSG_WriteShort (msg, states[entnum].state.modelindex);
		if (mask & SNAP_FRAME)
			MSG_WriteShort (msg, states[entnum].state.frame);
		if (mask & SNAP_COLORMAP)
			MSG_WriteByte (msg, states[entnum].state.colormap);
		if (mask & SNAP_SKIN)
			MSG_WriteByte (msg, states[entnum].state.skin);
		if (mask & SNAP_EFFECTS)
			MSG_WriteByte (msg, states[entnum].state.effects);
		if (mask & SNAP_ALPHA)
			MSG_WriteByte (msg, states[entnum].state.alpha);
		if (mask & SNAP_SCALE)
			MSG_WriteByte (msg, states[entnum].state.scale);
		if (mask & SNAP_STEP)
			MSG_WriteByte (msg, states[entnum].step);
		update_count--;
		update_written++;
	}

	if (out_remove)
		*out_remove = remove_written;
	if (out_add)
		*out_add = add_written;
	if (out_update)
		*out_update = update_written;
	if (out_incomplete)
		*out_incomplete = incomplete;
}

static void SV_SendSnapshot (client_t *client, sizebuf_t *msg)
{
	int start_size = msg->cursize;
	int add_count = 0;
	int remove_count = 0;
	int update_count = 0;
	int full_count = 0;
	qboolean continuation = false;
	int chunk_remaining = -1;
	unsigned int chunk_flags = 0;
	qboolean chunked_snapshot = false;
	double timeout_s = sv_snapshottimeout.value / 1000.0;
	double full_interval_s = sv_snap_full_interval.value;
	int force_full_frames = (int)sv_snap_force_full_after_frames.value;
	qboolean use_delta;
	qboolean forced_full = false;
	qboolean reason_no_base = false;
	qboolean reason_interval = false;
	qboolean reason_force_full = false;
	unsigned int extra_flags = 0;
	qboolean delta_incomplete = false;
	qboolean invalid_base = false;

	if (client->entstream.active && client->entstream.base_snapshot != (int)client->snapshot_pending_seq)
		client->entstream.active = false;

	if (net_test_drop.value > 0.0f && ((float)rand() / (float)RAND_MAX) < net_test_drop.value)
	{
		if (sv_snap_debug.value >= 1.0f)
			Con_Printf ("snapshot %s dropped by net_test_drop\n", client->name);
		return;
	}

	{
		const snapshot_state_t *baseline_states = NULL;
		const byte *baseline_present = NULL;
		unsigned int baseline_seq = 0;
		qboolean have_baseline = SV_SelectSnapshotBaseline (client, &baseline_seq, &baseline_states, &baseline_present);

		if (!have_baseline && client->snapshot_baseline_present[0])
			baseline_present = client->snapshot_baseline_present[0];

		if (client->snapshot_force_full)
		{
			forced_full = true;
			reason_force_full = true;
			client->snapshot_force_full = false;
		}
		if (force_full_frames > 0 && client->snapshot_unacked_frames >= force_full_frames)
		{
			forced_full = true;
			reason_force_full = true;
		}
		if (timeout_s > 0.0 && client->snapshot_last_acked_time > 0.0
			&& realtime - client->snapshot_last_acked_time > timeout_s)
		{
			forced_full = true;
			reason_force_full = true;
		}

		if (client->entstream.active)
		{
			if (client->snapshot_pending_dropped > 0)
				extra_flags |= SNAPSHOT_FLAG_INCOMPLETE;
			use_delta = false;
			client->snapshot_pending_is_delta = false;
			if (sv_snapshot2.value)
			{
				int total_count = 0;
				int total_size = SV_Snapshot2FullSize (client->snapshot_pending_present,
					client->snapshot_pending_flags, qcvm->max_edicts, &total_count);

				full_count = total_count;
				if (client->entstream.active)
				{
					chunked_snapshot = true;
					if (!SV_WriteSnapshot2FullChunked (client, msg,
						client->snapshot_pending_present, client->snapshot_pending_flags, extra_flags,
						&chunk_remaining, &chunk_flags))
					{
						msg->overflowed = true;
						msg->write_locked = true;
						return;
					}
				}
				else if (total_size > (msg->maxsize - msg->cursize))
				{
					SV_InitEntityStream (client, total_count, client->snapshot_pending_seq);
					chunked_snapshot = true;
					if (!SV_WriteSnapshot2FullChunked (client, msg,
						client->snapshot_pending_present, client->snapshot_pending_flags, extra_flags,
						&chunk_remaining, &chunk_flags))
					{
						msg->overflowed = true;
						msg->write_locked = true;
						return;
					}
				}
				else
				{
					SV_WriteSnapshot2Full (msg, client->snapshot_pending_seq, client->snapshot_pending,
						client->snapshot_pending_present, client->snapshot_pending_flags, qcvm->max_edicts,
						&full_count, extra_flags);
				}
			}
			else
			{
				SV_WriteSnapshotFull (msg, client->snapshot_pending_seq, client->snapshot_pending,
					client->snapshot_pending_present, client->snapshot_pending_flags, qcvm->max_edicts, &full_count);
			}
			client->snapshot_last_full_seq = client->snapshot_pending_seq;
			client->snapshot_last_full_time = realtime;
		}
		else
		{
			int mandatory_count = 0;
			int mandatory_bytes = 0;
			int dropped_count = 0;
			int dropped_tier1 = 0;
			int dropped_tier2 = 0;
			int mtu_payload_bytes = 0;
			int current_bytes = 0;
			int remaining_bytes = 0;
			int header_bytes = 0;
			int snapshot_budget_bytes = SV_SnapshotBudgetBytes (client, msg,
				&mtu_payload_bytes, &current_bytes, &remaining_bytes, &header_bytes);
			int mandatory_total_bytes = 0;
			qboolean budget_continuation = false;
			qboolean mandatory_oversize = false;
			qboolean debug_frame = false;

			if (net_snap_debug.value && realtime >= client->snapshot_debug_next_time)
			{
				debug_frame = true;
				client->snapshot_debug_next_time = realtime + 1.0;
			}

			SV_BuildClientSnapshot (client->edict, client->snapshot_pending, client->snapshot_pending_present,
				client->snapshot_pending_relevant, client->snapshot_pending_flags, remaining_bytes, snapshot_budget_bytes,
				header_bytes, baseline_present, &mandatory_count, &mandatory_bytes, NULL,
				&dropped_count, &dropped_tier1, &dropped_tier2, debug_frame, &budget_continuation,
				&mandatory_oversize);
			mandatory_total_bytes = header_bytes + mandatory_bytes;
			if (mtu_payload_bytes > 0 && current_bytes + mandatory_total_bytes > mtu_payload_bytes)
			{
				client->snapshot_pending_incomplete = true;
				client->snapshot_pending_mandatory = mandatory_count;
				client->snapshot_pending_dropped = 0;
				client->mtu_dropped_tier1 = 0;
				client->mtu_dropped_tier2 = 0;
				return;
			}
			if (mandatory_oversize)
			{
				client->snapshot_pending_incomplete = true;
				client->snapshot_pending_mandatory = mandatory_count;
				client->snapshot_pending_dropped = 0;
				client->mtu_dropped_tier1 = 0;
				client->mtu_dropped_tier2 = 0;
				return;
			}
			client->snapshot_pending_mandatory = mandatory_count;
			client->snapshot_pending_dropped = dropped_count;
			client->mtu_dropped_tier1 = dropped_tier1;
			client->mtu_dropped_tier2 = dropped_tier2;
			if (client->snapshot_pending_dropped > 0)
				extra_flags |= SNAPSHOT_FLAG_INCOMPLETE;
			client->snapshot_pending_seq = client->snapshot_next_seq++;
			if (!client->snapshot_next_seq)
				client->snapshot_next_seq = 1;
			client->snapshot_pending_time = realtime;
			use_delta = (sv_snapshotdelta.value && have_baseline);
			if (!have_baseline)
				reason_no_base = true;
			if (forced_full)
			{
				use_delta = false;
				reason_force_full = true;
			}
			if (full_interval_s > 0.0 && (realtime - client->snapshot_last_full_time) > full_interval_s)
			{
				use_delta = false;
				forced_full = true;
				reason_interval = true;
			}
			if (use_delta && baseline_seq == 0xFFFFFFFFu)
			{
				invalid_base = true;
				use_delta = false;
				forced_full = true;
				reason_no_base = true;
			}
			client->snapshot_pending_baseline_seq = use_delta ? baseline_seq : 0xFFFFFFFFu;
			client->snapshot_pending_is_delta = use_delta;

			if (use_delta)
			{
				if (sv_snapshot2.value)
					SV_WriteSnapshot2Delta (msg, client->snapshot_pending_seq, baseline_seq,
						client->snapshot_pending, client->snapshot_pending_present, client->snapshot_pending_relevant,
						baseline_states, baseline_present,
						client->snapshot_pending_flags, qcvm->max_edicts,
						&remove_count, &add_count, &update_count, extra_flags, &delta_incomplete);
				else
					SV_WriteSnapshotDelta (msg, client->snapshot_pending_seq, baseline_seq,
						client->snapshot_pending, client->snapshot_pending_present, client->snapshot_pending_relevant,
						baseline_states, baseline_present,
						client->snapshot_pending_flags, qcvm->max_edicts,
						&remove_count, &add_count, &update_count);
			}
			else
			{
				if (sv_snapshot2.value)
				{
					int total_count = 0;
					int total_size = SV_Snapshot2FullSize (client->snapshot_pending_present,
						client->snapshot_pending_flags, qcvm->max_edicts, &total_count);

					full_count = total_count;
					if (total_size > (msg->maxsize - msg->cursize))
					{
						SV_InitEntityStream (client, total_count, client->snapshot_pending_seq);
						chunked_snapshot = true;
						if (!SV_WriteSnapshot2FullChunked (client, msg,
							client->snapshot_pending_present, client->snapshot_pending_flags, extra_flags,
							&chunk_remaining, &chunk_flags))
						{
							msg->overflowed = true;
							msg->write_locked = true;
							return;
						}
					}
					else
					{
						SV_WriteSnapshot2Full (msg, client->snapshot_pending_seq, client->snapshot_pending,
							client->snapshot_pending_present, client->snapshot_pending_flags, qcvm->max_edicts,
							&full_count, extra_flags);
					}
				}
				else
				{
					SV_WriteSnapshotFull (msg, client->snapshot_pending_seq, client->snapshot_pending,
						client->snapshot_pending_present, client->snapshot_pending_flags, qcvm->max_edicts, &full_count);
				}
				client->snapshot_last_full_seq = client->snapshot_pending_seq;
				client->snapshot_last_full_time = realtime;
			}
		}

		client->snapshot_unacked_frames++;
		client->snapshot_pending_time = realtime;
		client->snapshot_last_sent_seq = client->snapshot_pending_seq;
		{
			qboolean packet_incomplete = delta_incomplete;
			if (chunked_snapshot)
				packet_incomplete = packet_incomplete || ((chunk_flags & SNAPSHOT_FLAG_INCOMPLETE) != 0);
			else
				packet_incomplete = packet_incomplete || ((extra_flags & SNAPSHOT_FLAG_INCOMPLETE) != 0);
			client->snapshot_pending_incomplete = packet_incomplete;
		}
	}

	client->mtu_last_snapshot_size = msg->cursize - start_size;
	if (chunked_snapshot)
		continuation = (chunk_remaining > 0);
	else
		continuation = msg->write_locked && client->entstream.active;
	if (!chunked_snapshot && (extra_flags & SNAPSHOT_FLAG_CONTINUE))
		continuation = true;
	if (client->snapshot_pending_seq && !client->snapshot_pending_incomplete && !continuation)
		SV_StoreSnapshotBaseline (client, client->snapshot_pending_seq,
			client->snapshot_pending, client->snapshot_pending_present);
	if (invalid_base)
	{
		client->snapshot_has_valid_base = false;
		client->snapshot_force_full = true;
		client->entstream.active = false;
		client->entstream.next_edict = 1;
		client->entstream.base_snapshot = 0;
		NET_DebugLogEvent (true,
			"NETDBG time %.3f snap_invalid_base %s seq %u base %u full %d delta %d force_full %d\n",
			realtime, client->name, client->snapshot_pending_seq,
			client->snapshot_pending_baseline_seq,
			use_delta ? 0 : 1, use_delta ? 1 : 0, forced_full ? 1 : 0);
	}

	if (forced_full)
		client->snapshot_stats_forced_full++;

	if (use_delta)
		client->snapshot_stats_delta++;
	else
		client->snapshot_stats_full++;
	client->snapshot_stats_mandatory += client->snapshot_pending_mandatory;
	client->snapshot_stats_dropped += client->snapshot_pending_dropped;

	if (net_snap_debug.value && realtime >= client->snapshot_stats_next_time)
	{
		Con_Printf ("snapstats %s full %d delta %d forced_full %d mandatory %d dropped %d\n",
			client->name, client->snapshot_stats_full, client->snapshot_stats_delta,
			client->snapshot_stats_forced_full, client->snapshot_stats_mandatory,
			client->snapshot_stats_dropped);
		client->snapshot_stats_full = 0;
		client->snapshot_stats_delta = 0;
		client->snapshot_stats_forced_full = 0;
		client->snapshot_stats_mandatory = 0;
		client->snapshot_stats_dropped = 0;
		client->snapshot_stats_next_time = realtime + 1.0;
	}

	if (sv_snapshotdebug.value)
	{
		int size_delta = msg->cursize - start_size;
		if (client->snapshot_pending_is_delta)
			Con_Printf ("snapshot %s seq %u base %u size %d add %d upd %d rem %d\n",
				client->name, client->snapshot_pending_seq, client->snapshot_pending_baseline_seq,
				size_delta, add_count, update_count, remove_count);
		else
			Con_Printf ("snapshot %s seq %u full size %d ents %d\n",
				client->name, client->snapshot_pending_seq, size_delta, full_count);
	}

	if (sv_snap_debug.value >= 1.0f)
	{
		unsigned int base_seq = use_delta ? client->snapshot_pending_baseline_seq : 0xFFFFFFFFu;
		unsigned int flags = 0;
		const char *reason = NULL;

		if (!use_delta)
			flags |= SNAPSHOT_FLAG_FULL;
		else
			flags |= SNAPSHOT_FLAG_DELTA;
		if (remove_count)
			flags |= SNAPSHOT_FLAG_HAS_REMOVE_LIST;
		if (chunked_snapshot)
		{
			if (chunk_flags & SNAPSHOT_FLAG_INCOMPLETE)
				flags |= SNAPSHOT_FLAG_INCOMPLETE;
		}
		else if (extra_flags & SNAPSHOT_FLAG_INCOMPLETE)
			flags |= SNAPSHOT_FLAG_INCOMPLETE;

		if (!use_delta && sv_snap_debug.value >= 2.0f)
		{
			if (reason_force_full)
				reason = "forced_full";
			else if (reason_interval)
				reason = "full_interval";
			else if (reason_no_base)
				reason = "no_base";
		}

		Con_Printf ("snap %s seq %u base %u flags 0x%x ents %d rem %d%s%s\n",
			client->name, client->snapshot_pending_seq, base_seq, flags,
			use_delta ? (add_count + update_count) : full_count, remove_count,
			reason ? " reason " : "", reason ? reason : "");
	}

	{
		unsigned int debug_flags = 0;
		unsigned int base_seq = use_delta ? client->snapshot_pending_baseline_seq : 0xFFFFFFFFu;

		debug_flags |= use_delta ? SNAPSHOT_FLAG_DELTA : SNAPSHOT_FLAG_FULL;
		if (remove_count)
			debug_flags |= SNAPSHOT_FLAG_HAS_REMOVE_LIST;
		if (client->snapshot_pending_incomplete)
			debug_flags |= SNAPSHOT_FLAG_INCOMPLETE;
		if (continuation)
			debug_flags |= SNAPSHOT_FLAG_CONTINUE;

		client->snapshot_debug_flags = debug_flags;
		client->snapshot_debug_base = base_seq;
		client->snapshot_debug_add = add_count;
		client->snapshot_debug_update = update_count;
		client->snapshot_debug_remove = remove_count;
		client->snapshot_debug_full_count = full_count;

		if (client->snapshot_pending_incomplete)
		{
			const char *reason = NULL;
			if (client->snapshot_pending_dropped > 0)
				reason = "budget_drop";
			else if (continuation)
				reason = "continuation";
			else if (delta_incomplete)
				reason = "delta_budget";
			else
				reason = "unknown";
			NET_DebugLogEvent (true,
				"NETDBG time %.3f snap_incomplete %s seq %u base %u reason %s mand %d drop %d full %d delta %d force_full %d\n",
				realtime, client->name, client->snapshot_pending_seq, base_seq, reason,
				client->snapshot_pending_mandatory, client->snapshot_pending_dropped,
				use_delta ? 0 : 1, use_delta ? 1 : 0, forced_full ? 1 : 0);
		}
	}

	if (client->snapshot_pending_seq)
	{
		unsigned int base_seq = use_delta ? client->snapshot_pending_baseline_seq : 0xFFFFFFFFu;
		int size_delta = msg->cursize - start_size;
		if (size_delta > 0)
		{
			int cursor = client->entstream.active ? client->entstream.next_edict : 0;
			int remaining = chunked_snapshot ? chunk_remaining : -1;
			qboolean same_seq = (client->snapshot_pending_seq == client->snapshot_no_progress_seq);
			qboolean same_base = (base_seq == client->snapshot_no_progress_base);
			qboolean same_cursor = (cursor == client->snapshot_no_progress_next_edict);
			qboolean continuation_stall = continuation && (remaining == 0 || same_cursor);

			if (same_seq && same_base && continuation_stall)
				client->snapshot_no_progress_count++;
			else
				client->snapshot_no_progress_count = 0;

			client->snapshot_no_progress_seq = client->snapshot_pending_seq;
			client->snapshot_no_progress_base = base_seq;
			client->snapshot_no_progress_next_edict = cursor;
			client->snapshot_no_progress_bytes = size_delta;
			client->snapshot_no_progress_remaining = remaining;

			if (client->snapshot_no_progress_count >= 2)
			{
				NET_DebugLogEvent (true,
					"NETDBG time %.3f snap_no_progress %s seq %u base %u size %d cursor %d rem %d full %d delta %d force_full %d\n",
					realtime, client->name, client->snapshot_pending_seq, base_seq, size_delta, cursor, remaining,
					use_delta ? 0 : 1, use_delta ? 1 : 0, forced_full ? 1 : 0);
				client->snapshot_force_full = true;
				client->snapshot_pending_is_delta = false;
				client->snapshot_pending_seq = 0;
				client->snapshot_pending_incomplete = false;
				client->snapshot_unacked_frames = 0;
				client->entstream.active = false;
				client->entstream.next_edict = 1;
				client->entstream.base_snapshot = 0;
				client->snapshot_no_progress_count = 0;
			}
		}
	}
}

void SV_SnapshotAck (client_t *client, unsigned int seq)
{
	int baseline_index;

	if (!client->snapshot_pending_present)
		return;

	if (seq == 0)
	{
		SV_ResetClientSnapshot (client);
		client->entstream.active = false;
		if (sv_snapshotdebug.value || sv_snap_debug.value)
			Con_Printf ("snapshot %s baseline reset request\n", client->name);
		return;
	}
	if (seq == 0xFFFFFFFFu)
	{
		client->snapshot_has_valid_base = false;
		client->snapshot_force_full = true;
		NET_DebugLogEvent (true,
			"NETDBG time %.3f snap_invalid_ack %s seq %u\n",
			realtime, client->name, seq);
		return;
	}

	baseline_index = SV_FindSnapshotBaselineIndex (client, seq);
	if (baseline_index < 0)
	{
		if (sv_snapshotdebug.value || sv_snap_debug.value)
			Con_Printf ("snapshot %s ack seq %u ignored (not in window)\n", client->name, seq);
		return;
	}

	if (seq != 0xFFFFFFFFu)
	{
		if (SV_SnapshotSeqNewer (seq, client->snapshot_last_acked_seq))
			client->snapshot_last_acked_seq = seq;
		client->snapshot_last_acked_time = realtime;
		client->snapshot_unacked_frames = 0;
		client->snapshot_has_valid_base = true;
	}

	if (sv_snapshotdebug.value || sv_snap_debug.value)
		Con_Printf ("snapshot %s ack seq %u\n", client->name, seq);
}

void SV_SnapshotNak (client_t *client, unsigned int expected_base, unsigned int received_base)
{
	if (sv_snap_debug.value >= 1.0f)
		Con_Printf ("snapshot %s nak expected %u received %u\n", client->name, expected_base, received_base);
	NET_DebugLogEvent (true,
		"NETDBG time %.3f snap_nak %s expected %u received %u\n",
		realtime, client->name, expected_base, received_base);
	client->snapshot_force_full = true;
	client->snapshot_pending_is_delta = false;
	client->snapshot_pending_seq = 0;
	client->snapshot_unacked_frames = 0;
	client->snapshot_pending_incomplete = false;
	client->entstream.active = false;
}

/*
=============
SV_CleanupEnts

=============
*/
void SV_CleanupEnts (void)
{
	int		e;
	edict_t	*ent;

	ent = NEXT_EDICT(qcvm->edicts);
	for (e=1 ; e<qcvm->num_edicts ; e++, ent = NEXT_EDICT(ent))
	{
		ent->v.effects = (int)ent->v.effects & ~EF_MUZZLEFLASH;
	}
}

/*
==================
SV_WriteClientdataToMessage

==================
*/
void SV_WriteClientdataToMessage (client_t *client, edict_t *ent, sizebuf_t *msg)
{
	int		bits;
	int		i;
	edict_t	*other;
	int		items;
	eval_t	*val;

//
// send a damage message
//
	if (ent->v.dmg_take || ent->v.dmg_save)
	{
		other = PROG_TO_EDICT(ent->v.dmg_inflictor);
		MSG_BEGINSVC (msg, svc_damage);
		MSG_WriteByte (msg, svc_damage);
		MSG_WriteByte (msg, ent->v.dmg_save);
		MSG_WriteByte (msg, ent->v.dmg_take);
		for (i=0 ; i<3 ; i++)
			MSG_WriteCoord (msg, other->v.origin[i] + 0.5*(other->v.mins[i] + other->v.maxs[i]), sv.protocolflags );

		ent->v.dmg_take = 0;
		ent->v.dmg_save = 0;
	}

//
// send the current viewpos offset from the view entity
//
	SV_SetIdealPitch ();		// how much to look up / down ideally

// a fixangle might get lost in a dropped packet.  Oh well.
	if ( ent->v.fixangle )
	{
		MSG_BEGINSVC (msg, svc_setangle);
		MSG_WriteByte (msg, svc_setangle);
		for (i=0 ; i < 3 ; i++)
			MSG_WriteAngle (msg, ent->v.angles[i], sv.protocolflags );
		ent->v.fixangle = 0;
	}

	bits = 0;

	if (ent->v.view_ofs[2] != DEFAULT_VIEWHEIGHT)
		bits |= SU_VIEWHEIGHT;

	if (ent->v.idealpitch)
		bits |= SU_IDEALPITCH;

// stuff the sigil bits into the high bits of items for sbar, or else
// mix in items2
	val = GetEdictFieldValueByName(ent, "items2");

	if (val)
		items = (int)ent->v.items | ((int)val->_float << 23);
	else
		items = (int)ent->v.items | ((int)pr_global_struct->serverflags << 28);

	bits |= SU_ITEMS;

	if ( (int)ent->v.flags & FL_ONGROUND)
		bits |= SU_ONGROUND;

	if ( ent->v.waterlevel >= 2)
		bits |= SU_INWATER;

	for (i=0 ; i<3 ; i++)
	{
		if (ent->v.punchangle[i])
			bits |= (SU_PUNCH1<<i);
		if (ent->v.velocity[i])
			bits |= (SU_VELOCITY1<<i);
	}

	if (ent->v.weaponframe)
		bits |= SU_WEAPONFRAME;

	if (ent->v.armorvalue)
		bits |= SU_ARMOR;

//	if (ent->v.weapon)
	  bits |= SU_WEAPON;

	bits |= SU_PREDICT;

	if (bits & SU_WEAPON && SV_ModelIndex(PR_GetString(ent->v.weaponmodel)) & 0xFF00) bits |= SU_WEAPON2;
	if ((int)ent->v.armorvalue & 0xFF00) bits |= SU_ARMOR2;
	if ((int)ent->v.currentammo & 0xFF00) bits |= SU_AMMO2;
	if ((int)ent->v.ammo_shells & 0xFF00) bits |= SU_SHELLS2;
	if ((int)ent->v.ammo_nails & 0xFF00) bits |= SU_NAILS2;
	if ((int)ent->v.ammo_rockets & 0xFF00) bits |= SU_ROCKETS2;
	if ((int)ent->v.ammo_cells & 0xFF00) bits |= SU_CELLS2;
	if (bits & SU_WEAPONFRAME && (int)ent->v.weaponframe & 0xFF00) bits |= SU_WEAPONFRAME2;
	if (bits & SU_WEAPON && ent->alpha != ENTALPHA_DEFAULT) bits |= SU_WEAPONALPHA; //for now, weaponalpha = client entity alpha
	if (bits >= 65536) bits |= SU_EXTEND1;
	if (bits >= 16777216) bits |= SU_EXTEND2;

// send the data

	MSG_WriteByte (msg, svc_clientdata);
	MSG_WriteShort (msg, bits);

	if (bits & SU_EXTEND1) MSG_WriteByte(msg, bits>>16);
	if (bits & SU_EXTEND2) MSG_WriteByte(msg, bits>>24);

	if (bits & SU_VIEWHEIGHT)
		MSG_WriteChar (msg, ent->v.view_ofs[2]);

	if (bits & SU_IDEALPITCH)
		MSG_WriteChar (msg, ent->v.idealpitch);

	for (i=0 ; i<3 ; i++)
	{
		if (bits & (SU_PUNCH1<<i))
			MSG_WriteChar (msg, ent->v.punchangle[i]);
		if (bits & (SU_VELOCITY1<<i))
			MSG_WriteChar (msg, ent->v.velocity[i]/16);
	}

// [always sent]	if (bits & SU_ITEMS)
	MSG_WriteLong (msg, items);

	if (bits & SU_WEAPONFRAME)
		MSG_WriteByte (msg, ent->v.weaponframe);
	if (bits & SU_ARMOR)
		MSG_WriteByte (msg, ent->v.armorvalue);
	if (bits & SU_WEAPON)
		MSG_WriteByte (msg, SV_ModelIndex(PR_GetString(ent->v.weaponmodel)));

	MSG_WriteShort (msg, ent->v.health);
	MSG_WriteByte (msg, ent->v.currentammo);
	MSG_WriteByte (msg, ent->v.ammo_shells);
	MSG_WriteByte (msg, ent->v.ammo_nails);
	MSG_WriteByte (msg, ent->v.ammo_rockets);
	MSG_WriteByte (msg, ent->v.ammo_cells);

	if (standard_quake)
	{
		MSG_WriteByte (msg, ent->v.weapon);
	}
	else
	{
		for(i=0;i<32;i++)
		{
			if ( ((int)ent->v.weapon) & (1<<i) )
			{
				MSG_WriteByte (msg, i);
				break;
			}
		}
	}

	if (bits & SU_WEAPON2)
		MSG_WriteByte (msg, SV_ModelIndex(PR_GetString(ent->v.weaponmodel)) >> 8);
	if (bits & SU_ARMOR2)
		MSG_WriteByte (msg, (int)ent->v.armorvalue >> 8);
	if (bits & SU_AMMO2)
		MSG_WriteByte (msg, (int)ent->v.currentammo >> 8);
	if (bits & SU_SHELLS2)
		MSG_WriteByte (msg, (int)ent->v.ammo_shells >> 8);
	if (bits & SU_NAILS2)
		MSG_WriteByte (msg, (int)ent->v.ammo_nails >> 8);
	if (bits & SU_ROCKETS2)
		MSG_WriteByte (msg, (int)ent->v.ammo_rockets >> 8);
	if (bits & SU_CELLS2)
		MSG_WriteByte (msg, (int)ent->v.ammo_cells >> 8);
	if (bits & SU_WEAPONFRAME2)
		MSG_WriteByte (msg, (int)ent->v.weaponframe >> 8);
	if (bits & SU_WEAPONALPHA)
		MSG_WriteByte (msg, ent->alpha); //for now, weaponalpha = client entity alpha
	//johnfitz

	MSG_WriteLong (msg, (int)client->last_cmd_seq);
	MSG_WriteLong (msg, (int)client->last_cmd_ack);
	for (i=0 ; i<3 ; i++)
		MSG_WriteCoord (msg, ent->v.origin[i], sv.protocolflags);
	for (i=0 ; i<3 ; i++)
		MSG_WriteCoord (msg, ent->v.velocity[i], sv.protocolflags);
	for (i=0 ; i<3 ; i++)
		MSG_WriteAngle16 (msg, ent->v.v_angle[i], sv.protocolflags);

	// Hack: Alkaline 1.1 uses bit flags to store the active weapon,
	// but we only send the stat as a byte, which can lead to truncation.
	// If we detect this, re-send the stat separately (as a 32-bit int).
	if ((byte)ent->v.weapon != (int)ent->v.weapon && msg->cursize + 6 <= msg->maxsize)
	{
		MSG_WriteByte (msg, svc_updatestat);
		MSG_WriteByte (msg, STAT_ACTIVEWEAPON);
		MSG_WriteLong (msg, (int)ent->v.weapon);
	}
}

/*
Outgoing server->client packet pipeline (queues/merge/send):
- Reliable queue: client->message and sv.reliable_datagram (SV_UpdateToReliableMessages,
  SV_WriteStats, signon buffers/chunks) flushed in SV_SendClientMessages via NET_SendMessage.
- Unreliable queue: sv.datagram (temp entities/sounds) plus snapshot stream (SV_SendSnapshot)
  merged in SV_SendClientDatagram after svc_time/clientdata are written.
- Flush/coalesce points: SV_SendClientMessages (per-tick), SV_SendClientDatagram (per-client),
  NET_Send(Un)reliableMessage -> driver sendto()/loopback.
- Resend path: net_dgrm reliable resend (ReSendMessage) and snapshot resend via SV_SnapshotAck/Nak.
- Continuations: snapshot2 entity stream chunking and signon chunk window (SV_SignonStreamSend).
*/

/*
=======================
SV_SendClientDatagram
=======================
*/
qboolean SV_SendClientDatagram (client_t *client)
{
	byte		buf[MAX_DATAGRAM];
	sizebuf_t	msg;
	int		mtu_cap;
	qboolean	packet_too_large = false;

	msg.data = buf;
	msg.maxsize = sizeof(buf);
	msg.cursize = 0;
	msg.allowoverflow = true;
	msg.overflowed = false;
	msg.overflowed_once = false;
	msg.write_blocked = false;
	msg.write_locked = false;
	msg.blocked_file = NULL;
	msg.blocked_line = 0;
	msg.dbg_name = "sv_client_datagram";
	msg.dbg_file = NULL;
	msg.dbg_line = 0;
	msg.dbg_msgkind = 0;
	msg.dbg_id = 0;
	msg.dbg_aux = 0;
	msg.bitpos = 0;

	mtu_cap = SV_MTUCap (client);
	if (mtu_cap > 0)
		msg.maxsize = q_min (msg.maxsize, mtu_cap);

	if (!NET_CanSendUnreliableMessage (client->netconnection))
		return false;

	MSG_BEGINSVC (&msg, svc_time);
	MSG_WriteByte (&msg, svc_time);
	MSG_WriteFloat (&msg, qcvm->time);

// add the client specific data to the datagram
	// INV-2: control/ack data (time + clientdata) is written first and never trimmed.
	SV_WriteClientdataToMessage (client, client->edict, &msg);
	if (msg.overflowed)
	{
		Con_Printf ("sv_mtu_cap %d too small for mandatory clientdata for %s\n",
			msg.maxsize, client->name);
		SV_DropClient (true);
		return false;
	}

	SV_SendSnapshot (client, &msg);

	if (msg.overflowed)
	{
		packet_too_large = true;
		goto datagram_too_large;
	}
	client->datagram_overflow_count = 0;

// copy the server datagram if there is space
	if (!msg.write_locked && msg.cursize + sv.datagram.cursize < msg.maxsize)
		SZ_Write (&msg, sv.datagram.data, sv.datagram.cursize);
	else if (!msg.write_locked && sv.datagram.cursize > 0)
	{
		// INV-2: keep control/snapshot data; trim low-priority sv.datagram instead.
		NET_DebugLogEvent (true,
			"NETDBG time %.3f trim_unreliable %s keep_control snapshot %d drop_datagram %d mtu_cap %d\n",
			realtime, client->name, client->mtu_last_snapshot_size, sv.datagram.cursize, msg.maxsize);
	}

	if (msg.cursize > msg.maxsize)
	{
		// INV-1: final MTU guard before NET_SendUnreliableMessage.
		packet_too_large = true;
		NET_DebugLogEvent (true,
			"NETDBG time %.3f PREVENTED_OVERSHOOT %s total %d max %d reliable_pending %d snapshot %d datagram %d\n",
			realtime, client->name, msg.cursize, msg.maxsize, client->message.cursize,
			client->mtu_last_snapshot_size, sv.datagram.cursize);
		goto datagram_too_large;
	}

	if (sv_mtu.value > 0)
	{
		int packet_bytes = msg.cursize + NET_HEADERSIZE + NET_UDPIP_HEADER_BYTES;
		if (packet_bytes > (int)sv_mtu.value)
		{
			packet_too_large = true;
			NET_DebugLogEvent (true,
				"NETDBG time %.3f PREVENTED_OVERSHOOT %s total %d mtu %d snapshot %d\n",
				realtime, client->name, packet_bytes, (int)sv_mtu.value, client->mtu_last_snapshot_size);
			goto datagram_too_large;
		}
	}

	if (sv_mtu_debug.value > 0 && realtime >= client->mtu_debug_next_time)
	{
		int packet_bytes = msg.cursize + NET_HEADERSIZE + NET_UDPIP_HEADER_BYTES;
		Con_Printf ("mtu %s sv_mtu %d packet %d reliable %d snapshot %d dropped tier1 %d tier2 %d\n",
			client->name,
			(int)sv_mtu.value,
			packet_bytes,
			client->message.cursize,
			client->mtu_last_snapshot_size,
			client->mtu_dropped_tier1,
			client->mtu_dropped_tier2);
		if (mtu_cap > 0 && sv_mtu.value > 0)
			SDL_assert (packet_bytes <= (int)sv_mtu.value);
		client->mtu_debug_next_time = realtime + 1.0;
	}

// send the datagram
	if (NET_SendUnreliableMessage (client->netconnection, &msg) == -1)
	{
		SV_DropClient (true);// if the message couldn't send, kick off
		return false;
	}

	NET_DebugLogEvent (false,
		"NETDBG time %.3f send_packet %s seq %u base %u flags 0x%x total %d reliable_pending %d "
		"unreliable %d snapshot %d signon %d queue_rel %d\n",
		realtime,
		client->name,
		client->snapshot_last_sent_seq,
		client->snapshot_debug_base,
		client->snapshot_debug_flags,
		msg.cursize,
		client->message.cursize,
		msg.cursize,
		client->mtu_last_snapshot_size,
		0,
		client->message.cursize);

	return true;

datagram_too_large:
	if (packet_too_large)
	{
		NET_DebugLogEvent (true,
			"NETDBG time %.3f PREVENTED_OVERSHOOT %s total %d max %d snapshot %d stage %d\n",
			realtime, client->name, msg.cursize, msg.maxsize, client->mtu_last_snapshot_size,
			client->sendsignon);
		client->datagram_overflow_count++;
		if (client->datagram_overflow_count > 1)
		{
			if (!dev_overflows.datagram || dev_overflows.datagram + CONSOLE_RESPAM_TIME < realtime )
			{
				Con_Printf ("Datagram overflow for %s (last write %s:%d msgkind=%d id=%d aux=%d)\n",
					client->name,
					msg.dbg_file ? msg.dbg_file : "unknown",
					msg.dbg_line,
					msg.dbg_msgkind,
					msg.dbg_id,
					msg.dbg_aux);
				dev_overflows.datagram = realtime;
			}
			client->datagram_overflow_count = 1;
		}
		if (realtime >= client->datagram_overflow_next_time)
		{
			if (client->entstream.active)
				Con_Printf ("Datagram too large for MTU cap %d for %s (size %d, stage %d, entities %d/%d)\n",
					msg.maxsize,
					client->name,
					msg.cursize,
					client->sendsignon,
					client->entstream.sent_entities,
					client->entstream.total_entities);
			else
				Con_Printf ("Datagram too large for MTU cap %d for %s (size %d, stage %d, mandatory %d dropped %d)\n",
					msg.maxsize,
					client->name,
					msg.cursize,
					client->sendsignon,
					client->snapshot_pending_mandatory,
					client->snapshot_pending_dropped);
			client->datagram_overflow_next_time = realtime + 1.0;
		}
		return false;
	}

	return false;
}

/*
=======================
SV_WriteStats

TODO: group multiple stats in a single stuffcmd, the client already supports this
=======================
*/
void SV_WriteStats (client_t *client)
{
	int			statsi[MAX_CL_STATS];
	float		statsf[MAX_CL_STATS];
	const char	*statss[MAX_CL_STATS];
	int			i;

	SV_CalcStats (client, statsi, statsf, statss);

	for (i = 0; i < MAX_CL_STATS; i++)
	{
		//small cleanup
		if (!statsi[i])
			statsi[i] =	statsf[i];
		else
			statsf[i] =	0;//statsi[i];

		if (i >= STAT_NONCLIENT && (statsi[i] != client->oldstats_i[i] || statsf[i] != client->oldstats_f[i]))
		{
			client->oldstats_i[i] = statsi[i];
			client->oldstats_f[i] = statsf[i];

			if ((double)statsi[i] != statsf[i] && statsf[i])
			{	//didn't round nicely, so send as a float
				MSG_WriteByte (&client->message, svc_stufftext);
				MSG_WriteString (&client->message, va ("//st %i %g\n", i, statsf[i]));
			}
			else
			{
				if (i < MAX_CL_BASE_STATS)
				{
					MSG_WriteByte (&client->message, svc_updatestat);
					MSG_WriteByte (&client->message, i);
					MSG_WriteLong (&client->message, statsi[i]);
				}
				else
				{
					MSG_WriteByte (&client->message, svc_stufftext);
					MSG_WriteString (&client->message, va ("//st %i %i\n", i, statsi[i]));
				}
			}
		}

		if (statss[i] || client->oldstats_s[i])
		{
			const char *os = client->oldstats_s[i];
			const char *ns = statss[i];
			if (!ns)	ns="";
			if (!os)	os="";
			if (strcmp(os,ns))
			{
				free(client->oldstats_s[i]);
				client->oldstats_s[i] = strdup(ns);

				MSG_WriteByte (&client->message, svc_stufftext);
				MSG_WriteString (&client->message, va ("//sts %i \"%s\"\n", i, ns));
			}
		}
	}
}

/*
=======================
SV_WriteUnderwaterOverride
=======================
*/
void SV_WriteUnderwaterOverride (client_t *client)
{
	if (!client->edict->sendforcewater)
		return;
	client->edict->sendforcewater = false;
	MSG_WriteByte (&client->message, svc_stufftext);
	MSG_WriteString (&client->message, va ("//v_water %i\n", client->edict->forcewater));
}


/*
=======================
SV_UpdateToReliableMessages
=======================
*/
void SV_UpdateToReliableMessages (void)
{
	int			i, j;
	client_t *client;

// check for changes to be sent over the reliable streams
	for (i=0, host_client = svs.clients ; i<svs.maxclients ; i++, host_client++)
	{
		if (host_client->old_frags != host_client->edict->v.frags)
		{
			for (j=0, client = svs.clients ; j<svs.maxclients ; j++, client++)
			{
				if (!client->active || client->is_bot)
					continue;
				MSG_WriteByte (&client->message, svc_updatefrags);
				MSG_WriteByte (&client->message, i);
				MSG_WriteShort (&client->message, host_client->edict->v.frags);
			}

			host_client->old_frags = host_client->edict->v.frags;
		}
	}

	for (j=0, client = svs.clients ; j<svs.maxclients ; j++, client++)
	{
		if (!client->active || client->is_bot)
			continue;
		SV_WriteStats (client);
		SV_WriteUnderwaterOverride (client);
		SZ_Write (&client->message, sv.reliable_datagram.data, sv.reliable_datagram.cursize);
	}

	SZ_Clear (&sv.reliable_datagram);
}


/*
=======================
SV_SendNop

Send a nop message without trashing or sending the accumulated client
message buffer
=======================
*/
void SV_SendNop (client_t *client)
{
	sizebuf_t	msg;
	byte		buf[4];

	msg.data = buf;
	msg.maxsize = sizeof(buf);
	msg.cursize = 0;
	msg.allowoverflow = false;
	msg.overflowed = false;
	msg.overflowed_once = false;
	msg.write_blocked = false;
	msg.write_locked = false;
	msg.blocked_file = NULL;
	msg.blocked_line = 0;
	msg.dbg_name = "sv_nop";
	msg.dbg_file = NULL;
	msg.dbg_line = 0;
	msg.dbg_msgkind = 0;
	msg.dbg_id = 0;
	msg.dbg_aux = 0;
	msg.bitpos = 0;

	MSG_BEGINSVC (&msg, svc_nop);
	MSG_WriteChar (&msg, svc_nop);

	if (NET_SendUnreliableMessage (client->netconnection, &msg) == -1)
		SV_DropClient (true);	// if the message couldn't send, kick off
	client->last_message = realtime;
}

/*
=======================
SV_SendClientMessages
=======================
*/
void SV_SendClientMessages (void)
{
	int			i;

// update frags, names, etc
	SV_UpdateToReliableMessages ();

// build individual updates
	for (i=0, host_client = svs.clients ; i<svs.maxclients ; i++, host_client++)
	{
		if (!host_client->active)
			continue;
		if (host_client->is_bot)
			continue;
		SZ_UNBLOCK_WRITES (&host_client->message);

		if (host_client->spawned)
		{
			if (!SV_SendClientDatagram (host_client))
				continue;
		}
		else
		{
		// the player isn't totally in the game yet
		// send small keepalive messages if too much time has passed
		// send a full message when the next signon stage has been requested
		// some other message data (name changes, etc) may accumulate
		// between signon stages
			if (!NET_CanSendMessage (host_client->netconnection))
			{
				SZ_BLOCK_WRITES (&host_client->message);
				continue;
			}
			if (!host_client->sendsignon)
			{
				if (realtime - host_client->last_message > 5)
					SV_SendNop (host_client);
				continue;	// don't send out non-signon messages
			}
			if (host_client->sendsignon == PRESPAWN_SIGNONBUFS)
			{
				if (SV_SignonChunksEnabled ())
				{
					SV_SignonStreamSend (host_client);
				}
				else
				{
					while (host_client->signonidx < sv.num_signon_buffers)
					{
						sizebuf_t *signon = sv.signon_buffers[host_client->signonidx];
						if (host_client->message.cursize + signon->cursize > host_client->message.maxsize)
							break;
						SZ_Write (&host_client->message, signon->data, signon->cursize);
						host_client->signonidx++;
						// send one signon buffer per tick to avoid bursty coalescing
						break;
					}
					if (host_client->signonidx == sv.num_signon_buffers)
						host_client->sendsignon = PRESPAWN_SIGNONMSG;
				}
			}
			if (host_client->sendsignon == PRESPAWN_SIGNONMSG)
			{
				if (host_client->message.cursize + 2 < host_client->message.maxsize)
				{
					MSG_BEGINSVC (&host_client->message, svc_signonnum);
					MSG_DBGAUX (&host_client->message, 2);
					MSG_WriteByte (&host_client->message, svc_signonnum);
					MSG_WriteByte (&host_client->message, 2);
					host_client->sendsignon = PRESPAWN_FLUSH;
				}
			}
		}

		// check for an overflowed message.  Should only happen
		// on a very fucked up connection that backs up a lot, then
		// changes level
		if (host_client->message.overflowed)
		{
			SV_DropClient (true);
			host_client->message.overflowed = false;
			continue;
		}

		if (host_client->message.cursize || host_client->dropasap)
		{
			if (!NET_CanSendMessage (host_client->netconnection))
			{
//				I_Printf ("can't write\n");
				SZ_BLOCK_WRITES (&host_client->message);
				continue;
			}

			if (host_client->dropasap)
				SV_DropClient (false);	// went to another level
			else
			{
				if (NET_SendMessage (host_client->netconnection
				, &host_client->message) == -1)
					SV_DropClient (true);	// if the message couldn't send, kick off
				else
				{
					NET_DebugLogEvent (false,
						"NETDBG time %.3f send_reliable %s size %d signon_stage %d queue_rel %d\n",
						realtime, host_client->name, host_client->message.cursize,
						(int)host_client->sendsignon, host_client->message.cursize);
				}
				SZ_Clear (&host_client->message);
				host_client->last_message = realtime;
				if (host_client->sendsignon == PRESPAWN_FLUSH)
					host_client->sendsignon = PRESPAWN_DONE;
			}
		}
	}


// clear muzzle flashes
	SV_CleanupEnts ();
}


/*
==============================================================================

SERVER SPAWNING

==============================================================================
*/

#define SIGNON_SIZE		(MAX_MSGLEN - 1024) // allow larger signon buffers for busy startups while leaving headroom
#define SIGNON_CHUNK_MAX_PAYLOAD	1100
#define SIGNON_CHUNK_HEADER_BYTES	7
#define SIGNON_RECORD_MAX_SIZE		65535

typedef struct
{
	int	buffer_index;
	int	buffer_offset;
} signon_reader_t;

static qboolean SV_SignonChunksEnabled (void)
{
	return (sv.protocolflags & PRFL_SIGNON_CHUNKS) != 0;
}

static qboolean SV_SignonReaderAtEnd (const signon_reader_t *reader)
{
	int index = reader->buffer_index;
	int offset = reader->buffer_offset;

	while (index < sv.num_signon_buffers)
	{
		sizebuf_t *sb = sv.signon_buffers[index];
		if (offset < sb->cursize)
			return false;
		index++;
		offset = 0;
	}
	return true;
}

static qboolean SV_SignonReaderRead (signon_reader_t *reader, void *out, int count)
{
	byte *outbytes = (byte *)out;
	int remaining = count;

	while (remaining > 0)
	{
		sizebuf_t *sb;
		int available;
		int take;

		while (reader->buffer_index < sv.num_signon_buffers)
		{
			sb = sv.signon_buffers[reader->buffer_index];
			if (reader->buffer_offset < sb->cursize)
				break;
			reader->buffer_index++;
			reader->buffer_offset = 0;
		}

		if (reader->buffer_index >= sv.num_signon_buffers)
			return false;

		sb = sv.signon_buffers[reader->buffer_index];
		available = sb->cursize - reader->buffer_offset;
		take = q_min(available, remaining);
		if (outbytes)
		{
			memcpy (outbytes, sb->data + reader->buffer_offset, take);
			outbytes += take;
		}
		reader->buffer_offset += take;
		remaining -= take;
	}

	return true;
}

static qboolean SV_SignonReadBytes (signon_reader_t *reader, int count, size_t *consumed)
{
	if (!SV_SignonReaderRead (reader, NULL, count))
		return false;
	if (consumed)
		*consumed += (size_t)count;
	return true;
}

static qboolean SV_SignonReadByte (signon_reader_t *reader, int *out, size_t *consumed)
{
	byte value;
	if (!SV_SignonReaderRead (reader, &value, 1))
		return false;
	if (out)
		*out = value;
	if (consumed)
		*consumed += 1;
	return true;
}

static qboolean SV_SignonReadShort (signon_reader_t *reader, size_t *consumed)
{
	return SV_SignonReadBytes (reader, 2, consumed);
}

static qboolean SV_SignonReadLong (signon_reader_t *reader, size_t *consumed)
{
	return SV_SignonReadBytes (reader, 4, consumed);
}

static qboolean SV_SignonReadString (signon_reader_t *reader, size_t *consumed)
{
	int c;
	do
	{
		if (!SV_SignonReadByte (reader, &c, consumed))
			return false;
	} while (c != 0);
	return true;
}

static int SV_SignonCoordBytes (unsigned int protocolflags)
{
	if (protocolflags & PRFL_INT32COORD)
		return 4;
	return 2;
}

static int SV_SignonAngleBytes (unsigned int protocolflags)
{
	if (protocolflags & PRFL_SHORTANGLE)
		return 2;
	return 1;
}

static signon_stage_t SV_SignonStageForCommand (int cmd)
{
	switch (cmd)
	{
	case svc_spawnbaseline:
	case svc_spawnbaseline2:
		return SIGNON_STAGE_BASELINES;
	case svc_spawnstatic:
	case svc_spawnstatic2:
	case svc_spawnstaticsound:
	case svc_spawnstaticsound2:
		return SIGNON_STAGE_STATIC_ENTS;
	case svc_lightstyle:
		return SIGNON_STAGE_LIGHTSTYLES;
	default:
		return SIGNON_STAGE_CUSTOM_EXT;
	}
}

static int SV_SignonCommandLength (const signon_reader_t *reader, int *out_cmd, signon_stage_t *out_stage)
{
	signon_reader_t temp = *reader;
	size_t consumed = 0;
	int cmd;
	int coord_bytes = SV_SignonCoordBytes (sv.protocolflags);
	int angle_bytes = SV_SignonAngleBytes (sv.protocolflags);

	if (!SV_SignonReadByte (&temp, &cmd, &consumed))
		return 0;

	if (cmd & 128)
		return -1;

	if (out_cmd)
		*out_cmd = cmd;
	if (out_stage)
		*out_stage = SV_SignonStageForCommand (cmd);

	switch (cmd)
	{
	case svc_nop:
	case svc_disconnect:
	case svc_bf:
	case svc_sellscreen:
		break;

	case svc_time:
		if (!SV_SignonReadLong (&temp, &consumed))
			return 0;
		break;

	case svc_clientdata:
		if (!SV_SignonReadShort (&temp, &consumed))
			return 0;
		// variable length based on bits - skip worst case by reading all bits
		// clientdata does not appear during signon, so reject to avoid mis-splitting
		return -1;

	case svc_version:
		if (!SV_SignonReadLong (&temp, &consumed))
			return 0;
		break;

	case svc_print:
	case svc_stufftext:
	case svc_centerprint:
	case svc_finale:
	case svc_cutscene:
	case svc_skybox:
	case svc_rawprint:
	case svc_servervars:
	case svc_chat:
	case svc_achievement:
		if (!SV_SignonReadString (&temp, &consumed))
			return 0;
		if (cmd == svc_finale)
		{
			if (!SV_SignonReadString (&temp, &consumed))
				return 0;
		}
		break;

	case svc_serverinfo:
		return -1;

	case svc_lightstyle:
		if (!SV_SignonReadByte (&temp, NULL, &consumed))
			return 0;
		if (!SV_SignonReadString (&temp, &consumed))
			return 0;
		break;

	case svc_setangle:
		if (!SV_SignonReadBytes (&temp, angle_bytes * 3, &consumed))
			return 0;
		break;

	case svc_setview:
		if (!SV_SignonReadShort (&temp, &consumed))
			return 0;
		break;

	case svc_sound:
	{
		int field_mask;
		if (!SV_SignonReadByte (&temp, &field_mask, &consumed))
			return 0;
		if (field_mask & SND_VOLUME)
		{
			if (!SV_SignonReadByte (&temp, NULL, &consumed))
				return 0;
		}
		if (field_mask & SND_ATTENUATION)
		{
			if (!SV_SignonReadByte (&temp, NULL, &consumed))
				return 0;
		}
		if (field_mask & SND_LARGEENTITY)
		{
			if (!SV_SignonReadShort (&temp, &consumed))
				return 0;
			if (!SV_SignonReadByte (&temp, NULL, &consumed))
				return 0;
		}
		else
		{
			if (!SV_SignonReadShort (&temp, &consumed))
				return 0;
		}
		if (field_mask & SND_LARGESOUND)
		{
			if (!SV_SignonReadShort (&temp, &consumed))
				return 0;
		}
		else
		{
			if (!SV_SignonReadByte (&temp, NULL, &consumed))
				return 0;
		}
		if (!SV_SignonReadBytes (&temp, coord_bytes * 3, &consumed))
			return 0;
		break;
	}

	case svc_stopsound:
		if (!SV_SignonReadShort (&temp, &consumed))
			return 0;
		break;

	case svc_updatename:
		if (!SV_SignonReadByte (&temp, NULL, &consumed))
			return 0;
		if (!SV_SignonReadString (&temp, &consumed))
			return 0;
		break;

	case svc_updatecolors:
		if (!SV_SignonReadBytes (&temp, 2, &consumed))
			return 0;
		break;

	case svc_updatestat:
		if (!SV_SignonReadByte (&temp, NULL, &consumed))
			return 0;
		if (!SV_SignonReadLong (&temp, &consumed))
			return 0;
		break;

	case svc_updatefrags:
		if (!SV_SignonReadByte (&temp, NULL, &consumed))
			return 0;
		if (!SV_SignonReadShort (&temp, &consumed))
			return 0;
		break;

	case svc_particle:
		if (!SV_SignonReadBytes (&temp, coord_bytes * 3, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, 3, &consumed))
			return 0;
		if (!SV_SignonReadByte (&temp, NULL, &consumed))
			return 0;
		if (!SV_SignonReadByte (&temp, NULL, &consumed))
			return 0;
		break;

	case svc_damage:
		if (!SV_SignonReadBytes (&temp, 2, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, coord_bytes * 3, &consumed))
			return 0;
		break;

	case svc_spawnstatic:
		if (!SV_SignonReadBytes (&temp, 1 + 1 + 1 + 1, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, coord_bytes * 3, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, angle_bytes * 3, &consumed))
			return 0;
		break;

	case svc_spawnbaseline:
		if (!SV_SignonReadShort (&temp, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, 1 + 1 + 1 + 1, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, coord_bytes * 3, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, angle_bytes * 3, &consumed))
			return 0;
		break;

	case svc_temp_entity:
		return -1;

	case svc_spawnstaticsound:
		if (!SV_SignonReadBytes (&temp, coord_bytes * 3, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, 1 + 1 + 1, &consumed))
			return 0;
		break;

	case svc_intermission:
		break;

	case svc_cdtrack:
		if (!SV_SignonReadBytes (&temp, 2, &consumed))
			return 0;
		break;

	case svc_fog:
		if (!SV_SignonReadBytes (&temp, 4, &consumed))
			return 0;
		if (!SV_SignonReadShort (&temp, &consumed))
			return 0;
		break;

	case svc_spawnbaseline2:
	{
		int bits;
		if (!SV_SignonReadShort (&temp, &consumed))
			return 0;
		if (!SV_SignonReadByte (&temp, &bits, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, (bits & B_LARGEMODEL) ? 2 : 1, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, (bits & B_LARGEFRAME) ? 2 : 1, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, 1 + 1, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, coord_bytes * 3, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, angle_bytes * 3, &consumed))
			return 0;
		if (bits & B_ALPHA)
		{
			if (!SV_SignonReadByte (&temp, NULL, &consumed))
				return 0;
		}
		if (bits & B_SCALE)
		{
			if (!SV_SignonReadByte (&temp, NULL, &consumed))
				return 0;
		}
		break;
	}

	case svc_spawnstatic2:
	{
		int bits;
		if (!SV_SignonReadByte (&temp, &bits, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, (bits & B_LARGEMODEL) ? 2 : 1, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, (bits & B_LARGEFRAME) ? 2 : 1, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, 1 + 1, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, coord_bytes * 3, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, angle_bytes * 3, &consumed))
			return 0;
		if (bits & B_ALPHA)
		{
			if (!SV_SignonReadByte (&temp, NULL, &consumed))
				return 0;
		}
		if (bits & B_SCALE)
		{
			if (!SV_SignonReadByte (&temp, NULL, &consumed))
				return 0;
		}
		break;
	}

	case svc_spawnstaticsound2:
		if (!SV_SignonReadBytes (&temp, coord_bytes * 3, &consumed))
			return 0;
		if (!SV_SignonReadShort (&temp, &consumed))
			return 0;
		if (!SV_SignonReadBytes (&temp, 2, &consumed))
			return 0;
		break;

	case svc_localsound:
	{
		int field_mask;
		if (!SV_SignonReadByte (&temp, &field_mask, &consumed))
			return 0;
		if (field_mask & SND_LARGESOUND)
		{
			if (!SV_SignonReadShort (&temp, &consumed))
				return 0;
		}
		else
		{
			if (!SV_SignonReadByte (&temp, NULL, &consumed))
				return 0;
		}
		break;
	}

	case svc_setpause:
		if (!SV_SignonReadByte (&temp, NULL, &consumed))
			return 0;
		break;

	case svc_signonnum:
		if (!SV_SignonReadByte (&temp, NULL, &consumed))
			return 0;
		break;

	default:
		return -1;
	}

	return (int)consumed;
}

static void SV_SignonStreamInit (client_t *client)
{
	signon_stream_t *stream = &client->signon_stream;

	memset (stream, 0, sizeof(*stream));
	stream->active = true;
	stream->stage = SIGNON_STAGE_PRECACHES;
	stream->next_seq = 0;
	stream->acked_seq = 0;
	stream->next_record_id = 0;
	stream->buffer_index = 0;
	stream->buffer_offset = 0;
	stream->record_len = 0;
	stream->record_offset = 0;
	stream->record_cmd = 0;
	stream->record_stage = SIGNON_STAGE_PRECACHES;
	stream->record_active = false;
	stream->start_time = realtime;
}

static void SV_SignonStreamFinish (client_t *client)
{
	signon_stream_t *stream = &client->signon_stream;
	double elapsed = realtime - stream->start_time;
	int stage;

	if (sv_signon_debug.value || sv_signon_chunk_debug.value || developer.value)
	{
		Con_Printf ("Signon chunks complete in %.3fs\n", elapsed);
		for (stage = 0; stage < SIGNON_STAGE_COUNT; stage++)
		{
			if (!stream->stage_records[stage])
				continue;
			Con_Printf ("  stage %d: bytes %u records %u max_record %u\n",
				stage,
				(unsigned)stream->stage_bytes[stage],
				(unsigned)stream->stage_records[stage],
				(unsigned)stream->stage_max_record[stage]);
		}
	}
	stream->active = false;
}

static client_t *SV_SignonFirewallClientForMessage (sizebuf_t *msg)
{
	int i;

	for (i = 0; i < svs.maxclients; i++)
	{
		if (&svs.clients[i].message == msg)
			return &svs.clients[i];
	}

	return NULL;
}

void SV_SignonFirewallCheck (sizebuf_t *msg, int svc_id, const char *file, int line)
{
	if (!sv.active)
		return;
	if (svc_id == svc_signon_chunk)
		return;
	if (msg != sv.signon)
	{
		client_t *client = SV_SignonFirewallClientForMessage (msg);
		if (!client || !client->signon_stream.active)
			return;
	}
	else
	{
		int i;
		qboolean active = false;
		for (i = 0; i < svs.maxclients; i++)
		{
			if (svs.clients[i].signon_stream.active)
			{
				active = true;
				break;
			}
		}
		if (!active)
			return;
	}
	switch (svc_id)
	{
	case svc_serverinfo:
	case svc_signonnum:
	case svc_spawnbaseline:
	case svc_spawnbaseline2:
	case svc_spawnstatic:
	case svc_spawnstatic2:
	case svc_spawnstaticsound:
	case svc_spawnstaticsound2:
	case svc_lightstyle:
		Host_Error ("Signon firewall: direct write of svc %d at %s:%d", svc_id, file, line);
		break;
	default:
		break;
	}
}

static void SV_SignonPayloadWriteByte (byte *payload, int *offset, int value)
{
	payload[*offset] = (byte)value;
	*offset += 1;
}

static void SV_SignonPayloadWriteShort (byte *payload, int *offset, int value)
{
	unsigned short out = (unsigned short)LittleShort(value);
	memcpy (payload + *offset, &out, sizeof(out));
	*offset += (int)sizeof(out);
}

static void SV_SignonStreamSend (client_t *client)
{
	signon_stream_t *stream = &client->signon_stream;
	int window = (int)sv_signon_chunk_window.value;
	int mtu_cap = SV_MTUCap (client);
	int payload_cap = SIGNON_CHUNK_MAX_PAYLOAD;

	if (!stream->active)
		SV_SignonStreamInit (client);

	if (window < 1)
		window = 1;
	if (window > 4)
		window = 4;
	if (mtu_cap > 0)
		payload_cap = q_min (payload_cap, mtu_cap - SIGNON_CHUNK_HEADER_BYTES);
	if (payload_cap < 1)
		return;

	while ((unsigned short)(stream->next_seq - stream->acked_seq) < window)
	{
		signon_reader_t reader = { stream->buffer_index, stream->buffer_offset };
		byte payload[SIGNON_CHUNK_MAX_PAYLOAD];
		int payload_len = 0;
		int cmd_len = 0;
		int cmd = 0;
		signon_stage_t stage = SIGNON_STAGE_PRECACHES;
		signon_stage_t chunk_stage = SIGNON_STAGE_PRECACHES;
		qboolean have_stage = false;
		byte flags = 0;
		int records_in_chunk = 0;

		while (payload_len < payload_cap)
		{
			int remaining = payload_cap - payload_len;
			int header_len;
			int take;

			if (!stream->record_active)
			{
				signon_reader_t temp = reader;
				cmd_len = SV_SignonCommandLength (&temp, &cmd, &stage);
				if (cmd_len == 0)
				{
					stream->complete = true;
					flags |= SIGNON_CHUNK_FLAG_STAGE_END;
					break;
				}
				if (cmd_len < 0)
					Host_Error ("Unsupported signon command %d in stream", cmd);
				if (cmd_len > SIGNON_RECORD_MAX_SIZE)
					Host_Error ("Signon record too large: type %d len %d (map/mod issue)", cmd, cmd_len);

				stream->record_len = cmd_len;
				stream->record_offset = 0;
				stream->record_cmd = cmd;
				stream->record_stage = (byte)stage;
				stream->record_active = true;
			}

			stage = (signon_stage_t)stream->record_stage;
			if (!have_stage)
			{
				chunk_stage = stage;
				have_stage = true;
			}
			else if (stage != chunk_stage)
			{
				flags |= SIGNON_CHUNK_FLAG_STAGE_END;
				break;
			}

			if (stream->record_offset == 0 && stream->record_len <= (remaining - 5))
			{
				header_len = 1 + 2 + 2;
				if (remaining < header_len + stream->record_len)
					break;
				SV_SignonPayloadWriteByte (payload, &payload_len, SIGNON_REC_RAW);
				SV_SignonPayloadWriteShort (payload, &payload_len, stream->next_record_id);
				SV_SignonPayloadWriteShort (payload, &payload_len, stream->record_len);
				if (!SV_SignonReaderRead (&reader, payload + payload_len, stream->record_len))
					Host_Error ("Signon stream read failed");
				payload_len += stream->record_len;
				stream->record_offset = stream->record_len;
			}
			else
			{
				header_len = 1 + 2 + 2 + 2 + 2;
				if (remaining <= header_len)
					break;
				take = q_min(remaining - header_len, stream->record_len - stream->record_offset);
				SV_SignonPayloadWriteByte (payload, &payload_len, SIGNON_REC_RAW_FRAG);
				SV_SignonPayloadWriteShort (payload, &payload_len, stream->next_record_id);
				SV_SignonPayloadWriteShort (payload, &payload_len, stream->record_len);
				SV_SignonPayloadWriteShort (payload, &payload_len, stream->record_offset);
				SV_SignonPayloadWriteShort (payload, &payload_len, take);
				if (!SV_SignonReaderRead (&reader, payload + payload_len, take))
					Host_Error ("Signon stream read failed");
				payload_len += take;
				stream->record_offset += take;
			}

			if (stream->record_offset == stream->record_len)
			{
				stream->record_active = false;
				stream->record_offset = 0;
				stream->stage_bytes[stage] += (size_t)stream->record_len;
				stream->stage_records[stage] += 1;
				if ((size_t)stream->record_len > stream->stage_max_record[stage])
					stream->stage_max_record[stage] = (size_t)stream->record_len;
				stream->next_record_id++;
			}

			records_in_chunk++;
		}

		if (payload_len == 0)
			break;

		if (SV_SignonReaderAtEnd (&reader) && !stream->record_active)
			flags |= SIGNON_CHUNK_FLAG_STAGE_END;

		if (client->message.maxsize - client->message.cursize < SIGNON_CHUNK_HEADER_BYTES + payload_len)
			break;

		MSG_BEGINSVC (&client->message, svc_signon_chunk);
		MSG_DBGAUX (&client->message, have_stage ? chunk_stage : 0);
		MSG_WriteByte (&client->message, svc_signon_chunk);
		MSG_WriteByte (&client->message, have_stage ? chunk_stage : stream->stage);
		MSG_WriteShort (&client->message, stream->next_seq);
		MSG_WriteByte (&client->message, flags);
		MSG_WriteShort (&client->message, payload_len);
		SZ_Write (&client->message, payload, payload_len);

		if (sv_signon_chunk_debug.value)
			Con_Printf ("SIGNONCHUNK: client %s stage %d seq %u len %d flags 0x%x records %d\n",
				client->name, have_stage ? chunk_stage : stream->stage, stream->next_seq, payload_len, flags, records_in_chunk);

		if (have_stage)
			stream->stage = chunk_stage;
		stream->next_seq++;
		stream->buffer_index = reader.buffer_index;
		stream->buffer_offset = reader.buffer_offset;

		if (SV_SignonReaderAtEnd (&reader) && !stream->record_active)
			stream->complete = true;
	}

	if (stream->complete && stream->acked_seq == stream->next_seq)
	{
		if (client->sendsignon == PRESPAWN_SIGNONBUFS)
			client->sendsignon = PRESPAWN_SIGNONMSG;
		if (stream->active)
			SV_SignonStreamFinish (client);
	}
}

/*
================
SV_AddSignonBuffer
================
*/
static void SV_AddSignonBuffer (void)
{
	sizebuf_t *sb;
	if (sv.num_signon_buffers >= MAX_SIGNON_BUFFERS)
		Host_Error ("SV_AddSignonBuffer overflow\n");

	sb = (sizebuf_t *) Hunk_AllocName (sizeof (sizebuf_t) + SIGNON_SIZE, "signon");
	memset (sb, 0, sizeof(*sb));
	sb->data = (byte *)(sb + 1);
	sb->maxsize = SIGNON_SIZE;
	sb->dbg_name = "signon";
	sv.signon_buffers[sv.num_signon_buffers++] = sb;
	sv.signon = sb;
}

/*
================
SV_ReserveSignonSpace
================
*/
void SV_ReserveSignonSpace (int numbytes)
{
	if (sv.signon->cursize + numbytes > sv.signon->maxsize)
		SV_AddSignonBuffer ();
}

/*
================
SV_ModelIndex

================
*/
int SV_ModelIndex (const char *name)
{
	int		i;

	if (!name || !name[0])
		return 0;

	for (i=0 ; i<MAX_MODELS && sv.model_precache[i] ; i++)
		if (!strcmp(sv.model_precache[i], name))
			return i;
	if (i==MAX_MODELS || !sv.model_precache[i])
		Sys_Error ("SV_ModelIndex: model %s not precached", name);
	return i;
}

/*
================
SV_CreateBaseline
================
*/
void SV_CreateBaseline (void)
{
	int			i;
	edict_t		*svent;
	int			entnum;
	int			bits;

	for (entnum = 0; entnum < qcvm->num_edicts ; entnum++)
	{
	// get the current server version
		svent = EDICT_NUM(entnum);
		if (svent->free)
			continue;
		if (entnum > svs.maxclients && !svent->v.modelindex)
			continue;

	//
	// create entity baseline
	//
		VectorCopy (svent->v.origin, svent->baseline.origin);
		VectorCopy (svent->v.angles, svent->baseline.angles);
		svent->baseline.frame = svent->v.frame;
		svent->baseline.skin = svent->v.skin;
		if (entnum > 0 && entnum <= svs.maxclients)
		{
			svent->baseline.colormap = entnum;
			svent->baseline.modelindex = SV_ModelIndex("progs/player.mdl");
			svent->baseline.alpha = ENTALPHA_DEFAULT; //johnfitz -- alpha support
			svent->baseline.scale = ENTSCALE_DEFAULT;
		}
		else
		{
			svent->baseline.colormap = 0;
			svent->baseline.modelindex = SV_ModelIndex(PR_GetString(svent->v.model));
			svent->baseline.alpha = svent->alpha; //johnfitz -- alpha support
			svent->baseline.scale = ENTSCALE_DEFAULT;
			eval_t* val;
			val = GetEdictFieldValueByName(svent, "scale");
			if (val)
				svent->baseline.scale = ENTSCALE_ENCODE(val->_float);
		}

		bits = 0;
		if (svent->baseline.modelindex & 0xFF00)
			bits |= B_LARGEMODEL;
		if (svent->baseline.frame & 0xFF00)
			bits |= B_LARGEFRAME;
		if (svent->baseline.alpha != ENTALPHA_DEFAULT)
			bits |= B_ALPHA;
		if (svent->baseline.scale != ENTSCALE_DEFAULT)
			bits |= B_SCALE;

	//
	// add to the message
	//
		{
			int coord_bytes;
			int angle_bytes;
			int reserve;

			coord_bytes = (sv.protocolflags & PRFL_INT32COORD) ? 4 : 2;
			angle_bytes = (sv.protocolflags & PRFL_SHORTANGLE) ? 2 : 1;

			reserve = 1 + 2;
			if (bits)
				reserve += 1;
			reserve += (bits & B_LARGEMODEL) ? 2 : 1;
			reserve += (bits & B_LARGEFRAME) ? 2 : 1;
			reserve += 1 + 1;
			reserve += 3 * (coord_bytes + angle_bytes);
			if (bits & B_ALPHA)
				reserve += 1;
			if (bits & B_SCALE)
				reserve += 1;

			SV_ReserveSignonSpace (reserve);
		}

		if (bits)
		{
			MSG_BEGINSVC (sv.signon, svc_spawnbaseline2);
			MSG_DBGAUX (sv.signon, entnum);
			MSG_WriteByte (sv.signon, svc_spawnbaseline2);
		}
		else
		{
			MSG_BEGINSVC (sv.signon, svc_spawnbaseline);
			MSG_DBGAUX (sv.signon, entnum);
			MSG_WriteByte (sv.signon, svc_spawnbaseline);
		}

		MSG_WriteShort (sv.signon,entnum);

		if (bits)
			MSG_WriteByte (sv.signon, bits);

		if (bits & B_LARGEMODEL)
			MSG_WriteShort (sv.signon, svent->baseline.modelindex);
		else
			MSG_WriteByte (sv.signon, svent->baseline.modelindex);

		if (bits & B_LARGEFRAME)
			MSG_WriteShort (sv.signon, svent->baseline.frame);
		else
			MSG_WriteByte (sv.signon, svent->baseline.frame);
		//johnfitz

		MSG_WriteByte (sv.signon, svent->baseline.colormap);
		MSG_WriteByte (sv.signon, svent->baseline.skin);
		for (i=0 ; i<3 ; i++)
		{
			MSG_WriteCoord(sv.signon, svent->baseline.origin[i], sv.protocolflags);
			MSG_WriteAngle(sv.signon, svent->baseline.angles[i], sv.protocolflags);
		}

		if (bits & B_ALPHA)
			MSG_WriteByte (sv.signon, svent->baseline.alpha);

		if (bits & B_SCALE)
			MSG_WriteByte (sv.signon, svent->baseline.scale);
	}
}


/*
================
SV_SendReconnect

Tell all the clients that the server is changing levels
================
*/
void SV_SendReconnect (void)
{
	byte	data[128];
	sizebuf_t	msg;

	msg.data = data;
	msg.cursize = 0;
	msg.maxsize = sizeof(data);
	msg.allowoverflow = false;
	msg.overflowed = false;
	msg.overflowed_once = false;
	msg.write_blocked = false;
	msg.write_locked = false;
	msg.blocked_file = NULL;
	msg.blocked_line = 0;
	msg.dbg_name = "sv_reconnect";
	msg.dbg_file = NULL;
	msg.dbg_line = 0;
	msg.dbg_msgkind = 0;
	msg.dbg_id = 0;
	msg.dbg_aux = 0;
	msg.bitpos = 0;

	MSG_BEGINSVC (&msg, svc_stufftext);
	MSG_WriteChar (&msg, svc_stufftext);
	MSG_WriteString (&msg, "reconnect\n");
	NET_SendToAll (&msg, 5.0);

	if (!isDedicated)
		Cmd_ExecuteString ("reconnect\n", src_command);
}


/*
================
SV_SaveSpawnparms

Grabs the current state of each client for saving across the
transition to another level
================
*/
void SV_SaveSpawnparms (void)
{
	int		i, j;

	svs.serverflags = pr_global_struct->serverflags;

	for (i=0, host_client = svs.clients ; i<svs.maxclients ; i++, host_client++)
	{
		if (!host_client->active)
			continue;

	// call the progs to get default spawn parms for the new client
		pr_global_struct->self = EDICT_TO_PROG(host_client->edict);
		PR_ExecuteProgram (pr_global_struct->SetChangeParms);
		for (j=0 ; j<NUM_SPAWN_PARMS ; j++)
			host_client->spawn_parms[j] = (&pr_global_struct->parm1)[j];
	}
}

typedef enum
{
	MAPCHECK_FAILED,
	MAPCHECK_PARTIAL,
	MAPCHECK_OK,
} mapcheck_t;


/*
================
SV_MapCheckThresh
================
*/
static mapcheck_t SV_MapCheckThresh (int current, int target)
{
	if (current <= 0)
		return MAPCHECK_FAILED;
	if (current >= target)
		return MAPCHECK_OK;
	return MAPCHECK_PARTIAL;
}

/*
================
SV_PrintMapCheck
================
*/
static void SV_PrintMapCheck (mapcheck_t status, const char *format, ...)
{
	char		str[1024];
	char		tinted[1024];
	va_list		argptr;

	va_start (argptr, format);
	q_vsnprintf (str, sizeof (str), format, argptr);
	va_end (argptr);

	if (status == MAPCHECK_OK)
	{
		Con_SafePrintf ("[x] %s\n", str);
	}
	else
	{
		Con_SafePrintf ("\xDB%c\xDD %s\n", status == MAPCHECK_PARTIAL ? '-' ^ 0x80 : ' ', COM_TintString (str, tinted, sizeof (tinted)));
		sv.mapchecks.numwarnings++;
	}
}


/*
================
SV_PrintMapChecklist
================
*/
static void SV_PrintMapChecklist (void)
{
	const int	MIN_DM_SPAWN_POINTS = 5;
	const int	MIN_COOP_SPAWN_POINTS = 3;

	qboolean	skill_levels;
	char		buf[1024];
	int			i, track, numskies, count;

	//
	// header
	//
	Con_SafePrintf ("\n");
	Con_SafePrintf ("=====================================\n");
	Con_SafePrintf ("\n");
	Con_SafePrintf ("Map checklist (%s):\n\n", COM_SkipPath (sv.modelname));

	//
	// light data
	//
	SV_PrintMapCheck (sv.worldmodel->lightdata != NULL ? MAPCHECK_OK : MAPCHECK_FAILED, "lightmap data");

	//
	// vis data
	//
	if (!sv.worldmodel->visdata)
	{
		char pointfile[MAX_OSPATH];
		q_snprintf (pointfile, sizeof (pointfile), "maps/%s.pts", sv.name);
		if (COM_FileExists (pointfile, NULL))
			SV_PrintMapCheck (MAPCHECK_FAILED, "vis data (unsealed map?)");
		else
			SV_PrintMapCheck (MAPCHECK_FAILED, "vis data");
	}
	else
		SV_PrintMapCheck (MAPCHECK_OK, "vis data");

	//
	// changelevel trigger
	//
	if (!sv.mapchecks.trigger_changelevel)
		SV_PrintMapCheck (MAPCHECK_FAILED, "trigger_changelevel");
	else if (sv.mapchecks.trigger_changelevel == 1)
	{
		if (sv.mapchecks.valid_changelevel == sv.mapchecks.trigger_changelevel)
			SV_PrintMapCheck (MAPCHECK_OK, "trigger_changelevel (%s)", sv.mapchecks.changelevel);
		else
			SV_PrintMapCheck (MAPCHECK_PARTIAL, "trigger_changelevel (missing \"map\" key)");
	}
	else
	{
		if (sv.mapchecks.valid_changelevel == sv.mapchecks.trigger_changelevel)
			SV_PrintMapCheck (MAPCHECK_OK, "trigger_changelevel (%d)", sv.mapchecks.trigger_changelevel);
		else
			SV_PrintMapCheck (MAPCHECK_PARTIAL, "trigger_changelevel (%d/%d missing \"map\" key)",
				sv.mapchecks.trigger_changelevel - sv.mapchecks.valid_changelevel,
				sv.mapchecks.trigger_changelevel
			);
	}

	//
	// intermission camera
	//
	if (!sv.mapchecks.intermission)
		SV_PrintMapCheck (MAPCHECK_FAILED, "info_intermission");
	else
		SV_PrintMapCheck (MAPCHECK_OK, "info_intermission (%d)", sv.mapchecks.intermission);

	//
	// skill levels
	//
	skill_levels = sv.mapchecks.skill_triggers > 0 ||
		(sv.mapchecks.skill_ents[0] != sv.mapchecks.skill_ents[1] || sv.mapchecks.skill_ents[1] != sv.mapchecks.skill_ents[2]);
	SV_PrintMapCheck (skill_levels ? MAPCHECK_OK : MAPCHECK_FAILED, "skill spawnflags/triggers");

	//
	// coop spawn points
	//
	SV_PrintMapCheck (SV_MapCheckThresh (sv.mapchecks.coop_spawns, MIN_COOP_SPAWN_POINTS),
		"info_player_coop (%d/%d+)", sv.mapchecks.coop_spawns, MIN_COOP_SPAWN_POINTS);

	//
	// deathmatch spawn points
	//
	SV_PrintMapCheck (SV_MapCheckThresh (sv.mapchecks.dm_spawns, MIN_DM_SPAWN_POINTS),
		"info_player_deathmatch (%d/%d+)", sv.mapchecks.dm_spawns, MIN_DM_SPAWN_POINTS);

	//
	// music track
	//
	track = (int)qcvm->edicts->v.sounds;
	if (track == 0)
		SV_PrintMapCheck (MAPCHECK_FAILED, "music track (worldspawn \"sounds\" field)");
	else if (track < 2 || track > 255) 
		SV_PrintMapCheck (MAPCHECK_FAILED, "music track (%d, should be between 2 and 255)");
	else
		SV_PrintMapCheck (MAPCHECK_OK, "music track (%d)", track);

	//
	// map title
	//
	Mod_SanitizeMapDescription (buf, sizeof (buf), PR_GetString ((int)qcvm->edicts->v.message));
	if (buf[0])
		SV_PrintMapCheck (MAPCHECK_OK, "map title (%s)", buf);
	else
		SV_PrintMapCheck (MAPCHECK_FAILED, "map title (worldspawn \"message\" field)");

	//
	// warn about multiple skies (e.g. "Slip Tripping" / markiesm1.bsp), of which only the last would get rendered in other engines,
	// and sky textures with non-standard sizes (e.g. "Tears of the False God" / ad_tears.bsp), which could even crash older engines 
	//
	numskies = sv.worldmodel->texofs[(int)TEXTYPE_SKY + 1] - sv.worldmodel->texofs[TEXTYPE_SKY];
	count = 0;
	for (i = 0; i < numskies; i++)
	{
		texture_t *tex = sv.worldmodel->textures[sv.worldmodel->usedtextures[sv.worldmodel->texofs[TEXTYPE_SKY] + i]];
		if (tex->width != 256 || tex->height != 128)
			count++;
	}
	if (numskies > 1 || count > 0)
	{
		SV_PrintMapCheck (MAPCHECK_FAILED, "compat: single %ssky texture (%d found)", count > 0 ? "256 x 128 " : "", numskies);
		for (i = 0; i < numskies; i++)
		{
			texture_t *tex = sv.worldmodel->textures[sv.worldmodel->usedtextures[sv.worldmodel->texofs[TEXTYPE_SKY] + i]];
			if (tex->width != 256 || tex->height != 128)
				q_snprintf (buf, sizeof (buf), " (%d x %d)", tex->width, tex->height);
			else
				buf[0] = '\0';
			Con_SafePrintf ("\x02    %d. %s%s\n", i + 1, tex->name, buf);
		}
	}

	//
	// footer
	//
	Con_SafePrintf ("\n");
	Con_SafePrintf ("=====================================\n");
	Con_SafePrintf ("\n");
}


/*
================
SV_SpawnServer

This is called at the start of each level
================
*/
extern float		scr_centertime_off;
void SV_SpawnServer (const char *server)
{
	static char	dummy[8] = { 0,0,0,0,0,0,0,0 };
	edict_t		*ent;
	int			i, signonsize;
	qcvm_t		*vm = qcvm;

	// let's not have any servers with no name
	if (hostname.string[0] == 0)
		Cvar_Set ("hostname", "UNNAMED");
	scr_centertime_off = 0;

	Con_DPrintf ("SpawnServer: %s\n",server);
	svs.changelevel_issued = false;		// now safe to issue another

	PR_SwitchQCVM(NULL);

//
// tell all connected clients that we are going to a new level
//
	if (sv.active)
	{
		SV_KickBots ();
		SV_SendReconnect ();
	}

//
// make cvars consistant
//
	if (coop.value)
		Cvar_Set ("deathmatch", "0");
	current_skill = (int)(skill.value + 0.5);
	if (current_skill < 0)
		current_skill = 0;
	if (current_skill > 3)
		current_skill = 3;

	Cvar_SetValue ("skill", (float)current_skill);

//
// set up the new server
//
	//memset (&sv, 0, sizeof(sv));
	Host_ClearMemory ();

	q_strlcpy (sv.name, server, sizeof(sv.name));
	if (developer.value || map_checks.value)
		sv.mapchecks.active = true;

	sv.protocol = PROTOCOL_RMQ;
	sv.protocolflags = PROTOCOL_RMQ_FLAGS;

	PR_SwitchQCVM(vm);
// load progs to get entity field count
	PR_LoadProgs ("progs.dat", true);

// allocate server memory
	/* Host_ClearMemory() called above already cleared the whole sv structure */
	qcvm->max_edicts = CLAMP (MIN_EDICTS,(int)max_edicts.value,MAX_EDICTS); //johnfitz -- max_edicts cvar
	qcvm->edicts = (edict_t *) malloc (qcvm->max_edicts*qcvm->edict_size); // ericw -- sv.edicts switched to use malloc()
	if (!qcvm->edicts)
		Sys_Error ("SV_SpawnServer: out of memory (%d edicts x %d bytes)", qcvm->max_edicts, qcvm->edict_size);
	ClearLink (&qcvm->free_edicts);

	sv.datagram.maxsize = sizeof(sv.datagram_buf);
	sv.datagram.cursize = 0;
	sv.datagram.data = sv.datagram_buf;
	sv.datagram.dbg_name = "sv_datagram";

	sv.reliable_datagram.maxsize = sizeof(sv.reliable_datagram_buf);
	sv.reliable_datagram.cursize = 0;
	sv.reliable_datagram.data = sv.reliable_datagram_buf;
	sv.reliable_datagram.dbg_name = "sv_reliable_datagram";

	SV_AddSignonBuffer ();

// leave slots at start for clients only
	qcvm->num_edicts = svs.maxclients+1;
	memset(qcvm->edicts, 0, qcvm->num_edicts*qcvm->edict_size); // ericw -- sv.edicts switched to use malloc()
	for (i=0 ; i<svs.maxclients ; i++)
	{
		ent = EDICT_NUM(i+1);
		svs.clients[i].edict = ent;
	}
	for (i=0 ; i<svs.maxclients ; i++)
	{
		SV_InitClientSnapshotData (&svs.clients[i]);
		SV_ResetClientSnapshot (&svs.clients[i]);
		svs.clients[i].last_cmd_seq = 0;
		svs.clients[i].last_cmd_ack = 0;
		svs.clients[i].invalid_cmd_ack_count = 0;
		svs.clients[i].invalid_cmd_ack_time = 0.0;
	}

	sv.state = ss_loading;
	sv.paused = false;
	sv.nomonsters = (nomonsters.value != 0.f);

	qcvm->time = 1.0;

	q_strlcpy (sv.name, server, sizeof(sv.name));
	q_snprintf (sv.modelname, sizeof(sv.modelname), "maps/%s.bsp", server);
	sv.worldmodel = Mod_ForName (sv.modelname, false);
	if (!sv.worldmodel)
	{
		Con_Printf ("Couldn't spawn server %s\n", sv.modelname);
		sv.active = false;
		return;
	}
	sv.models[1] = sv.worldmodel;

//
// clear world interaction links
//
	SV_ClearWorld ();

	sv.sound_precache[0] = dummy;
	sv.model_precache[0] = dummy;
	sv.model_precache[1] = sv.modelname;
	for (i=1 ; i<sv.worldmodel->numsubmodels ; i++)
	{
		sv.model_precache[1+i] = localmodels[i];
		sv.models[i+1] = Mod_ForName (localmodels[i], false);
	}

//
// load the rest of the entities
//
	ent = EDICT_NUM(0);
	memset (&ent->v, 0, qcvm->progs->entityfields * 4);
	ent->v.model = PR_SetEngineString(sv.worldmodel->name);
	ent->v.modelindex = 1;		// world model
	ent->v.solid = SOLID_BSP;
	ent->v.movetype = MOVETYPE_PUSH;

	if (coop.value)
		pr_global_struct->coop = coop.value;
	else
		pr_global_struct->deathmatch = deathmatch.value;

	pr_global_struct->mapname = PR_SetEngineString(sv.name);

// serverflags are for cross level information (sigils)
	pr_global_struct->serverflags = svs.serverflags;

	ED_LoadFromFile (sv.worldmodel->entities);

	sv.active = true;

// all setup is completed, any further precache statements are errors
	sv.state = ss_active;

// run two frames to allow everything to settle
	host_frametime = 0.1;
	SV_Physics ();
	SV_Physics ();

// create a baseline for more efficient communications
	SV_CreateBaseline ();

	//johnfitz -- warn if signon buffer larger than standard server can handle
	for (i = 0, signonsize = 0; i < sv.num_signon_buffers; i++)
		signonsize += sv.signon_buffers[i]->cursize;
	if (signonsize > 64000-2)
		Con_DWarning ("%i byte signon buffer exceeds QS limit of 63998.\n", signonsize);
	else if (signonsize > 8000-2) //max size that will fit into 8000-sized client->message buffer with 2 extra bytes on the end
		Con_DWarning ("%i byte signon buffer exceeds standard limit of 7998.\n", signonsize);
	//johnfitz

// send serverinfo to all connected clients
	for (i=0,host_client = svs.clients ; i<svs.maxclients ; i++, host_client++)
		if (host_client->active)
			SV_SendServerinfo (host_client);

	Con_DPrintf ("Server spawned.\n");

	if (sv.mapchecks.active)
		SV_PrintMapChecklist ();
}
