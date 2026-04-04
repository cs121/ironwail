#ifndef BOT_MAIN_H
#define BOT_MAIN_H

#include "quakedef.h"
#include "bot_nav2.h"

typedef enum bot_ai_state_e
{
	BOT_STATE_ROAM = 0,
	BOT_STATE_SEEK_ITEM,
	BOT_STATE_CHASE_ENEMY,
	BOT_STATE_ATTACK,
	BOT_STATE_RETREAT,
	BOT_STATE_STUCK_RECOVERY
} bot_ai_state_t;

typedef struct bot_state_s
{
	qboolean	inuse;
	int		clientnum;
	char		name[32];
	int		forced_team;

	bot_ai_state_t	state;
	edict_t		*enemy;
	vec3_t		enemy_last_pos;
	double		enemy_last_seen_time;

	edict_t		*goal_item;
	vec3_t		goal_pos;
	qboolean	has_goal;
	double		goal_timeout;

	bot_path_t	path;
	qboolean	has_path;
	int		path_index;
	double		next_repath_time;

	vec3_t		last_origin;
	double		last_progress_time;
	double		stuck_until;
	double		next_item_scan_time;
	double		next_debug_time;
	double		respawn_time;

	float		strafe_dir;
	double		next_strafe_change;

	double		last_weapon_switch_time;
	int		last_requested_weapon;
	int		roam_point;
} bot_state_t;

extern cvar_t bot_debug;
extern cvar_t bot_nav_debug;
extern cvar_t bot_think_debug;
extern cvar_t bot_aim_debug;
extern cvar_t bot_skill;
extern cvar_t bot_use_nav2;
extern cvar_t bot_call_clientconnect;
extern cvar_t bot_count;

void Bot_Init (void);
void Bot_Shutdown (void);
void Bot_OnServerSpawnedMap (void);
qboolean Bot_IsClientBot (const client_t *client);
void Bot_RunFrameForClient (client_t *client);
void Bot_OnClientDropped (client_t *client);
int Bot_GetActiveCount (void);

#endif /* BOT_MAIN_H */
