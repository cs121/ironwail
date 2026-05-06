/*
r_shadow_debug.h -- Shadow subsystem debug helpers

Uses the core debug system (debug_core.h) for shadow-specific
debug output. Provides convenience wrappers that bridge legacy
r_shadow_log CVar with the new DBG_CH_SHADOW channel.

Ownership: Renderer/ref_gl only. May include GL headers.
Copyright (C) 2026 Ironwail developers
*/

#ifndef R_SHADOW_DEBUG_H
#define R_SHADOW_DEBUG_H

#include "debug_core.h"

/*
Shadow logging is enabled when:
  - Legacy r_shadow_log > 0, OR
  - New debug_enable != 0 AND dbg_channels includes DBG_CH_SHADOW
*/
#define SHADOW_LOG_ENABLED() \
	(r_shadow_log.value > 0.f || \
	 (debug_enable.value != 0.f && DBG_ChannelEnabled(DBG_CH_SHADOW)))

#define SHADOW_LOG_VERBOSE_ENABLED() \
	(r_shadow_log.value > 1.f || \
	 (debug_enable.value != 0.f && DBG_ChannelEnabled(DBG_CH_SHADOW) && DBG_GetLevel() >= DBG_LEVEL_VERBOSE))

#endif /* R_SHADOW_DEBUG_H */
