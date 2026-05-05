#include "quakedef.h"

#include "r_framegraph.h"
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

extern cvar_t r_gl_state_validate;
extern cvar_t r_framegraph_autobind;
extern cvar_t r_framegraph_debug;
extern cvar_t r_framegraph_pass_debug;
extern cvar_t r_speeds;
extern cvar_t r_ref_enable_postfx;
extern cvar_t r_srgb_framebuffer;
qboolean R_Shadow_Enabled (void);

void R_RegisterFrameGraphPasses (void);

enum
{
	FG_MAX_RUNTIME_PASSES = 64,
	FG_MAX_PROFILE_SLOTS = R_BACKEND_MAX_PROFILE_SLOTS
};

typedef struct fg_runtime_pass_entry_s
{
	const RenderPassDesc *desc;
	int profile_slot;
} fg_runtime_pass_entry_t;

typedef struct framegraph_pass_stats_s
{
	double cpu_ms;
	double cpu_avg_ms;
	double gpu_ms;
	double gpu_avg_ms;
	unsigned cpu_samples;
	unsigned gpu_samples;
} framegraph_pass_stats_t;

typedef struct framegraph_channel_stats_s
{
	double cpu_ms;
	double cpu_avg_ms;
	unsigned cpu_samples;
} framegraph_channel_stats_t;

static framegraph_pass_stats_t s_pass_stats[FG_MAX_PROFILE_SLOTS];
static framegraph_channel_stats_t s_channel_stats[FG_PASS_STATS_COUNT];
static char s_profile_slot_names[FG_MAX_PROFILE_SLOTS][64];
static const RenderPassDesc *s_profile_slot_descs[FG_MAX_PROFILE_SLOTS];
static int s_profile_slot_count = 0;
static int s_last_stats_print = -120;
static RenderFramePlan s_cached_plan;
static int s_cached_plan_frame = -1;
static fg_runtime_pass_entry_t s_runtime_passes[FG_MAX_RUNTIME_PASSES];
static int s_runtime_pass_count = 0;
static qboolean s_pass_registration_locked = false;
static unsigned s_cycle_warning_signature = 0u;
static qboolean s_cycle_warning_emitted = false;
static int s_pass_baseline_autobind_warn_frame = -1;
static int s_frameplan_log_frame = -1;

static qboolean FG_AutobindEnabled (void)
{
	const float value = Cvar_VariableValue ("r_framegraph_autobind");
	return value > 0.f;
}

static void FG_FatalErrorLogAndExit (const char *fmt, ...)
{
	char message[1024];
	va_list args;
	FILE *f = NULL;

	va_start (args, fmt);
	q_vsnprintf (message, sizeof (message), fmt, args);
	va_end (args);

#if defined(_WIN32)
	if (fopen_s (&f, "C:\\Quake\\rerelease\\framegraph_fatal.log", "a") != 0)
		f = NULL;
#else
	f = fopen ("C:\\Quake\\rerelease\\framegraph_fatal.log", "a");
#endif
	if (f)
	{
		fprintf (f, "FrameGraph fatal: %s\n", message);
		fclose (f);
	}

	Sys_Printf ("FrameGraph fatal: %s\n", message);
	Host_Shutdown ();
	exit (1);
}

static qboolean FG_AddRuntimePassInternal (const RenderPassDesc *pass_desc);
static int FG_FindOrCreateProfileSlot (const RenderPassDesc *pass_desc);
static int FG_MoveSetupStagePassesToFront (void);
static void FG_SortRuntimePassesTopologically (int first_pass, int pass_count);
static void FG_ConsumeBackendTimerSamples (const IRenderBackend *backend);

static const unsigned s_fg_resource_bits[] = {
	RENDER_RES_SCENE_COLOR,
	RENDER_RES_SCENE_DEPTH,
	RENDER_RES_COMPOSITE_COLOR,
	RENDER_RES_COMPOSITE_DEPTH,
	RENDER_RES_SHADOW_SUN_DEPTH,
	RENDER_RES_VELOCITY,
	RENDER_RES_DECALS,
	RENDER_RES_SSAO_FOG_STATE
};
static render_backend_resource_state_t s_resource_states[Q_COUNTOF (s_fg_resource_bits)];

static qboolean FG_QueryResourceBinding (unsigned bit, render_backend_resource_slot_t *out_slot, qboolean *out_requires_backend_resource)
{
	return R_Backend_GetFrameGraphResourceBinding (bit, out_slot, out_requires_backend_resource);
}

static qboolean FG_GetResourceSlotForBit (unsigned bit, render_backend_resource_slot_t *out_slot)
{
	return FG_QueryResourceBinding (bit, out_slot, NULL);
}

static int FG_FindResourceMappingIndex (unsigned bit)
{
	int i;
	for (i = 0; i < (int)Q_COUNTOF (s_fg_resource_bits); ++i)
	{
		if (s_fg_resource_bits[i] == bit)
			return i;
	}
	return -1;
}

static const char *FG_GetResourceBitName (unsigned bit)
{
	switch (bit)
	{
	case RENDER_RES_SCENE_COLOR: return "scene_color";
	case RENDER_RES_SCENE_DEPTH: return "scene_depth";
	case RENDER_RES_COMPOSITE_COLOR: return "composite_color";
	case RENDER_RES_COMPOSITE_DEPTH: return "composite_depth";
	case RENDER_RES_SHADOW_SUN_DEPTH: return "shadow_sun_depth";
	case RENDER_RES_VELOCITY: return "velocity";
	case RENDER_RES_DECALS: return "decals";
	case RENDER_RES_SSAO_FOG_STATE: return "ssao_fog_state";
	default: return "unknown";
	}
}

static const char *FG_GetOutputTargetName (unsigned output_target)
{
	switch (output_target)
	{
	case FG_PASS_OUTPUT_KEEP: return "keep";
	case FG_PASS_OUTPUT_BACKBUFFER: return "backbuffer";
	case FG_PASS_OUTPUT_SCENE_FBO: return "scene_fbo";
	case FG_PASS_OUTPUT_COMPOSITE_FBO: return "composite_fbo";
	case FG_PASS_OUTPUT_AUTO_SCENE: return "auto_scene";
	case FG_PASS_OUTPUT_AUTO_WARP: return "auto_warp";
	default: return "unknown";
	}
}

static const char *FG_GetViewportModeName (unsigned viewport_mode)
{
	switch (viewport_mode)
	{
	case FG_PASS_VIEWPORT_KEEP: return "keep";
	case FG_PASS_VIEWPORT_FULL_WINDOW: return "full_window";
	case FG_PASS_VIEWPORT_VIEW_RECT: return "view_rect";
	case FG_PASS_VIEWPORT_VIEW_RECT_SCALED: return "view_rect_scaled";
	default: return "unknown";
	}
}

static const char *FG_GetLoadOpName (render_backend_load_op_t load_op)
{
	switch (load_op)
	{
	case R_BACKEND_LOAD_OP_LOAD: return "load";
	case R_BACKEND_LOAD_OP_CLEAR: return "clear";
	case R_BACKEND_LOAD_OP_DONT_CARE: return "dont_care";
	default: return "unknown";
	}
}

static const char *FG_GetStoreOpName (render_backend_store_op_t store_op)
{
	switch (store_op)
	{
	case R_BACKEND_STORE_OP_STORE: return "store";
	case R_BACKEND_STORE_OP_DONT_CARE: return "dont_care";
	default: return "unknown";
	}
}

static void FG_FormatResourceBits (unsigned bits, char *out, size_t out_size)
{
	unsigned bit;
	qboolean first = true;

	if (!out || out_size == 0)
		return;
	out[0] = '\0';

	for (bit = 1u; bit != 0; bit <<= 1)
	{
		if ((bits & bit) == 0u)
			continue;
		if (!first)
			q_strlcat (out, "|", out_size);
		q_strlcat (out, FG_GetResourceBitName (bit), out_size);
		first = false;
	}

	if (first)
		q_strlcat (out, "none", out_size);
}

static const char *FG_GetSideEffectBitName (unsigned bit)
{
	switch (bit)
	{
	case FG_SIDEFX_GLOBAL_STATE: return "global_state";
	case FG_SIDEFX_TEMPORAL_HISTORY: return "temporal_history";
	case FG_SIDEFX_CPU_SIM_UPDATE: return "cpu_sim_update";
	default: return "unknown";
	}
}

static void FG_FormatSideEffectBits (unsigned bits, char *out, size_t out_size)
{
	unsigned bit;
	qboolean first = true;

	if (!out || out_size == 0)
		return;
	out[0] = '\0';

	for (bit = 1u; bit != 0; bit <<= 1)
	{
		if ((bits & bit) == 0u)
			continue;
		if (!first)
			q_strlcat (out, "|", out_size);
		q_strlcat (out, FG_GetSideEffectBitName (bit), out_size);
		first = false;
	}

	if (first)
		q_strlcat (out, "none", out_size);
}

static void FG_ResolvePassOutputAndViewport (const RenderPassDesc *pass, const RenderPassContext *ctx,
	unsigned *out_output_target, unsigned *out_viewport_mode)
{
	unsigned output_target;
	unsigned viewport_mode;

	if (!pass)
		return;

	output_target = pass->output_target;
	viewport_mode = pass->viewport_mode;

	if (output_target == FG_PASS_OUTPUT_AUTO_SCENE)
	{
		if (ctx && ctx->frame_plan && ctx->frame_plan->needs_scene_effects)
		{
			output_target = FG_PASS_OUTPUT_SCENE_FBO;
			viewport_mode = FG_PASS_VIEWPORT_VIEW_RECT_SCALED;
		}
		else
		{
			qboolean needs_post = (ctx && ctx->frame_plan) ? ctx->frame_plan->needs_postprocess : false;
			output_target = needs_post ? FG_PASS_OUTPUT_COMPOSITE_FBO : FG_PASS_OUTPUT_BACKBUFFER;
			viewport_mode = FG_PASS_VIEWPORT_VIEW_RECT;
		}
	}
	else if (output_target == FG_PASS_OUTPUT_AUTO_WARP)
	{
		if (ctx && ctx->frame_plan && ctx->frame_plan->needs_scene_effects)
		{
			output_target = ctx->frame_plan->needs_postprocess ? FG_PASS_OUTPUT_COMPOSITE_FBO : FG_PASS_OUTPUT_BACKBUFFER;
			viewport_mode = FG_PASS_VIEWPORT_VIEW_RECT;
		}
		else
		{
			output_target = FG_PASS_OUTPUT_KEEP;
			viewport_mode = FG_PASS_VIEWPORT_KEEP;
		}
	}

	if (out_output_target)
		*out_output_target = output_target;
	if (out_viewport_mode)
		*out_viewport_mode = viewport_mode;
}

static void FG_DebugPrintPassInfo (const RenderPassDesc *pass, const RenderPassContext *ctx)
{
	char reads_buf[128];
	char writes_buf[128];
	char sidefx_buf[128];
	unsigned output_target = FG_PASS_OUTPUT_KEEP;
	unsigned viewport_mode = FG_PASS_VIEWPORT_KEEP;

	if (!pass || r_framegraph_pass_debug.value <= 0.f)
		return;

	FG_FormatResourceBits (pass->reads, reads_buf, sizeof (reads_buf));
	FG_FormatResourceBits (pass->writes, writes_buf, sizeof (writes_buf));
	FG_FormatSideEffectBits (pass->side_effects, sidefx_buf, sizeof (sidefx_buf));
	FG_ResolvePassOutputAndViewport (pass, ctx, &output_target, &viewport_mode);

	Con_DPrintf ("FrameGraph pass: name='%s' reads=%s writes=%s sidefx=%s output_target=%s viewport_mode=%s\n",
		pass->name ? pass->name : "<unnamed>",
		reads_buf,
		writes_buf,
		sidefx_buf,
		FG_GetOutputTargetName (output_target),
		FG_GetViewportModeName (viewport_mode));

	if (pass->color_attachments && pass->num_color_attachments > 0)
	{
		unsigned i;
		for (i = 0; i < pass->num_color_attachments; ++i)
		{
			Con_DPrintf ("FrameGraph pass: name='%s' color_attachment[%u] resource=%s load=%s store=%s\n",
				pass->name ? pass->name : "<unnamed>",
				i,
				FG_GetResourceBitName (pass->color_attachments[i].resource_bit),
				FG_GetLoadOpName (pass->color_attachments[i].load_op),
				FG_GetStoreOpName (pass->color_attachments[i].store_op));
		}
	}

	if (pass->depth_attachment)
	{
		Con_DPrintf ("FrameGraph pass: name='%s' depth_attachment resource=%s load=%s store=%s\n",
			pass->name ? pass->name : "<unnamed>",
			FG_GetResourceBitName (pass->depth_attachment->resource_bit),
			FG_GetLoadOpName (pass->depth_attachment->load_op),
			FG_GetStoreOpName (pass->depth_attachment->store_op));
	}
}

static void FG_DebugPrintResolvedSlots (const RenderPassDesc *pass, const RenderPassContext *ctx)
{
	unsigned bit;

	if (!pass || !ctx || !ctx->resources || r_framegraph_pass_debug.value <= 0.f)
		return;

	for (bit = 1u; bit != 0; bit <<= 1)
	{
		render_backend_resource_slot_t slot;
		unsigned resolved;

		if (((pass->reads | pass->writes) & bit) == 0u)
			continue;
		if (!FG_GetResourceSlotForBit (bit, &slot) || slot == R_BACKEND_RESOURCE_SLOT_NONE)
			continue;

		resolved = R_FrameGraph_ResolveResourceBySlot (ctx->resources, slot);
		Con_DPrintf ("FrameGraph pass: name='%s' slot_resolve resource=%s slot=%d resolved=%u access=%s%s\n",
			pass->name ? pass->name : "<unnamed>",
			FG_GetResourceBitName (bit),
			(int)slot,
			resolved,
			(pass->reads & bit) ? "r" : "",
			(pass->writes & bit) ? "w" : "");
	}
}

static const char *FG_GetResourceStateName (render_backend_resource_state_t state)
{
	switch (state)
	{
	case R_BACKEND_RESOURCE_STATE_UNKNOWN: return "unknown";
	case R_BACKEND_RESOURCE_STATE_ATTACHMENT_READ: return "attachment_read";
	case R_BACKEND_RESOURCE_STATE_ATTACHMENT_WRITE: return "attachment_write";
	case R_BACKEND_RESOURCE_STATE_ATTACHMENT_READ_WRITE: return "attachment_read_write";
	case R_BACKEND_RESOURCE_STATE_SAMPLED: return "sampled";
	case R_BACKEND_RESOURCE_STATE_STORAGE_READ: return "storage_read";
	case R_BACKEND_RESOURCE_STATE_STORAGE_WRITE: return "storage_write";
	case R_BACKEND_RESOURCE_STATE_PRESENT: return "present";
	default: return "invalid";
	}
}

static render_backend_resource_state_t FG_GetReadStateForBit (unsigned bit)
{
	(void)bit;
	return R_BACKEND_RESOURCE_STATE_SAMPLED;
}

static render_backend_resource_state_t FG_GetWriteStateForBit (unsigned bit)
{
	(void)bit;
	return R_BACKEND_RESOURCE_STATE_ATTACHMENT_WRITE;
}

static render_backend_resource_state_t FG_GetReadWriteStateForBit (unsigned bit)
{
	(void)bit;
	return R_BACKEND_RESOURCE_STATE_ATTACHMENT_READ_WRITE;
}

static qboolean FG_PassHasAttachmentForBit (const RenderPassDesc *pass_desc, unsigned bit)
{
	unsigned i;

	if (!pass_desc)
		return false;

	if (pass_desc->color_attachments && pass_desc->num_color_attachments > 0)
	{
		for (i = 0; i < pass_desc->num_color_attachments; ++i)
		{
			if (pass_desc->color_attachments[i].resource_bit == bit)
				return true;
		}
	}

	if (pass_desc->depth_attachment && pass_desc->depth_attachment->resource_bit == bit)
		return true;

	return false;
}

static void FG_ResetResourceStates (void)
{
	memset (s_resource_states, 0, sizeof (s_resource_states));
}

static void FG_EmitPassBarriers (const RenderPassDesc *pass, RenderPassContext *ctx)
{
	RenderBackendResourceBarrier barriers[16];
	unsigned barrier_count = 0;
	unsigned bit;

	if (!pass || !ctx || !ctx->resources)
		return;

	for (bit = 1u; bit != 0; bit <<= 1)
	{
		render_backend_resource_state_t desired_state = R_BACKEND_RESOURCE_STATE_UNKNOWN;
		const render_backend_resource_ref_t *resource_ref;
		int mapping_index;
		render_backend_resource_state_t before_state;
		render_backend_resource_slot_t slot = R_BACKEND_RESOURCE_SLOT_NONE;
		qboolean requires_backend_resource = false;

		if (((pass->reads | pass->writes) & bit) == 0u)
			continue;

		if (!FG_QueryResourceBinding (bit, &slot, &requires_backend_resource) || !requires_backend_resource)
			continue;

		if ((pass->writes & bit) != 0u)
		{
			if ((pass->reads & bit) != 0u)
				desired_state = FG_GetReadWriteStateForBit (bit);
			else
				desired_state = FG_GetWriteStateForBit (bit);
		}
		else if ((pass->reads & bit) != 0u)
		{
			desired_state = FG_GetReadStateForBit (bit);
		}

		if (desired_state == R_BACKEND_RESOURCE_STATE_UNKNOWN)
			continue;

		resource_ref = R_FrameGraph_GetResourceRef (ctx->resources, slot);
		if (!resource_ref || (ctx->backend && ctx->backend->is_resource_valid
			&& !ctx->backend->is_resource_valid (ctx->resources, resource_ref)))
			continue;

		mapping_index = FG_FindResourceMappingIndex (bit);
		if (mapping_index < 0)
			continue;
		before_state = s_resource_states[mapping_index];
		if (before_state == desired_state)
			continue;

		if (barrier_count >= Q_COUNTOF (barriers))
			break;
		barriers[barrier_count].resource = resource_ref;
		barriers[barrier_count].before = before_state;
		barriers[barrier_count].after = desired_state;
		if (r_framegraph_debug.value > 0.f)
		{
			Con_DPrintf ("FrameGraph barrier: pass='%s' resource='%s' %s -> %s\n",
				pass->name ? pass->name : "<unnamed>",
				FG_GetResourceBitName (bit),
				FG_GetResourceStateName (before_state),
				FG_GetResourceStateName (desired_state));
		}
		s_resource_states[mapping_index] = desired_state;
		barrier_count++;
	}

	if (barrier_count > 0)
		R_Backend_ResourceBarrier (ctx->resources, barriers, barrier_count);
}

static qboolean FG_ValidatePassResourceDecls (const RenderPassDesc *pass_desc, qboolean emit_warning)
{
	unsigned bit;
	unsigned i;

	if (!pass_desc)
		return false;

	for (bit = 1u; bit != 0; bit <<= 1)
	{
		render_backend_resource_slot_t slot;
		qboolean requires_backend_resource;
		if (((pass_desc->reads | pass_desc->writes) & bit) == 0u)
			continue;
		if (!FG_QueryResourceBinding (bit, &slot, &requires_backend_resource))
		{
			if (emit_warning)
				Con_Warning ("FrameGraph: pass '%s' uses unmapped resource bit 0x%x\n",
					pass_desc->name ? pass_desc->name : "<unnamed>", bit);
			FG_FatalErrorLogAndExit ("pass '%s' uses unmapped resource bit 0x%x",
				pass_desc->name ? pass_desc->name : "<unnamed>", bit);
		}
	}

	for (bit = 1u; bit != 0; bit <<= 1)
	{
		render_backend_resource_slot_t slot;
		qboolean requires_backend_resource;
		if ((pass_desc->writes & bit) == 0u)
			continue;
		if (!FG_QueryResourceBinding (bit, &slot, &requires_backend_resource) || !requires_backend_resource)
			continue;
		if (!FG_PassHasAttachmentForBit (pass_desc, bit))
		{
			if (emit_warning || r_framegraph_debug.value > 0.f)
			{
				Con_Warning ("FrameGraph: pass '%s' writes '%s' without declaring it as pass attachment\n",
					pass_desc->name ? pass_desc->name : "<unnamed>",
					FG_GetResourceBitName (bit));
			}
			FG_FatalErrorLogAndExit ("pass '%s' writes '%s' without pass attachment declaration",
				pass_desc->name ? pass_desc->name : "<unnamed>",
				FG_GetResourceBitName (bit));
		}
	}

	if (pass_desc->color_attachments && pass_desc->num_color_attachments > 0)
	{
		for (i = 0; i < pass_desc->num_color_attachments; ++i)
		{
			unsigned resource_bit = pass_desc->color_attachments[i].resource_bit;
			render_backend_resource_slot_t slot = R_BACKEND_RESOURCE_SLOT_NONE;
			qboolean requires_backend_resource = false;

			if (!FG_QueryResourceBinding (resource_bit, &slot, &requires_backend_resource)
				|| !requires_backend_resource
				|| slot == R_BACKEND_RESOURCE_SLOT_NONE)
			{
				if (emit_warning)
					Con_Warning ("FrameGraph: pass '%s' color attachment[%u] does not map to backend resource\n",
						pass_desc->name ? pass_desc->name : "<unnamed>", i);
				FG_FatalErrorLogAndExit ("pass '%s' color attachment[%u] has no backend resource mapping",
					pass_desc->name ? pass_desc->name : "<unnamed>", i);
			}
			if ((pass_desc->writes & resource_bit) == 0u)
			{
				if (emit_warning)
					Con_Warning ("FrameGraph: pass '%s' color attachment[%u] must be declared in writes mask\n",
						pass_desc->name ? pass_desc->name : "<unnamed>", i);
				FG_FatalErrorLogAndExit ("pass '%s' color attachment[%u] missing in writes mask",
					pass_desc->name ? pass_desc->name : "<unnamed>", i);
			}
			if ((pass_desc->reads & resource_bit) != 0u
				&& pass_desc->color_attachments[i].load_op == R_BACKEND_LOAD_OP_DONT_CARE)
			{
				if (emit_warning || r_framegraph_debug.value > 0.f)
					Con_Warning ("FrameGraph: pass '%s' color attachment[%u] reads and writes '%s' but load_op is DONT_CARE\n",
						pass_desc->name ? pass_desc->name : "<unnamed>",
						i,
						FG_GetResourceBitName (resource_bit));
				FG_FatalErrorLogAndExit ("pass '%s' color attachment[%u] has invalid DONT_CARE load_op for read/write",
					pass_desc->name ? pass_desc->name : "<unnamed>", i);
			}
		}
	}

	if (pass_desc->depth_attachment)
	{
		unsigned resource_bit = pass_desc->depth_attachment->resource_bit;
		render_backend_resource_slot_t slot = R_BACKEND_RESOURCE_SLOT_NONE;
		qboolean requires_backend_resource = false;

		if (!FG_QueryResourceBinding (resource_bit, &slot, &requires_backend_resource)
			|| !requires_backend_resource
			|| slot == R_BACKEND_RESOURCE_SLOT_NONE)
		{
			if (emit_warning)
				Con_Warning ("FrameGraph: pass '%s' depth attachment does not map to backend resource\n",
					pass_desc->name ? pass_desc->name : "<unnamed>");
			FG_FatalErrorLogAndExit ("pass '%s' depth attachment has no backend resource mapping",
				pass_desc->name ? pass_desc->name : "<unnamed>");
		}
		if ((pass_desc->writes & resource_bit) == 0u)
		{
			if (emit_warning)
				Con_Warning ("FrameGraph: pass '%s' depth attachment must be declared in writes mask\n",
					pass_desc->name ? pass_desc->name : "<unnamed>");
			FG_FatalErrorLogAndExit ("pass '%s' depth attachment missing in writes mask",
				pass_desc->name ? pass_desc->name : "<unnamed>");
		}
		if ((pass_desc->reads & resource_bit) != 0u
			&& pass_desc->depth_attachment->load_op == R_BACKEND_LOAD_OP_DONT_CARE)
		{
			if (emit_warning || r_framegraph_debug.value > 0.f)
				Con_Warning ("FrameGraph: pass '%s' depth attachment reads and writes '%s' but load_op is DONT_CARE\n",
					pass_desc->name ? pass_desc->name : "<unnamed>",
					FG_GetResourceBitName (resource_bit));
			FG_FatalErrorLogAndExit ("pass '%s' depth attachment has invalid DONT_CARE load_op for read/write",
				pass_desc->name ? pass_desc->name : "<unnamed>");
		}
	}

	return true;
}

static qboolean FG_AddRuntimePassInternal (const RenderPassDesc *pass_desc)
{
	int profile_slot;

	if (!pass_desc || !pass_desc->name || !pass_desc->execute)
		return false;
	if (!FG_ValidatePassResourceDecls (pass_desc, true))
		return false;
	if (s_runtime_pass_count >= FG_MAX_RUNTIME_PASSES)
	{
		Con_Warning ("FrameGraph: pass capacity reached (%d), cannot add '%s'\n",
			FG_MAX_RUNTIME_PASSES,
			pass_desc->name ? pass_desc->name : "<unnamed>");
		return false;
	}

	profile_slot = FG_FindOrCreateProfileSlot (pass_desc);
	s_runtime_passes[s_runtime_pass_count].desc = pass_desc;
	s_runtime_passes[s_runtime_pass_count].profile_slot = profile_slot;
	s_runtime_pass_count++;
	return true;
}

void R_FrameGraph_ResetPasses (void)
{
	s_runtime_pass_count = 0;
	s_pass_registration_locked = false;
}

qboolean R_FrameGraph_AddPass (const RenderPassDesc *pass_desc)
{
	if (s_pass_registration_locked)
	{
		Con_Warning ("FrameGraph: registration closed for current frame, ignoring pass '%s'\n",
			(pass_desc && pass_desc->name) ? pass_desc->name : "<unnamed>");
		return false;
	}

	return FG_AddRuntimePassInternal (pass_desc);
}

static int FG_FindOrCreateProfileSlot (const RenderPassDesc *pass_desc)
{
	int i;
	const char *pass_name;

	if (!pass_desc || !pass_desc->name || !pass_desc->name[0])
		return -1;
	pass_name = pass_desc->name;

	for (i = 0; i < s_profile_slot_count; ++i)
	{
		if (s_profile_slot_descs[i] == pass_desc)
			return i;
	}

	if (s_profile_slot_count >= FG_MAX_PROFILE_SLOTS)
	{
		Con_Warning ("FrameGraph: profile slot capacity reached (%d), '%s' will be untimed\n",
			FG_MAX_PROFILE_SLOTS,
			pass_name);
		return -1;
	}

	s_profile_slot_descs[s_profile_slot_count] = pass_desc;
	q_strlcpy (s_profile_slot_names[s_profile_slot_count], pass_name, sizeof (s_profile_slot_names[0]));
	s_profile_slot_count++;
	return s_profile_slot_count - 1;
}

static int FG_MoveSetupStagePassesToFront (void)
{
	fg_runtime_pass_entry_t reordered[FG_MAX_RUNTIME_PASSES];
	int setup_count = 0;
	int main_count = 0;
	int i;

	memset (reordered, 0, sizeof (reordered));

	for (i = 0; i < s_runtime_pass_count; ++i)
	{
		const fg_runtime_pass_entry_t *entry = &s_runtime_passes[i];
		qboolean is_setup = entry->desc && entry->desc->stage == FG_PASS_STAGE_SETUP;
		if (is_setup)
			reordered[setup_count++] = *entry;
	}

	for (i = 0; i < s_runtime_pass_count; ++i)
	{
		const fg_runtime_pass_entry_t *entry = &s_runtime_passes[i];
		qboolean is_setup = entry->desc && entry->desc->stage == FG_PASS_STAGE_SETUP;
		if (!is_setup)
			reordered[setup_count + main_count++] = *entry;
	}

	if (setup_count > 0)
		memcpy (s_runtime_passes, reordered, sizeof (reordered[0]) * s_runtime_pass_count);

	return setup_count;
}

static void FG_SortRuntimePassesTopologically (int first_pass, int pass_count)
{
	unsigned long long incoming[FG_MAX_RUNTIME_PASSES];
	int indegree[FG_MAX_RUNTIME_PASSES];
	qboolean emitted[FG_MAX_RUNTIME_PASSES];
	fg_runtime_pass_entry_t sorted[FG_MAX_RUNTIME_PASSES];
	unsigned cycle_signature = 2166136261u;
	int total;
	int out_index;
	int i;

	if (first_pass < 0)
		first_pass = 0;
	if (pass_count > s_runtime_pass_count)
		pass_count = s_runtime_pass_count;
	if (pass_count <= first_pass + 1)
		return;

	total = pass_count - first_pass;
	if (total > FG_MAX_RUNTIME_PASSES)
		total = FG_MAX_RUNTIME_PASSES;

	memset (incoming, 0, sizeof (incoming));
	memset (indegree, 0, sizeof (indegree));
	memset (emitted, 0, sizeof (emitted));
	memset (sorted, 0, sizeof (sorted));

	for (i = 0; i < total; ++i)
	{
		const RenderPassDesc *pass = s_runtime_passes[first_pass + i].desc;
		const char *name = (pass && pass->name) ? pass->name : "";
		int c;

		/* Stable warning dedup key: pass order + declared read/write masks. */
		cycle_signature ^= (unsigned)(uintptr_t)pass;
		cycle_signature *= 16777619u;
		if (pass)
		{
			cycle_signature ^= (unsigned)pass->reads;
			cycle_signature *= 16777619u;
			cycle_signature ^= (unsigned)pass->writes;
			cycle_signature *= 16777619u;
		}
		for (c = 0; name[c]; ++c)
		{
			cycle_signature ^= (unsigned char)name[c];
			cycle_signature *= 16777619u;
		}

		indegree[i] = 0;
	}

	for (i = 0; i < total; ++i)
	{
		const RenderPassDesc *pass_a = s_runtime_passes[first_pass + i].desc;
		int j;

		if (!pass_a)
			continue;

		for (j = i + 1; j < total; ++j)
		{
			const RenderPassDesc *pass_b = s_runtime_passes[first_pass + j].desc;
			qboolean a_to_b;
			qboolean b_to_a;
			qboolean waw_conflict;

			if (!pass_b)
				continue;

			a_to_b = (pass_a->writes & pass_b->reads) != 0;
			b_to_a = (pass_b->writes & pass_a->reads) != 0;
			waw_conflict = (pass_a->writes & pass_b->writes) != 0;

			if (a_to_b && b_to_a)
			{
				/* If only one pass has side effects, prefer data-only pass first.
				 * Otherwise keep registration order for stability. */
				if ((pass_a->side_effects != 0) != (pass_b->side_effects != 0))
				{
					if (pass_a->side_effects != 0)
					{
						if (r_framegraph_debug.value > 0.f)
						{
							char sidefx_buf[96];
							FG_FormatSideEffectBits (pass_a->side_effects, sidefx_buf, sizeof (sidefx_buf));
							Con_DPrintf ("FrameGraph sort: '%s' delayed after '%s' (read/write cycle, sidefx=%s)\n",
								pass_a->name ? pass_a->name : "<unnamed>",
								pass_b->name ? pass_b->name : "<unnamed>",
								sidefx_buf);
						}
						incoming[i] |= (1ull << j);
						indegree[i]++;
					}
					else
					{
						if (r_framegraph_debug.value > 0.f)
						{
							char sidefx_buf[96];
							FG_FormatSideEffectBits (pass_b->side_effects, sidefx_buf, sizeof (sidefx_buf));
							Con_DPrintf ("FrameGraph sort: '%s' delayed after '%s' (read/write cycle, sidefx=%s)\n",
								pass_b->name ? pass_b->name : "<unnamed>",
								pass_a->name ? pass_a->name : "<unnamed>",
								sidefx_buf);
						}
						incoming[j] |= (1ull << i);
						indegree[j]++;
					}
				}
				else
				{
					incoming[j] |= (1ull << i);
					indegree[j]++;
				}
			}
			else if (a_to_b)
			{
				incoming[j] |= (1ull << i);
				indegree[j]++;
			}
			else if (b_to_a)
			{
				/* WAR hazard (later pass writes what an earlier pass reads):
				 * keep registration order so the read observes the pre-overwrite value. */
				incoming[j] |= (1ull << i);
				indegree[j]++;
			}
			else if (waw_conflict)
			{
				/* Preserve registration order for writes to the same resource. */
				incoming[j] |= (1ull << i);
				indegree[j]++;
			}
		}
	}

	for (out_index = 0; out_index < total; ++out_index)
	{
		int ready = -1;
		int j;

		for (j = 0; j < total; ++j)
		{
			if (!emitted[j] && indegree[j] == 0)
			{
				ready = j;
				break;
			}
		}

		if (ready < 0)
		{
			if (r_framegraph_debug.value > 0.f
				&& (!s_cycle_warning_emitted || s_cycle_warning_signature != cycle_signature))
			{
				Con_Warning ("FrameGraph: pass dependency cycle detected (including sidefx ordering), keeping registration order (further identical warnings suppressed)\n");
				s_cycle_warning_signature = cycle_signature;
				s_cycle_warning_emitted = true;
			}
			return;
		}

		sorted[out_index] = s_runtime_passes[first_pass + ready];
		emitted[ready] = true;

		for (j = 0; j < total; ++j)
		{
			if (!emitted[j] && (incoming[j] & (1ull << ready)))
			{
				if (indegree[j] > 0)
					indegree[j]--;
			}
		}
	}

	for (i = 0; i < total; ++i)
		s_runtime_passes[first_pass + i] = sorted[i];
}

static void FG_ConsumeBackendTimerSamples (const IRenderBackend *backend)
{
	const RenderBackendCaps *caps = R_Backend_GetCaps ();
	int i;

	if (!backend || !backend->consume_timer_sample)
		return;
	if (!caps || !caps->supports_timestamps)
		return;

	for (i = 0; i < s_profile_slot_count; ++i)
	{
		double gpu_ms = 0.0;
		framegraph_pass_stats_t *stats = &s_pass_stats[i];
		if (!backend->consume_timer_sample (i, &gpu_ms))
			continue;
		stats->gpu_ms = gpu_ms;
		stats->gpu_avg_ms = (stats->gpu_samples == 0) ? gpu_ms : (stats->gpu_avg_ms * 0.8 + gpu_ms * 0.2);
		stats->gpu_samples++;
	}
}

static void FG_BuildResourceHandles (RenderGraphResourceHandle *out_handles)
{
	if (!out_handles)
		return;

	R_Backend_PopulateFrameGraphResources (out_handles);
}

static unsigned long long FG_BuildActivePassMask (const RenderPassContext *ctx, int first_pass, int pass_count)
{
	unsigned long long active_mask = 0;
	unsigned needed_resources = RENDER_RES_NONE;
	int i;

	if (first_pass < 0)
		first_pass = 0;
	if (pass_count < 0)
		pass_count = 0;
	if (pass_count > FG_MAX_RUNTIME_PASSES)
		pass_count = FG_MAX_RUNTIME_PASSES;
	if (first_pass >= pass_count)
		return 0;

	for (i = pass_count - 1; i >= first_pass; --i)
	{
		const RenderPassDesc *pass = s_runtime_passes[i].desc;
		unsigned long long pass_bit = 1ull << i;
		qboolean enabled = (!pass->enabled || pass->enabled (ctx));

		if (!enabled)
			continue;

		if (pass->side_effects)
		{
			if ((pass->writes & needed_resources) == 0u
				&& r_framegraph_debug.value > 0.f)
			{
				char sidefx_buf[96];
				char needed_buf[128];
				FG_FormatSideEffectBits (pass->side_effects, sidefx_buf, sizeof (sidefx_buf));
				FG_FormatResourceBits (needed_resources, needed_buf, sizeof (needed_buf));
				Con_DPrintf ("FrameGraph prune: keeping '%s' for sidefx-only execution (sidefx=%s, needed=%s)\n",
					pass->name ? pass->name : "<unnamed>",
					sidefx_buf,
					needed_buf);
			}
			active_mask |= pass_bit;
			needed_resources = (needed_resources & ~pass->writes) | pass->reads;
			continue;
		}

		if (pass->writes & needed_resources)
		{
			active_mask |= pass_bit;
			needed_resources = (needed_resources & ~pass->writes) | pass->reads;
		}
	}

	return active_mask;
}

static void FG_DebugPrintPrunedPasses (unsigned long long active_mask, int first_pass, int pass_count)
{
	int i;

	if (r_gl_state_validate.value < 2.f)
		return;
	if (r_framegraph_debug.value <= 0.f)
		return;
	if (pass_count < 0)
		pass_count = 0;
	if (pass_count > FG_MAX_RUNTIME_PASSES)
		pass_count = FG_MAX_RUNTIME_PASSES;

	for (i = first_pass; i < pass_count; ++i)
	{
		unsigned long long pass_bit = 1ull << i;
		if ((active_mask & pass_bit) == 0)
			Con_DPrintf ("FrameGraph prune: skipped '%s'\n", s_runtime_passes[i].desc->name);
	}
}

static void FG_AccumulateCPUStats (int pass_id, double cpu_ms)
{
	framegraph_pass_stats_t *stats;

	if (pass_id < 0 || pass_id >= s_profile_slot_count)
		return;

	stats = &s_pass_stats[pass_id];
	stats->cpu_ms = cpu_ms;
	stats->cpu_avg_ms = (stats->cpu_samples == 0) ? cpu_ms : (stats->cpu_avg_ms * 0.8 + cpu_ms * 0.2);
	stats->cpu_samples++;
}

static void FG_AccumulateChannelCPUStats (int channel, double cpu_ms)
{
	framegraph_channel_stats_t *stats;

	if (channel <= FG_PASS_STATS_NONE || channel >= FG_PASS_STATS_COUNT)
		return;

	stats = &s_channel_stats[channel];
	stats->cpu_ms = cpu_ms;
	stats->cpu_avg_ms = (stats->cpu_samples == 0) ? cpu_ms : (stats->cpu_avg_ms * 0.8 + cpu_ms * 0.2);
	stats->cpu_samples++;
}

static void FG_MaybePrintStats (void)
{
	double gpu_channels[FG_PASS_STATS_COUNT];
	int i;

	if (r_framegraph_debug.value <= 0.f)
		return;
	if (r_speeds.value < 3.f)
		return;
	if (host_framecount < s_last_stats_print + 60)
		return;

	memset (gpu_channels, 0, sizeof (gpu_channels));

	for (i = 0; i < s_runtime_pass_count; ++i)
	{
		const fg_runtime_pass_entry_t *entry = &s_runtime_passes[i];
		unsigned channel;
		const framegraph_pass_stats_t *stats;

		if (!entry->desc)
			continue;

		channel = entry->desc->stats_channel;
		if (channel <= FG_PASS_STATS_NONE || channel >= FG_PASS_STATS_COUNT)
			continue;
		if (entry->profile_slot < 0 || entry->profile_slot >= s_profile_slot_count)
			continue;

		stats = &s_pass_stats[entry->profile_slot];
		gpu_channels[channel] += stats->gpu_avg_ms;
	}

	Con_Printf ("FrameGraph CPUms setup=%.2f shadow=%.2f scene=%.2f warp=%.2f fog=%.2f post=%.2f overlay=%.2f | GPUms shadow=%.2f scene=%.2f fog=%.2f post=%.2f\n",
		s_channel_stats[FG_PASS_STATS_SETUP].cpu_avg_ms,
		s_channel_stats[FG_PASS_STATS_SHADOW].cpu_avg_ms,
		s_channel_stats[FG_PASS_STATS_SCENE].cpu_avg_ms,
		s_channel_stats[FG_PASS_STATS_WARP].cpu_avg_ms,
		s_channel_stats[FG_PASS_STATS_FOG].cpu_avg_ms,
		s_channel_stats[FG_PASS_STATS_POST].cpu_avg_ms,
		s_channel_stats[FG_PASS_STATS_OVERLAY].cpu_avg_ms,
		gpu_channels[FG_PASS_STATS_SHADOW],
		gpu_channels[FG_PASS_STATS_SCENE],
		gpu_channels[FG_PASS_STATS_FOG],
		gpu_channels[FG_PASS_STATS_POST]);

	s_last_stats_print = host_framecount;
}

static void FG_ApplyPassBaseline (const RenderPassDesc *pass, const RenderPassContext *ctx)
{
	unsigned baseline_bits = 0u;
	/* TODO_STATE_BASELINE:
	 * Baselines are pass-scoped state expectations delegated to backend. */

	if (pass && pass->baseline_bits != 0u)
		baseline_bits = pass->baseline_bits;

	if ((baseline_bits & FG_PASS_BASELINE_REQUIRE_AUTOBIND) != 0u
		&& !FG_AutobindEnabled ()
		&& s_pass_baseline_autobind_warn_frame != host_framecount)
	{
		Con_DWarning ("FrameGraph: pass baseline requires r_framegraph_autobind 1, but it is disabled\n");
		s_pass_baseline_autobind_warn_frame = host_framecount;
	}

	(void)ctx;
	R_Backend_ApplyFrameGraphBaseline (baseline_bits);
}

static void FG_ApplyPassOutputBinding (const RenderPassDesc *pass, RenderPassContext *ctx)
{
	const render_backend_resource_ref_t *target_resource = NULL;
	const IRenderBackend *backend = ctx ? ctx->backend : NULL;
	RenderBackendSurfaceInfo surface_info;
	int view_x, view_y, view_w, view_h;
	unsigned output_target;
	unsigned viewport_mode;
	qboolean autobind_enabled = FG_AutobindEnabled ();
	qboolean bind_backbuffer = false;
	qboolean bind_target = false;
	/* TODO_PASS_BOUNDARY:
	 * Core resolves output intent (backbuffer/scene/composite), backend owns
	 * native target binding and concrete attachment execution. */

	if (!pass)
		return;
	if (pass->baseline_bits & FG_PASS_BASELINE_REQUIRE_AUTOBIND)
		autobind_enabled = true;
	if (!autobind_enabled)
		return;

	output_target = pass->output_target;
	viewport_mode = pass->viewport_mode;
	FG_ResolvePassOutputAndViewport (pass, ctx, &output_target, &viewport_mode);

	switch (output_target)
	{
	case FG_PASS_OUTPUT_BACKBUFFER:
		bind_target = true;
		bind_backbuffer = true;
		break;
	case FG_PASS_OUTPUT_SCENE_FBO:
		target_resource = R_FrameGraph_GetResourceRef (ctx ? ctx->resources : NULL, R_BACKEND_RESOURCE_SLOT_SCENE_FBO);
		if (target_resource
			&& backend
			&& backend->is_resource_valid
			&& backend->is_resource_valid (ctx->resources, target_resource))
		{
			bind_target = true;
		}
		break;
	case FG_PASS_OUTPUT_COMPOSITE_FBO:
		target_resource = R_FrameGraph_GetResourceRef (ctx ? ctx->resources : NULL, R_BACKEND_RESOURCE_SLOT_COMPOSITE_FBO);
		if (target_resource
			&& backend
			&& backend->is_resource_valid
			&& backend->is_resource_valid (ctx->resources, target_resource))
		{
			bind_target = true;
		}
		break;
	case FG_PASS_OUTPUT_AUTO_SCENE:
	case FG_PASS_OUTPUT_AUTO_WARP:
	case FG_PASS_OUTPUT_KEEP:
	default:
		break;
	}

	if (bind_target)
	{
		if (backend && backend->bind_render_target)
			backend->bind_render_target (ctx ? ctx->resources : NULL, target_resource, bind_backbuffer);
		if (ctx && output_target == FG_PASS_OUTPUT_COMPOSITE_FBO)
			ctx->composite_written_this_frame = true;
	}

	memset (&surface_info, 0, sizeof (surface_info));
	R_Backend_QuerySurfaceInfo (&surface_info);

	switch (viewport_mode)
	{
	case FG_PASS_VIEWPORT_FULL_WINDOW:
		if (backend && backend->set_viewport)
			backend->set_viewport (
				surface_info.surface_x,
				surface_info.surface_y,
				q_max (1, surface_info.surface_width),
				q_max (1, surface_info.surface_height));
		break;
	case FG_PASS_VIEWPORT_VIEW_RECT:
		view_x = surface_info.view_x;
		view_y = surface_info.view_y;
		view_w = q_max (1, surface_info.view_width);
		view_h = q_max (1, surface_info.view_height);
		if (backend && backend->set_viewport)
			backend->set_viewport (view_x, view_y, view_w, view_h);
		break;
	case FG_PASS_VIEWPORT_VIEW_RECT_SCALED:
	{
		/* Scaled scene passes render into offscreen scene targets, so the viewport
		 * origin must be texture-space (0,0), not window/view-rect offset space. */
		view_x = 0;
		view_y = 0;
		view_w = q_max (1, surface_info.scene_width);
		view_h = q_max (1, surface_info.scene_height);
		if (backend && backend->set_viewport)
			backend->set_viewport (view_x, view_y, view_w, view_h);
		break;
	}
	case FG_PASS_VIEWPORT_KEEP:
	default:
		break;
	}
}

static void FG_RunPass (int profile_slot, const RenderPassDesc *pass, RenderPassContext *ctx)
{
	double cpu_start;
	double cpu_ms;
	unsigned bit;
	unsigned output_target = FG_PASS_OUTPUT_KEEP;
	unsigned viewport_mode = FG_PASS_VIEWPORT_KEEP;
	RenderBackendPassAttachmentDesc color_attachments[4];
	RenderBackendPassAttachmentDesc depth_attachment;
	RenderBackendPassDesc backend_pass_desc;
	qboolean has_depth_attachment = false;

	if (!pass || !ctx)
		return;
	if (pass->enabled && !pass->enabled (ctx))
		return;
	if (!FG_ValidatePassResourceDecls (pass, false))
		return;
	FG_ResolvePassOutputAndViewport (pass, ctx, &output_target, &viewport_mode);
	FG_ApplyPassBaseline (pass, ctx);
	FG_DebugPrintPassInfo (pass, ctx);

	for (bit = 1u; bit != 0; bit <<= 1)
	{
		qboolean requires_backend_resource = false;
		render_backend_resource_slot_t slot = R_BACKEND_RESOURCE_SLOT_NONE;
		const render_backend_resource_ref_t *resource_ref;
		if ((pass->reads & bit) == 0)
			continue;
		if (!FG_QueryResourceBinding (bit, &slot, &requires_backend_resource))
		{
			Con_DWarning ("FrameGraph: pass '%s' uses unmapped resource bit 0x%x\n", pass->name, bit);
			FG_FatalErrorLogAndExit ("pass '%s' uses unmapped resource bit 0x%x", pass->name, bit);
		}
		if (!requires_backend_resource)
			continue;

		resource_ref = R_FrameGraph_GetResourceRef (ctx->resources, slot);
		if (!ctx->backend || !ctx->backend->is_resource_valid || !resource_ref
			|| !ctx->backend->is_resource_valid (ctx->resources, resource_ref))
		{
			Con_DWarning ("FrameGraph: pass '%s' read slot %d resolved invalid resource\n", pass->name, (int)slot);
			FG_FatalErrorLogAndExit ("pass '%s' read slot %d resolved invalid resource", pass->name, (int)slot);
		}
	}

	FG_ApplyPassOutputBinding (pass, ctx);

	memset (color_attachments, 0, sizeof (color_attachments));
	memset (&depth_attachment, 0, sizeof (depth_attachment));
	memset (&backend_pass_desc, 0, sizeof (backend_pass_desc));
	backend_pass_desc.name = pass->name;
	backend_pass_desc.resources = ctx->resources;
	backend_pass_desc.backbuffer = (output_target == FG_PASS_OUTPUT_BACKBUFFER);

	if (pass->color_attachments && pass->num_color_attachments > 0)
	{
		unsigned i;
		unsigned color_count = pass->num_color_attachments;
		if (color_count > Q_COUNTOF (color_attachments))
			color_count = Q_COUNTOF (color_attachments);
		for (i = 0; i < color_count; ++i)
		{
			render_backend_resource_slot_t slot = R_BACKEND_RESOURCE_SLOT_NONE;
			if (!FG_GetResourceSlotForBit (pass->color_attachments[i].resource_bit, &slot))
				continue;
			color_attachments[i].resource = R_FrameGraph_GetResourceRef (ctx->resources, slot);
			if (!color_attachments[i].resource || (ctx->backend && ctx->backend->is_resource_valid
				&& !ctx->backend->is_resource_valid (ctx->resources, color_attachments[i].resource)))
			{
				Con_DWarning ("FrameGraph: pass '%s' color attachment[%u] resolved invalid resource\n", pass->name, i);
				FG_FatalErrorLogAndExit ("pass '%s' color attachment[%u] resolved invalid resource", pass->name, i);
			}
			color_attachments[i].load_op = pass->color_attachments[i].load_op;
			color_attachments[i].store_op = pass->color_attachments[i].store_op;
		}
		backend_pass_desc.color_attachments = color_attachments;
		backend_pass_desc.num_color_attachments = color_count;
	}

	if (pass->depth_attachment)
	{
		render_backend_resource_slot_t depth_slot = R_BACKEND_RESOURCE_SLOT_NONE;
		if (FG_GetResourceSlotForBit (pass->depth_attachment->resource_bit, &depth_slot))
		{
			depth_attachment.resource = R_FrameGraph_GetResourceRef (ctx->resources, depth_slot);
			if (!depth_attachment.resource || (ctx->backend && ctx->backend->is_resource_valid
				&& !ctx->backend->is_resource_valid (ctx->resources, depth_attachment.resource)))
			{
				Con_DWarning ("FrameGraph: pass '%s' depth attachment resolved invalid resource\n", pass->name);
				FG_FatalErrorLogAndExit ("pass '%s' depth attachment resolved invalid resource", pass->name);
			}
			depth_attachment.load_op = pass->depth_attachment->load_op;
			depth_attachment.store_op = pass->depth_attachment->store_op;
			has_depth_attachment = true;
		}
	}

	if (has_depth_attachment)
		backend_pass_desc.depth_attachment = &depth_attachment;

	FG_EmitPassBarriers (pass, ctx);
	FG_DebugPrintResolvedSlots (pass, ctx);

	if (ctx->backend && ctx->backend->validate_pass_state)
		ctx->backend->validate_pass_state (pass->name, true);
	R_Backend_BeginPassEx (&backend_pass_desc);
	if (ctx->frame_plan && ctx->frame_plan->run_gpu_timers && ctx->backend && ctx->backend->begin_timer)
		ctx->backend->begin_timer (profile_slot);

	cpu_start = Sys_DoubleTime ();
	pass->execute (ctx);
	cpu_ms = (Sys_DoubleTime () - cpu_start) * 1000.0;
	FG_AccumulateCPUStats (profile_slot, cpu_ms);
	FG_AccumulateChannelCPUStats (pass->stats_channel, cpu_ms);

	if (ctx->frame_plan && ctx->frame_plan->run_gpu_timers && ctx->backend && ctx->backend->end_timer)
		ctx->backend->end_timer (profile_slot);
	R_Backend_EndPassEx ();
	if (ctx->backend && ctx->backend->validate_pass_state)
		ctx->backend->validate_pass_state (pass->name, false);
}

void R_FrameGraph_BuildRenderFramePlan (RenderFramePlan *out_plan)
{
	const RenderBackendCaps *caps;
	RenderBackendSurfaceInfo surface_info;

	if (!out_plan)
		return;

	memset (out_plan, 0, sizeof (*out_plan));
	memset (&surface_info, 0, sizeof (surface_info));
	R_Backend_QuerySurfaceInfo (&surface_info);
	out_plan->needs_scene_effects = surface_info.needs_scene_effects;
	out_plan->needs_postprocess = surface_info.needs_postprocess;
	if (out_plan->needs_scene_effects)
		out_plan->needs_postprocess = true;
	/* Safety: when rendering to a non-sRGB backbuffer with postfx enabled,
	 * ensure postprocess runs to apply output transfer/tonemap. */
	if (r_ref_enable_postfx.value != 0.f && r_srgb_framebuffer.value <= 0.f)
		out_plan->needs_postprocess = true;
	out_plan->run_shadowmaps = R_Shadow_Enabled ();
	out_plan->run_postprocess = out_plan->needs_postprocess;
	out_plan->run_viewmodel = true;
	out_plan->run_polyblend = true;
	out_plan->run_store_prev = true;
	caps = R_Backend_GetCaps ();
	out_plan->run_gpu_timers = (caps && caps->supports_timestamps);

	if (r_framegraph_debug.value > 0.f && s_frameplan_log_frame != host_framecount)
	{
		s_frameplan_log_frame = host_framecount;
		Con_DPrintf ("frameplan: scenefx=%d postfx=%d run_shadow=%d run_post=%d\n",
			out_plan->needs_scene_effects ? 1 : 0,
			out_plan->needs_postprocess ? 1 : 0,
			out_plan->run_shadowmaps ? 1 : 0,
			out_plan->run_postprocess ? 1 : 0);
	}
}

void R_FrameGraph_SetRenderFramePlan (const RenderFramePlan *plan)
{
	if (!plan)
	{
		s_cached_plan_frame = -1;
		memset (&s_cached_plan, 0, sizeof (s_cached_plan));
		return;
	}

	s_cached_plan = *plan;
	s_cached_plan_frame = host_framecount;
}

qboolean R_FrameGraph_GetRenderFramePlan (RenderFramePlan *out_plan)
{
	if (s_cached_plan_frame != host_framecount)
		return false;
	if (out_plan)
	*out_plan = s_cached_plan;
	return true;
}

void R_FrameGraph_GetTimingSummary (double *out_gpu_ms, double *out_cpu_ms, qboolean *out_gpu_valid)
{
	double gpu_total = 0.0;
	double cpu_total = 0.0;
	qboolean gpu_valid = false;
	const IRenderBackend *backend = R_GetRenderBackend ();
	const RenderBackendCaps *caps = R_Backend_GetCaps ();
	int i;

	for (i = 1; i < FG_PASS_STATS_COUNT; ++i)
		cpu_total += s_channel_stats[i].cpu_avg_ms;

	for (i = 0; i < s_runtime_pass_count; ++i)
	{
		const fg_runtime_pass_entry_t *entry = &s_runtime_passes[i];
		unsigned channel;
		const framegraph_pass_stats_t *stats;

		if (!entry->desc)
			continue;

		channel = entry->desc->stats_channel;
		if (channel <= FG_PASS_STATS_NONE || channel >= FG_PASS_STATS_COUNT)
			continue;
		if (entry->profile_slot < 0 || entry->profile_slot >= s_profile_slot_count)
			continue;

		stats = &s_pass_stats[entry->profile_slot];
		gpu_total += stats->gpu_avg_ms;
		if (stats->gpu_samples > 0)
			gpu_valid = true;
	}

	if (!backend || !backend->consume_timer_sample || !caps || !caps->supports_timestamps)
		gpu_valid = false;

	if (out_gpu_ms)
		*out_gpu_ms = gpu_total;
	if (out_cpu_ms)
		*out_cpu_ms = cpu_total;
	if (out_gpu_valid)
		*out_gpu_valid = gpu_valid;
}

/*
 * Framegraph pass order and data dependencies
 * ------------------------------------------
 * 1) Build a deterministic per-frame plan once and cache it.
 * 2) Execute explicit passes with declared contracts and timing instrumentation.
 * 3) Keep postprocess + overlays in the same scheduler so ordering remains stable.
 */
void R_FrameGraph_RenderView (void)
{
	RenderFramePlan frame_plan;
	RenderGraphResourceHandle resources;
	RenderPassContext pass_ctx;
	const RenderBackendCaps *caps;
	unsigned long long active_pass_mask;
	int setup_pass_count;
	int pass_count;
	int i = 0;

	memset (&frame_plan, 0, sizeof (frame_plan));
	FG_BuildResourceHandles (&resources);
	R_FrameGraph_ResetPasses ();
	R_RegisterFrameGraphPasses ();

	pass_ctx.frame_plan = &frame_plan;
	pass_ctx.resources = &resources;
	pass_ctx.backend = R_GetRenderBackend ();
	pass_ctx.composite_written_this_frame = false;
	caps = R_Backend_GetCaps ();

	if (caps && caps->supports_timestamps
		&& pass_ctx.backend && pass_ctx.backend->resolve_timers)
		pass_ctx.backend->resolve_timers ();
	FG_ConsumeBackendTimerSamples (pass_ctx.backend);
	FG_ResetResourceStates ();

	/* Setup-stage passes run before plan build so frame state is current. */
	setup_pass_count = FG_MoveSetupStagePassesToFront ();
	if (setup_pass_count == 0 && r_gl_state_validate.value > 0.f && r_framegraph_debug.value > 0.f)
		Con_DPrintf ("FrameGraph: no setup-stage pass registered\n");
	FG_SortRuntimePassesTopologically (0, setup_pass_count);

	for (i = 0; i < setup_pass_count; ++i)
	{
		const fg_runtime_pass_entry_t *setup_entry = &s_runtime_passes[i];
		FG_RunPass (setup_entry->profile_slot, setup_entry->desc, &pass_ctx);
	}
	i = setup_pass_count;

	FG_BuildResourceHandles (&resources);

	R_FrameGraph_BuildRenderFramePlan (&frame_plan);
	R_FrameGraph_SetRenderFramePlan (&frame_plan);
	pass_count = s_runtime_pass_count;
	FG_SortRuntimePassesTopologically (i, pass_count);
	active_pass_mask = FG_BuildActivePassMask (&pass_ctx, i, pass_count);
	FG_DebugPrintPrunedPasses (active_pass_mask, i, pass_count);
	s_pass_registration_locked = true;
	{
		RenderBackendCommandEncoderDesc encoder_desc;
		encoder_desc.name = "framegraph-main";
		encoder_desc.frame_index = (unsigned)host_framecount;
		encoder_desc.flags = R_BACKEND_COMMAND_ENCODER_DEBUG_LABEL;
		R_Backend_BeginCommandEncoder (&encoder_desc);
	}

	for (; i < pass_count; ++i)
	{
		unsigned long long pass_bit = 1ull << i;
		const fg_runtime_pass_entry_t *entry = &s_runtime_passes[i];
		if ((active_pass_mask & pass_bit) == 0)
			continue;
		FG_RunPass (entry->profile_slot, entry->desc, &pass_ctx);
	}
	R_Backend_EndCommandEncoder ();
	R_Backend_SubmitCommandEncoder ();

	FG_MaybePrintStats ();
}
