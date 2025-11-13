#include "quakedef.h"
#include "q_ctype.h"

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
	BOT_PRIORITY_ARMOR
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



static float SV_Bot_Frand (void)
{
	return (float)rand () / (float)RAND_MAX;
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
                Z_Free (buffer);
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
		Z_Free (buffer);
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
	default:
	break;
	}

	return false;
}

static void SV_Bot_UpdateGoal (edict_t *self, sv_bot_state_t *state, bot_priority_t priority, double now)
{
	edict_t *goal;

	if (!state)
	return;

	if (priority == BOT_PRIORITY_NONE)
	{
	state->goal_edict = NULL;
	state->goal_priority = BOT_PRIORITY_NONE;
	state->goal_best_dist = 0.0f;
	return;
	}

	if (state->goal_priority != priority || !SV_Bot_GoalStillValid (state->goal_edict, priority, self))
	{
	goal = SV_Bot_FindGoal (self, priority);
	state->goal_edict = goal;
	state->goal_priority = priority;
	if (goal)
	{
	state->goal_best_dist = SV_Bot_Distance (goal->v.origin, self->v.origin);
	state->goal_last_progress_time = now;
	}
	else
	{
	state->goal_best_dist = 0.0f;
	}
	}
}

static void SV_Bot_CheckGoalProgress (edict_t *self, sv_bot_state_t *state, double now)
{
	float dist;

	if (!state || !state->goal_edict)
	return;

	if (!SV_Bot_GoalStillValid (state->goal_edict, state->goal_priority, self))
	{
	state->goal_edict = NULL;
	state->goal_priority = BOT_PRIORITY_NONE;
	return;
	}

	dist = SV_Bot_Distance (state->goal_edict->v.origin, self->v.origin);
	if (dist < 48.0f)
	{
	state->goal_edict = NULL;
	state->goal_priority = BOT_PRIORITY_NONE;
	state->goal_best_dist = 0.0f;
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

void SV_Bot_Reset (void)
{
        int i;
        memset (sv_bot_states, 0, sizeof(sv_bot_states));
        for (i = 0; i < (int)(sizeof(sv_bot_states) / sizeof(sv_bot_states[0])); i++)
        {
                sv_bot_states[i].enemy_slot = -1;
        }
        bot_name_counter = 1;
        SV_Bot_ResetProfiles ();
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
	vec3_t delta;
	vec3_t base_angles;
	qboolean have_base_angles = false;

	if (!self || self->free)
		return;

	enemy = SV_Bot_FindTarget (client, state, true);
	if (!enemy)
		enemy = SV_Bot_FindTarget (client, state, false);

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
		SV_Bot_UpdateGoal (self, state, priority, now);
		SV_Bot_CheckGoalProgress (self, state, now);
		goal = state->goal_edict;

		if (!goal && state->next_wander_time <= now)
			SV_Bot_PickWanderDirection (state);
	}

	if (enemy && state && settings && !settings->behaviors.allow_grab_items_in_combat)
	{
		priority = BOT_PRIORITY_NONE;
		state->goal_edict = NULL;
		state->goal_priority = BOT_PRIORITY_NONE;
		goal = NULL;
	}

	if (goal)
	{
		vec3_t dir;
		float dist;

		VectorSubtract (goal->v.origin, self->v.origin, dir);
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
