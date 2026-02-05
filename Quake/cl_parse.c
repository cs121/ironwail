/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
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
// cl_parse.c  -- parse a message received from the server

#include <string.h>

#include "quakedef.h"
#include "arch_def.h"
#include "net_sys.h"
#include "net_defs.h"
#include "net_dgrm.h"
#include "bgmusic.h"
#include "steam.h"

extern cvar_t cl_netdebug_parse;
extern cvar_t cl_netdebug_hexdump;
extern cvar_t cl_netdebug_dropbad;
extern cvar_t cl_netdebug_maxdump;
extern cvar_t cl_signon_chunk_debug;
extern cvar_t cl_signon_debug;
extern qboolean cl_viewent_needs_init;
extern cvar_t cl_interp;
extern cvar_t cl_jitter;
extern cvar_t cl_updaterate;

static qboolean warn_about_nehahra_protocol;

const char *svc_strings[] =
{
	"svc_bad",
	"svc_nop",
	"svc_disconnect",
	"svc_updatestat",
	"svc_version",		// [long] server version
	"svc_setview",		// [short] entity number
	"svc_sound",			// <see code>
	"svc_time",			// [float] server time
	"svc_print",			// [string] null terminated string
	"svc_stufftext",		// [string] stuffed into client's console buffer
						// the string should be \n terminated
	"svc_setangle",		// [vec3] set the view angle to this absolute value

	"svc_serverinfo",		// [long] version
						// [string] signon string
						// [string]..[0]model cache [string]...[0]sounds cache
						// [string]..[0]item cache
	"svc_lightstyle",		// [byte] [string]
	"svc_updatename",		// [byte] [string]
	"svc_updatefrags",	// [byte] [short]
	"svc_clientdata",		// <shortbits + data>
	"svc_stopsound",		// <see code>
	"svc_updatecolors",	// [byte] [byte]
	"svc_particle",		// [vec3] <variable>
	"svc_damage",			// [byte] impact [byte] blood [vec3] from

	"svc_spawnstatic",
	"OBSOLETE svc_spawnbinary",
	"svc_spawnbaseline",

	"svc_temp_entity",		// <variable>
	"svc_setpause",
	"svc_signonnum",
	"svc_centerprint",
	"svc_killedmonster",
	"svc_foundsecret",
	"svc_spawnstaticsound",
	"svc_intermission",
	"svc_finale",			// [string] music [string] text
	"svc_cdtrack",			// [byte] track [byte] looptrack
	"svc_sellscreen",
	"svc_cutscene",
//johnfitz -- new server messages
	"",	// 35
	"",	// 36
	"svc_skybox", // 37					// [string] skyname
	"svc_botchat", // 38 (2021 RE-RELEASE)
	"", // 39
	"svc_bf", // 40						// no data
	"svc_fog", // 41					// [byte] density [byte] red [byte] green [byte] blue [float] time
	"svc_spawnbaseline2", //42			// support for large modelindex, large framenum, alpha, using flags
	"svc_spawnstatic2", // 43			// support for large modelindex, large framenum, alpha, using flags
	"svc_spawnstaticsound2", //	44		// [coord3] [short] samp [byte] vol [byte] aten
//johnfitz

// 2021 RE-RELEASE:
	"svc_setviews", // 45
	"svc_updateping", // 46
	"svc_updatesocial", // 47
	"svc_updateplinfo", // 48
	"svc_rawprint", // 49
	"svc_servervars", // 50
	"svc_seq", // 51
	"svc_achievement", // 52
	"svc_chat", // 53
	"svc_levelcompleted", // 54
	"svc_backtolobby", // 55
	"svc_localsound", // 56
	"svc_snapshot_full", // 57
	"svc_snapshot_delta", // 58
	"svc_snapshot2", // 59
	"svc_signon_chunk" // 60
};
#define NUM_SVC_STRINGS Q_COUNTOF(svc_strings)

static void CL_RequestFullSnapshot (const char *reason, qboolean count_parse_error);
static void CL_ApplySnapshotOrigin (vec3_t out, const vec3_t in);
static void CL_ApplyEntityOrigin (entity_t *ent, int entnum);
static void CL_ResetSnapshotChunkAssembly (cl_snapshot_chunk_asm_t *chunk);
static qboolean CL_HasActiveSnapshotChunks (void);

static const char *CL_SvcName (int cmd)
{
	if (cmd >= 0 && cmd < (int)NUM_SVC_STRINGS)
		return svc_strings[cmd];
	return "svc_unknown";
}

static void CL_NetHexDump (const byte *data, int len, int max)
{
	int i;
	int dump_len = q_min(len, max);

	if (dump_len <= 0)
		return;

	Con_Printf ("NETDBG: hexdump %d bytes\n", dump_len);
	JITTER_LOG ("NETDBG: hexdump %d bytes\n", dump_len);
	for (i = 0; i < dump_len; i += 16)
	{
		int j;
		char line[128];
		char *out = line;
		int line_len = q_min(16, dump_len - i);

		out += q_snprintf (out, sizeof(line), "NETDBG: %04x:", i);
		for (j = 0; j < line_len; j++)
			out += q_snprintf (out, (size_t)(line + sizeof(line) - out), " %02x", data[i + j]);
		*out = '\0';
		Con_Printf ("%s\n", line);
		JITTER_LOG ("%s\n", line);
	}
}

static int CL_NetDebugDumpLength (void)
{
	int maxdump = (int)cl_netdebug_maxdump.value;

	if (maxdump <= 0)
		return 0;
	if (maxdump > 64)
		maxdump = 64;
	if (maxdump > net_message.cursize)
		maxdump = net_message.cursize;
	return maxdump;
}

static int CL_NetDebugReadcount (void)
{
	int readcount = msg_readcount;

	if (readcount < 0)
		readcount = 0;
	if (readcount > net_message.cursize)
		readcount = net_message.cursize;

	return readcount;
}

static void CL_NetDebugHeader (void)
{
	if (!cl_netdebug_parse.value && !cl_netdebug_hexdump.value)
		return;

	Con_Printf ("NETDBG: msg len %d time %.3f state %d signon %d\n",
		net_message.cursize, realtime, cls.state, cls.signon);
	JITTER_LOG ("NETDBG: msg len %d time %.3f state %d signon %d\n",
		net_message.cursize, realtime, cls.state, cls.signon);
	if (cls.netcon)
	{
		double now = Sys_DoubleTime ();
		double last_msg_age = 0.0;
		double last_send_age = 0.0;

		if (cls.netcon->lastMessageTime > 0.0)
			last_msg_age = now - cls.netcon->lastMessageTime;
		if (cls.netcon->lastSendTime > 0.0)
			last_send_age = now - cls.netcon->lastSendTime;

		Con_Printf ("NETDBG: from %s rseq %u sseq %u rack %u unrel %u "
			"rmask 0x%x sbase %u rate_budget %d can_send %d disc %d "
			"lastmsg %.3f lastsend %.3f\n",
			NET_QSocketGetAddressString (cls.netcon),
			cls.netcon->receiveSequence,
			cls.netcon->sendSequence,
			cls.netcon->reliableReceiveSequence,
			cls.netcon->unreliableReceiveSequence,
			cls.netcon->reliableReceiveMask,
			cls.netcon->sendReliableBase,
			cls.netcon->rate_budget,
			cls.netcon->canSend ? 1 : 0,
			cls.netcon->disconnected ? 1 : 0,
			last_msg_age, last_send_age);
		JITTER_LOG ("NETDBG: from %s rseq %u sseq %u rack %u unrel %u "
			"rmask 0x%x sbase %u rate_budget %d can_send %d disc %d "
			"lastmsg %.3f lastsend %.3f\n",
			NET_QSocketGetAddressString (cls.netcon),
			cls.netcon->receiveSequence,
			cls.netcon->sendSequence,
			cls.netcon->reliableReceiveSequence,
			cls.netcon->unreliableReceiveSequence,
			cls.netcon->reliableReceiveMask,
			cls.netcon->sendReliableBase,
			cls.netcon->rate_budget,
			cls.netcon->canSend ? 1 : 0,
			cls.netcon->disconnected ? 1 : 0,
			last_msg_age, last_send_age);
	}
	if (net_last_incoming.valid)
	{
		Con_Printf ("NETDBG: packet seq %u flags 0x%x len %u payload %u %s\n",
			net_last_incoming.sequence,
			net_last_incoming.flags,
			net_last_incoming.packet_length,
			net_last_incoming.payload_length,
			net_last_incoming.unreliable ? "unreliable" : "reliable");
		JITTER_LOG ("NETDBG: packet seq %u flags 0x%x len %u payload %u %s\n",
			net_last_incoming.sequence,
			net_last_incoming.flags,
			net_last_incoming.packet_length,
			net_last_incoming.payload_length,
			net_last_incoming.unreliable ? "unreliable" : "reliable");
	}
	if (cl_netdebug_hexdump.value)
		CL_NetHexDump (net_message.data, net_message.cursize, CL_NetDebugDumpLength ());
}

static double cl_badmsg_next_warn_time = 0.0;

static void CL_NetHexDumpTail (const byte *data, int len, int tail)
{
	int dump_len = q_min (tail, len);
	int start = len - dump_len;
	int i;
	char line[256];
	char *out = line;
	char *line_end = line + sizeof(line);

	if (dump_len <= 0)
		return;

	Con_Printf ("NETDBG: tail %d bytes (cursize %d):", dump_len, len);
	out += q_snprintf (out, (size_t)(line_end - out), "NETDBG: tail %d bytes (cursize %d):", dump_len, len);
	for (i = 0; i < dump_len; i++)
	{
		Con_Printf (" %02x", data[start + i]);
		out += q_snprintf (out, (size_t)(line_end - out), " %02x", data[start + i]);
	}
	Con_Printf ("\n");
	out += q_snprintf (out, (size_t)(line_end - out), "\n");
	JITTER_LOG ("%s", line);
}

static void CL_BadServerMessage (const char *reason, int cmd, int cmd_offset,
	int lastcmd, int lastcmd_offset, int prevcmd, int prevcmd_offset)
{
	if (cl_netdebug_parse.value || cl_netdebug_hexdump.value)
	{
		Con_Printf ("NETDBG: bad server message: %s\n", reason);
		JITTER_LOG ("NETDBG: bad server message: %s\n", reason);
		Con_Printf ("NETDBG: readcount %d/%d cmd %d (%s) @%d last %d (%s) @%d prev %d (%s) @%d badread %d\n",
			msg_readcount, net_message.cursize,
			cmd, CL_SvcName (cmd), cmd_offset,
			lastcmd, CL_SvcName (lastcmd), lastcmd_offset,
			prevcmd, CL_SvcName (prevcmd), prevcmd_offset,
			msg_badread);
		JITTER_LOG ("NETDBG: readcount %d/%d cmd %d (%s) @%d last %d (%s) @%d prev %d (%s) @%d badread %d\n",
			msg_readcount, net_message.cursize,
			cmd, CL_SvcName (cmd), cmd_offset,
			lastcmd, CL_SvcName (lastcmd), lastcmd_offset,
			prevcmd, CL_SvcName (prevcmd), prevcmd_offset,
			msg_badread);
		if (cl_netdebug_hexdump.value)
			CL_NetHexDump (net_message.data, net_message.cursize, CL_NetDebugDumpLength ());
	}

	if (cls.signon < SIGNONS)
	{
		Con_Printf ("NETDBG: disconnecting due to parse error: %s\n", reason);
		JITTER_LOG ("NETDBG: disconnecting due to parse error: %s\n", reason);
		Con_Printf ("NETDBG: last svc %d (%s) @%d readpos %d cursize %d state %d signon %d\n",
			cmd, CL_SvcName (cmd), cmd_offset, msg_readcount, net_message.cursize,
			(int)cls.state, (int)cls.signon);
		JITTER_LOG ("NETDBG: last svc %d (%s) @%d readpos %d cursize %d state %d signon %d\n",
			cmd, CL_SvcName (cmd), cmd_offset, msg_readcount, net_message.cursize,
			(int)cls.state, (int)cls.signon);
		CL_NetHexDumpTail (net_message.data, net_message.cursize, 64);
		Con_Printf ("Malformed server message during signon: %s\n", reason);
		CL_Disconnect ();
		return;
	}

	if (realtime >= cl_badmsg_next_warn_time)
	{
		Con_Printf ("Dropping malformed server message: %s\n", reason);
		cl_badmsg_next_warn_time = realtime + 1.0;
	}
	CL_RequestFullSnapshot (reason, true);
}

static void CL_ParseSignonRecord (const byte *payload, int payload_len)
{
	sizebuf_t saved_message = net_message;
	int saved_readcount = msg_readcount;
	qboolean saved_badread = msg_badread;
	int saved_readbitpos = msg_readbitpos;
	sizebuf_t record_message;

	record_message.data = (byte *)payload;
	record_message.cursize = payload_len;
	record_message.maxsize = payload_len;
	record_message.allowoverflow = false;
	record_message.overflowed = false;
	record_message.bitpos = 0;

	net_message = record_message;
	CL_ParseServerMessage ();

	net_message = saved_message;
	msg_readcount = saved_readcount;
	msg_badread = saved_badread;
	msg_readbitpos = saved_readbitpos;
}

static void CL_ResetSignonFragment (void)
{
	if (cl.signon_frag_buf)
	{
		Z_Free (cl.signon_frag_buf);
		cl.signon_frag_buf = NULL;
	}
	cl.signon_frag_id = 0;
	cl.signon_frag_total = 0;
	cl.signon_frag_offset = 0;
}

static void CL_ParseSignonChunkPayload (const byte *payload, int payload_len, byte stage, unsigned short seq)
{
	int offset = 0;
	int records = 0;

	while (offset < payload_len)
	{
		int rec_type;
		unsigned short rec_id;

		if (offset + 1 > payload_len)
			Host_Error ("Signon chunk record truncated");

		rec_type = payload[offset++];
		if (rec_type == SIGNON_REC_RAW)
		{
			unsigned short len;
			if (offset + 4 > payload_len)
				Host_Error ("Signon record header truncated");
			rec_id = (unsigned short)LittleShort(*(short *)(payload + offset));
			offset += 2;
			len = (unsigned short)LittleShort(*(short *)(payload + offset));
			offset += 2;
			if (offset + len > payload_len)
				Host_Error ("Signon record length exceeds chunk payload");
			if (cl.signon_frag_buf)
				Host_Error ("Signon record %u arrived while fragment %u incomplete", rec_id, cl.signon_frag_id);
			CL_ParseSignonRecord (payload + offset, len);
			offset += len;
			records++;
			continue;
		}

		if (rec_type == SIGNON_REC_RAW_FRAG)
		{
			unsigned short total_len;
			unsigned short frag_offset;
			unsigned short frag_len;
			if (offset + 8 > payload_len)
				Host_Error ("Signon fragment header truncated");
			rec_id = (unsigned short)LittleShort(*(short *)(payload + offset));
			offset += 2;
			total_len = (unsigned short)LittleShort(*(short *)(payload + offset));
			offset += 2;
			frag_offset = (unsigned short)LittleShort(*(short *)(payload + offset));
			offset += 2;
			frag_len = (unsigned short)LittleShort(*(short *)(payload + offset));
			offset += 2;
			if (offset + frag_len > payload_len)
				Host_Error ("Signon fragment length exceeds chunk payload");
			if (total_len == 0)
				Host_Error ("Signon fragment total length is zero for record %u", rec_id);
			if (!cl.signon_frag_buf || cl.signon_frag_id != rec_id)
			{
				CL_ResetSignonFragment ();
				cl.signon_frag_buf = (byte *)Z_Malloc (total_len);
				cl.signon_frag_id = rec_id;
				cl.signon_frag_total = total_len;
				cl.signon_frag_offset = 0;
			}
			if (total_len != cl.signon_frag_total)
				Host_Error ("Signon fragment total length mismatch for record %u", rec_id);
			if (frag_offset != cl.signon_frag_offset)
				Host_Error ("Signon fragment offset mismatch for record %u (expected %u got %u)",
					rec_id, cl.signon_frag_offset, frag_offset);
			if (frag_offset + frag_len > total_len)
				Host_Error ("Signon fragment exceeds record length for record %u", rec_id);

			memcpy (cl.signon_frag_buf + frag_offset, payload + offset, frag_len);
			offset += frag_len;
			cl.signon_frag_offset = frag_offset + frag_len;
			if (cl.signon_frag_offset == cl.signon_frag_total)
			{
				CL_ParseSignonRecord (cl.signon_frag_buf, cl.signon_frag_total);
				CL_ResetSignonFragment ();
				records++;
			}
			continue;
		}

		Host_Error ("Signon chunk record type %d unknown", rec_type);
	}

	if (cl_signon_debug.value)
		Con_Printf ("SIGNON: stage %u seq %u records %d payload %d\n",
			stage, seq, records, payload_len);
}

extern vec3_t	v_punchangles[2]; //johnfitz

//=============================================================================

/*
===============
CL_EntityNum

This error checks and tracks the total number of entities
===============
*/
entity_t	*CL_EntityNum (int num)
{
	//johnfitz -- check minimum number too
	if (num < 0)
		Host_Error ("CL_EntityNum: %i is an invalid number",num);
	//john

	if (num >= cl.num_entities)
	{
		if (num >= cl_max_edicts) //johnfitz -- no more MAX_EDICTS
			Host_Error ("CL_EntityNum: %i is an invalid number",num);
		while (cl.num_entities<=num)
		{
			cl_entities[cl.num_entities].colormap = vid.colormap;
			cl_entities[cl.num_entities].lerpflags |= LERP_RESETMOVE|LERP_RESETANIM; //johnfitz
			cl_entities[cl.num_entities].baseline.scale = ENTSCALE_DEFAULT;
			cl.num_entities++;
		}
	}

	return &cl_entities[num];
}


/*
==================
CL_ParseStartSoundPacket
==================
*/
void CL_ParseStartSoundPacket(void)
{
	vec3_t	pos;
	int	channel, ent;
	int	sound_num;
	int	volume;
	int	field_mask;
	float	attenuation;
	int	i;

	field_mask = MSG_ReadByte();

	if (field_mask & SND_VOLUME)
		volume = MSG_ReadByte ();
	else
		volume = DEFAULT_SOUND_PACKET_VOLUME;

	if (field_mask & SND_ATTENUATION)
		attenuation = MSG_ReadByte () / 64.0;
	else
		attenuation = DEFAULT_SOUND_PACKET_ATTENUATION;

	if (field_mask & SND_LARGEENTITY)
	{
		ent = (unsigned short) MSG_ReadShort ();
		channel = MSG_ReadByte ();
	}
	else
	{
		channel = (unsigned short) MSG_ReadShort ();
		ent = channel >> 3;
		channel &= 7;
	}

	if (field_mask & SND_LARGESOUND)
		sound_num = (unsigned short) MSG_ReadShort ();
	else
		sound_num = MSG_ReadByte ();

	if (sound_num >= MAX_SOUNDS)
		Host_Error ("CL_ParseStartSoundPacket: %i > MAX_SOUNDS", sound_num);

	if (ent > cl_max_edicts) //johnfitz -- no more MAX_EDICTS
		Host_Error ("CL_ParseStartSoundPacket: ent = %i", ent);

	for (i = 0; i < 3; i++)
		pos[i] = MSG_ReadCoord (cl.protocolflags);

	S_StartSound (ent, channel, cl.sound_precache[sound_num], pos, volume/255.0, attenuation);
}

/*
==================
CL_ParseLocalSound - for 2021 rerelease
==================
*/
void CL_ParseLocalSound(void)
{
	int field_mask, sound_num;

	field_mask = MSG_ReadByte();
	sound_num = (field_mask&SND_LARGESOUND) ? MSG_ReadShort() : MSG_ReadByte();
	if (sound_num >= MAX_SOUNDS)
		Host_Error ("CL_ParseLocalSound: %i > MAX_SOUNDS", sound_num);

	S_LocalSound (cl.sound_precache[sound_num]->name);
}

/*
==================
CL_KeepaliveMessage

When the client is taking a long time to load stuff, send keepalive messages
so the server doesn't disconnect.
==================
*/
static byte	net_olddata[NET_MAXMESSAGE];
void CL_KeepaliveMessage (void)
{
	float	time;
	static float lastmsg;
	int		ret;
	sizebuf_t	old;
	byte	*olddata;

	if (sv.active)
		return;		// no need if server is local
	if (cls.demoplayback)
		return;

// read messages from server, should just be nops
	olddata = net_olddata;
	old = net_message;
	memcpy (olddata, net_message.data, net_message.cursize);

	do
	{
		ret = CL_GetMessage ();
		switch (ret)
		{
		default:
			Host_Error ("CL_KeepaliveMessage: CL_GetMessage failed");
		case 0:
			break;	// nothing waiting
		case 1:
			Host_Error ("CL_KeepaliveMessage: received a message");
			break;
		case 2:
			if (MSG_ReadByte() != svc_nop)
				Host_Error ("CL_KeepaliveMessage: datagram wasn't a nop");
			break;
		}
	} while (ret);

	net_message = old;
	memcpy (net_message.data, olddata, net_message.cursize);

// check time
	time = Sys_DoubleTime ();
	if (time - lastmsg < 5)
		return;
	lastmsg = time;

// write out a nop
	Con_Printf ("--> client to server keepalive\n");

	MSG_WriteByte (&cls.message, clc_nop);
	NET_SendMessage (cls.netcon, &cls.message);
	SZ_Clear (&cls.message);
}

/*
==================
CL_ParseServerInfo
==================
*/
void CL_ParseServerInfo (void)
{
	const char	*str;
	int		i;
	int		nummodels, numsounds;
	char	model_precache[MAX_MODELS][MAX_QPATH];
	char	sound_precache[MAX_SOUNDS][MAX_QPATH];

	Con_DPrintf ("Serverinfo packet received.\n");

	Host_BeginAssetLoading ();

// ericw -- bring up loading plaque for map changes within a demo.
//          it will be hidden in CL_SignonReply.
	if (cls.demoplayback)
		SCR_BeginLoadingPlaque();

//
// wipe the client_state_t struct
//
	CL_ClearState ();

// parse protocol version number
	i = MSG_ReadLong ();
	if (i != PROTOCOL_RMQ)
	{
		Con_Printf ("\n"); //because there's no newline after serverinfo print
		Host_Error ("Server returned protocol %i, expected %i", i, PROTOCOL_RMQ);
	}
	cl.protocol = PROTOCOL_RMQ;

	cl.protocolflags = (unsigned int) MSG_ReadLong ();
	if (cl.protocolflags != PROTOCOL_RMQ_FLAGS)
		Host_Error ("Server protocol flags 0x%x do not match required flags 0x%x", cl.protocolflags, PROTOCOL_RMQ_FLAGS);
	cl.signon_chunking = true;
	cl.signon_chunk_stage = SIGNON_STAGE_PRECACHES;
	cl.signon_chunk_next_seq = 0;
	CL_ResetSignonFragment ();

// parse maxclients
	cl.maxclients = MSG_ReadByte ();
	if (cl.maxclients < 1 || cl.maxclients > MAX_SCOREBOARD)
	{
		Host_Error ("Bad maxclients (%u) from server", cl.maxclients);
	}
	cl.scores = (scoreboard_t *) Hunk_AllocName (cl.maxclients*sizeof(*cl.scores), "scores");

// parse gametype
	cl.gametype = MSG_ReadByte ();

// parse signon message
	str = MSG_ReadString ();
	q_strlcpy (cl.levelname, str, sizeof(cl.levelname));

// seperate the printfs so the server message can have a color
	Con_Printf ("\n%s\n", Con_Quakebar(40)); //johnfitz
	Con_Printf ("%c%s\n", 2, str);

	Con_Printf ("Using protocol %i\n", cl.protocol);

// first we go through and touch all of the precache data that still
// happens to be in the cache, so precaching something else doesn't
// needlessly purge it

// precache models
	memset (cl.model_precache, 0, sizeof(cl.model_precache));
	for (nummodels = 1 ; ; nummodels++)
	{
		str = MSG_ReadString ();
		if (!str[0])
			break;
		if (nummodels == MAX_MODELS)
		{
			Host_Error ("Server sent too many model precaches");
		}
		q_strlcpy (model_precache[nummodels], str, MAX_QPATH);
		Mod_TouchModel (str);
	}

	//johnfitz -- check for excessive models
	if (nummodels >= 2048)
		Con_Warning ("%i models exceeds QS limit of 2048 (max = %d).\n", nummodels, MAX_MODELS);
	else if (nummodels >= 256)
		Con_DWarning ("%i models exceeds standard limit of 256 (max = %d).\n", nummodels, MAX_MODELS);
	//johnfitz

// precache sounds
	memset (cl.sound_precache, 0, sizeof(cl.sound_precache));
	for (numsounds = 1 ; ; numsounds++)
	{
		str = MSG_ReadString ();
		if (!str[0])
			break;
		if (numsounds == MAX_SOUNDS)
		{
			Host_Error ("Server sent too many sound precaches");
		}
		q_strlcpy (sound_precache[numsounds], str, MAX_QPATH);
		S_TouchSound (str);
	}

	//johnfitz -- check for excessive sounds
	if (numsounds >= 256)
		Con_DWarning ("%i sounds exceeds standard limit of 256 (max = %d).\n", numsounds, MAX_SOUNDS);
	//johnfitz

//
// now we try to load everything else until a cache allocation fails
//

	// copy the naked name of the map file to the cl structure -- O.S
	COM_StripExtension (COM_SkipPath(model_precache[1]), cl.mapname, sizeof(cl.mapname));

	for (i = 1; i < nummodels; i++)
	{
		cl.model_precache[i] = Mod_ForName (model_precache[i], false);
		if (cl.model_precache[i] == NULL)
		{
			Host_Error ("Model %s not found", model_precache[i]);
		}
		CL_KeepaliveMessage ();
	}

	S_BeginPrecaching ();
	for (i = 1; i < numsounds; i++)
	{
		cl.sound_precache[i] = S_PrecacheSound (sound_precache[i]);
		CL_KeepaliveMessage ();
	}
	S_EndPrecaching ();

// local state
	cl_entities[0].model = cl.worldmodel = cl.model_precache[1];

	R_NewMap ();

	//johnfitz -- clear out string; we don't consider identical
	//messages to be duplicates if the map has changed in between
	con_lastcenterstring[0] = 0;
	//johnfitz

	Hunk_Check ();		// make sure nothing is hurt

	noclip_anglehack = false;		// noclip is turned off at start

	warn_about_nehahra_protocol = true; //johnfitz -- warn about nehahra protocol hack once per server connection

	Host_EndAssetLoading ();

//johnfitz -- reset developer stats
	memset(&dev_stats, 0, sizeof(dev_stats));
	memset(&dev_peakstats, 0, sizeof(dev_peakstats));
	memset(&dev_overflows, 0, sizeof(dev_overflows));
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

static qboolean CL_SnapshotOriginIsSane (const vec3_t origin)
{
	const float max_abs = 65536.0f;
	int i;

	for (i = 0; i < 3; i++)
	{
		if (!isfinite (origin[i]) || fabsf (origin[i]) > max_abs)
			return false;
	}
	return true;
}

static int CL_CountMaskBits (unsigned int mask)
{
	int count = 0;

	while (mask)
	{
		count += mask & 1u;
		mask >>= 1;
	}
	return count;
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

// INV-2: keep control/acks from starving behind queued reliable data.
static void CL_EnsureReliableControlSpace (int needed, const char *reason)
{
	if (cls.demoplayback || cls.state != ca_connected)
		return;
	if (cls.message.maxsize - cls.message.cursize >= needed)
		return;

	if (NET_CanSendMessage (cls.netcon) && cls.message.cursize > 0)
	{
		if (NET_SendMessage (cls.netcon, &cls.message) == -1)
			Host_Error ("CL_EnsureReliableControlSpace: lost server connection");
		SZ_Clear (&cls.message);
	}

	if (cls.message.maxsize - cls.message.cursize < needed)
	{
		NET_DebugLogEvent (true,
			"NETDBG time %.3f control_trim reason %s dropped %d needed %d\n",
			realtime, reason ? reason : "unknown", cls.message.cursize, needed);
		SZ_Clear (&cls.message);
	}
}

static unsigned int CL_GetSnapshotAckSeq (void)
{
	if (!cl.has_full_snapshot)
		return 0;
	// Ack the last applied COMPLETE snapshot baseline (never incomplete/buffered).
	return cl.snapshot_baseline_seq;
}

static int CL_FindSnapshotBaselineIndex (unsigned int seq)
{
	int i;

	if (!seq)
		return -1;

	for (i = 0; i < CL_SNAPSHOT_BASELINE_HISTORY; i++)
	{
		if (!cl.snapshot_baseline_valid[i])
			continue;
		if (cl.snapshot_baseline_seqs[i] == seq)
			return i;
	}

	return -1;
}

static void CL_SetSnapshotBaselineIndex (int index)
{
	cl.snapshot_baseline_index = index;
	if (index >= 0 && index < CL_SNAPSHOT_BASELINE_HISTORY)
	{
		cl.snapshot_baseline = cl.snapshot_baselines[index];
		cl.snapshot_present = cl.snapshot_baseline_present[index];
		cl.snapshot_baseline_seq = cl.snapshot_baseline_seqs[index];
		return;
	}
	cl.snapshot_baseline = NULL;
	cl.snapshot_present = NULL;
	cl.snapshot_baseline_seq = 0;
}

static int CL_AllocSnapshotBaselineSlot (unsigned int seq)
{
	int index = cl.snapshot_baseline_head;
	int next_index;

	if (index == cl.snapshot_baseline_index && cl.snapshot_baseline_valid[index])
		index = (index + 1) % CL_SNAPSHOT_BASELINE_HISTORY;
	next_index = (index + 1) % CL_SNAPSHOT_BASELINE_HISTORY;
	cl.snapshot_baseline_head = next_index;
	cl.snapshot_baseline_valid[index] = 1;
	cl.snapshot_baseline_seqs[index] = seq;
	return index;
}

static int CL_StoreSnapshotBaseline (unsigned int seq, const snapshot_state_t *states, const byte *present)
{
	int index = CL_AllocSnapshotBaselineSlot (seq);

	memcpy (cl.snapshot_baselines[index], states, cl_max_edicts * sizeof(snapshot_state_t));
	memcpy (cl.snapshot_baseline_present[index], present, cl_max_edicts * sizeof(byte));
	CL_SetSnapshotBaselineIndex (index);
	return index;
}

static int CL_StoreSnapshotBaselineFromBase (unsigned int seq, const snapshot_state_t *base_states,
	const byte *base_present)
{
	int i;
	int index = CL_AllocSnapshotBaselineSlot (seq);
	snapshot_state_t *dest_states = cl.snapshot_baselines[index];
	byte *dest_present = cl.snapshot_baseline_present[index];

	memcpy (dest_states, base_states, cl_max_edicts * sizeof(snapshot_state_t));
	memcpy (dest_present, base_present, cl_max_edicts * sizeof(byte));

	for (i = 1; i < cl_max_edicts; i++)
	{
		if (cl.snapshot_stage_remove[i])
			dest_present[i] = 0;
		if (!cl.snapshot_stage_present[i])
			continue;
		dest_states[i] = cl.snapshot_stage[i];
		dest_present[i] = 1;
	}

	CL_SetSnapshotBaselineIndex (index);
	return index;
}

static qboolean CL_SendSnapshotAck (unsigned int seq)
{
	if (cls.demoplayback || cls.state != ca_connected)
		return false;
	CL_EnsureReliableControlSpace (1 + 4, "snapshot_ack");
	if (cls.message.maxsize - cls.message.cursize < (1 + 4))
	{
		NET_DebugLogEvent (true,
			"NETDBG time %.3f snap_ack_drop seq %u\n",
			realtime, seq);
		return false;
	}
	MSG_WriteByte (&cls.message, clc_snapshot_ack);
	MSG_WriteLong (&cls.message, (int)seq);
	cl.last_snapshot_ack_sent = seq;
	NET_DebugLogEvent (true,
		"NETDBG time %.3f snap_ack_send seq %u\n",
		realtime, seq);
	if (cl_snap_debug.value >= 1.0f)
	{
		NET_DebugLogEvent (false,
			"NETDBG time %.3f snap_ack_status seq %u last_complete %u\n",
			realtime, seq, cl.snapshot_baseline_seq);
	}
	return true;
}

static void CL_GetSnapshotHistoryRange (unsigned int *out_min, unsigned int *out_max, int *out_count)
{
	unsigned int min_seq = 0;
	unsigned int max_seq = 0;
	int count = 0;
	int i;

	for (i = 0; i < CL_SNAPSHOT_BASELINE_HISTORY; i++)
	{
		if (!cl.snapshot_baseline_valid[i])
			continue;
		if (count == 0)
		{
			min_seq = cl.snapshot_baseline_seqs[i];
			max_seq = cl.snapshot_baseline_seqs[i];
		}
		else
		{
			min_seq = q_min (min_seq, cl.snapshot_baseline_seqs[i]);
			max_seq = q_max (max_seq, cl.snapshot_baseline_seqs[i]);
		}
		count++;
	}

	if (out_min)
		*out_min = min_seq;
	if (out_max)
		*out_max = max_seq;
	if (out_count)
		*out_count = count;
}

static qboolean CL_SnapshotReasonIsBaseMismatch (const char *reason)
{
	if (!reason)
		return false;
	return strstr (reason, "base mismatch")
		|| strstr (reason, "invalid base")
		|| strstr (reason, "missing base");
}

static void CL_RequestFullSnapshot (const char *reason, qboolean count_parse_error)
{
	unsigned int ack_seq;
	unsigned int hist_min = 0;
	unsigned int hist_max = 0;
	int hist_count = 0;

	if (cls.demoplayback || cls.state != ca_connected)
		return;
	if (count_parse_error)
	{
		cl.snap_parse_errors++;
		cl.snap_parse_consecutive++;
		cl.snap_decode_error_count++;
		if (cl.snap_parse_consecutive >= MAX_SNAPSHOT_PARSE_ERRORS)
		{
			Host_EndGame ("Too many malformed packets\n");
			return;
		}
	}
	else if (CL_SnapshotReasonIsBaseMismatch (reason))
	{
		cl.snap_base_mismatch_count++;
	}
	NET_DebugLogEvent (true,
		"NETDBG time %.3f snap_abort reason %s parse_errors %d need_full %d\n",
		realtime, reason ? reason : "unknown", cl.snap_parse_errors, cl.need_full_snapshot ? 1 : 0);
	CL_GetSnapshotHistoryRange (&hist_min, &hist_max, &hist_count);
	NET_DebugLogEvent (true,
		"NETDBG time %.3f snap_full_request reason %s base_wanted %u history %u-%u count %d\n",
		realtime, reason ? reason : "unknown", cl.snapshot_baseline_seq,
		hist_min, hist_max, hist_count);
	if (!cl.need_full_snapshot)
	{
		cl.need_full_snapshot = true;
		if (cl_snap_debug.value >= 1.0f && reason)
			Con_Printf ("cl_snap request full: %s\n", reason);
		ack_seq = CL_GetSnapshotAckSeq ();
		(void)CL_SendSnapshotAck (ack_seq);
	}
}

static void CL_SendSnapshotNak (unsigned int expected_base, unsigned int received_base)
{
	if (cls.demoplayback || cls.state != ca_connected)
		return;
	CL_EnsureReliableControlSpace (1 + 4 + 4, "snapshot_nak");
	if (cls.message.maxsize - cls.message.cursize < (1 + 4 + 4))
	{
		NET_DebugLogEvent (true,
			"NETDBG time %.3f snap_nak_drop expected %u received %u\n",
			realtime, expected_base, received_base);
		return;
	}
	MSG_WriteByte (&cls.message, clc_snapshot_nak);
	MSG_WriteLong (&cls.message, (int)expected_base);
	MSG_WriteLong (&cls.message, (int)received_base);
	NET_DebugLogEvent (true,
		"NETDBG time %.3f snap_nak expected %u received %u\n",
		realtime, expected_base, received_base);
}

static float CL_ReadSnapshotCoord (qboolean hires)
{
	if (hires && (cl.protocolflags & PRFL_SNAPSHOT_HIRES))
		return MSG_ReadFloat ();
	return MSG_ReadCoord (cl.protocolflags);
}

static float CL_ReadSnapshotAngle (qboolean hires)
{
	if (hires && (cl.protocolflags & PRFL_SNAPSHOT_HIRES))
		return MSG_ReadFloat ();
	return MSG_ReadAngle (cl.protocolflags);
}

static void CL_ReadSnapshotState (snapshot_state_t *state, unsigned int snapflags)
{
	int i;
	qboolean hires_origin = (snapflags & SNAPFL_HIRES_ORIGIN) != 0;
	qboolean hires_angles = (snapflags & SNAPFL_HIRES_ANGLES) != 0;

	for (i = 0; i < 3; i++)
	{
		state->state.origin[i] = CL_ReadSnapshotCoord (hires_origin);
		state->state.angles[i] = CL_ReadSnapshotAngle (hires_angles);
	}
	state->state.modelindex = (unsigned short)MSG_ReadShort ();
	state->state.frame = (unsigned short)MSG_ReadShort ();
	state->state.colormap = (unsigned char)MSG_ReadByte ();
	state->state.skin = (unsigned char)MSG_ReadByte ();
	state->state.effects = MSG_ReadByte ();
	state->state.alpha = (unsigned char)MSG_ReadByte ();
	state->state.scale = (unsigned char)MSG_ReadByte ();
	state->step = (byte)MSG_ReadByte ();
	state->health = (short)MSG_ReadShort ();
	state->armor = (short)MSG_ReadShort ();
	state->ammo = (short)MSG_ReadShort ();
	state->weaponmodel = (unsigned short)MSG_ReadShort ();
	state->weaponframe = (unsigned short)MSG_ReadShort ();
	state->items = (unsigned int)MSG_ReadLong ();
	state->activeweapon = (unsigned int)MSG_ReadLong ();
	state->ammo_shells = (byte)MSG_ReadByte ();
	state->ammo_nails = (byte)MSG_ReadByte ();
	state->ammo_rockets = (byte)MSG_ReadByte ();
	state->ammo_cells = (byte)MSG_ReadByte ();
	state->viewheight = (char)MSG_ReadChar ();
	state->idealpitch = (char)MSG_ReadChar ();
	for (i = 0; i < 3; i++)
		state->punchangle[i] = (char)MSG_ReadChar ();
	for (i = 0; i < 3; i++)
		state->velocity[i] = (char)MSG_ReadChar ();
	state->movetype = (byte)MSG_ReadByte ();
	state->pm_flags = (byte)MSG_ReadByte ();
	state->waterlevel = (byte)MSG_ReadByte ();
	state->watertype = (byte)MSG_ReadByte ();
	state->event = (byte)MSG_ReadByte ();
	state->event_param = (byte)MSG_ReadByte ();
	state->event_seq = (unsigned short)MSG_ReadShort ();
}

static void CL_ReadSnapshotDeltaFields (snapshot_state_t *state, unsigned int mask)
{
	qboolean hires_origin = (mask & SNAP_HIRES_ORIGIN) != 0;
	qboolean hires_angles = (mask & SNAP_HIRES_ANGLES) != 0;
	mask &= ~(SNAP_HIRES_ORIGIN | SNAP_HIRES_ANGLES);

	if (mask & SNAP_ORIGIN1)
		state->state.origin[0] = CL_ReadSnapshotCoord (hires_origin);
	if (mask & SNAP_ORIGIN2)
		state->state.origin[1] = CL_ReadSnapshotCoord (hires_origin);
	if (mask & SNAP_ORIGIN3)
		state->state.origin[2] = CL_ReadSnapshotCoord (hires_origin);
	if (mask & SNAP_ANGLE1)
		state->state.angles[0] = CL_ReadSnapshotAngle (hires_angles);
	if (mask & SNAP_ANGLE2)
		state->state.angles[1] = CL_ReadSnapshotAngle (hires_angles);
	if (mask & SNAP_ANGLE3)
		state->state.angles[2] = CL_ReadSnapshotAngle (hires_angles);
	if (mask & SNAP_MODEL)
		state->state.modelindex = (unsigned short)MSG_ReadShort ();
	if (mask & SNAP_FRAME)
		state->state.frame = (unsigned short)MSG_ReadShort ();
	if (mask & SNAP_COLORMAP)
		state->state.colormap = (unsigned char)MSG_ReadByte ();
	if (mask & SNAP_SKIN)
		state->state.skin = (unsigned char)MSG_ReadByte ();
	if (mask & SNAP_EFFECTS)
		state->state.effects = MSG_ReadByte ();
	if (mask & SNAP_ALPHA)
		state->state.alpha = (unsigned char)MSG_ReadByte ();
	if (mask & SNAP_SCALE)
		state->state.scale = (unsigned char)MSG_ReadByte ();
	if (mask & SNAP_STEP)
		state->step = (byte)MSG_ReadByte ();
	if (mask & SNAP_PLAYERSTATS)
	{
		state->health = (short)MSG_ReadShort ();
		state->armor = (short)MSG_ReadShort ();
		state->ammo = (short)MSG_ReadShort ();
		state->weaponmodel = (unsigned short)MSG_ReadShort ();
		state->weaponframe = (unsigned short)MSG_ReadShort ();
		state->items = (unsigned int)MSG_ReadLong ();
		state->activeweapon = (unsigned int)MSG_ReadLong ();
		state->ammo_shells = (byte)MSG_ReadByte ();
		state->ammo_nails = (byte)MSG_ReadByte ();
		state->ammo_rockets = (byte)MSG_ReadByte ();
		state->ammo_cells = (byte)MSG_ReadByte ();
	}
	if (mask & SNAP_PLAYERMOVE)
	{
		int i;

		state->viewheight = (char)MSG_ReadChar ();
		state->idealpitch = (char)MSG_ReadChar ();
		for (i = 0; i < 3; i++)
			state->punchangle[i] = (char)MSG_ReadChar ();
		for (i = 0; i < 3; i++)
			state->velocity[i] = (char)MSG_ReadChar ();
		state->movetype = (byte)MSG_ReadByte ();
		state->pm_flags = (byte)MSG_ReadByte ();
		state->waterlevel = (byte)MSG_ReadByte ();
		state->watertype = (byte)MSG_ReadByte ();
	}
	if (mask & SNAP_PLAYEREVENTS)
	{
		state->event = (byte)MSG_ReadByte ();
		state->event_param = (byte)MSG_ReadByte ();
		state->event_seq = (unsigned short)MSG_ReadShort ();
	}
}

static void CL_ClearSnapshotStage (void);
static void CL_PrepareFullSnapshotInterpReset (void);
static void CL_FinalizeFullSnapshotInterpReset (void);

static void CL_UpdateServerTimeEstimate (double server_time, const char *source)
{
	double offset;
	double delta;
	double max_step = 0.05;

	if (server_time <= 0.0)
		return;

	if (cl.latest_server_time > 0.0 && server_time + 0.001 < cl.latest_server_time)
	{
		if (cl_netdbg_interp.value > 0.0f)
		{
			Con_Printf ("NETDBG: server_time non-monotonic %.3f < %.3f (%s)\n",
				server_time, cl.latest_server_time, source ? source : "unknown");
			JITTER_LOG ("NETDBG: server_time non-monotonic %.3f < %.3f (%s)\n",
				server_time, cl.latest_server_time, source ? source : "unknown");
		}
		server_time = cl.latest_server_time;
	}

	cl.latest_server_time = server_time;
	cl.server_time_base = server_time;
	cl.server_time_skew = realtime;

	offset = server_time - realtime;
	if (cl.clock_offset == 0.0)
	{
		cl.clock_offset = offset;
		return;
	}

	delta = offset - cl.clock_offset;
	if (delta > max_step)
		delta = max_step;
	else if (delta < -max_step)
		delta = -max_step;

	cl.clock_offset += delta * 0.25;
}

static double CL_ClampSnapshotServerTime (double snap_time, unsigned int seq)
{
	if (snap_time <= 0.0)
		return 0.0;

	if (cl.snap_last_server_time > 0.0 && snap_time + 0.001 < cl.snap_last_server_time)
	{
		if (cl_netdbg_interp.value > 0.0f)
		{
			Con_Printf ("NETDBG: snap_time non-monotonic %.3f < %.3f seq %u\n",
				snap_time, cl.snap_last_server_time, seq);
			JITTER_LOG ("NETDBG: snap_time non-monotonic %.3f < %.3f seq %u\n",
				snap_time, cl.snap_last_server_time, seq);
		}
		snap_time = cl.snap_last_server_time;
	}

	cl.snap_last_server_time = snap_time;
	return snap_time;
}

static void CL_ClearEntitySnapHistory (int entnum)
{
	if (entnum <= 0 || entnum >= cl_max_edicts)
		return;
	if (!cl.entity_snapshots || !cl.entity_snap_head || !cl.entity_snap_count)
		return;
	cl.entity_snap_head[entnum] = 0;
	cl.entity_snap_count[entnum] = 0;
	memset (&cl.entity_snapshots[entnum * CL_ENTITY_SNAP_HISTORY], 0,
		sizeof(cl_entity_snap_t) * CL_ENTITY_SNAP_HISTORY);
}

static void CL_ClearSnapshotEntity (int entnum)
{
	entity_t *ent;

	if (entnum <= 0 || entnum >= cl_max_edicts)
		return;
	ent = &cl_entities[entnum];

	ent->model = NULL;
	ent->effects = 0;
	ent->forcelink = true;
	if (cl.snapshot_active)
		cl.snapshot_active[entnum] = 0;
	if (cl.snapshot_last_update_time)
		cl.snapshot_last_update_time[entnum] = 0;
	if (cl.snapshot_missing_grace_until)
		cl.snapshot_missing_grace_until[entnum] = 0;
	if (cl.snapshot_resync_start_time)
		cl.snapshot_resync_start_time[entnum] = 0;
	if (cl.snapshot_resync_end_time)
		cl.snapshot_resync_end_time[entnum] = 0;
	if (cl.snapshot_resync_from_origin)
		VectorClear (cl.snapshot_resync_from_origin[entnum]);
	if (cl.snapshot_resync_from_angles)
		VectorClear (cl.snapshot_resync_from_angles[entnum]);
	CL_ClearEntitySnapHistory (entnum);
}

static qboolean CL_ShouldDropSnapshot (void)
{
	if (cl_test_drop.value <= 0.0f)
		return false;
	return ((float)rand() / (float)RAND_MAX) < cl_test_drop.value;
}

static qboolean CL_SnapshotWorldModelReady (const char *reason)
{
	if (cl.worldmodel && cl.worldmodel->type == mod_brush)
		return true;

	cl.need_full_snapshot = true;
	cl.has_valid_worldstate = false;
	CL_RequestFullSnapshot (reason, false);
	return false;
}

static void CL_ApplySnapshotOrigin (vec3_t out, const vec3_t in)
{
	// Snapshot origins are model-space; offset by worldmodel origin to render in world-space.
	if (cl.worldmodel && cl.worldmodel->type == mod_brush
		&& cl.worldmodel->submodels && cl.worldmodel->numsubmodels > 0)
		VectorAdd (in, cl.worldmodel->submodels[0].origin, out);
	else
		VectorCopy (in, out);
}

static void CL_InitViewEntityFromSim (void)
{
	entity_t *ent;

	if (!cl_viewent_needs_init)
		return;
	if (!cl_entities || cl.viewentity <= 0 || cl.viewentity >= cl_max_edicts)
		return;

	ent = &cl_entities[cl.viewentity];
	VectorCopy (cl.simorg, ent->origin);
	VectorCopy (cl.simorg, ent->msg_origins[0]);
	VectorCopy (cl.simorg, ent->msg_origins[1]);
	cl_viewent_needs_init = false;
}

static void CL_ApplyEntityOrigin (entity_t *ent, int entnum)
{
	VectorCopy (ent->msg_origins[0], ent->origin);

	if (entnum == cl.viewentity)
	{
		if (!VectorCompare (ent->msg_origins[0], vec3_origin))
			VectorCopy (ent->msg_origins[0], cl.simorg);
		if (VectorCompare (ent->origin, vec3_origin))
			VectorCopy (cl.simorg, ent->origin);
	}
}

static void CL_LogDeltaReject (const char *reason, unsigned int seq, unsigned int base_seq)
{
	NET_DebugLogEvent (true,
		"NETDBG time %.3f delta_reject %s seq %u base %u\n",
		realtime, reason ? reason : "unknown", seq, base_seq);
	if (cl_delta_reject_debug.value < 1.0f)
		return;
	Con_Printf ("cl_delta_reject %s seq %u base %u\n", reason, seq, base_seq);
}

static void CL_CheckSnapshotChunkTimeouts (void)
{
	double timeout_s = cl_full_reasm_timeout_ms.value * 0.001;
	int i;

	if (timeout_s <= 0.0)
		return;

	for (i = 0; i < CL_SNAPSHOT_CHUNK_INFLIGHT; i++)
	{
		cl_snapshot_chunk_asm_t *chunk = &cl.snapshot_chunk_assemblies[i];

		if (!chunk->active)
			continue;
		if ((realtime - chunk->start_time) <= timeout_s)
			continue;
		if (cl_full_reasm_debug.value >= 1.0f)
			Con_Printf ("cl_snap2 full reasm timeout seq %u\n", chunk->seq);
		CL_ResetSnapshotChunkAssembly (chunk);
		CL_RequestFullSnapshot ("snapshot2 full reasm timeout", false);
	}
}

static void CL_TrackSnapshotIncomplete (unsigned short seq16, const char *reason)
{
	double timeout_s = cl_snap_incomplete_timeout_ms.value * 0.001;

	if (!cl.snap_last_incomplete || cl.snap_last_incomplete_seq != seq16)
		cl.snap_incomplete_start_time = realtime;

	cl.snap_last_incomplete = true;
	cl.snap_last_incomplete_seq = seq16;
	cl.snap_incomplete_count++;

	if (timeout_s <= 0.0)
		return;
	if ((realtime - cl.snap_incomplete_start_time) <= timeout_s)
		return;

	if (cl_snap_debug.value >= 1.0f)
	{
		NET_DebugLogEvent (false,
			"NETDBG time %.3f snap2_incomplete_timeout seq %u reason %s\n",
			realtime, (unsigned int)seq16, reason ? reason : "unknown");
	}
	CL_RequestFullSnapshot ("snapshot2 incomplete timeout", false);
	cl.snap_incomplete_start_time = realtime;
	cl.snap_incomplete_count = 0;
}

static void CL_MarkSnapshotEntityUpdated (int entnum)
{
	double stamp = cl.snapshot_time > 0.0 ? cl.snapshot_time : cl.mtime[0];

	if (entnum <= 0 || entnum >= cl_max_edicts)
		return;
	if (cl.snapshot_active)
		cl.snapshot_active[entnum] = 1;
	if (cl.snapshot_last_update_time)
		cl.snapshot_last_update_time[entnum] = stamp;
	if (cl.snapshot_missing_grace_until)
		cl.snapshot_missing_grace_until[entnum] = 0;
}

static void CL_RecordEntitySnap (int entnum, const snapshot_state_t *state)
{
	int base;
	byte head;
	cl_entity_snap_t *snap;

	if (entnum <= 0 || entnum >= cl_max_edicts)
		return;
	if (!cl.entity_snapshots || !cl.entity_snap_head || !cl.entity_snap_count)
		return;

	base = entnum * CL_ENTITY_SNAP_HISTORY;
	head = cl.entity_snap_head[entnum];
	if (cl.entity_snap_count[entnum] == 0)
		head = 0;
	else
		head = (byte)((head + 1) % CL_ENTITY_SNAP_HISTORY);
	cl.entity_snap_head[entnum] = head;
	if (cl.entity_snap_count[entnum] < CL_ENTITY_SNAP_HISTORY)
		cl.entity_snap_count[entnum]++;

	snap = &cl.entity_snapshots[base + head];
	snap->servertime = cl.snapshot_time > 0.0 ? cl.snapshot_time : cl.mtime[0];
	VectorCopy (state->state.origin, snap->origin);
	VectorCopy (state->state.angles, snap->angles);
}

static qboolean CL_IsSnapshotSeqStale (unsigned short seq)
{
	if (cl.snap_last_applied_seq == 0)
		return false;
	return (short)(seq - cl.snap_last_applied_seq) <= 0;
}

static void CL_ApplySnapshotState (int entnum, const snapshot_state_t *state)
{
	entity_t	*ent;
	qboolean	forcelink;
	double		oldtime;
	double		stamp;
	int			i;
	int			skin;
	qmodel_t	*model;

	ent = CL_EntityNum (entnum);
	forcelink = (ent->msgtime != cl.mtime[1]);
	oldtime = ent->msgtime;
	stamp = cl.snapshot_time > 0.0 ? cl.snapshot_time : cl.mtime[0];

	if (oldtime + 0.2 < stamp)
		ent->lerpflags |= LERP_RESETANIM;

	ent->msgtime = stamp;

	VectorCopy (ent->msg_origins[0], ent->msg_origins[1]);
	VectorCopy (ent->msg_angles[0], ent->msg_angles[1]);

	ent->frame = state->state.frame;
	ent->effects = state->state.effects;
	ent->alpha = state->state.alpha;
	ent->scale = state->state.scale;

	i = state->state.colormap;
	if (!i)
		ent->colormap = vid.colormap;
	else
	{
		if (i > cl.maxclients)
			Sys_Error ("CL_ApplySnapshotState: i >= cl.maxclients");
		ent->colormap = cl.scores[i-1].translations;
	}

	skin = state->state.skin;
	if (skin != ent->skinnum)
	{
		ent->skinnum = skin;
		if (entnum > 0 && entnum <= cl.maxclients)
			R_TranslateNewPlayerSkin (entnum - 1);
	}

	CL_ApplySnapshotOrigin (ent->msg_origins[0], state->state.origin);
	VectorCopy (state->state.angles, ent->msg_angles[0]);
	CL_ApplyEntityOrigin (ent, entnum);

	if (state->step)
	{
		ent->lerpflags |= LERP_MOVESTEP;
		ent->forcelink = true;
	}
	else
		ent->lerpflags &= ~LERP_MOVESTEP;

	model = cl.model_precache[state->state.modelindex];
	if (model != ent->model)
	{
		ent->model = model;
		if (model)
		{
			if (model->synctype == ST_FRAMETIME)
				ent->syncbase = -cl.time;
			else if (model->synctype == ST_RAND)
				ent->syncbase = (float)(rand()&0x7fff) / 0x7fff;
			else
				ent->syncbase = 0.0;
		}
		else
			forcelink = true;
		if (entnum > 0 && entnum <= cl.maxclients)
			R_TranslateNewPlayerSkin (entnum - 1);

		ent->lerpflags |= LERP_RESETANIM;
	}
	else if (model && model->synctype == ST_FRAMETIME)
		ent->syncbase = -cl.time;

	if (forcelink)
	{
		VectorCopy (ent->msg_origins[0], ent->msg_origins[1]);
		VectorCopy (ent->msg_origins[0], ent->origin);
		VectorCopy (ent->msg_angles[0], ent->msg_angles[1]);
		VectorCopy (ent->msg_angles[0], ent->angles);
		ent->forcelink = true;
	}

	CL_RecordEntitySnap (entnum, state);
}

static int CL_ApplySnapshotOverlay (const snapshot_state_t *states, const byte *present)
{
	int i;
	int touched = 0;

	if (!states || !present)
		return 0;

	for (i = 1; i < cl_max_edicts; i++)
	{
		if (!present[i])
			continue;
		cl.snapshot_present[i] = 1;
		CL_ApplySnapshotState (i, &states[i]);
		CL_MarkSnapshotEntityUpdated (i);
		touched++;
	}

	return touched;
}

static void CL_LogSnapshotDebug (const char *label, unsigned int seq, unsigned int flags,
	qboolean snap_complete, int removes_parsed, int touched, qboolean base_advanced)
{
	if (cl_snapshot_debug.value < 1.0f)
		return;
	Con_Printf ("cl_snapshot %s seq %u flags 0x%x complete %d removes %d touched %d base_adv %d\n",
		label ? label : "unknown", seq, flags, snap_complete ? 1 : 0,
		removes_parsed, touched, base_advanced ? 1 : 0);
}

static void CL_CountSnapshotEntities (const byte *prev_present, const byte *cur_present,
	int *out_total, int *out_created, int *out_missing, int *out_dormant)
{
	int total = 0;
	int created = 0;
	int missing = 0;
	int dormant = 0;
	int i;
	double dormant_s = cl_entity_dormant_ms.value * 0.001;
	double now = cl.snapshot_time > 0.0 ? cl.snapshot_time : cl.mtime[0];

	if (!cur_present)
	{
		if (out_total)
			*out_total = 0;
		if (out_created)
			*out_created = 0;
		if (out_missing)
			*out_missing = 0;
		if (out_dormant)
			*out_dormant = 0;
		return;
	}

	for (i = 1; i < cl_max_edicts; i++)
	{
		if (cur_present[i])
		{
			total++;
			if (prev_present && !prev_present[i])
				created++;
		}
		else if (prev_present && prev_present[i])
		{
			missing++;
			if (dormant_s > 0.0 && cl.snapshot_last_update_time)
			{
				if (cl.snapshot_last_update_time[i] > 0.0
					&& (now - cl.snapshot_last_update_time[i]) <= dormant_s)
				{
					dormant++;
				}
			}
		}
	}

	if (out_total)
		*out_total = total;
	if (out_created)
		*out_created = created;
	if (out_missing)
		*out_missing = missing;
	if (out_dormant)
		*out_dormant = dormant;
}

typedef struct snapshot_header_s
{
	unsigned int	seq;
	unsigned int	base_seq;
	unsigned int	flags;
	unsigned short	num_entities;
	unsigned short	num_removed;
	unsigned short	chunk_total_entities;
	unsigned short	chunk_index;
	unsigned short	chunk_total_chunks;
	double		server_time;
} snapshot_header_t;

static void CL_NetDbg_LogWatchEnt (const snapshot_header_t *header, int removed)
{
	int watch = (int)cl_netdbg_watch_ent.value;
	cl_entity_snap_t *snaps;
	const cl_entity_snap_t *newest = NULL;
	const cl_entity_snap_t *prev = NULL;
	int count;
	int head;
	const snapshot_state_t *state;
	const char *status = "absent";
	const char *reason = "absent";

	if (watch <= 0 || watch >= cl_max_edicts)
		return;
	if (!cl.snapshot_present)
		return;

	if (removed)
	{
		status = "removed";
		reason = "remove_list";
	}
	else if (cl.snapshot_present[watch])
	{
		status = "present";
		reason = "present";
	}

	state = &cl.snapshot_baseline[watch];
	if (cl.entity_snapshots && cl.entity_snap_head && cl.entity_snap_count)
	{
		count = cl.entity_snap_count[watch];
		head = cl.entity_snap_head[watch];
		snaps = &cl.entity_snapshots[watch * CL_ENTITY_SNAP_HISTORY];
		if (count > 0)
		{
			newest = &snaps[head];
			if (count > 1)
			{
				int prev_idx = (head - 1 + CL_ENTITY_SNAP_HISTORY) % CL_ENTITY_SNAP_HISTORY;
				prev = &snaps[prev_idx];
			}
		}
	}

	Con_Printf ("NETDBG: watch_ent %d seq %u base %u state %s reason %s origin %.1f %.1f %.1f "
		"vel %d %d %d",
		watch, header->seq, header->base_seq, status, reason,
		state->state.origin[0], state->state.origin[1], state->state.origin[2],
		state->velocity[0], state->velocity[1], state->velocity[2]);
	{
		char line[512];
		char *out = line;
		char *line_end = line + sizeof(line);

		out += q_snprintf (out, (size_t)(line_end - out),
			"NETDBG: watch_ent %d seq %u base %u state %s reason %s origin %.1f %.1f %.1f "
			"vel %d %d %d",
			watch, header->seq, header->base_seq, status, reason,
			state->state.origin[0], state->state.origin[1], state->state.origin[2],
			state->velocity[0], state->velocity[1], state->velocity[2]);

	if (newest)
	{
		Con_Printf (" snap0 %.1f %.1f %.1f", newest->origin[0], newest->origin[1], newest->origin[2]);
		out += q_snprintf (out, (size_t)(line_end - out), " snap0 %.1f %.1f %.1f",
			newest->origin[0], newest->origin[1], newest->origin[2]);
	}
	if (prev)
	{
		Con_Printf (" snap1 %.1f %.1f %.1f", prev->origin[0], prev->origin[1], prev->origin[2]);
		out += q_snprintf (out, (size_t)(line_end - out), " snap1 %.1f %.1f %.1f",
			prev->origin[0], prev->origin[1], prev->origin[2]);
	}
	Con_Printf ("\n");
	out += q_snprintf (out, (size_t)(line_end - out), "\n");
	JITTER_LOG ("%s", line);
	}
}

static double CL_GetInterpDelaySecondsDebug (void)
{
	double interp = cl_interp.value;
	double jitter = cl_jitter.value;
	double min_delay = 0.0;
	double base;
	double jitter_scale = 1.0;

	if (interp <= 0.0)
	{
		if (cl_lerp_ms.value <= 0.0)
			return 0.0;
		interp = cl_lerp_ms.value * 0.001;
	}

	if (jitter < 0.0)
		jitter = 0.0;
	if (cl_updaterate.value > 0.0)
		min_delay = 2.0 / cl_updaterate.value;
	base = interp;
	if (min_delay > 0.0 && base < min_delay)
	{
		jitter_scale = interp > 0.0 ? (interp / min_delay) : 0.0;
		base = min_delay;
	}

	return base + (jitter * jitter_scale);
}

static void CL_NetDbg_LogInterpSnapshot (const snapshot_header_t *header, int removes_parsed,
	const byte *prev_present)
{
	double interp_delay;
	double server_now;
	double render_time;
	double arrival_dt = 0.0;
	double oldest = 0.0;
	double newest = 0.0;
	double buffer_fill_ms = 0.0;
	double render_vs_oldest_ms = 0.0;
	double render_vs_newest_ms = 0.0;
	double server_time = 0.0;
	double server_skew_age = 0.0;
	double snap_age = 0.0;
	int psnap_count = 0;
	int total = 0;
	int created = 0;
	int missing = 0;
	int dormant = 0;

	if (cl_netdbg_interp.value <= 0.0f)
		return;

	if (cl.snap_last_arrival_time > 0.0)
		arrival_dt = realtime - cl.snap_last_arrival_time;
	cl.snap_last_arrival_time = realtime;

	interp_delay = CL_GetInterpDelaySecondsDebug ();

	if (cl.clock_offset != 0.0)
		server_now = realtime + cl.clock_offset;
	else if (cl.latest_server_time > 0.0)
		server_now = cl.latest_server_time;
	else
		server_now = cl.mtime[0];

	render_time = server_now - interp_delay;
	server_time = cl.snapshot_time > 0.0 ? cl.snapshot_time : header->server_time;
	if (cl.server_time_skew > 0.0)
		server_skew_age = realtime - cl.server_time_skew;
	if (server_now > 0.0 && server_time > 0.0)
		snap_age = server_now - server_time;

	CL_GetPlayerSnapRange (&oldest, &newest, &psnap_count);
	if (psnap_count > 1)
		buffer_fill_ms = (newest - oldest) * 1000.0;
	if (psnap_count > 0)
	{
		render_vs_oldest_ms = (render_time - oldest) * 1000.0;
		render_vs_newest_ms = (render_time - newest) * 1000.0;
	}

	CL_CountSnapshotEntities (prev_present, cl.snapshot_present, &total, &created, &missing, &dormant);

	Con_Printf ("NETDBG: interp seq %u base %u srvtime %.3f arrival_dt %.3f "
		"render_time %.3f interp_delay %.3f buf_oldest %.3f buf_newest %.3f buf_fill_ms %.1f "
		"render_gap_ms oldest %.1f newest %.1f srvnow %.3f srv_skew_age %.3f snap_age %.3f "
		"ents total %d created %d removed %d missing %d dormant %d "
		"snap_seq last %u complete %u incomplete %u need_full %d delta_mismatch %d parse_err %d incomplete_cnt %d\n",
		header->seq, header->base_seq, server_time,
		arrival_dt, render_time, interp_delay, oldest, newest, buffer_fill_ms,
		render_vs_oldest_ms, render_vs_newest_ms, server_now, server_skew_age, snap_age,
		total, created, removes_parsed, missing, dormant,
		cl.snap_last_applied_seq, cl.snap_last_complete_seq, cl.snap_last_incomplete_seq,
		cl.need_full_snapshot ? 1 : 0, cl.snap_delta_mismatch, cl.snap_parse_errors,
		cl.snap_incomplete_count);
	JITTER_LOG ("NETDBG: interp seq %u base %u srvtime %.3f arrival_dt %.3f "
		"render_time %.3f interp_delay %.3f buf_oldest %.3f buf_newest %.3f buf_fill_ms %.1f "
		"render_gap_ms oldest %.1f newest %.1f srvnow %.3f srv_skew_age %.3f snap_age %.3f "
		"ents total %d created %d removed %d missing %d dormant %d "
		"snap_seq last %u complete %u incomplete %u need_full %d delta_mismatch %d parse_err %d incomplete_cnt %d\n",
		header->seq, header->base_seq, server_time,
		arrival_dt, render_time, interp_delay, oldest, newest, buffer_fill_ms,
		render_vs_oldest_ms, render_vs_newest_ms, server_now, server_skew_age, snap_age,
		total, created, removes_parsed, missing, dormant,
		cl.snap_last_applied_seq, cl.snap_last_complete_seq, cl.snap_last_incomplete_seq,
		cl.need_full_snapshot ? 1 : 0, cl.snap_delta_mismatch, cl.snap_parse_errors,
		cl.snap_incomplete_count);
}

/*
==================
CL_ParseBaseline
==================
*/
void CL_ParseBaseline (entity_t *ent, int version) //johnfitz -- added argument
{
	int	i;
	int bits; //johnfitz

	bits = (version == 2) ? MSG_ReadByte() : 0;
	ent->baseline.modelindex = (bits & B_LARGEMODEL) ? MSG_ReadShort() : MSG_ReadByte();
	ent->baseline.frame = (bits & B_LARGEFRAME) ? MSG_ReadShort() : MSG_ReadByte();

	ent->baseline.colormap = MSG_ReadByte();
	ent->baseline.skin = MSG_ReadByte();
	for (i = 0; i < 3; i++)
	{
		ent->baseline.origin[i] = MSG_ReadCoord (cl.protocolflags);
		ent->baseline.angles[i] = MSG_ReadAngle (cl.protocolflags);
	}

	ent->baseline.alpha = (bits & B_ALPHA) ? MSG_ReadByte() : ENTALPHA_DEFAULT;
	ent->baseline.scale = (bits & B_SCALE) ? MSG_ReadByte() : ENTSCALE_DEFAULT;
}

static void CL_ParseSnapshotFull (void)
{
	unsigned int seq;
	int count;
	int i;
	qboolean drop;
	unsigned short seq16;

	seq = (unsigned int)MSG_ReadLong ();
	count = (unsigned short)MSG_ReadShort ();
	drop = CL_ShouldDropSnapshot ();
	seq16 = (unsigned short)seq;

	if (cl_snap_debug.value >= 1.0f)
		Con_Printf ("cl_snap full seq %u ents %d%s\n", seq, count, drop ? " drop" : "");

	if (msg_badread || count < 0 || count > cl_max_edicts || count > MAX_SNAPSHOT_ENTITIES)
	{
		CL_RequestFullSnapshot ("snapshot_full header", true);
		msg_readcount = net_message.cursize;
		return;
	}

	CL_ClearSnapshotStage ();

	for (i = 0; i < count; i++)
	{
		int entnum;
		snapshot_state_t state;
		unsigned int snapflags = 0;

		entnum = (unsigned short)MSG_ReadShort ();
		if (entnum <= 0 || entnum >= cl_max_edicts)
		{
			CL_RequestFullSnapshot ("snapshot_full entnum", true);
			msg_readcount = net_message.cursize;
			return;
		}
		if (cl.protocolflags & PRFL_SNAPSHOT_HIRES)
			snapflags = (unsigned int)MSG_ReadByte ();
		CL_ReadSnapshotState (&state, snapflags);
		if (msg_badread)
		{
			CL_RequestFullSnapshot ("snapshot_full state", true);
			msg_readcount = net_message.cursize;
			return;
		}
		if (!CL_SnapshotOriginIsSane (state.state.origin))
		{
			CL_RequestFullSnapshot ("snapshot_full origin guard", true);
			msg_readcount = net_message.cursize;
			return;
		}
		if (!drop)
		{
			cl.snapshot_stage[entnum] = state;
			cl.snapshot_stage_present[entnum] = 1;
		}
	}

	if (drop)
		return;

	{
		qboolean reset_interp = (!cl.has_full_snapshot || cl.need_full_snapshot);
		double snap_time = CL_ClampSnapshotServerTime (cl.mtime[0], seq);

		if (reset_interp)
			CL_PrepareFullSnapshotInterpReset ();

		if (snap_time > 0.0)
		{
			cl.snapshot_time = snap_time;
			CL_UpdateServerTimeEstimate (snap_time, "snap_full");
		}

		// INV-5: commit staged snapshot only after full parse.
		CL_StoreSnapshotBaseline (seq, cl.snapshot_stage, cl.snapshot_stage_present);
		for (i = 1; i < cl_max_edicts; i++)
		{
			if (!cl.snapshot_present[i])
				continue;
			CL_ApplySnapshotState (i, &cl.snapshot_baseline[i]);
			CL_MarkSnapshotEntityUpdated (i);
		}

		if (reset_interp)
			CL_FinalizeFullSnapshotInterpReset ();

		CL_InitViewEntityFromSim ();
	}

	cl.snap_last_applied_seq = seq16;
	cl.snap_last_complete_seq = seq16;
	cl.snap_last_incomplete = false;
	cl.snap_incomplete_start_time = 0;
	cl.snap_parse_consecutive = 0;
	cl.need_full_snapshot = false;
	cl.has_valid_worldstate = true;
	cl.has_full_snapshot = true;
	(void)CL_SendSnapshotAck (CL_GetSnapshotAckSeq ());
	CL_RecordPlayerSnap ();
	NET_DebugLogEvent (true,
		"NETDBG time %.3f snap_complete_apply seq %u\n",
		realtime, seq);
}

static void CL_ParseSnapshotDelta (void)
{
	unsigned int seq;
	unsigned int baseline_seq;
	unsigned int mask;
	int count;
	int i;
	int removes_parsed = 0;
	int touched = 0;
	qboolean baseline_match;
	qboolean base_is_current;
	const snapshot_state_t *base_states = NULL;
	const byte *base_present = NULL;
	qboolean drop;
	unsigned short seq16;
	qboolean need_full;
	qboolean continuation_pending;
	qboolean base_advanced = false;

	seq = (unsigned int)MSG_ReadLong ();
	baseline_seq = (unsigned int)MSG_ReadLong ();
	seq16 = (unsigned short)seq;
	need_full = (cl.need_full_snapshot || !cl.has_full_snapshot);
	{
		int base_index = CL_FindSnapshotBaselineIndex (baseline_seq);
		baseline_match = (base_index >= 0);
		base_is_current = (baseline_match && baseline_seq == cl.snapshot_baseline_seq);
		if (baseline_match)
		{
			base_states = cl.snapshot_baselines[base_index];
			base_present = cl.snapshot_baseline_present[base_index];
		}
	}
	continuation_pending = CL_HasActiveSnapshotChunks ();
	drop = CL_ShouldDropSnapshot ();

	if (cl_snap_debug.value >= 1.0f)
		Con_Printf ("cl_snap delta seq %u base %u match %d%s\n",
			seq, baseline_seq, baseline_match, drop ? " drop" : "");

	if (msg_badread)
	{
		CL_RequestFullSnapshot ("snapshot_delta header", true);
		msg_readcount = net_message.cursize;
		return;
	}

	if (baseline_seq == 0xFFFFFFFFu)
	{
		NET_DebugLogEvent (true,
			"NETDBG time %.3f snap_invalid_base_recv seq %u base %u\n",
			realtime, seq, baseline_seq);
		CL_RequestFullSnapshot ("snapshot_delta invalid base", false);
		drop = true;
	}

	if (!drop && base_is_current)
		SDL_assert (baseline_seq == cl.snapshot_baseline_seq);

	if (!drop && (!baseline_match || continuation_pending || need_full))
	{
		if (cl_snap_debug.value >= 1.0f)
			Con_Printf ("cl_snap delta mismatch base %u last %u need_full %d\n",
				baseline_seq, cl.snap_last_complete_seq, need_full);
		if (!baseline_match)
			CL_LogDeltaReject ("base_mismatch", seq, baseline_seq);
		else if (continuation_pending)
			CL_LogDeltaReject ("base_continuation", seq, baseline_seq);
		else
			CL_LogDeltaReject ("need_full", seq, baseline_seq);
		cl.snap_delta_mismatch++;
		CL_RequestFullSnapshot ("snapshot_delta base mismatch", false);
		drop = true;
	}

	if (!drop && CL_IsSnapshotSeqStale (seq16))
	{
		CL_LogDeltaReject ("seq_stale", seq, baseline_seq);
		cl.snap_delta_mismatch++;
		CL_RequestFullSnapshot ("snapshot_delta seq stale", false);
		drop = true;
	}

	CL_ClearSnapshotStage ();

	count = (unsigned short)MSG_ReadShort ();
	removes_parsed = count;
	if (msg_badread || count < 0 || count > cl_max_edicts || count > MAX_REMOVE_ENTITIES)
	{
		CL_RequestFullSnapshot ("snapshot_delta remove header", true);
		msg_readcount = net_message.cursize;
		return;
	}
	for (i = 0; i < count; i++)
	{
		int entnum = (unsigned short)MSG_ReadShort ();
		if (msg_badread)
		{
			CL_RequestFullSnapshot ("snapshot_delta remove list", true);
			msg_readcount = net_message.cursize;
			return;
		}
		if (!drop && baseline_match && base_present && entnum > 0 && entnum < cl_max_edicts
			&& base_present[entnum])
			cl.snapshot_stage_remove[entnum] = 1;
	}

	count = (unsigned short)MSG_ReadShort ();
	if (msg_badread || count < 0 || count > cl_max_edicts || count > MAX_SNAPSHOT_ENTITIES)
	{
		CL_RequestFullSnapshot ("snapshot_delta add header", true);
		msg_readcount = net_message.cursize;
		return;
	}
	for (i = 0; i < count; i++)
	{
		int entnum = (unsigned short)MSG_ReadShort ();
		snapshot_state_t state;
		unsigned int snapflags = 0;

		if (cl.protocolflags & PRFL_SNAPSHOT_HIRES)
			snapflags = (unsigned int)MSG_ReadByte ();
		CL_ReadSnapshotState (&state, snapflags);
		if (msg_badread)
		{
			CL_RequestFullSnapshot ("snapshot_delta add state", true);
			msg_readcount = net_message.cursize;
			return;
		}
		if (!CL_SnapshotOriginIsSane (state.state.origin))
		{
			CL_RequestFullSnapshot ("snapshot_delta origin guard", true);
			msg_readcount = net_message.cursize;
			return;
		}
		if (!drop && baseline_match && entnum > 0 && entnum < cl_max_edicts)
		{
			cl.snapshot_stage[entnum] = state;
			cl.snapshot_stage_present[entnum] = 1;
		}
	}

	count = (unsigned short)MSG_ReadShort ();
	if (msg_badread || count < 0 || count > cl_max_edicts || count > MAX_SNAPSHOT_ENTITIES)
	{
		CL_RequestFullSnapshot ("snapshot_delta update header", true);
		msg_readcount = net_message.cursize;
		return;
	}
	for (i = 0; i < count; i++)
	{
		int entnum = (unsigned short)MSG_ReadShort ();
		mask = (unsigned int)MSG_ReadLong ();
		if (msg_badread || (mask & ~SNAP_VALID_MASK) || CL_CountMaskBits (mask & SNAP_VALID_MASK) > MAX_PACKED_FIELDS)
		{
			CL_RequestFullSnapshot ("snapshot_delta update mask", true);
			msg_readcount = net_message.cursize;
			return;
		}
		if (!drop && baseline_match && base_present && entnum > 0 && entnum < cl_max_edicts
			&& base_present[entnum])
		{
			snapshot_state_t state = base_states[entnum];
			CL_ReadSnapshotDeltaFields (&state, mask);
			if (msg_badread)
			{
				CL_RequestFullSnapshot ("snapshot_delta update fields", true);
				msg_readcount = net_message.cursize;
				return;
			}
			if (!CL_SnapshotOriginIsSane (state.state.origin))
			{
				CL_RequestFullSnapshot ("snapshot_delta origin guard", true);
				msg_readcount = net_message.cursize;
				return;
			}
			cl.snapshot_stage[entnum] = state;
			cl.snapshot_stage_present[entnum] = 1;
		}
		else
		{
			snapshot_state_t scratch;
			memset (&scratch, 0, sizeof(scratch));
			CL_ReadSnapshotDeltaFields (&scratch, mask);
			if (msg_badread)
			{
				CL_RequestFullSnapshot ("snapshot_delta update skip", true);
				msg_readcount = net_message.cursize;
				return;
			}
			if (!CL_SnapshotOriginIsSane (scratch.state.origin))
			{
				CL_RequestFullSnapshot ("snapshot_delta origin guard", true);
				msg_readcount = net_message.cursize;
				return;
			}
		}
	}

	// INV-5: commit staged snapshot only after full parse/validation.
	if (!drop && baseline_match)
	{
		CL_StoreSnapshotBaselineFromBase (seq, base_states, base_present);
		{
			double snap_time = CL_ClampSnapshotServerTime (cl.mtime[0], seq);

			if (snap_time > 0.0)
			{
				cl.snapshot_time = snap_time;
				CL_UpdateServerTimeEstimate (snap_time, "snap_delta");
			}
		}
		if (base_is_current)
		{
			for (i = 1; i < cl_max_edicts; i++)
			{
				if (cl.snapshot_stage_remove[i])
					CL_ClearSnapshotEntity (i);
				if (!cl.snapshot_stage_present[i])
					continue;
				CL_ApplySnapshotState (i, &cl.snapshot_baseline[i]);
				CL_MarkSnapshotEntityUpdated (i);
				touched++;
			}
		}
		else
		{
			for (i = 1; i < cl_max_edicts; i++)
			{
				if (cl.snapshot_present[i])
				{
					CL_ApplySnapshotState (i, &cl.snapshot_baseline[i]);
					CL_MarkSnapshotEntityUpdated (i);
					touched++;
				}
			}
		}

		for (i = 1; i < cl_max_edicts; i++)
		{
			if (cl.snapshot_present[i] && cl_entities[i].msgtime != cl.mtime[0])
				cl_entities[i].msgtime = cl.mtime[0];
		}
		cl.snap_last_applied_seq = seq16;
		cl.snap_last_complete_seq = seq16;
		cl.snap_last_incomplete = false;
		cl.snap_incomplete_start_time = 0;
		cl.snap_parse_consecutive = 0;
		cl.has_full_snapshot = true;
		base_advanced = true;
		(void)CL_SendSnapshotAck (CL_GetSnapshotAckSeq ());
		CL_RecordPlayerSnap ();
		NET_DebugLogEvent (true,
			"NETDBG time %.3f snap_complete_apply seq %u\n",
			realtime, seq);
		CL_LogSnapshotDebug ("delta", seq, 0, true, removes_parsed, touched, base_advanced);
	}
	else if (!drop)
	{
		CL_SendSnapshotNak (cl.snapshot_baseline_seq, baseline_seq);
		CL_LogSnapshotDebug ("delta", seq, 0, true, removes_parsed, touched, base_advanced);
	}
}

static void CL_ReadSnapshotHeader (snapshot_header_t *header)
{
	header->seq = (unsigned int)MSG_ReadLong ();
	header->base_seq = (unsigned int)MSG_ReadLong ();
	header->flags = (unsigned int)MSG_ReadByte ();
	header->num_entities = (unsigned short)MSG_ReadShort ();
	header->num_removed = (unsigned short)MSG_ReadShort ();
	header->chunk_total_entities = 0;
	header->chunk_index = 0;
	header->chunk_total_chunks = 0;
	header->server_time = (cl.latest_server_time > 0.0) ? cl.latest_server_time : cl.mtime[0];
	if (header->flags & SNAPSHOT_FLAG_CHUNKINFO)
	{
		header->chunk_total_entities = (unsigned short)MSG_ReadShort ();
		header->chunk_index = (unsigned short)MSG_ReadShort ();
		header->chunk_total_chunks = (unsigned short)MSG_ReadShort ();
	}
}

static void CL_ClearSnapshotStage (void)
{
	if (cl.snapshot_stage_present)
		memset (cl.snapshot_stage_present, 0, cl_max_edicts * sizeof(byte));
	if (cl.snapshot_stage_remove)
		memset (cl.snapshot_stage_remove, 0, cl_max_edicts * sizeof(byte));
}

static void CL_PrepareFullSnapshotInterpReset (void)
{
	int i;

	cl.mtime[1] = cl.mtime[0];
	cl.time = cl.mtime[0];
	cl.oldtime = cl.mtime[0];
	CL_ResetPlayerSnaps ();

	for (i = 1; i < cl_max_edicts; i++)
	{
		CL_ClearEntitySnapHistory (i);
		if (cl.snapshot_missing_grace_until)
			cl.snapshot_missing_grace_until[i] = 0;
		if (cl.snapshot_resync_start_time)
			cl.snapshot_resync_start_time[i] = 0;
		if (cl.snapshot_resync_end_time)
			cl.snapshot_resync_end_time[i] = 0;
	}
}

static void CL_FinalizeFullSnapshotInterpReset (void)
{
	int i;

	for (i = 1; i < cl_max_edicts; i++)
	{
		entity_t *ent;

		if (!cl.snapshot_present || !cl.snapshot_present[i])
			continue;
		ent = &cl_entities[i];
		VectorCopy (ent->msg_origins[0], ent->msg_origins[1]);
		VectorCopy (ent->msg_angles[0], ent->msg_angles[1]);
		VectorCopy (ent->msg_origins[0], ent->origin);
		VectorCopy (ent->msg_angles[0], ent->angles);
		ent->forcelink = true;
		ent->lerpflags |= LERP_RESETMOVE|LERP_RESETANIM;
	}
}

static double CL_Snap2ResyncBlendSeconds (void)
{
	double ms = CLAMP (0.0, cl_snap2_resync_blend_ms.value, 1000.0);
	return ms * 0.001;
}

static double CL_Snap2MissingGraceSeconds (void)
{
	double ms = CLAMP (0.0, cl_snap2_missing_grace_ms.value, 2000.0);
	return ms * 0.001;
}

static void CL_ResetSnapshotChunkAssembly (cl_snapshot_chunk_asm_t *chunk)
{
	int i;

	if (!chunk)
		return;

	for (i = 0; i < CL_SNAPSHOT_MAX_CHUNKS; i++)
	{
		if (chunk->buffers[i])
		{
			Z_Free (chunk->buffers[i]);
			chunk->buffers[i] = NULL;
		}
		chunk->sizes[i] = 0;
		chunk->received_mask[i] = 0;
	}
	if (cl.snapshot_chunk_present)
		memset (cl.snapshot_chunk_present, 0, cl_max_edicts * sizeof(byte));
	memset (chunk, 0, sizeof(*chunk));
}

static cl_snapshot_chunk_asm_t *CL_FindSnapshotChunkAssembly (unsigned int seq)
{
	int i;

	for (i = 0; i < CL_SNAPSHOT_CHUNK_INFLIGHT; i++)
	{
		cl_snapshot_chunk_asm_t *chunk = &cl.snapshot_chunk_assemblies[i];

		if (chunk->active && chunk->seq == seq)
			return chunk;
	}

	return NULL;
}

static cl_snapshot_chunk_asm_t *CL_AllocSnapshotChunkAssembly (unsigned int seq)
{
	int i;
	cl_snapshot_chunk_asm_t *oldest = NULL;

	for (i = 0; i < CL_SNAPSHOT_CHUNK_INFLIGHT; i++)
	{
		cl_snapshot_chunk_asm_t *chunk = &cl.snapshot_chunk_assemblies[i];

		if (!chunk->active)
			return chunk;
		if (!oldest || chunk->start_time < oldest->start_time)
			oldest = chunk;
	}

	if (oldest)
	{
		if (cl_snap_debug.value >= 1.0f)
		{
			NET_DebugLogEvent (true,
				"NETDBG time %.3f snap2_chunk_evict seq %u new_seq %u\n",
				realtime, oldest->seq, seq);
		}
		CL_ResetSnapshotChunkAssembly (oldest);
		return oldest;
	}

	return NULL;
}

static qboolean CL_HasActiveSnapshotChunks (void)
{
	int i;

	for (i = 0; i < CL_SNAPSHOT_CHUNK_INFLIGHT; i++)
	{
		if (cl.snapshot_chunk_assemblies[i].active)
			return true;
	}

	return false;
}

static qboolean CL_ParseSnapshot2FullPayload (const snapshot_header_t *header, qboolean drop, qboolean incomplete,
	unsigned short seq16, qboolean is_full, qboolean is_delta, int *out_touched, qboolean *out_base_advanced)
{
	int i;
	int touched = 0;
	qboolean base_advanced = false;

	CL_ClearSnapshotStage ();

	for (i = 0; i < header->num_entities; i++)
	{
		int entnum;
		snapshot_state_t state;
		unsigned int snapflags = 0;

		entnum = (unsigned short)MSG_ReadShort ();
		if (entnum <= 0 || entnum >= cl_max_edicts)
		{
			CL_RequestFullSnapshot ("snapshot2 full entnum", true);
			msg_readcount = net_message.cursize;
			return false;
		}
		if (cl.protocolflags & PRFL_SNAPSHOT_HIRES)
			snapflags = (unsigned int)MSG_ReadByte ();
		CL_ReadSnapshotState (&state, snapflags);
		if (msg_badread)
		{
			CL_RequestFullSnapshot ("snapshot2 full state", true);
			msg_readcount = net_message.cursize;
			return false;
		}
		if (!CL_SnapshotOriginIsSane (state.state.origin))
		{
			CL_RequestFullSnapshot ("snapshot2 origin guard", true);
			msg_readcount = net_message.cursize;
			return false;
		}
		if (!drop)
		{
			cl.snapshot_stage[entnum] = state;
			cl.snapshot_stage_present[entnum] = 1;
		}
	}

	if (!drop)
	{
		if (!incomplete)
		{
			qboolean reset_interp = (!cl.has_full_snapshot || cl.need_full_snapshot);
			qboolean midgame_full = (cl.has_full_snapshot && !cl.need_full_snapshot && cls.signon == SIGNONS);
			double blend_s = midgame_full ? CL_Snap2ResyncBlendSeconds () : 0.0;
			double grace_s = midgame_full ? CL_Snap2MissingGraceSeconds () : 0.0;
			const byte *prev_present = cl.snapshot_present;
			double snap_time = CL_ClampSnapshotServerTime (header->server_time, header->seq);
			double snap_stamp = 0.0;

			if (reset_interp)
				CL_PrepareFullSnapshotInterpReset ();

			if (snap_time > 0.0)
			{
				cl.snapshot_time = snap_time;
				CL_UpdateServerTimeEstimate (snap_time, "snap2_full");
			}
			snap_stamp = (snap_time > 0.0) ? snap_time
				: (cl.snapshot_time > 0.0 ? cl.snapshot_time : cl.mtime[0]);

			// INV-5: commit staged snapshot only after full parse.
			CL_StoreSnapshotBaseline (header->seq, cl.snapshot_stage, cl.snapshot_stage_present);
			if (midgame_full)
			{
				int resync_count = 0;
				int missing_grace_count = 0;

				cl.snap_full_midgame_count++;
				if (prev_present && cl.snapshot_present)
				{
					for (i = 1; i < cl_max_edicts; i++)
					{
						if (!prev_present[i])
							continue;
						if (cl.snapshot_present[i])
						{
							if (blend_s > 0.0 && cl.snapshot_resync_start_time && cl.snapshot_resync_end_time
								&& cl.snapshot_resync_from_origin && cl.snapshot_resync_from_angles)
							{
								entity_t *ent = &cl_entities[i];
								VectorCopy (ent->origin, cl.snapshot_resync_from_origin[i]);
								VectorCopy (ent->angles, cl.snapshot_resync_from_angles[i]);
								cl.snapshot_resync_start_time[i] = snap_stamp;
								cl.snapshot_resync_end_time[i] = snap_stamp + blend_s;
								resync_count++;
							}
							if (cl.snapshot_missing_grace_until)
								cl.snapshot_missing_grace_until[i] = 0;
						}
						else if (grace_s > 0.0 && cl.snapshot_missing_grace_until)
						{
							cl.snapshot_missing_grace_until[i] = snap_stamp + grace_s;
							missing_grace_count++;
						}
					}
				}
				if (resync_count > 0)
					cl.snap_full_midgame_soft_count++;
				if (missing_grace_count > 0)
					cl.snap_full_midgame_missing_grace += missing_grace_count;
				NET_DebugLogEvent (true,
					"NETDBG time %.3f snap2_midgame_full seq %u blend_ms %.0f grace_ms %.0f resync %d missing_grace %d\n",
					realtime, header->seq, blend_s * 1000.0, grace_s * 1000.0,
					resync_count, missing_grace_count);
			}
			for (i = 1; i < cl_max_edicts; i++)
			{
				if (!cl.snapshot_present[i])
					continue;
				CL_ApplySnapshotState (i, &cl.snapshot_baseline[i]);
				CL_MarkSnapshotEntityUpdated (i);
				touched++;
			}

			if (reset_interp)
				CL_FinalizeFullSnapshotInterpReset ();

			CL_InitViewEntityFromSim ();
			CL_Predict_Reapply ();

			cl.snap_last_applied_seq = seq16;
			cl.snap_last_complete_seq = seq16;
			cl.snap_last_incomplete = false;
			cl.snap_incomplete_start_time = 0;
			cl.snap_parse_consecutive = 0;
			cl.need_full_snapshot = false;
			cl.has_full_snapshot = true;
			cl.has_valid_worldstate = true;
			cl.snap_incomplete_count = 0;
			CL_RecordPlayerSnap ();
			CL_EnsureViewEntityOrigin ("snapshot2");
			base_advanced = true;
			NET_DebugLogEvent (true,
				"NETDBG time %.3f snap2_apply seq %u flags 0x%x full %d delta %d has_full %d\n",
				realtime, header->seq, header->flags, is_full ? 1 : 0, is_delta ? 1 : 0,
				cl.has_full_snapshot ? 1 : 0);
			CL_LogSnapshotDebug ("snap2_full", header->seq, header->flags, true,
				0, touched, base_advanced);
			CL_NetDbg_LogInterpSnapshot (header, 0, prev_present);
			CL_NetDbg_LogWatchEnt (header, 0);
		}
		else
		{
			double snap_time = CL_ClampSnapshotServerTime (header->server_time, header->seq);

			if (snap_time > 0.0)
			{
				cl.snapshot_time = snap_time;
				CL_UpdateServerTimeEstimate (snap_time, "snap2_full_incomplete");
			}
			// INCOMPLETE snapshots: keep existing entities, overlay only touched updates.
			touched = CL_ApplySnapshotOverlay (cl.snapshot_stage, cl.snapshot_stage_present);
			cl.snap_parse_consecutive = 0;
			CL_TrackSnapshotIncomplete (seq16, "full");
			NET_DebugLogEvent (true,
				"NETDBG time %.3f snap2_buffer seq %u flags 0x%x incomplete %d has_full %d\n",
				realtime, header->seq, header->flags, cl.snap_incomplete_count,
				cl.has_full_snapshot ? 1 : 0);
			CL_LogSnapshotDebug ("snap2_full", header->seq, header->flags, false,
				0, touched, base_advanced);
		}
		if (!CL_SendSnapshotAck (CL_GetSnapshotAckSeq ()))
			CL_RequestFullSnapshot ("snapshot2 ack failed", false);
	}

	if (out_touched)
		*out_touched = touched;
	if (out_base_advanced)
		*out_base_advanced = base_advanced;
	return !drop;
}

static void CL_LogSnapshot2Receive (const snapshot_header_t *header, int start_pos, const char *status)
{
	int bytes = msg_readcount - start_pos;

	if (bytes < 0)
		bytes = 0;
	NET_DebugLogEvent (true,
		"NETDBG time %.3f snap2_recv_status seq %u base %u flags 0x%x bytes %d status %s\n",
		realtime, header->seq, header->base_seq, header->flags, bytes, status ? status : "unknown");
}

static void CL_ParseSnapshot2 (void)
{
	snapshot_header_t header;
	qboolean baseline_match;
	qboolean base_is_current;
	const snapshot_state_t *base_states = NULL;
	const byte *base_present = NULL;
	qboolean drop;
	int i;
	int removes_parsed = 0;
	int touched = 0;
	unsigned short seq16;
	qboolean incomplete;
	qboolean need_full;
	qboolean is_full;
	qboolean is_delta;
	qboolean continuation;
	qboolean base_advanced = false;
	const char *snap_status = "drop";
	int start_pos = msg_readcount;

	CL_ReadSnapshotHeader (&header);
	drop = CL_ShouldDropSnapshot ();
	if (!drop && (header.flags & SNAPSHOT_FLAG_FULL)
		&& (header.flags & SNAPSHOT_FLAG_CONTINUE)
		&& cl_snap_chunk_drop.value > 0.0f
		&& ((float)rand() / (float)RAND_MAX) < cl_snap_chunk_drop.value)
	{
		if (cl_snap_debug.value >= 1.0f)
			Con_Printf ("cl_snap2 chunk dropped seq %u\n", header.seq);
		drop = true;
	}
	seq16 = (unsigned short)header.seq;
	incomplete = (header.flags & SNAPSHOT_FLAG_INCOMPLETE) != 0;
	need_full = (cl.need_full_snapshot || !cl.has_full_snapshot);
	is_full = (header.flags & SNAPSHOT_FLAG_FULL) != 0;
	is_delta = (header.flags & SNAPSHOT_FLAG_DELTA) != 0;
	continuation = (header.flags & SNAPSHOT_FLAG_CONTINUE) != 0;

	if (is_full && !cl.has_full_snapshot && !continuation)
		incomplete = false;
	if (is_delta && incomplete)
	{
		if (cl_snap_debug.value >= 1.0f)
			Con_Printf ("cl_snap2 delta incomplete ignored seq %u base %u\n",
				header.seq, header.base_seq);
		incomplete = false;
	}

	if (!CL_SnapshotWorldModelReady ("snapshot2 worldmodel"))
		drop = true;

	NET_DebugLogEvent (true,
		"NETDBG time %.3f snap2_recv seq %u base %u flags 0x%x ents %u rem %u full %d delta %d has_full %d\n",
		realtime, header.seq, header.base_seq, header.flags, header.num_entities, header.num_removed,
		is_full ? 1 : 0, is_delta ? 1 : 0, cl.has_full_snapshot ? 1 : 0);

	CL_CheckSnapshotChunkTimeouts ();

	if (cl_snap_debug.value >= 1.0f)
	{
		Con_Printf ("cl_snap2 seq %u base %u flags 0x%x ents %u rem %u%s\n",
			header.seq, header.base_seq, header.flags,
			header.num_entities, header.num_removed, drop ? " drop" : "");
	}

	if (msg_badread || header.num_entities > cl_max_edicts || header.num_removed > cl_max_edicts
		|| header.num_entities > MAX_SNAPSHOT_ENTITIES || header.num_removed > MAX_REMOVE_ENTITIES
		|| (unsigned int)header.num_entities + (unsigned int)header.num_removed > MAX_SNAPSHOT_ENTITIES)
	{
		CL_RequestFullSnapshot ("snapshot2 header", true);
		msg_readcount = net_message.cursize;
		return;
	}
	if (incomplete)
	{
		NET_DebugLogEvent (true,
			"NETDBG time %.3f snap_incomplete_recv seq %u base %u flags 0x%x ents %u rem %u\n",
			realtime, header.seq, header.base_seq, header.flags, header.num_entities, header.num_removed);
		if ((header.flags & SNAPSHOT_FLAG_FULL) && header.num_removed == 0)
		{
			if (header.base_seq == cl.snap_rem0_base && header.seq != cl.snap_rem0_seq)
				cl.snap_rem0_count++;
			else
				cl.snap_rem0_count = 0;
			cl.snap_rem0_base = header.base_seq;
			cl.snap_rem0_seq = header.seq;
			if (cl.snap_rem0_count >= 2)
			{
				NET_DebugLogEvent (true,
					"NETDBG time %.3f snap_rem0_stall seq %u base %u\n",
					realtime, header.seq, header.base_seq);
				CL_RequestFullSnapshot ("snapshot2 rem0 stall", false);
			}
		}
		else
		{
			cl.snap_rem0_count = 0;
		}
	}
	else
	{
		cl.snap_rem0_count = 0;
	}

	if (is_delta && !cl.has_full_snapshot)
	{
		CL_LogDeltaReject ("need_full", header.seq, header.base_seq);
		CL_RequestFullSnapshot ("snapshot2 need full", false);
		drop = true;
	}

	if (is_full)
	{
		qboolean rem_zero = (header.num_removed == 0);
		qboolean final_chunk = (!continuation || rem_zero);
		qboolean finalize_invalid = (!continuation && !rem_zero);
		qboolean chunkinfo_ok = (header.flags & SNAPSHOT_FLAG_CHUNKINFO) != 0;
		qboolean is_chunked = (chunkinfo_ok || continuation);

		if (finalize_invalid)
		{
			NET_DebugLogEvent (true,
				"NETDBG time %.3f snap_finalize_skip seq %u base %u flags 0x%x reason flag_mismatch\n",
				realtime, header.seq, header.base_seq, header.flags);
			CL_RequestFullSnapshot ("snapshot2 flag mismatch", false);
			drop = true;
		}

		if (is_chunked)
		{
			int payload_start = msg_readcount;
			int payload_end = 0;
			int chunk_len = 0;
			unsigned short chunk_index = header.chunk_index;
			unsigned short total_chunks = header.chunk_total_chunks;
			unsigned short total_entities = header.chunk_total_entities;
			qboolean duplicate_chunk = false;
			qboolean chunk_done = false;
			cl_snapshot_chunk_asm_t *chunk = NULL;

			if (!chunkinfo_ok)
			{
				CL_RequestFullSnapshot ("snapshot2 chunk info", true);
				drop = true;
			}
			if (total_chunks == 0 || total_chunks > CL_SNAPSHOT_MAX_CHUNKS)
			{
				CL_RequestFullSnapshot ("snapshot2 chunk total", true);
				drop = true;
			}
			if (total_entities == 0 || total_entities > cl_max_edicts || total_entities > MAX_SNAPSHOT_ENTITIES)
			{
				CL_RequestFullSnapshot ("snapshot2 chunk entities", true);
				drop = true;
			}
			if (chunk_index >= total_chunks)
			{
				CL_RequestFullSnapshot ("snapshot2 chunk index", true);
				drop = true;
			}

			chunk = CL_FindSnapshotChunkAssembly (header.seq);
			if (!chunk)
				chunk = CL_AllocSnapshotChunkAssembly (header.seq);
			if (!chunk)
			{
				CL_RequestFullSnapshot ("snapshot2 chunk seq", false);
				drop = true;
			}
			else if (!chunk->active)
			{
				chunk->active = true;
				chunk->seq = header.seq;
				chunk->start_time = realtime;
				chunk->server_time = header.server_time;
				chunk->packets = 0;
				chunk->total_chunks = total_chunks;
				chunk->total_entities = total_entities;
				chunk->expected_total = total_entities;
				chunk->received = 0;
				chunk->remaining = header.num_removed;
				chunk->flags = header.flags;
			}
			else
			{
				if (chunk->total_chunks != total_chunks
					|| chunk->total_entities != total_entities)
				{
					CL_RequestFullSnapshot ("snapshot2 chunk mismatch", false);
					CL_ResetSnapshotChunkAssembly (chunk);
					drop = true;
				}
				if (chunk->server_time <= 0.0 && header.server_time > 0.0)
					chunk->server_time = header.server_time;
			}

			if (chunk)
			{
				chunk->packets++;
				if (cl_full_reasm_debug.value >= 2.0f)
					Con_Printf ("cl_snap2 full reasm seq %u packet %d flags 0x%x ents %u\n",
						header.seq, chunk->packets, header.flags, header.num_entities);
			}

			for (i = 0; i < header.num_entities; i++)
			{
				int entnum;
				snapshot_state_t state;
				unsigned int snapflags = 0;

				entnum = (unsigned short)MSG_ReadShort ();
				if (entnum <= 0 || entnum >= cl_max_edicts)
				{
					CL_RequestFullSnapshot ("snapshot2 chunk entnum", true);
					msg_readcount = net_message.cursize;
					if (chunk)
						CL_ResetSnapshotChunkAssembly (chunk);
					return;
				}
				if (cl.protocolflags & PRFL_SNAPSHOT_HIRES)
					snapflags = (unsigned int)MSG_ReadByte ();
				CL_ReadSnapshotState (&state, snapflags);
				if (msg_badread)
				{
					CL_RequestFullSnapshot ("snapshot2 chunk state", true);
					msg_readcount = net_message.cursize;
					if (chunk)
						CL_ResetSnapshotChunkAssembly (chunk);
					return;
				}
				if (!CL_SnapshotOriginIsSane (state.state.origin))
				{
					CL_RequestFullSnapshot ("snapshot2 origin guard", true);
					msg_readcount = net_message.cursize;
					if (chunk)
						CL_ResetSnapshotChunkAssembly (chunk);
					return;
				}
			}

			payload_end = msg_readcount;
			chunk_len = payload_end - payload_start;
			if (!drop && chunk)
			{
				if (chunk_len <= 0 || chunk->total_bytes + (unsigned int)chunk_len > MAX_MSGLEN)
				{
					CL_RequestFullSnapshot ("snapshot2 chunk bytes", true);
					drop = true;
				}
			}

			if (drop)
			{
				if (chunk)
					CL_ResetSnapshotChunkAssembly (chunk);
				snap_status = "drop";
				CL_LogSnapshot2Receive (&header, start_pos, snap_status);
				return;
			}

			if (chunk->received_mask[chunk_index])
				duplicate_chunk = true;

			if (!duplicate_chunk)
			{
				byte *buffer = (byte *)Z_Malloc (chunk_len);
				memcpy (buffer, net_message.data + payload_start, chunk_len);
				chunk->buffers[chunk_index] = buffer;
				chunk->sizes[chunk_index] = (unsigned short)chunk_len;
				chunk->received_mask[chunk_index] = 1;
				chunk->received_chunks++;
				chunk->received += header.num_entities;
				chunk->total_bytes += (unsigned int)chunk_len;
			}

			NET_DebugLogEvent (true,
				"NETDBG time %.3f snap2_chunk_buffer seq %u flags 0x%x recv %u total %u rem %u packets %d\n",
				realtime, header.seq, header.flags, chunk->received,
				chunk->expected_total, header.num_removed,
				chunk->packets);

			if (chunk->received > chunk->expected_total)
			{
				CL_RequestFullSnapshot ("snapshot2 chunk overrun", false);
				CL_ResetSnapshotChunkAssembly (chunk);
				return;
			}

			if (chunk->received_chunks < chunk->total_chunks)
			{
				if (cl_snap_debug.value >= 1.0f)
				{
					NET_DebugLogEvent (true,
						"NETDBG time %.3f full_chunk_in_progress seq %u recv %u/%u\n",
						realtime, header.seq, chunk->received_chunks, chunk->total_chunks);
				}
				snap_status = "buffered";
				CL_LogSnapshot2Receive (&header, start_pos, snap_status);
				return;
			}

			if (chunk->received != chunk->expected_total)
			{
				NET_DebugLogEvent (true,
					"NETDBG time %.3f snap2_chunk_mismatch seq %u recv %u total %u\n",
					realtime, header.seq, chunk->received, chunk->expected_total);
				CL_RequestFullSnapshot ("snapshot2 chunk total", false);
				CL_ResetSnapshotChunkAssembly (chunk);
				snap_status = "drop";
				CL_LogSnapshot2Receive (&header, start_pos, snap_status);
				return;
			}

			{
				byte *assembled = (byte *)Z_Malloc (chunk->total_bytes);
				int offset = 0;
				snapshot_header_t full_header = header;
				sizebuf_t saved_message = net_message;
				int saved_readcount = msg_readcount;
				qboolean saved_badread = msg_badread;
				int saved_readbitpos = msg_readbitpos;

				full_header.flags &= ~(SNAPSHOT_FLAG_CONTINUE | SNAPSHOT_FLAG_CHUNKINFO);
				full_header.num_entities = chunk->total_entities;
				full_header.num_removed = 0;
				full_header.server_time = chunk->server_time;

				for (i = 0; i < chunk->total_chunks; i++)
				{
					if (!chunk->received_mask[i])
					{
						Z_Free (assembled);
						CL_RequestFullSnapshot ("snapshot2 chunk missing", false);
						CL_ResetSnapshotChunkAssembly (chunk);
						snap_status = "drop";
						CL_LogSnapshot2Receive (&header, start_pos, snap_status);
						return;
					}
					memcpy (assembled + offset, chunk->buffers[i],
						chunk->sizes[i]);
					offset += chunk->sizes[i];
				}

				net_message.data = assembled;
				net_message.cursize = chunk->total_bytes;
				net_message.maxsize = chunk->total_bytes;
				net_message.allowoverflow = false;
				net_message.overflowed = false;
				net_message.bitpos = 0;
				msg_readcount = 0;
				msg_badread = false;
				msg_readbitpos = 0;

				chunk_done = CL_ParseSnapshot2FullPayload (&full_header, drop, incomplete,
					seq16, is_full, is_delta, &touched, &base_advanced);

				net_message = saved_message;
				msg_readcount = saved_readcount;
				msg_badread = saved_badread;
				msg_readbitpos = saved_readbitpos;
				Z_Free (assembled);
			}

			CL_ResetSnapshotChunkAssembly (chunk);
			snap_status = chunk_done ? (incomplete ? "buffered" : "accepted") : "drop";
			CL_LogSnapshot2Receive (&header, start_pos, snap_status);
			return;
		}

		if (CL_ParseSnapshot2FullPayload (&header, drop, incomplete, seq16,
			is_full, is_delta, &touched, &base_advanced))
			snap_status = incomplete ? "buffered" : "accepted";
		else
			snap_status = "drop";
		CL_LogSnapshot2Receive (&header, start_pos, snap_status);
		return;
	}

	if (header.base_seq == 0xFFFFFFFFu)
	{
		NET_DebugLogEvent (true,
			"NETDBG time %.3f snap_invalid_base_recv seq %u base %u\n",
			realtime, header.seq, header.base_seq);
		CL_RequestFullSnapshot ("snapshot2 invalid base", false);
		drop = true;
	}

	{
		int base_index = CL_FindSnapshotBaselineIndex (header.base_seq);
		baseline_match = (base_index >= 0);
		base_is_current = (baseline_match && header.base_seq == cl.snapshot_baseline_seq);
		if (baseline_match)
		{
			base_states = cl.snapshot_baselines[base_index];
			base_present = cl.snapshot_baseline_present[base_index];
		}
	}
	if (cl_snap_debug.value >= 1.0f)
	{
		NET_DebugLogEvent (false,
			"NETDBG time %.3f snap2_base seq %u base %u flags 0x%x baseline %d base_current %d need_full %d\n",
			realtime, header.seq, header.base_seq, header.flags,
			baseline_match ? 1 : 0, base_is_current ? 1 : 0, need_full ? 1 : 0);
	}
	if (!drop && base_is_current)
		SDL_assert (header.base_seq == cl.snapshot_baseline_seq);

	if (!drop && (!baseline_match || CL_HasActiveSnapshotChunks () || need_full))
	{
		if (cl_snap_debug.value >= 1.0f)
			Con_Printf ("cl_snap2 delta mismatch base %u last %u need_full %d\n",
				header.base_seq, cl.snap_last_complete_seq, need_full);
		if (!baseline_match)
			CL_LogDeltaReject ("base_mismatch", header.seq, header.base_seq);
		else if (CL_HasActiveSnapshotChunks ())
			CL_LogDeltaReject ("base_continuation", header.seq, header.base_seq);
		else
			CL_LogDeltaReject ("need_full", header.seq, header.base_seq);
		cl.snap_delta_mismatch++;
		CL_RequestFullSnapshot ("snapshot2 base mismatch", false);
		drop = true;
	}

	if (!drop && CL_IsSnapshotSeqStale (seq16))
	{
		CL_LogDeltaReject ("seq_stale", header.seq, header.base_seq);
		cl.snap_delta_mismatch++;
		CL_RequestFullSnapshot ("snapshot2 seq stale", false);
		drop = true;
	}

	CL_ClearSnapshotStage ();

	if (header.flags & SNAPSHOT_FLAG_HAS_REMOVE_LIST)
	{
		for (i = 0; i < header.num_removed; i++)
		{
			int entnum = (unsigned short)MSG_ReadShort ();
			removes_parsed++;
			if (msg_badread)
			{
				CL_RequestFullSnapshot ("snapshot2 remove list", true);
				msg_readcount = net_message.cursize;
				return;
			}
			if (!drop && baseline_match && !incomplete && base_present && entnum > 0 && entnum < cl_max_edicts
				&& base_present[entnum])
				cl.snapshot_stage_remove[entnum] = 1;
		}
	}

	{
		int add_count = (unsigned short)MSG_ReadShort ();
		if (msg_badread || add_count < 0 || add_count > cl_max_edicts || add_count > MAX_SNAPSHOT_ENTITIES)
		{
			CL_RequestFullSnapshot ("snapshot2 add header", true);
			msg_readcount = net_message.cursize;
			return;
		}
		for (i = 0; i < add_count; i++)
		{
			int entnum = (unsigned short)MSG_ReadShort ();
			snapshot_state_t state;
			unsigned int snapflags = 0;

			if (cl.protocolflags & PRFL_SNAPSHOT_HIRES)
				snapflags = (unsigned int)MSG_ReadByte ();
			CL_ReadSnapshotState (&state, snapflags);
			if (msg_badread)
			{
				CL_RequestFullSnapshot ("snapshot2 add state", true);
				msg_readcount = net_message.cursize;
				return;
			}
			if (!drop && baseline_match && entnum > 0 && entnum < cl_max_edicts)
			{
				if (!CL_SnapshotOriginIsSane (state.state.origin))
				{
					CL_RequestFullSnapshot ("snapshot2 origin guard", true);
					msg_readcount = net_message.cursize;
					return;
				}
				cl.snapshot_stage[entnum] = state;
				cl.snapshot_stage_present[entnum] = 1;
			}
		}

		{
			int update_count = (unsigned short)MSG_ReadShort ();
		if (msg_badread || update_count < 0 || update_count > cl_max_edicts || update_count > MAX_SNAPSHOT_ENTITIES)
			{
				CL_RequestFullSnapshot ("snapshot2 update header", true);
				msg_readcount = net_message.cursize;
				return;
			}
			for (i = 0; i < update_count; i++)
			{
				int entnum = (unsigned short)MSG_ReadShort ();
				unsigned int mask = (unsigned int)MSG_ReadLong ();
				if (msg_badread || (mask & ~SNAP_VALID_MASK) || CL_CountMaskBits (mask & SNAP_VALID_MASK) > MAX_PACKED_FIELDS)
				{
					CL_RequestFullSnapshot ("snapshot2 update mask", true);
					msg_readcount = net_message.cursize;
					return;
				}

				if (!drop && baseline_match && base_present && entnum > 0 && entnum < cl_max_edicts
					&& base_present[entnum])
				{
					snapshot_state_t state = base_states[entnum];
					CL_ReadSnapshotDeltaFields (&state, mask);
					if (msg_badread)
					{
						CL_RequestFullSnapshot ("snapshot2 update fields", true);
						msg_readcount = net_message.cursize;
						return;
					}
					if (!CL_SnapshotOriginIsSane (state.state.origin))
					{
						CL_RequestFullSnapshot ("snapshot2 origin guard", true);
						msg_readcount = net_message.cursize;
						return;
					}
					cl.snapshot_stage[entnum] = state;
					cl.snapshot_stage_present[entnum] = 1;
				}
				else
				{
					snapshot_state_t scratch;
					memset (&scratch, 0, sizeof(scratch));
					CL_ReadSnapshotDeltaFields (&scratch, mask);
					if (msg_badread)
					{
						CL_RequestFullSnapshot ("snapshot2 update skip", true);
						msg_readcount = net_message.cursize;
						return;
					}
					if (!CL_SnapshotOriginIsSane (scratch.state.origin))
					{
						CL_RequestFullSnapshot ("snapshot2 origin guard", true);
						msg_readcount = net_message.cursize;
						return;
					}
				}
			}
		}
	}

	if (!drop && baseline_match)
	{
		if (!incomplete)
		{
			const byte *prev_present = cl.snapshot_present;
			double snap_time = CL_ClampSnapshotServerTime (header.server_time, header.seq);

			if (snap_time > 0.0)
			{
				cl.snapshot_time = snap_time;
				CL_UpdateServerTimeEstimate (snap_time, "snap2_delta");
			}

			CL_StoreSnapshotBaselineFromBase (header.seq, base_states, base_present);
			if (base_is_current)
			{
				for (i = 1; i < cl_max_edicts; i++)
				{
					if (cl.snapshot_stage_remove[i])
						CL_ClearSnapshotEntity (i);
					if (!cl.snapshot_stage_present[i])
						continue;
					CL_ApplySnapshotState (i, &cl.snapshot_baseline[i]);
					CL_MarkSnapshotEntityUpdated (i);
					touched++;
				}
			}
			else
			{
				for (i = 1; i < cl_max_edicts; i++)
				{
					if (cl.snapshot_present[i])
					{
						CL_ApplySnapshotState (i, &cl.snapshot_baseline[i]);
						CL_MarkSnapshotEntityUpdated (i);
						touched++;
					}
				}
			}

			CL_Predict_Reapply ();

			for (i = 1; i < cl_max_edicts; i++)
			{
				if (cl.snapshot_present[i] && cl_entities[i].msgtime != cl.mtime[0])
					cl_entities[i].msgtime = cl.mtime[0];
			}

			cl.snap_last_applied_seq = seq16;
			cl.snap_last_complete_seq = seq16;
			cl.snap_last_incomplete = false;
			cl.snap_incomplete_start_time = 0;
			cl.snap_parse_consecutive = 0;
			cl.snap_incomplete_count = 0;
			cl.has_full_snapshot = true;
			base_advanced = true;
			CL_RecordPlayerSnap ();
			CL_EnsureViewEntityOrigin ("snapshot2");
			NET_DebugLogEvent (true,
				"NETDBG time %.3f snap2_apply seq %u flags 0x%x full %d delta %d has_full %d\n",
				realtime, header.seq, header.flags, is_full ? 1 : 0, is_delta ? 1 : 0,
				cl.has_full_snapshot ? 1 : 0);
			CL_LogSnapshotDebug ("snap2_delta", header.seq, header.flags, true,
				removes_parsed, touched, base_advanced);
			CL_NetDbg_LogInterpSnapshot (&header, removes_parsed, prev_present);
			{
				int watch_removed = 0;
				int watch = (int)cl_netdbg_watch_ent.value;

				if (watch > 0 && watch < cl_max_edicts && cl.snapshot_stage_remove
					&& cl.snapshot_stage_remove[watch])
				{
					watch_removed = 1;
				}
				CL_NetDbg_LogWatchEnt (&header, watch_removed);
			}
		}
		else
		{
			if (!base_is_current)
			{
				CL_RequestFullSnapshot ("snapshot2 incomplete base mismatch", false);
				drop = true;
			}
			else
			{
				double snap_time = CL_ClampSnapshotServerTime (header.server_time, header.seq);

				if (snap_time > 0.0)
				{
					cl.snapshot_time = snap_time;
					CL_UpdateServerTimeEstimate (snap_time, "snap2_delta_incomplete");
				}
				// INCOMPLETE snapshots: keep existing entities, overlay only touched updates.
				touched = CL_ApplySnapshotOverlay (cl.snapshot_stage, cl.snapshot_stage_present);
				CL_TrackSnapshotIncomplete (seq16, "delta");
				NET_DebugLogEvent (true,
					"NETDBG time %.3f snap2_buffer seq %u flags 0x%x incomplete %d has_full %d\n",
					realtime, header.seq, header.flags, cl.snap_incomplete_count,
					cl.has_full_snapshot ? 1 : 0);
				CL_LogSnapshotDebug ("snap2_delta", header.seq, header.flags, false,
					removes_parsed, touched, base_advanced);
			}
		}
		if (!drop)
		{
			if (!CL_SendSnapshotAck (CL_GetSnapshotAckSeq ()))
				CL_RequestFullSnapshot ("snapshot2 ack failed", false);
		}
	}
	else if (!drop)
	{
		CL_SendSnapshotNak (cl.snapshot_baseline_seq, header.base_seq);
		CL_LogSnapshotDebug ("snap2_delta", header.seq, header.flags, !incomplete,
			removes_parsed, touched, base_advanced);
	}

	if (is_delta)
	{
		if (!drop && baseline_match)
			snap_status = incomplete ? "buffered" : "accepted";
		else if (!drop)
			snap_status = "buffered";
		else
			snap_status = "drop";
		CL_LogSnapshot2Receive (&header, start_pos, snap_status);
	}
}

#define CL_SetHudStat(stat) cl.statsf[stat] = cl.stats[stat]

/*
==================
CL_ParseClientdata

Server information pertaining to this client only
==================
*/
void CL_ParseClientdata (void)
{
	int		i, j;
	int		bits; //johnfitz

	bits = (unsigned short)MSG_ReadShort (); //johnfitz -- read bits here isntead of in CL_ParseServerMessage()

	if (bits & SU_EXTEND1)
		bits |= (MSG_ReadByte() << 16);
	if (bits & SU_EXTEND2)
		bits |= (MSG_ReadByte() << 24);

	if (bits & SU_VIEWHEIGHT)
		cl.viewheight = MSG_ReadChar ();
	else
		cl.viewheight = DEFAULT_VIEWHEIGHT;

	if (bits & SU_IDEALPITCH)
		cl.idealpitch = MSG_ReadChar ();
	else
		cl.idealpitch = 0;

	// preserve initial angles (mostly for savegames)
	if (cls.signon < SIGNONS)
		V_StopPitchDrift ();

	VectorCopy (cl.mvelocity[0], cl.mvelocity[1]);
	for (i = 0; i < 3; i++)
	{
		if (bits & (SU_PUNCH1<<i) )
			cl.punchangle[i] = MSG_ReadChar();
		else
			cl.punchangle[i] = 0;

		if (bits & (SU_VELOCITY1<<i) )
			cl.mvelocity[0][i] = MSG_ReadChar()*16;
		else
			cl.mvelocity[0][i] = 0;
	}

	//johnfitz -- update v_punchangles
	if (v_punchangles[0][0] != cl.punchangle[0] || v_punchangles[0][1] != cl.punchangle[1] || v_punchangles[0][2] != cl.punchangle[2])
	{
		VectorCopy (v_punchangles[0], v_punchangles[1]);
		VectorCopy (cl.punchangle, v_punchangles[0]);
		cl.punchtime = cl.time;
	}
	//johnfitz

// [always sent]	if (bits & SU_ITEMS)
		i = MSG_ReadLong ();

	if (cl.items != i)
	{	// set flash times
		Sbar_Changed ();
		for (j = 0; j < 32; j++)
			if ( (i & (1<<j)) && !(cl.items & (1<<j)))
				cl.item_gettime[j] = cl.time;
		cl.items = i;
		cl.stats[STAT_ITEMS] = i;
	}

	cl.onground = (bits & SU_ONGROUND) != 0;
	cl.inwater = (bits & SU_INWATER) != 0;

	if (bits & SU_WEAPONFRAME)
		cl.stats[STAT_WEAPONFRAME] = MSG_ReadByte ();
	else
		cl.stats[STAT_WEAPONFRAME] = 0;

	if (bits & SU_ARMOR)
		i = MSG_ReadByte ();
	else
		i = 0;
	if (cl.stats[STAT_ARMOR] != i)
	{
		cl.stats[STAT_ARMOR] = i;
		Sbar_Changed ();
	}

	if (bits & SU_WEAPON)
		i = MSG_ReadByte ();
	else
		i = 0;
	if (cl.stats[STAT_WEAPON] != i)
	{
		cl.stats[STAT_WEAPON] = i;
		Sbar_Changed ();
	}

	i = MSG_ReadShort ();
	if (cl.stats[STAT_HEALTH] != i)
	{
		cl.stats[STAT_HEALTH] = i;
		Sbar_Changed ();
	}

	i = MSG_ReadByte ();
	if (cl.stats[STAT_AMMO] != i)
	{
		cl.stats[STAT_AMMO] = i;
		Sbar_Changed ();
	}

	for (i = 0; i < 4; i++)
	{
		j = MSG_ReadByte ();
		if (cl.stats[STAT_SHELLS+i] != j)
		{
			cl.stats[STAT_SHELLS+i] = j;
			Sbar_Changed ();
		}
	}

	i = MSG_ReadByte ();

	if (standard_quake)
	{
		if (cl.stats[STAT_ACTIVEWEAPON] != i)
		{
			cl.stats[STAT_ACTIVEWEAPON] = i;
			Sbar_Changed ();
		}
	}
	else
	{
		if (cl.stats[STAT_ACTIVEWEAPON] != (1<<i))
		{
			cl.stats[STAT_ACTIVEWEAPON] = (1<<i);
			Sbar_Changed ();
		}
	}

	if (bits & SU_WEAPON2)
		cl.stats[STAT_WEAPON] |= (MSG_ReadByte() << 8);
	if (bits & SU_ARMOR2)
		cl.stats[STAT_ARMOR] |= (MSG_ReadByte() << 8);
	if (bits & SU_AMMO2)
		cl.stats[STAT_AMMO] |= (MSG_ReadByte() << 8);
	if (bits & SU_SHELLS2)
		cl.stats[STAT_SHELLS] |= (MSG_ReadByte() << 8);
	if (bits & SU_NAILS2)
		cl.stats[STAT_NAILS] |= (MSG_ReadByte() << 8);
	if (bits & SU_ROCKETS2)
		cl.stats[STAT_ROCKETS] |= (MSG_ReadByte() << 8);
	if (bits & SU_CELLS2)
		cl.stats[STAT_CELLS] |= (MSG_ReadByte() << 8);
	if (bits & SU_WEAPONFRAME2)
		cl.stats[STAT_WEAPONFRAME] |= (MSG_ReadByte() << 8);
	if (bits & SU_WEAPONALPHA)
		cl.viewent.alpha = MSG_ReadByte();
	else
		cl.viewent.alpha = ENTALPHA_DEFAULT;

	// svc_clientdata carries pmove-critical fields; snapshot2 owns local player position/orientation.
	if (bits & SU_PREDICT)
	{
		unsigned int ack = (unsigned int)MSG_ReadLong ();
		unsigned int ack_echo = (unsigned int)MSG_ReadLong ();
		vec3_t origin;
		vec3_t velocity;
		vec3_t viewangles;

		if (NETSEQ_GT (ack, cl.last_cmd_ack))
			cl.last_cmd_ack = ack;
		if (NETSEQ_GT (ack_echo, cl.last_cmd_ack_echo))
			cl.last_cmd_ack_echo = ack_echo;

		for (i = 0; i < 3; i++)
			origin[i] = MSG_ReadCoord (cl.protocolflags);
		for (i = 0; i < 3; i++)
			velocity[i] = MSG_ReadCoord (cl.protocolflags);
		for (i = 0; i < 3; i++)
			viewangles[i] = MSG_ReadAngle16 (cl.protocolflags);

		CL_Predict_ServerUpdate (ack, origin, velocity, viewangles, cl.onground);
	}

	CL_SetHudStat (STAT_WEAPONFRAME);
	CL_SetHudStat (STAT_ARMOR);
	CL_SetHudStat (STAT_WEAPON);
	CL_SetHudStat (STAT_ACTIVEWEAPON);
	CL_SetHudStat (STAT_HEALTH);
	CL_SetHudStat (STAT_AMMO);
	CL_SetHudStat (STAT_SHELLS);
	CL_SetHudStat (STAT_NAILS);
	CL_SetHudStat (STAT_ROCKETS);
	CL_SetHudStat (STAT_CELLS);

	//johnfitz -- lerping
	//ericw -- this was done before the upper 8 bits of cl.stats[STAT_WEAPON] were filled in, breaking on large maps like zendar.bsp
	if (cl.viewent.model != cl.model_precache[cl.stats[STAT_WEAPON]])
	{
		cl.viewent.lerpflags |= LERP_RESETANIM; //don't lerp animation across model changes
	}
	//johnfitz
}

/*
=====================
CL_NewTranslation
=====================
*/
void CL_NewTranslation (int slot)
{
	int		i, j;
	int		top, bottom;
	byte	*dest, *source;

	if (slot > cl.maxclients)
		Sys_Error ("CL_NewTranslation: slot > cl.maxclients");
	dest = cl.scores[slot].translations;
	source = vid.colormap;
	memcpy (dest, vid.colormap, sizeof(cl.scores[slot].translations));
	top = cl.scores[slot].colors & 0xf0;
	bottom = (cl.scores[slot].colors &15)<<4;
	R_TranslatePlayerSkin (slot);

	for (i = 0; i < VID_GRADES; i++, dest += 256, source+=256)
	{
		if (top < 128)	// the artists made some backwards ranges.  sigh.
			memcpy (dest + TOP_RANGE, source + top, 16);
		else
		{
			for (j = 0; j < 16; j++)
				dest[TOP_RANGE+j] = source[top+15-j];
		}

		if (bottom < 128)
			memcpy (dest + BOTTOM_RANGE, source + bottom, 16);
		else
		{
			for (j = 0; j < 16; j++)
				dest[BOTTOM_RANGE+j] = source[bottom+15-j];
		}
	}
}

/*
=====================
CL_ParseStatic
=====================
*/
void CL_ParseStatic (int version) //johnfitz -- added a parameter
{
	entity_t *ent;
	int		i;

	i = cl.num_statics;
	if (i >= MAX_STATIC_ENTITIES)
		Host_Error ("Too many static entities");

	ent = &cl_static_entities[i];
	cl.num_statics++;
	CL_ParseBaseline (ent, version); //johnfitz -- added second parameter

// copy it to the current state

	ent->model = cl.model_precache[ent->baseline.modelindex];
	ent->lerpflags |= LERP_RESETANIM; //johnfitz -- lerping
	ent->frame = ent->baseline.frame;

	ent->colormap = vid.colormap;
	ent->skinnum = ent->baseline.skin;
	ent->effects = ent->baseline.effects;
	ent->alpha = ent->baseline.alpha; //johnfitz -- alpha
	ent->scale = ent->baseline.scale;
	VectorCopy (ent->baseline.origin, ent->origin);
	VectorCopy (ent->baseline.angles, ent->angles);
	R_AddEfrags (ent);
}

/*
===================
CL_ParseStaticSound
===================
*/
void CL_ParseStaticSound (int version) //johnfitz -- added argument
{
	vec3_t		org;
	int			sound_num, vol, atten;
	int			i;

	for (i = 0; i < 3; i++)
		org[i] = MSG_ReadCoord (cl.protocolflags);

	if (version == 2)
		sound_num = MSG_ReadShort ();
	else
		sound_num = MSG_ReadByte ();

	vol = MSG_ReadByte ();
	atten = MSG_ReadByte ();

	S_StaticSound (cl.sound_precache[sound_num], org, vol, atten);
}


#if 0	/* for debugging. from fteqw. */
static void CL_DumpPacket (void)
{
	int			i, pos;
	unsigned char	*packet = net_message.data;

	Con_Printf("CL_DumpPacket, BEGIN:\n");
	pos = 0;
	while (pos < net_message.cursize)
	{
		Con_Printf("%5i ", pos);
		for (i = 0; i < 16; i++)
		{
			if (pos >= net_message.cursize)
				Con_Printf(" X ");
			else	Con_Printf("%2x ", packet[pos]);
			pos++;
		}
		pos -= 16;
		for (i = 0; i < 16; i++)
		{
			if (pos >= net_message.cursize)
				Con_Printf("X");
			else if (packet[pos] == 0)
				Con_Printf(".");
			else	Con_Printf("%c", packet[pos]);
			pos++;
		}
		Con_Printf("\n");
	}

	Con_Printf("CL_DumpPacket, --- END ---\n");
}
#endif	/* CL_DumpPacket */

#define SHOWNET(x) if(cl_shownet.value==2)Con_Printf ("%3i:%s\n", msg_readcount-1, x);

//mods and servers might not send the \n instantly.
//some mods bug out and omit the \n entirely, this function helps prevent the damage from spreading too much.
//some servers or mods use //prefixed commands as extensions to avoid spam about unrecognised commands.
static void CL_ParseStuffText(const char *msg)
{
	const char *str;
	q_strlcat(cl.stuffcmdbuf, msg, sizeof(cl.stuffcmdbuf));
	for (; (str = strchr(cl.stuffcmdbuf, '\n')); memmove(cl.stuffcmdbuf, str, Q_strlen(str)+1))
	{
		qboolean handled = false;

		str++;//skip past the \n

		//handle special commands
		if (cl.stuffcmdbuf[0] == '/' && cl.stuffcmdbuf[1] == '/')
		{
			handled = Cmd_ExecuteString(cl.stuffcmdbuf+2, src_server);
			if (!handled)
				Con_DPrintf("Server sent unknown command %s\n", Cmd_Argv(0));
		}
		else
			handled = Cmd_ExecuteString(cl.stuffcmdbuf, src_server);

		//let the server exec general user commands (massive security hole)
		if (!handled)
			Cbuf_AddTextLen(cl.stuffcmdbuf, str-cl.stuffcmdbuf);
	}
}

/*
=====================
CL_ParseServerMessage
=====================
*/
void CL_ParseServerMessage (void)
{
	int			cmd;
	int			i;
	const char		*str; //johnfitz
	int			lastcmd; //johnfitz
	int			lastcmd_offset;
	int			prevcmd;
	int			prevcmd_offset;
	int			cmd_offset;

	if (sys_step_debug.value > 0.0f)
		sys_step_debug_info.cl_parse_calls++;

//
// if recording demos, copy the message out
//
	if (cl_shownet.value == 1)
		Con_Printf ("%i ",net_message.cursize);
	else if (cl_shownet.value == 2)
		Con_Printf ("------------------\n");

//	cl.onground = false;	// unless the server says otherwise

//
// parse the message
//
	MSG_BeginReading ();

	CL_NetDebugHeader ();

	lastcmd = -1;
	lastcmd_offset = -1;
	prevcmd = -1;
	prevcmd_offset = -1;
	if (net_message.cursize <= 0)
	{
		CL_BadServerMessage ("empty server message", -1, msg_readcount,
			lastcmd, lastcmd_offset, prevcmd, prevcmd_offset);
		return;
	}

	while (1)
	{
		if (msg_badread)
		{
			CL_BadServerMessage ("message read past end", -1, msg_readcount,
				lastcmd, lastcmd_offset, prevcmd, prevcmd_offset);
			return;
		}

		cmd = MSG_ReadByte ();
		cmd_offset = msg_readcount - 1;

		if (cmd == -1)
		{
			SHOWNET("END OF MESSAGE");

			if (*cl.stuffcmdbuf && net_message.cursize < 512)
				CL_ParseStuffText("\n");	//there's a few mods that forget to write \ns, that then fuck up other things too. So make sure it gets flushed to the cbuf. the cursize check is to reduce backbuffer overflows that would give a false positive.

			CL_FinishDemoFrame ();
			return;		// end of message
		}

		if (cmd == svc_bad)
		{
			CL_BadServerMessage ("svc_bad", cmd, cmd_offset,
				lastcmd, lastcmd_offset, prevcmd, prevcmd_offset);
			return;
		}

		// fast updates are not supported
		if (cmd & 128)
		{
			CL_BadServerMessage ("unsupported fast update", cmd, cmd_offset,
				lastcmd, lastcmd_offset, prevcmd, prevcmd_offset);
			return;
		}

		if (cmd < (int)NUM_SVC_STRINGS) {
			SHOWNET(svc_strings[cmd]);
		}
		if (cl_netdebug_parse.value)
			Con_Printf ("NETDBG: cmd %s (%d) @%d\n", CL_SvcName (cmd), cmd, cmd_offset);
		if (cl_netdebug_parse.value)
			JITTER_LOG ("NETDBG: cmd %s (%d) @%d\n", CL_SvcName (cmd), cmd, cmd_offset);

	// other commands
		switch (cmd)
		{
		default:
			CL_BadServerMessage ("unknown command", cmd, cmd_offset,
				lastcmd, lastcmd_offset, prevcmd, prevcmd_offset);
			return;

		case svc_nop:
		//	Con_Printf ("svc_nop\n");
			break;

		case svc_time:
		{
			double server_time = MSG_ReadFloat ();

			cl.mtime[1] = cl.mtime[0];
			cl.mtime[0] = server_time;
			CL_UpdateServerTimeEstimate (server_time, "svc_time");
			cl.fixangle = false;
			if (cls.signon == SIGNONS - 1)
			{	// allow map loads with no visible entity updates to finish signon
				cls.signon = SIGNONS;
				CL_SignonReply ();
			}
			break;
		}

		case svc_clientdata:
			CL_ParseClientdata (); //johnfitz -- removed bits parameter, we will read this inside CL_ParseClientdata()
			break;

		case svc_version:
			i = MSG_ReadLong ();
			if (i != PROTOCOL_RMQ)
				Host_Error ("Server returned protocol %i, expected %i", i, PROTOCOL_RMQ);
			cl.protocol = PROTOCOL_RMQ;
			break;

		case svc_disconnect:
			Host_EndGame ("Server disconnected\n");

		case svc_print:
			Con_Printf ("%s", MSG_ReadString ());
			break;

		case svc_centerprint:
			//johnfitz -- log centerprints to console
			str = MSG_ReadString ();
			SCR_CenterPrint (str);
			Con_LogCenterPrint (str);
			//johnfitz
			break;

		case svc_stufftext:
			CL_ParseStuffText (MSG_ReadString ());
			break;

		case svc_damage:
			V_ParseDamage ();
			break;

		case svc_serverinfo:
			CL_ParseServerInfo ();
			vid.recalc_refdef = true;	// leave intermission full screen
			break;

		case svc_setangle:
			for (i=0 ; i<3 ; i++)
				cl.viewangles[i] = MSG_ReadAngle (cl.protocolflags);
			cl.fixangle = true;
			break;

		case svc_setview:
			// Viewentity is per-client and must be set immediately for snapshot2/clientdata merge.
			cl.viewentity = MSG_ReadShort ();
			CL_Predict_ResetGround ();
			break;

		case svc_lightstyle:
			i = MSG_ReadByte ();
			if (i >= MAX_LIGHTSTYLES)
				Sys_Error ("svc_lightstyle > MAX_LIGHTSTYLES");
			CL_SetLightstyle (i, MSG_ReadString ());
			break;

		case svc_sound:
			CL_ParseStartSoundPacket();
			break;

		case svc_stopsound:
			i = MSG_ReadShort();
			S_StopSound(i>>3, i&7);
			break;

		case svc_updatename:
			Sbar_Changed ();
			i = MSG_ReadByte ();
			if (i >= cl.maxclients)
				Host_Error ("CL_ParseServerMessage: svc_updatename > MAX_SCOREBOARD");
			q_strlcpy (cl.scores[i].name, MSG_ReadString(), MAX_SCOREBOARDNAME);
			break;

		case svc_updatefrags:
			Sbar_Changed ();
			i = MSG_ReadByte ();
			if (i >= cl.maxclients)
				Host_Error ("CL_ParseServerMessage: svc_updatefrags > MAX_SCOREBOARD");
			cl.scores[i].frags = MSG_ReadShort ();
			break;

		case svc_updatecolors:
			Sbar_Changed ();
			i = MSG_ReadByte ();
			if (i >= cl.maxclients)
				Host_Error ("CL_ParseServerMessage: svc_updatecolors > MAX_SCOREBOARD");
			cl.scores[i].colors = MSG_ReadByte ();
			CL_NewTranslation (i);
			break;

		case svc_particle:
			R_ParseParticleEffect ();
			break;

		case svc_spawnbaseline:
			i = MSG_ReadShort ();
			// must use CL_EntityNum() to force cl.num_entities up
			CL_ParseBaseline (CL_EntityNum(i), 1); // johnfitz -- added second parameter
			break;

		case svc_spawnstatic:
			CL_ParseStatic (1); //johnfitz -- added parameter
			break;

		case svc_temp_entity:
			CL_ParseTEnt ();
			break;

		case svc_setpause:
			cl.paused = MSG_ReadByte ();
			if (cl.paused)
			{
				CDAudio_Pause ();
				BGM_Pause ();
			}
			else
			{
				CDAudio_Resume ();
				BGM_Resume ();
			}
			break;

		case svc_signonnum:
			i = MSG_ReadByte ();
			if (i <= cls.signon)
				Host_Error ("Received signon %i when at %i", i, cls.signon);
			cls.signon = i;
			//johnfitz -- if signonnum==2, signon packet has been fully parsed, so check for excessive static ents and efrags
			if (i == 2)
			{
				if (cl.num_statics > 128)
					Con_DWarning ("%i static entities exceeds standard limit of 128 (max = %d).\n", cl.num_statics, MAX_STATIC_ENTITIES);
				R_CheckEfrags ();
			}
			//johnfitz
			CL_SignonReply ();
			break;

		case svc_signon_chunk:
		{
			byte stage = (byte)MSG_ReadByte ();
			unsigned short seq = (unsigned short)MSG_ReadShort ();
			byte flags = (byte)MSG_ReadByte ();
			unsigned short payload_len = (unsigned short)MSG_ReadShort ();
			const byte *payload;

			if (payload_len > MAX_RELIABLE_QUEUE_BYTES)
			{
				CL_BadServerMessage ("signon chunk length exceeds cap", cmd, cmd_offset,
					lastcmd, lastcmd_offset, prevcmd, prevcmd_offset);
				return;
			}

			if (payload_len > (unsigned short)(net_message.cursize - msg_readcount))
			{
				CL_BadServerMessage ("signon chunk length exceeds packet", cmd, cmd_offset,
					lastcmd, lastcmd_offset, prevcmd, prevcmd_offset);
				return;
			}

			payload = net_message.data + msg_readcount;
			msg_readcount += payload_len;

			if (cl_signon_chunk_debug.value)
				Con_Printf ("SIGNONCHUNK: stage %u seq %u len %u flags 0x%x\n",
					stage, seq, payload_len, flags);

			if (!cl.signon_chunking)
			{
				Con_Warning ("Received signon chunk without negotiation\n");
				break;
			}

			if (stage != cl.signon_chunk_stage && cl.signon_chunk_next_seq == 0)
				cl.signon_chunk_stage = stage;

			if (seq != cl.signon_chunk_next_seq)
			{
				if (cl_signon_chunk_debug.value)
					Con_Printf ("SIGNONCHUNK: expected seq %u, got %u (stage %u)\n",
						cl.signon_chunk_next_seq, seq, stage);
				CL_EnsureReliableControlSpace (1 + 1 + 2, "signon_ack_resend");
				MSG_BEGINCLC (&cls.message, clc_signon_ack);
				MSG_DBGAUX (&cls.message, stage);
				MSG_WriteByte (&cls.message, clc_signon_ack);
				MSG_WriteByte (&cls.message, cl.signon_chunk_stage);
				MSG_WriteShort (&cls.message, cl.signon_chunk_next_seq);
				break;
			}

			CL_ParseSignonChunkPayload (payload, payload_len, stage, seq);
			cl.signon_chunk_next_seq++;
			if (flags & SIGNON_CHUNK_FLAG_STAGE_END)
				cl.signon_chunk_stage = stage + 1;

			CL_EnsureReliableControlSpace (1 + 1 + 2, "signon_ack");
			MSG_BEGINCLC (&cls.message, clc_signon_ack);
			MSG_DBGAUX (&cls.message, stage);
			MSG_WriteByte (&cls.message, clc_signon_ack);
			MSG_WriteByte (&cls.message, stage);
			MSG_WriteShort (&cls.message, cl.signon_chunk_next_seq);
			break;
		}

		case svc_killedmonster:
			cl.stats[STAT_MONSTERS]++;
			cl.statsf[STAT_MONSTERS] = cl.stats[STAT_MONSTERS];
			break;

		case svc_foundsecret:
			cl.stats[STAT_SECRETS]++;
			cl.statsf[STAT_SECRETS] = cl.stats[STAT_SECRETS];
			break;

		case svc_updatestat:
			i = MSG_ReadByte ();
			if (i < 0 || i >= MAX_CL_STATS)
				Sys_Error ("svc_updatestat: %i is invalid", i);
			cl.stats[i] = MSG_ReadLong ();
			cl.statsf[i] = cl.stats[i];
			break;

		case svc_spawnstaticsound:
			CL_ParseStaticSound (1); //johnfitz -- added parameter
			break;

		case svc_cdtrack:
			cl.cdtrack = MSG_ReadByte ();
			cl.looptrack = MSG_ReadByte ();
			if ( (cls.demoplayback || cls.demorecording) && (cls.forcetrack != -1) )
				BGM_PlayCDtrack ((byte)cls.forcetrack, true);
			else
				BGM_PlayCDtrack ((byte)cl.cdtrack, true);
			break;

		case svc_intermission:
			cl.intermission = 1;
			cl.completed_time = cl.time;
			vid.recalc_refdef = true;	// go to full screen
			V_RestoreAngles ();
			break;

		case svc_finale:
			cl.intermission = 2;
			cl.completed_time = cl.time;
			vid.recalc_refdef = true;	// go to full screen
			//johnfitz -- log centerprints to console
			str = MSG_ReadString ();
			SCR_CenterPrint (str);
			Con_LogCenterPrint (str);
			//johnfitz
			V_RestoreAngles ();
			break;

		case svc_cutscene:
			cl.intermission = 3;
			cl.completed_time = cl.time;
			vid.recalc_refdef = true;	// go to full screen
			//johnfitz -- log centerprints to console
			str = MSG_ReadString ();
			SCR_CenterPrint (str);
			Con_LogCenterPrint (str);
			//johnfitz
			V_RestoreAngles ();
			break;

		case svc_sellscreen:
			Cmd_ExecuteString ("help", src_command);
			break;

		//johnfitz -- new svc types
		case svc_skybox:
			Sky_LoadSkyBox (MSG_ReadString());
			break;

		case svc_bf:
			Cmd_ExecuteString ("bf", src_command);
			break;

		case svc_fog:
			Fog_ParseServerMessage ();
			break;

		case svc_spawnbaseline2:
			i = MSG_ReadShort ();
			// must use CL_EntityNum() to force cl.num_entities up
			CL_ParseBaseline (CL_EntityNum(i), 2);
			break;

		case svc_spawnstatic2:
			CL_ParseStatic (2);
			break;

		case svc_spawnstaticsound2:
			CL_ParseStaticSound (2);
			break;
		//johnfitz

		//used by the 2021 rerelease
		case svc_achievement:
			str = MSG_ReadString();
			if (cls.demoplayback)
				Con_DPrintf ("Ignoring svc_achievement (%s)\n", str);
			else if (!Steam_SetAchievement (str))
				Con_DPrintf ("Couldn't set achievement \"%s\"\n", str);
			break;
		case svc_localsound:
			CL_ParseLocalSound();
			break;

		case svc_snapshot_full:
			CL_ParseSnapshotFull ();
			break;

		case svc_snapshot_delta:
			CL_ParseSnapshotDelta ();
			break;

		case svc_snapshot2:
			CL_ParseSnapshot2 ();
			break;

		}

		if (msg_badread)
		{
			CL_BadServerMessage ("message read past end", cmd, cmd_offset,
				lastcmd, lastcmd_offset, prevcmd, prevcmd_offset);
			return;
		}
		if (cl_netdebug_parse.value)
		{
			int cmd_end = CL_NetDebugReadcount ();
			if (cmd_end < cmd_offset)
				cmd_end = cmd_offset;
			Con_Printf ("NETDBG: cmd %s end %d bytes %d\n",
				CL_SvcName (cmd), cmd_end, cmd_end - cmd_offset);
			JITTER_LOG ("NETDBG: cmd %s end %d bytes %d\n",
				CL_SvcName (cmd), cmd_end, cmd_end - cmd_offset);
		}

		prevcmd = lastcmd;
		prevcmd_offset = lastcmd_offset;
		lastcmd = cmd; //johnfitz
		lastcmd_offset = cmd_offset;
	}
}
