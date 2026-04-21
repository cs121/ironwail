#include "quakedef.h"
#include "bot_nav2.h"

#define BOT_NAV_MAX_NODES 8192
#define BOT_NAV_MAX_LINKS 65536
#define BOT_NAV_MAX_TRAVERSALS 65536
#define BOT_NAV_INF 1.0e30f

#define BOT_NAV2_MAGIC "NAV2"
#define BOT_NAV2_VERSION 15

typedef struct nav2_header_s
{
	char	magic[4];
	int	version;
	int	node_count;
	int	link_count;
	int	traversal_count;
} nav2_header_t;

typedef struct nav2_node_s
{
	int	flags;
	int	link_count;
	int	first_link;
	float	radius;
} nav2_node_t;

typedef struct nav2_link_s
{
	int	target_node;
	short	type;
	short	traversal_index;
} nav2_link_t;

typedef struct nav2_traversal_s
{
	int	type;
	int	param0;
	int	param1;
	int	param2;
} nav2_traversal_t;

typedef struct nav2_reader_s
{
	const byte	*data;
	size_t	len;
	size_t	ofs;
} nav2_reader_t;

typedef struct bot_nav_node_s
{
	vec3_t	pos;
	int		first_link;
	int		link_count;
	int		flags;
	float	radius;
} bot_nav_node_t;

typedef struct bot_nav_link_s
{
	int		to;
	int		next;
	float	cost;
	int		type;
	int		traversal_index;
} bot_nav_link_t;

typedef struct bot_nav_graph_s
{
	qboolean	loaded;
	char		mapname[64];
	int		version;
	int		node_count;
	int		link_count;
	int		traversal_count;
	int		edict_ref_count;
	bot_nav_node_t	nodes[BOT_NAV_MAX_NODES];
	bot_nav_link_t	links[BOT_NAV_MAX_LINKS];

	uint32_t	search_gen;
	uint32_t	visited_gen[BOT_NAV_MAX_NODES];
	uint32_t	closed_gen[BOT_NAV_MAX_NODES];
	float		gscore[BOT_NAV_MAX_NODES];
	float		fscore[BOT_NAV_MAX_NODES];
	int		came_from[BOT_NAV_MAX_NODES];
	int		open_heap[BOT_NAV_MAX_NODES];
	int		open_heap_size;
	int		open_heap_index[BOT_NAV_MAX_NODES];

	uint32_t	debug_search_calls;
	uint32_t	debug_expanded_total;
	uint32_t	debug_expanded_last;
} bot_nav_graph_t;

static bot_nav_graph_t g_bot_nav;

extern cvar_t bot_nav_debug;

static qboolean BotNav_ReadBytes (nav2_reader_t *r, void *out, size_t size)
{
	if (!r || !out)
		return false;
	if (size > r->len - r->ofs)
		return false;
	Q_memcpy (out, r->data + r->ofs, size);
	r->ofs += size;
	return true;
}

static qboolean BotNav_ReadU32Raw (nav2_reader_t *r, uint32_t *out)
{
	uint32_t value;

	if (!BotNav_ReadBytes (r, &value, sizeof (value)))
		return false;
	*out = (uint32_t) LittleLong ((int) value);
	return true;
}

static qboolean BotNav_ReadS32Raw (nav2_reader_t *r, int *out)
{
	uint32_t value;

	if (!BotNav_ReadU32Raw (r, &value))
		return false;
	*out = (int) value;
	return true;
}

static qboolean BotNav_ReadF32Raw (nav2_reader_t *r, float *out)
{
	union
	{
		uint32_t u;
		float f;
	} value;

	if (!BotNav_ReadU32Raw (r, &value.u))
		return false;
	*out = value.f;
	return true;
}

static qboolean BotNav_ReadS16Raw (nav2_reader_t *r, short *out)
{
	short value;

	if (!BotNav_ReadBytes (r, &value, sizeof (value)))
		return false;
	*out = (short) LittleShort ((short) value);
	return true;
}

static qboolean BotNav_ReadHeader (nav2_reader_t *r, nav2_header_t *header)
{
	if (!r || !header)
		return false;
	if (!BotNav_ReadBytes (r, header->magic, sizeof (header->magic)))
		return false;
	if (!BotNav_ReadS32Raw (r, &header->version))
		return false;
	if (!BotNav_ReadS32Raw (r, &header->node_count))
		return false;
	if (!BotNav_ReadS32Raw (r, &header->link_count))
		return false;
	if (!BotNav_ReadS32Raw (r, &header->traversal_count))
		return false;
	return true;
}

static void BotNav_ResetGraph (void)
{
	int i;

	g_bot_nav.loaded = false;
	g_bot_nav.mapname[0] = '\0';
	g_bot_nav.version = 0;
	g_bot_nav.node_count = 0;
	g_bot_nav.link_count = 0;
	g_bot_nav.traversal_count = 0;
	g_bot_nav.edict_ref_count = 0;
	g_bot_nav.search_gen = 0;
	g_bot_nav.open_heap_size = 0;
	g_bot_nav.debug_search_calls = 0;
	g_bot_nav.debug_expanded_total = 0;
	g_bot_nav.debug_expanded_last = 0;

	for (i = 0; i < BOT_NAV_MAX_NODES; ++i)
	{
		g_bot_nav.nodes[i].first_link = -1;
		g_bot_nav.open_heap_index[i] = -1;
	}
}

static qboolean BotNav_OpenHeapNodeLess (int a, int b)
{
	if (g_bot_nav.fscore[a] < g_bot_nav.fscore[b])
		return true;
	if (g_bot_nav.fscore[a] > g_bot_nav.fscore[b])
		return false;
	if (g_bot_nav.gscore[a] < g_bot_nav.gscore[b])
		return true;
	if (g_bot_nav.gscore[a] > g_bot_nav.gscore[b])
		return false;
	return a < b;
}

static void BotNav_OpenHeapSwap (int a, int b)
{
	int tmp = g_bot_nav.open_heap[a];
	g_bot_nav.open_heap[a] = g_bot_nav.open_heap[b];
	g_bot_nav.open_heap[b] = tmp;
	g_bot_nav.open_heap_index[g_bot_nav.open_heap[a]] = a;
	g_bot_nav.open_heap_index[g_bot_nav.open_heap[b]] = b;
}

static void BotNav_OpenHeapSiftUp (int idx)
{
	while (idx > 0)
	{
		int parent = (idx - 1) >> 1;
		if (!BotNav_OpenHeapNodeLess (g_bot_nav.open_heap[idx], g_bot_nav.open_heap[parent]))
			break;
		BotNav_OpenHeapSwap (idx, parent);
		idx = parent;
	}
}

static void BotNav_OpenHeapSiftDown (int idx)
{
	for (;;)
	{
		int left = (idx << 1) + 1;
		int right = left + 1;
		int best = idx;

		if (left < g_bot_nav.open_heap_size
			&& BotNav_OpenHeapNodeLess (g_bot_nav.open_heap[left], g_bot_nav.open_heap[best]))
			best = left;
		if (right < g_bot_nav.open_heap_size
			&& BotNav_OpenHeapNodeLess (g_bot_nav.open_heap[right], g_bot_nav.open_heap[best]))
			best = right;
		if (best == idx)
			break;
		BotNav_OpenHeapSwap (idx, best);
		idx = best;
	}
}

static void BotNav_OpenHeapPushOrUpdate (int node)
{
	int idx = g_bot_nav.open_heap_index[node];

	if (idx < 0)
	{
		if (g_bot_nav.open_heap_size >= BOT_NAV_MAX_NODES)
			return;
		idx = g_bot_nav.open_heap_size++;
		g_bot_nav.open_heap[idx] = node;
		g_bot_nav.open_heap_index[node] = idx;
		BotNav_OpenHeapSiftUp (idx);
	}
	else
	{
		BotNav_OpenHeapSiftUp (idx);
		BotNav_OpenHeapSiftDown (idx);
	}
}

static int BotNav_OpenHeapPopMin (void)
{
	int min_node;

	if (g_bot_nav.open_heap_size <= 0)
		return -1;

	min_node = g_bot_nav.open_heap[0];
	g_bot_nav.open_heap_index[min_node] = -1;
	--g_bot_nav.open_heap_size;

	if (g_bot_nav.open_heap_size > 0)
	{
		g_bot_nav.open_heap[0] = g_bot_nav.open_heap[g_bot_nav.open_heap_size];
		g_bot_nav.open_heap_index[g_bot_nav.open_heap[0]] = 0;
		BotNav_OpenHeapSiftDown (0);
	}

	return min_node;
}

static uint32_t BotNav_BeginSearchGen (void)
{
	++g_bot_nav.search_gen;
	if (!g_bot_nav.search_gen)
	{
		Q_memset (g_bot_nav.visited_gen, 0, sizeof (g_bot_nav.visited_gen));
		Q_memset (g_bot_nav.closed_gen, 0, sizeof (g_bot_nav.closed_gen));
		g_bot_nav.search_gen = 1;
	}
	g_bot_nav.open_heap_size = 0;
	return g_bot_nav.search_gen;
}

static void BotNav_InitNodeForSearch (int node, uint32_t search_gen)
{
	if (g_bot_nav.visited_gen[node] == search_gen)
		return;

	g_bot_nav.visited_gen[node] = search_gen;
	g_bot_nav.closed_gen[node] = 0;
	g_bot_nav.gscore[node] = BOT_NAV_INF;
	g_bot_nav.fscore[node] = BOT_NAV_INF;
	g_bot_nav.came_from[node] = -1;
	g_bot_nav.open_heap_index[node] = -1;
}

static qboolean BotNav_LinkExists (int from, int to, int type, int traversal_index)
{
	int link_idx;

	for (link_idx = g_bot_nav.nodes[from].first_link; link_idx >= 0; link_idx = g_bot_nav.links[link_idx].next)
	{
		if (g_bot_nav.links[link_idx].to == to
			&& g_bot_nav.links[link_idx].type == type
			&& g_bot_nav.links[link_idx].traversal_index == traversal_index)
			return true;
	}

	return false;
}

static qboolean BotNav_AddDirectedLink (int from, int to, float cost, int type, int traversal_index)
{
	bot_nav_link_t *link;

	if ((unsigned int) from >= (unsigned int) g_bot_nav.node_count || (unsigned int) to >= (unsigned int) g_bot_nav.node_count)
		return false;

	if (from == to)
		return false;

	if (BotNav_LinkExists (from, to, type, traversal_index))
		return true;

	if (g_bot_nav.link_count >= BOT_NAV_MAX_LINKS)
		return false;

	if (cost <= 0.f)
	{
		vec3_t delta;
		VectorSubtract (g_bot_nav.nodes[to].pos, g_bot_nav.nodes[from].pos, delta);
		cost = VectorLength (delta);
		if (cost <= 0.f)
			cost = 1.f;
	}

	link = &g_bot_nav.links[g_bot_nav.link_count++];
	link->to = to;
	link->cost = cost;
	link->type = type;
	link->traversal_index = traversal_index;
	link->next = g_bot_nav.nodes[from].first_link;
	g_bot_nav.nodes[from].first_link = g_bot_nav.link_count - 1;

	return true;
}

static qboolean BotNav_AddNode (const vec3_t pos, int flags, float radius)
{
	bot_nav_node_t *node;

	if (g_bot_nav.node_count >= BOT_NAV_MAX_NODES)
		return false;

	node = &g_bot_nav.nodes[g_bot_nav.node_count++];
	VectorCopy (pos, node->pos);
	node->flags = flags;
	node->radius = radius;
	node->first_link = -1;
	node->link_count = 0;
	return true;
}

void BotNav_Shutdown (void)
{
	BotNav_ResetGraph ();
}

void BotNav_LoadForMap (const char *mapname)
{
	byte *data;
	nav2_reader_t reader;
	nav2_header_t header;
	nav2_node_t *nodes;
	vec3_t *origins;
	nav2_link_t *links;
	byte *link_used;
	int *edict_refs;
	size_t len;
	char path[MAX_QPATH];
	char loaded_path[MAX_QPATH];
	const char *candidates[] =
	{
		"maps/%s.nav2",
		"bots/navigation/%s.nav2",
		"bots/nav/%s.nav2"
	};
	int ci;
	int i;
	int j;
	int edict_ref_count;
	qboolean success;

	BotNav_ResetGraph ();

	if (!mapname || !mapname[0])
		return;

	data = NULL;
	loaded_path[0] = '\0';
	for (ci = 0; ci < (int) countof (candidates); ++ci)
	{
		q_snprintf (path, sizeof (path), candidates[ci], mapname);
		data = COM_LoadMallocFile (path, NULL);
		if (data)
		{
			q_strlcpy (loaded_path, path, sizeof (loaded_path));
			break;
		}
	}
	if (!data)
	{
		if (bot_nav_debug.value)
			Con_Printf ("BotNav: no nav2 found for %s\n", mapname);
		return;
	}

	len = (size_t) com_filesize;
	reader.data = data;
	reader.len = len;
	reader.ofs = 0;
	nodes = NULL;
	origins = NULL;
	links = NULL;
	link_used = NULL;
	edict_refs = NULL;
	edict_ref_count = 0;
	success = false;

	if (!BotNav_ReadHeader (&reader, &header))
		goto cleanup;
	if (memcmp (header.magic, BOT_NAV2_MAGIC, 4) != 0)
		goto cleanup;
	if (header.version != BOT_NAV2_VERSION)
		goto cleanup;
	if (header.node_count <= 0 || header.node_count > BOT_NAV_MAX_NODES)
		goto cleanup;
	if (header.link_count <= 0 || header.link_count > BOT_NAV_MAX_LINKS)
		goto cleanup;
	if (header.traversal_count < 0 || header.traversal_count > BOT_NAV_MAX_TRAVERSALS)
		goto cleanup;

	nodes = (nav2_node_t *) q_malloc ((size_t) header.node_count * sizeof (*nodes));
	origins = (vec3_t *) q_malloc ((size_t) header.node_count * sizeof (*origins));
	links = (nav2_link_t *) q_malloc ((size_t) header.link_count * sizeof (*links));
	link_used = (byte *) q_malloc ((size_t) header.link_count);
	if (!nodes || !origins || !links || !link_used)
		goto cleanup;
	Q_memset (link_used, 0, (size_t) header.link_count);

	for (i = 0; i < header.node_count; ++i)
	{
		if (!BotNav_ReadS32Raw (&reader, &nodes[i].flags)
			|| !BotNav_ReadS32Raw (&reader, &nodes[i].link_count)
			|| !BotNav_ReadS32Raw (&reader, &nodes[i].first_link)
			|| !BotNav_ReadF32Raw (&reader, &nodes[i].radius))
			goto cleanup;
		if (nodes[i].link_count < 0)
			goto cleanup;
		if (!(nodes[i].radius >= 0.f))
			goto cleanup;
	}

	for (i = 0; i < header.node_count; ++i)
	{
		if (!BotNav_ReadF32Raw (&reader, &origins[i][0])
			|| !BotNav_ReadF32Raw (&reader, &origins[i][1])
			|| !BotNav_ReadF32Raw (&reader, &origins[i][2]))
			goto cleanup;
	}

	for (i = 0; i < header.link_count; ++i)
	{
		if (!BotNav_ReadS32Raw (&reader, &links[i].target_node)
			|| !BotNav_ReadS16Raw (&reader, &links[i].type)
			|| !BotNav_ReadS16Raw (&reader, &links[i].traversal_index))
			goto cleanup;
	}

	for (i = 0; i < header.traversal_count; ++i)
	{
		int traversal_type;
		int param0;
		int param1;
		int param2;

		if (!BotNav_ReadS32Raw (&reader, &traversal_type)
			|| !BotNav_ReadS32Raw (&reader, &param0)
			|| !BotNav_ReadS32Raw (&reader, &param1)
			|| !BotNav_ReadS32Raw (&reader, &param2))
			goto cleanup;
		(void) traversal_type;
		(void) param0;
		(void) param1;
		(void) param2;
	}

	if (reader.ofs < reader.len)
	{
		size_t remaining = reader.len - reader.ofs;

		if (remaining % sizeof (int) != 0)
			goto cleanup;
		edict_ref_count = (int) (remaining / sizeof (int));
		if (edict_ref_count > 0)
		{
			edict_refs = (int *) q_malloc ((size_t) edict_ref_count * sizeof (*edict_refs));
			if (!edict_refs)
				goto cleanup;
			for (i = 0; i < edict_ref_count; ++i)
			{
				if (!BotNav_ReadS32Raw (&reader, &edict_refs[i]))
					goto cleanup;
			}
		}
	}

	for (i = 0; i < header.node_count; ++i)
	{
		int first_link = nodes[i].first_link;
		int link_count = nodes[i].link_count;

		if (first_link < 0 || link_count < 0)
			goto cleanup;
		if (first_link > header.link_count)
			goto cleanup;
		if (link_count > header.link_count - first_link)
			goto cleanup;

		for (j = 0; j < link_count; ++j)
		{
			int link_index = first_link + j;

			if (links[link_index].target_node < 0 || links[link_index].target_node >= header.node_count)
				goto cleanup;
			if (links[link_index].traversal_index < -1 || links[link_index].traversal_index >= header.traversal_count)
				goto cleanup;
			if (link_used[link_index])
				goto cleanup;
			link_used[link_index] = 1;
		}
	}

	for (i = 0; i < header.link_count; ++i)
	{
		if (!link_used[i])
			goto cleanup;
	}

	BotNav_ResetGraph ();
	g_bot_nav.version = header.version;
	g_bot_nav.traversal_count = header.traversal_count;
	g_bot_nav.edict_ref_count = edict_ref_count;

	for (i = 0; i < header.node_count; ++i)
	{
		if (!BotNav_AddNode (origins[i], nodes[i].flags, nodes[i].radius))
			goto cleanup;
		g_bot_nav.nodes[i].link_count = nodes[i].link_count;
	}

	for (i = 0; i < header.node_count; ++i)
	{
		int first_link = nodes[i].first_link;
		int link_count = nodes[i].link_count;

		for (j = 0; j < link_count; ++j)
		{
			int link_index = first_link + j;
			vec3_t delta;
			float cost;

			VectorSubtract (origins[links[link_index].target_node], origins[i], delta);
			cost = VectorLength (delta);
			if (cost <= 0.f)
				cost = 1.f;
			if (!BotNav_AddDirectedLink (i, links[link_index].target_node, cost, links[link_index].type, links[link_index].traversal_index))
				goto cleanup;
		}
	}

	g_bot_nav.loaded = true;
	q_strlcpy (g_bot_nav.mapname, mapname, sizeof (g_bot_nav.mapname));
	success = true;

cleanup:
	if (!success)
	{
		if (bot_nav_debug.value)
			Con_Printf ("BotNav: failed to load strict NAV2 %s\n", loaded_path);
		BotNav_ResetGraph ();
	}

	if (edict_refs)
		q_free (edict_refs);
	if (link_used)
		q_free (link_used);
	if (links)
		q_free (links);
	if (origins)
		q_free (origins);
	if (nodes)
		q_free (nodes);
	q_free(data);
}

qboolean BotNav_IsLoaded (void)
{
	return g_bot_nav.loaded;
}

int BotNav_NodeCount (void)
{
	return g_bot_nav.node_count;
}

int BotNav_FindNearestNode (const vec3_t pos)
{
	int i;
	int nearest = -1;
	int nearest_reachable = -1;
	float best_dist2 = BOT_NAV_INF;
	float best_reachable_dist2 = BOT_NAV_INF;

	if (!g_bot_nav.loaded || g_bot_nav.node_count <= 0)
		return -1;

	for (i = 0; i < g_bot_nav.node_count; ++i)
	{
		vec3_t delta;
		float dist2;

		VectorSubtract (g_bot_nav.nodes[i].pos, pos, delta);
		dist2 = DotProduct (delta, delta);
		if (dist2 < best_dist2)
		{
			best_dist2 = dist2;
			nearest = i;
		}

		if (dist2 < best_reachable_dist2)
		{
			vec3_t mins = {-16.f, -16.f, -24.f};
			vec3_t maxs = {16.f, 16.f, 32.f};
			vec3_t start;
			vec3_t end;
			trace_t tr;

			VectorCopy (pos, start);
			start[2] += 18.f;
			VectorCopy (g_bot_nav.nodes[i].pos, end);
			end[2] += 18.f;
			tr = SV_Move (start, mins, maxs, end, MOVE_NOMONSTERS, NULL);
			if (!tr.startsolid && !tr.allsolid && tr.fraction >= 0.98f)
			{
				best_reachable_dist2 = dist2;
				nearest_reachable = i;
			}
		}
	}

	if (nearest_reachable >= 0)
		return nearest_reachable;

	return nearest;
}

qboolean BotNav_GetNodePosition (int node_index, vec3_t out_pos)
{
	if (!out_pos)
		return false;
	if ((unsigned int) node_index >= (unsigned int) g_bot_nav.node_count)
		return false;

	VectorCopy (g_bot_nav.nodes[node_index].pos, out_pos);
	return true;
}

qboolean BotNav_FindPath (int start_node, int goal_node, bot_path_t *out_path)
{
	int i;
	int current;
	uint32_t search_gen;
	uint32_t expanded = 0;
	qboolean collect_debug_stats;

	if (!out_path)
		return false;

	out_path->count = 0;
	out_path->total_cost = 0.f;

	if (!g_bot_nav.loaded)
		return false;

	if ((unsigned int) start_node >= (unsigned int) g_bot_nav.node_count || (unsigned int) goal_node >= (unsigned int) g_bot_nav.node_count)
		return false;

	collect_debug_stats = bot_nav_debug.value != 0.f;
	if (collect_debug_stats)
		++g_bot_nav.debug_search_calls;

	if (start_node == goal_node)
	{
		out_path->count = 1;
		out_path->nodes[0] = start_node;
		return true;
	}

	search_gen = BotNav_BeginSearchGen ();
	BotNav_InitNodeForSearch (start_node, search_gen);
	g_bot_nav.gscore[start_node] = 0.f;
	{
		vec3_t delta;
		VectorSubtract (g_bot_nav.nodes[goal_node].pos, g_bot_nav.nodes[start_node].pos, delta);
		g_bot_nav.fscore[start_node] = VectorLength (delta);
	}
	BotNav_OpenHeapPushOrUpdate (start_node);

	while (g_bot_nav.open_heap_size > 0)
	{
		current = BotNav_OpenHeapPopMin ();
		if (current < 0)
			break;
		if (g_bot_nav.closed_gen[current] == search_gen)
			continue;

		if (current == goal_node)
		{
			int reverse[BOT_NAV_MAX_PATH_NODES * 4];
			int reverse_count = 0;
			int node = goal_node;

			while (node >= 0 && reverse_count < (int) countof (reverse))
			{
				reverse[reverse_count++] = node;
				if (node == start_node)
					break;
				node = g_bot_nav.came_from[node];
			}

			if (reverse_count <= 0 || reverse[reverse_count - 1] != start_node)
				return false;
			if (reverse_count > BOT_NAV_MAX_PATH_NODES)
				return false;

			out_path->count = reverse_count;
			for (i = 0; i < reverse_count; ++i)
				out_path->nodes[i] = reverse[reverse_count - 1 - i];
			out_path->total_cost = g_bot_nav.gscore[goal_node];
			if (collect_debug_stats)
			{
				g_bot_nav.debug_expanded_last = expanded;
				g_bot_nav.debug_expanded_total += expanded;
			}
			return true;
		}

		g_bot_nav.closed_gen[current] = search_gen;
		++expanded;

		for (i = g_bot_nav.nodes[current].first_link; i >= 0; i = g_bot_nav.links[i].next)
		{
			int neighbor;
			float tentative;

			if ((unsigned int) i >= (unsigned int) g_bot_nav.link_count)
				break;
			neighbor = g_bot_nav.links[i].to;
			if ((unsigned int) neighbor >= (unsigned int) g_bot_nav.node_count)
				continue;
			BotNav_InitNodeForSearch (neighbor, search_gen);

			if (g_bot_nav.closed_gen[neighbor] == search_gen)
				continue;

			tentative = g_bot_nav.gscore[current] + g_bot_nav.links[i].cost;
			if (tentative < g_bot_nav.gscore[neighbor])
			{
				vec3_t delta;
				g_bot_nav.came_from[neighbor] = current;
				g_bot_nav.gscore[neighbor] = tentative;
				VectorSubtract (g_bot_nav.nodes[goal_node].pos, g_bot_nav.nodes[neighbor].pos, delta);
				g_bot_nav.fscore[neighbor] = tentative + VectorLength (delta);
				BotNav_OpenHeapPushOrUpdate (neighbor);
			}
		}
	}

	if (collect_debug_stats)
	{
		g_bot_nav.debug_expanded_last = expanded;
		g_bot_nav.debug_expanded_total += expanded;
	}

	return false;
}

void BotNav_DebugDraw (void)
{
	static double next_log_time;

	if (!bot_nav_debug.value)
		return;
	if (realtime < next_log_time)
		return;

	next_log_time = realtime + 1.0;
	if (!g_bot_nav.loaded)
		Con_Printf ("BotNav: not loaded\n");
	else
		Con_Printf ("BotNav: map=%s nodes=%d links=%d version=%d path_calls=%u expanded_last=%u expanded_total=%u\n",
			g_bot_nav.mapname, g_bot_nav.node_count, g_bot_nav.link_count, g_bot_nav.version,
			g_bot_nav.debug_search_calls, g_bot_nav.debug_expanded_last, g_bot_nav.debug_expanded_total);
}
