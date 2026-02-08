/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
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
/* Q3MINI PLAN:
 * - Respect sv_tickrate/sv_sendrate=0 legacy path and clamp tick/send cadence.
 */
// host.c -- coordinates spawning and killing of local servers

#include "quakedef.h"
#include "bgmusic.h"
#include "steam.h"
#include <setjmp.h>
#include <time.h>

/*

A server can allways be started, even if the system started out as a client
to a remote system.

A client can NOT be started if the system started as a dedicated server.

Memory is cleared / released when a server or client begins, not when they end.

*/

quakeparms_t *host_parms;

qboolean	host_initialized;		// true if into command execution

double		host_frametime;
double		host_rawframetime;
double		realtime;				// without any filtering or bounding
double		oldrealtime;			// last frame run

int		host_framecount;

int		host_hunklevel;

static int		host_assetloading_depth;
static qboolean	host_assetloading_sound_blocked;
static qboolean	host_assetloading_was_demopaused;
static qboolean	host_assetloading_was_demoplayback;

int		minimum_memory;

client_t	*host_client;			// current client

jmp_buf 	host_abortserver;

byte		*host_colormap;
float	host_netinterval;
cvar_t	host_framerate = {"host_framerate","0",CVAR_NONE};	// set for slow motion
cvar_t	host_speeds = {"host_speeds","0",CVAR_NONE};			// set for running times
cvar_t	host_maxfps = {"host_maxfps", "250", CVAR_ARCHIVE}; //johnfitz
cvar_t	host_timescale = {"host_timescale", "0", CVAR_NONE}; //johnfitz
cvar_t	max_edicts = {"max_edicts", "16384", CVAR_NONE}; //johnfitz //ericw -- changed from 2048 to 8192, removed CVAR_ARCHIVE
cvar_t	cl_nocsqc = {"cl_nocsqc", "0", CVAR_NONE};	//spike -- blocks the loading of any csqc modules

cvar_t	sys_ticrate = {"sys_ticrate","0.05",CVAR_NONE}; // dedicated server
cvar_t	serverprofile = {"serverprofile","0",CVAR_NONE};
cvar_t	sys_step_debug = {"sys_step_debug", "0", CVAR_NONE};
cvar_t	sys_step_dump = {"sys_step_dump", "0", CVAR_NONE};
cvar_t	sys_step_hitch_ms = {"sys_step_hitch_ms", "50", CVAR_NONE};
cvar_t	jitter_time_debug = {"jitter_time_debug", "0", CVAR_NONE};

cvar_t	fraglimit = {"fraglimit","0",CVAR_NOTIFY|CVAR_SERVERINFO};
cvar_t	timelimit = {"timelimit","0",CVAR_NOTIFY|CVAR_SERVERINFO};
cvar_t	teamplay = {"teamplay","0",CVAR_NOTIFY|CVAR_SERVERINFO};
cvar_t	samelevel = {"samelevel","0",CVAR_NONE};
cvar_t	noexit = {"noexit","0",CVAR_NOTIFY|CVAR_SERVERINFO};
cvar_t	skill = {"skill","1",CVAR_NONE};			// 0 - 3
cvar_t	deathmatch = {"deathmatch","0",CVAR_NONE};	// 0, 1, or 2
cvar_t	coop = {"coop","0",CVAR_NONE};			// 0 or 1

cvar_t	pausable = {"pausable","1",CVAR_NONE};

cvar_t	developer = {"developer","0",CVAR_NONE};
cvar_t	map_checks = {"map_checks","0",CVAR_NONE};

cvar_t	temp1 = {"temp1","0",CVAR_NONE};

cvar_t devstats = {"devstats","0",CVAR_NONE}; //johnfitz -- track developer statistics that vary every frame
cvar_t cl_titlestats = {"cl_titlestats","1",CVAR_ARCHIVE};

cvar_t	campaign = {"campaign","0",CVAR_NONE}; // for the 2021 rerelease
cvar_t	horde = {"horde","0",CVAR_NONE}; // for the 2021 rerelease
cvar_t	sv_cheats = {"sv_cheats","0",CVAR_NONE}; // for the 2021 rerelease

cvar_t	sv_autosave = {"sv_autosave", "1", CVAR_ARCHIVE};
cvar_t	sv_autosave_interval = {"sv_autosave_interval", "30", CVAR_ARCHIVE};

devstats_t dev_stats, dev_peakstats;
overflowtimes_t dev_overflows; //this stores the last time overflow messages were displayed, not the last time overflows occured
extern cvar_t sv_tickrate;
extern cvar_t sv_netrate;
extern cvar_t sv_tick_maxcatchup;
extern cvar_t sv_fixedtick;
extern cvar_t sv_maxsteps_per_frame;
extern cvar_t sv_tick_debug;
extern cvar_t sv_timewarp;

sys_step_debug_info_t sys_step_debug_info;

/*
================
Host_ShouldWriteNetErrorReport
================
*/
static qboolean Host_ShouldWriteNetErrorReport (const char *message)
{
	if (!message)
		return false;

	if (q_strcasestr (message, "overflow"))
		return true;
	if (q_strcasestr (message, "bad") && (q_strcasestr (message, "packet") || q_strcasestr (message, "message") || q_strcasestr (message, "server")))
		return true;
	if (q_strcasestr (message, "illegible") || q_strcasestr (message, "packet") || q_strcasestr (message, "datagram"))
		return true;

	return false;
}

/*
================
Host_WriteNetErrorReport
================
*/
static void Host_WriteNetErrorReport (const char *message)
{
	static qboolean inreport = false;
	FILE *file;
	const char *path;
	time_t now;

	if (inreport || !host_parms || !host_parms->userdir)
		return;

	inreport = true;

	path = va ("%s/%s", host_parms->userdir, "debug_net_error.txt");
	file = Sys_fopen (path, "w");
	if (!file)
	{
		inreport = false;
		return;
	}

	now = time (NULL);
	fprintf (file, "Ironwail network error report\n");
	fprintf (file, "Timestamp: %s", ctime (&now));
	fprintf (file, "Error: %s\n\n", message ? message : "<null>");
	fprintf (file, "Engine version: Quake %1.2f\n", VERSION);
	fprintf (file, "QuakeSpasm version: " QUAKESPASM_VER_STRING "\n");
	fprintf (file, "Ironwail version: " IRONWAIL_VER_STRING "\n");
	fprintf (file, "Host frame: %d\n", host_framecount);
	fprintf (file, "Realtime: %.3f\n", realtime);
	fprintf (file, "Client state: %d\n", cls.state);
	fprintf (file, "Client signon: %d\n", cls.signon);
	fprintf (file, "Demo playback: %d\n", cls.demoplayback);
	fprintf (file, "Demo recording: %d\n", cls.demorecording);
	fprintf (file, "Server active: %d\n", sv.active);
	fprintf (file, "Server map: %s\n", sv.name[0] ? sv.name : "<none>");
	fprintf (file, "Max clients: %d\n", svs.maxclients);
	fclose (file);

	inreport = false;
}


/*
================
Host_BeginAssetLoading
================
*/
void Host_BeginAssetLoading (void)
{
	if (host_assetloading_depth++ > 0)
		return;

	host_assetloading_sound_blocked = S_IsSoundBlocked ();
	host_assetloading_was_demoplayback = cls.demoplayback;
	host_assetloading_was_demopaused = cls.demopaused;

	if (!host_assetloading_sound_blocked)
		S_BlockSound ();

	if (cls.demoplayback)
		cls.demopaused = true;
}

/*
================
Host_EndAssetLoading
================
*/
void Host_EndAssetLoading (void)
{
	if (host_assetloading_depth == 0)
		return;

	host_assetloading_depth--;
	if (host_assetloading_depth > 0)
		return;

	if (!host_assetloading_sound_blocked)
		S_UnblockSound ();

	if (host_assetloading_was_demoplayback && cls.demoplayback)
		cls.demopaused = host_assetloading_was_demopaused;
}

/*
================
Host_IsAssetLoading
================
*/
qboolean Host_IsAssetLoading (void)
{
	return host_assetloading_depth > 0;
}

/*
================
Max_Edicts_f -- johnfitz
================
*/
static void Max_Edicts_f (cvar_t *var)
{
	//TODO: clamp it here?
	if (cls.state == ca_connected || sv.active)
		Con_Printf ("Changes to max_edicts will not take effect until the next time a map is loaded.\n");
}

/*
================
Max_Fps_f -- ericw
================
*/
static void Max_Fps_f (cvar_t *var)
{
	if (var->value > 72 || var->value <= 0)
	{
		if (!host_netinterval)
			Con_Printf ("Using renderer/network isolation.\n");
		host_netinterval = 1.0/72;
	}
	else
	{
		if (host_netinterval)
			Con_Printf ("Disabling renderer/network isolation.\n");
		host_netinterval = 0;

		if (var->value > 72)
			Con_Warning ("host_maxfps above 72 breaks physics.\n");
	}
}

/*
================
Map_Checks_f -- called when map_checks cvar changes
================
*/
static void Map_Checks_f (cvar_t *var)
{
	static qboolean showed_message = false;
	if (!var->value || showed_message)
		return;
	showed_message = true;
	Con_SafePrintf ("\x02Note: ");
	Con_SafePrintf ("%s overrides gl_zfix, r_oit, and r_alphasort\n", var->name);
}

/*
================
Host_EndGame
================
*/
void Host_EndGame (const char *message, ...)
{
	va_list		argptr;
	char		string[1024];

	va_start (argptr,message);
	q_vsnprintf (string, sizeof(string), message, argptr);
	va_end (argptr);
	Con_DPrintf ("Host_EndGame: %s\n",string);

	PR_SwitchQCVM(NULL);

	if (sv.active)
		Host_ShutdownServer (false);

	if (cls.state == ca_dedicated)
		Sys_Error ("Host_EndGame: %s\n",string);	// dedicated servers exit

	if (cls.demonum != -1 && cls.demoloop)
		CL_NextDemo ();
	else
		CL_Disconnect ();

	longjmp (host_abortserver, 1);
}

/*
================
Host_ReportError

This shuts down both the client and server
================
*/
void Host_ReportError (const char *error, ...)
{
	va_list		argptr;
	char		string[1024];
	static	qboolean inerror = false;

	if (inerror)
		Sys_Error ("Host_Error: recursively entered");
	inerror = true;

	PR_SwitchQCVM(NULL);

	SCR_EndLoadingPlaque ();		// reenable screen updates

	va_start (argptr,error);
	q_vsnprintf (string, sizeof(string), error, argptr);
	va_end (argptr);
	Con_Printf ("Host_Error: %s\n",string);
	if (Host_ShouldWriteNetErrorReport (string))
		Host_WriteNetErrorReport (string);

	if (sv.active)
		Host_ShutdownServer (false);

	if (cls.state == ca_dedicated)
		Sys_Error ("Host_Error: %s\n",string);	// dedicated servers exit

	CL_Disconnect ();
	cls.demonum = -1;
	cl.intermission = 0; //johnfitz -- for errors during intermissions (changelevel with no map found, etc.)

	inerror = false;

	longjmp (host_abortserver, 1);
}

/*
================
Host_FindMaxClients
================
*/
void	Host_FindMaxClients (void)
{
	int		i;

	svs.maxclients = 1;

	i = COM_CheckParm ("-dedicated");
	if (i)
	{
		cls.state = ca_dedicated;
		if (i != (com_argc - 1))
		{
			svs.maxclients = Q_atoi (com_argv[i+1]);
		}
		else
			svs.maxclients = 8;
	}
	else
		cls.state = ca_disconnected;

	i = COM_CheckParm ("-listen");
	if (i)
	{
		if (cls.state == ca_dedicated)
			Sys_Error ("Only one of -dedicated or -listen can be specified");
		if (i != (com_argc - 1))
			svs.maxclients = Q_atoi (com_argv[i+1]);
		else
			svs.maxclients = 8;
	}
	if (svs.maxclients < 1)
		svs.maxclients = 8;
	else if (svs.maxclients > MAX_SCOREBOARD)
		svs.maxclients = MAX_SCOREBOARD;

	svs.maxclientslimit = svs.maxclients;
	if (svs.maxclientslimit < 32)
		svs.maxclientslimit = 32;
	svs.clients = (struct client_s *) Hunk_AllocName (svs.maxclientslimit*sizeof(client_t), "clients");

	if (svs.maxclients > 1)
		Cvar_SetQuick (&deathmatch, "1");
	else
		Cvar_SetQuick (&deathmatch, "0");
}

void Host_Version_f (void)
{
	SDL_version sdl_linked;
	SDL_GetVersion (&sdl_linked);

	Con_Printf ("\n");
	Con_Printf ("Quake      %1.2f\n", VERSION);
	Con_Printf ("QuakeSpasm " QUAKESPASM_VER_STRING "\n");
	Con_Printf ("Ironwail   " IRONWAIL_VER_STRING "\n");
	Con_Printf ("Exe        " __TIME__ " " __DATE__ "\n");
	Con_Printf ("SDL        " Q_SDL_COMPILED_VERSION_STRING " (compiled)\n");
	Con_Printf ("           %d.%d.%d (linked)\n", sdl_linked.major, sdl_linked.minor, sdl_linked.patch);
	Con_Printf ("OS         %s %d-bit\n", SDL_GetPlatform (), (int)sizeof(void*)*8);
	Con_Printf ("\n");
}

/* cvar callback functions : */
void Host_Callback_Notify (cvar_t *var)
{
	if (sv.active)
		SV_BroadcastPrintf ("\"%s\" changed to \"%s\"\n", var->name, var->string);
}

/*
===============
Host_WriteConfigurationToFile

Writes key bindings and archived cvars to specified file
===============
*/
void Host_WriteConfigurationToFile (const char *name)
{
	FILE	*f;

// dedicated servers initialize the host but don't parse and set the
// config.cfg cvars
	if (host_initialized && !isDedicated && !host_parms->errstate)
	{
		char fullname[MAX_OSPATH];
		q_snprintf (fullname, sizeof (fullname), "%s/%s", com_gamedir, name);
		f = Sys_fopen (fullname, "w");
		if (!f)
		{
			Con_Printf ("Couldn't write %s.\n", name);
			return;
		}

		//VID_SyncCvars (); //johnfitz -- write actual current mode to config file, in case cvars were messed with

		Key_WriteBindings (f);
		Cvar_WriteVariables (f);

		//johnfitz -- extra commands to preserve state
		fprintf (f, "vid_restart\n");
		if (in_mlook.state & 1) fprintf (f, "+mlook\n");
		//johnfitz

		fclose (f);

		Con_SafePrintf ("Wrote ");
		Con_LinkPrintf (fullname, "%s", name);
		Con_SafePrintf (".\n");
	}
}

/*
=======================
Host_WriteConfig_f
======================
*/
void Host_WriteConfig_f (void)
{
	char filename[MAX_QPATH];
	q_strlcpy (filename, Cmd_Argc () >= 2 ? Cmd_Argv (1) : CONFIG_NAME, sizeof (filename));
	COM_AddExtension (filename, ".cfg", sizeof (filename));
	Host_WriteConfigurationToFile (filename);
}

/*
=======================
Host_InitLocal
======================
*/
void Host_InitLocal (void)
{
	Cmd_AddCommand ("version", Host_Version_f);
        Cmd_AddCommand ("writeconfig", Host_WriteConfig_f);

        Host_InitCommands ();

    Cvar_RegisterVariable (&host_framerate);
	Cvar_RegisterVariable (&host_speeds);
	Cvar_RegisterVariable (&host_maxfps); //johnfitz
	Cvar_SetCallback (&host_maxfps, Max_Fps_f);
	Max_Fps_f (&host_maxfps);
	Cvar_RegisterVariable (&host_timescale); //johnfitz

	Cvar_RegisterVariable (&cl_nocsqc);	//spike
	Cvar_RegisterVariable (&max_edicts); //johnfitz
	Cvar_SetCallback (&max_edicts, Max_Edicts_f);
	Cvar_RegisterVariable (&devstats); //johnfitz

	Cvar_RegisterVariable (&cl_titlestats);

	Cvar_RegisterVariable (&sys_ticrate);
	Cvar_RegisterVariable (&serverprofile);
	Cvar_RegisterVariable (&sys_step_debug);
	Cvar_RegisterVariable (&sys_step_dump);
	Cvar_RegisterVariable (&sys_step_hitch_ms);
	Cvar_RegisterVariable (&jitter_time_debug);
	Cvar_RegisterVariable (&jitter_log_enable);
	Cvar_RegisterVariable (&jitter_log_file);
	Cvar_RegisterVariable (&jitter_log_flush);
	Cvar_RegisterVariable (&jitter_log_force_developer);

	Cvar_RegisterVariable (&fraglimit);
	Cvar_RegisterVariable (&timelimit);
	Cvar_RegisterVariable (&teamplay);
	Cvar_SetCallback (&fraglimit, Host_Callback_Notify);
	Cvar_SetCallback (&timelimit, Host_Callback_Notify);
	Cvar_SetCallback (&teamplay, Host_Callback_Notify);
	Cvar_RegisterVariable (&samelevel);
	Cvar_RegisterVariable (&noexit);
	Cvar_SetCallback (&noexit, Host_Callback_Notify);
	Cvar_RegisterVariable (&skill);
	Cvar_RegisterVariable (&developer);
	Cvar_RegisterVariable (&map_checks);
	Cvar_SetCallback (&map_checks, Map_Checks_f);
	Cvar_RegisterVariable (&coop);
	Cvar_RegisterVariable (&deathmatch);

	Cvar_RegisterVariable (&campaign);
	Cvar_RegisterVariable (&horde);
	Cvar_RegisterVariable (&sv_cheats);

	Cvar_RegisterVariable (&pausable);

	Cvar_RegisterVariable (&temp1);
	Cvar_RegisterVariable (&sz_debug_hexdump);

	Host_FindMaxClients ();
}


/*
===============
Host_WriteConfiguration

Writes key bindings and archived cvars to engine config file
===============
*/
void Host_WriteConfiguration (void)
{
	Host_WriteConfigurationToFile (CONFIG_NAME);
}


/*
=================
SV_ClientPrintf

Sends text across to be displayed
FIXME: make this just a stuffed echo?
=================
*/
void SV_ClientPrintf (const char *fmt, ...)
{
	va_list		argptr;
	char		string[1024];

	va_start (argptr,fmt);
	q_vsnprintf (string, sizeof(string), fmt,argptr);
	va_end (argptr);

	MSG_WriteByte (&host_client->message, svc_print);
	MSG_WriteString (&host_client->message, string);
}

/*
=================
SV_BroadcastPrintf

Sends text to all active clients
=================
*/
void SV_BroadcastPrintf (const char *fmt, ...)
{
	va_list		argptr;
	char		string[1024];
	int			i;

	va_start (argptr,fmt);
	q_vsnprintf (string, sizeof(string), fmt, argptr);
	va_end (argptr);

	for (i = 0; i < svs.maxclients; i++)
	{
		if (svs.clients[i].active && svs.clients[i].spawned)
		{
			MSG_WriteByte (&svs.clients[i].message, svc_print);
			MSG_WriteString (&svs.clients[i].message, string);
		}
	}
}

/*
=================
Host_ClientCommands

Send text over to the client to be executed
=================
*/
void Host_ClientCommands (const char *fmt, ...)
{
	va_list		argptr;
	char		string[1024];

	va_start (argptr,fmt);
	q_vsnprintf (string, sizeof(string), fmt, argptr);
	va_end (argptr);

	MSG_WriteByte (&host_client->message, svc_stufftext);
	MSG_WriteString (&host_client->message, string);
}

/*
=====================
SV_DropClient

Called when the player is getting totally kicked off the host
if (crash = true), don't bother sending signofs
=====================
*/
void SV_DropClient (qboolean crash)
{
	int		saveSelf;
	int		i;
	client_t *client;
	qboolean is_bot;

	is_bot = host_client->is_bot;
	if (!crash)
	{
		// send any final messages (don't check for errors)
		if (!is_bot && host_client->netconnection && NET_CanSendMessage (host_client->netconnection))
		{
			MSG_WriteByte (&host_client->message, svc_disconnect);
			NET_SendMessage (host_client->netconnection, &host_client->message);
		}

		if (host_client->edict && host_client->spawned)
		{
		// call the prog function for removing a client
		// this will set the body to a dead frame, among other things
			qcvm_t *oldvm = qcvm;
			PR_SwitchQCVM(NULL);
			PR_SwitchQCVM(&sv.qcvm);
			saveSelf = pr_global_struct->self;
			pr_global_struct->self = EDICT_TO_PROG(host_client->edict);
			PR_ExecuteProgram (pr_global_struct->ClientDisconnect);
			pr_global_struct->self = saveSelf;
			PR_SwitchQCVM(NULL);
			PR_SwitchQCVM(oldvm);
		}

		Sys_Printf ("Client %s removed\n",host_client->name);
	}

// break the net connection
	if (!is_bot && host_client->netconnection)
	{
		NET_Close (host_client->netconnection);
		host_client->netconnection = NULL;
	}

// free the client (the body stays around)
	host_client->active = false;
	host_client->is_bot = false;
	host_client->name[0] = 0;
	host_client->old_frags = -999999;
	if (!is_bot)
		net_activeconnections--;

// send notification to all clients
	for (i = 0, client = svs.clients; i < svs.maxclients; i++, client++)
	{
		if (!client->active)
			continue;
		MSG_WriteByte (&client->message, svc_updatename);
		MSG_WriteByte (&client->message, host_client - svs.clients);
		MSG_WriteString (&client->message, "");
		MSG_WriteByte (&client->message, svc_updatefrags);
		MSG_WriteByte (&client->message, host_client - svs.clients);
		MSG_WriteShort (&client->message, 0);
		MSG_WriteByte (&client->message, svc_updatecolors);
		MSG_WriteByte (&client->message, host_client - svs.clients);
		MSG_WriteByte (&client->message, 0);
	}
}

/*
==================
Host_ShutdownServer

This only happens at the end of a game, not between levels
==================
*/
void Host_ShutdownServer(qboolean crash)
{
	int		i;
	int		count;
	sizebuf_t	buf;
	byte		message[4];
	double	start;

	if (!sv.active)
		return;

	sv.active = false;

// stop all client sounds immediately
	if (cls.state == ca_connected)
		CL_Disconnect ();

// flush any pending messages - like the score!!!
	start = Sys_DoubleTime();
	do
	{
		count = 0;
		for (i=0, host_client = svs.clients ; i<svs.maxclients ; i++, host_client++)
		{
			if (host_client->active && host_client->message.cursize && !host_client->is_bot)
			{
				if (NET_CanSendMessage (host_client->netconnection))
				{
					NET_SendMessage(host_client->netconnection, &host_client->message);
					SZ_Clear (&host_client->message);
				}
				else
				{
					NET_GetMessage(host_client->netconnection);
					count++;
				}
			}
		}
		if ((Sys_DoubleTime() - start) > 3.0)
			break;
	}
	while (count);

// make sure all the clients know we're disconnecting
	buf.data = message;
	buf.maxsize = 4;
	buf.cursize = 0;
	buf.allowoverflow = false;
	buf.overflowed = false;
	buf.overflowed_once = false;
	buf.write_blocked = false;
	buf.write_locked = false;
	buf.blocked_file = NULL;
	buf.blocked_line = 0;
	buf.dbg_name = "shutdown_disconnect";
	buf.dbg_file = NULL;
	buf.dbg_line = 0;
	buf.dbg_msgkind = 0;
	buf.dbg_id = 0;
	buf.dbg_aux = 0;
	buf.bitpos = 0;
	MSG_WriteByte(&buf, svc_disconnect);
	count = NET_SendToAll(&buf, 5.0);
	if (count)
		Con_Printf("Host_ShutdownServer: NET_SendToAll failed for %u clients\n", count);

	PR_SwitchQCVM(&sv.qcvm);
	for (i = 0, host_client = svs.clients; i < svs.maxclients; i++, host_client++)
		if (host_client->active)
			SV_DropClient(crash);

	PR_SwitchQCVM(NULL);

//
// clear structures
//
//	memset (&sv, 0, sizeof(sv)); // ServerSpawn already do this by Host_ClearMemory
	memset (svs.clients, 0, svs.maxclientslimit*sizeof(client_t));
}


/*
================
Host_ClearMemory

This clears all the memory used by both the client and server, but does
not reinitialize anything.
================
*/
void Host_ClearMemory (void)
{
	R_ClearBoundingBoxes ();

	if (cl.qcvm.extfuncs.CSQC_Shutdown)
	{
		PR_SwitchQCVM(&cl.qcvm);
		PR_ExecuteProgram(qcvm->extfuncs.CSQC_Shutdown);
		qcvm->extfuncs.CSQC_Shutdown = 0;
		PR_SwitchQCVM(NULL);
	}

	Con_DPrintf ("Clearing memory\n");
	Mod_ClearAll ();
	Sky_ClearAll();
	PR_ClearProgs(&sv.qcvm);
	PR_ClearProgs(&cl.qcvm);
/* host_hunklevel MUST be set at this point */
	Hunk_FreeToLowMark (host_hunklevel);
	cls.signon = 0; // not CL_ClearSignons()
	memset (&sv, 0, sizeof(sv));

	CL_FreeState ();
}


//==============================================================================
//
// Main thread APC queue
//
//==============================================================================

typedef struct asyncproc_s
{
	void				(*func) (void *param);
	void				*param;
} asyncproc_t;

typedef struct asyncqueue_s
{
	size_t				head;
	size_t				tail;
	size_t				capacity;
	qboolean			teardown;
	SDL_mutex			*mutex;
	SDL_cond			*notfull;
	asyncproc_t			*procs;
} asyncqueue_t;

static asyncqueue_t		async_queue;

static void AsyncQueue_Init (asyncqueue_t *queue, size_t capacity)
{
	memset (queue, 0, sizeof (*queue));

	if (!capacity)
		capacity = 1024;
	else
		capacity = Q_nextPow2 (capacity);
	queue->capacity = capacity;
	capacity *= sizeof (queue->procs[0]);
	queue->procs = (asyncproc_t *) malloc (capacity);
	if (!queue->procs)
		Sys_Error ("AsyncQueue_Init: malloc failed on %" SDL_PRIu64 " bytes", (uint64_t) capacity);

	queue->mutex = SDL_CreateMutex ();
	if (!queue->mutex)
		Sys_Error ("AsyncQueue_Init: could not create mutex");

	queue->notfull = SDL_CreateCond ();
	if (!queue->notfull)
		Sys_Error ("AsyncQueue_Init: could not create condition variable");
}

static void AsyncQueue_Push (asyncqueue_t *queue, void (*func) (void *param), void *param)
{
	asyncproc_t *proc;

	if (!queue->mutex)
		return;
	SDL_LockMutex (queue->mutex);
	while (!queue->teardown && queue->tail - queue->head >= queue->capacity)
		SDL_CondWait (queue->notfull, queue->mutex);

	if (!queue->teardown)
	{
		proc = &queue->procs[(queue->tail++) & (queue->capacity - 1)];
		proc->func = func;
		proc->param = param;
	}

	SDL_UnlockMutex (queue->mutex);
}

static void AsyncQueue_Drain (asyncqueue_t *queue)
{
	SDL_LockMutex (queue->mutex);
	while (queue->head != queue->tail)
	{
		asyncproc_t *proc = &queue->procs[(queue->head++) & (queue->capacity - 1)];
		proc->func (proc->param);
		SDL_CondSignal (queue->notfull);
	}
	SDL_UnlockMutex (queue->mutex);
}

static void AsyncQueue_Destroy (asyncqueue_t *queue)
{
	if (!queue->mutex)
		return;

	SDL_LockMutex (queue->mutex);
	queue->teardown = true;
	SDL_UnlockMutex (queue->mutex);
	SDL_CondBroadcast (queue->notfull);

	AsyncQueue_Drain (queue);

	SDL_DestroyCond (queue->notfull);
	SDL_DestroyMutex (queue->mutex);
	memset (queue, 0, sizeof (*queue));
}

void Host_InvokeOnMainThread (void (*func) (void *param), void *param)
{
	AsyncQueue_Push (&async_queue, func, param);
}

//==============================================================================
//
// Host Frame
//
//==============================================================================

/*
===================
Host_GetFrameInterval
===================
*/
double Host_GetFrameInterval (void)
{
	if ((host_maxfps.value || cls.state == ca_disconnected) && !cls.timedemo)
	{
		float maxfps;
		if (cls.state == ca_disconnected)
		{
			maxfps = vid.refreshrate ? vid.refreshrate : 60.f;
			if (host_maxfps.value)
				maxfps = q_min (maxfps, host_maxfps.value);
			maxfps = CLAMP (10.f, maxfps, 1000.f);
		}
		else
		{
			maxfps = CLAMP (10.f, host_maxfps.value, 1000.f);
		}

		return 1.0 / maxfps;
	}

	return 0.0;
}

static int64_t Host_SecondsToUsec (double seconds)
{
	if (seconds <= 0.0)
		return 0;
	return (int64_t)(seconds * 1000000.0 + 0.5);
}

static void Sys_StepDebug_BeginFrame (void)
{
	if (sys_step_debug.value <= 0.0f)
		return;

	memset (&sys_step_debug_info, 0, sizeof (sys_step_debug_info));
	sys_step_debug_info.frame = host_framecount;
	sys_step_debug_info.cl_cmd_msec_min = 9999;
}

static void Sys_StepDebug_LogFrame (void)
{
	qboolean dump_frame;
	int debug_level;
	int cmd_msec_min;
	int cmd_msec_max;

	if (sys_step_debug.value <= 0.0f)
		return;

	debug_level = (int)sys_step_debug.value;
	dump_frame = sys_step_dump.value > 0.0f;
	if (dump_frame)
		Cvar_SetValueQuick (&sys_step_dump, 0.0f);

	sys_step_debug_info.realtime = realtime;
	sys_step_debug_info.host_frametime = host_frametime;
	sys_step_debug_info.host_rawframetime = host_rawframetime;
	sys_step_debug_info.cl_time = cl.time;
	sys_step_debug_info.cl_servertime = cl.latest_server_time;
	sys_step_debug_info.cl_snapshot_time = cl.snapshot_time;

	cmd_msec_min = sys_step_debug_info.cl_cmd_msec_min;
	cmd_msec_max = sys_step_debug_info.cl_cmd_msec_max;
	if (cmd_msec_min == 9999)
		cmd_msec_min = 0;

	Con_Printf ("STEPDBG frame %d rt %.6f ft %.6f raw %.6f cl.time %.6f cl.srv %.6f cl.snap %.6f\n",
		sys_step_debug_info.frame,
		sys_step_debug_info.realtime,
		sys_step_debug_info.host_frametime,
		sys_step_debug_info.host_rawframetime,
		sys_step_debug_info.cl_time,
		sys_step_debug_info.cl_servertime,
		sys_step_debug_info.cl_snapshot_time);
	JITTER_LOG ("STEPDBG frame %d rt %.6f ft %.6f raw %.6f cl.time %.6f cl.srv %.6f cl.snap %.6f\n",
		sys_step_debug_info.frame,
		sys_step_debug_info.realtime,
		sys_step_debug_info.host_frametime,
		sys_step_debug_info.host_rawframetime,
		sys_step_debug_info.cl_time,
		sys_step_debug_info.cl_servertime,
		sys_step_debug_info.cl_snapshot_time);
	Con_Printf ("[TIMING] rt %.6f host_frametime %.6f cl.time %.6f\n",
		sys_step_debug_info.realtime,
		sys_step_debug_info.host_frametime,
		sys_step_debug_info.cl_time);
	JITTER_LOG ("[TIMING] rt %.6f host_frametime %.6f cl.time %.6f\n",
		sys_step_debug_info.realtime,
		sys_step_debug_info.host_frametime,
		sys_step_debug_info.cl_time);
	Con_Printf ("[PRED] steps %d cmd.msec %d skipped %d zero %d\n",
		sys_step_debug_info.cl_pred_steps,
		sys_step_debug_info.cl_cmd_msec_last,
		sys_step_debug_info.cl_cmd_skipped_frame,
		sys_step_debug_info.cl_cmd_msec_zero > 0 ? 1 : 0);
	JITTER_LOG ("[PRED] steps %d cmd.msec %d skipped %d zero %d\n",
		sys_step_debug_info.cl_pred_steps,
		sys_step_debug_info.cl_cmd_msec_last,
		sys_step_debug_info.cl_cmd_skipped_frame,
		sys_step_debug_info.cl_cmd_msec_zero > 0 ? 1 : 0);
	Con_Printf ("STEPDBG sv ticks %d phys %d tick_us %lld frame_us %lld accum %lld->%lld maxsteps %d\n",
		sys_step_debug_info.sv_ticks,
		sys_step_debug_info.sv_physics_calls,
		(long long)sys_step_debug_info.tick_us,
		(long long)sys_step_debug_info.frame_us,
		(long long)sys_step_debug_info.sv_accum_us_before,
		(long long)sys_step_debug_info.sv_accum_us_after,
		(int)(sv_maxsteps_per_frame.value > 0 ? sv_maxsteps_per_frame.value : sv_tick_maxcatchup.value));
	JITTER_LOG ("STEPDBG sv ticks %d phys %d tick_us %lld frame_us %lld accum %lld->%lld maxsteps %d\n",
		sys_step_debug_info.sv_ticks,
		sys_step_debug_info.sv_physics_calls,
		(long long)sys_step_debug_info.tick_us,
		(long long)sys_step_debug_info.frame_us,
		(long long)sys_step_debug_info.sv_accum_us_before,
		(long long)sys_step_debug_info.sv_accum_us_after,
		(int)(sv_maxsteps_per_frame.value > 0 ? sv_maxsteps_per_frame.value : sv_tick_maxcatchup.value));
	Con_Printf ("STEPDBG cl sendcmd %d read %d parse %d predsteps %d cmds built %d sent %d packets %d accum %lld->%lld\n",
		sys_step_debug_info.cl_sendcmd_calls,
		sys_step_debug_info.cl_readfromserver_calls,
		sys_step_debug_info.cl_parse_calls,
		sys_step_debug_info.cl_pred_steps,
		sys_step_debug_info.cl_cmds_built,
		sys_step_debug_info.cl_cmds_sent,
		sys_step_debug_info.cl_cmd_packets,
		(long long)sys_step_debug_info.cl_cmd_accum_us_before,
		(long long)sys_step_debug_info.cl_cmd_accum_us_after);
	JITTER_LOG ("STEPDBG cl sendcmd %d read %d parse %d predsteps %d cmds built %d sent %d packets %d accum %lld->%lld\n",
		sys_step_debug_info.cl_sendcmd_calls,
		sys_step_debug_info.cl_readfromserver_calls,
		sys_step_debug_info.cl_parse_calls,
		sys_step_debug_info.cl_pred_steps,
		sys_step_debug_info.cl_cmds_built,
		sys_step_debug_info.cl_cmds_sent,
		sys_step_debug_info.cl_cmd_packets,
		(long long)sys_step_debug_info.cl_cmd_accum_us_before,
		(long long)sys_step_debug_info.cl_cmd_accum_us_after);
	Con_Printf ("STEPDBG cmd msec min %d max %d last %d no_cmd %d dropped %d wild %d zero %d over %d\n",
		cmd_msec_min,
		cmd_msec_max,
		sys_step_debug_info.cl_cmd_msec_last,
		sys_step_debug_info.cl_cmd_no_cmd,
		sys_step_debug_info.cl_cmds_dropped,
		sys_step_debug_info.cl_cmd_msec_wild,
		sys_step_debug_info.cl_cmd_msec_zero,
		sys_step_debug_info.cl_cmd_msec_over);
	JITTER_LOG ("STEPDBG cmd msec min %d max %d last %d no_cmd %d dropped %d wild %d zero %d over %d\n",
		cmd_msec_min,
		cmd_msec_max,
		sys_step_debug_info.cl_cmd_msec_last,
		sys_step_debug_info.cl_cmd_no_cmd,
		sys_step_debug_info.cl_cmds_dropped,
		sys_step_debug_info.cl_cmd_msec_wild,
		sys_step_debug_info.cl_cmd_msec_zero,
		sys_step_debug_info.cl_cmd_msec_over);

	if (sys_step_debug_info.player_valid)
	{
		Con_Printf ("STEPDBG player org %.2f %.2f %.2f -> %.2f %.2f %.2f vel %.2f %.2f %.2f -> %.2f %.2f %.2f\n",
			sys_step_debug_info.player_origin_before[0],
			sys_step_debug_info.player_origin_before[1],
			sys_step_debug_info.player_origin_before[2],
			sys_step_debug_info.player_origin_after[0],
			sys_step_debug_info.player_origin_after[1],
			sys_step_debug_info.player_origin_after[2],
			sys_step_debug_info.player_vel_before[0],
			sys_step_debug_info.player_vel_before[1],
			sys_step_debug_info.player_vel_before[2],
			sys_step_debug_info.player_vel_after[0],
			sys_step_debug_info.player_vel_after[1],
			sys_step_debug_info.player_vel_after[2]);
		JITTER_LOG ("STEPDBG player org %.2f %.2f %.2f -> %.2f %.2f %.2f vel %.2f %.2f %.2f -> %.2f %.2f %.2f\n",
			sys_step_debug_info.player_origin_before[0],
			sys_step_debug_info.player_origin_before[1],
			sys_step_debug_info.player_origin_before[2],
			sys_step_debug_info.player_origin_after[0],
			sys_step_debug_info.player_origin_after[1],
			sys_step_debug_info.player_origin_after[2],
			sys_step_debug_info.player_vel_before[0],
			sys_step_debug_info.player_vel_before[1],
			sys_step_debug_info.player_vel_before[2],
			sys_step_debug_info.player_vel_after[0],
			sys_step_debug_info.player_vel_after[1],
			sys_step_debug_info.player_vel_after[2]);
		Con_Printf ("STEPDBG player onground %d->%d groundent %d->%d class %s trace frac %.3f normalz %.3f solid %d/%d fallback %d mover %d gvel %.2f %.2f %.2f tick_us %lld\n",
			sys_step_debug_info.player_onground_before,
			sys_step_debug_info.player_onground_after,
			sys_step_debug_info.player_groundent_before,
			sys_step_debug_info.player_groundent_after,
			(sys_step_debug_info.player_groundent_after > 0) ? PR_GetString (PROG_TO_EDICT(sys_step_debug_info.player_groundent_after)->v.classname) : "world",
			sys_step_debug_info.player_ground_trace_fraction,
			sys_step_debug_info.player_ground_trace_normal_z,
			sys_step_debug_info.player_ground_trace_startsolid,
			sys_step_debug_info.player_ground_trace_allsolid,
			sys_step_debug_info.player_ground_trace_fallback,
			sys_step_debug_info.player_ground_is_mover,
			sys_step_debug_info.player_ground_vel[0],
			sys_step_debug_info.player_ground_vel[1],
			sys_step_debug_info.player_ground_vel[2],
			(long long)sys_step_debug_info.tick_us);
		JITTER_LOG ("STEPDBG player onground %d->%d groundent %d->%d class %s trace frac %.3f normalz %.3f solid %d/%d fallback %d mover %d gvel %.2f %.2f %.2f tick_us %lld\n",
			sys_step_debug_info.player_onground_before,
			sys_step_debug_info.player_onground_after,
			sys_step_debug_info.player_groundent_before,
			sys_step_debug_info.player_groundent_after,
			(sys_step_debug_info.player_groundent_after > 0) ? PR_GetString (PROG_TO_EDICT(sys_step_debug_info.player_groundent_after)->v.classname) : "world",
			sys_step_debug_info.player_ground_trace_fraction,
			sys_step_debug_info.player_ground_trace_normal_z,
			sys_step_debug_info.player_ground_trace_startsolid,
			sys_step_debug_info.player_ground_trace_allsolid,
			sys_step_debug_info.player_ground_trace_fallback,
			sys_step_debug_info.player_ground_is_mover,
			sys_step_debug_info.player_ground_vel[0],
			sys_step_debug_info.player_ground_vel[1],
			sys_step_debug_info.player_ground_vel[2],
			(long long)sys_step_debug_info.tick_us);
		Con_Printf ("[PHYS] onground %d->%d groundent %d->%d basevel %.2f %.2f %.2f\n",
			sys_step_debug_info.player_onground_before,
			sys_step_debug_info.player_onground_after,
			sys_step_debug_info.player_groundent_before,
			sys_step_debug_info.player_groundent_after,
			sys_step_debug_info.player_basevel_after[0],
			sys_step_debug_info.player_basevel_after[1],
			sys_step_debug_info.player_basevel_after[2]);
		JITTER_LOG ("[PHYS] onground %d->%d groundent %d->%d basevel %.2f %.2f %.2f\n",
			sys_step_debug_info.player_onground_before,
			sys_step_debug_info.player_onground_after,
			sys_step_debug_info.player_groundent_before,
			sys_step_debug_info.player_groundent_after,
			sys_step_debug_info.player_basevel_after[0],
			sys_step_debug_info.player_basevel_after[1],
			sys_step_debug_info.player_basevel_after[2]);
		if (sys_step_debug_info.player_ground_is_mover)
		{
			vec3_t vel_delta;
			VectorSubtract (sys_step_debug_info.player_vel_after, sys_step_debug_info.player_vel_before, vel_delta);
			Con_Printf ("[MOVER] groundent %d mover_vel %.2f %.2f %.2f player_dvel %.2f %.2f %.2f\n",
				sys_step_debug_info.player_groundent_after,
				sys_step_debug_info.player_ground_vel[0],
				sys_step_debug_info.player_ground_vel[1],
				sys_step_debug_info.player_ground_vel[2],
				vel_delta[0], vel_delta[1], vel_delta[2]);
			JITTER_LOG ("[MOVER] groundent %d mover_vel %.2f %.2f %.2f player_dvel %.2f %.2f %.2f\n",
				sys_step_debug_info.player_groundent_after,
				sys_step_debug_info.player_ground_vel[0],
				sys_step_debug_info.player_ground_vel[1],
				sys_step_debug_info.player_ground_vel[2],
				vel_delta[0], vel_delta[1], vel_delta[2]);
		}
	}

	if (sys_step_debug_info.warn_host_frametime_clamped)
		Con_Printf ("STEPWARN host_frametime clamped raw %.6f ft %.6f\n", host_rawframetime, host_frametime);
	if (sys_step_debug_info.warn_zero_frametime)
		Con_Printf ("STEPWARN host_frametime <= 0\n");
	if (sys_step_debug_info.warn_zero_sim_dt)
		Con_Printf ("STEPWARN sim dt is zero\n");
	if (sys_step_debug_info.warn_many_ticks)
		Con_Printf ("STEPWARN max ticks exceeded in frame\n");
	if (sys_step_debug_info.warn_host_frametime_clamped)
		JITTER_LOG ("STEPWARN host_frametime clamped raw %.6f ft %.6f\n", host_rawframetime, host_frametime);
	if (sys_step_debug_info.warn_zero_frametime)
		JITTER_LOG ("STEPWARN host_frametime <= 0\n");
	if (sys_step_debug_info.warn_zero_sim_dt)
		JITTER_LOG ("STEPWARN sim dt is zero\n");
	if (sys_step_debug_info.warn_many_ticks)
		JITTER_LOG ("STEPWARN max ticks exceeded in frame\n");
	if (sys_step_debug_info.host_frametime < 0.001 || sys_step_debug_info.host_frametime > 0.05)
	{
		Con_Printf ("[WARN] host_frametime out of range %.6f\n", sys_step_debug_info.host_frametime);
		JITTER_LOG ("[WARN] host_frametime out of range %.6f\n", sys_step_debug_info.host_frametime);
	}
	if (sys_step_debug_info.cl_cmd_msec_zero > 0)
	{
		Con_Printf ("[WARN] cmd.msec == 0 encountered count %d\n", sys_step_debug_info.cl_cmd_msec_zero);
		JITTER_LOG ("[WARN] cmd.msec == 0 encountered count %d\n", sys_step_debug_info.cl_cmd_msec_zero);
	}
	if (sys_step_debug_info.sv_ticks > 4)
	{
		Con_Printf ("[WARN] simulation ran %d ticks in one frame\n", sys_step_debug_info.sv_ticks);
		JITTER_LOG ("[WARN] simulation ran %d ticks in one frame\n", sys_step_debug_info.sv_ticks);
	}
	if (sys_step_debug_info.player_valid
		&& sys_step_debug_info.player_groundent_before != sys_step_debug_info.player_groundent_after
		&& VectorLength (sys_step_debug_info.player_vel_before) < 5.0f
		&& VectorLength (sys_step_debug_info.player_vel_after) < 5.0f)
	{
		Con_Printf ("[WARN] groundent changed at near-zero velocity %d -> %d\n",
			sys_step_debug_info.player_groundent_before,
			sys_step_debug_info.player_groundent_after);
		JITTER_LOG ("[WARN] groundent changed at near-zero velocity %d -> %d\n",
			sys_step_debug_info.player_groundent_before,
			sys_step_debug_info.player_groundent_after);
	}

	if (sys_step_hitch_ms.value > 0.0f)
	{
		double hitch_ms = sys_step_hitch_ms.value;
		if (host_rawframetime * 1000.0 >= hitch_ms)
			Con_Printf ("STEPWARN hitch %.2fms >= %.2fms\n", host_rawframetime * 1000.0, hitch_ms);
		if (host_rawframetime * 1000.0 >= hitch_ms)
			JITTER_LOG ("STEPWARN hitch %.2fms >= %.2fms\n", host_rawframetime * 1000.0, hitch_ms);
	}

	if (dump_frame && debug_level <= 0)
	{
		Con_Printf ("STEPDBG dump requested; enable sys_step_debug for continuous logging.\n");
		JITTER_LOG ("STEPDBG dump requested; enable sys_step_debug for continuous logging.\n");
	}
}

/*
===================
Host_AdvanceTime
===================
*/
static void Host_AdvanceTime (double dt)
{
	realtime += dt;
	host_frametime = host_rawframetime = realtime - oldrealtime;
	oldrealtime = realtime;

	//johnfitz -- host_timescale is more intuitive than host_framerate
	if (host_timescale.value > 0)
		host_frametime *= host_timescale.value;
	//johnfitz
	else if (host_framerate.value > 0)
		host_frametime = host_framerate.value;
	else if (host_maxfps.value)// don't allow really long or short frames
		host_frametime = CLAMP (0.0001, host_frametime, 0.1); //johnfitz -- use CLAMP

	if (sys_step_debug.value > 0.0f && host_maxfps.value)
	{
		if (host_frametime != host_rawframetime)
			sys_step_debug_info.warn_host_frametime_clamped = 1;
	}
	if (jitter_time_debug.value > 0.0f && host_frametime != host_rawframetime)
	{
		Con_Printf ("JITWARN host_frametime_clamped\n");
		JITTER_LOG ("JITWARN host_frametime_clamped\n");
	}
	if (sys_step_debug.value > 0.0f && host_frametime <= 0.0)
		sys_step_debug_info.warn_zero_frametime = 1;
}

/*
===================
Host_GetConsoleCommands

Add them exactly as if they had been typed at the console
===================
*/
void Host_GetConsoleCommands (void)
{
	const char	*cmd;

	if (!isDedicated)
		return;	// no stdin necessary in graphical mode

	while (1)
	{
		cmd = Sys_ConsoleInput ();
		if (!cmd)
			break;
		Cbuf_AddText (cmd);
	}
}

/*
==================
Host_CheckAutosave
==================
*/
static void Host_CheckAutosave (void)
{
	float health_change, speed, elapsed, score;

	if (!sv_player)
	{
		Con_Printf ("NETDBG autosave skipped: sv_player NULL cl %d\n", SV_CurrentClientIndex ());
		JITTER_LOG ("NETDBG autosave skipped: sv_player NULL cl %d\n", SV_CurrentClientIndex ());
		return;
	}

	if (cls.state != ca_connected)
	{
		Con_Printf ("NETDBG autosave skipped: cls.state %d\n", cls.state);
		JITTER_LOG ("NETDBG autosave skipped: cls.state %d\n", cls.state);
		return;
	}

	if (cls.signon != SIGNONS)
	{
		Con_Printf ("NETDBG autosave skipped: signon %d\n", cls.signon);
		JITTER_LOG ("NETDBG autosave skipped: signon %d\n", cls.signon);
		return;
	}

	if (cl.intermission)
	{
		Con_Printf ("NETDBG autosave skipped: intermission\n");
		JITTER_LOG ("NETDBG autosave skipped: intermission\n");
		return;
	}

	if (!sv_autosave.value || sv_autosave_interval.value <= 0.f || svs.maxclients != 1 || sv_player->v.health <= 0.f)
		return;

	if (cls.signon == SIGNONS)
	{
		// Track new secrets
		if (pr_global_struct->found_secrets != sv.autosave.prev_secrets)
		{
			sv.autosave.prev_secrets = pr_global_struct->found_secrets;
			sv.autosave.secret_boost = 1.f;
		}
		else
			sv.autosave.secret_boost = q_max (0.f, sv.autosave.secret_boost - host_frametime / 1.5f);
	}

	// Track health changes
	if (!sv.autosave.prev_health)
		sv.autosave.prev_health = sv_player->v.health;
	health_change = sv_player->v.health - sv.autosave.prev_health;
	if (health_change < 0.f)
		if (health_change < -3.f || sv_player->v.health < 100.f || sv_player->v.watertype == CONTENTS_SLIME || sv_player->v.watertype == CONTENTS_LAVA)
			sv.autosave.hurt_time = qcvm->time;
	sv.autosave.prev_health = sv_player->v.health;

	// Track attacking
	if (sv_player->v.button0)
		sv.autosave.shoot_time = qcvm->time;

	// Time spent with cheats active doesn't count
	if (sv_player->v.movetype == MOVETYPE_NOCLIP || (int)sv_player->v.flags & (FL_GODMODE|FL_NOTARGET))
	{
		sv.autosave.cheat += host_frametime;
		return;
	}

	// Don't save if the player has been hurt recently
	if (qcvm->time - sv.autosave.hurt_time < 3.f)
		return;

	// Don't save if the player has fired recently
	if (qcvm->time - sv.autosave.shoot_time < 3.f)
		return;

	// Only save when the player slows down a bit
	speed = VectorLength (sv_player->v.velocity);
	if (speed > 100.f)
		return;

	// Copper's func_void holds the player at the bottom for a bit before inflicting damage,
	// so we can't assume it's safe to save just because we're no longer falling
	if ((int)sv_player->v.movetype == MOVETYPE_NONE)
		return;

	// Don't save too often
	elapsed = qcvm->time - sv.autosave.time - sv.autosave.cheat;
	if (elapsed < 3.f)
		return;

	// Compute a normalized autosave score

	// Base value is the fraction of the autosave interval already passed
	score = elapsed / sv_autosave_interval.value;
	// Scale down the score if health + armor is below 100 (save less often with lower health)
	score *= q_min (100.f, (sv_player->v.health + sv_player->v.armortype * sv_player->v.armorvalue)) / 100.f;
	// Boost the score right after picking up health
	score += q_max (0.f, health_change) / 100.f;
	// Lower score a bit based on speed (favor standing still/slowing down)
	score -= (speed / 100.f) * 0.25f;
	// Boost the score after finding a secret
	score += sv.autosave.secret_boost * 0.25f;
	// Boost the score after teleporting
	score += CLAMP (0.f, 1.f - (qcvm->time - sv_player->v.teleport_time) / 1.5f, 1.f) * 0.5f;

	// Only save if the score is high enough
	if (score < 1.f)
		return;

	sv.autosave.time = qcvm->time;
	sv.autosave.cheat = 0;
	Cbuf_AddText (va ("save \"autosave/%s\" 0\n", sv.name));
}

/*
==================
Host_ServerFrame
==================
*/
static void SV_RunOneTick (double tick_dt)
{
	static double next_sv_skip_log = 0.0;
	static int paused_frame_count = 0;
	const int paused_frame_threshold = 120;
	const int paused_force_threshold = 300;

	if (sys_step_debug.value > 0.0f)
		sys_step_debug_info.sv_ticks++;

	host_frametime = tick_dt;
	pr_global_struct->frametime = host_frametime;

	// read client messages and run per-client think
	SV_RunClients ();

	// move things around and think
	if (!sv.paused)
	{
		paused_frame_count = 0;
		SV_Physics ();
	}
	else
	{
		paused_frame_count++;
		if (paused_frame_count == paused_frame_threshold)
		{
			Con_Printf ("NETDBG SV_RunOneTick paused for %d frames (paused=%d key_dest=%d)\n",
				paused_frame_count, sv.paused, key_dest);
			JITTER_LOG ("NETDBG SV_RunOneTick paused for %d frames (paused=%d key_dest=%d)\n",
				paused_frame_count, sv.paused, key_dest);
		}
		if (realtime >= next_sv_skip_log)
		{
			Con_Printf ("NETDBG SV_Physics skipped (SV_RunOneTick) paused=%d key_dest=%d\n",
				sv.paused, key_dest);
			JITTER_LOG ("NETDBG SV_Physics skipped (SV_RunOneTick) paused=%d key_dest=%d\n",
				sv.paused, key_dest);
			next_sv_skip_log = realtime + 1.0;
		}
		if (paused_frame_count >= paused_force_threshold && key_dest == key_game && svs.maxclients <= 1 && !cls.demoplayback)
		{
			sv.paused = false;
			cl.paused = false;
			paused_frame_count = 0;
			Con_Printf ("NETDBG SV_RunOneTick forced unpause (paused=%d key_dest=%d)\n",
				sv.paused, key_dest);
			JITTER_LOG ("NETDBG SV_RunOneTick forced unpause (paused=%d key_dest=%d)\n",
				sv.paused, key_dest);
			MSG_WriteByte (&sv.reliable_datagram, svc_setpause);
			MSG_WriteByte (&sv.reliable_datagram, sv.paused);
		}
	}

	if (jitter_time_debug.value > 0.0f && sv_player)
	{
		Con_Printf ("JITTIME SV tick svt=%.6f svft=%.6f plorg=%.2f %.2f %.2f plvel=%.2f %.2f %.2f ongr=%d grent=%d\n",
			qcvm->time, host_frametime,
			sv_player->v.origin[0], sv_player->v.origin[1], sv_player->v.origin[2],
			sv_player->v.velocity[0], sv_player->v.velocity[1], sv_player->v.velocity[2],
			((int)sv_player->v.flags & FL_ONGROUND) ? 1 : 0, (int)sv_player->v.groundentity);
		JITTER_LOG ("JITTIME SV tick svt=%.6f svft=%.6f plorg=%.2f %.2f %.2f plvel=%.2f %.2f %.2f ongr=%d grent=%d\n",
			qcvm->time, host_frametime,
			sv_player->v.origin[0], sv_player->v.origin[1], sv_player->v.origin[2],
			sv_player->v.velocity[0], sv_player->v.velocity[1], sv_player->v.velocity[2],
			((int)sv_player->v.flags & FL_ONGROUND) ? 1 : 0, (int)sv_player->v.groundentity);
	}

	if (sys_step_debug.value > 0.0f)
	{
		Con_Printf ("[PHYS] sv.time %.6f sv.frametime %.6f ticks_this_frame %d\n",
			qcvm->time, host_frametime, sys_step_debug_info.sv_ticks);
		JITTER_LOG ("[PHYS] sv.time %.6f sv.frametime %.6f ticks_this_frame %d\n",
			qcvm->time, host_frametime, sys_step_debug_info.sv_ticks);
		if (sys_step_debug_info.player_valid)
		{
			Con_Printf ("[PHYS] player org %.2f %.2f %.2f -> %.2f %.2f %.2f vel %.2f %.2f %.2f -> %.2f %.2f %.2f onground %d groundent %d basevel %.2f %.2f %.2f\n",
				sys_step_debug_info.player_origin_before[0],
				sys_step_debug_info.player_origin_before[1],
				sys_step_debug_info.player_origin_before[2],
				sys_step_debug_info.player_origin_after[0],
				sys_step_debug_info.player_origin_after[1],
				sys_step_debug_info.player_origin_after[2],
				sys_step_debug_info.player_vel_before[0],
				sys_step_debug_info.player_vel_before[1],
				sys_step_debug_info.player_vel_before[2],
				sys_step_debug_info.player_vel_after[0],
				sys_step_debug_info.player_vel_after[1],
				sys_step_debug_info.player_vel_after[2],
				sys_step_debug_info.player_onground_after,
				sys_step_debug_info.player_groundent_after,
				sys_step_debug_info.player_basevel_after[0],
				sys_step_debug_info.player_basevel_after[1],
				sys_step_debug_info.player_basevel_after[2]);
			JITTER_LOG ("[PHYS] player org %.2f %.2f %.2f -> %.2f %.2f %.2f vel %.2f %.2f %.2f -> %.2f %.2f %.2f onground %d groundent %d basevel %.2f %.2f %.2f\n",
				sys_step_debug_info.player_origin_before[0],
				sys_step_debug_info.player_origin_before[1],
				sys_step_debug_info.player_origin_before[2],
				sys_step_debug_info.player_origin_after[0],
				sys_step_debug_info.player_origin_after[1],
				sys_step_debug_info.player_origin_after[2],
				sys_step_debug_info.player_vel_before[0],
				sys_step_debug_info.player_vel_before[1],
				sys_step_debug_info.player_vel_before[2],
				sys_step_debug_info.player_vel_after[0],
				sys_step_debug_info.player_vel_after[1],
				sys_step_debug_info.player_vel_after[2],
				sys_step_debug_info.player_onground_after,
				sys_step_debug_info.player_groundent_after,
				sys_step_debug_info.player_basevel_after[0],
				sys_step_debug_info.player_basevel_after[1],
				sys_step_debug_info.player_basevel_after[2]);
		}
	}
}

void Host_ServerFrame (void)
{
	int		i, active; //johnfitz
	edict_t	*ent; //johnfitz
	static double next_sv_report_time = 0.0;
	static double next_sv_zero_frametime_log = 0.0;
	static double next_sv_stall_log = 0.0;
	static double last_sv_time = -1.0;
	static double next_sv_tick_log = 0.0;
	static double sv_accum = 0.0;
	static int64_t sv_accum_us = 0;
	static double sv_next_snapshot_time = 0.0;
	int		active_clients = 0;
	double		clamped_frametime;
	double		tick_dt;
	double		net_dt;
	double		jit_accum_before;
	int64_t		frame_us;
	int64_t		tick_dt_us;
	int64_t		max_accum_us;
	int		ticks = 0;
	int		maxcatchup;
	int		sends = 0;
	const int	max_sends = 2;

// accumulate the world state
	clamped_frametime = host_frametime;
	if (clamped_frametime <= 0.0f)
	{
		if (realtime >= next_sv_zero_frametime_log)
		{
			Con_Printf ("NETDBG sv.frametime <= 0 (%.6f) clamping; sv.time %.3f\n",
				host_frametime, qcvm->time);
			JITTER_LOG ("NETDBG sv.frametime <= 0 (%.6f) clamping; sv.time %.3f\n",
				host_frametime, qcvm->time);
			next_sv_zero_frametime_log = realtime + 1.0;
		}
		clamped_frametime = 0.0001f;
	}
	if (clamped_frametime > 0.1f)
		clamped_frametime = 0.1f;
	frame_us = Host_SecondsToUsec (clamped_frametime);
	if (sys_step_debug.value > 0.0f)
	{
		sys_step_debug_info.frame_us = frame_us;
		sys_step_debug_info.sv_accum_us_before = sv_accum_us;
		if (frame_us == 0)
			sys_step_debug_info.warn_zero_sim_dt = 1;
	}

	// Q3MINI BEGIN
	qboolean use_fixed_tick = (sv_tickrate.value > 0.0f);
	if (use_fixed_tick)
	{
		tick_dt = 1.0 / (sv_tickrate.value > 1.0 ? sv_tickrate.value : 1.0);
		tick_dt_us = Host_SecondsToUsec (tick_dt);
		if (tick_dt_us <= 0)
			tick_dt_us = 1;
		if (sys_step_debug.value > 0.0f)
			sys_step_debug_info.tick_us = tick_dt_us;

		jit_accum_before = sv_fixedtick.value > 0.0f ? (double)sv_accum_us / 1000000.0 : sv_accum;

		if (sv_fixedtick.value > 0.0f)
		{
			sv_accum_us += frame_us;
			if (sv_accum_us < 0)
				sv_accum_us = 0;
		}
	}
	else
	{
		tick_dt = clamped_frametime;
		tick_dt_us = Host_SecondsToUsec (tick_dt);
		if (tick_dt_us <= 0)
			tick_dt_us = 1;
		if (sys_step_debug.value > 0.0f)
			sys_step_debug_info.tick_us = tick_dt_us;
		jit_accum_before = 0.0;
	}

	for (i = 0; i < svs.maxclients; i++)
	{
		if (svs.clients[i].active)
			active_clients++;
	}
	if (sv_tick_debug.value && realtime >= next_sv_report_time)
	{
		Con_Printf ("NETDBG sv.time %.3f sv.frametime %.6f edicts %d active_clients %d\n",
			qcvm->time, clamped_frametime, qcvm->num_edicts, active_clients);
		JITTER_LOG ("NETDBG sv.time %.3f sv.frametime %.6f edicts %d active_clients %d\n",
			qcvm->time, clamped_frametime, qcvm->num_edicts, active_clients);
		next_sv_report_time = realtime + 1.0;
	}

// check for new clients
	SV_CheckForNewClients ();

	maxcatchup = (int)(sv_maxsteps_per_frame.value > 0.0f ? sv_maxsteps_per_frame.value : sv_tick_maxcatchup.value);
	if (maxcatchup < 1)
		maxcatchup = 1;
	if (use_fixed_tick)
	{
		if (sv_fixedtick.value > 0.0f)
		{
			max_accum_us = tick_dt_us * (int64_t)maxcatchup * 2;
			if (max_accum_us < tick_dt_us)
				max_accum_us = tick_dt_us;
			if (sv_accum_us > max_accum_us)
			{
				sv_accum_us = max_accum_us;
				if (sys_step_debug.value > 0.0f)
					sys_step_debug_info.warn_many_ticks = 1;
			}
			while (sv_accum_us >= tick_dt_us && ticks < maxcatchup)
			{
				SV_RunOneTick (tick_dt);
				sv_accum_us -= tick_dt_us;
				ticks++;
			}
			if (sv_accum_us >= tick_dt_us && ticks >= maxcatchup)
			{
				if (!sv_timewarp.value)
					sv_accum_us = tick_dt_us;
				if (sys_step_debug.value > 0.0f)
					sys_step_debug_info.warn_many_ticks = 1;
				if (sv_tick_debug.value)
				{
					Con_Printf ("NETDBG sv_tick catchup clamped ticks %d accum %.6f dt %.6f\n",
						ticks, (double)sv_accum_us / 1000000.0, tick_dt);
					JITTER_LOG ("NETDBG sv_tick catchup clamped ticks %d accum %.6f dt %.6f\n",
						ticks, (double)sv_accum_us / 1000000.0, tick_dt);
				}
			}
		}
		else
		{
			sv_accum += clamped_frametime;
			while (sv_accum >= tick_dt && ticks < maxcatchup)
			{
				SV_RunOneTick (tick_dt);
				sv_accum -= tick_dt;
				ticks++;
			}
			if (sv_accum >= tick_dt && ticks >= maxcatchup)
			{
				if (!sv_timewarp.value)
					sv_accum = tick_dt;
				if (sv_tick_debug.value)
				{
					Con_Printf ("NETDBG sv_tick catchup clamped ticks %d accum %.6f dt %.6f\n",
						ticks, sv_accum, tick_dt);
					JITTER_LOG ("NETDBG sv_tick catchup clamped ticks %d accum %.6f dt %.6f\n",
						ticks, sv_accum, tick_dt);
				}
			}
		}
	}
	else
	{
		SV_RunOneTick (tick_dt);
		ticks = 1;
	}
	// Q3MINI END

	if (jitter_time_debug.value > 0.0f)
	{
		double accum_before = jit_accum_before;
		double accum_after = sv_fixedtick.value > 0.0f ? (double)sv_accum_us / 1000000.0 : sv_accum;
		Con_Printf ("JITTIME SV frame rt=%.6f hf=%.6f ticks=%d acc=%.6f->%.6f\n", realtime, clamped_frametime, ticks, accum_before, accum_after);
		JITTER_LOG ("JITTIME SV frame rt=%.6f hf=%.6f ticks=%d acc=%.6f->%.6f\n", realtime, clamped_frametime, ticks, accum_before, accum_after);
		if (ticks == 0 || ticks > 3)
		{
			Con_Printf ("JITWARN ticks_this_frame==0 or >3\n");
			JITTER_LOG ("JITWARN ticks_this_frame==0 or >3\n");
		}
	}

	if (sys_step_debug.value > 0.0f)
		sys_step_debug_info.sv_accum_us_after = sv_accum_us;

	if (last_sv_time >= 0.0)
	{
		if (qcvm->time + 0.001 < last_sv_time)
		{
			last_sv_time = qcvm->time;
			sv_accum = 0.0;
			sv_accum_us = 0;
			sv_next_snapshot_time = qcvm->time;
		}
		else if (sv_tick_debug.value && qcvm->time <= last_sv_time && realtime >= next_sv_stall_log)
		{
			Con_Printf ("NETDBG sv.time stalled at %.3f (frametime %.6f)\n",
				qcvm->time, clamped_frametime);
			JITTER_LOG ("NETDBG sv.time stalled at %.3f (frametime %.6f)\n",
				qcvm->time, clamped_frametime);
			next_sv_stall_log = realtime + 1.0;
		}
	}
	last_sv_time = qcvm->time;

//johnfitz -- devstats
	if (cls.signon == SIGNONS)
	{
		for (i=0, active=0; i<qcvm->num_edicts; i++)
		{
			ent = EDICT_NUM(i);
			if (!ent->free)
				active++;
		}
		if (active > 600 && dev_peakstats.edicts <= 600)
			Con_DWarning ("%i edicts exceeds standard limit of 600 (max = %d).\n", active, qcvm->max_edicts);
		dev_stats.edicts = active;
		dev_peakstats.edicts = q_max(active, dev_peakstats.edicts);
	}
//johnfitz

	// Q3MINI BEGIN
	{
		double netrate = sv_sendrate.value;

		if (netrate <= 0.0)
			netrate = sv_netrate.value;
		if (netrate <= 1.0)
			netrate = sv_tickrate.value > 1.0 ? sv_tickrate.value : 1.0;
		net_dt = 1.0 / netrate;
	}
	// Q3MINI END
	if (sv_next_snapshot_time <= 0.0 || qcvm->time < sv_next_snapshot_time - 1.0)
		sv_next_snapshot_time = qcvm->time;
	while (qcvm->time >= sv_next_snapshot_time && sends < max_sends)
	{
		int temp_bytes = sv.datagram.cursize;

		SV_SendClientMessages ();
		if (sv_tick_debug.value)
		{
			Con_Printf ("NETDBG sv_snap_send time %.3f next %.3f clients %d bytes %d\n",
				qcvm->time, sv_next_snapshot_time + net_dt, active_clients, temp_bytes);
			JITTER_LOG ("NETDBG sv_snap_send time %.3f next %.3f clients %d bytes %d\n",
				qcvm->time, sv_next_snapshot_time + net_dt, active_clients, temp_bytes);
		}
		SV_ClearDatagram ();
		sv_next_snapshot_time += net_dt;
		sends++;
	}
	if (active_clients == 0 && sv.datagram.cursize > 0)
		SV_ClearDatagram ();
	if (sv_tick_debug.value && realtime >= next_sv_tick_log)
	{
		double accum_s = sv_fixedtick.value > 0.0f ? (double)sv_accum_us / 1000000.0 : sv_accum;

		Con_Printf ("NETDBG sv_tick frame_dt %.6f tick_dt %.6f ticks %d accum %.6f time %.3f sends %d\n",
			clamped_frametime, tick_dt, ticks, accum_s, qcvm->time, sends);
		JITTER_LOG ("NETDBG sv_tick frame_dt %.6f tick_dt %.6f ticks %d accum %.6f time %.3f sends %d\n",
			clamped_frametime, tick_dt, ticks, accum_s, qcvm->time, sends);
		next_sv_tick_log = realtime + 1.0;
	}

	Host_CheckAutosave ();
}

typedef struct summary_s {
	struct {
		int		players;
		int		max_players;
		int		skill;
		int		monsters;
		int		total_monsters;
		int		secrets;
		int		total_secrets;
	}			stats;
	char		map[countof (cl.mapname)];
} summary_t;

/*
==================
CountActiveClients
==================
*/
static int CountActiveClients (void)
{
	int i, active;
	for (i = active = 0; i < cl.maxclients; i++)
		if (cl.scores[i].name[0])
			active++;
	return active;
}

/*
==================
GetGameSummary
==================
*/
static void GetGameSummary (summary_t *s)
{
	if (!cl_titlestats.value || cls.state != ca_connected || cls.signon != SIGNONS)
	{
		s->map[0] = 0;
		memset (&s->stats, 0, sizeof (s->stats));
	}
	else
	{
		q_strlcpy (s->map, cl.mapname, countof (s->map));
		s->stats.players        = CountActiveClients ();
		s->stats.max_players    = cl.maxclients;
		s->stats.skill          = (int) skill.value;
		s->stats.monsters       = cl.stats[STAT_MONSTERS];
		s->stats.total_monsters = cl.stats[STAT_TOTALMONSTERS];
		s->stats.secrets        = cl.stats[STAT_SECRETS];
		s->stats.total_secrets  = cl.stats[STAT_TOTALSECRETS];
	}
}

/*
==================
UpdateWindowTitle
==================
*/
static void UpdateWindowTitle (void)
{
	static float timeleft = 0.f;
	static summary_t last = {{-1}}; // negative value to force initial update
	summary_t current;

	timeleft -= host_frametime;
	if (timeleft > 0.f)
		return;
	timeleft = 0.125f;

	GetGameSummary (&current);
	if (!strcmp (current.map, last.map) && !memcmp (&current.stats, &last.stats, sizeof (current.stats)))
		return;
	last = current;

	if (current.map[0])
	{
		char cleanname[sizeof (cl.levelname)];
		char utf8name[4 * sizeof (cl.levelname)];
		char title[1024];

		Mod_SanitizeMapDescription (cleanname, sizeof (cleanname), cl.levelname);

		UTF8_FromQuake (utf8name, sizeof (utf8name), cleanname);
		q_snprintf (title, sizeof (title),
			utf8name[0] ?
				"%s (%s)  |  skill %d  |  %d/%d kills  |  %d/%d secrets  -  " WINDOW_TITLE_STRING :
				"%s%s  |  skill %d  |  %d/%d kills  |  %d/%d secrets  -  " WINDOW_TITLE_STRING,
			utf8name, current.map,
			current.stats.skill,
			current.stats.monsters, current.stats.total_monsters,
			current.stats.secrets, current.stats.total_secrets
		);
		VID_SetWindowTitle (title);

		if (current.stats.max_players > 1)
			Steam_SetStatus_Multiplayer (current.stats.players, current.stats.max_players, utf8name[0] ? utf8name : current.map);
		else
			Steam_SetStatus_SinglePlayer (utf8name[0] ? utf8name : current.map);
	}
	else
	{
		VID_SetWindowTitle (WINDOW_TITLE_STRING);
		if (cls.state == ca_connected)
			Steam_ClearStatus ();
		else
			Steam_SetStatus_Menu ();
	}
}

static void CL_LoadCSProgs (void)
{
	PR_ClearProgs (&cl.qcvm);
	if (pr_checkextension.value && !cl_nocsqc.value)
	{ // only try to use csqc if qc extensions are enabled.
		PR_SwitchQCVM (&cl.qcvm);

		// try csprogs.dat first, then fall back on progs.dat in case someone tried merging the two.
		// we only care about it if it actually contains a CSQC_DrawHud, otherwise its either just a (misnamed) ssqc progs or a full csqc progs that would just
		// crash us on 3d stuff.
		if ((PR_LoadProgs ("csprogs.dat", false) && (qcvm->extfuncs.CSQC_DrawHud||qcvm->extfuncs.CSQC_DrawScores)) ||
		    (PR_LoadProgs ("progs.dat", false) && qcvm->extfuncs.CSQC_DrawHud))
		{
			qcvm->max_edicts = CLAMP (MIN_EDICTS, (int)max_edicts.value, MAX_EDICTS);
			qcvm->edicts = (edict_t *)malloc (qcvm->max_edicts * qcvm->edict_size);
			qcvm->num_edicts = qcvm->reserved_edicts = 1;
			memset (qcvm->edicts, 0, qcvm->num_edicts * qcvm->edict_size);

			if (!qcvm->extfuncs.CSQC_DrawHud)
			{ // no simplecsqc entry points... abort entirely!
				PR_ClearProgs (qcvm);
				PR_SwitchQCVM (NULL);
				return;
			}

			// set a few globals, if they exist
			if (qcvm->extglobals.maxclients)
				*qcvm->extglobals.maxclients = cl.maxclients;
			pr_global_struct->time = cl.time;
			pr_global_struct->mapname = PR_SetEngineString (cl.mapname);
			pr_global_struct->total_monsters = cl.stats[STAT_TOTALMONSTERS];
			pr_global_struct->total_secrets = cl.stats[STAT_TOTALSECRETS];
			pr_global_struct->deathmatch = cl.gametype;
			pr_global_struct->coop = (cl.gametype == GAME_COOP) && cl.maxclients != 1;
			if (qcvm->extglobals.player_localnum)
				*qcvm->extglobals.player_localnum = cl.viewentity - 1; // this is a guess, but is important for scoreboards.

			// set a few worldspawn fields too
			qcvm->edicts->v.solid = SOLID_BSP;
			qcvm->edicts->v.modelindex = 1;
			qcvm->edicts->v.model = PR_SetEngineString (cl.worldmodel->name);
			VectorCopy (cl.worldmodel->mins, qcvm->edicts->v.mins);
			VectorCopy (cl.worldmodel->maxs, qcvm->edicts->v.maxs);
			qcvm->edicts->v.message = PR_SetEngineString (cl.levelname);

			// and call the init function... if it exists.
			if (qcvm->extfuncs.CSQC_Init)
			{
				G_FLOAT (OFS_PARM0) = false;
				G_INT (OFS_PARM1) = PR_SetEngineString ("Ironwail");
				G_FLOAT (OFS_PARM2) = 10000 * IRONWAIL_VER_MAJOR + 100 * IRONWAIL_VER_MINOR + IRONWAIL_VER_PATCH;
				PR_ExecuteProgram (qcvm->extfuncs.CSQC_Init);
			}
		}
		else
			PR_ClearProgs (qcvm);
		PR_SwitchQCVM (NULL);
	}
}

/*
==================
Host_PrintTimes
==================
*/
static void Host_PrintTimes (const double times[], const char *names[], int count, qboolean showtotal)
{
	char line[1024];
	double total = 0.0;
	int i, worst;

	for (i = 0, worst = -1; i < count; i++)
	{
		if (worst == -1 || times[i] > times[worst])
			worst = i;
		total += times[i];
	}

	if (showtotal)
		q_snprintf (line, sizeof (line), "%5.2f tot | ", total * 1000.0);
	else
		line[0] = '\0';

	for (i = 0; i < count; i++)
	{
		char entry[256];
		q_snprintf (entry, sizeof (entry), "%5.2f %s", times[i] * 1000.0, names[i]);
		if (i == worst)
			COM_TintString (entry, entry, sizeof (entry));
		if (i != 0)
			q_strlcat (line, " | ", sizeof (line));
		q_strlcat (line, entry, sizeof (line));
	}

	Con_Printf ("%s\n", line);
}

/*
==================
Host_Frame

Runs all active servers
==================
*/
void _Host_Frame (double time)
{
	static double	accumtime = 0;
	double time1, time2, time3;
	qboolean ranserver = false;

	time1 = Sys_DoubleTime ();

	if (setjmp (host_abortserver) )
		return;			// something bad happened, or the server disconnected

// keep the random time dependent
	rand ();

	Sys_StepDebug_BeginFrame ();

// decide the simulation time
	accumtime += host_netinterval?CLAMP(0.0, time, 0.2):0.0;	//for renderer/server isolation
	Host_AdvanceTime (time);
	if (jitter_time_debug.value > 0.0f && host_frametime <= 0.0)
	{
		Con_Printf ("JITWARN host_frame_skipped\n");
		JITTER_LOG ("JITWARN host_frame_skipped\n");
	}

// run async procs
	AsyncQueue_Drain (&async_queue);

// get new key events
	Key_UpdateForDest ();
	IN_UpdateInputMode ();
	Sys_SendKeyEvents ();

// allow mice or other external controllers to add commands
	IN_Commands ();

//check the stdin for commands (dedicated servers)
	Host_GetConsoleCommands ();

// process console commands
	Cbuf_Execute ();

	NET_Poll();

	if (cl.sendprespawn)
	{
		CL_LoadCSProgs();

		cl.sendprespawn = false;
		MSG_WriteByte (&cls.message, clc_stringcmd);
		MSG_WriteString (&cls.message, "prespawn");
		vid.recalc_refdef = true;
	}

	CL_AccumulateCmd ();

	//Run the server+networking (client->server->client), at a different rate from everyt
	if (accumtime >= host_netinterval)
	{
		float realframetime = host_frametime;
		if (host_netinterval)
		{
			host_frametime = q_max(accumtime, (double)host_netinterval);
			accumtime -= host_frametime;
			if (host_timescale.value > 0)
				host_frametime *= host_timescale.value;
			else if (host_framerate.value)
				host_frametime = host_framerate.value;
		}
		else
			accumtime -= host_netinterval;
		CL_SendCmd ();
		if (sv.active)
		{
			PR_SwitchQCVM(&sv.qcvm);
			Host_ServerFrame ();
			PR_SwitchQCVM(NULL);
		}
		host_frametime = realframetime;
		Cbuf_Waited();
		ranserver = true;
	}

// fetch results from server
	if (cls.state == ca_connected)
		CL_ReadFromServer ();

// update video
	if (host_speeds.value)
		time2 = Sys_DoubleTime ();

	SCR_UpdateScreen ();

	CL_RunParticles (); //johnfitz -- seperated from rendering

	if (host_speeds.value)
		time3 = Sys_DoubleTime ();

// update audio
	BGM_Update();	// adds music raw samples and/or advances midi driver
	if (cls.signon == SIGNONS)
	{
		S_Update (r_origin, vpn, vright, vup);
		CL_DecayLights ();
	}
	else
		S_Update (vec3_origin, vec3_origin, vec3_origin, vec3_origin);

	CDAudio_Update();
	UpdateWindowTitle();

	if (host_speeds.value)
	{
		static double pass[3] = {0.0, 0.0, 0.0};
		static double elapsed = 0.0;
		static int numframes = 0;
		static int numserverframes = 0;

		time1 = time2 - time1;
		time2 = time3 - time2;
		time3 = Sys_DoubleTime () - time3;

		if (ranserver || host_speeds.value < 0.f)
		{
			pass[0] += time1;
			numserverframes++;
		}
		numframes++;
		pass[1] += time2;
		pass[2] += time3;
		elapsed += time;

		if (elapsed >= host_speeds.value * 0.375)
		{
			const char *names[3] = {"server", "gfx", "snd"};
			pass[0] /= q_max (numserverframes, 1);
			pass[1] /= numframes;
			pass[2] /= numframes;

			Host_PrintTimes (pass, names, countof (pass), host_speeds.value < 0.f);

			pass[0] = pass[1] = pass[2] = elapsed = 0.0;
			numframes = numserverframes = 0;
		}
	}

	host_framecount++;

	Sys_StepDebug_LogFrame ();
}

void Host_Frame (double time)
{
	double	time1, time2;
	static double	timetotal;
	static int		timecount;
	int		i, c, m;

	if (!serverprofile.value)
	{
		_Host_Frame (time);
		return;
	}

	time1 = Sys_DoubleTime ();
	_Host_Frame (time);
	time2 = Sys_DoubleTime ();

	timetotal += time2 - time1;
	timecount++;

	if (timecount < 1000)
		return;

	m = timetotal*1000/timecount;
	timecount = 0;
	timetotal = 0;
	c = 0;
	for (i = 0; i < svs.maxclients; i++)
	{
		if (svs.clients[i].active)
			c++;
	}

	Con_Printf ("serverprofile: %2i clients %2i msec\n",  c,  m);
}

/*
====================
Host_Init
====================
*/
void Host_Init (void)
{
	if (standard_quake)
		minimum_memory = MINIMUM_MEMORY;
	else	minimum_memory = MINIMUM_MEMORY_LEVELPAK;

	if (COM_CheckParm ("-minmemory"))
		host_parms->memsize = minimum_memory;

	if (host_parms->memsize < minimum_memory)
		Sys_Error ("Only %4.1f megs of memory available, can't execute game", host_parms->memsize / (float)0x100000);

	Memory_Init (host_parms->membase, host_parms->memsize);
	AsyncQueue_Init (&async_queue, 1024);
	Cbuf_Init ();
	Cmd_Init ();
	LOG_Init (host_parms);
	Cvar_Init (); //johnfitz
	COM_Init ();
	COM_InitFilesystem ();
	Host_InitLocal ();
	W_LoadWadFile (); //johnfitz -- filename is now hard-coded for honesty
	if (cls.state != ca_dedicated)
	{
		Key_Init ();
		Con_Init ();
	}
	PR_Init ();
	Mod_Init ();
	NET_Init ();
	SV_Init ();

	Con_Printf ("Exe: " __TIME__ " " __DATE__ " (%s %d-bit)\n", SDL_GetPlatform (), (int)sizeof(void*)*8);
	Con_Printf ("%4.1f megabyte heap\n", host_parms->memsize/ (1024*1024.0));

	if (cls.state != ca_dedicated)
	{
		host_colormap = (byte *)COM_LoadHunkFile ("gfx/colormap.lmp", NULL);
		if (!host_colormap)
			Con_Warning ("Couldn't load gfx/colormap.lmp\n");

		V_Init ();
		Chase_Init ();
		M_Init ();
		VID_Init ();
		IN_Init ();
		TexMgr_Init (); //johnfitz
		Draw_Init ();
		SCR_Init ();
		R_Init ();
		S_Init ();
		CDAudio_Init ();
		BGM_Init();
		Sbar_Init ();
		CL_Init ();
		ExtraMaps_Init (); //johnfitz
		DemoList_Init (); //ericw
		SaveList_Init ();
		SkyList_Init ();
		M_CheckMods ();
	}

	LOC_Init (); // for 2021 rerelease support.

	Hunk_AllocName (0, "-HOST_HUNKLEVEL-");
	host_hunklevel = Hunk_LowMark ();

	host_initialized = true;
	Con_Printf ("\n========= Quake Initialized =========\n\n");

	if (!COM_CheckParm ("-nomapchecks") && Sys_IsStartedFromMapEditor ())
	{
		Con_Printf (
			"Level editing environment detected, enabling map_checks\n"
			"(pass -nomapchecks or set map_checks to 0 to disable)\n"
		);
		Cvar_SetValueQuick (&map_checks, 1.f);
	}

	if (cls.state != ca_dedicated)
	{
		Cbuf_InsertText ("exec quake.rc\n");
	// johnfitz -- in case the vid mode was locked during vid_init, we can unlock it now.
		// note: two leading newlines because the command buffer swallows one of them.
		Cbuf_AddText ("\n\nvid_unlock\n");
	}

	if (cls.state == ca_dedicated)
	{
		Cbuf_AddText ("exec autoexec.cfg\n");
		Cbuf_AddText ("stuffcmds");
		Cbuf_Execute ();
		if (!sv.active)
			Cbuf_AddText ("map start\n");
	}
}


/*
===============
Host_Shutdown

FIXME: this is a callback from Sys_Quit and Sys_Error.  It would be better
to run quit through here before the final handoff to the sys code.
===============
*/
void Host_Shutdown(void)
{
	static qboolean isdown = false;

	if (isdown)
	{
		printf ("recursive shutdown\n");
		return;
	}
	isdown = true;

// keep Con_Printf from trying to update the screen
	scr_disabled_for_loading = true;

	Steam_Shutdown ();

	AsyncQueue_Destroy (&async_queue);

	Host_ShutdownSave ();
	Host_WriteConfiguration ();
	Jitter_Log_Close ();

// stop downloads before shutting down networking
        Modlist_ShutDown ();

        NET_Shutdown ();

        if (cls.state != ca_dedicated)
	{
		if (con_initialized)
		History_Shutdown ();
		ExtraMaps_ShutDown ();
		BGM_Shutdown();
		CDAudio_Shutdown ();
		S_Shutdown ();
		IN_Shutdown ();
		VID_Shutdown();
        }

        LOG_Close ();
        LOC_Shutdown ();
}
