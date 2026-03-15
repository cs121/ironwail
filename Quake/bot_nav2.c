#include "quakedef.h"
#include "bot_nav2.h"

#include <ctype.h>

#define BOT_NAV_MAX_NODES 8192
#define BOT_NAV_MAX_LINKS 65536
#define BOT_NAV_INF 1.0e30f

typedef struct bot_nav_node_s
{
	vec3_t	pos;
	int		first_link;
	int		flags;
} bot_nav_node_t;

typedef struct bot_nav_link_s
{
	int		to;
	int		next;
	float	cost;
	int		flags;
} bot_nav_link_t;

typedef struct bot_nav_graph_s
{
	qboolean	loaded;
	char		mapname[64];
	int		version;
	int		node_count;
	int		link_count;
	bot_nav_node_t	nodes[BOT_NAV_MAX_NODES];
	bot_nav_link_t	links[BOT_NAV_MAX_LINKS];

	qboolean	open[BOT_NAV_MAX_NODES];
	qboolean	closed[BOT_NAV_MAX_NODES];
	float		gscore[BOT_NAV_MAX_NODES];
	float		fscore[BOT_NAV_MAX_NODES];
	int		came_from[BOT_NAV_MAX_NODES];
} bot_nav_graph_t;

static bot_nav_graph_t g_bot_nav;

extern cvar_t bot_nav_debug;

static uint32_t BotNav_ReadU32 (const byte *p)
{
	uint32_t value;
	Q_memcpy (&value, p, sizeof (value));
	return (uint32_t) LittleLong ((int) value);
}

static int BotNav_ReadS32 (const byte *p)
{
	return (int) BotNav_ReadU32 (p);
}

static float BotNav_ReadF32 (const byte *p)
{
	union
	{
		uint32_t u;
		float f;
	} value;

	value.u = BotNav_ReadU32 (p);
	return value.f;
}

static void BotNav_ResetGraph (void)
{
	int i;

	g_bot_nav.loaded = false;
	g_bot_nav.mapname[0] = '\0';
	g_bot_nav.version = 0;
	g_bot_nav.node_count = 0;
	g_bot_nav.link_count = 0;

	for (i = 0; i < BOT_NAV_MAX_NODES; ++i)
		g_bot_nav.nodes[i].first_link = -1;
}

static qboolean BotNav_LinkExists (int from, int to)
{
	int link_idx;

	for (link_idx = g_bot_nav.nodes[from].first_link; link_idx >= 0; link_idx = g_bot_nav.links[link_idx].next)
	{
		if (g_bot_nav.links[link_idx].to == to)
			return true;
	}

	return false;
}

static qboolean BotNav_AddDirectedLink (int from, int to, float cost, int flags)
{
	bot_nav_link_t *link;

	if ((unsigned int) from >= (unsigned int) g_bot_nav.node_count || (unsigned int) to >= (unsigned int) g_bot_nav.node_count)
		return false;

	if (from == to)
		return false;

	if (BotNav_LinkExists (from, to))
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
	link->flags = flags;
	link->next = g_bot_nav.nodes[from].first_link;
	g_bot_nav.nodes[from].first_link = g_bot_nav.link_count - 1;

	return true;
}

static qboolean BotNav_AddLinkBidirectional (int a, int b, float cost, int flags)
{
	qboolean ok1;
	qboolean ok2;

	ok1 = BotNav_AddDirectedLink (a, b, cost, flags);
	ok2 = BotNav_AddDirectedLink (b, a, cost, flags);
	return ok1 && ok2;
}

static qboolean BotNav_AddNode (const vec3_t pos, int flags)
{
	bot_nav_node_t *node;

	if (g_bot_nav.node_count >= BOT_NAV_MAX_NODES)
		return false;

	node = &g_bot_nav.nodes[g_bot_nav.node_count++];
	VectorCopy (pos, node->pos);
	node->flags = flags;
	node->first_link = -1;
	return true;
}

static char *BotNav_Trim (char *str)
{
	char *end;

	while (*str && isspace ((unsigned char) *str))
		++str;

	end = str + strlen (str);
	while (end > str && isspace ((unsigned char) end[-1]))
		*--end = '\0';

	return str;
}

static qboolean BotNav_ProbablyText (const byte *data, size_t len)
{
	size_t i;
	size_t printable = 0;
	size_t zeroes = 0;

	for (i = 0; i < len; ++i)
	{
		unsigned char c = data[i];
		if (!c)
		{
			zeroes++;
			continue;
		}
		if (isprint (c) || isspace (c))
			printable++;
	}

	if (zeroes > len / 32)
		return false;

	return printable > (len * 3) / 4;
}

static void BotNav_AutoLinkNodes (void)
{
	const float max_dist = 640.f;
	const float max_dist2 = max_dist * max_dist;
	int i;

	if (g_bot_nav.node_count <= 1)
		return;

	if (g_bot_nav.node_count > 2048)
		return;

	for (i = 0; i < g_bot_nav.node_count; ++i)
	{
		int nearest_idx[4] = {-1, -1, -1, -1};
		float nearest_dist2[4] = {BOT_NAV_INF, BOT_NAV_INF, BOT_NAV_INF, BOT_NAV_INF};
		int j;
		int slot;

		for (j = 0; j < g_bot_nav.node_count; ++j)
		{
			vec3_t delta;
			float dist2;

			if (i == j)
				continue;

			VectorSubtract (g_bot_nav.nodes[j].pos, g_bot_nav.nodes[i].pos, delta);
			dist2 = DotProduct (delta, delta);
			if (dist2 > max_dist2)
				continue;

			for (slot = 0; slot < 4; ++slot)
			{
				int move;

				if (dist2 >= nearest_dist2[slot])
					continue;

				for (move = 3; move > slot; --move)
				{
					nearest_dist2[move] = nearest_dist2[move - 1];
					nearest_idx[move] = nearest_idx[move - 1];
				}

				nearest_dist2[slot] = dist2;
				nearest_idx[slot] = j;
				break;
			}
		}

		for (slot = 0; slot < 4; ++slot)
		{
			if (nearest_idx[slot] >= 0)
				BotNav_AddLinkBidirectional (i, nearest_idx[slot], sqrtf (nearest_dist2[slot]), 0);
		}
	}
}

static qboolean BotNav_FinalizeLoad (const char *mapname)
{
	if (g_bot_nav.node_count < 2)
		return false;

	if (g_bot_nav.link_count <= 0)
		BotNav_AutoLinkNodes ();

	if (g_bot_nav.link_count <= 0)
		return false;

	g_bot_nav.loaded = true;
	q_strlcpy (g_bot_nav.mapname, mapname, sizeof (g_bot_nav.mapname));
	return true;
}

static qboolean BotNav_ParseBinaryLayout (const byte *data, size_t len, int version, int node_count, int link_count, size_t node_ofs, size_t link_ofs)
{
	int i;
	size_t stride;

	if (node_count <= 0 || node_count > BOT_NAV_MAX_NODES)
		return false;
	if (link_count <= 0 || link_count > BOT_NAV_MAX_LINKS)
		return false;

	if (node_ofs >= len || node_ofs + (size_t) node_count * 12 > len)
		return false;

	if (!link_ofs)
		link_ofs = node_ofs + (size_t) node_count * 12;

	if (link_ofs >= len)
		return false;

	BotNav_ResetGraph ();
	g_bot_nav.version = version;

	for (i = 0; i < node_count; ++i)
	{
		vec3_t pos;
		const byte *p = data + node_ofs + i * 12;

		pos[0] = BotNav_ReadF32 (p + 0);
		pos[1] = BotNav_ReadF32 (p + 4);
		pos[2] = BotNav_ReadF32 (p + 8);
		if (!BotNav_AddNode (pos, 0))
			return false;
	}

	stride = 8;
	if (link_ofs + (size_t) link_count * 12 <= len)
		stride = 12;
	else if (link_ofs + (size_t) link_count * 8 > len)
		return false;

	for (i = 0; i < link_count; ++i)
	{
		const byte *p = data + link_ofs + i * stride;
		int from = BotNav_ReadS32 (p + 0);
		int to = BotNav_ReadS32 (p + 4);
		float cost = 0.f;

		if ((unsigned int) from >= (unsigned int) node_count || (unsigned int) to >= (unsigned int) node_count)
			continue;

		if (stride >= 12)
			cost = BotNav_ReadF32 (p + 8);

		BotNav_AddLinkBidirectional (from, to, cost, 0);
	}

	return true;
}

static qboolean BotNav_ParseBinary (const byte *data, size_t len)
{
	int version;
	int node_count;
	int link_count;
	size_t node_ofs;
	size_t link_ofs;

	if (len < 16)
		return false;

	if (memcmp (data, "NAV2", 4) != 0 && memcmp (data, "2VAN", 4) != 0 && memcmp (data, "nav2", 4) != 0)
		return false;

	version = BotNav_ReadS32 (data + 4);
	node_count = BotNav_ReadS32 (data + 8);
	link_count = BotNav_ReadS32 (data + 12);

	if (BotNav_ParseBinaryLayout (data, len, version, node_count, link_count, 16u, 0u))
		return true;

	if (len < 24)
		return false;

	node_ofs = (size_t) BotNav_ReadU32 (data + 16);
	link_ofs = (size_t) BotNav_ReadU32 (data + 20);
	if (BotNav_ParseBinaryLayout (data, len, version, node_count, link_count, node_ofs, link_ofs))
		return true;

	return false;
}

static qboolean BotNav_ParseText (char *text)
{
	const char *cursor;
	stringview_t line;
	int pending_from[BOT_NAV_MAX_LINKS];
	int pending_to[BOT_NAV_MAX_LINKS];
	float pending_cost[BOT_NAV_MAX_LINKS];
	int pending_count;
	int link_idx;

	BotNav_ResetGraph ();
	cursor = text;
	pending_count = 0;

	while (COM_ParseLine (&cursor, &line))
	{
		char buffer[512];
		char *trim;
		float x, y, z;
		float cost;
		int a, b;
		int copied;

		copied = (int) q_min (line.len, sizeof (buffer) - 1);
		Q_memcpy (buffer, line.data, copied);
		buffer[copied] = '\0';

		trim = BotNav_Trim (buffer);
		if (!trim[0])
			continue;
		if (trim[0] == '#' || trim[0] == ';' || (trim[0] == '/' && trim[1] == '/'))
			continue;

		if (sscanf (trim, "node %f %f %f", &x, &y, &z) == 3 ||
			sscanf (trim, "v %f %f %f", &x, &y, &z) == 3)
		{
			vec3_t pos;
			pos[0] = x;
			pos[1] = y;
			pos[2] = z;
			BotNav_AddNode (pos, 0);
			continue;
		}

		if (sscanf (trim, "%*d %f %f %f", &x, &y, &z) == 3)
		{
			vec3_t pos;
			pos[0] = x;
			pos[1] = y;
			pos[2] = z;
			BotNav_AddNode (pos, 0);
			continue;
		}

		cost = 0.f;
		if (sscanf (trim, "link %d %d %f", &a, &b, &cost) >= 2 ||
			sscanf (trim, "edge %d %d %f", &a, &b, &cost) >= 2 ||
			sscanf (trim, "e %d %d %f", &a, &b, &cost) >= 2 ||
			sscanf (trim, "%d %d %f", &a, &b, &cost) >= 2)
		{
			if (pending_count < BOT_NAV_MAX_LINKS)
			{
				pending_from[pending_count] = a;
				pending_to[pending_count] = b;
				pending_cost[pending_count] = cost;
				pending_count++;
			}
			continue;
		}
	}

	for (link_idx = 0; link_idx < pending_count; ++link_idx)
	{
		int from = pending_from[link_idx];
		int to = pending_to[link_idx];
		float cost = pending_cost[link_idx];

		if ((unsigned int) from >= (unsigned int) g_bot_nav.node_count || (unsigned int) to >= (unsigned int) g_bot_nav.node_count)
			continue;

		BotNav_AddLinkBidirectional (from, to, cost, 0);
	}

	return true;
}

void BotNav_Shutdown (void)
{
	BotNav_ResetGraph ();
}

void BotNav_LoadForMap (const char *mapname)
{
	byte *data;
	char *text_copy;
	size_t len;
	char path[MAX_QPATH];
	qboolean parsed;

	BotNav_ResetGraph ();

	if (!mapname || !mapname[0])
		return;

	q_snprintf (path, sizeof (path), "maps/%s.nav2", mapname);
	data = COM_LoadMallocFile (path, NULL);
	if (!data)
	{
		if (bot_nav_debug.value)
			Con_Printf ("BotNav: no nav2 found for %s\n", mapname);
		return;
	}

	len = (size_t) com_filesize;
	parsed = false;
	text_copy = NULL;

	if (!BotNav_ProbablyText (data, len))
		parsed = BotNav_ParseBinary (data, len);

	if (!parsed)
	{
		text_copy = (char *) q_malloc(len + 1);
		if (text_copy)
		{
			if (len > 0)
				Q_memcpy (text_copy, data, len);
			text_copy[len] = '\0';
			parsed = BotNav_ParseText (text_copy);
			if (parsed)
				g_bot_nav.version = 1;
		}
		else if (bot_nav_debug.value)
		{
			Con_Printf ("BotNav: out of memory while parsing %s\n", path);
		}
	}

	if (parsed && BotNav_FinalizeLoad (mapname))
	{
		if (bot_nav_debug.value)
		{
			Con_Printf (
				"BotNav: loaded %s (%d nodes, %d links, v%d)\n",
				path,
				g_bot_nav.node_count,
				g_bot_nav.link_count,
				g_bot_nav.version
			);
		}
	}
	else
	{
		if (bot_nav_debug.value)
			Con_Printf ("BotNav: failed to parse %s, fallback roaming active\n", path);
		BotNav_ResetGraph ();
	}

	if (text_copy)
		q_free(text_copy);
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
	float best_dist2 = BOT_NAV_INF;

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
	}

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

	if (!out_path)
		return false;

	out_path->count = 0;
	out_path->total_cost = 0.f;

	if (!g_bot_nav.loaded)
		return false;

	if ((unsigned int) start_node >= (unsigned int) g_bot_nav.node_count || (unsigned int) goal_node >= (unsigned int) g_bot_nav.node_count)
		return false;

	if (start_node == goal_node)
	{
		out_path->count = 1;
		out_path->nodes[0] = start_node;
		return true;
	}

	for (i = 0; i < g_bot_nav.node_count; ++i)
	{
		g_bot_nav.open[i] = false;
		g_bot_nav.closed[i] = false;
		g_bot_nav.gscore[i] = BOT_NAV_INF;
		g_bot_nav.fscore[i] = BOT_NAV_INF;
		g_bot_nav.came_from[i] = -1;
	}

	g_bot_nav.open[start_node] = true;
	g_bot_nav.gscore[start_node] = 0.f;
	{
		vec3_t delta;
		VectorSubtract (g_bot_nav.nodes[goal_node].pos, g_bot_nav.nodes[start_node].pos, delta);
		g_bot_nav.fscore[start_node] = VectorLength (delta);
	}

	while (1)
	{
		float best_f = BOT_NAV_INF;
		current = -1;

		for (i = 0; i < g_bot_nav.node_count; ++i)
		{
			if (!g_bot_nav.open[i])
				continue;
			if (g_bot_nav.fscore[i] < best_f)
			{
				best_f = g_bot_nav.fscore[i];
				current = i;
			}
		}

		if (current < 0)
			break;

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
			return true;
		}

		g_bot_nav.open[current] = false;
		g_bot_nav.closed[current] = true;

		for (i = g_bot_nav.nodes[current].first_link; i >= 0; i = g_bot_nav.links[i].next)
		{
			int neighbor = g_bot_nav.links[i].to;
			float tentative;

			if (g_bot_nav.closed[neighbor])
				continue;

			tentative = g_bot_nav.gscore[current] + g_bot_nav.links[i].cost;
			if (!g_bot_nav.open[neighbor] || tentative < g_bot_nav.gscore[neighbor])
			{
				vec3_t delta;
				g_bot_nav.came_from[neighbor] = current;
				g_bot_nav.gscore[neighbor] = tentative;
				VectorSubtract (g_bot_nav.nodes[goal_node].pos, g_bot_nav.nodes[neighbor].pos, delta);
				g_bot_nav.fscore[neighbor] = tentative + VectorLength (delta);
				g_bot_nav.open[neighbor] = true;
			}
		}
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
		Con_Printf ("BotNav: map=%s nodes=%d links=%d version=%d\n", g_bot_nav.mapname, g_bot_nav.node_count, g_bot_nav.link_count, g_bot_nav.version);
}
