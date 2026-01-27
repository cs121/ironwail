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

#include "quakedef.h"
#include "r_godray_emit.h"
#include "r_godrayvol.h"

typedef struct godray_volume_vertex_s
{
	vec3_t pos;
} godray_volume_vertex_t;

static cvar_t r_godrays_volumes_enable = { "r_godrays_volumes", "0", CVAR_ARCHIVE };
static cvar_t r_godrays_volume_steps = { "r_godrays_volume_steps", "24", CVAR_ARCHIVE };
static cvar_t r_godrays_volume_noise_scale = { "r_godrays_volume_noise_scale", "0.03", CVAR_ARCHIVE };
static cvar_t r_godrays_volume_noise_amount = { "r_godrays_volume_noise_amount", "0.6", CVAR_ARCHIVE };
static cvar_t r_godrays_volume_debug = { "r_godrays_volume_debug", "0", CVAR_ARCHIVE };

static void R_GodrayVolume_TransformPoint (const float matrix[16], const vec3_t in, vec3_t out)
{
	out[0] = matrix[0] * in[0] + matrix[4] * in[1] + matrix[8] * in[2] + matrix[12];
	out[1] = matrix[1] * in[0] + matrix[5] * in[1] + matrix[9] * in[2] + matrix[13];
	out[2] = matrix[2] * in[0] + matrix[6] * in[1] + matrix[10] * in[2] + matrix[14];
}

static void R_GodrayVolume_TransformVector (const float matrix[16], const vec3_t in, vec3_t out)
{
	out[0] = matrix[0] * in[0] + matrix[4] * in[1] + matrix[8] * in[2];
	out[1] = matrix[1] * in[0] + matrix[5] * in[1] + matrix[9] * in[2];
	out[2] = matrix[2] * in[0] + matrix[6] * in[1] + matrix[10] * in[2];
}


void R_GodrayVolume_Init (void)
{
	Cvar_RegisterVariable (&r_godrays_volumes_enable);
	Cvar_RegisterVariable (&r_godrays_volume_steps);
	Cvar_RegisterVariable (&r_godrays_volume_noise_scale);
	Cvar_RegisterVariable (&r_godrays_volume_noise_amount);
	Cvar_RegisterVariable (&r_godrays_volume_debug);
}

void R_GodrayVolume_Render (void)
{
	static const GLushort box_indices[36] = {
		0, 1, 2, 0, 2, 3,
		4, 5, 6, 4, 6, 7,
		0, 1, 5, 0, 5, 4,
		1, 2, 6, 1, 6, 5,
		2, 3, 7, 2, 7, 6,
		3, 0, 4, 3, 4, 7
	};
	GLuint buf;
	GLbyte *ofs;
	int volume_emitters = 0;
	int postfx_emitters = 0;
	const godray_emitter_t *emitters = NULL;

	if (r_godrays_volumes_enable.value <= 0.f)
		return;
	if (!glprogs.godray_volume)
		return;

	R_GodrayEmitters_GetRenderCounts (&volume_emitters, &postfx_emitters);
	emitters = R_GodrayEmitters_GetList (NULL);
	if (volume_emitters <= 0 || !emitters)
	{
		R_GodrayEmitters_SetRenderedCounts (0, postfx_emitters);
		return;
	}

	GL_BeginGroup ("Godray volumes");
	int debug_mode = (int)Q_rint (r_godrays_volume_debug.value);
	debug_mode = CLAMP (0, debug_mode, 3);

	if (debug_mode > 0 && glprogs.godray_volume_debug)
	{
		GL_UseProgram (glprogs.godray_volume_debug);
		if (debug_mode == 1 || debug_mode == 2)
			GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (1));
		else
			GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (1));
	}
	else
	{
		GL_UseProgram (glprogs.godray_volume);
		GL_SetState (GLS_BLEND_ADD | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (1));
		debug_mode = 0;
	}

	int steps = (int)Q_rint (r_godrays_volume_steps.value);
	steps = CLAMP (8, steps, 64);
	if (debug_mode == 0)
		GL_Uniform1iFunc (8, steps);

	if (debug_mode == 3)
		glPolygonMode (GL_FRONT_AND_BACK, GL_LINE);

	float noise_scale = q_max (0.0001f, r_godrays_volume_noise_scale.value);
	float noise_amount = CLAMP (0.f, r_godrays_volume_noise_amount.value, 1.f);
	for (int i = 0; i < volume_emitters; ++i)
	{
		const godray_emitter_t *emitter = &emitters[i];
		godray_volume_vertex_t verts[8];
		vec3_t base_corners[4];
		vec3_t origin_vs;
		vec3_t axis_t_vs;
		vec3_t axis_b_vs;
		vec3_t axis_r_vs;
		vec3_t ray_dir_vs;

		VectorMA (emitter->center_ws, emitter->mins[0], emitter->axis_t, base_corners[0]);
		VectorMA (base_corners[0], emitter->mins[1], emitter->axis_b, base_corners[0]);
		VectorMA (emitter->center_ws, emitter->maxs[0], emitter->axis_t, base_corners[1]);
		VectorMA (base_corners[1], emitter->mins[1], emitter->axis_b, base_corners[1]);
		VectorMA (emitter->center_ws, emitter->maxs[0], emitter->axis_t, base_corners[2]);
		VectorMA (base_corners[2], emitter->maxs[1], emitter->axis_b, base_corners[2]);
		VectorMA (emitter->center_ws, emitter->mins[0], emitter->axis_t, base_corners[3]);
		VectorMA (base_corners[3], emitter->maxs[1], emitter->axis_b, base_corners[3]);

		VectorCopy (base_corners[0], verts[0].pos);
		VectorCopy (base_corners[1], verts[1].pos);
		VectorCopy (base_corners[2], verts[2].pos);
		VectorCopy (base_corners[3], verts[3].pos);
		for (int v = 0; v < 4; ++v)
		{
			VectorMA (base_corners[v], emitter->ray_length, emitter->axis_r, verts[v + 4].pos);
		}

		R_GodrayVolume_TransformPoint (r_matview, emitter->center_ws, origin_vs);
		R_GodrayVolume_TransformVector (r_matview, emitter->axis_t, axis_t_vs);
		R_GodrayVolume_TransformVector (r_matview, emitter->axis_b, axis_b_vs);
		R_GodrayVolume_TransformVector (r_matview, emitter->axis_r, axis_r_vs);
		R_GodrayVolume_TransformVector (r_matview, emitter->ray_dir_ws, ray_dir_vs);
		VectorNormalize (axis_t_vs);
		VectorNormalize (axis_b_vs);
		VectorNormalize (axis_r_vs);
		VectorNormalize (ray_dir_vs);

		if (debug_mode == 0)
		{
			GL_Uniform3fFunc (0, origin_vs[0], origin_vs[1], origin_vs[2]);
			GL_Uniform3fFunc (1, axis_t_vs[0], axis_t_vs[1], axis_t_vs[2]);
			GL_Uniform3fFunc (2, axis_b_vs[0], axis_b_vs[1], axis_b_vs[2]);
			GL_Uniform3fFunc (3, axis_r_vs[0], axis_r_vs[1], axis_r_vs[2]);
			GL_Uniform3fFunc (4, emitter->mins[0], emitter->mins[1], emitter->mins[2]);
			GL_Uniform3fFunc (5, emitter->maxs[0], emitter->maxs[1], emitter->maxs[2]);
			GL_Uniform4fFunc (6, emitter->color[0], emitter->color[1],
				emitter->color[2], emitter->density);
			GL_Uniform4fFunc (7, noise_scale, noise_amount, emitter->intensity, 0.f);
		}
		else
		{
			GL_Uniform3fFunc (9, ray_dir_vs[0], ray_dir_vs[1], ray_dir_vs[2]);
			GL_Uniform1iFunc (10, debug_mode);
		}

		GL_Upload (GL_ARRAY_BUFFER, verts, sizeof (verts), &buf, &ofs);
		GL_BindBuffer (GL_ARRAY_BUFFER, buf);
		GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof (verts[0]), ofs + offsetof (godray_volume_vertex_t, pos));

		GL_Upload (GL_ELEMENT_ARRAY_BUFFER, box_indices, sizeof (box_indices), &buf, &ofs);
		GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, buf);
		glDrawElements (GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, ofs);
	}

	if (debug_mode == 3)
		glPolygonMode (GL_FRONT_AND_BACK, GL_FILL);

	R_GodrayEmitters_SetRenderedCounts (volume_emitters, postfx_emitters);
	GL_EndGroup ();
}
