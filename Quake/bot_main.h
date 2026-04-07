#ifndef BOT_MAIN_H
#define BOT_MAIN_H

#include "quakedef.h"
#include "bot_nav2.h"

typedef enum bot_ai_state_e
{
	BOT_STATE_ROAM = 0,
	BOT_STATE_SEEK_ITEM,
	BOT_STATE_CHASE_ENEMY,
	BOT_STATE_FOLLOW,
	BOT_STATE_SEARCH,
	BOT_STATE_ATTACK,
	BOT_STATE_RETREAT,
	BOT_STATE_STUCK_RECOVERY
} bot_ai_state_t;

typedef struct bot_nav_nearest_cache_s
{
	int		node_index;
	int		nav_node_count;
	double		timestamp;
	vec3_t		sampled_origin;
} bot_nav_nearest_cache_t;

#define BOT_TARGET_EVAL_CACHE_SIZE 8

typedef struct bot_target_eval_cache_entry_s
{
	int		target_entnum;
	double		timestamp;
	qboolean	has_visibility;
	qboolean	visibility;
	qboolean	has_pathable;
	qboolean	pathable;
} bot_target_eval_cache_entry_t;

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
	edict_t		*failed_goal_item;
	double		failed_goal_item_until;
	int		failed_goal_node;
	double		failed_goal_node_until;

	bot_path_t	path;
	qboolean	has_path;
	int		path_index;
	double		next_repath_time;
	bot_nav_nearest_cache_t	self_nearest_cache;
	bot_nav_nearest_cache_t	goal_nearest_cache;
	bot_target_eval_cache_entry_t	target_eval_cache[BOT_TARGET_EVAL_CACHE_SIZE];
	vec3_t		target_eval_origin;
	qboolean	target_eval_origin_valid;

	vec3_t		last_origin;
	double		last_progress_time;
	double		stuck_until;
	double		next_item_scan_time;
	double		next_debug_time;
	double		respawn_time;
	float		last_health;
	float		last_leader_health;
	double		alert_until;
	vec3_t		alert_pos;

	float		strafe_dir;
	double		next_strafe_change;
	double		next_jump_time;

	double		last_weapon_switch_time;
	int		last_requested_weapon;
	int		roam_point;

	double		retreat_hold_until;
	int		fire_block_streak;
	double		last_fire_block_time;

	int		failed_edge_from[12];
	int		failed_edge_to[12];
	double		failed_edge_until[12];
	int		failed_edge_cursor;

	double		stuck_window_start;
	int		stuck_window_count;
	int		no_move_target_streak;
	int		obstacle_avoid_streak;

	int		dbg_stuck_events;
	int		dbg_repaths;
	int		dbg_blocked_shots;
	int		dbg_weapon_downgrades;

	edict_t		*follow_target;
	double		follow_unreachable_since;
	double		next_follow_teleport_time;

	double		next_full_think_time;
	double		last_full_think_time;
	bot_ai_state_t	last_decision_state;
	edict_t		*last_decision_enemy;
	vec3_t		last_decision_goal_pos;
	qboolean	last_decision_has_goal;
	qboolean	last_decision_has_path;
	bot_path_t	last_decision_path;
	int		last_decision_path_index;
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
qboolean Bot_RespawnClient (client_t *client);

#endif /* BOT_MAIN_H */
