/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// phys_interact.c -- lightweight engine-side interactive physics

#include <float.h>

#include "quakedef.h"
#include "phys_interact.h"

extern cvar_t sv_gravity;

typedef struct
{
	qboolean	physics_set;
	qboolean	physics_enabled;

	qboolean	mass_set;
	float		mass;

	qboolean	friction_set;
	float		friction;

	qboolean	restitution_set;
	float		restitution;

	qboolean	phys_type_set;
	char		phys_type[32];
} phys_interact_authored_t;

typedef struct
{
	qboolean	enabled;
	qboolean	initialized;
	qboolean	sleeping;
	qboolean	at_rest;

	float		mass;
	float		inv_mass;
	float		friction;
	float		restitution;

	vec3_t		velocity;
	vec3_t		angular_velocity;	// placeholder for later milestones
	vec3_t		pending_impulse;

	vec3_t		base_angles;
	vec3_t		visual_tilt;

	int			flags;
	float		sleep_time;
	float		support_fraction;
} phys_interact_body_t;

static phys_interact_authored_t	*phys_authored;
static phys_interact_body_t		*phys_bodies;
static int						*phys_active_indices;
static int						phys_capacity;
static int						phys_active_count;

// Optional high spawnflag for opt-in map usage without conflicting with stock bits.
#define SPAWNFLAG_PHYS_INTERACT	(1u << 24)

// world-only integration uses slide clipping epsilon similar to SV_FlyMove.
#define PHYS_STOP_EPSILON		0.1f
#define PHYS_MAX_BUMPS			4
#define PHYS_BODY_FLAG_HIT_FLOOR	(1u << 0)
#define PHYS_BODY_FLAG_WAS_ON_FLOOR	(1u << 1)

cvar_t sv_phys_interact = {"sv_phys_interact", "0", CVAR_NONE};
cvar_t sv_phys_gravity_scale = {"sv_phys_gravity_scale", "1", CVAR_NONE};
cvar_t sv_phys_friction = {"sv_phys_friction", "0.8", CVAR_NONE};
cvar_t sv_phys_restitution = {"sv_phys_restitution", "0.05", CVAR_NONE};
cvar_t sv_phys_sleep_epsilon = {"sv_phys_sleep_epsilon", "5", CVAR_NONE};
cvar_t sv_phys_debug = {"sv_phys_debug", "0", CVAR_NONE};
cvar_t sv_phys_debug_spawn = {"sv_phys_debug_spawn", "0", CVAR_NONE};
cvar_t sv_phys_autospawn_test = {"sv_phys_autospawn_test", "0", CVAR_NONE};
cvar_t sv_phys_player_push = {"sv_phys_player_push", "1", CVAR_NONE};
cvar_t sv_phys_player_push_max = {"sv_phys_player_push_max", "140", CVAR_NONE};
cvar_t sv_phys_player_mass_virtual = {"sv_phys_player_mass_virtual", "90", CVAR_NONE};
cvar_t sv_phys_player_push_vertical = {"sv_phys_player_push_vertical", "0", CVAR_NONE};
cvar_t sv_phys_player_push_debug = {"sv_phys_player_push_debug", "0", CVAR_NONE};
cvar_t sv_phys_solver_iterations = {"sv_phys_solver_iterations", "4", CVAR_NONE};
cvar_t sv_phys_penetration_slop = {"sv_phys_penetration_slop", "0.05", CVAR_NONE};
cvar_t sv_phys_pos_correct = {"sv_phys_pos_correct", "0.6", CVAR_NONE};
cvar_t sv_phys_stack_damping = {"sv_phys_stack_damping", "0.98", CVAR_NONE};
cvar_t sv_phys_tilt = {"sv_phys_tilt", "1", CVAR_NONE};
cvar_t sv_phys_tilt_scale = {"sv_phys_tilt_scale", "10", CVAR_NONE};
cvar_t sv_phys_angular_damping = {"sv_phys_angular_damping", "6", CVAR_NONE};
cvar_t sv_phys_tip_threshold = {"sv_phys_tip_threshold", "0.35", CVAR_NONE};

static qboolean phys_autospawn_done;

static void PhysInteract_SpawnTest_f (void);
static void PhysInteract_SpawnStack_f (void);
static void PhysInteract_SpawnFront_f (void);
static void PhysInteract_List_f (void);
static qboolean PhysInteract_FindSupportedSpawnOrigin (edict_t *player, vec3_t out_origin);
static qboolean PhysInteract_FindFrontSpawnOrigin (edict_t *player, float preferred_distance, vec3_t out_origin);
static void PhysInteract_GetAbsBounds (const edict_t *ent, vec3_t absmins, vec3_t absmaxs);
static void PhysInteract_HandleDebugSpawnCvar (void);

static float PhysInteract_Clamp (float value, float min_value, float max_value)
{
	if (value < min_value)
		return min_value;
	if (value > max_value)
		return max_value;
	return value;
}

static void PhysInteract_ClearBodySlot (int entnum)
{
	if (entnum < 0 || entnum >= phys_capacity)
		return;

	memset (&phys_bodies[entnum], 0, sizeof (phys_bodies[entnum]));
	memset (&phys_authored[entnum], 0, sizeof (phys_authored[entnum]));
}

static qboolean PhysInteract_DebugEnabled (void)
{
	return sv_phys_debug.value > 0.f || developer.value > 0.f || (debug_enable.value != 0.f && DBG_ChannelEnabled(DBG_CH_PHYSICS));
}

static qboolean PhysInteract_EnsureStorage (void)
{
	int target;

	if (!qcvm)
		return false;

	target = qcvm->max_edicts;
	if (target <= 0)
		return false;

	if (phys_capacity == target && phys_authored && phys_bodies && phys_active_indices)
		return true;

	if (phys_authored)
		q_free (phys_authored);
	if (phys_bodies)
		q_free (phys_bodies);
	if (phys_active_indices)
		q_free (phys_active_indices);

	phys_capacity = 0;
	phys_authored = (phys_interact_authored_t *) q_calloc ((size_t)target, sizeof (*phys_authored));
	phys_bodies = (phys_interact_body_t *) q_calloc ((size_t)target, sizeof (*phys_bodies));
	phys_active_indices = (int *) q_calloc ((size_t)target, sizeof (*phys_active_indices));
	if (!phys_authored || !phys_bodies || !phys_active_indices)
		Sys_Error ("PhysInteract_EnsureStorage: out of memory for %d edicts", target);

	phys_capacity = target;
	phys_active_count = 0;

	return true;
}

static edict_t *PhysInteract_GetPrimaryPlayer (void)
{
	int i;

	for (i = 0; i < svs.maxclients; i++)
	{
		client_t *client = &svs.clients[i];
		if (!client->active || !client->edict || client->edict->free)
			continue;
		return client->edict;
	}

	return NULL;
}

static qboolean PhysInteract_ClassnameAllowsPhysics (const char *classname)
{
	if (!classname || !classname[0])
		return false;

	if (!q_strcasecmp (classname, "physics_prop"))
		return true;
	if (!q_strcasecmp (classname, "physics_box"))
		return true;

	return false;
}

static void PhysInteract_ApplyTypePreset (const char *phys_type, float *mass, float *friction, float *restitution)
{
	if (!phys_type || !phys_type[0])
		return;

	if (!q_strcasecmp (phys_type, "crate"))
	{
		*mass = 25.f;
		*friction = 0.8f;
		*restitution = 0.04f;
	}
	else if (!q_strcasecmp (phys_type, "barrel"))
	{
		*mass = 18.f;
		*friction = 0.65f;
		*restitution = 0.06f;
	}
	else if (!q_strcasecmp (phys_type, "metal"))
	{
		*mass = 45.f;
		*friction = 0.5f;
		*restitution = 0.03f;
	}
	else if (!q_strcasecmp (phys_type, "debris"))
	{
		*mass = 10.f;
		*friction = 0.9f;
		*restitution = 0.02f;
	}
}

static qboolean PhysInteract_IsMovetypeSupported (const edict_t *ent)
{
	switch ((int)ent->v.movetype)
	{
	case MOVETYPE_NONE:
	case MOVETYPE_TOSS:
	case MOVETYPE_BOUNCE:
	case MOVETYPE_GIB:
	case MOVETYPE_FLY:
	case MOVETYPE_FLYMISSILE:
		return true;
	default:
		return false;
	}
}

static qboolean PhysInteract_ShouldOptIn (edict_t *ent, int entnum)
{
	phys_interact_authored_t *auth;
	const char *classname;
	int spawnflags;
	qboolean opt_in;

	auth = &phys_authored[entnum];

	if (auth->physics_set)
		return auth->physics_enabled;

	classname = PR_GetString (ent->v.classname);
	spawnflags = (int)ent->v.spawnflags;
	opt_in = PhysInteract_ClassnameAllowsPhysics (classname);
	if ((spawnflags & SPAWNFLAG_PHYS_INTERACT) != 0)
		opt_in = true;

	return opt_in;
}

static int PhysInteract_ClipVelocity (const vec3_t in, const vec3_t normal, vec3_t out, float overbounce)
{
	float backoff;
	float change;
	int i;
	int blocked;

	blocked = 0;
	if (normal[2] > 0.f)
		blocked |= 1;
	if (!normal[2])
		blocked |= 2;

	backoff = DotProduct (in, normal) * overbounce;

	for (i = 0; i < 3; i++)
	{
		change = normal[i] * backoff;
		out[i] = in[i] - change;
		if (out[i] > -PHYS_STOP_EPSILON && out[i] < PHYS_STOP_EPSILON)
			out[i] = 0.f;
	}

	return blocked;
}

static void PhysInteract_WakeBody (phys_interact_body_t *body)
{
	body->sleeping = false;
	body->at_rest = false;
	body->sleep_time = 0.f;
}

static qboolean PhysInteract_CheckGroundSupport (edict_t *ent, edict_t **ground_ent_out)
{
	trace_t tr;
	vec3_t end;

	VectorCopy (ent->v.origin, end);
	end[2] -= 2.f;
	tr = SV_Move (ent->v.origin, ent->v.mins, ent->v.maxs, end, MOVE_NOMONSTERS, ent);

	if (ground_ent_out)
		*ground_ent_out = tr.ent;

	return tr.fraction < 1.f && tr.plane.normal[2] > 0.7f;
}

static void PhysInteract_MoveAgainstWorld (edict_t *ent, phys_interact_body_t *body, float dt, qboolean *floor_hit, edict_t **ground_ent)
{
	int bump;
	float time_left;
	vec3_t original_velocity;
	vec3_t primal_velocity;
	vec3_t new_velocity;
	vec3_t end;
	vec3_t origin;
	trace_t tr;

	*floor_hit = false;
	*ground_ent = NULL;
	time_left = dt;

	VectorCopy (ent->v.origin, origin);
	VectorCopy (body->velocity, original_velocity);
	VectorCopy (body->velocity, primal_velocity);

	for (bump = 0; bump < PHYS_MAX_BUMPS; bump++)
	{
		if (!body->velocity[0] && !body->velocity[1] && !body->velocity[2])
			break;

		VectorMA (origin, time_left, body->velocity, end);
		tr = SV_Move (origin, ent->v.mins, ent->v.maxs, end, MOVE_NOMONSTERS, ent);

		if (tr.allsolid)
		{
			VectorCopy (vec3_origin, body->velocity);
			break;
		}

		if (tr.fraction > 0.f)
		{
			VectorCopy (tr.endpos, origin);
			VectorCopy (body->velocity, original_velocity);
		}

		if (tr.fraction == 1.f)
			break;

		if (tr.plane.normal[2] > 0.7f)
		{
			*floor_hit = true;
			*ground_ent = tr.ent;
		}

		PhysInteract_ClipVelocity (original_velocity, tr.plane.normal, new_velocity, 1.f + body->restitution);
		VectorCopy (new_velocity, body->velocity);

		if (*floor_hit && fabs (body->velocity[2]) < 20.f)
			body->velocity[2] = 0.f;

		time_left -= time_left * tr.fraction;
		if (time_left <= 0.f)
			break;

		if (DotProduct (body->velocity, primal_velocity) <= 0.f)
		{
			VectorCopy (vec3_origin, body->velocity);
			break;
		}
	}

	VectorCopy (origin, ent->v.origin);
}

static qboolean PhysInteract_InitBodyForEntity (edict_t *ent, int entnum)
{
	phys_interact_body_t *body;
	phys_interact_authored_t *auth;
	float mass;
	float friction;
	float restitution;
	const char *classname;

	if (!PhysInteract_IsMovetypeSupported (ent))
		return false;

	if ((int)ent->v.solid != SOLID_BBOX && (int)ent->v.solid != SOLID_SLIDEBOX)
		return false;

	if (ent->v.mins[0] == ent->v.maxs[0] && ent->v.mins[1] == ent->v.maxs[1] && ent->v.mins[2] == ent->v.maxs[2])
		return false;

	body = &phys_bodies[entnum];
	auth = &phys_authored[entnum];

	memset (body, 0, sizeof (*body));

	mass = 25.f;
	friction = sv_phys_friction.value;
	restitution = sv_phys_restitution.value;

	if (auth->phys_type_set)
		PhysInteract_ApplyTypePreset (auth->phys_type, &mass, &friction, &restitution);

	if (auth->mass_set)
		mass = auth->mass;
	if (auth->friction_set)
		friction = auth->friction;
	if (auth->restitution_set)
		restitution = auth->restitution;

	mass = PhysInteract_Clamp (mass, 1.f, 500.f);
	friction = PhysInteract_Clamp (friction, 0.f, 4.f);
	restitution = PhysInteract_Clamp (restitution, 0.f, 0.5f);

	body->mass = mass;
	body->inv_mass = 1.f / mass;
	body->friction = friction;
	body->restitution = restitution;
	body->enabled = true;
	body->initialized = true;
	body->sleeping = false;
	body->at_rest = false;
	body->sleep_time = 0.f;
	body->support_fraction = 0.f;

	VectorCopy (ent->v.velocity, body->velocity);
	VectorCopy (vec3_origin, body->angular_velocity);
	VectorCopy (vec3_origin, body->pending_impulse);
	VectorCopy (ent->v.angles, body->base_angles);
	VectorCopy (vec3_origin, body->visual_tilt);

	if (PhysInteract_DebugEnabled ())
	{
		classname = PR_GetString (ent->v.classname);
		Con_Printf ("phys_interact: enabled %s #%d (mass %.1f, friction %.2f, restitution %.2f)\n",
			classname && classname[0] ? classname : "<unnamed>", entnum, mass, friction, restitution);
	}

	return true;
}

static edict_t *PhysInteract_SpawnTestPropAt (const vec3_t origin, float mass)
{
	edict_t *ent;
	int entnum;

	if (!sv.active)
		return NULL;
	if (!PhysInteract_EnsureStorage ())
		return NULL;

	ent = ED_Alloc ();
	if (!ent)
		return NULL;

	VectorCopy (origin, ent->v.origin);
	VectorSet (ent->v.mins, -16.f, -16.f, -16.f);
	VectorSet (ent->v.maxs, 16.f, 16.f, 16.f);
	VectorCopy (vec3_origin, ent->v.velocity);
	VectorCopy (vec3_origin, ent->v.avelocity);
	VectorCopy (vec3_origin, ent->v.angles);
	ent->v.solid = SOLID_BBOX;
	ent->v.movetype = MOVETYPE_TOSS;
	ent->v.flags = (int)ent->v.flags & ~FL_ONGROUND;
	ent->v.classname = PR_SetEngineString ("physics_prop");
	SV_LinkEdict (ent, true);

	entnum = NUM_FOR_EDICT (ent);
	if (entnum > 0 && entnum < phys_capacity)
	{
		phys_authored[entnum].physics_set = true;
		phys_authored[entnum].physics_enabled = true;
		phys_authored[entnum].mass_set = true;
		phys_authored[entnum].mass = PhysInteract_Clamp (mass, 1.f, 500.f);
	}

	PhysInteract_OnEntitySpawned (ent);
	return ent;
}

static void PhysInteract_GetDebugSpawnOrigin (edict_t *player, vec3_t origin)
{
	if (player)
	{
		if (!PhysInteract_FindSupportedSpawnOrigin (player, origin))
		{
			VectorCopy (player->v.origin, origin);
			origin[0] += 8.f;
			origin[2] += 48.f;
		}
	}
	else
	{
		// Early in local startup, the player edict may be unavailable.
		// Keep debug spawn commands deterministic for smoke tests.
		VectorSet (origin, 0.f, 0.f, 96.f);
	}
}

static void PhysInteract_GetFrontSpawnOrigin (edict_t *player, float distance, vec3_t out_origin)
{
	if (!player || !PhysInteract_FindFrontSpawnOrigin (player, distance, out_origin))
		PhysInteract_GetDebugSpawnOrigin (player, out_origin);
}

static qboolean PhysInteract_FindFrontSpawnOrigin (edict_t *player, float preferred_distance, vec3_t out_origin)
{
	static const float test_offsets[] = {0.f, -4.f, 4.f, -8.f, 8.f, -12.f, 12.f};
	vec3_t forward;
	vec3_t player_eye;
	vec3_t line_end;
	vec3_t down_start;
	vec3_t down_end;
	vec3_t candidate;
	trace_t tr_line;
	trace_t tr_down;
	trace_t tr_box;
	float base_dist;
	int i;

	// Spawn close in front of the player for deterministic push smoke tests.
	AngleVectors (player->v.angles, forward, NULL, NULL);
	forward[2] = 0.f;
	if (VectorNormalize (forward) < 0.01f)
		VectorSet (forward, 1.f, 0.f, 0.f);

	base_dist = PhysInteract_Clamp (preferred_distance, 8.f, 64.f);
	VectorCopy (player->v.origin, player_eye);
	player_eye[2] += q_max (0.f, player->v.view_ofs[2]);

	for (i = 0; i < (int)countof(test_offsets); i++)
	{
		float dist = PhysInteract_Clamp (base_dist + test_offsets[i], 8.f, 64.f);

		VectorMA (player->v.origin, dist, forward, candidate);

		// Reject placements behind immediate world obstruction.
		VectorMA (player_eye, dist, forward, line_end);
		tr_line = SV_Move (player_eye, vec3_origin, vec3_origin, line_end, MOVE_NOMONSTERS, player);
		if (tr_line.fraction < 0.95f && tr_line.ent == qcvm->edicts)
			continue;

		// Drop to floor so the prop appears in the player lane.
		VectorCopy (candidate, down_start);
		down_start[2] += 64.f;
		VectorCopy (candidate, down_end);
		down_end[2] -= 192.f;
		tr_down = SV_Move (down_start, vec3_origin, vec3_origin, down_end, MOVE_NOMONSTERS, player);
		if (tr_down.startsolid || tr_down.fraction >= 1.f || tr_down.plane.normal[2] <= 0.6f)
			continue;

		VectorCopy (tr_down.endpos, candidate);
		candidate[2] += 16.f;

		// Ensure spawn bbox is not embedded.
		tr_box = SV_Move (candidate, (vec3_t){-16.f, -16.f, -16.f}, (vec3_t){16.f, 16.f, 16.f}, candidate, MOVE_NOMONSTERS, player);
		if (tr_box.startsolid || tr_box.allsolid)
			continue;

		VectorCopy (candidate, out_origin);
		return true;
	}

	return false;
}

static qboolean PhysInteract_FindSupportedSpawnOrigin (edict_t *player, vec3_t out_origin)
{
	static const vec2_t offsets[] = {
		{0.f, 0.f},
		{32.f, 0.f},
		{-32.f, 0.f},
		{0.f, 32.f},
		{0.f, -32.f},
		{48.f, 48.f},
		{-48.f, 48.f},
		{48.f, -48.f},
		{-48.f, -48.f},
	};
	trace_t tr;
	vec3_t start;
	vec3_t end;
	int i;

	for (i = 0; i < (int)countof(offsets); i++)
	{
		VectorCopy (player->v.origin, start);
		start[0] += offsets[i][0];
		start[1] += offsets[i][1];
		start[2] += 96.f;

		VectorCopy (start, end);
		end[2] -= 512.f;

		tr = SV_Move (start, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, player);
		if (tr.startsolid || tr.fraction >= 1.f || tr.plane.normal[2] <= 0.7f)
			continue;

		VectorCopy (tr.endpos, out_origin);
		out_origin[2] += 48.f;
		return true;
	}

	return false;
}

static void PhysInteract_SpawnTest_f (void)
{
	edict_t *player;
	edict_t *ent;
	vec3_t origin;
	float mass;
	qcvm_t *oldvm;

	if (!sv.active)
	{
		Con_Printf ("phys_spawn_test: no active server\n");
		return;
	}

	PR_PushQCVM (&sv.qcvm, &oldvm);

	player = PhysInteract_GetPrimaryPlayer ();
	PhysInteract_GetDebugSpawnOrigin (player, origin);

	mass = (Cmd_Argc () >= 2) ? Q_atof (Cmd_Argv (1)) : 25.f;
	ent = PhysInteract_SpawnTestPropAt (origin, mass);
	PR_PopQCVM (oldvm);
	if (!ent)
	{
		Con_Printf ("phys_spawn_test: failed\n");
		return;
	}

	Con_Printf ("phys_spawn_test: spawned physics_prop #%d at (%.1f %.1f %.1f), mass %.1f\n",
		NUM_FOR_EDICT (ent), origin[0], origin[1], origin[2], mass);
}

static void PhysInteract_SpawnStack_f (void)
{
	int count;
	float mass;
	float spacing;
	int i;
	int spawned;
	edict_t *player;
	vec3_t origin;
	vec3_t spawn_origin;
	qcvm_t *oldvm;

	if (!sv.active)
	{
		Con_Printf ("phys_spawn_stack: no active server\n");
		return;
	}

	count = (Cmd_Argc () >= 2) ? Q_atoi (Cmd_Argv (1)) : 3;
	mass = (Cmd_Argc () >= 3) ? Q_atof (Cmd_Argv (2)) : 25.f;
	spacing = (Cmd_Argc () >= 4) ? Q_atof (Cmd_Argv (3)) : 36.f;
	count = CLAMP (1, count, 16);
	mass = PhysInteract_Clamp (mass, 1.f, 500.f);
	spacing = PhysInteract_Clamp (spacing, 16.f, 128.f);

	PR_PushQCVM (&sv.qcvm, &oldvm);
	player = PhysInteract_GetPrimaryPlayer ();
	PhysInteract_GetDebugSpawnOrigin (player, origin);

	spawned = 0;
	for (i = 0; i < count; i++)
	{
		VectorCopy (origin, spawn_origin);
		spawn_origin[2] += i * spacing;

		if (!PhysInteract_SpawnTestPropAt (spawn_origin, mass))
			break;

		spawned++;
	}
	PR_PopQCVM (oldvm);

	Con_Printf ("phys_spawn_stack: spawned %d/%d props at (%.1f %.1f %.1f), mass %.1f, spacing %.1f\n",
		spawned, count, origin[0], origin[1], origin[2], mass, spacing);
}

static void PhysInteract_SpawnFront_f (void)
{
	float distance;
	float mass;
	edict_t *player;
	edict_t *ent;
	vec3_t spawn_origin;
	qcvm_t *oldvm;

	if (!sv.active)
	{
		Con_Printf ("phys_spawn_front: no active server\n");
		return;
	}

	distance = (Cmd_Argc () >= 2) ? Q_atof (Cmd_Argv (1)) : 20.f;
	mass = (Cmd_Argc () >= 3) ? Q_atof (Cmd_Argv (2)) : 25.f;
	distance = PhysInteract_Clamp (distance, 0.f, 256.f);
	mass = PhysInteract_Clamp (mass, 1.f, 500.f);

	PR_PushQCVM (&sv.qcvm, &oldvm);
	player = PhysInteract_GetPrimaryPlayer ();
	if (!player)
	{
		PR_PopQCVM (oldvm);
		Con_Printf ("phys_spawn_front: no active player\n");
		return;
	}

	PhysInteract_GetFrontSpawnOrigin (player, distance, spawn_origin);
	ent = PhysInteract_SpawnTestPropAt (spawn_origin, mass);
	PR_PopQCVM (oldvm);

	if (!ent)
	{
		Con_Printf ("phys_spawn_front: failed\n");
		return;
	}

	Con_Printf ("phys_spawn_front: spawned physics_prop #%d at (%.1f %.1f %.1f), mass %.1f, distance %.1f\n",
		NUM_FOR_EDICT (ent), spawn_origin[0], spawn_origin[1], spawn_origin[2], mass, distance);
}

static void PhysInteract_List_f (void)
{
	qcvm_t *oldvm;
	int i;
	int listed = 0;

	if (!sv.active)
	{
		Con_Printf ("phys_list: no active server\n");
		return;
	}

	PR_PushQCVM (&sv.qcvm, &oldvm);
	if (!PhysInteract_EnsureStorage ())
	{
		PR_PopQCVM (oldvm);
		Con_Printf ("phys_list: no active server context\n");
		return;
	}

	Con_Printf ("phys_list: sv_phys_interact=%.0f\n", sv_phys_interact.value);
	for (i = svs.maxclients + 1; i < qcvm->num_edicts; i++)
	{
		edict_t *ent = EDICT_NUM (i);
		phys_interact_body_t *body;
		const char *classname;
		float speed;

		if (ent->free)
			continue;

		body = &phys_bodies[i];
		if (!body->initialized)
			continue;

		classname = PR_GetString (ent->v.classname);
		speed = sqrtf (DotProduct (body->velocity, body->velocity));

		Con_Printf ("  #%d %-16s mass %.1f speed %.2f sleep %d support %.2f org (%.1f %.1f %.1f)\n",
			i,
			(classname && classname[0]) ? classname : "<unnamed>",
			body->mass,
			speed,
			body->sleeping ? 1 : 0,
			body->support_fraction,
			ent->v.origin[0], ent->v.origin[1], ent->v.origin[2]);
		listed++;
	}

	if (!listed)
		Con_Printf ("  (no active interactive physics bodies)\n");

	PR_PopQCVM (oldvm);
}

static float PhysInteract_ComputeSupportFraction (edict_t *ent, vec2_t lean_dir_out)
{
	vec3_t sample;
	vec3_t end;
	trace_t tr;
	float support_count = 0.f;
	float unsupported_x = 0.f;
	float unsupported_y = 0.f;
	float half_x = q_max (8.f, (ent->v.maxs[0] - ent->v.mins[0]) * 0.45f);
	float half_y = q_max (8.f, (ent->v.maxs[1] - ent->v.mins[1]) * 0.45f);
	vec2_t offsets[5] = {
		{0.f, 0.f},
		{half_x, half_y},
		{half_x, -half_y},
		{-half_x, half_y},
		{-half_x, -half_y},
	};
	int i;
	float len;

	lean_dir_out[0] = 0.f;
	lean_dir_out[1] = 0.f;

	for (i = 0; i < 5; i++)
	{
		VectorCopy (ent->v.origin, sample);
		sample[0] += offsets[i][0];
		sample[1] += offsets[i][1];
		sample[2] += ent->v.mins[2] + 1.f;
		VectorCopy (sample, end);
		end[2] -= 6.f;

		tr = SV_Move (sample, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, ent);
		if (tr.fraction < 1.f && tr.plane.normal[2] > 0.6f)
		{
			support_count += 1.f;
		}
		else
		{
			unsupported_x += offsets[i][0];
			unsupported_y += offsets[i][1];
		}
	}

	len = sqrtf (unsupported_x * unsupported_x + unsupported_y * unsupported_y);
	if (len > 0.f)
	{
		lean_dir_out[0] = unsupported_x / len;
		lean_dir_out[1] = unsupported_y / len;
	}

	return support_count / 5.f;
}

static void PhysInteract_UpdateTilt (edict_t *ent, phys_interact_body_t *body, qboolean floor_hit, qboolean just_landed, float dt)
{
	float target_pitch = 0.f;
	float target_roll = 0.f;
	float support;
	float tip_threshold;
	float tilt_scale;
	float angular_damping;
	vec2_t lean_dir;
	float damping_factor;
	float correction_scale;
	int i;

	if (sv_phys_tilt.value <= 0.f)
		return;

	support = floor_hit ? PhysInteract_ComputeSupportFraction (ent, lean_dir) : 0.f;
	body->support_fraction = support;
	tip_threshold = PhysInteract_Clamp (sv_phys_tip_threshold.value, 0.f, 1.f);
	tilt_scale = q_max (0.f, sv_phys_tilt_scale.value);
	angular_damping = q_max (0.f, sv_phys_angular_damping.value);

	if (floor_hit)
	{
		float lack = 1.f - support;
		target_pitch = -lean_dir[1] * tilt_scale * lack;
		target_roll = lean_dir[0] * tilt_scale * lack;

		if (just_landed)
		{
			body->angular_velocity[0] += -body->velocity[1] * 0.03f;
			body->angular_velocity[2] += body->velocity[0] * 0.03f;
		}

		if (support < tip_threshold)
		{
			float bias = (tip_threshold - support) * 80.f * dt;
			body->velocity[0] += lean_dir[0] * bias;
			body->velocity[1] += lean_dir[1] * bias;
		}
	}

	damping_factor = PhysInteract_Clamp (1.f - angular_damping * dt, 0.f, 1.f);
	for (i = 0; i < 3; i++)
		body->angular_velocity[i] *= damping_factor;

	correction_scale = 8.f * dt;
	body->angular_velocity[0] += (target_pitch - body->visual_tilt[0]) * correction_scale;
	body->angular_velocity[2] += (target_roll - body->visual_tilt[2]) * correction_scale;

	body->visual_tilt[0] += body->angular_velocity[0] * dt;
	body->visual_tilt[2] += body->angular_velocity[2] * dt;
	body->visual_tilt[0] = PhysInteract_Clamp (body->visual_tilt[0], -35.f, 35.f);
	body->visual_tilt[2] = PhysInteract_Clamp (body->visual_tilt[2], -35.f, 35.f);

	if (body->sleeping)
	{
		body->visual_tilt[0] *= 0.9f;
		body->visual_tilt[2] *= 0.9f;
	}

	ent->v.angles[0] = body->base_angles[0] + body->visual_tilt[0];
	ent->v.angles[2] = body->base_angles[2] + body->visual_tilt[2];
}

static void PhysInteract_SimulateBody (edict_t *ent, phys_interact_body_t *body, float dt)
{
	float speed;
	float damping;
	float sleep_eps;
	edict_t *ground_ent;
	qboolean floor_hit;
	qboolean was_on_floor;
	qboolean just_landed;

	if (!SV_RunThink (ent))
		return;
	if (ent->free)
		return;

	if (body->sleeping)
	{
		if (!PhysInteract_CheckGroundSupport (ent, &ground_ent))
		{
			PhysInteract_WakeBody (body);
			if (PhysInteract_DebugEnabled ())
				Con_Printf ("phys_interact: waking body #%d (support lost)\n", NUM_FOR_EDICT (ent));
		}
		else
		{
			VectorCopy (vec3_origin, body->velocity);
			VectorCopy (vec3_origin, ent->v.velocity);
			ent->v.flags = (int)ent->v.flags | FL_ONGROUND;
			ent->v.groundentity = EDICT_TO_PROG (ground_ent);
			SV_LinkEdict (ent, true);
			return;
		}
	}

	if (!VectorCompare (body->pending_impulse, vec3_origin))
	{
		VectorAdd (body->velocity, body->pending_impulse, body->velocity);
		VectorCopy (vec3_origin, body->pending_impulse);
		PhysInteract_WakeBody (body);
	}

	body->velocity[2] -= sv_gravity.value * sv_phys_gravity_scale.value * dt;
	PhysInteract_MoveAgainstWorld (ent, body, dt, &floor_hit, &ground_ent);
	was_on_floor = (body->flags & PHYS_BODY_FLAG_WAS_ON_FLOOR) != 0;
	just_landed = floor_hit && !was_on_floor;
	if (floor_hit)
		body->flags |= PHYS_BODY_FLAG_WAS_ON_FLOOR;
	else
		body->flags &= ~PHYS_BODY_FLAG_WAS_ON_FLOOR;

	if (floor_hit)
	{
		if (!(body->flags & PHYS_BODY_FLAG_HIT_FLOOR) && PhysInteract_DebugEnabled ())
			Con_Printf ("phys_interact: body #%d first floor contact\n", NUM_FOR_EDICT (ent));
		body->flags |= PHYS_BODY_FLAG_HIT_FLOOR;

		damping = 1.f - PhysInteract_Clamp (sv_phys_friction.value * body->friction * dt, 0.f, 1.f);
		body->velocity[0] *= damping;
		body->velocity[1] *= damping;
		if (fabs (body->velocity[0]) < 0.01f)
			body->velocity[0] = 0.f;
		if (fabs (body->velocity[1]) < 0.01f)
			body->velocity[1] = 0.f;

		ent->v.flags = (int)ent->v.flags | FL_ONGROUND;
		if (ground_ent)
			ent->v.groundentity = EDICT_TO_PROG (ground_ent);
	}
	else
	{
		ent->v.flags = (int)ent->v.flags & ~FL_ONGROUND;
		ent->v.groundentity = 0;
	}

	sleep_eps = q_max (0.1f, sv_phys_sleep_epsilon.value);
	speed = sqrtf (DotProduct (body->velocity, body->velocity));
	if (floor_hit && speed < sleep_eps)
	{
		body->sleep_time += dt;
		if (body->sleep_time >= 0.25f)
		{
			VectorCopy (vec3_origin, body->velocity);
			body->sleeping = true;
			body->at_rest = true;
			if (PhysInteract_DebugEnabled ())
				Con_Printf ("phys_interact: body #%d entered sleep\n", NUM_FOR_EDICT (ent));
		}
	}
	else
	{
		body->sleep_time = 0.f;
		body->at_rest = false;
	}

	VectorCopy (body->velocity, ent->v.velocity);
	PhysInteract_UpdateTilt (ent, body, floor_hit, just_landed, dt);
	SV_LinkEdict (ent, true);
}

void PhysInteract_Init (void)
{
	Cvar_RegisterVariable (&sv_phys_interact);
	Cvar_RegisterVariable (&sv_phys_gravity_scale);
	Cvar_RegisterVariable (&sv_phys_friction);
	Cvar_RegisterVariable (&sv_phys_restitution);
	Cvar_RegisterVariable (&sv_phys_sleep_epsilon);
	Cvar_RegisterVariable (&sv_phys_debug);
	Cvar_RegisterVariable (&sv_phys_debug_spawn);
	Cvar_RegisterVariable (&sv_phys_autospawn_test);
	Cvar_RegisterVariable (&sv_phys_player_push);
	Cvar_RegisterVariable (&sv_phys_player_push_max);
	Cvar_RegisterVariable (&sv_phys_player_mass_virtual);
	Cvar_RegisterVariable (&sv_phys_player_push_vertical);
	Cvar_RegisterVariable (&sv_phys_player_push_debug);
	Cvar_RegisterVariable (&sv_phys_solver_iterations);
	Cvar_RegisterVariable (&sv_phys_penetration_slop);
	Cvar_RegisterVariable (&sv_phys_pos_correct);
	Cvar_RegisterVariable (&sv_phys_stack_damping);
	Cvar_RegisterVariable (&sv_phys_tilt);
	Cvar_RegisterVariable (&sv_phys_tilt_scale);
	Cvar_RegisterVariable (&sv_phys_angular_damping);
	Cvar_RegisterVariable (&sv_phys_tip_threshold);
	Cmd_AddCommand ("phys_spawn_test", PhysInteract_SpawnTest_f);
	Cmd_AddCommand ("phys_spawn_stack", PhysInteract_SpawnStack_f);
	Cmd_AddCommand ("phys_spawn_front", PhysInteract_SpawnFront_f);
	Cmd_AddCommand ("phys_list", PhysInteract_List_f);
}

void PhysInteract_Shutdown (void)
{
	if (phys_authored)
		q_free (phys_authored);
	if (phys_bodies)
		q_free (phys_bodies);
	if (phys_active_indices)
		q_free (phys_active_indices);

	phys_authored = NULL;
	phys_bodies = NULL;
	phys_active_indices = NULL;
	phys_capacity = 0;
	phys_active_count = 0;
}

void PhysInteract_OnServerSpawned (void)
{
	if (!PhysInteract_EnsureStorage ())
		return;

	memset (phys_authored, 0, sizeof (*phys_authored) * (size_t)phys_capacity);
	memset (phys_bodies, 0, sizeof (*phys_bodies) * (size_t)phys_capacity);
	memset (phys_active_indices, 0, sizeof (*phys_active_indices) * (size_t)phys_capacity);
	phys_active_count = 0;
	phys_autospawn_done = false;
}

void PhysInteract_OnEdictCleared (edict_t *ent)
{
	int entnum;

	if (!ent || !phys_capacity)
		return;

	entnum = NUM_FOR_EDICT (ent);
	PhysInteract_ClearBodySlot (entnum);
}

void PhysInteract_OnEdictFreed (edict_t *ent)
{
	int entnum;

	if (!ent || !phys_capacity)
		return;

	entnum = NUM_FOR_EDICT (ent);
	PhysInteract_ClearBodySlot (entnum);
}

void PhysInteract_BeginParseEntity (edict_t *ent)
{
	int entnum;

	if (!ent || !PhysInteract_EnsureStorage ())
		return;

	entnum = NUM_FOR_EDICT (ent);
	if (entnum < 0 || entnum >= phys_capacity)
		return;

	memset (&phys_authored[entnum], 0, sizeof (phys_authored[entnum]));
}

void PhysInteract_ParseEntityKey (edict_t *ent, const char *key, const char *value)
{
	int entnum;
	phys_interact_authored_t *auth;

	if (!ent || !key || !value || !PhysInteract_EnsureStorage ())
		return;

	entnum = NUM_FOR_EDICT (ent);
	if (entnum < 0 || entnum >= phys_capacity)
		return;

	auth = &phys_authored[entnum];

	if (!q_strcasecmp (key, "physics"))
	{
		auth->physics_set = true;
		auth->physics_enabled = (Q_atof (value) != 0.f);
		return;
	}
	if (!q_strcasecmp (key, "mass"))
	{
		auth->mass_set = true;
		auth->mass = Q_atof (value);
		return;
	}
	if (!q_strcasecmp (key, "friction"))
	{
		auth->friction_set = true;
		auth->friction = Q_atof (value);
		return;
	}
	if (!q_strcasecmp (key, "restitution"))
	{
		auth->restitution_set = true;
		auth->restitution = Q_atof (value);
		return;
	}
	if (!q_strcasecmp (key, "phys_type"))
	{
		auth->phys_type_set = true;
		q_strlcpy (auth->phys_type, value, sizeof (auth->phys_type));
		return;
	}
}

void PhysInteract_OnEntitySpawned (edict_t *ent)
{
	int entnum;

	if (!ent || !PhysInteract_EnsureStorage ())
		return;

	entnum = NUM_FOR_EDICT (ent);
	if (entnum < 0 || entnum >= phys_capacity)
		return;

	memset (&phys_bodies[entnum], 0, sizeof (phys_bodies[entnum]));
}

void PhysInteract_BeginFrame (void)
{
	phys_active_count = 0;
}

qboolean PhysInteract_ShouldHandleEntity (edict_t *ent, int entnum)
{
	if (!sv_phys_interact.value)
		return false;

	if (!ent || ent->free)
		return false;

	if (!PhysInteract_EnsureStorage ())
		return false;

	if (entnum <= svs.maxclients)
		return false;
	if (entnum <= 0 || entnum >= qcvm->num_edicts)
		return false;

	if (!PhysInteract_ShouldOptIn (ent, entnum))
		return false;

	if (!PhysInteract_IsMovetypeSupported (ent))
		return false;

	return true;
}

static qboolean PhysInteract_GetBodyForEntity (edict_t *ent, phys_interact_body_t **body_out, int *entnum_out)
{
	int entnum;
	phys_interact_body_t *body;

	if (!ent || ent->free)
		return false;
	if (!PhysInteract_EnsureStorage ())
		return false;

	entnum = NUM_FOR_EDICT (ent);
	if (entnum <= 0 || entnum >= qcvm->num_edicts)
		return false;
	if (!PhysInteract_ShouldHandleEntity (ent, entnum))
		return false;

	body = &phys_bodies[entnum];
	if (!body->initialized)
	{
		if (!PhysInteract_InitBodyForEntity (ent, entnum))
			return false;
	}

	if (entnum_out)
		*entnum_out = entnum;
	*body_out = body;
	return true;
}

static qboolean PhysInteract_FindTouchingBodyForPlayer (edict_t *player, edict_t **ent_out, phys_interact_body_t **body_out, int *entnum_out)
{
	vec3_t pmins, pmaxs;
	float best_dist2 = FLT_MAX;
	int best_idx = -1;
	int i;

	if (!player || !PhysInteract_EnsureStorage ())
		return false;

	VectorAdd (player->v.origin, player->v.mins, pmins);
	VectorAdd (player->v.origin, player->v.maxs, pmaxs);

	for (i = svs.maxclients + 1; i < qcvm->num_edicts; i++)
	{
		edict_t *ent = EDICT_NUM (i);
		phys_interact_body_t *body;
		vec3_t emins, emaxs;
		vec3_t delta;
		float dist2;

		if (ent->free || ent == player)
			continue;
		if (!PhysInteract_ShouldHandleEntity (ent, i))
			continue;

		body = &phys_bodies[i];
		if (!body->initialized)
		{
			if (!PhysInteract_InitBodyForEntity (ent, i))
				continue;
		}

		PhysInteract_GetAbsBounds (ent, emins, emaxs);

		// Conservative near-contact check; allows tiny numeric separation.
		if (pmaxs[0] < emins[0] - 2.f || pmins[0] > emaxs[0] + 2.f)
			continue;
		if (pmaxs[1] < emins[1] - 2.f || pmins[1] > emaxs[1] + 2.f)
			continue;
		if (pmaxs[2] < emins[2] - 8.f || pmins[2] > emaxs[2] + 8.f)
			continue;

		VectorSubtract (ent->v.origin, player->v.origin, delta);
		dist2 = DotProduct (delta, delta);
		if (dist2 >= best_dist2)
			continue;

		best_dist2 = dist2;
		best_idx = i;
	}

	if (best_idx < 0)
		return false;

	*ent_out = EDICT_NUM (best_idx);
	*body_out = &phys_bodies[best_idx];
	if (entnum_out)
		*entnum_out = best_idx;
	return true;
}

static void PhysInteract_GetAbsBounds (const edict_t *ent, vec3_t absmins, vec3_t absmaxs)
{
	VectorAdd (ent->v.origin, ent->v.mins, absmins);
	VectorAdd (ent->v.origin, ent->v.maxs, absmaxs);
}

static qboolean PhysInteract_ResolveBodyPair (edict_t *a_ent, phys_interact_body_t *a_body, edict_t *b_ent, phys_interact_body_t *b_body)
{
	vec3_t amin, amax, bmin, bmax;
	vec3_t center_a, center_b;
	vec3_t normal;
	vec3_t correction;
	float overlap_x, overlap_y, overlap_z;
	float min_overlap;
	float slop;
	float pos_correct;
	float total_inv_mass;
	float share_a, share_b;
	vec3_t rel_vel;
	float vel_normal;
	float restitution;
	float j;
	qboolean resolved;

	PhysInteract_GetAbsBounds (a_ent, amin, amax);
	PhysInteract_GetAbsBounds (b_ent, bmin, bmax);

	overlap_x = q_min (amax[0], bmax[0]) - q_max (amin[0], bmin[0]);
	overlap_y = q_min (amax[1], bmax[1]) - q_max (amin[1], bmin[1]);
	overlap_z = q_min (amax[2], bmax[2]) - q_max (amin[2], bmin[2]);
	if (overlap_x <= 0.f || overlap_y <= 0.f || overlap_z <= 0.f)
		return false;

	VectorAdd (amin, amax, center_a);
	VectorScale (center_a, 0.5f, center_a);
	VectorAdd (bmin, bmax, center_b);
	VectorScale (center_b, 0.5f, center_b);

	min_overlap = overlap_x;
	VectorSet (normal, center_a[0] < center_b[0] ? -1.f : 1.f, 0.f, 0.f);
	if (overlap_y < min_overlap)
	{
		min_overlap = overlap_y;
		VectorSet (normal, 0.f, center_a[1] < center_b[1] ? -1.f : 1.f, 0.f);
	}
	if (overlap_z < min_overlap)
	{
		min_overlap = overlap_z;
		VectorSet (normal, 0.f, 0.f, center_a[2] < center_b[2] ? -1.f : 1.f);
	}

	total_inv_mass = a_body->inv_mass + b_body->inv_mass;
	if (total_inv_mass <= 0.f)
		return false;

	slop = q_max (0.f, sv_phys_penetration_slop.value);
	pos_correct = PhysInteract_Clamp (sv_phys_pos_correct.value, 0.f, 1.f);
	min_overlap = q_max (0.f, min_overlap - slop) * pos_correct;

	share_a = a_body->inv_mass / total_inv_mass;
	share_b = b_body->inv_mass / total_inv_mass;
	VectorScale (normal, min_overlap, correction);
	VectorMA (a_ent->v.origin, -share_a, correction, a_ent->v.origin);
	VectorMA (b_ent->v.origin, share_b, correction, b_ent->v.origin);

	VectorSubtract (b_body->velocity, a_body->velocity, rel_vel);
	vel_normal = DotProduct (rel_vel, normal);
	if (vel_normal > 0.f)
	{
		SV_LinkEdict (a_ent, true);
		SV_LinkEdict (b_ent, true);
		return true;
	}

	restitution = q_min (a_body->restitution, b_body->restitution);
	j = -(1.f + restitution) * vel_normal;
	j /= total_inv_mass;

	VectorMA (a_body->velocity, -j * a_body->inv_mass, normal, a_body->velocity);
	VectorMA (b_body->velocity, j * b_body->inv_mass, normal, b_body->velocity);

	if (fabs (normal[2]) > 0.7f)
	{
		a_body->velocity[0] *= PhysInteract_Clamp (sv_phys_stack_damping.value, 0.f, 1.f);
		a_body->velocity[1] *= PhysInteract_Clamp (sv_phys_stack_damping.value, 0.f, 1.f);
		b_body->velocity[0] *= PhysInteract_Clamp (sv_phys_stack_damping.value, 0.f, 1.f);
		b_body->velocity[1] *= PhysInteract_Clamp (sv_phys_stack_damping.value, 0.f, 1.f);
	}

	resolved = true;
	if (resolved)
	{
		SV_LinkEdict (a_ent, true);
		SV_LinkEdict (b_ent, true);
	}
	return resolved;
}

void PhysInteract_Frame (void)
{
	int i;
	edict_t *ent;
	phys_interact_body_t *body;

	if (!sv_phys_interact.value)
		return;
	if (!PhysInteract_EnsureStorage ())
		return;

	PhysInteract_HandleDebugSpawnCvar ();

	if (sv_phys_autospawn_test.value > 0.f && !phys_autospawn_done)
	{
		vec3_t origin;
		edict_t *player = PhysInteract_GetPrimaryPlayer ();
		if (!player || (int)player->v.movetype != MOVETYPE_WALK || player->v.modelindex == 0)
			goto skip_autospawn;

		if (sv_phys_autospawn_test.value >= 2.f)
		{
			vec3_t forward;
			AngleVectors (player->v.angles, forward, NULL, NULL);
			forward[2] = 0.f;
			if (VectorNormalize (forward) < 0.01f)
				VectorSet (forward, 1.f, 0.f, 0.f);

			// Debug-only deterministic placement: keep the prop close enough
			// that holding +forward at spawn can immediately contact it.
			VectorMA (player->v.origin, 4.f, forward, origin);
			origin[2] = player->v.origin[2] + 24.f;
		}
		else if (!PhysInteract_FindSupportedSpawnOrigin (player, origin))
		{
			VectorCopy (player->v.origin, origin);
			origin[0] += 8.f;
			origin[2] += 48.f;
		}

		if (PhysInteract_SpawnTestPropAt (origin, 25.f))
		{
			phys_autospawn_done = true;
			if (PhysInteract_DebugEnabled ())
				Con_Printf ("phys_interact: autospawned test prop at (%.1f %.1f %.1f)\n", origin[0], origin[1], origin[2]);
		}
	}
skip_autospawn:

	for (i = svs.maxclients + 1; i < qcvm->num_edicts; i++)
	{
		ent = EDICT_NUM (i);
		if (ent->free)
			continue;
		if (!PhysInteract_ShouldHandleEntity (ent, i))
			continue;

		body = &phys_bodies[i];
		if (!body->initialized)
		{
			if (!PhysInteract_InitBodyForEntity (ent, i))
				continue;
		}

		PhysInteract_SimulateBody (ent, body, host_frametime);

		if (phys_active_count < phys_capacity)
			phys_active_indices[phys_active_count++] = i;
	}

	if (phys_active_count > 1)
	{
		int iters = CLAMP (1, (int)sv_phys_solver_iterations.value, 12);
		int iter, ai, bi;
		for (iter = 0; iter < iters; iter++)
		{
			for (ai = 0; ai < phys_active_count; ai++)
			{
				int a_idx = phys_active_indices[ai];
				edict_t *a_ent = EDICT_NUM (a_idx);
				phys_interact_body_t *a_body = &phys_bodies[a_idx];
				if (a_ent->free || !a_body->initialized)
					continue;

				for (bi = ai + 1; bi < phys_active_count; bi++)
				{
					int b_idx = phys_active_indices[bi];
					edict_t *b_ent = EDICT_NUM (b_idx);
					phys_interact_body_t *b_body = &phys_bodies[b_idx];
					if (b_ent->free || !b_body->initialized)
						continue;
					PhysInteract_ResolveBodyPair (a_ent, a_body, b_ent, b_body);
				}
			}
		}

		for (i = 0; i < phys_active_count; i++)
		{
			int idx = phys_active_indices[i];
			edict_t *resolved_ent = EDICT_NUM (idx);
			phys_interact_body_t *resolved_body = &phys_bodies[idx];
			if (resolved_ent->free || !resolved_body->initialized)
				continue;
			VectorCopy (resolved_body->velocity, resolved_ent->v.velocity);
			SV_LinkEdict (resolved_ent, true);
		}
	}
}

static void PhysInteract_HandleDebugSpawnCvar (void)
{
	int mode;
	edict_t *player;
	edict_t *ent;
	vec3_t origin;

	if (sv_phys_debug_spawn.value <= 0.f)
		return;
	if (!sv.active)
		return;

	player = PhysInteract_GetPrimaryPlayer ();
	if (!player || (int)player->v.movetype != MOVETYPE_WALK || player->v.modelindex == 0)
		return;

	mode = (int)sv_phys_debug_spawn.value;
	if (mode >= 2)
		PhysInteract_GetFrontSpawnOrigin (player, 20.f, origin);
	else
		PhysInteract_GetDebugSpawnOrigin (player, origin);

	ent = PhysInteract_SpawnTestPropAt (origin, 25.f);
	Cvar_SetValueQuick (&sv_phys_debug_spawn, 0.f);

	if (!PhysInteract_DebugEnabled ())
		return;

	if (ent)
	{
		Con_Printf ("phys_interact: sv_phys_debug_spawn spawned physics_prop #%d at (%.1f %.1f %.1f) mode %d\n",
			NUM_FOR_EDICT (ent), origin[0], origin[1], origin[2], mode);
	}
	else
	{
		Con_Printf ("phys_interact: sv_phys_debug_spawn failed\n");
	}
}

void PhysInteract_PlayerContactPush (edict_t *player, edict_t *other, const vec3_t player_velocity, const vec3_t hit_normal)
{
	phys_interact_body_t *body;
	edict_t *push_target;
	int entnum;
	vec3_t push_dir;
	float push_speed;
	float mass_scale;
	float max_push_speed;
	qboolean log_push;
	const char *other_classname;
	int other_num;
	float incoming_speed;

	log_push = (sv_phys_player_push_debug.value > 0.f || PhysInteract_DebugEnabled ());
	other_classname = NULL;
	other_num = -1;

	if (other && !other->free)
	{
		other_num = NUM_FOR_EDICT (other);
		other_classname = PR_GetString (other->v.classname);
	}

	incoming_speed = sqrtf (DotProduct (player_velocity, player_velocity));
	if (log_push)
	{
		Con_Printf ("phys_interact: walk block contact ent #%d %s normal (%.2f %.2f %.2f) in_speed %.1f\n",
			other_num,
			(other_classname && other_classname[0]) ? other_classname : "<none>",
			hit_normal[0], hit_normal[1], hit_normal[2], incoming_speed);
	}

	if (!sv_phys_interact.value || sv_phys_player_push.value <= 0.f)
	{
		if (log_push)
			Con_Printf ("phys_interact: push skipped (disabled cvars)\n");
		return;
	}
	if (!player)
	{
		if (log_push)
			Con_Printf ("phys_interact: push skipped (no player)\n");
		return;
	}

	push_target = other;
	if (!other || other->free)
	{
		if (log_push)
			Con_Printf ("phys_interact: push skipped (no solid trace entity)\n");
	}

	if (!push_target || push_target->free || !PhysInteract_GetBodyForEntity (push_target, &body, &entnum))
	{
		if (PhysInteract_FindTouchingBodyForPlayer (player, &push_target, &body, &entnum))
		{
			if (log_push)
				Con_Printf ("phys_interact: push probe matched ent #%d\n", entnum);
		}
		else
		{
		if (log_push)
			Con_Printf ("phys_interact: push skipped (entity #%d not interactive body)\n", other_num);
		return;
		}
	}

	VectorCopy (player_velocity, push_dir);
	if (sv_phys_player_push_vertical.value <= 0.f)
		push_dir[2] = 0.f;

	push_speed = VectorNormalize (push_dir);
	if (push_speed <= 1.f)
	{
		if (log_push)
			Con_Printf ("phys_interact: push skipped (incoming speed %.2f)\n", push_speed);
		return;
	}

	max_push_speed = q_max (1.f, sv_phys_player_push_max.value);
	push_speed = q_min (push_speed, max_push_speed);
	push_speed *= q_max (0.f, sv_phys_player_push.value);

	mass_scale = q_max (1.f, sv_phys_player_mass_virtual.value) * body->inv_mass;
	VectorMA (body->velocity, push_speed * mass_scale * host_frametime, push_dir, body->velocity);

	if (sv_phys_player_push_vertical.value <= 0.f && body->velocity[2] > 0.f)
		body->velocity[2] = 0.f;

	PhysInteract_WakeBody (body);
	push_target->v.flags = (int)push_target->v.flags & ~FL_ONGROUND;
	push_target->v.groundentity = 0;

	if (log_push)
	{
		Con_Printf ("phys_interact: player push -> ent #%d speed %.1f mass %.1f new_vel (%.2f %.2f %.2f)\n",
			entnum, push_speed, body->mass, body->velocity[0], body->velocity[1], body->velocity[2]);
	}
}
