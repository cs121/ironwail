// Patched: Robustheit, Aliasing-Fix, Atlas-Upload, Partikel-Lifecycle (2025-11-05)
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

#define MAX_PARTICLES            16384   // default max # of particles at one
//  time
#define ABSOLUTE_MIN_PARTICLES   512     // no fewer than this no matter what's
										//  on the command line

static int  ramp1[8] = { 0x6f, 0x6d, 0x6b, 0x69, 0x67, 0x65, 0x63, 0x61 };
static int  ramp2[8] = { 0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x68, 0x66 };
static int  ramp3[8] = { 0x6d, 0x6b, 6, 5, 4, 3 };

particle_t* particles;
int         r_numparticles, r_numactiveparticles;

static float uvscale;
static float texturescalefactor; //johnfitz -- compensate for apparent size of different particle textures

// Hinweis: texturescalefactor wird später * 0.375f skaliert.
// Das entspricht 1.5f (Billboard-Kompensation) * 0.25f (Quad-Partikel-Grundgröße).

#define PARTICLE_TEX_TILE_SIZE          32
#define NUM_PARTICLE_TEXTURES           4

typedef struct particleappearance_s
{
	float   alpha_start;
	float   alpha_end;
	float   fade;
	float   glow;
	int     texture;
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

static const particleappearance_t particle_appearance_default = { 1.f, 0.f, 1.f, 0.f, PARTICLE_TEX_SOFT };

static gltexture_t* particle_atlas;
static float particle_atlas_tile_width;
static float particle_atlas_tile_height;
static float particle_atlas_tile_margin;

#define MAX_EFFECTINFO               256

typedef enum effectshape_e
{
        EFFECT_SHAPE_SMOKE,
        EFFECT_SHAPE_SPARK,
        EFFECT_SHAPE_BEAM,
        EFFECT_SHAPE_GENERIC
} effectshape_t;

typedef struct effectinfo_s
{
        char            name[64];
        effectshape_t   shape;
        int             count;
        vec3_t          color;
        float           alpha_start;
        float           alpha_end;
        float           alpha_power;
        float           size_min;
        float           size_max;
        float           size_increase_min;
        float           size_increase_max;
        float           velocity_min;
        float           velocity_max;
        float           velocity_jitter;
        float           gravity;
        float           airfriction;
        float           lifetime;
        float           spawnradius;
        float           glow;
        int             texture;
} effectinfo_t;

static effectinfo_t effectinfos[MAX_EFFECTINFO];
static int          num_effectinfos;
static qboolean     effectinfo_active;
static char         effectinfo_source[MAX_OSPATH];

static float R_Effectinfo_Random (float min, float max)
{
        return min + (max - min) * ((float)rand () / (float)RAND_MAX);
}

static float R_Effectinfo_CRandom (float range)
{
        return (2.0f * ((float)rand () / (float)RAND_MAX) - 1.0f) * range;
}

static void R_Effectinfo_Clear (void)
{
        memset (effectinfos, 0, sizeof (effectinfos));
        num_effectinfos = 0;
        effectinfo_active = false;
        effectinfo_source[0] = '\0';
}

static void R_Effectinfo_SetDefaults (effectinfo_t* fx)
{
        fx->shape = EFFECT_SHAPE_SMOKE;
        fx->count = 1;
        VectorSet (fx->color, 1.f, 1.f, 1.f);
        fx->alpha_start = 1.f;
        fx->alpha_end = 0.f;
        fx->alpha_power = 1.f;
        fx->size_min = 16.f;
        fx->size_max = 16.f;
        fx->size_increase_min = 0.f;
        fx->size_increase_max = 0.f;
        fx->velocity_min = 0.f;
        fx->velocity_max = 0.f;
        fx->velocity_jitter = 0.f;
        fx->gravity = 0.f;
        fx->airfriction = 0.f;
        fx->lifetime = 0.5f;
        fx->spawnradius = 0.f;
        fx->glow = 0.f;
        fx->texture = -1;
}

static int R_Effectinfo_TextureForShape (effectshape_t shape)
{
        switch (shape)
        {
        case EFFECT_SHAPE_SPARK:
                return PARTICLE_TEX_GLOW;
        case EFFECT_SHAPE_BEAM:
                return PARTICLE_TEX_STREAK;
        case EFFECT_SHAPE_GENERIC:
        case EFFECT_SHAPE_SMOKE:
        default:
                return PARTICLE_TEX_SMOKE;
        }
}

static effectshape_t R_Effectinfo_ParseShape (const char* token)
{
        if (!q_strcasecmp (token, "smoke"))
                return EFFECT_SHAPE_SMOKE;
        if (!q_strcasecmp (token, "spark"))
                return EFFECT_SHAPE_SPARK;
        if (!q_strcasecmp (token, "beam"))
                return EFFECT_SHAPE_BEAM;
        return EFFECT_SHAPE_GENERIC;
}

static int R_Effectinfo_ParseTexture (const char* token)
{
        if (!q_strcasecmp (token, "soft"))
                return PARTICLE_TEX_SOFT;
        if (!q_strcasecmp (token, "glow"))
                return PARTICLE_TEX_GLOW;
        if (!q_strcasecmp (token, "smoke"))
                return PARTICLE_TEX_SMOKE;
        if (!q_strcasecmp (token, "streak"))
                return PARTICLE_TEX_STREAK;
        return -1;
}

static const effectinfo_t* R_Effectinfo_Find (const char* name)
{
        int i;
        if (!effectinfo_active || !name || !*name)
                return NULL;
        for (i = 0; i < num_effectinfos; ++i)
        {
                if (!q_strcasecmp (effectinfos[i].name, name))
                        return &effectinfos[i];
        }
        return NULL;
}

static void R_Effectinfo_Load (void)
{
        char* text;
        const char* data;
        const char* next;
        effectinfo_t* current = NULL;
        qboolean warned_limit = false;

        R_Effectinfo_Clear ();

        text = (char*)COM_LoadMallocFile ("effectinfo.txt", NULL);
        if (!text)
        {
                Con_Printf ("effectinfo.txt not found, using legacy particles\n");
                return;
        }

        data = text;
        while ((next = COM_Parse (data)) != NULL)
        {
                data = next;

                if (!com_token[0])
                        break;

                if (!q_strcasecmp (com_token, "effect"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        if (!com_token[0])
                                break;

                        if (num_effectinfos >= MAX_EFFECTINFO)
                        {
                                if (!warned_limit)
                                {
                                        Con_Printf ("effectinfo.txt: maximum of %d definitions reached, ignoring the rest\n", MAX_EFFECTINFO);
                                        warned_limit = true;
                                }
                                current = NULL;
                                continue;
                        }

                        current = &effectinfos[num_effectinfos++];
                        memset (current, 0, sizeof (*current));
                        R_Effectinfo_SetDefaults (current);
                        q_strlcpy (current->name, com_token, sizeof (current->name));
                        effectinfo_active = true;
                        continue;
                }

                if (!current)
                        continue;

                if (!q_strcasecmp (com_token, "type"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->shape = R_Effectinfo_ParseShape (com_token);
                        if (current->texture < 0)
                                current->texture = R_Effectinfo_TextureForShape (current->shape);
                        continue;
                }

                if (!q_strcasecmp (com_token, "count"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->count = (int)Q_atof (com_token);
                        if (current->count < 0)
                                current->count = 0;
                        continue;
                }

                if (!q_strcasecmp (com_token, "color"))
                {
                        int c;
                        for (c = 0; c < 3; c++)
                        {
                                next = COM_Parse (data);
                                if (!next)
                                        break;
                                data = next;
                                current->color[c] = Q_atof (com_token) * (1.0f / 255.0f);
                                current->color[c] = q_max (0.f, q_min (current->color[c], 1.f));
                        }
                        continue;
                }

                if (!q_strcasecmp (com_token, "alpha"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->alpha_start = Q_atof (com_token);
                        continue;
                }

                if (!q_strcasecmp (com_token, "alpha2"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->alpha_end = Q_atof (com_token);
                        continue;
                }

                if (!q_strcasecmp (com_token, "fade"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->alpha_power = Q_atof (com_token);
                        if (current->alpha_power <= 0.f)
                                current->alpha_power = 1.f;
                        continue;
                }

                if (!q_strcasecmp (com_token, "size"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->size_min = Q_atof (com_token);
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->size_max = Q_atof (com_token);
                        continue;
                }

                if (!q_strcasecmp (com_token, "sizeincrease"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->size_increase_min = Q_atof (com_token);
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->size_increase_max = Q_atof (com_token);
                        continue;
                }

                if (!q_strcasecmp (com_token, "velocity"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->velocity_min = Q_atof (com_token);
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->velocity_max = Q_atof (com_token);
                        continue;
                }

                if (!q_strcasecmp (com_token, "velocityjitter"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->velocity_jitter = Q_atof (com_token);
                        continue;
                }

                if (!q_strcasecmp (com_token, "gravity"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->gravity = Q_atof (com_token);
                        continue;
                }

                if (!q_strcasecmp (com_token, "airfriction"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->airfriction = Q_atof (com_token);
                        if (current->airfriction < 0.f)
                                current->airfriction = 0.f;
                        continue;
                }

                if (!q_strcasecmp (com_token, "lifetime"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->lifetime = Q_atof (com_token);
                        if (current->lifetime <= 0.f)
                                current->lifetime = 0.05f;
                        continue;
                }

                if (!q_strcasecmp (com_token, "spawnradius"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->spawnradius = Q_atof (com_token);
                        if (current->spawnradius < 0.f)
                                current->spawnradius = 0.f;
                        continue;
                }

                if (!q_strcasecmp (com_token, "glow"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->glow = Q_atof (com_token);
                        current->glow = q_max (0.f, q_min (current->glow, 4.f));
                        continue;
                }

                if (!q_strcasecmp (com_token, "texture"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        current->texture = R_Effectinfo_ParseTexture (com_token);
                        continue;
                }
        }

        free (text);

        if (effectinfo_active)
        {
                q_strlcpy (effectinfo_source, "effectinfo.txt", sizeof (effectinfo_source));
                Con_Printf ("Loaded %d particle effects from %s\n", num_effectinfos, effectinfo_source);
        }
        else
        {
                Con_Printf ("effectinfo.txt present but empty, using legacy particles\n");
        }
}

static qboolean R_Effectinfo_SpawnFx (const effectinfo_t* fx, const vec3_t org, const vec3_t dir, float count_scale)
{
        int             i, total;
        float           spawn_total;
        vec3_t          dirnorm;
        float           dirlen;

        if (!fx)
                return false;

        spawn_total = (fx->count > 0) ? (float)fx->count : 1.f;
        if (count_scale > 0.f)
                spawn_total *= count_scale;
        if (spawn_total < 1.f)
                spawn_total = 1.f;

        total = (int)ceilf (spawn_total);
        if (total < 1)
                total = 1;

        dirlen = VectorLength (dir);
        if (dirlen > 0.0001f)
                VectorScale (dir, 1.f / dirlen, dirnorm);
        else
                VectorClear (dirnorm);

        for (i = 0; i < total; ++i)
        {
                particle_t* p = R_AllocParticle ();
                vec3_t velocity;
                vec3_t choose_dir;
                float  speed;

                if (!p)
                        return (i > 0);

                p->custom = 1;
                p->type = pt_static;
                p->spawn = cl.time;
                p->die = cl.time + fx->lifetime;
                if (p->die <= p->spawn)
                        p->die = p->spawn + 0.05f;

                VectorCopy (org, p->org);
                if (fx->spawnradius > 0.f)
                {
                        p->org[0] += R_Effectinfo_CRandom (fx->spawnradius);
                        p->org[1] += R_Effectinfo_CRandom (fx->spawnradius);
                        p->org[2] += R_Effectinfo_CRandom (fx->spawnradius);
                }

                if (dirlen > 0.0001f)
                {
                        VectorCopy (dirnorm, choose_dir);
                }
                else
                {
                        do
                        {
                                choose_dir[0] = R_Effectinfo_CRandom (1.f);
                                choose_dir[1] = R_Effectinfo_CRandom (1.f);
                                choose_dir[2] = R_Effectinfo_CRandom (1.f);
                        } while (VectorLength (choose_dir) < 0.0001f);
                        VectorNormalize (choose_dir);
                }

                speed = fx->velocity_min;
                if (fx->velocity_max > fx->velocity_min)
                        speed = R_Effectinfo_Random (fx->velocity_min, fx->velocity_max);
                VectorScale (choose_dir, speed, velocity);

                if (fx->velocity_jitter != 0.f)
                {
                        velocity[0] += R_Effectinfo_CRandom (fx->velocity_jitter);
                        velocity[1] += R_Effectinfo_CRandom (fx->velocity_jitter);
                        velocity[2] += R_Effectinfo_CRandom (fx->velocity_jitter);
                }

                VectorCopy (velocity, p->vel);
                VectorClear (p->accel);
                if (fx->gravity != 0.f)
                        p->accel[2] = -fx->gravity;

                p->airfriction = fx->airfriction;
                p->alpha_start = fx->alpha_start;
                p->alpha_end = fx->alpha_end;
                p->alpha_power = (fx->alpha_power > 0.f) ? fx->alpha_power : 1.f;
                p->glow = fx->glow;
                p->texture = (fx->texture >= 0) ? fx->texture : R_Effectinfo_TextureForShape (fx->shape);
                p->custom_color[0] = q_max (0.f, q_min (fx->color[0], 1.f));
                p->custom_color[1] = q_max (0.f, q_min (fx->color[1], 1.f));
                p->custom_color[2] = q_max (0.f, q_min (fx->color[2], 1.f));

                {
                        float size = fx->size_min;
                        if (fx->size_max > fx->size_min)
                                size = R_Effectinfo_Random (fx->size_min, fx->size_max);
                        if (size <= 0.f)
                                size = 1.f;
                        p->size = q_max (0.05f, size * (1.f / 16.f));
                }

                {
                        float sizedelta = fx->size_increase_min;
                        if (fx->size_increase_max > fx->size_increase_min)
                                sizedelta = R_Effectinfo_Random (fx->size_increase_min, fx->size_increase_max);
                        p->size_vel = sizedelta * (1.f / 16.f);
                }

                p->color = 0;
                p->ramp = 0.f;
        }

        return total > 0;
}

cvar_t  r_particles = { "r_particles","2", CVAR_ARCHIVE }; //johnfitz

typedef struct particlevert_t {
        vec3_t      pos;
        GLubyte     color[4];
        GLubyte     params[4];
        GLfloat     custom[2];
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

enum
{
	PARTICLE_ATLAS_WIDTH = PARTICLE_TEX_TILE_SIZE * NUM_PARTICLE_TEXTURES,
	PARTICLE_ATLAS_HEIGHT = PARTICLE_TEX_TILE_SIZE
};

static void R_InitParticleAtlas (void)
{
	if (particle_atlas)
		return;

	const int atlas_width = PARTICLE_ATLAS_WIDTH;
	const int atlas_height = PARTICLE_ATLAS_HEIGHT;
	// RGBA8, byte-orientiert → unabhängig von Endianness
	byte data[PARTICLE_ATLAS_WIDTH * PARTICLE_ATLAS_HEIGHT * 4];
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
				float radius = sqrtf (u * u + v * v);
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

				const byte r = (byte)q_min (255, (int)(intensity * 255.f + 0.5f));
				const byte g = r;
				const byte b = r;
				const byte a = (byte)q_min (255, (int)(alpha * 255.f + 0.5f));

				const int pix = (y * atlas_width + atlas_x) * 4;
				data[pix + 0] = r; // R
				data[pix + 1] = g; // G
				data[pix + 2] = b; // B
				data[pix + 3] = a; // A
			}
		}
	}

	particle_atlas = TexMgr_LoadImageEx (NULL, "particles/atlas",
		atlas_width, atlas_height, 1, SRC_RGBA,
		(byte*)data, "", 0,
		TEXPREF_LINEAR | TEXPREF_NOPICMIP | TEXPREF_PERSIST | TEXPREF_CLAMP);

	if (!particle_atlas) {
		Con_Printf ("R_InitParticleAtlas: failed to create particle atlas\n");
	}

	particle_atlas_tile_width = 1.0f / (float)NUM_PARTICLE_TEXTURES;
	particle_atlas_tile_height = 1.0f;
	particle_atlas_tile_margin = 0.5f / (float)PARTICLE_TEX_TILE_SIZE;
}

/*
===============
R_SetParticleTexture_f -- johnfitz
===============
*/
static void R_SetParticleTexture_f (cvar_t* var)
{
	switch ((int)(r_particles.value))
	{
	case 1:
		uvscale = 1.0f;
		texturescalefactor = 1.27f;
		break;
	case 2:
		uvscale = 0.25f;
		texturescalefactor = 1.0f;
		break;
	default:
		// robuster Default (entspricht Modus 2 / moderne Quad-Partikel)
		uvscale = 0.25f;
		texturescalefactor = 1.0f;
		break;
	}
}

/*
===============
R_AllocParticle
===============
*/
particle_t* R_AllocParticle (void)
{
        if (r_numactiveparticles < r_numparticles)
        {
                particle_t* p = &particles[r_numactiveparticles++];
                p->spawn = cl.time;
                p->custom = 0;
                p->type = pt_static;
                p->alpha_start = 1.f;
                p->alpha_end = 0.f;
                p->alpha_power = 1.f;
                p->airfriction = 0.f;
                p->glow = 0.f;
                p->texture = PARTICLE_TEX_SOFT;
                p->size = 1.f;
                p->size_vel = 0.f;
                p->color = 0;
                p->ramp = 0.f;
                VectorClear (p->accel);
                VectorClear (p->custom_color);
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
	int     i;

	i = COM_CheckParm ("-particles");

	if (i && i < com_argc - 1)
	{
		r_numparticles = atoi (com_argv[i + 1]);
		if (r_numparticles < ABSOLUTE_MIN_PARTICLES)
			r_numparticles = ABSOLUTE_MIN_PARTICLES;
	}
	else
	{
		r_numparticles = MAX_PARTICLES;
	}

	particles = (particle_t*)
		Hunk_AllocName (r_numparticles * sizeof (particle_t), "particles");
	r_numactiveparticles = 0;

	Cvar_RegisterVariable (&r_particles); //johnfitz
        Cvar_SetCallback (&r_particles, R_SetParticleTexture_f);
        R_SetParticleTexture_f (&r_particles); // set default

        R_InitParticleAtlas ();
        R_Effectinfo_Load ();
}

/*
===============
R_EntityParticles
===============
*/
static vec3_t  avelocities[NUMVERTEXNORMALS];
static float   beamlength = 16;

void R_EntityParticles (entity_t* ent)
{
	int         i;
	particle_t* p;
	float       angle;
	float       sp, sy, cp, cy;
	//  float     sr, cr;
	//  int      count;
	vec3_t      forward;
	float       dist;

	dist = 64;
	//  count = 50;

	if (!avelocities[0][0])
	{
		for (i = 0; i < NUMVERTEXNORMALS; i++)
		{
			avelocities[i][0] = (rand () & 255) * 0.01;
			avelocities[i][1] = (rand () & 255) * 0.01;
			avelocities[i][2] = (rand () & 255) * 0.01;
		}
	}

	for (i = 0; i < NUMVERTEXNORMALS; i++)
	{
		angle = cl.time * avelocities[i][0];
		sy = sin (angle);
		cy = cos (angle);
		angle = cl.time * avelocities[i][1];
		sp = sin (angle);
		cp = cos (angle);
		angle = cl.time * avelocities[i][2];
		//  sr = sin(angle);
		//  cr = cos(angle);

		forward[0] = cp * cy;
		forward[1] = cp * sy;
		forward[2] = -sp;

		if (!(p = R_AllocParticle ()))
			return;

		p->die = cl.time + 0.01;
		p->color = 0x6f;
		p->type = pt_explode;

		p->org[0] = ent->origin[0] + r_avertexnormals[i][0] * dist + forward[0] * beamlength;
		p->org[1] = ent->origin[1] + r_avertexnormals[i][1] * dist + forward[1] * beamlength;
		p->org[2] = ent->origin[2] + r_avertexnormals[i][2] * dist + forward[2] * beamlength;
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
	vec3_t      org, dir;
	int         i, count, msgcount, color;

	for (i = 0; i < 3; i++)
		org[i] = MSG_ReadCoord (cl.protocolflags);
	for (i = 0; i < 3; i++)
		dir[i] = MSG_ReadChar () * (1.0 / 16);
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
	int         i, j;
	particle_t* p;

	for (i = 0; i < 1024; i++)
	{
		if (!(p = R_AllocParticle ()))
			return;

		p->die = cl.time + 5;
		p->color = ramp1[0];
		p->ramp = rand () & 3;
		if (i & 1)
		{
			p->type = pt_explode;
			for (j = 0; j < 3; j++)
			{
				p->org[j] = org[j] + ((rand () % 32) - 16);
				p->vel[j] = (rand () % 512) - 256;
			}
		}
		else
		{
			p->type = pt_explode2;
			for (j = 0; j < 3; j++)
			{
				p->org[j] = org[j] + ((rand () % 32) - 16);
				p->vel[j] = (rand () % 512) - 256;
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
	int         i, j;
	particle_t* p;
	int         colorMod = 0;

	for (i = 0; i < 512; i++)
	{
		if (!(p = R_AllocParticle ()))
			return;

		p->die = cl.time + 0.3;
		p->color = colorStart + (colorMod % colorLength);
		colorMod++;

		p->type = pt_blob;
		for (j = 0; j < 3; j++)
		{
			p->org[j] = org[j] + ((rand () % 32) - 16);
			p->vel[j] = (rand () % 512) - 256;
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
	int         i, j;
	particle_t* p;

	for (i = 0; i < 1024; i++)
	{
		if (!(p = R_AllocParticle ()))
			return;

		p->die = cl.time + 1 + (rand () & 8) * 0.05;

		if (i & 1)
		{
			p->type = pt_blob;
			p->color = 66 + rand () % 6;
			for (j = 0; j < 3; j++)
			{
				p->org[j] = org[j] + ((rand () % 32) - 16);
				p->vel[j] = (rand () % 512) - 256;
			}
		}
		else
		{
			p->type = pt_blob2;
			p->color = 150 + rand () % 6;
			for (j = 0; j < 3; j++)
			{
				p->org[j] = org[j] + ((rand () % 32) - 16);
				p->vel[j] = (rand () % 512) - 256;
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
	int         i, j;
	particle_t* p;

	if (count > 0)
	{
		int basecolor = color & ~7;

		if (basecolor == 64 || basecolor == 72)
		{
			R_AddBloodDecal (org, dir);
			R_AddGibDecal (org, count);
		}
	}

	for (i = 0; i < count; i++)
	{
		if (!(p = R_AllocParticle ()))
			return;

		if (count == 1024)
		{   // rocket explosion
			p->die = cl.time + 5;
			p->color = ramp1[0];
			p->ramp = rand () & 3;
			if (i & 1)
			{
				p->type = pt_explode;
				for (j = 0; j < 3; j++)
				{
					p->org[j] = org[j] + ((rand () % 32) - 16);
					p->vel[j] = (rand () % 512) - 256;
				}
			}
			else
			{
				p->type = pt_explode2;
				for (j = 0; j < 3; j++)
				{
					p->org[j] = org[j] + ((rand () % 32) - 16);
					p->vel[j] = (rand () % 512) - 256;
				}
			}
		}
		else
		{
			p->die = cl.time + 0.1 * (rand () % 5);
			p->color = (color & ~7) + (rand () & 7);
			p->type = pt_slowgrav;
			for (j = 0; j < 3; j++)
			{
				p->org[j] = org[j] + ((rand () & 15) - 8);
				p->vel[j] = dir[j] * 15;// + (rand()%300)-150;
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
	int         i, j, k;
	particle_t* p;
	float       vel;
	vec3_t      dir;

	for (i = -16; i < 16; i++)
		for (j = -16; j < 16; j++)
			for (k = 0; k < 1; k++)
			{
				if (!(p = R_AllocParticle ()))
					return;

				p->die = cl.time + 2 + (rand () & 31) * 0.02;
				p->color = 224 + (rand () & 7);
				p->type = pt_slowgrav;

				dir[0] = j * 8 + (rand () & 7);
				dir[1] = i * 8 + (rand () & 7);
				dir[2] = 256;

				p->org[0] = org[0] + dir[0];
				p->org[1] = org[1] + dir[1];
				p->org[2] = org[2] + (rand () & 63);

				VectorNormalize (dir);
				vel = 50 + (rand () & 63);
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
	int         i, j, k;
	particle_t* p;
	float       vel;
	vec3_t      dir;

	for (i = -16; i < 16; i += 4)
	{
		for (j = -16; j < 16; j += 4)
		{
			for (k = -24; k < 32; k += 4)
			{
				if (!(p = R_AllocParticle ()))
					return;

				p->die = cl.time + 0.2 + (rand () & 7) * 0.02;
				p->color = 7 + (rand () & 7);
				p->type = pt_slowgrav;

				dir[0] = j * 8;
				dir[1] = i * 8;
				dir[2] = k * 8;

				p->org[0] = org[0] + i + (rand () & 3);
				p->org[1] = org[1] + j + (rand () & 3);
				p->org[2] = org[2] + k + (rand () & 3);

				VectorNormalize (dir);
				vel = 50 + (rand () & 63);
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
	vec3_t      vec;
	float       len;
	int         j;
	particle_t* p;
	int         dec;
	static int  tracercount;

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
		case 0: // rocket trail
			p->ramp = (rand () & 3);
			p->color = ramp3[(int)p->ramp];
			p->type = pt_fire;
			for (j = 0; j < 3; j++)
				p->org[j] = start[j] + ((rand () % 6) - 3);
			break;

		case 1: // smoke smoke
			p->ramp = (rand () & 3) + 2;
			p->color = ramp3[(int)p->ramp];
			p->type = pt_fire;
			for (j = 0; j < 3; j++)
				p->org[j] = start[j] + ((rand () % 6) - 3);
			break;

		case 2: // blood
			p->type = pt_grav;
			p->color = 67 + (rand () & 3);
			for (j = 0; j < 3; j++)
				p->org[j] = start[j] + ((rand () % 6) - 3);
			break;

		case 3:
		case 5: // tracer
			p->die = cl.time + 0.5;
			p->type = pt_static;
			if (type == 3)
				p->color = 52 + ((tracercount & 4) << 1);
			else
				p->color = 230 + ((tracercount & 4) << 1);

			tracercount++;

			VectorCopy (start, p->org);
			if (tracercount & 1)
			{
				p->vel[0] = 30 * vec[1];
				p->vel[1] = 30 * -vec[0];
			}
			else
			{
				p->vel[0] = 30 * -vec[1];
				p->vel[1] = 30 * vec[0];
			}
			break;

		case 4: // slight blood
			p->type = pt_grav;
			p->color = 67 + (rand () & 3);
			for (j = 0; j < 3; j++)
				p->org[j] = start[j] + ((rand () % 6) - 3);
			len -= 3;
			break;

		case 6: // voor trail
			p->color = 9 * 16 + 8 + (rand () & 3);
			p->type = pt_static;
			p->die = cl.time + 0.3;
			for (j = 0; j < 3; j++)
				p->org[j] = start[j] + ((rand () % 15) - 8);
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
	particle_t* p;
	int             i, cur, active;
	float           time1, time2, time3, dvel, frametime, grav;
	extern  cvar_t  sv_gravity;

	frametime = cl.time - cl.oldtime;
	time3 = frametime * 15;
	time2 = frametime * 10;
	time1 = frametime * 5;
	grav = frametime * sv_gravity.value * 0.05;
	dvel = 4 * frametime;

	for (cur = active = 0, p = particles; cur < r_numactiveparticles; cur++, p++)
	{
		if (p->die < cl.time)
			continue;
		if (p->spawn > cl.time) {
			// noch nicht aktiv, aber behalten
			if (cur != active) particles[active] = *p;
			active++;
			continue;
		}

                if (p->custom)
                {
                        vec3_t vel;
                        VectorCopy (p->vel, vel);
                        vel[0] += p->accel[0] * frametime;
                        vel[1] += p->accel[1] * frametime;
                        vel[2] += p->accel[2] * frametime;
                        if (p->airfriction > 0.f)
                        {
                                float friction = 1.f - p->airfriction * frametime;
                                if (friction < 0.f)
                                        friction = 0.f;
                                vel[0] *= friction;
                                vel[1] *= friction;
                                vel[2] *= friction;
                        }
                        p->org[0] += vel[0] * frametime;
                        p->org[1] += vel[1] * frametime;
                        p->org[2] += vel[2] * frametime;
                        VectorCopy (vel, p->vel);
                        p->size = q_max (0.01f, p->size + p->size_vel * frametime);
                }
                else
                {
                        p->org[0] += p->vel[0] * frametime;
                        p->org[1] += p->vel[1] * frametime;
                        p->org[2] += p->vel[2] * frametime;

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
                                if (p->ramp >= 8)
                                        p->die = -1;
                                else
                                        p->color = ramp1[(int)p->ramp];
                                for (i = 0; i < 3; i++)
                                        p->vel[i] += p->vel[i] * dvel;
                                p->vel[2] -= grav;
                                break;

                        case pt_explode2:
                                p->ramp += time3;
                                if (p->ramp >= 8)
                                        p->die = -1;
                                else
                                        p->color = ramp2[(int)p->ramp];
                                for (i = 0; i < 3; i++)
                                        p->vel[i] -= p->vel[i] * frametime;
                                p->vel[2] -= grav;
                                break;

                        case pt_blob:
                                for (i = 0; i < 3; i++)
                                        p->vel[i] += p->vel[i] * dvel;
                                p->vel[2] -= grav;
                                break;

                        case pt_blob2:
                                for (i = 0; i < 2; i++)
                                        p->vel[i] -= p->vel[i] * dvel;
                                p->vel[2] -= grav;
                                break;

                        case pt_grav:
                        case pt_slowgrav:
                                p->vel[2] -= grav;
                                break;
                        }
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
	GLbyte* ofs;

	if (!numpartverts)
		return;

	GL_Upload (GL_ARRAY_BUFFER, partverts, sizeof (partverts[0]) * numpartverts, &buf, &ofs);
	GL_BindBuffer (GL_ARRAY_BUFFER, buf);
        GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof (partverts[0]), ofs + offsetof (particlevert_t, pos));
        GL_VertexAttribPointerFunc (1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (partverts[0]), ofs + offsetof (particlevert_t, color));
        GL_VertexAttribPointerFunc (2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof (partverts[0]), ofs + offsetof (particlevert_t, params));
        GL_VertexAttribPointerFunc (3, 2, GL_FLOAT, GL_FALSE, sizeof (partverts[0]), ofs + offsetof (particlevert_t, custom));

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
	particle_t* p;
	particlevert_t* v;
	GLubyte         color[4] = { 255, 255, 255, 255 }, * c; //johnfitz -- particle transparency
	extern  cvar_t  r_particles; //johnfitz
	float           scalex, scaley;
	qboolean        dither, oit, wants_alpha;
	int             i;

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
	scalex *= r_matproj[1 * 4 + 0]; // -1 / tan (fovx/2)
	scaley *= -r_matproj[2 * 4 + 1]; // -1 / tan (fovy/2)
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
                GL_SetState (GLS_BLEND_ALPHA_OIT | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (4) | GLS_INSTANCED_ATTRIBS (4));
        else
                GL_SetState (GLS_BLEND_OPAQUE | GLS_CULL_NONE | GLS_ATTRIBS (4) | GLS_INSTANCED_ATTRIBS (4));

	numpartverts = 0;
	for (i = 0, p = particles; i < r_numactiveparticles; i++, p++)
	{
		if (numpartverts == countof (partverts))
			R_FlushParticleBatch ();

                {
                        const particleappearance_t* appearance;
                        float lifetime, life, fade, alphaval, glow, size_scale;
                        GLubyte alphabyte, glowbyte;
                        int texindex;

                        lifetime = p->die - p->spawn;
                        life = 0.f;
                        if (lifetime > 0.f)
                                life = (cl.time - p->spawn) / lifetime;
                        life = q_min (1.f, q_max (0.f, life));

                        v = &partverts[numpartverts];
                        VectorCopy (p->org, v->pos);

                        if (p->custom)
                        {
                                fade = (p->alpha_power > 0.f) ? powf (life, p->alpha_power) : life;
                                alphaval = p->alpha_start + (p->alpha_end - p->alpha_start) * fade;
                                alphaval = q_min (1.f, q_max (0.f, alphaval));
                                if (!showtris && alphaval <= 0.f)
                                        continue;

                                glow = q_min (1.f, q_max (0.f, p->glow));
                                texindex = (p->texture >= 0 && p->texture < NUM_PARTICLE_TEXTURES) ? p->texture : PARTICLE_TEX_SOFT;
                                size_scale = q_max (0.05f, p->size);

                                if (showtris)
                                {
                                        v->color[0] = color[0];
                                        v->color[1] = color[1];
                                        v->color[2] = color[2];
                                        v->color[3] = 255;
                                        alphabyte = 255;
                                        glowbyte = 0;
                                }
                                else
                                {
                                        v->color[0] = (GLubyte)q_min (255, (int)(p->custom_color[0] * 255.f + 0.5f));
                                        v->color[1] = (GLubyte)q_min (255, (int)(p->custom_color[1] * 255.f + 0.5f));
                                        v->color[2] = (GLubyte)q_min (255, (int)(p->custom_color[2] * 255.f + 0.5f));
                                        alphabyte = (GLubyte)q_min (255, (int)(alphaval * 255.f + 0.5f));
                                        glowbyte = (GLubyte)q_min (255, (int)(glow * 255.f + 0.5f));
                                        v->color[3] = alphabyte;
                                }
                        }
                        else
                        {
                                if ((unsigned)p->type < countof (particle_appearance))
                                        appearance = &particle_appearance[p->type];
                                else
                                        appearance = &particle_appearance_default;

                                fade = appearance->fade > 0.f ? powf (life, appearance->fade) : life;
                                alphaval = appearance->alpha_start + (appearance->alpha_end - appearance->alpha_start) * fade;
                                alphaval = q_min (1.f, q_max (0.f, alphaval));
                                if (!showtris && alphaval <= 0.f)
                                        continue;

                                glow = q_min (1.f, q_max (0.f, appearance->glow));
                                texindex = appearance->texture;
                                if (texindex < 0 || texindex >= NUM_PARTICLE_TEXTURES)
                                        texindex = PARTICLE_TEX_SOFT;
                                size_scale = 1.f;

                                alphabyte = (GLubyte)q_min (255, (int)(alphaval * 255.f + 0.5f));
                                glowbyte = (GLubyte)q_min (255, (int)(glow * 255.f + 0.5f));

                                c = showtris ? color : (GLubyte*)&d_8to24table[(int)p->color];
                                v->color[0] = c[0];
                                v->color[1] = c[1];
                                v->color[2] = c[2];
                                v->color[3] = showtris ? 255 : alphabyte;
                        }

                        v->params[0] = showtris ? 0 : glowbyte;
                        v->params[1] = (GLubyte)texindex;
                        v->params[2] = 0;
                        v->params[3] = 0;
                        v->custom[0] = size_scale;
                        v->custom[1] = 0.f;

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

int R_Effectinfo_Index (const char *name)
{
        const effectinfo_t *fx;
        int i;

        if (!effectinfo_active || !name || !*name)
                return -1;

        for (i = 0; i < num_effectinfos; ++i)
        {
                fx = &effectinfos[i];
                if (!q_strcasecmp (fx->name, name))
                        return i;
        }

        return -1;
}

qboolean R_Effectinfo_SpawnIndex (int index, const vec3_t org, const vec3_t dir, float count)
{
        if (!effectinfo_active)
                return false;
        if (index < 0 || index >= num_effectinfos)
                return false;
        return R_Effectinfo_SpawnFx (&effectinfos[index], org, dir, count);
}

qboolean R_Effectinfo_SpawnName (const char *name, const vec3_t org, const vec3_t dir, float count)
{
        const effectinfo_t *fx = R_Effectinfo_Find (name);
        if (!fx)
                return false;
        return R_Effectinfo_SpawnFx (fx, org, dir, count);
}

qboolean R_Effectinfo_Active (void)
{
        return effectinfo_active;
}
