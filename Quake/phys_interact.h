#ifndef QUAKE_PHYS_INTERACT_H
#define QUAKE_PHYS_INTERACT_H

/*
==============================================================================

Lightweight interactive physics (engine-side)

This module is intentionally conservative:
- Opt-in only (per-entity)
- Singleplayer-first gameplay approximation
- Easy global disable (`sv_phys_interact`)
- No full rigid-body solver

==============================================================================
*/

void PhysInteract_Init (void);
void PhysInteract_Shutdown (void);

/* Called when a new server/map is spawned (after edicts are allocated). */
void PhysInteract_OnServerSpawned (void);

/* Edict lifetime hooks. */
void PhysInteract_OnEdictCleared (edict_t *ent);
void PhysInteract_OnEdictFreed (edict_t *ent);

/* Entity parse/spawn integration hooks. */
void PhysInteract_BeginParseEntity (edict_t *ent);
void PhysInteract_ParseEntityKey (edict_t *ent, const char *key, const char *value);
void PhysInteract_OnEntitySpawned (edict_t *ent);

/* Server frame hooks. */
void PhysInteract_BeginFrame (void);
void PhysInteract_Frame (void);

/* True when this entity should skip legacy movetype physics this frame. */
qboolean PhysInteract_ShouldHandleEntity (edict_t *ent, int entnum);

/* Player push contact hook (implemented in later milestones). */
void PhysInteract_PlayerContactPush (edict_t *player, edict_t *other, const vec3_t player_velocity, const vec3_t hit_normal);

#endif /* QUAKE_PHYS_INTERACT_H */
