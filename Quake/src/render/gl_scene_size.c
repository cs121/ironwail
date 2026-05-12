/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "quakedef.h"
#include "glquake.h"
#include "r_framegraph.h"

extern cvar_t vid_fsaa;

typedef enum scene_scale_source_e
{
	SCENE_SCALE_SOURCE_MANUAL = 0,
	SCENE_SCALE_SOURCE_DYNAMIC
} scene_scale_source_t;

typedef struct render_scene_size_state_s
{
	int native_width;
	int native_height;
	int scene_width;
	int scene_height;
	int scene_scale;
	float scene_resolution_ratio;
	int prev_scene_width;
	int prev_scene_height;
	int prev_scene_scale;
	float prev_scene_resolution_ratio;
	scene_scale_source_t scale_source;
	scene_scale_source_t prev_scale_source;
	qboolean size_changed;
	qboolean scale_changed;
	qboolean source_changed;
	qboolean initialized;
} render_scene_size_state_t;

typedef struct render_drs_state_s
{
	float dynamic_resolution;
	float filtered_ms;
	float last_raw_ms;
	qboolean initialized;
	qboolean last_gpu_valid;
	int upscale_cooldown;
	int down_steps_this_second;
	float down_rate_accumulator;
} render_drs_state_t;

static render_scene_size_state_t r_scene_size_state;
static render_drs_state_t r_drs_state;
static int r_scene_resize_generation = 0;
static qboolean r_scene_resize_pending_invalidation = false;

static float R_GetDRSMinResolution (void)
{
	return CLAMP (0.01f, r_drs_min_scale.value * 0.01f, 1.f);
}

static float R_GetDRSMaxResolution (void)
{
	float min_resolution = R_GetDRSMinResolution ();
	return CLAMP (min_resolution, r_drs_max_scale.value * 0.01f, 1.f);
}

static float R_QuantizeResolution (float ratio)
{
	float step = CLAMP (0.01f, 0.02f, 0.10f);
	return floorf (ratio / step + 0.5f) * step;
}

static const char *R_GetSceneScaleSourceNameInternal (scene_scale_source_t source)
{
	return source == SCENE_SCALE_SOURCE_DYNAMIC ? "dynamic" : "manual";
}

void R_GetSceneRenderTargetAllocationSize (int native_w, int native_h, int scene_w, int scene_h, int *out_alloc_w, int *out_alloc_h)
{
	int alloc_w = q_max (1, scene_w);
	int alloc_h = q_max (1, scene_h);

	if (r_drs.value > 0.f)
	{
		alloc_w = q_max (alloc_w, q_max (1, native_w));
		alloc_h = q_max (alloc_h, q_max (1, native_h));
	}

	if (out_alloc_w)
		*out_alloc_w = alloc_w;
	if (out_alloc_h)
		*out_alloc_h = alloc_h;
}

void R_UpdateDynamicResolutionScale (void)
{
	double gpu_ms = 0.0;
	double cpu_ms = 0.0;
	qboolean gpu_valid = false;
	double raw_ms = 0.0;
	float target_ms;
	float alpha;
	float hysteresis_ms;
	float min_resolution;
	float max_resolution;
	float step;
	int cooldown_after_down;
	int cooldown_after_up;
	float current_resolution;
	float new_resolution;
	int debug_level;
	qboolean use_gpu = (r_drs_use_gpu.value != 0.f);
	qboolean use_gpu_timing = false;
	const int max_down_rate = 4;

	if (r_drs.value <= 0.f)
	{
		memset (&r_drs_state, 0, sizeof (r_drs_state));
		return;
	}

	R_FrameGraph_GetTimingSummary (&gpu_ms, &cpu_ms, &gpu_valid);
	use_gpu_timing = (use_gpu && gpu_valid && gpu_ms > 0.0);
	raw_ms = use_gpu_timing ? gpu_ms : ((host_frametime > 0.0) ? host_frametime * 1000.0 : cpu_ms);
	if (raw_ms <= 0.0)
		raw_ms = 0.001;

	if (r_drs_target_fps.value > 1.f)
		target_ms = 1000.f / r_drs_target_fps.value;
	else
		target_ms = 1000.f / 60.f;
	target_ms = CLAMP (1.f, target_ms, 1000.f);

	alpha = CLAMP (0.01f, r_drs_filter_alpha.value, 1.f);
	hysteresis_ms = q_max (0.f, r_drs_hysteresis_ms.value);
	min_resolution = R_GetDRSMinResolution ();
	max_resolution = R_GetDRSMaxResolution ();
	step = CLAMP (0.01f, r_drs_step_size.value, 0.10f);
	cooldown_after_down = CLAMP (0, (int)Q_rint (r_drs_cooldown_after_down.value), 120);
	cooldown_after_up = CLAMP (0, (int)Q_rint (r_drs_cooldown_after_up.value), 120);
	debug_level = (int)Q_rint (r_drs_debug.value);

	if (!r_drs_state.initialized)
	{
		r_drs_state.dynamic_resolution = max_resolution;
		r_drs_state.filtered_ms = (float)raw_ms;
		r_drs_state.last_raw_ms = (float)raw_ms;
		r_drs_state.last_gpu_valid = use_gpu_timing;
		r_drs_state.initialized = true;
		r_drs_state.upscale_cooldown = 0;
		r_drs_state.down_steps_this_second = 0;
		r_drs_state.down_rate_accumulator = 0.f;
		if (debug_level > 0)
			Con_DPrintf ("drs init: res=%.0f%% raw_ms=%.2f source=%s target=%.2f\n",
				r_drs_state.dynamic_resolution * 100.f, (float)raw_ms, use_gpu_timing ? "gpu" : "frame", target_ms);
		return;
	}

	r_drs_state.filtered_ms = r_drs_state.filtered_ms * (1.f - alpha) + (float)raw_ms * alpha;
	r_drs_state.last_raw_ms = (float)raw_ms;
	r_drs_state.last_gpu_valid = use_gpu_timing;

	if (host_frametime > 0.0)
	{
		r_drs_state.down_rate_accumulator += (float)host_frametime;
		if (r_drs_state.down_rate_accumulator >= 1.f)
		{
			r_drs_state.down_steps_this_second = 0;
			r_drs_state.down_rate_accumulator -= 1.f;
		}
	}

	current_resolution = CLAMP (min_resolution, r_drs_state.dynamic_resolution, max_resolution);
	new_resolution = current_resolution;

	if (r_drs_state.filtered_ms > target_ms + hysteresis_ms)
	{
		if (r_drs_state.down_steps_this_second < max_down_rate)
		{
			new_resolution = q_max (min_resolution, current_resolution - step);
			r_drs_state.down_steps_this_second++;
		}
		r_drs_state.upscale_cooldown = cooldown_after_down;
	}
	else if (r_drs_state.filtered_ms < target_ms - hysteresis_ms)
	{
		if (r_drs_state.upscale_cooldown > 0)
			r_drs_state.upscale_cooldown--;
		else
		{
			new_resolution = max_resolution;
			r_drs_state.upscale_cooldown = cooldown_after_up;
		}
	}
	else if (r_drs_state.upscale_cooldown > 0)
	{
		r_drs_state.upscale_cooldown--;
	}

	if (new_resolution != current_resolution && debug_level > 0)
	{
		Con_DPrintf ("drs res: %.0f%% -> %.0f%% (%dx%d) raw_ms=%.2f filtered_ms=%.2f target_ms=%.2f source=%s\n",
			current_resolution * 100.f, new_resolution * 100.f,
			(int)ceilf (q_max (1, r_refdef.vrect.width) * new_resolution),
			(int)ceilf (q_max (1, r_refdef.vrect.height) * new_resolution),
			(float)raw_ms, r_drs_state.filtered_ms, target_ms,
			use_gpu_timing ? "gpu" : "frame");
	}
	else if (debug_level > 1)
	{
		Con_DPrintf ("drs sample: res=%.0f%% raw_ms=%.2f filtered_ms=%.2f target_ms=%.2f source=%s\n",
			current_resolution * 100.f, (float)raw_ms, r_drs_state.filtered_ms, target_ms,
			use_gpu_timing ? "gpu" : "frame");
	}

	r_drs_state.dynamic_resolution = new_resolution;
}

static void R_UpdateSceneSizeState (void)
{
	int debug = (int)Q_rint (r_scene_scale_debug.value);
	int drs_debug = (int)Q_rint (r_drs_debug.value);
	int requested_scale = q_max (1, r_refdef.scale);
	int native_width = q_max (1, vid.width);
	int native_height = q_max (1, vid.height);
	int base_scene_width = q_max (1, r_refdef.vrect.width);
	int base_scene_height = q_max (1, r_refdef.vrect.height);
	scene_scale_source_t scale_source = SCENE_SCALE_SOURCE_MANUAL;
	qboolean was_initialized = r_scene_size_state.initialized;
	float resolution_ratio = 1.f;
	qboolean using_drs = false;

	if (r_drs.value > 0.f && r_drs_state.initialized && r_drs_state.dynamic_resolution > 0.f)
	{
		scale_source = SCENE_SCALE_SOURCE_DYNAMIC;
		using_drs = true;
		float clamped = CLAMP (R_GetDRSMinResolution (), r_drs_state.dynamic_resolution, R_GetDRSMaxResolution ());
		resolution_ratio = R_QuantizeResolution (clamped);
		resolution_ratio = CLAMP (R_GetDRSMinResolution (), resolution_ratio, R_GetDRSMaxResolution ());
		requested_scale = 1;
	}
	else
	{
		requested_scale = q_max (1, requested_scale);
		resolution_ratio = 1.f / (float)requested_scale;
	}

	int scene_width, scene_height;
	if (using_drs)
	{
		scene_width = q_max (1, ((int)ceilf ((float)base_scene_width * resolution_ratio) + 3) & ~3);
		scene_height = q_max (1, ((int)ceilf ((float)base_scene_height * resolution_ratio) + 3) & ~3);
	}
	else
	{
		scene_width = (base_scene_width + requested_scale - 1) / requested_scale;
		scene_height = (base_scene_height + requested_scale - 1) / requested_scale;
	}

	qboolean size_changed;
	qboolean scale_changed;
	qboolean source_changed;

	if (!was_initialized)
	{
		r_scene_size_state.prev_scene_width = scene_width;
		r_scene_size_state.prev_scene_height = scene_height;
		r_scene_size_state.prev_scene_scale = requested_scale;
		r_scene_size_state.prev_scale_source = scale_source;
		r_scene_size_state.prev_scene_resolution_ratio = resolution_ratio;
	}

	size_changed = !was_initialized
		|| r_scene_size_state.scene_width != scene_width
		|| r_scene_size_state.scene_height != scene_height
		|| r_scene_size_state.native_width != native_width
		|| r_scene_size_state.native_height != native_height;
	scale_changed = !was_initialized
		|| r_scene_size_state.scene_scale != requested_scale
		|| r_scene_size_state.scene_resolution_ratio != resolution_ratio;
	source_changed = !was_initialized
		|| r_scene_size_state.scale_source != scale_source;

	if (size_changed || scale_changed || source_changed)
	{
		r_scene_size_state.prev_scene_width = was_initialized ? r_scene_size_state.scene_width : scene_width;
		r_scene_size_state.prev_scene_height = was_initialized ? r_scene_size_state.scene_height : scene_height;
		r_scene_size_state.prev_scene_scale = was_initialized ? r_scene_size_state.scene_scale : requested_scale;
		r_scene_size_state.prev_scale_source = was_initialized ? r_scene_size_state.scale_source : scale_source;
		r_scene_size_state.prev_scene_resolution_ratio = was_initialized ? r_scene_size_state.scene_resolution_ratio : resolution_ratio;
		r_scene_resize_generation++;
		r_scene_resize_pending_invalidation = true;
	}

	r_scene_size_state.native_width = native_width;
	r_scene_size_state.native_height = native_height;
	r_scene_size_state.scene_width = scene_width;
	r_scene_size_state.scene_height = scene_height;
	r_scene_size_state.scene_scale = requested_scale;
	r_scene_size_state.scene_resolution_ratio = resolution_ratio;
	r_scene_size_state.scale_source = scale_source;
	r_scene_size_state.size_changed = size_changed;
	r_scene_size_state.scale_changed = scale_changed;
	r_scene_size_state.source_changed = source_changed;
	r_scene_size_state.initialized = true;

	if ((drs_debug > 0 || debug > 0) && was_initialized && (scale_changed || source_changed))
	{
		if (using_drs)
			Con_DPrintf ("scene_scale: %.0f%%(%s) -> %.0f%%(%s)\n",
				r_scene_size_state.prev_scene_resolution_ratio * 100.f,
				R_GetSceneScaleSourceNameInternal (r_scene_size_state.prev_scale_source),
				r_scene_size_state.scene_resolution_ratio * 100.f,
				R_GetSceneScaleSourceNameInternal (r_scene_size_state.scale_source));
		else
			Con_DPrintf ("scene_scale: 1/%d(%s) -> 1/%d(%s)\n",
				r_scene_size_state.prev_scene_scale,
				R_GetSceneScaleSourceNameInternal (r_scene_size_state.prev_scale_source),
				r_scene_size_state.scene_scale,
				R_GetSceneScaleSourceNameInternal (r_scene_size_state.scale_source));
	}

	if (debug > 0 && (debug > 1 || size_changed || scale_changed || source_changed))
	{
		Con_DPrintf ("scene_size: output=%dx%d view=%dx%d scale=%d res=%.0f%%(%s) scene=%dx%d prev=%dx%d changed(size=%d scale=%d source=%d)\n",
			r_scene_size_state.native_width, r_scene_size_state.native_height,
			base_scene_width, base_scene_height,
			r_scene_size_state.scene_scale, r_scene_size_state.scene_resolution_ratio * 100.f,
			R_GetSceneScaleSourceNameInternal (r_scene_size_state.scale_source),
			r_scene_size_state.scene_width, r_scene_size_state.scene_height,
			r_scene_size_state.prev_scene_width, r_scene_size_state.prev_scene_height,
			size_changed ? 1 : 0, scale_changed ? 1 : 0, source_changed ? 1 : 0);
	}
}

void R_ResetDRSState (void)
{
	memset (&r_drs_state, 0, sizeof (r_drs_state));
}

int R_GetNativeRenderWidth (void)
{
	R_UpdateSceneSizeState ();
	return r_scene_size_state.native_width;
}

int R_GetNativeRenderHeight (void)
{
	R_UpdateSceneSizeState ();
	return r_scene_size_state.native_height;
}

int R_GetSceneRenderWidth (void)
{
	R_UpdateSceneSizeState ();
	return r_scene_size_state.scene_width;
}

int R_GetSceneRenderHeight (void)
{
	R_UpdateSceneSizeState ();
	return r_scene_size_state.scene_height;
}

int R_GetSceneRenderScale (void)
{
	R_UpdateSceneSizeState ();
	return r_scene_size_state.scene_scale;
}

float R_GetSceneResolutionRatio (void)
{
	R_UpdateSceneSizeState ();
	return r_scene_size_state.scene_resolution_ratio;
}

void R_GetSceneTexelSize (float *out_inv_w, float *out_inv_h)
{
	int w = R_GetSceneRenderWidth ();
	int h = R_GetSceneRenderHeight ();
	if (out_inv_w)
		*out_inv_w = 1.f / (float)q_max (1, w);
	if (out_inv_h)
		*out_inv_h = 1.f / (float)q_max (1, h);
}

void R_GetOutputTexelSize (float *out_inv_w, float *out_inv_h)
{
	int w = R_GetNativeRenderWidth ();
	int h = R_GetNativeRenderHeight ();
	if (out_inv_w)
		*out_inv_w = 1.f / (float)q_max (1, w);
	if (out_inv_h)
		*out_inv_h = 1.f / (float)q_max (1, h);
}

int R_GetSceneResizeGeneration (void)
{
	R_UpdateSceneSizeState ();
	return r_scene_resize_generation;
}

qboolean R_ConsumeSceneResizePendingInvalidation (void)
{
	qboolean pending = r_scene_resize_pending_invalidation;
	r_scene_resize_pending_invalidation = false;
	return pending;
}

int R_GetDesiredSceneSampleCount (void)
{
	int desired = Q_nextPow2 ((int)q_max (1.f, vid_fsaa.value));
	int max_samples = framebufs.max_samples > 0 ? framebufs.max_samples : 1;

	return CLAMP (1, desired, max_samples);
}

void R_EnsureRenderTargetSampleState (void)
{
	int desired_samples = R_GetDesiredSceneSampleCount ();
	int current_samples = framebufs.scene.samples > 0 ? framebufs.scene.samples : 1;
	int desired_scene_w = R_GetSceneRenderWidth ();
	int desired_scene_h = R_GetSceneRenderHeight ();
	int native_w = R_GetNativeRenderWidth ();
	int native_h = R_GetNativeRenderHeight ();
	int current_scene_w = framebufs.scene.width > 0 ? framebufs.scene.width : desired_scene_w;
	int current_scene_h = framebufs.scene.height > 0 ? framebufs.scene.height : desired_scene_h;
	int desired_alloc_w = desired_scene_w;
	int desired_alloc_h = desired_scene_h;
	qboolean sample_changed = (current_samples != desired_samples);

	R_GetSceneRenderTargetAllocationSize (native_w, native_h, desired_scene_w, desired_scene_h, &desired_alloc_w, &desired_alloc_h);
	qboolean size_changed = (current_scene_w != desired_alloc_w || current_scene_h != desired_alloc_h);
	qboolean targets_uninitialized = (framebufs.scene.fbo == 0u || framebufs.scene.color_tex == 0u || framebufs.scene.depth_stencil_tex == 0u);

	if (R_ConsumeSceneResizePendingInvalidation ())
	{
		R_InvalidateTemporalHistoryOnSceneResize ();
	}

	if (!sample_changed && !size_changed && !targets_uninitialized)
		return;

	Con_DPrintf ("%s render targets (alloc %dx%d -> %dx%d, samples %d -> %d)\n",
		targets_uninitialized ? "Initializing" : "Recreating",
		current_scene_w, current_scene_h, desired_alloc_w, desired_alloc_h,
		current_samples, desired_samples);
	GL_DeleteFrameBuffers ();
	GL_CreateFrameBuffers ();
	if (!R_ConsumeSceneResizePendingInvalidation ())
		R_InvalidateTemporalHistoryOnSceneResize ();
	R_FrameGraph_SetRenderFramePlan (NULL);
}

void R_GetSceneSizeInfo (scene_size_info_t *out_info)
{
	R_UpdateSceneSizeState ();
	if (!out_info)
		return;

	out_info->native_width = r_scene_size_state.native_width;
	out_info->native_height = r_scene_size_state.native_height;
	out_info->scene_width = r_scene_size_state.scene_width;
	out_info->scene_height = r_scene_size_state.scene_height;
	out_info->scene_scale = r_scene_size_state.scene_scale;
	out_info->resolution_ratio = r_scene_size_state.scene_resolution_ratio;
	out_info->size_changed = r_scene_size_state.size_changed;
	out_info->scale_changed = r_scene_size_state.scale_changed;
	out_info->source_changed = r_scene_size_state.source_changed;
	out_info->dynamic_scale_source = (r_scene_size_state.scale_source == SCENE_SCALE_SOURCE_DYNAMIC);
}

qboolean R_HasSceneSizeChanged (void)
{
	R_UpdateSceneSizeState ();
	return r_scene_size_state.size_changed
		|| r_scene_size_state.scale_changed
		|| r_scene_size_state.source_changed;
}
