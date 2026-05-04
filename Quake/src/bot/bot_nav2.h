#ifndef BOT_NAV2_H
#define BOT_NAV2_H

#include "quakedef.h"

#define BOT_NAV_MAX_PATH_NODES 128

typedef struct bot_path_s
{
	int		count;
	int		nodes[BOT_NAV_MAX_PATH_NODES];
	float	total_cost;
} bot_path_t;

void BotNav_Shutdown (void);
void BotNav_LoadForMap (const char *mapname);
qboolean BotNav_IsLoaded (void);
int BotNav_NodeCount (void);
int BotNav_FindNearestNode (const vec3_t pos);
qboolean BotNav_FindPath (int start_node, int goal_node, bot_path_t *out_path);
qboolean BotNav_GetNodePosition (int node_index, vec3_t out_pos);
void BotNav_DebugDraw (void);

#endif /* BOT_NAV2_H */
