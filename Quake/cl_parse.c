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
	"svc_packedentities", // 59
	"svc_snapshot2", // 60
	"svc_signon_chunk" // 61
};
#define NUM_SVC_STRINGS Q_COUNTOF(svc_strings)

static void CL_RequestFullSnapshot (const char *reason, qboolean count_parse_error);
static void CL_ApplySnapshotOrigin (vec3_t out, const vec3_t in);

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
	if (cls.netcon)
	{
		Con_Printf ("NETDBG: from %s rseq %u sseq %u ack %u unrel %u\n",
			NET_QSocketGetAddressString (cls.netcon),
			cls.netcon->receiveSequence,
			cls.netcon->sendSequence,
			cls.netcon->ackSequence,
			cls.netcon->unreliableReceiveSequence);
	}
	if (net_last_incoming.valid)
	{
		Con_Printf ("NETDBG: packet seq %u flags 0x%x len %u payload %u %s\n",
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
static double cl_unknown_cmd_next_warn_time = 0.0;

static void CL_BadServerMessage (const char *reason, int cmd, int cmd_offset,
	int lastcmd, int lastcmd_offset, int prevcmd, int prevcmd_offset)
{
	if (cl_netdebug_parse.value || cl_netdebug_hexdump.value)
	{
		Con_Printf ("NETDBG: bad server message: %s\n", reason);
		Con_Printf ("NETDBG: readcount %d/%d cmd %d (%s) @%d last %d (%s) @%d prev %d (%s) @%d badread %d\n",
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
		Con_Printf ("Malformed server message during signon: %s\n", reason);
		CL_Disconnect ();
		return;
	}

	if (realtime >= cl_badmsg_next_warn_time)
	{
		Con_Printf ("Dropping malformed server message: %s\n", reason);
		cl_badmsg_next_warn_time = realtime + 1.0;
	}
}

qboolean warn_about_nehahra_protocol; //johnfitz

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

	//johnfitz -- PROTOCOL_FITZQUAKE
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
	//johnfitz

	//johnfitz -- check soundnum
	if (sound_num >= MAX_SOUNDS)
		Host_Error ("CL_ParseStartSoundPacket: %i > MAX_SOUNDS", sound_num);
	//johnfitz

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
	//johnfitz -- support multiple protocols
	if (i != PROTOCOL_NETQUAKE && i != PROTOCOL_FITZQUAKE && i != PROTOCOL_RMQ) {
		Con_Printf ("\n"); //because there's no newline after serverinfo print
		Host_Error ("Server returned version %i, not %i or %i or %i", i, PROTOCOL_NETQUAKE, PROTOCOL_FITZQUAKE, PROTOCOL_RMQ);
	}
	cl.protocol = i;
	//johnfitz

	if (cl.protocol == PROTOCOL_RMQ)
	{
		const unsigned int supportedflags = (PRFL_SHORTANGLE | PRFL_FLOATANGLE | PRFL_24BITCOORD | PRFL_FLOATCOORD | PRFL_EDICTSCALE | PRFL_INT32COORD | PRFL_SNAPSHOT_HIRES | PRFL_SIGNON_CHUNKS);
		
		// mh - read protocol flags from server so that we know what protocol features to expect
		cl.protocolflags = (unsigned int) MSG_ReadLong ();
		
		if (0 != (cl.protocolflags & (~supportedflags)))
		{
			Con_Warning("PROTOCOL_RMQ protocolflags %i contains unsupported flags\n", cl.protocolflags);
		}
		cl.signon_chunking = (cl.protocolflags & PRFL_SIGNON_CHUNKS) != 0;
	}
	else
	{
		cl.protocolflags = 0;
		cl.signon_chunking = false;
	}
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

//johnfitz -- tell user which protocol this is
	Con_Printf ("Using protocol %i\n", i);

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

/*
==================
CL_ParseUpdate

Parse an entity update message from the server
If an entities model or origin changes from frame to frame, it must be
relinked.  Other attributes can change without relinking.
==================
*/
void CL_ParseUpdate (int bits)
{
	int		i;
	qmodel_t	*model;
	int		modnum;
	qboolean	forcelink;
	entity_t	*ent;
	int		num;
	int		skin;
	int		prevframe;

	if (cls.signon == SIGNONS - 1)
	{	// first update is the final signon stage
		cls.signon = SIGNONS;
		CL_SignonReply ();
	}

	if (bits & U_MOREBITS)
	{
		i = MSG_ReadByte ();
		bits |= (i<<8);
	}

	//johnfitz -- PROTOCOL_FITZQUAKE
	if (cl.protocol == PROTOCOL_FITZQUAKE || cl.protocol == PROTOCOL_RMQ)
	{
		if (bits & U_EXTEND1)
			bits |= MSG_ReadByte() << 16;
		if (bits & U_EXTEND2)
			bits |= MSG_ReadByte() << 24;
	}
	//johnfitz

	if (bits & U_LONGENTITY)
		num = MSG_ReadShort ();
	else
		num = MSG_ReadByte ();

	ent = CL_EntityNum (num);

	if (ent->msgtime != cl.mtime[1])
		forcelink = true;	// no previous frame to lerp from
	else
		forcelink = false;

	//johnfitz -- lerping
	if (ent->msgtime + 0.2 < cl.mtime[0]) //more than 0.2 seconds since the last message (most entities think every 0.1 sec)
		ent->lerpflags |= LERP_RESETANIM; //if we missed a think, we'd be lerping from the wrong frame
	//johnfitz

	ent->msgtime = cl.mtime[0];

	if (bits & U_MODEL)
	{
		modnum = MSG_ReadByte ();
		if (modnum >= MAX_MODELS)
			Host_Error ("CL_ParseModel: bad modnum");
	}
	else
		modnum = ent->baseline.modelindex;

	prevframe = ent->frame;
	if (bits & U_FRAME)
		ent->frame = MSG_ReadByte ();
	else
		ent->frame = ent->baseline.frame;

	if (bits & U_COLORMAP)
		i = MSG_ReadByte();
	else
		i = ent->baseline.colormap;
	if (!i)
		ent->colormap = vid.colormap;
	else
	{
		if (i > cl.maxclients)
			Sys_Error ("i >= cl.maxclients");
		ent->colormap = cl.scores[i-1].translations;
	}
	if (bits & U_SKIN)
		skin = MSG_ReadByte();
	else
		skin = ent->baseline.skin;
	if (skin != ent->skinnum)
	{
		ent->skinnum = skin;
		if (num > 0 && num <= cl.maxclients)
			R_TranslateNewPlayerSkin (num - 1); //johnfitz -- was R_TranslatePlayerSkin
	}
	if (bits & U_EFFECTS)
		ent->effects = MSG_ReadByte();
	else
		ent->effects = ent->baseline.effects;

// shift the known values for interpolation
	VectorCopy (ent->msg_origins[0], ent->msg_origins[1]);
	VectorCopy (ent->msg_angles[0], ent->msg_angles[1]);

	if (bits & U_ORIGIN1)
		ent->msg_origins[0][0] = MSG_ReadCoord (cl.protocolflags);
	else
		ent->msg_origins[0][0] = ent->baseline.origin[0];
	if (bits & U_ANGLE1)
		ent->msg_angles[0][0] = MSG_ReadAngle(cl.protocolflags);
	else
		ent->msg_angles[0][0] = ent->baseline.angles[0];

	if (bits & U_ORIGIN2)
		ent->msg_origins[0][1] = MSG_ReadCoord (cl.protocolflags);
	else
		ent->msg_origins[0][1] = ent->baseline.origin[1];
	if (bits & U_ANGLE2)
		ent->msg_angles[0][1] = MSG_ReadAngle(cl.protocolflags);
	else
		ent->msg_angles[0][1] = ent->baseline.angles[1];

	if (bits & U_ORIGIN3)
		ent->msg_origins[0][2] = MSG_ReadCoord (cl.protocolflags);
	else
		ent->msg_origins[0][2] = ent->baseline.origin[2];
	if (bits & U_ANGLE3)
		ent->msg_angles[0][2] = MSG_ReadAngle(cl.protocolflags);
	else
		ent->msg_angles[0][2] = ent->baseline.angles[2];

	//johnfitz -- lerping for movetype_step entities
	if (bits & U_STEP)
	{
		ent->lerpflags |= LERP_MOVESTEP;
		ent->forcelink = true;
	}
	else
		ent->lerpflags &= ~LERP_MOVESTEP;
	//johnfitz

	//johnfitz -- PROTOCOL_FITZQUAKE and PROTOCOL_NEHAHRA
	if (cl.protocol == PROTOCOL_FITZQUAKE || cl.protocol == PROTOCOL_RMQ)
	{
		if (bits & U_ALPHA)
			ent->alpha = MSG_ReadByte();
		else
			ent->alpha = ent->baseline.alpha;
		if (bits & U_SCALE)
			ent->scale = MSG_ReadByte();
		else
			ent->scale = ent->baseline.scale;
		if (bits & U_FRAME2)
			ent->frame = (ent->frame & 0x00FF) | (MSG_ReadByte() << 8);
		if (bits & U_MODEL2)
			modnum = (modnum & 0x00FF) | (MSG_ReadByte() << 8);
		if (bits & U_LERPFINISH)
		{
			ent->lerpfinish = ent->msgtime + ((float)(MSG_ReadByte()) / 255);
			ent->lerpflags |= LERP_FINISH;
		}
		else
			ent->lerpflags &= ~LERP_FINISH;
	}
	else if (cl.protocol == PROTOCOL_NETQUAKE)
	{
		//HACK: if this bit is set, assume this is PROTOCOL_NEHAHRA
		if (bits & U_TRANS)
		{
			float a, b;

			if (warn_about_nehahra_protocol)
			{
				Con_Warning ("nonstandard update bit, assuming Nehahra protocol\n");
				warn_about_nehahra_protocol = false;
			}

			a = MSG_ReadFloat();
			b = MSG_ReadFloat(); //alpha
			if (a == 2)
				MSG_ReadFloat(); //fullbright (not using this yet)
			ent->alpha = ENTALPHA_ENCODE(b);
		}
		else
			ent->alpha = ent->baseline.alpha;
		ent->scale = ent->baseline.scale;
	}
	//johnfitz

	//johnfitz -- moved here from above
	model = cl.model_precache[modnum];
	if (model != ent->model)
	{
		ent->model = model;
	// automatic animation (torches, etc) can be either all together
	// or randomized
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
			forcelink = true;	// hack to make null model players work
		if (num > 0 && num <= cl.maxclients)
			R_TranslateNewPlayerSkin (num - 1); //johnfitz -- was R_TranslatePlayerSkin

		ent->lerpflags |= LERP_RESETANIM; //johnfitz -- don't lerp animation across model changes
	}
	//johnfitz
	else if (model && model->synctype == ST_FRAMETIME && ent->frame != prevframe)
		ent->syncbase = -cl.time;

	if ( forcelink )
	{	// didn't have an update last message
		VectorCopy (ent->msg_origins[0], ent->msg_origins[1]);
		VectorCopy (ent->msg_origins[0], ent->origin);
		VectorCopy (ent->msg_angles[0], ent->msg_angles[1]);
		VectorCopy (ent->msg_angles[0], ent->angles);
		ent->forcelink = true;
	}
}

static qboolean CL_ReadPackedByte (int *out)
{
	if (msg_readcount + 1 > net_message.cursize)
		return false;
	*out = net_message.data[msg_readcount];
	msg_readcount++;
	return true;
}

static qboolean CL_ReadPackedInt16 (short *out)
{
	if (msg_readcount + 2 > net_message.cursize)
		return false;
	*out = (short)(net_message.data[msg_readcount]
		+ (net_message.data[msg_readcount+1] << 8));
	msg_readcount += 2;
	return true;
}

static qboolean CL_ReadPackedUInt16 (unsigned int *out)
{
	if (msg_readcount + 2 > net_message.cursize)
		return false;
	*out = (unsigned int)net_message.data[msg_readcount]
		| ((unsigned int)net_message.data[msg_readcount+1] << 8);
	msg_readcount += 2;
	return true;
}

static qboolean CL_PackedOriginIsSane (const vec3_t origin)
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

static void CL_ParsePackedEntities (void)
{
	unsigned int count;
	unsigned int i;

	if (cls.signon == SIGNONS - 1)
	{	// first update is the final signon stage
		cls.signon = SIGNONS;
		CL_SignonReply ();
	}

	if (!CL_ReadPackedUInt16 (&count))
	{
		Con_DPrintf ("CL_ParsePackedEntities: short read on count\n");
		msg_readcount = net_message.cursize;
		return;
	}

	for (i = 0; i < count; i++)
	{
		entity_t *ent;
		qmodel_t *model;
		qboolean forcelink;
		unsigned int entnum;
		unsigned int lowmask;
		unsigned int highmask = 0;
		uint32_t mask;
		int modnum;
		int skin;
		int prevframe;
		int temp;
		short coord;
		qboolean origin_full;
		vec3_t parsed_origin;

		if (!CL_ReadPackedUInt16 (&entnum))
			goto short_read;
		if (!CL_ReadPackedUInt16 (&lowmask))
			goto short_read;
		if (lowmask & PACKEDENT_MASK_EXTEND)
		{
			if (!CL_ReadPackedUInt16 (&highmask))
				goto short_read;
			mask = ((uint32_t)highmask << 16) | (lowmask & ~PACKEDENT_MASK_EXTEND);
		}
		else
			mask = lowmask;

		if (mask & ~PACKEDENT_MASK_VALID)
		{
			Con_DPrintf ("CL_ParsePackedEntities: invalid mask 0x%x\n", mask);
			msg_readcount = net_message.cursize;
			return;
		}

		if (entnum >= (unsigned int)cl_max_edicts)
		{
			Con_DPrintf ("CL_ParsePackedEntities: invalid entnum %u\n", entnum);
			msg_readcount = net_message.cursize;
			return;
		}

		ent = CL_EntityNum ((int)entnum);

		if (ent->msgtime != cl.mtime[1])
			forcelink = true;	// no previous frame to lerp from
		else
			forcelink = false;

		if (ent->msgtime + 0.2 < cl.mtime[0])
			ent->lerpflags |= LERP_RESETANIM;

		ent->msgtime = cl.mtime[0];

		if (mask & PACKEDENT_MASK_MODEL)
		{
			unsigned int umod;
			if (!CL_ReadPackedUInt16 (&umod))
				goto short_read;
			modnum = (int)umod;
			if (modnum >= MAX_MODELS)
			{
				Con_DPrintf ("CL_ParsePackedEntities: bad modnum\n");
				msg_readcount = net_message.cursize;
				return;
			}
		}
		else
			modnum = ent->baseline.modelindex;

		prevframe = ent->frame;
		if (mask & PACKEDENT_MASK_FRAME)
		{
			unsigned int uframe;
			if (!CL_ReadPackedUInt16 (&uframe))
				goto short_read;
			ent->frame = (int)uframe;
		}
		else
			ent->frame = ent->baseline.frame;

		if (mask & PACKEDENT_MASK_COLORMAP)
		{
			if (!CL_ReadPackedByte (&temp))
				goto short_read;
		}
		else
			temp = ent->baseline.colormap;

		if (!temp)
			ent->colormap = vid.colormap;
		else
		{
			if (temp > cl.maxclients)
			{
				Con_DPrintf ("CL_ParsePackedEntities: bad colormap\n");
				msg_readcount = net_message.cursize;
				return;
			}
			ent->colormap = cl.scores[temp-1].translations;
		}

		if (mask & PACKEDENT_MASK_SKIN)
		{
			if (!CL_ReadPackedByte (&skin))
				goto short_read;
		}
		else
			skin = ent->baseline.skin;
		if (skin != ent->skinnum)
		{
			ent->skinnum = skin;
			if (entnum > 0 && entnum <= (unsigned int)cl.maxclients)
				R_TranslateNewPlayerSkin ((int)entnum - 1);
		}

		if (mask & PACKEDENT_MASK_EFFECTS)
		{
			if (!CL_ReadPackedByte (&ent->effects))
				goto short_read;
		}
		else
			ent->effects = ent->baseline.effects;

		VectorCopy (ent->msg_origins[0], ent->msg_origins[1]);
		VectorCopy (ent->msg_angles[0], ent->msg_angles[1]);

		origin_full = (mask & PACKEDENT_MASK_POS_FULL) != 0;
		VectorCopy (ent->baseline.origin, parsed_origin);

		if (mask & PACKEDENT_MASK_ORIGIN_X)
		{
			if (origin_full)
				parsed_origin[0] = MSG_ReadCoord (cl.protocolflags);
			else
			{
				if (!CL_ReadPackedInt16 (&coord))
					goto short_read;
				parsed_origin[0] = coord / PACKEDENT_POS_SCALE;
			}
		}

		if (mask & PACKEDENT_MASK_ANGLE_PITCH)
		{
			unsigned int angle;
			if (!CL_ReadPackedUInt16 (&angle))
				goto short_read;
			ent->msg_angles[0][0] = (float)angle * (360.0f / 65536.0f);
		}
		else
			ent->msg_angles[0][0] = ent->baseline.angles[0];

		if (mask & PACKEDENT_MASK_ORIGIN_Y)
		{
			if (origin_full)
				parsed_origin[1] = MSG_ReadCoord (cl.protocolflags);
			else
			{
				if (!CL_ReadPackedInt16 (&coord))
					goto short_read;
				parsed_origin[1] = coord / PACKEDENT_POS_SCALE;
			}
		}

		if (mask & PACKEDENT_MASK_ANGLE_YAW)
		{
			unsigned int angle;
			if (!CL_ReadPackedUInt16 (&angle))
				goto short_read;
			ent->msg_angles[0][1] = (float)angle * (360.0f / 65536.0f);
		}
		else
			ent->msg_angles[0][1] = ent->baseline.angles[1];

		if (mask & PACKEDENT_MASK_ORIGIN_Z)
		{
			if (origin_full)
				parsed_origin[2] = MSG_ReadCoord (cl.protocolflags);
			else
			{
				if (!CL_ReadPackedInt16 (&coord))
					goto short_read;
				parsed_origin[2] = coord / PACKEDENT_POS_SCALE;
			}
		}

		if (mask & PACKEDENT_MASK_ANGLE_ROLL)
		{
			unsigned int angle;
			if (!CL_ReadPackedUInt16 (&angle))
				goto short_read;
			ent->msg_angles[0][2] = (float)angle * (360.0f / 65536.0f);
		}
		else
			ent->msg_angles[0][2] = ent->baseline.angles[2];

		if (!CL_PackedOriginIsSane (parsed_origin))
		{
			CL_RequestFullSnapshot ("packedents origin guard", true);
			msg_readcount = net_message.cursize;
			return;
		}
		CL_ApplySnapshotOrigin (ent->msg_origins[0], parsed_origin);
		CL_ApplyEntityOrigin (ent, (int)entnum);

		if (mask & PACKEDENT_MASK_VEL_X)
		{
			if (!CL_ReadPackedInt16 (&coord))
				goto short_read;
			ent->packed_velocity[0] = coord / PACKEDENT_VEL_SCALE;
		}
		else
			ent->packed_velocity[0] = 0.0f;

		if (mask & PACKEDENT_MASK_VEL_Y)
		{
			if (!CL_ReadPackedInt16 (&coord))
				goto short_read;
			ent->packed_velocity[1] = coord / PACKEDENT_VEL_SCALE;
		}
		else
			ent->packed_velocity[1] = 0.0f;

		if (mask & PACKEDENT_MASK_VEL_Z)
		{
			if (!CL_ReadPackedInt16 (&coord))
				goto short_read;
			ent->packed_velocity[2] = coord / PACKEDENT_VEL_SCALE;
		}
		else
			ent->packed_velocity[2] = 0.0f;

		if (mask & PACKEDENT_MASK_ALPHA)
		{
			if (!CL_ReadPackedByte (&temp))
				goto short_read;
			ent->alpha = temp;
		}
		else
			ent->alpha = ent->baseline.alpha;

		if (mask & PACKEDENT_MASK_SCALE)
		{
			if (!CL_ReadPackedByte (&temp))
				goto short_read;
			ent->scale = temp;
		}
		else
			ent->scale = ent->baseline.scale;

		if (mask & PACKEDENT_MASK_LERPFINISH)
		{
			if (!CL_ReadPackedByte (&temp))
				goto short_read;
			ent->lerpfinish = ent->msgtime + ((float)temp / 255.0f);
			ent->lerpflags |= LERP_FINISH;
		}
		else
			ent->lerpflags &= ~LERP_FINISH;

		if (mask & PACKEDENT_MASK_STEP)
		{
			ent->lerpflags |= LERP_MOVESTEP;
			ent->forcelink = true;
		}
		else
			ent->lerpflags &= ~LERP_MOVESTEP;

		model = cl.model_precache[modnum];
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
			if (entnum > 0 && entnum <= (unsigned int)cl.maxclients)
				R_TranslateNewPlayerSkin ((int)entnum - 1);

			ent->lerpflags |= LERP_RESETANIM;
		}
		else if (model && model->synctype == ST_FRAMETIME && ent->frame != prevframe)
			ent->syncbase = -cl.time;

		if (forcelink)
		{
			VectorCopy (ent->msg_origins[0], ent->msg_origins[1]);
			VectorCopy (ent->msg_origins[0], ent->origin);
			VectorCopy (ent->msg_angles[0], ent->msg_angles[1]);
			VectorCopy (ent->msg_angles[0], ent->angles);
			ent->forcelink = true;
		}
	}

	return;

short_read:
	Con_DPrintf ("CL_ParsePackedEntities: short read\n");
	msg_readcount = net_message.cursize;
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
	return cl.snapshot_baseline_seq;
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
	NET_DebugLogEvent (true,
		"NETDBG time %.3f snap_ack_send seq %u\n",
		realtime, seq);
	return true;
}

static void CL_RequestFullSnapshot (const char *reason, qboolean count_parse_error)
{
	unsigned int ack_seq;

	if (cls.demoplayback || cls.state != ca_connected)
		return;
	if (count_parse_error)
		cl.snap_parse_errors++;
	NET_DebugLogEvent (true,
		"NETDBG time %.3f snap_abort reason %s parse_errors %d need_full %d\n",
		realtime, reason ? reason : "unknown", cl.snap_parse_errors, cl.need_full_snapshot ? 1 : 0);
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
}

static void CL_ClearSnapshotStage (void);
static void CL_PrepareFullSnapshotInterpReset (void);
static void CL_FinalizeFullSnapshotInterpReset (void);

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
	// Snapshot/packedent origins are model-space; offset by worldmodel origin to render in world-space.
	if (cl.worldmodel && cl.worldmodel->type == mod_brush
		&& cl.worldmodel->submodels && cl.worldmodel->numsubmodels > 0)
		VectorAdd (in, cl.worldmodel->submodels[0].origin, out);
	else
		VectorCopy (in, out);
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

static qboolean CL_SnapshotChunkTimedOut (void)
{
	double timeout_s = cl_full_reasm_timeout_ms.value * 0.001;

	if (!cl.snapshot_chunk_active)
		return false;
	if (timeout_s <= 0.0)
		return false;
	return (realtime - cl.snapshot_chunk_start_time) > timeout_s;
}

static void CL_MarkSnapshotEntityUpdated (int entnum)
{
	if (entnum <= 0 || entnum >= cl_max_edicts)
		return;
	if (cl.snapshot_active)
		cl.snapshot_active[entnum] = 1;
	if (cl.snapshot_last_update_time)
		cl.snapshot_last_update_time[entnum] = cl.time;
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
	snap->servertime = cl.mtime[0];
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
	int			i;
	int			skin;
	qmodel_t	*model;

	ent = CL_EntityNum (entnum);
	forcelink = (ent->msgtime != cl.mtime[1]);
	oldtime = ent->msgtime;

	if (oldtime + 0.2 < cl.mtime[0])
		ent->lerpflags |= LERP_RESETANIM;

	ent->msgtime = cl.mtime[0];

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

/*
==================
CL_ParseBaseline
==================
*/
void CL_ParseBaseline (entity_t *ent, int version) //johnfitz -- added argument
{
	int	i;
	int bits; //johnfitz

	//johnfitz -- PROTOCOL_FITZQUAKE
	bits = (version == 2) ? MSG_ReadByte() : 0;
	ent->baseline.modelindex = (bits & B_LARGEMODEL) ? MSG_ReadShort() : MSG_ReadByte();
	ent->baseline.frame = (bits & B_LARGEFRAME) ? MSG_ReadShort() : MSG_ReadByte();
	//johnfitz

	ent->baseline.colormap = MSG_ReadByte();
	ent->baseline.skin = MSG_ReadByte();
	for (i = 0; i < 3; i++)
	{
		ent->baseline.origin[i] = MSG_ReadCoord (cl.protocolflags);
		ent->baseline.angles[i] = MSG_ReadAngle (cl.protocolflags);
	}

	ent->baseline.alpha = (bits & B_ALPHA) ? MSG_ReadByte() : ENTALPHA_DEFAULT; //johnfitz -- PROTOCOL_FITZQUAKE
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

	if (msg_badread || count < 0 || count > cl_max_edicts)
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

		if (reset_interp)
			CL_PrepareFullSnapshotInterpReset ();

	// INV-5: commit staged snapshot only after full parse.
	for (i = 1; i < cl_max_edicts; i++)
	{
		if (!cl.snapshot_stage_present[i])
			continue;
		cl.snapshot_baseline[i] = cl.snapshot_stage[i];
		cl.snapshot_present[i] = 1;
		CL_ApplySnapshotState (i, &cl.snapshot_baseline[i]);
		CL_MarkSnapshotEntityUpdated (i);
	}

		if (reset_interp)
			CL_FinalizeFullSnapshotInterpReset ();
	}

	cl.snapshot_baseline_seq = seq;
	cl.snap_last_applied_seq = seq16;
	cl.snap_last_complete_seq = seq16;
	cl.snap_last_incomplete = false;
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
	qboolean baseline_match;
	qboolean drop;
	unsigned short seq16;
	unsigned short base16;
	qboolean need_full;
	qboolean base_complete;
	qboolean continuation_pending;

	seq = (unsigned int)MSG_ReadLong ();
	baseline_seq = (unsigned int)MSG_ReadLong ();
	seq16 = (unsigned short)seq;
	base16 = (unsigned short)baseline_seq;
	need_full = (cl.need_full_snapshot || !cl.has_full_snapshot);
	baseline_match = (baseline_seq != 0 && baseline_seq == cl.snapshot_baseline_seq);
	base_complete = (cl.snap_last_complete_seq != 0 && base16 == cl.snap_last_complete_seq);
	continuation_pending = cl.snapshot_chunk_active;
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

	if (!drop && baseline_match)
		SDL_assert (baseline_seq == cl.snapshot_baseline_seq);

	if (!drop && (!baseline_match || !base_complete || continuation_pending || need_full))
	{
		if (cl_snap_debug.value >= 1.0f)
			Con_Printf ("cl_snap delta mismatch base %u last %u need_full %d\n",
				baseline_seq, cl.snap_last_complete_seq, need_full);
		if (!baseline_match)
			CL_LogDeltaReject ("base_mismatch", seq, baseline_seq);
		else if (!base_complete)
			CL_LogDeltaReject ("base_incomplete", seq, baseline_seq);
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
	if (msg_badread || count < 0 || count > cl_max_edicts)
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
		if (!drop && baseline_match && entnum > 0 && entnum < cl_max_edicts)
			cl.snapshot_stage_remove[entnum] = 1;
	}

	count = (unsigned short)MSG_ReadShort ();
	if (msg_badread || count < 0 || count > cl_max_edicts)
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
		if (!drop && baseline_match && entnum > 0 && entnum < cl_max_edicts)
		{
			cl.snapshot_stage[entnum] = state;
			cl.snapshot_stage_present[entnum] = 1;
		}
	}

	count = (unsigned short)MSG_ReadShort ();
	if (msg_badread || count < 0 || count > cl_max_edicts)
	{
		CL_RequestFullSnapshot ("snapshot_delta update header", true);
		msg_readcount = net_message.cursize;
		return;
	}
	for (i = 0; i < count; i++)
	{
		int entnum = (unsigned short)MSG_ReadShort ();
		mask = (unsigned int)MSG_ReadLong ();
		if (msg_badread || (mask & ~SNAP_VALID_MASK))
		{
			CL_RequestFullSnapshot ("snapshot_delta update mask", true);
			msg_readcount = net_message.cursize;
			return;
		}
		if (!drop && baseline_match && entnum > 0 && entnum < cl_max_edicts && cl.snapshot_present[entnum])
		{
			snapshot_state_t state = cl.snapshot_baseline[entnum];
			CL_ReadSnapshotDeltaFields (&state, mask);
			if (msg_badread)
			{
				CL_RequestFullSnapshot ("snapshot_delta update fields", true);
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
		}
	}

	// INV-5: commit staged snapshot only after full parse/validation.
	if (!drop && baseline_match)
	{
		for (i = 1; i < cl_max_edicts; i++)
		{
			if (cl.snapshot_stage_remove[i])
			{
				cl.snapshot_present[i] = 0;
				CL_ClearSnapshotEntity (i);
			}
			if (!cl.snapshot_stage_present[i])
				continue;
			cl.snapshot_baseline[i] = cl.snapshot_stage[i];
			cl.snapshot_present[i] = 1;
			CL_ApplySnapshotState (i, &cl.snapshot_baseline[i]);
			CL_MarkSnapshotEntityUpdated (i);
		}

		for (i = 1; i < cl_max_edicts; i++)
		{
			if (cl.snapshot_present[i] && cl_entities[i].msgtime != cl.mtime[0])
				cl_entities[i].msgtime = cl.mtime[0];
		}
		cl.snapshot_baseline_seq = seq;
		cl.snap_last_applied_seq = seq16;
		cl.snap_last_complete_seq = seq16;
		cl.snap_last_incomplete = false;
		cl.has_full_snapshot = true;
		(void)CL_SendSnapshotAck (CL_GetSnapshotAckSeq ());
		CL_RecordPlayerSnap ();
		NET_DebugLogEvent (true,
			"NETDBG time %.3f snap_complete_apply seq %u\n",
			realtime, seq);
	}
	else if (!drop)
	{
		CL_SendSnapshotNak (cl.snapshot_baseline_seq, baseline_seq);
	}
}

typedef struct
{
	unsigned int	seq;
	unsigned int	base_seq;
	unsigned int	flags;
	unsigned short	num_entities;
	unsigned short	num_removed;
} snapshot_header_t;

static void CL_ReadSnapshotHeader (snapshot_header_t *header)
{
	header->seq = (unsigned int)MSG_ReadLong ();
	header->base_seq = (unsigned int)MSG_ReadLong ();
	header->flags = (unsigned int)MSG_ReadByte ();
	header->num_entities = (unsigned short)MSG_ReadShort ();
	header->num_removed = (unsigned short)MSG_ReadShort ();
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
		CL_ClearEntitySnapHistory (i);
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

static void CL_ResetSnapshotChunk (void)
{
	if (cl.snapshot_chunk_present)
		memset (cl.snapshot_chunk_present, 0, cl_max_edicts * sizeof(byte));
	cl.snapshot_chunk_active = false;
	cl.snapshot_chunk_seq = 0;
	cl.snapshot_chunk_expected_total = 0;
	cl.snapshot_chunk_received = 0;
	cl.snapshot_chunk_remaining = 0;
	cl.snapshot_chunk_start_time = 0;
	cl.snapshot_chunk_packets = 0;
}

static void CL_ParseSnapshot2 (void)
{
	snapshot_header_t header;
	qboolean baseline_match;
	qboolean drop;
	int i;
	unsigned short seq16;
	unsigned short base16;
	qboolean incomplete;
	qboolean need_full;
	qboolean is_full;
	qboolean is_delta;
	const int incomplete_limit = 3;

	CL_ReadSnapshotHeader (&header);
	drop = CL_ShouldDropSnapshot ();
	seq16 = (unsigned short)header.seq;
	base16 = (unsigned short)header.base_seq;
	incomplete = (header.flags & SNAPSHOT_FLAG_INCOMPLETE) != 0;
	need_full = (cl.need_full_snapshot || !cl.has_full_snapshot);
	is_full = (header.flags & SNAPSHOT_FLAG_FULL) != 0;
	is_delta = (header.flags & SNAPSHOT_FLAG_DELTA) != 0;

	if (!CL_SnapshotWorldModelReady ("snapshot2 worldmodel"))
		drop = true;

	NET_DebugLogEvent (true,
		"NETDBG time %.3f snap2_recv seq %u base %u flags 0x%x ents %u rem %u full %d delta %d has_full %d\n",
		realtime, header.seq, header.base_seq, header.flags, header.num_entities, header.num_removed,
		is_full ? 1 : 0, is_delta ? 1 : 0, cl.has_full_snapshot ? 1 : 0);

	if (CL_SnapshotChunkTimedOut ())
	{
		if (cl_full_reasm_debug.value >= 1.0f)
			Con_Printf ("cl_snap2 full reasm timeout seq %u\n", cl.snapshot_chunk_seq);
		CL_ResetSnapshotChunk ();
		CL_RequestFullSnapshot ("snapshot2 full reasm timeout", false);
	}

	if (cl_snap_debug.value >= 1.0f)
	{
		Con_Printf ("cl_snap2 seq %u base %u flags 0x%x ents %u rem %u%s\n",
			header.seq, header.base_seq, header.flags,
			header.num_entities, header.num_removed, drop ? " drop" : "");
	}

	if (msg_badread || header.num_entities > cl_max_edicts || header.num_removed > cl_max_edicts)
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
		qboolean continuation = (header.flags & SNAPSHOT_FLAG_CONTINUE) != 0;
		qboolean rem_zero = (header.num_removed == 0);
		qboolean final_chunk = (!continuation || rem_zero);
		qboolean finalize_invalid = (!continuation && !rem_zero);

		if (finalize_invalid)
		{
			NET_DebugLogEvent (true,
				"NETDBG time %.3f snap_finalize_skip seq %u base %u flags 0x%x reason flag_mismatch\n",
				realtime, header.seq, header.base_seq, header.flags);
			CL_RequestFullSnapshot ("snapshot2 flag mismatch", false);
			drop = true;
		}

		if (continuation || cl.snapshot_chunk_active)
		{
			qboolean apply_complete;
			qboolean chunk_order_error = false;

			if (!cl.snapshot_chunk_active || cl.snapshot_chunk_seq != header.seq)
			{
				if (cl.snapshot_chunk_active && cl.snapshot_chunk_seq != header.seq)
				{
					NET_DebugLogEvent (true,
						"NETDBG time %.3f snap2_chunk_abort seq %u new_seq %u\n",
						realtime, cl.snapshot_chunk_seq, header.seq);
					CL_RequestFullSnapshot ("snapshot2 chunk seq", false);
				}
				CL_ResetSnapshotChunk ();
				cl.snapshot_chunk_active = true;
				cl.snapshot_chunk_seq = header.seq;
				cl.snapshot_chunk_start_time = realtime;
				cl.snapshot_chunk_packets = 0;
				cl.snapshot_chunk_expected_total = header.num_entities + header.num_removed;
				cl.snapshot_chunk_received = 0;
				cl.snapshot_chunk_remaining = header.num_removed;
			}
			else
			{
				if (header.num_removed > cl.snapshot_chunk_remaining)
					chunk_order_error = true;
			}

			cl.snapshot_chunk_packets++;
			if (chunk_order_error)
			{
				NET_DebugLogEvent (true,
					"NETDBG time %.3f snap2_chunk_order seq %u rem %u prev_rem %u\n",
					realtime, header.seq, header.num_removed, cl.snapshot_chunk_remaining);
				CL_RequestFullSnapshot ("snapshot2 chunk order", false);
				drop = true;
			}
			if (cl_full_reasm_debug.value >= 2.0f)
				Con_Printf ("cl_snap2 full reasm seq %u packet %d flags 0x%x ents %u\n",
					header.seq, cl.snapshot_chunk_packets, header.flags, header.num_entities);

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
					CL_ResetSnapshotChunk ();
					return;
				}
				if (cl.protocolflags & PRFL_SNAPSHOT_HIRES)
					snapflags = (unsigned int)MSG_ReadByte ();
				CL_ReadSnapshotState (&state, snapflags);
				if (msg_badread)
				{
					CL_RequestFullSnapshot ("snapshot2 chunk state", true);
					msg_readcount = net_message.cursize;
					CL_ResetSnapshotChunk ();
					return;
				}
				if (!drop)
				{
					cl.snapshot_chunk[entnum] = state;
					cl.snapshot_chunk_present[entnum] = 1;
				}
			}

			if (drop)
			{
				CL_ResetSnapshotChunk ();
				return;
			}

			cl.snapshot_chunk_received += header.num_entities;
			cl.snapshot_chunk_remaining = header.num_removed;

			NET_DebugLogEvent (true,
				"NETDBG time %.3f snap2_chunk_buffer seq %u flags 0x%x recv %u total %u rem %u packets %d\n",
				realtime, header.seq, header.flags, cl.snapshot_chunk_received,
				cl.snapshot_chunk_expected_total, cl.snapshot_chunk_remaining,
				cl.snapshot_chunk_packets);

			if (!final_chunk)
				return;

			apply_complete = !drop && final_chunk && !finalize_invalid && !incomplete;
			if (apply_complete && rem_zero)
				incomplete = false;
			if (apply_complete && cl.snapshot_chunk_expected_total
				&& cl.snapshot_chunk_received != cl.snapshot_chunk_expected_total)
			{
				NET_DebugLogEvent (true,
					"NETDBG time %.3f snap2_chunk_mismatch seq %u recv %u total %u\n",
					realtime, header.seq, cl.snapshot_chunk_received, cl.snapshot_chunk_expected_total);
				CL_RequestFullSnapshot ("snapshot2 chunk total", false);
				apply_complete = false;
			}

			if (apply_complete)
			{
				qboolean reset_interp = (!cl.has_full_snapshot || cl.need_full_snapshot);

				if (reset_interp)
					CL_PrepareFullSnapshotInterpReset ();

				// INV-5: commit reassembled snapshot only after full chunk parse.
				for (i = 1; i < cl_max_edicts; i++)
				{
					if (!cl.snapshot_chunk_present[i])
						continue;
					cl.snapshot_baseline[i] = cl.snapshot_chunk[i];
					cl.snapshot_present[i] = 1;
					CL_ApplySnapshotState (i, &cl.snapshot_chunk[i]);
					CL_MarkSnapshotEntityUpdated (i);
				}

				if (reset_interp)
					CL_FinalizeFullSnapshotInterpReset ();

				CL_Predict_Reapply ();

				cl.snapshot_baseline_seq = header.seq;
				cl.snap_last_applied_seq = seq16;
				cl.snap_last_complete_seq = seq16;
				cl.snap_last_incomplete = false;
				cl.need_full_snapshot = false;
				cl.has_full_snapshot = true;
				cl.has_valid_worldstate = true;
				cl.snap_incomplete_count = 0;
				CL_RecordPlayerSnap ();
				NET_DebugLogEvent (true,
					"NETDBG time %.3f snap2_apply seq %u flags 0x%x full %d delta %d has_full %d\n",
					realtime, header.seq, header.flags, is_full ? 1 : 0, is_delta ? 1 : 0,
					cl.has_full_snapshot ? 1 : 0);
			}
			else if (!drop)
			{
				cl.snap_last_incomplete_seq = seq16;
				cl.snap_last_incomplete = true;
				cl.snap_incomplete_count++;
				NET_DebugLogEvent (true,
					"NETDBG time %.3f snap2_buffer seq %u flags 0x%x incomplete %d has_full %d\n",
					realtime, header.seq, header.flags, cl.snap_incomplete_count,
					cl.has_full_snapshot ? 1 : 0);
				if (cl.snap_incomplete_count >= incomplete_limit)
					CL_RequestFullSnapshot ("snapshot2 incomplete", false);
			}
			if (!CL_SendSnapshotAck (CL_GetSnapshotAckSeq ()))
				CL_RequestFullSnapshot ("snapshot2 ack failed", false);
			CL_ResetSnapshotChunk ();
			return;
		}

		CL_ClearSnapshotStage ();

		for (i = 0; i < header.num_entities; i++)
		{
			int entnum;
			snapshot_state_t state;
			unsigned int snapflags = 0;

			entnum = (unsigned short)MSG_ReadShort ();
			if (entnum <= 0 || entnum >= cl_max_edicts)
			{
				CL_RequestFullSnapshot ("snapshot2 full entnum", true);
				msg_readcount = net_message.cursize;
				return;
			}
			if (cl.protocolflags & PRFL_SNAPSHOT_HIRES)
				snapflags = (unsigned int)MSG_ReadByte ();
			CL_ReadSnapshotState (&state, snapflags);
			if (msg_badread)
			{
				CL_RequestFullSnapshot ("snapshot2 full state", true);
				msg_readcount = net_message.cursize;
				return;
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

				if (reset_interp)
					CL_PrepareFullSnapshotInterpReset ();

				// INV-5: commit staged snapshot only after full parse.
				for (i = 1; i < cl_max_edicts; i++)
				{
					if (!cl.snapshot_stage_present[i])
						continue;
					cl.snapshot_baseline[i] = cl.snapshot_stage[i];
					cl.snapshot_present[i] = 1;
					CL_ApplySnapshotState (i, &cl.snapshot_baseline[i]);
					CL_MarkSnapshotEntityUpdated (i);
				}

				if (reset_interp)
					CL_FinalizeFullSnapshotInterpReset ();

				CL_Predict_Reapply ();

				cl.snapshot_baseline_seq = header.seq;
				cl.snap_last_applied_seq = seq16;
				cl.snap_last_complete_seq = seq16;
				cl.snap_last_incomplete = false;
				cl.need_full_snapshot = false;
				cl.has_full_snapshot = true;
				cl.has_valid_worldstate = true;
				cl.snap_incomplete_count = 0;
				CL_RecordPlayerSnap ();
				NET_DebugLogEvent (true,
					"NETDBG time %.3f snap2_apply seq %u flags 0x%x full %d delta %d has_full %d\n",
					realtime, header.seq, header.flags, is_full ? 1 : 0, is_delta ? 1 : 0,
					cl.has_full_snapshot ? 1 : 0);
			}
			else
			{
				cl.snap_last_incomplete_seq = seq16;
				cl.snap_last_incomplete = true;
				cl.snap_incomplete_count++;
				NET_DebugLogEvent (true,
					"NETDBG time %.3f snap2_buffer seq %u flags 0x%x incomplete %d has_full %d\n",
					realtime, header.seq, header.flags, cl.snap_incomplete_count,
					cl.has_full_snapshot ? 1 : 0);
				if (cl.snap_incomplete_count >= incomplete_limit)
					CL_RequestFullSnapshot ("snapshot2 incomplete", false);
			}
			if (!CL_SendSnapshotAck (CL_GetSnapshotAckSeq ()))
				CL_RequestFullSnapshot ("snapshot2 ack failed", false);
		}
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

	baseline_match = (header.base_seq != 0xFFFFFFFFu && header.base_seq == cl.snapshot_baseline_seq);
	if (!drop && baseline_match)
		SDL_assert (header.base_seq == cl.snapshot_baseline_seq);

	if (!drop && (!baseline_match || base16 != cl.snap_last_complete_seq || cl.snapshot_chunk_active || need_full))
	{
		if (cl_snap_debug.value >= 1.0f)
			Con_Printf ("cl_snap2 delta mismatch base %u last %u need_full %d\n",
				header.base_seq, cl.snap_last_complete_seq, need_full);
		if (!baseline_match)
			CL_LogDeltaReject ("base_mismatch", header.seq, header.base_seq);
		else if (base16 != cl.snap_last_complete_seq)
			CL_LogDeltaReject ("base_incomplete", header.seq, header.base_seq);
		else if (cl.snapshot_chunk_active)
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
			if (msg_badread)
			{
				CL_RequestFullSnapshot ("snapshot2 remove list", true);
				msg_readcount = net_message.cursize;
				return;
			}
			if (!drop && baseline_match && entnum > 0 && entnum < cl_max_edicts)
				cl.snapshot_stage_remove[entnum] = 1;
		}
	}

	{
		int add_count = (unsigned short)MSG_ReadShort ();
		if (msg_badread || add_count < 0 || add_count > cl_max_edicts)
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
				cl.snapshot_stage[entnum] = state;
				cl.snapshot_stage_present[entnum] = 1;
			}
		}

		{
			int update_count = (unsigned short)MSG_ReadShort ();
			if (msg_badread || update_count < 0 || update_count > cl_max_edicts)
			{
				CL_RequestFullSnapshot ("snapshot2 update header", true);
				msg_readcount = net_message.cursize;
				return;
			}
			for (i = 0; i < update_count; i++)
			{
				int entnum = (unsigned short)MSG_ReadShort ();
				unsigned int mask = (unsigned int)MSG_ReadLong ();
				if (msg_badread || (mask & ~SNAP_VALID_MASK))
				{
					CL_RequestFullSnapshot ("snapshot2 update mask", true);
					msg_readcount = net_message.cursize;
					return;
				}

				if (!drop && baseline_match && entnum > 0 && entnum < cl_max_edicts && cl.snapshot_present[entnum])
				{
					snapshot_state_t state = cl.snapshot_baseline[entnum];
					CL_ReadSnapshotDeltaFields (&state, mask);
					if (msg_badread)
					{
						CL_RequestFullSnapshot ("snapshot2 update fields", true);
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
				}
			}
		}
	}

	if (!drop && baseline_match)
	{
		if (!incomplete)
		{
			for (i = 1; i < cl_max_edicts; i++)
			{
				if (cl.snapshot_stage_remove[i])
				{
					cl.snapshot_present[i] = 0;
					CL_ClearSnapshotEntity (i);
				}
				if (!cl.snapshot_stage_present[i])
					continue;
				cl.snapshot_baseline[i] = cl.snapshot_stage[i];
				CL_ApplySnapshotState (i, &cl.snapshot_stage[i]);
				CL_MarkSnapshotEntityUpdated (i);
			}

			CL_Predict_Reapply ();

			for (i = 1; i < cl_max_edicts; i++)
			{
				if (cl.snapshot_present[i] && cl_entities[i].msgtime != cl.mtime[0])
					cl_entities[i].msgtime = cl.mtime[0];
			}

			cl.snapshot_baseline_seq = header.seq;
			cl.snap_last_applied_seq = seq16;
			cl.snap_last_complete_seq = seq16;
			cl.snap_last_incomplete = false;
			cl.snap_incomplete_count = 0;
			cl.has_full_snapshot = true;
			CL_RecordPlayerSnap ();
			NET_DebugLogEvent (true,
				"NETDBG time %.3f snap2_apply seq %u flags 0x%x full %d delta %d has_full %d\n",
				realtime, header.seq, header.flags, is_full ? 1 : 0, is_delta ? 1 : 0,
				cl.has_full_snapshot ? 1 : 0);
		}
		else
		{
			cl.snap_last_incomplete_seq = seq16;
			cl.snap_last_incomplete = true;
			cl.snap_incomplete_count++;
			NET_DebugLogEvent (true,
				"NETDBG time %.3f snap2_buffer seq %u flags 0x%x incomplete %d has_full %d\n",
				realtime, header.seq, header.flags, cl.snap_incomplete_count,
				cl.has_full_snapshot ? 1 : 0);
			if (cl.snap_incomplete_count >= incomplete_limit)
				CL_RequestFullSnapshot ("snapshot2 incomplete", false);
		}
		if (!CL_SendSnapshotAck (CL_GetSnapshotAckSeq ()))
			CL_RequestFullSnapshot ("snapshot2 ack failed", false);
	}
	else if (!drop)
	{
		CL_SendSnapshotNak (cl.snapshot_baseline_seq, header.base_seq);
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

	//johnfitz -- PROTOCOL_FITZQUAKE
	if (bits & SU_EXTEND1)
		bits |= (MSG_ReadByte() << 16);
	if (bits & SU_EXTEND2)
		bits |= (MSG_ReadByte() << 24);
	//johnfitz

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

	//johnfitz -- PROTOCOL_FITZQUAKE
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
	//johnfitz

	// svc_clientdata carries pmove-critical fields; snapshot2 owns local player position/orientation.
	if (bits & SU_PREDICT)
	{
		unsigned int ack = (unsigned int)MSG_ReadLong ();
		vec3_t origin;
		vec3_t velocity;
		vec3_t viewangles;

		for (i = 0; i < 3; i++)
			origin[i] = MSG_ReadCoord (cl.protocolflags);
		for (i = 0; i < 3; i++)
			velocity[i] = MSG_ReadCoord (cl.protocolflags);
		for (i = 0; i < 3; i++)
			if (cl.protocol == PROTOCOL_NETQUAKE)
				viewangles[i] = MSG_ReadAngle (cl.protocolflags);
			else
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

	//johnfitz -- PROTOCOL_FITZQUAKE
	if (version == 2)
		sound_num = MSG_ReadShort ();
	else
		sound_num = MSG_ReadByte ();
	//johnfitz

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
//proquake has its own extension coding thing.
static void CL_ParseStuffText(const char *msg)
{
	const char *str;
	q_strlcat(cl.stuffcmdbuf, msg, sizeof(cl.stuffcmdbuf));
	for (; (str = strchr(cl.stuffcmdbuf, '\n')); memmove(cl.stuffcmdbuf, str, Q_strlen(str)+1))
	{
		qboolean handled = false;

		str++;//skip past the \n

		if (*cl.stuffcmdbuf == 0x01 && cl.protocol == PROTOCOL_NETQUAKE) //proquake message, just strip this and try again (doesn't necessarily have a trailing \n straight away)
		{
			for (str = cl.stuffcmdbuf+1; *str >= 0x01 && *str <= 0x1f; str++)
				;//FIXME: parse properly
			continue;
		}

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

	// if the high bit of the command byte is set, it is a fast update
		if (cmd & U_SIGNAL) //johnfitz -- was 128, changed for clarity
		{
			SHOWNET("fast update");
			if (cl_netdebug_parse.value)
				Con_Printf ("NETDBG: cmd fast_update 0x%x @%d\n", cmd, cmd_offset);
			CL_ParseUpdate (cmd&127);
			if (msg_badread)
			{
				CL_BadServerMessage ("bad fast update", cmd, cmd_offset,
					lastcmd, lastcmd_offset, prevcmd, prevcmd_offset);
				return;
			}
			if (cl_netdebug_parse.value)
			{
				int cmd_end = CL_NetDebugReadcount ();
				if (cmd_end < cmd_offset)
					cmd_end = cmd_offset;
				Con_Printf ("NETDBG: cmd fast_update end %d bytes %d\n",
					cmd_end, cmd_end - cmd_offset);
			}
			prevcmd = lastcmd;
			prevcmd_offset = lastcmd_offset;
			lastcmd = cmd;
			lastcmd_offset = cmd_offset;
			continue;
		}

		if (cmd < (int)NUM_SVC_STRINGS) {
			SHOWNET(svc_strings[cmd]);
		}
		if (cl_netdebug_parse.value)
			Con_Printf ("NETDBG: cmd %s (%d) @%d\n", CL_SvcName (cmd), cmd, cmd_offset);

	// other commands
		switch (cmd)
		{
		default:
		//	CL_DumpPacket ();
			if (realtime >= cl_unknown_cmd_next_warn_time)
			{
				Con_Printf ("Dropping server packet with unknown command %s (%d)\n",
					CL_SvcName (cmd), cmd);
				cl_unknown_cmd_next_warn_time = realtime + 1.0;
			}
			msg_readcount = net_message.cursize;
			return;

		case svc_nop:
		//	Con_Printf ("svc_nop\n");
			break;

		case svc_time:
			cl.mtime[1] = cl.mtime[0];
			cl.mtime[0] = MSG_ReadFloat ();
			cl.fixangle = false;
			if (cls.signon == SIGNONS - 1)
			{	// allow map loads with no visible entity updates to finish signon
				cls.signon = SIGNONS;
				CL_SignonReply ();
			}
			break;

		case svc_clientdata:
			CL_ParseClientdata (); //johnfitz -- removed bits parameter, we will read this inside CL_ParseClientdata()
			break;

		case svc_version:
			i = MSG_ReadLong ();
			//johnfitz -- support multiple protocols
			if (i != PROTOCOL_NETQUAKE && i != PROTOCOL_FITZQUAKE && i != PROTOCOL_RMQ)
				Host_Error ("Server returned version %i, not %i or %i or %i", i, PROTOCOL_NETQUAKE, PROTOCOL_FITZQUAKE, PROTOCOL_RMQ);
			cl.protocol = i;
			//johnfitz
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

		case svc_spawnbaseline2: //PROTOCOL_FITZQUAKE
			i = MSG_ReadShort ();
			// must use CL_EntityNum() to force cl.num_entities up
			CL_ParseBaseline (CL_EntityNum(i), 2);
			break;

		case svc_spawnstatic2: //PROTOCOL_FITZQUAKE
			CL_ParseStatic (2);
			break;

		case svc_spawnstaticsound2: //PROTOCOL_FITZQUAKE
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

		case svc_packedentities:
			CL_ParsePackedEntities ();
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
		}

		prevcmd = lastcmd;
		prevcmd_offset = lastcmd_offset;
		lastcmd = cmd; //johnfitz
		lastcmd_offset = cmd_offset;
	}
}
