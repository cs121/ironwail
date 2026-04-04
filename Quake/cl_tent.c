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
// cl_tent.c -- client side temporary entities

#include "quakedef.h"
#include "r_part_q3p.h"

extern cvar_t r_particles_mode;

int			num_temp_entities;
entity_t	cl_temp_entities[MAX_TEMP_ENTITIES];
beam_t		cl_beams[MAX_BEAMS];

sfx_t			*cl_sfx_wizhit;
sfx_t			*cl_sfx_knighthit;
sfx_t			*cl_sfx_tink1;
sfx_t			*cl_sfx_ric1;
sfx_t			*cl_sfx_ric2;
sfx_t			*cl_sfx_ric3;
sfx_t			*cl_sfx_r_exp3;

/*
=================
CL_ParseTEnt
=================
*/
void CL_InitTEnts (void)
{
	cl_sfx_wizhit = S_PrecacheSound ("wizard/hit.wav");
	cl_sfx_knighthit = S_PrecacheSound ("hknight/hit.wav");
	cl_sfx_tink1 = S_PrecacheSound ("weapons/tink1.wav");
	cl_sfx_ric1 = S_PrecacheSound ("weapons/ric1.wav");
	cl_sfx_ric2 = S_PrecacheSound ("weapons/ric2.wav");
	cl_sfx_ric3 = S_PrecacheSound ("weapons/ric3.wav");
	cl_sfx_r_exp3 = S_PrecacheSound ("weapons/r_exp3.wav");
}

/*
=================
CL_ParseBeam
=================
*/

static qboolean CL_BeamCanUseQ3P (void)
{
	return !q_strcasecmp (r_particles_mode.string, "q3p")
		|| !q_strcasecmp (r_particles_mode.string, "hybrid");
}

static qboolean CL_BeamResolveAttachment (int entnum, qboolean local_space, const vec3_t fallback, vec3_t out)
{
	/* World-space beams already carry absolute coordinates in fallback. */
	if (!local_space)
	{
		VectorCopy (fallback, out);
		return false;
	}

	if (entnum <= 0 || entnum >= cl.num_entities)
	{
		VectorCopy (fallback, out);
		return false;
	}

	if (!cl_entities[entnum].model)
	{
		VectorCopy (fallback, out);
		return false;
	}

	VectorAdd (cl_entities[entnum].origin, fallback, out);
	return true;
}

void CL_ParseBeam (qmodel_t *m)
{
	int		ent;
	vec3_t	start, end;
	beam_t	*b;
	int		i;

	ent = MSG_ReadShort ();

	start[0] = MSG_ReadCoord (cl.protocolflags);
	start[1] = MSG_ReadCoord (cl.protocolflags);
	start[2] = MSG_ReadCoord (cl.protocolflags);

	end[0] = MSG_ReadCoord (cl.protocolflags);
	end[1] = MSG_ReadCoord (cl.protocolflags);
	end[2] = MSG_ReadCoord (cl.protocolflags);

// override any beam with the same entity
	for (i=0, b=cl_beams ; i< MAX_BEAMS ; i++, b++)
		if (b->entity == ent)
		{
			b->entity = ent;
			b->model = m;
			b->starttime = cl.time - 0.001;
			b->endtime = cl.time + 0.2;
			b->attach_entity = ent;
			b->local_space = false;
			VectorCopy (start, b->start);
			VectorCopy (end, b->end);
			VectorCopy (start, b->start_local);
			VectorCopy (end, b->end_local);
			return;
		}

// find a free beam
	for (i=0, b=cl_beams ; i< MAX_BEAMS ; i++, b++)
	{
		if (!b->model || b->starttime > cl.time || b->endtime < cl.time)
		{
			b->entity = ent;
			b->model = m;
			b->starttime = cl.time - 0.001;
			b->endtime = cl.time + 0.2;
			b->attach_entity = ent;
			b->local_space = false;
			VectorCopy (start, b->start);
			VectorCopy (end, b->end);
			VectorCopy (start, b->start_local);
			VectorCopy (end, b->end_local);
			return;
		}
	}

	//johnfitz -- less spammy overflow message
	if (!dev_overflows.beams || dev_overflows.beams + CONSOLE_RESPAM_TIME < realtime )
	{
		Con_Printf ("Beam list overflow!\n");
		dev_overflows.beams = realtime;
	}
	//johnfitz
}

/*
=================
CL_ParseTEnt
=================
*/
static qboolean CL_DecalNormalFromView (const vec3_t impact, vec3_t out_normal)
{
	trace_t trace;
	vec3_t from, to, dir;
	float len;

	if (!cl.worldmodel)
		return false;

	VectorCopy (cl_entities[cl.viewentity].origin, from);
	VectorSubtract (impact, from, dir);
	len = VectorLength (dir);
	if (len < 0.0001f)
		return false;
	VectorScale (dir, 1.f / len, dir);
	/*
	 * TE impacts are quantized and can end up slightly in front of/behind the
	 * touched plane. Extend the line a little past the impact so hull tracing
	 * can still recover the contacted world normal.
	 */
	VectorMA (impact, 2.f, dir, to);

	memset (&trace, 0, sizeof (trace));
	trace.fraction = 1.f;
	trace.allsolid = true;
	VectorCopy (to, trace.endpos);
	SV_RecursiveHullCheck (cl.worldmodel->hulls, 0, 0, 1, from, to, &trace);

	if (trace.fraction < 1.f && VectorLengthSquared (trace.plane.normal) >= 0.0001f)
	{
		VectorCopy (trace.plane.normal, out_normal);
		VectorNormalizeFast (out_normal);
		return true;
	}

	/* fall back to the legacy approximation when no plane was hit */
	VectorSubtract (from, impact, out_normal);
	if (VectorLengthSquared (out_normal) < 0.0001f)
		return false;
	VectorNormalizeFast (out_normal);
	return true;
}

static qboolean CL_DecalNormalFromImpactTrace (const vec3_t impact, vec3_t out_normal)
{
	static const vec3_t directions[] = {
		{ 1.f, 0.f, 0.f }, {-1.f, 0.f, 0.f },
		{ 0.f, 1.f, 0.f }, { 0.f,-1.f, 0.f },
		{ 0.f, 0.f, 1.f }, { 0.f, 0.f,-1.f },
	};
	const float trace_dist = 8.f;
	qboolean found = false;
	float best_fraction = 2.f;
	int i;

	if (!cl.worldmodel)
		return false;

	for (i = 0; i < (int)countof (directions); ++i)
	{
		trace_t trace;
		vec3_t start, end;

		VectorCopy (impact, start);
		VectorMA (impact, trace_dist, directions[i], end);

		memset (&trace, 0, sizeof(trace));
		trace.fraction = 1.f;
		trace.allsolid = true;
		VectorCopy (end, trace.endpos);
		SV_RecursiveHullCheck (cl.worldmodel->hulls, 0, 0, 1, start, end, &trace);

		if (VectorLengthSquared (trace.plane.normal) < 0.0001f)
			continue;

		if (trace.fraction >= best_fraction)
			continue;

		best_fraction = trace.fraction;
		VectorCopy (trace.plane.normal, out_normal);
		found = true;
	}

	if (found)
		VectorNormalizeFast (out_normal);

	return found;
}

static qboolean CL_DecalNormalForImpact (const vec3_t impact, vec3_t out_normal)
{
	if (CL_DecalNormalFromView (impact, out_normal))
		return true;

	if (CL_DecalNormalFromImpactTrace (impact, out_normal))
		return true;

	return false;
}

static qboolean CL_TEntCanUseQ3P (void)
{
	return !q_strcasecmp (r_particles_mode.string, "q3p")
		|| !q_strcasecmp (r_particles_mode.string, "hybrid");
}

static qboolean CL_TEntTrySpawnQ3P (const char *material, const vec3_t pos, const vec3_t vel_base,
	int count, float lifetime_min, float lifetime_rand, float size, float size_rand,
	float alpha_ramp, float gravity, float drag, int color)
{
	int i;
	q3p_effectdef_t def;
	qboolean has_def = Q3P_GetEffectDef (material, &def);

	if (has_def)
	{
		count = q_max (1, def.count);
		lifetime_min = def.lifetime_min;
		lifetime_rand = def.lifetime_rand;
		size = def.size;
		size_rand = def.size_rand;
		alpha_ramp = def.alpha_ramp;
		gravity = def.gravity;
		drag = def.drag;
		color = def.color;
	}

	if (!CL_TEntCanUseQ3P ())
		return false;

	for (i = 0; i < count; ++i)
	{
		q3p_particle_t p;

		memset (&p, 0, sizeof(p));
		p.spawn_time = cl.time;
		p.lifetime = lifetime_min + ((rand() & 255) / 255.0f) * lifetime_rand;
			p.size = size + ((rand() & 255) / 255.0f) * size_rand;
			p.size_ramp = has_def ? def.size_ramp : p.size * 8.0f;
			p.alpha = has_def ? def.alpha : 1.0f;
			p.alpha_ramp = alpha_ramp;
			p.color = color;
			p.gravity = gravity;
			p.drag = drag;
			p.restitution = has_def ? def.restitution : 0.35f;
			p.min_bounce_speed = has_def ? def.min_bounce_speed : 35.f;
			p.flags = (has_def ? (def.collide_world ? Q3P_PARTICLE_COLLIDE_WORLD : 0) : Q3P_PARTICLE_COLLIDE_WORLD);

			q_strlcpy (p.material, has_def ? def.material : material, sizeof(p.material));

			p.org[0] = pos[0] + ((rand() & 15) - 8) * (has_def ? def.org_jitter / 8.f : 1.f);
			p.org[1] = pos[1] + ((rand() & 15) - 8) * (has_def ? def.org_jitter / 8.f : 1.f);
			p.org[2] = pos[2] + ((rand() & 15) - 8) * (has_def ? def.org_jitter / 8.f : 1.f);

			p.vel[0] = vel_base[0] * (has_def ? def.vel_scale : 1.f) + (float)((rand() & 255) - 128) * (has_def ? def.vel_jitter / 128.f : 1.f);
			p.vel[1] = vel_base[1] * (has_def ? def.vel_scale : 1.f) + (float)((rand() & 255) - 128) * (has_def ? def.vel_jitter / 128.f : 1.f);
			p.vel[2] = vel_base[2] * (has_def ? def.vel_scale : 1.f) + (float)((rand() & 255) - 128) * (has_def ? def.vel_jitter / 128.f : 1.f);

		if (!Q3P_Spawn (&p))
			return false;
	}

	return true;
}

static qboolean CL_TEntRunImpactDispatcher (int type, const vec3_t pos)
{
	static const vec3_t vel_zero = {0.f, 0.f, 0.f};

	switch (type)
	{
	case TE_SPIKE:
		return CL_TEntTrySpawnQ3P ("sparks", pos, vel_zero, 10, 0.16f, 0.08f, 0.9f, 0.4f, -4.0f, 420.f, 0.35f, 0x6f);
	case TE_SUPERSPIKE:
	case TE_GUNSHOT:
		return CL_TEntTrySpawnQ3P ("impact_puff", pos, vel_zero, 20, 0.20f, 0.14f, 1.35f, 0.65f, -3.0f, 260.f, 0.25f, 0x6f);
	case TE_EXPLOSION:
		return CL_TEntTrySpawnQ3P ("debris", pos, vel_zero, 96, 0.50f, 0.24f, 2.2f, 1.8f, -1.4f, 220.f, 0.35f, 0x6f);
	case TE_EXPLOSION2:
		return CL_TEntTrySpawnQ3P ("debris", pos, vel_zero, 96, 0.50f, 0.24f, 2.2f, 1.8f, -1.4f, 220.f, 0.35f, 0x6f);
	default:
		break;
	}

	return false;
}

void CL_ParseTEnt (void)
{
	int		type;
	vec3_t	pos;
	vec3_t	decal_normal;
	dlight_t	*dl;
	int		rnd;
	int		colorStart, colorLength;

	type = MSG_ReadByte ();
	switch (type)
	{
	case TE_WIZSPIKE:			// spike hitting wall
		pos[0] = MSG_ReadCoord (cl.protocolflags);
		pos[1] = MSG_ReadCoord (cl.protocolflags);
		pos[2] = MSG_ReadCoord (cl.protocolflags);
		R_RunParticleEffect (pos, vec3_origin, 20, 30);
		S_StartSound (-1, 0, cl_sfx_wizhit, pos, 1, 1);
		break;

	case TE_KNIGHTSPIKE:			// spike hitting wall
		pos[0] = MSG_ReadCoord (cl.protocolflags);
		pos[1] = MSG_ReadCoord (cl.protocolflags);
		pos[2] = MSG_ReadCoord (cl.protocolflags);
		R_RunParticleEffect (pos, vec3_origin, 226, 20);
		S_StartSound (-1, 0, cl_sfx_knighthit, pos, 1, 1);
		break;

	case TE_SPIKE:			// spike hitting wall
		pos[0] = MSG_ReadCoord (cl.protocolflags);
		pos[1] = MSG_ReadCoord (cl.protocolflags);
		pos[2] = MSG_ReadCoord (cl.protocolflags);
		if (!CL_TEntRunImpactDispatcher (TE_SPIKE, pos))
			R_RunParticleEffect (pos, vec3_origin, 0, 10);
		if (CL_DecalNormalForImpact (pos, decal_normal))
			R_SpawnImpactDecal ("bullet", pos, decal_normal);
		if ( rand() % 5 )
			S_StartSound (-1, 0, cl_sfx_tink1, pos, 1, 1);
		else
		{
			rnd = rand() & 3;
			if (rnd == 1)
				S_StartSound (-1, 0, cl_sfx_ric1, pos, 1, 1);
			else if (rnd == 2)
				S_StartSound (-1, 0, cl_sfx_ric2, pos, 1, 1);
			else
				S_StartSound (-1, 0, cl_sfx_ric3, pos, 1, 1);
		}
		break;
	case TE_SUPERSPIKE:			// super spike hitting wall
		pos[0] = MSG_ReadCoord (cl.protocolflags);
		pos[1] = MSG_ReadCoord (cl.protocolflags);
		pos[2] = MSG_ReadCoord (cl.protocolflags);
		if (!CL_TEntRunImpactDispatcher (TE_SUPERSPIKE, pos))
			R_RunParticleEffect (pos, vec3_origin, 0, 20);
		if (CL_DecalNormalForImpact (pos, decal_normal))
			R_SpawnImpactDecal ("bullet", pos, decal_normal);

		if ( rand() % 5 )
			S_StartSound (-1, 0, cl_sfx_tink1, pos, 1, 1);
		else
		{
			rnd = rand() & 3;
			if (rnd == 1)
				S_StartSound (-1, 0, cl_sfx_ric1, pos, 1, 1);
			else if (rnd == 2)
				S_StartSound (-1, 0, cl_sfx_ric2, pos, 1, 1);
			else
				S_StartSound (-1, 0, cl_sfx_ric3, pos, 1, 1);
		}
		break;

	case TE_GUNSHOT:			// bullet hitting wall
		pos[0] = MSG_ReadCoord (cl.protocolflags);
		pos[1] = MSG_ReadCoord (cl.protocolflags);
		pos[2] = MSG_ReadCoord (cl.protocolflags);
		if (!CL_TEntRunImpactDispatcher (TE_GUNSHOT, pos))
			R_RunParticleEffect (pos, vec3_origin, 0, 20);
		if (CL_DecalNormalForImpact (pos, decal_normal))
			R_SpawnImpactDecal ("bullet", pos, decal_normal);
		break;

	case TE_EXPLOSION:                      // rocket explosion
		pos[0] = MSG_ReadCoord (cl.protocolflags);
		pos[1] = MSG_ReadCoord (cl.protocolflags);
		pos[2] = MSG_ReadCoord (cl.protocolflags);
		V_AddExplosionVibration (pos);
		if (!CL_TEntRunImpactDispatcher (TE_EXPLOSION, pos))
			R_ParticleExplosion (pos);
		if (CL_DecalNormalForImpact (pos, decal_normal))
			R_SpawnImpactDecal ("scorch", pos, decal_normal);
		dl = CL_AllocDlight (0);
		VectorCopy (pos, dl->origin);
		dl->radius = 350;
		dl->baseradius = dl->radius;
		dl->color[0] = 1.00f; dl->color[1] = 0.50f; dl->color[2] = 0.25f;
		dl->die = cl.time + 0.5;
		dl->decay = 300;
		dl->type = DLIGHT_EXPLOSION;
		S_StartSound (-1, 0, cl_sfx_r_exp3, pos, 1, 1);
		break;

	case TE_TAREXPLOSION:                   // tarbaby explosion
		pos[0] = MSG_ReadCoord (cl.protocolflags);
		pos[1] = MSG_ReadCoord (cl.protocolflags);
		pos[2] = MSG_ReadCoord (cl.protocolflags);
		V_AddExplosionVibration (pos);
		R_BlobExplosion (pos);
		if (CL_DecalNormalForImpact (pos, decal_normal))
			R_SpawnImpactDecal ("scorch", pos, decal_normal);

		S_StartSound (-1, 0, cl_sfx_r_exp3, pos, 1, 1);
		break;

	case TE_LIGHTNING1:			     // lightning bolts
		CL_ParseBeam (Mod_ForName("progs/bolt.mdl", true));
		break;

	case TE_LIGHTNING2:			     // lightning bolts
		CL_ParseBeam (Mod_ForName("progs/bolt2.mdl", true));
		break;

	case TE_LIGHTNING3:			     // lightning bolts
		CL_ParseBeam (Mod_ForName("progs/bolt3.mdl", true));
		break;

// PGM 01/21/97
	case TE_BEAM:				// grappling hook beam
		CL_ParseBeam (Mod_ForName("progs/beam.mdl", true));
		break;
// PGM 01/21/97

	case TE_LAVASPLASH:
		pos[0] = MSG_ReadCoord (cl.protocolflags);
		pos[1] = MSG_ReadCoord (cl.protocolflags);
		pos[2] = MSG_ReadCoord (cl.protocolflags);
		R_LavaSplash (pos);
		break;

	case TE_TELEPORT:
		pos[0] = MSG_ReadCoord (cl.protocolflags);
		pos[1] = MSG_ReadCoord (cl.protocolflags);
		pos[2] = MSG_ReadCoord (cl.protocolflags);
		R_TeleportSplash (pos);
		break;

	case TE_EXPLOSION2:				// color mapped explosion
		pos[0] = MSG_ReadCoord (cl.protocolflags);
		pos[1] = MSG_ReadCoord (cl.protocolflags);
		pos[2] = MSG_ReadCoord (cl.protocolflags);
		V_AddExplosionVibration (pos);
		colorStart = MSG_ReadByte ();
		colorLength = MSG_ReadByte ();
		if (!CL_TEntRunImpactDispatcher (TE_EXPLOSION2, pos))
			R_ParticleExplosion2 (pos, colorStart, colorLength);
		if (CL_DecalNormalForImpact (pos, decal_normal))
			R_SpawnImpactDecal ("scorch", pos, decal_normal);
		dl = CL_AllocDlight (0);
		VectorCopy (pos, dl->origin);
		dl->radius = 350;
		dl->baseradius = dl->radius;
		dl->die = cl.time + 0.5;
		dl->decay = 300;
		dl->type = DLIGHT_EXPLOSION;
		S_StartSound (-1, 0, cl_sfx_r_exp3, pos, 1, 1);
		break;

	default:
		Sys_Error ("CL_ParseTEnt: bad type");
	}
}



static void CL_AddQ3PBeamSegments (const beam_t *b, const vec3_t start, const vec3_t end)
{
	vec3_t dir, org;
	float len;
	float step = 20.f;
	int color;

	if (!CL_BeamCanUseQ3P ())
		return;

	if (!b || !b->model)
		return;

	VectorSubtract (end, start, dir);
	len = VectorNormalize (dir);
	if (len <= 0.01f)
		return;

	if (strstr (b->model->name, "progs/bolt"))
		color = 0xe8;
	else
		color = 0x6f;

	VectorCopy (start, org);
	while (len > 0.f)
	{
		q3p_particle_t p;
		memset (&p, 0, sizeof(p));
		p.spawn_time = cl.time;
		p.lifetime = 0.08f;
		p.size = 3.0f;
		p.size_ramp = -1.0f;
		p.alpha = 1.0f;
		p.alpha_ramp = -8.0f;
		p.color = color;
		p.drag = 0.0f;
		p.gravity = 0.0f;
		q_strlcpy (p.material, "lightning", sizeof(p.material));
		VectorCopy (org, p.org);
		if (!Q3P_Spawn (&p))
			break;
		VectorMA (org, step, dir, org);
		len -= step;
	}
}

/*
=================
CL_NewTempEntity
=================
*/
entity_t *CL_NewTempEntity (void)
{
	entity_t	*ent;

	if (cl_numvisedicts == MAX_VISEDICTS)
		return NULL;
	if (num_temp_entities == MAX_TEMP_ENTITIES)
		return NULL;
	ent = &cl_temp_entities[num_temp_entities];
	memset (ent, 0, sizeof(*ent));
	num_temp_entities++;
	cl_visedicts[cl_numvisedicts] = ent;
	cl_numvisedicts++;
	ent->scale = ENTSCALE_DEFAULT;
	ent->colormap = vid.colormap;
	return ent;
}


/*
=================
CL_UpdateTEnts
=================
*/
void CL_UpdateTEnts (void)
{
	int			i, j; //johnfitz -- use j instead of using i twice, so we don't corrupt memory
	beam_t		*b;
	vec3_t		dist, org;
	float		d;
	entity_t	*ent;
	float		yaw, pitch;
	float		forward;

	num_temp_entities = 0;

	srand ((int) (cl.time * 1000)); //johnfitz -- freeze beams when paused

// update lightning
	for (i=0, b=cl_beams ; i< MAX_BEAMS ; i++, b++)
	{
		if (!b->model || b->starttime > cl.time || b->endtime < cl.time)
			continue;

	// resolve attached beam endpoints (worldspace or localspace offsets)
		CL_BeamResolveAttachment (b->attach_entity, b->local_space, b->start_local, b->start);
		CL_BeamResolveAttachment (b->attach_entity, b->local_space, b->end_local, b->end);

	// if coming from the player, keep the start position synced to the view entity
		if (b->entity == cl.viewentity && !b->local_space)
			VectorCopy (cl_entities[cl.viewentity].origin, b->start);

		if (CL_BeamCanUseQ3P ())
		{
			CL_AddQ3PBeamSegments (b, b->start, b->end);
			continue;
		}

	// calculate pitch and yaw
		VectorSubtract (b->end, b->start, dist);

		if (dist[1] == 0 && dist[0] == 0)
		{
			yaw = 0;
			if (dist[2] > 0)
				pitch = 90;
			else
				pitch = 270;
		}
		else
		{
			yaw = (int) (atan2(dist[1], dist[0]) * 180 / M_PI);
			if (yaw < 0)
				yaw += 360;

			forward = sqrt (dist[0]*dist[0] + dist[1]*dist[1]);
			pitch = (int) (atan2(dist[2], forward) * 180 / M_PI);
			if (pitch < 0)
				pitch += 360;
		}

	// add new entities for the lightning
		VectorCopy (b->start, org);
		d = VectorNormalize(dist);
		while (d > 0)
		{
			ent = CL_NewTempEntity ();
			if (!ent)
				return;
			VectorCopy (org, ent->origin);
			ent->model = b->model;
			ent->angles[0] = pitch;
			ent->angles[1] = yaw;
			ent->angles[2] = rand()%360;

			//johnfitz -- use j instead of using i twice, so we don't corrupt memory
			for (j=0 ; j<3 ; j++)
				org[j] += dist[j]*30;
			d -= 30;
		}
	}
}
