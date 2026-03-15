#include "quakedef.h"
#include "bgmusic.h"
#include "cl_iwmusic.h"

#define IWM_MAX_TRIGGERS 128
#define IWM_MAX_ZONES 128
#define IWM_MAX_STATES 64
#define IWM_MAX_RULES 128

typedef struct
{
	vec3_t origin;
	float radius;
	float volume;
	int fade_ms;
	int priority;
	int enter_serial;
	qboolean active;
	char state[64];
	char track[MAX_QPATH];
} iwm_trigger_t;

typedef struct
{
	vec3_t origin;
	float radius;
	float multiplier;
	qboolean active;
} iwm_zone_t;

typedef struct
{
	char name[64];
	char track[MAX_QPATH];
	float volume;
	int fade_ms;
} iwm_state_t;

typedef enum
{
	IWR_ALWAYS,
	IWR_CVAR,
	IWR_TIME_SINCE_DAMAGE_GT
} iwm_rule_type_t;

typedef struct
{
	iwm_rule_type_t type;
	char a[64];
	char b[64];
	int threshold_ms;
	char then_state[64];
	int priority;
} iwm_rule_t;

static cvar_t cl_iwmusic_enable = {"cl_iwmusic_enable", "0", CVAR_ARCHIVE};
static cvar_t cl_iwmusic_debug = {"cl_iwmusic_debug", "0", CVAR_NONE};
static cvar_t cl_iwmusic_eventfile = {"cl_iwmusic_eventfile", "", CVAR_ARCHIVE};
static cvar_t cl_iwmusic_basevolume = {"cl_iwmusic_basevolume", "1", CVAR_ARCHIVE};

static iwm_trigger_t iwm_triggers[IWM_MAX_TRIGGERS];
static iwm_zone_t iwm_zones[IWM_MAX_ZONES];
static iwm_state_t iwm_states[IWM_MAX_STATES];
static iwm_rule_t iwm_rules[IWM_MAX_RULES];

static int iwm_num_triggers;
static int iwm_num_zones;
static int iwm_num_states;
static int iwm_num_rules;
static iwm_state_t iwm_defaults;
static qboolean iwm_has_defaults;

static int iwm_enter_serial_counter;
static float iwm_zone_duck_current = 1.f;
static double iwm_last_damage_realtime;

static qboolean iwm_forced_active;
static char iwm_forced_state[64];
static char iwm_forced_track[MAX_QPATH];
static float iwm_forced_volume = 1.f;
static int iwm_forced_fade_ms = 500;

static qboolean iwm_switch_pending;
static qboolean iwm_switch_fadein;
static char iwm_pending_track[MAX_QPATH];
static float iwm_target_track_volume = 1.f;
static float iwm_music_gain = 1.f;
static float iwm_gain_from;
static float iwm_gain_to;
static double iwm_gain_start;
static int iwm_gain_duration_ms;

static char iwm_current_track[MAX_QPATH];
static char iwm_current_state[64];

static qboolean CL_IWMusic_DebugEnabled (void)
{
	return cl_iwmusic_debug.value > 0;
}

static void CL_IWMusic_DPrintf (const char *fmt, ...)
{
	va_list argptr;
	char msg[1024];

	if (!CL_IWMusic_DebugEnabled())
		return;

	va_start(argptr, fmt);
	q_vsnprintf(msg, sizeof(msg), fmt, argptr);
	va_end(argptr);

	Con_Printf("[iwmusic] %s", msg);
}

static qboolean CL_IWMusic_ParseVec3 (const char *s, vec3_t out)
{
	if (sscanf(s, "%f %f %f", &out[0], &out[1], &out[2]) == 3)
		return true;
	VectorClear(out);
	return false;
}

static void CL_IWMusic_ResetRuntime (void)
{
	iwm_num_triggers = 0;
	iwm_num_zones = 0;
	iwm_num_states = 0;
	iwm_num_rules = 0;
	iwm_has_defaults = false;
	iwm_enter_serial_counter = 1;
	iwm_zone_duck_current = 1.f;
	iwm_forced_active = false;
	iwm_forced_state[0] = 0;
	iwm_forced_track[0] = 0;
	iwm_forced_volume = 1.f;
	iwm_forced_fade_ms = 500;
	iwm_current_track[0] = 0;
	iwm_current_state[0] = 0;
	iwm_switch_pending = false;
	iwm_switch_fadein = false;
	iwm_music_gain = 1.f;
	iwm_gain_from = 1.f;
	iwm_gain_to = 1.f;
	iwm_gain_start = realtime;
	iwm_gain_duration_ms = 1;
	BGM_SetVolumeScale(1.f);
}

static void CL_IWMusic_AddState (const char *name, const char *track, float volume, int fade_ms)
{
	iwm_state_t *st;
	if (!name || !name[0] || iwm_num_states >= IWM_MAX_STATES)
		return;
	st = &iwm_states[iwm_num_states++];
	q_strlcpy(st->name, name, sizeof(st->name));
	q_strlcpy(st->track, track ? track : "", sizeof(st->track));
	st->volume = CLAMP(0.f, volume, 1.f);
	st->fade_ms = fade_ms > 0 ? fade_ms : 500;
}

static const iwm_state_t *CL_IWMusic_FindState (const char *name)
{
	int i;
	if (!name || !name[0])
		return NULL;
	for (i = 0; i < iwm_num_states; ++i)
		if (!q_strcasecmp(iwm_states[i].name, name))
			return &iwm_states[i];
	return NULL;
}

static void CL_IWMusic_ParseMapEntities (void)
{
	const char *data;
	if (!cl.worldmodel || !cl.worldmodel->entities)
		return;

	data = cl.worldmodel->entities;
	while ((data = COM_Parse(data)) != NULL)
	{
		char classname[64] = "";
		char state[64] = "";
		char track[MAX_QPATH] = "";
		char origin[64] = "";
		float volume = 1.f;
		float radius = 256.f;
		float duck = 1.f;
		int fade_ms = 500;
		int priority = 0;
		qboolean have_radius = false;
		if (strcmp(com_token, "{"))
			break;
		while ((data = COM_Parse(data)) != NULL)
		{
			char key[64];
			if (!strcmp(com_token, "}"))
				break;
			q_strlcpy(key, com_token, sizeof(key));
			data = COM_Parse(data);
			if (!data)
				break;
			if (!q_strcasecmp(key, "classname")) q_strlcpy(classname, com_token, sizeof(classname));
			else if (!q_strcasecmp(key, "state")) q_strlcpy(state, com_token, sizeof(state));
			else if (!q_strcasecmp(key, "track")) q_strlcpy(track, com_token, sizeof(track));
			else if (!q_strcasecmp(key, "origin")) q_strlcpy(origin, com_token, sizeof(origin));
			else if (!q_strcasecmp(key, "volume")) volume = atof(com_token);
			else if (!q_strcasecmp(key, "radius")) { radius = atof(com_token); have_radius = true; }
			else if (!q_strcasecmp(key, "fade_ms")) fade_ms = atoi(com_token);
			else if (!q_strcasecmp(key, "priority")) priority = atoi(com_token);
			else if (!q_strcasecmp(key, "duck")) duck = atof(com_token);
		}

		if (!q_strcasecmp(classname, "info_iwmusic_trigger") && iwm_num_triggers < IWM_MAX_TRIGGERS)
		{
			iwm_trigger_t *t = &iwm_triggers[iwm_num_triggers++];
			memset(t, 0, sizeof(*t));
			CL_IWMusic_ParseVec3(origin, t->origin);
			t->radius = q_max(1.f, radius);
			t->volume = CLAMP(0.f, volume, 1.f);
			t->fade_ms = fade_ms > 0 ? fade_ms : 500;
			t->priority = priority;
			q_strlcpy(t->state, state, sizeof(t->state));
			q_strlcpy(t->track, track, sizeof(t->track));
			if (!have_radius)
				CL_IWMusic_DPrintf("trigger without radius uses fallback 256 (brush bounds unavailable client-side)\n");
		}
		else if (!q_strcasecmp(classname, "info_iwmusic_zone") && iwm_num_zones < IWM_MAX_ZONES)
		{
			iwm_zone_t *z = &iwm_zones[iwm_num_zones++];
			memset(z, 0, sizeof(*z));
			CL_IWMusic_ParseVec3(origin, z->origin);
			z->radius = q_max(1.f, radius);
			if (duck < 1.f)
				z->multiplier = CLAMP(0.f, duck, 1.f);
			else
				z->multiplier = CLAMP(0.f, volume, 1.f);
		}
	}
}

static void CL_IWMusic_ParseEventDefaultsBlock (const char **p)
{
	const char *data = *p;
	if (!(data = COM_Parse(data)) || strcmp(com_token, "{"))
		return;
	while ((data = COM_Parse(data)) != NULL)
	{
		if (!strcmp(com_token, "}"))
			break;
		if (!q_strcasecmp(com_token, "state")) { if (!(data = COM_Parse(data))) break; q_strlcpy(iwm_defaults.name, com_token, sizeof(iwm_defaults.name)); }
		else if (!q_strcasecmp(com_token, "track")) { if (!(data = COM_Parse(data))) break; q_strlcpy(iwm_defaults.track, com_token, sizeof(iwm_defaults.track)); }
		else if (!q_strcasecmp(com_token, "volume")) { if (!(data = COM_Parse(data))) break; iwm_defaults.volume = CLAMP(0.f, atof(com_token), 1.f); }
		else if (!q_strcasecmp(com_token, "fade_ms")) { if (!(data = COM_Parse(data))) break; iwm_defaults.fade_ms = q_max(1, atoi(com_token)); }
		else { if (!(data = COM_Parse(data))) break; }
	}
	iwm_has_defaults = true;
	*p = data;
}

static void CL_IWMusic_ParseStateBlock (const char **p)
{
	const char *data = *p;
	char name[64] = "";
	char track[MAX_QPATH] = "";
	float volume = 1.f;
	int fade_ms = 500;

	if (!(data = COM_Parse(data))) return;
	q_strlcpy(name, com_token, sizeof(name));
	if (!(data = COM_Parse(data)) || strcmp(com_token, "{")) return;
	while ((data = COM_Parse(data)) != NULL)
	{
		if (!strcmp(com_token, "}")) break;
		if (!q_strcasecmp(com_token, "track")) { if (!(data = COM_Parse(data))) break; q_strlcpy(track, com_token, sizeof(track)); }
		else if (!q_strcasecmp(com_token, "volume")) { if (!(data = COM_Parse(data))) break; volume = atof(com_token); }
		else if (!q_strcasecmp(com_token, "fade_ms")) { if (!(data = COM_Parse(data))) break; fade_ms = atoi(com_token); }
		else { if (!(data = COM_Parse(data))) break; }
	}
	CL_IWMusic_AddState(name, track, volume, fade_ms);
	*p = data;
}

static void CL_IWMusic_ParseRuleBlock (const char **p)
{
	const char *data = *p;
	iwm_rule_t *r;
	if (iwm_num_rules >= IWM_MAX_RULES)
		return;
	if (!(data = COM_Parse(data)) || strcmp(com_token, "{"))
		return;
	r = &iwm_rules[iwm_num_rules++];
	memset(r, 0, sizeof(*r));
	r->priority = 0;
	while ((data = COM_Parse(data)) != NULL)
	{
		if (!strcmp(com_token, "}"))
			break;
		if (!q_strcasecmp(com_token, "when"))
		{
			if (!(data = COM_Parse(data))) break;
			if (!q_strcasecmp(com_token, "always")) r->type = IWR_ALWAYS;
			else if (!q_strcasecmp(com_token, "cvar"))
			{
				r->type = IWR_CVAR;
				if (!(data = COM_Parse(data))) break;
				q_strlcpy(r->a, com_token, sizeof(r->a));
				if (!(data = COM_Parse(data))) break;
				q_strlcpy(r->b, com_token, sizeof(r->b));
			}
			else if (!q_strcasecmp(com_token, "time_since_last_damage_ms"))
			{
				r->type = IWR_TIME_SINCE_DAMAGE_GT;
				if (!(data = COM_Parse(data))) break; /* operator */
				if (!(data = COM_Parse(data))) break;
				r->threshold_ms = atoi(com_token);
			}
		}
		else if (!q_strcasecmp(com_token, "then_state")) { if (!(data = COM_Parse(data))) break; q_strlcpy(r->then_state, com_token, sizeof(r->then_state)); }
		else if (!q_strcasecmp(com_token, "priority")) { if (!(data = COM_Parse(data))) break; r->priority = atoi(com_token); }
		else { if (!(data = COM_Parse(data))) break; }
	}
	*p = data;
}

static void CL_IWMusic_LoadEventFile (void)
{
	char path[MAX_QPATH];
	byte *buf;
	const char *data;
	if (cl_iwmusic_eventfile.string[0])
		q_strlcpy(path, cl_iwmusic_eventfile.string, sizeof(path));
	else if (cl.mapname[0])
		q_snprintf(path, sizeof(path), "music/%s.iwm", cl.mapname);
	else
		q_strlcpy(path, "music/events.iwm", sizeof(path));

	buf = (byte *)COM_LoadMallocFile(path, NULL);
	if (!buf)
		buf = (byte *)COM_LoadMallocFile("music/events.iwm", NULL);
	if (!buf)
	{
		CL_IWMusic_DPrintf("no event file found (%s)\n", path);
		return;
	}
	data = (const char *)buf;
	if (!(data = COM_Parse(data)) || q_strcasecmp(com_token, "iw_music"))
	{
		Con_DWarning("iwmusic: invalid header in event file\n");
		q_free(buf);
		return;
	}
	if (!(data = COM_Parse(data)) || strcmp(com_token, "{"))
	{
		Con_DWarning("iwmusic: expected root block\n");
		q_free(buf);
		return;
	}
	while ((data = COM_Parse(data)) != NULL)
	{
		if (!strcmp(com_token, "}"))
			break;
		if (!q_strcasecmp(com_token, "defaults")) CL_IWMusic_ParseEventDefaultsBlock(&data);
		else if (!q_strcasecmp(com_token, "state")) CL_IWMusic_ParseStateBlock(&data);
		else if (!q_strcasecmp(com_token, "rule")) CL_IWMusic_ParseRuleBlock(&data);
	}
	q_free(buf);
}

static const cvar_t *CL_IWMusic_FindCvar (const char *name)
{
	return Cvar_FindVar(name);
}

static qboolean CL_IWMusic_RuleMatches (const iwm_rule_t *r)
{
	if (r->type == IWR_ALWAYS)
		return true;
	if (r->type == IWR_CVAR)
	{
		const cvar_t *cv = CL_IWMusic_FindCvar(r->a);
		return cv && !strcmp(cv->string, r->b);
	}
	if (r->type == IWR_TIME_SINCE_DAMAGE_GT)
	{
		int ms = (int)((realtime - iwm_last_damage_realtime) * 1000.0);
		return iwm_last_damage_realtime > 0 && ms > r->threshold_ms;
	}
	return false;
}

static void CL_IWMusic_BeginGainRamp (float from, float to, int ms)
{
	iwm_gain_from = CLAMP(0.f, from, 1.f);
	iwm_gain_to = CLAMP(0.f, to, 1.f);
	iwm_gain_start = realtime;
	iwm_gain_duration_ms = q_max(1, ms);
}

static void CL_IWMusic_ApplyGainRamp (void)
{
	float t;
	if (iwm_gain_duration_ms <= 1)
		t = 1.f;
	else
		t = CLAMP(0.f, (float)((realtime - iwm_gain_start) * 1000.0 / iwm_gain_duration_ms), 1.f);
	iwm_music_gain = iwm_gain_from + (iwm_gain_to - iwm_gain_from) * t;
}

static void CL_IWMusic_StartSwitch (const char *track, int fade_ms, float target_volume)
{
	q_strlcpy(iwm_pending_track, track, sizeof(iwm_pending_track));
	iwm_target_track_volume = CLAMP(0.f, target_volume, 1.f);
	iwm_switch_pending = true;
	iwm_switch_fadein = false;
	CL_IWMusic_BeginGainRamp(iwm_music_gain, 0.f, q_max(1, fade_ms));
	CL_IWMusic_DPrintf("switching track to %s (fade %dms)\n", track, fade_ms);
}

static void CL_IWMusic_UpdateSwitch (void)
{
	if (!iwm_switch_pending)
		return;
	if (!iwm_switch_fadein)
	{
		if (iwm_music_gain > 0.001f)
			return;
		BGM_Play(iwm_pending_track);
		if (!BGM_HasStream())
		{
			CL_IWMusic_DPrintf("failed to load %s, staying on current stream\n", iwm_pending_track);
			iwm_switch_pending = false;
			CL_IWMusic_BeginGainRamp(iwm_music_gain, 1.f, 300);
			return;
		}
		q_strlcpy(iwm_current_track, iwm_pending_track, sizeof(iwm_current_track));
		iwm_switch_fadein = true;
		CL_IWMusic_BeginGainRamp(0.f, 1.f, 300);
	}
	else if (iwm_music_gain >= 0.999f)
	{
		iwm_switch_pending = false;
	}
}

static void CL_IWMusic_Dump_f (void)
{
	int i;
	Con_Printf("IWMUSIC dump: triggers=%d zones=%d states=%d rules=%d\n", iwm_num_triggers, iwm_num_zones, iwm_num_states, iwm_num_rules);
	for (i = 0; i < iwm_num_triggers; ++i)
		Con_Printf(" trigger[%d] state=%s track=%s pri=%d active=%d\n", i, iwm_triggers[i].state, iwm_triggers[i].track, iwm_triggers[i].priority, iwm_triggers[i].active);
	for (i = 0; i < iwm_num_zones; ++i)
		Con_Printf(" zone[%d] mult=%.2f active=%d\n", i, iwm_zones[i].multiplier, iwm_zones[i].active);
	Con_Printf(" resolved state=%s current_track=%s gain=%.2f duck=%.2f\n", iwm_current_state, iwm_current_track, iwm_music_gain, iwm_zone_duck_current);
}

static void CL_IWMusic_Clear_f (void)
{
	iwm_forced_active = false;
	iwm_forced_state[0] = 0;
	iwm_forced_track[0] = 0;
}

static void CL_IWMusic_State_f (void)
{
	if (Cmd_Argc() < 2)
	{
		Con_Printf("iwmusic_state <name> [fade_ms]\n");
		return;
	}
	iwm_forced_active = true;
	q_strlcpy(iwm_forced_state, Cmd_Argv(1), sizeof(iwm_forced_state));
	iwm_forced_track[0] = 0;
	iwm_forced_fade_ms = (Cmd_Argc() >= 3) ? atoi(Cmd_Argv(2)) : 500;
}

static void CL_IWMusic_Track_f (void)
{
	if (Cmd_Argc() < 2)
	{
		Con_Printf("iwmusic_track <path> [fade_ms] [volume]\n");
		return;
	}
	iwm_forced_active = true;
	iwm_forced_state[0] = 0;
	q_strlcpy(iwm_forced_track, Cmd_Argv(1), sizeof(iwm_forced_track));
	iwm_forced_fade_ms = (Cmd_Argc() >= 3) ? atoi(Cmd_Argv(2)) : 500;
	iwm_forced_volume = (Cmd_Argc() >= 4) ? CLAMP(0.f, atof(Cmd_Argv(3)), 1.f) : 1.f;
}

static void CL_IWMusic_Reload_f (void)
{
	iwm_num_states = 0;
	iwm_num_rules = 0;
	iwm_has_defaults = false;
	CL_IWMusic_LoadEventFile();
}

void CL_IWMusic_NotifyDamage (void)
{
	iwm_last_damage_realtime = realtime;
}

void CL_IWMusic_NewMap (void)
{
	CL_IWMusic_ResetRuntime();
	CL_IWMusic_ParseMapEntities();
	CL_IWMusic_LoadEventFile();
}

void CL_IWMusic_Disconnect (void)
{
	CL_IWMusic_ResetRuntime();
}

void CL_IWMusic_Init (void)
{
	Cvar_RegisterVariable(&cl_iwmusic_enable);
	Cvar_RegisterVariable(&cl_iwmusic_debug);
	Cvar_RegisterVariable(&cl_iwmusic_eventfile);
	Cvar_RegisterVariable(&cl_iwmusic_basevolume);
	Cmd_AddCommand("iwmusic_state", CL_IWMusic_State_f);
	Cmd_AddCommand("iwmusic_track", CL_IWMusic_Track_f);
	Cmd_AddCommand("iwmusic_clear", CL_IWMusic_Clear_f);
	Cmd_AddCommand("iwmusic_reload", CL_IWMusic_Reload_f);
	Cmd_AddCommand("iwmusic_dump", CL_IWMusic_Dump_f);
	CL_IWMusic_ResetRuntime();
}

void CL_IWMusic_Shutdown (void)
{
	CL_IWMusic_ResetRuntime();
}

void CL_IWMusic_Update (void)
{
	vec3_t pos;
	int i;
	float target_duck = 1.f;
	float attack_lerp, release_lerp;
	int best_pri = -100000;
	int best_enter = -1;
	const char *desired_state = "";
	const char *desired_track = "";
	float desired_volume = 1.f;
	int desired_fade = 500;

	if (!cl_iwmusic_enable.value)
	{
		BGM_SetVolumeScale(1.f);
		return;
	}

	if (!cl.worldmodel || cl.viewentity <= 0)
		return;

	VectorCopy(cl_entities[cl.viewentity].origin, pos);

	for (i = 0; i < iwm_num_triggers; ++i)
	{
		iwm_trigger_t *t = &iwm_triggers[i];
		vec3_t d; VectorSubtract(pos, t->origin, d); qboolean in = (VectorLength(d) <= t->radius);
		if (in && !t->active)
			t->enter_serial = iwm_enter_serial_counter++;
		t->active = in;
		if (!in)
			continue;
		if (t->priority > best_pri || (t->priority == best_pri && t->enter_serial > best_enter))
		{
			best_pri = t->priority;
			best_enter = t->enter_serial;
			desired_state = t->state;
			desired_track = t->track;
			desired_volume = t->volume;
			desired_fade = t->fade_ms;
		}
	}

	for (i = 0; i < iwm_num_zones; ++i)
	{
		iwm_zone_t *z = &iwm_zones[i];
		{ vec3_t d; VectorSubtract(pos, z->origin, d); z->active = (VectorLength(d) <= z->radius); }
		if (z->active)
			target_duck = q_min(target_duck, z->multiplier);
	}

	for (i = 0; i < iwm_num_rules; ++i)
	{
		const iwm_rule_t *r = &iwm_rules[i];
		if (r->priority < best_pri)
			continue;
		if (!CL_IWMusic_RuleMatches(r))
			continue;
		if (r->priority > best_pri)
			best_enter = -1;
		best_pri = r->priority;
		desired_state = r->then_state;
		desired_track = "";
	}

	if (iwm_forced_active)
	{
		best_pri = 1000;
		desired_state = iwm_forced_state;
		desired_track = iwm_forced_track;
		desired_volume = iwm_forced_volume;
		desired_fade = iwm_forced_fade_ms;
	}
	else if (best_pri < -100 && iwm_has_defaults)
	{
		best_pri = -100;
		desired_state = iwm_defaults.name;
		desired_track = iwm_defaults.track;
		desired_volume = iwm_defaults.volume;
		desired_fade = iwm_defaults.fade_ms;
	}

	if ((!desired_track || !desired_track[0]) && desired_state && desired_state[0])
	{
		const iwm_state_t *st = CL_IWMusic_FindState(desired_state);
		if (st)
		{
			desired_track = st->track;
			desired_volume = st->volume;
			desired_fade = st->fade_ms;
		}
	}

	if (desired_state && desired_state[0] && q_strcasecmp(iwm_current_state, desired_state))
	{
		q_strlcpy(iwm_current_state, desired_state, sizeof(iwm_current_state));
		CL_IWMusic_DPrintf("state => %s (pri %d)\n", iwm_current_state, best_pri);
	}

	if (desired_track && desired_track[0] && q_strcasecmp(desired_track, iwm_current_track) && !iwm_switch_pending)
		CL_IWMusic_StartSwitch(desired_track, desired_fade, desired_volume);

	attack_lerp = CLAMP(0.f, host_frametime / 0.150f, 1.f);
	release_lerp = CLAMP(0.f, host_frametime / 0.300f, 1.f);
	if (target_duck < iwm_zone_duck_current)
		iwm_zone_duck_current += (target_duck - iwm_zone_duck_current) * attack_lerp;
	else
		iwm_zone_duck_current += (target_duck - iwm_zone_duck_current) * release_lerp;

	CL_IWMusic_ApplyGainRamp();
	CL_IWMusic_UpdateSwitch();
	BGM_SetVolumeScale(CLAMP(0.f, cl_iwmusic_basevolume.value, 1.f) * desired_volume * iwm_zone_duck_current * iwm_music_gain);
}

/*
Acceptance tests:
1) cl_iwmusic_enable 0: behavior remains unchanged (no IWMUSIC logs/music changes).
2) Trigger map marker with state="combat" track="music/combat.ogg" radius=256 crossfades on enter and falls back on exit.
3) Zone marker with volume=0.3 radius=512 ducks smoothly in/out.
4) Eventfile cvar rule: cl_iwmusic_combat 1 -> combat; cl_iwmusic_combat 0 -> calm.
5) iwmusic_state boss forced override wins over all other sources.
6) iwmusic_reload reloads event file without crash.
*/
