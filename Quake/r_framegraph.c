#include "quakedef.h"

#include "r_framegraph.h"

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

static render_backend_resource_ref_t FG_MakeResourceRef (render_backend_resource_type_t type, render_backend_resource_slot_t slot);
static qboolean FG_AddRuntimePassInternal (const RenderPassDesc *pass_desc);
static int FG_FindOrCreateProfileSlot (const RenderPassDesc *pass_desc);
static int FG_MoveSetupStagePassesToFront (void);
static void FG_SortRuntimePassesTopologically (int first_pass, int pass_count);
static void FG_ConsumeBackendTimerSamples (const IRenderBackend *backend);

static qboolean FG_AddRuntimePassInternal (const RenderPassDesc *pass_desc)
{
	int profile_slot;

	if (!pass_desc || !pass_desc->name || !pass_desc->execute)
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
						incoming[i] |= (1ull << j);
						indegree[i]++;
					}
					else
					{
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
				Con_Warning ("FrameGraph: pass dependency cycle detected, keeping registration order (further identical warnings suppressed)\n");
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

static render_backend_resource_ref_t FG_MakeResourceRef (render_backend_resource_type_t type, render_backend_resource_slot_t slot)
{
	render_backend_resource_ref_t ref;
	ref.type = (unsigned char)type;
	ref.slot = (unsigned short)slot;
	return ref;
}

static void FG_ConsumeBackendTimerSamples (const IRenderBackend *backend)
{
	int i;

	if (!backend || !backend->consume_timer_sample)
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

	memset (out_handles, 0, sizeof (*out_handles));
	out_handles->refs[R_BACKEND_RESOURCE_SLOT_SCENE_FBO] = FG_MakeResourceRef (R_BACKEND_RESOURCE_FRAMEBUFFER, R_BACKEND_RESOURCE_SLOT_SCENE_FBO);
	out_handles->refs[R_BACKEND_RESOURCE_SLOT_SCENE_COLOR] = FG_MakeResourceRef (R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_SCENE_COLOR);
	out_handles->refs[R_BACKEND_RESOURCE_SLOT_SCENE_VELOCITY] = FG_MakeResourceRef (R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_SCENE_VELOCITY);
	out_handles->refs[R_BACKEND_RESOURCE_SLOT_SCENE_DEPTH] = FG_MakeResourceRef (R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_SCENE_DEPTH);
	out_handles->refs[R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_FBO] = FG_MakeResourceRef (R_BACKEND_RESOURCE_FRAMEBUFFER, R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_FBO);
	out_handles->refs[R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_COLOR] = FG_MakeResourceRef (R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_COLOR);
	out_handles->refs[R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_VELOCITY] = FG_MakeResourceRef (R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_RESOLVED_SCENE_VELOCITY);
	out_handles->refs[R_BACKEND_RESOURCE_SLOT_COMPOSITE_FBO] = FG_MakeResourceRef (R_BACKEND_RESOURCE_FRAMEBUFFER, R_BACKEND_RESOURCE_SLOT_COMPOSITE_FBO);
	out_handles->refs[R_BACKEND_RESOURCE_SLOT_COMPOSITE_COLOR] = FG_MakeResourceRef (R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_COMPOSITE_COLOR);
	out_handles->refs[R_BACKEND_RESOURCE_SLOT_COMPOSITE_DEPTH] = FG_MakeResourceRef (R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_COMPOSITE_DEPTH);
	out_handles->refs[R_BACKEND_RESOURCE_SLOT_SHADOW_SUN_DEPTH] = FG_MakeResourceRef (R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_SHADOW_SUN_DEPTH);
	out_handles->refs[R_BACKEND_RESOURCE_SLOT_VELOCITY] = FG_MakeResourceRef (R_BACKEND_RESOURCE_TEXTURE, R_BACKEND_RESOURCE_SLOT_VELOCITY);
	out_handles->scene_samples = framebufs.scene.samples;
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
	if (r_framecount < s_last_stats_print + 60)
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

	s_last_stats_print = r_framecount;
}

static void FG_ApplyPassOutputBinding (const RenderPassDesc *pass, const RenderPassContext *ctx)
{
	const render_backend_resource_ref_t *target_resource = NULL;
	const IRenderBackend *backend = ctx ? ctx->backend : NULL;
	int view_x, view_y, view_w, view_h;
	unsigned output_target;
	unsigned viewport_mode;
	qboolean bind_backbuffer = false;
	qboolean bind_target = false;

	if (r_framegraph_autobind.value <= 0.f)
		return;
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
	}

	switch (viewport_mode)
	{
	case FG_PASS_VIEWPORT_FULL_WINDOW:
		if (backend && backend->set_viewport)
			backend->set_viewport (glx, gly, glwidth, glheight);
		break;
	case FG_PASS_VIEWPORT_VIEW_RECT:
		view_x = glx + r_refdef.vrect.x;
		view_y = gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height;
		view_w = q_max (1, r_refdef.vrect.width);
		view_h = q_max (1, r_refdef.vrect.height);
		if (backend && backend->set_viewport)
			backend->set_viewport (view_x, view_y, view_w, view_h);
		break;
	case FG_PASS_VIEWPORT_VIEW_RECT_SCALED:
	{
		/* Scaled scene passes render into offscreen scene targets, so the viewport
		 * origin must be texture-space (0,0), not window/view-rect offset space. */
		view_x = 0;
		view_y = 0;
		view_w = R_GetSceneRenderWidth ();
		view_h = R_GetSceneRenderHeight ();
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

	if (!pass || !ctx)
		return;
	if (pass->enabled && !pass->enabled (ctx))
		return;

	FG_ApplyPassOutputBinding (pass, ctx);

	if (ctx->backend && ctx->backend->validate_pass_state)
		ctx->backend->validate_pass_state (pass->name, true);
	if (ctx->backend && ctx->backend->begin_pass)
		ctx->backend->begin_pass (pass->name);
	if (ctx->backend && ctx->backend->begin_timer)
		ctx->backend->begin_timer (profile_slot);

	cpu_start = Sys_DoubleTime ();
	pass->execute (ctx);
	cpu_ms = (Sys_DoubleTime () - cpu_start) * 1000.0;
	FG_AccumulateCPUStats (profile_slot, cpu_ms);
	FG_AccumulateChannelCPUStats (pass->stats_channel, cpu_ms);

	if (ctx->backend && ctx->backend->end_timer)
		ctx->backend->end_timer (profile_slot);
	if (ctx->backend && ctx->backend->end_pass)
		ctx->backend->end_pass ();
	if (ctx->backend && ctx->backend->validate_pass_state)
		ctx->backend->validate_pass_state (pass->name, false);
}

void R_FrameGraph_BuildRenderFramePlan (RenderFramePlan *out_plan)
{
	if (!out_plan)
		return;

	memset (out_plan, 0, sizeof (*out_plan));
	out_plan->needs_scene_effects = GL_NeedsSceneEffects ();
	out_plan->needs_postprocess = GL_NeedsPostprocess ();
	out_plan->run_shadowmaps = (r_shadow.value > 0.f);
	out_plan->run_postprocess = out_plan->needs_postprocess;
	out_plan->run_viewmodel = true;
	out_plan->run_polyblend = true;
	out_plan->run_store_prev = true;
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
	s_cached_plan_frame = r_framecount;
}

qboolean R_FrameGraph_GetRenderFramePlan (RenderFramePlan *out_plan)
{
	if (s_cached_plan_frame != r_framecount)
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

	if (!backend || !backend->consume_timer_sample)
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

	if (pass_ctx.backend && pass_ctx.backend->resolve_timers)
		pass_ctx.backend->resolve_timers ();
	FG_ConsumeBackendTimerSamples (pass_ctx.backend);

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

	R_FrameGraph_BuildRenderFramePlan (&frame_plan);
	R_FrameGraph_SetRenderFramePlan (&frame_plan);
	pass_count = s_runtime_pass_count;
	FG_SortRuntimePassesTopologically (i, pass_count);
	active_pass_mask = FG_BuildActivePassMask (&pass_ctx, i, pass_count);
	FG_DebugPrintPrunedPasses (active_pass_mask, i, pass_count);
	s_pass_registration_locked = true;

	for (; i < pass_count; ++i)
	{
		unsigned long long pass_bit = 1ull << i;
		const fg_runtime_pass_entry_t *entry = &s_runtime_passes[i];
		if ((active_pass_mask & pass_bit) == 0)
			continue;
		FG_RunPass (entry->profile_slot, entry->desc, &pass_ctx);
	}

	FG_MaybePrintStats ();
}
