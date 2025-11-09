#include "quakedef.h"

extern edict_t *sv_player;

static cvar_t sv_bot_spawn = {"sv_bot_spawn", "0", CVAR_NONE};
static cvar_t sv_bot_remove = {"sv_bot_remove", "0", CVAR_NONE};

#define MAX_BOT_CHAT_LINES			8
#define MAX_BOT_CHAT_LENGTH			96
#define MAX_BOT_PROFILES			32

typedef enum
{
	BOT_PRIORITY_NONE = 0,
	BOT_PRIORITY_WEAPON,
	BOT_PRIORITY_HEALTH,
	BOT_PRIORITY_ARMOR
} bot_priority_t;

typedef struct bot_profile_s
{
	char		name[32];
	int		topcolor;
	int		bottomcolor;
	int		skin;
	float		skill;
	int		chat_count;
	char		chat_lines[MAX_BOT_CHAT_LINES][MAX_BOT_CHAT_LENGTH];
} bot_profile_t;

typedef struct
{
	qboolean		active;
	vec3_t		wander_dir;
	float		wander_yaw;
	double		next_wander_time;
	double		last_stuck_time;
	vec3_t		last_position;
	const bot_profile_t	*profile;
	float		skill;
	bot_priority_t	goal_priority;
	edict_t		*goal_edict;
	float		goal_best_dist;
	double		goal_last_progress_time;
	double		next_chat_time;
} sv_bot_state_t;

static sv_bot_state_t sv_bot_states[MAX_SCOREBOARD];
static int bot_name_counter = 1;
static bot_profile_t bot_profiles[MAX_BOT_PROFILES];
static int bot_profile_count = 0;
static int bot_profile_cursor = 0;
static qboolean bot_profiles_loaded = false;



static float SV_Bot_Frand (void)
{
	return (float)rand () / (float)RAND_MAX;
}

static void SV_Bot_ResetProfiles (void)
{
	bot_profile_count = 0;
	bot_profile_cursor = 0;
	bot_profiles_loaded = false;
}

static void SV_Bot_AddProfile (const bot_profile_t *profile)
{
	if (!profile || !profile->name[0])
		return;
	if (bot_profile_count >= MAX_BOT_PROFILES)
		return;
	bot_profiles[bot_profile_count++] = *profile;
}

static void SV_Bot_AddFallbackProfiles (void)
{
	bot_profile_t profile;

	memset (&profile, 0, sizeof(profile));
	q_strlcpy (profile.name, "Ranger", sizeof(profile.name));
	profile.topcolor = 4;
	profile.bottomcolor = 12;
	profile.skin = 0;
	profile.skill = 1.2f;
	profile.chat_count = 3;
	q_strlcpy (profile.chat_lines[0], "Ready to frag.", sizeof(profile.chat_lines[0]));
	q_strlcpy (profile.chat_lines[1], "Keep moving!", sizeof(profile.chat_lines[1]));
	q_strlcpy (profile.chat_lines[2], "No escape for you.", sizeof(profile.chat_lines[2]));
	SV_Bot_AddProfile (&profile);

	memset (&profile, 0, sizeof(profile));
	q_strlcpy (profile.name, "Hunter", sizeof(profile.name));
	profile.topcolor = 13;
	profile.bottomcolor = 3;
	profile.skin = 1;
	profile.skill = 1.4f;
	profile.chat_count = 2;
	q_strlcpy (profile.chat_lines[0], "The hunt never stops.", sizeof(profile.chat_lines[0]));
	q_strlcpy (profile.chat_lines[1], "Lock and load.", sizeof(profile.chat_lines[1]));
	SV_Bot_AddProfile (&profile);

	memset (&profile, 0, sizeof(profile));
	q_strlcpy (profile.name, "Visor", sizeof(profile.name));
	profile.topcolor = 8;
	profile.bottomcolor = 2;
	profile.skin = 2;
	profile.skill = 0.9f;
	profile.chat_count = 2;
	q_strlcpy (profile.chat_lines[0], "Systems calibrated.", sizeof(profile.chat_lines[0]));
	q_strlcpy (profile.chat_lines[1], "Target acquired.", sizeof(profile.chat_lines[1]));
	SV_Bot_AddProfile (&profile);
}

static qboolean SV_Bot_ParseProfiles (const char *data)
{
	bot_profile_t profile;
	qboolean parsed_any = false;

	while ((data = COM_Parse (data)) != NULL)
	{
		if (q_strcasecmp (com_token, "bot") != 0)
			continue;

		data = COM_Parse (data);
		if (!data || com_token[0] != '{')
			break;

		memset (&profile, 0, sizeof(profile));
		profile.skill = 1.0f;

		for (;;)
		{
			data = COM_Parse (data);
			if (!data)
				break;
			if (com_token[0] == '}')
			{
				SV_Bot_AddProfile (&profile);
				parsed_any = true;
				break;
			}

			if (!q_strcasecmp (com_token, "name"))
			{
				data = COM_Parse (data);
				if (!data)
					break;
				q_strlcpy (profile.name, com_token, sizeof(profile.name));
				continue;
			}
			if (!q_strcasecmp (com_token, "topcolor"))
			{
				data = COM_Parse (data);
				if (!data)
					break;
				profile.topcolor = atoi (com_token);
				continue;
			}
			if (!q_strcasecmp (com_token, "bottomcolor"))
			{
				data = COM_Parse (data);
				if (!data)
					break;
				profile.bottomcolor = atoi (com_token);
				continue;
			}
			if (!q_strcasecmp (com_token, "skin"))
			{
				data = COM_Parse (data);
				if (!data)
					break;
				profile.skin = atoi (com_token);
				continue;
			}
			if (!q_strcasecmp (com_token, "skill"))
			{
				data = COM_Parse (data);
				if (!data)
					break;
				profile.skill = (float)atof (com_token);
				continue;
			}
			if (!q_strcasecmp (com_token, "chat"))
			{
				data = COM_Parse (data);
				if (!data)
					break;
				if (profile.chat_count < MAX_BOT_CHAT_LINES)
				{
					q_strlcpy (profile.chat_lines[profile.chat_count], com_token, sizeof(profile.chat_lines[0]));
					profile.chat_count++;
				}
				continue;
			}
		}
	}

	return parsed_any;
}

static void SV_Bot_LoadProfiles (void)
{
	char *buffer = NULL;

	if (bot_profiles_loaded)
		return;

	bot_profiles_loaded = true;
	bot_profile_count = 0;

	buffer = (char *)COM_LoadMallocFile ("botprofiles.cfg", NULL);
	if (!buffer)
		buffer = (char *)COM_LoadMallocFile ("config/botprofiles.cfg", NULL);

	if (buffer)
	{
		if (!SV_Bot_ParseProfiles (buffer))
			SV_Bot_AddFallbackProfiles ();
		Z_Free (buffer);
	}
	else
	{
		SV_Bot_AddFallbackProfiles ();
	}
}

static const bot_profile_t *SV_Bot_SelectProfile (void)
{
	SV_Bot_LoadProfiles ();
	if (!bot_profile_count)
		return NULL;
	if (bot_profile_cursor >= bot_profile_count)
		bot_profile_cursor = 0;
	return &bot_profiles[bot_profile_cursor++];
}

static float SV_Bot_Distance (const vec3_t a, const vec3_t b)
{
	vec3_t delta;
	VectorSubtract (a, b, delta);
	return VectorLength (delta);
}

static int SV_Bot_WeaponBitForClassname (const char *classname, int *out_rank)
{
	struct weapon_map_s
	{
		const char *classname;
		int bit;
		int rank;
	};
	static const struct weapon_map_s weapon_map[] =
	{
		{ "weapon_lightning", IT_LIGHTNING, 6 },
		{ "weapon_rocketlauncher", IT_ROCKET_LAUNCHER, 5 },
		{ "weapon_grenadelauncher", IT_GRENADE_LAUNCHER, 4 },
		{ "weapon_supernailgun", IT_SUPER_NAILGUN, 3 },
		{ "weapon_nailgun", IT_NAILGUN, 2 },
		{ "weapon_supershotgun", IT_SUPER_SHOTGUN, 1 },
		{ "weapon_shotgun", IT_SHOTGUN, 0 },
	};
	size_t i;

	for (i = 0; i < sizeof(weapon_map)/sizeof(weapon_map[0]); i++)
	{
		if (!q_strcasecmp (classname, weapon_map[i].classname))
		{
			if (out_rank)
				*out_rank = weapon_map[i].rank;
			return weapon_map[i].bit;
		}
	}

	return 0;
}

static edict_t *SV_Bot_FindGoal (edict_t *self, bot_priority_t priority)
{
	edict_t *best = NULL;
	float best_dist = 0.0f;
	int best_rank = -1;
	int i;

	for (i = svs.maxclients + 1; i < qcvm->num_edicts; i++)
	{
		edict_t *ent = EDICT_NUM (i);
		const char *classname;

		if (ent->free || !ent->v.classname || ent->v.solid == SOLID_NOT)
			continue;

		classname = PR_GetString (ent->v.classname);
		if (!classname || !classname[0])
			continue;

		if (priority == BOT_PRIORITY_WEAPON)
		{
			int rank = 0;
			int bit = SV_Bot_WeaponBitForClassname (classname, &rank);

			if (!bit || ((int)self->v.items & bit))
				continue;
			if (rank <= 0)
				continue;

			if (rank > best_rank || (rank == best_rank && (!best || SV_Bot_Distance (ent->v.origin, self->v.origin) < best_dist)))
			{
				best = ent;
				best_rank = rank;
				best_dist = SV_Bot_Distance (ent->v.origin, self->v.origin);
			}
			continue;
		}

		if (priority == BOT_PRIORITY_HEALTH)
		{
			if (q_strcasecmp (classname, "item_health") != 0)
				continue;
		}
		else if (priority == BOT_PRIORITY_ARMOR)
		{
			if (q_strcasecmp (classname, "item_armor1") && q_strcasecmp (classname, "item_armor2") && q_strcasecmp (classname, "item_armorInv"))
				continue;
		}
		else
		{
			continue;
		}

		if (!best || SV_Bot_Distance (ent->v.origin, self->v.origin) < best_dist)
		{
			best = ent;
			best_dist = SV_Bot_Distance (ent->v.origin, self->v.origin);
		}
	}

	return best;
}

static bot_priority_t SV_Bot_EvaluateNeeds (const edict_t *self)
{
	qboolean needs_weapon = false;
	qboolean needs_health = false;
	qboolean needs_armor = false;
	int items = (int)self->v.items;
	int weapon = (int)self->v.weapon;

	if (!(items & (IT_SUPER_SHOTGUN | IT_NAILGUN | IT_SUPER_NAILGUN | IT_GRENADE_LAUNCHER | IT_ROCKET_LAUNCHER | IT_LIGHTNING)))
		needs_weapon = true;
	if (weapon == IT_AXE || weapon == IT_SHOTGUN)
		needs_weapon = true;

	if (self->v.health < 80)
		needs_health = true;

	if (self->v.armorvalue < 100 || self->v.armortype < 0.3f)
		needs_armor = true;

	if (needs_weapon)
		return BOT_PRIORITY_WEAPON;
	if (needs_health)
		return BOT_PRIORITY_HEALTH;
	if (needs_armor)
		return BOT_PRIORITY_ARMOR;
	return BOT_PRIORITY_NONE;
}

static qboolean SV_Bot_GoalStillValid (edict_t *goal, bot_priority_t priority, const edict_t *self)
{
	const char *classname;

	if (!goal || goal->free || goal->v.solid == SOLID_NOT)
		return false;
	if (!goal->v.classname)
		return false;

	classname = PR_GetString (goal->v.classname);
	if (!classname[0])
		return false;

	switch (priority)
	{
	case BOT_PRIORITY_WEAPON:
	return true;
	case BOT_PRIORITY_HEALTH:
	return !q_strcasecmp (classname, "item_health");
	case BOT_PRIORITY_ARMOR:
	return !q_strcasecmp (classname, "item_armor1") || !q_strcasecmp (classname, "item_armor2") || !q_strcasecmp (classname, "item_armorInv");
	default:
	break;
	}

	return false;
}

static void SV_Bot_UpdateGoal (edict_t *self, sv_bot_state_t *state, bot_priority_t priority, double now)
{
	edict_t *goal;

	if (!state)
	return;

	if (priority == BOT_PRIORITY_NONE)
	{
	state->goal_edict = NULL;
	state->goal_priority = BOT_PRIORITY_NONE;
	state->goal_best_dist = 0.0f;
	return;
	}

	if (state->goal_priority != priority || !SV_Bot_GoalStillValid (state->goal_edict, priority, self))
	{
	goal = SV_Bot_FindGoal (self, priority);
	state->goal_edict = goal;
	state->goal_priority = priority;
	if (goal)
	{
	state->goal_best_dist = SV_Bot_Distance (goal->v.origin, self->v.origin);
	state->goal_last_progress_time = now;
	}
	else
	{
	state->goal_best_dist = 0.0f;
	}
	}
}

static void SV_Bot_CheckGoalProgress (edict_t *self, sv_bot_state_t *state, double now)
{
	float dist;

	if (!state || !state->goal_edict)
	return;

	if (!SV_Bot_GoalStillValid (state->goal_edict, state->goal_priority, self))
	{
	state->goal_edict = NULL;
	state->goal_priority = BOT_PRIORITY_NONE;
	return;
	}

	dist = SV_Bot_Distance (state->goal_edict->v.origin, self->v.origin);
	if (dist < 48.0f)
	{
	state->goal_edict = NULL;
	state->goal_priority = BOT_PRIORITY_NONE;
	state->goal_best_dist = 0.0f;
	return;
	}

	if (dist < state->goal_best_dist - 16.0f)
	{
	state->goal_best_dist = dist;
	state->goal_last_progress_time = now;
	}
	else if (now - state->goal_last_progress_time > 3.0)
	{
	state->goal_edict = NULL;
	state->goal_priority = BOT_PRIORITY_NONE;
	state->next_wander_time = 0.0;
	}
}

static qboolean SV_Bot_CanSeeTarget (edict_t *self, edict_t *target)
{
	vec3_t start, end;
	trace_t trace;

	if (!self || !target)
		return false;

	VectorCopy (self->v.origin, start);
	start[2] += self->v.view_ofs[2];
	VectorCopy (target->v.origin, end);
	end[2] += target->v.view_ofs[2];

	trace = SV_Move (start, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, self);
	return trace.fraction >= 0.99f || trace.ent == target;
}

static void SV_Bot_MaybeChat (client_t *client, sv_bot_state_t *state)
{
	double now;
	int line;

	if (!client || !state || !state->profile || state->profile->chat_count <= 0)
		return;
	if (!client->edict || client->edict->v.health <= 0)
		return;

	now = qcvm->time;
	if (now < state->next_chat_time)
		return;
	if (SV_Bot_Frand () > 0.1f)
	{
		state->next_chat_time = now + 5.0 + SV_Bot_Frand () * 10.0;
		return;
	}

	line = rand () % state->profile->chat_count;
	SV_BroadcastPrintf ("%s: %s\n", client->name, state->profile->chat_lines[line]);
	state->next_chat_time = now + 20.0 + SV_Bot_Frand () * 25.0;
}

static sv_bot_state_t *SV_BotStateForClient (const client_t *client)
{
	ptrdiff_t index = client - svs.clients;
	if (index < 0 || index >= MAX_SCOREBOARD)
		return NULL;
	return &sv_bot_states[index];
}

void SV_Bot_Reset (void)
{
        memset (sv_bot_states, 0, sizeof(sv_bot_states));
        bot_name_counter = 1;
        SV_Bot_ResetProfiles ();
}

static void SV_Bot_PickWanderDirection (sv_bot_state_t *state)
{
	float yaw = (float)(rand () % 360);
	float radians = yaw * (float)M_PI / 180.0f;
	vec3_t dir = { (float)cos (radians), (float)sin (radians), 0.0f };

	VectorNormalize (dir);
	VectorCopy (dir, state->wander_dir);
	state->wander_yaw = yaw;
	state->next_wander_time = qcvm->time + 2.0 + ((float)(rand () & 255) / 255.0f) * 4.0f;
}

static qboolean SV_Bot_NameExists (const char *name)
{
	int i;

	for (i = 0; i < svs.maxclients; i++)
	{
		if (!svs.clients[i].active)
			continue;
		if (q_strcasecmp (svs.clients[i].name, name) == 0)
			return true;
	}

	return false;
}

static void SV_Bot_AssignName (client_t *client, const char *preferred)
{
        char name[sizeof(client->name)];
        int attempts = 0;

        if (preferred && *preferred)
        {
                q_strlcpy (name, preferred, sizeof(name));
                if (!SV_Bot_NameExists (name))
                {
                        q_strlcpy (client->name, name, sizeof(client->name));
                        return;
                }

                for (attempts = 1; attempts < 32; attempts++)
                {
                        q_snprintf (name, sizeof(name), "%s_%d", preferred, attempts + 1);
                        if (!SV_Bot_NameExists (name))
                        {
                                q_strlcpy (client->name, name, sizeof(client->name));
                                return;
                        }
                }
        }

        attempts = 0;
        do
        {
                q_snprintf (name, sizeof(name), "Bot%d", bot_name_counter++);
                if (bot_name_counter > 9999)
                        bot_name_counter = 1;
        } while (SV_Bot_NameExists (name) && attempts++ < 10000);

        q_strlcpy (client->name, name, sizeof(client->name));
}

static client_t *SV_Bot_FindTarget (client_t *bot, qboolean prefer_players)
{
	client_t *best = NULL;
	float best_dist = 0.0f;
	int i;
	edict_t *self = bot->edict;

	for (i = 0; i < svs.maxclients; i++)
	{
		client_t *candidate = &svs.clients[i];
		vec3_t delta;
		float dist;

		if (!candidate->active || !candidate->spawned)
			continue;
		if (candidate == bot)
			continue;
		if (candidate->edict->free)
			continue;
		if (candidate->edict->v.health <= 0)
			continue;
		if (prefer_players && candidate->isbot)
			continue;

		VectorSubtract (candidate->edict->v.origin, self->v.origin, delta);
		dist = VectorLength (delta);

		if (!best || dist < best_dist)
		{
			best = candidate;
			best_dist = dist;
		}
	}

	return best;
}

static void SV_Bot_UpdateMovement (client_t *client, sv_bot_state_t *state)
{
	edict_t *self = client->edict;
	usercmd_t *cmd = &client->cmd;
	client_t *enemy;
	double now = qcvm->time;
	float skill = state ? state->skill : 1.0f;
	bot_priority_t priority = BOT_PRIORITY_NONE;
	edict_t *goal = NULL;
	vec3_t delta;
	vec3_t base_angles;
	qboolean have_base_angles = false;

	if (!self || self->free)
		return;

	enemy = SV_Bot_FindTarget (client, true);
	if (!enemy)
		enemy = SV_Bot_FindTarget (client, false);

	memset (cmd, 0, sizeof(*cmd));
	self->v.button0 = 0;
	self->v.button2 = 0;

	if (state)
	{
		if (now - state->last_stuck_time > 1.0)
		{
			VectorSubtract (self->v.origin, state->last_position, delta);
			if (VectorLength (delta) < 16.0f)
			{
				state->next_wander_time = 0.0;
				if (state->goal_edict)
					state->goal_last_progress_time = now - 10.0;
			}
			VectorCopy (self->v.origin, state->last_position);
			state->last_stuck_time = now;
		}
	}

	if (state)
	{
		priority = SV_Bot_EvaluateNeeds (self);
		SV_Bot_UpdateGoal (self, state, priority, now);
		SV_Bot_CheckGoalProgress (self, state, now);
		goal = state->goal_edict;

		if (!goal && state->next_wander_time <= now)
			SV_Bot_PickWanderDirection (state);
	}

	if (goal)
	{
		vec3_t dir;
		float dist;

		VectorSubtract (goal->v.origin, self->v.origin, dir);
		dist = VectorLength (dir);
		if (dist > 0)
			VectorScale (dir, 1.0f / dist, dir);
		else
			VectorClear (dir);

		VectorAngles (dir, base_angles);
		base_angles[PITCH] = 0.0f;
		have_base_angles = true;

		cmd->forwardmove = 160.0f + 60.0f * (skill - 1.0f);
		if (dist > 120.0f)
			cmd->sidemove = (SV_Bot_Frand () > 0.5f) ? 120.0f : -120.0f;
	}
	else if (state)
	{
		VectorAngles (state->wander_dir, base_angles);
		have_base_angles = true;
		cmd->forwardmove = 150.0f + 50.0f * skill;
		if (SV_Bot_Frand () > 0.5f)
			cmd->sidemove = (SV_Bot_Frand () > 0.5f) ? 80.0f : -80.0f;
	}

	if (enemy)
	{
		vec3_t dir;
		vec3_t angles;
		float dist;
		qboolean can_shoot;

		VectorSubtract (enemy->edict->v.origin, self->v.origin, dir);
		dist = VectorLength (dir);
		if (dist > 0)
			VectorScale (dir, 1.0f / dist, dir);
		else
			VectorClear (dir);

		VectorAngles (dir, angles);
		angles[PITCH] = CLAMP (-60.0f, angles[PITCH], 60.0f);

		{
			float jitter = CLAMP (0.0f, 1.5f - skill, 1.5f) * 6.0f;
			angles[YAW] += (SV_Bot_Frand () * 2.0f - 1.0f) * jitter;
			angles[PITCH] += (SV_Bot_Frand () * 2.0f - 1.0f) * (jitter * 0.5f);
		}

		VectorCopy (angles, self->v.v_angle);
		can_shoot = SV_Bot_CanSeeTarget (self, enemy->edict);

		if (priority != BOT_PRIORITY_NONE)
		{
			if (dist < 200.0f)
			{
				cmd->forwardmove = -220.0f;
				cmd->sidemove = (SV_Bot_Frand () > 0.5f) ? 200.0f : -200.0f;
				if (can_shoot && dist < 180.0f)
					self->v.button0 = 1;
			}
		}
		else
		{
			float attack_range = 420.0f + skill * 220.0f;

			if (!goal)
			{
				if (dist > 160.0f)
					cmd->forwardmove = 220.0f;
				else if (dist < 90.0f)
					cmd->forwardmove = -150.0f;
				if (dist > 80.0f)
					cmd->sidemove = (SV_Bot_Frand () > 0.5f) ? 140.0f : -140.0f;
			}

			if (can_shoot && dist < attack_range)
				self->v.button0 = 1;
		}
	}
	else if (have_base_angles)
	{
		VectorCopy (base_angles, self->v.v_angle);
	}

	if (self->v.waterlevel >= 2)
		cmd->upmove = 200.0f;

	VectorCopy (self->v.v_angle, cmd->viewangles);
}


static qboolean SV_Bot_SpawnOne (void)
{
        client_t *client;
        sv_bot_state_t *state;
        const bot_profile_t *profile = NULL;
        client_t *saved_client;
        edict_t *saved_player;
        cmd_source_t saved_source;
        qcvm_t *oldqcvm = NULL;
        qboolean pushed_vm = false;
	qboolean success = false;
	int slot;

	if (!sv.active || sv.state != ss_active)
	{
		Con_Printf ("Cannot spawn bot: server is not active.\n");
		goto cleanup;
	}

	for (slot = 0; slot < svs.maxclients; slot++)
	{
		if (!svs.clients[slot].active)
			break;
	}

	if (slot == svs.maxclients)
	{
		Con_Printf ("No free client slots for bot.\n");
		goto cleanup;
	}

	PR_PushQCVM (&sv.qcvm, &oldqcvm);
	pushed_vm = true;

	client = &svs.clients[slot];
        client->netconnection = NULL;

        SV_ConnectClient (slot);
        client->isbot = true;
        client->dropasap = false;

        profile = SV_Bot_SelectProfile ();

        if (profile)
        {
                int topcolor = CLAMP (0, profile->topcolor, 15);
                int bottomcolor = CLAMP (0, profile->bottomcolor, 15);
                client->colors = ((topcolor & 15) << 4) | (bottomcolor & 15);
                if (client->edict)
                        client->edict->v.skin = profile->skin;
        }
        else
        {
                client->colors = ((rand () & 15) << 4) | (rand () & 15);
        }

        SV_Bot_AssignName (client, profile ? profile->name : NULL);

	saved_client = host_client;
	saved_player = sv_player;
	saved_source = cmd_source;

	host_client = client;
	sv_player = client->edict;
	cmd_source = src_client;

	Cmd_ExecuteString ("prespawn", src_client);
	Cmd_ExecuteString ("spawn", src_client);
	Cmd_ExecuteString ("begin", src_client);

	host_client = saved_client;
	sv_player = saved_player;
	cmd_source = saved_source;

	client->spawned = true;
	client->sendsignon = PRESPAWN_DONE;
	client->last_message = realtime;
	SZ_Clear (&client->message);

	state = SV_BotStateForClient (client);
        if (state)
        {
                memset (state, 0, sizeof(*state));
                state->active = true;
                state->profile = profile;
                state->skill = profile ? CLAMP (0.5f, profile->skill, 2.0f) : 1.0f;
                state->next_chat_time = qcvm->time + 10.0 + SV_Bot_Frand () * 10.0;
                VectorCopy (client->edict->v.origin, state->last_position);
                state->last_stuck_time = qcvm->time;
                state->next_wander_time = 0.0;
        }

	MSG_WriteByte (&sv.reliable_datagram, svc_updatename);
	MSG_WriteByte (&sv.reliable_datagram, slot);
	MSG_WriteString (&sv.reliable_datagram, client->name);
	MSG_WriteByte (&sv.reliable_datagram, svc_updatecolors);
	MSG_WriteByte (&sv.reliable_datagram, slot);
	MSG_WriteByte (&sv.reliable_datagram, client->colors);

	SV_BroadcastPrintf ("%s has spawned.\n", client->name);

	success = true;

cleanup:
	if (pushed_vm)
		PR_PopQCVM (oldqcvm);

	return success;
}

static qboolean SV_Bot_RemoveOne (void)
{
	int slot;
	client_t *client;
	client_t *saved_client;

	for (slot = svs.maxclients - 1; slot >= 0; slot--)
	{
		if (svs.clients[slot].active && svs.clients[slot].isbot)
			break;
	}

	if (slot < 0)
	{
		Con_Printf ("No bots to remove.\n");
		return false;
	}

	client = &svs.clients[slot];
	saved_client = host_client;
	host_client = client;

	SV_Bot_ClientDisconnected (client);
	SV_DropClient (false);

	host_client = saved_client;
	return true;
}

static void SV_BotSpawn_cvar (cvar_t *var)
{
	int count = (int)var->value;
	int spawned = 0;

	if (count <= 0)
		return;

	while (spawned < count)
	{
		if (!SV_Bot_SpawnOne ())
			break;
		spawned++;
	}

	if (spawned)
		Con_Printf ("Spawned %d bot(s).\n", spawned);

	Cvar_SetValueQuick (var, 0.0f);
}

static void SV_BotRemove_cvar (cvar_t *var)
{
	int count = (int)var->value;
	int removed = 0;

	if (count <= 0)
		return;

	while (removed < count)
	{
		if (!SV_Bot_RemoveOne ())
			break;
		removed++;
	}

	if (removed)
		Con_Printf ("Removed %d bot(s).\n", removed);

	Cvar_SetValueQuick (var, 0.0f);
}

void SV_Bot_Init (void)
{
	Cvar_RegisterVariable (&sv_bot_spawn);
	Cvar_RegisterVariable (&sv_bot_remove);
	Cvar_SetCallback (&sv_bot_spawn, SV_BotSpawn_cvar);
	Cvar_SetCallback (&sv_bot_remove, SV_BotRemove_cvar);
	SV_Bot_Reset ();
}

void SV_Bot_RunFrame (client_t *client)
{
	sv_bot_state_t *state;

	if (!client || !client->isbot)
		return;

        state = SV_BotStateForClient (client);
        if (!client->spawned)
                return;

        SV_Bot_UpdateMovement (client, state);
        SV_Bot_MaybeChat (client, state);
}

void SV_Bot_ClientDisconnected (client_t *client)
{
	sv_bot_state_t *state;

	if (!client)
		return;

	state = SV_BotStateForClient (client);
	if (state)
		memset (state, 0, sizeof(*state));
	client->isbot = false;
}
