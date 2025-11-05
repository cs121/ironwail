/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
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

#define MAX_PARTICLES			16384	// default max # of particles at one
										//  time
#define ABSOLUTE_MIN_PARTICLES	512		// no fewer than this no matter what's
										//  on the command line

static int	ramp1[8] = {0x6f, 0x6d, 0x6b, 0x69, 0x67, 0x65, 0x63, 0x61};
static int	ramp2[8] = {0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x68, 0x66};
static int	ramp3[8] = {0x6d, 0x6b, 6, 5, 4, 3};

particle_t	*particles;
int			r_numparticles, r_numactiveparticles;

static float uvscale;
static float texturescalefactor; //johnfitz -- compensate for apparent size of different particle textures

#define PARTICLE_TEX_TILE_SIZE          32
#define NUM_PARTICLE_TEXTURES           4

typedef struct particleappearance_s
{
        float   alpha_start;
        float   alpha_end;
        float   fade;
        float   glow;
        int             texture;
} particleappearance_t;

enum
{
        PARTICLE_TEX_SOFT = 0,
        PARTICLE_TEX_GLOW,
        PARTICLE_TEX_SMOKE,
        PARTICLE_TEX_STREAK
};

static const particleappearance_t particle_appearance[] =
{
        {1.f, 0.f, 1.4f, 0.2f, PARTICLE_TEX_STREAK}, // pt_static
        {1.f, 0.f, 1.1f, 0.0f, PARTICLE_TEX_SMOKE},   // pt_grav
        {1.f, 0.f, 1.0f, 0.0f, PARTICLE_TEX_SMOKE},   // pt_slowgrav
        {1.f, 0.f, 0.7f, 0.6f, PARTICLE_TEX_GLOW},    // pt_fire
        {1.f, 0.f, 0.8f, 0.5f, PARTICLE_TEX_GLOW},    // pt_explode
        {1.f, 0.f, 0.9f, 0.45f, PARTICLE_TEX_GLOW},   // pt_explode2
        {1.f, 0.f, 1.2f, 0.0f, PARTICLE_TEX_SMOKE},   // pt_blob
        {1.f, 0.f, 1.2f, 0.0f, PARTICLE_TEX_SMOKE}    // pt_blob2
};

static const particleappearance_t particle_appearance_default = {1.f, 0.f, 1.f, 0.f, PARTICLE_TEX_SOFT};

static gltexture_t *particle_atlas;
static float particle_atlas_tile_width;
static float particle_atlas_tile_height;
static float particle_atlas_tile_margin;

cvar_t	r_particles = {"r_particles","2", CVAR_ARCHIVE}; //johnfitz

typedef struct particlevert_t {
	vec3_t		pos;
	GLubyte		color[4];
	GLubyte		params[4];
} particlevert_t;

static particlevert_t partverts[MAX_PARTICLES];
static int numpartverts = 0;

static float R_ParticleNoise (int x, int y, int seed)
{
        uint32_t n = (uint32_t)(x * 1973 + y * 9277 + seed * 26699);
        n ^= n >> 13;
        n *= 1274126177u;
        return (n & 0x00FFFFFFu) * (1.0f / 16777215.0f);
}

static void R_InitParticleAtlas (void)
{
        if (particle_atlas)
                return;

        const int atlas_width = PARTICLE_TEX_TILE_SIZE * NUM_PARTICLE_TEXTURES;
        const int atlas_height = PARTICLE_TEX_TILE_SIZE;
        uint32_t data[atlas_width * atlas_height];
        int tex, y, x;

        for (tex = 0; tex < NUM_PARTICLE_TEXTURES; tex++)
        {
                for (y = 0; y < PARTICLE_TEX_TILE_SIZE; y++)
                {
                        for (x = 0; x < PARTICLE_TEX_TILE_SIZE; x++)
                        {
                                int atlas_x = tex * PARTICLE_TEX_TILE_SIZE + x;
                                float u = ((float)x + 0.5f) / (float)PARTICLE_TEX_TILE_SIZE - 0.5f;
                                float v = ((float)y + 0.5f) / (float)PARTICLE_TEX_TILE_SIZE - 0.5f;
                                float radius = sqrtf (u*u + v*v);
                                float alpha = 0.0f;
                                float intensity;

                                switch (tex)
                                {
                                case PARTICLE_TEX_SOFT:
                                        alpha = q_max (0.f, 1.f - radius * 2.f);
                                        alpha = powf (alpha, 1.2f);
                                        break;

                                case PARTICLE_TEX_GLOW:
                                        alpha = q_max (0.f, 1.f - radius * 2.f);
                                        alpha = powf (alpha, 3.0f);
                                        break;

                                case PARTICLE_TEX_SMOKE:
                                {
                                        float noise = R_ParticleNoise (x, y, 11);
                                        float noise2 = R_ParticleNoise (x, y, 29);
                                        float base = q_max (0.f, 1.f - radius * 2.f);
                                        alpha = base * (0.55f + 0.45f * noise);
                                        alpha = powf (alpha, 1.4f);
                                        alpha = q_min (1.f, alpha + noise2 * 0.2f * base);
                                        break;
                                }

                                case PARTICLE_TEX_STREAK:
                                {
                                        float horiz = q_max (0.f, 1.f - fabsf (v) * 6.f);
                                        float taper = q_max (0.f, 1.f - fabsf (u) * 1.6f);
                                        alpha = powf (horiz * taper, 1.1f);
                                        break;
                                }
                                }

                                alpha = q_max (0.f, q_min (alpha, 1.f));
                                intensity = alpha;

                                ((byte *)&data[y * atlas_width + atlas_x])[0] = (byte)q_min (255, (int)(intensity * 255.f + 0.5f));
                                ((byte *)&data[y * atlas_width + atlas_x])[1] = ((byte *)&data[y * atlas_width + atlas_x])[0];
                                ((byte *)&data[y * atlas_width + atlas_x])[2] = ((byte *)&data[y * atlas_width + atlas_x])[0];
                                ((byte *)&data[y * atlas_width + atlas_x])[3] = (byte)q_min (255, (int)(alpha * 255.f + 0.5f));
                        }
                }
        }

        particle_atlas = TexMgr_LoadImageEx (NULL, "particles/atlas", atlas_width, atlas_height, 1, SRC_RGBA,
                (byte *)data, "", 0, TEXPREF_LINEAR | TEXPREF_NOPICMIP | TEXPREF_PERSIST | TEXPREF_CLAMP);

        particle_atlas_tile_width = 1.0f / (float)NUM_PARTICLE_TEXTURES;
        particle_atlas_tile_height = 1.0f;
        particle_atlas_tile_margin = 0.5f / (float)PARTICLE_TEX_TILE_SIZE;
}

/*
===============
R_SetParticleTexture_f -- johnfitz
===============
*/
static void R_SetParticleTexture_f (cvar_t *var)
{
	switch ((int)(r_particles.value))
	{
	case 1:
		uvscale = 1;
		texturescalefactor = 1.27;
		break;
	case 2:
		uvscale = 0.25;
		texturescalefactor = 1.0;
		break;
	}
}

/*
===============
R_AllocParticle
===============
*/
particle_t *R_AllocParticle (void)
{
	if (r_numactiveparticles < r_numparticles)
	{
		particle_t *p = &particles[r_numactiveparticles++];
                p->spawn = cl.time;
		return p;
	}
	return NULL;
}

/*
===============
R_InitParticles
===============
*/
void R_InitParticles (void)
{
	int		i;

	i = COM_CheckParm ("-particles");

	if (i && i < com_argc - 1)
	{
		r_numparticles = atoi(com_argv[i + 1]);
		if (r_numparticles < ABSOLUTE_MIN_PARTICLES)
			r_numparticles = ABSOLUTE_MIN_PARTICLES;
	}
	else
	{
		r_numparticles = MAX_PARTICLES;
	}

	particles = (particle_t *)
			Hunk_AllocName (r_numparticles * sizeof(particle_t), "particles");
	r_numactiveparticles = 0;

        Cvar_RegisterVariable (&r_particles); //johnfitz
        Cvar_SetCallback (&r_particles, R_SetParticleTexture_f);
        R_SetParticleTexture_f (&r_particles); // set default

        R_InitParticleAtlas ();
}

/*
===============
R_EntityParticles
===============
*/
static vec3_t	avelocities[NUMVERTEXNORMALS];
static float	beamlength = 16;

void R_EntityParticles (entity_t *ent)
{
	int		i;
	particle_t	*p;
	float		angle;
	float		sp, sy, cp, cy;
//	float		sr, cr;
//	int		count;
	vec3_t		forward;
	float		dist;

	dist = 64;
//	count = 50;

	if (!avelocities[0][0])
	{
		for (i = 0; i < NUMVERTEXNORMALS; i++)
		{
			avelocities[i][0] = (rand() & 255) * 0.01;
			avelocities[i][1] = (rand() & 255) * 0.01;
			avelocities[i][2] = (rand() & 255) * 0.01;
		}
	}

	for (i = 0; i < NUMVERTEXNORMALS; i++)
	{
		angle = cl.time * avelocities[i][0];
		sy = sin(angle);
		cy = cos(angle);
		angle = cl.time * avelocities[i][1];
		sp = sin(angle);
		cp = cos(angle);
		angle = cl.time * avelocities[i][2];
	//	sr = sin(angle);
	//	cr = cos(angle);

		forward[0] = cp*cy;
		forward[1] = cp*sy;
		forward[2] = -sp;

		if (!(p = R_AllocParticle ()))
			return;

		p->die = cl.time + 0.01;
		p->color = 0x6f;
		p->type = pt_explode;

		p->org[0] = ent->origin[0] + r_avertexnormals[i][0]*dist + forward[0]*beamlength;
		p->org[1] = ent->origin[1] + r_avertexnormals[i][1]*dist + forward[1]*beamlength;
		p->org[2] = ent->origin[2] + r_avertexnormals[i][2]*dist + forward[2]*beamlength;
	}
}

/*
===============
R_ClearParticles
===============
*/
void R_ClearParticles (void)
{
	r_numactiveparticles = 0;
}

/*
===============
R_ParseParticleEffect

Parse an effect out of the server message
===============
*/
void R_ParseParticleEffect (void)
{
	vec3_t		org, dir;
	int			i, count, msgcount, color;

	for (i=0 ; i<3 ; i++)
		org[i] = MSG_ReadCoord (cl.protocolflags);
	for (i=0 ; i<3 ; i++)
		dir[i] = MSG_ReadChar () * (1.0/16);
	msgcount = MSG_ReadByte ();
	color = MSG_ReadByte ();

	if (msgcount == 255)
		count = 1024;
	else
		count = msgcount;

	R_RunParticleEffect (org, dir, color, count);
}

/*
===============
R_ParticleExplosion
===============
*/
void R_ParticleExplosion (vec3_t org)
{
	int			i, j;
	particle_t	*p;

	for (i=0 ; i<1024 ; i++)
	{
		if (!(p = R_AllocParticle ()))
			return;

		p->die = cl.time + 5;
		p->color = ramp1[0];
		p->ramp = rand()&3;
		if (i & 1)
		{
			p->type = pt_explode;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%512)-256;
			}
		}
		else
		{
			p->type = pt_explode2;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%512)-256;
			}
		}
	}
}

/*
===============
R_ParticleExplosion2
===============
*/
void R_ParticleExplosion2 (vec3_t org, int colorStart, int colorLength)
{
	int			i, j;
	particle_t	*p;
	int			colorMod = 0;

	for (i=0; i<512; i++)
	{
		if (!(p = R_AllocParticle ()))
			return;

		p->die = cl.time + 0.3;
		p->color = colorStart + (colorMod % colorLength);
		colorMod++;

		p->type = pt_blob;
		for (j=0 ; j<3 ; j++)
		{
			p->org[j] = org[j] + ((rand()%32)-16);
			p->vel[j] = (rand()%512)-256;
		}
	}
}

/*
===============
R_BlobExplosion
===============
*/
void R_BlobExplosion (vec3_t org)
{
	int			i, j;
	particle_t	*p;

	for (i=0 ; i<1024 ; i++)
	{
		if (!(p = R_AllocParticle ()))
			return;

		p->die = cl.time + 1 + (rand()&8)*0.05;

		if (i & 1)
		{
			p->type = pt_blob;
			p->color = 66 + rand()%6;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%512)-256;
			}
		}
		else
		{
			p->type = pt_blob2;
			p->color = 150 + rand()%6;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%512)-256;
			}
		}
	}
}

/*
===============
R_RunParticleEffect
===============
*/
void R_RunParticleEffect (vec3_t org, vec3_t dir, int color, int count)
{
	int			i, j;
	particle_t	*p;

	if (count > 0)
	{
		int basecolor = color & ~7;

		if (basecolor == 64 || basecolor == 72)
		{
			R_AddBloodDecal (org, dir);
			R_AddGibDecal (org, count);
		}
	}

	for (i=0 ; i<count ; i++)
	{
		if (!(p = R_AllocParticle ()))
			return;

		if (count == 1024)
		{	// rocket explosion
			p->die = cl.time + 5;
			p->color = ramp1[0];
			p->ramp = rand()&3;
			if (i & 1)
			{
				p->type = pt_explode;
				for (j=0 ; j<3 ; j++)
				{
					p->org[j] = org[j] + ((rand()%32)-16);
					p->vel[j] = (rand()%512)-256;
				}
			}
			else
			{
				p->type = pt_explode2;
				for (j=0 ; j<3 ; j++)
				{
					p->org[j] = org[j] + ((rand()%32)-16);
					p->vel[j] = (rand()%512)-256;
				}
			}
		}
		else
		{
			p->die = cl.time + 0.1*(rand()%5);
			p->color = (color&~7) + (rand()&7);
			p->type = pt_slowgrav;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()&15)-8);
				p->vel[j] = dir[j]*15;// + (rand()%300)-150;
			}
		}
	}
}

/*
===============
R_LavaSplash
===============
*/
void R_LavaSplash (vec3_t org)
{
	int			i, j, k;
	particle_t	*p;
	float		vel;
	vec3_t		dir;

	for (i=-16 ; i<16 ; i++)
		for (j=-16 ; j<16 ; j++)
			for (k=0 ; k<1 ; k++)
			{
				if (!(p = R_AllocParticle ()))
					return;

				p->die = cl.time + 2 + (rand()&31) * 0.02;
				p->color = 224 + (rand()&7);
				p->type = pt_slowgrav;

				dir[0] = j*8 + (rand()&7);
				dir[1] = i*8 + (rand()&7);
				dir[2] = 256;

				p->org[0] = org[0] + dir[0];
				p->org[1] = org[1] + dir[1];
				p->org[2] = org[2] + (rand()&63);

				VectorNormalize (dir);
				vel = 50 + (rand()&63);
				VectorScale (dir, vel, p->vel);
			}
}

/*
===============
R_TeleportSplash
===============
*/
void R_TeleportSplash (vec3_t org)
{
	int			i, j, k;
	particle_t	*p;
	float		vel;
	vec3_t		dir;

	for (i=-16 ; i<16 ; i+=4)
	{
		for (j=-16 ; j<16 ; j+=4)
		{
			for (k=-24 ; k<32 ; k+=4)
			{
				if (!(p = R_AllocParticle ()))
					return;

				p->die = cl.time + 0.2 + (rand()&7) * 0.02;
				p->color = 7 + (rand()&7);
				p->type = pt_slowgrav;

				dir[0] = j*8;
				dir[1] = i*8;
				dir[2] = k*8;

				p->org[0] = org[0] + i + (rand()&3);
				p->org[1] = org[1] + j + (rand()&3);
				p->org[2] = org[2] + k + (rand()&3);

				VectorNormalize (dir);
				vel = 50 + (rand()&63);
				VectorScale (dir, vel, p->vel);
			}
		}
	}
}

/*
===============
R_RocketTrail

FIXME -- rename function and use #defined types instead of numbers
===============
*/
void R_RocketTrail (vec3_t start, vec3_t end, int type)
{
	vec3_t		vec;
	float		len;
	int			j;
	particle_t	*p;
	int			dec;
	static int	tracercount;

	VectorSubtract (end, start, vec);
	len = VectorNormalize (vec);
	if (type < 128)
		dec = 3;
	else
	{
		dec = 1;
		type -= 128;
	}

	while (len > 0)
	{
		len -= dec;

		if (!(p = R_AllocParticle ()))
			return;

		VectorCopy (vec3_origin, p->vel);
		p->die = cl.time + 2;

		switch (type)
		{
			case 0:	// rocket trail
				p->ramp = (rand()&3);
				p->color = ramp3[(int)p->ramp];
				p->type = pt_fire;
				for (j=0 ; j<3 ; j++)
					p->org[j] = start[j] + ((rand()%6)-3);
				break;

			case 1:	// smoke smoke
				p->ramp = (rand()&3) + 2;
				p->color = ramp3[(int)p->ramp];
				p->type = pt_fire;
				for (j=0 ; j<3 ; j++)
					p->org[j] = start[j] + ((rand()%6)-3);
				break;

			case 2:	// blood
				p->type = pt_grav;
				p->color = 67 + (rand()&3);
				for (j=0 ; j<3 ; j++)
					p->org[j] = start[j] + ((rand()%6)-3);
				break;

			case 3:
			case 5:	// tracer
				p->die = cl.time + 0.5;
				p->type = pt_static;
				if (type == 3)
					p->color = 52 + ((tracercount&4)<<1);
				else
					p->color = 230 + ((tracercount&4)<<1);

				tracercount++;

				VectorCopy (start, p->org);
				if (tracercount & 1)
				{
					p->vel[0] = 30*vec[1];
					p->vel[1] = 30*-vec[0];
				}
				else
				{
					p->vel[0] = 30*-vec[1];
					p->vel[1] = 30*vec[0];
				}
				break;

			case 4:	// slight blood
				p->type = pt_grav;
				p->color = 67 + (rand()&3);
				for (j=0 ; j<3 ; j++)
					p->org[j] = start[j] + ((rand()%6)-3);
				len -= 3;
				break;

			case 6:	// voor trail
				p->color = 9*16 + 8 + (rand()&3);
				p->type = pt_static;
				p->die = cl.time + 0.3;
				for (j=0 ; j<3 ; j++)
					p->org[j] = start[j] + ((rand()&15)-8);
				break;
		}

		VectorAdd (start, vec, start);
	}
}

/*
===============
CL_RunParticles -- johnfitz -- all the particle behavior, separated from R_DrawParticles
===============
*/
void CL_RunParticles (void)
{
	particle_t		*p;
	int				i, cur, active;
	float			time1, time2, time3, dvel, frametime, grav;
	extern	cvar_t	sv_gravity;

	frametime = cl.time - cl.oldtime;
	time3 = frametime * 15;
	time2 = frametime * 10;
	time1 = frametime * 5;
	grav = frametime * sv_gravity.value * 0.05;
	dvel = 4*frametime;

	for (cur = active = 0, p = particles; cur < r_numactiveparticles; cur++, p++)
	{
		if (p->die < cl.time || p->spawn > cl.time)
			continue;

		p->org[0] += p->vel[0]*frametime;
		p->org[1] += p->vel[1]*frametime;
		p->org[2] += p->vel[2]*frametime;

		switch (p->type)
		{
		case pt_static:
			break;
		case pt_fire:
			p->ramp += time1;
			if (p->ramp >= 6)
				p->die = -1;
			else
				p->color = ramp3[(int)p->ramp];
			p->vel[2] += grav;
			break;

		case pt_explode:
			p->ramp += time2;
			if (p->ramp >=8)
				p->die = -1;
			else
				p->color = ramp1[(int)p->ramp];
			for (i=0 ; i<3 ; i++)
				p->vel[i] += p->vel[i]*dvel;
			p->vel[2] -= grav;
			break;

		case pt_explode2:
			p->ramp += time3;
			if (p->ramp >=8)
				p->die = -1;
			else
				p->color = ramp2[(int)p->ramp];
			for (i=0 ; i<3 ; i++)
				p->vel[i] -= p->vel[i]*frametime;
			p->vel[2] -= grav;
			break;

		case pt_blob:
			for (i=0 ; i<3 ; i++)
				p->vel[i] += p->vel[i]*dvel;
			p->vel[2] -= grav;
			break;

		case pt_blob2:
			for (i=0 ; i<2 ; i++)
				p->vel[i] -= p->vel[i]*dvel;
			p->vel[2] -= grav;
			break;

		case pt_grav:
		case pt_slowgrav:
			p->vel[2] -= grav;
			break;
		}

		if (cur != active)
			particles[active] = *p;
		active++;
	}

	r_numactiveparticles = active;
}

/*
===============
R_FlushParticleBatch
===============
*/
static void R_FlushParticleBatch (void)
{
	GLuint buf;
	GLbyte *ofs;

	if (!numpartverts)
		return;

	GL_Upload (GL_ARRAY_BUFFER, partverts, sizeof(partverts[0]) * numpartverts, &buf, &ofs);
	GL_BindBuffer (GL_ARRAY_BUFFER, buf);
        GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof(partverts[0]), ofs + offsetof(particlevert_t, pos));
        GL_VertexAttribPointerFunc (1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(partverts[0]), ofs + offsetof(particlevert_t, color));
        GL_VertexAttribPointerFunc (2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(partverts[0]), ofs + offsetof(particlevert_t, params));

        GL_DrawArraysInstancedFunc (GL_TRIANGLE_STRIP, 0, 4, numpartverts);

	numpartverts = 0;
}

/*
===============
R_DrawParticles_Real -- johnfitz -- moved all non-drawing code to CL_RunParticles
===============
*/
static void R_DrawParticles_Real (qboolean alpha, qboolean showtris)
{
	particle_t		*p;
	particlevert_t	*v;
	GLubyte			color[4] = {255, 255, 255, 255}, *c; //johnfitz -- particle transparency
	extern	cvar_t	r_particles; //johnfitz
	//float			alpha; //johnfitz -- particle transparency
	float			scalex, scaley;
	qboolean		dither, oit, wants_alpha;
	int				i;

	if (!r_particles.value)
		return;

	if (!r_numactiveparticles)
		return;

	wants_alpha = ((int)r_particles.value != 1);

	if (!showtris && alpha != wants_alpha)
		return;

	GL_BeginGroup ("Particles");

	dither = (softemu == SOFTEMU_COARSE && !showtris);
	oit = (alpha && R_GetEffectiveAlphaMode () == ALPHAMODE_OIT);
	GL_UseProgram (glprogs.particles[oit][dither]);

	// compensate for apparent size of different particle textures
	// this bakes in the additional scaling of vup and vright by 1.5f for billboarding,
	// then down by 0.25f for quad particles
	scalex = scaley = texturescalefactor * 0.375f;
	// projection factors (see GL_FrustumMatrix), negated to make things easier in the shader
	scalex *=	r_matproj[1*4 + 0]; // -1 / tan (fovx/2)
	scaley *= -r_matproj[2*4 + 1]; // -1 / tan (fovy/2)
	GL_Uniform3fFunc (0, scalex, scaley, uvscale);

	if (particle_atlas)
	{
		GL_Bind (GL_TEXTURE0, particle_atlas);
		GL_Uniform4fFunc (1, particle_atlas_tile_width, particle_atlas_tile_height, particle_atlas_tile_margin, particle_atlas_tile_margin);
	}
	else
	{
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, 0);
		GL_Uniform4fFunc (1, 1.f, 1.f, 0.f, 0.f);
	}

	if (alpha)
		GL_SetState (GLS_BLEND_ALPHA_OIT | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (3) | GLS_INSTANCED_ATTRIBS (3));
	else
		GL_SetState (GLS_BLEND_OPAQUE | GLS_CULL_NONE | GLS_ATTRIBS (3) | GLS_INSTANCED_ATTRIBS (3));

	numpartverts = 0;
	for (i = 0, p = particles; i < r_numactiveparticles; i++, p++)
	{
		if (numpartverts == countof(partverts))
			R_FlushParticleBatch ();

		{
			const particleappearance_t *appearance;
			float lifetime, life, fade, alphaval, glow;
			GLubyte alphabyte, glowbyte;
			int texindex;

			if ((unsigned)p->type < countof(particle_appearance))
				appearance = &particle_appearance[p->type];
			else
				appearance = &particle_appearance_default;

			lifetime = p->die - p->spawn;
			life = 0.f;
			if (lifetime > 0.f)
				life = (cl.time - p->spawn) / lifetime;
			life = q_min (1.f, q_max (0.f, life));
			fade = appearance->fade > 0.f ? powf (life, appearance->fade) : life;
			alphaval = appearance->alpha_start + (appearance->alpha_end - appearance->alpha_start) * fade;
			alphaval = q_min (1.f, q_max (0.f, alphaval));
			if (!showtris && alphaval <= 0.f)
				continue;

			glow = q_min (1.f, q_max (0.f, appearance->glow));
			texindex = appearance->texture;
			if (texindex < 0 || texindex >= NUM_PARTICLE_TEXTURES)
				texindex = PARTICLE_TEX_SOFT;

			alphabyte = (GLubyte)q_min (255, (int)(alphaval * 255.f + 0.5f));
			glowbyte = (GLubyte)q_min (255, (int)(glow * 255.f + 0.5f));

			v = &partverts[numpartverts];
			VectorCopy (p->org, v->pos);

			c = showtris ? color : (GLubyte *) &d_8to24table[(int)p->color];
			*(uint32_t*)&v->color = *(uint32_t*)c;
			v->color[0] = c[0];
			v->color[1] = c[1];
			v->color[2] = c[2];
			v->color[3] = showtris ? 255 : alphabyte;

			v->params[0] = showtris ? 0 : glowbyte;
			v->params[1] = (GLubyte)texindex;
			v->params[2] = 0;
			v->params[3] = 0;

			numpartverts++;
		}
	}

	R_FlushParticleBatch ();

	GL_EndGroup ();
}

/*
===============
R_DrawParticles -- johnfitz -- moved all non-drawing code to CL_RunParticles
===============
*/
void R_DrawParticles (qboolean alpha)
{
	R_DrawParticles_Real (alpha, false);
}
/*
===============
R_DrawParticles_ShowTris -- johnfitz
===============
*/
void R_DrawParticles_ShowTris (void)
{
	R_DrawParticles_Real (false, true);
}

