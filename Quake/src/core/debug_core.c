/*
debug_core.c -- Modular debug system implementation

Provides channel-based, level-filtered debug output.
All output routes through Con_Printf / Con_Warning to respect
existing console and logging infrastructure.

Copyright (C) 2026 Ironwail developers
*/

#include "quakedef.h"
#include "debug_core.h"

/*
==============================================================================	CVars==============================================================================
*/
cvar_t	debug_enable		= { "debug_enable", "0", CVAR_NONE };
cvar_t	dbg_level		= { "dbg_level", "3", CVAR_NONE };
cvar_t	dbg_channels		= { "dbg_channels", "0", CVAR_NONE };
cvar_t	dbg_timestamps		= { "dbg_timestamps", "0", CVAR_NONE };
cvar_t	dbg_frame_numbers	= { "dbg_frame_numbers", "0", CVAR_NONE };

/*
==============================================================================	Channel enable check
==============================================================================
*/
qboolean DBG_ChannelEnabled (int channel)
{
	int mask = (int)dbg_channels.value;
	if (mask == 0)
		return false;
	if (mask == DBG_CH_ALL)
		return true;
	return (channel & mask) != 0;
}

/*
==============================================================================	Level check
==============================================================================
*/
int DBG_GetLevel (void)
{
	return (int)dbg_level.value;
}

/*
==============================================================================	Once-tracking ring buffer

Stores hashes of format strings that have already been emitted.
Fixed-size ring so we never allocate at runtime.
==============================================================================
*/
#define DBG_ONCE_CAPACITY 128

static unsigned int	dbg_once_hashes[DBG_ONCE_CAPACITY];
static int		dbg_once_count;

static qboolean DBG_CheckOnce (const char *fmt)
{
	unsigned int h;
	int		i;
	const unsigned char *p;

	h = 5381;
	for (p = (const unsigned char *)fmt; *p; p++)
		h = ((h << 5) + h) + *p;

	for (i = 0; i < dbg_once_count; i++)
	{
		if (dbg_once_hashes[i] == h)
			return false;
	}

	if (dbg_once_count < DBG_ONCE_CAPACITY)
		dbg_once_hashes[dbg_once_count++] = h;
	return true;
}

/*
==============================================================================	Rate-limit tracking

Per-channel last-emit time. Uses realtime from quakedef.h.
==============================================================================
*/
#define DBG_RATE_SLOTS 16

typedef struct
{
	int		channel;
	double		last_time;
} dbg_rate_slot_t;

static dbg_rate_slot_t	dbg_rate_slots[DBG_RATE_SLOTS];
static int		dbg_rate_slot_count;

static qboolean DBG_CheckRate (int channel, float seconds)
{
	int	i;
	double	now = realtime;
	double	threshold = (double)seconds;

	for (i = 0; i < dbg_rate_slot_count; i++)
	{
		if (dbg_rate_slots[i].channel == channel)
		{
			if (now - dbg_rate_slots[i].last_time < threshold)
				return false;
			dbg_rate_slots[i].last_time = now;
			return true;
		}
	}

	if (dbg_rate_slot_count < DBG_RATE_SLOTS)
	{
		dbg_rate_slots[dbg_rate_slot_count].channel = channel;
		dbg_rate_slots[dbg_rate_slot_count].last_time = now;
		dbg_rate_slot_count++;
	}
	return true;
}

/*
==============================================================================	Prefix helpers
==============================================================================
*/
static const char *DBG_LevelPrefix (int level)
{
	switch (level)
	{
	case DBG_LEVEL_ERROR:	return "^1[DBG_ERR]";
	case DBG_LEVEL_WARN:	return "^3[DBG_WRN]";
	case DBG_LEVEL_INFO:	return "[DBG_INF]";
	case DBG_LEVEL_VERBOSE:	return "^2[DBG_VRB]";
	case DBG_LEVEL_TRACE:	return "^5[DBG_TRC]";
	default:		return "[DBG]";
	}
}

static void DBG_PrintWithPrefix (int level, int channel, const char *text)
{
	const char *prefix = DBG_LevelPrefix(level);

#ifdef RENDERER_PLUGIN_BUILD
	(void)channel;
	Con_Printf("%s%s", prefix, text);
#else
	if (dbg_frame_numbers.value != 0.f)
		Con_Printf("%s[f:%d] %s", prefix, host_framecount, text);
	else
		Con_Printf("%s%s", prefix, text);
#endif
}

/*
==============================================================================	Core output functions
==============================================================================
*/
void DBG_Output (int level, int channel, const char *fmt, ...)
{
	va_list	argptr;
	char	msg[1024];
	int	current_level;

	current_level = DBG_GetLevel ();
	if (current_level <= DBG_LEVEL_OFF || level > current_level)
		return;

	va_start(argptr, fmt);
	q_vsnprintf(msg, sizeof(msg), fmt, argptr);
	va_end(argptr);

	if (level <= DBG_LEVEL_ERROR)
	{
		const char *prefix = DBG_LevelPrefix(level);
		Con_Warning("%s %s", prefix + 2, msg);
	}
	else if (level <= DBG_LEVEL_WARN)
	{
		const char *prefix = DBG_LevelPrefix(level);
		Con_Warning("%s %s", prefix + 2, msg);
	}
	else
	{
		DBG_PrintWithPrefix(level, channel, msg);
	}
}

void DBG_OutputOnce (int level, int channel, const char *fmt, ...)
{
	va_list	argptr;
	char	msg[1024];

	if (!DBG_CheckOnce(fmt))
		return;

	va_start(argptr, fmt);
	q_vsnprintf(msg, sizeof(msg), fmt, argptr);
	va_end(argptr);

	DBG_Output(level, channel, "%s", msg);
}

void DBG_OutputRate (int level, int channel, float seconds, const char *fmt, ...)
{
	va_list	argptr;
	char	msg[1024];

	if (!DBG_CheckRate(channel, seconds))
		return;

	va_start(argptr, fmt);
	q_vsnprintf(msg, sizeof(msg), fmt, argptr);
	va_end(argptr);

	DBG_Output(level, channel, "%s", msg);
}

/*
==============================================================================	Init
==============================================================================
*/
void DBG_Init (void)
{
	Cvar_RegisterVariable(&debug_enable);
	Cvar_RegisterVariable(&dbg_level);
	Cvar_RegisterVariable(&dbg_channels);
	Cvar_RegisterVariable(&dbg_timestamps);
	Cvar_RegisterVariable(&dbg_frame_numbers);

	dbg_once_count = 0;
	dbg_rate_slot_count = 0;
}
