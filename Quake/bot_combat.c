#include "quakedef.h"
#include "bot_main.h"
#include "bot_combat.h"

extern cvar_t bot_skill;
extern cvar_t bot_aim_debug;

static float BotCombat_ClampedSkill (void)
{
	return CLAMP (0.1f, bot_skill.value, 1.f);
}

static qboolean BotCombat_HasWeapon (edict_t *self, int weapon)
{
	return ((int) self->v.items & weapon) != 0;
}

static float BotCombat_Noise01 (uint32_t seed)
{
	seed ^= seed >> 16;
	seed *= 0x7feb352dU;
	seed ^= seed >> 15;
	seed *= 0x846ca68bU;
	seed ^= seed >> 16;
	return (float) (seed & 0xffffU) / 65535.f;
}

qboolean BotCombat_HasAmmoForWeapon (edict_t *self, int weapon)
{
	if (!self)
		return false;

	switch (weapon)
	{
	case IT_AXE:
		return true;
	case IT_SHOTGUN:
		return self->v.ammo_shells >= 1;
	case IT_SUPER_SHOTGUN:
		return self->v.ammo_shells >= 2;
	case IT_NAILGUN:
		return self->v.ammo_nails >= 1;
	case IT_SUPER_NAILGUN:
		return self->v.ammo_nails >= 2;
	case IT_GRENADE_LAUNCHER:
	case IT_ROCKET_LAUNCHER:
		return self->v.ammo_rockets >= 1;
	case IT_LIGHTNING:
		return self->v.ammo_cells >= 1;
	default:
		return false;
	}
}

qboolean BotCombat_HasAnyRangedAmmo (edict_t *self)
{
	if (!self)
		return false;
	return
		(BotCombat_HasWeapon (self, IT_SHOTGUN) && self->v.ammo_shells >= 1) ||
		(BotCombat_HasWeapon (self, IT_SUPER_SHOTGUN) && self->v.ammo_shells >= 2) ||
		(BotCombat_HasWeapon (self, IT_NAILGUN) && self->v.ammo_nails >= 1) ||
		(BotCombat_HasWeapon (self, IT_SUPER_NAILGUN) && self->v.ammo_nails >= 2) ||
		(BotCombat_HasWeapon (self, IT_GRENADE_LAUNCHER) && self->v.ammo_rockets >= 1) ||
		(BotCombat_HasWeapon (self, IT_ROCKET_LAUNCHER) && self->v.ammo_rockets >= 1) ||
		(BotCombat_HasWeapon (self, IT_LIGHTNING) && self->v.ammo_cells >= 1);
}

static float BotCombat_WeaponScore (edict_t *self, int weapon, float dist, qboolean line_of_sight)
{
	float score = -10000.f;

	if (!BotCombat_HasWeapon (self, weapon))
		return score;
	if (!BotCombat_HasAmmoForWeapon (self, weapon))
		return score;

	switch (weapon)
	{
	case IT_AXE:
		score = (dist < 80.f) ? 40.f : -200.f;
		break;

	case IT_SHOTGUN:
		score = 120.f - dist * 0.18f;
		break;

	case IT_SUPER_SHOTGUN:
		score = 170.f - dist * 0.24f;
		break;

	case IT_NAILGUN:
		score = 150.f - fabsf (dist - 500.f) * 0.15f;
		break;

	case IT_SUPER_NAILGUN:
		score = 180.f - fabsf (dist - 420.f) * 0.17f;
		break;

	case IT_GRENADE_LAUNCHER:
		score = (dist > 170.f && dist < 700.f) ? (130.f - fabsf (dist - 420.f) * 0.1f) : -120.f;
		break;

	case IT_ROCKET_LAUNCHER:
		score = (dist > 140.f && dist < 900.f) ? (250.f - fabsf (dist - 420.f) * 0.11f) : -150.f;
		break;

	case IT_LIGHTNING:
		score = line_of_sight ? (230.f - dist * 0.20f) : -180.f;
		break;

	default:
		break;
	}

	return score;
}

static float BotCombat_WeaponTierBonus (int weapon)
{
	switch (weapon)
	{
	case IT_LIGHTNING:
		return 32.f;
	case IT_ROCKET_LAUNCHER:
		return 28.f;
	case IT_SUPER_NAILGUN:
		return 24.f;
	case IT_GRENADE_LAUNCHER:
		return 20.f;
	case IT_NAILGUN:
		return 16.f;
	case IT_SUPER_SHOTGUN:
		return 12.f;
	case IT_SHOTGUN:
		return 8.f;
	default:
		return 0.f;
	}
}

int BotCombat_SelectWeaponAvoid (edict_t *self, float enemy_dist, qboolean line_of_sight, int avoid_weapon)
{
	static const int weapons[] = {IT_LIGHTNING, IT_ROCKET_LAUNCHER, IT_SUPER_NAILGUN, IT_GRENADE_LAUNCHER, IT_NAILGUN, IT_SUPER_SHOTGUN, IT_SHOTGUN, IT_AXE};
	int i;
	int best_weapon = IT_AXE;
	float best_score = -10000.f;

	if (!self)
		return IT_AXE;

	for (i = 0; i < (int) countof (weapons); ++i)
	{
		int weapon = weapons[i];
		float score;

		if (weapon == avoid_weapon)
			continue;
		score = BotCombat_WeaponScore (self, weapon, enemy_dist, line_of_sight);
		if (score <= -9000.f)
			continue;
		score += BotCombat_WeaponTierBonus (weapon);
		if (score > best_score)
		{
			best_score = score;
			best_weapon = weapon;
		}
	}

	return best_weapon;
}

int BotCombat_SelectWeapon (edict_t *self, float enemy_dist, qboolean line_of_sight)
{
	return BotCombat_SelectWeaponAvoid (self, enemy_dist, line_of_sight, 0);
}

int BotCombat_WeaponImpulse (int weapon)
{
	switch (weapon)
	{
	case IT_AXE:
		return 1;
	case IT_SHOTGUN:
		return 2;
	case IT_SUPER_SHOTGUN:
		return 3;
	case IT_NAILGUN:
		return 4;
	case IT_SUPER_NAILGUN:
		return 5;
	case IT_GRENADE_LAUNCHER:
		return 6;
	case IT_ROCKET_LAUNCHER:
		return 7;
	case IT_LIGHTNING:
		return 8;
	default:
		return 0;
	}
}

void BotCombat_ComputeAim (bot_state_t *bot, edict_t *self, edict_t *enemy, vec3_t out_angles)
{
	vec3_t target;
	vec3_t to_target;
	vec3_t base_angles;
	float dist;
	float bot_skill_value;
	float lead_time;
	float jitter;
	uint32_t seed;

	if (!self || !enemy)
	{
		VectorCopy (vec3_origin, out_angles);
		return;
	}

	bot_skill_value = BotCombat_ClampedSkill ();
	VectorAdd (enemy->v.origin, enemy->v.view_ofs, target);
	VectorSubtract (target, self->v.origin, to_target);
	dist = VectorLength (to_target);

	lead_time = CLAMP (0.f, dist / 1400.f, 0.22f) * (0.35f + 0.65f * bot_skill_value);
	target[0] += enemy->v.velocity[0] * lead_time;
	target[1] += enemy->v.velocity[1] * lead_time;
	target[2] += enemy->v.velocity[2] * (lead_time * 0.4f);

	VectorSubtract (target, self->v.origin, to_target);
	VectorAngles (to_target, base_angles);

	jitter = (1.f - bot_skill_value) * 5.5f;
	seed = (uint32_t) (host_framecount + bot->clientnum * 17 + (int) dist * 31);
	base_angles[YAW] += (BotCombat_Noise01 (seed) - 0.5f) * 2.f * jitter;
	base_angles[PITCH] += (BotCombat_Noise01 (seed ^ 0x9e3779b9U) - 0.5f) * 1.4f * jitter;

	base_angles[PITCH] = CLAMP (-70.f, base_angles[PITCH], 70.f);
	base_angles[ROLL] = 0.f;
	VectorCopy (base_angles, out_angles);

	if (bot_aim_debug.value >= 2.f && bot->next_debug_time < qcvm->time)
	{
		Con_Printf ("BotAim: %s dist=%0.0f pitch=%0.1f yaw=%0.1f\n", bot->name, dist, out_angles[PITCH], out_angles[YAW]);
		bot->next_debug_time = qcvm->time + 1.0;
	}
}

qboolean BotCombat_ShouldFire (edict_t *self, edict_t *enemy, int weapon, vec3_t aim_angles, qboolean *out_blocked)
{
	vec3_t start;
	vec3_t end;
	vec3_t dir;
	vec3_t forward;
	vec3_t right;
	trace_t tr;
	trace_t world_tr;
	float dist;
	float dot;

	if (out_blocked)
		*out_blocked = false;

	if (!self || !enemy)
		return false;
	if (!BotCombat_HasAmmoForWeapon (self, weapon))
		return false;

	VectorAdd (self->v.origin, self->v.view_ofs, start);
	VectorAdd (enemy->v.origin, enemy->v.view_ofs, end);
	VectorSubtract (end, start, dir);
	dist = VectorLength (dir);
	if (dist <= 1.f)
		return false;
	VectorScale (dir, 1.f / dist, dir);

	AngleVectors (aim_angles, forward, NULL, NULL);
	dot = DotProduct (forward, dir);
	if (dot < 0.95f)
		return false;

	world_tr = SV_Move (start, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, self);
	if (world_tr.startsolid || world_tr.fraction < 0.99f)
	{
		if (out_blocked)
			*out_blocked = true;
		return false;
	}

	tr = SV_Move (start, vec3_origin, vec3_origin, end, MOVE_NORMAL, self);
	if (tr.fraction < 1.f && tr.ent != enemy)
	{
		if (out_blocked)
			*out_blocked = true;
		return false;
	}

	if (weapon == IT_ROCKET_LAUNCHER || weapon == IT_GRENADE_LAUNCHER)
	{
		vec3_t near_end;
		vec3_t side_start;
		vec3_t side_end;
		trace_t near_tr;
		trace_t side_tr;

		if (dist < 170.f)
			return false;

		AngleVectors (aim_angles, forward, right, NULL);
		VectorMA (start, 96.f, forward, near_end);
		near_tr = SV_Move (start, vec3_origin, vec3_origin, near_end, MOVE_NORMAL, self);
		if (near_tr.fraction < 0.55f)
		{
			if (out_blocked)
				*out_blocked = true;
			return false;
		}

		VectorMA (start, 10.f, right, side_start);
		VectorMA (side_start, 80.f, forward, side_end);
		side_tr = SV_Move (side_start, vec3_origin, vec3_origin, side_end, MOVE_NORMAL, self);
		if (side_tr.fraction < 0.55f)
		{
			if (out_blocked)
				*out_blocked = true;
			return false;
		}

		VectorMA (start, -10.f, right, side_start);
		VectorMA (side_start, 80.f, forward, side_end);
		side_tr = SV_Move (side_start, vec3_origin, vec3_origin, side_end, MOVE_NORMAL, self);
		if (side_tr.fraction < 0.55f)
		{
			if (out_blocked)
				*out_blocked = true;
			return false;
		}
	}

	return true;
}
