#include "quakedef.h"

extern edict_t *sv_player;

static cvar_t sv_bot_spawn = {"sv_bot_spawn", "0", CVAR_NONE};
static cvar_t sv_bot_remove = {"sv_bot_remove", "0", CVAR_NONE};

typedef struct
{
	qboolean	active;
	vec3_t		wander_dir;
	float		wander_yaw;
	double		next_wander_time;
	double		last_stuck_time;
	vec3_t		last_position;
} sv_bot_state_t;

static sv_bot_state_t sv_bot_states[MAX_SCOREBOARD];
static int bot_name_counter = 1;

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

static void SV_Bot_AssignName (client_t *client)
{
	char name[sizeof(client->name)];
	int attempts = 0;

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
	client_t *enemy = SV_Bot_FindTarget (client, true);
	vec3_t delta;
	double now = qcvm->time;

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
				state->next_wander_time = 0.0;
			VectorCopy (self->v.origin, state->last_position);
			state->last_stuck_time = now;
		}

		if (state->next_wander_time <= now)
		{
			SV_Bot_PickWanderDirection (state);
		}
	}

	if (enemy)
	{
		vec3_t dir;
		vec3_t angles;
		float dist;
		qboolean retreat;

		VectorSubtract (enemy->edict->v.origin, self->v.origin, dir);
		dist = VectorLength (dir);
		if (dist > 0)
			VectorScale (dir, 1.0f / dist, dir);
		else
			VectorClear (dir);

		VectorAngles (dir, angles);
		angles[PITCH] = CLAMP (-60.0f, angles[PITCH], 60.0f);
		VectorCopy (angles, self->v.v_angle);

		retreat = (self->v.health < 40) || (dist < 100.0f);
		if (retreat && dist < 400.0f)
		{
			cmd->forwardmove = -200.0f;
			cmd->sidemove = (rand () & 1) ? 180.0f : -180.0f;
		}
		else
		{
			cmd->forwardmove = 200.0f;
			if (dist > 80.0f)
				cmd->sidemove = (rand () & 1) ? 120.0f : -120.0f;
			if (dist < 600.0f)
				self->v.button0 = 1;
		}
	}
	else if (state)
	{
		vec3_t angles;

		VectorAngles (state->wander_dir, angles);
		VectorCopy (angles, self->v.v_angle);
		cmd->forwardmove = 200.0f;
		if (rand () & 1)
			cmd->sidemove = (rand () & 1) ? 80.0f : -80.0f;
	}

	if (self->v.waterlevel >= 2)
		cmd->upmove = 200.0f;

	VectorCopy (self->v.v_angle, cmd->viewangles);
}

static qboolean SV_Bot_SpawnOne (void)
{
	client_t *client;
	sv_bot_state_t *state;
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

	client->colors = ((rand () & 15) << 4) | (rand () & 15);
	SV_Bot_AssignName (client);

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
