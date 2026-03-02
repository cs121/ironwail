#include "quakedef.h"
#include "r_part_q3p.h"

extern cvar_t r_particles_max;
extern cvar_t r_particles_sort;
extern cvar_t r_particles_shader_strict;
extern cvar_t r_particles_cull_dist;
extern cvar_t r_particles_collision;
extern cvar_t r_particles_spawn_max;

static q3p_particle_t *q3p_particles;
static int q3p_maxparticles;
static int q3p_numactive;
static int q3p_budgetparticles;
static int q3p_spawned_counter;
static int q3p_dropped_counter;
static int q3p_culled_counter;
static int q3p_culled_counter_frame;
static float q3p_spawn_budget_time;
static int q3p_spawn_budget_remaining;

#define Q3P_MAX_WORLD_EMITTERS 128
static q3p_emitter_t q3p_emitters[Q3P_MAX_WORLD_EMITTERS];

#define Q3P_MATERIAL_DEFAULT_REF "*particle"
#define Q3P_PARTICLE_SHADER_PREFIX "particles/"
#define Q3P_MATERIAL_CACHE_SIZE 128

enum
{
	Q3P_MATFLAG_RESOLVED = 1u << 0,
	Q3P_MATFLAG_STAGE0_VALID = 1u << 1,
	Q3P_MATFLAG_FALLBACK = 1u << 2
};

enum
{
	Q3P_MATWARN_STRICT_SKIP = 1u << 0,
	Q3P_MATWARN_BEST_EFFORT = 1u << 1
};

typedef struct q3p_material_cache_entry_s {
	qboolean used;
	char key[64];
	unsigned material_id;
	char resolved_key[64];
	unsigned resolved_material_id;
	const shader_material_t *material;
	unsigned flags;
	unsigned warned_flags;
	int supported_stage_count;
} q3p_material_cache_entry_t;

static q3p_material_cache_entry_t q3p_material_cache[Q3P_MATERIAL_CACHE_SIZE];

typedef struct q3p_drawitem_s {
	int particle_index;
	int blend_group;
	unsigned material_id;
	float depth;
} q3p_drawitem_t;

typedef struct q3p_particlevert_s {
	vec3_t		pos;
	GLubyte		color[4];
} q3p_particlevert_t;

static q3p_drawitem_t *q3p_drawitems;
static q3p_particlevert_t *q3p_partverts;
static int q3p_numdrawitems;
static int q3p_numpartverts;

#define Q3P_MAX_EFFECT_DEFS 256
typedef struct q3p_named_effectdef_s {
	qboolean used;
	char name[64];
	q3p_effectdef_t def;
} q3p_named_effectdef_t;

static q3p_named_effectdef_t q3p_effect_defs[Q3P_MAX_EFFECT_DEFS];

static void Q3P_EffectDefSetDefaults (q3p_effectdef_t *def)
{
	memset (def, 0, sizeof (*def));
	def->count = 1;
	def->lifetime_min = 0.2f;
	def->lifetime_rand = 0.0f;
	def->size = 1.0f;
	def->size_rand = 0.0f;
	def->size_ramp = 0.0f;
	def->alpha = 1.0f;
	def->alpha_ramp = -1.0f;
	def->color = 0x6f;
	def->gravity = 0.0f;
	def->drag = 0.0f;
	def->restitution = 0.0f;
	def->min_bounce_speed = 0.0f;
	def->org_jitter = 0.0f;
	def->vel_jitter = 0.0f;
	def->vel_scale = 1.0f;
	def->collide_world = false;
	q_strlcpy (def->material, "*particle", sizeof (def->material));
}

static q3p_named_effectdef_t *Q3P_FindOrAllocEffectDef (const char *name)
{
	int i;
	int free_slot = -1;

	for (i = 0; i < Q3P_MAX_EFFECT_DEFS; ++i)
	{
		if (q3p_effect_defs[i].used)
		{
			if (!q_strcasecmp (q3p_effect_defs[i].name, name))
				return &q3p_effect_defs[i];
		}
		else if (free_slot < 0)
		{
			free_slot = i;
		}
	}

	if (free_slot < 0)
		return NULL;

	q3p_effect_defs[free_slot].used = true;
	q_strlcpy (q3p_effect_defs[free_slot].name, name, sizeof (q3p_effect_defs[free_slot].name));
	Q3P_EffectDefSetDefaults (&q3p_effect_defs[free_slot].def);
	return &q3p_effect_defs[free_slot];
}

static void Q3P_ParseEffectDefText (const char *source_name, char *text)
{
	const char *cursor = text;

	while ((cursor = COM_Parse (cursor)) != NULL)
	{
		q3p_named_effectdef_t *entry;
		q3p_effectdef_t def;
		char effect_name[64];

		if (!com_token[0])
			break;

		q_strlcpy (effect_name, com_token, sizeof (effect_name));
		entry = Q3P_FindOrAllocEffectDef (effect_name);
		if (!entry)
		{
			Con_Warning ("Q3P: effect def table full, skipping '%s' from %s\n", effect_name, source_name);
			return;
		}

		def = entry->def;

		cursor = COM_Parse (cursor);
		if (!cursor || q_strcmp (com_token, "{"))
		{
			Con_Warning ("Q3P: expected '{' after effect '%s' in %s\n", effect_name, source_name);
			return;
		}

		while ((cursor = COM_Parse (cursor)) != NULL)
		{
			if (!q_strcmp (com_token, "}"))
				break;

			if (!q_strcasecmp (com_token, "material"))
			{
				if (!(cursor = COM_Parse (cursor))) break;
				q_strlcpy (def.material, com_token, sizeof (def.material));
			}
			else if (!q_strcasecmp (com_token, "count"))
			{
				if (!(cursor = COM_Parse (cursor))) break;
				def.count = q_max (1, Q_atoi (com_token));
			}
			else if (!q_strcasecmp (com_token, "lifetime_min")) { if (!(cursor = COM_Parse (cursor))) break; def.lifetime_min = Q_atof (com_token); }
			else if (!q_strcasecmp (com_token, "lifetime_rand")) { if (!(cursor = COM_Parse (cursor))) break; def.lifetime_rand = Q_atof (com_token); }
			else if (!q_strcasecmp (com_token, "size")) { if (!(cursor = COM_Parse (cursor))) break; def.size = Q_atof (com_token); }
			else if (!q_strcasecmp (com_token, "size_rand")) { if (!(cursor = COM_Parse (cursor))) break; def.size_rand = Q_atof (com_token); }
			else if (!q_strcasecmp (com_token, "size_ramp")) { if (!(cursor = COM_Parse (cursor))) break; def.size_ramp = Q_atof (com_token); }
			else if (!q_strcasecmp (com_token, "alpha")) { if (!(cursor = COM_Parse (cursor))) break; def.alpha = CLAMP (0.f, Q_atof (com_token), 1.f); }
			else if (!q_strcasecmp (com_token, "alpha_ramp")) { if (!(cursor = COM_Parse (cursor))) break; def.alpha_ramp = Q_atof (com_token); }
			else if (!q_strcasecmp (com_token, "color")) { if (!(cursor = COM_Parse (cursor))) break; def.color = Q_atoi (com_token); }
			else if (!q_strcasecmp (com_token, "gravity")) { if (!(cursor = COM_Parse (cursor))) break; def.gravity = Q_atof (com_token); }
			else if (!q_strcasecmp (com_token, "drag")) { if (!(cursor = COM_Parse (cursor))) break; def.drag = Q_atof (com_token); }
			else if (!q_strcasecmp (com_token, "restitution")) { if (!(cursor = COM_Parse (cursor))) break; def.restitution = Q_atof (com_token); }
			else if (!q_strcasecmp (com_token, "min_bounce_speed")) { if (!(cursor = COM_Parse (cursor))) break; def.min_bounce_speed = Q_atof (com_token); }
			else if (!q_strcasecmp (com_token, "org_jitter")) { if (!(cursor = COM_Parse (cursor))) break; def.org_jitter = Q_atof (com_token); }
			else if (!q_strcasecmp (com_token, "vel_jitter")) { if (!(cursor = COM_Parse (cursor))) break; def.vel_jitter = Q_atof (com_token); }
			else if (!q_strcasecmp (com_token, "vel_scale")) { if (!(cursor = COM_Parse (cursor))) break; def.vel_scale = Q_atof (com_token); }
			else if (!q_strcasecmp (com_token, "collide_world")) { if (!(cursor = COM_Parse (cursor))) break; def.collide_world = Q_atoi (com_token) != 0; }
			else
			{
				Con_DWarning ("Q3P: unknown key '%s' in effect '%s' (%s)\n", com_token, effect_name, source_name);
				cursor = COM_Parse (cursor);
			}
		}

		def.loaded = true;
		entry->def = def;
	}
}

static void Q3P_LoadEffectDefs (void)
{
	searchpath_t *search;
	int loaded = 0;

	memset (q3p_effect_defs, 0, sizeof (q3p_effect_defs));

	for (search = com_searchpaths; search; search = search->next)
	{
		if (search->pack)
		{
			int i;
			for (i = 0; i < search->pack->numfiles; ++i)
			{
				const char *path = search->pack->files[i].name;
				byte *buffer;
				size_t size = (size_t)search->pack->files[i].filelen;

				if (q_strncasecmp (path, "particles/", 10) || q_strcasecmp (COM_FileGetExtension (path), "prt"))
					continue;
				buffer = COM_LoadMallocFile (path, NULL);
				if (!buffer)
					continue;
				Q3P_ParseEffectDefText (path, (char *)buffer);
				Z_Free (buffer);
				if (size > 0)
					++loaded;
			}
		}
		else
		{
			char folder[MAX_OSPATH];
			findfile_t *find;

			if ((size_t)q_snprintf (folder, sizeof (folder), "%s/particles", search->filename) >= sizeof (folder))
				continue;

			for (find = Sys_FindFirst (folder, "prt"); find; find = Sys_FindNext (find))
			{
				char rel[MAX_QPATH];
				byte *buffer;

				if (find->attribs & FA_DIRECTORY)
					continue;
				q_snprintf (rel, sizeof (rel), "particles/%s", find->name);
				buffer = COM_LoadMallocFile (rel, NULL);
				if (!buffer)
					continue;
				Q3P_ParseEffectDefText (rel, (char *)buffer);
				Z_Free (buffer);
				++loaded;
			}
		}
	}

	if (loaded > 0)
		Con_Printf ("Q3P: loaded %d .prt particle definition files\n", loaded);
}

qboolean Q3P_GetEffectDef (const char *name, q3p_effectdef_t *out_def)
{
	int i;

	if (!name || !name[0] || !out_def)
		return false;

	for (i = 0; i < Q3P_MAX_EFFECT_DEFS; ++i)
	{
		if (!q3p_effect_defs[i].used)
			continue;
		if (q_strcasecmp (q3p_effect_defs[i].name, name))
			continue;
		*out_def = q3p_effect_defs[i].def;
		return out_def->loaded;
	}

	return false;
}


static void Q3P_RefreshBudget (void)
{
	int target = q_max (1, (int)r_particles_max.value);

	if (!q3p_particles)
		return;
	if (target > q3p_maxparticles)
		target = q3p_maxparticles;
	q3p_budgetparticles = target;
	if (q3p_numactive > q3p_budgetparticles)
	{
		q3p_dropped_counter += q3p_numactive - q3p_budgetparticles;
		q3p_numactive = q3p_budgetparticles;
	}
}

static void Q3P_RefreshSpawnBudget (void)
{
	int per_frame = q_max (0, (int)r_particles_spawn_max.value);

	if (q3p_spawn_budget_time != cl.time)
	{
		q3p_spawn_budget_time = cl.time;
		q3p_spawn_budget_remaining = per_frame;
	}
}

static qboolean Q3P_TestPointFrustum (const vec3_t point, float radius)
{
	int i;

	for (i = 0; i < 4; ++i)
	{
		if (DotProduct (point, frustum[i].normal) - frustum[i].dist <= -radius)
			return false;
	}

	return true;
}

static void Q3P_UpdateEmitters (float frametime);

static float Q3P_Clamp01 (float value)
{
	if (value < 0.f)
		return 0.f;
	if (value > 1.f)
		return 1.f;
	return value;
}

static void Q3P_SetStageTexMatrixUniform (const mat_texmatrix_t *matrix)
{
	float m[9];

	if (!matrix)
	{
		m[0] = 1.f; m[1] = 0.f; m[2] = 0.f;
		m[3] = 0.f; m[4] = 1.f; m[5] = 0.f;
		m[6] = 0.f; m[7] = 0.f; m[8] = 1.f;
	}
	else
	{
		m[0] = matrix->m[0][0]; m[1] = matrix->m[1][0]; m[2] = matrix->m[2][0];
		m[3] = matrix->m[0][1]; m[4] = matrix->m[1][1]; m[5] = matrix->m[2][1];
		m[6] = matrix->m[0][2]; m[7] = matrix->m[1][2]; m[8] = matrix->m[2][2];
	}

	GL_UniformMatrix3fvFunc (1, 1, GL_FALSE, m);
}

static void Q3P_EvalStageColorMul (const mat_shader_stage_t *stage, float particle_alpha, vec4_t out)
{
	out[0] = 1.f;
	out[1] = 1.f;
	out[2] = 1.f;
	out[3] = Q3P_Clamp01 (particle_alpha);

	if (!stage)
		return;

	switch (stage->rgbgen)
	{
	case MAT_RGBGEN_CONST:
		out[0] = stage->const_color[0];
		out[1] = stage->const_color[1];
		out[2] = stage->const_color[2];
		break;
	case MAT_RGBGEN_WAVE:
		out[0] = out[1] = out[2] = Q3P_Clamp01 (Mat_Shader_EvalWaveValue (&stage->rgb_wave, cl.time));
		break;
	case MAT_RGBGEN_VERTEX:
	case MAT_RGBGEN_IDENTITY:
	default:
		break;
	}

	switch (stage->alphagen)
	{
	case MAT_ALPHAGEN_CONST:
		out[3] *= Q3P_Clamp01 (stage->const_alpha);
		break;
	case MAT_ALPHAGEN_WAVE:
		out[3] *= Q3P_Clamp01 (Mat_Shader_EvalWaveValue (&stage->alpha_wave, cl.time));
		break;
	case MAT_ALPHAGEN_VERTEX:
	case MAT_ALPHAGEN_IDENTITY:
	default:
		break;
	}
}

static gltexture_t *Q3P_ResolveStageTexture (const mat_shader_stage_t *stage)
{
	const char *path = NULL;

	if (!stage)
		return particletexture;

	switch (stage->map_type)
	{
	case MAT_MAP_WHITE:
		return whitetexture;
	case MAT_MAP_BLACK:
		return blacktexture;
	case MAT_MAP_CLAMPMAP:
	case MAT_MAP_MAP:
	default:
		break;
	}

	path = MatStage_GetAnimMapPath ((mat_shader_stage_t *)stage, cl.time);
	if (!path || !path[0])
		path = stage->map_path;
	if (!path || !path[0])
		return particletexture;

	return TexMgr_FindTexture (cl.worldmodel, path);
}

static unsigned Q3P_HashMaterialId (const char *material)
{
	unsigned id = 2166136261u;

	if (!material || !*material)
		return 0;

	while (*material)
	{
		id ^= (unsigned char)*material++;
		id *= 16777619u;
	}

	return id;
}

static unsigned Q3P_MaterialCacheSlotForName (const char *name)
{
	return Q3P_HashMaterialId (name) & (Q3P_MATERIAL_CACHE_SIZE - 1);
}

static void Q3P_NormalizeMaterialRef (const char *name, char *out, size_t out_size)
{
	char canonical[MAX_QPATH];

	if (!name || !name[0])
	{
		q_strlcpy (out, Q3P_MATERIAL_DEFAULT_REF, out_size);
		return;
	}

	Mat_Shader_Canonicalize (name, canonical, sizeof (canonical));
	if (!canonical[0])
	{
		q_strlcpy (out, Q3P_MATERIAL_DEFAULT_REF, out_size);
		return;
	}

	if (!q_strncasecmp (canonical, Q3P_PARTICLE_SHADER_PREFIX, strlen (Q3P_PARTICLE_SHADER_PREFIX)))
		q_strlcpy (out, canonical, out_size);
	else
		q_snprintf (out, out_size, "%s%s", Q3P_PARTICLE_SHADER_PREFIX, canonical);
}

static const q3p_material_cache_entry_t *Q3P_ResolveMaterialCached (const char *name)
{
	unsigned slot;
	unsigned i;
	char normalized[64];
	char fallback_reason[128];
	const shader_material_t *material;
	qboolean stage0_ok;

	Q3P_NormalizeMaterialRef (name, normalized, sizeof (normalized));
	slot = Q3P_MaterialCacheSlotForName (normalized);

	for (i = 0; i < Q3P_MATERIAL_CACHE_SIZE; ++i)
	{
		q3p_material_cache_entry_t *entry = &q3p_material_cache[(slot + i) & (Q3P_MATERIAL_CACHE_SIZE - 1)];

		if (!entry->used)
			break;
		if (!q_strcasecmp (entry->key, normalized))
			return entry;
	}

	for (i = 0; i < Q3P_MATERIAL_CACHE_SIZE; ++i)
	{
		q3p_material_cache_entry_t *entry = &q3p_material_cache[(slot + i) & (Q3P_MATERIAL_CACHE_SIZE - 1)];

		if (entry->used)
			continue;

		memset (entry, 0, sizeof (*entry));
		entry->used = true;
		q_strlcpy (entry->key, normalized, sizeof (entry->key));
		entry->material_id = Q3P_HashMaterialId (entry->key);
		q_strlcpy (entry->resolved_key, entry->key, sizeof (entry->resolved_key));
		entry->resolved_material_id = entry->material_id;

		material = Mat_Shader_Find (entry->key);
		if (!material)
		{
			const char *texname = entry->key;
			if (!q_strncasecmp (texname, Q3P_PARTICLE_SHADER_PREFIX, strlen (Q3P_PARTICLE_SHADER_PREFIX)))
				texname += strlen (Q3P_PARTICLE_SHADER_PREFIX);
			material = Mat_Shader_FindForTextureName (texname, NULL);
		}

		stage0_ok = false;
		entry->supported_stage_count = 0;
		if (material)
		{
			int stage_count = (int)VEC_SIZE (material->stages);
			int si;
			mat_particle_policy_t policy = r_particles_shader_strict.value > 0.f
				? MAT_PARTICLE_POLICY_STRICT
				: MAT_PARTICLE_POLICY_TOLERANT;

			entry->flags |= Q3P_MATFLAG_RESOLVED;
			stage0_ok = Mat_Shader_StageSupportsParticleMVP (&material->stage0, fallback_reason, sizeof (fallback_reason));
			if (stage0_ok)
				entry->flags |= Q3P_MATFLAG_STAGE0_VALID;

			for (si = 0; si < stage_count; ++si)
			{
				const mat_shader_stage_t *stage = &material->stages[si];
				if (Mat_Shader_ClassifyParticleStage (stage, policy, fallback_reason, sizeof (fallback_reason)) == MAT_PARTICLE_STAGE_SUPPORTED)
					entry->supported_stage_count = si + 1;
				else if (r_particles_shader_strict.value > 0.f)
					break;
			}

			if (entry->supported_stage_count <= 0 && stage0_ok)
				entry->supported_stage_count = 1;
		}

		if (!material || (!stage0_ok && r_particles_shader_strict.value > 0.f))
		{
			if (material && !stage0_ok && !(entry->warned_flags & Q3P_MATWARN_STRICT_SKIP))
			{
				Con_Warning ("Q3P: strict mode skipping particle material '%s' (%s)\n", normalized, fallback_reason[0] ? fallback_reason : "unsupported stage");
				entry->warned_flags |= Q3P_MATWARN_STRICT_SKIP;
			}
			entry->flags |= Q3P_MATFLAG_FALLBACK;
			entry->material = NULL;
			q_strlcpy (entry->resolved_key, Q3P_MATERIAL_DEFAULT_REF, sizeof (entry->resolved_key));
			entry->resolved_material_id = Q3P_HashMaterialId (entry->resolved_key);
		}
		else
		{
			if (entry->supported_stage_count < (int)VEC_SIZE (material->stages)
				&& !(entry->warned_flags & Q3P_MATWARN_BEST_EFFORT))
			{
				Con_Warning ("Q3P: particle material '%s' has unsupported stages (%d/%d supported), rendering best-effort\n",
					normalized, entry->supported_stage_count, (int)VEC_SIZE (material->stages));
				entry->warned_flags |= Q3P_MATWARN_BEST_EFFORT;
			}
			entry->material = material;
		}

		return entry;
	}

	return NULL;
}

static int Q3P_DebugMaterialCacheUsedCount (void)
{
	int i;
	int used = 0;

	for (i = 0; i < Q3P_MATERIAL_CACHE_SIZE; ++i)
	{
		if (q3p_material_cache[i].used)
			++used;
	}

	return used;
}

qboolean Q3P_DebugStressMaterialCache (int iterations, int unique_names, int *before_out, int *after_out)
{
	int i;
	int before;
	int after;
	char material[64];

	if (iterations <= 0)
		iterations = 256;
	if (unique_names <= 0)
		unique_names = 1;

	before = Q3P_DebugMaterialCacheUsedCount ();
	if (before_out)
		*before_out = before;

	for (i = 0; i < iterations; ++i)
	{
		q_snprintf (material, sizeof (material), "__q3p_missing_material_%d", i % unique_names);
		if (!Q3P_ResolveMaterialCached (material))
			break;
	}

	after = Q3P_DebugMaterialCacheUsedCount ();
	if (after_out)
		*after_out = after;

	if (i < iterations)
		return false;

	/* Re-resolving the same unsupported material should not consume additional slots. */
	Q3P_ResolveMaterialCached ("__q3p_missing_material_repeat");
	if (Q3P_DebugMaterialCacheUsedCount () > after)
		return false;

	return true;
}


static qboolean Q3P_ParticleFinite (const q3p_particle_t *p)
{
	return !IS_NAN (p->org[0]) && !IS_NAN (p->org[1]) && !IS_NAN (p->org[2])
		&& !IS_NAN (p->vel[0]) && !IS_NAN (p->vel[1]) && !IS_NAN (p->vel[2])
		&& !IS_NAN (p->alpha) && !IS_NAN (p->size);
}

static qboolean Q3P_TraceWorld (const vec3_t start, const vec3_t end, trace_t *trace)
{
	vec3_t start_copy, end_copy;

	if (!cl.worldmodel)
		return false;

	memset (trace, 0, sizeof (*trace));
	trace->fraction = 1.f;
	VectorCopy (start, start_copy);
	VectorCopy (end, end_copy);
	SV_RecursiveHullCheck (cl.worldmodel->hulls, 0, 0.f, 1.f, start_copy, end_copy, trace);
	return true;
}

static qboolean Q3P_ShouldCullParticle (const q3p_particle_t *p)
{
	float cull_dist = q_max (0.f, r_particles_cull_dist.value);
	float radius = q_max (0.5f, p->size * 0.5f);
	vec3_t to_particle;

	if (cull_dist > 0.f)
	{
		VectorSubtract (p->org, r_origin, to_particle);
		if (DotProduct (to_particle, to_particle) > cull_dist * cull_dist)
			return true;
	}

	if (!Q3P_TestPointFrustum (p->org, radius))
		return true;

	return false;
}

static int Q3P_DrawSortCmp (const void *a, const void *b)
{
	const q3p_drawitem_t *da = (const q3p_drawitem_t *)a;
	const q3p_drawitem_t *db = (const q3p_drawitem_t *)b;

	if (da->blend_group != db->blend_group)
		return da->blend_group - db->blend_group;
	if (da->material_id < db->material_id)
		return -1;
	if (da->material_id > db->material_id)
		return 1;
	if (r_particles_sort.value > 0.f)
	{
		if (da->depth < db->depth)
			return 1;
		if (da->depth > db->depth)
			return -1;
	}

	return da->particle_index - db->particle_index;
}

void Q3P_Init (void)
{
	if (q3p_particles)
		return;

	q3p_maxparticles = q_max ((int)r_particles_max.value, 0);
	if (q3p_maxparticles <= 0)
		q3p_maxparticles = 1;

	q3p_particles = (q3p_particle_t *)Hunk_AllocName (q3p_maxparticles * sizeof(*q3p_particles), "q3particles");
	q3p_drawitems = (q3p_drawitem_t *)Hunk_AllocName (q3p_maxparticles * sizeof(*q3p_drawitems), "q3pdrawitems");
	q3p_partverts = (q3p_particlevert_t *)Hunk_AllocName (q3p_maxparticles * sizeof(*q3p_partverts), "q3ppartverts");
	q3p_numactive = 0;
	q3p_numdrawitems = 0;
	q3p_numpartverts = 0;
	q3p_spawned_counter = 0;
	q3p_dropped_counter = 0;
	q3p_culled_counter = 0;
	q3p_culled_counter_frame = -1;
	q3p_spawn_budget_time = -1.f;
	q3p_spawn_budget_remaining = 0;
	memset (q3p_emitters, 0, sizeof (q3p_emitters));
	memset (q3p_material_cache, 0, sizeof (q3p_material_cache));
	Q3P_LoadEffectDefs ();
	Q3P_RefreshBudget ();
}

void Q3P_Shutdown (void)
{
	q3p_particles = NULL;
	q3p_drawitems = NULL;
	q3p_partverts = NULL;
	q3p_maxparticles = 0;
	q3p_numactive = 0;
	q3p_numdrawitems = 0;
	q3p_numpartverts = 0;
	q3p_spawned_counter = 0;
	q3p_dropped_counter = 0;
	q3p_culled_counter = 0;
	q3p_culled_counter_frame = -1;
	q3p_spawn_budget_time = -1.f;
	q3p_spawn_budget_remaining = 0;
	memset (q3p_emitters, 0, sizeof (q3p_emitters));
	memset (q3p_material_cache, 0, sizeof (q3p_material_cache));
	Q3P_RefreshBudget ();
}

void Q3P_Clear (void)
{
	q3p_numactive = 0;
	q3p_numdrawitems = 0;
	q3p_numpartverts = 0;
	q3p_spawned_counter = 0;
	q3p_dropped_counter = 0;
	q3p_culled_counter = 0;
	q3p_culled_counter_frame = -1;
	q3p_spawn_budget_time = -1.f;
	q3p_spawn_budget_remaining = 0;
}

qboolean Q3P_Spawn (const q3p_particle_t *particle)
{
	q3p_particle_t *dst;
	const q3p_material_cache_entry_t *cache_entry;

	if (!particle)
		return false;
	if (!q3p_particles)
		Q3P_Init ();

	Q3P_RefreshBudget ();
	Q3P_RefreshSpawnBudget ();
	if (q3p_spawn_budget_remaining <= 0)
	{
		++q3p_dropped_counter;
		return false;
	}
	if (q3p_numactive >= q3p_budgetparticles)
	{
		++q3p_dropped_counter;
		return false;
	}

	--q3p_spawn_budget_remaining;
	++q3p_spawned_counter;
	dst = &q3p_particles[q3p_numactive++];
	*dst = *particle;
	cache_entry = Q3P_ResolveMaterialCached (particle->material);
	if (cache_entry)
	{
		q_strlcpy (dst->material, cache_entry->key, sizeof (dst->material));
		dst->material_id = cache_entry->resolved_material_id;
	}
	else
	{
		q_strlcpy (dst->material, Q3P_MATERIAL_DEFAULT_REF, sizeof (dst->material));
		dst->material_id = Q3P_HashMaterialId (dst->material);
	}
	return true;
}

void Q3P_Update (float frametime)
{
	int i, active;
	q3p_particle_t *p;
	qboolean collision_enabled = r_particles_collision.value > 0.f;

	if (!q3p_particles)
		return;

	Q3P_RefreshBudget ();
	Q3P_UpdateEmitters (frametime);
	if (q3p_numactive <= 0)
		return;

	for (i = active = 0, p = q3p_particles; i < q3p_numactive; ++i, ++p)
	{
		float age = cl.time - p->spawn_time;
		vec3_t prevorg, nextorg;

		if (age < 0 || age >= p->lifetime)
			continue;
		if (!Q3P_ParticleFinite (p))
			continue;
		p->size += p->size_ramp * frametime;
		p->alpha += p->alpha_ramp * frametime;
		p->alpha = CLAMP (0, p->alpha, 1);

		VectorCopy (p->org, prevorg);
		VectorMA (p->org, frametime, p->vel, nextorg);
		p->vel[2] -= p->gravity * frametime;
		VectorScale (p->vel, q_max (0.f, 1.f - p->drag * frametime), p->vel);

		if (collision_enabled && (p->flags & Q3P_PARTICLE_COLLIDE_WORLD))
		{
			trace_t trace;
			if (Q3P_TraceWorld (prevorg, nextorg, &trace) && trace.fraction < 1.f)
			{
				float vn;
				vec3_t vnormal;
				float speed;

				VectorCopy (trace.endpos, p->org);
				vn = DotProduct (p->vel, trace.plane.normal);
				VectorScale (trace.plane.normal, vn, vnormal);
				VectorMA (p->vel, -(1.f + CLAMP (0.f, p->restitution, 1.f)), vnormal, p->vel);
				p->org[0] += trace.plane.normal[0] * 0.5f;
				p->org[1] += trace.plane.normal[1] * 0.5f;
				p->org[2] += trace.plane.normal[2] * 0.5f;

				speed = VectorLength (p->vel);
				if (speed < q_max (0.f, p->min_bounce_speed))
					continue;
			}
			else
			{
				VectorCopy (nextorg, p->org);
			}
		}
		else
		{
			VectorCopy (nextorg, p->org);
		}

		if (!Q3P_ParticleFinite (p))
			continue;

		if (i != active)
			q3p_particles[active] = *p;
		++active;
	}

	q3p_numactive = active;
}

static float Q3P_RandUnit (void)
{
	return (float)(rand () & 0x7fff) / 32767.0f;
}

static void Q3P_UpdateEmitters (float frametime)
{
	int i;

	for (i = 0; i < Q3P_MAX_WORLD_EMITTERS; ++i)
	{
		q3p_emitter_t *e = &q3p_emitters[i];
		int spawn_count;
		int j;

		if (!e->active)
			continue;
		if (e->cull_dist > 0.f)
		{
			vec3_t d;
			VectorSubtract (e->org, r_origin, d);
			if (DotProduct (d, d) > e->cull_dist * e->cull_dist)
				continue;
		}
		if (!Q3P_TestPointFrustum (e->org, q_max (e->spread, 16.f)))
			continue;

		e->time_accum += frametime * q_max (0.f, e->rate);
		spawn_count = (int)e->time_accum;
		e->time_accum -= spawn_count;
		if (e->burst > 0)
			spawn_count += e->burst;
		e->burst = 0;

		for (j = 0; j < spawn_count; ++j)
		{
			q3p_particle_t p;
			memset (&p, 0, sizeof (p));
			p.spawn_time = cl.time;
			p.lifetime = q_max (0.05f, e->lifetime * (0.7f + 0.6f * Q3P_RandUnit ()));
			p.size = q_max (0.1f, e->size * (0.6f + 0.8f * Q3P_RandUnit ()));
			p.size_ramp = p.size * 0.5f;
			p.alpha = CLAMP (0.f, e->alpha, 1.f);
			p.alpha_ramp = -0.35f;
			p.color = e->color;
			p.gravity = e->gravity;
			p.drag = e->drag;
			p.restitution = 0.f;
			p.min_bounce_speed = 0.f;
			p.flags = 0;
			q_strlcpy (p.material, e->material[0] ? e->material : "fog_dust", sizeof (p.material));
			p.org[0] = e->org[0] + (Q3P_RandUnit () * 2.f - 1.f) * e->spread;
			p.org[1] = e->org[1] + (Q3P_RandUnit () * 2.f - 1.f) * e->spread;
			p.org[2] = e->org[2] + (Q3P_RandUnit () * 2.f - 1.f) * e->spread * 0.5f;
			p.vel[0] = (Q3P_RandUnit () * 2.f - 1.f) * 8.f;
			p.vel[1] = (Q3P_RandUnit () * 2.f - 1.f) * 8.f;
			p.vel[2] = 3.f + Q3P_RandUnit () * 8.f;
			Q3P_Spawn (&p);
		}
	}
}

static void Q3P_FlushParticleBatch (void)
{
	GLuint buf;
	GLbyte *ofs;

	if (!q3p_numpartverts)
		return;

	GL_Upload (GL_ARRAY_BUFFER, q3p_partverts, sizeof(q3p_partverts[0]) * q3p_numpartverts, &buf, &ofs);
	GL_BindBuffer (GL_ARRAY_BUFFER, buf);
	GL_VertexAttribPointerFunc (0, 3, GL_FLOAT, GL_FALSE, sizeof(q3p_partverts[0]), ofs + offsetof(q3p_particlevert_t, pos));
	GL_VertexAttribPointerFunc (1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(q3p_partverts[0]), ofs + offsetof(q3p_particlevert_t, color));
	GL_DrawArraysInstancedFunc (GL_TRIANGLE_STRIP, 0, 4, q3p_numpartverts);

	q3p_numpartverts = 0;
}

void Q3P_Draw (qboolean alpha, qboolean showtris)
{
	int i;
	int stage_index;
	int max_stage_count = 1;
	float scalex, scaley;
	qboolean dither, oit;
	unsigned saved_state;
	GLubyte white[4] = {255, 255, 255, 255};

	if (!q3p_particles)
		return;

	Q3P_RefreshBudget ();

	if (q3p_numactive <= 0)
		return;

	/* Track render-time culls once per rendered frame (opaque pass). */
	if (q3p_culled_counter_frame != r_framecount)
	{
		q3p_culled_counter = 0;
		q3p_culled_counter_frame = r_framecount;
	}

	q3p_numdrawitems = 0;
	for (i = 0; i < q3p_numactive; ++i)
	{
		q3p_particle_t *p = &q3p_particles[i];
		int blend_group = p->alpha < 1.f;

		if (!showtris && alpha != blend_group)
			continue;
		if (Q3P_ShouldCullParticle (p))
		{
			if (!showtris && !alpha)
				++q3p_culled_counter;
			continue;
		}

		q3p_drawitems[q3p_numdrawitems].particle_index = i;
		q3p_drawitems[q3p_numdrawitems].blend_group = blend_group;
		q3p_drawitems[q3p_numdrawitems].material_id = p->material_id;
		q3p_drawitems[q3p_numdrawitems].depth = DotProduct (r_origin, vpn) - DotProduct (p->org, vpn);
		++q3p_numdrawitems;
	}

	if (!q3p_numdrawitems)
		return;

	qsort (q3p_drawitems, q3p_numdrawitems, sizeof(q3p_drawitems[0]), Q3P_DrawSortCmp);

	for (i = 0; i < q3p_numdrawitems; ++i)
	{
		const q3p_material_cache_entry_t *entry = Q3P_ResolveMaterialCached (q3p_particles[q3p_drawitems[i].particle_index].material);
		if (entry && entry->supported_stage_count > max_stage_count)
			max_stage_count = entry->supported_stage_count;
	}

	GL_BeginGroup ("Q3P Particles");

	dither = (softemu == SOFTEMU_COARSE && !showtris);
	oit = (alpha && R_GetEffectiveAlphaMode () == ALPHAMODE_OIT);
	GL_UseProgram (glprogs.particles[oit][dither]);

	scalex = scaley = 0.375f;
	scalex *=  r_matproj[1*4 + 0];
	scaley *= -r_matproj[2*4 + 1];
	GL_Uniform3fFunc (0, scalex, scaley, 0.25f);
	Q3P_SetStageTexMatrixUniform (NULL);
	GL_Uniform4fFunc (2, 1.f, 1.f, 1.f, 1.f);

	saved_state = glstate;
	if (alpha)
		GL_SetState (GLS_BLEND_ALPHA_OIT | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (2) | GLS_INSTANCED_ATTRIBS (2));
	else
		GL_SetState (GLS_BLEND_OPAQUE | GLS_CULL_NONE | GLS_ATTRIBS (2) | GLS_INSTANCED_ATTRIBS (2));

	for (stage_index = 0; stage_index < max_stage_count; ++stage_index)
	{
		unsigned current_material = 0;
		const q3p_material_cache_entry_t *current_entry = NULL;
		const mat_shader_stage_t *current_stage = NULL;
		vec4_t stage_mul;

		q3p_numpartverts = 0;
		for (i = 0; i < q3p_numdrawitems; ++i)
		{
			q3p_particle_t *p = &q3p_particles[q3p_drawitems[i].particle_index];
			q3p_particlevert_t *v;
			const GLubyte *c = showtris ? white : (const GLubyte *)&d_8to24table[p->color & 0xff];
			const q3p_material_cache_entry_t *entry = Q3P_ResolveMaterialCached (p->material);

			if (!entry || stage_index >= entry->supported_stage_count)
				continue;

			if (!current_entry || p->material_id != current_material)
			{
				current_material = p->material_id;
				current_entry = entry;
				current_stage = (entry->material && stage_index < (int)VEC_SIZE (entry->material->stages))
					? &entry->material->stages[stage_index]
					: NULL;

				if (q3p_numpartverts)
					Q3P_FlushParticleBatch ();

				GL_Bind (GL_TEXTURE0, current_stage ? Q3P_ResolveStageTexture (current_stage) : particletexture);
				Q3P_SetStageTexMatrixUniform (current_stage ? MatStage_EvalTexMatrix ((mat_shader_stage_t *)current_stage, cl.time) : NULL);
				Q3P_EvalStageColorMul (current_stage, 1.f, stage_mul);
				GL_Uniform4fFunc (2, stage_mul[0], stage_mul[1], stage_mul[2], stage_mul[3]);
			}

			if (q3p_numpartverts == q3p_maxparticles)
				Q3P_FlushParticleBatch ();

			v = &q3p_partverts[q3p_numpartverts++];
			VectorCopy (p->org, v->pos);
			v->color[0] = c[0];
			v->color[1] = c[1];
			v->color[2] = c[2];
			v->color[3] = (GLubyte)(CLAMP (0.f, p->alpha, 1.f) * 255.f);
		}

		Q3P_FlushParticleBatch ();
	}

	GL_SetState (saved_state);
	GL_EndGroup ();
}

int Q3P_ActiveCount (void)
{
	return q3p_numactive;
}

void Q3P_GetDebugStats (q3p_debug_stats_t *stats)
{
	if (!stats)
		return;

	stats->active = q3p_numactive;
	stats->spawned = q3p_spawned_counter;
	stats->dropped = q3p_dropped_counter;
	stats->culled = q3p_culled_counter;
}

void Q3P_ResetDebugStats (void)
{
	q3p_spawned_counter = 0;
	q3p_dropped_counter = 0;
	q3p_culled_counter = 0;
	q3p_culled_counter_frame = -1;
}

int Q3P_AddWorldEmitter (const q3p_emitter_t *emitter)
{
	int i;

	if (!emitter)
		return -1;

	for (i = 0; i < Q3P_MAX_WORLD_EMITTERS; ++i)
	{
		if (q3p_emitters[i].active)
			continue;
		q3p_emitters[i] = *emitter;
		q3p_emitters[i].active = true;
		q3p_emitters[i].time_accum = 0.f;
		return i;
	}

	return -1;
}

void Q3P_ClearWorldEmitters (void)
{
	memset (q3p_emitters, 0, sizeof (q3p_emitters));
}
