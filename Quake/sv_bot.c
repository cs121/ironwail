#include "quakedef.h"
#include "q_ctype.h"

#include <float.h>
#include <limits.h>
#include <stdint.h>

extern edict_t *sv_player;

static cvar_t sv_bot_spawn = {"sv_bot_spawn", "0", CVAR_NONE};
static cvar_t sv_bot_remove = {"sv_bot_remove", "0", CVAR_NONE};
static cvar_t sv_bot_difficulty = {"sv_bot_difficulty", "medium", CVAR_NONE};

#define MAX_BOT_CHAT_LINES			8
#define MAX_BOT_CHAT_LENGTH			96
#define MAX_BOT_PROFILES			32
#define MAX_BOT_SKILLS			8

typedef struct bot_skill_aiming_s
{
	float		max_acceleration;
	float		spring_stiffness;
	float		damping;
	float		velocity_offset;
	float		modifier_max_angle;
	float		modifier_apply_time;
	float		modifier_accel_scalar;
	float		modifier_spring_scalar;
	float		modifier_damping_scalar;
} bot_skill_aiming_t;

typedef struct bot_skill_behaviors_s
{
	qboolean	allow_combat;
	qboolean	allow_grab_items_in_combat;
	qboolean	allow_melee;
	qboolean	allow_check_six;
	qboolean	allow_grab_items;
	qboolean	allow_grab_power_items;
	qboolean	defer_power_items_to_humans;
	float		min_respawn_time;
	float		max_respawn_time;
} bot_skill_behaviors_t;

typedef struct bot_skill_movement_s
{
	qboolean	allow_jumping_in_combat;
	float		jump_chance;
	float		jump_cooldown;
	qboolean	walk_only;
} bot_skill_movement_t;

typedef struct bot_skill_senses_s
{
	float		sight_time;
	float		sight_decay_time;
	float		invis_enemy_sight_scalar;
	float		max_invis_enemy_sight_dist;
	float		fov_angle;
	float		forget_non_vis_enemy_time;
	float		sound_range;
	float		sound_time;
	float		sound_decay_time;
	float		sound_persist_time;
} bot_skill_senses_t;

typedef struct bot_skill_weapons_s
{
	float		sight_time;
	float		decay_time;
	float		fov_angle;
} bot_skill_weapons_t;

typedef struct bot_skill_settings_s
{
	char		name[32];
	bot_skill_aiming_t		aiming;
	bot_skill_behaviors_t	behaviors;
	bot_skill_movement_t	movement;
	bot_skill_senses_t		senses;
	bot_skill_weapons_t		weapons;
} bot_skill_settings_t;

typedef enum
{
        BOT_PRIORITY_NONE = 0,
        BOT_PRIORITY_WEAPON,
        BOT_PRIORITY_HEALTH,
        BOT_PRIORITY_ARMOR,
        BOT_PRIORITY_ENEMY
} bot_priority_t;

typedef struct bot_profile_s
{
	char		name[32];
	int		topcolor;
	int		bottomcolor;
	int		skin;
	float		skill;
	int		chat_count;
	char		chat_lines[MAX_BOT_CHAT_LINES][MAX_BOT_CHAT_LENGTH];
} bot_profile_t;

#define BOT_MAX_WAYPOINTS			256
#define BOT_MAX_PATH_LENGTH		 64
#define BOT_MAX_NODES			(BOT_MAX_WAYPOINTS + 2)
#define BOT_WALL_CHECK_INTERVAL 0.4
#define BOT_ROCKET_MIN_SAFE_DIST		160.0f

typedef struct
{
	edict_t		*edict;
	vec3_t		position;
	float		radius;
} bot_path_node_t;

typedef struct
{
	vec3_t		origin;
	float		radius;
	unsigned int	first_link;
	unsigned int	link_count;
	unsigned int	flags;
} bot_nav_node_t;

typedef struct
{
	unsigned int	to;
	unsigned int	type;
	unsigned int	hint_index;
	float		cost;
} bot_nav_link_t;

typedef struct
{
	vec3_t		points[4];
	int		point_count;
} bot_nav_hint_t;

typedef struct
{
	bot_nav_node_t	*nodes;
	bot_nav_link_t	*links;
	bot_nav_hint_t	*hints;
	int		node_count;
	int		link_count;
	int		hint_count;
} bot_nav_mesh_t;

static bot_nav_mesh_t bot_nav_mesh;

typedef struct
{
	const byte		*data;
	size_t		size;
	size_t		offset;
	qboolean		error;
} bot_nav_reader_t;

static void SV_Bot_NavReadBytes (bot_nav_reader_t *reader, void *dest, size_t count)
{
	if (!reader || reader->offset + count > reader->size)
	{
		if (dest && count > 0)
			memset (dest, 0, count);
		if (reader)
			reader->error = true;
		return;
	}
	if (count > 0 && dest)
		memcpy (dest, reader->data + reader->offset, count);
	reader->offset += count;
}

static unsigned int SV_Bot_NavReadU32 (bot_nav_reader_t *reader)
{
	unsigned int value = 0;
	SV_Bot_NavReadBytes (reader, &value, sizeof(value));
	return LittleLong ((int)value);
}

static unsigned short SV_Bot_NavReadU16 (bot_nav_reader_t *reader)
{
	unsigned short value = 0;
	SV_Bot_NavReadBytes (reader, &value, sizeof(value));
	return LittleShort ((short)value);
}

static float SV_Bot_NavReadFloat (bot_nav_reader_t *reader)
{
	float value = 0.0f;
	SV_Bot_NavReadBytes (reader, &value, sizeof(value));
	return LittleFloat (value);
}

typedef struct
{
	qboolean		active;
	vec3_t		wander_dir;
	float		wander_yaw;
	double		next_wander_time;
	double		last_stuck_time;
	vec3_t		last_position;
	const bot_profile_t	*profile;
	float		skill;
	const bot_skill_settings_t *settings;
	vec3_t		aim_angles;
	vec3_t		aim_velocity;
	double		aim_modifier_end_time;
	bot_priority_t	goal_priority;
	edict_t		*goal_edict;
	float		goal_best_dist;
	double		goal_last_progress_time;
	double		next_chat_time;
	int			enemy_slot;
	double		enemy_first_seen_time;
	double		enemy_last_visible_time;
	double		next_jump_check_time;
	qboolean		aim_initialized;
	double		next_wall_check_time;
	double		next_path_recalc_time;
	bot_path_node_t	path_nodes[BOT_MAX_PATH_LENGTH];
	int			path_length;
	int			path_index;
} sv_bot_state_t;

static sv_bot_state_t sv_bot_states[MAX_SCOREBOARD];
static int bot_name_counter = 1;
static bot_profile_t bot_profiles[MAX_BOT_PROFILES];
static int bot_profile_count = 0;
static int bot_profile_cursor = 0;
static qboolean bot_profiles_loaded = false;
static bot_skill_settings_t bot_skill_settings[MAX_BOT_SKILLS];
static int bot_skill_settings_count = 0;
static qboolean bot_skill_settings_loaded = false;

typedef struct
{
        edict_t         *edict;
} bot_waypoint_entry_t;

static bot_waypoint_entry_t bot_waypoints[BOT_MAX_WAYPOINTS];
static int bot_waypoint_count = 0;
static qboolean bot_waypoints_initialized = false;

static qboolean SV_Bot_WaypointReachable (client_t *bot, edict_t *waypoint);



static void SV_Bot_FreeNavMesh (void)
{
	if (bot_nav_mesh.nodes)
	{
		Z_Free (bot_nav_mesh.nodes);
		bot_nav_mesh.nodes = NULL;
	}
	if (bot_nav_mesh.links)
	{
		Z_Free (bot_nav_mesh.links);
		bot_nav_mesh.links = NULL;
	}
	if (bot_nav_mesh.hints)
	{
		Z_Free (bot_nav_mesh.hints);
		bot_nav_mesh.hints = NULL;
	}
	bot_nav_mesh.node_count = 0;
	bot_nav_mesh.link_count = 0;
	bot_nav_mesh.hint_count = 0;
}

static qboolean SV_Bot_HasNavMesh (void)
{
	return bot_nav_mesh.node_count > 0 && bot_nav_mesh.nodes && bot_nav_mesh.links;
}

static qboolean SV_Bot_ParseNavFile (const byte *data, size_t size, const char *source)
{
	bot_nav_reader_t reader;
	char magic[4];
	unsigned int version;
	unsigned int base_version;
	unsigned int node_count;
	unsigned int link_count;
	unsigned int hint_count;
	unsigned int entity_count = 0;
	qboolean is_nav3 = false;
	qboolean use_wide_indices = false;
	qboolean radius_is_float = false;
	qboolean flags_are_u32 = false;
	qboolean link_types_are_u32 = false;
	qboolean hint_indices_are_u32 = false;
	unsigned int i;
	float z_offset;

	if (!data || size < 16)
		return false;

	reader.data = data;
	reader.size = size;
	reader.offset = 0;
	reader.error = false;

	SV_Bot_NavReadBytes (&reader, magic, sizeof(magic));
	if (reader.error)
		return false;
	if (memcmp (magic, "NAV2", sizeof(magic)) == 0)
	{
		is_nav3 = false;
	}
	else if (memcmp (magic, "NAV3", sizeof(magic)) == 0)
	{
		is_nav3 = true;
	}
	else
	{
		return false;
	}

	version = SV_Bot_NavReadU32 (&reader);
	base_version = version;
	if (reader.error)
		return false;
	if (is_nav3)
	{
		if (base_version < 2 || base_version > 6)
			return false;
		version = base_version;
		use_wide_indices = true;
		radius_is_float = true;
		flags_are_u32 = true;
		link_types_are_u32 = true;
		hint_indices_are_u32 = true;
	}
	else
	{
		if (version < 12 || version > 21)
			return false;
		if (version >= 18)
		{
			use_wide_indices = true;
			radius_is_float = true;
			flags_are_u32 = true;
			link_types_are_u32 = true;
			hint_indices_are_u32 = true;
		}
	}
       if (!hint_indices_are_u32)
               hint_indices_are_u32 = use_wide_indices;

	node_count = SV_Bot_NavReadU32 (&reader);
	link_count = SV_Bot_NavReadU32 (&reader);
	hint_count = SV_Bot_NavReadU32 (&reader);
	if (reader.error)
		return false;
	if (node_count == 0 || link_count == 0)
		return false;
	if (node_count > (unsigned int)INT_MAX || link_count > (unsigned int)INT_MAX || hint_count > (unsigned int)INT_MAX)
		return false;
	if (node_count > 131072 || link_count > 1048576 || hint_count > 262144)
		return false;

	if (!is_nav3 && version >= 16)
	{
		(void) SV_Bot_NavReadFloat (&reader);
		if (reader.error)
			return false;
	}

	if (node_count > 0 && node_count > SIZE_MAX / sizeof(*bot_nav_mesh.nodes))
		return false;
	if (link_count > 0 && link_count > SIZE_MAX / sizeof(*bot_nav_mesh.links))
		return false;
	if (hint_count > 0 && hint_count > SIZE_MAX / sizeof(*bot_nav_mesh.hints))
		return false;

	bot_nav_mesh.nodes = (bot_nav_node_t *) Z_Malloc (node_count * sizeof(*bot_nav_mesh.nodes));
	bot_nav_mesh.links = (bot_nav_link_t *) Z_Malloc (link_count * sizeof(*bot_nav_mesh.links));
	bot_nav_mesh.hints = hint_count ? (bot_nav_hint_t *) Z_Malloc (hint_count * sizeof(*bot_nav_mesh.hints)) : NULL;
	bot_nav_mesh.node_count = (int) node_count;
	bot_nav_mesh.link_count = (int) link_count;
	bot_nav_mesh.hint_count = (int) hint_count;

	memset (bot_nav_mesh.nodes, 0, node_count * sizeof(*bot_nav_mesh.nodes));
	memset (bot_nav_mesh.links, 0, link_count * sizeof(*bot_nav_mesh.links));
	if (bot_nav_mesh.hints)
		memset (bot_nav_mesh.hints, 0, hint_count * sizeof(*bot_nav_mesh.hints));

	for (i = 0; i < node_count; i++)
	{
		unsigned int flags = flags_are_u32 ? SV_Bot_NavReadU32 (&reader) : (unsigned int) SV_Bot_NavReadU16 (&reader);
		unsigned int link_total = use_wide_indices ? SV_Bot_NavReadU32 (&reader) : (unsigned int) SV_Bot_NavReadU16 (&reader);
		unsigned int first_link = use_wide_indices ? SV_Bot_NavReadU32 (&reader) : (unsigned int) SV_Bot_NavReadU16 (&reader);
		float radius = radius_is_float ? SV_Bot_NavReadFloat (&reader) : (float) SV_Bot_NavReadU16 (&reader);
		if (reader.error)
			goto nav_fail;
		if (first_link > link_count || first_link + link_total > link_count)
			goto nav_fail;
		bot_nav_mesh.nodes[i].flags = flags;
		bot_nav_mesh.nodes[i].first_link = first_link;
		bot_nav_mesh.nodes[i].link_count = link_total;
		bot_nav_mesh.nodes[i].radius = (radius > 0.0f) ? radius : 0.0f;
	}

	z_offset = is_nav3 ? 0.0f : 24.0f;

	for (i = 0; i < node_count; i++)
	{
		bot_nav_mesh.nodes[i].origin[0] = SV_Bot_NavReadFloat (&reader);
		bot_nav_mesh.nodes[i].origin[1] = SV_Bot_NavReadFloat (&reader);
		bot_nav_mesh.nodes[i].origin[2] = SV_Bot_NavReadFloat (&reader) + z_offset;
		if (reader.error)
			goto nav_fail;
	}

	for (i = 0; i < link_count; i++)
	{
		unsigned int to = use_wide_indices ? SV_Bot_NavReadU32 (&reader) : (unsigned int) SV_Bot_NavReadU16 (&reader);
		unsigned int type = link_types_are_u32 ? SV_Bot_NavReadU32 (&reader) : (unsigned int) SV_Bot_NavReadU16 (&reader);
		unsigned int hint = hint_indices_are_u32 ? SV_Bot_NavReadU32 (&reader) : (unsigned int) SV_Bot_NavReadU16 (&reader);
		if (reader.error)
			goto nav_fail;
		bot_nav_mesh.links[i].to = to;
		bot_nav_mesh.links[i].type = type;
		bot_nav_mesh.links[i].hint_index = (hint < hint_count) ? hint : 0xffffffffu;
		bot_nav_mesh.links[i].cost = 0.0f;
	}

	for (i = 0; i < hint_count; i++)
	{
		int j;
		for (j = 0; j < 3; j++)
		{
			bot_nav_mesh.hints[i].points[j][0] = SV_Bot_NavReadFloat (&reader);
			bot_nav_mesh.hints[i].points[j][1] = SV_Bot_NavReadFloat (&reader);
			bot_nav_mesh.hints[i].points[j][2] = SV_Bot_NavReadFloat (&reader);
		}
		bot_nav_mesh.hints[i].point_count = 3;
		if (is_nav3 && base_version >= 6)
		{
			bot_nav_mesh.hints[i].points[3][0] = SV_Bot_NavReadFloat (&reader);
			bot_nav_mesh.hints[i].points[3][1] = SV_Bot_NavReadFloat (&reader);
			bot_nav_mesh.hints[i].points[3][2] = SV_Bot_NavReadFloat (&reader);
			bot_nav_mesh.hints[i].point_count = 4;
		}
		if (reader.error)
			goto nav_fail;
	}

	if (!is_nav3 && reader.size - reader.offset >= sizeof(unsigned int))
	{
		entity_count = SV_Bot_NavReadU32 (&reader);
		if (reader.error)
			goto nav_fail;
		for (i = 0; i < entity_count; i++)
		{
			unsigned int link = use_wide_indices ? SV_Bot_NavReadU32 (&reader) : (unsigned int) SV_Bot_NavReadU16 (&reader);
			float ignore;
			if (reader.error)
				goto nav_fail;
			if (link >= link_count)
				goto nav_fail;
			ignore = SV_Bot_NavReadFloat (&reader);
			ignore = SV_Bot_NavReadFloat (&reader);
			ignore = SV_Bot_NavReadFloat (&reader);
			ignore = SV_Bot_NavReadFloat (&reader);
			ignore = SV_Bot_NavReadFloat (&reader);
			ignore = SV_Bot_NavReadFloat (&reader);
			(void)ignore;
			if (reader.error)
				goto nav_fail;
			if (version == 14)
			{
				SV_Bot_NavReadU32 (&reader);
				SV_Bot_NavReadU32 (&reader);
			}
			else if (version >= 15)
			{
				SV_Bot_NavReadU32 (&reader);
			}
			if (reader.error)
				goto nav_fail;
		}
	}

	for (i = 0; i < node_count; i++)
	{
		unsigned int j;
		bot_nav_node_t *node = &bot_nav_mesh.nodes[i];
		for (j = 0; j < node->link_count; j++)
		{
			unsigned int link_index = node->first_link + j;
			vec3_t delta;
			bot_nav_link_t *link;
			if (link_index >= (unsigned int) bot_nav_mesh.link_count)
				goto nav_fail;
			link = &bot_nav_mesh.links[link_index];
			if (link->to >= node_count)
				goto nav_fail;
			VectorSubtract (bot_nav_mesh.nodes[link->to].origin, node->origin, delta);
			link->cost = VectorLength (delta);
			if (link->cost <= 0.0f)
				link->cost = 1.0f;
		}
	}

	if (reader.error)
		goto nav_fail;

	return true;

nav_fail:
	SV_Bot_FreeNavMesh ();
	return false;
}

static qboolean SV_Bot_LoadNavMeshFromPath (const char *path)
{
	byte *buffer;
	size_t size;
	qboolean result;

	buffer = COM_LoadMallocFile (path, NULL);
	if (!buffer)
		return false;

	size = (com_filesize > 0) ? (size_t) com_filesize : 0;
	if (size == 0)
	{
		free (buffer);
		return false;
	}

	result = SV_Bot_ParseNavFile (buffer, size, path);
	free (buffer);
        if (result)
        {
                Con_Printf ("Using bot navigation file: %s (%d nodes, %d links)\n", path, bot_nav_mesh.node_count, bot_nav_mesh.link_count);
        }
        return result;
}

void SV_Bot_LoadNavigation (const char *mapname)
{
	static const char *patterns[] =
	{
		"maps/%s.nav2",
		"maps/%s.nav",
		"data/%s.nav2",
		"data/%s.nav",
		"bots/navigation/%s.nav2",
		"bots/navigation/%s.nav"
	};
	int i;

	SV_Bot_FreeNavMesh ();

	if (!mapname || !*mapname)
		return;

        for (i = 0; i < (int)(sizeof(patterns) / sizeof(patterns[0])); i++)
        {
                char path[MAX_QPATH];
                q_snprintf (path, sizeof(path), patterns[i], mapname);
                if (SV_Bot_LoadNavMeshFromPath (path))
                        return;
        }

        Con_Printf ("No bot navigation file present for %s\n", mapname);
}

static void SV_Bot_InitPathNode (bot_path_node_t *node)
{
	if (!node)
		return;
	node->edict = NULL;
	VectorClear (node->position);
	node->radius = 0.0f;
}

static void SV_Bot_PathNodeFromEdict (bot_path_node_t *node, edict_t *ent)
{
	if (!node)
		return;
	node->edict = ent;
	if (ent && !ent->free)
		VectorCopy (ent->v.origin, node->position);
	else
		VectorClear (node->position);
	node->radius = 64.0f;
}

static void SV_Bot_PathNodeFromNav (bot_path_node_t *node, const bot_nav_node_t *nav)
{
	if (!node)
		return;
	node->edict = NULL;
	if (nav)
	{
		VectorCopy (nav->origin, node->position);
		node->radius = (nav->radius > 0.0f) ? nav->radius : 64.0f;
	}
	else
	{
		VectorClear (node->position);
		node->radius = 64.0f;
	}
}

static void SV_Bot_PathNodeOrigin (const bot_path_node_t *node, vec3_t out)
{
	if (!node)
	{
		VectorClear (out);
		return;
	}
	if (node->edict && !node->edict->free)
		VectorCopy (node->edict->v.origin, out);
	else
		VectorCopy (node->position, out);
}

static float SV_Bot_PathNodeRadius (const bot_path_node_t *node)
{
	if (!node)
		return 0.0f;
	if (node->radius > 0.0f)
		return node->radius;
	return 64.0f;
}

static int SV_Bot_FindClosestNavNode (const vec3_t pos, edict_t *ignore)
{
	int best = -1;
	float best_score = FLT_MAX;
	int fallback = -1;
	float fallback_score = FLT_MAX;
	int i;

	if (!SV_Bot_HasNavMesh ())
		return -1;

	for (i = 0; i < bot_nav_mesh.node_count; i++)
	{
		const bot_nav_node_t *node = &bot_nav_mesh.nodes[i];
		vec3_t delta;
		float dist2;
		float radius = (node->radius > 0.0f) ? node->radius : 32.0f;
		float score;

		VectorSubtract (node->origin, pos, delta);
		dist2 = DotProduct (delta, delta);
		score = dist2;
		if (radius > 0.0f)
		{
			float radius2 = radius * radius;
			if (dist2 <= radius2)
				score = dist2 - radius2;
		}

		if (score < fallback_score)
		{
			fallback_score = score;
			fallback = i;
		}

                if (score >= best_score)
                        continue;

                if (radius > 0.0f && dist2 > radius * radius)
                {
                        vec3_t start;
                        vec3_t end;
                        trace_t trace;

                        VectorCopy (pos, start);
                        VectorCopy (node->origin, end);
                        trace = SV_Move (start, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, ignore);
                        if (trace.fraction < 0.99f)
                                continue;
                }

		best_score = score;
		best = i;
	}

	return (best >= 0) ? best : fallback;
}

static qboolean SV_Bot_BuildNavPath (edict_t *self, edict_t *goal, bot_path_node_t *out_path, int *out_length)
{
	float *g_cost = NULL;
	float *f_cost = NULL;
	int *came_from = NULL;
	int *open_list = NULL;
	qboolean *in_open = NULL;
	qboolean *closed = NULL;
	int open_count = 0;
	int start_node;
	int goal_node;
	int node_count;
	int i;
	qboolean success = false;

	if (!self || !goal || !out_path || !out_length)
		return false;
	if (!SV_Bot_HasNavMesh ())
		return false;

	node_count = bot_nav_mesh.node_count;
	if (node_count <= 0)
		return false;

	start_node = SV_Bot_FindClosestNavNode (self->v.origin, self);
	goal_node = SV_Bot_FindClosestNavNode (goal->v.origin, goal);
	if (start_node < 0 || goal_node < 0)
		return false;
	if (start_node == goal_node)
	{
		*out_length = 0;
		return true;
	}

	g_cost = (float *) malloc (node_count * sizeof(*g_cost));
	f_cost = (float *) malloc (node_count * sizeof(*f_cost));
	came_from = (int *) malloc (node_count * sizeof(*came_from));
	open_list = (int *) malloc (node_count * sizeof(*open_list));
	in_open = (qboolean *) malloc (node_count * sizeof(*in_open));
	closed = (qboolean *) malloc (node_count * sizeof(*closed));
	if (!g_cost || !f_cost || !came_from || !open_list || !in_open || !closed)
		goto nav_cleanup;

	for (i = 0; i < node_count; i++)
	{
		g_cost[i] = FLT_MAX;
		f_cost[i] = FLT_MAX;
		came_from[i] = -1;
		in_open[i] = false;
		closed[i] = false;
	}

	g_cost[start_node] = 0.0f;
	{
		vec3_t diff;
		VectorSubtract (bot_nav_mesh.nodes[start_node].origin, bot_nav_mesh.nodes[goal_node].origin, diff);
		f_cost[start_node] = VectorLength (diff);
	}
	open_list[open_count++] = start_node;
	in_open[start_node] = true;

        while (open_count > 0)
        {
                int best_pos = 0;
                int current = open_list[0];
                float best_f = f_cost[current];
                int j;

                for (j = 1; j < open_count; j++)
                {
                        int node = open_list[j];
                        if (f_cost[node] < best_f)
                        {
                                best_f = f_cost[node];
				current = node;
				best_pos = j;
			}
		}

		open_count--;
		open_list[best_pos] = open_list[open_count];
		in_open[current] = false;

		if (current == goal_node)
		{
			success = true;
			break;
		}

                closed[current] = true;

                for (unsigned int link_offset = 0; link_offset < bot_nav_mesh.nodes[current].link_count; link_offset++)
                {
                        unsigned int link_index = bot_nav_mesh.nodes[current].first_link + link_offset;
                        const bot_nav_link_t *link;
                        int neighbor;
                        float step;
                        float tentative_g;
                        vec3_t diff;

			if (link_index >= (unsigned int) bot_nav_mesh.link_count)
				continue;
			link = &bot_nav_mesh.links[link_index];
			if (link->to >= (unsigned int) node_count)
				continue;
			neighbor = (int) link->to;
			if (closed[neighbor])
				continue;

			step = (link->cost > 0.0f) ? link->cost : 1.0f;
			tentative_g = g_cost[current] + step;
			if (tentative_g >= g_cost[neighbor])
				continue;

			came_from[neighbor] = current;
			g_cost[neighbor] = tentative_g;
			VectorSubtract (bot_nav_mesh.nodes[neighbor].origin, bot_nav_mesh.nodes[goal_node].origin, diff);
			f_cost[neighbor] = tentative_g + VectorLength (diff);

			if (!in_open[neighbor])
			{
				if (open_count >= node_count)
				{
					success = false;
					goto nav_cleanup;
				}
				open_list[open_count++] = neighbor;
				in_open[neighbor] = true;
			}
		}
	}

	if (!success)
		goto nav_cleanup;

	{
		int order[BOT_MAX_PATH_LENGTH];
		int order_length = 0;
		int current = goal_node;

		while (current != start_node && order_length < BOT_MAX_PATH_LENGTH)
		{
			order[order_length++] = current;
			current = came_from[current];
			if (current < 0)
			{
				success = false;
				goto nav_cleanup;
			}
		}

		*out_length = 0;
		while (order_length > 0)
		{
			int node_index = order[--order_length];
			if (*out_length >= BOT_MAX_PATH_LENGTH)
			{
				success = false;
				goto nav_cleanup;
			}
			SV_Bot_PathNodeFromNav (&out_path[*out_length], &bot_nav_mesh.nodes[node_index]);
			(*out_length)++;
		}
	}

	success = true;

nav_cleanup:
	if (g_cost) free (g_cost);
	if (f_cost) free (f_cost);
	if (came_from) free (came_from);
	if (open_list) free (open_list);
	if (in_open) free (in_open);
	if (closed) free (closed);
	return success;
}

static float SV_Bot_Distance (const vec3_t a, const vec3_t b);



static float SV_Bot_Frand (void)
{
        return (float)rand () / (float)RAND_MAX;
}

static qboolean SV_Bot_IsPickupWaypoint (const edict_t *ent)
{
        const char *classname;

        if (!ent || ent->free || !ent->v.classname)
                return false;

        classname = PR_GetString (ent->v.classname);
        if (!classname || !classname[0])
                return false;

        if (!q_strncasecmp (classname, "weapon_", 7))
                return true;
        if (!q_strncasecmp (classname, "item_", 5))
                return true;
        if (!q_strncasecmp (classname, "ammo_", 5))
                return true;

        return false;
}

static void SV_Bot_BuildWaypointList (void)
{
        int i;

        bot_waypoint_count = 0;

        for (i = svs.maxclients + 1; i < qcvm->num_edicts && bot_waypoint_count < BOT_MAX_WAYPOINTS; i++)
        {
                edict_t *ent = EDICT_NUM (i);

                if (!SV_Bot_IsPickupWaypoint (ent))
                        continue;

                bot_waypoints[bot_waypoint_count].edict = ent;
                bot_waypoint_count++;
        }

        bot_waypoints_initialized = true;
}

static void SV_Bot_EnsureWaypoints (void)
{
        int i;

        if (!bot_waypoints_initialized)
        {
                SV_Bot_BuildWaypointList ();
                return;
        }

        for (i = 0; i < bot_waypoint_count; i++)
        {
                if (!bot_waypoints[i].edict || bot_waypoints[i].edict->free)
                {
                        bot_waypoints_initialized = false;
                        SV_Bot_BuildWaypointList ();
                        break;
                }
        }
}

static qboolean SV_Bot_WaypointReachable (client_t *bot, edict_t *waypoint)
{
	bot_path_node_t path[BOT_MAX_PATH_LENGTH];
	int path_length = 0;

	if (!bot || !bot->edict || bot->edict->free || !waypoint || waypoint->free)
		return false;

	return SV_Bot_HasNavMesh () && SV_Bot_BuildNavPath (bot->edict, waypoint, path, &path_length) && path_length > 0;
}

static void SV_Bot_GetNodePosition (const edict_t *ent, vec3_t out)
{
	if (!ent)
	{
		VectorClear (out);
		return;
	}

	VectorCopy (ent->v.origin, out);
	if (ent->v.view_ofs[2] != 0.0f)
		out[2] += ent->v.view_ofs[2];
	else
		out[2] += 16.0f;
}

static void SV_Bot_ClearPath (sv_bot_state_t *state)
{
        if (!state)
                return;

        state->path_length = 0;
        state->path_index = 0;
}

static qboolean SV_Bot_RecalculatePath (edict_t *self, sv_bot_state_t *state, double now)
{
        bot_path_node_t path[BOT_MAX_PATH_LENGTH];
        int length = 0;
        int i;
        edict_t *goal;

	if (!self || !state)
		return false;

	goal = state->goal_edict;
	state->path_length = 0;
	state->path_index = 0;
	state->next_path_recalc_time = now + 1.0;

	if (!goal)
		return false;

	if (!SV_Bot_HasNavMesh () || !SV_Bot_BuildNavPath (self, goal, path, &length))
		return false;

        if (length < BOT_MAX_PATH_LENGTH)
        {
                bot_path_node_t goal_node;
                SV_Bot_PathNodeFromEdict (&goal_node, goal);
                if (length == 0 || path[length - 1].edict != goal)
                        path[length++] = goal_node;
                else
                        path[length - 1] = goal_node;
        }

        if (length > BOT_MAX_PATH_LENGTH)
                length = BOT_MAX_PATH_LENGTH;

        for (i = 0; i < length; i++)
                state->path_nodes[i] = path[i];

        state->path_length = length;
        state->path_index = 0;
        state->next_path_recalc_time = now + 0.5;

        return true;
}

static void SV_Bot_UpdatePathProgress (edict_t *self, sv_bot_state_t *state)
{
        if (!self || !state || state->path_length <= 0)
                return;

        while (state->path_index < state->path_length)
        {
                const bot_path_node_t *target = &state->path_nodes[state->path_index];
                vec3_t target_origin;
                float radius;

                if (target->edict && target->edict->free)
                {
                        state->path_length = 0;
                        state->path_index = 0;
                        return;
                }

                SV_Bot_PathNodeOrigin (target, target_origin);
                radius = SV_Bot_PathNodeRadius (target);
                if (radius <= 0.0f)
                        radius = 64.0f;

                if (SV_Bot_Distance (target_origin, self->v.origin) <= radius)
                {
                        state->path_index++;
                        continue;
                }

                break;
        }

        if (state->path_index >= state->path_length)
        {
                state->path_length = 0;
                state->path_index = 0;
        }
}

static void SV_Bot_ResetProfiles (void)
{
	bot_profile_count = 0;
	bot_profile_cursor = 0;
	bot_profiles_loaded = false;
}

static void SV_Bot_AddProfile (const bot_profile_t *profile)
{
	if (!profile || !profile->name[0])
		return;
	if (bot_profile_count >= MAX_BOT_PROFILES)
		return;
	bot_profiles[bot_profile_count++] = *profile;
}

static void SV_Bot_AddFallbackProfiles (void)
{
        bot_profile_t profile;

        memset (&profile, 0, sizeof(profile));
        q_strlcpy (profile.name, "Ranger", sizeof(profile.name));
        profile.topcolor = 4;
        profile.bottomcolor = 12;
        profile.skin = 0;
        profile.skill = 1.2f;
        profile.chat_count = 3;
        q_strlcpy (profile.chat_lines[0], "Ready to frag.", sizeof(profile.chat_lines[0]));
        q_strlcpy (profile.chat_lines[1], "Keep moving!", sizeof(profile.chat_lines[1]));
        q_strlcpy (profile.chat_lines[2], "No escape for you.", sizeof(profile.chat_lines[2]));
        SV_Bot_AddProfile (&profile);

        memset (&profile, 0, sizeof(profile));
        q_strlcpy (profile.name, "Hunter", sizeof(profile.name));
        profile.topcolor = 13;
        profile.bottomcolor = 3;
        profile.skin = 1;
        profile.skill = 1.4f;
        profile.chat_count = 2;
        q_strlcpy (profile.chat_lines[0], "The hunt never stops.", sizeof(profile.chat_lines[0]));
        q_strlcpy (profile.chat_lines[1], "Lock and load.", sizeof(profile.chat_lines[1]));
        SV_Bot_AddProfile (&profile);

        memset (&profile, 0, sizeof(profile));
        q_strlcpy (profile.name, "Visor", sizeof(profile.name));
        profile.topcolor = 8;
        profile.bottomcolor = 2;
        profile.skin = 2;
        profile.skill = 0.9f;
        profile.chat_count = 2;
        q_strlcpy (profile.chat_lines[0], "Systems calibrated.", sizeof(profile.chat_lines[0]));
        q_strlcpy (profile.chat_lines[1], "Target acquired.", sizeof(profile.chat_lines[1]));
        SV_Bot_AddProfile (&profile);
}

static qboolean SV_Bot_ParseBool (const char *token)
{
        if (!token)
                return false;
        if (!q_strcasecmp (token, "true") || !q_strcasecmp (token, "1") || !q_strcasecmp (token, "yes") || !q_strcasecmp (token, "on"))
                return true;
        return false;
}

static void SV_Bot_DefaultSkillSettings (bot_skill_settings_t *settings, const char *name)
{
        memset (settings, 0, sizeof(*settings));
        if (name)
                q_strlcpy (settings->name, name, sizeof(settings->name));
        settings->aiming.max_acceleration = 200.0f;
        settings->aiming.spring_stiffness = 40.0f;
        settings->aiming.damping = 5.0f;
        settings->aiming.modifier_max_angle = 45.0f;
        settings->aiming.modifier_apply_time = 0.5f;
        settings->aiming.modifier_accel_scalar = 1.0f;
        settings->aiming.modifier_spring_scalar = 1.0f;
        settings->aiming.modifier_damping_scalar = 1.0f;

        settings->behaviors.allow_combat = true;
        settings->behaviors.allow_check_six = true;
        settings->behaviors.allow_grab_items = true;
        settings->behaviors.allow_grab_power_items = true;
        settings->behaviors.defer_power_items_to_humans = true;
        settings->behaviors.min_respawn_time = 1.0f;
        settings->behaviors.max_respawn_time = 2.0f;

        settings->movement.allow_jumping_in_combat = true;
        settings->movement.jump_cooldown = 1.0f;

        settings->senses.fov_angle = 140.0f;
        settings->senses.sound_range = 512.0f;
        settings->senses.sound_time = 0.4f;
        settings->senses.sound_decay_time = 2.0f;
        settings->senses.sound_persist_time = 0.3f;
        settings->senses.forget_non_vis_enemy_time = 2.0f;

        settings->weapons.sight_time = 0.2f;
        settings->weapons.decay_time = 1.0f;
        settings->weapons.fov_angle = 45.0f;
}

static qboolean SV_Bot_SetSkillField (bot_skill_settings_t *settings, const char *key, const char *value)
{
        if (!settings || !key || !value)
                return false;

#define BOT_SET_FLOAT(_field) settings->_field = (float)atof (value)
#define BOT_SET_BOOL(_field) settings->_field = SV_Bot_ParseBool (value)

        if (!q_strcasecmp (key, "aiming.max_acceleration"))
        { BOT_SET_FLOAT (aiming.max_acceleration); return true; }
        if (!q_strcasecmp (key, "aiming.spring_stiffness"))
        { BOT_SET_FLOAT (aiming.spring_stiffness); return true; }
        if (!q_strcasecmp (key, "aiming.damping"))
        { BOT_SET_FLOAT (aiming.damping); return true; }
        if (!q_strcasecmp (key, "aiming.velocity_offset"))
        { BOT_SET_FLOAT (aiming.velocity_offset); return true; }
        if (!q_strcasecmp (key, "aiming.modifier.max_angle"))
        { BOT_SET_FLOAT (aiming.modifier_max_angle); return true; }
        if (!q_strcasecmp (key, "aiming.modifier.apply_time"))
        { BOT_SET_FLOAT (aiming.modifier_apply_time); return true; }
        if (!q_strcasecmp (key, "aiming.modifier.accel_scalar"))
        { BOT_SET_FLOAT (aiming.modifier_accel_scalar); return true; }
        if (!q_strcasecmp (key, "aiming.modifier.spring_scalar"))
        { BOT_SET_FLOAT (aiming.modifier_spring_scalar); return true; }
        if (!q_strcasecmp (key, "aiming.modifier.damping_scalar"))
        { BOT_SET_FLOAT (aiming.modifier_damping_scalar); return true; }

        if (!q_strcasecmp (key, "behaviors.allow_combat"))
        { BOT_SET_BOOL (behaviors.allow_combat); return true; }
        if (!q_strcasecmp (key, "behaviors.allow_grab_items_in_combat"))
        { BOT_SET_BOOL (behaviors.allow_grab_items_in_combat); return true; }
        if (!q_strcasecmp (key, "behaviors.allow_melee"))
        { BOT_SET_BOOL (behaviors.allow_melee); return true; }
        if (!q_strcasecmp (key, "behaviors.allow_check_six"))
        { BOT_SET_BOOL (behaviors.allow_check_six); return true; }
        if (!q_strcasecmp (key, "behaviors.allow_grab_items"))
        { BOT_SET_BOOL (behaviors.allow_grab_items); return true; }
        if (!q_strcasecmp (key, "behaviors.allow_grab_power_items"))
        { BOT_SET_BOOL (behaviors.allow_grab_power_items); return true; }
        if (!q_strcasecmp (key, "behaviors.defer_power_items_to_humans"))
        { BOT_SET_BOOL (behaviors.defer_power_items_to_humans); return true; }
        if (!q_strcasecmp (key, "behaviors.min_respawn_time"))
        { BOT_SET_FLOAT (behaviors.min_respawn_time); return true; }
        if (!q_strcasecmp (key, "behaviors.max_respawn_time"))
        { BOT_SET_FLOAT (behaviors.max_respawn_time); return true; }

        if (!q_strcasecmp (key, "movement.allow_jumping_in_combat"))
        { BOT_SET_BOOL (movement.allow_jumping_in_combat); return true; }
        if (!q_strcasecmp (key, "movement.jump_chance"))
        {
                float chance = (float)atof (value);
                settings->movement.jump_chance = CLAMP (0.0f, chance, 100.0f);
                return true;
        }
        if (!q_strcasecmp (key, "movement.jump_cooldown"))
        {
                float cooldown = (float)atof (value);
                settings->movement.jump_cooldown = (cooldown < 0.0f) ? 0.0f : cooldown;
                return true;
        }
        if (!q_strcasecmp (key, "movement.walk_only"))
        { BOT_SET_BOOL (movement.walk_only); return true; }

        if (!q_strcasecmp (key, "senses.sight_time"))
        { BOT_SET_FLOAT (senses.sight_time); return true; }
        if (!q_strcasecmp (key, "senses.sight_decay_time"))
        { BOT_SET_FLOAT (senses.sight_decay_time); return true; }
        if (!q_strcasecmp (key, "senses.invis_enemy_sight_scalar"))
        { BOT_SET_FLOAT (senses.invis_enemy_sight_scalar); return true; }
        if (!q_strcasecmp (key, "senses.max_invis_enemy_sight_dist"))
        { BOT_SET_FLOAT (senses.max_invis_enemy_sight_dist); return true; }
        if (!q_strcasecmp (key, "senses.fov_angle"))
        { BOT_SET_FLOAT (senses.fov_angle); return true; }
        if (!q_strcasecmp (key, "senses.forget_non_vis_enemy_time"))
        { BOT_SET_FLOAT (senses.forget_non_vis_enemy_time); return true; }
        if (!q_strcasecmp (key, "senses.sound_range"))
        { BOT_SET_FLOAT (senses.sound_range); return true; }
        if (!q_strcasecmp (key, "senses.sound_time"))
        { BOT_SET_FLOAT (senses.sound_time); return true; }
        if (!q_strcasecmp (key, "senses.sound_decay_time"))
        { BOT_SET_FLOAT (senses.sound_decay_time); return true; }
        if (!q_strcasecmp (key, "senses.sound_persist_time"))
        { BOT_SET_FLOAT (senses.sound_persist_time); return true; }

        if (!q_strcasecmp (key, "weapons.sight_time"))
        { BOT_SET_FLOAT (weapons.sight_time); return true; }
        if (!q_strcasecmp (key, "weapons.decay_time"))
        { BOT_SET_FLOAT (weapons.decay_time); return true; }
        if (!q_strcasecmp (key, "weapons.fov_angle"))
        { BOT_SET_FLOAT (weapons.fov_angle); return true; }

#undef BOT_SET_FLOAT
#undef BOT_SET_BOOL

        return false;
}

static void SV_Bot_AddSkillSettings (const bot_skill_settings_t *settings)
{
        if (!settings || !settings->name[0])
                return;

        {
                int i;

                for (i = 0; i < bot_skill_settings_count; i++)
                {
                        if (!q_strcasecmp (bot_skill_settings[i].name, settings->name))
                        {
                                bot_skill_settings[i] = *settings;
                                return;
                        }
                }
        }

        if (bot_skill_settings_count >= MAX_BOT_SKILLS)
        {
                Con_Printf ("Too many bot skill presets, ignoring '%s'.\n", settings->name);
                return;
        }

        bot_skill_settings[bot_skill_settings_count++] = *settings;
}

static void SV_Bot_AddFallbackSkillSettings (void)
{
        bot_skill_settings_t settings;

        SV_Bot_DefaultSkillSettings (&settings, "medium");
        settings.aiming.max_acceleration = 315.0f;
        settings.aiming.spring_stiffness = 80.0f;
        settings.aiming.damping = 15.0f;
        settings.aiming.velocity_offset = -0.15f;
        settings.aiming.modifier_max_angle = 40.0f;
        settings.aiming.modifier_apply_time = 1.0f;
        settings.aiming.modifier_accel_scalar = 1.1f;
        settings.aiming.modifier_spring_scalar = 1.1f;
        settings.aiming.modifier_damping_scalar = 1.1f;

        settings.behaviors.allow_grab_items_in_combat = false;
        settings.behaviors.allow_melee = true;
        settings.behaviors.min_respawn_time = 0.9f;
        settings.behaviors.max_respawn_time = 1.5f;

        settings.movement.allow_jumping_in_combat = true;
        settings.movement.jump_chance = 25.0f;
        settings.movement.jump_cooldown = 1.0f;
        settings.movement.walk_only = false;

        settings.senses.sight_time = 0.25f;
        settings.senses.sight_decay_time = 0.4f;
        settings.senses.invis_enemy_sight_scalar = 2.0f;
        settings.senses.max_invis_enemy_sight_dist = 384.0f;
        settings.senses.fov_angle = 140.0f;
        settings.senses.sound_range = 768.0f;
        settings.senses.sound_time = 0.3f;
        settings.senses.sound_decay_time = 3.0f;
        settings.senses.sound_persist_time = 0.5f;

        settings.weapons.sight_time = 0.15f;
        settings.weapons.decay_time = 2.0f;
        settings.weapons.fov_angle = 30.0f;

        SV_Bot_AddSkillSettings (&settings);
}

static qboolean SV_Bot_ParseSkillSettings (const char *data)
{
        bot_skill_settings_t settings;
        char key[64];
        qboolean parsed_any = false;

        while ((data = COM_Parse (data)) != NULL)
        {
                if (q_strcasecmp (com_token, "skill"))
                        continue;

                data = COM_Parse (data);
                if (!data)
                        break;

                SV_Bot_DefaultSkillSettings (&settings, com_token);

                data = COM_Parse (data);
                if (!data || com_token[0] != '{')
                        break;

                for (;;)
                {
                        data = COM_Parse (data);
                        if (!data)
                                break;
                        if (com_token[0] == '}')
                        {
                                SV_Bot_AddSkillSettings (&settings);
                                parsed_any = true;
                                break;
                        }

                        q_strlcpy (key, com_token, sizeof(key));
                        data = COM_Parse (data);
                        if (!data)
                                break;

                        if (!SV_Bot_SetSkillField (&settings, key, com_token))
                                Con_Printf ("Unknown bot skill field '%s' for '%s'.\n", key, settings.name);
                }
        }

        return parsed_any;
}

static void SV_Bot_LoadSkillSettings (void)
{
        char *buffer = NULL;

        if (bot_skill_settings_loaded)
                return;

        bot_skill_settings_loaded = true;
        bot_skill_settings_count = 0;

        buffer = (char *)COM_LoadMallocFile ("bots/settings_PC.txt", NULL);
        if (!buffer)
                buffer = (char *)COM_LoadMallocFile ("config/bots/settings_PC.txt", NULL);

        if (buffer)
        {
                if (!SV_Bot_ParseSkillSettings (buffer))
                        SV_Bot_AddFallbackSkillSettings ();
                free (buffer);
        }
        else
        {
                SV_Bot_AddFallbackSkillSettings ();
        }
}

static const bot_skill_settings_t *SV_Bot_FindSkillSettings (const char *name)
{
        int i;

        if (!name || !*name)
                return NULL;

        for (i = 0; i < bot_skill_settings_count; i++)
        {
                if (!q_strcasecmp (name, bot_skill_settings[i].name))
                        return &bot_skill_settings[i];
        }

        return NULL;
}

static const bot_skill_settings_t *SV_Bot_GetSkillSettingsByIndex (int index)
{
        if (index < 0 || index >= bot_skill_settings_count)
                return NULL;
        return &bot_skill_settings[index];
}

static const bot_skill_settings_t *SV_Bot_GetSkillSettings (const char *name)
{
        const bot_skill_settings_t *settings;

        if (!bot_skill_settings_loaded)
                SV_Bot_LoadSkillSettings ();

        if (!bot_skill_settings_count)
                return NULL;

        settings = SV_Bot_FindSkillSettings (name);
        if (settings)
                return settings;

        if (name && *name && q_isdigit (name[0]))
        {
                int index = atoi (name);
                settings = SV_Bot_GetSkillSettingsByIndex (index);
                if (settings)
                        return settings;
        }

        return &bot_skill_settings[0];
}

static qboolean SV_Bot_ParseProfiles (const char *data)
{
	bot_profile_t profile;
	qboolean parsed_any = false;

	while ((data = COM_Parse (data)) != NULL)
	{
		if (q_strcasecmp (com_token, "bot") != 0)
			continue;

		data = COM_Parse (data);
		if (!data || com_token[0] != '{')
			break;

		memset (&profile, 0, sizeof(profile));
		profile.skill = 1.0f;

		for (;;)
		{
			data = COM_Parse (data);
			if (!data)
				break;
			if (com_token[0] == '}')
			{
				SV_Bot_AddProfile (&profile);
				parsed_any = true;
				break;
			}

			if (!q_strcasecmp (com_token, "name"))
			{
				data = COM_Parse (data);
				if (!data)
					break;
				q_strlcpy (profile.name, com_token, sizeof(profile.name));
				continue;
			}
			if (!q_strcasecmp (com_token, "topcolor"))
			{
				data = COM_Parse (data);
				if (!data)
					break;
				profile.topcolor = atoi (com_token);
				continue;
			}
			if (!q_strcasecmp (com_token, "bottomcolor"))
			{
				data = COM_Parse (data);
				if (!data)
					break;
				profile.bottomcolor = atoi (com_token);
				continue;
			}
			if (!q_strcasecmp (com_token, "skin"))
			{
				data = COM_Parse (data);
				if (!data)
					break;
				profile.skin = atoi (com_token);
				continue;
			}
			if (!q_strcasecmp (com_token, "skill"))
			{
				data = COM_Parse (data);
				if (!data)
					break;
				profile.skill = (float)atof (com_token);
				continue;
			}
			if (!q_strcasecmp (com_token, "chat"))
			{
				data = COM_Parse (data);
				if (!data)
					break;
				if (profile.chat_count < MAX_BOT_CHAT_LINES)
				{
					q_strlcpy (profile.chat_lines[profile.chat_count], com_token, sizeof(profile.chat_lines[0]));
					profile.chat_count++;
				}
				continue;
			}
		}
	}

	return parsed_any;
}

static void SV_Bot_LoadProfiles (void)
{
	char *buffer = NULL;

	if (bot_profiles_loaded)
		return;

	bot_profiles_loaded = true;
	bot_profile_count = 0;

	buffer = (char *)COM_LoadMallocFile ("botprofiles.cfg", NULL);
	if (!buffer)
		buffer = (char *)COM_LoadMallocFile ("config/botprofiles.cfg", NULL);

	if (buffer)
	{
                if (!SV_Bot_ParseProfiles (buffer))
                        SV_Bot_AddFallbackProfiles ();
                free (buffer);
	}
	else
	{
		SV_Bot_AddFallbackProfiles ();
	}
}

static const bot_profile_t *SV_Bot_SelectProfile (void)
{
	SV_Bot_LoadProfiles ();
	if (!bot_profile_count)
		return NULL;
	if (bot_profile_cursor >= bot_profile_count)
		bot_profile_cursor = 0;
	return &bot_profiles[bot_profile_cursor++];
}

static float SV_Bot_Distance (const vec3_t a, const vec3_t b)
{
	vec3_t delta;
	VectorSubtract (a, b, delta);
	return VectorLength (delta);
}

static float SV_Bot_ClampMoveValue (float value, const bot_skill_settings_t *settings)
{
	float limit = (settings && settings->movement.walk_only) ? 140.0f : 320.0f;
	return CLAMP (-limit, value, limit);
}

static void SV_Bot_UpdateAimAngles (edict_t *self, sv_bot_state_t *state, const vec3_t desired_angles, double now)
{
	const bot_skill_settings_t *settings = state ? state->settings : NULL;
	float dt;
	int axis;

	if (!self)
		return;

	if (!settings || !state)
	{
		VectorCopy (desired_angles, self->v.v_angle);
		return;
	}

	if (!state->aim_initialized)
	{
		VectorCopy (self->v.v_angle, state->aim_angles);
		VectorClear (state->aim_velocity);
		state->aim_initialized = true;
	}

	dt = (float)CLAMP (0.001, host_frametime, 0.1);
	if (dt <= 0.0f)
		dt = 0.01f;

	for (axis = 0; axis < 2; axis++)
	{
		float stiffness = settings->aiming.spring_stiffness;
		float damping = settings->aiming.damping;
		float max_accel = settings->aiming.max_acceleration;
		float error = AngleDifference (desired_angles[axis], state->aim_angles[axis]);
		float accel;

		if (settings->aiming.modifier_apply_time > 0.0f && fabsf (error) > settings->aiming.modifier_max_angle)
			state->aim_modifier_end_time = now + settings->aiming.modifier_apply_time;

		if (state->aim_modifier_end_time > now)
		{
			stiffness *= settings->aiming.modifier_spring_scalar;
			damping *= settings->aiming.modifier_damping_scalar;
			max_accel *= settings->aiming.modifier_accel_scalar;
		}

		accel = stiffness * error - damping * state->aim_velocity[axis];
		accel = CLAMP (-max_accel, accel, max_accel);
		state->aim_velocity[axis] += accel * dt;
		state->aim_angles[axis] = NormalizeAngle (state->aim_angles[axis] + state->aim_velocity[axis] * dt);
	}

	state->aim_angles[PITCH] = CLAMP (-70.0f, state->aim_angles[PITCH], 70.0f);
	state->aim_angles[YAW] = NormalizeAngle (state->aim_angles[YAW]);
	VectorCopy (state->aim_angles, self->v.v_angle);
}

static int SV_Bot_WeaponBitForClassname (const char *classname, int *out_rank)
{
        struct weapon_map_s
        {
                const char *classname;
		int bit;
		int rank;
	};
	static const struct weapon_map_s weapon_map[] =
	{
		{ "weapon_lightning", IT_LIGHTNING, 6 },
		{ "weapon_rocketlauncher", IT_ROCKET_LAUNCHER, 5 },
		{ "weapon_grenadelauncher", IT_GRENADE_LAUNCHER, 4 },
		{ "weapon_supernailgun", IT_SUPER_NAILGUN, 3 },
		{ "weapon_nailgun", IT_NAILGUN, 2 },
		{ "weapon_supershotgun", IT_SUPER_SHOTGUN, 1 },
		{ "weapon_shotgun", IT_SHOTGUN, 0 },
	};
	size_t i;

	for (i = 0; i < sizeof(weapon_map)/sizeof(weapon_map[0]); i++)
	{
		if (!q_strcasecmp (classname, weapon_map[i].classname))
		{
			if (out_rank)
				*out_rank = weapon_map[i].rank;
			return weapon_map[i].bit;
		}
	}

        return 0;
}

static qboolean SV_Bot_HasAmmoForWeapon (edict_t *self, int weapon_bit)
{
        if (!self)
                return false;

        switch (weapon_bit)
        {
        case IT_LIGHTNING:
                return self->v.ammo_cells > 0;
        case IT_ROCKET_LAUNCHER:
        case IT_GRENADE_LAUNCHER:
                return self->v.ammo_rockets > 0;
        case IT_SUPER_NAILGUN:
        case IT_NAILGUN:
                return self->v.ammo_nails > 0;
        case IT_SUPER_SHOTGUN:
        case IT_SHOTGUN:
                return self->v.ammo_shells > 0;
        case IT_AXE:
                return true;
        default:
                return true;
        }
}

static void SV_Bot_SelectBestWeapon (edict_t *self, edict_t *enemy)
{
        static const struct weapon_choice_s
        {
                int bit;
                int impulse;
        } choices[] =
        {
                { IT_LIGHTNING, 8 },
                { IT_ROCKET_LAUNCHER, 7 },
                { IT_GRENADE_LAUNCHER, 6 },
                { IT_SUPER_NAILGUN, 5 },
                { IT_NAILGUN, 4 },
                { IT_SUPER_SHOTGUN, 3 },
                { IT_SHOTGUN, 2 },
                { IT_AXE, 1 },
        };
        int items;
        float enemy_dist = FLT_MAX;
        int desired_bit = 0;
        int desired_impulse = 0;
        size_t i;

        if (!self)
                return;

        items = (int)self->v.items;
        if (enemy)
                enemy_dist = SV_Bot_Distance (enemy->v.origin, self->v.origin);

        for (i = 0; i < sizeof(choices)/sizeof(choices[0]); i++)
        {
                int bit = choices[i].bit;
                int impulse = choices[i].impulse;

                if (!(items & bit))
                        continue;
                if (!SV_Bot_HasAmmoForWeapon (self, bit))
                        continue;
                if (bit == IT_ROCKET_LAUNCHER && enemy && enemy_dist < BOT_ROCKET_MIN_SAFE_DIST)
                        continue;

                desired_bit = bit;
                desired_impulse = impulse;
                break;
        }

        if (!desired_bit)
        {
                for (i = 0; i < sizeof(choices)/sizeof(choices[0]); i++)
                {
                        int bit = choices[i].bit;
                        int impulse = choices[i].impulse;

                        if (!(items & bit))
                                continue;
                        if (!SV_Bot_HasAmmoForWeapon (self, bit))
                                continue;

                        desired_bit = bit;
                        desired_impulse = impulse;
                        break;
                }
        }

        if (!desired_bit)
        {
                for (i = 0; i < sizeof(choices)/sizeof(choices[0]); i++)
                {
                        int bit = choices[i].bit;
                        int impulse = choices[i].impulse;

                        if (!(items & bit))
                                continue;

                        desired_bit = bit;
                        desired_impulse = impulse;
                        break;
                }
        }

        if (desired_bit && (int)self->v.weapon != desired_bit)
                self->v.impulse = desired_impulse;
}

static edict_t *SV_Bot_FindGoal (edict_t *self, bot_priority_t priority)
{
        edict_t *best = NULL;
        float best_dist = 0.0f;
        int best_rank = -1;
	int i;

	for (i = svs.maxclients + 1; i < qcvm->num_edicts; i++)
	{
		edict_t *ent = EDICT_NUM (i);
		const char *classname;

		if (ent->free || !ent->v.classname || ent->v.solid == SOLID_NOT)
			continue;

		classname = PR_GetString (ent->v.classname);
		if (!classname || !classname[0])
			continue;

		if (priority == BOT_PRIORITY_WEAPON)
		{
			int rank = 0;
			int bit = SV_Bot_WeaponBitForClassname (classname, &rank);

			if (!bit || ((int)self->v.items & bit))
				continue;
			if (rank <= 0)
				continue;

			if (rank > best_rank || (rank == best_rank && (!best || SV_Bot_Distance (ent->v.origin, self->v.origin) < best_dist)))
			{
				best = ent;
				best_rank = rank;
				best_dist = SV_Bot_Distance (ent->v.origin, self->v.origin);
			}
			continue;
		}

		if (priority == BOT_PRIORITY_HEALTH)
		{
			if (q_strcasecmp (classname, "item_health") != 0)
				continue;
		}
		else if (priority == BOT_PRIORITY_ARMOR)
		{
			if (q_strcasecmp (classname, "item_armor1") && q_strcasecmp (classname, "item_armor2") && q_strcasecmp (classname, "item_armorInv"))
				continue;
		}
		else
		{
			continue;
		}

		if (!best || SV_Bot_Distance (ent->v.origin, self->v.origin) < best_dist)
		{
			best = ent;
			best_dist = SV_Bot_Distance (ent->v.origin, self->v.origin);
		}
	}

	return best;
}

static bot_priority_t SV_Bot_EvaluateNeeds (const edict_t *self, const bot_skill_settings_t *settings)
{
	qboolean needs_weapon = false;
	qboolean needs_health = false;
	qboolean needs_armor = false;
	int items = (int)self->v.items;
	int weapon = (int)self->v.weapon;

	if (settings && !settings->behaviors.allow_grab_items)
		return BOT_PRIORITY_NONE;

	if (!(items & (IT_SUPER_SHOTGUN | IT_NAILGUN | IT_SUPER_NAILGUN | IT_GRENADE_LAUNCHER | IT_ROCKET_LAUNCHER | IT_LIGHTNING)))
		needs_weapon = true;
	if (weapon == IT_AXE || weapon == IT_SHOTGUN)
		needs_weapon = true;

	if (self->v.health < 80)
		needs_health = true;

	if (self->v.armorvalue < 100 || self->v.armortype < 0.3f)
		needs_armor = true;

	if (needs_weapon)
		return BOT_PRIORITY_WEAPON;
	if (needs_health)
		return BOT_PRIORITY_HEALTH;
	if (needs_armor)
		return BOT_PRIORITY_ARMOR;
	return BOT_PRIORITY_NONE;
}

static qboolean SV_Bot_GoalStillValid (edict_t *goal, bot_priority_t priority, const edict_t *self)
{
        const char *classname;

	if (!goal || goal->free || goal->v.solid == SOLID_NOT)
		return false;
	if (!goal->v.classname)
		return false;

	classname = PR_GetString (goal->v.classname);
	if (!classname[0])
		return false;

        switch (priority)
        {
        case BOT_PRIORITY_WEAPON:
                return true;
        case BOT_PRIORITY_HEALTH:
                return !q_strcasecmp (classname, "item_health");
        case BOT_PRIORITY_ARMOR:
                return !q_strcasecmp (classname, "item_armor1") || !q_strcasecmp (classname, "item_armor2") || !q_strcasecmp (classname, "item_armorInv");
        case BOT_PRIORITY_ENEMY:
                return !goal->free && goal->v.takedamage;
        default:
                break;
        }

	return false;
}

static void SV_Bot_UpdateGoal (edict_t *self, sv_bot_state_t *state, bot_priority_t priority, edict_t *enemy, double now)
{
        edict_t *goal;

        if (!state)
                return;

        if (priority == BOT_PRIORITY_NONE)
        {
                state->goal_edict = NULL;
                state->goal_priority = BOT_PRIORITY_NONE;
                state->goal_best_dist = 0.0f;
                SV_Bot_ClearPath (state);
                return;
        }

        if (priority == BOT_PRIORITY_ENEMY)
        {
                if (!enemy || enemy->free)
                {
                        state->goal_edict = NULL;
                        state->goal_priority = BOT_PRIORITY_NONE;
                        state->goal_best_dist = 0.0f;
                        SV_Bot_ClearPath (state);
                        return;
                }

                goal = enemy;
                if (state->goal_priority != BOT_PRIORITY_ENEMY || state->goal_edict != goal)
                {
                        state->goal_edict = goal;
                        state->goal_priority = BOT_PRIORITY_ENEMY;
                        state->goal_best_dist = SV_Bot_Distance (goal->v.origin, self->v.origin);
                        state->goal_last_progress_time = now;
                        SV_Bot_ClearPath (state);
                }
                if (state->path_length <= 0 || now >= state->next_path_recalc_time)
                        SV_Bot_RecalculatePath (self, state, now);
                return;
        }

        if (state->goal_priority != priority || !SV_Bot_GoalStillValid (state->goal_edict, priority, self))
        {
                goal = SV_Bot_FindGoal (self, priority);
                state->goal_edict = goal;
        state->goal_priority = priority;
        SV_Bot_ClearPath (state);
        if (goal)
        {
        state->goal_best_dist = SV_Bot_Distance (goal->v.origin, self->v.origin);
        state->goal_last_progress_time = now;
        SV_Bot_RecalculatePath (self, state, now);
        }
        else
        {
        state->goal_best_dist = 0.0f;
        }
        }
        else if (state->goal_edict && state->path_length <= 0 && now >= state->next_path_recalc_time)
        {
        SV_Bot_RecalculatePath (self, state, now);
        }
}

static void SV_Bot_CheckGoalProgress (edict_t *self, sv_bot_state_t *state, double now)
{
        float dist;

        if (!state || !state->goal_edict)
                return;

        if (state->goal_priority == BOT_PRIORITY_ENEMY)
                return;

        if (!SV_Bot_GoalStillValid (state->goal_edict, state->goal_priority, self))
        {
                state->goal_edict = NULL;
                state->goal_priority = BOT_PRIORITY_NONE;
                SV_Bot_ClearPath (state);
                return;
        }

        dist = SV_Bot_Distance (state->goal_edict->v.origin, self->v.origin);
        if (dist < 48.0f)
        {
                state->goal_edict = NULL;
                state->goal_priority = BOT_PRIORITY_NONE;
                state->goal_best_dist = 0.0f;
                SV_Bot_ClearPath (state);
                return;
        }

        if (dist < state->goal_best_dist - 16.0f)
        {
                state->goal_best_dist = dist;
                state->goal_last_progress_time = now;
        }
        else if (now - state->goal_last_progress_time > 3.0)
        {
                state->goal_edict = NULL;
                state->goal_priority = BOT_PRIORITY_NONE;
                state->next_wander_time = 0.0;
                SV_Bot_ClearPath (state);
        }
}

static qboolean SV_Bot_CanSeeTarget (edict_t *self, edict_t *target)
{
	vec3_t start, end;
	trace_t trace;

	if (!self || !target)
		return false;

	VectorCopy (self->v.origin, start);
	start[2] += self->v.view_ofs[2];
	VectorCopy (target->v.origin, end);
	end[2] += target->v.view_ofs[2];

	trace = SV_Move (start, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, self);
	return trace.fraction >= 0.99f || trace.ent == target;
}

static void SV_Bot_MaybeChat (client_t *client, sv_bot_state_t *state)
{
	double now;
	int line;

	if (!client || !state || !state->profile || state->profile->chat_count <= 0)
		return;
	if (!client->edict || client->edict->v.health <= 0)
		return;

	now = qcvm->time;
	if (now < state->next_chat_time)
		return;
	if (SV_Bot_Frand () > 0.1f)
	{
		state->next_chat_time = now + 5.0 + SV_Bot_Frand () * 10.0;
		return;
	}

	line = rand () % state->profile->chat_count;
	SV_BroadcastPrintf ("%s: %s\n", client->name, state->profile->chat_lines[line]);
	state->next_chat_time = now + 20.0 + SV_Bot_Frand () * 25.0;
}

static sv_bot_state_t *SV_BotStateForClient (const client_t *client)
{
	ptrdiff_t index = client - svs.clients;
	if (index < 0 || index >= MAX_SCOREBOARD)
		return NULL;
	return &sv_bot_states[index];
}

static void SV_Bot_RefreshSettingsForState (sv_bot_state_t *state)
{
        const bot_skill_settings_t *desired;

        if (!state)
                return;

        desired = SV_Bot_GetSkillSettings (sv_bot_difficulty.string);
        if (!desired && bot_skill_settings_count > 0)
                desired = &bot_skill_settings[0];

        if (state->settings != desired)
        {
                state->settings = desired;
                state->aim_initialized = false;
                VectorClear (state->aim_velocity);
                state->aim_modifier_end_time = 0.0;
        }
}

int SV_Bot_GetDebugWaypoints (bot_debug_waypoint_t *out, int max_waypoints)
{
        int i, j;
        int count = 0;
        int bot_count = 0;

        if (!out || max_waypoints <= 0 || !sv.active)
                return 0;

        SV_Bot_EnsureWaypoints ();

        for (i = 0; i < svs.maxclients; i++)
        {
                client_t *bot = &svs.clients[i];
                if (!bot->active || !bot->spawned || !bot->isbot || !bot->edict || bot->edict->free)
                        continue;

                bot_count++;
        }

        for (i = 0; i < bot_waypoint_count && count < max_waypoints; i++)
        {
                edict_t *wp = bot_waypoints[i].edict;
                bot_debug_waypoint_t *info;
                vec3_t extents;
                float radius = 0.0f;
                qboolean reachable = (bot_count == 0);

                if (!wp || wp->free)
                        continue;

                info = &out[count];
                SV_Bot_GetNodePosition (wp, info->origin);
                VectorSubtract (wp->v.maxs, wp->v.mins, extents);

                for (j = 0; j < 3; j++)
                {
                        float extent_radius = fabsf (extents[j]) * 0.5f;
                        if (extent_radius > radius)
                                radius = extent_radius;
                }

                info->radius = (radius > 0.0f) ? radius : 16.0f;
                info->is_target = false;

                for (j = 0; j < svs.maxclients; j++)
                {
                        client_t *bot = &svs.clients[j];
                        sv_bot_state_t *state = &sv_bot_states[j];

                        if (!bot->active || !bot->spawned || !bot->isbot || !bot->edict || bot->edict->free)
                                continue;
                        if (!state || !state->active)
                                continue;

                        if (state->goal_edict == wp)
                                info->is_target = true;

                        if (!reachable && SV_Bot_WaypointReachable (bot, wp))
                                reachable = true;
                }

                info->unreachable = !reachable;
                count++;
        }

        return count;
}

void SV_Bot_Reset (void)
{
        int i;
        SV_Bot_FreeNavMesh ();
        memset (sv_bot_states, 0, sizeof(sv_bot_states));
        for (i = 0; i < (int)(sizeof(sv_bot_states) / sizeof(sv_bot_states[0])); i++)
        {
                sv_bot_states[i].enemy_slot = -1;
        }
        bot_name_counter = 1;
        SV_Bot_ResetProfiles ();
        bot_waypoint_count = 0;
        bot_waypoints_initialized = false;
}

static void SV_Bot_PickWanderDirection (sv_bot_state_t *state)
{
        float yaw = (float)(rand () % 360);
        float radians = yaw * (float)M_PI / 180.0f;
        vec3_t dir = { (float)cos (radians), (float)sin (radians), 0.0f };

	VectorNormalize (dir);
	VectorCopy (dir, state->wander_dir);
	state->wander_yaw = yaw;
        state->next_wander_time = qcvm->time + 2.0 + ((float)(rand () & 255) / 255.0f) * 4.0f;
}

static void SV_Bot_CheckForObstacles (edict_t *self, sv_bot_state_t *state, const bot_path_node_t *move_target, double now)
{
        vec3_t dir;
        vec3_t start, end;
        float length;
        trace_t trace;

        if (!self || !state)
                return;

        if (now < state->next_wall_check_time)
                return;

        if (move_target)
        {
                vec3_t target_origin;
                SV_Bot_PathNodeOrigin (move_target, target_origin);
                VectorSubtract (target_origin, self->v.origin, dir);
        }
        else
        {
                VectorCopy (state->wander_dir, dir);
        }

        length = VectorNormalize (dir);
        if (length <= 0.0f)
        {
                state->next_wall_check_time = now + BOT_WALL_CHECK_INTERVAL;
                return;
        }

        VectorCopy (self->v.origin, start);
        start[2] += (self->v.view_ofs[2] != 0.0f) ? self->v.view_ofs[2] : 16.0f;
        VectorMA (start, 48.0f, dir, end);

        trace = SV_Move (start, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, self);
        state->next_wall_check_time = now + BOT_WALL_CHECK_INTERVAL;

        if (trace.fraction < 0.3f)
        {
                if (state->goal_edict)
                {
                        state->next_path_recalc_time = now;
                        SV_Bot_RecalculatePath (self, state, now);
                }
                else
                {
                        SV_Bot_PickWanderDirection (state);
                }
        }
}

static qboolean SV_Bot_NameExists (const char *name)
{
        int i;

        for (i = 0; i < svs.maxclients; i++)
	{
		if (!svs.clients[i].active)
			continue;
		if (q_strcasecmp (svs.clients[i].name, name) == 0)
			return true;
	}

	return false;
}

static void SV_Bot_AssignName (client_t *client, const char *preferred)
{
        char name[sizeof(client->name)];
        int attempts = 0;

        if (preferred && *preferred)
        {
                q_strlcpy (name, preferred, sizeof(name));
                if (!SV_Bot_NameExists (name))
                {
                        q_strlcpy (client->name, name, sizeof(client->name));
                        return;
                }

                for (attempts = 1; attempts < 32; attempts++)
                {
                        q_snprintf (name, sizeof(name), "%s_%d", preferred, attempts + 1);
                        if (!SV_Bot_NameExists (name))
                        {
                                q_strlcpy (client->name, name, sizeof(client->name));
                                return;
                        }
                }
        }

        attempts = 0;
        do
        {
                q_snprintf (name, sizeof(name), "Bot%d", bot_name_counter++);
                if (bot_name_counter > 9999)
                        bot_name_counter = 1;
        } while (SV_Bot_NameExists (name) && attempts++ < 10000);

        q_strlcpy (client->name, name, sizeof(client->name));
}

static client_t *SV_Bot_FindTarget (client_t *bot, sv_bot_state_t *state, qboolean prefer_players)
{
	client_t *best = NULL;
	float best_dist = 0.0f;
	int i;
	edict_t *self = bot->edict;
	const bot_skill_settings_t *settings = state ? state->settings : NULL;
	float fov = settings ? settings->senses.fov_angle : 180.0f;
	float half_fov = CLAMP (0.0f, fov * 0.5f, 179.0f);
	float cos_half_fov = (float)cos (half_fov * (float)M_PI / 180.0f);
	float sound_range = settings ? settings->senses.sound_range : 0.0f;
	vec3_t forward;

	if (!self)
		return NULL;

	AngleVectors (self->v.v_angle, forward, NULL, NULL);

	for (i = 0; i < svs.maxclients; i++)
	{
		client_t *candidate = &svs.clients[i];
		vec3_t delta;
		float dist;
		vec3_t dir;
		float dot;
		qboolean in_fov = true;

		if (!candidate->active || !candidate->spawned)
			continue;
		if (candidate == bot)
			continue;
		if (candidate->edict->free)
			continue;
		if (candidate->edict->v.health <= 0)
			continue;
		if (prefer_players && candidate->isbot)
			continue;

		VectorSubtract (candidate->edict->v.origin, self->v.origin, delta);
		dist = VectorLength (delta);

		if (dist <= 0.0f)
			continue;

		VectorScale (delta, 1.0f / dist, dir);
		dot = DotProduct (forward, dir);
		if (cos_half_fov > -0.99f)
			in_fov = dot >= cos_half_fov;

		if (!in_fov)
		{
			if (sound_range <= 0.0f || dist > sound_range)
				continue;
		}

		if (!best || dist < best_dist)
		{
			best = candidate;
			best_dist = dist;
		}
	}

	return best;
}

static void SV_Bot_UpdateMovement (client_t *client, sv_bot_state_t *state)
{
	edict_t *self = client->edict;
	usercmd_t *cmd = &client->cmd;
	client_t *enemy;
	double now = qcvm->time;
	float skill = state ? state->skill : 1.0f;
	const bot_skill_settings_t *settings = state ? state->settings : NULL;
        bot_priority_t priority = BOT_PRIORITY_NONE;
        edict_t *goal = NULL;
        const bot_path_node_t *move_target = NULL;
        bot_path_node_t goal_node;
        vec3_t delta;
        vec3_t base_angles;
        qboolean have_base_angles = false;

        SV_Bot_InitPathNode (&goal_node);

        if (!self || self->free)
                return;

        enemy = SV_Bot_FindTarget (client, state, true);
        if (!enemy)
                enemy = SV_Bot_FindTarget (client, state, false);

        SV_Bot_SelectBestWeapon (self, enemy ? enemy->edict : NULL);

        memset (cmd, 0, sizeof(*cmd));
        self->v.button0 = 0;
        self->v.button2 = 0;

        if (state)
        {
                if (now - state->last_stuck_time > 1.0)
                {
                        VectorSubtract (self->v.origin, state->last_position, delta);
			if (VectorLength (delta) < 16.0f)
			{
				state->next_wander_time = 0.0;
				if (state->goal_edict)
					state->goal_last_progress_time = now - 10.0;
			}
			VectorCopy (self->v.origin, state->last_position);
			state->last_stuck_time = now;
		}
	}

        if (state)
        {
                priority = SV_Bot_EvaluateNeeds (self, settings);
                if (enemy && settings && !settings->behaviors.allow_grab_items_in_combat)
                        priority = BOT_PRIORITY_NONE;
                if (priority == BOT_PRIORITY_NONE && enemy)
                        priority = BOT_PRIORITY_ENEMY;

                SV_Bot_UpdateGoal (self, state, priority, enemy ? enemy->edict : NULL, now);
                SV_Bot_CheckGoalProgress (self, state, now);
                goal = state->goal_edict;

                if (!goal && state->next_wander_time <= now)
                        SV_Bot_PickWanderDirection (state);
        }

        if (state)
                SV_Bot_UpdatePathProgress (self, state);

        if (goal && state)
        {
                if (state->path_length > 0 && state->path_index < state->path_length)
                {
                        const bot_path_node_t *candidate = &state->path_nodes[state->path_index];

                        if (candidate->edict && candidate->edict->free)
                        {
                                state->next_path_recalc_time = now;
                        }
                        else
                        {
                                move_target = candidate;
                        }

                        if (!move_target && now >= state->next_path_recalc_time)
                        {
                                if (SV_Bot_RecalculatePath (self, state, now) && state->path_length > 0 && state->path_index < state->path_length)
                                        move_target = &state->path_nodes[state->path_index];
                        }
                }
                else if (now >= state->next_path_recalc_time)
                {
                        if (SV_Bot_RecalculatePath (self, state, now) && state->path_length > 0 && state->path_index < state->path_length)
                                move_target = &state->path_nodes[state->path_index];
                }
        }

        if (!move_target && goal)
        {
                SV_Bot_PathNodeFromEdict (&goal_node, goal);
                move_target = &goal_node;
        }

        if (move_target)
        {
                vec3_t dir;
                float dist;
                vec3_t target_origin;

                SV_Bot_PathNodeOrigin (move_target, target_origin);
                VectorSubtract (target_origin, self->v.origin, dir);
                dist = VectorLength (dir);
                if (dist > 0)
                        VectorScale (dir, 1.0f / dist, dir);
                else
                        VectorClear (dir);

                VectorAngles (dir, base_angles);
                base_angles[PITCH] = 0.0f;
                have_base_angles = true;

                cmd->forwardmove = SV_Bot_ClampMoveValue (160.0f + 60.0f * (skill - 1.0f), settings);
                if (dist > 120.0f)
                        cmd->sidemove = SV_Bot_ClampMoveValue ((SV_Bot_Frand () > 0.5f) ? 120.0f : -120.0f, settings);
        }
        else if (state)
        {
                VectorAngles (state->wander_dir, base_angles);
		have_base_angles = true;
		cmd->forwardmove = SV_Bot_ClampMoveValue (150.0f + 50.0f * skill, settings);
		if (SV_Bot_Frand () > 0.5f)
			cmd->sidemove = SV_Bot_ClampMoveValue ((SV_Bot_Frand () > 0.5f) ? 80.0f : -80.0f, settings);
	}

	if (enemy)
	{
		vec3_t target_origin;
		vec3_t dir;
		vec3_t desired_angles;
		float dist;
		qboolean can_shoot;
		float attack_range = 420.0f + skill * 220.0f;
		float weapon_fov = settings ? settings->weapons.fov_angle : 45.0f;
		qboolean aim_aligned;
		double sight_delay = settings ? settings->weapons.sight_time : 0.2f;
		double decay_time = settings ? settings->weapons.decay_time : 1.0f;
		double time_since_visible = 0.0;
		qboolean allow_attack = true;

		VectorCopy (enemy->edict->v.origin, target_origin);
		if (settings)
			VectorMA (target_origin, settings->aiming.velocity_offset, enemy->edict->v.velocity, target_origin);

		VectorSubtract (target_origin, self->v.origin, dir);
		dist = VectorLength (dir);
		if (dist > 0)
			VectorScale (dir, 1.0f / dist, dir);
		else
			VectorClear (dir);

		VectorAngles (dir, desired_angles);
		desired_angles[PITCH] = CLAMP (-60.0f, desired_angles[PITCH], 60.0f);

		if (state)
		{
			int enemy_index = (int)(enemy - svs.clients);
			if (state->enemy_slot != enemy_index)
			{
				state->enemy_slot = enemy_index;
				state->enemy_first_seen_time = now;
				state->enemy_last_visible_time = now;
			}
		}

		SV_Bot_UpdateAimAngles (self, state, desired_angles, now);

		can_shoot = SV_Bot_CanSeeTarget (self, enemy->edict);
		if (state && can_shoot)
			state->enemy_last_visible_time = now;

		if (state)
			time_since_visible = now - state->enemy_last_visible_time;

		if (state && settings && settings->senses.forget_non_vis_enemy_time > 0.0f && time_since_visible > settings->senses.forget_non_vis_enemy_time)
			state->enemy_first_seen_time = now;

		if (priority != BOT_PRIORITY_NONE)
		{
			if (dist < 200.0f)
			{
				cmd->forwardmove = SV_Bot_ClampMoveValue (-220.0f, settings);
				cmd->sidemove = SV_Bot_ClampMoveValue ((SV_Bot_Frand () > 0.5f) ? 200.0f : -200.0f, settings);
				if (can_shoot && dist < 180.0f)
					self->v.button0 = 1;
			}
		}
		else
		{
			float yaw_delta;
			float pitch_delta;

			if (settings && !settings->behaviors.allow_combat)
				attack_range *= 0.7f;

			if (!goal)
			{
				if (dist > 160.0f)
					cmd->forwardmove = SV_Bot_ClampMoveValue (220.0f, settings);
				else if (dist < 90.0f)
					cmd->forwardmove = SV_Bot_ClampMoveValue (-150.0f, settings);
				if (dist > 80.0f)
					cmd->sidemove = SV_Bot_ClampMoveValue ((SV_Bot_Frand () > 0.5f) ? 140.0f : -140.0f, settings);
			}

			yaw_delta = fabsf (AngleDifference (desired_angles[YAW], self->v.v_angle[YAW]));
			pitch_delta = fabsf (AngleDifference (desired_angles[PITCH], self->v.v_angle[PITCH]));
			aim_aligned = yaw_delta <= weapon_fov * 0.5f && pitch_delta <= weapon_fov * 0.5f;

			if (state)
			{
				double seen_time = now - state->enemy_first_seen_time;

				allow_attack = seen_time >= sight_delay;
				if (!can_shoot && time_since_visible > decay_time)
					allow_attack = false;

				if (settings && !settings->behaviors.allow_melee)
				{
					int weapon_id = (int)self->v.weapon;
					if (weapon_id == IT_AXE)
						allow_attack = false;
				}

				if (allow_attack && aim_aligned && ((can_shoot && dist < attack_range) || (!can_shoot && time_since_visible <= decay_time)))
					self->v.button0 = 1;
			}
			else if (aim_aligned && can_shoot && dist < attack_range)
			{
				self->v.button0 = 1;
			}
		}

		if (state && settings && settings->movement.allow_jumping_in_combat && now >= state->next_jump_check_time)
		{
			if (settings->movement.jump_chance > 0.0f && SV_Bot_Frand () * 100.0f < settings->movement.jump_chance)
				cmd->upmove = 200.0f;
			state->next_jump_check_time = now + ((settings->movement.jump_cooldown > 0.0f) ? settings->movement.jump_cooldown : 0.5f);
		}

                if (settings && !settings->behaviors.allow_combat)
                {
                        cmd->sidemove = SV_Bot_ClampMoveValue (cmd->sidemove * 0.5f, settings);
                        cmd->forwardmove = SV_Bot_ClampMoveValue (cmd->forwardmove * 0.5f, settings);
                }
        }
        else if (have_base_angles)
        {
                SV_Bot_UpdateAimAngles (self, state, base_angles, now);
        }

        if (state)
                SV_Bot_CheckForObstacles (self, state, move_target, now);

        if (!enemy && state)
                state->enemy_slot = -1;

	if (self->v.waterlevel >= 2)
		cmd->upmove = 200.0f;

	VectorCopy (self->v.v_angle, cmd->viewangles);
}


static qboolean SV_Bot_SpawnOne (void)
{
        client_t *client;
        sv_bot_state_t *state;
        const bot_profile_t *profile = NULL;
        client_t *saved_client;
        edict_t *saved_player;
        cmd_source_t saved_source;
        qcvm_t *oldqcvm = NULL;
        qboolean pushed_vm = false;
	qboolean success = false;
	int slot;

	if (!sv.active || sv.state != ss_active)
	{
		Con_Printf ("Cannot spawn bot: server is not active.\n");
		goto cleanup;
	}

	for (slot = 0; slot < svs.maxclients; slot++)
	{
		if (!svs.clients[slot].active)
			break;
	}

	if (slot == svs.maxclients)
	{
		Con_Printf ("No free client slots for bot.\n");
		goto cleanup;
	}

	PR_PushQCVM (&sv.qcvm, &oldqcvm);
	pushed_vm = true;

	client = &svs.clients[slot];
        client->netconnection = NULL;

        SV_ConnectClient (slot);
        client->isbot = true;
        client->dropasap = false;

        profile = SV_Bot_SelectProfile ();

        if (profile)
        {
                int topcolor = CLAMP (0, profile->topcolor, 15);
                int bottomcolor = CLAMP (0, profile->bottomcolor, 15);
                client->colors = ((topcolor & 15) << 4) | (bottomcolor & 15);
                if (client->edict)
                        client->edict->v.skin = profile->skin;
        }
        else
        {
                client->colors = ((rand () & 15) << 4) | (rand () & 15);
        }

        SV_Bot_AssignName (client, profile ? profile->name : NULL);

	saved_client = host_client;
	saved_player = sv_player;
	saved_source = cmd_source;

	host_client = client;
	sv_player = client->edict;
	cmd_source = src_client;

	Cmd_ExecuteString ("prespawn", src_client);
	Cmd_ExecuteString ("spawn", src_client);
	Cmd_ExecuteString ("begin", src_client);

	host_client = saved_client;
	sv_player = saved_player;
	cmd_source = saved_source;

	client->spawned = true;
	client->sendsignon = PRESPAWN_DONE;
	client->last_message = realtime;
	SZ_Clear (&client->message);

	state = SV_BotStateForClient (client);
        if (state)
        {
                memset (state, 0, sizeof(*state));
                state->active = true;
                state->profile = profile;
                state->skill = profile ? CLAMP (0.5f, profile->skill, 2.0f) : 1.0f;
                state->next_chat_time = qcvm->time + 10.0 + SV_Bot_Frand () * 10.0;
                VectorCopy (client->edict->v.origin, state->last_position);
                state->last_stuck_time = qcvm->time;
                state->next_wander_time = 0.0;
                state->enemy_slot = -1;
                SV_Bot_RefreshSettingsForState (state);
                if (!state->settings && bot_skill_settings_count > 0)
                        state->settings = &bot_skill_settings[0];
                VectorClear (state->aim_velocity);
                state->aim_modifier_end_time = 0.0;
                state->aim_initialized = false;
                state->next_jump_check_time = 0.0;
        }

	MSG_WriteByte (&sv.reliable_datagram, svc_updatename);
	MSG_WriteByte (&sv.reliable_datagram, slot);
	MSG_WriteString (&sv.reliable_datagram, client->name);
	MSG_WriteByte (&sv.reliable_datagram, svc_updatecolors);
	MSG_WriteByte (&sv.reliable_datagram, slot);
	MSG_WriteByte (&sv.reliable_datagram, client->colors);

	SV_BroadcastPrintf ("%s has spawned.\n", client->name);

	success = true;

cleanup:
	if (pushed_vm)
		PR_PopQCVM (oldqcvm);

	return success;
}

static qboolean SV_Bot_RemoveOne (void)
{
	int slot;
	client_t *client;
	client_t *saved_client;

	for (slot = svs.maxclients - 1; slot >= 0; slot--)
	{
		if (svs.clients[slot].active && svs.clients[slot].isbot)
			break;
	}

	if (slot < 0)
	{
		Con_Printf ("No bots to remove.\n");
		return false;
	}

	client = &svs.clients[slot];
	saved_client = host_client;
	host_client = client;

	SV_Bot_ClientDisconnected (client);
	SV_DropClient (false);

	host_client = saved_client;
	return true;
}

static void SV_BotSpawn_cvar (cvar_t *var)
{
	int count = (int)var->value;
	int spawned = 0;

	if (count <= 0)
		return;

	while (spawned < count)
	{
		if (!SV_Bot_SpawnOne ())
			break;
		spawned++;
	}

	if (spawned)
		Con_Printf ("Spawned %d bot(s).\n", spawned);

	Cvar_SetValueQuick (var, 0.0f);
}

static void SV_BotRemove_cvar (cvar_t *var)
{
	int count = (int)var->value;
	int removed = 0;

	if (count <= 0)
		return;

	while (removed < count)
	{
		if (!SV_Bot_RemoveOne ())
			break;
		removed++;
	}

	if (removed)
		Con_Printf ("Removed %d bot(s).\n", removed);

	Cvar_SetValueQuick (var, 0.0f);
}

void SV_Bot_Init (void)
{
	Cvar_RegisterVariable (&sv_bot_spawn);
	Cvar_RegisterVariable (&sv_bot_remove);
	Cvar_RegisterVariable (&sv_bot_difficulty);
	Cvar_SetCallback (&sv_bot_spawn, SV_BotSpawn_cvar);
	Cvar_SetCallback (&sv_bot_remove, SV_BotRemove_cvar);
	SV_Bot_LoadSkillSettings ();
	SV_Bot_Reset ();
}

void SV_Bot_RunFrame (client_t *client)
{
	sv_bot_state_t *state;

	if (!client || !client->isbot)
		return;

        state = SV_BotStateForClient (client);
        if (!client->spawned)
                return;

        if (state)
                SV_Bot_RefreshSettingsForState (state);

        SV_Bot_UpdateMovement (client, state);
        SV_Bot_MaybeChat (client, state);
}

void SV_Bot_ClientDisconnected (client_t *client)
{
	sv_bot_state_t *state;

	if (!client)
		return;

	state = SV_BotStateForClient (client);
	if (state)
		memset (state, 0, sizeof(*state));
	client->isbot = false;
}
