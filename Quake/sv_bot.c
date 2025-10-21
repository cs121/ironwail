#include "quakedef.h"

extern cvar_t sv_maxspeed;

#define BOT_ATTACK_RANGE        800.0f
#define BOT_FIRE_RANGE          600.0f
#define BOT_STUCK_INTERVAL      1.0f
#define BOT_WANDER_DISTANCE     256.0f
#define BOT_RESPAWN_WAIT        0.2f

typedef struct bot_skill_config_s
{
	float			forward_speed;
	float			strafe_speed;
	float			reaction_time;
	float			attack_delay;
	float			aim_inaccuracy;
	float			strafe_interval_min;
	float			strafe_interval_max;
	float			chat_interval;
	float			engage_taunt_chance;
	float			kill_taunt_chance;
	float			death_taunt_chance;
} bot_skill_config_t;

static const bot_skill_config_t bot_skill_configs[BOT_SKILL_COUNT] = {
	[ BOT_SKILL_EASY ] = {
		.forward_speed = 160.0f,
		.strafe_speed = 90.0f,
		.reaction_time = 1.4f,
		.attack_delay = 0.45f,
		.aim_inaccuracy = 12.0f,
		.strafe_interval_min = 1.1f,
		.strafe_interval_max = 1.6f,
		.chat_interval = 9.0f,
		.engage_taunt_chance = 0.25f,
		.kill_taunt_chance = 0.5f,
		.death_taunt_chance = 0.6f,
	},
	[ BOT_SKILL_NORMAL ] = {
		.forward_speed = 200.0f,
		.strafe_speed = 140.0f,
		.reaction_time = 0.9f,
		.attack_delay = 0.3f,
		.aim_inaccuracy = 6.0f,
		.strafe_interval_min = 0.75f,
		.strafe_interval_max = 1.2f,
		.chat_interval = 7.0f,
		.engage_taunt_chance = 0.35f,
		.kill_taunt_chance = 0.6f,
		.death_taunt_chance = 0.7f,
	},
	[ BOT_SKILL_HARD ] = {
		.forward_speed = 240.0f,
		.strafe_speed = 200.0f,
		.reaction_time = 0.45f,
		.attack_delay = 0.18f,
		.aim_inaccuracy = 2.5f,
		.strafe_interval_min = 0.5f,
		.strafe_interval_max = 0.9f,
		.chat_interval = 5.0f,
		.engage_taunt_chance = 0.5f,
		.kill_taunt_chance = 0.75f,
		.death_taunt_chance = 0.8f,
	}
};

static const char *bot_skill_names[BOT_SKILL_COUNT] =
{
	
	"easy",
	"normal",
	"hard"
};

static const char *bot_chat_engage[] =
{
	
	"I've got you now, %s.",
	"Let's see what you've got, %s.",
	"%s, I'm coming for you."
};

static const char *bot_chat_kill[] =
{
	
	"%s couldn't handle it.",
	"Another frag for me, %s.",
	"Sleep tight, %s."
};

static const char *bot_chat_death[] =
{
	
	"You got lucky, %s.",
	"I'll be back for you, %s!",
	"Ouch... nice shot, %s."
};

static cvar_t bot_skill_cvar = { "bot_skill", "2" };

static const char *bot_default_names[] = {
        "Atlas", "Blaze", "Cipher", "Dagger", "Echo", "Frost", "Gunner", "Havoc",
        "Iris", "Jinx", "Knight", "Lancer", "Mara", "Nova", "Onyx", "Phantom",
        "Quill", "Rogue", "Shade", "Talon", "Vex", "Warden", "Yarrow", "Zeal"
};

static size_t bot_name_cursor;
static int bot_name_serial = 1;

typedef enum
{
	BOT_CHAT_ENGAGE,
	BOT_CHAT_KILL,
	BOT_CHAT_DEATH
} bot_chat_event_t;

static float SV_BotRandomFloat (float min, float max);

static bot_skill_t SV_BotClampSkill (bot_skill_t skill)
{
	if ((int) skill < 0 || (int) skill >= BOT_SKILL_COUNT)
		return BOT_SKILL_NORMAL;
	return skill;
}

static const bot_skill_config_t *SV_BotSkillConfig (const bot_state_t *bot)
{
	return &bot_skill_configs[SV_BotClampSkill (bot ? bot->skill : BOT_SKILL_NORMAL)];
}

static bot_skill_t SV_BotDefaultSkill (void)
{
	int value = (int) Q_rint (bot_skill_cvar.value);

	value = CLAMP (1, value, BOT_SKILL_COUNT);
	return (bot_skill_t) (value - 1);
}

static qboolean SV_BotTryParseSkillToken (const char *token, bot_skill_t *out)
{
	int i;

	if (!token || !*token || !out)
		return false;

	for (i = 0; i < BOT_SKILL_COUNT; i++)
	{
		if (!q_strcasecmp (token, bot_skill_names[i]))
		{
			*out = (bot_skill_t) i;
			return true;
		}
	}

	if (token[0] >= '0' && token[0] <= '9')
	{
		int value = Q_atoi (token);

		if (value >= 1 && value <= BOT_SKILL_COUNT)
			value -= 1;
		value = CLAMP (0, value, BOT_SKILL_COUNT - 1);
		*out = (bot_skill_t) value;
		return true;
	}

	return false;
}

static void SV_BotBroadcastChat (client_t *client, const char *text)
{
	char message[128];
	size_t len;
	client_t *save;
	int i;

	if (!client || !text || !*text)
		return;

	q_snprintf (message, sizeof (message), "\001%s: %s", client->name, text);
	len = strlen (message);
	if (len >= sizeof (message) - 1)
	{
		message[sizeof (message) - 2] = '\n';
		message[sizeof (message) - 1] = '\0';
	}
	else
	{
		message[len] = '\n';
		message[len + 1] = '\0';
	}

	save = host_client;

	for (i = 0; i < svs.maxclients; i++)
	{
		client_t *other = &svs.clients[i];

		if (!other->active || !other->spawned)
			continue;

		host_client = other;
		SV_ClientPrintf ("%s", message);
	}

	host_client = save;

	if (cls.state == ca_dedicated)
		Sys_Printf ("%s", message[0] == '\001' ? message + 1 : message);
}

static void SV_BotChatf (client_t *client, const char *fmt, ...)
{
	char text[96];
	va_list argptr;

	if (!client || !fmt)
		return;

	va_start (argptr, fmt);
	q_vsnprintf (text, sizeof (text), fmt, argptr);
	va_end (argptr);

	text[sizeof (text) - 1] = '\0';

	SV_BotBroadcastChat (client, text);
}

static void SV_BotMaybeTaunt (client_t *client, bot_chat_event_t event, edict_t *target)
{
	bot_state_t *bot;
	const bot_skill_config_t *config;
	const char *format = NULL;
	const char *target_name = NULL;
	float chance = 0.0f;

	if (!client)
		return;

	bot = &client->bot;
	config = SV_BotSkillConfig (bot);

	if (!config)
		return;

	if (qcvm->time < bot->next_chat_time)
		return;

	switch (event)
	{
	case BOT_CHAT_ENGAGE:
		chance = config->engage_taunt_chance;
		if (SV_BotRandomFloat (0.0f, 1.0f) > chance)
			return;
		format = bot_chat_engage[rand () % countof (bot_chat_engage)];
		break;
	case BOT_CHAT_KILL:
		chance = config->kill_taunt_chance;
		if (SV_BotRandomFloat (0.0f, 1.0f) > chance)
			return;
		format = bot_chat_kill[rand () % countof (bot_chat_kill)];
		break;
	case BOT_CHAT_DEATH:
		chance = config->death_taunt_chance;
		if (SV_BotRandomFloat (0.0f, 1.0f) > chance)
			return;
		format = bot_chat_death[rand () % countof (bot_chat_death)];
		break;
	}

	if (!format)
		return;

	if (target && !target->free)
		target_name = PR_GetString (target->v.netname);

	if (!target_name || !*target_name)
		target_name = "you";

	SV_BotChatf (client, format, target_name);
	bot->next_chat_time = qcvm->time + config->chat_interval;
}

static float SV_BotRandomFloat (float min, float max)
{
	return min + (max - min) * ((float) rand () / (float) RAND_MAX);
}

qboolean SV_ClientIsBot (const client_t *client)
{
        return client && client->isbot;
}

static qboolean SV_BotNameInUse (const char *name)
{
        size_t i;
        if (!name || !*name)
                return false;

        for (i = 0; i < (size_t)svs.maxclients; i++)
        {
                const client_t *client = &svs.clients[i];
                if (!client->active)
                        continue;
                if (!Q_strcmp (client->name, name))
                        return true;
        }

        return false;
}

static void SV_BotChooseName (char *buffer, size_t size, const char *requested)
{
        size_t attempt;

        if (requested && requested[0])
        {
                q_strlcpy (buffer, requested, size);
                if (!SV_BotNameInUse (buffer))
                        return;

                for (attempt = 1; attempt < 100; attempt++)
                {
                        q_snprintf (buffer, size, "%s%zu", requested, attempt);
                        if (!SV_BotNameInUse (buffer))
                                return;
                }
        }

        for (attempt = 0; attempt < countof (bot_default_names); attempt++)
        {
                const char *base = bot_default_names[bot_name_cursor++ % countof (bot_default_names)];
                q_snprintf (buffer, size, "%s%d", base, bot_name_serial);
                bot_name_serial++;
                if (!SV_BotNameInUse (buffer))
                        return;
        }

        for (attempt = 0; attempt < 1024; attempt++)
        {
                q_snprintf (buffer, size, "bot%d", bot_name_serial++);
                if (!SV_BotNameInUse (buffer))
                        return;
        }

        q_strlcpy (buffer, "bot", size);
}

static client_t *SV_BotFindFreeClient (void)
{
        int i;

        for (i = 0; i < svs.maxclients; i++)
        {
                if (!svs.clients[i].active)
                        return &svs.clients[i];
        }

        return NULL;
}

static void SV_BotBroadcastInfo (client_t *client)
{
        int slot = (int)(client - svs.clients);

        MSG_WriteByte (&sv.reliable_datagram, svc_updatename);
        MSG_WriteByte (&sv.reliable_datagram, slot);
        MSG_WriteString (&sv.reliable_datagram, client->name);

        MSG_WriteByte (&sv.reliable_datagram, svc_updatefrags);
        MSG_WriteByte (&sv.reliable_datagram, slot);
        MSG_WriteShort (&sv.reliable_datagram, (int) client->edict->v.frags);

        MSG_WriteByte (&sv.reliable_datagram, svc_updatecolors);
        MSG_WriteByte (&sv.reliable_datagram, slot);
        MSG_WriteByte (&sv.reliable_datagram, client->colors);
}

static void SV_BotFinalize (client_t *client)
{
        qcvm_t *oldqcvm;
        client_t *old_client = host_client;
        edict_t *old_player = sv_player;
        size_t i;

        PR_PushQCVM (&sv.qcvm, &oldqcvm);

        host_client = client;
        sv_player = client->edict;

        memset (&client->edict->v, 0, qcvm->progs->entityfields * 4);
        client->edict->v.colormap = NUM_FOR_EDICT (client->edict);
        client->edict->v.team = (client->colors & 15) + 1;
        client->edict->v.netname = PR_SetEngineString (client->name);

        for (i = 0; i < NUM_SPAWN_PARMS; i++)
                (&pr_global_struct->parm1)[i] = client->spawn_parms[i];

        pr_global_struct->time = qcvm->time;
        pr_global_struct->self = EDICT_TO_PROG (client->edict);
        PR_ExecuteProgram (pr_global_struct->ClientConnect);
        PR_ExecuteProgram (pr_global_struct->PutClientInServer);

        client->spawned = true;
        client->sendsignon = PRESPAWN_DONE;
        client->last_message = realtime;
        client->old_frags = (int) client->edict->v.frags;

        SZ_Clear (&client->message);
        VectorCopy (client->edict->v.origin, client->bot.last_origin);
        VectorClear (client->bot.wander_dir);
        client->bot.next_repath = qcvm->time;
        client->bot.strafe_change_time = 0.0f;
        client->bot.strafe_dir = 0.0f;
        client->bot.stuck_check = qcvm->time + BOT_STUCK_INTERVAL;
        client->bot.target_ent = 0;
        client->bot.has_enemy = false;
        client->bot.next_attack_time = 0.0f;
        client->bot.last_enemy_ent = 0;
        client->bot.last_enemy_health = 0.0f;
        client->bot.next_chat_time = 0.0f;
        client->bot.death_taunted = false;

        host_client = old_client;
        sv_player = old_player;
        PR_PopQCVM (oldqcvm);
}

client_t *SV_BotSpawn (const char *requested_name, bot_skill_t skill)
{
        client_t *client;
        char name[sizeof (((client_t *)0)->name)];
        int slot;

        if (!sv.active)
        {
                Con_Printf ("Cannot add bot: no active server.\n");
                return NULL;
        }

        client = SV_BotFindFreeClient ();
        if (!client)
        {
                Con_Printf ("Cannot add bot: server is full.\n");
                return NULL;
        }

        slot = (int)(client - svs.clients);
        client->netconnection = NULL;
        SV_ConnectClient (slot);

        client->isbot = true;
        client->bot.skill = SV_BotClampSkill (skill);

        SV_BotChooseName (name, sizeof (name), requested_name);
        q_strlcpy (client->name, name, sizeof (client->name));
        client->edict->v.netname = PR_SetEngineString (client->name);

        client->colors = ((rand () & 15) << 4) | (rand () & 15);

        SV_BotFinalize (client);
        SV_BotBroadcastInfo (client);

        Con_Printf ("Spawned %s bot %s\n", bot_skill_names[client->bot.skill], client->name);
        return client;
}

static client_t *SV_BotFindByName (const char *name)
{
        size_t i;

        if (!name || !*name)
                return NULL;

        for (i = 0; i < (size_t)svs.maxclients; i++)
        {
                client_t *client = &svs.clients[i];
                if (!client->active || !client->isbot)
                        continue;
                if (!Q_strcmp (client->name, name))
                        return client;
        }

        return NULL;
}

static void SV_BotRemoveClient (client_t *client)
{
        client_t *previous = host_client;

        if (!client)
        {
                Con_Printf ("No bot to remove.\n");
                return;
        }

        host_client = client;
        SV_DropClient (false);
        host_client = previous;
}

static void SV_BotAdd_f (void)
{
        const char *name = NULL;
        const char *args = NULL;
        bot_skill_t skill = SV_BotDefaultSkill ();

        if (Cmd_Argc () > 1)
        {
                const char *cursor;
                bot_skill_t parsed;

                args = Cmd_Args ();
                cursor = args;
                cursor = COM_Parse (cursor);
                if (cursor && SV_BotTryParseSkillToken (com_token, &parsed))
                {
                        skill = parsed;
                        while (*cursor && *cursor <= ' ')
                                cursor++;
                        if (cursor && *cursor)
                                name = cursor;
                }
                else
                {
                        name = args;
                }
        }

        SV_BotSpawn (name, skill);
}

static void SV_BotRemove_f (void)
{
        client_t *client = NULL;
        int i;

        if (Cmd_Argc () > 1)
                client = SV_BotFindByName (Cmd_Args ());

        if (!client)
        {
                for (i = svs.maxclients - 1; i >= 0; i--)
                {
                        if (svs.clients[i].active && svs.clients[i].isbot)
                        {
                                client = &svs.clients[i];
                                break;
                        }
                }
        }

        if (!client)
        {
                Con_Printf ("No bots are currently active.\n");
                return;
        }

        SV_BotRemoveClient (client);
}

void SV_BotInit (void)
{
        Cvar_RegisterVariable (&bot_skill_cvar);
        Cmd_AddCommand ("bot_add", SV_BotAdd_f);
        Cmd_AddCommand ("bot_remove", SV_BotRemove_f);
}

static qboolean SV_BotIsItem (edict_t *ent)
{
        const char *classname;

        if (!ent || ent->free)
                return false;
        if ((int) ent->v.solid != SOLID_TRIGGER)
                return false;

        classname = PR_GetString (ent->v.classname);
        if (!classname || !classname[0])
                return false;

        if (!q_strncasecmp (classname, "item_", 5))
                return true;
        if (!q_strncasecmp (classname, "weapon_", 7))
                return true;
        if (!q_strncasecmp (classname, "ammo_", 5))
                return true;

        return false;
}

static edict_t *SV_BotFindPickup (client_t *client)
{
        edict_t *best = NULL;
        float best_dist = 0.0f;
        int i;

        for (i = svs.maxclients + 1; i < qcvm->num_edicts; i++)
        {
            edict_t *ent = EDICT_NUM (i);
            float dist;

            if (!SV_BotIsItem (ent))
                    continue;

            dist = Distance (client->edict->v.origin, ent->v.origin);
            if (!best || dist < best_dist)
            {
                    best = ent;
                    best_dist = dist;
            }
        }

        return best;
}

static edict_t *SV_BotFindEnemy (client_t *client)
{
        edict_t *best = NULL;
        float best_dist = 0.0f;
        int i;

        for (i = 0; i < svs.maxclients; i++)
        {
                client_t *other = &svs.clients[i];
                edict_t *ent;
                float dist;

                if (!other->active || !other->spawned)
                        continue;
                if (other == client)
                        continue;

                ent = other->edict;
                if (!ent || ent->free)
                        continue;
                if (ent->v.health <= 0)
                        continue;

                dist = Distance (client->edict->v.origin, ent->v.origin);
                if (!best || dist < best_dist)
                {
                        best = ent;
                        best_dist = dist;
                }
        }

        return best;
}

static qboolean SV_BotHasLineOfSight (edict_t *self, edict_t *target)
{
        vec3_t start, end;
        trace_t trace;

        VectorAdd (self->v.origin, self->v.view_ofs, start);
        VectorCopy (target->v.origin, end);
        if ((int) target->v.flags & FL_CLIENT)
                VectorAdd (end, target->v.view_ofs, end);

        trace = SV_Move (start, vec3_origin, vec3_origin, end, MOVE_NORMAL, self);

        return trace.ent == target || trace.fraction >= 1.0f;
}

static void SV_BotSelectGoal (client_t *client)
{
        edict_t *enemy = SV_BotFindEnemy (client);
        bot_state_t *bot = &client->bot;
        const bot_skill_config_t *config = SV_BotSkillConfig (bot);
        float reaction;

        if (enemy)
        {
                bot->target_ent = NUM_FOR_EDICT (enemy);
                bot->has_enemy = true;
        }
        else
        {
                edict_t *pickup = SV_BotFindPickup (client);
                if (pickup)
                {
                        bot->target_ent = NUM_FOR_EDICT (pickup);
                        bot->has_enemy = false;
                }
                else
                {
                        float angle = SV_BotRandomFloat (0.f, 360.f);
                        bot->target_ent = 0;
                        bot->has_enemy = false;
                        bot->wander_dir[0] = cos (DEG2RAD (angle));
                        bot->wander_dir[1] = sin (DEG2RAD (angle));
                        bot->wander_dir[2] = 0;
                }
        }

        reaction = config ? config->reaction_time : 1.0f;
        bot->next_repath = qcvm->time + q_max (0.1f, reaction);
}

static edict_t *SV_BotGetTarget (const bot_state_t *bot)
{
        if (bot->target_ent <= 0 || bot->target_ent >= qcvm->num_edicts)
                return NULL;
        return EDICT_NUM (bot->target_ent);
}

void SV_BotFrame (client_t *client)
{
	bot_state_t *bot;
	edict_t *self;
	edict_t *target;
	usercmd_t *cmd;
	float maxspeed;
	vec3_t goal;
	vec3_t to_goal;
	float dist;
	const bot_skill_config_t *config;

	if (!SV_ClientIsBot (client))
		return;

	self = client->edict;
	if (!self || self->free)
		return;

	bot = &client->bot;
	config = SV_BotSkillConfig (bot);
	cmd = &client->cmd;
	memset (cmd, 0, sizeof (*cmd));

	self->v.button0 = 0;
	self->v.button2 = 0;
	self->v.impulse = 0;

	if (!client->spawned)
		return;

	if (self->v.health <= 0)
	{
		if (!bot->death_taunted)
		{
			edict_t *killer = NULL;

			if (bot->last_enemy_ent > 0 && bot->last_enemy_ent < qcvm->num_edicts)
				killer = EDICT_NUM (bot->last_enemy_ent);

			SV_BotMaybeTaunt (client, BOT_CHAT_DEATH, killer);
			bot->death_taunted = true;
		}

		self->v.button0 = 1;
		bot->next_repath = qcvm->time + BOT_RESPAWN_WAIT;
		return;
	}

	bot->death_taunted = false;

	if (bot->stuck_check <= qcvm->time)
	{
		float moved = Distance (self->v.origin, bot->last_origin);
		if (moved < 8.0f)
			bot->next_repath = qcvm->time;
		VectorCopy (self->v.origin, bot->last_origin);
		bot->stuck_check = qcvm->time + BOT_STUCK_INTERVAL;
	}

	target = SV_BotGetTarget (bot);
        if (bot->has_enemy && target && !target->free && target->v.health <= 0)
        {
                bot->has_enemy = false;
                bot->target_ent = 0;
                bot->next_repath = qcvm->time;
        }

	if (bot->next_repath <= qcvm->time || !target || target->free)
		SV_BotSelectGoal (client);

	target = SV_BotGetTarget (bot);
	VectorClear (goal);

	if (target && !target->free)
	{
		VectorCopy (target->v.origin, goal);
		if (bot->has_enemy)
			VectorAdd (goal, target->v.view_ofs, goal);
	}
	else
	{
		if (VectorLengthSquared (bot->wander_dir) < 0.01f)
		{
			float angle = SV_BotRandomFloat (0.f, 360.f);
			bot->wander_dir[0] = cos (DEG2RAD (angle));
			bot->wander_dir[1] = sin (DEG2RAD (angle));
			bot->wander_dir[2] = 0;
		}
		VectorMA (self->v.origin, BOT_WANDER_DISTANCE, bot->wander_dir, goal);
	}

	VectorSubtract (goal, self->v.origin, to_goal);
	dist = VectorNormalize (to_goal);

	if (dist > 0.001f)
	{
		vec3_t angles;
		VectorAngles (to_goal, angles);
		if (bot->has_enemy && config && config->aim_inaccuracy > 0.0f)
		{
			angles[0] += SV_BotRandomFloat (-config->aim_inaccuracy, config->aim_inaccuracy);
			angles[1] += SV_BotRandomFloat (-config->aim_inaccuracy, config->aim_inaccuracy);
		}
		VectorCopy (angles, self->v.v_angle);
	}

	maxspeed = sv_maxspeed.value > 0 ? sv_maxspeed.value : 320.0f;
	cmd->upmove = 0;

	if (dist > 24.0f)
	{
		float forward_speed = config ? config->forward_speed : 200.0f;
		cmd->forwardmove = q_min (maxspeed, forward_speed);
	}
	else
	{
		cmd->forwardmove = 0;
	}

	if (bot->has_enemy)
	{
		float interval_min = config ? config->strafe_interval_min : 0.75f;
		float interval_max = config ? config->strafe_interval_max : 1.25f;
		float sidemove = config ? config->strafe_speed : 150.0f;

		if (interval_max < interval_min)
			interval_max = interval_min;

		if (bot->strafe_change_time <= qcvm->time)
		{
			bot->strafe_change_time = qcvm->time + SV_BotRandomFloat (interval_min, interval_max);
			bot->strafe_dir = (rand () & 1 ? 1.0f : -1.0f) * q_min (maxspeed, sidemove);
		}
		cmd->sidemove = bot->strafe_dir;
	}
	else
	{
		cmd->sidemove = 0;
		bot->strafe_dir = 0.0f;
	}

	if (bot->has_enemy && target && !target->free)
	{
		int enemy_ent = NUM_FOR_EDICT (target);
		if (bot->last_enemy_ent != enemy_ent)
		{
			bot->last_enemy_ent = enemy_ent;
			bot->last_enemy_health = target->v.health;
			bot->next_attack_time = qcvm->time;
			SV_BotMaybeTaunt (client, BOT_CHAT_ENGAGE, target);
		}
		else
		{
                        if (bot->last_enemy_health > 0 && target->v.health <= 0)
                        {
                                SV_BotMaybeTaunt (client, BOT_CHAT_KILL, target);
                                bot->has_enemy = false;
                                bot->target_ent = 0;
                                bot->next_repath = qcvm->time;
                        }
			bot->last_enemy_health = target->v.health;
		}

		if (SV_BotHasLineOfSight (self, target) && dist <= BOT_ATTACK_RANGE)
		{
			float attack_delay = config ? config->attack_delay : 0.2f;

			if (qcvm->time >= bot->next_attack_time)
			{
				self->v.button0 = 1;
				bot->next_attack_time = qcvm->time + q_max (0.05f, attack_delay);
			}
		}

		if (dist > BOT_FIRE_RANGE)
		{
			float forward_speed = config ? config->forward_speed : 200.0f;
			cmd->forwardmove = q_min (maxspeed, forward_speed);
		}
	}
        else
        {
                if (!bot->has_enemy)
                {
                        bot->last_enemy_health = 0.0f;
                        if (bot->last_enemy_ent <= 0 || bot->last_enemy_ent >= qcvm->num_edicts || EDICT_NUM (bot->last_enemy_ent)->free)
                                bot->last_enemy_ent = 0;
                }
        }

	if (!bot->has_enemy && dist < 16.0f)
		bot->next_repath = qcvm->time;
}

