#include "quakedef.h"
#include "glquake.h"
#include "mat_material.h"
#include "r_framegraph.h"
#include "r_part_q3p.h"

extern cvar_t r_particles_max;
extern cvar_t r_particles_sort;
extern cvar_t r_particles_material_strict;
extern cvar_t r_particles_cull_dist;
extern cvar_t r_particles_collision;
extern cvar_t r_particles_spawn_max;
extern cvar_t r_particles_gpu_sim;
extern cvar_t r_particles_gpu_cullsort;
extern cvar_t r_particles_prt_debug;

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
	const material_t *material;
	unsigned flags;
	unsigned warned_flags;
	int supported_stage_count;
} q3p_material_cache_entry_t;

static q3p_material_cache_entry_t q3p_material_cache[Q3P_MATERIAL_CACHE_SIZE];

typedef struct q3p_drawitem_s {
	int particle_index;
	int blend_group;
	unsigned material_id;
	const q3p_material_cache_entry_t *material_entry;
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

static GLubyte Q3P_ParticleLitByte (GLubyte value, float light)
{
	return (GLubyte)CLAMP (0.f, floorf ((float)value * light + 0.5f), 255.f);
}

#define Q3P_GPU_LOCAL_SIZE 64

typedef struct q3p_particle_gpu_s {
	/* std430-compatible vec4 blocks mirrored in shaders/q3p_sim.comp */
	float spawn_life_size_sizeramp[4];
	float alpha_alpharamp_gravity_drag[4];
	float org[4];
	float vel[4];
	unsigned int color_flags_material_alive[4];
	float bounce[4];
} q3p_particle_gpu_t;

typedef struct q3p_sort_entry_gpu_s {
	unsigned int key0;
	unsigned int key1;
	unsigned int index;
	unsigned int visible;
} q3p_sort_entry_gpu_t;

typedef struct q3p_gpu_state_s {
	GLuint sim_buffer[2];
	GLuint sort_buffer;
	GLuint visible_count_buffer;
	int sim_src_index;
	int sort_capacity;
	q3p_particle_gpu_t *particle_staging;
	q3p_sort_entry_gpu_t *sort_staging;
	int sim_frames;
	int sim_fallbacks;
	int cullsort_frames;
	int last_visible;
	int last_culled;
} q3p_gpu_state_t;

static q3p_gpu_state_t q3p_gpu;

COMPILE_TIME_ASSERT (q3p_particle_gpu_std430_size, sizeof (q3p_particle_gpu_t) == 96);
COMPILE_TIME_ASSERT (q3p_sort_entry_gpu_std430_size, sizeof (q3p_sort_entry_gpu_t) == 16);

#define Q3P_MAX_EFFECT_DEFS 256
typedef struct q3p_named_effectdef_s {
	qboolean used;
	char name[64];
	q3p_effectdef_t def;
} q3p_named_effectdef_t;

static q3p_named_effectdef_t q3p_effect_defs[Q3P_MAX_EFFECT_DEFS];

typedef struct q3p_prt_parser_s {
	const char *source_name;
	const char *text_start;
	const char *cursor;
	const char *token_start;
	int token_line;
	int token_col;
	char token[1024];
	int debug_level;
	int files_loaded;
	int defs_parsed;
	int warnings;
	int errors;
	qboolean had_error;
} q3p_prt_parser_t;

static int Q3P_NextPow2 (int value);
static qboolean Q3P_GPU_ComputeAvailable (void);
static void Q3P_GPU_ResetResources (void);
static qboolean Q3P_GPU_EnsureBuffers (void);
static void Q3P_GPU_PackParticle (const q3p_particle_t *src, q3p_particle_gpu_t *dst);
static void Q3P_GPU_UnpackParticle (const q3p_particle_gpu_t *src, q3p_particle_t *dst);
static qboolean Q3P_GPU_ReadBuffer (GLuint buffer, size_t size, void *dst);
static qboolean Q3P_GPU_RunSim (float frametime, qboolean collision_enabled);
static qboolean Q3P_GPU_BuildDrawList (qboolean alpha, qboolean showtris);
static void Q3P_GPU_BitonicSortEntries (int count);

static void Q3P_PRT_Log (q3p_prt_parser_t *parser, int level, const char *fmt, ...)
{
	char message[1024];
	va_list argptr;

	if (!parser || parser->debug_level < level)
		return;

	va_start (argptr, fmt);
	q_vsnprintf (message, sizeof (message), fmt, argptr);
	va_end (argptr);
	Con_Printf ("%s", message);
}

static void Q3P_PRT_Warn (q3p_prt_parser_t *parser, int level, const char *fmt, ...)
{
	char message[1024];
	va_list argptr;

	if (!parser)
		return;
	parser->warnings++;
	if (parser->debug_level < level)
		return;

	va_start (argptr, fmt);
	q_vsnprintf (message, sizeof (message), fmt, argptr);
	va_end (argptr);

	Con_Warning ("Q3P PRT WARNING: %s:%d:%d: %s (token='%s')\n",
		parser->source_name, parser->token_line, parser->token_col, message, parser->token[0] ? parser->token : "<eof>");
}

static void Q3P_PRT_ComputeLineCol (const char *base, const char *point, int *line, int *col, const char **line_start)
{
	const char *scan;
	int out_line = 1;
	int out_col = 1;
	const char *out_line_start = base;

	for (scan = base; scan && scan < point && *scan; ++scan)
	{
		if (*scan == '\n')
		{
			out_line++;
			out_col = 1;
			out_line_start = scan + 1;
		}
		else
		{
			out_col++;
		}
	}

	if (line)
		*line = out_line;
	if (col)
		*col = out_col;
	if (line_start)
		*line_start = out_line_start;
}

static qboolean Q3P_PRT_NextToken (q3p_prt_parser_t *parser)
{
	const char *token_start;

	if (!parser)
		return false;

	if (!(parser->cursor = COM_Parse (parser->cursor)))
	{
		parser->token[0] = '\0';
		parser->token_start = parser->cursor;
		Q3P_PRT_ComputeLineCol (parser->text_start, parser->cursor, &parser->token_line, &parser->token_col, NULL);
		return false;
	}

	q_strlcpy (parser->token, com_token, sizeof (parser->token));
	if (!parser->token[0])
		return false;

	token_start = parser->cursor - strlen (parser->token);
	if (token_start < parser->text_start)
		token_start = parser->text_start;
	parser->token_start = token_start;
	Q3P_PRT_ComputeLineCol (parser->text_start, parser->token_start, &parser->token_line, &parser->token_col, NULL);

	Q3P_PRT_Log (parser, 3, "Q3P PRT TRACE: %s:%d:%d token='%s'\n", parser->source_name, parser->token_line, parser->token_col, parser->token);

	return true;
}

static void Q3P_PRT_PrintErrorContext (q3p_prt_parser_t *parser)
{
	const char *line_start = NULL;
	const char *line_end;
	char snippet[256];
	int line_len;
	int col_in_snippet;
	int i;

	if (!parser || parser->debug_level < 1)
		return;

	Q3P_PRT_ComputeLineCol (parser->text_start, parser->token_start ? parser->token_start : parser->cursor,
		NULL, NULL, &line_start);
	if (!line_start)
		line_start = parser->text_start;

	for (line_end = line_start; *line_end && *line_end != '\n' && *line_end != '\r'; ++line_end)
		;
	line_len = (int)(line_end - line_start);
	if (line_len > 200)
		line_len = 200;
	memcpy (snippet, line_start, line_len);
	snippet[line_len] = '\0';

	Con_Printf ("PRT CONTEXT: %s\n", snippet);

	col_in_snippet = q_max (1, parser->token_col);
	if (col_in_snippet > line_len + 1)
		col_in_snippet = line_len + 1;
	for (i = 1; i < col_in_snippet; ++i)
		snippet[i - 1] = ' ';
	snippet[col_in_snippet - 1] = '^';
	snippet[col_in_snippet] = '\0';
	Con_Printf ("PRT CONTEXT: %s\n", snippet);
}

static qboolean Q3P_PRT_Error (q3p_prt_parser_t *parser, const char *fmt, ...)
{
	char message[1024];
	va_list argptr;

	if (!parser)
		return false;

	parser->errors++;
	parser->had_error = true;

	va_start (argptr, fmt);
	q_vsnprintf (message, sizeof (message), fmt, argptr);
	va_end (argptr);

	if (parser->debug_level >= 1)
	{
		Con_Warning ("PRT ERROR: %s:%d:%d: %s (token='%s')\n",
			parser->source_name, parser->token_line, parser->token_col, message,
			parser->token[0] ? parser->token : "<eof>");
		Q3P_PRT_PrintErrorContext (parser);
	}

	return false;
}

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

enum
{
	Q3P_KEY_MATERIAL,
	Q3P_KEY_COUNT,
	Q3P_KEY_LIFETIME_MIN,
	Q3P_KEY_LIFETIME_RAND,
	Q3P_KEY_SIZE,
	Q3P_KEY_SIZE_RAND,
	Q3P_KEY_SIZE_RAMP,
	Q3P_KEY_ALPHA,
	Q3P_KEY_ALPHA_RAMP,
	Q3P_KEY_COLOR,
	Q3P_KEY_GRAVITY,
	Q3P_KEY_DRAG,
	Q3P_KEY_RESTITUTION,
	Q3P_KEY_MIN_BOUNCE_SPEED,
	Q3P_KEY_ORG_JITTER,
	Q3P_KEY_VEL_JITTER,
	Q3P_KEY_VEL_SCALE,
	Q3P_KEY_COLLIDE_WORLD,
	Q3P_KEY_TOTAL
};

static qboolean Q3P_PRT_MarkKeySeen (q3p_prt_parser_t *parser, unsigned int *seen_keys, int key_bit, const char *effect_name, const char *key_name)
{
	unsigned int mask = 1u << key_bit;

	if (*seen_keys & mask)
		Q3P_PRT_Warn (parser, 2, "duplicate key '%s' in effect '%s'; last value wins", key_name, effect_name);
	*seen_keys |= mask;
	return true;
}

static qboolean Q3P_ParseEffectDefText (q3p_prt_parser_t *parser, const char *source_name, char *text)
{
	parser->source_name = source_name;
	parser->text_start = text;
	parser->cursor = text;
	parser->token_start = text;
	parser->token[0] = '\0';

	while (Q3P_PRT_NextToken (parser))
	{
		q3p_named_effectdef_t *entry;
		q3p_effectdef_t def;
		char effect_name[64];
		unsigned int seen_keys = 0;
		qboolean closed = false;

		q_strlcpy (effect_name, parser->token, sizeof (effect_name));
		entry = Q3P_FindOrAllocEffectDef (effect_name);
		if (!entry)
			return Q3P_PRT_Error (parser, "effect definition table full; cannot add '%s'", effect_name);

		if (entry->def.loaded)
			Q3P_PRT_Warn (parser, 1, "duplicate definition for '%s'; newer definition overrides older one", effect_name);

		def = entry->def;

		if (!Q3P_PRT_NextToken (parser))
			return Q3P_PRT_Error (parser, "expected '{' after effect '%s'", effect_name);
		if (strcmp (parser->token, "{"))
			return Q3P_PRT_Error (parser, "expected '{' after effect '%s'", effect_name);

		while (Q3P_PRT_NextToken (parser))
		{
			if (!strcmp (parser->token, "}"))
			{
				closed = true;
				break;
			}

			if (!q_strcasecmp (parser->token, "material"))
			{
				Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_MATERIAL, effect_name, "material");
				if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'material' in effect '%s'", effect_name);
				q_strlcpy (def.material, parser->token, sizeof (def.material));
			}
			else if (!q_strcasecmp (parser->token, "count"))
			{
				int raw;
				Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_COUNT, effect_name, "count");
				if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'count' in effect '%s'", effect_name);
				raw = Q_atoi (parser->token);
				def.count = q_max (1, raw);
				if (def.count != raw)
					Q3P_PRT_Warn (parser, 2, "clamped count from %d to %d in effect '%s'", raw, def.count, effect_name);
			}
			else if (!q_strcasecmp (parser->token, "lifetime_min")) { Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_LIFETIME_MIN, effect_name, "lifetime_min"); if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'lifetime_min' in effect '%s'", effect_name); def.lifetime_min = Q_atof (parser->token); }
			else if (!q_strcasecmp (parser->token, "lifetime_rand")) { Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_LIFETIME_RAND, effect_name, "lifetime_rand"); if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'lifetime_rand' in effect '%s'", effect_name); def.lifetime_rand = Q_atof (parser->token); }
			else if (!q_strcasecmp (parser->token, "size")) { Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_SIZE, effect_name, "size"); if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'size' in effect '%s'", effect_name); def.size = Q_atof (parser->token); }
			else if (!q_strcasecmp (parser->token, "size_rand")) { Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_SIZE_RAND, effect_name, "size_rand"); if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'size_rand' in effect '%s'", effect_name); def.size_rand = Q_atof (parser->token); }
			else if (!q_strcasecmp (parser->token, "size_ramp")) { Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_SIZE_RAMP, effect_name, "size_ramp"); if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'size_ramp' in effect '%s'", effect_name); def.size_ramp = Q_atof (parser->token); }
			else if (!q_strcasecmp (parser->token, "alpha"))
			{
				float raw;
				Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_ALPHA, effect_name, "alpha");
				if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'alpha' in effect '%s'", effect_name);
				raw = Q_atof (parser->token);
				def.alpha = CLAMP (0.f, raw, 1.f);
				if (def.alpha != raw)
					Q3P_PRT_Warn (parser, 2, "clamped alpha from %.3f to %.3f in effect '%s'", raw, def.alpha, effect_name);
			}
			else if (!q_strcasecmp (parser->token, "alpha_ramp")) { Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_ALPHA_RAMP, effect_name, "alpha_ramp"); if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'alpha_ramp' in effect '%s'", effect_name); def.alpha_ramp = Q_atof (parser->token); }
			else if (!q_strcasecmp (parser->token, "color")) { Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_COLOR, effect_name, "color"); if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'color' in effect '%s'", effect_name); def.color = Q_atoi (parser->token); }
			else if (!q_strcasecmp (parser->token, "gravity")) { Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_GRAVITY, effect_name, "gravity"); if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'gravity' in effect '%s'", effect_name); def.gravity = Q_atof (parser->token); }
			else if (!q_strcasecmp (parser->token, "drag")) { Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_DRAG, effect_name, "drag"); if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'drag' in effect '%s'", effect_name); def.drag = Q_atof (parser->token); }
			else if (!q_strcasecmp (parser->token, "restitution")) { Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_RESTITUTION, effect_name, "restitution"); if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'restitution' in effect '%s'", effect_name); def.restitution = Q_atof (parser->token); }
			else if (!q_strcasecmp (parser->token, "min_bounce_speed")) { Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_MIN_BOUNCE_SPEED, effect_name, "min_bounce_speed"); if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'min_bounce_speed' in effect '%s'", effect_name); def.min_bounce_speed = Q_atof (parser->token); }
			else if (!q_strcasecmp (parser->token, "org_jitter")) { Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_ORG_JITTER, effect_name, "org_jitter"); if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'org_jitter' in effect '%s'", effect_name); def.org_jitter = Q_atof (parser->token); }
			else if (!q_strcasecmp (parser->token, "vel_jitter")) { Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_VEL_JITTER, effect_name, "vel_jitter"); if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'vel_jitter' in effect '%s'", effect_name); def.vel_jitter = Q_atof (parser->token); }
			else if (!q_strcasecmp (parser->token, "vel_scale")) { Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_VEL_SCALE, effect_name, "vel_scale"); if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'vel_scale' in effect '%s'", effect_name); def.vel_scale = Q_atof (parser->token); }
			else if (!q_strcasecmp (parser->token, "collide_world")) { Q3P_PRT_MarkKeySeen (parser, &seen_keys, Q3P_KEY_COLLIDE_WORLD, effect_name, "collide_world"); if (!Q3P_PRT_NextToken (parser)) return Q3P_PRT_Error (parser, "missing value for key 'collide_world' in effect '%s'", effect_name); def.collide_world = Q_atoi (parser->token) != 0; }
			else
			{
				Q3P_PRT_Warn (parser, 1, "unknown key '%s' in effect '%s'", parser->token, effect_name);
				if (!Q3P_PRT_NextToken (parser))
					return Q3P_PRT_Error (parser, "missing value after unknown key in effect '%s'", effect_name);
			}
		}

		if (!closed)
			return Q3P_PRT_Error (parser, "unexpected end of file while parsing effect '%s'", effect_name);

		def.loaded = true;
		entry->def = def;
		parser->defs_parsed++;
		Q3P_PRT_Log (parser, 2, "Q3P: loaded effect '%s' from %s\n", effect_name, source_name);
	}

	return !parser->had_error;
}

static qboolean Q3P_LoadAndParseEffectDefFile (q3p_prt_parser_t *parser, const char *path)
{
	byte *buffer;
	qboolean ok = true;

	buffer = COM_LoadMallocFile (path, NULL);
	if (!buffer)
		return false;

	if (buffer[0] == '\0')
	{
		Q3P_PRT_Warn (parser, 1, "Q3P: skipping empty .prt file '%s'\n", path);
		ok = false;
	}
	else if (!Q3P_ParseEffectDefText (parser, path, (char *)buffer))
	{
		parser->had_error = true;
		ok = false;
	}

	q_free(buffer);
	return ok;
}

static void Q3P_LoadEffectDefs (void)
{
	searchpath_t *search;
	int loaded = 0;
	q3p_prt_parser_t parser;
	double start_time;
	double elapsed;
	int active_defs = 0;
	int i;

	memset (&parser, 0, sizeof (parser));
	parser.debug_level = q_max (0, (int)r_particles_prt_debug.value);
	start_time = Sys_DoubleTime ();

	memset (q3p_effect_defs, 0, sizeof (q3p_effect_defs));

	for (search = com_searchpaths; search; search = search->next)
	{
		if (search->pack)
		{
			for (i = 0; i < search->pack->numfiles; ++i)
			{
				const char *path = search->pack->files[i].name;

				if (q_strncasecmp (path, "particles/", 10) || q_strcasecmp (COM_FileGetExtension (path), "prt"))
					continue;
				if (Q3P_LoadAndParseEffectDefFile (&parser, path))
				{
					++loaded;
					parser.files_loaded++;
				}
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

				if (find->attribs & FA_DIRECTORY)
					continue;
				q_snprintf (rel, sizeof (rel), "particles/%s", find->name);
				if (Q3P_LoadAndParseEffectDefFile (&parser, rel))
				{
					++loaded;
					parser.files_loaded++;
				}
			}
		}
	}

	for (i = 0; i < Q3P_MAX_EFFECT_DEFS; ++i)
	{
		if (q3p_effect_defs[i].used && q3p_effect_defs[i].def.loaded)
			active_defs++;
	}

	elapsed = Sys_DoubleTime () - start_time;

	Q3P_PRT_Log (&parser, 1, "Q3P: loaded %d .prt particle definition files\n", loaded);
	Q3P_PRT_Log (&parser, 1, "Q3P PRT SUMMARY: files=%d defs=%d active_defs=%d warnings=%d errors=%d parse_ms=%.2f\n",
		parser.files_loaded, parser.defs_parsed, active_defs, parser.warnings, parser.errors, elapsed * 1000.0);
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

	GL_Uniform3fvFunc (1, 3, m);
}

static void Q3P_EvalStageColorMul (const material_stage_t *stage, float particle_alpha, vec4_t out)
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
		out[0] = out[1] = out[2] = Q3P_Clamp01 (Material_EvalWaveValue (&stage->rgb_wave, cl.time));
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
		out[3] *= Q3P_Clamp01 (Material_EvalWaveValue (&stage->alpha_wave, cl.time));
		break;
	case MAT_ALPHAGEN_VERTEX:
	case MAT_ALPHAGEN_IDENTITY:
	default:
		break;
	}
}

static gltexture_t *Q3P_ResolveStageTexture (const material_stage_t *stage)
{
	const char *path = NULL;

	if (!stage)
		return notexture;

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

	path = MaterialStage_GetAnimMapPath ((material_stage_t *)stage, cl.time);
	if (!path || !path[0])
		path = stage->map_path;
	if (!path || !path[0])
		return notexture;

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

	Material_Canonicalize (name, canonical, sizeof (canonical));
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
	const material_t *material;
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

		material = Material_Find (entry->key);
		if (!material)
		{
			const char *texname = entry->key;
			if (!q_strncasecmp (texname, Q3P_PARTICLE_SHADER_PREFIX, strlen (Q3P_PARTICLE_SHADER_PREFIX)))
				texname += strlen (Q3P_PARTICLE_SHADER_PREFIX);
			material = Material_FindForTextureName (texname, NULL);
		}

		stage0_ok = false;
		entry->supported_stage_count = 0;
		if (material)
		{
			int stage_count = (int)VEC_SIZE (material->stages);
			int si;
			mat_particle_policy_t policy = r_particles_material_strict.value > 0.f
				? MAT_PARTICLE_POLICY_STRICT
				: MAT_PARTICLE_POLICY_TOLERANT;

			entry->flags |= Q3P_MATFLAG_RESOLVED;
			stage0_ok = Material_StageSupportsParticleMVP (&material->stage0, fallback_reason, sizeof (fallback_reason));
			if (stage0_ok)
				entry->flags |= Q3P_MATFLAG_STAGE0_VALID;

			for (si = 0; si < stage_count; ++si)
			{
				const material_stage_t *stage = &material->stages[si];
				if (Material_ClassifyParticleStage (stage, policy, fallback_reason, sizeof (fallback_reason)) == MAT_PARTICLE_STAGE_SUPPORTED)
					entry->supported_stage_count = si + 1;
				else if (r_particles_material_strict.value > 0.f)
					break;
			}

			if (entry->supported_stage_count <= 0 && stage0_ok)
				entry->supported_stage_count = 1;
		}

		if (!material || (!stage0_ok && r_particles_material_strict.value > 0.f))
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

static int Q3P_NextPow2 (int value)
{
	int pow2 = 1;

	while (pow2 < value && pow2 < (1 << 30))
		pow2 <<= 1;
	return q_max (1, pow2);
}

static qboolean Q3P_GPU_ComputeAvailable (void)
{
	const RenderBackendCaps *caps = R_Backend_GetCaps ();
	return caps
		&& caps->supports_compute
		&& caps->supports_memory_barrier
		&& GL_MapBufferRangeFunc
		&& GL_UnmapBufferFunc
		&& glprogs.q3p_sim
		&& glprogs.q3p_cull_key
		&& glprogs.gpu_bitonic_pairs;
}

static void Q3P_GPU_ResetResources (void)
{
	if (GL_DeleteBuffersFunc)
	{
		if (q3p_gpu.sim_buffer[0] || q3p_gpu.sim_buffer[1])
			GL_DeleteBuffersFunc (2, q3p_gpu.sim_buffer);
		if (q3p_gpu.sort_buffer)
			GL_DeleteBuffersFunc (1, &q3p_gpu.sort_buffer);
		if (q3p_gpu.visible_count_buffer)
			GL_DeleteBuffersFunc (1, &q3p_gpu.visible_count_buffer);
	}

	q3p_gpu.sim_buffer[0] = 0;
	q3p_gpu.sim_buffer[1] = 0;
	q3p_gpu.sort_buffer = 0;
	q3p_gpu.visible_count_buffer = 0;
	q3p_gpu.sim_src_index = 0;
}

static qboolean Q3P_GPU_EnsureBuffers (void)
{
	const GLsizeiptr particle_bytes = (GLsizeiptr)(sizeof (q3p_particle_gpu_t) * q3p_maxparticles);
	const GLsizeiptr sort_bytes = (GLsizeiptr)(sizeof (q3p_sort_entry_gpu_t) * q3p_gpu.sort_capacity);

	if (!Q3P_GPU_ComputeAvailable ())
		return false;
	if (!q3p_gpu.sort_capacity)
		q3p_gpu.sort_capacity = Q3P_NextPow2 (q3p_maxparticles);

	if (!q3p_gpu.sim_buffer[0] || !q3p_gpu.sim_buffer[1])
	{
		GL_GenBuffersFunc (2, q3p_gpu.sim_buffer);
		if (!q3p_gpu.sim_buffer[0] || !q3p_gpu.sim_buffer[1])
			return false;
		GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, q3p_gpu.sim_buffer[0]);
		GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, particle_bytes, NULL, GL_DYNAMIC_DRAW);
		GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, q3p_gpu.sim_buffer[1]);
		GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, particle_bytes, NULL, GL_DYNAMIC_DRAW);
	}

	if (!q3p_gpu.sort_buffer)
	{
		GL_GenBuffersFunc (1, &q3p_gpu.sort_buffer);
		if (!q3p_gpu.sort_buffer)
			return false;
		GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, q3p_gpu.sort_buffer);
		GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, sort_bytes, NULL, GL_DYNAMIC_DRAW);
	}

	if (!q3p_gpu.visible_count_buffer)
	{
		GL_GenBuffersFunc (1, &q3p_gpu.visible_count_buffer);
		if (!q3p_gpu.visible_count_buffer)
			return false;
		GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, q3p_gpu.visible_count_buffer);
		GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, sizeof (unsigned int), NULL, GL_DYNAMIC_DRAW);
	}

	return true;
}

static void Q3P_GPU_PackParticle (const q3p_particle_t *src, q3p_particle_gpu_t *dst)
{
	dst->spawn_life_size_sizeramp[0] = src->spawn_time;
	dst->spawn_life_size_sizeramp[1] = src->lifetime;
	dst->spawn_life_size_sizeramp[2] = src->size;
	dst->spawn_life_size_sizeramp[3] = src->size_ramp;

	dst->alpha_alpharamp_gravity_drag[0] = src->alpha;
	dst->alpha_alpharamp_gravity_drag[1] = src->alpha_ramp;
	dst->alpha_alpharamp_gravity_drag[2] = src->gravity;
	dst->alpha_alpharamp_gravity_drag[3] = src->drag;

	dst->org[0] = src->org[0];
	dst->org[1] = src->org[1];
	dst->org[2] = src->org[2];
	dst->org[3] = 0.f;

	dst->vel[0] = src->vel[0];
	dst->vel[1] = src->vel[1];
	dst->vel[2] = src->vel[2];
	dst->vel[3] = 0.f;

	dst->color_flags_material_alive[0] = (unsigned int)src->color;
	dst->color_flags_material_alive[1] = (unsigned int)src->flags;
	dst->color_flags_material_alive[2] = (unsigned int)src->material_id;
	dst->color_flags_material_alive[3] = 1u;

	dst->bounce[0] = src->restitution;
	dst->bounce[1] = src->min_bounce_speed;
	dst->bounce[2] = 0.f;
	dst->bounce[3] = 0.f;
}

static void Q3P_GPU_UnpackParticle (const q3p_particle_gpu_t *src, q3p_particle_t *dst)
{
	dst->size = src->spawn_life_size_sizeramp[2];
	dst->alpha = src->alpha_alpharamp_gravity_drag[0];
	dst->org[0] = src->org[0];
	dst->org[1] = src->org[1];
	dst->org[2] = src->org[2];
	dst->vel[0] = src->vel[0];
	dst->vel[1] = src->vel[1];
	dst->vel[2] = src->vel[2];
	dst->material_id = src->color_flags_material_alive[2];
}

static qboolean Q3P_GPU_ReadBuffer (GLuint buffer, size_t size, void *dst)
{
	void *mapped;

	if (!buffer || !dst || !size)
		return false;

	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, buffer);
	mapped = GL_MapBufferRangeFunc (GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)size, GL_MAP_READ_BIT);
	if (!mapped)
		return false;

	memcpy (dst, mapped, size);
	if (!GL_UnmapBufferFunc (GL_SHADER_STORAGE_BUFFER))
		return false;
	return true;
}

static void Q3P_GPU_BitonicSortEntries (int count)
{
	int groups;
	int k;

	if (count <= 1)
		return;

	groups = (count + (Q3P_GPU_LOCAL_SIZE - 1)) / Q3P_GPU_LOCAL_SIZE;
	GL_UseProgram (glprogs.gpu_bitonic_pairs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, q3p_gpu.sort_buffer, 0, sizeof (q3p_sort_entry_gpu_t) * count);

	for (k = 2; k <= count; k <<= 1)
	{
		int j;
		for (j = k >> 1; j > 0; j >>= 1)
		{
			GL_Uniform4fFunc (0, (float)count, (float)j, (float)k, 0.f);
			R_Backend_Dispatch ((unsigned)groups, 1u, 1u);
			R_Backend_MemoryBarrier (R_BACKEND_BARRIER_SHADER_STORAGE);
		}
	}
}

static qboolean Q3P_GPU_RunSim (float frametime, qboolean collision_enabled)
{
	const int src_index = q3p_gpu.sim_src_index & 1;
	const int dst_index = (src_index + 1) & 1;
	const GLuint src_buffer = q3p_gpu.sim_buffer[src_index];
	const GLuint dst_buffer = q3p_gpu.sim_buffer[dst_index];
	const int groups = (q3p_numactive + (Q3P_GPU_LOCAL_SIZE - 1)) / Q3P_GPU_LOCAL_SIZE;
	int i;
	int active = 0;
	qboolean allow_gpu_collision = false;

	if (q3p_numactive <= 0)
		return true;
	if (!Q3P_GPU_EnsureBuffers () || !src_buffer || !dst_buffer || !q3p_gpu.particle_staging)
		return false;

	/* Keep CPU collision as authoritative world behavior until a BSP-aware GPU trace path exists. */
	if (collision_enabled)
		return false;

	for (i = 0; i < q3p_numactive; ++i)
		Q3P_GPU_PackParticle (&q3p_particles[i], &q3p_gpu.particle_staging[i]);

	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, src_buffer);
	GL_BufferSubDataFunc (GL_SHADER_STORAGE_BUFFER, 0, sizeof (q3p_particle_gpu_t) * q3p_numactive, q3p_gpu.particle_staging);

	GL_UseProgram (glprogs.q3p_sim);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, src_buffer, 0, sizeof (q3p_particle_gpu_t) * q3p_numactive);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, dst_buffer, 0, sizeof (q3p_particle_gpu_t) * q3p_numactive);
	GL_Uniform4fFunc (0, cl.time, frametime, allow_gpu_collision ? 1.f : 0.f, (float)q3p_numactive);
	GL_Uniform4fFunc (1, 0.f, 0.f, 0.f, 0.f);
	R_Backend_Dispatch ((unsigned)groups, 1u, 1u);
	R_Backend_MemoryBarrier (R_BACKEND_BARRIER_SHADER_STORAGE);

	if (!Q3P_GPU_ReadBuffer (dst_buffer, sizeof (q3p_particle_gpu_t) * q3p_numactive, q3p_gpu.particle_staging))
		return false;

	for (i = 0; i < q3p_numactive; ++i)
	{
		const q3p_particle_gpu_t *gpu = &q3p_gpu.particle_staging[i];
		q3p_particle_t *dst;

		if (!gpu->color_flags_material_alive[3])
			continue;

		if (i != active)
			q3p_particles[active] = q3p_particles[i];
		dst = &q3p_particles[active];
		Q3P_GPU_UnpackParticle (gpu, dst);
		++active;
	}

	q3p_numactive = active;
	q3p_gpu.sim_src_index = dst_index;
	q3p_gpu.sim_frames++;
	return true;
}

static qboolean Q3P_GPU_BuildDrawList (qboolean alpha, qboolean showtris)
{
	const int active_count = q3p_numactive;
	const int sort_count = q3p_gpu.sort_capacity;
	const GLuint particle_buffer = q3p_gpu.sim_buffer[q3p_gpu.sim_src_index & 1];
	const int groups = (sort_count + (Q3P_GPU_LOCAL_SIZE - 1)) / Q3P_GPU_LOCAL_SIZE;
	const float cull_dist = q_max (0.f, r_particles_cull_dist.value);
	unsigned int visible_count = 0u;
	int i;

	if (active_count <= 0)
	{
		q3p_numdrawitems = 0;
		return true;
	}
	if (!Q3P_GPU_EnsureBuffers () || !particle_buffer || !q3p_gpu.sort_staging || sort_count < active_count)
		return false;

	for (i = 0; i < active_count; ++i)
		Q3P_GPU_PackParticle (&q3p_particles[i], &q3p_gpu.particle_staging[i]);

	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, particle_buffer);
	GL_BufferSubDataFunc (GL_SHADER_STORAGE_BUFFER, 0, sizeof (q3p_particle_gpu_t) * active_count, q3p_gpu.particle_staging);

	visible_count = 0u;
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, q3p_gpu.visible_count_buffer);
	GL_BufferSubDataFunc (GL_SHADER_STORAGE_BUFFER, 0, sizeof (visible_count), &visible_count);

	GL_UseProgram (glprogs.q3p_cull_key);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, particle_buffer, 0, sizeof (q3p_particle_gpu_t) * q_max (1, active_count));
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, q3p_gpu.sort_buffer, 0, sizeof (q3p_sort_entry_gpu_t) * q_max (1, sort_count));
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, q3p_gpu.visible_count_buffer, 0, sizeof (visible_count));
	GL_Uniform4fFunc (0, (float)active_count, cull_dist * cull_dist, (r_particles_sort.value > 0.f) ? 1.f : 0.f, alpha ? 1.f : 0.f);
	GL_Uniform4fFunc (1, showtris ? 1.f : 0.f, DotProduct (r_origin, vpn), 0.f, 0.f);
	GL_Uniform4fFunc (2, frustum[0].normal[0], frustum[0].normal[1], frustum[0].normal[2], frustum[0].dist);
	GL_Uniform4fFunc (3, frustum[1].normal[0], frustum[1].normal[1], frustum[1].normal[2], frustum[1].dist);
	GL_Uniform4fFunc (4, frustum[2].normal[0], frustum[2].normal[1], frustum[2].normal[2], frustum[2].dist);
	GL_Uniform4fFunc (5, frustum[3].normal[0], frustum[3].normal[1], frustum[3].normal[2], frustum[3].dist);
	GL_Uniform4fFunc (6, r_origin[0], r_origin[1], r_origin[2], 0.f);
	GL_Uniform4fFunc (7, vpn[0], vpn[1], vpn[2], 0.f);
	R_Backend_Dispatch ((unsigned)groups, 1u, 1u);
	R_Backend_MemoryBarrier (R_BACKEND_BARRIER_SHADER_STORAGE);

	Q3P_GPU_BitonicSortEntries (sort_count);

	if (!Q3P_GPU_ReadBuffer (q3p_gpu.visible_count_buffer, sizeof (visible_count), &visible_count))
		return false;
	if ((int)visible_count > active_count)
		visible_count = (unsigned int)active_count;
	if ((int)visible_count > q3p_maxparticles)
		visible_count = (unsigned int)q3p_maxparticles;

	if (visible_count > 0
		&& !Q3P_GPU_ReadBuffer (q3p_gpu.sort_buffer, sizeof (q3p_sort_entry_gpu_t) * visible_count, q3p_gpu.sort_staging))
		return false;

	q3p_numdrawitems = 0;
	for (i = 0; i < (int)visible_count && q3p_numdrawitems < q3p_maxparticles; ++i)
	{
		const q3p_sort_entry_gpu_t *entry_gpu = &q3p_gpu.sort_staging[i];
		int particle_index = (int)entry_gpu->index;
		q3p_particle_t *p;
		const q3p_material_cache_entry_t *entry;
		unsigned material_id;
		int blend_group;

		if (!entry_gpu->visible || particle_index < 0 || particle_index >= active_count)
			continue;

		p = &q3p_particles[particle_index];
		blend_group = p->alpha < 1.f;
		if (!showtris && alpha != blend_group)
			continue;

		entry = Q3P_ResolveMaterialCached (p->material);
		material_id = entry ? entry->resolved_material_id : p->material_id;
		q3p_drawitems[q3p_numdrawitems].particle_index = particle_index;
		q3p_drawitems[q3p_numdrawitems].blend_group = blend_group;
		q3p_drawitems[q3p_numdrawitems].material_id = material_id;
		q3p_drawitems[q3p_numdrawitems].material_entry = entry;
		q3p_drawitems[q3p_numdrawitems].depth = DotProduct (p->org, vpn) - DotProduct (r_origin, vpn);
		++q3p_numdrawitems;
	}

	q3p_gpu.cullsort_frames++;
	q3p_gpu.last_visible = q3p_numdrawitems;
	q3p_gpu.last_culled = 0;
	if (!showtris && !alpha)
	{
		int opaque_total = 0;
		for (i = 0; i < active_count; ++i)
		{
			if (q3p_particles[i].alpha >= 1.f)
				++opaque_total;
		}
		q3p_gpu.last_culled = q_max (0, opaque_total - q3p_numdrawitems);
		q3p_culled_counter += q3p_gpu.last_culled;
	}

	return true;
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
	qboolean alpha_group;

	if (da->blend_group != db->blend_group)
		return da->blend_group - db->blend_group;

	alpha_group = da->blend_group != 0;
	if (alpha_group && r_particles_sort.value > 0.f)
	{
		if (da->depth < db->depth)
			return 1;
		if (da->depth > db->depth)
			return -1;
	}

	if (da->material_id < db->material_id)
		return -1;
	if (da->material_id > db->material_id)
		return 1;
	if (!alpha_group && r_particles_sort.value > 0.f)
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
	memset (&q3p_gpu, 0, sizeof (q3p_gpu));
	q3p_gpu.sort_capacity = Q3P_NextPow2 (q3p_maxparticles);
	q3p_gpu.particle_staging = (q3p_particle_gpu_t *)Hunk_AllocName (q3p_maxparticles * sizeof (*q3p_gpu.particle_staging), "q3pgpupart");
	q3p_gpu.sort_staging = (q3p_sort_entry_gpu_t *)Hunk_AllocName (q3p_gpu.sort_capacity * sizeof (*q3p_gpu.sort_staging), "q3pgpusort");
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
	Q3P_GPU_ResetResources ();

	q3p_particles = NULL;
	q3p_drawitems = NULL;
	q3p_partverts = NULL;
	q3p_gpu.particle_staging = NULL;
	q3p_gpu.sort_staging = NULL;
	q3p_gpu.sort_capacity = 0;
	q3p_gpu.sim_frames = 0;
	q3p_gpu.sim_fallbacks = 0;
	q3p_gpu.cullsort_frames = 0;
	q3p_gpu.last_visible = 0;
	q3p_gpu.last_culled = 0;
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
	q3p_gpu.sim_src_index = 0;
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

	if (r_particles_gpu_sim.value > 0.f)
	{
		if (!collision_enabled && Q3P_GPU_RunSim (frametime, false))
			return;
		q3p_gpu.sim_fallbacks++;
	}

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
	R_Backend_DrawInstanced (R_BACKEND_PRIMITIVE_TRIANGLE_STRIP, 0, 4, q3p_numpartverts);

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
	if (r_particles_gpu_cullsort.value > 0.f && Q3P_GPU_BuildDrawList (alpha, showtris))
	{
		/* draw items are already culled and sorted on GPU */
	}
	else
	{
		for (i = 0; i < q3p_numactive; ++i)
		{
			q3p_particle_t *p = &q3p_particles[i];
			const q3p_material_cache_entry_t *entry = Q3P_ResolveMaterialCached (p->material);
			unsigned material_id = p->material_id;
			int blend_group = p->alpha < 1.f;

			if (!showtris && alpha != blend_group)
				continue;
			if (Q3P_ShouldCullParticle (p))
			{
				if (!showtris && !alpha)
					++q3p_culled_counter;
				continue;
			}

			if (entry)
				material_id = entry->resolved_material_id;

			q3p_drawitems[q3p_numdrawitems].particle_index = i;
			q3p_drawitems[q3p_numdrawitems].blend_group = blend_group;
			q3p_drawitems[q3p_numdrawitems].material_id = material_id;
			q3p_drawitems[q3p_numdrawitems].material_entry = entry;
			q3p_drawitems[q3p_numdrawitems].depth = DotProduct (p->org, vpn) - DotProduct (r_origin, vpn);
			++q3p_numdrawitems;
		}

		if (q3p_numdrawitems)
			qsort (q3p_drawitems, q3p_numdrawitems, sizeof(q3p_drawitems[0]), Q3P_DrawSortCmp);
	}

	if (!q3p_numdrawitems)
		return;

	for (i = 0; i < q3p_numdrawitems; ++i)
	{
		const q3p_material_cache_entry_t *entry = q3p_drawitems[i].material_entry;
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
		R_Backend_SetPipelineState (GLS_BLEND_ALPHA_OIT | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (2) | GLS_INSTANCED_ATTRIBS (2));
	else
		R_Backend_SetPipelineState (GLS_BLEND_OPAQUE | GLS_CULL_NONE | GLS_ATTRIBS (2) | GLS_INSTANCED_ATTRIBS (2));

	for (stage_index = 0; stage_index < max_stage_count; ++stage_index)
	{
		unsigned current_material = 0;
		const q3p_material_cache_entry_t *current_entry = NULL;
		const material_stage_t *current_stage = NULL;
		vec4_t stage_mul;

		q3p_numpartverts = 0;
		for (i = 0; i < q3p_numdrawitems; ++i)
		{
			q3p_drawitem_t *drawitem = &q3p_drawitems[i];
			q3p_particle_t *p = &q3p_particles[q3p_drawitems[i].particle_index];
			q3p_particlevert_t *v;
			vec3_t particle_light;
			const GLubyte *c = showtris ? white : (const GLubyte *)&d_8to24table[p->color & 0xff];
			const q3p_material_cache_entry_t *entry = drawitem->material_entry;

			if (!entry || stage_index >= entry->supported_stage_count)
				continue;

			if (!current_entry || drawitem->material_id != current_material)
			{
				current_material = drawitem->material_id;
				current_entry = entry;
				current_stage = (entry->material && stage_index < (int)VEC_SIZE (entry->material->stages))
					? &entry->material->stages[stage_index]
					: NULL;

				if (q3p_numpartverts)
					Q3P_FlushParticleBatch ();

				GL_Bind (GL_TEXTURE0, current_stage ? Q3P_ResolveStageTexture (current_stage) : notexture);
				Q3P_SetStageTexMatrixUniform (current_stage ? MaterialStage_EvalTexMatrix ((material_stage_t *)current_stage, cl.time) : NULL);
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
			if (!showtris)
			{
				R_SampleReceiverLighting (p->org, particle_light);
				v->color[0] = Q3P_ParticleLitByte (v->color[0], particle_light[0]);
				v->color[1] = Q3P_ParticleLitByte (v->color[1], particle_light[1]);
				v->color[2] = Q3P_ParticleLitByte (v->color[2], particle_light[2]);
			}
			v->color[3] = (GLubyte)(CLAMP (0.f, p->alpha, 1.f) * 255.f);
		}

		Q3P_FlushParticleBatch ();
	}

	R_Backend_SetPipelineState (saved_state);
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
	stats->gpu_sim_frames = q3p_gpu.sim_frames;
	stats->gpu_sim_fallbacks = q3p_gpu.sim_fallbacks;
	stats->gpu_cullsort_frames = q3p_gpu.cullsort_frames;
	stats->gpu_visible = q3p_gpu.last_visible;
	stats->gpu_culled = q3p_gpu.last_culled;
}

void Q3P_ResetDebugStats (void)
{
	q3p_spawned_counter = 0;
	q3p_dropped_counter = 0;
	q3p_culled_counter = 0;
	q3p_culled_counter_frame = -1;
	q3p_gpu.sim_frames = 0;
	q3p_gpu.sim_fallbacks = 0;
	q3p_gpu.cullsort_frames = 0;
	q3p_gpu.last_visible = 0;
	q3p_gpu.last_culled = 0;
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

