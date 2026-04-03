#include "quakedef.h"
#include "bot_main.h"
#include "bot_ai.h"
#include "bot_nav2.h"

cvar_t bot_debug = {"bot_debug", "0", CVAR_NONE};
cvar_t bot_nav_debug = {"bot_nav_debug", "0", CVAR_NONE};
cvar_t bot_think_debug = {"bot_think_debug", "0", CVAR_NONE};
cvar_t bot_aim_debug = {"bot_aim_debug", "0", CVAR_NONE};
cvar_t bot_skill = {"bot_skill", "0.55", CVAR_ARCHIVE};
cvar_t bot_use_nav2 = {"bot_use_nav2", "1", CVAR_ARCHIVE};
cvar_t bot_call_clientconnect = {"bot_call_clientconnect", "0", CVAR_ARCHIVE};
cvar_t bot_count = {"bot_count", "0", CVAR_NONE};

static bot_state_t g_bot_states[MAX_SCOREBOARD];
static qboolean g_bot_initialized;
static qboolean g_bot_nav_enabled_cache;

static int Bot_ClientIndex (const client_t *client)
{
	if (!client)
		return -1;
	return (int) (client - svs.clients);
}

int Bot_GetActiveCount (void)
{
	int i;
	int count = 0;

	for (i = 0; i < svs.maxclients; ++i)
	{
		if (svs.clients[i].active && svs.clients[i].isbot)
			count++;
	}

	return count;
}

static void Bot_UpdateCountCvar (void)
{
	Cvar_SetValueQuick (&bot_count, (float) Bot_GetActiveCount ());
}

static bot_state_t *Bot_GetStateForClient (client_t *client)
{
	int index = Bot_ClientIndex (client);
	if ((unsigned int) index >= (unsigned int) countof (g_bot_states))
		return NULL;
	return &g_bot_states[index];
}

static int Bot_GenerateUniqueName (char *out_name, size_t out_size)
{
	int suffix;

	for (suffix = 1; suffix < 1000; ++suffix)
	{
		int i;
		qboolean used = false;
		q_snprintf (out_name, out_size, "bot_%d", suffix);
		for (i = 0; i < svs.maxclients; ++i)
		{
			if (!svs.clients[i].active)
				continue;
			if (q_strcasecmp (svs.clients[i].name, out_name) == 0)
			{
				used = true;
				break;
			}
		}
		if (!used)
			return suffix;
	}

	q_strlcpy (out_name, "bot", out_size);
	return 0;
}

static int Bot_PickTeam (int requested_team)
{
	int t;
	int best_team;
	int best_count;
	int team_counts[4] = {0, 0, 0, 0};

	if (requested_team >= 1 && requested_team <= 4)
		return requested_team;

	if (!teamplay.value)
		return 1;

	for (t = 0; t < svs.maxclients; ++t)
	{
		client_t *client = &svs.clients[t];
		int team;

		if (!client->active || !client->edict)
			continue;
		team = (int) client->edict->v.team;
		if (team >= 1 && team <= 4)
			team_counts[team - 1]++;
	}

	best_team = 1;
	best_count = team_counts[0];
	for (t = 1; t < 4; ++t)
	{
		if (team_counts[t] < best_count)
		{
			best_count = team_counts[t];
			best_team = t + 1;
		}
	}

	return best_team;
}

static void Bot_AssignColorsForTeam (client_t *client, int clientnum, int team)
{
	int top;
	int bottom;

	if (team <= 0)
		team = 1;

	top = (clientnum * 3 + 2) % 14;
	bottom = CLAMP (0, team - 1, 13);
	client->colors = top * 16 + bottom;
}

static void Bot_BroadcastClientInfo (int clientnum)
{
	client_t *client;

	if ((unsigned int) clientnum >= (unsigned int) svs.maxclients)
		return;
	client = &svs.clients[clientnum];

	MSG_WriteByte (&sv.reliable_datagram, svc_updatename);
	MSG_WriteByte (&sv.reliable_datagram, clientnum);
	MSG_WriteString (&sv.reliable_datagram, client->name);

	MSG_WriteByte (&sv.reliable_datagram, svc_updatefrags);
	MSG_WriteByte (&sv.reliable_datagram, clientnum);
	MSG_WriteShort (&sv.reliable_datagram, (int) client->edict->v.frags);

	MSG_WriteByte (&sv.reliable_datagram, svc_updatecolors);
	MSG_WriteByte (&sv.reliable_datagram, clientnum);
	MSG_WriteByte (&sv.reliable_datagram, client->colors);
}

static qboolean Bot_PerformClientSpawn (client_t *client, qboolean announce)
{
	int i;
	int saved_self;
	client_t *saved_host_client;
	edict_t *saved_sv_player;
	qcvm_t *saved_vm;
	edict_t *ent;

	if (!client || !client->active || !client->edict)
		return false;
	if (!sv.active || sv.state != ss_active)
		return false;
	if (!sv.qcvm.progs || !sv.qcvm.edicts)
		return false;

	ent = client->edict;
	saved_host_client = host_client;
	saved_sv_player = sv_player;
	saved_vm = qcvm;

	PR_SwitchQCVM (NULL);
	PR_SwitchQCVM (&sv.qcvm);
	saved_self = pr_global_struct->self;

	host_client = client;
	sv_player = ent;

	if (!sv.loadgame)
	{
		memset (&ent->v, 0, qcvm->progs->entityfields * 4);
		ent->v.colormap = NUM_FOR_EDICT (ent);
		ent->v.team = (client->colors & 15) + 1;
		ent->v.netname = PR_SetEngineString (client->name);

		for (i = 0; i < NUM_SPAWN_PARMS; ++i)
			(&pr_global_struct->parm1)[i] = client->spawn_parms[i];

		pr_global_struct->time = qcvm->time;
		pr_global_struct->self = EDICT_TO_PROG (ent);
		/*
		 * Bot-Spawn ist nicht an ein echtes Net-Client-Handshake gebunden.
		 * Einige Mods nehmen in ClientConnect implizit eine gültige netconnection
		 * an und können bei Bots hart abstürzen. Deshalb standardmäßig aus.
		 */
		if (bot_call_clientconnect.value != 0.f && pr_global_struct->ClientConnect)
			PR_ExecuteProgram (pr_global_struct->ClientConnect);
		if (pr_global_struct->PutClientInServer)
			PR_ExecuteProgram (pr_global_struct->PutClientInServer);
	}

	client->spawned = true;
	client->sendsignon = PRESPAWN_DONE;
	client->dropasap = false;
	client->last_message = realtime;
	memset (&client->cmd, 0, sizeof (client->cmd));
	SZ_Clear (&client->message);
	client->old_frags = (int) ent->v.frags;

	pr_global_struct->self = saved_self;
	PR_SwitchQCVM (NULL);
	PR_SwitchQCVM (saved_vm);
	host_client = saved_host_client;
	sv_player = saved_sv_player;

	if (announce)
		Sys_Printf ("%s entered the game\n", client->name);

	return true;
}

static qboolean Bot_AddClient (int requested_team)
{
	int i;
	int clientnum = -1;
	int first_client_slot;
	int team;
	client_t *client;
	bot_state_t *state;
	char botname[sizeof (client->name)];

	if (!sv.active || sv.state != ss_active)
	{
		Con_Printf ("bot_add requires an active server\n");
		return false;
	}

	/*
	 * In listen servers slot 0 belongs to the local host player.
	 * Startup command buffers can invoke bot_add before that slot becomes active,
	 * so reserve it to avoid corrupting local-client setup.
	 */
	first_client_slot = (cls.state == ca_dedicated) ? 0 : 1;

	for (i = first_client_slot; i < svs.maxclients; ++i)
	{
		if (!svs.clients[i].active)
		{
			clientnum = i;
			break;
		}
	}

	if (clientnum < 0)
	{
		Con_Printf ("bot_add failed: no free client slot\n");
		return false;
	}

	team = Bot_PickTeam (requested_team);
	Bot_GenerateUniqueName (botname, sizeof (botname));

	client = &svs.clients[clientnum];
	memset (client, 0, sizeof (*client));
	client->active = true;
	client->spawned = false;
	client->isbot = true;
	client->message.data = client->msgbuf;
	client->message.maxsize = sizeof (client->msgbuf);
	client->message.allowoverflow = true;
	q_strlcpy (client->name, botname, sizeof (client->name));
	Bot_AssignColorsForTeam (client, clientnum, team);

	{
		qcvm_t *saved_vm = qcvm;
		PR_SwitchQCVM (NULL);
		PR_SwitchQCVM (&sv.qcvm);
		client->edict = EDICT_NUM (clientnum + 1);
		memset (client->spawn_parms, 0, sizeof (client->spawn_parms));
		if (pr_global_struct->SetNewParms)
		{
			PR_ExecuteProgram (pr_global_struct->SetNewParms);
			for (i = 0; i < NUM_SPAWN_PARMS; ++i)
				client->spawn_parms[i] = (&pr_global_struct->parm1)[i];
		}
		PR_SwitchQCVM (NULL);
		PR_SwitchQCVM (saved_vm);
	}

	state = &g_bot_states[clientnum];
	memset (state, 0, sizeof (*state));
	state->inuse = true;
	state->clientnum = clientnum;
	state->forced_team = team;
	q_strlcpy (state->name, client->name, sizeof (state->name));
	BotAI_ResetState (state);

	if (!Bot_PerformClientSpawn (client, true))
	{
		Con_Printf ("bot_add failed: spawn failed\n");
		memset (client, 0, sizeof (*client));
		memset (state, 0, sizeof (*state));
		return false;
	}

	Bot_BroadcastClientInfo (clientnum);
	Bot_UpdateCountCvar ();

	if (bot_debug.value)
		Con_Printf ("Bot: added %s (slot %d team %d)\n", client->name, clientnum + 1, team);

	return true;
}

static void Bot_Add_f (void)
{
	int team = 0;

	if (Cmd_Argc () >= 2)
		team = Q_atoi (Cmd_Argv (1));

	Bot_AddClient (team);
}

static void Bot_AddTeam_f (void)
{
	int team;

	if (Cmd_Argc () != 2)
	{
		Con_Printf ("usage: bot_addteam <1-4>\n");
		return;
	}

	team = CLAMP (1, Q_atoi (Cmd_Argv (1)), 4);
	Bot_AddClient (team);
}

static void Bot_KickAll_f (void)
{
	int i;
	int removed = 0;
	client_t *saved = host_client;

	if (!sv.active)
	{
		Con_Printf ("bot_kickall requires an active server\n");
		return;
	}

	for (i = 0; i < svs.maxclients; ++i)
	{
		if (!svs.clients[i].active || !svs.clients[i].isbot)
			continue;

		host_client = &svs.clients[i];
		SV_DropClient (false);
		removed++;
	}

	host_client = saved;
	Bot_UpdateCountCvar ();
	Con_Printf ("bot_kickall removed %d bot%s\n", PLURAL (removed));
}

qboolean Bot_IsClientBot (const client_t *client)
{
	return client && client->isbot;
}

static void Bot_CheckNavReload (void)
{
	qboolean want_nav;

	if (!sv.active || sv.state != ss_active)
		return;

	want_nav = (bot_use_nav2.value != 0.f);
	if (want_nav == g_bot_nav_enabled_cache)
		return;

	if (want_nav)
		BotNav_LoadForMap (sv.name);
	else
		BotNav_Shutdown ();

	g_bot_nav_enabled_cache = want_nav;

	if (bot_debug.value)
		Con_Printf ("Bot: nav2 %s\n", want_nav ? "enabled" : "disabled");
}

void Bot_RunFrameForClient (client_t *client)
{
	bot_state_t *state;
	usercmd_t cmd;
	vec3_t v_angle;
	qboolean attack;
	int impulse;

	if (!client || !client->active || !client->isbot)
		return;

	Bot_CheckNavReload ();

	state = Bot_GetStateForClient (client);
	if (!state)
		return;
	if (!state->inuse)
	{
		memset (state, 0, sizeof (*state));
		state->inuse = true;
		state->clientnum = Bot_ClientIndex (client);
		q_strlcpy (state->name, client->name, sizeof (state->name));
		state->forced_team = (client->colors & 15) + 1;
		BotAI_ResetState (state);
	}

	if (!client->spawned)
	{
		if (!Bot_PerformClientSpawn (client, false))
		{
			memset (&client->cmd, 0, sizeof (client->cmd));
			return;
		}
		BotAI_ResetState (state);
	}

	BotAI_BuildCommand (state, client, &cmd, v_angle, &attack, &impulse);
	client->cmd = cmd;
	VectorCopy (v_angle, client->edict->v.v_angle);
	client->edict->v.button0 = attack ? 1.f : 0.f;
	client->edict->v.button2 = 0.f;
	if (impulse)
		client->edict->v.impulse = impulse;
}

void Bot_OnServerSpawnedMap (void)
{
	int i;

	if (!g_bot_initialized)
		return;

	if (bot_use_nav2.value)
		BotNav_LoadForMap (sv.name);
	else
		BotNav_Shutdown ();
	g_bot_nav_enabled_cache = (bot_use_nav2.value != 0.f);

	BotAI_OnMapSpawn ();

	for (i = 0; i < svs.maxclients; ++i)
	{
		client_t *client = &svs.clients[i];
		bot_state_t *state = &g_bot_states[i];

		if (!client->active || !client->isbot)
			continue;

		state->inuse = true;
		state->clientnum = i;
		q_strlcpy (state->name, client->name, sizeof (state->name));
		state->forced_team = (client->colors & 15) + 1;
		BotAI_ResetState (state);

		client->spawned = false;
		Bot_PerformClientSpawn (client, false);
	}

	Bot_UpdateCountCvar ();
}

void Bot_OnClientDropped (client_t *client)
{
	int index;

	if (!client || !client->isbot)
		return;

	index = Bot_ClientIndex (client);
	if ((unsigned int) index < (unsigned int) countof (g_bot_states))
		memset (&g_bot_states[index], 0, sizeof (g_bot_states[index]));

	client->isbot = false;
	Bot_UpdateCountCvar ();
}

void Bot_Init (void)
{
	if (g_bot_initialized)
		return;

	Cvar_RegisterVariable (&bot_debug);
	Cvar_RegisterVariable (&bot_nav_debug);
	Cvar_RegisterVariable (&bot_think_debug);
	Cvar_RegisterVariable (&bot_aim_debug);
	Cvar_RegisterVariable (&bot_skill);
	Cvar_RegisterVariable (&bot_use_nav2);
	Cvar_RegisterVariable (&bot_call_clientconnect);
	Cvar_RegisterVariable (&bot_count);

	Cmd_AddCommand ("bot_add", Bot_Add_f);
	Cmd_AddCommand ("bot_addteam", Bot_AddTeam_f);
	Cmd_AddCommand ("bot_kickall", Bot_KickAll_f);

	memset (g_bot_states, 0, sizeof (g_bot_states));
	g_bot_nav_enabled_cache = (bot_use_nav2.value != 0.f);
	Bot_UpdateCountCvar ();
	g_bot_initialized = true;
}

void Bot_Shutdown (void)
{
	memset (g_bot_states, 0, sizeof (g_bot_states));
	BotNav_Shutdown ();
	Bot_UpdateCountCvar ();
	g_bot_nav_enabled_cache = false;
}
