/*
debug_core.h -- Modular debug system for Ironwail

Provides channel-based, level-filtered debug output with zero cost
when disabled. Core stays OpenGL-agnostic; renderer subsystems may
use this API freely.

Ownership rules:
  Core/Engine:  provides API, channels, levels, CVar routing
  Core/Engine:  does NOT include GL headers or touch GL state
  Renderer:     may use API, add renderer-specific channels/helpers
  Renderer:     owns GL-state dumps, FBO/texture/shader debug

Copyright (C) 2026 Ironwail developers
*/

#ifndef DEBUG_CORE_H
#define DEBUG_CORE_H

#include "q_stdinc.h"
#include "cvar.h"

/*
==============================================================================	Debug Levels==============================================================================
*/
typedef enum
{
	DBG_LEVEL_OFF	= 0,
	DBG_LEVEL_ERROR	= 1,
	DBG_LEVEL_WARN	= 2,
	DBG_LEVEL_INFO	= 3,
	DBG_LEVEL_VERBOSE	= 4,
	DBG_LEVEL_TRACE	= 5,
} debug_level_t;

/*
==============================================================================	Debug Channels (bitmasks)
==============================================================================
*/
typedef enum
{
	DBG_CH_NONE		= 0,
	DBG_CH_CORE		= (1 << 0),
	DBG_CH_RENDER		= (1 << 1),
	DBG_CH_BACKEND		= (1 << 2),
	DBG_CH_GL		= (1 << 3),
	DBG_CH_FRAMEGRAPH	= (1 << 4),
	DBG_CH_SHADOW		= (1 << 5),
	DBG_CH_FOGVOL		= (1 << 6),
	DBG_CH_TEXTURE		= (1 << 7),
	DBG_CH_SHADER		= (1 << 8),
	DBG_CH_FBO		= (1 << 9),
	DBG_CH_CVAR		= (1 << 10),
	DBG_CH_PERF		= (1 << 11),
	DBG_CH_AUDIO		= (1 << 12),
	DBG_CH_NET		= (1 << 13),
	DBG_CH_FILE		= (1 << 14),
	DBG_CH_PHYSICS		= (1 << 15),
	DBG_CH_BOT		= (1 << 16),
	DBG_CH_ALL		= 0x7FFFFFFF,
} debug_channel_t;

/*
==============================================================================	CVars (declared in debug_core.c)
==============================================================================
*/
extern cvar_t	debug_enable;
extern cvar_t	dbg_level;
extern cvar_t	dbg_channels;
extern cvar_t	dbg_timestamps;
extern cvar_t	dbg_frame_numbers;

/*
==============================================================================	Core API functions
==============================================================================
*/

void DBG_Init (void);

qboolean DBG_ChannelEnabled (int channel);
int	DBG_GetLevel (void);

void DBG_Output (int level, int channel, const char *fmt, ...) FUNC_PRINTF(3,4);

void DBG_OutputOnce (int level, int channel, const char *fmt, ...) FUNC_PRINTF(3,4);

void DBG_OutputRate (int level, int channel, float seconds, const char *fmt, ...) FUNC_PRINTF(4,5);

/*
==============================================================================	Macros -- zero cost when channel/level is disabled
==============================================================================
*/

#define DBG_ERROR(channel, fmt, ...) \
	do { \
		if (debug_enable.value != 0.f && DBG_ChannelEnabled(channel) && DBG_GetLevel() >= DBG_LEVEL_ERROR) \
			DBG_Output(DBG_LEVEL_ERROR, channel, fmt, ##__VA_ARGS__); \
	} while (0)

#define DBG_WARN(channel, fmt, ...) \
	do { \
		if (debug_enable.value != 0.f && DBG_ChannelEnabled(channel) && DBG_GetLevel() >= DBG_LEVEL_WARN) \
			DBG_Output(DBG_LEVEL_WARN, channel, fmt, ##__VA_ARGS__); \
	} while (0)

#define DBG_INFO(channel, fmt, ...) \
	do { \
		if (debug_enable.value != 0.f && DBG_ChannelEnabled(channel) && DBG_GetLevel() >= DBG_LEVEL_INFO) \
			DBG_Output(DBG_LEVEL_INFO, channel, fmt, ##__VA_ARGS__); \
	} while (0)

#define DBG_VERBOSE(channel, fmt, ...) \
	do { \
		if (debug_enable.value != 0.f && DBG_ChannelEnabled(channel) && DBG_GetLevel() >= DBG_LEVEL_VERBOSE) \
			DBG_Output(DBG_LEVEL_VERBOSE, channel, fmt, ##__VA_ARGS__); \
	} while (0)

#define DBG_TRACE(channel, fmt, ...) \
	do { \
		if (debug_enable.value != 0.f && DBG_ChannelEnabled(channel) && DBG_GetLevel() >= DBG_LEVEL_TRACE) \
			DBG_Output(DBG_LEVEL_TRACE, channel, fmt, ##__VA_ARGS__); \
	} while (0)

#define DBG_ONCE(channel, level, fmt, ...) \
	do { \
		if (debug_enable.value != 0.f && DBG_ChannelEnabled(channel) && DBG_GetLevel() >= (level)) \
			DBG_OutputOnce(level, channel, fmt, ##__VA_ARGS__); \
	} while (0)

#define DBG_RATE(channel, level, seconds, fmt, ...) \
	do { \
		if (debug_enable.value != 0.f && DBG_ChannelEnabled(channel) && DBG_GetLevel() >= (level)) \
			DBG_OutputRate(level, channel, seconds, fmt, ##__VA_ARGS__); \
	} while (0)

#define DBG_ASSERT_MSG(condition, channel, fmt, ...) \
	do { \
		if (!(condition) && debug_enable.value != 0.f) \
			DBG_Output(DBG_LEVEL_ERROR, channel, "ASSERT FAILED: " fmt, ##__VA_ARGS__); \
	} while (0)

#endif /* DEBUG_CORE_H */
