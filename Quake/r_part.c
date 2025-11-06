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
#include "image.h"

extern cvar_t sv_gravity;

static int R_PointContentsSafe (vec3_t p)
{
        if (cl.worldmodel)
        {
                mleaf_t *leaf = Mod_PointInLeaf (p, cl.worldmodel);
                if (leaf)
                {
                        int cont = leaf->contents;
                        if (cont <= CONTENTS_CURRENT_0 && cont >= CONTENTS_CURRENT_DOWN)
                                cont = CONTENTS_WATER;
                        return cont;
                }
        }

        if (sv.worldmodel)
                return SV_PointContents (p);

        return CONTENTS_EMPTY;
}

#define MAX_PARTICLES            16384   // default max # of particles at one
//  time
#define ABSOLUTE_MIN_PARTICLES   512     // no fewer than this no matter what's
                                                                                //  on the command line

typedef enum
{
        EFFECT_TYPE_ALPHASTATIC = 0,
        EFFECT_TYPE_STATIC,
        EFFECT_TYPE_SPARK,
        EFFECT_TYPE_BEAM,
        EFFECT_TYPE_BUBBLE,
        EFFECT_TYPE_BLOOD,
        EFFECT_TYPE_SMOKE,
        EFFECT_TYPE_DECAL,
        EFFECT_TYPE_ENTITY,
        EFFECT_TYPE_GENERIC
} effecttype_t;

typedef enum
{
        EFFECT_BLEND_DEFAULT = -1,
        EFFECT_BLEND_ALPHA = 0,
        EFFECT_BLEND_ADD,
        EFFECT_BLEND_INVMOD
} effectblend_t;

typedef enum
{
        EFFECT_ORIENT_DEFAULT = -1,
        EFFECT_ORIENT_BILLBOARD = 0,
        EFFECT_ORIENT_ORIENTED,
        EFFECT_ORIENT_BEAM,
        EFFECT_ORIENT_SPARK
} effectorient_t;

typedef enum
{
        EFFECT_ALPHA_LEGACY = 0,
        EFFECT_ALPHA_LERP,
        EFFECT_ALPHA_FADERATE
} effectalphamode_t;

typedef struct particle_dp_s
{
        effectblend_t   blend;
        effectorient_t  orient;
        effectalphamode_t alpha_mode;
        float           alpha_current;
        float           alpha_decay;
        float           size_y;
        float           stretch;
        float           rotation;
        float           spin;
        float           bounce;
        float           liquidfriction;
        qboolean        kill_on_draw;
        qboolean        underwater_only;
        qboolean        notunderwater;
        qboolean        leave_decal;
} particle_dp_t;

typedef struct effect_emitter_s
{
        effecttype_t    type;
        effectblend_t   blend;
        effectorient_t  orient;
        effectalphamode_t alpha_mode;
        int             count;
        float           count_scale;
        vec3_t          color_min;
        vec3_t          color_max;
        int             tex_min;
        int             tex_max;
        float           size_min;
        float           size_max;
        float           size_increase;
        float           sizey_min;
        float           sizey_max;
        float           alpha_min;
        float           alpha_max;
        float           alpha_fade;
        float           time_min;
        float           time_max;
        float           gravity;
        float           bounce;
        float           airfriction;
        float           liquidfriction;
        float           trail_spacing;
        vec3_t          origin_offset;
        vec3_t          origin_rel;
        vec3_t          origin_jitter;
        vec3_t          velocity_offset;
        vec3_t          velocity_rel;
        vec3_t          velocity_jitter;
        float           velocity_multiplier;
        float           stretch;
        float           rotate_start_min;
        float           rotate_start_max;
        float           rotate_speed_min;
        float           rotate_speed_max;
        qboolean        underwater_only;
        qboolean        notunderwater;
        qboolean        leave_decal;
        qboolean        kill_on_draw;
        qboolean        blend_explicit;
        qboolean        orient_explicit;
        qboolean        alpha_explicit;
        qboolean        sizey_explicit;
        qboolean        tex_explicit;
} effect_emitter_t;

typedef struct effectinfo_s
{
        char            name[64];
        int             first_emitter;
        int             num_emitters;
} effectinfo_t;

static int  ramp1[8] = { 0x6f, 0x6d, 0x6b, 0x69, 0x67, 0x65, 0x63, 0x61 };
static int  ramp2[8] = { 0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x68, 0x66 };
static int  ramp3[8] = { 0x6d, 0x6b, 6, 5, 4, 3 };

particle_t* particles;
int         r_numparticles, r_numactiveparticles;

static particle_dp_t         particle_dp[MAX_PARTICLES];
static cvar_t        r_particles_pool = { "r_particles_pool", "16384", CVAR_ARCHIVE };

#define MAX_EFFECTINFO               256
#define MAX_EFFECT_EMITTERS          1024

static effectinfo_t          effect_defs[MAX_EFFECTINFO];
static effect_emitter_t      effect_emitters[MAX_EFFECT_EMITTERS];
static int                   num_effectinfos;
static int                   num_effect_emitters;
static qboolean              effectinfo_active;
static char                  effectinfo_source[MAX_OSPATH];

static cvar_t        r_particles_debug = { "r_particles_debug", "0", 0 };

static particle_t* R_AllocParticle (void);

static float uvscale;
static float texturescalefactor; //johnfitz -- compensate for apparent size of different particle textures

// Hinweis: p->size entspricht Quake-Welteinheiten. Der texturescalefactor dient
// ausschließlich dazu, kleine Anpassungen für verschiedene Texturtypen
// vorzunehmen. Eine zusätzliche pauschale Quad-Skalierung findet nicht mehr
// statt, damit effectinfo-Größenwerten direkt der erwarteten Partikelgröße
// entsprechen.

#define PARTICLE_TEX_TILE_SIZE          32
#define PARTICLE_ATLAS_COLS             8
#define PARTICLE_ATLAS_ROWS             8
#define NUM_PARTICLE_TEXTURES           (PARTICLE_ATLAS_COLS * PARTICLE_ATLAS_ROWS)

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
        memset (effect_defs, 0, sizeof (effect_defs));
        memset (effect_emitters, 0, sizeof (effect_emitters));
        num_effectinfos = 0;
        num_effect_emitters = 0;
        effectinfo_active = false;
        effectinfo_source[0] = '\0';
}

static effectinfo_t* R_Effectinfo_FindDef (const char* name)
{
        int i;
        if (!name || !*name)
                return NULL;
        for (i = 0; i < num_effectinfos; ++i)
        {
                if (!q_strcasecmp (effect_defs[i].name, name))
                        return &effect_defs[i];
        }
        return NULL;
}

static effectinfo_t* R_Effectinfo_GetOrCreate (const char* name)
{
        effectinfo_t* def = R_Effectinfo_FindDef (name);
        if (def)
                return def;
        if (num_effectinfos >= MAX_EFFECTINFO)
                return NULL;
        def = &effect_defs[num_effectinfos++];
        memset (def, 0, sizeof (*def));
        q_strlcpy (def->name, name, sizeof (def->name));
        def->first_emitter = num_effect_emitters;
        def->num_emitters = 0;
        effectinfo_active = true;
        return def;
}

static const char* R_Effectinfo_TypeName (effecttype_t type)
{
        static const char* names[] =
        {
                "alphastatic",
                "static",
                "spark",
                "beam",
                "bubble",
                "blood",
                "smoke",
                "decal",
                "entity",
                "generic"
        };
        if ((unsigned)type < Q_COUNTOF (names))
                return names[(int)type];
        return "unknown";
}

static const char* R_Effectinfo_BlendName (effectblend_t blend)
{
        switch (blend)
        {
        case EFFECT_BLEND_DEFAULT:       return "default";
        case EFFECT_BLEND_ALPHA:         return "alpha";
        case EFFECT_BLEND_ADD:           return "add";
        case EFFECT_BLEND_INVMOD:        return "invmod";
        default:                         return "unknown";
        }
}

static const char* R_Effectinfo_OrientName (effectorient_t orient)
{
        switch (orient)
        {
        case EFFECT_ORIENT_DEFAULT:      return "default";
        case EFFECT_ORIENT_BILLBOARD:    return "billboard";
        case EFFECT_ORIENT_ORIENTED:     return "oriented";
        case EFFECT_ORIENT_BEAM:         return "beam";
        case EFFECT_ORIENT_SPARK:        return "spark";
        default:                         return "unknown";
        }
}

static void R_Effectinfo_DebugPrintLoaded (void)
{
        int i, j;

        if (!r_particles_debug.value)
                return;

        Con_Printf ("effectinfo debug: %d effects (%d emitters) loaded from %s\n",
                num_effectinfos, num_effect_emitters,
                effectinfo_source[0] ? effectinfo_source : "effectinfo.txt");

        for (i = 0; i < num_effectinfos; ++i)
        {
                const effectinfo_t* def = &effect_defs[i];
                Con_Printf ("  %2d: %s (%d emitters)\n", i, def->name[0] ? def->name : "<unnamed>", def->num_emitters);
                for (j = 0; j < def->num_emitters; ++j)
                {
                        const effect_emitter_t* emit = &effect_emitters[def->first_emitter + j];
                        Con_Printf ("      emitter %d: type=%s blend=%s orient=%s count=%d scale=%.2f tex=%d-%d size=%.1f-%.1f time=%.2f-%.2f alpha=%.2f-%.2f\n",
                                j,
                                R_Effectinfo_TypeName (emit->type),
                                R_Effectinfo_BlendName (emit->blend),
                                R_Effectinfo_OrientName (emit->orient),
                                emit->count,
                                emit->count_scale,
                                emit->tex_min,
                                emit->tex_max,
                                emit->size_min,
                                emit->size_max,
                                emit->time_min,
                                emit->time_max,
                                emit->alpha_min,
                                emit->alpha_max);
                }
        }
}

static effect_emitter_t* R_Effectinfo_NewEmitter (effectinfo_t* def)
{
        effect_emitter_t* emit;
        if (num_effect_emitters >= MAX_EFFECT_EMITTERS)
                return NULL;
        emit = &effect_emitters[num_effect_emitters++];
        memset (emit, 0, sizeof (*emit));
        emit->type = EFFECT_TYPE_ALPHASTATIC;
        emit->blend = EFFECT_BLEND_DEFAULT;
        emit->orient = EFFECT_ORIENT_DEFAULT;
        emit->alpha_mode = EFFECT_ALPHA_FADERATE;
        emit->count = 1;
        emit->count_scale = 1.f;
        VectorSet (emit->color_min, 1.f, 1.f, 1.f);
        VectorSet (emit->color_max, 1.f, 1.f, 1.f);
        emit->tex_min = 0;
        emit->tex_max = 0;
        emit->size_min = 4.f;
        emit->size_max = 4.f;
        emit->size_increase = 0.f;
        emit->sizey_min = 4.f;
        emit->sizey_max = 4.f;
        emit->alpha_min = 1.f;
        emit->alpha_max = 1.f;
        emit->alpha_fade = 0.f;
        emit->time_min = 0.5f;
        emit->time_max = 0.5f;
        emit->gravity = 0.f;
        emit->bounce = 0.f;
        emit->airfriction = 0.f;
        emit->liquidfriction = 0.f;
        emit->trail_spacing = 0.f;
        VectorClear (emit->origin_offset);
        VectorClear (emit->origin_rel);
        VectorClear (emit->origin_jitter);
        VectorClear (emit->velocity_offset);
        VectorClear (emit->velocity_rel);
        VectorClear (emit->velocity_jitter);
        emit->velocity_multiplier = 1.f;
        emit->stretch = 1.f;
        emit->rotate_start_min = 0.f;
        emit->rotate_start_max = 0.f;
        emit->rotate_speed_min = 0.f;
        emit->rotate_speed_max = 0.f;
        emit->underwater_only = false;
        emit->notunderwater = false;
        emit->leave_decal = false;
        emit->kill_on_draw = false;
        emit->blend_explicit = false;
        emit->orient_explicit = false;
        emit->alpha_explicit = false;
        emit->sizey_explicit = false;
        emit->tex_explicit = false;
        if (def->num_emitters == 0)
                def->first_emitter = (int)(emit - effect_emitters);
        def->num_emitters++;
        return emit;
}

static effecttype_t R_Effectinfo_ParseType (const char* token)
{
        if (!q_strcasecmp (token, "alphastatic"))
                return EFFECT_TYPE_ALPHASTATIC;
        if (!q_strcasecmp (token, "static"))
                return EFFECT_TYPE_STATIC;
        if (!q_strcasecmp (token, "spark"))
                return EFFECT_TYPE_SPARK;
        if (!q_strcasecmp (token, "beam"))
                return EFFECT_TYPE_BEAM;
        if (!q_strcasecmp (token, "bubble"))
                return EFFECT_TYPE_BUBBLE;
        if (!q_strcasecmp (token, "blood"))
                return EFFECT_TYPE_BLOOD;
        if (!q_strcasecmp (token, "smoke"))
                return EFFECT_TYPE_SMOKE;
        if (!q_strcasecmp (token, "decal"))
                return EFFECT_TYPE_DECAL;
        if (!q_strcasecmp (token, "entityparticle"))
                return EFFECT_TYPE_ENTITY;
        return EFFECT_TYPE_GENERIC;
}

static effectblend_t R_Effectinfo_ParseBlend (const char* token)
{
        if (!q_strcasecmp (token, "alpha"))
                return EFFECT_BLEND_ALPHA;
        if (!q_strcasecmp (token, "add"))
                return EFFECT_BLEND_ADD;
        if (!q_strcasecmp (token, "invmod"))
                return EFFECT_BLEND_INVMOD;
        return EFFECT_BLEND_DEFAULT;
}

static effectorient_t R_Effectinfo_ParseOrient (const char* token)
{
        if (!q_strcasecmp (token, "billboard"))
                return EFFECT_ORIENT_BILLBOARD;
        if (!q_strcasecmp (token, "oriented"))
                return EFFECT_ORIENT_ORIENTED;
        if (!q_strcasecmp (token, "beam"))
                return EFFECT_ORIENT_BEAM;
        if (!q_strcasecmp (token, "spark"))
                return EFFECT_ORIENT_SPARK;
        return EFFECT_ORIENT_DEFAULT;
}

static effectblend_t R_Effectinfo_DefaultBlend (effecttype_t type)
{
        switch (type)
        {
        case EFFECT_TYPE_STATIC:
        case EFFECT_TYPE_SPARK:
        case EFFECT_TYPE_BEAM:
                return EFFECT_BLEND_ADD;
        case EFFECT_TYPE_BLOOD:
        case EFFECT_TYPE_DECAL:
                return EFFECT_BLEND_INVMOD;
        default:
                return EFFECT_BLEND_ALPHA;
        }
}

static effectorient_t R_Effectinfo_DefaultOrient (effecttype_t type)
{
        switch (type)
        {
        case EFFECT_TYPE_SPARK:
                return EFFECT_ORIENT_SPARK;
        case EFFECT_TYPE_BEAM:
                return EFFECT_ORIENT_BEAM;
        case EFFECT_TYPE_DECAL:
                return EFFECT_ORIENT_ORIENTED;
        default:
                return EFFECT_ORIENT_BILLBOARD;
        }
}

static void R_Effectinfo_ApplyTypeDefaults (effect_emitter_t* emit)
{
        effectblend_t defblend;
        effectorient_t deforient;

        defblend = R_Effectinfo_DefaultBlend (emit->type);
        deforient = R_Effectinfo_DefaultOrient (emit->type);

        if (!emit->blend_explicit)
                emit->blend = defblend;
        if (!emit->orient_explicit)
                emit->orient = deforient;

        switch (emit->type)
        {
        case EFFECT_TYPE_SMOKE:
                if (!emit->tex_explicit)
                {
                        emit->tex_min = 8;
                        emit->tex_max = 15;
                }
                break;
        case EFFECT_TYPE_SPARK:
                if (!emit->tex_explicit)
                {
                        emit->tex_min = 24;
                        emit->tex_max = 31;
                }
                if (!emit->sizey_explicit)
                {
                        emit->sizey_min = emit->size_min * 0.35f;
                        emit->sizey_max = emit->size_max * 0.35f;
                }
                if (emit->stretch == 1.f)
                        emit->stretch = 1.5f;
                break;
        case EFFECT_TYPE_BEAM:
                if (!emit->tex_explicit)
                {
                        emit->tex_min = 24;
                        emit->tex_max = 31;
                }
                if (!emit->sizey_explicit)
                {
                        emit->sizey_min = emit->size_min * 0.5f;
                        emit->sizey_max = emit->size_max * 0.5f;
                }
                break;
        case EFFECT_TYPE_BUBBLE:
                if (!emit->tex_explicit)
                {
                        emit->tex_min = 32;
                        emit->tex_max = 39;
                }
                emit->underwater_only = true;
                break;
        case EFFECT_TYPE_BLOOD:
                emit->leave_decal = true;
                if (!emit->blend_explicit)
                        emit->blend = EFFECT_BLEND_INVMOD;
                if (!emit->tex_explicit)
                {
                        emit->tex_min = 40;
                        emit->tex_max = 47;
                }
                break;
        case EFFECT_TYPE_ENTITY:
                emit->kill_on_draw = true;
                break;
        case EFFECT_TYPE_DECAL:
                if (!emit->tex_explicit)
                {
                        emit->tex_min = 48;
                        emit->tex_max = 55;
                }
                emit->leave_decal = true;
                break;
        default:
                if (!emit->tex_explicit)
                {
                        emit->tex_min = 0;
                        emit->tex_max = 7;
                }
                break;
        }
}

static qboolean R_Effectinfo_ParseHexColor (const char* token, vec3_t out)
{
        char* end;
        unsigned long value;
        if (!token || !*token)
                return false;
        value = strtoul (token, &end, 0);
        if (*end != '\0')
                return false;
        out[0] = ((value >> 16) & 0xFF) * (1.0f / 255.0f);
        out[1] = ((value >> 8) & 0xFF) * (1.0f / 255.0f);
        out[2] = (value & 0xFF) * (1.0f / 255.0f);
        return true;
}

static void R_Effectinfo_SetColorRange (effect_emitter_t* emit, const char* first, const char** data)
{
        vec3_t minc, maxc;
        const char* before;
        const char* next;

        if (!R_Effectinfo_ParseHexColor (first, minc))
                return;

        before = *data;
        next = COM_Parse (*data);
        if (!next || !com_token[0] || !R_Effectinfo_ParseHexColor (com_token, maxc))
        {
                *data = before;
                VectorCopy (minc, emit->color_min);
                VectorCopy (minc, emit->color_max);
                return;
        }

        *data = next;
        VectorCopy (minc, emit->color_min);
        VectorCopy (maxc, emit->color_max);
}

static qboolean R_Effectinfo_ParseFloat (const char** data, float* out)
{
        const char* next = COM_Parse (*data);
        if (!next || !com_token[0])
                return false;
        *out = Q_atof (com_token);
        *data = next;
        return true;
}

static qboolean R_Effectinfo_ParseInt (const char** data, int* out)
{
        const char* next = COM_Parse (*data);
        if (!next || !com_token[0])
                return false;
        *out = (int)Q_atoi (com_token);
        *data = next;
        return true;
}

static qboolean R_Effectinfo_ParseFloatPair (const char** data, float* out_min, float* out_max)
{
        const char* before = *data;
        if (!R_Effectinfo_ParseFloat (data, out_min))
                return false;
        if (!R_Effectinfo_ParseFloat (data, out_max))
        {
                *data = before;
                return false;
        }
        return true;
}

static qboolean R_Effectinfo_ParseIntPair (const char** data, int* out_min, int* out_max)
{
        const char* before = *data;
        if (!R_Effectinfo_ParseInt (data, out_min))
                return false;
        if (!R_Effectinfo_ParseInt (data, out_max))
        {
                *data = before;
                return false;
        }
        return true;
}

static qboolean R_Effectinfo_ParseVec3 (const char** data, vec3_t out)
{
        int i;
        for (i = 0; i < 3; ++i)
        {
                if (!R_Effectinfo_ParseFloat (data, &out[i]))
                        return false;
        }
        return true;
}

static void R_Effectinfo_FinalizeEmitters (void)
{
        int i;
        for (i = 0; i < num_effect_emitters; ++i)
        {
                effect_emitter_t* emit = &effect_emitters[i];
                R_Effectinfo_ApplyTypeDefaults (emit);
        }
}

static void R_Effectinfo_Load (void)
{
        char* text;
        const char* data;
        const char* next;
        effectinfo_t* current_def = NULL;
        effect_emitter_t* current_emitter = NULL;
        qboolean warned_def_limit = false;
        qboolean warned_emit_limit = false;

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

                        current_def = R_Effectinfo_GetOrCreate (com_token);
                        if (!current_def)
                        {
                                if (!warned_def_limit)
                                {
                                        Con_Printf ("effectinfo.txt: maximum of %d effects reached, ignoring the rest\n", MAX_EFFECTINFO);
                                        warned_def_limit = true;
                                }
                                current_emitter = NULL;
                                continue;
                        }

                        current_emitter = R_Effectinfo_NewEmitter (current_def);
                        if (!current_emitter)
                        {
                                if (!warned_emit_limit)
                                {
                                        Con_Printf ("effectinfo.txt: maximum of %d emitters reached, ignoring the rest\n", MAX_EFFECT_EMITTERS);
                                        warned_emit_limit = true;
                                }
                                continue;
                        }
                        continue;
                }

                if (!current_emitter)
                        continue;

                if (!q_strcasecmp (com_token, "type"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        if (!com_token[0])
                                break;
                        current_emitter->type = R_Effectinfo_ParseType (com_token);
                        R_Effectinfo_ApplyTypeDefaults (current_emitter);
                        continue;
                }

                if (!q_strcasecmp (com_token, "blend"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        if (!com_token[0])
                                break;
                        current_emitter->blend = R_Effectinfo_ParseBlend (com_token);
                        current_emitter->blend_explicit = (current_emitter->blend != EFFECT_BLEND_DEFAULT);
                        continue;
                }

                if (!q_strcasecmp (com_token, "orientation"))
                {
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        if (!com_token[0])
                                break;
                        current_emitter->orient = R_Effectinfo_ParseOrient (com_token);
                        current_emitter->orient_explicit = (current_emitter->orient != EFFECT_ORIENT_DEFAULT);
                        continue;
                }

                if (!q_strcasecmp (com_token, "count"))
                {
                        int count;
                        if (R_Effectinfo_ParseInt (&data, &count))
                                current_emitter->count = q_max (1, count);
                        continue;
                }

                if (!q_strcasecmp (com_token, "color"))
                {
                        const char* before = data;
                        next = COM_Parse (data);
                        if (!next)
                                break;
                        data = next;
                        if (!com_token[0])
                                break;
                        R_Effectinfo_SetColorRange (current_emitter, com_token, &data);
                        continue;
                }

                if (!q_strcasecmp (com_token, "tex"))
                {
                        int tmin, tmax;
                        if (R_Effectinfo_ParseIntPair (&data, &tmin, &tmax))
                        {
                                current_emitter->tex_min = tmin;
                                current_emitter->tex_max = tmax;
                                current_emitter->tex_explicit = true;
                        }
                        continue;
                }

                if (!q_strcasecmp (com_token, "size"))
                {
                        float smin, smax;
                        if (R_Effectinfo_ParseFloatPair (&data, &smin, &smax))
                        {
                                current_emitter->size_min = q_max (0.01f, smin);
                                current_emitter->size_max = q_max (current_emitter->size_min, smax);
                                if (!current_emitter->sizey_explicit)
                                {
                                        current_emitter->sizey_min = current_emitter->size_min;
                                        current_emitter->sizey_max = current_emitter->size_max;
                                }
                        }
                        continue;
                }

                if (!q_strcasecmp (com_token, "sizeincrease"))
                {
                        float inc;
                        if (R_Effectinfo_ParseFloat (&data, &inc))
                                current_emitter->size_increase = inc;
                        continue;
                }

                if (!q_strcasecmp (com_token, "sizey"))
                {
                        float symin, symax;
                        if (R_Effectinfo_ParseFloatPair (&data, &symin, &symax))
                        {
                                current_emitter->sizey_min = q_max (0.01f, symin);
                                current_emitter->sizey_max = q_max (current_emitter->sizey_min, symax);
                                current_emitter->sizey_explicit = true;
                        }
                        else if (R_Effectinfo_ParseFloat (&data, &symin))
                        {
                                current_emitter->sizey_min = q_max (0.01f, symin);
                                current_emitter->sizey_max = current_emitter->sizey_min;
                                current_emitter->sizey_explicit = true;
                        }
                        continue;
                }

                if (!q_strcasecmp (com_token, "alpha"))
                {
                        float amin, amax, fade;
                        if (R_Effectinfo_ParseFloatPair (&data, &amin, &amax))
                        {
                                if (!R_Effectinfo_ParseFloat (&data, &fade))
                                        fade = 0.f;
                                current_emitter->alpha_min = amin * (1.f / 256.f);
                                current_emitter->alpha_max = amax * (1.f / 256.f);
                                current_emitter->alpha_fade = fade * (1.f / 256.f);
                                current_emitter->alpha_mode = EFFECT_ALPHA_FADERATE;
                                current_emitter->alpha_explicit = true;
                        }
                        continue;
                }

                if (!q_strcasecmp (com_token, "time"))
                {
                        float tmin, tmax;
                        if (R_Effectinfo_ParseFloatPair (&data, &tmin, &tmax))
                        {
                                current_emitter->time_min = q_max (0.01f, tmin);
                                current_emitter->time_max = q_max (current_emitter->time_min, tmax);
                        }
                        continue;
                }

                if (!q_strcasecmp (com_token, "gravity"))
                {
                        float g;
                        if (R_Effectinfo_ParseFloat (&data, &g))
                                current_emitter->gravity = g;
                        continue;
                }

                if (!q_strcasecmp (com_token, "bounce"))
                {
                        float b;
                        if (R_Effectinfo_ParseFloat (&data, &b))
                                current_emitter->bounce = b;
                        continue;
                }

                if (!q_strcasecmp (com_token, "airfriction"))
                {
                        float f;
                        if (R_Effectinfo_ParseFloat (&data, &f))
                                current_emitter->airfriction = f;
                        continue;
                }

                if (!q_strcasecmp (com_token, "liquidfriction"))
                {
                        float f;
                        if (R_Effectinfo_ParseFloat (&data, &f))
                                current_emitter->liquidfriction = f;
                        continue;
                }

                if (!q_strcasecmp (com_token, "originoffset"))
                {
                        R_Effectinfo_ParseVec3 (&data, current_emitter->origin_offset);
                        continue;
                }

                if (!q_strcasecmp (com_token, "relativeoriginoffset"))
                {
                        R_Effectinfo_ParseVec3 (&data, current_emitter->origin_rel);
                        continue;
                }

                if (!q_strcasecmp (com_token, "originjitter"))
                {
                        R_Effectinfo_ParseVec3 (&data, current_emitter->origin_jitter);
                        continue;
                }

                if (!q_strcasecmp (com_token, "velocityoffset"))
                {
                        R_Effectinfo_ParseVec3 (&data, current_emitter->velocity_offset);
                        continue;
                }

                if (!q_strcasecmp (com_token, "relativevelocityoffset"))
                {
                        R_Effectinfo_ParseVec3 (&data, current_emitter->velocity_rel);
                        continue;
                }

                if (!q_strcasecmp (com_token, "velocityjitter"))
                {
                        R_Effectinfo_ParseVec3 (&data, current_emitter->velocity_jitter);
                        continue;
                }

                if (!q_strcasecmp (com_token, "velocitymultiplier"))
                {
                        float m;
                        if (R_Effectinfo_ParseFloat (&data, &m))
                                current_emitter->velocity_multiplier = m;
                        continue;
                }

                if (!q_strcasecmp (com_token, "underwater"))
                {
                        current_emitter->underwater_only = true;
                        continue;
                }

                if (!q_strcasecmp (com_token, "notunderwater"))
                {
                        current_emitter->notunderwater = true;
                        continue;
                }

                if (!q_strcasecmp (com_token, "trailspacing"))
                {
                        R_Effectinfo_ParseFloat (&data, &current_emitter->trail_spacing);
                        continue;
                }

                if (!q_strcasecmp (com_token, "stretchfactor"))
                {
                        R_Effectinfo_ParseFloat (&data, &current_emitter->stretch);
                        continue;
                }

                if (!q_strcasecmp (com_token, "rotate"))
                {
                        float a0, a1, s0, s1;
                        if (R_Effectinfo_ParseFloatPair (&data, &a0, &a1))
                        {
                                if (!R_Effectinfo_ParseFloatPair (&data, &s0, &s1))
                                {
                                        current_emitter->rotate_start_min = a0;
                                        current_emitter->rotate_start_max = a1;
                                        current_emitter->rotate_speed_min = 0.f;
                                        current_emitter->rotate_speed_max = 0.f;
                                }
                                else
                                {
                                        current_emitter->rotate_start_min = a0;
                                        current_emitter->rotate_start_max = a1;
                                        current_emitter->rotate_speed_min = s0;
                                        current_emitter->rotate_speed_max = s1;
                                }
                        }
                        continue;
                }
        }

        free (text);

        if (!effectinfo_active)
        {
                Con_Printf ("effectinfo.txt present but empty, using legacy particles\n");
                return;
        }

        R_Effectinfo_FinalizeEmitters ();
        q_strlcpy (effectinfo_source, "effectinfo.txt", sizeof (effectinfo_source));
        Con_Printf ("Loaded %d particle effects (%d emitters) from %s\n", num_effectinfos, num_effect_emitters, effectinfo_source);
        R_Effectinfo_DebugPrintLoaded ();
}

static void R_Effectinfo_BuildBasis (const vec3_t dir, vec3_t forward, vec3_t right, vec3_t up)
{
        float len = VectorLength (dir);
        if (len > 0.0001f)
        {
                VectorScale (dir, 1.f / len, forward);
        }
        else
        {
                forward[0] = 0.f;
                forward[1] = 0.f;
                forward[2] = 1.f;
        }

        PerpendicularVector (right, forward);
        VectorNormalize (right);
        CrossProduct (forward, right, up);
        VectorNormalize (up);
}

static int R_Effectinfo_ClampTexture (int index)
{
        if (index < 0)
                return 0;
        if (index >= NUM_PARTICLE_TEXTURES)
                return NUM_PARTICLE_TEXTURES - 1;
        return index;
}

static qboolean R_Effectinfo_SpawnEmitter (const effect_emitter_t* emit, const vec3_t org, const vec3_t dir, float count_scale)
{
        int spawned = 0;
        float spawn_total;
        vec3_t forward, right, up;

        if (!emit)
                return false;

        spawn_total = (emit->count > 0) ? (float)emit->count : 1.f;
        if (count_scale > 0.f)
                spawn_total *= count_scale;
        if (spawn_total <= 0.f)
                return false;

        {
                int total = (int)floorf (spawn_total);
                float frac = spawn_total - (float)total;
                if (frac > 0.f && R_Effectinfo_Random (0.f, 1.f) < frac)
                        total++;
                if (total <= 0)
                        total = 1;

                R_Effectinfo_BuildBasis (dir, forward, right, up);

                while (total-- > 0)
                {
                        vec3_t spawn_org;
                        vec3_t velocity;
                        float lifetime;
                        float size_x, size_y;
                        float alpha;
                        int tex_index;
                        particle_t* p;
                        particle_dp_t* ext;
                        int idx;

                        VectorCopy (org, spawn_org);

                        if (!VectorCompare (emit->origin_offset, vec3_origin))
                                VectorAdd (spawn_org, emit->origin_offset, spawn_org);

                        if (!VectorCompare (emit->origin_rel, vec3_origin))
                        {
                                vec3_t rel;
                                rel[0] = forward[0] * emit->origin_rel[0] + right[0] * emit->origin_rel[1] + up[0] * emit->origin_rel[2];
                                rel[1] = forward[1] * emit->origin_rel[0] + right[1] * emit->origin_rel[1] + up[1] * emit->origin_rel[2];
                                rel[2] = forward[2] * emit->origin_rel[0] + right[2] * emit->origin_rel[1] + up[2] * emit->origin_rel[2];
                                VectorAdd (spawn_org, rel, spawn_org);
                        }

                        if (!VectorCompare (emit->origin_jitter, vec3_origin))
                        {
                                spawn_org[0] += R_Effectinfo_CRandom (emit->origin_jitter[0]);
                                spawn_org[1] += R_Effectinfo_CRandom (emit->origin_jitter[1]);
                                spawn_org[2] += R_Effectinfo_CRandom (emit->origin_jitter[2]);
                        }

                        VectorCopy (dir, velocity);
                        if (emit->velocity_multiplier != 0.f)
                                VectorScale (velocity, emit->velocity_multiplier, velocity);

                        if (!VectorCompare (emit->velocity_offset, vec3_origin))
                                VectorAdd (velocity, emit->velocity_offset, velocity);

                        if (!VectorCompare (emit->velocity_rel, vec3_origin))
                        {
                                vec3_t rel;
                                rel[0] = forward[0] * emit->velocity_rel[0] + right[0] * emit->velocity_rel[1] + up[0] * emit->velocity_rel[2];
                                rel[1] = forward[1] * emit->velocity_rel[0] + right[1] * emit->velocity_rel[1] + up[1] * emit->velocity_rel[2];
                                rel[2] = forward[2] * emit->velocity_rel[0] + right[2] * emit->velocity_rel[1] + up[2] * emit->velocity_rel[2];
                                VectorAdd (velocity, rel, velocity);
                        }

                        if (!VectorCompare (emit->velocity_jitter, vec3_origin))
                        {
                                velocity[0] += R_Effectinfo_CRandom (emit->velocity_jitter[0]);
                                velocity[1] += R_Effectinfo_CRandom (emit->velocity_jitter[1]);
                                velocity[2] += R_Effectinfo_CRandom (emit->velocity_jitter[2]);
                        }

                        if (emit->underwater_only || emit->notunderwater)
                        {
                                int contents = R_PointContentsSafe (spawn_org);
                                qboolean in_liquid = (contents <= CONTENTS_WATER);
                                if ((emit->underwater_only && !in_liquid) || (emit->notunderwater && in_liquid))
                                        continue;
                        }

                        p = R_AllocParticle ();
                        if (!p)
                                break;

                        idx = (int)(p - particles);
                        ext = &particle_dp[idx];
                        memset (ext, 0, sizeof (*ext));

                        p->custom = 1;
                        p->type = pt_static;
                        p->spawn = cl.time;

                        lifetime = R_Effectinfo_Random (emit->time_min, emit->time_max);
                        if (lifetime <= 0.01f)
                                lifetime = 0.01f;
                        p->die = p->spawn + lifetime;

                        VectorCopy (spawn_org, p->org);

                        VectorClear (p->accel);
                        if (emit->gravity != 0.f)
                                p->accel[2] = -emit->gravity * sv_gravity.value;

                        VectorCopy (velocity, p->vel);

                        size_x = R_Effectinfo_Random (emit->size_min, emit->size_max);
                        size_x = q_max (0.01f, size_x);
                        size_y = R_Effectinfo_Random (emit->sizey_min, emit->sizey_max);
                        size_y = q_max (0.01f, size_y);
                        p->size = size_x;
                        p->size_vel = emit->size_increase;
                        ext->size_y = size_y;

                        alpha = R_Effectinfo_Random (emit->alpha_min, emit->alpha_max);
                        alpha = q_clamp (alpha, 0.f, 1.f);
                        ext->alpha_current = alpha;
                        ext->alpha_decay = q_max (0.f, emit->alpha_fade);
                        ext->alpha_mode = emit->alpha_mode;
                        p->alpha_start = alpha;
                        p->alpha_end = 0.f;
                        p->alpha_power = 1.f;

                        p->airfriction = emit->airfriction;
                        ext->liquidfriction = emit->liquidfriction;
                        ext->bounce = emit->bounce;
                        ext->stretch = emit->stretch;
                        ext->blend = emit->blend;
                        ext->orient = emit->orient;
                        ext->kill_on_draw = emit->kill_on_draw;
                        ext->underwater_only = emit->underwater_only;
                        ext->notunderwater = emit->notunderwater;
                        ext->leave_decal = emit->leave_decal;

                        ext->rotation = DEG2RAD (R_Effectinfo_Random (emit->rotate_start_min, emit->rotate_start_max));
                        ext->spin = DEG2RAD (R_Effectinfo_Random (emit->rotate_speed_min, emit->rotate_speed_max));

                        p->glow = 0.f;
                        p->color = 0;
                        p->ramp = 0.f;

                        VectorSet (p->custom_color,
                                q_clamp (R_Effectinfo_Random (emit->color_min[0], emit->color_max[0]), 0.f, 1.f),
                                q_clamp (R_Effectinfo_Random (emit->color_min[1], emit->color_max[1]), 0.f, 1.f),
                                q_clamp (R_Effectinfo_Random (emit->color_min[2], emit->color_max[2]), 0.f, 1.f));

                        tex_index = R_Effectinfo_ClampTexture (emit->tex_min);
                        {
                                int tmax = R_Effectinfo_ClampTexture (emit->tex_max);
                                if (tmax < tex_index)
                                        tmax = tex_index;
                                if (tmax > tex_index)
                                        tex_index += rand () % (tmax - tex_index + 1);
                        }
                        p->texture = (byte)tex_index;

                        spawned++;
                }
        }

        return (spawned > 0);
}

static qboolean R_Effectinfo_SpawnEffect (const effectinfo_t* def, const vec3_t org, const vec3_t dir, float count_scale)
{
        int i;
        qboolean spawned = false;

        if (!def)
        {
                if (r_particles_debug.value)
                        Con_Printf ("effectinfo debug: attempted to spawn a NULL particle effect\n");
                return false;
        }

        if (def->num_emitters <= 0)
        {
                if (r_particles_debug.value)
                        Con_Printf ("effectinfo debug: particle effect '%s' has no emitters\n", def->name);
                return false;
        }

        for (i = 0; i < def->num_emitters; ++i)
        {
                const effect_emitter_t* emit = &effect_emitters[def->first_emitter + i];
                if (R_Effectinfo_SpawnEmitter (emit, org, dir, count_scale))
                        spawned = true;
        }

        if (!spawned && r_particles_debug.value)
                Con_Printf ("effectinfo debug: particle effect '%s' produced no particles\n", def->name);

        return spawned;
}

cvar_t  r_particles = { "r_particles","2", CVAR_ARCHIVE }; //johnfitz

typedef struct particlevert_t {
        vec3_t      pos;
        vec3_t      vel;
        GLubyte     color[4];
        GLubyte     params[4];
        GLfloat     custom[4];
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

        {
                int mark = Hunk_LowMark ();
                int ext_width = 0;
                int ext_height = 0;
                enum srcformat ext_fmt = SRC_RGBA;
                byte* ext_data = Image_LoadImage ("particles/particlefont", &ext_width, &ext_height, &ext_fmt);

                if (ext_data)
                {
                        if (ext_fmt == SRC_RGBA && ext_width == atlas_width && ext_height == atlas_height)
                        {
                                particle_atlas = TexMgr_LoadImageEx (NULL, "particles/atlas",
                                        ext_width, ext_height, 1, ext_fmt,
                                        ext_data, "particles/particlefont", 0,
                                        TEXPREF_LINEAR | TEXPREF_NOPICMIP | TEXPREF_PERSIST | TEXPREF_CLAMP);
                        }
                        else
                        {
                                Con_Printf ("R_InitParticleAtlas: particles/particlefont has unexpected dimensions (%dx%d) or format\n",
                                        ext_width, ext_height);
                        }
                }

                Hunk_FreeToLowMark (mark);
        }

        if (!particle_atlas)
        {
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
                                        int atlas_y = y;
                                        float u = (float)x / (float)(PARTICLE_TEX_TILE_SIZE - 1);
                                        float v = (float)y / (float)(PARTICLE_TEX_TILE_SIZE - 1);
                                        float center_u = u - 0.5f;
                                        float center_v = v - 0.5f;
                                        float radius = sqrtf (center_u * center_u + center_v * center_v);
                                        float intensity = 0.f;
                                        float alpha = 0.f;
                                        int variant = tex % PARTICLE_ATLAS_COLS;

                                        switch (tex / PARTICLE_ATLAS_COLS)
                                        {
                                        case 0: // soft particles
                                        {
                                                float base = q_max (0.f, 1.f - radius * (1.0f + 0.12f * variant));
                                                float noise = R_ParticleNoise (x, y, 43 + tex * 97);
                                                float spike = sinf (radius * (10.f + variant * 2.f));
                                                intensity = powf (base, 1.1f + 0.2f * variant);
                                                alpha = q_clamp (intensity * (0.9f + 0.1f * noise) + spike * 0.03f, 0.f, 1.f);
                                                break;
                                        }

                                        case 1: // glows / energy
                                        {
                                                float base = q_max (0.f, 1.f - radius * (0.85f + 0.12f * variant));
                                                float swirl = sinf ((u * (3.0f + variant) - v * (2.5f + variant)) * 4.0f);
                                                float noise = R_ParticleNoise (x, y, 59 + tex * 53);
                                                intensity = base * (0.4f + 0.6f * noise) + fabsf (swirl) * 0.2f;
                                                alpha = q_min (1.f, intensity * (0.8f + 0.2f * noise));
                                                break;
                                        }

                                        case 2: // smoke / steam
                                        {
                                                float base = q_max (0.f, 1.f - radius * (0.95f + 0.1f * variant));
                                                float noise = R_ParticleNoise (x, y, 73 + tex * 79);
                                                float noise2 = R_ParticleNoise (x, y, 89 + tex * 31);
                                                intensity = powf (base, 1.6f + 0.15f * variant) * (0.75f + 0.25f * noise);
                                                alpha = intensity * (0.85f + 0.15f * noise2);
                                                break;
                                        }

                                        case 3: // streaks / trails
                                        {
                                                float streak = q_max (0.f, 1.f - fabsf (u - 0.5f) * (18.f + 2.f * variant));
                                                float noise = R_ParticleNoise (x, y, 101 + tex * 47);
                                                float noise2 = R_ParticleNoise (x, y, 107 + tex * 13);
                                                intensity = powf (streak, 2.0f) * (0.75f + 0.25f * noise);
                                                alpha = intensity * (0.8f + 0.2f * noise2);
                                                break;
                                        }

                                        case 4: // sparks
                                        {
                                                float spark = q_max (0.f, 1.f - radius * (1.2f + 0.1f * variant));
                                                float noise = R_ParticleNoise (x, y, 109 + tex * 23);
                                                float noise2 = R_ParticleNoise (x, y, 127 + tex * 11);
                                                intensity = powf (spark, 0.8f + 0.1f * variant) * (0.6f + 0.4f * noise);
                                                alpha = intensity * (0.7f + 0.3f * noise2);
                                                break;
                                        }

                                        case 5: // blood splats
                                        {
                                                float base = q_max (0.f, 1.f - radius * 1.45f);
                                                float noise = R_ParticleNoise (x, y, 113 + tex * 17);
                                                float noise2 = R_ParticleNoise (x, y, 167 + tex * 9);
                                                alpha = powf (base, 1.05f + 0.08f * variant) * (0.7f + 0.3f * noise);
                                                alpha += 0.2f * (noise2 - 0.5f) * base;
                                                break;
                                        }

                                        case 6: // decals
                                        {
                                                float ring = q_max (0.f, 1.f - powf (radius * 1.1f, 3.5f));
                                                float noise = R_ParticleNoise (x, y, 191 + tex * 5);
                                                alpha = ring * (0.9f + 0.1f * noise);
                                                break;
                                        }

                                        default: // energy / noise
                                        {
                                                float base = q_max (0.f, 1.f - radius * 1.15f);
                                                float swirl = sinf ((u * (3.0f + variant) - v * (2.5f + variant)) * 5.0f);
                                                float noise = R_ParticleNoise (x, y, 223 + tex * 3);
                                                alpha = clamp (base * (0.55f + 0.45f * noise) + fabsf (swirl) * 0.15f, 0.f, 1.f);
                                                break;
                                        }
                                        }

                                        alpha = q_clamp (alpha, 0.f, 1.f);
                                        intensity = alpha;

                                        const byte r = (byte)q_min (255, (int)(intensity * 255.f + 0.5f));
                                        const byte g = r;
                                        const byte b = r;
                                        const byte a = (byte)q_min (255, (int)(alpha * 255.f + 0.5f));

                                        const int pix = (y * atlas_width + atlas_x) * 4;
                                        data[pix + 0] = r;
                                        data[pix + 1] = g;
                                        data[pix + 2] = b;
                                        data[pix + 3] = a;
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
        }

        if (particle_atlas)
        {
                particle_atlas_tile_width = 1.0f / (float)NUM_PARTICLE_TEXTURES;
                particle_atlas_tile_height = 1.0f;
                particle_atlas_tile_margin = 0.5f / (float)PARTICLE_TEX_TILE_SIZE;
        }
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
static particle_t* R_AllocParticle (void)
{
        int idx;

        if (r_numparticles <= 0)
                return NULL;

        if (r_numactiveparticles < r_numparticles)
        {
                idx = r_numactiveparticles++;
        }
        else
        {
                double now = cl.time;
                float best_remaining = 0.f;
                int best_idx = -1;
                int i;

                for (i = 0; i < r_numactiveparticles; i++)
                {
                        particle_t *candidate = &particles[i];
                        float remaining = candidate->die - (float)now;

                        if (remaining <= 0.f)
                        {
                                best_idx = i;
                                break;
                        }

                        if (best_idx < 0 || remaining < best_remaining)
                        {
                                best_idx = i;
                                best_remaining = remaining;
                        }
                }

                if (best_idx < 0)
                        best_idx = 0;

                idx = best_idx;
        }

        {
                particle_t* p = &particles[idx];
                particle_dp_t* ext = &particle_dp[idx];
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
                memset (ext, 0, sizeof (*ext));
                return p;
        }
}

/*
===============
R_InitParticles
===============
*/
void R_InitParticles (void)
{
        int     i;
        int     pool_size;

        Cvar_RegisterVariable (&r_particles_pool);

        pool_size = (int)CLAMP ((float)ABSOLUTE_MIN_PARTICLES, r_particles_pool.value, (float)MAX_PARTICLES);

        i = COM_CheckParm ("-particles");

        if (i && i < com_argc - 1)
        {
                pool_size = atoi (com_argv[i + 1]);
                if (pool_size < ABSOLUTE_MIN_PARTICLES)
                        pool_size = ABSOLUTE_MIN_PARTICLES;
        }
        if (pool_size > MAX_PARTICLES)
                pool_size = MAX_PARTICLES;

        Cvar_SetValueQuick (&r_particles_pool, (float)pool_size);

        r_numparticles = pool_size;

        particles = (particle_t*)
                Hunk_AllocName (r_numparticles * sizeof (particle_t), "particles");
        memset (particles, 0, (size_t)r_numparticles * sizeof (*particles));
        memset (particle_dp, 0, (size_t)r_numparticles * sizeof (*particle_dp));
        r_numactiveparticles = 0;

        Cvar_RegisterVariable (&r_particles); //johnfitz
        Cvar_RegisterVariable (&r_particles_debug);
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
                        particle_dp_t* ext = &particle_dp[cur];
                        vec3_t vel, neworg;
                        vec3_t oldorg;
                        float friction = p->airfriction;
                        int contents;
                        qboolean in_liquid;

                        VectorCopy (p->vel, vel);
                        vel[0] += p->accel[0] * frametime;
                        vel[1] += p->accel[1] * frametime;
                        vel[2] += p->accel[2] * frametime;

                        contents = R_PointContentsSafe (p->org);
                        in_liquid = (contents <= CONTENTS_WATER);
                        if (in_liquid && ext->liquidfriction > 0.f)
                                friction = ext->liquidfriction;

                        if (friction > 0.f)
                        {
                                float scale = 1.f - friction * frametime;
                                if (scale < 0.f)
                                        scale = 0.f;
                                vel[0] *= scale;
                                vel[1] *= scale;
                                vel[2] *= scale;
                        }

                        VectorCopy (p->org, oldorg);
                        neworg[0] = p->org[0] + vel[0] * frametime;
                        neworg[1] = p->org[1] + vel[1] * frametime;
                        neworg[2] = p->org[2] + vel[2] * frametime;

                        if (ext->bounce >= 0.f)
                        {
                                trace_t trace;
                                memset (&trace, 0, sizeof (trace));
                                SV_RecursiveHullCheck (cl.worldmodel->hulls, 0, 0, 1, oldorg, neworg, &trace);
                                if (trace.fraction < 1.f)
                                {
                                        VectorCopy (trace.endpos, neworg);
                                        if (ext->bounce < 0.001f)
                                        {
                                                p->die = cl.time;
                                        }
                                        else
                                        {
                                                float backoff = DotProduct (vel, trace.plane.normal);
                                                backoff *= (1.f + ext->bounce);
                                                vel[0] -= backoff * trace.plane.normal[0];
                                                vel[1] -= backoff * trace.plane.normal[1];
                                                vel[2] -= backoff * trace.plane.normal[2];
                                                if (ext->leave_decal)
                                                {
                                                        R_AddGibDecal (trace.endpos, 1);
                                                        ext->leave_decal = false;
                                                }
                                        }
                                }
                        }

                        VectorCopy (neworg, p->org);
                        VectorCopy (vel, p->vel);

                        if (ext->underwater_only && !in_liquid)
                        {
                                p->die = cl.time;
                        }
                        if (ext->notunderwater && in_liquid)
                        {
                                p->die = cl.time;
                        }

                        p->size = q_max (0.01f, p->size + p->size_vel * frametime);
                        if (ext->size_y > 0.01f)
                                ext->size_y = q_max (0.01f, ext->size_y + p->size_vel * frametime);

                        if (ext->alpha_mode == EFFECT_ALPHA_FADERATE)
                        {
                                ext->alpha_current = q_max (0.f, ext->alpha_current - ext->alpha_decay * frametime);
                                if (ext->alpha_current <= 0.f)
                                        p->die = cl.time;
                        }

                        ext->rotation += ext->spin * frametime;
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
                {
                        particles[active] = *p;
                        particle_dp[active] = particle_dp[cur];
                }
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
        GL_VertexAttribPointerFunc (1, 3, GL_FLOAT, GL_FALSE, sizeof (partverts[0]), ofs + offsetof (particlevert_t, vel));
        GL_VertexAttribPointerFunc (2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (partverts[0]), ofs + offsetof (particlevert_t, color));
        GL_VertexAttribPointerFunc (3, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof (partverts[0]), ofs + offsetof (particlevert_t, params));
        GL_VertexAttribPointerFunc (4, 4, GL_FLOAT, GL_FALSE, sizeof (partverts[0]), ofs + offsetof (particlevert_t, custom));

        GL_DrawArraysInstancedFunc (GL_TRIANGLE_STRIP, 0, 4, numpartverts);

	numpartverts = 0;
}

/*
===============
R_DrawParticles_Real -- johnfitz -- moved all non-drawing code to CL_RunParticles
===============
*/
static void R_DrawParticles_Pass (effectblend_t blend, qboolean showtris, qboolean dither)
{
        particle_t* p;
        int i;
        qboolean oit = (!showtris && blend == EFFECT_BLEND_ALPHA && R_GetEffectiveAlphaMode () == ALPHAMODE_OIT);
        const float size_factor = texturescalefactor;

        GL_UseProgram (glprogs.particles[oit][dither]);
        GL_Uniform3fFunc (0, uvscale, 0.f, 0.f);

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

        GL_Uniform4fFunc (2, vright[0], vright[1], vright[2], 0.f);
        GL_Uniform4fFunc (3, vup[0], vup[1], vup[2], 0.f);
        GL_Uniform4fFunc (4, vpn[0], vpn[1], vpn[2], 0.f);

        {
                unsigned state = GLS_CULL_NONE | GLS_ATTRIBS (5) | GLS_INSTANCED_ATTRIBS (5);
                if (!showtris)
                        state |= GLS_NO_ZWRITE;
                switch (blend)
                {
                case EFFECT_BLEND_ADD:
                        state |= GLS_BLEND_ADD;
                        break;
                case EFFECT_BLEND_INVMOD:
                        state |= GLS_BLEND_INVMOD;
                        break;
                case EFFECT_BLEND_ALPHA:
                default:
                        state |= oit ? GLS_BLEND_ALPHA_OIT : GLS_BLEND_ALPHA;
                        break;
                }
                GL_SetState (state);
        }

        numpartverts = 0;

        for (i = 0, p = particles; i < r_numactiveparticles; ++i, ++p)
        {
                particle_dp_t* ext = &particle_dp[i];
                particlevert_t* v;
                effectblend_t particle_blend = EFFECT_BLEND_ALPHA;
                effectorient_t orient = EFFECT_ORIENT_BILLBOARD;
                float alphaval, glow, size_x, size_y;
                float length_param = 0.f;
                GLubyte alphabyte, glowbyte;
                int texindex;

                if (p->custom)
                {
                        particle_blend = (ext->blend != EFFECT_BLEND_DEFAULT) ? ext->blend : EFFECT_BLEND_ALPHA;
                        orient = (ext->orient != EFFECT_ORIENT_DEFAULT) ? ext->orient : EFFECT_ORIENT_BILLBOARD;
                }

                if (!showtris)
                {
                        if (p->custom)
                        {
                                if (particle_blend != blend)
                                        continue;
                        }
                        else if (blend != EFFECT_BLEND_ALPHA)
                        {
                                continue;
                        }
                }

                if (p->custom)
                {
                        float lifetime = p->die - p->spawn;
                        float life = 0.f;
                        if (lifetime > 0.f)
                                life = (cl.time - p->spawn) / lifetime;
                        life = q_clamp (life, 0.f, 1.f);

                        switch (ext->alpha_mode)
                        {
                        case EFFECT_ALPHA_FADERATE:
                                alphaval = ext->alpha_current;
                                break;
                        case EFFECT_ALPHA_LERP:
                        case EFFECT_ALPHA_LEGACY:
                        default:
                        {
                                float fade = (p->alpha_power > 0.f) ? powf (life, p->alpha_power) : life;
                                alphaval = p->alpha_start + (p->alpha_end - p->alpha_start) * fade;
                                break;
                        }
                        }

                        alphaval = q_clamp (alphaval, 0.f, 1.f);
                        glow = q_clamp (p->glow, 0.f, 1.f);
                        texindex = (p->texture >= 0 && p->texture < NUM_PARTICLE_TEXTURES) ? p->texture : PARTICLE_TEX_SOFT;
                        size_x = q_max (0.01f, p->size);
                        size_y = q_max (0.01f, ext->size_y);
                }
                else
                {
                        const particleappearance_t* appearance;
                        float lifetime = p->die - p->spawn;
                        float life = 0.f;
                        float fade;

                        if ((unsigned)p->type < countof (particle_appearance))
                                appearance = &particle_appearance[p->type];
                        else
                                appearance = &particle_appearance_default;

                        if (lifetime > 0.f)
                                life = (cl.time - p->spawn) / lifetime;
                        life = q_clamp (life, 0.f, 1.f);
                        fade = appearance->fade > 0.f ? powf (life, appearance->fade) : life;

                        alphaval = appearance->alpha_start + (appearance->alpha_end - appearance->alpha_start) * fade;
                        alphaval = q_clamp (alphaval, 0.f, 1.f);
                        glow = q_clamp (appearance->glow, 0.f, 1.f);
                        texindex = (appearance->texture >= 0 && appearance->texture < NUM_PARTICLE_TEXTURES) ? appearance->texture : PARTICLE_TEX_SOFT;
                        size_x = q_max (0.05f, p->size);
                        size_y = size_x;
                }

                if (!showtris && alphaval <= 0.f)
                        continue;

                if (numpartverts == countof (partverts))
                        R_FlushParticleBatch ();

                v = &partverts[numpartverts];
                VectorCopy (p->org, v->pos);
                VectorCopy (p->vel, v->vel);

                if (showtris)
                {
                        v->color[0] = 255;
                        v->color[1] = 255;
                        v->color[2] = 255;
                        v->color[3] = 255;
                        alphabyte = 255;
                        glowbyte = 0;
                }
                else if (p->custom)
                {
                        v->color[0] = (GLubyte)q_min (255, (int)(p->custom_color[0] * 255.f + 0.5f));
                        v->color[1] = (GLubyte)q_min (255, (int)(p->custom_color[1] * 255.f + 0.5f));
                        v->color[2] = (GLubyte)q_min (255, (int)(p->custom_color[2] * 255.f + 0.5f));
                        alphabyte = (GLubyte)q_min (255, (int)(alphaval * 255.f + 0.5f));
                        glowbyte = (GLubyte)q_min (255, (int)(glow * 255.f + 0.5f));
                        v->color[3] = alphabyte;
                }
                else
                {
                        GLubyte* c = (GLubyte*)&d_8to24table[(int)p->color];
                        alphabyte = (GLubyte)q_min (255, (int)(alphaval * 255.f + 0.5f));
                        glowbyte = (GLubyte)q_min (255, (int)(glow * 255.f + 0.5f));
                        v->color[0] = c[0];
                        v->color[1] = c[1];
                        v->color[2] = c[2];
                        v->color[3] = alphabyte;
                }

                v->params[0] = showtris ? 0 : glowbyte;
                v->params[1] = (GLubyte)texindex;
                v->params[2] = (GLubyte)orient;
                v->params[3] = 0;

                {
                        float width = size_x * size_factor;
                        float height = size_y * size_factor;

                        v->custom[0] = width;
                        v->custom[1] = height;
                        v->custom[2] = (p->custom) ? ext->rotation : 0.f;

                        if (p->custom)
                        {
                                switch (orient)
                                {
                                case EFFECT_ORIENT_BEAM:
                                        length_param = VectorLength (p->vel);
                                        if (length_param <= 0.001f)
                                                length_param = size_x;
                                        v->custom[3] = length_param;
                                        break;
                                case EFFECT_ORIENT_SPARK:
                                        length_param = VectorLength (p->vel);
                                        if (ext->stretch != 0.f)
                                                length_param *= fabsf (ext->stretch);
                                        if (length_param < size_x)
                                                length_param = size_x;
                                        v->custom[3] = length_param;
                                        break;
                                default:
                                        v->custom[3] = ext->stretch;
                                        break;
                                }
                        }
                        else
                        {
                                v->custom[3] = 0.f;
                        }
                }

                numpartverts++;

                if (p->custom && ext->kill_on_draw && !showtris)
                        p->die = cl.time;
        }

        R_FlushParticleBatch ();
}

static void R_DrawParticles_Real (qboolean alpha, qboolean showtris)
{
        extern cvar_t r_particles;
        qboolean dither;

        if (!r_particles.value)
                return;

        if (!r_numactiveparticles)
                return;

        if (!showtris && !alpha)
                return;

        GL_BeginGroup ("Particles");

        dither = (softemu == SOFTEMU_COARSE && !showtris);

        if (showtris)
        {
                R_DrawParticles_Pass (EFFECT_BLEND_ALPHA, true, dither);
        }
        else
        {
                R_DrawParticles_Pass (EFFECT_BLEND_ALPHA, false, dither);
                R_DrawParticles_Pass (EFFECT_BLEND_ADD, false, dither);
                R_DrawParticles_Pass (EFFECT_BLEND_INVMOD, false, dither);
        }

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
        effectinfo_t *def;
        int i;

        if (!effectinfo_active || !name || !*name)
                return -1;

        for (i = 0; i < num_effectinfos; ++i)
        {
                def = &effect_defs[i];
                if (!q_strcasecmp (def->name, name))
                        return i;
        }

        return -1;
}

qboolean R_Effectinfo_SpawnIndex (int index, const vec3_t org, const vec3_t dir, float count)
{
        if (!effectinfo_active)
        {
                if (r_particles_debug.value)
                        Con_Printf ("effectinfo debug: attempted to spawn effect index %d but effectinfo is inactive\n", index);
                return false;
        }
        if (index < 0 || index >= num_effectinfos)
        {
                if (r_particles_debug.value)
                        Con_Printf ("effectinfo debug: particle effect index %d is out of range (max %d)\n", index, num_effectinfos > 0 ? num_effectinfos - 1 : 0);
                return false;
        }
        return R_Effectinfo_SpawnEffect (&effect_defs[index], org, dir, count);
}

qboolean R_Effectinfo_SpawnName (const char *name, const vec3_t org, const vec3_t dir, float count)
{
        effectinfo_t *def;

        if (!effectinfo_active)
        {
                if (r_particles_debug.value)
                        Con_Printf ("effectinfo debug: attempted to spawn effect '%s' but effectinfo is inactive\n", name ? name : "(null)");
                return false;
        }

        if (!name || !*name)
        {
                if (r_particles_debug.value)
                        Con_Printf ("effectinfo debug: attempted to spawn effect with empty name\n");
                return false;
        }

        def = R_Effectinfo_FindDef (name);
        if (!def)
        {
                if (r_particles_debug.value)
                        Con_Printf ("effectinfo debug: particle effect '%s' not found in %s\n", name, effectinfo_source[0] ? effectinfo_source : "effectinfo.txt");
                return false;
        }
        return R_Effectinfo_SpawnEffect (def, org, dir, count);
}

qboolean R_Effectinfo_Active (void)
{
        return effectinfo_active;
}
