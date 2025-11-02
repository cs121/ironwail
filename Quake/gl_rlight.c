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
// r_light.c

#include "quakedef.h"
#include "gl_shadow.h"
#include <string.h> // [CHG] for memcpy, memset
#include <math.h>   // [CHG] for fabsf, modf

// [CHG] small, centralized constants
#define LIGHTCACHE_MOVE_EPS2     1.0f     // squared threshold for cache movement invalidation
#define LIGHT_DUP_POS_EPS2       1e-4f
#define LIGHT_DUP_RADIUS_EPS_REL 0.001f
#define LIGHT_DUP_RADIUS_EPS_MIN 0.01f

enum
{
	BSPX_INDEX_MAP_CAP = MAX_DLIGHTS
};

extern cvar_t r_flatlightstyles; //johnfitz
extern cvar_t r_lerplightstyles;
extern cvar_t r_dynamic;

gpulightbuffer_t r_lightbuffer;

static void R_FillGpuLightFromBspx (const bspx_static_light_t* src, gpulight_t* dst)
{
	// [CHG] defensive checks
	if (!src || !dst)
		return;

	const float intensity = q_max (0.0f, src->intensity);

	VectorCopy (src->origin, dst->pos);
	dst->radius = q_max (0.0f, src->radius);
	dst->color[0] = q_max (0.0f, src->color[0] * intensity);
	dst->color[1] = q_max (0.0f, src->color[1] * intensity);
	dst->color[2] = q_max (0.0f, src->color[2] * intensity);
	dst->minlight = 0.0f;
}

static int R_AppendBspxLights (const bspx_static_light_t* lights, int count, int* index_map, int index_map_cap)
{
	if (!lights || count <= 0)
		return 0;

	int appended = 0;

	// [CHG] initialize index map in one go
	if (index_map && index_map_cap > 0)
		memset (index_map, 0xff, sizeof (index_map[0]) * (size_t)index_map_cap);

	for (int i = 0; i < count; ++i)
	{
		const bspx_static_light_t* src = &lights[i];

		if (!src || src->radius <= 0.0f)
			continue;

		if (r_framedata.numlights >= MAX_DLIGHTS)
			break;

		gpulight_t* dst = &r_lightbuffer.lights[r_framedata.numlights++];
		if (index_map && i < index_map_cap)
			index_map[i] = r_framedata.numlights - 1;

		R_FillGpuLightFromBspx (src, dst);
		appended++;
	}

	return appended;
}

/*
==================
R_AnimateLight
==================
*/
void R_AnimateLight (void)
{
	int			j, k, n;
	double		intpart;
	double		pos_t = cl.time * 10.0;

	// [CHG] modf avoids separate floor cast and subtraction
	double f = modf (pos_t, &intpart);
	if (!r_lerplightstyles.value)
		f = 0.0;

	for (j = 0; j < MAX_LIGHTSTYLES; j++)
	{
		if (!cl_lightstyle[j].length)
		{
			d_lightstylevalue[j] = 256;
			r_lightbuffer.lightstyles[j] = 1.f;
			continue;
		}

		//johnfitz -- r_flatlightstyles
		if (r_flatlightstyles.value == 2)
			k = n = cl_lightstyle[j].peak - 'a';
		else if (r_flatlightstyles.value == 1 || !r_dynamic.value)
			k = n = cl_lightstyle[j].average - 'a';
		else
		{
			const int len = cl_lightstyle[j].length;
			const int idx0 = ((int)intpart) % len;
			int idx1 = idx0 + 1;
			if (idx1 == len) idx1 = 0;

			k = cl_lightstyle[j].map[idx0] - 'a';
			n = cl_lightstyle[j].map[idx1] - 'a';
		}

		// [CHG] only interpolate abrupt changes (e.g. flicker) if >= 2
		if (r_lerplightstyles.value < 2.f && abs (n - k) >= ('m' - 'a') / 2)
			n = k;

		d_lightstylevalue[j] = (int)(k * 22 + (n - k) * 22 * f);
		r_lightbuffer.lightstyles[j] = (k + (n - k) * f) * (22.f / 256.f);
	}

	if (r_fullbright_cheatsafe)
		r_lightbuffer.lightstyles[0] = 1.f;
}

/*
=============================================================================

DYNAMIC LIGHTS

=============================================================================
*/

static GLuint gl_lightclustertexture;

typedef struct gpu_cluster_inputs_s {
	float		transposed_proj[16];
	float		view_matrix[16];
} gpu_cluster_inputs_t;

/*
=============
GLLight_CreateResources
=============
*/
void GLLight_CreateResources (void)
{
	glGenTextures (1, &gl_lightclustertexture);
	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_3D, gl_lightclustertexture);
	GL_ObjectLabelFunc (GL_TEXTURE, gl_lightclustertexture, -1, "light clusters");
	GL_TexImage3DFunc (GL_TEXTURE_3D, 0, GL_RG32UI, LIGHT_TILES_X, LIGHT_TILES_Y, LIGHT_TILES_Z, 0, GL_RG_INTEGER, GL_UNSIGNED_INT, NULL);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
}

/*
=============
GLLight_DeleteResources
=============
*/
void GLLight_DeleteResources (void)
{
	if (gl_lightclustertexture)
	{
		glDeleteTextures (1, &gl_lightclustertexture);
		gl_lightclustertexture = 0;
	}
}

/*
=============
R_PushDlights
=============
*/
void R_PushDlights (void)
{
	GLuint			buf;
	GLbyte* ofs;
	gpu_cluster_inputs_t cluster_inputs;

	int dynamic_count = 0;

	// [CHG] guard worldmodel-dependent counts
	const qboolean have_world = (cl.worldmodel != NULL);
	const int static_light_count = have_world ? cl.worldmodel->bspx_num_static_lights : 0;
	const int static_shadow_light_count = have_world ? cl.worldmodel->bspx_num_static_shadow_lights : 0;
	const int static_light_map_cap = q_min (static_light_count, BSPX_INDEX_MAP_CAP);
	const int static_shadow_light_map_cap = q_min (static_shadow_light_count, BSPX_INDEX_MAP_CAP);

	int static_light_index_map[BSPX_INDEX_MAP_CAP];
	int static_shadow_light_index_map[BSPX_INDEX_MAP_CAP];

	for (int ii = 0; ii < BSPX_INDEX_MAP_CAP; ++ii)
	{
		static_light_index_map[ii] = -1;
		static_shadow_light_index_map[ii] = -1;
	}

	r_framedata.numlights = 0;

	if (r_dynamic.value)
	{
		// [CHG] cull against frustum and lifetime; keep branchless early-outs
		for (int i = 0; i < MAX_DLIGHTS; i++)
		{
			dlight_t* l = &cl_dlights[i];
			if (l->spawn > cl.time)
			{
				l->die = 0.f;
				continue;
			}
			if (l->die < cl.time || !l->radius)
				continue;

			if (r_framedata.numlights >= MAX_DLIGHTS)
				break; // prevent OOB writes

			qboolean cull = false;
			for (int j = 0; j < 4; j++)
			{
				mplane_t* p = &frustum[j];
				if (DotProduct (p->normal, l->origin) - p->dist + l->radius < 0.f)
				{
					cull = true;
					break;
				}
			}
			if (cull)
				continue;

			gpulight_t* out = &r_lightbuffer.lights[r_framedata.numlights++];
			out->pos[0] = l->origin[0];
			out->pos[1] = l->origin[1];
			out->pos[2] = l->origin[2];
			out->radius = l->radius;
			out->color[0] = l->color[0];
			out->color[1] = l->color[1];
			out->color[2] = l->color[2];
			out->minlight = l->minlight;
		}

		dynamic_count = r_framedata.numlights;

		if (have_world)
		{
			R_AppendBspxLights (cl.worldmodel->bspx_static_lights,
				cl.worldmodel->bspx_num_static_lights,
				(static_light_map_cap > 0) ? static_light_index_map : NULL,
				static_light_map_cap);

			R_AppendBspxLights (cl.worldmodel->bspx_static_shadow_lights,
				cl.worldmodel->bspx_num_static_shadow_lights,
				(static_shadow_light_map_cap > 0) ? static_shadow_light_index_map : NULL,
				static_shadow_light_map_cap);
		}
	}

	if (have_world &&
		(cl.worldmodel->bspx_num_static_shadow_lights > 0 ||
			cl.worldmodel->bspx_num_static_shadow_indices > 0))
	{
		gpulight_t shadow_lights[MAX_DLIGHTS];
		int shadow_count = 0;

		// [CHG] copy dynamic first
		for (int i = 0; i < dynamic_count && shadow_count < MAX_DLIGHTS; ++i)
			shadow_lights[shadow_count++] = r_lightbuffer.lights[i];

		// [CHG] then mapped static shadow lights
		if (cl.worldmodel->bspx_static_shadow_lights &&
			cl.worldmodel->bspx_num_static_shadow_lights > 0)
		{
			const int count = cl.worldmodel->bspx_num_static_shadow_lights;
			for (int k = 0; k < count && shadow_count < MAX_DLIGHTS; ++k)
			{
				const int mapped_index = (k < static_shadow_light_map_cap) ?
					static_shadow_light_index_map[k] : -1;

				if (mapped_index < 0 || mapped_index >= r_framedata.numlights)
					continue;

				shadow_lights[shadow_count++] = r_lightbuffer.lights[mapped_index];
			}
		}

		// [CHG] explicit indices that refer into the non-shadow static lights
		if (cl.worldmodel->bspx_static_shadow_indices &&
			cl.worldmodel->bspx_num_static_shadow_indices > 0)
		{
			const int* indices = cl.worldmodel->bspx_static_shadow_indices;
			const int count = cl.worldmodel->bspx_num_static_shadow_indices;
			const int maxlights = cl.worldmodel->bspx_num_static_lights;

			if (cl.worldmodel->bspx_static_lights && maxlights > 0)
			{
				for (int k = 0; k < count && shadow_count < MAX_DLIGHTS; ++k)
				{
					const int idx = indices[k];
					if (idx < 0 || idx >= maxlights)
						continue;

					if (static_light_map_cap <= 0)
						continue;

					const int mapped_index = (idx < static_light_map_cap) ?
						static_light_index_map[idx] : -1;

					if (mapped_index < 0 || mapped_index >= r_framedata.numlights)
						continue;

					// [CHG] duplicate suppression
					qboolean duplicate = false;
					for (int n = dynamic_count; n < shadow_count; ++n)
					{
						const gpulight_t* existing = &shadow_lights[n];
						const gpulight_t* candidate = &r_lightbuffer.lights[mapped_index];

						const float dx = existing->pos[0] - candidate->pos[0];
						const float dy = existing->pos[1] - candidate->pos[1];
						const float dz = existing->pos[2] - candidate->pos[2];
						const float d2 = dx * dx + dy * dy + dz * dz;
						const float rdiff = fabsf (existing->radius - candidate->radius);

						if (d2 <= LIGHT_DUP_POS_EPS2 &&
							rdiff <= q_max (LIGHT_DUP_RADIUS_EPS_MIN, LIGHT_DUP_RADIUS_EPS_REL * candidate->radius))
						{
							duplicate = true;
							break;
						}
					}
					if (duplicate)
						continue;

					if (shadow_count < MAX_DLIGHTS)
						shadow_lights[shadow_count++] = r_lightbuffer.lights[mapped_index];
				}
			}
		}

		R_ShadowSyncWorldLights (shadow_lights, shadow_count);
	}
	else
	{
		R_ShadowSyncWorldLights (r_lightbuffer.lights, r_framedata.numlights);
	}

	GL_BeginGroup ("Light clustering");

	R_UploadFrameData ();

	// [CHG] compute transpose once
	for (int i = 0; i < 16; i++)
		cluster_inputs.transposed_proj[i] = r_matproj[((i & 3) << 2) | (i >> 2)];
	memcpy (cluster_inputs.view_matrix, r_matview, 16 * sizeof (float));

	GL_UseProgram (glprogs.cluster_lights);
	GL_Upload (GL_UNIFORM_BUFFER, &cluster_inputs, sizeof (cluster_inputs), &buf, &ofs);
	GL_BindBufferRange (GL_UNIFORM_BUFFER, 1, buf, (GLintptr)ofs, sizeof (cluster_inputs));
	GL_BindImageTextureFunc (0, gl_lightclustertexture, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RG32UI);
	GL_DispatchComputeFunc ((LIGHT_TILES_X + 7) / 8, (LIGHT_TILES_Y + 7) / 8, LIGHT_TILES_Z);
	GL_MemoryBarrierFunc (GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	GL_BindImageTextureFunc (0, gl_lightclustertexture, 0, GL_TRUE, 0, GL_READ_ONLY, GL_RG32UI);

	GL_EndGroup ();
}


/*
=============================================================================

LIGHT SAMPLING

=============================================================================
*/

vec3_t lightcolor; //johnfitz -- lit support via lordhavoc

static void InterpolateLightmap (vec3_t color, msurface_t* surf, int ds, int dt)
{
	// [CHG] defensive checks
	if (!color || !surf || !surf->samples)
	{
		if (color) { color[0] = color[1] = color[2] = 0.f; }
		return;
	}

	byte* lightmap;
	int maps, line3, dsfrac = ds & 15, dtfrac = dt & 15;
	int r00 = 0, g00 = 0, b00 = 0, r01 = 0, g01 = 0, b01 = 0, r10 = 0, g10 = 0, b10 = 0, r11 = 0, g11 = 0, b11 = 0;
	int scale;

	line3 = ((surf->extents[0] >> 4) + 1) * 3;

	lightmap = surf->samples + ((dt >> 4) * ((surf->extents[0] >> 4) + 1) + (ds >> 4)) * 3; // LordHavoc: *3 for color

	for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
	{
		scale = d_lightstylevalue[surf->styles[maps]];
		r00 += lightmap[0] * scale; g00 += lightmap[1] * scale; b00 += lightmap[2] * scale;
		r01 += lightmap[3] * scale; g01 += lightmap[4] * scale; b01 += lightmap[5] * scale;
		r10 += lightmap[line3 + 0] * scale; g10 += lightmap[line3 + 1] * scale; b10 += lightmap[line3 + 2] * scale;
		r11 += lightmap[line3 + 3] * scale; g11 += lightmap[line3 + 4] * scale; b11 += lightmap[line3 + 5] * scale;
		lightmap += ((surf->extents[0] >> 4) + 1) * ((surf->extents[1] >> 4) + 1) * 3; // LordHavoc: *3 for colored lighting
	}

	// Bilinear in int, then scale
	const int r0 = (((r01 - r00) * dsfrac) >> 4) + r00;
	const int r1 = (((r11 - r10) * dsfrac) >> 4) + r10;
	const int g0 = (((g01 - g00) * dsfrac) >> 4) + g00;
	const int g1 = (((g11 - g10) * dsfrac) >> 4) + g10;
	const int b0 = (((b01 - b00) * dsfrac) >> 4) + b00;
	const int b1 = (((b11 - b10) * dsfrac) >> 4) + b10;

	color[0] = (((r1 - r0) * dtfrac) >> 4) + r0; color[0] *= (1.f / 256.f);
	color[1] = (((g1 - g0) * dtfrac) >> 4) + g0; color[1] *= (1.f / 256.f);
	color[2] = (((b1 - b0) * dtfrac) >> 4) + b0; color[2] *= (1.f / 256.f);
}

/*
=============
RecursiveLightPoint -- johnfitz -- replaced entire function for lit support via lordhavoc
=============
*/
int RecursiveLightPoint (lightcache_t* cache, mnode_t* node, vec3_t rayorg, vec3_t start, vec3_t end, float* maxdist)
{
	if (!node)
		return false;

	float		front, back, frac;
	vec3_t		mid;

loc0:
	if (node->contents < 0)
		return false;		// didn't hit anything

	// calculate mid point
	if (node->plane->type < 3)
	{
		front = start[node->plane->type] - node->plane->dist;
		back = end[node->plane->type] - node->plane->dist;
	}
	else
	{
		front = DotProduct (start, node->plane->normal) - node->plane->dist;
		back = DotProduct (end, node->plane->normal) - node->plane->dist;
	}

	// LordHavoc: optimized recursion
	if ((back < 0) == (front < 0))
	{
		node = node->children[front < 0];
		if (!node) return false;
		goto loc0;
	}

	frac = front / (front - back);
	mid[0] = start[0] + (end[0] - start[0]) * frac;
	mid[1] = start[1] + (end[1] - start[1]) * frac;
	mid[2] = start[2] + (end[2] - start[2]) * frac;

	// go down front side
	if (RecursiveLightPoint (cache, node->children[front < 0], rayorg, start, mid, maxdist))
		return true;	// hit something
	else
	{
		unsigned int i;
		int ds, dt;
		msurface_t* surf;

		// check for impact on this node
		surf = cl.worldmodel->surfaces + node->firstsurface;
		for (i = 0; i < node->numsurfaces; i++, surf++)
		{
			float sfront, sback, dist;
			vec3_t raydelta;

			if (surf->flags & SURF_DRAWTILED)
				continue;	// no lightmaps

			// ericw -- added double casts to force 64-bit precision.
			// Without them the zombie at the start of jam3_ericw.bsp was
			// incorrectly being lit up in SSE builds.
			ds = (int)((double)DoublePrecisionDotProduct (mid, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3]);
			dt = (int)((double)DoublePrecisionDotProduct (mid, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3]);

			if (ds < surf->texturemins[0] || dt < surf->texturemins[1])
				continue;

			ds -= surf->texturemins[0];
			dt -= surf->texturemins[1];

			if (ds > surf->extents[0] || dt > surf->extents[1])
				continue;

			if (surf->plane->type < 3)
			{
				sfront = rayorg[surf->plane->type] - surf->plane->dist;
				sback = end[surf->plane->type] - surf->plane->dist;
			}
			else
			{
				sfront = DotProduct (rayorg, surf->plane->normal) - surf->plane->dist;
				sback = DotProduct (end, surf->plane->normal) - surf->plane->dist;
			}
			VectorSubtract (end, rayorg, raydelta);
			dist = sfront / (sfront - sback) * VectorLength (raydelta);

			if (!surf->samples)
			{
				// Wir haben eine surface, die lightmapped ist, aber keine Samples hat.
				// Suche weiter nach einer nahegelegenen gültigen Surface, um schwarze Spots zu vermeiden.
				const float nearby = 8.f;
				dist += nearby;
				*maxdist = q_min (*maxdist, dist);
				continue;
			}

			if (dist < *maxdist)
			{
				cache->surfidx = (int)(surf - cl.worldmodel->surfaces) + 1;
				cache->ds = ds;
				cache->dt = dt;
			}
			else
			{
				cache->surfidx = -1;
			}

			return true; // success
		}

		// go down back side
		return RecursiveLightPoint (cache, node->children[front >= 0], rayorg, mid, end, maxdist);
	}
}

/*
=============
R_LightPoint -- johnfitz -- replaced entire function for lit support via lordhavoc
=============
*/
int R_LightPoint (vec3_t p, float ofs, lightcache_t* cache)
{
	if (!cl.worldmodel || !cl.worldmodel->lightdata)
	{
		lightcolor[0] = lightcolor[1] = lightcolor[2] = 255;
		return 255;
	}

	vec3_t		start, end;
	float		maxdist = 8192.f; //johnfitz -- was 2048

	start[0] = p[0];
	start[1] = p[1];
	start[2] = p[2] + ofs;
	end[0] = start[0];
	end[1] = start[1];
	end[2] = start[2] - maxdist;

	lightcolor[0] = lightcolor[1] = lightcolor[2] = 0;

	if (!cache)
	{
		lightcache_t local = { 0 };
		cache = &local;
	}

	if (cache->surfidx <= 0 // no cache or pitch black
		|| cache->surfidx > cl.worldmodel->numsurfaces)
	{
		cache->surfidx = 0;
		VectorCopy (p, cache->pos);
		RecursiveLightPoint (cache, cl.worldmodel->nodes, start, start, end, &maxdist);
	}
	else
	{
		const float dx = cache->pos[0] - p[0];
		const float dy = cache->pos[1] - p[1];
		const float dz = cache->pos[2] - p[2];
		if (dx * dx + dy * dy + dz * dz >= LIGHTCACHE_MOVE_EPS2) {
			cache->surfidx = 0;
			VectorCopy (p, cache->pos);
			RecursiveLightPoint (cache, cl.worldmodel->nodes, start, start, end, &maxdist);
		}
	}

	if (cache->surfidx > 0)
		InterpolateLightmap (lightcolor, cl.worldmodel->surfaces + cache->surfidx - 1, cache->ds, cache->dt);

	return (int)((lightcolor[0] + lightcolor[1] + lightcolor[2]) * (1.0f / 3.0f));
}
