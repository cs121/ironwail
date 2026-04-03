#include "quakedef.h"
#include "bot_main.h"
#include "bot_ai.h"
#include "bot_nav2.h"
#include "bot_combat.h"

#define BOT_MAX_ROAM_POINTS 512
#define BOT_GOAL_REACH_DIST 56.f
#define BOT_ENEMY_FORGET_TIME 2.5

typedef enum bot_item_type_e
{
	BOT_ITEM_NONE = 0,
	BOT_ITEM_WEAPON,
	BOT_ITEM_AMMO,
	BOT_ITEM_HEALTH,
	BOT_ITEM_ARMOR,
	BOT_ITEM_POWERUP
} bot_item_type_t;

typedef enum bot_ammo_type_e
{
	BOT_AMMO_NONE = 0,
	BOT_AMMO_SHELLS,
	BOT_AMMO_NAILS,
	BOT_AMMO_ROCKETS,
	BOT_AMMO_CELLS
} bot_ammo_type_t;

static vec3_t g_roam_points[BOT_MAX_ROAM_POINTS];
static int g_roam_count;

static float BotAI_Noise01 (uint32_t seed)
{
	seed ^= seed >> 16;
	seed *= 0x7feb352dU;
	seed ^= seed >> 15;
	seed *= 0x846ca68bU;
	seed ^= seed >> 16;
	return (float) (seed & 0xffffU) / 65535.f;
}

static float BotAI_Random01 (const bot_state_t *bot, uint32_t salt)
{
	uint32_t frame = (uint32_t) host_framecount;
	uint32_t botseed = bot ? (uint32_t) (bot->clientnum + 1) * 1315423911U : 0U;
	return BotAI_Noise01 (frame * 1664525U + botseed + salt * 374761393U);
}

static qboolean BotAI_HasPrefix (const char *value, const char *prefix)
{
	return q_strncasecmp (value, prefix, strlen (prefix)) == 0;
}

static qboolean BotAI_IsAlivePlayer (edict_t *ent)
{
	if (!ent)
		return false;
	if (ent->free)
		return false;
	if (ent->v.health <= 0.f)
		return false;
	if ((int) ent->v.deadflag != DEAD_NO)
		return false;
	return true;
}

static qboolean BotAI_IsTeammate (edict_t *self, edict_t *other)
{
	if (!self || !other)
		return false;
	if (!teamplay.value)
		return false;
	if ((int) self->v.team <= 0 || (int) other->v.team <= 0)
		return false;
	return (int) self->v.team == (int) other->v.team;
}

static qboolean BotAI_CanSee (edict_t *self, edict_t *other)
{
	vec3_t start;
	vec3_t end;
	trace_t tr;

	if (!self || !other)
		return false;

	VectorAdd (self->v.origin, self->v.view_ofs, start);
	VectorAdd (other->v.origin, other->v.view_ofs, end);
	tr = SV_Move (start, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, self);
	return tr.fraction >= 1.f;
}

static qboolean BotAI_IsInterestingRoamClass (const char *classname)
{
	if (!classname || !classname[0])
		return false;

	if (BotAI_HasPrefix (classname, "info_player_"))
		return true;
	if (BotAI_HasPrefix (classname, "weapon_"))
		return true;
	if (BotAI_HasPrefix (classname, "ammo_"))
		return true;
	if (q_strcasestr (classname, "health"))
		return true;
	if (q_strcasestr (classname, "armor"))
		return true;
	if (q_strcasestr (classname, "artifact"))
		return true;

	return false;
}

static void BotAI_AddRoamPoint (const vec3_t point)
{
	int i;

	if (g_roam_count >= BOT_MAX_ROAM_POINTS)
		return;

	for (i = 0; i < g_roam_count; ++i)
	{
		vec3_t delta;
		VectorSubtract (point, g_roam_points[i], delta);
		if (DotProduct (delta, delta) < 80.f * 80.f)
			return;
	}

	VectorCopy (point, g_roam_points[g_roam_count]);
	g_roam_count++;
}

void BotAI_OnMapSpawn (void)
{
	int i;

	g_roam_count = 0;

	for (i = 1; i < qcvm->num_edicts; ++i)
	{
		edict_t *ent = EDICT_NUM (i);
		const char *classname;
		vec3_t point;

		if (ent->free)
			continue;

		classname = PR_GetString (ent->v.classname);
		if (!BotAI_IsInterestingRoamClass (classname))
			continue;

		VectorCopy (ent->v.origin, point);
		point[2] += 20.f;
		BotAI_AddRoamPoint (point);
	}

	for (i = 0; i < svs.maxclients; ++i)
	{
		client_t *client = &svs.clients[i];
		if (!client->active || !client->edict)
			continue;
		BotAI_AddRoamPoint (client->edict->v.origin);
	}

	if (!g_roam_count)
	{
		vec3_t fallback = {0.f, 0.f, 32.f};
		BotAI_AddRoamPoint (fallback);
	}

	if (bot_think_debug.value)
		Con_Printf ("BotAI: collected %d roam points\n", g_roam_count);
}

static bot_item_type_t BotAI_ClassifyItem (const char *classname)
{
	if (!classname || !classname[0])
		return BOT_ITEM_NONE;
	if (BotAI_HasPrefix (classname, "weapon_"))
		return BOT_ITEM_WEAPON;
	if (BotAI_HasPrefix (classname, "ammo_"))
		return BOT_ITEM_AMMO;
	if (q_strcasestr (classname, "item_health"))
		return BOT_ITEM_HEALTH;
	if (q_strcasestr (classname, "armor"))
		return BOT_ITEM_ARMOR;
	if (q_strcasestr (classname, "artifact") || q_strcasestr (classname, "powerup"))
		return BOT_ITEM_POWERUP;
	return BOT_ITEM_NONE;
}

static int BotAI_WeaponForClassname (const char *classname)
{
	if (!classname)
		return 0;
	if (q_strcasecmp (classname, "weapon_shotgun") == 0)
		return IT_SHOTGUN;
	if (q_strcasecmp (classname, "weapon_supershotgun") == 0)
		return IT_SUPER_SHOTGUN;
	if (q_strcasecmp (classname, "weapon_nailgun") == 0)
		return IT_NAILGUN;
	if (q_strcasecmp (classname, "weapon_supernailgun") == 0)
		return IT_SUPER_NAILGUN;
	if (q_strcasecmp (classname, "weapon_grenadelauncher") == 0)
		return IT_GRENADE_LAUNCHER;
	if (q_strcasecmp (classname, "weapon_rocketlauncher") == 0)
		return IT_ROCKET_LAUNCHER;
	if (q_strcasecmp (classname, "weapon_lightning") == 0)
		return IT_LIGHTNING;
	return 0;
}

static bot_ammo_type_t BotAI_AmmoForClassname (const char *classname)
{
	if (!classname)
		return BOT_AMMO_NONE;
	if (q_strcasestr (classname, "shell"))
		return BOT_AMMO_SHELLS;
	if (q_strcasestr (classname, "nail"))
		return BOT_AMMO_NAILS;
	if (q_strcasestr (classname, "rocket"))
		return BOT_AMMO_ROCKETS;
	if (q_strcasestr (classname, "cell"))
		return BOT_AMMO_CELLS;
	return BOT_AMMO_NONE;
}

static qboolean BotAI_ItemPotentiallyReachable (edict_t *self, edict_t *item)
{
	vec3_t start;
	vec3_t end;
	trace_t tr;

	if (!self || !item)
		return false;

	if (BotNav_IsLoaded () && bot_use_nav2.value)
	{
		int from = BotNav_FindNearestNode (self->v.origin);
		int to = BotNav_FindNearestNode (item->v.origin);
		bot_path_t path;

		if (from < 0 || to < 0)
			return false;
		return BotNav_FindPath (from, to, &path);
	}

	VectorAdd (self->v.origin, self->v.view_ofs, start);
	VectorCopy (item->v.origin, end);
	tr = SV_Move (start, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, self);
	return tr.fraction >= 1.f;
}

static float BotAI_ItemScore (bot_state_t *bot, edict_t *self, edict_t *item, const char *classname, qboolean enemy_visible, float dist)
{
	bot_item_type_t item_type;
	float score = -1.f;

	(void) item;

	item_type = BotAI_ClassifyItem (classname);
	if (item_type == BOT_ITEM_NONE)
		return -1.f;

	switch (item_type)
	{
	case BOT_ITEM_POWERUP:
		score = 260.f;
		break;

	case BOT_ITEM_WEAPON:
	{
		int weapon = BotAI_WeaponForClassname (classname);
		if (weapon)
		{
			qboolean has_weapon = (((int) self->v.items) & weapon) != 0;
			score = has_weapon ? 50.f : 170.f;
			if (weapon == IT_ROCKET_LAUNCHER || weapon == IT_LIGHTNING || weapon == IT_SUPER_NAILGUN)
				score += has_weapon ? 20.f : 70.f;
		}
		break;
	}

	case BOT_ITEM_AMMO:
	{
		bot_ammo_type_t ammo = BotAI_AmmoForClassname (classname);
		switch (ammo)
		{
		case BOT_AMMO_SHELLS:
			score = self->v.ammo_shells < 25 ? 110.f : 50.f;
			break;
		case BOT_AMMO_NAILS:
			score = self->v.ammo_nails < 40 ? 120.f : 55.f;
			break;
		case BOT_AMMO_ROCKETS:
			score = self->v.ammo_rockets < 10 ? 165.f : 65.f;
			break;
		case BOT_AMMO_CELLS:
			score = self->v.ammo_cells < 20 ? 175.f : 70.f;
			break;
		default:
			score = 35.f;
			break;
		}
		break;
	}

	case BOT_ITEM_HEALTH:
		if (self->v.health < 35.f)
			score = 240.f;
		else if (self->v.health < 70.f)
			score = 170.f;
		else
			score = 45.f;
		break;

	case BOT_ITEM_ARMOR:
		if (self->v.armorvalue < 40.f)
			score = 150.f;
		else if (self->v.armorvalue < 90.f)
			score = 90.f;
		else
			score = 35.f;
		break;

	default:
		break;
	}

	if (score < 0.f)
		return -1.f;

	score -= dist * 0.05f;
	if (enemy_visible && item_type != BOT_ITEM_HEALTH && item_type != BOT_ITEM_POWERUP)
		score *= 0.65f;
	if (!BotAI_ItemPotentiallyReachable (self, item))
		score *= 0.2f;
	if (bot->state == BOT_STATE_RETREAT && item_type == BOT_ITEM_HEALTH)
		score += 80.f;

	return score;
}

static edict_t *BotAI_FindBestItem (bot_state_t *bot, edict_t *self, qboolean enemy_visible)
{
	int i;
	float best_score = 0.f;
	edict_t *best = NULL;

	for (i = svs.maxclients + 1; i < qcvm->num_edicts; ++i)
	{
		edict_t *item = EDICT_NUM (i);
		const char *classname;
		vec3_t delta;
		float dist;
		float score;

		if (!item || item->free)
			continue;
		if ((int) item->v.solid == SOLID_NOT)
			continue;

		classname = PR_GetString (item->v.classname);
		if (BotAI_ClassifyItem (classname) == BOT_ITEM_NONE)
			continue;

		VectorSubtract (item->v.origin, self->v.origin, delta);
		dist = VectorLength (delta);
		if (dist > 2200.f)
			continue;

		score = BotAI_ItemScore (bot, self, item, classname, enemy_visible, dist);
		if (score > best_score)
		{
			best_score = score;
			best = item;
		}
	}

	return best;
}

static edict_t *BotAI_FindBestEnemy (bot_state_t *bot, edict_t *self, qboolean *out_visible)
{
	int i;
	float best_score = -1.f;
	edict_t *best = NULL;

	*out_visible = false;

	for (i = 0; i < svs.maxclients; ++i)
	{
		client_t *client = &svs.clients[i];
		edict_t *candidate;
		vec3_t delta;
		float dist;
		float score;

		if (!client->active || !client->spawned)
			continue;
		if (i == bot->clientnum)
			continue;

		candidate = client->edict;
		if (!BotAI_IsAlivePlayer (candidate))
			continue;
		if (BotAI_IsTeammate (self, candidate))
			continue;
		if (!BotAI_CanSee (self, candidate))
			continue;

		VectorSubtract (candidate->v.origin, self->v.origin, delta);
		dist = VectorLength (delta);
		if (dist > 2500.f)
			continue;

		score = 2600.f - dist;
		score += candidate->v.frags * 2.f;
		if (score > best_score)
		{
			best_score = score;
			best = candidate;
		}
	}

	if (best)
		*out_visible = true;

	return best;
}

static float BotAI_AngleDelta (float from, float to)
{
	float delta = anglemod (to) - anglemod (from);
	if (delta > 180.f)
		delta -= 360.f;
	if (delta < -180.f)
		delta += 360.f;
	return delta;
}

static float BotAI_ApproachAngle (float from, float to, float max_step)
{
	float delta = BotAI_AngleDelta (from, to);
	if (delta > max_step)
		delta = max_step;
	if (delta < -max_step)
		delta = -max_step;
	return anglemod (from + delta);
}

const char *BotAI_StateName (bot_ai_state_t state)
{
	switch (state)
	{
	case BOT_STATE_ROAM:
		return "ROAM";
	case BOT_STATE_SEEK_ITEM:
		return "SEEK_ITEM";
	case BOT_STATE_CHASE_ENEMY:
		return "CHASE_ENEMY";
	case BOT_STATE_ATTACK:
		return "ATTACK";
	case BOT_STATE_RETREAT:
		return "RETREAT";
	case BOT_STATE_STUCK_RECOVERY:
		return "STUCK_RECOVERY";
	default:
		return "UNKNOWN";
	}
}

void BotAI_ResetState (bot_state_t *bot)
{
	double now = (sv.active && sv.qcvm.progs) ? sv.qcvm.time : 0.0;

	if (!bot)
		return;

	bot->state = BOT_STATE_ROAM;
	bot->enemy = NULL;
	VectorCopy (vec3_origin, bot->enemy_last_pos);
	bot->enemy_last_seen_time = -9999.0;
	bot->goal_item = NULL;
	VectorCopy (vec3_origin, bot->goal_pos);
	bot->has_goal = false;
	bot->goal_timeout = 0.0;
	bot->has_path = false;
	bot->path_index = 0;
	bot->next_repath_time = 0.0;
	VectorCopy (vec3_origin, bot->last_origin);
	bot->last_progress_time = now;
	bot->stuck_until = 0.0;
	bot->next_item_scan_time = 0.0;
	bot->next_debug_time = 0.0;
	bot->respawn_time = 0.0;
	bot->strafe_dir = 1.f;
	bot->next_strafe_change = 0.0;
	bot->last_weapon_switch_time = 0.0;
	bot->last_requested_weapon = IT_AXE;
	bot->roam_point = -1;
}

static void BotAI_SelectRoamGoal (bot_state_t *bot, edict_t *self)
{
	int index;

	if (!g_roam_count)
		return;

	index = ((int) (BotAI_Random01 (bot, 11U) * g_roam_count)) % g_roam_count;
	if (g_roam_count > 1 && index == bot->roam_point)
		index = (index + 1) % g_roam_count;

	bot->roam_point = index;
	VectorCopy (g_roam_points[index], bot->goal_pos);
	if (self && BotNav_IsLoaded () && bot_use_nav2.value)
		bot->next_repath_time = 0.0;
}

static void BotAI_UpdatePath (bot_state_t *bot, edict_t *self)
{
	if (!bot->has_goal)
	{
		bot->has_path = false;
		return;
	}

	if (!BotNav_IsLoaded () || !bot_use_nav2.value)
	{
		bot->has_path = false;
		return;
	}

	if (qcvm->time < bot->next_repath_time)
		return;

	bot->next_repath_time = qcvm->time + 0.8;

	{
		int start = BotNav_FindNearestNode (self->v.origin);
		int goal = BotNav_FindNearestNode (bot->goal_pos);
		if (start >= 0 && goal >= 0 && BotNav_FindPath (start, goal, &bot->path))
		{
			bot->has_path = true;
			bot->path_index = 0;
		}
		else
		{
			bot->has_path = false;
		}
	}
}

static qboolean BotAI_GetMoveTarget (bot_state_t *bot, edict_t *self, vec3_t out_target)
{
	if (bot->has_path)
	{
		while (bot->path_index < bot->path.count)
		{
			vec3_t node_pos;
			vec3_t delta;

			if (!BotNav_GetNodePosition (bot->path.nodes[bot->path_index], node_pos))
				break;

			VectorSubtract (node_pos, self->v.origin, delta);
			if (VectorLength (delta) < 64.f && bot->path_index < bot->path.count - 1)
			{
				bot->path_index++;
				continue;
			}

			VectorCopy (node_pos, out_target);
			return true;
		}

		bot->has_path = false;
	}

	if (!bot->has_goal)
		return false;

	VectorCopy (bot->goal_pos, out_target);
	return true;
}

static void BotAI_ComputeDirectionalMove (const vec3_t view_angles, const vec3_t move_dir, float speed, usercmd_t *cmd)
{
	vec3_t flat_angles;
	vec3_t forward;
	vec3_t right;

	VectorCopy (view_angles, flat_angles);
	flat_angles[PITCH] = 0.f;
	flat_angles[ROLL] = 0.f;

	AngleVectors (flat_angles, forward, right, NULL);
	cmd->forwardmove = DotProduct (move_dir, forward) * speed;
	cmd->sidemove = DotProduct (move_dir, right) * speed;
}

void BotAI_BuildCommand (bot_state_t *bot, client_t *client, usercmd_t *outcmd, vec3_t out_vangle, qboolean *out_attack, int *out_impulse)
{
	edict_t *self;
	edict_t *visible_enemy;
	edict_t *best_item = NULL;
	qboolean enemy_visible = false;
	vec3_t move_target;
	vec3_t move_dir;
	vec3_t move_angles;
	float move_speed = 0.f;
	qboolean has_move_target;

	memset (outcmd, 0, sizeof (*outcmd));
	*out_attack = false;
	*out_impulse = 0;

	if (!bot || !client || !client->edict)
	{
		VectorCopy (vec3_origin, out_vangle);
		return;
	}

	self = client->edict;
	VectorCopy (self->v.v_angle, out_vangle);

	if (!client->spawned)
		return;

	if (!BotAI_IsAlivePlayer (self))
	{
		if (bot->respawn_time <= qcvm->time)
			*out_attack = true;
		bot->respawn_time = qcvm->time + 0.45;
		bot->state = BOT_STATE_ROAM;
		bot->enemy = NULL;
		bot->has_goal = false;
		bot->has_path = false;
		return;
	}

	if (bot_nav_debug.value)
		BotNav_DebugDraw ();

	if (bot->last_progress_time <= 0.0)
	{
		VectorCopy (self->v.origin, bot->last_origin);
		bot->last_progress_time = qcvm->time;
	}

	visible_enemy = BotAI_FindBestEnemy (bot, self, &enemy_visible);
	if (visible_enemy)
	{
		bot->enemy = visible_enemy;
		VectorCopy (visible_enemy->v.origin, bot->enemy_last_pos);
		bot->enemy_last_seen_time = qcvm->time;
	}
	else if (bot->enemy && qcvm->time - bot->enemy_last_seen_time > BOT_ENEMY_FORGET_TIME)
	{
		bot->enemy = NULL;
	}

	if (qcvm->time >= bot->next_item_scan_time)
	{
		bot->next_item_scan_time = qcvm->time + 0.4;
		best_item = BotAI_FindBestItem (bot, self, enemy_visible);
		if (best_item)
		{
			bot->goal_item = best_item;
			VectorCopy (best_item->v.origin, bot->goal_pos);
			bot->goal_pos[2] += 16.f;
			bot->has_goal = true;
			bot->goal_timeout = qcvm->time + 8.0;
		}
	}

	if (bot->goal_item)
	{
		if (bot->goal_item->free || (int) bot->goal_item->v.solid == SOLID_NOT || qcvm->time > bot->goal_timeout)
		{
			bot->goal_item = NULL;
			bot->has_goal = false;
			bot->has_path = false;
		}
		else
		{
			VectorCopy (bot->goal_item->v.origin, bot->goal_pos);
			bot->goal_pos[2] += 16.f;
		}
	}

	if (qcvm->time < bot->stuck_until)
		bot->state = BOT_STATE_STUCK_RECOVERY;
	else if (enemy_visible && bot->enemy)
	{
		if (self->v.health < 30.f)
			bot->state = BOT_STATE_RETREAT;
		else
			bot->state = BOT_STATE_ATTACK;
	}
	else if (bot->enemy && (qcvm->time - bot->enemy_last_seen_time) <= BOT_ENEMY_FORGET_TIME)
	{
		bot->state = BOT_STATE_CHASE_ENEMY;
		bot->has_goal = true;
		VectorCopy (bot->enemy_last_pos, bot->goal_pos);
		bot->goal_pos[2] += 20.f;
	}
	else if (bot->has_goal)
		bot->state = BOT_STATE_SEEK_ITEM;
	else
		bot->state = BOT_STATE_ROAM;

	if (bot->state == BOT_STATE_ROAM)
	{
		vec3_t delta;
		if (bot->roam_point < 0)
			BotAI_SelectRoamGoal (bot, self);
		VectorSubtract (bot->goal_pos, self->v.origin, delta);
		if (!bot->has_goal || VectorLength (delta) < BOT_GOAL_REACH_DIST)
			BotAI_SelectRoamGoal (bot, self);
		bot->has_goal = true;
	}

	if (bot->state == BOT_STATE_RETREAT && bot->goal_item)
	{
		/* keep existing item goal */
	}
	else if (bot->state == BOT_STATE_RETREAT && bot->enemy)
	{
		vec3_t away;
		VectorSubtract (self->v.origin, bot->enemy->v.origin, away);
		away[2] = 0.f;
		if (VectorNormalize (away) > 0.f)
		{
			VectorMA (self->v.origin, 300.f, away, bot->goal_pos);
			bot->goal_pos[2] = self->v.origin[2];
			bot->has_goal = true;
		}
	}

	if (bot->has_goal)
		BotAI_UpdatePath (bot, self);

	has_move_target = BotAI_GetMoveTarget (bot, self, move_target);
	if (!has_move_target)
		VectorCopy (self->v.origin, move_target);

	VectorSubtract (move_target, self->v.origin, move_dir);
	move_dir[2] = 0.f;
	if (VectorNormalize (move_dir) <= 0.f)
	{
		move_dir[0] = 1.f;
		move_dir[1] = 0.f;
		move_dir[2] = 0.f;
	}

	VectorAngles (move_dir, move_angles);
	out_vangle[YAW] = BotAI_ApproachAngle (self->v.v_angle[YAW], move_angles[YAW], 540.f * host_frametime);
	out_vangle[PITCH] = BotAI_ApproachAngle (self->v.v_angle[PITCH], 0.f, 360.f * host_frametime);
	out_vangle[ROLL] = 0.f;

	switch (bot->state)
	{
	case BOT_STATE_ATTACK:
		if (bot->enemy)
		{
			vec3_t enemy_dir;
			vec3_t strafe;
			float dist;
			int desired_weapon;
			int current_weapon;
			vec3_t aim_angles;

			VectorSubtract (bot->enemy->v.origin, self->v.origin, enemy_dir);
			enemy_dir[2] = 0.f;
			dist = VectorNormalize (enemy_dir);

			if (qcvm->time >= bot->next_strafe_change)
			{
				bot->strafe_dir = BotAI_Random01 (bot, 21U) > 0.5f ? 1.f : -1.f;
				bot->next_strafe_change = qcvm->time + 0.6 + BotAI_Random01 (bot, 22U) * 0.4;
			}

			strafe[0] = -enemy_dir[1] * bot->strafe_dir;
			strafe[1] = enemy_dir[0] * bot->strafe_dir;
			strafe[2] = 0.f;

			if (dist < 170.f)
				VectorMA (strafe, -0.8f, enemy_dir, move_dir);
			else if (dist > 450.f)
				VectorMA (strafe, 0.8f, enemy_dir, move_dir);
			else
				VectorCopy (strafe, move_dir);
			VectorNormalize (move_dir);
			move_speed = 290.f;

			desired_weapon = BotCombat_SelectWeapon (self, dist, enemy_visible);
			current_weapon = (int) self->v.weapon;
			if (desired_weapon != current_weapon && qcvm->time - bot->last_weapon_switch_time > 0.35)
			{
				*out_impulse = BotCombat_WeaponImpulse (desired_weapon);
				bot->last_weapon_switch_time = qcvm->time;
				bot->last_requested_weapon = desired_weapon;
			}

			BotCombat_ComputeAim (bot, self, bot->enemy, aim_angles);
			out_vangle[YAW] = BotAI_ApproachAngle (out_vangle[YAW], aim_angles[YAW], 720.f * host_frametime);
			out_vangle[PITCH] = BotAI_ApproachAngle (out_vangle[PITCH], aim_angles[PITCH], 540.f * host_frametime);

			if (BotCombat_ShouldFire (self, bot->enemy, desired_weapon, out_vangle))
				*out_attack = true;
		}
		else
			move_speed = 260.f;
		break;

	case BOT_STATE_RETREAT:
		move_speed = 320.f;
		break;

	case BOT_STATE_STUCK_RECOVERY:
		move_speed = 180.f;
		outcmd->forwardmove = 180.f;
		outcmd->sidemove = (BotAI_Random01 (bot, 23U) > 0.5f ? 1.f : -1.f) * 320.f;
		outcmd->upmove = 200.f;
		break;

	case BOT_STATE_CHASE_ENEMY:
		move_speed = 310.f;
		break;

	case BOT_STATE_SEEK_ITEM:
		move_speed = 300.f;
		break;

	case BOT_STATE_ROAM:
	default:
		move_speed = 290.f;
		break;
	}

	if (bot->state != BOT_STATE_STUCK_RECOVERY)
	{
		BotAI_ComputeDirectionalMove (out_vangle, move_dir, move_speed, outcmd);

		if (((int) self->v.flags & FL_ONGROUND) && (move_target[2] - self->v.origin[2] > 22.f))
			outcmd->upmove = 200.f;
		else if (((int) self->v.flags & FL_ONGROUND) && BotAI_Random01 (bot, 24U) < 0.002f)
			outcmd->upmove = 200.f;
	}

	{
		vec3_t delta;
		float moved2;
		float cmd_mag;

		VectorSubtract (self->v.origin, bot->last_origin, delta);
		moved2 = DotProduct (delta, delta);
		cmd_mag = fabsf (outcmd->forwardmove) + fabsf (outcmd->sidemove);

		if (moved2 > 12.f * 12.f)
		{
			VectorCopy (self->v.origin, bot->last_origin);
			bot->last_progress_time = qcvm->time;
		}
		else if (cmd_mag > 80.f && (qcvm->time - bot->last_progress_time) > 1.25)
		{
			bot->stuck_until = qcvm->time + 0.75;
			bot->has_path = false;
			bot->has_goal = false;
			bot->roam_point = -1;
			BotAI_SelectRoamGoal (bot, self);
		}
	}

	if (bot->has_goal)
	{
		vec3_t delta;
		VectorSubtract (bot->goal_pos, self->v.origin, delta);
		if (VectorLength (delta) < BOT_GOAL_REACH_DIST)
		{
			bot->has_goal = false;
			bot->goal_item = NULL;
			bot->has_path = false;
		}
	}

	if (bot_think_debug.value && bot->next_debug_time <= qcvm->time)
	{
		const char *enemy_name = "none";
		if (bot->enemy)
			enemy_name = PR_GetString (bot->enemy->v.netname);
		Con_Printf (
			"BotThink: %s state=%s enemy=%s goal=%s path=%d/%d stuck=%0.2f\n",
			bot->name,
			BotAI_StateName (bot->state),
			enemy_name,
			bot->has_goal ? "yes" : "no",
			bot->path_index,
			bot->has_path ? bot->path.count : 0,
			(float) q_max (0.0, bot->stuck_until - qcvm->time)
		);
		bot->next_debug_time = qcvm->time + 1.0;
	}
}
