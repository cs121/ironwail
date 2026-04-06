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

static qboolean BotAI_ShouldUseNav2 (void)
{
	return BotNav_IsLoaded ();
}

static const char *BotAI_TryGetString (int string_id)
{
	if (!qcvm)
		return NULL;
	if (string_id >= 0 && string_id < qcvm->stringssize)
		return PR_GetString (string_id);
	if (string_id < 0 && string_id >= -qcvm->numknownstrings)
		return PR_GetString (string_id);
	return NULL;
}

static qboolean BotAI_IsFailedEdge (const bot_state_t *bot, int from, int to)
{
	int i;
	if (!bot || from < 0 || to < 0)
		return false;

	for (i = 0; i < (int) countof (bot->failed_edge_from); ++i)
	{
		if (bot->failed_edge_until[i] < qcvm->time)
			continue;
		if (bot->failed_edge_from[i] == from && bot->failed_edge_to[i] == to)
			return true;
	}
	return false;
}

static void BotAI_RegisterFailedEdge (bot_state_t *bot, int from, int to)
{
	int i;
	int slot;
	if (!bot || from < 0 || to < 0)
		return;

	for (i = 0; i < (int) countof (bot->failed_edge_from); ++i)
	{
		if (bot->failed_edge_from[i] == from && bot->failed_edge_to[i] == to)
		{
			bot->failed_edge_until[i] = qcvm->time + 6.0;
			return;
		}
	}

	slot = bot->failed_edge_cursor % (int) countof (bot->failed_edge_from);
	bot->failed_edge_cursor++;
	bot->failed_edge_from[slot] = from;
	bot->failed_edge_to[slot] = to;
	bot->failed_edge_until[slot] = qcvm->time + 6.0;
}

static qboolean BotAI_IsFailedGoalNode (const bot_state_t *bot, int node)
{
	if (!bot || node < 0)
		return false;
	return bot->failed_goal_node == node && qcvm->time < bot->failed_goal_node_until;
}

static qboolean BotAI_CanPathToPos (bot_state_t *bot, edict_t *self, const vec3_t goal_pos, qboolean reject_failed_first_edge)
{
	int start;
	int goal;
	bot_path_t path;

	if (!bot || !self || !BotAI_ShouldUseNav2 ())
		return true;

	start = BotNav_FindNearestNode (self->v.origin);
	goal = BotNav_FindNearestNode (goal_pos);
	if (start < 0 || goal < 0)
		return false;
	if (!BotNav_FindPath (start, goal, &path))
		return false;
	if (path.count <= 1)
		return false;
	if (reject_failed_first_edge && BotAI_IsFailedEdge (bot, path.nodes[0], path.nodes[1]))
		return false;

	return true;
}

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

static qboolean BotAI_IsCompanionMode (const bot_state_t *bot)
{
	int i;
	int humans = 0;

	if (!bot || cls.state == ca_dedicated)
		return false;
	if (deathmatch.value)
		return false;

	for (i = 0; i < svs.maxclients; ++i)
	{
		const client_t *candidate_client = &svs.clients[i];
		if (!candidate_client->active || !candidate_client->spawned || candidate_client->isbot || !candidate_client->edict)
			continue;
		if (!BotAI_IsAlivePlayer (candidate_client->edict))
			continue;
		humans++;
	}

	if (coop.value)
		return humans > 0;

	return humans == 1;
}

static edict_t *BotAI_FindLeaderPlayer (const bot_state_t *bot, edict_t *self)
{
	int i;
	float best_dist = 999999.f;
	edict_t *best = NULL;

	if (!bot || !self || !BotAI_IsCompanionMode (bot))
		return NULL;

	for (i = 0; i < svs.maxclients; ++i)
	{
		client_t *candidate_client = &svs.clients[i];
		edict_t *candidate;
		vec3_t delta;
		float dist;

		if (!candidate_client->active || !candidate_client->spawned || candidate_client->isbot || !candidate_client->edict)
			continue;
		candidate = candidate_client->edict;
		if (!BotAI_IsAlivePlayer (candidate))
			continue;
		VectorSubtract (candidate->v.origin, self->v.origin, delta);
		dist = VectorLength (delta);
		if (dist < best_dist)
		{
			best_dist = dist;
			best = candidate;
		}
	}

	return best;
}

static qboolean BotAI_IsEntityMoving (edict_t *ent, float min_speed)
{
	vec3_t velocity;

	if (!ent)
		return false;

	VectorCopy (ent->v.velocity, velocity);
	velocity[2] = 0.f;
	return VectorLength (velocity) > min_speed;
}

static qboolean BotAI_UpdateDamageAlert (float current_health, float *last_health)
{
	qboolean took_damage = false;

	if (!last_health)
		return false;

	if (*last_health > 0.f && current_health < (*last_health - 2.f))
		took_damage = true;

	*last_health = current_health;
	return took_damage;
}

static qboolean BotAI_UpdateCompanionAlert (bot_state_t *bot, edict_t *self, edict_t *leader)
{
	qboolean alerted = false;

	if (!bot || !self)
		return false;

	if (BotAI_UpdateDamageAlert (self->v.health, &bot->last_health))
	{
		VectorCopy (self->v.origin, bot->alert_pos);
		bot->alert_until = qcvm->time + 3.0;
		alerted = true;
	}

	if (leader && BotAI_UpdateDamageAlert (leader->v.health, &bot->last_leader_health))
	{
		VectorCopy (leader->v.origin, bot->alert_pos);
		bot->alert_until = qcvm->time + 3.0;
		alerted = true;
	}
	else if (!leader)
	{
		bot->last_leader_health = 0.f;
	}

	return alerted;
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

static qboolean BotAI_IsMonsterEnemyCandidate (edict_t *ent)
{
	const char *classname;

	if (!ent || ent->free)
		return false;
	if (ent->v.health <= 0.f)
		return false;
	if ((int) ent->v.takedamage == DAMAGE_NO)
		return false;
	if (((int) ent->v.flags & FL_MONSTER) == 0)
		return false;

	classname = BotAI_TryGetString ((int) ent->v.classname);
	if (classname && BotAI_HasPrefix (classname, "monster_"))
		return true;

	return true;
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

			classname = BotAI_TryGetString ((int) ent->v.classname);
			if (!classname)
				continue;
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

	if (BotAI_ShouldUseNav2 ())
	{
		int from = BotNav_FindNearestNode (self->v.origin);
		int to = BotNav_FindNearestNode (item->v.origin);
		bot_path_t path;
		vec3_t to_pos;
		trace_t item_link_tr;

		if (from < 0 || to < 0)
			return false;
		if (!BotNav_GetNodePosition (to, to_pos))
			return false;
		VectorCopy (to_pos, start);
		start[2] += 18.f;
		VectorCopy (item->v.origin, end);
		end[2] += 18.f;
		item_link_tr = SV_Move (start, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, self);
		if (item_link_tr.startsolid || item_link_tr.fraction < 0.9f)
			return false;
		if (from == to)
		{
			VectorAdd (self->v.origin, self->v.view_ofs, start);
			VectorCopy (item->v.origin, end);
			tr = SV_Move (start, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, self);
			return tr.fraction >= 0.98f;
		}
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
			if (!BotCombat_HasAnyRangedAmmo (self))
				score += has_weapon ? 15.f : 90.f;
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
		if (!BotCombat_HasAnyRangedAmmo (self))
			score += 120.f;
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
	if (bot->state == BOT_STATE_RETREAT && item_type == BOT_ITEM_AMMO)
		score += 70.f;
	if (bot->state == BOT_STATE_RETREAT && item_type == BOT_ITEM_WEAPON)
		score += 40.f;

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
		if (bot->failed_goal_item == item && qcvm->time < bot->failed_goal_item_until)
			continue;
		if (BotAI_ShouldUseNav2 ())
		{
			int item_node = BotNav_FindNearestNode (item->v.origin);
			if (BotAI_IsFailedGoalNode (bot, item_node))
				continue;
		}

		classname = BotAI_TryGetString ((int) item->v.classname);
		if (!classname)
			continue;
		if (BotAI_ClassifyItem (classname) == BOT_ITEM_NONE)
			continue;

		VectorSubtract (item->v.origin, self->v.origin, delta);
		dist = VectorLength (delta);
		if (dist > 2200.f)
			continue;

		score = BotAI_ItemScore (bot, self, item, classname, enemy_visible, dist);
		if (!BotAI_ItemPotentiallyReachable (self, item))
			continue;
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
	qboolean companion_mode;

	*out_visible = false;
	companion_mode = BotAI_IsCompanionMode (bot);

	for (i = 0; i < svs.maxclients; ++i)
	{
		client_t *client = &svs.clients[i];
		edict_t *candidate;
		vec3_t delta;
		float dist;
		float score;
		qboolean visible;

		if (!client->active || !client->spawned)
			continue;
		if (i == bot->clientnum)
			continue;

		candidate = client->edict;
		if (!BotAI_IsAlivePlayer (candidate))
			continue;
		if (BotAI_IsTeammate (self, candidate))
			continue;
		if (companion_mode)
			continue;
		VectorSubtract (candidate->v.origin, self->v.origin, delta);
		dist = VectorLength (delta);
		if (dist > 2500.f)
			continue;
		visible = BotAI_CanSee (self, candidate);
		if (!visible && dist > 900.f)
			continue;
		if (!visible && BotAI_ShouldUseNav2 () && !BotAI_CanPathToPos (bot, self, candidate->v.origin, true))
			continue;

		score = 2600.f - dist;
		score += candidate->v.frags * 2.f;
		if (visible)
			score += 250.f;
		else
			score -= 180.f;
		if (score > best_score)
		{
			best_score = score;
			best = candidate;
			*out_visible = visible;
		}
	}

	if (companion_mode)
	{
		for (i = svs.maxclients + 1; i < qcvm->num_edicts; ++i)
		{
			edict_t *candidate = EDICT_NUM (i);
			vec3_t delta;
			float dist;
			float score;
			qboolean visible;

			if (!BotAI_IsMonsterEnemyCandidate (candidate))
				continue;

			VectorSubtract (candidate->v.origin, self->v.origin, delta);
			dist = VectorLength (delta);
			if (dist > 2500.f)
				continue;
			visible = BotAI_CanSee (self, candidate);
			if (!visible && dist > 900.f)
				continue;
			if (!visible && BotAI_ShouldUseNav2 () && !BotAI_CanPathToPos (bot, self, candidate->v.origin, true))
				continue;

			score = 2300.f - dist;
			if (visible)
				score += 200.f;
			else
				score -= 150.f;
			if (score > best_score)
			{
				best_score = score;
				best = candidate;
				*out_visible = visible;
			}
		}
	}

	return best;
}

static edict_t *BotAI_FindThreatNearPoint (bot_state_t *bot, edict_t *self, edict_t *leader, const vec3_t focus, qboolean *out_visible)
{
	int i;
	float best_score = -1.f;
	edict_t *best = NULL;

	if (out_visible)
		*out_visible = false;
	if (!bot || !self)
		return NULL;

	for (i = svs.maxclients + 1; i < qcvm->num_edicts; ++i)
	{
		edict_t *candidate = EDICT_NUM (i);
		vec3_t to_focus;
		vec3_t to_self;
		float focus_dist;
		float self_dist;
		float score;
		qboolean visible;
		qboolean leader_visible;

		if (!BotAI_IsMonsterEnemyCandidate (candidate))
			continue;

		VectorSubtract (candidate->v.origin, focus, to_focus);
		focus_dist = VectorLength (to_focus);
		if (focus_dist > 1200.f)
			continue;

		VectorSubtract (candidate->v.origin, self->v.origin, to_self);
		self_dist = VectorLength (to_self);
		visible = BotAI_CanSee (self, candidate);
		leader_visible = leader ? BotAI_CanSee (leader, candidate) : false;
		if (!visible && !leader_visible && BotAI_ShouldUseNav2 () && !BotAI_CanPathToPos (bot, self, candidate->v.origin, true))
			continue;

		score = 1500.f - focus_dist - self_dist * 0.2f;
		if (visible)
			score += 250.f;
		else if (leader_visible)
			score += 125.f;

		if (score > best_score)
		{
			best_score = score;
			best = candidate;
			if (out_visible)
				*out_visible = visible;
		}
	}

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
	case BOT_STATE_FOLLOW:
		return "FOLLOW";
	case BOT_STATE_SEARCH:
		return "SEARCH";
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
	bot->failed_goal_item = NULL;
	bot->failed_goal_item_until = 0.0;
	bot->failed_goal_node = -1;
	bot->failed_goal_node_until = 0.0;
	bot->has_path = false;
	bot->path_index = 0;
	bot->next_repath_time = 0.0;
	VectorCopy (vec3_origin, bot->last_origin);
	bot->last_progress_time = now;
	bot->stuck_until = 0.0;
	bot->next_item_scan_time = 0.0;
	bot->next_debug_time = 0.0;
	bot->respawn_time = 0.0;
	bot->last_health = 0.f;
	bot->last_leader_health = 0.f;
	bot->alert_until = 0.0;
	VectorCopy (vec3_origin, bot->alert_pos);
	bot->strafe_dir = 1.f;
	bot->next_strafe_change = 0.0;
	bot->last_weapon_switch_time = 0.0;
	bot->last_requested_weapon = IT_AXE;
	bot->roam_point = -1;
	bot->retreat_hold_until = 0.0;
	bot->fire_block_streak = 0;
	bot->last_fire_block_time = 0.0;
	memset (bot->failed_edge_from, -1, sizeof (bot->failed_edge_from));
	memset (bot->failed_edge_to, -1, sizeof (bot->failed_edge_to));
	memset (bot->failed_edge_until, 0, sizeof (bot->failed_edge_until));
	bot->failed_edge_cursor = 0;
	bot->stuck_window_start = 0.0;
	bot->stuck_window_count = 0;
	bot->no_move_target_streak = 0;
	bot->obstacle_avoid_streak = 0;
	bot->dbg_stuck_events = 0;
	bot->dbg_repaths = 0;
	bot->dbg_blocked_shots = 0;
	bot->dbg_weapon_downgrades = 0;
	bot->follow_target = NULL;
	bot->follow_unreachable_since = 0.0;
	bot->next_follow_teleport_time = 0.0;
}

static void BotAI_SelectRoamGoal (bot_state_t *bot, edict_t *self)
{
	int nav_node_count;
	int index;

	nav_node_count = BotAI_ShouldUseNav2 () ? BotNav_NodeCount () : 0;
	if (self && nav_node_count > 0)
	{
		int start = BotNav_FindNearestNode (self->v.origin);
		int tries;

		if (start >= 0)
		{
			for (tries = 0; tries < 12; ++tries)
			{
				vec3_t candidate_pos;
				vec3_t delta;
				int candidate;
				bot_path_t path;

				candidate = ((int) (BotAI_Random01 (bot, 101u + (uint32_t) tries) * nav_node_count)) % nav_node_count;
				if (nav_node_count > 1 && candidate == start)
					continue;
				if (BotAI_IsFailedGoalNode (bot, candidate))
					continue;
				if (!BotNav_GetNodePosition (candidate, candidate_pos))
					continue;
				VectorSubtract (candidate_pos, self->v.origin, delta);
				if (tries < 8 && VectorLength (delta) < 192.f)
					continue;
				if (bot->stuck_window_count >= 3 && VectorLength (delta) < 360.f)
					continue;
				if (!BotNav_FindPath (start, candidate, &path) || path.count <= 1)
					continue;
				if (path.count > 1 && BotAI_IsFailedEdge (bot, path.nodes[0], path.nodes[1]))
					continue;

				VectorCopy (candidate_pos, bot->goal_pos);
				bot->goal_pos[2] += 20.f;
				bot->has_goal = true;
				bot->has_path = false;
				bot->next_repath_time = 0.0;
				bot->goal_item = NULL;
				bot->roam_point = candidate;
				return;
			}
		}
	}

	if (!g_roam_count)
		return;

	index = ((int) (BotAI_Random01 (bot, 11U) * g_roam_count)) % g_roam_count;
	if (g_roam_count > 1 && index == bot->roam_point)
		index = (index + 1) % g_roam_count;

	bot->roam_point = index;
	VectorCopy (g_roam_points[index], bot->goal_pos);
	bot->goal_pos[2] += 20.f;
	bot->has_goal = true;
	bot->goal_item = NULL;
	bot->has_path = false;
	if (self && BotAI_ShouldUseNav2 ())
		bot->next_repath_time = 0.0;
}

static void BotAI_SelectRetreatGoal (bot_state_t *bot, edict_t *self, edict_t *enemy)
{
	int nav_node_count;
	int start;
	int tries;
	float best_score = -999999.f;
	vec3_t best_pos;
	qboolean found = false;

	if (!bot || !self || !enemy)
		return;

	nav_node_count = BotAI_ShouldUseNav2 () ? BotNav_NodeCount () : 0;
	start = nav_node_count > 0 ? BotNav_FindNearestNode (self->v.origin) : -1;
	if (start >= 0)
	{
		for (tries = 0; tries < 40; ++tries)
		{
			int candidate = ((int) (BotAI_Random01 (bot, 330u + (uint32_t) tries) * nav_node_count)) % nav_node_count;
			bot_path_t path;
			vec3_t pos;
			vec3_t to_enemy;
			vec3_t to_self;
			float enemy_dist;
			float self_dist;
			float score;

			if (candidate == start && nav_node_count > 1)
				continue;
			if (BotAI_IsFailedGoalNode (bot, candidate))
				continue;
			if (!BotNav_FindPath (start, candidate, &path) || path.count <= 1)
				continue;
			if (BotAI_IsFailedEdge (bot, path.nodes[0], path.nodes[1]))
				continue;
			if (!BotNav_GetNodePosition (candidate, pos))
				continue;

			VectorSubtract (pos, enemy->v.origin, to_enemy);
			VectorSubtract (pos, self->v.origin, to_self);
			enemy_dist = VectorLength (to_enemy);
			self_dist = VectorLength (to_self);
			score = enemy_dist - self_dist * 0.35f;
			if (bot->stuck_window_count >= 3)
				score += enemy_dist * 0.25f;
			if (score > best_score)
			{
				best_score = score;
				VectorCopy (pos, best_pos);
				found = true;
			}
		}
	}

	if (found)
	{
		VectorCopy (best_pos, bot->goal_pos);
		bot->goal_pos[2] += 20.f;
		bot->has_goal = true;
		bot->goal_item = NULL;
		bot->has_path = false;
		bot->next_repath_time = 0.0;
		return;
	}

	{
		vec3_t away;
		VectorSubtract (self->v.origin, enemy->v.origin, away);
		away[2] = 0.f;
		if (VectorNormalize (away) > 0.f)
		{
			VectorMA (self->v.origin, 300.f, away, bot->goal_pos);
			bot->goal_pos[2] = self->v.origin[2];
			bot->has_goal = true;
		}
	}
}

static void BotAI_UpdatePath (bot_state_t *bot, edict_t *self)
{
	int start;
	int goal;
	vec3_t goal_node_pos;
	float goal_node_dist = 0.f;

	if (!bot->has_goal)
	{
		bot->has_path = false;
		return;
	}

	if (!BotAI_ShouldUseNav2 ())
	{
		bot->has_path = false;
		return;
	}

	if (qcvm->time < bot->next_repath_time)
		return;

	bot->next_repath_time = qcvm->time + 0.8;
	bot->dbg_repaths++;

	start = BotNav_FindNearestNode (self->v.origin);
	goal = BotNav_FindNearestNode (bot->goal_pos);
	if (goal >= 0 && BotNav_GetNodePosition (goal, goal_node_pos))
	{
		vec3_t d;
		VectorSubtract (bot->goal_pos, goal_node_pos, d);
		goal_node_dist = VectorLength (d);
	}

	if (start >= 0 && goal >= 0 && BotNav_FindPath (start, goal, &bot->path))
	{
		if (bot->path.count == 1 && goal_node_dist > 220.f)
		{
			bot->has_path = false;
			bot->next_repath_time = qcvm->time + 0.2;
			bot->failed_goal_node = goal;
			bot->failed_goal_node_until = qcvm->time + 4.0;
			if (bot->goal_item && !bot->goal_item->free)
			{
				bot->failed_goal_item = bot->goal_item;
				bot->failed_goal_item_until = qcvm->time + 4.0;
			}
			bot->goal_item = NULL;
			bot->has_goal = false;
			return;
		}
		bot->has_path = true;
		bot->path_index = 0;
	}
	else
	{
		bot->has_path = false;
	}
}

static qboolean BotAI_GetMoveTarget (bot_state_t *bot, edict_t *self, vec3_t out_target)
{
	if (bot->has_path)
	{
		if (bot->path.count == 1 && bot->has_goal)
		{
			VectorCopy (bot->goal_pos, out_target);
			return true;
		}

		while (bot->path_index < bot->path.count)
		{
			trace_t tr;
			vec3_t node_pos;
			vec3_t delta;
			int from_node;
			int to_node;

			if (!BotNav_GetNodePosition (bot->path.nodes[bot->path_index], node_pos))
				break;
			from_node = bot->path.nodes[q_max (0, bot->path_index - 1)];
			to_node = bot->path.nodes[bot->path_index];
			if (BotAI_IsFailedEdge (bot, from_node, to_node))
			{
				bot->has_path = false;
				bot->next_repath_time = 0.0;
				return false;
			}

			VectorSubtract (node_pos, self->v.origin, delta);
			if (VectorLength (delta) < 64.f && bot->path_index < bot->path.count - 1)
			{
				bot->path_index++;
				continue;
			}

			tr = SV_Move (self->v.origin, self->v.mins, self->v.maxs, node_pos, MOVE_NOMONSTERS, self);
			if (tr.startsolid || tr.fraction < 0.55f)
			{
				if (tr.startsolid)
				{
					BotAI_RegisterFailedEdge (bot, from_node, to_node);
					bot->has_path = false;
					bot->next_repath_time = 0.0;
					return false;
				}
			}

			VectorCopy (node_pos, out_target);
			return true;
		}

		bot->has_path = false;
	}

	if (!bot->has_goal)
		return false;

	if (BotAI_ShouldUseNav2 ())
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

static qboolean BotAI_TestMoveBlocked (edict_t *self, const vec3_t move_dir, float dist, trace_t *out_tr)
{
	vec3_t end;
	trace_t tr;

	if (!self)
		return true;

	VectorMA (self->v.origin, dist, move_dir, end);
	tr = SV_Move (self->v.origin, self->v.mins, self->v.maxs, end, MOVE_NOMONSTERS, self);
	if (out_tr)
		*out_tr = tr;

	return tr.startsolid || tr.fraction < 0.2f;
}

static qboolean BotAI_ShouldJumpForward (bot_state_t *bot, edict_t *self, const vec3_t move_dir)
{
	trace_t tr_wall;
	trace_t tr_jump;
	vec3_t jump_start;
	vec3_t jump_end;

	if (!bot || !self)
		return false;
	if (!((int) self->v.flags & FL_ONGROUND))
		return false;
	if (self->v.waterlevel >= 2.f)
		return false;
	if (qcvm->time < bot->next_jump_time)
		return false;
	if (!BotAI_TestMoveBlocked (self, move_dir, 30.f, &tr_wall))
		return false;
	if (tr_wall.startsolid)
		return false;
	if (fabsf (tr_wall.plane.normal[2]) > 0.6f)
		return false;

	VectorCopy (self->v.origin, jump_start);
	jump_start[2] += 24.f;
	VectorMA (jump_start, 44.f, move_dir, jump_end);
	tr_jump = SV_Move (jump_start, self->v.mins, self->v.maxs, jump_end, MOVE_NOMONSTERS, self);

	return !tr_jump.startsolid && tr_jump.fraction >= 0.95f;
}

static qboolean BotAI_FindLocalEscapeTarget (bot_state_t *bot, edict_t *self, vec3_t out_target)
{
	static const float yaw_offsets[] = {0.f, 30.f, -30.f, 60.f, -60.f, 90.f, -90.f, 120.f, -120.f, 150.f, -150.f, 180.f};
	vec3_t goal_dir = {0.f, 0.f, 0.f};
	float best_score = -9999.f;
	qboolean found = false;
	int i;

	if (!bot || !self || !out_target)
		return false;

	if (bot->has_goal)
	{
		VectorSubtract (bot->goal_pos, self->v.origin, goal_dir);
		goal_dir[2] = 0.f;
		VectorNormalize (goal_dir);
	}

	for (i = 0; i < (int) countof (yaw_offsets); ++i)
	{
		vec3_t dir;
		vec3_t end;
		vec3_t ang = {0.f, 0.f, 0.f};
		vec3_t candidate;
		trace_t tr;
		float score;

		ang[YAW] = self->v.v_angle[YAW] + yaw_offsets[i];
		AngleVectors (ang, dir, NULL, NULL);
		dir[2] = 0.f;
		if (VectorNormalize (dir) <= 0.f)
			continue;

		VectorMA (self->v.origin, 110.f, dir, end);
		tr = SV_Move (self->v.origin, self->v.mins, self->v.maxs, end, MOVE_NOMONSTERS, self);
		if (tr.startsolid)
			continue;

		score = tr.fraction;
		if (goal_dir[0] != 0.f || goal_dir[1] != 0.f)
		{
			float align = DotProduct (dir, goal_dir);
			if (align > 0.f)
				score += 0.3f * align;
		}

		if (score <= best_score)
			continue;

		VectorMA (self->v.origin, 36.f + 64.f * tr.fraction, dir, candidate);
		VectorCopy (candidate, out_target);
		best_score = score;
		found = true;
	}

	return found;
}

static void BotAI_SetGoalFromItem (bot_state_t *bot, edict_t *item)
{
	vec3_t goal;

	if (!bot || !item)
		return;

	VectorCopy (item->v.origin, goal);
	goal[2] += 16.f;

	if (BotAI_ShouldUseNav2 ())
	{
		int item_node = BotNav_FindNearestNode (item->v.origin);
		vec3_t node_pos;

		if (item_node >= 0 && BotNav_GetNodePosition (item_node, node_pos))
		{
			VectorCopy (node_pos, goal);
			goal[2] += 20.f;
		}
	}

	VectorCopy (goal, bot->goal_pos);
}

static void BotAI_SnapGoalToNavNode (bot_state_t *bot, const vec3_t wanted, vec3_t out_goal)
{
	int node;
	vec3_t node_pos;

	VectorCopy (wanted, out_goal);
	if (!bot || !BotAI_ShouldUseNav2 ())
		return;

	node = BotNav_FindNearestNode (wanted);
	if (node < 0 || BotAI_IsFailedGoalNode (bot, node))
		return;
	if (BotNav_GetNodePosition (node, node_pos))
		VectorCopy (node_pos, out_goal);
}

static qboolean BotAI_AdjustForImmediateObstacle (bot_state_t *bot, edict_t *self, vec3_t move_dir)
{
	trace_t tr;
	float probe_dist = 56.f;
	qboolean adjusted = false;

	if (!bot || !self)
		return false;

	if (!BotAI_TestMoveBlocked (self, move_dir, probe_dist, &tr))
		return false;

	if (!tr.startsolid && tr.fraction < 1.f && fabsf (tr.plane.normal[2]) < 0.7f)
	{
		vec3_t slide;
		float d = DotProduct (move_dir, tr.plane.normal);
		VectorMA (move_dir, -d, tr.plane.normal, slide);
		slide[2] = 0.f;
		if (VectorNormalize (slide) > 0.2f)
		{
			VectorCopy (slide, move_dir);
			if (!BotAI_TestMoveBlocked (self, move_dir, probe_dist, NULL))
				adjusted = true;
		}
	}

	if (!adjusted)
	{
		vec3_t side;
		side[0] = -move_dir[1] * bot->strafe_dir;
		side[1] = move_dir[0] * bot->strafe_dir;
		side[2] = 0.f;
		if (VectorNormalize (side) > 0.f)
		{
			VectorCopy (side, move_dir);
			if (!BotAI_TestMoveBlocked (self, move_dir, probe_dist, NULL))
				adjusted = true;
			else
			{
				bot->strafe_dir = -bot->strafe_dir;
				side[0] = -move_dir[1] * bot->strafe_dir;
				side[1] = move_dir[0] * bot->strafe_dir;
				side[2] = 0.f;
				if (VectorNormalize (side) > 0.f)
				{
					VectorCopy (side, move_dir);
					if (!BotAI_TestMoveBlocked (self, move_dir, probe_dist, NULL))
						adjusted = true;
				}
			}
		}
	}

	if (!adjusted)
	{
		vec3_t back;
		back[0] = -move_dir[0];
		back[1] = -move_dir[1];
		back[2] = 0.f;
		if (VectorNormalize (back) > 0.f)
		{
			VectorCopy (back, move_dir);
			adjusted = !BotAI_TestMoveBlocked (self, move_dir, probe_dist, NULL);
		}
	}

	(void) bot;
	return adjusted;
}

static void BotAI_TeleportNearLeader (bot_state_t *bot, edict_t *self, edict_t *leader)
{
	int i;
	vec3_t base;

	if (!bot || !self || !leader)
		return;

	VectorCopy (leader->v.origin, base);

	for (i = 0; i < 8; ++i)
	{
		float ang = (360.f / 8.f) * i;
		vec3_t angv = {0.f, 0.f, 0.f};
		vec3_t dir;
		vec3_t target;
		trace_t tr;

		angv[YAW] = ang;
		AngleVectors (angv, dir, NULL, NULL);
		dir[2] = 0.f;
		VectorMA (base, 72.f + 20.f * i, dir, target);
		target[2] += 24.f;

		tr = SV_Move (target, self->v.mins, self->v.maxs, target, MOVE_NORMAL, self);
		if (tr.startsolid)
			continue;

		VectorCopy (target, self->v.origin);
		VectorCopy (vec3_origin, self->v.velocity);
		SV_LinkEdict (self, true);
		bot->follow_unreachable_since = 0.0;
		bot->next_follow_teleport_time = qcvm->time + 4.0;
		return;
	}
}

void BotAI_BuildCommand (bot_state_t *bot, client_t *client, usercmd_t *outcmd, vec3_t out_vangle, qboolean *out_attack, int *out_impulse)
{
	edict_t *self;
	edict_t *visible_enemy;
	edict_t *best_item = NULL;
	qboolean enemy_visible = false;
	qboolean no_combat_ammo = false;
	qboolean should_retreat = false;
	qboolean can_exit_retreat = false;
	qboolean companion_mode = false;
	vec3_t move_target;
	vec3_t move_dir;
	vec3_t nav_move_dir;
	vec3_t move_angles;
	float move_speed = 0.f;
	float weapon_eval_dist = 0.f;
	qboolean has_move_target;
	int desired_weapon = IT_AXE;
	int current_weapon = IT_AXE;
	qboolean keep_attack_aim = false;
	edict_t *leader = NULL;
	float leader_dist = 0.f;
	qboolean leader_moving = false;
	qboolean companion_alerted = false;
	qboolean alert_enemy_visible = false;
	qboolean hold_position = false;

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
		bot->last_health = 0.f;
		bot->last_leader_health = 0.f;
		bot->alert_until = 0.0;
		return;
	}

	if (bot_nav_debug.value)
		BotNav_DebugDraw ();

	no_combat_ammo = !BotCombat_HasAnyRangedAmmo (self);
	companion_mode = BotAI_IsCompanionMode (bot);
	leader = BotAI_FindLeaderPlayer (bot, self);
	bot->follow_target = leader;
	if (leader)
	{
		vec3_t to_leader;
		VectorSubtract (leader->v.origin, self->v.origin, to_leader);
		leader_dist = VectorLength (to_leader);
		leader_moving = BotAI_IsEntityMoving (leader, 35.f);
	}
	else
	{
		bot->last_leader_health = 0.f;
	}

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

	companion_alerted = companion_mode && leader && BotAI_UpdateCompanionAlert (bot, self, leader);
	if (companion_alerted && !bot->enemy)
	{
		edict_t *alert_enemy = BotAI_FindThreatNearPoint (bot, self, leader, bot->alert_pos, &alert_enemy_visible);
		if (alert_enemy)
		{
			bot->enemy = alert_enemy;
			VectorCopy (alert_enemy->v.origin, bot->enemy_last_pos);
			bot->enemy_last_seen_time = qcvm->time;
			if (alert_enemy_visible)
				enemy_visible = true;
		}
	}

	if (qcvm->time >= bot->next_item_scan_time)
	{
		bot->next_item_scan_time = qcvm->time + 0.4;
		best_item = NULL;
		if (!companion_mode || self->v.health < 40.f || no_combat_ammo)
			best_item = BotAI_FindBestItem (bot, self, enemy_visible);
		if (best_item)
			{
				bot->goal_item = best_item;
				BotAI_SetGoalFromItem (bot, best_item);
				bot->has_goal = true;
				bot->goal_timeout = qcvm->time + ((bot->state == BOT_STATE_RETREAT) ? 3.5 : 8.0);
			}
		else if (companion_mode && bot->goal_item)
		{
			bot->goal_item = NULL;
			bot->has_goal = false;
			bot->has_path = false;
		}
		}

	if (bot->goal_item)
	{
		if (bot->goal_item->free || (int) bot->goal_item->v.solid == SOLID_NOT || qcvm->time > bot->goal_timeout || !BotAI_ItemPotentiallyReachable (self, bot->goal_item))
		{
			if (!bot->goal_item->free)
			{
				bot->failed_goal_item = bot->goal_item;
				bot->failed_goal_item_until = qcvm->time + 5.0;
			}
			bot->goal_item = NULL;
			bot->has_goal = false;
			bot->has_path = false;
		}
		else
		{
			BotAI_SetGoalFromItem (bot, bot->goal_item);
		}
	}

	should_retreat = (self->v.health < 40.f || no_combat_ammo);
	can_exit_retreat = (self->v.health > 55.f && !no_combat_ammo);

	if (qcvm->time < bot->stuck_until)
		bot->state = BOT_STATE_STUCK_RECOVERY;
	else if (enemy_visible && bot->enemy)
	{
		if (should_retreat || qcvm->time < bot->retreat_hold_until)
		{
			bot->state = BOT_STATE_RETREAT;
			bot->retreat_hold_until = q_max (bot->retreat_hold_until, qcvm->time + 1.2);
		}
		else
			bot->state = BOT_STATE_ATTACK;
	}
	else if (bot->enemy && (qcvm->time - bot->enemy_last_seen_time) <= BOT_ENEMY_FORGET_TIME)
	{
		vec3_t chase_goal;

		BotAI_SnapGoalToNavNode (bot, bot->enemy_last_pos, chase_goal);
		if (!can_exit_retreat || qcvm->time < bot->retreat_hold_until)
		{
			bot->state = BOT_STATE_RETREAT;
			bot->retreat_hold_until = q_max (bot->retreat_hold_until, qcvm->time + 0.9);
		}
		else if (companion_mode)
			bot->state = BOT_STATE_SEARCH;
		else
			bot->state = BOT_STATE_CHASE_ENEMY;
		if (BotAI_CanPathToPos (bot, self, chase_goal, true))
		{
			bot->has_goal = true;
			VectorCopy (chase_goal, bot->goal_pos);
			bot->goal_pos[2] += 20.f;
		}
		else
		{
			bot->has_goal = false;
			bot->has_path = false;
			if (qcvm->time - bot->enemy_last_seen_time > 0.6)
				bot->enemy = NULL;
		}
	}
	else if (bot->has_goal)
		bot->state = BOT_STATE_SEEK_ITEM;
	else
		bot->state = BOT_STATE_ROAM;

	if (companion_mode && leader && !bot->enemy)
	{
		if (qcvm->time < bot->alert_until)
		{
			vec3_t search_goal;
			bot->state = BOT_STATE_SEARCH;
			BotAI_SnapGoalToNavNode (bot, bot->alert_pos, search_goal);
			VectorCopy (search_goal, bot->goal_pos);
			bot->goal_pos[2] += 20.f;
			if (BotAI_CanPathToPos (bot, self, bot->goal_pos, true))
			{
				bot->has_goal = true;
				bot->goal_item = NULL;
				bot->goal_timeout = qcvm->time + 1.5;
			}
			else
			{
				bot->has_goal = false;
				bot->has_path = false;
			}
		}
		else
		{
			float follow_dist = leader_moving ? 120.f : 180.f;

			bot->state = BOT_STATE_FOLLOW;
			if (leader_dist > follow_dist)
			{
				vec3_t follow_goal;

				VectorCopy (leader->v.origin, follow_goal);
				if (leader_moving)
				{
					vec3_t trail_dir;
					VectorCopy (leader->v.velocity, trail_dir);
					trail_dir[2] = 0.f;
					if (VectorNormalize (trail_dir) > 0.f)
						VectorMA (follow_goal, -72.f, trail_dir, follow_goal);
				}

				BotAI_SnapGoalToNavNode (bot, follow_goal, bot->goal_pos);
				bot->goal_pos[2] += 20.f;
				bot->has_goal = true;
				bot->goal_item = NULL;
				bot->goal_timeout = qcvm->time + 1.2;
			}
			else
			{
				bot->has_goal = false;
				bot->has_path = false;
				hold_position = true;
			}
		}
	}

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
		if (qcvm->time > bot->goal_timeout || !BotAI_ItemPotentiallyReachable (self, bot->goal_item))
		{
			bot->goal_item = NULL;
			bot->has_goal = false;
			bot->has_path = false;
		}
	}
	else if (bot->state == BOT_STATE_RETREAT && bot->enemy)
	{
		BotAI_SelectRetreatGoal (bot, self, bot->enemy);
	}

	if (bot->has_goal)
		BotAI_UpdatePath (bot, self);

	if (bot->enemy)
	{
		vec3_t enemy_delta;
		VectorSubtract (bot->enemy->v.origin, self->v.origin, enemy_delta);
		weapon_eval_dist = VectorLength (enemy_delta);
	}
	desired_weapon = BotCombat_SelectWeapon (self, weapon_eval_dist, enemy_visible);
	current_weapon = (int) self->v.weapon;
	if (desired_weapon != current_weapon &&
		(qcvm->time - bot->last_weapon_switch_time > 0.7 || !BotCombat_HasAmmoForWeapon (self, current_weapon)))
	{
		*out_impulse = BotCombat_WeaponImpulse (desired_weapon);
		bot->last_weapon_switch_time = qcvm->time;
		bot->last_requested_weapon = desired_weapon;
	}

	has_move_target = BotAI_GetMoveTarget (bot, self, move_target);
	if (has_move_target || hold_position || (bot->state == BOT_STATE_FOLLOW && !bot->has_goal))
		bot->no_move_target_streak = 0;
	else if (BotAI_ShouldUseNav2 ())
	{
		bot->no_move_target_streak++;

		if (bot->no_move_target_streak >= 2)
		{
			if (bot->goal_item)
			{
				bot->goal_item = NULL;
				bot->has_goal = false;
				bot->has_path = false;
			}

			if (bot->state == BOT_STATE_RETREAT && bot->enemy)
				BotAI_SelectRetreatGoal (bot, self, bot->enemy);
			else if (companion_mode && (bot->state == BOT_STATE_FOLLOW || bot->state == BOT_STATE_SEARCH))
			{
				bot->has_path = false;
				bot->next_repath_time = 0.0;
			}
			else if (!bot->has_goal || bot->no_move_target_streak >= 4)
			{
				bot->has_goal = false;
				bot->has_path = false;
				bot->roam_point = -1;
				BotAI_SelectRoamGoal (bot, self);
			}

			if (bot->has_goal)
			{
				BotAI_UpdatePath (bot, self);
				has_move_target = BotAI_GetMoveTarget (bot, self, move_target);
			}
		}

		if (!has_move_target && !BotAI_ShouldUseNav2 ())
		{
			if (BotAI_FindLocalEscapeTarget (bot, self, move_target))
				has_move_target = true;
		}
		else if (!has_move_target)
		{
			bot->stuck_until = q_max (bot->stuck_until, qcvm->time + 0.25);
		}

		if (!has_move_target && bot->has_goal && (companion_mode || bot->no_move_target_streak >= 8))
		{
			VectorCopy (bot->goal_pos, move_target);
			has_move_target = true;
		}

		if (has_move_target)
			bot->no_move_target_streak = 0;
	}
	if (!has_move_target)
		VectorCopy (self->v.origin, move_target);

	if (companion_mode && leader && leader_dist > 900.f)
	{
		if (bot->follow_unreachable_since <= 0.0)
			bot->follow_unreachable_since = qcvm->time;
		else if (qcvm->time - bot->follow_unreachable_since > 5.0 && qcvm->time >= bot->next_follow_teleport_time)
			BotAI_TeleportNearLeader (bot, self, leader);
	}
	else
		bot->follow_unreachable_since = 0.0;

	VectorSubtract (move_target, self->v.origin, move_dir);
	move_dir[2] = 0.f;
	if (VectorNormalize (move_dir) <= 0.f)
	{
		AngleVectors (out_vangle, move_dir, NULL, NULL);
		move_dir[2] = 0.f;
		if (VectorNormalize (move_dir) <= 0.f)
		{
			move_dir[0] = 1.f;
			move_dir[1] = 0.f;
			move_dir[2] = 0.f;
		}
	}
	VectorCopy (move_dir, nav_move_dir);

	VectorAngles (move_dir, move_angles);
	out_vangle[YAW] = BotAI_ApproachAngle (self->v.v_angle[YAW], move_angles[YAW], 540.f * host_frametime);
	out_vangle[PITCH] = BotAI_ApproachAngle (self->v.v_angle[PITCH], 0.f, 360.f * host_frametime);
	out_vangle[ROLL] = 0.f;
	if (hold_position && leader)
	{
		out_vangle[YAW] = BotAI_ApproachAngle (self->v.v_angle[YAW], leader->v.v_angle[YAW], 360.f * host_frametime);
		out_vangle[PITCH] = 0.f;
	}

	switch (bot->state)
	{
	case BOT_STATE_ATTACK:
		if (bot->enemy)
		{
			vec3_t enemy_dir;
			vec3_t strafe;
			float dist;
			vec3_t aim_angles;
			qboolean blocked_fire = false;
			qboolean can_fire;
			int fire_weapon = desired_weapon;

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

			VectorCopy (nav_move_dir, move_dir);
			move_speed = 300.f;
			if (dist < 700.f && !BotAI_TestMoveBlocked (self, enemy_dir, 72.f, NULL))
			{
				if (dist < 170.f)
					VectorMA (strafe, -0.8f, enemy_dir, move_dir);
				else if (dist > 450.f)
					VectorMA (strafe, 0.8f, enemy_dir, move_dir);
				else
					VectorCopy (strafe, move_dir);
				VectorNormalize (move_dir);
				if (BotAI_TestMoveBlocked (self, move_dir, 72.f, NULL))
					VectorCopy (nav_move_dir, move_dir);
			}

			BotCombat_ComputeAim (bot, self, bot->enemy, aim_angles);
			out_vangle[YAW] = BotAI_ApproachAngle (out_vangle[YAW], aim_angles[YAW], 720.f * host_frametime);
			out_vangle[PITCH] = BotAI_ApproachAngle (out_vangle[PITCH], aim_angles[PITCH], 540.f * host_frametime);
			keep_attack_aim = true;

			if (*out_impulse != 0)
				fire_weapon = current_weapon;
			can_fire = BotCombat_ShouldFire (self, bot->enemy, fire_weapon, self->v.v_angle, &blocked_fire);
			if (can_fire)
			{
				*out_attack = true;
				bot->fire_block_streak = 0;
			}
			else
			{
				if (blocked_fire)
				{
					bot->dbg_blocked_shots++;
					bot->fire_block_streak++;
					bot->last_fire_block_time = qcvm->time;
					move_speed = 320.f;
					VectorMA (strafe, 1.0f, enemy_dir, move_dir);
					VectorNormalize (move_dir);
					if (BotAI_TestMoveBlocked (self, move_dir, 72.f, NULL))
						VectorCopy (nav_move_dir, move_dir);
				}
				else if (qcvm->time - bot->last_fire_block_time > 0.6)
				{
					bot->fire_block_streak = 0;
				}

				if (bot->fire_block_streak >= 3 && qcvm->time - bot->last_weapon_switch_time > 0.25)
				{
					int fallback_weapon = BotCombat_SelectWeaponAvoid (self, dist, enemy_visible, fire_weapon);
					if (fallback_weapon != fire_weapon && fallback_weapon != current_weapon)
					{
						*out_impulse = BotCombat_WeaponImpulse (fallback_weapon);
						bot->last_weapon_switch_time = qcvm->time;
						bot->last_requested_weapon = fallback_weapon;
						bot->dbg_weapon_downgrades++;
					}
					bot->fire_block_streak = 0;
				}
			}
		}
		else
			move_speed = 260.f;
		break;

	case BOT_STATE_RETREAT:
		move_speed = 320.f;
		break;

	case BOT_STATE_STUCK_RECOVERY:
		move_speed = 180.f;
		bot->strafe_dir = (BotAI_Random01 (bot, 23U) > 0.5f ? 1.f : -1.f);
		outcmd->forwardmove = -120.f;
		outcmd->sidemove = bot->strafe_dir * 260.f;
		break;

	case BOT_STATE_SEARCH:
		move_speed = 250.f;
		break;

	case BOT_STATE_CHASE_ENEMY:
		move_speed = 310.f;
		break;

	case BOT_STATE_FOLLOW:
		move_speed = hold_position ? 0.f : (leader_moving ? 250.f : 210.f);
		break;

	case BOT_STATE_SEEK_ITEM:
		move_speed = 300.f;
		break;

	case BOT_STATE_ROAM:
	default:
		move_speed = 290.f;
		break;
	}

	if (bot->state != BOT_STATE_STUCK_RECOVERY && !hold_position)
	{
		qboolean avoided_wall = BotAI_AdjustForImmediateObstacle (bot, self, move_dir);
		if (avoided_wall)
		{
			bot->obstacle_avoid_streak++;
			move_speed = q_max (move_speed, 285.f);
			if (!keep_attack_aim)
			{
				VectorAngles (move_dir, move_angles);
				out_vangle[YAW] = BotAI_ApproachAngle (out_vangle[YAW], move_angles[YAW], 720.f * host_frametime);
			}
			if (bot->obstacle_avoid_streak >= 18)
			{
				bot->stuck_until = qcvm->time + 0.75;
				bot->dbg_stuck_events++;
				if (BotAI_ShouldUseNav2 ())
				{
					bot->failed_goal_node = BotNav_FindNearestNode (bot->goal_pos);
					bot->failed_goal_node_until = qcvm->time + 4.0;
				}
				if (bot->goal_item && !bot->goal_item->free)
				{
					bot->failed_goal_item = bot->goal_item;
					bot->failed_goal_item_until = qcvm->time + 4.0;
				}
				bot->goal_item = NULL;
				bot->has_goal = false;
				bot->has_path = false;
				bot->roam_point = -1;
				bot->next_repath_time = 0.0;
				bot->obstacle_avoid_streak = 0;
			}
		}
		else
			bot->obstacle_avoid_streak = 0;

		BotAI_ComputeDirectionalMove (out_vangle, move_dir, move_speed, outcmd);

		if (BotAI_ShouldJumpForward (bot, self, move_dir))
		{
			outcmd->upmove = 320.f;
			bot->next_jump_time = qcvm->time + 0.45;
		}
		else
			outcmd->upmove = 0.f;
	}
	else if (hold_position)
	{
		outcmd->forwardmove = 0.f;
		outcmd->sidemove = 0.f;
		outcmd->upmove = 0.f;
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
				if (qcvm->time - bot->stuck_window_start > 8.0)
					bot->stuck_window_count = 0;
			}
			else if (cmd_mag > 80.f && (qcvm->time - bot->last_progress_time) > 1.25)
			{
				bot->stuck_until = qcvm->time + 0.75;
				bot->dbg_stuck_events++;
				if (bot->stuck_window_start <= 0.0 || qcvm->time - bot->stuck_window_start > 8.0)
				{
					bot->stuck_window_start = qcvm->time;
					bot->stuck_window_count = 1;
				}
				else
				{
					bot->stuck_window_count++;
				}
				bot->next_repath_time = 0.0;
				if (BotAI_ShouldUseNav2 ())
				{
					bot->failed_goal_node = BotNav_FindNearestNode (bot->goal_pos);
					bot->failed_goal_node_until = qcvm->time + 4.0;
				}
				if (bot->goal_item && !bot->goal_item->free)
				{
					bot->failed_goal_item = bot->goal_item;
					bot->failed_goal_item_until = qcvm->time + 4.0;
				}
				bot->goal_item = NULL;
				if (bot->has_path && bot->path_index < bot->path.count - 1)
				{
					int from = bot->path.nodes[bot->path_index];
					int to = bot->path.nodes[bot->path_index + 1];
					BotAI_RegisterFailedEdge (bot, from, to);
				}
				bot->has_path = false;
				if (!bot->has_goal)
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
			if (bot->goal_item && !bot->goal_item->free)
			{
				vec3_t item_delta;
				VectorSubtract (bot->goal_item->v.origin, self->v.origin, item_delta);
				if (VectorLength (item_delta) > 110.f)
				{
					bot->failed_goal_item = bot->goal_item;
					bot->failed_goal_item_until = qcvm->time + 5.0;
				}
			}
			bot->has_goal = false;
			bot->goal_item = NULL;
			bot->has_path = false;
			if (bot->state == BOT_STATE_SEARCH)
				bot->alert_until = q_min (bot->alert_until, qcvm->time);
		}
	}

		if (bot_think_debug.value && bot->next_debug_time <= qcvm->time)
		{
			const char *enemy_name = "none";
			if (bot->enemy)
			{
				const char *safe_name = BotAI_TryGetString ((int) bot->enemy->v.netname);
				if (safe_name && safe_name[0])
					enemy_name = safe_name;
			}
			Con_Printf (
				"BotThink: %s state=%s enemy=%s goal=%s path=%d/%d stuck=%0.2f dbg(stk=%d rp=%d blk=%d wd=%d nt=%d)\n",
				bot->name,
				BotAI_StateName (bot->state),
				enemy_name,
				bot->has_goal ? "yes" : "no",
				bot->path_index,
				bot->has_path ? bot->path.count : 0,
				(float) q_max (0.0, bot->stuck_until - qcvm->time),
				bot->dbg_stuck_events,
				bot->dbg_repaths,
				bot->dbg_blocked_shots,
				bot->dbg_weapon_downgrades,
				bot->no_move_target_streak
			);
		bot->next_debug_time = qcvm->time + 1.0;
	}
}
