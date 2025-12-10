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
#include "../common/lightgrid.h"
#include "gl_lightgrid.h"
#include <float.h>
#include <math.h>

extern cvar_t r_flatlightstyles; //johnfitz
extern cvar_t r_lerplightstyles;
extern cvar_t r_dynamic;
extern cvar_t r_lightgrid;
extern cvar_t r_lightgrid_force;
extern cvar_t r_rgblighting_enable;

gpulightbuffer_t r_lightbuffer;
float r_lightstyle_framefrac;

static qboolean R_IsFinite (float v)
{
#if defined(_MSC_VER)
        return _finite(v) != 0;
#else
        return isfinite(v);
#endif
}

int RecursiveLightPoint (qmodel_t *model, lightcache_t *cache, mnode_t *node, vec3_t rayorg, vec3_t start, vec3_t end, float *maxdist);
static qboolean R_LightgridEnabledInternal (const lightgrid_t *lg)
{
        if (r_lightgrid.value <= 0.f)
                return false;

        if (lg && lg->probes && lg->cellsize > 0.f)
                return true;

        return r_lightgrid_force.value > 0.f;
}

/*
==================
R_AnimateLight
==================
*/
void R_AnimateLight (void)
{
	int			i,j,k,n;
	double		f,base;

//
// light animations
// 'm' is normal light, 'a' is no light, 'z' is double bright
	f = cl.time * 10.0;
	base = floor(f);
	i = (int)base;
        f -= base;
        if (!r_lerplightstyles.value)
                f = 0.0;
        r_lightstyle_framefrac = (float)f;

	for (j=0 ; j<MAX_LIGHTSTYLES ; j++)
	{
		if (!cl_lightstyle[j].length)
		{
                        d_lightstylevalue[j] = 256;
                        r_lightbuffer.lightstyles[j * 2 + 0] = 1.f;
                        r_lightbuffer.lightstyles[j * 2 + 1] = 1.f;
                        continue;
                }
		//johnfitz -- r_flatlightstyles
		if (r_flatlightstyles.value == 2)
			k = n = cl_lightstyle[j].peak - 'a';
		else if (r_flatlightstyles.value == 1 || !r_dynamic.value)
			k = n = cl_lightstyle[j].average - 'a';
		else
		{
			k = i % cl_lightstyle[j].length;
			n = k + 1;
			if (n == cl_lightstyle[j].length)
				n = 0;
			k = cl_lightstyle[j].map[k] - 'a';
			n = cl_lightstyle[j].map[n] - 'a';
		}
		// only interpolate abrupt changes (e.g. flickering light in e1m1) if r_lerplightstyles >= 2
		if (r_lerplightstyles.value < 2.f && abs(n - k) >= ('m' - 'a') / 2)
			n = k;
                d_lightstylevalue[j] = (int)(k*22 + (n-k)*22*f);
                r_lightbuffer.lightstyles[j * 2 + 0] = k * (22.f/256.f);
                r_lightbuffer.lightstyles[j * 2 + 1] = n * (22.f/256.f);
                //johnfitz
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

const vec3_t *R_GetDynamicLightTemperature (int type)
{
	static const vec3_t temps[DLIGHT_MAX_TYPES] = {
		{ 1.0f, 1.0f, 1.0f }, // default
		{ 1.25f, 0.90f, 0.65f }, // rocket
		{ 1.30f, 1.10f, 1.50f }, // plasma
		{ 0.60f, 0.75f, 1.40f }, // lightning
		{ 1.30f, 1.00f, 0.50f }, // explosion
		{ 1.30f, 1.10f, 0.80f }, // torch
		{ 1.00f, 0.60f, 1.50f }, // teleport
	};

	if (type < 0 || type >= DLIGHT_MAX_TYPES)
		type = DLIGHT_DEFAULT;

	return &temps[type];
}

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
	glDeleteTextures (1, &gl_lightclustertexture);
	gl_lightclustertexture = 0;
}

/*
=============
R_PushDlights
=============
*/
void R_PushDlights (void)
{
	int				i, j;
	GLuint			buf;
	GLbyte			*ofs;
	gpu_cluster_inputs_t cluster_inputs;

	r_framedata.numlights = 0;

	if (r_dynamic.value)
	{
		dlight_t *l;
		for (i = 0, l = cl_dlights; i < MAX_DLIGHTS; i++, l++)
		{
			gpulight_t *out;
                        qboolean cull = false;
                        float radius;

                        if (l->spawn > cl.time)
                        {
                                l->die = 0.f;
                                continue;
                        }

                        if (l->die < cl.time || !l->baseradius)
                                continue;

                        radius = l->baseradius * (1.f + 0.1f * (float) sin (cl.time * 9.0 + l->flicker_seed));
                        radius = q_max (radius, 0.f);
                        l->radius = radius;

                        for (j = 0; j < 4; j++)
                        {
                                mplane_t *p = &frustum[j];
                                if (DotProduct (p->normal, l->origin) - p->dist + radius < 0.f)
                                {
                                        cull = true;
                                        break;
                                }
                        }
                        if (cull)
                                continue;

                        out = &r_lightbuffer.lights[r_framedata.numlights++];
                        const vec3_t *temp = R_GetDynamicLightTemperature (l->type);
                        float radiusFactor = q_min (1.f, q_max (radius / 350.f, 0.2f));
                        float flicker = 1.f + (float)sin (cl.time * 15.0 + l->key) * 0.1f;
                        vec3_t finalcolor;
                        finalcolor[0] = l->color[0] * (*temp)[0] * radiusFactor;
                        finalcolor[1] = l->color[1] * (*temp)[1] * radiusFactor;
                        finalcolor[2] = l->color[2] * (*temp)[2] * radiusFactor;
                        finalcolor[0] *= flicker;
                        finalcolor[1] *= flicker;
                        finalcolor[2] *= flicker;
                        if (l->type == DLIGHT_TORCH)
                        {
                                float colorshift = (float)sin (cl.time * 11.0 + l->key) * 0.1f;
                                finalcolor[0] *= 1.0f + colorshift;
                                finalcolor[1] *= 1.0f - colorshift;
                        }
                        out->pos[0]   = l->origin[0];
                        out->pos[1]   = l->origin[1];
                        out->pos[2]   = l->origin[2];
                        out->radius   = radius;
                        out->color[0] = finalcolor[0];
                        out->color[1] = finalcolor[1];
                        out->color[2] = finalcolor[2];
                        out->minlight = l->minlight;
                }
        }

	GL_BeginGroup ("Light clustering");

	R_UploadFrameData ();

	for (i = 0; i < 16; i++)
		cluster_inputs.transposed_proj[i] = r_matproj[((i & 3) << 2) | (i >> 2)];
	memcpy (cluster_inputs.view_matrix, r_matview, 16 * sizeof (float));

	GL_UseProgram (glprogs.cluster_lights);
	GL_Upload (GL_UNIFORM_BUFFER, &cluster_inputs, sizeof (cluster_inputs), &buf, &ofs);
	GL_BindBufferRange (GL_UNIFORM_BUFFER, 1, buf, (GLintptr) ofs, sizeof (cluster_inputs));
	GL_BindImageTextureFunc (0, gl_lightclustertexture, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RG32UI);
	GL_DispatchComputeFunc ((LIGHT_TILES_X+7)/8, (LIGHT_TILES_Y+7)/8, LIGHT_TILES_Z);
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

/*
==================
R_LightgridEnabled
==================
*/
qboolean R_LightgridEnabled (void)
{
        return R_LightgridEnabledInternal (Lightgrid_Get ());
}

/*
==================
R_LightgridLighting
==================
*/
void R_LightgridLighting (const vec3_t pos, vec3_t out_color)
{
        vec3_t dummy_dir;

        R_LightgridLightingDir (pos, out_color, dummy_dir);
}

/*
==================
R_LightgridLightingDir
==================
*/
void R_LightgridLightingDir (const vec3_t pos, vec3_t out_color, vec3_t out_dir)
{
        const lightgrid_t *lg = Lightgrid_Get ();

        if (!R_LightgridEnabledInternal (lg))
        {
                VectorClear (out_color);
                VectorSet (out_dir, 0.f, 0.f, 1.f);
                return;
        }

        Lightgrid_Sample (pos, out_color, out_dir);
}

/*
==================
R_AddDynamicLights_Lightgrid
==================
*/
void R_AddDynamicLights_Lightgrid (const vec3_t pos, vec3_t lightcolor)
{
        int i;

        if (!r_dynamic.value)
                return;

        for (i = 0; i < MAX_DLIGHTS; i++)
        {
                const dlight_t *l = &cl_dlights[i];
                vec3_t dist;
                float add;

                if (l->die < cl.time || l->spawn > cl.time || !l->baseradius)
                        continue;

                VectorSubtract (pos, l->origin, dist);
                add = l->radius - VectorLength (dist);

                if (add <= l->minlight)
                        continue;

                add -= l->minlight;
                VectorMA (lightcolor, add, l->color, lightcolor);
        }
}

static inline int LightStyleValue (unsigned short style)
{
	if (style < 256)
		return d_lightstylevalue[style];

	return d_lightstylevalue[0];
}

static void InterpolateLightmap (qmodel_t *model, vec3_t color, msurface_t *surf, int ds, int dt, qboolean use_rgb)
{
        const byte *samples;
        int smax, tmax;
        int dsfrac = ds & 15, dtfrac = dt & 15;
        float fsfrac = dsfrac * (1.f / 16.f);
        float ftfrac = dtfrac * (1.f / 16.f);
        int bytes_per_pixel;

        if (!surf->samples)
        {
                VectorClear(color);
                return;
        }

        samples = surf->samples;
        bytes_per_pixel = (model && (model->flags & MOD_HDRLIGHTING)) ? 4 : 3;

        if (use_rgb && model && model->lightdata && model->lightdata_rgb)
        {
                const int bytes_per_sample = bytes_per_pixel;
                const ptrdiff_t offset = samples - model->lightdata;

                if (bytes_per_sample > 0 && offset >= 0 && (offset % bytes_per_sample) == 0)
                {
                        const size_t sample_offset = (size_t)offset / (size_t)bytes_per_sample;
                        const size_t rgb_offset = sample_offset * 3;

                        if (rgb_offset + 3 <= (size_t)model->lightdata_rgb_size)
                        {
                                samples = model->lightdata_rgb + rgb_offset;
                                bytes_per_pixel = 3;
                        }
                }
        }

        smax = (surf->extents[0] >> 4) + 1;
        tmax = (surf->extents[1] >> 4) + 1;

        const int s0 = ds >> 4;
        const int t0 = dt >> 4;
        const int stride = smax * bytes_per_pixel;
        const int facesize = smax * tmax * bytes_per_pixel;

        VectorClear(color);

        for (int maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != INVALID_LIGHTSTYLE; maps++)
        {
                const float scale = LightStyleValue(surf->styles[maps]) * (1.f / 256.f);
                const byte *lightmap = samples + facesize * maps;

                const byte *row0 = lightmap + t0 * stride;
                const byte *row1 = (t0 + 1 < tmax) ? row0 + stride : row0;
                const byte *col00 = row0 + s0 * bytes_per_pixel;
                const byte *col01 = (s0 + 1 < smax) ? col00 + bytes_per_pixel : col00;
                const byte *col10 = row1 + s0 * bytes_per_pixel;
                const byte *col11 = (s0 + 1 < smax) ? col10 + bytes_per_pixel : col10;

                float w00 = (1.f - fsfrac) * (1.f - ftfrac);
                float w01 = fsfrac * (1.f - ftfrac);
                float w10 = (1.f - fsfrac) * ftfrac;
                float w11 = fsfrac * ftfrac;

                color[0] += scale * (w00 * col00[0] + w01 * col01[0] + w10 * col10[0] + w11 * col11[0]);
                color[1] += scale * (w00 * col00[1] + w01 * col01[1] + w10 * col10[1] + w11 * col11[1]);
                color[2] += scale * (w00 * col00[2] + w01 * col01[2] + w10 * col10[2] + w11 * col11[2]);
        }
}

static void R_SurfaceFallbackColor(const qmodel_t *model, const msurface_t *surf, vec3_t out)
{
        const texture_t *tex = NULL;

        if (model && surf && surf->texinfo && surf->texinfo->texnum >= 0 && surf->texinfo->texnum < model->numtextures)
                tex = model->textures[surf->texinfo->texnum];

        VectorSet(out, 127.f, 127.f, 127.f);

        if (!tex)
                return;

        switch (tex->type)
        {
        case TEXTYPE_LAVA:
                VectorSet(out, 255.f, 128.f, 32.f);
                break;
        case TEXTYPE_SLIME:
                VectorSet(out, 64.f, 200.f, 96.f);
                break;
        case TEXTYPE_WATER:
                VectorSet(out, 64.f, 96.f, 196.f);
                break;
        default:
                break;
        }
}

static void R_ClampSampleColor(vec3_t color)
{
        for (int i = 0; i < 3; i++)
        {
                if (!R_IsFinite(color[i]) || color[i] < 0.f)
                        color[i] = 0.f;
        }

        if (VectorLength(color) < 1e-6f)
                VectorClear(color);
}

static qboolean R_AdjustPointForLeaf (qmodel_t *model, vec3_t point)
{
        mleaf_t *leaf;

        for (int i = 0; i < 8; i++)
        {
                leaf = Mod_PointInLeaf (point, model);

                if (!leaf || leaf->contents != CONTENTS_SOLID)
                        return true;

                point[2] += 1.f;
        }

        return leaf && leaf->contents != CONTENTS_SOLID;
}

static void R_SamplePointInternal (qmodel_t *model, const vec3_t pos, float ofs, qboolean use_rgblight, lightcache_t *cache, vec3_t out_color)
{
        vec3_t start, end;
        float maxdist = 8192.f;
        lightcache_t local_cache;

        if (!cache)
        {
                memset (&local_cache, 0, sizeof(local_cache));
                cache = &local_cache;
        }

        start[0] = pos[0];
        start[1] = pos[1];
        start[2] = pos[2] + ofs;
        end[0] = start[0];
        end[1] = start[1];
        end[2] = start[2] - maxdist;

        VectorClear (out_color);

        if (cache->surfidx <= 0 // no cache or pitch black
                || (model && cache->surfidx > model->numsurfaces)
                || fabsf (cache->pos[0] - pos[0]) >= 1.f
                || fabsf (cache->pos[1] - pos[1]) >= 1.f
                || fabsf (cache->pos[2] - pos[2]) >= 1.f)
        {
                cache->surfidx = 0;
                VectorCopy (pos, cache->pos);
                RecursiveLightPoint (model, cache, model->nodes, start, start, end, &maxdist);
        }

        if (cache->surfidx > 0)
        {
                InterpolateLightmap (model, out_color, model->surfaces + cache->surfidx - 1, cache->ds, cache->dt, use_rgblight);
                R_ClampSampleColor (out_color);
        }
}

qboolean R_SampleLightmapAtPoint(const vec3_t pos, vec3_t out_rgb)
{
        qmodel_t *model = cl.worldmodel;
        qboolean use_rgblight;
        qboolean found = false;
        float best_dist = FLT_MAX;

        VectorClear(out_rgb);

        if (!model || !model->lightdata)
                return false;

        use_rgblight = model->has_lightdata_rgb && r_rgblighting_enable.value;

        vec3_t pos_copy;
        VectorCopy(pos, pos_copy);
        mleaf_t *leaf = Mod_PointInLeaf(pos_copy, model);
        if (!leaf || leaf->contents == CONTENTS_SOLID)
                return false;

        for (int i = 0; i < leaf->nummarksurfaces; i++)
        {
                msurface_t *surf = &model->surfaces[leaf->firstmarksurface[i]];

                if (!surf->texinfo || (surf->flags & SURF_DRAWTILED))
                        continue;

                const float pdist = DotProduct(pos, surf->plane->normal) - surf->plane->dist;
                if (fabsf(pdist) > 8.f)
                        continue;

                int ds = (int)((double)DoublePrecisionDotProduct(pos, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3]);
                int dt = (int)((double)DoublePrecisionDotProduct(pos, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3]);

                if (ds < surf->texturemins[0] || dt < surf->texturemins[1])
                        continue;

                ds -= surf->texturemins[0];
                dt -= surf->texturemins[1];

                if (ds > surf->extents[0] || dt > surf->extents[1])
                        continue;

                vec3_t color;
                if (surf->samples)
                {
                        InterpolateLightmap(model, color, surf, ds, dt, use_rgblight);
                }
                else
                {
                        R_SurfaceFallbackColor(model, surf, color);
                }

                if (!VectorLength(color))
                        continue;

                const float adist = fabsf(pdist);
                if (adist < best_dist)
                {
                        VectorCopy(color, out_rgb);
                        best_dist = adist;
                        found = true;
                }
        }

        if (found)
        {
                VectorScale(out_rgb, 1.f / 255.f, out_rgb);
                return true;
        }

        R_SurfaceFallbackColor(model, NULL, out_rgb);
        VectorScale(out_rgb, 1.f / 255.f, out_rgb);
        return false;
}

/*
=============
RecursiveLightPoint -- johnfitz -- replaced entire function for lit support via lordhavoc
=============
*/
int RecursiveLightPoint (qmodel_t *model, lightcache_t *cache, mnode_t *node, vec3_t rayorg, vec3_t start, vec3_t end, float *maxdist)
{
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
		front = DotProduct(start, node->plane->normal) - node->plane->dist;
		back = DotProduct(end, node->plane->normal) - node->plane->dist;
	}

	// LordHavoc: optimized recursion
	if ((back < 0) == (front < 0))
//		return RecursiveLightPoint (model, cache, node->children[front < 0], rayorg, start, end, maxdist);
	{
		node = node->children[front < 0];
		goto loc0;
	}

	frac = front / (front-back);
	mid[0] = start[0] + (end[0] - start[0])*frac;
	mid[1] = start[1] + (end[1] - start[1])*frac;
	mid[2] = start[2] + (end[2] - start[2])*frac;

// go down front side
	if (RecursiveLightPoint (model, cache, node->children[front < 0], rayorg, start, mid, maxdist))
		return true;	// hit something
	else
	{
		unsigned int i;
		int ds, dt;
		msurface_t *surf;
	// check for impact on this node

		surf = model->surfaces + node->firstsurface;
		for (i = 0;i < node->numsurfaces;i++, surf++)
		{
			float sfront, sback, dist;
			vec3_t raydelta;

			if (surf->flags & SURF_DRAWTILED)
				continue;	// no lightmaps

		// ericw -- added double casts to force 64-bit precision.
		// Without them the zombie at the start of jam3_ericw.bsp was
		// incorrectly being lit up in SSE builds.
			ds = (int) ((double) DoublePrecisionDotProduct (mid, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3]);
			dt = (int) ((double) DoublePrecisionDotProduct (mid, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3]);

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
				sfront = DotProduct(rayorg, surf->plane->normal) - surf->plane->dist;
				sback = DotProduct(end, surf->plane->normal) - surf->plane->dist;
			}
			VectorSubtract(end, rayorg, raydelta);
			dist = sfront / (sfront - sback) * VectorLength(raydelta);

			if (!surf->samples)
			{
				// We hit a surface that is flagged as lightmapped, but doesn't have actual lightmap info.
				// Instead of just returning black, we'll keep looking for nearby surfaces that do have valid samples.
				// This fixes occasional pitch-black models in otherwise well-lit areas in DOTM (e.g. mge1m1, mge4m1)
				// caused by overlapping surfaces with mixed lighting data.
				const float nearby = 8.f;
				dist += nearby;
				*maxdist = q_min(*maxdist, dist);
				continue;
			}

			if (dist < *maxdist)
			{
				cache->surfidx = surf - model->surfaces + 1;
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
		return RecursiveLightPoint (model, cache, node->children[front >= 0], rayorg, mid, end, maxdist);
	}
}

/*
=============
R_LightPoint -- johnfitz -- replaced entire function for lit support via lordhavoc
=============
*/
int R_LightPoint (qmodel_t *model, vec3_t p, float ofs, lightcache_t *cache)
{
        qboolean        use_rgblight = false;
        qboolean        lightgrid_active;

        cache->lightgrid_has_sample = false;
        cache->lightgrid_intensity = 0.f;
        VectorClear (cache->lightgrid_color);
        VectorClear (cache->lightgrid_dir);

        lightgrid_active = R_LightgridEnabled ();

        if (lightgrid_active)
        {
                vec3_t lg_color, lg_dir, lg_color255;

                R_LightgridLightingDir (p, lg_color, lg_dir);
                VectorScale (lg_color, 255.f, lg_color255);

                VectorCopy (lg_color255, lightcolor);
                R_AddDynamicLights_Lightgrid (p, lightcolor);

                cache->surfidx = 0;
                VectorCopy (p, cache->pos);
                cache->lightgrid_has_sample = true;
                cache->lightgrid_intensity = 1.f;
                VectorCopy (lg_color, cache->lightgrid_color);
                VectorCopy (lg_dir, cache->lightgrid_dir);

                return ((lightcolor[0] + lightcolor[1] + lightcolor[2]) * (1.0f / 3.0f));
        }

        if (model && model->has_lightdata_rgb && r_rgblighting_enable.value)
                use_rgblight = true;

        if (!model || !model->lightdata)
        {
                lightcolor[0] = lightcolor[1] = lightcolor[2] = 255;
                return 255;
        }

        vec3_t sample_pos;
        VectorCopy (p, sample_pos);

        if (!R_AdjustPointForLeaf (model, sample_pos))
        {
                const float ambient = 0.04f * 255.f;
                VectorSet (lightcolor, ambient, ambient, ambient);
                cache->surfidx = -1;
                VectorCopy (sample_pos, cache->pos);
                R_ClampSampleColor (lightcolor);
                return (int)ambient;
        }

        R_SamplePointInternal (model, sample_pos, ofs, use_rgblight, cache, lightcolor);

        const vec3_t fallback_offsets[] = {
                { 0.f, 0.f, 0.f }, { 2.f, 0.f, 0.f }, { -2.f, 0.f, 0.f }, { 0.f, 2.f, 0.f }, { 0.f, -2.f, 0.f },
                { 0.f, 0.f, 2.f }, { 0.f, 0.f, -2.f }, { 4.f, 4.f, 0.f }, { -4.f, -4.f, 0.f }, { 0.f, 4.f, 4.f }
        };

        float intensity = (lightcolor[0] + lightcolor[1] + lightcolor[2]) * (1.f / (3.f * 255.f));

        if (intensity < 0.001f)
        {
                vec3_t accum = {0, 0, 0};
                int hits = 0;

                for (size_t i = 0; i < sizeof(fallback_offsets) / sizeof(fallback_offsets[0]); i++)
                {
                        vec3_t test;
                        VectorAdd (sample_pos, fallback_offsets[i], test);

                        if (!R_AdjustPointForLeaf (model, test))
                                continue;

                        vec3_t temp_color;
                        lightcache_t temp_cache = {0};
                        R_SamplePointInternal (model, test, ofs, use_rgblight, &temp_cache, temp_color);

                        if (VectorLength (temp_color) > 0.f)
                        {
                                VectorAdd (accum, temp_color, accum);
                                hits++;
                        }
                }

                if (hits > 0)
                {
                        VectorScale (accum, 1.f / (float)hits, lightcolor);
                        intensity = (lightcolor[0] + lightcolor[1] + lightcolor[2]) * (1.f / (3.f * 255.f));
                }
        }

        if (intensity > 0.f && intensity < 0.015f)
        {
                vec3_t raised;
                VectorCopy (sample_pos, raised);
                raised[2] += 4.f;

                if (R_AdjustPointForLeaf (model, raised))
                {
                        vec3_t above_color;
                        lightcache_t temp_cache = {0};
                        R_SamplePointInternal (model, raised, ofs, use_rgblight, &temp_cache, above_color);
                        VectorMA (lightcolor, 0.2f, above_color, lightcolor);
                }
        }

        R_ClampSampleColor (lightcolor);

        return ((lightcolor[0] + lightcolor[1] + lightcolor[2]) * (1.0f / 3.0f));
}

const lightgrid_probe_t *R_GetLightgridSample (const vec3_t pos)
{
        const lightgrid_t *lg = Lightgrid_Get ();
        int x, y, z;

        if (!lg || !lg->probes || lg->cellsize <= 0.f)
                return NULL;

        if (!R_LightgridEnabledInternal (lg))
                return NULL;

        x = (int)floorf((pos[0] - lg->mins[0]) / lg->cellsize);
        y = (int)floorf((pos[1] - lg->mins[1]) / lg->cellsize);
        z = (int)floorf((pos[2] - lg->mins[2]) / lg->cellsize);

        x = CLAMP(0, x, lg->nx - 1);
        y = CLAMP(0, y, lg->ny - 1);
        z = CLAMP(0, z, lg->nz - 1);

        return &lg->probes[(z * lg->ny + y) * lg->nx + x];
}
