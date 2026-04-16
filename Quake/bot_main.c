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
cvar_t bot_call_clientconnect = {"bot_call_clientconnect", "1", CVAR_ARCHIVE};
cvar_t bot_full_think_ms = {"bot_full_think_ms", "75", CVAR_ARCHIVE};
cvar_t bot_count = {"bot_count", "0", CVAR_NONE};

static bot_state_t g_bot_states[MAX_SCOREBOARD];
static qboolean g_bot_initialized;
static qboolean g_bot_nav_enabled_cache;

static int Bot_MaxTrackedClients (void)
{
	return q_min (svs.maxclients, (int) countof (g_bot_states));
}

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

typedef struct bot_template_s
{
	char		filename[MAX_OSPATH];
	char		name[32];
	float		skill;
	int		preferred_weapon;
	int		least_preferred_weapon;
	float		agility;
	char		taunts[8][96];
	int		taunt_count;
} bot_template_t;

#define BOT_TEMPLATE_MAX_COUNT 64

static bot_template_t g_bot_templates[BOT_TEMPLATE_MAX_COUNT];
static int g_bot_template_count;

static bot_state_t *Bot_GetStateForClient (client_t *client)
{
	int index = Bot_ClientIndex (client);
	if ((unsigned int) index >= (unsigned int) countof (g_bot_states))
		return NULL;
	return &g_bot_states[index];
}

static void Bot_MakeUniqueName (const char *base_name, char *out_name, size_t out_size)
{
	int suffix;
	char candidate[32];
	const char *base = (base_name && base_name[0]) ? base_name : "bot";

	for (suffix = 1; suffix < 1000; ++suffix)
	{
		int i;
		qboolean used = false;
		if (suffix == 1)
			q_strlcpy (candidate, base, sizeof (candidate));
		else
			q_snprintf (candidate, sizeof (candidate), "%s_%d", base, suffix);
		for (i = 0; i < svs.maxclients; ++i)
		{
			if (!svs.clients[i].active)
				continue;
			if (q_strcasecmp (svs.clients[i].name, candidate) == 0)
			{
				used = true;
				break;
			}
		}
		if (!used)
		{
			q_strlcpy (out_name, candidate, out_size);
			return;
		}
	}

	q_strlcpy (out_name, base, out_size);
}

static int Bot_WeaponFromToken (const char *token)
{
	if (!token)
		return 0;
	if (!q_strcasecmp (token, "axe")) return IT_AXE;
	if (!q_strcasecmp (token, "shotgun") || !q_strcasecmp (token, "sg")) return IT_SHOTGUN;
	if (!q_strcasecmp (token, "supershotgun") || !q_strcasecmp (token, "super_shotgun") || !q_strcasecmp (token, "ssg")) return IT_SUPER_SHOTGUN;
	if (!q_strcasecmp (token, "nailgun")) return IT_NAILGUN;
	if (!q_strcasecmp (token, "supernailgun") || !q_strcasecmp (token, "super_nailgun")) return IT_SUPER_NAILGUN;
	if (!q_strcasecmp (token, "grenadelauncher") || !q_strcasecmp (token, "grenade_launcher") || !q_strcasecmp (token, "gl")) return IT_GRENADE_LAUNCHER;
	if (!q_strcasecmp (token, "rocketlauncher") || !q_strcasecmp (token, "rocket_launcher") || !q_strcasecmp (token, "rl")) return IT_ROCKET_LAUNCHER;
	if (!q_strcasecmp (token, "lightning") || !q_strcasecmp (token, "lg")) return IT_LIGHTNING;
	if (!q_strcasecmp (token, "superlightning") || !q_strcasecmp (token, "super_lightning")) return IT_LIGHTNING;
	return 0;
}

static void Bot_TemplateDefaults (bot_template_t *template, const char *path)
{
	memset (template, 0, sizeof (*template));
	q_strlcpy (template->filename, path, sizeof (template->filename));
	COM_StripExtension (COM_SkipPath (path), template->name, sizeof (template->name));
	if (!template->name[0])
		q_strlcpy (template->name, "bot", sizeof (template->name));
	template->skill = bot_skill.value;
	template->preferred_weapon = 0;
	template->least_preferred_weapon = 0;
	template->agility = 1.f;
	template->taunt_count = 0;
}

static qboolean Bot_TemplateParseFile (const char *path, const char *data, bot_template_t *template)
{
	const char *cursor = data;

	Bot_TemplateDefaults (template, path);
	while ((cursor = COM_Parse (cursor)))
	{
		if (!com_token[0])
			break;
		if (!q_strcasecmp (com_token, "{"))
			continue;
		if (!q_strcasecmp (com_token, "}"))
			break;

		if (!q_strcasecmp (com_token, "name"))
		{
			if (!(cursor = COM_Parse (cursor)))
				break;
			q_strlcpy (template->name, com_token, sizeof (template->name));
		}
		else if (!q_strcasecmp (com_token, "skill"))
		{
			if (!(cursor = COM_Parse (cursor)))
				break;
			template->skill = CLAMP (0.1f, (float) Q_atof (com_token), 1.f);
		}
		else if (!q_strcasecmp (com_token, "preferred_weapon"))
		{
			if (!(cursor = COM_Parse (cursor)))
				break;
			template->preferred_weapon = Bot_WeaponFromToken (com_token);
		}
		else if (!q_strcasecmp (com_token, "least_preferred_weapon"))
		{
			if (!(cursor = COM_Parse (cursor)))
				break;
			template->least_preferred_weapon = Bot_WeaponFromToken (com_token);
		}
		else if (!q_strcasecmp (com_token, "agility"))
		{
			if (!(cursor = COM_Parse (cursor)))
				break;
			template->agility = CLAMP (0.4f, (float) Q_atof (com_token), 1.4f);
		}
		else if (!q_strcasecmp (com_token, "taunt"))
		{
			if (template->taunt_count >= (int) countof (template->taunts))
			{
				if (!(cursor = COM_Parse (cursor)))
					break;
				continue;
			}
			if (!(cursor = COM_Parse (cursor)))
				break;
			q_strlcpy (template->taunts[template->taunt_count], com_token, sizeof (template->taunts[template->taunt_count]));
			template->taunt_count++;
		}
		else if (cursor)
		{
			const char *ignored = COM_Parse (cursor);
			if (!ignored)
				break;
			cursor = ignored;
		}
	}

	if (!template->name[0])
		COM_StripExtension (COM_SkipPath (path), template->name, sizeof (template->name));

	return true;
}

static void Bot_LoadTemplates (void)
{
	findfile_t *find;
	char dir[MAX_OSPATH];
	int loaded = 0;

	g_bot_template_count = 0;
	q_strlcpy (dir, com_gamedir, sizeof (dir));
	q_strlcat (dir, "/bots", sizeof (dir));
	find = Sys_FindFirst (dir, "bot");
	if (!find)
	{
		Con_DPrintf ("Bot: no template directory at %s\n", dir);
		return;
	}

	for (; find; find = Sys_FindNext (find))
	{
		char path[MAX_OSPATH];
		const char *data;
		int mark;
		bot_template_t template;

		if (find->attribs & FA_DIRECTORY)
			continue;

		q_snprintf (path, sizeof (path), "bots/%s", find->name);
		mark = Hunk_LowMark ();
		data = (const char *) COM_LoadHunkFile (path, NULL);
		if (!data)
		{
			Hunk_FreeToLowMark (mark);
			continue;
		}

		Bot_TemplateParseFile (path, data, &template);
		Hunk_FreeToLowMark (mark);

		if (g_bot_template_count < BOT_TEMPLATE_MAX_COUNT)
		{
			g_bot_templates[g_bot_template_count++] = template;
			loaded++;
		}
	}

	Con_Printf ("Bot: loaded %d template%s\n", PLURAL (loaded));
}

static int Bot_PickTemplateIndex (void)
{
	if (!g_bot_template_count)
		return -1;
	return rand () % g_bot_template_count;
}

static void Bot_ApplyTemplateToClient (client_t *client, bot_state_t *state, int template_index)
{
	const bot_template_t *template;

	if (!client || !state)
		return;

	if (template_index < 0 || template_index >= g_bot_template_count)
	{
		client->bot_template_index = -1;
		client->bot_skill = bot_skill.value;
		client->bot_preferred_weapon = 0;
		client->bot_least_preferred_weapon = 0;
		client->bot_agility = 1.f;
		state->template_index = -1;
		state->skill = client->bot_skill;
		state->preferred_weapon = 0;
		state->least_preferred_weapon = 0;
		state->agility = 1.f;
		return;
	}

	template = &g_bot_templates[template_index];
	client->bot_template_index = template_index;
	client->bot_skill = template->skill;
	client->bot_preferred_weapon = template->preferred_weapon;
	client->bot_least_preferred_weapon = template->least_preferred_weapon;
	client->bot_agility = template->agility;
	state->template_index = template_index;
	state->skill = template->skill;
	state->preferred_weapon = template->preferred_weapon;
	state->least_preferred_weapon = template->least_preferred_weapon;
	state->agility = template->agility;
}

static int Bot_PickTeam (int requested_team)
{
	int t;
	int best_team;
	int best_count;
	int team_counts[4] = {0, 0, 0, 0};

	if (!deathmatch.value)
	{
		for (t = 0; t < svs.maxclients; ++t)
		{
			client_t *client = &svs.clients[t];
			int team;

			if (!client->active || client->isbot || !client->edict)
				continue;
			team = (int) client->edict->v.team;
			if (team >= 1 && team <= 4)
				return team;
		}
	}

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
		 * Bots should follow the same QC initialization path as normal clients
		 * unless the user explicitly disables it for a problematic mod.
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
	int team;
	int template_index;
	const bot_template_t *template = NULL;
	client_t *client;
	bot_state_t *state;
	char botname[sizeof (client->name)];

	if (!sv.active || sv.state != ss_active)
	{
		Con_Printf ("bot_add requires an active server\n");
		return false;
	}

	for (i = 0; i < svs.maxclients; ++i)
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
	template_index = Bot_PickTemplateIndex ();
	if (template_index >= 0 && template_index < g_bot_template_count)
		template = &g_bot_templates[template_index];
	Bot_MakeUniqueName (template ? template->name : NULL, botname, sizeof (botname));

	if ((unsigned int) clientnum >= (unsigned int) countof (g_bot_states))
	{
		Con_Printf ("bot_add failed: slot %d exceeds bot state capacity (%d)\n", clientnum + 1, (int) countof (g_bot_states));
		return false;
	}

	client = &svs.clients[clientnum];
	memset (client, 0, sizeof (*client));
	client->active = true;
	client->spawned = false;
	client->isbot = true;
	if (!sv.qcvm.progs || !sv.qcvm.edicts || sv.qcvm.edict_size <= 0 || (clientnum + 1) >= sv.qcvm.max_edicts)
	{
		Con_Printf ("bot_add failed: server edict state unavailable\n");
		memset (client, 0, sizeof (*client));
		return false;
	}
	client->edict = (edict_t *) ((byte *) sv.qcvm.edicts + (clientnum + 1) * sv.qcvm.edict_size);
	client->message.data = client->msgbuf;
	client->message.maxsize = sizeof (client->msgbuf);
	client->message.allowoverflow = true;
	q_strlcpy (client->name, botname, sizeof (client->name));
	client->bot_template_index = template_index;
	client->bot_skill = template ? template->skill : bot_skill.value;
	client->bot_preferred_weapon = template ? template->preferred_weapon : 0;
	client->bot_least_preferred_weapon = template ? template->least_preferred_weapon : 0;
	client->bot_agility = template ? template->agility : 1.f;
	Bot_AssignColorsForTeam (client, clientnum, team);

	{
		qcvm_t *saved_vm = qcvm;
		PR_SwitchQCVM (NULL);
		PR_SwitchQCVM (&sv.qcvm);
		PR_ExecuteProgram (pr_global_struct->SetNewParms);
		for (i = 0; i < NUM_SPAWN_PARMS; ++i)
			client->spawn_parms[i] = (&pr_global_struct->parm1)[i];
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
	Bot_ApplyTemplateToClient (client, state, template_index);

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

static void Bot_ReloadTemplates_f (void)
{
	Bot_LoadTemplates ();
	Bot_UpdateCountCvar ();
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
	double now;
	double think_interval;
	double stagger_offset;
	qboolean force_full_think;
	qboolean full_think;

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
		state->template_index = client->bot_template_index;
		state->skill = client->bot_skill;
		state->preferred_weapon = client->bot_preferred_weapon;
		state->least_preferred_weapon = client->bot_least_preferred_weapon;
		state->agility = client->bot_agility;
		BotAI_ResetState (state);
	}
	now = qcvm->time;
	think_interval = CLAMP (0.05, bot_full_think_ms.value * 0.001, 0.25);
	stagger_offset = think_interval * (((state->clientnum % 8) + 8) % 8) / 8.0;
	if (state->next_full_think_time <= 0.0)
		state->next_full_think_time = now + stagger_offset;

	if (!client->spawned)
	{
		if (!Bot_PerformClientSpawn (client, false))
		{
			memset (&client->cmd, 0, sizeof (client->cmd));
			return;
		}
		BotAI_ResetState (state);
		state->next_full_think_time = now + stagger_offset;
	}

	force_full_think = BotAI_ShouldForceFullThink (state, client);
	full_think = force_full_think || (now >= state->next_full_think_time);
	if (full_think)
	{
		state->last_full_think_time = now;
		state->next_full_think_time = now + think_interval;
	}

	BotAI_BuildCommand (state, client, &cmd, v_angle, &attack, &impulse, full_think);
	client->cmd = cmd;
	VectorCopy (v_angle, client->edict->v.v_angle);
	client->edict->v.fixangle = 1.0f;
	client->edict->v.angles[PITCH] = 0.0f;
	client->edict->v.angles[YAW] = v_angle[YAW];
	client->edict->v.angles[ROLL] = 0.0f;
	client->edict->v.button0 = attack ? 1.f : 0.f;
	client->edict->v.button2 = 0.f;
	if (impulse)
		client->edict->v.impulse = impulse;
}

void Bot_OnServerSpawnedMap (void)
{
	int i;
	int limit;

	if (!g_bot_initialized)
		return;
	if (!sv.active || sv.state != ss_active)
		return;
	if (!sv.qcvm.progs || !sv.qcvm.edicts)
		return;
	if (!sv.name[0])
		return;

	if (bot_use_nav2.value)
		BotNav_LoadForMap (sv.name);
	else
		BotNav_Shutdown ();
	g_bot_nav_enabled_cache = (bot_use_nav2.value != 0.f);

	BotAI_OnMapSpawn ();

	limit = Bot_MaxTrackedClients ();
	for (i = 0; i < limit; ++i)
	{
		client_t *client = &svs.clients[i];
		bot_state_t *state;

		if (!client->active || !client->isbot)
			continue;
		if (!client->edict)
			continue;

		state = &g_bot_states[i];

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

qboolean Bot_RespawnClient (client_t *client)
{
	return Bot_PerformClientSpawn (client, false);
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
	Cvar_RegisterVariable (&bot_full_think_ms);
	Cvar_RegisterVariable (&bot_count);
	Bot_LoadTemplates ();

	Cmd_AddCommand ("bot_add", Bot_Add_f);
	Cmd_AddCommand ("bot_addteam", Bot_AddTeam_f);
	Cmd_AddCommand ("bot_kickall", Bot_KickAll_f);
	Cmd_AddCommand ("bot_reloadtemplates", Bot_ReloadTemplates_f);

	memset (g_bot_states, 0, sizeof (g_bot_states));
	g_bot_nav_enabled_cache = (bot_use_nav2.value != 0.f);
	Bot_UpdateCountCvar ();
	g_bot_initialized = true;
}

void Bot_Shutdown (void)
{
	memset (g_bot_states, 0, sizeof (g_bot_states));
	g_bot_template_count = 0;
	BotNav_Shutdown ();
	Bot_UpdateCountCvar ();
	g_bot_nav_enabled_cache = false;
}
