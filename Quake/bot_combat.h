#ifndef BOT_COMBAT_H
#define BOT_COMBAT_H

#include "quakedef.h"

struct bot_state_s;

qboolean BotCombat_HasAmmoForWeapon (edict_t *self, int weapon);
qboolean BotCombat_HasAnyRangedAmmo (edict_t *self);
int BotCombat_SelectWeapon (edict_t *self, float enemy_dist, qboolean line_of_sight);
int BotCombat_SelectWeaponAvoid (edict_t *self, float enemy_dist, qboolean line_of_sight, int avoid_weapon);
int BotCombat_WeaponImpulse (int weapon);
void BotCombat_ComputeAim (struct bot_state_s *bot, edict_t *self, edict_t *enemy, vec3_t out_angles);
qboolean BotCombat_ShouldFire (edict_t *self, edict_t *enemy, int weapon, vec3_t aim_angles, qboolean *out_blocked);

#endif /* BOT_COMBAT_H */
