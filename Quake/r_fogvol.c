/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
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
#include "draw.h"
#include "gl_lightgrid.h"
#include "r_fogvol.h"
#include "r_dlight_pool.h"
#include "r_godrays.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

typedef struct fog_volume_gpu_s
{
	float mins[4];
	float maxs[4];
	float sphere[4];
	float color_density[4];
	float noise_params[4];
	float velocity_windspeed[4];
	float wind_turbulence[4];
	float misc[4];
	float extra[4];
	float params2[4];
} fog_volume_gpu_t;

COMPILE_TIME_ASSERT (fog_volume_gpu_align16, (sizeof (fog_volume_gpu_t) % 16) == 0);

typedef struct fog_light_gpu_s
{
	float pos_rad[4];
	float col_int[4];
} fog_light_gpu_t;

typedef struct fog_light_list_gpu_s
{
	int offset_count[4];
} fog_light_list_gpu_t;

typedef struct fog_light_lists_gpu_s
{
	fog_light_list_gpu_t volumes[MAX_FOGVOLUMES];
	fog_light_gpu_t lights[MAX_FOGVOLUMES * MAX_FOGLIGHTS];
} fog_light_lists_gpu_t;

typedef struct fog_lightgrid_gpu_s
{
	float probe_rgb[8][4];
} fog_lightgrid_gpu_t;

COMPILE_TIME_ASSERT (fog_light_gpu_align16, (sizeof (fog_light_gpu_t) % 16) == 0);
COMPILE_TIME_ASSERT (fog_light_list_gpu_align16, (sizeof (fog_light_list_gpu_t) % 16) == 0);
COMPILE_TIME_ASSERT (fog_light_lists_gpu_align16, (sizeof (fog_light_lists_gpu_t) % 16) == 0);
COMPILE_TIME_ASSERT (fog_lightgrid_gpu_align16, (sizeof (fog_lightgrid_gpu_t) % 16) == 0);

typedef struct fogvol_light_candidate_s
{
	int index;
	float score;
	fog_light_gpu_t light;
	vec3_t bounds_mins;
	vec3_t bounds_maxs;
} fogvol_light_candidate_t;

typedef struct fogvol_visible_volume_s
{
	const fog_volume_t *volume;
	float screen_weight;
} fogvol_visible_volume_t;

typedef struct fogvol_light_stats_s
{
	int broadphase_candidates;
	int narrowphase_candidates;
	int narrowphase_accepted;
	int lightgrid_probes;
	double broadphase_seconds;
	double narrowphase_seconds;
	double lightgrid_seconds;
	double lightgrid_upload_seconds;
} fogvol_light_stats_t;

static fog_volume_t r_fogvolumes[MAX_FOGVOLUMES];
static int r_fogvolume_count = 0;
static fog_volume_t r_fogvolume_entities[MAX_FOGVOLUMES];
static int r_fogvolume_entity_count = 0;

#define MAX_FOG_DLIGHTS 64

/* Variant C split: these lights are fog-only and must never enter the normal
 * geometry/cluster/shadow dlight pipelines. */
static int r_num_fog_dlights = 0;
static dlight_t r_fog_dlights[MAX_FOG_DLIGHTS];

#define MAX_FOG_STATIC_LIGHTS 192
static int r_num_fog_static_lights = 0;
static fog_light_gpu_t r_fog_static_lights[MAX_FOG_STATIC_LIGHTS];

typedef struct fogvol_static_field_s
{
	int dims[3];
	vec3_t mins;
	vec3_t maxs;
	vec3_t inv_size;
	float *rgb;
	float *weight;
	qboolean valid;
	qboolean build_complete;
	int next_surface;
	int total_samples;
} fogvol_static_field_t;

static fogvol_static_field_t r_fog_static_field;

cvar_t r_fogvol = { "r_fogvol", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_steps = { "r_fogvol_steps", "32", CVAR_ARCHIVE };
cvar_t r_fogvol_maxsteps = { "r_fogvol_maxsteps", "128", CVAR_ARCHIVE };
cvar_t r_fogvol_stepsize = { "r_fogvol_stepsize", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_halfres = { "r_fogvol_halfres", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_steps_scale_halfres = { "r_fogvol_steps_scale_halfres", "0.5", CVAR_ARCHIVE };
cvar_t r_fogvol_noise = { "r_fogvol_noise", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_noise_subsample = { "r_fogvol_noise_subsample", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_noise_lod_switch_dist = { "r_fogvol_noise_lod_switch_dist", "64", CVAR_ARCHIVE };
cvar_t r_fogvol_domainwarp_dist = { "r_fogvol_domainwarp_dist", "128", CVAR_ARCHIVE };
cvar_t r_fogvol_noise_detail_strength = { "r_fogvol_noise_detail_strength", "0.35", CVAR_ARCHIVE };
cvar_t r_fogvol_quality = { "r_fogvol_quality", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_atmosphere = { "r_fogvol_atmosphere", "0.8", CVAR_ARCHIVE };
cvar_t r_fogvol_noisemode = { "r_fogvol_noisemode", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_testvolumes = { "r_fogvol_testvolumes", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_testvolumes_dumpstate = { "r_fogvol_testvolumes_dumpstate", "0", CVAR_NONE };
cvar_t r_fogvol_physblend = { "r_fogvol_physblend", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_blendmode = { "r_fogvol_blendmode", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_emissive = { "r_fogvol_emissive", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_temporal_alpha = { "r_fogvol_temporal_alpha", "0.9", CVAR_ARCHIVE };
cvar_t r_fogvol_temporal_depth_reject = { "r_fogvol_temporal_depth_reject", "0.01", CVAR_ARCHIVE };
cvar_t r_fogvol_temporal_confidence_min_alpha = { "r_fogvol_temporal_confidence_min_alpha", "0.05", CVAR_ARCHIVE };
cvar_t r_fogvol_temporal_disocclusion_bias = { "r_fogvol_temporal_disocclusion_bias", "0.5", CVAR_ARCHIVE };
cvar_t r_fogvol_temporal_clamp_strength = { "r_fogvol_temporal_clamp_strength", "1.5", CVAR_ARCHIVE };
cvar_t r_fogvol_checkerboard = { "r_fogvol_checkerboard", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_light_subsample = { "r_fogvol_light_subsample", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_jitter = { "r_fogvol_jitter", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_debug = { "r_fogvol_debug", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_inject_debug = { "r_fogvol_inject_debug", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_density_scale = { "r_fogvol_density_scale", "1", CVAR_ARCHIVE };
/* r_fogvol_sigma_max: maximum optical depth *per raymarching step*.
 * exp(-4) ≈ 0.018, so 4.0 is already near-black in one step.
 * Previous default of "2" was applied as a per-unit extinction cap, which
 * saturated instantly at any stepLen > ~1 unit.  Now treated as an optical
 * depth cap (sigma*stepLen), making the value scale-independent. */
cvar_t r_fogvol_sigma_max = { "r_fogvol_sigma_max", "4", CVAR_ARCHIVE };
cvar_t r_fogvol_global = { "r_fogvol_global", "1", CVAR_ARCHIVE };
/* r_fogvol_global_density_scale: scales the global volumetric density.
 * If the map has classic fog density > 0, that value is used as the base.
 * If the map fog density is 0, a base density of 1 is used so r_fogvol_global
 * still produces visible fog on maps without a fog key.
 * Quake fog density is dimensionless (used in GL_EXP as exp(-density*z));
 * the raymarcher treats density as an extinction coefficient in quake-units^-1.
 * A map fog density of 0.1 means GL_EXP gives 37% transmittance at z=10 units,
 * which is far too dense for the raymarcher (sigma=0.1, stepLen~64 → opaque).
 * Scale down by ~0.01–0.1 to get visually comparable results.
 * Default changed from 1 to 0.06 so global fog is not white-out by default. */
cvar_t r_fogvol_global_density_scale = { "r_fogvol_global_density_scale", "0.06", CVAR_ARCHIVE };
cvar_t r_fogvol_global_falloff = { "r_fogvol_global_falloff", "64", CVAR_ARCHIVE };
cvar_t r_fogvol_global_noise_scale = { "r_fogvol_global_noise_scale", "0.014", CVAR_ARCHIVE };
/* r_fogvol_global_noise_amount: 0=uniform fog, 1=fully noise-modulated.
 * Was 0 (disabled) by default, making global fog always uniform regardless of
 * other noise settings. Default 0.78 gives stronger cloud breakup. */
cvar_t r_fogvol_global_noise_amount = { "r_fogvol_global_noise_amount", "0.78", CVAR_ARCHIVE };
cvar_t r_fogvol_global_noise_bias = { "r_fogvol_global_noise_bias", "0.0", CVAR_ARCHIVE };
cvar_t r_fogvol_global_velocity_x = { "r_fogvol_global_velocity_x", "1.25", CVAR_ARCHIVE };
cvar_t r_fogvol_global_velocity_y = { "r_fogvol_global_velocity_y", "0.65", CVAR_ARCHIVE };
cvar_t r_fogvol_global_velocity_z = { "r_fogvol_global_velocity_z", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_global_height = { "r_fogvol_global_height", "56", CVAR_ARCHIVE };
cvar_t r_fogvol_global_height_scale = { "r_fogvol_global_height_scale", "0.0020", CVAR_ARCHIVE };
cvar_t r_fogvol_height_mist_strength = { "r_fogvol_height_mist_strength", "0.3", CVAR_ARCHIVE };
cvar_t r_fogvol_global_priority = { "r_fogvol_global_priority", "-1", CVAR_ARCHIVE };
cvar_t r_fogvol_light = { "r_fogvol_light", "0", CVAR_ARCHIVE };
/* Central volumetric lighting path selector:
 * 0=off, 1=raymarch lights, 2=froxel lights, 3=froxel base + raymarch detail.
 * Default 2 keeps historical behavior where enabling froxel disabled full
 * raymarch local lights to avoid double-lighting. */
cvar_t r_fogvol_lighting_mode = { "r_fogvol_lighting_mode", "2", CVAR_ARCHIVE };
cvar_t r_fogvol_lightgrid = { "r_fogvol_lightgrid", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_light_max = { "r_fogvol_light_max", "16", CVAR_ARCHIVE };
cvar_t r_fogvol_dlightscale = { "r_fogvol_dlightscale", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_light_stats = { "r_fogvol_light_stats", "0", CVAR_NONE };
cvar_t r_fogvol_gpu_light_select = { "r_fogvol_gpu_light_select", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_gpu_static_build = { "r_fogvol_gpu_static_build", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_shadow = { "r_fogvol_shadow", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_shadow_samples = { "r_fogvol_shadow_samples", "2", CVAR_ARCHIVE };
cvar_t r_fogvol_shadow_strength = { "r_fogvol_shadow_strength", "0.8", CVAR_ARCHIVE };
cvar_t r_fogvol_shadow_jitter = { "r_fogvol_shadow_jitter", "1", CVAR_ARCHIVE };
/* Per-local-light occlusion term in fogvol.frag:
 * 0=off, 1=cheap signed depth test along light ray, 2=multi-tap depth cone trace.
 * Mode 2 is automatically downgraded to mode 1 when shadow marching is disabled
 * because there is no additional shadow context to justify the extra taps. */
cvar_t r_fogvol_local_occlusion = { "r_fogvol_local_occlusion", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_sun_dir = { "r_fogvol_sun_dir", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_sun_scatter = { "r_fogvol_sun_scatter", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_sun_color = { "r_fogvol_sun_color", "0 0 0", CVAR_ARCHIVE };
cvar_t r_fogvol_froxel = { "r_fogvol_froxel", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_froxel_parity = { "r_fogvol_froxel_parity", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_froxel_sun = { "r_fogvol_froxel_sun", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_froxel_sun_legacy = { "r_fogvol_froxel_sun_legacy", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_froxel_static = { "r_fogvol_froxel_static", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_static_probe_mode = { "r_fogvol_static_probe_mode", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_static_probe_density = { "r_fogvol_static_probe_density", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_static_probe_budget_load = { "r_fogvol_static_probe_budget_load", "12000", CVAR_ARCHIVE };
cvar_t r_fogvol_static_probe_budget_frame = { "r_fogvol_static_probe_budget_frame", "2000", CVAR_ARCHIVE };
cvar_t r_fogvol_static_probe_grid = { "r_fogvol_static_probe_grid", "24", CVAR_ARCHIVE };
cvar_t r_fogvol_froxel_godrays = { "r_fogvol_froxel_godrays", "0", CVAR_ARCHIVE };
cvar_t r_fogvol_froxel_temporal_alpha = { "r_fogvol_froxel_temporal_alpha", "0.85", CVAR_ARCHIVE };
cvar_t r_fogvol_froxel_temporal_reject_threshold = { "r_fogvol_froxel_temporal_reject_threshold", "24", CVAR_ARCHIVE };
cvar_t r_fogvol_godray_coupling = { "r_fogvol_godray_coupling", "1", CVAR_ARCHIVE };
/* Source weighting policy for local fog lighting contributions.
 * - r_fogvol_froxel_scale controls FogFroxelLightTex contribution when enabled.
 * - r_fogvol_lightlist_scale controls per-volume FogLights list contribution.
 * - r_fogvol_lightgrid_scale controls baked/static FogLightgrid contribution.
 *
 * Default policy keeps baked/static lighting fully visible even with froxel enabled,
 * while splitting local-light energy evenly between froxel and explicit local lists
 * to avoid double-counting when both carry overlapping lights. */
cvar_t r_fogvol_froxel_scale = { "r_fogvol_froxel_scale", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_lightlist_scale = { "r_fogvol_lightlist_scale", "1", CVAR_ARCHIVE };
cvar_t r_fogvol_lightgrid_scale = { "r_fogvol_lightgrid_scale", "1", CVAR_ARCHIVE };
/* r_froxel_debug: 0=off, 1=froxel rgb at first hit, 2=max-energy heat, 3=temporal history weight, 4=directional sun intensity */
cvar_t r_froxel_debug = { "r_froxel_debug", "0", CVAR_ARCHIVE };

typedef struct fogvol_cvar_reg_s
{
	cvar_t *var;
	const char *category;
	const char *default_value;
} fogvol_cvar_reg_t;

static const fogvol_cvar_reg_t fogvol_cvar_table[] = {
	{&r_fogvol, "core", "0"},
	{&r_fogvol_steps, "quality", "32"},
	{&r_fogvol_maxsteps, "quality", "128"},
	{&r_fogvol_stepsize, "quality", "0"},
	{&r_fogvol_halfres, "quality", "0"},
	{&r_fogvol_steps_scale_halfres, "quality", "0.5"},
	{&r_fogvol_noise, "noise", "1"},
	{&r_fogvol_noise_subsample, "noise", "1"},
	{&r_fogvol_noise_lod_switch_dist, "noise", "64"},
	{&r_fogvol_domainwarp_dist, "noise", "128"},
	{&r_fogvol_noise_detail_strength, "noise", "0.35"},
	{&r_fogvol_quality, "quality", "1"},
	{&r_fogvol_atmosphere, "quality", "0.8"},
	{&r_fogvol_noisemode, "noise", "0"},
	{&r_fogvol_testvolumes, "debug", "0"},
	{&r_fogvol_testvolumes_dumpstate, "debug", "0"},
	{&r_fogvol_physblend, "core", "1"},
	{&r_fogvol_blendmode, "core", "0"},
	{&r_fogvol_emissive, "lighting", "1"},
	{&r_fogvol_temporal_alpha, "temporal", "0.9"},
	{&r_fogvol_temporal_depth_reject, "temporal", "0.01"},
	{&r_fogvol_temporal_confidence_min_alpha, "temporal", "0.05"},
	{&r_fogvol_temporal_disocclusion_bias, "temporal", "0.5"},
	{&r_fogvol_temporal_clamp_strength, "temporal", "1.5"},
	{&r_fogvol_checkerboard, "quality", "0"},
	{&r_fogvol_light_subsample, "lighting", "1"},
	{&r_fogvol_jitter, "quality", "1"},
	{&r_fogvol_debug, "debug", "0"},
	{&r_fogvol_inject_debug, "debug", "0"},
	{&r_fogvol_density_scale, "core", "1"},
	{&r_fogvol_sigma_max, "core", "4"},
	{&r_fogvol_global, "global", "1"},
	{&r_fogvol_global_density_scale, "global", "0.06"},
	{&r_fogvol_global_falloff, "global", "64"},
	{&r_fogvol_global_noise_scale, "global", "0.014"},
	{&r_fogvol_global_noise_amount, "global", "0.78"},
	{&r_fogvol_global_noise_bias, "global", "0.0"},
	{&r_fogvol_global_velocity_x, "global", "1.25"},
	{&r_fogvol_global_velocity_y, "global", "0.65"},
	{&r_fogvol_global_velocity_z, "global", "0"},
	{&r_fogvol_global_height, "global", "56"},
	{&r_fogvol_global_height_scale, "global", "0.0020"},
	{&r_fogvol_height_mist_strength, "global", "0.3"},
	{&r_fogvol_global_priority, "global", "-1"},
	{&r_fogvol_light, "lighting", "0"},
	{&r_fogvol_lighting_mode, "lighting", "2"},
	{&r_fogvol_lightgrid, "lighting", "1"},
	{&r_fogvol_light_max, "lighting", "16"},
	{&r_fogvol_dlightscale, "lighting", "1"},
	{&r_fogvol_light_stats, "debug", "0"},
	{&r_fogvol_gpu_light_select, "lighting", "0"},
	{&r_fogvol_gpu_static_build, "lighting", "0"},
	{&r_fogvol_shadow, "lighting", "1"},
	{&r_fogvol_shadow_samples, "lighting", "2"},
	{&r_fogvol_shadow_strength, "lighting", "0.8"},
	{&r_fogvol_shadow_jitter, "lighting", "1"},
	{&r_fogvol_local_occlusion, "lighting", "0"},
	{&r_fogvol_sun_dir, "lighting", "1"},
	{&r_fogvol_sun_scatter, "lighting", "1"},
	{&r_fogvol_sun_color, "lighting", "0 0 0"},
	{&r_fogvol_froxel, "lighting", "0"},
	{&r_fogvol_froxel_parity, "lighting", "0"},
	{&r_fogvol_froxel_sun, "lighting", "1"},
	{&r_fogvol_froxel_sun_legacy, "lighting", "0"},
	{&r_fogvol_froxel_static, "lighting", "1"},
	{&r_fogvol_static_probe_mode, "lighting", "1"},
	{&r_fogvol_static_probe_density, "lighting", "1"},
	{&r_fogvol_static_probe_budget_load, "lighting", "12000"},
	{&r_fogvol_static_probe_budget_frame, "lighting", "2000"},
	{&r_fogvol_static_probe_grid, "lighting", "24"},
	{&r_fogvol_froxel_godrays, "lighting", "0"},
	{&r_fogvol_froxel_temporal_alpha, "temporal", "0.85"},
	{&r_fogvol_froxel_temporal_reject_threshold, "temporal", "24"},
	{&r_fogvol_godray_coupling, "lighting", "1"},
	{&r_fogvol_froxel_scale, "lighting", "1"},
	{&r_fogvol_lightlist_scale, "lighting", "1"},
	{&r_fogvol_lightgrid_scale, "lighting", "1"},
	{&r_froxel_debug, "debug", "0"},
};

/* NOTE: Keep this mapping synchronized with shaders/fogvol.frag layout(location=...). */
enum
{
	FOGVOL_U_STEPS = 0,
	FOGVOL_U_NOISE_ENABLED = 1,
	FOGVOL_U_DEBUG_MODE = 2,
	FOGVOL_U_VOLUME_INDEX = 3,
	FOGVOL_U_INV_VIEWPROJ = 4,
	FOGVOL_U_NOISE_MODE = 5,
	FOGVOL_U_PHYS_BLEND = 6,
	FOGVOL_U_JITTER_ENABLED = 7,
	FOGVOL_U_CAMERA_POS_WS = 8,
	FOGVOL_U_VIEWPORT_PARAMS = 9,
	FOGVOL_U_DEPTH_SCALE = 10,
	FOGVOL_U_VIEW_PARAMS = 11,
	FOGVOL_U_DEPTH_PARAMS = 12,
	FOGVOL_U_DENSITY_PARAMS = 13,
	FOGVOL_U_EMISSIVE_ENABLED = 14,
	FOGVOL_U_BLEND_MODE_DEFAULT = 15,
	FOGVOL_U_LIGHT_ENABLED = 16,
	FOGVOL_U_SHADOW_ENABLED = 17,
	FOGVOL_U_SHADOW_SAMPLES = 18,
	FOGVOL_U_SHADOW_STRENGTH = 19,
	FOGVOL_U_SHADOW_JITTER = 20,
	FOGVOL_U_SHADOW_DIR = 21,
	FOGVOL_U_LIGHTGRID_ENABLED = 22,
	FOGVOL_U_FRAME_INDEX = 23,
	FOGVOL_U_NOISE_SUBSAMPLE = 24,
	FOGVOL_U_NOISE_LOD_SWITCH_DIST = 25,
	FOGVOL_U_DOMAIN_WARP_MAX_DIST = 26,
	FOGVOL_U_CHECKERBOARD = 27,
	FOGVOL_U_HALFRES = 28,
	FOGVOL_U_LIGHT_SUBSAMPLE = 29,
	FOGVOL_U_SUN_SCATTER = 30,
	FOGVOL_U_SUN_COLOR = 31,
	FOGVOL_U_SUN_ANISOTROPY = 32,
	FOGVOL_U_DLIGHT_SCALE = 33,
	FOGVOL_U_FROXEL_ENABLED = 34,
	FOGVOL_U_FROXEL_PARAMS0 = 35,
	FOGVOL_U_FROXEL_PARAMS1 = 36,
	FOGVOL_U_FROXEL_DEBUG = 37,
	FOGVOL_U_FROXEL_PARITY_MODE = 38,
	FOGVOL_U_LIGHT_SOURCE_SCALES = 39,
	FOGVOL_U_LIGHTING_MODE = 40,
	FOGVOL_U_GODRAY_COUPLING = 41,
	FOGVOL_U_GODRAY_SHAFTS_PARAMS = 42,
	FOGVOL_U_FROXEL_TEMPORAL_PARAMS = 43,
	FOGVOL_U_LOCAL_OCCLUSION_MODE = 44,
	FOGVOL_U_LOCAL_OCCLUSION_PARAMS = 45,
	FOGVOL_U_NOISE_DETAIL_STRENGTH = 46,
	FOGVOL_U_HEIGHT_MIST_STRENGTH = 47,
	FOGVOL_U_ATMOSPHERE_BOOST = 48,
	FOGVOL_U_COUNT = 49
};

COMPILE_TIME_ASSERT (fogvol_uniform_location_max, FOGVOL_U_ATMOSPHERE_BOOST == 48);

typedef struct froxel_state_s
{
	GLuint light_tex;
	GLuint history_tex;
	GLuint light_ssbo;
	int dims[3];
	float near_clip;
	float far_clip;
	float tan_half_fov_x;
	float tan_half_fov_y;
	float log_far_near;
	vec3_t prev_vieworg;
	int prev_frame;
	int prev_lighting_mode;
	int prev_parity_mode;
	qboolean prev_froxel_enabled;
	qboolean prev_mode_valid;
	qboolean prev_valid;
	qboolean valid;
	int light_count;
} froxel_state_t;

static froxel_state_t r_froxel;

#define MAX_FROXEL_GPU_LIGHTS 32
typedef struct froxel_gpu_light_s
{
	float pos_rad[4];
	float color_intensity[4];
	uint32_t type;
	uint32_t _pad[3];
} froxel_gpu_light_t;

static froxel_gpu_light_t r_froxel_gpu_lights[MAX_FROXEL_GPU_LIGHTS];

#define FOGVOL_GPU_LOCAL_SIZE 64
#define FOGVOL_GPU_DLIGHT_MAX_CANDIDATES 256
typedef struct fogvol_dlight_score_gpu_s
{
	float pos_rad[4];
	float color_bias[4];   /* rgb color, w score bias */
	uint32_t meta[4];      /* x flags(bit0 inject), y stable id */
} fogvol_dlight_score_gpu_t;

typedef struct fogvol_sort_entry_gpu_s
{
	uint32_t key0;
	uint32_t key1;
	uint32_t index;
	uint32_t _pad;
} fogvol_sort_entry_gpu_t;

typedef struct fogvol_gpu_light_select_state_s
{
	GLuint candidate_ssbo;
	GLuint sort_ssbo;
	int sort_capacity;
	fogvol_dlight_score_gpu_t staging_candidates[FOGVOL_GPU_DLIGHT_MAX_CANDIDATES];
	fogvol_sort_entry_gpu_t staging_sorted[FOGVOL_GPU_DLIGHT_MAX_CANDIDATES];
	int gpu_frames;
	int gpu_fallbacks;
	int last_candidates;
	int last_selected;
} fogvol_gpu_light_select_state_t;

static fogvol_gpu_light_select_state_t r_fogvol_gpu_light_select_state;
static void R_FogVol_GPUSelectReset (void);

static GLuint r_fogvol_static_rgb_ssbo = 0;
static GLuint r_fogvol_static_weight_ssbo = 0;
static GLuint r_fogvol_static_out_ssbo = 0;
static int r_fogvol_static_capacity_cells = 0;
static void R_FogVol_GPUStaticBuildReset (void);

static GLuint r_fogvol_godray_shafts_tex = 0;
static GLuint r_fogvol_godray_mask_tex = 0;
static GLuint r_fogvol_godray_source_tex = 0;
static qboolean r_fogvol_godray_ready = false;
static float *r_fogvol_godray_mask_cpu = NULL;
static float *r_fogvol_godray_source_cpu = NULL;
static int r_fogvol_godray_mask_w = 0;
static int r_fogvol_godray_mask_h = 0;
static int r_fogvol_godray_source_w = 0;
static int r_fogvol_godray_source_h = 0;

extern float skyflatcolor[3];

extern cvar_t gl_farclip;
extern cvar_t r_lightmap_colorspace;
extern cvar_t r_sun_light;

static int r_fogvol_history_index = 0;
static int r_fogvol_history_width = 0;
static int r_fogvol_history_height = 0;
static qboolean r_fogvol_composite_valid = false;
static int r_fogvol_alpha_extreme_streak = 0;
static int r_fogvol_alpha_extreme_last_frame = -1;

static const vec3_t r_fogvol_corner_lut[8] = {
	{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {1.f, 1.f, 0.f},
	{0.f, 0.f, 1.f}, {1.f, 0.f, 1.f}, {0.f, 1.f, 1.f}, {1.f, 1.f, 1.f}
};


void R_FogVol_ClearHistory (void)
{
	r_fogvol_history_index = 0;
	r_fogvol_history_width = 0;
	r_fogvol_history_height = 0;
	r_fogvol_composite_valid = false;
	r_fogvol_godray_shafts_tex = 0;
	r_fogvol_godray_mask_tex = 0;
	r_fogvol_godray_source_tex = 0;
	r_fogvol_godray_ready = false;
	R_Froxel_ResetResources ();

	if (!framebufs.fogvol.history_fbo[0] || !framebufs.fogvol.history_fbo[1])
		return;

	/* BUG FIX (C-04): Save the previously bound draw FBO so we can restore it
	 * after clearing.  R_FogVol_ClearHistory() may be called during map loading
	 * or from contexts where a different FBO is active — unconditionally leaving
	 * composite.fbo bound corrupts the GL state for the caller. */
	{
		const float zero[4] = {0.f, 0.f, 0.f, 0.f};
		GLint prev_fbo = 0;
		glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);

		GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.fogvol.history_fbo[0]);
		GL_ClearBufferfvFunc (GL_COLOR, 0, zero);
		GL_BindFramebufferFunc (GL_FRAMEBUFFER, framebufs.fogvol.history_fbo[1]);
		GL_ClearBufferfvFunc (GL_COLOR, 0, zero);

		GL_BindFramebufferFunc (GL_FRAMEBUFFER, (GLuint)prev_fbo);
	}
}

void R_FogVol_SetGodrayCouplingTextures (GLuint shafts_tex, GLuint mask_tex, GLuint source_tex, qboolean ready)
{
	r_fogvol_godray_shafts_tex = shafts_tex;
	r_fogvol_godray_mask_tex = mask_tex;
	r_fogvol_godray_source_tex = source_tex;
	r_fogvol_godray_ready = ready && shafts_tex != 0;
}

typedef struct fogvol_test_state_s
{
	unsigned statebits;
	unsigned cache_statebits;
	GLint viewport[4];
	GLint scissor_box[4];
	GLint draw_fbo;
	GLint read_fbo;
	GLint draw_buffer;
	GLint read_buffer;
	GLint draw_buffers[8];
	GLint num_draw_buffers;
	GLint draw_status;
	GLint read_status;
	GLint draw_color_type[2];
	GLint draw_color_name[2];
	GLint draw_depth_type;
	GLint draw_depth_name;
	GLint read_color_type;
	GLint read_color_name;
	GLint read_depth_type;
	GLint read_depth_name;
	GLint program;
	GLint vao;
	GLint blend_src_rgb;
	GLint blend_dst_rgb;
	GLint blend_equation_rgb;
	GLint polygon_mode[2];
	GLboolean scissor_test;
	GLboolean blend;
	GLboolean depth_test;
	GLboolean depth_writemask;
	GLboolean color_writemask[4];
	GLboolean cull_face;
	GLfloat color_clear_value[4];
	GLboolean framebuffer_srgb;
	GLboolean dither;
	GLboolean multisample;
	GLint cache_draw_fbo;
	GLint cache_read_fbo;
	GLint cache_program;
	GLint cache_vao;
} fogvol_test_state_t;

typedef struct fogvol_state_cache_s
{
	GLint draw_fbo;
	GLint read_fbo;
	GLint program;
	GLint vao;
} fogvol_state_cache_t;

typedef struct fogvol_restore_state_s
{
	GLint viewport[4];
	GLint scissor_box[4];
	GLint draw_fbo;
	GLint read_fbo;
	GLint draw_buffer;
	GLint read_buffer;
	GLint program;
	GLint vao;
	GLint active_texture;
	unsigned int glstate_bits;
	GLboolean scissor_test;
	GLboolean framebuffer_srgb;
} fogvol_restore_state_t;

static fogvol_state_cache_t r_fogvol_state_cache = { 0, 0, 0, 0 };

static void R_FogVol_BindFramebuffer (GLenum target, GLuint fbo)
{
	GL_BindFramebufferFunc (target, fbo);
	/* BEST PRACTICE #10: GL_FRAMEBUFFER is an alias that binds both draw and
	 * read targets simultaneously (GL 3.0+).  Reflect that in the cache so
	 * that subsequent GL_DRAW/READ_FRAMEBUFFER queries against the cache are
	 * correct even if they weren't set individually. */
	if (target == GL_FRAMEBUFFER)
	{
		r_fogvol_state_cache.draw_fbo = (GLint)fbo;
		r_fogvol_state_cache.read_fbo = (GLint)fbo;
	}
	else if (target == GL_DRAW_FRAMEBUFFER)
		r_fogvol_state_cache.draw_fbo = (GLint)fbo;
	else if (target == GL_READ_FRAMEBUFFER)
		r_fogvol_state_cache.read_fbo = (GLint)fbo;
}

static void R_FogVol_UseProgram (GLuint prog)
{
	GL_UseProgram (prog);
	r_fogvol_state_cache.program = (GLint)prog;
}

static void R_FogVol_BindVertexArray (GLuint vao)
{
	GL_BindVertexArrayFunc (vao);
	r_fogvol_state_cache.vao = (GLint)vao;
}

static void R_FogVol_SetDepthMask (qboolean enabled)
{
	if (enabled)
		GL_SetState (glstate & ~GLS_NO_ZWRITE);
	else
		GL_SetState (glstate | GLS_NO_ZWRITE);
}

static qboolean R_FogVol_TestDebugEnabled (void)
{
	return r_fogvol_testvolumes.value > 0.f || r_fogvol_testvolumes_dumpstate.value > 0.f;
}

static qboolean R_FogVol_StateDebugEnabled (void)
{
	return r_fogvol_debug.value > 0.f || developer.value > 0.f;
}

static void R_FogVol_LogStateSnapshot (const char *marker)
{
	GLint viewport[4] = {0};
	GLint scissor_box[4] = {0};
	GLint draw_fbo = 0, read_fbo = 0;
	GLint draw_buffer = 0, read_buffer = 0;
	GLint program = 0;
	GLint active_texture = 0;
	GLboolean scissor_enabled = GL_FALSE;

	if (!R_FogVol_StateDebugEnabled ())
		return;

	glGetIntegerv (GL_VIEWPORT, viewport);
	scissor_enabled = glIsEnabled (GL_SCISSOR_TEST);
	glGetIntegerv (GL_SCISSOR_BOX, scissor_box);
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv (GL_DRAW_BUFFER, &draw_buffer);
	glGetIntegerv (GL_READ_BUFFER, &read_buffer);
	glGetIntegerv (GL_CURRENT_PROGRAM, &program);
	glGetIntegerv (GL_ACTIVE_TEXTURE, &active_texture);

	Con_DPrintf (
		"FOGVOL_STATE %s draw_fbo=%d read_fbo=%d viewport=(%d %d %d %d) scissor=%d box=(%d %d %d %d) drawbuf=0x%04x readbuf=0x%04x prog=%d active_tex=%d\n",
		marker,
		draw_fbo, read_fbo,
		viewport[0], viewport[1], viewport[2], viewport[3],
		scissor_enabled,
		scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3],
		(unsigned)draw_buffer, (unsigned)read_buffer,
		program, active_texture - GL_TEXTURE0);
}

static void R_FogVol_LogBufferMarker (const char *marker)
{
	GLint draw_fbo = 0, read_fbo = 0;
	GLint draw_buffer = 0, read_buffer = 0;

	if (!R_FogVol_TestDebugEnabled ())
		return;

	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv (GL_DRAW_BUFFER, &draw_buffer);
	glGetIntegerv (GL_READ_BUFFER, &read_buffer);
	Con_Printf ("FOGVOL_TEST marker=%s draw_fbo=%d read_fbo=%d draw_buffer=0x%04x read_buffer=0x%04x\n",
		marker, draw_fbo, read_fbo, (unsigned)draw_buffer, (unsigned)read_buffer);
}

static void R_FogVol_SetDrawBufferDebug (GLenum buf, const char *marker)
{
	glDrawBuffer (buf);
	R_FogVol_LogBufferMarker (marker);
}

static void R_FogVol_SetReadBufferDebug (GLenum buf, const char *marker)
{
	glReadBuffer (buf);
	R_FogVol_LogBufferMarker (marker);
}

static void R_FogVol_DebugCheckCompositeAlphaDynamics (int fog_width, int fog_height)
{
	float alpha_sample = 0.f;
	int px;
	int py;
	GLint prev_read_fbo = 0;
	GLint prev_read_buf = 0;

	/* PERF: This readback is only for explicit fogvol debugging. */
	if (r_fogvol_debug.value <= 0.f)
		return;

	if (!r_fogvol_composite_valid || fog_width <= 0 || fog_height <= 0)
	{
		r_fogvol_alpha_extreme_streak = 0;
		return;
	}

	if (r_fogvol_alpha_extreme_last_frame >= 0 && r_framecount != r_fogvol_alpha_extreme_last_frame + 1)
		r_fogvol_alpha_extreme_streak = 0;
	r_fogvol_alpha_extreme_last_frame = r_framecount;

	px = fog_width / 2;
	py = fog_height / 2;
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
	glGetIntegerv (GL_READ_BUFFER, &prev_read_buf);
	R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, framebufs.composite.fbo);
	glReadBuffer (GL_COLOR_ATTACHMENT0);
	glReadPixels (px, py, 1, 1, GL_ALPHA, GL_FLOAT, &alpha_sample);
	R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);
	glReadBuffer ((GLenum)prev_read_buf);

	if (alpha_sample <= 0.001f || alpha_sample >= 0.999f)
		++r_fogvol_alpha_extreme_streak;
	else
		r_fogvol_alpha_extreme_streak = 0;

	if (r_fogvol_alpha_extreme_streak >= 8 && (r_fogvol_alpha_extreme_streak % 32) == 8)
	{
		Con_DPrintf (
			"FOGVOL_WARN composite alpha appears static/extreme for %d frames (sample=%.3f at center). Contract expects dynamic A=1-transmittance where fog is present.\n",
			r_fogvol_alpha_extreme_streak, alpha_sample);
	}
}

static void R_FogVol_LogPipelineState (const char *marker)
{
	GLint viewport[4] = {0};
	GLint scissor_box[4] = {0};
	GLint draw_fbo = 0, read_fbo = 0;
	GLint draw_buffer = 0, read_buffer = 0;
	GLint program = 0;
	GLboolean scissor_enabled = GL_FALSE;

	if (!R_FogVol_TestDebugEnabled ())
		return;

	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv (GL_VIEWPORT, viewport);
	scissor_enabled = glIsEnabled (GL_SCISSOR_TEST);
	glGetIntegerv (GL_SCISSOR_BOX, scissor_box);
	glGetIntegerv (GL_CURRENT_PROGRAM, &program);
	glGetIntegerv (GL_DRAW_BUFFER, &draw_buffer);
	glGetIntegerv (GL_READ_BUFFER, &read_buffer);

	Con_Printf (
		"FOGVOL_TEST marker=%s draw_fbo=%d read_fbo=%d viewport=(%d %d %d %d) "
		"scissor=%d scissor_box=(%d %d %d %d) prog=%d draw_buffer=0x%04x read_buffer=0x%04x\n",
		marker,
		draw_fbo,
		read_fbo,
		viewport[0], viewport[1], viewport[2], viewport[3],
		scissor_enabled,
		scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3],
		program,
		(unsigned)draw_buffer,
		(unsigned)read_buffer);
}

static void R_FogDlights_Clear (void)
{
	r_num_fog_dlights = 0;
}

static void R_FogDlights_Add (const vec3_t origin, float radius, const vec3_t color)
{
	dlight_t *dl;

	if (r_num_fog_dlights >= MAX_FOG_DLIGHTS)
		return;
	if (radius <= 0.f)
		return;

	dl = &r_fog_dlights[r_num_fog_dlights++];
	memset (dl, 0, sizeof (*dl));
	VectorCopy (origin, dl->origin);
	dl->radius = q_max (0.f, radius);
	dl->color[0] = CLAMP (0.f, color[0], 1.f);
	dl->color[1] = CLAMP (0.f, color[1], 1.f);
	dl->color[2] = CLAMP (0.f, color[2], 1.f);
	dl->active = true;
	dl->spawn = cl.time;
	dl->die = cl.time + 0.1;
	dl->baseradius = dl->radius;
}

static qboolean R_FogVol_DlightIsInjectable (const dlight_t *dl)
{
	if (!dl || !dl->active || dl->radius <= 0.f)
		return false;
	if (dl->spawn > cl.time)
		return false;
	if (dl->die > 0.0 && dl->die < cl.time)
		return false;
	return true;
}

static float R_FogVol_DlightScoreBias (const dlight_t *dl)
{
	if (!dl)
		return 1.f;
	switch (dl->type)
	{
	case DLIGHT_ROCKET:
		return 1.9f;
	case DLIGHT_EXPLOSION:
		return 1.6f;
	case DLIGHT_PLASMA:
		return 1.5f;
	case DLIGHT_TORCH:
		return 0.9f;
	default:
		return 1.f;
	}
}

static GLuint R_FogVol_GetFramebufferColorAttachmentTexture (GLenum target)
{
	GLint object_type = GL_NONE;
	GLint object_name = 0;

	GL_GetFramebufferAttachmentParameterivFunc (target, GL_COLOR_ATTACHMENT0,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &object_type);
	if (object_type == GL_TEXTURE)
	{
		GL_GetFramebufferAttachmentParameterivFunc (target, GL_COLOR_ATTACHMENT0,
			GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object_name);
		return (GLuint)object_name;
	}

	return 0;
}

static void R_FogVol_LogHazardPass (const char *pass,
	GLuint main_tex,
	GLuint history_tex,
	GLuint fog_tex)
{
	GLint draw_fbo = 0;
	GLint read_fbo = 0;
	GLuint draw_tex = 0;
	GLuint read_tex = 0;

	if (!R_FogVol_TestDebugEnabled ())
		return;

	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	draw_tex = R_FogVol_GetFramebufferColorAttachmentTexture (GL_DRAW_FRAMEBUFFER);
	read_tex = R_FogVol_GetFramebufferColorAttachmentTexture (GL_READ_FRAMEBUFFER);

	Con_Printf (
		"FOGVOL_HAZARD pass=%s draw_fbo=%d read_fbo=%d draw_tex=%u read_tex=%u input_main=%u input_history=%u input_fog=%u\n",
		pass,
		draw_fbo,
		read_fbo,
		draw_tex,
		read_tex,
		main_tex,
		history_tex,
		fog_tex);

	if (draw_tex && (main_tex == draw_tex || history_tex == draw_tex || fog_tex == draw_tex))
	{
		Con_Printf ("FOGVOL_HAZARD pass=%s hazard=feedback_loop draw_tex=%u\n", pass, draw_tex);
	}

	if (!q_strcasecmp (pass, "FINAL_COPY") && draw_tex && read_tex && draw_tex == read_tex)
	{
		Con_Printf ("FOGVOL_HAZARD pass=%s hazard=inplace_copy tex=%u\n", pass, draw_tex);
	}
}

static void R_FogVol_AssertNoFeedbackHazard (const char *pass_name, GLuint draw_tex, GLuint read_tex)
{
#if !defined(NDEBUG)
	if (draw_tex && read_tex && draw_tex == read_tex)
		Sys_Error ("Fogvol feedback hazard in pass %s", pass_name);
#else
	(void)pass_name;
	(void)draw_tex;
	(void)read_tex;
#endif
}

static void R_FogVol_AssertNoBoundFeedbackHazard (const char *pass_name)
{
#if !defined(NDEBUG)
	const GLuint draw_tex = R_FogVol_GetFramebufferColorAttachmentTexture (GL_DRAW_FRAMEBUFFER);
	const GLuint read_tex = R_FogVol_GetFramebufferColorAttachmentTexture (GL_READ_FRAMEBUFFER);
	R_FogVol_AssertNoFeedbackHazard (pass_name, draw_tex, read_tex);
#else
	(void)pass_name;
#endif
}

static void R_FogVol_TestState_Capture (fogvol_test_state_t *state)
{
	GLenum draw_color_attachment = GL_BACK_LEFT;
	GLenum read_color_attachment = GL_BACK_LEFT;

	state->statebits = glstate;
	/* BEST PRACTICE #9: cache_statebits mirrors glstate at capture time.
	 * Both fields currently hold identical values; if the engine ever
	 * separates its internal cache bitfield from the applied bitfield,
	 * record the cache-side value here independently. */
	state->cache_statebits = glstate;
	glGetIntegerv (GL_VIEWPORT, state->viewport);
	state->scissor_test = glIsEnabled (GL_SCISSOR_TEST);
	glGetIntegerv (GL_SCISSOR_BOX, state->scissor_box);
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &state->draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &state->read_fbo);
	glGetIntegerv (GL_DRAW_BUFFER, &state->draw_buffer);
	glGetIntegerv (GL_READ_BUFFER, &state->read_buffer);
	state->num_draw_buffers = 0;
	for (int i = 0; i < (int)countof (state->draw_buffers); ++i)
		state->draw_buffers[i] = GL_NONE;
	{
		GLint max_draw_buffers = 0;
		glGetIntegerv (GL_MAX_DRAW_BUFFERS, &max_draw_buffers);
		state->num_draw_buffers = q_min (max_draw_buffers, (int)countof (state->draw_buffers));
		for (int i = 0; i < state->num_draw_buffers; ++i)
			glGetIntegerv (GL_DRAW_BUFFER0 + i, &state->draw_buffers[i]);
	}
	state->draw_status = (GLint)GL_CheckFramebufferStatusFunc (GL_DRAW_FRAMEBUFFER);
	state->read_status = (GLint)GL_CheckFramebufferStatusFunc (GL_READ_FRAMEBUFFER);
	if (state->draw_fbo)
		draw_color_attachment = GL_COLOR_ATTACHMENT0;
	if (state->read_fbo)
		read_color_attachment = GL_COLOR_ATTACHMENT0;
	GL_GetFramebufferAttachmentParameterivFunc (GL_DRAW_FRAMEBUFFER, draw_color_attachment,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &state->draw_color_type[0]);
	GL_GetFramebufferAttachmentParameterivFunc (GL_DRAW_FRAMEBUFFER, draw_color_attachment,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &state->draw_color_name[0]);
	if (state->draw_fbo)
	{
		GL_GetFramebufferAttachmentParameterivFunc (GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
			GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &state->draw_color_type[1]);
		GL_GetFramebufferAttachmentParameterivFunc (GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
			GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &state->draw_color_name[1]);
	}
	else
	{
		state->draw_color_type[1] = GL_NONE;
		state->draw_color_name[1] = 0;
	}
	GL_GetFramebufferAttachmentParameterivFunc (GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &state->draw_depth_type);
	GL_GetFramebufferAttachmentParameterivFunc (GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &state->draw_depth_name);
	GL_GetFramebufferAttachmentParameterivFunc (GL_READ_FRAMEBUFFER, read_color_attachment,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &state->read_color_type);
	GL_GetFramebufferAttachmentParameterivFunc (GL_READ_FRAMEBUFFER, read_color_attachment,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &state->read_color_name);
	GL_GetFramebufferAttachmentParameterivFunc (GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &state->read_depth_type);
	GL_GetFramebufferAttachmentParameterivFunc (GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &state->read_depth_name);
	glGetIntegerv (GL_CURRENT_PROGRAM, &state->program);
	glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &state->vao);
	state->blend = glIsEnabled (GL_BLEND);
	glGetIntegerv (GL_BLEND_SRC_RGB, &state->blend_src_rgb);
	glGetIntegerv (GL_BLEND_DST_RGB, &state->blend_dst_rgb);
	glGetIntegerv (GL_BLEND_EQUATION_RGB, &state->blend_equation_rgb);
	state->depth_test = glIsEnabled (GL_DEPTH_TEST);
	glGetBooleanv (GL_DEPTH_WRITEMASK, &state->depth_writemask);
	glGetBooleanv (GL_COLOR_WRITEMASK, state->color_writemask);
	state->cull_face = glIsEnabled (GL_CULL_FACE);
	glGetIntegerv (GL_POLYGON_MODE, state->polygon_mode);
	glGetFloatv (GL_COLOR_CLEAR_VALUE, state->color_clear_value);
	state->framebuffer_srgb = glIsEnabled (GL_FRAMEBUFFER_SRGB);
	state->dither = glIsEnabled (GL_DITHER);
	state->multisample = glIsEnabled (GL_MULTISAMPLE);
	state->cache_draw_fbo = r_fogvol_state_cache.draw_fbo;
	state->cache_read_fbo = r_fogvol_state_cache.read_fbo;
	state->cache_program = r_fogvol_state_cache.program;
	state->cache_vao = r_fogvol_state_cache.vao;
}


static void R_FogVol_TestState_Log (const char *phase, const fogvol_test_state_t *state)
{
	Con_Printf (
		"FOGVOL_TEST %s viewport=(%d %d %d %d) scissor_test=%d scissor_box=(%d %d %d %d) "
		"draw_fbo=%d read_fbo=%d draw_buffer=0x%04x read_buffer=0x%04x "
		"draw_buffers=[0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x 0x%04x] "
		"draw_status=0x%04x read_status=0x%04x "
		"draw_att0=(type=0x%04x,name=%d) draw_att1=(type=0x%04x,name=%d) draw_depth=(type=0x%04x,name=%d) "
		"read_att0=(type=0x%04x,name=%d) read_depth=(type=0x%04x,name=%d) "
		"prog=%d vao=%d blend=%d blend_func=(%d,%d) blend_eq=%d "
		"depth_test=%d depth_writemask=%d color_writemask=(%d %d %d %d) cull=%d poly=(%d,%d) "
		"clear_color=(%.3f %.3f %.3f %.3f) srgb=%d dither=%d multisample=%d glstate=0x%08x "
		"cache(glstate=0x%08x draw_fbo=%d read_fbo=%d prog=%d vao=%d)\n",
		phase,
		state->viewport[0], state->viewport[1], state->viewport[2], state->viewport[3],
		state->scissor_test,
		state->scissor_box[0], state->scissor_box[1], state->scissor_box[2], state->scissor_box[3],
		state->draw_fbo, state->read_fbo,
		(unsigned)state->draw_buffer, (unsigned)state->read_buffer,
		(unsigned)state->draw_buffers[0], (unsigned)state->draw_buffers[1],
		(unsigned)state->draw_buffers[2], (unsigned)state->draw_buffers[3],
		(unsigned)state->draw_buffers[4], (unsigned)state->draw_buffers[5],
		(unsigned)state->draw_buffers[6], (unsigned)state->draw_buffers[7],
		(unsigned)state->draw_status, (unsigned)state->read_status,
		(unsigned)state->draw_color_type[0], state->draw_color_name[0],
		(unsigned)state->draw_color_type[1], state->draw_color_name[1],
		(unsigned)state->draw_depth_type, state->draw_depth_name,
		(unsigned)state->read_color_type, state->read_color_name,
		(unsigned)state->read_depth_type, state->read_depth_name,
		state->program,
		state->vao,
		state->blend,
		state->blend_src_rgb, state->blend_dst_rgb,
		state->blend_equation_rgb,
		state->depth_test,
		state->depth_writemask,
		state->color_writemask[0], state->color_writemask[1], state->color_writemask[2], state->color_writemask[3],
		state->cull_face,
		state->polygon_mode[0], state->polygon_mode[1],
		state->color_clear_value[0], state->color_clear_value[1], state->color_clear_value[2], state->color_clear_value[3],
		state->framebuffer_srgb,
		state->dither,
		state->multisample,
		state->statebits,
		state->cache_statebits,
		state->cache_draw_fbo,
		state->cache_read_fbo,
		state->cache_program,
		state->cache_vao);
}


static void R_FogVol_CaptureRestoreState (fogvol_restore_state_t *state)
{
	glGetIntegerv (GL_VIEWPORT, state->viewport);
	state->scissor_test = glIsEnabled (GL_SCISSOR_TEST);
	glGetIntegerv (GL_SCISSOR_BOX, state->scissor_box);
	glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &state->draw_fbo);
	glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &state->read_fbo);
	glGetIntegerv (GL_DRAW_BUFFER, &state->draw_buffer);
	glGetIntegerv (GL_READ_BUFFER, &state->read_buffer);
	glGetIntegerv (GL_CURRENT_PROGRAM, &state->program);
	glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &state->vao);
	glGetIntegerv (GL_ACTIVE_TEXTURE, &state->active_texture);
	state->glstate_bits = glstate;
	state->framebuffer_srgb = glIsEnabled (GL_FRAMEBUFFER_SRGB);
}

static void R_FogVol_Restore3DRenderState (const fogvol_restore_state_t *state)
{
	R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, state->draw_fbo);
	R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, state->read_fbo);
	glDrawBuffer ((GLenum)state->draw_buffer);
	glReadBuffer ((GLenum)state->read_buffer);
	glViewport (state->viewport[0], state->viewport[1], state->viewport[2], state->viewport[3]);
	if (state->scissor_test)
		GL_SetScissorEnabled (true);
	else
		GL_SetScissorEnabled (false);
	glScissor (state->scissor_box[0], state->scissor_box[1], state->scissor_box[2], state->scissor_box[3]);
	R_FogVol_UseProgram ((GLuint)state->program);
	R_FogVol_BindVertexArray ((GLuint)state->vao);
	GL_ActiveTextureFunc ((GLenum)state->active_texture);

	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	R_FogVol_SetDepthMask (true);
	glPolygonMode (GL_FRONT_AND_BACK, GL_FILL);
	GL_SetState (state->glstate_bits);
	if (state->framebuffer_srgb)
		glEnable (GL_FRAMEBUFFER_SRGB);
	else
		glDisable (GL_FRAMEBUFFER_SRGB);
}

static int R_FogVol_ComparePriority (const void *a, const void *b)
{
	const fog_volume_t *va = (const fog_volume_t *)a;
	const fog_volume_t *vb = (const fog_volume_t *)b;

	if (va->priority < vb->priority)
		return -1;
	if (va->priority > vb->priority)
		return 1;
	return 0;
}

static int R_FogVol_CompareLightCandidate (const void *a, const void *b)
{
	const fogvol_light_candidate_t *la = (const fogvol_light_candidate_t *)a;
	const fogvol_light_candidate_t *lb = (const fogvol_light_candidate_t *)b;

	if (la->score > lb->score)
		return -1;
	if (la->score < lb->score)
		return 1;
	if (la->index < lb->index)
		return -1;
	if (la->index > lb->index)
		return 1;
	return 0;
}

static float R_FogVol_DistanceToVolume (const fog_volume_t *volume, const vec3_t p)
{
	if (!volume)
		return 999999.f;

	if (volume->shape == FOGVOL_SHAPE_SPHERE)
	{
		vec3_t d;
		VectorSubtract (p, volume->sphereCenter, d);
		return q_max (0.f, VectorLength (d) - q_max (0.f, volume->sphereRadius));
	}

	{
		float dx = q_max (q_max (volume->mins[0] - p[0], 0.f), p[0] - volume->maxs[0]);
		float dy = q_max (q_max (volume->mins[1] - p[1], 0.f), p[1] - volume->maxs[1]);
		float dz = q_max (q_max (volume->mins[2] - p[2], 0.f), p[2] - volume->maxs[2]);
		return sqrtf (dx * dx + dy * dy + dz * dz);
	}
}

static qboolean R_FogVol_LightOverlapsVolume (const fog_volume_t *volume, const vec3_t light_origin, float light_radius)
{
	float dist;

	if (!volume || light_radius <= 0.f)
		return false;

	dist = R_FogVol_DistanceToVolume (volume, light_origin);
	return dist <= light_radius;
}

static void R_FogVol_GetVolumeBounds (const fog_volume_t *vol, vec3_t bmins, vec3_t bmaxs)
{
	if (vol->shape == FOGVOL_SHAPE_SPHERE)
	{
		for (int a = 0; a < 3; ++a)
		{
			bmins[a] = vol->sphereCenter[a] - vol->sphereRadius;
			bmaxs[a] = vol->sphereCenter[a] + vol->sphereRadius;
		}
	}
	else
	{
		VectorCopy (vol->mins, bmins);
		VectorCopy (vol->maxs, bmaxs);
	}
}

static qboolean R_FogVol_LightOverlapsBounds (const vec3_t light_origin, float light_radius, const vec3_t bmins, const vec3_t bmaxs)
{
	float dist2 = 0.f;

	for (int a = 0; a < 3; ++a)
	{
		if (light_origin[a] < bmins[a])
		{
			float d = bmins[a] - light_origin[a];
			dist2 += d * d;
		}
		else if (light_origin[a] > bmaxs[a])
		{
			float d = light_origin[a] - bmaxs[a];
			dist2 += d * d;
		}
	}

	return dist2 <= light_radius * light_radius;
}

static qboolean R_FogVol_BoundsOverlap (const vec3_t mins0, const vec3_t maxs0, const vec3_t mins1, const vec3_t maxs1)
{
	for (int a = 0; a < 3; ++a)
	{
		if (maxs0[a] < mins1[a] || mins0[a] > maxs1[a])
			return false;
	}
	return true;
}

static int R_FogVol_BuildVisibleVolumeCache (fogvol_visible_volume_t *visible, int max_visible, vec3_t fog_bounds_mins, vec3_t fog_bounds_maxs, qboolean *has_fog_bounds)
{
	int visible_count = 0;

	*has_fog_bounds = false;
	VectorSet (fog_bounds_mins, 0.f, 0.f, 0.f);
	VectorSet (fog_bounds_maxs, 0.f, 0.f, 0.f);

	for (int i = 0; i < r_fogvolume_count; ++i)
	{
		const fog_volume_t *vol = &r_fogvolumes[i];
		vec3_t bmins, bmaxs;
		int sx0, sy0, sx1, sy1;

		R_FogVol_GetVolumeBounds (vol, bmins, bmaxs);

		if (!*has_fog_bounds)
		{
			VectorCopy (bmins, fog_bounds_mins);
			VectorCopy (bmaxs, fog_bounds_maxs);
			*has_fog_bounds = true;
		}
		else
		{
			for (int a = 0; a < 3; ++a)
			{
				fog_bounds_mins[a] = q_min (fog_bounds_mins[a], bmins[a]);
				fog_bounds_maxs[a] = q_max (fog_bounds_maxs[a], bmaxs[a]);
			}
		}

		if (visible_count >= max_visible)
			continue;

		if (R_FogVol_ProjectAABBToScreenRect (vol, &sx0, &sy0, &sx1, &sy1, true))
		{
			const float view_area = (float)(q_max (1, r_refdef.vrect.width) * q_max (1, r_refdef.vrect.height));
			const float rect_area = (float)q_max (1, (sx1 - sx0) * (sy1 - sy0));
			visible[visible_count].volume = vol;
			visible[visible_count].screen_weight = CLAMP (0.1f, rect_area / view_area, 1.f);
			visible_count++;
		}
	}

	return visible_count;
}

static int R_FogVol_BuildFrameLightBroadphase (const fogvol_visible_volume_t *visible_volumes, int visible_count, const vec3_t fog_bounds_mins, const vec3_t fog_bounds_maxs, qboolean has_fog_bounds, fogvol_light_candidate_t *out, int max_out)
{
	const dlight_t *const *active;
	int active_count = 0;
	int candidate_count = 0;

	if (!out || max_out <= 0 || visible_count <= 0)
		return 0;

	active = DLightPool_GetActiveList (&active_count);
	if (active && active_count > 0)
	{
		for (int i = 0; i < active_count && candidate_count < max_out; ++i)
		{
			const dlight_t *dl = active[i];
			vec3_t to_light;
			float overlap_score = 0.f;
			float score;
			float intensity;
			float radius;

			if (!R_FogVol_DlightIsInjectable (dl))
				continue;

			radius = dl->radius;
			if (radius <= 0.f)
				continue;

			intensity = q_max (dl->color[0], q_max (dl->color[1], dl->color[2]));
			if (intensity <= 0.f)
				continue;

			if (active_count > 192 && has_fog_bounds)
			{
				if (!R_FogVol_LightOverlapsBounds (dl->origin, radius * 4.f, fog_bounds_mins, fog_bounds_maxs))
					continue;
			}

			for (int v = 0; v < visible_count; ++v)
			{
				if (!R_FogVol_LightOverlapsVolume (visible_volumes[v].volume, dl->origin, radius))
					continue;
				overlap_score += visible_volumes[v].screen_weight;
			}

			if (overlap_score <= 0.f)
				continue;

			VectorSubtract (dl->origin, r_refdef.vieworg, to_light);
			score = overlap_score * intensity * radius;
			score *= 1.f / (1.f + 0.0025f * sqrtf (DotProduct (to_light, to_light)));
			if (score <= 0.f)
				continue;

			out[candidate_count].index = (dl->id != 0) ? dl->id : i;
			out[candidate_count].score = score;
			out[candidate_count].light.pos_rad[0] = dl->origin[0];
			out[candidate_count].light.pos_rad[1] = dl->origin[1];
			out[candidate_count].light.pos_rad[2] = dl->origin[2];
			out[candidate_count].light.pos_rad[3] = radius;
			out[candidate_count].light.col_int[0] = dl->color[0];
			out[candidate_count].light.col_int[1] = dl->color[1];
			out[candidate_count].light.col_int[2] = dl->color[2];
			out[candidate_count].light.col_int[3] = 1.f;
			out[candidate_count].bounds_mins[0] = dl->origin[0] - radius;
			out[candidate_count].bounds_mins[1] = dl->origin[1] - radius;
			out[candidate_count].bounds_mins[2] = dl->origin[2] - radius;
			out[candidate_count].bounds_maxs[0] = dl->origin[0] + radius;
			out[candidate_count].bounds_maxs[1] = dl->origin[1] + radius;
			out[candidate_count].bounds_maxs[2] = dl->origin[2] + radius;
			candidate_count++;
		}
	}

	for (int i = 0; i < r_num_fog_dlights && candidate_count < max_out; ++i)
	{
		const dlight_t *dl = &r_fog_dlights[i];
		float overlap_score = 0.f;

		if (!R_FogVol_DlightIsInjectable (dl))
			continue;

		for (int v = 0; v < visible_count; ++v)
		{
			if (!R_FogVol_LightOverlapsVolume (visible_volumes[v].volume, dl->origin, dl->radius))
				continue;
			overlap_score += visible_volumes[v].screen_weight;
		}

		if (overlap_score <= 0.f)
			continue;

		out[candidate_count].index = 100000 + i;
		out[candidate_count].score = overlap_score * dl->radius;
		out[candidate_count].light.pos_rad[0] = dl->origin[0];
		out[candidate_count].light.pos_rad[1] = dl->origin[1];
		out[candidate_count].light.pos_rad[2] = dl->origin[2];
		out[candidate_count].light.pos_rad[3] = dl->radius;
		out[candidate_count].light.col_int[0] = dl->color[0];
		out[candidate_count].light.col_int[1] = dl->color[1];
		out[candidate_count].light.col_int[2] = dl->color[2];
		out[candidate_count].light.col_int[3] = 1.f;
		out[candidate_count].bounds_mins[0] = dl->origin[0] - dl->radius;
		out[candidate_count].bounds_mins[1] = dl->origin[1] - dl->radius;
		out[candidate_count].bounds_mins[2] = dl->origin[2] - dl->radius;
		out[candidate_count].bounds_maxs[0] = dl->origin[0] + dl->radius;
		out[candidate_count].bounds_maxs[1] = dl->origin[1] + dl->radius;
		out[candidate_count].bounds_maxs[2] = dl->origin[2] + dl->radius;
		candidate_count++;
	}


	for (int i = 0; i < r_num_fog_static_lights && candidate_count < max_out; ++i)
	{
		const fog_light_gpu_t *sl = &r_fog_static_lights[i];
		float overlap_score = 0.f;
		float radius = sl->pos_rad[3];

		if (radius <= 0.f)
			continue;

		for (int v = 0; v < visible_count; ++v)
		{
			if (!R_FogVol_LightOverlapsVolume (visible_volumes[v].volume, sl->pos_rad, radius))
				continue;
			overlap_score += visible_volumes[v].screen_weight;
		}

		if (overlap_score <= 0.f)
			continue;

		out[candidate_count].index = 200000 + i;
		out[candidate_count].score = overlap_score * radius;
		out[candidate_count].light = *sl;
		out[candidate_count].bounds_mins[0] = sl->pos_rad[0] - radius;
		out[candidate_count].bounds_mins[1] = sl->pos_rad[1] - radius;
		out[candidate_count].bounds_mins[2] = sl->pos_rad[2] - radius;
		out[candidate_count].bounds_maxs[0] = sl->pos_rad[0] + radius;
		out[candidate_count].bounds_maxs[1] = sl->pos_rad[1] + radius;
		out[candidate_count].bounds_maxs[2] = sl->pos_rad[2] + radius;
		candidate_count++;
	}

	if (candidate_count > 1)
		qsort (out, candidate_count, sizeof (out[0]), R_FogVol_CompareLightCandidate);

	return candidate_count;
}

static float R_FogVol_SRGBToLinear (float c)
{
	if (c <= 0.04045f)
		return c / 12.92f;
	return powf ((c + 0.055f) / 1.055f, 2.4f);
}

static qboolean R_FogVol_TryGetSurfaceLightSampleAtTexel (const qmodel_t *model, const msurface_t *surf, int sample_s, int sample_t, vec3_t out_rgb)
{
	const byte *samples;
	int bytes_per_pixel;
	int smax;
	int tmax;
	int facesize;
	float accum[3] = {0.f, 0.f, 0.f};
	float accum_weight = 0.f;
	const qboolean use_rgb = (model && model->has_lightdata_rgb && model->lightdata && model->lightdata_rgb);

	if (!model || !surf || !surf->samples)
		return false;

	samples = surf->samples;
	bytes_per_pixel = (model->flags & MOD_HDRLIGHTING) ? 4 : 3;
	if (use_rgb)
	{
		const ptrdiff_t offset = samples - model->lightdata;
		if (offset >= 0 && bytes_per_pixel > 0 && (offset % bytes_per_pixel) == 0)
		{
			const size_t sample_offset = (size_t)offset / (size_t)bytes_per_pixel;
			const size_t rgb_offset = sample_offset * 3;
			if (rgb_offset + 3 <= (size_t)model->lightdata_rgb_size)
			{
				samples = model->lightdata_rgb + rgb_offset;
				bytes_per_pixel = 3;
			}
		}
	}

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	if (smax <= 0 || tmax <= 0)
		return false;
	facesize = smax * tmax * bytes_per_pixel;
	sample_s = CLAMP (0, sample_s, smax - 1);
	sample_t = CLAMP (0, sample_t, tmax - 1);

	for (int map = 0; map < MAXLIGHTMAPS && surf->styles[map] != INVALID_LIGHTSTYLE; ++map)
	{
		const float style_scale = ((surf->styles[map] < 256) ? d_lightstylevalue[surf->styles[map]] : d_lightstylevalue[0]) * (1.f / 256.f);
		const byte *lightmap = samples + facesize * map;
		const byte *texel = lightmap + (sample_t * smax + sample_s) * bytes_per_pixel;
		accum[0] += style_scale * texel[0];
		accum[1] += style_scale * texel[1];
		accum[2] += style_scale * texel[2];
		accum_weight += style_scale;
	}

	if (accum_weight <= 0.f)
		return false;

	out_rgb[0] = q_max (0.f, accum[0] * (1.f / 255.f));
	out_rgb[1] = q_max (0.f, accum[1] * (1.f / 255.f));
	out_rgb[2] = q_max (0.f, accum[2] * (1.f / 255.f));
	if (!q_strcasecmp (r_lightmap_colorspace.string, "srgb"))
	{
		out_rgb[0] = R_FogVol_SRGBToLinear (out_rgb[0]);
		out_rgb[1] = R_FogVol_SRGBToLinear (out_rgb[1]);
		out_rgb[2] = R_FogVol_SRGBToLinear (out_rgb[2]);
	}
	return (out_rgb[0] > 0.f || out_rgb[1] > 0.f || out_rgb[2] > 0.f);
}

static qboolean R_FogVol_TryGetSurfaceLightSample (const qmodel_t *model, const msurface_t *surf, vec3_t out_rgb)
{
	int smax;
	int tmax;

	if (!surf)
		return false;
	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	if (smax <= 0 || tmax <= 0)
		return false;
	return R_FogVol_TryGetSurfaceLightSampleAtTexel (model, surf, smax / 2, tmax / 2, out_rgb);
}

static void R_FogVol_FreeStaticField (void)
{
	if (r_fog_static_field.rgb)
		free (r_fog_static_field.rgb);
	if (r_fog_static_field.weight)
		free (r_fog_static_field.weight);
	R_FogVol_GPUStaticBuildReset ();
	memset (&r_fog_static_field, 0, sizeof (r_fog_static_field));
}

static qboolean R_FogVol_GPUStaticBuildAvailable (void)
{
	return GL_DispatchComputeFunc
		&& GL_MemoryBarrierFunc
		&& GL_MapBufferRangeFunc
		&& GL_UnmapBufferFunc
		&& glprogs.fogvol_static_build;
}

static void R_FogVol_GPUStaticBuildReset (void)
{
	if (GL_DeleteBuffersFunc)
	{
		if (r_fogvol_static_rgb_ssbo)
			GL_DeleteBuffersFunc (1, &r_fogvol_static_rgb_ssbo);
		if (r_fogvol_static_weight_ssbo)
			GL_DeleteBuffersFunc (1, &r_fogvol_static_weight_ssbo);
		if (r_fogvol_static_out_ssbo)
			GL_DeleteBuffersFunc (1, &r_fogvol_static_out_ssbo);
	}

	r_fogvol_static_rgb_ssbo = 0;
	r_fogvol_static_weight_ssbo = 0;
	r_fogvol_static_out_ssbo = 0;
	r_fogvol_static_capacity_cells = 0;
}

static qboolean R_FogVol_GPUStaticBuildEnsureBuffers (int total_cells)
{
	if (!R_FogVol_GPUStaticBuildAvailable () || total_cells <= 0)
		return false;

	if (!r_fogvol_static_rgb_ssbo)
		GL_GenBuffersFunc (1, &r_fogvol_static_rgb_ssbo);
	if (!r_fogvol_static_weight_ssbo)
		GL_GenBuffersFunc (1, &r_fogvol_static_weight_ssbo);
	if (!r_fogvol_static_out_ssbo)
		GL_GenBuffersFunc (1, &r_fogvol_static_out_ssbo);
	if (!r_fogvol_static_rgb_ssbo || !r_fogvol_static_weight_ssbo || !r_fogvol_static_out_ssbo)
		return false;

	if (r_fogvol_static_capacity_cells < total_cells)
	{
		r_fogvol_static_capacity_cells = total_cells;
		GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fogvol_static_rgb_ssbo);
		GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, sizeof (float) * 3 * r_fogvol_static_capacity_cells, NULL, GL_DYNAMIC_DRAW);
		GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fogvol_static_weight_ssbo);
		GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, sizeof (float) * r_fogvol_static_capacity_cells, NULL, GL_DYNAMIC_DRAW);
	}

	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fogvol_static_out_ssbo);
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, sizeof (fog_light_gpu_t) * MAX_FOG_STATIC_LIGHTS, NULL, GL_DYNAMIC_DRAW);
	return true;
}

static qboolean R_FogVol_SummarizeStaticLightsFromFieldGPU (int nx, int ny, int nz, int total, float sx, float sy, float sz, float radius, int stride)
{
	const int out_slots = q_min (MAX_FOG_STATIC_LIGHTS, (total + stride - 1) / stride);
	const int groups = (out_slots + (FOGVOL_GPU_LOCAL_SIZE - 1)) / FOGVOL_GPU_LOCAL_SIZE;
	fog_light_gpu_t gpu_out[MAX_FOG_STATIC_LIGHTS];
	void *mapped;

	if (out_slots <= 0)
		return false;
	if (!R_FogVol_GPUStaticBuildEnsureBuffers (total))
		return false;

	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fogvol_static_rgb_ssbo);
	GL_BufferSubDataFunc (GL_SHADER_STORAGE_BUFFER, 0, sizeof (float) * 3 * total, r_fog_static_field.rgb);
	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fogvol_static_weight_ssbo);
	GL_BufferSubDataFunc (GL_SHADER_STORAGE_BUFFER, 0, sizeof (float) * total, r_fog_static_field.weight);

	GL_UseProgram (glprogs.fogvol_static_build);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, r_fogvol_static_rgb_ssbo, 0, sizeof (float) * 3 * q_max (1, total));
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, r_fogvol_static_weight_ssbo, 0, sizeof (float) * q_max (1, total));
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 2, r_fogvol_static_out_ssbo, 0, sizeof (fog_light_gpu_t) * MAX_FOG_STATIC_LIGHTS);
	GL_Uniform4fFunc (0, (float)total, (float)stride, (float)out_slots, (float)nx);
	GL_Uniform4fFunc (1, (float)ny, (float)nz, 0.f, 0.f);
	GL_Uniform4fFunc (2, r_fog_static_field.mins[0], r_fog_static_field.mins[1], r_fog_static_field.mins[2], 0.f);
	GL_Uniform4fFunc (3, sx, sy, sz, radius);
	GL_Uniform4fFunc (4, 0.65f, 0.f, 0.f, 0.f);
	GL_DispatchComputeFunc (groups, 1, 1);
	GL_MemoryBarrierFunc (GL_SHADER_STORAGE_BARRIER_BIT);

	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fogvol_static_out_ssbo);
	mapped = GL_MapBufferRangeFunc (GL_SHADER_STORAGE_BUFFER, 0, sizeof (fog_light_gpu_t) * out_slots, GL_MAP_READ_BIT);
	if (!mapped)
		return false;
	memcpy (gpu_out, mapped, sizeof (fog_light_gpu_t) * out_slots);
	if (!GL_UnmapBufferFunc (GL_SHADER_STORAGE_BUFFER))
		return false;

	r_num_fog_static_lights = 0;
	for (int i = 0; i < out_slots && r_num_fog_static_lights < MAX_FOG_STATIC_LIGHTS; ++i)
	{
		const fog_light_gpu_t *src = &gpu_out[i];
		fog_light_gpu_t *dst;

		if (src->col_int[3] <= 0.f || src->pos_rad[3] <= 0.f)
			continue;
		dst = &r_fog_static_lights[r_num_fog_static_lights++];
		*dst = *src;
	}

	return true;
}

static qboolean R_FogVol_WorldToStaticFieldCell (const vec3_t pos, int out_cell[3])
{
	for (int axis = 0; axis < 3; ++axis)
	{
		float f;
		if (!r_fog_static_field.valid || r_fog_static_field.dims[axis] <= 0)
			return false;
		f = (pos[axis] - r_fog_static_field.mins[axis]) * r_fog_static_field.inv_size[axis] * (float)r_fog_static_field.dims[axis];
		out_cell[axis] = CLAMP (0, (int)f, r_fog_static_field.dims[axis] - 1);
	}
	return true;
}

static qboolean R_FogVol_SurfaceTexelToWorld (const msurface_t *surf, float sample_s, float sample_t, vec3_t out_pos)
{
	vec3_t row0, row1, row2;
	vec3_t rhs;
	vec3_t cross12, cross20, cross01;
	float det;

	if (!surf || !surf->texinfo || !surf->plane)
		return false;

	VectorSet (row0, surf->texinfo->vecs[0][0], surf->texinfo->vecs[0][1], surf->texinfo->vecs[0][2]);
	VectorSet (row1, surf->texinfo->vecs[1][0], surf->texinfo->vecs[1][1], surf->texinfo->vecs[1][2]);
	VectorCopy (surf->plane->normal, row2);
	VectorSet (rhs,
		(float)surf->texturemins[0] + sample_s * 16.f - surf->texinfo->vecs[0][3],
		(float)surf->texturemins[1] + sample_t * 16.f - surf->texinfo->vecs[1][3],
		surf->plane->dist);

	CrossProduct (row1, row2, cross12);
	det = DotProduct (row0, cross12);
	if (fabsf (det) < 1e-6f)
		return false;
	CrossProduct (rhs, row2, cross20);
	CrossProduct (row1, rhs, cross01);
	out_pos[0] = DotProduct (rhs, cross12) / det;
	out_pos[1] = DotProduct (row0, cross20) / det;
	out_pos[2] = DotProduct (row0, cross01) / det;
	return true;
}

static void R_FogVol_SummarizeStaticLightsFromField (void)
{
	const int nx = r_fog_static_field.dims[0];
	const int ny = r_fog_static_field.dims[1];
	const int nz = r_fog_static_field.dims[2];
	const int total = nx * ny * nz;
	const float sx = (r_fog_static_field.maxs[0] - r_fog_static_field.mins[0]) / (float)q_max (1, nx);
	const float sy = (r_fog_static_field.maxs[1] - r_fog_static_field.mins[1]) / (float)q_max (1, ny);
	const float sz = (r_fog_static_field.maxs[2] - r_fog_static_field.mins[2]) / (float)q_max (1, nz);
	const float radius = CLAMP (32.f, 1.25f * q_max (sx, q_max (sy, sz)), 320.f);
	int stride;

	r_num_fog_static_lights = 0;
	if (!r_fog_static_field.valid || total <= 0)
		return;

	stride = q_max (1, total / MAX_FOG_STATIC_LIGHTS);
	if (r_fogvol_gpu_static_build.value > 0.f)
	{
		if (R_FogVol_SummarizeStaticLightsFromFieldGPU (nx, ny, nz, total, sx, sy, sz, radius, stride))
			return;
	}

	for (int i = 0; i < total && r_num_fog_static_lights < MAX_FOG_STATIC_LIGHTS; i += stride)
	{
		const float w = r_fog_static_field.weight[i];
		fog_light_gpu_t *dst;
		int x, y, z;
		if (w <= 0.f)
			continue;
		x = i % nx;
		y = (i / nx) % ny;
		z = i / (nx * ny);
		dst = &r_fog_static_lights[r_num_fog_static_lights++];
		dst->pos_rad[0] = r_fog_static_field.mins[0] + ((float)x + 0.5f) * sx;
		dst->pos_rad[1] = r_fog_static_field.mins[1] + ((float)y + 0.5f) * sy;
		dst->pos_rad[2] = r_fog_static_field.mins[2] + ((float)z + 0.5f) * sz;
		dst->pos_rad[3] = radius;
		dst->col_int[0] = q_max (0.f, r_fog_static_field.rgb[i * 3 + 0] / w) * 0.65f;
		dst->col_int[1] = q_max (0.f, r_fog_static_field.rgb[i * 3 + 1] / w) * 0.65f;
		dst->col_int[2] = q_max (0.f, r_fog_static_field.rgb[i * 3 + 2] / w) * 0.65f;
		dst->col_int[3] = 1.f;
	}
}

static void R_FogVol_ContinueStaticFieldBuild (int sample_budget)
{
	qmodel_t *model = cl.worldmodel;
	const float density = q_max (0.25f, r_fogvol_static_probe_density.value);
	const int texel_step = CLAMP (1, (int)Q_rint (2.5f / density), 8);

	if (!r_fog_static_field.valid || r_fog_static_field.build_complete)
		return;
	if (!model || model->type != mod_brush || !model->surfaces)
		return;

	while (r_fog_static_field.next_surface < model->numsurfaces && sample_budget > 0)
	{
		msurface_t *surf = &model->surfaces[r_fog_static_field.next_surface++];
		vec3_t sample_rgb;
		vec3_t sample_pos;
		int smax;
		int tmax;

		if (!surf->samples)
			continue;
		if (!surf->texinfo || surf->texinfo->texnum < 0 || surf->texinfo->texnum >= model->numtextures)
			continue;
		if (!model->textures[surf->texinfo->texnum] || (surf->texinfo->flags & TEX_SPECIAL))
			continue;

		smax = (surf->extents[0] >> 4) + 1;
		tmax = (surf->extents[1] >> 4) + 1;
		if (smax <= 0 || tmax <= 0)
			continue;

		for (int t = 0; t < tmax && sample_budget > 0; t += texel_step)
		{
			for (int s = 0; s < smax && sample_budget > 0; s += texel_step)
			{
				int cell[3];
				int idx;
				if (!R_FogVol_TryGetSurfaceLightSampleAtTexel (model, surf, s, t, sample_rgb))
				{
					sample_budget--;
					continue;
				}
				if (!R_FogVol_SurfaceTexelToWorld (surf, (float)s, (float)t, sample_pos))
				{
					sample_budget--;
					continue;
				}
				if (!R_FogVol_WorldToStaticFieldCell (sample_pos, cell))
				{
					sample_budget--;
					continue;
				}
				idx = cell[0] + r_fog_static_field.dims[0] * (cell[1] + r_fog_static_field.dims[1] * cell[2]);
				r_fog_static_field.rgb[idx * 3 + 0] += sample_rgb[0];
				r_fog_static_field.rgb[idx * 3 + 1] += sample_rgb[1];
				r_fog_static_field.rgb[idx * 3 + 2] += sample_rgb[2];
				r_fog_static_field.weight[idx] += 1.f;
				r_fog_static_field.total_samples++;
				sample_budget--;
			}
		}
	}

	if (r_fog_static_field.next_surface >= model->numsurfaces)
	{
		r_fog_static_field.build_complete = true;
		R_FogVol_SummarizeStaticLightsFromField ();
	}
}

static void R_FogVol_BuildStaticLightInjection (void)
{
	qmodel_t *model = cl.worldmodel;
	int dims;
	size_t total_cells;

	r_num_fog_static_lights = 0;
	R_FogVol_FreeStaticField ();
	if (!model || model->type != mod_brush || model->numsurfaces <= 0)
		return;
	if (!model->lightdata)
		return;

	dims = CLAMP (8, (int)Q_rint (r_fogvol_static_probe_grid.value), 64);
	total_cells = (size_t)dims * (size_t)dims * (size_t)dims;
	r_fog_static_field.rgb = (float *)calloc (total_cells * 3u, sizeof (float));
	r_fog_static_field.weight = (float *)calloc (total_cells, sizeof (float));
	if (!r_fog_static_field.rgb || !r_fog_static_field.weight)
	{
		R_FogVol_FreeStaticField ();
		return;
	}

	r_fog_static_field.dims[0] = dims;
	r_fog_static_field.dims[1] = dims;
	r_fog_static_field.dims[2] = dims;
	VectorCopy (model->mins, r_fog_static_field.mins);
	VectorCopy (model->maxs, r_fog_static_field.maxs);
	for (int axis = 0; axis < 3; ++axis)
	{
		const float extent = r_fog_static_field.maxs[axis] - r_fog_static_field.mins[axis];
		r_fog_static_field.inv_size[axis] = (extent > 1e-6f) ? (1.f / extent) : 1.f;
	}
	r_fog_static_field.valid = true;
	r_fog_static_field.build_complete = false;
	r_fog_static_field.next_surface = 0;
	r_fog_static_field.total_samples = 0;

	if (r_fogvol_static_probe_mode.value <= 0.f)
	{
		/* low-end mode: preserve legacy pseudo-light list path */
		for (int i = 0, stride = q_max (1, model->numsurfaces / MAX_FOG_STATIC_LIGHTS);
			i < model->numsurfaces && r_num_fog_static_lights < MAX_FOG_STATIC_LIGHTS; i += stride)
		{
			msurface_t *surf = &model->surfaces[i];
			fog_light_gpu_t *dst;
			vec3_t center;
			vec3_t color;
			float radius;
			if (!surf->samples)
				continue;
			if (!surf->texinfo || surf->texinfo->texnum < 0 || surf->texinfo->texnum >= model->numtextures)
				continue;
			if (!model->textures[surf->texinfo->texnum] || (surf->texinfo->flags & TEX_SPECIAL))
				continue;
			if (!R_FogVol_TryGetSurfaceLightSample (model, surf, color))
				continue;
			center[0] = 0.5f * (surf->mins[0] + surf->maxs[0]);
			center[1] = 0.5f * (surf->mins[1] + surf->maxs[1]);
			center[2] = 0.5f * (surf->mins[2] + surf->maxs[2]);
			radius = 0.5f * sqrtf (
				(surf->maxs[0] - surf->mins[0]) * (surf->maxs[0] - surf->mins[0]) +
				(surf->maxs[1] - surf->mins[1]) * (surf->maxs[1] - surf->mins[1]) +
				(surf->maxs[2] - surf->mins[2]) * (surf->maxs[2] - surf->mins[2]));
			radius = CLAMP (64.f, radius * 1.5f, 384.f);
			dst = &r_fog_static_lights[r_num_fog_static_lights++];
			dst->pos_rad[0] = center[0];
			dst->pos_rad[1] = center[1];
			dst->pos_rad[2] = center[2];
			dst->pos_rad[3] = radius;
			dst->col_int[0] = color[0] * 0.65f;
			dst->col_int[1] = color[1] * 0.65f;
			dst->col_int[2] = color[2] * 0.65f;
			dst->col_int[3] = 1.f;
		}
		r_fog_static_field.build_complete = true;
	}
	else
	{
		R_FogVol_ContinueStaticFieldBuild (q_max (1, (int)Q_rint (r_fogvol_static_probe_budget_load.value)));
	}

	if (developer.value > 0.f)
	{
		Con_DPrintf ("FogVol: static probes mode=%d lights=%d samples=%d complete=%d\n",
			(int)Q_rint (r_fogvol_static_probe_mode.value), r_num_fog_static_lights,
			r_fog_static_field.total_samples, r_fog_static_field.build_complete ? 1 : 0);
	}
}

static int R_FogVol_BuildLightListForVolume (const fog_volume_t *volume, const fogvol_light_candidate_t *prefiltered, int prefiltered_count, fog_light_gpu_t *out, int max_count, fogvol_light_stats_t *stats)
{
	vec3_t volume_mins;
	vec3_t volume_maxs;
	int copy_count = 0;

	if (!volume || !out || max_count <= 0)
		return 0;
	if (!prefiltered || prefiltered_count <= 0)
		return 0;

	R_FogVol_GetVolumeBounds (volume, volume_mins, volume_maxs);
	for (int i = 0; i < prefiltered_count && copy_count < max_count; ++i)
	{
		const fogvol_light_candidate_t *candidate = &prefiltered[i];
		if (stats)
			stats->narrowphase_candidates++;

		if (!R_FogVol_BoundsOverlap (candidate->bounds_mins, candidate->bounds_maxs, volume_mins, volume_maxs))
			continue;

		out[copy_count++] = candidate->light;
		if (stats)
			stats->narrowphase_accepted++;
	}

	return copy_count;
}

void R_FogVol_RegisterCvars (void)
{
	for (size_t i = 0; i < countof (fogvol_cvar_table); ++i)
	{
		const fogvol_cvar_reg_t *reg = &fogvol_cvar_table[i];
#if !defined(NDEBUG)
		assert (reg->var != NULL);
		assert (reg->category != NULL);
		assert (reg->default_value != NULL);
		assert (reg->var->name != NULL);
		assert (reg->var->string != NULL);
		assert (!q_strcasecmp (reg->var->string, reg->default_value));
#endif
		Cvar_RegisterVariable (reg->var);
	}
}

static void R_FogVol_FreeGodrayReadbackBuffers (void)
{
	if (r_fogvol_godray_mask_cpu)
	{
		free (r_fogvol_godray_mask_cpu);
		r_fogvol_godray_mask_cpu = NULL;
	}
	if (r_fogvol_godray_source_cpu)
	{
		free (r_fogvol_godray_source_cpu);
		r_fogvol_godray_source_cpu = NULL;
	}
	r_fogvol_godray_mask_w = 0;
	r_fogvol_godray_mask_h = 0;
	r_fogvol_godray_source_w = 0;
	r_fogvol_godray_source_h = 0;
}

void R_Froxel_ResetResources (void)
{
	if (r_froxel.light_tex)
		glDeleteTextures (1, &r_froxel.light_tex);
	if (r_froxel.history_tex)
		glDeleteTextures (1, &r_froxel.history_tex);
	if (r_froxel.light_ssbo)
		GL_DeleteBuffersFunc (1, &r_froxel.light_ssbo);
	r_froxel.light_tex = 0;
	r_froxel.history_tex = 0;
	r_froxel.light_ssbo = 0;
	R_FogVol_FreeGodrayReadbackBuffers ();
	r_froxel.dims[0] = 0;
	r_froxel.dims[1] = 0;
	r_froxel.dims[2] = 0;
	r_froxel.light_count = 0;
	r_froxel.prev_frame = 0;
	r_froxel.prev_mode_valid = false;
	r_froxel.prev_valid = false;
	r_froxel.valid = false;
	R_FogVol_GPUSelectReset ();
	r_fogvol_gpu_light_select_state.gpu_frames = 0;
	r_fogvol_gpu_light_select_state.gpu_fallbacks = 0;
	r_fogvol_gpu_light_select_state.last_candidates = 0;
	r_fogvol_gpu_light_select_state.last_selected = 0;
}

static qboolean R_Froxel_EnsureResources (int nx, int ny, int nz)
{
	if (nx <= 0 || ny <= 0 || nz <= 0)
		return false;

	if (r_froxel.dims[0] != nx || r_froxel.dims[1] != ny || r_froxel.dims[2] != nz)
	{
		if (r_froxel.light_tex)
			glDeleteTextures (1, &r_froxel.light_tex);
		if (r_froxel.history_tex)
			glDeleteTextures (1, &r_froxel.history_tex);
		r_froxel.light_tex = 0;
		r_froxel.history_tex = 0;
		r_froxel.dims[0] = nx;
		r_froxel.dims[1] = ny;
		r_froxel.dims[2] = nz;
		r_froxel.prev_valid = false;
	}

	if (!r_froxel.light_tex)
		glGenTextures (1, &r_froxel.light_tex);
	if (!r_froxel.history_tex)
		glGenTextures (1, &r_froxel.history_tex);
	if (!r_froxel.light_ssbo)
		GL_GenBuffersFunc (1, &r_froxel.light_ssbo);

	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_3D, r_froxel.light_tex);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	GL_TexImage3DFunc (GL_TEXTURE_3D, 0, GL_RGBA16F, nx, ny, nz, 0, GL_RGBA, GL_FLOAT, NULL);

	GL_BindNative (GL_TEXTURE0, GL_TEXTURE_3D, r_froxel.history_tex);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	GL_TexImage3DFunc (GL_TEXTURE_3D, 0, GL_RGBA16F, nx, ny, nz, 0, GL_RGBA, GL_FLOAT, NULL);

	return true;
}

void R_Froxel_BeginFrame (float near_clip, float far_clip)
{
	int nx, ny, nz;
	const int lighting_mode = CLAMP (0, (int)Q_rint (r_fogvol_lighting_mode.value), 3);
	const int parity_mode = CLAMP (0, (int)Q_rint (r_fogvol_froxel_parity.value), 1);
	const qboolean froxel_enabled = (r_fogvol_froxel.value > 0.f);

	r_froxel.valid = false;
	if (!froxel_enabled)
		return;

	nx = CLAMP (16, (r_refdef.vrect.width + 15) / 16, 192);
	ny = CLAMP (12, (r_refdef.vrect.height + 15) / 16, 128);
	nz = 32;

	r_froxel.near_clip = q_max (near_clip, 1.f);
	r_froxel.far_clip = q_max (far_clip, r_froxel.near_clip + 1.f);
	r_froxel.tan_half_fov_x = tanf (DEG2RAD (r_refdef.fov_x) * 0.5f);
	r_froxel.tan_half_fov_y = tanf (DEG2RAD (r_refdef.fov_y) * 0.5f);
	r_froxel.log_far_near = logf (r_froxel.far_clip / r_froxel.near_clip);

	if (!R_Froxel_EnsureResources (nx, ny, nz))
		return;

	if (r_froxel.prev_mode_valid
		&& (r_froxel.prev_froxel_enabled != froxel_enabled
			|| r_froxel.prev_lighting_mode != lighting_mode
			|| r_froxel.prev_parity_mode != parity_mode))
		r_froxel.prev_valid = false;

	r_froxel.prev_froxel_enabled = froxel_enabled;
	r_froxel.prev_lighting_mode = lighting_mode;
	r_froxel.prev_parity_mode = parity_mode;
	r_froxel.prev_mode_valid = true;
	r_froxel.light_count = 0;
	r_froxel.valid = true;
}

static void R_Froxel_AddLight (const vec3_t origin, float radius, const vec3_t color, float intensity, uint32_t type)
{
	froxel_gpu_light_t *out;

	if (!r_froxel.valid || r_froxel.light_count >= MAX_FROXEL_GPU_LIGHTS)
		return;
	if (radius <= 0.f || intensity <= 0.f)
		return;
	out = &r_froxel_gpu_lights[r_froxel.light_count++];
	out->pos_rad[0] = origin[0];
	out->pos_rad[1] = origin[1];
	out->pos_rad[2] = origin[2];
	out->pos_rad[3] = radius;
	out->color_intensity[0] = q_max (0.f, color[0]);
	out->color_intensity[1] = q_max (0.f, color[1]);
	out->color_intensity[2] = q_max (0.f, color[2]);
	out->color_intensity[3] = intensity;
	out->type = type;
	out->_pad[0] = out->_pad[1] = out->_pad[2] = 0u;
}

static void R_Froxel_InjectSun (void)
{
	const sun_t *sun;
	vec3_t sun_color;
	float sun_intensity;
	float sun_scale;
	vec3_t inject_origin;
	vec3_t inject_dir;
	float inject_radius;

	if (!r_froxel.valid || r_fogvol_froxel_sun.value <= 0.f)
		return;

	sun = R_GetSun ();
	if (!sun || !R_WorldHasSun ())
		return;

	sun_intensity = q_max (0.f, sun->intensity);
	if (sun_intensity <= 0.f)
		return;

	VectorCopy (sun->color, sun_color);
	if (sun_color[0] <= 0.f && sun_color[1] <= 0.f && sun_color[2] <= 0.f)
		VectorSet (sun_color, 1.f, 1.f, 1.f);

	sun_scale = q_max (0.f, r_fogvol_froxel_sun.value);
	if (sun_scale <= 0.f)
		return;

	VectorScale (sun->dir, -1.f, inject_dir);
	if (VectorNormalize (inject_dir) <= 0.f)
		return;
	inject_radius = q_max (384.f, r_froxel.far_clip * 0.40f);
	VectorMA (r_refdef.vieworg, r_froxel.far_clip * 0.55f, inject_dir, inject_origin);
	R_Froxel_AddLight (inject_origin, inject_radius, sun_color, sun_intensity * sun_scale, (uint32_t)DLIGHT_DEFAULT);
}

typedef struct froxel_dlight_candidate_s
{
	const dlight_t *dl;
	float score;
} froxel_dlight_candidate_t;

static void R_Froxel_InjectStaticLights (void)
{
	float static_scale;

	if (!r_froxel.valid || r_fogvol_froxel_static.value <= 0.f)
		return;

	static_scale = q_max (0.f, r_fogvol_froxel_static.value);
	if (static_scale <= 0.f)
		return;

	if (r_num_fog_static_lights <= 0)
		return;

	for (int i = 0; i < r_num_fog_static_lights; ++i)
	{
		const fog_light_gpu_t *sl = &r_fog_static_lights[i];
		vec3_t color;
		float intensity = q_max (0.f, sl->col_int[3]);

		if (sl->pos_rad[3] <= 0.f || intensity <= 0.f)
			continue;

		color[0] = q_max (0.f, sl->col_int[0]);
		color[1] = q_max (0.f, sl->col_int[1]);
		color[2] = q_max (0.f, sl->col_int[2]);
		if (color[0] <= 0.f && color[1] <= 0.f && color[2] <= 0.f)
			continue;

		R_Froxel_AddLight (sl->pos_rad, sl->pos_rad[3], color, intensity * static_scale, (uint32_t)DLIGHT_DEFAULT);
	}
}

static int R_FogVol_NextPow2 (int value)
{
	int pow2 = 1;
	while (pow2 < value && pow2 < (1 << 30))
		pow2 <<= 1;
	return q_max (1, pow2);
}

static qboolean R_FogVol_GPUSelectAvailable (void)
{
	return GL_DispatchComputeFunc
		&& GL_MemoryBarrierFunc
		&& GL_MapBufferRangeFunc
		&& GL_UnmapBufferFunc
		&& glprogs.fogvol_dlight_score
		&& glprogs.gpu_bitonic_pairs;
}

static void R_FogVol_GPUSelectReset (void)
{
	if (GL_DeleteBuffersFunc)
	{
		if (r_fogvol_gpu_light_select_state.candidate_ssbo)
			GL_DeleteBuffersFunc (1, &r_fogvol_gpu_light_select_state.candidate_ssbo);
		if (r_fogvol_gpu_light_select_state.sort_ssbo)
			GL_DeleteBuffersFunc (1, &r_fogvol_gpu_light_select_state.sort_ssbo);
	}
	r_fogvol_gpu_light_select_state.candidate_ssbo = 0;
	r_fogvol_gpu_light_select_state.sort_ssbo = 0;
	r_fogvol_gpu_light_select_state.sort_capacity = 0;
}

static qboolean R_FogVol_GPUSelectEnsureBuffers (int candidate_count)
{
	const int sort_count = R_FogVol_NextPow2 (candidate_count);

	if (!R_FogVol_GPUSelectAvailable ())
		return false;

	if (!r_fogvol_gpu_light_select_state.candidate_ssbo)
	{
		GL_GenBuffersFunc (1, &r_fogvol_gpu_light_select_state.candidate_ssbo);
		if (!r_fogvol_gpu_light_select_state.candidate_ssbo)
			return false;
		GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fogvol_gpu_light_select_state.candidate_ssbo);
		GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER,
			sizeof (fogvol_dlight_score_gpu_t) * FOGVOL_GPU_DLIGHT_MAX_CANDIDATES, NULL, GL_DYNAMIC_DRAW);
	}

	if (!r_fogvol_gpu_light_select_state.sort_ssbo
		|| r_fogvol_gpu_light_select_state.sort_capacity < sort_count)
	{
		if (!r_fogvol_gpu_light_select_state.sort_ssbo)
			GL_GenBuffersFunc (1, &r_fogvol_gpu_light_select_state.sort_ssbo);
		if (!r_fogvol_gpu_light_select_state.sort_ssbo)
			return false;

		r_fogvol_gpu_light_select_state.sort_capacity = sort_count;
		GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fogvol_gpu_light_select_state.sort_ssbo);
		GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER,
			sizeof (fogvol_sort_entry_gpu_t) * r_fogvol_gpu_light_select_state.sort_capacity, NULL, GL_DYNAMIC_DRAW);
	}

	return true;
}

static void R_FogVol_GPUBitonicSort (GLuint sort_buffer, int count)
{
	const int groups = (count + (FOGVOL_GPU_LOCAL_SIZE - 1)) / FOGVOL_GPU_LOCAL_SIZE;
	int k;

	GL_UseProgram (glprogs.gpu_bitonic_pairs);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, sort_buffer, 0, sizeof (fogvol_sort_entry_gpu_t) * count);

	for (k = 2; k <= count; k <<= 1)
	{
		int j;
		for (j = k >> 1; j > 0; j >>= 1)
		{
			GL_Uniform4fFunc (0, (float)count, (float)j, (float)k, 0.f);
			GL_DispatchComputeFunc (groups, 1, 1);
			GL_MemoryBarrierFunc (GL_SHADER_STORAGE_BARRIER_BIT);
		}
	}
}

static qboolean R_FogVol_GPUReadBuffer (GLuint buffer, size_t size, void *dst)
{
	void *mapped;

	if (!buffer || !dst || !size)
		return false;

	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, buffer);
	mapped = GL_MapBufferRangeFunc (GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)size, GL_MAP_READ_BIT);
	if (!mapped)
		return false;
	memcpy (dst, mapped, size);
	if (!GL_UnmapBufferFunc (GL_SHADER_STORAGE_BUFFER))
		return false;
	return true;
}

static qboolean R_Froxel_InjectDlights_GPU (float dlight_scale, int selected_cap)
{
	const dlight_t *const *active;
	const dlight_t *candidate_ptrs[FOGVOL_GPU_DLIGHT_MAX_CANDIDATES];
	int active_count = 0;
	int candidate_count = 0;
	int sort_count;
	int groups;
	int selected_count = 0;

	active = DLightPool_GetActiveList (&active_count);
	if (active)
	{
		for (int i = 0; i < active_count && candidate_count < FOGVOL_GPU_DLIGHT_MAX_CANDIDATES; ++i)
		{
			const dlight_t *dl = active[i];
			float intensity;
			fogvol_dlight_score_gpu_t *gpu;

			if (!R_FogVol_DlightIsInjectable (dl))
				continue;
			intensity = q_max (dl->color[0], q_max (dl->color[1], dl->color[2]));
			if (intensity <= 0.f || dl->radius <= 0.f)
				continue;

			candidate_ptrs[candidate_count] = dl;
			gpu = &r_fogvol_gpu_light_select_state.staging_candidates[candidate_count];
			gpu->pos_rad[0] = dl->origin[0];
			gpu->pos_rad[1] = dl->origin[1];
			gpu->pos_rad[2] = dl->origin[2];
			gpu->pos_rad[3] = dl->radius;
			gpu->color_bias[0] = dl->color[0];
			gpu->color_bias[1] = dl->color[1];
			gpu->color_bias[2] = dl->color[2];
			gpu->color_bias[3] = R_FogVol_DlightScoreBias (dl);
			gpu->meta[0] = 1u;
			gpu->meta[1] = (uint32_t)((dl->id > 0) ? dl->id : (i + 1));
			gpu->meta[2] = 0u;
			gpu->meta[3] = 0u;
			candidate_count++;
		}
	}

	for (int i = 0; i < r_num_fog_dlights && candidate_count < FOGVOL_GPU_DLIGHT_MAX_CANDIDATES; ++i)
	{
		const dlight_t *dl = &r_fog_dlights[i];
		float intensity;
		fogvol_dlight_score_gpu_t *gpu;

		if (!R_FogVol_DlightIsInjectable (dl))
			continue;
		intensity = q_max (dl->color[0], q_max (dl->color[1], dl->color[2]));
		if (intensity <= 0.f || dl->radius <= 0.f)
			continue;

		candidate_ptrs[candidate_count] = dl;
		gpu = &r_fogvol_gpu_light_select_state.staging_candidates[candidate_count];
		gpu->pos_rad[0] = dl->origin[0];
		gpu->pos_rad[1] = dl->origin[1];
		gpu->pos_rad[2] = dl->origin[2];
		gpu->pos_rad[3] = dl->radius;
		gpu->color_bias[0] = dl->color[0];
		gpu->color_bias[1] = dl->color[1];
		gpu->color_bias[2] = dl->color[2];
		gpu->color_bias[3] = R_FogVol_DlightScoreBias (dl);
		gpu->meta[0] = 1u;
		gpu->meta[1] = 0x80000000u | (uint32_t)i;
		gpu->meta[2] = 0u;
		gpu->meta[3] = 0u;
		candidate_count++;
	}

	r_fogvol_gpu_light_select_state.last_candidates = candidate_count;
	r_fogvol_gpu_light_select_state.last_selected = 0;

	if (candidate_count <= 0)
		return true;
	if (!R_FogVol_GPUSelectEnsureBuffers (candidate_count))
		return false;

	sort_count = R_FogVol_NextPow2 (candidate_count);
	groups = (sort_count + (FOGVOL_GPU_LOCAL_SIZE - 1)) / FOGVOL_GPU_LOCAL_SIZE;

	GL_BindBuffer (GL_SHADER_STORAGE_BUFFER, r_fogvol_gpu_light_select_state.candidate_ssbo);
	GL_BufferSubDataFunc (GL_SHADER_STORAGE_BUFFER, 0,
		sizeof (fogvol_dlight_score_gpu_t) * candidate_count,
		r_fogvol_gpu_light_select_state.staging_candidates);

	GL_UseProgram (glprogs.fogvol_dlight_score);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, r_fogvol_gpu_light_select_state.candidate_ssbo, 0,
		sizeof (fogvol_dlight_score_gpu_t) * q_max (1, candidate_count));
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 1, r_fogvol_gpu_light_select_state.sort_ssbo, 0,
		sizeof (fogvol_sort_entry_gpu_t) * q_max (1, sort_count));
	GL_Uniform4fFunc (0, r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2], (float)candidate_count);
	GL_DispatchComputeFunc (groups, 1, 1);
	GL_MemoryBarrierFunc (GL_SHADER_STORAGE_BARRIER_BIT);

	R_FogVol_GPUBitonicSort (r_fogvol_gpu_light_select_state.sort_ssbo, sort_count);

	if (!R_FogVol_GPUReadBuffer (r_fogvol_gpu_light_select_state.sort_ssbo,
		sizeof (fogvol_sort_entry_gpu_t) * q_min (selected_cap, FOGVOL_GPU_DLIGHT_MAX_CANDIDATES),
		r_fogvol_gpu_light_select_state.staging_sorted))
		return false;

	for (int i = 0; i < selected_cap; ++i)
	{
		const fogvol_sort_entry_gpu_t *entry = &r_fogvol_gpu_light_select_state.staging_sorted[i];
		int candidate_index = (int)entry->index;
		const dlight_t *dl;
		float lum;
		float energy;

		if (entry->key0 == 0xffffffffu)
			break;
		if (candidate_index < 0 || candidate_index >= candidate_count)
			continue;
		dl = candidate_ptrs[candidate_index];
		if (!dl)
			continue;

		lum = q_max (dl->color[0], q_max (dl->color[1], dl->color[2]));
		energy = dlight_scale * (0.5f + lum) * R_FogVol_DlightScoreBias (dl);
		if (dl->type == DLIGHT_ROCKET || dl->type == DLIGHT_EXPLOSION)
			energy *= 1.35f;
		R_Froxel_AddLight (dl->origin, dl->radius, dl->color, energy, (uint32_t)dl->type);
		selected_count++;
	}

	r_fogvol_gpu_light_select_state.gpu_frames++;
	r_fogvol_gpu_light_select_state.last_selected = selected_count;
	return true;
}

void R_Froxel_InjectDlights (void)
{
	const dlight_t *const *active;
	froxel_dlight_candidate_t selected[64];
	int selected_count = 0;
	int selected_cap;
	int active_count = 0;
	const float dlight_scale = q_max (0.f, r_fogvol_dlightscale.value);
	const int froxel_dlight_cap = MAX_FROXEL_GPU_LIGHTS;

	if (!r_froxel.valid || dlight_scale <= 0.f)
		return;

	selected_cap = CLAMP (1, (int)Q_rint (r_fogvol_light_max.value), froxel_dlight_cap);
	selected_cap = CLAMP (1, selected_cap, (int)countof (selected));

	if (r_fogvol_gpu_light_select.value > 0.f)
	{
		if (R_Froxel_InjectDlights_GPU (dlight_scale, selected_cap))
			return;
		r_fogvol_gpu_light_select_state.gpu_fallbacks++;
	}

	active = DLightPool_GetActiveList (&active_count);
	if (active)
	{
		for (int i = 0; i < active_count; ++i)
		{
			const dlight_t *dl = active[i];
			float score;
			float intensity;
			vec3_t to_light;
			int min_index = 0;
			float min_score;

			if (!R_FogVol_DlightIsInjectable (dl))
				continue;
			intensity = q_max (0.f, q_max (dl->color[0], q_max (dl->color[1], dl->color[2])));
			if (intensity <= 0.f)
				continue;
			VectorSubtract (dl->origin, r_refdef.vieworg, to_light);
			score = dl->radius * intensity * R_FogVol_DlightScoreBias (dl) * (1.f / (1.f + 0.0025f * sqrtf (DotProduct (to_light, to_light))));

			if (selected_count < selected_cap)
			{
				selected[selected_count].dl = dl;
				selected[selected_count].score = score;
				selected_count++;
				continue;
			}

			min_score = selected[0].score;
			for (int k = 1; k < selected_count; ++k)
			{
				if (selected[k].score < min_score)
				{
					min_score = selected[k].score;
					min_index = k;
				}
			}
			if (score > min_score)
			{
				selected[min_index].dl = dl;
				selected[min_index].score = score;
			}
		}
	}

	for (int i = 0; i < r_num_fog_dlights; ++i)
	{
		const dlight_t *dl = &r_fog_dlights[i];
		float score;
		float intensity;
		vec3_t to_light;
		int min_index = 0;
		float min_score;

		if (!R_FogVol_DlightIsInjectable (dl))
			continue;
		intensity = q_max (0.f, q_max (dl->color[0], q_max (dl->color[1], dl->color[2])));
		if (intensity <= 0.f)
			continue;
		VectorSubtract (dl->origin, r_refdef.vieworg, to_light);
		score = dl->radius * intensity * R_FogVol_DlightScoreBias (dl) * (1.f / (1.f + 0.0025f * sqrtf (DotProduct (to_light, to_light))));

		if (selected_count < selected_cap)
		{
			selected[selected_count].dl = dl;
			selected[selected_count].score = score;
			selected_count++;
			continue;
		}

		min_score = selected[0].score;
		for (int k = 1; k < selected_count; ++k)
		{
			if (selected[k].score < min_score)
			{
				min_score = selected[k].score;
				min_index = k;
			}
		}
		if (score > min_score)
		{
			selected[min_index].dl = dl;
			selected[min_index].score = score;
		}
	}

	for (int i = 0; i < selected_count; ++i)
	{
		float lum;
		float energy;
		if (!selected[i].dl)
			continue;
		lum = q_max (selected[i].dl->color[0], q_max (selected[i].dl->color[1], selected[i].dl->color[2]));
		energy = dlight_scale * (0.5f + lum) * R_FogVol_DlightScoreBias (selected[i].dl);
		if (selected[i].dl->type == DLIGHT_ROCKET || selected[i].dl->type == DLIGHT_EXPLOSION)
			energy *= 1.35f;
		R_Froxel_AddLight (selected[i].dl->origin, selected[i].dl->radius, selected[i].dl->color, energy, (uint32_t)selected[i].dl->type);
	}

	if (r_fogvol_gpu_light_select.value > 0.f)
	{
		r_fogvol_gpu_light_select_state.last_candidates = active_count + r_num_fog_dlights;
		r_fogvol_gpu_light_select_state.last_selected = selected_count;
	}
}

typedef enum fogvol_lighting_mode_e
{
	FOGVOL_LIGHTING_OFF = 0,
	FOGVOL_LIGHTING_RAYMARCH = 1,
	FOGVOL_LIGHTING_FROXEL = 2,
	FOGVOL_LIGHTING_FROXEL_PLUS_DETAIL = 3
} fogvol_lighting_mode_t;

static fogvol_lighting_mode_t R_FogVol_LightingMode (void)
{
	return (fogvol_lighting_mode_t)CLAMP (0, (int)Q_rint (r_fogvol_lighting_mode.value), 3);
}

static qboolean R_FogVol_UseRaymarchLights (fogvol_lighting_mode_t mode)
{
	return (mode == FOGVOL_LIGHTING_RAYMARCH);
}

static qboolean R_FogVol_UseFroxelLights (fogvol_lighting_mode_t mode)
{
	return (mode == FOGVOL_LIGHTING_FROXEL || mode == FOGVOL_LIGHTING_FROXEL_PLUS_DETAIL);
}

static qboolean R_FogVol_UseRaymarchDetail (fogvol_lighting_mode_t mode)
{
	return (mode == FOGVOL_LIGHTING_FROXEL_PLUS_DETAIL);
}

static void R_FogVol_WarnFroxelLightingConfig (void)
{
	static qboolean warned = false;

	if (warned)
		return;
	if (r_fogvol_froxel.value > 0.f && R_FogVol_UseFroxelLights (R_FogVol_LightingMode ()) && r_fogvol_light.value <= 0.f)
	{
		Con_Printf ("Note: r_fogvol_light<=0 disables raymarch light lists, but froxel dlight injection stays enabled.\n");
		warned = true;
	}
}

void R_Froxel_EndFrame (void)
{
	float inv_view[16];
	float camera_delta = 0.f;
	GLuint out_tex;
	GLuint history_tex;
	GLuint tmp_tex;
	int groups_x;
	int groups_y;
	int groups_z;

	if (!r_froxel.valid || !r_froxel.light_tex || !r_froxel.history_tex || !r_froxel.light_ssbo || !glprogs.fogvol_froxel_inject)
		return;
	if (!Mat4_Inverse (r_matview, inv_view))
		return;
	if (r_froxel.prev_valid)
	{
		vec3_t delta;
		VectorSubtract (r_refdef.vieworg, r_froxel.prev_vieworg, delta);
		camera_delta = sqrtf (DotProduct (delta, delta));
	}

	out_tex = r_froxel.light_tex;
	history_tex = r_froxel.history_tex;
	groups_x = (r_froxel.dims[0] + 3) / 4;
	groups_y = (r_froxel.dims[1] + 3) / 4;
	groups_z = (r_froxel.dims[2] + 3) / 4;

	GL_UseProgram (glprogs.fogvol_froxel_inject);
	glBindTexture (GL_TEXTURE_3D, 0);
	GL_BindBufferRange (GL_SHADER_STORAGE_BUFFER, 0, r_froxel.light_ssbo, 0, sizeof (froxel_gpu_light_t) * q_max (1, MAX_FROXEL_GPU_LIGHTS));
	GL_BufferDataFunc (GL_SHADER_STORAGE_BUFFER, sizeof (froxel_gpu_light_t) * q_max (1, MAX_FROXEL_GPU_LIGHTS), r_froxel_gpu_lights, GL_STREAM_DRAW);
	GL_BindImageTextureFunc (0, out_tex, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
	GL_BindNative (GL_TEXTURE1, GL_TEXTURE_3D, history_tex);
	GL_Uniform4fFunc (0,
		(float)r_froxel.dims[0],
		(float)r_froxel.dims[1],
		(float)r_froxel.dims[2],
		(float)CLAMP (0, r_froxel.light_count, MAX_FROXEL_GPU_LIGHTS));
	GL_Uniform4fFunc (1, r_froxel.near_clip, r_froxel.far_clip, r_froxel.tan_half_fov_x, r_froxel.tan_half_fov_y);
	GL_Uniform4fFunc (2, r_froxel.log_far_near, 0.f, 0.f, 0.f);
	GL_UniformMatrix4fvFunc (3, 1, GL_FALSE, inv_view);
	GL_Uniform4fFunc (7,
		CLAMP (0.f, r_fogvol_froxel_temporal_alpha.value, 1.f),
		q_max (1.f, r_fogvol_froxel_temporal_reject_threshold.value),
		camera_delta,
		r_froxel.prev_valid ? 1.f : 0.f);
	GL_DispatchComputeFunc (groups_x, groups_y, groups_z);
	GL_MemoryBarrierFunc (GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

	tmp_tex = r_froxel.light_tex;
	r_froxel.light_tex = r_froxel.history_tex;
	r_froxel.history_tex = tmp_tex;
	VectorCopy (r_refdef.vieworg, r_froxel.prev_vieworg);
	r_froxel.prev_frame = r_framecount;
	r_froxel.prev_valid = true;
}

void R_FogVol_Init (void)
{
	memset (&r_froxel, 0, sizeof (r_froxel));
	R_FogVol_WarnFroxelLightingConfig ();
}

void R_FogVol_Clear (void)
{
	r_fogvolume_count = 0;
	r_num_fog_static_lights = 0;
	R_FogVol_FreeStaticField ();
}

static void R_FogVol_ClearEntities (void)
{
	r_fogvolume_entity_count = 0;
}

static qboolean R_FogVol_IsSupportedShape (int shape)
{
	return shape == FOGVOL_SHAPE_BOX || shape == FOGVOL_SHAPE_SPHERE;
}

static int R_FogVol_NormalizeShape (int shape)
{
	if (shape == FOGVOL_SHAPE_SPHERE)
		return FOGVOL_SHAPE_SPHERE;
	return FOGVOL_SHAPE_BOX;
}

static void R_FogVol_ClampVolume (fog_volume_t *volume)
{
	volume->density = CLAMP (0.f, volume->density, 10.f);
	volume->falloff = CLAMP (0.f, volume->falloff, 256.f);
	volume->noiseAmount = CLAMP (0.f, volume->noiseAmount, 1.f);
	volume->noiseScale = CLAMP (0.001f, volume->noiseScale, 0.5f);
	volume->turbulence = CLAMP (0.f, volume->turbulence, 2.f);
	volume->emissiveStrength = CLAMP (0.f, volume->emissiveStrength, 16.f);
	volume->shape = R_FogVol_NormalizeShape (volume->shape);
	volume->blendMode = CLAMP (-1, volume->blendMode, 1);
	volume->edgeSoftness = CLAMP (0.f, volume->edgeSoftness, 1.f);
}

static void R_FogVol_AddVolume (const fog_volume_t *volume)
{
	fog_volume_t clamped;

	if (r_fogvolume_count >= MAX_FOGVOLUMES)
		return;
	clamped = *volume;
	R_FogVol_ClampVolume (&clamped);
	r_fogvolumes[r_fogvolume_count++] = clamped;
}

static void R_FogVol_AddEntityVolume (const fog_volume_t *volume)
{
	fog_volume_t clamped;

	if (r_fogvolume_entity_count >= MAX_FOGVOLUMES)
		return;
	clamped = *volume;
	R_FogVol_ClampVolume (&clamped);
	r_fogvolume_entities[r_fogvolume_entity_count++] = clamped;
}

static void R_FogVol_ParseColor (const char *value, vec3_t color)
{
	float r = 1.f, g = 1.f, b = 1.f;
	if (value && sscanf (value, "%f %f %f", &r, &g, &b) == 3)
	{
		/* BUG FIX #7a: Threshold was > 2.f which wrongly keeps values in
		 * (1,2] as-is even though linear RGB is defined on [0,1].  Use > 1.f
		 * so that any channel exceeding the linear range triggers the 0-255
		 * normalisation path.
		 * BUG FIX #7b: If sscanf returns < 3 we fall through to the defaults
		 * (r=g=b=1), but the original code mixed partial results from sscanf
		 * with the uninitialised defaults in the caller's stack frame.
		 * The explicit == 3 guard above is correct; no change needed there. */
		if (r > 1.f || g > 1.f || b > 1.f)
		{
			r *= 1.f / 255.f;
			g *= 1.f / 255.f;
			b *= 1.f / 255.f;
		}
	}
	color[0] = r;
	color[1] = g;
	color[2] = b;
}

static qboolean R_FogVol_ParseVector (const char *value, vec3_t out)
{
	return value && sscanf (value, "%f %f %f", &out[0], &out[1], &out[2]) == 3;
}

typedef struct fogvol_entity_parse_state_s
{
	qboolean is_fog_volume;
	char modelname[64];
	vec3_t origin;
	qboolean has_origin;
} fogvol_entity_parse_state_t;

typedef enum fogvol_entity_key_type_e
{
	FOGVOL_ENTITY_KEY_FLOAT,
	FOGVOL_ENTITY_KEY_INT,
	FOGVOL_ENTITY_KEY_VEC3,
	FOGVOL_ENTITY_KEY_COLOR,
	FOGVOL_ENTITY_KEY_SPECIAL
} fogvol_entity_key_type_t;

typedef void (*fogvol_entity_special_setter_fn_t) (fog_volume_t *volume, fogvol_entity_parse_state_t *state, const char *value);

typedef struct fogvol_entity_key_dispatch_s
{
	const char *key;
	fogvol_entity_key_type_t type;
	size_t offset;
	fogvol_entity_special_setter_fn_t special_setter;
} fogvol_entity_key_dispatch_t;

static void R_FogVol_EntitySetFloat (fog_volume_t *volume, size_t offset, const char *value)
{
	float *field = (float *)((char *)volume + offset);
	*field = atof (value);
}

static void R_FogVol_EntitySetInt (fog_volume_t *volume, size_t offset, const char *value)
{
	int *field = (int *)((char *)volume + offset);
	*field = atoi (value);
}

static void R_FogVol_EntitySetVec3 (fog_volume_t *volume, size_t offset, const char *value)
{
	vec3_t *field = (vec3_t *)((char *)volume + offset);
	R_FogVol_ParseVector (value, *field);
}

static void R_FogVol_EntitySetColor (fog_volume_t *volume, size_t offset, const char *value)
{
	vec3_t *field = (vec3_t *)((char *)volume + offset);
	R_FogVol_ParseColor (value, *field);
}

static void R_FogVol_EntitySetClassname (fog_volume_t *volume, fogvol_entity_parse_state_t *state, const char *value)
{
	(void)volume;
	if (!strcmp (value, "func_fog_volume") || !strcmp (value, "trigger_fog_volume"))
		state->is_fog_volume = true;
}

static void R_FogVol_EntitySetModel (fog_volume_t *volume, fogvol_entity_parse_state_t *state, const char *value)
{
	(void)volume;
	q_strlcpy (state->modelname, value, sizeof (state->modelname));
}

static void R_FogVol_EntitySetOrigin (fog_volume_t *volume, fogvol_entity_parse_state_t *state, const char *value)
{
	(void)volume;
	state->has_origin = R_FogVol_ParseVector (value, state->origin);
}

static void R_FogVol_EntitySetShape (fog_volume_t *volume, fogvol_entity_parse_state_t *state, const char *value)
{
	int parsed_shape;
	char *endptr = NULL;
	(void)state;

	if (!q_strcasecmp (value, "sphere"))
		parsed_shape = FOGVOL_SHAPE_SPHERE;
	else if (!q_strcasecmp (value, "box"))
		parsed_shape = FOGVOL_SHAPE_BOX;
	else
		parsed_shape = (int)strtol (value, &endptr, 10);

	if ((endptr && *endptr != '\0') || !R_FogVol_IsSupportedShape (parsed_shape))
	{
		if (developer.value > 0.f)
			Con_DPrintf ("FogVol: unsupported shape '%s' (expected %d=box or %d=sphere), defaulting to box\n",
				value,
				FOGVOL_SHAPE_BOX,
				FOGVOL_SHAPE_SPHERE);
		parsed_shape = FOGVOL_SHAPE_BOX;
	}

	volume->shape = parsed_shape;
}

/* Supported fog volume entity keys:
 *   classname, model, origin
 *   color
 *   density, falloff, maxdist, priority
 *   noise_scale, noise_amount, noise_bias, velocity, mode
 *   shape, radius, center
 *   blendmode, emissive
 *   wind_dir, wind_speed, turbulence
 *   height, height_scale, edge_softness */
static const fogvol_entity_key_dispatch_t fogvol_entity_key_dispatch[] = {
	{"classname", FOGVOL_ENTITY_KEY_SPECIAL, 0, R_FogVol_EntitySetClassname},
	{"model", FOGVOL_ENTITY_KEY_SPECIAL, 0, R_FogVol_EntitySetModel},
	{"origin", FOGVOL_ENTITY_KEY_SPECIAL, 0, R_FogVol_EntitySetOrigin},
	{"color", FOGVOL_ENTITY_KEY_COLOR, offsetof (fog_volume_t, color), NULL},
	{"density", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, density), NULL},
	{"falloff", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, falloff), NULL},
	{"maxdist", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, maxDistance), NULL},
	{"priority", FOGVOL_ENTITY_KEY_INT, offsetof (fog_volume_t, priority), NULL},
	{"noise_scale", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, noiseScale), NULL},
	{"noise_amount", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, noiseAmount), NULL},
	{"noise_bias", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, noiseBias), NULL},
	{"velocity", FOGVOL_ENTITY_KEY_VEC3, offsetof (fog_volume_t, velocity), NULL},
	{"mode", FOGVOL_ENTITY_KEY_INT, offsetof (fog_volume_t, mode), NULL},
	{"shape", FOGVOL_ENTITY_KEY_SPECIAL, 0, R_FogVol_EntitySetShape},
	{"radius", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, sphereRadius), NULL},
	{"center", FOGVOL_ENTITY_KEY_VEC3, offsetof (fog_volume_t, sphereCenter), NULL},
	{"blendmode", FOGVOL_ENTITY_KEY_INT, offsetof (fog_volume_t, blendMode), NULL},
	{"emissive", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, emissiveStrength), NULL},
	{"wind_dir", FOGVOL_ENTITY_KEY_VEC3, offsetof (fog_volume_t, windDir), NULL},
	{"wind_speed", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, windSpeed), NULL},
	{"turbulence", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, turbulence), NULL},
	{"height", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, height), NULL},
	{"height_scale", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, heightScale), NULL},
	{"edge_softness", FOGVOL_ENTITY_KEY_FLOAT, offsetof (fog_volume_t, edgeSoftness), NULL},
};

static const fogvol_entity_key_dispatch_t *R_FogVol_FindEntityKeyDispatch (const char *key)
{
	int i;

	for (i = 0; i < (int)countof (fogvol_entity_key_dispatch); ++i)
	{
		if (!strcmp (fogvol_entity_key_dispatch[i].key, key))
			return &fogvol_entity_key_dispatch[i];
	}

	return NULL;
}

static void R_FogVol_ApplyEntityKey (const fogvol_entity_key_dispatch_t *dispatch, fog_volume_t *volume, fogvol_entity_parse_state_t *state, const char *value)
{
	if (!dispatch)
		return;

	switch (dispatch->type)
	{
	case FOGVOL_ENTITY_KEY_FLOAT:
		R_FogVol_EntitySetFloat (volume, dispatch->offset, value);
		break;
	case FOGVOL_ENTITY_KEY_INT:
		R_FogVol_EntitySetInt (volume, dispatch->offset, value);
		break;
	case FOGVOL_ENTITY_KEY_VEC3:
		R_FogVol_EntitySetVec3 (volume, dispatch->offset, value);
		break;
	case FOGVOL_ENTITY_KEY_COLOR:
		R_FogVol_EntitySetColor (volume, dispatch->offset, value);
		break;
	case FOGVOL_ENTITY_KEY_SPECIAL:
		if (dispatch->special_setter)
			dispatch->special_setter (volume, state, value);
		break;
	default:
		break;
	}
}

static float R_FogVol_PointDistance (const vec3_t point, const fog_volume_t *volume)
{
	/* BUG FIX (C-07): Declare all locals at the top of the function so this
	 * code compiles cleanly under strict C89 (-std=c89 / /Za).  Mixed
	 * declarations after statements are a C99 extension that some Quake-engine
	 * toolchains reject. */
	float dist2 = 0.f;
	int i;

	if (R_FogVol_NormalizeShape (volume->shape) == FOGVOL_SHAPE_SPHERE)
	{
		vec3_t delta;
		float dist;
		VectorSubtract (point, volume->sphereCenter, delta);
		dist = VectorLength (delta) - volume->sphereRadius;
		return q_max (0.f, dist);
	}

	for (i = 0; i < 3; ++i)
	{
		if (point[i] < volume->mins[i])
		{
			float d = volume->mins[i] - point[i];
			dist2 += d * d;
		}
		else if (point[i] > volume->maxs[i])
		{
			float d = point[i] - volume->maxs[i];
			dist2 += d * d;
		}
	}
	return sqrtf (dist2);
}

static void R_FogVol_AddGlobalFog (void)
{
	fog_volume_t volume;
	float base_density;
	float *color = Fog_GetColor ();

	if (r_fogvol_global.value <= 0.f)
		return;
	if (r_fogvol.value <= 0.f)
		return;

	memset (&volume, 0, sizeof (volume));

	VectorSet (volume.mins, -8192.f, -8192.f, -8192.f);
	VectorSet (volume.maxs, 8192.f, 8192.f, 8192.f);

	VectorCopy (color, volume.color);

	base_density = Fog_GetDensity ();
	if (base_density <= 0.f)
		base_density = 1.f;

	volume.density = base_density * q_max (0.f, r_fogvol_global_density_scale.value);
	volume.falloff = q_max (0.f, r_fogvol_global_falloff.value);
	volume.mode = 0;
	volume.noiseScale = q_max (0.f, r_fogvol_global_noise_scale.value);
	volume.noiseAmount = CLAMP (0.f, r_fogvol_global_noise_amount.value, 1.f);
	volume.noiseBias = r_fogvol_global_noise_bias.value;
	volume.velocity[0] = r_fogvol_global_velocity_x.value;
	volume.velocity[1] = r_fogvol_global_velocity_y.value;
	volume.velocity[2] = r_fogvol_global_velocity_z.value;
	volume.maxDistance = 0.f;
	volume.priority = (int)Q_rint (r_fogvol_global_priority.value);
	volume.enabled = 1;
	volume.height = r_fogvol_global_height.value;
	volume.heightScale = r_fogvol_global_height_scale.value;
	volume.edgeSoftness = 0.f;

	R_FogVol_AddVolume (&volume);
}

qboolean R_FogVol_IsEnabledForFrame (void)
{
	if (r_fogvol.value <= 0.f)
		return false;
	if (!glprogs.fogvol)
		return false;
	if (framebufs.composite.color_tex == 0 || framebufs.fogvol.color_tex[0] == 0)
		return false;
	if (framebufs.composite.depth_stencil_tex == 0)
		return false;
	return true;
}

qboolean R_FogVol_HasValidComposite (void)
{
	if (!R_FogVol_IsEnabledForFrame ())
		return false;
	if (!r_fogvol_composite_valid)
		return false;
	if (framebufs.fogvol.composite_tex[r_fogvol_history_index] == 0)
		return false;
	return true;
}

qboolean R_FogVol_ShouldAffectPostFX (void)
{
	/* PostFX should reserve volumetric integration/composite paths whenever
	 * fogvol rendering is available this frame, even if classic global fog
	 * density is currently zero. */
	return R_FogVol_IsEnabledForFrame ();
}

qboolean R_FogVol_CanRenderGlobal (void)
{
	if (!R_FogVol_IsEnabledForFrame ())
		return false;
	if (r_fogvol_global.value <= 0.f)
		return false;

	return true;
}

/* Returns the fogvol composite texture that was rendered this frame.
 * After R_FogVol_Render(), r_fogvol_history_index points to the slot that
 * received the temporal composite output — that is the most recent result.
 * Returns 0 if fogvol did not render this frame (no active volumes).
 * Semantics: returns a non-zero texture only when output is valid this frame. */
GLuint R_FogVol_GetCompositeTex (void)
{
	if (!R_FogVol_HasValidComposite ())
		return 0;

	return framebufs.fogvol.composite_tex[r_fogvol_history_index];
}

void R_FogVol_ParseEntities (void)
{
	const char *data;

	R_FogVol_ClearEntities ();
	r_num_fog_static_lights = 0;
	R_FogVol_FreeStaticField ();

	if (!cl.worldmodel || !cl.worldmodel->entities)
		return;

	data = cl.worldmodel->entities;
	data = COM_Parse (data);
	while (data && com_token[0])
	{
		fog_volume_t volume;
		fogvol_entity_parse_state_t parse_state;

		if (com_token[0] != '{')
			break;

		memset (&volume, 0, sizeof (volume));
		VectorSet (volume.color, 1.f, 1.f, 1.f);
		volume.density = 0.1f;
		volume.falloff = 16.f;
		volume.mode = 0;
		volume.shape = FOGVOL_SHAPE_BOX;
		volume.blendMode = -1;
		volume.emissiveStrength = 0.f;
		volume.noiseScale = 0.05f;
		volume.noiseAmount = 0.5f;
		volume.noiseBias = 0.f;
		volume.turbulence = 0.f;
		VectorSet (volume.velocity, 0.f, 0.f, 0.f);
		volume.windSpeed = 0.f;
		VectorSet (volume.windDir, 0.f, 0.f, 0.f);
		volume.maxDistance = 2048.f;
		volume.priority = 0;
		volume.enabled = 1;
		volume.height = 0.f;
		volume.heightScale = 0.f;
		volume.edgeSoftness = 0.f;
		memset (&parse_state, 0, sizeof (parse_state));

		while (1)
		{
			char key[64], value[1024];
			data = COM_Parse (data);
			if (!data || !com_token[0])
				return;
			if (com_token[0] == '}')
				break;
			q_strlcpy (key, com_token, sizeof (key));
			/* BUG FIX #1: original used strlen(key) which is the length of the
			 * string *before* the shift, so the NUL terminator was correctly
			 * included only by accident when key length > 1.  For a key of
			 * exactly "_" (len=1) strlen returns 1, memmove copies 1 byte
			 * ("" with no NUL) leaving garbage after position 0.
			 * Use strlen(key)+1 to always include the NUL terminator. */
			if (key[0] == '_')
				memmove (key, key + 1, strlen (key) + 1);
			data = COM_ParseEx (data, CPE_ALLOWTRUNC);
			if (!data)
				return;
			q_strlcpy (value, com_token, sizeof (value));
			R_FogVol_ApplyEntityKey (R_FogVol_FindEntityKeyDispatch (key), &volume, &parse_state, value);
		}

		if (parse_state.is_fog_volume && parse_state.modelname[0])
		{
			qmodel_t *model = Mod_ForName (parse_state.modelname, false);
			if (model && model->type == mod_brush)
			{
				vec3_t mins;
				vec3_t maxs;
				VectorCopy (model->mins, mins);
				VectorCopy (model->maxs, maxs);
				if (parse_state.has_origin)
				{
					VectorAdd (mins, parse_state.origin, mins);
					VectorAdd (maxs, parse_state.origin, maxs);
				}
				VectorCopy (mins, volume.mins);
				VectorCopy (maxs, volume.maxs);
				if (R_FogVol_NormalizeShape (volume.shape) == FOGVOL_SHAPE_SPHERE)
				{
					for (int a = 0; a < 3; ++a)
						volume.sphereCenter[a] = 0.5f * (volume.mins[a] + volume.maxs[a]);
					if (volume.sphereRadius <= 0.f)
					{
						float ex = 0.5f * (volume.maxs[0] - volume.mins[0]);
						float ey = 0.5f * (volume.maxs[1] - volume.mins[1]);
						float ez = 0.5f * (volume.maxs[2] - volume.mins[2]);
						volume.sphereRadius = q_max (ex, q_max (ey, ez));
					}
				}
				R_FogVol_AddEntityVolume (&volume);
			}
		}

		data = COM_Parse (data);
	}

	R_FogVol_BuildStaticLightInjection ();
}

void R_FogVol_AddTestVolumes (void)
{
	fog_volume_t volume;
	vec3_t origin, forward, right, up;
	AngleVectors (r_refdef.viewangles, forward, right, up);
	VectorCopy (r_refdef.vieworg, origin);

	/* Single test volume: a fog patch placed 100 units ahead of the camera.
	 * The player is OUTSIDE the volume looking in, so this cleanly tests
	 * the compositing pipeline without camera-inside-volume complications.
	 *
	 * Expected result: a visible blue-grey fog patch floating in the scene.
	 * If the scene outside the volume looks correct and the volume itself
	 * shows fog colour, the pipeline is working.
	 *
	 * density=0.02 → tau≈2.0 at 100 units → transmittance≈0.14 (clearly
	 * visible, opaque-looking fog patch). */
	memset (&volume, 0, sizeof (volume));
	VectorSet (volume.color, 0.6f, 0.75f, 1.0f);
	volume.density    = 0.02f;
	volume.falloff    = 16.f;
	volume.mode       = 0;
	volume.shape      = FOGVOL_SHAPE_BOX;
	volume.blendMode  = -1;
	volume.noiseScale = 0.05f;
	volume.noiseAmount = 0.6f;
	volume.noiseBias  = 0.0f;
	VectorSet (volume.velocity, 0.f, 0.f, 4.f);
	volume.maxDistance = 0.f;
	volume.priority   = 0;
	volume.enabled    = 1;
	volume.edgeSoftness = 0.f;
	/* Place the box 100..260 units ahead, 96 units wide, 80 units tall.
	 * Compute proper axis-aligned bounds from the oriented corners. */
	{
		vec3_t c0, c1;
		VectorMA (origin, 100.f, forward, c0);
		VectorMA (origin, 260.f, forward, c1);
		/* half-width along right axis */
		for (int a = 0; a < 3; ++a)
		{
			float span = fabsf (right[a]) * 96.f;
			volume.mins[a] = q_min (c0[a], c1[a]) - span;
			volume.maxs[a] = q_max (c0[a], c1[a]) + span;
		}
		volume.mins[2] = origin[2] - 40.f;
		volume.maxs[2] = origin[2] + 80.f;
	}
	R_FogVol_AddVolume (&volume);
}

void R_FogVol_BuildList (void)
{
	R_FogVol_Clear ();
	R_FogDlights_Clear ();

	/* BUG FIX (testvolumes): r_fogvol_testvolumes must work independently of
	 * r_fogvol so developers can test the pipeline without setting up entity
	 * fog volumes.  Add test volumes first if requested, then check r_fogvol
	 * for the entity-volume path. */
	if (r_fogvol_testvolumes.value > 0.f)
		R_FogVol_AddTestVolumes ();

	if (r_fogvol.value <= 0.f)
		goto sort_and_done;

	R_FogVol_AddGlobalFog ();

	for (int i = 0; i < r_fogvolume_entity_count; ++i)
	{
		const fog_volume_t *volume = &r_fogvolume_entities[i];

		if (!volume->enabled)
			continue;
		if (volume->maxDistance > 0.f)
		{
			float dist = R_FogVol_PointDistance (r_refdef.vieworg, volume);
			if (dist > volume->maxDistance)
				continue;
		}
		R_FogVol_AddVolume (volume);
	}

sort_and_done:
	if (r_fogvolume_count > 1)
		qsort (r_fogvolumes, r_fogvolume_count, sizeof (fog_volume_t), R_FogVol_ComparePriority);
}

qboolean R_FogVol_ProjectAABBToScreenRect (const fog_volume_t *v, int *x0, int *y0, int *x1, int *y1, qboolean fullres)
{
	vec3_t corners[8];
	vec3_t proj;
	float minx = 1e30f;
	float miny = 1e30f;
	float maxx = -1e30f;
	float maxy = -1e30f;
	int view_x = 0;
	int view_y = 0;
	int view_w = r_refdef.vrect.width / q_max (1, r_refdef.scale);
	int view_h = r_refdef.vrect.height / q_max (1, r_refdef.scale);
	int valid = 0;

	if (fullres || !GL_NeedsSceneEffects ())
	{
		view_x = glx + r_refdef.vrect.x;
		view_y = gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height;
		view_w = r_refdef.vrect.width;
		view_h = r_refdef.vrect.height;
	}

	{
		vec3_t bmins, bmaxs;
		if (v->shape == FOGVOL_SHAPE_SPHERE)
		{
			for (int a = 0; a < 3; ++a)
			{
				bmins[a] = v->sphereCenter[a] - v->sphereRadius;
				bmaxs[a] = v->sphereCenter[a] + v->sphereRadius;
			}
		}
		else
		{
			VectorCopy (v->mins, bmins);
			VectorCopy (v->maxs, bmaxs);
		}

		/* BUG FIX (C-SCISSOR-01): When the camera is inside the fog volume,
		 * projecting the 8 AABB corners can produce a degenerate or zero-area
		 * scissor rect (all corners behind near-plane → behind==8 → return false,
		 * or clipped NDC rect that doesn't cover the screen centre).
		 * This caused the fog to disappear entirely when looking away from the
		 * volume's corners while standing inside it (Image 1 vs Image 2 bug).
		 *
		 * Fix: detect camera-inside early and return the full viewport immediately.
		 * For sphere volumes use a radius check; for AABB volumes use component-wise
		 * comparison against bmins/bmaxs (already computed above). */
		{
			qboolean cam_inside;
			if (v->shape == FOGVOL_SHAPE_SPHERE)
			{
				vec3_t d;
				VectorSubtract (r_refdef.vieworg, v->sphereCenter, d);
				cam_inside = (DotProduct (d, d) < v->sphereRadius * v->sphereRadius);
			}
			else
			{
				cam_inside = (r_refdef.vieworg[0] >= bmins[0] && r_refdef.vieworg[0] <= bmaxs[0] &&
				              r_refdef.vieworg[1] >= bmins[1] && r_refdef.vieworg[1] <= bmaxs[1] &&
				              r_refdef.vieworg[2] >= bmins[2] && r_refdef.vieworg[2] <= bmaxs[2]);
			}
			if (cam_inside)
			{
				*x0 = view_x;
				*y0 = view_y;
				*x1 = view_x + view_w;
				*y1 = view_y + view_h;
				return true;
			}
		}

		corners[0][0] = bmins[0]; corners[0][1] = bmins[1]; corners[0][2] = bmins[2];
		corners[1][0] = bmaxs[0]; corners[1][1] = bmins[1]; corners[1][2] = bmins[2];
		corners[2][0] = bmins[0]; corners[2][1] = bmaxs[1]; corners[2][2] = bmins[2];
		corners[3][0] = bmaxs[0]; corners[3][1] = bmaxs[1]; corners[3][2] = bmins[2];
		corners[4][0] = bmins[0]; corners[4][1] = bmins[1]; corners[4][2] = bmaxs[2];
		corners[5][0] = bmaxs[0]; corners[5][1] = bmins[1]; corners[5][2] = bmaxs[2];
		corners[6][0] = bmins[0]; corners[6][1] = bmaxs[1]; corners[6][2] = bmaxs[2];
		corners[7][0] = bmaxs[0]; corners[7][1] = bmaxs[1]; corners[7][2] = bmaxs[2];
	}

	{
		int behind = 0;
		for (int i = 0; i < 8; ++i)
		{
			ProjectVector (corners[i], r_matviewproj, proj);
			/* BUG FIX #2 / BUG-C-01: Track corners behind the near plane.
			 * If ALL 8 corners are behind the camera the volume is fully
			 * occluded and we return false.  If some are behind and some in
			 * front, the AABB straddles the near plane — conservatively expand
			 * to the full viewport so we never miss geometry near the edges.
			 * The original code broke out of the loop on the first behind-corner,
			 * which prevented detection of the all-behind case. */
			if (proj[2] <= 0.f)
			{
				behind++;
				/* Expand to full viewport and continue counting. */
				minx = -1.f;
				miny = -1.f;
				maxx =  1.f;
				maxy =  1.f;
				valid = 1;
				continue;
			}
			minx = q_min (minx, proj[0]);
			miny = q_min (miny, proj[1]);
			maxx = q_max (maxx, proj[0]);
			maxy = q_max (maxy, proj[1]);
			valid = 1;
		}
		/* All corners behind the near plane — volume is not visible. */
		if (behind == 8)
			return false;
	}

	if (!valid)
		return false;

	{
		float fx0 = (minx * 0.5f + 0.5f) * (float)view_w + (float)view_x;
		float fy0 = (miny * 0.5f + 0.5f) * (float)view_h + (float)view_y;
		float fx1 = (maxx * 0.5f + 0.5f) * (float)view_w + (float)view_x;
		float fy1 = (maxy * 0.5f + 0.5f) * (float)view_h + (float)view_y;

		int ix0 = (int)floorf (fx0);
		int iy0 = (int)floorf (fy0);
		int ix1 = (int)ceilf (fx1);
		int iy1 = (int)ceilf (fy1);

		ix0 = CLAMP (view_x, ix0, view_x + view_w);
		iy0 = CLAMP (view_y, iy0, view_y + view_h);
		ix1 = CLAMP (view_x, ix1, view_x + view_w);
		iy1 = CLAMP (view_y, iy1, view_y + view_h);

		if (ix1 <= ix0 || iy1 <= iy0)
			return false;

		*x0 = ix0;
		*y0 = iy0;
		*x1 = ix1;
		*y1 = iy1;
	}

	return true;
}

void R_FogVol_DrawDebug2D (void)
{
}

static void R_FogVol_SetShaderUniforms (int steps, int mode, qboolean use_halfres,
	int glwidth, int glheight, float depth_scale_x, float depth_scale_y,
	const float *inv_viewproj, float view_x, float view_y, float view_w, float view_h,
	float depth_near, float depth_far, float depth_sky_cutoff,
	qboolean fog_light_enabled, qboolean fog_lightgrid_enabled)
{
	vec3_t shadow_dir;
	vec3_t sun_color;
	float sun_intensity;
	float sun_scatter;
	float noise_detail_strength;
	float height_mist_strength;
	float atmosphere_boost;
	float quality_fx_scale;
	float quality_step_scale;
	int quality_mode;
	int effective_steps;
	int noise_subsample;
	const sun_t *sun = R_GetSun ();
	int shadow_samples;

#if !defined(NDEBUG)
	assert (FOGVOL_U_COUNT == 49);
	assert (glwidth > 0);
	assert (glheight > 0);
	assert (view_w > 0.f);
	assert (view_h > 0.f);
#endif
	quality_mode = CLAMP (0, (int)Q_rint (r_fogvol_quality.value), 2);
	quality_step_scale = 1.f;
	quality_fx_scale = 1.f;
	if (quality_mode == 0)
	{
		quality_step_scale = 0.8f;
		quality_fx_scale = 0.7f;
	}
	else if (quality_mode == 2)
	{
		quality_step_scale = 1.15f;
		quality_fx_scale = 1.15f;
	}
	effective_steps = (int)Q_rint ((float)steps * quality_step_scale);
	effective_steps = CLAMP (8, effective_steps, CLAMP (8, (int)Q_rint (r_fogvol_maxsteps.value), 256));
	GL_Uniform1iFunc (FOGVOL_U_STEPS, effective_steps);
	GL_Uniform1iFunc (FOGVOL_U_NOISE_ENABLED, r_fogvol_noise.value > 0.f ? 1 : 0);
	GL_Uniform1iFunc (FOGVOL_U_DEBUG_MODE, mode);
	GL_Uniform1iFunc (FOGVOL_U_NOISE_MODE, (int)Q_rint (r_fogvol_noisemode.value));
	GL_Uniform1iFunc (FOGVOL_U_PHYS_BLEND, r_fogvol_physblend.value > 0.f ? 1 : 0);
	GL_Uniform1iFunc (FOGVOL_U_JITTER_ENABLED, r_fogvol_jitter.value > 0.f ? 1 : 0);
	GL_Uniform1iFunc (FOGVOL_U_FRAME_INDEX, r_framecount);
	noise_subsample = r_fogvol_noise_subsample.value > 0.f ? 1 : 0;
	if (quality_mode == 0)
		noise_subsample = 1;
	GL_Uniform1iFunc (FOGVOL_U_NOISE_SUBSAMPLE, noise_subsample);
	GL_Uniform1fFunc (FOGVOL_U_NOISE_LOD_SWITCH_DIST, q_max (1.f, r_fogvol_noise_lod_switch_dist.value));
	GL_Uniform1fFunc (FOGVOL_U_DOMAIN_WARP_MAX_DIST, q_max (0.f, r_fogvol_domainwarp_dist.value));
	GL_Uniform1iFunc (FOGVOL_U_CHECKERBOARD, r_fogvol_checkerboard.value > 0.f ? 1 : 0);
	GL_Uniform1iFunc (FOGVOL_U_HALFRES, use_halfres ? 1 : 0);
	GL_Uniform1iFunc (FOGVOL_U_LIGHT_SUBSAMPLE, r_fogvol_light_subsample.value > 0.f ? 1 : 0);
	GL_UniformMatrix4fvFunc (FOGVOL_U_INV_VIEWPROJ, 1, GL_FALSE, inv_viewproj);
	GL_Uniform3fFunc (FOGVOL_U_CAMERA_POS_WS, r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2]);
	GL_Uniform4fFunc (FOGVOL_U_VIEWPORT_PARAMS, (float)glwidth, (float)glheight, 1.f / (float)glwidth, 1.f / (float)glheight);
	GL_Uniform2fFunc (FOGVOL_U_DEPTH_SCALE, depth_scale_x, depth_scale_y);
	GL_Uniform4fFunc (FOGVOL_U_VIEW_PARAMS,
		use_halfres ? 0.f : view_x,
		use_halfres ? 0.f : view_y,
		1.f / view_w, 1.f / view_h);
	GL_Uniform4fFunc (FOGVOL_U_DEPTH_PARAMS, depth_near, depth_far, gl_clipcontrol_able ? 1.f : 0.f, depth_sky_cutoff);
	GL_Uniform2fFunc (FOGVOL_U_DENSITY_PARAMS, q_max (0.f, r_fogvol_density_scale.value), q_max (0.001f, r_fogvol_sigma_max.value));
	GL_Uniform1iFunc (FOGVOL_U_EMISSIVE_ENABLED, r_fogvol_emissive.value > 0.f ? 1 : 0);
	GL_Uniform1iFunc (FOGVOL_U_BLEND_MODE_DEFAULT, CLAMP (0, (int)Q_rint (r_fogvol_blendmode.value), 1));
	GL_Uniform1iFunc (FOGVOL_U_LIGHT_ENABLED, fog_light_enabled ? 1 : 0);
	shadow_samples = CLAMP (1, (int)Q_rint (r_fogvol_shadow_samples.value), 8);
	if (r_fogvol_sun_dir.value > 0.f && R_WorldHasSun ())
	{
		/* Convention: sun->dir points from scene toward the sun (light source).
		 * Fog shadow marching moves from sample point toward the blocker, so use
		 * the same direction without flipping to keep shader/CPU conventions aligned. */
		VectorCopy (sun->dir, shadow_dir);
	}
	else
	{
		VectorScale (vpn, -1.f, shadow_dir);
	}
	if (VectorLength (shadow_dir) < 0.001f)
		VectorSet (shadow_dir, 0.f, 0.f, -1.f);
	else
		VectorNormalize (shadow_dir);
	GL_Uniform1iFunc (FOGVOL_U_SHADOW_ENABLED, r_fogvol_shadow.value > 0.f ? 1 : 0);
	GL_Uniform1iFunc (FOGVOL_U_SHADOW_SAMPLES, shadow_samples);
	GL_Uniform1fFunc (FOGVOL_U_SHADOW_STRENGTH, sun->shadow_strength);
	GL_Uniform1fFunc (FOGVOL_U_SHADOW_JITTER, r_fogvol_shadow_jitter.value > 0.f ? 1.f : 0.f);
	GL_Uniform3fFunc (FOGVOL_U_SHADOW_DIR, shadow_dir[0], shadow_dir[1], shadow_dir[2]);
	GL_Uniform1iFunc (FOGVOL_U_LIGHTGRID_ENABLED, fog_lightgrid_enabled ? 1 : 0);
	sun_scatter = sun->volumetric_intensity * q_max (0.f, r_fogvol_sun_scatter.value);
	if (R_WorldHasSun ())
	{
		VectorCopy (sun->color, sun_color);
		sun_intensity = sun->intensity;
	}
	else
	{
		VectorSet (sun_color, skyflatcolor[0], skyflatcolor[1], skyflatcolor[2]);
		sun_intensity = 1.f;
		if (sun_color[0] <= 0.f && sun_color[1] <= 0.f && sun_color[2] <= 0.f)
			VectorSet (sun_color, 1.f, 1.f, 1.f);
	}
	if (r_fogvol_sun_color.string && r_fogvol_sun_color.string[0])
	{
		vec3_t user_sun_color;
		R_FogVol_ParseColor (r_fogvol_sun_color.string, user_sun_color);
		if (user_sun_color[0] > 0.f || user_sun_color[1] > 0.f || user_sun_color[2] > 0.f)
			VectorCopy (user_sun_color, sun_color);
	}
	GL_Uniform1fFunc (FOGVOL_U_SUN_SCATTER, sun_scatter * q_max (0.f, sun_intensity));
	GL_Uniform3fFunc (FOGVOL_U_SUN_COLOR, q_max (0.f, sun_color[0]), q_max (0.f, sun_color[1]), q_max (0.f, sun_color[2]));
	GL_Uniform1fFunc (FOGVOL_U_SUN_ANISOTROPY, sun->anisotropy);
	GL_Uniform1fFunc (FOGVOL_U_DLIGHT_SCALE, q_max (0.f, r_fogvol_dlightscale.value));
	GL_Uniform1iFunc (FOGVOL_U_FROXEL_ENABLED, (r_froxel.valid && r_froxel.light_tex) ? 1 : 0);
	GL_Uniform4fFunc (FOGVOL_U_FROXEL_PARAMS0, r_froxel.near_clip, r_froxel.far_clip, r_froxel.tan_half_fov_x, r_froxel.tan_half_fov_y);
	GL_Uniform4fFunc (FOGVOL_U_FROXEL_PARAMS1, r_froxel.log_far_near,
		(float)q_max (1, r_froxel.dims[0]), (float)q_max (1, r_froxel.dims[1]), (float)q_max (1, r_froxel.dims[2]));
	GL_Uniform1iFunc (FOGVOL_U_FROXEL_DEBUG, CLAMP (0, (int)Q_rint (r_froxel_debug.value), 4));
	GL_Uniform1iFunc (FOGVOL_U_FROXEL_PARITY_MODE, CLAMP (0, (int)Q_rint (r_fogvol_froxel_parity.value), 1));
	GL_Uniform3fFunc (FOGVOL_U_LIGHT_SOURCE_SCALES,
		q_max (0.f, r_fogvol_froxel_scale.value),
		q_max (0.f, r_fogvol_lightlist_scale.value),
		q_max (0.f, r_fogvol_lightgrid_scale.value));
	GL_Uniform1iFunc (FOGVOL_U_LIGHTING_MODE, CLAMP (0, (int)Q_rint (r_fogvol_lighting_mode.value), 3));
	GL_Uniform1iFunc (FOGVOL_U_GODRAY_COUPLING, r_fogvol_godray_coupling.value > 0.f ? 1 : 0);
	GL_Uniform4fFunc (FOGVOL_U_GODRAY_SHAFTS_PARAMS,
		(float)q_max (1, framebufs.godrays.width),
		(float)q_max (1, framebufs.godrays.height),
		q_max (0.f, r_fogvol_godray_coupling.value),
		q_max (0.f, r_fogvol_froxel_godrays.value));
	GL_Uniform4fFunc (FOGVOL_U_FROXEL_TEMPORAL_PARAMS,
		CLAMP (0.f, r_fogvol_froxel_temporal_alpha.value, 1.f),
		q_max (1.f, r_fogvol_froxel_temporal_reject_threshold.value),
		(r_froxel.prev_valid ? sqrtf (
			(r_refdef.vieworg[0] - r_froxel.prev_vieworg[0]) * (r_refdef.vieworg[0] - r_froxel.prev_vieworg[0]) +
			(r_refdef.vieworg[1] - r_froxel.prev_vieworg[1]) * (r_refdef.vieworg[1] - r_froxel.prev_vieworg[1]) +
			(r_refdef.vieworg[2] - r_froxel.prev_vieworg[2]) * (r_refdef.vieworg[2] - r_froxel.prev_vieworg[2])) : 0.f),
		r_froxel.prev_valid ? 1.f : 0.f);
	GL_Uniform1iFunc (FOGVOL_U_LOCAL_OCCLUSION_MODE, CLAMP (0, (int)Q_rint (r_fogvol_local_occlusion.value), 2));
	GL_Uniform4fFunc (FOGVOL_U_LOCAL_OCCLUSION_PARAMS, 4.f, 0.02f, 1.f, 0.f);
	noise_detail_strength = CLAMP (0.f, r_fogvol_noise_detail_strength.value, 1.f);
	noise_detail_strength *= quality_mode == 1 ? 0.75f : quality_fx_scale;
	height_mist_strength = CLAMP (0.f, r_fogvol_height_mist_strength.value, 1.f);
	height_mist_strength = CLAMP (0.f, height_mist_strength * quality_fx_scale, 1.f);
	atmosphere_boost = CLAMP (0.f, r_fogvol_atmosphere.value * quality_fx_scale, 1.5f);
	GL_Uniform1fFunc (FOGVOL_U_NOISE_DETAIL_STRENGTH, noise_detail_strength);
	GL_Uniform1fFunc (FOGVOL_U_HEIGHT_MIST_STRENGTH, height_mist_strength);
	GL_Uniform1fFunc (FOGVOL_U_ATMOSPHERE_BOOST, atmosphere_boost);
}

void R_FogVol_Render (void)
{
	static const vec3_t fog_lightgrid_corner_lut[8] = {
		{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {1.f, 1.f, 0.f},
		{0.f, 0.f, 1.f}, {1.f, 0.f, 1.f}, {0.f, 1.f, 1.f}, {1.f, 1.f, 1.f}
	};
	static int last_dumpstate = -1;
	int steps;
	GLuint buf;
	GLbyte *ofs;
	fog_volume_gpu_t gpu_volumes[MAX_FOGVOLUMES];
	fog_light_lists_gpu_t fog_lights;
	fog_lightgrid_gpu_t fog_lightgrid[MAX_FOGVOLUMES];
	const int mode = CLAMP (0, (int)Q_rint (r_fogvol_debug.value), 9);
	float inv_viewproj[16];
	GLuint src_tex;
	GLuint dst_tex;
	GLuint dst_fbo;
	GLuint depth_tex;
	GLuint velocity_tex;
	GLuint fog_tex[2];
	GLuint fog_fbo[2];
	GLuint history_tex[2];
	GLuint history_fbo[2];
	qboolean has_drawn = false;
	qboolean use_halfres;
	int fog_width;
	int fog_height;
	float depth_scale_x;
	float depth_scale_y;
	float view_x;
	float view_y;
	float view_w;
	float view_h;
	float depth_near;
	float depth_far;
	float depth_sky_cutoff;
	int fog_src = 0;
	int fog_dst = 0;
	GLuint final_tex = 0;
	GLuint composite_src_tex = 0;
	GLuint composite_src_fbo = 0;
	qboolean use_test_guard;
	qboolean dumpstate_always;
	fogvol_restore_state_t restore_state;
	qboolean fog_light_enabled = false;
	qboolean fog_lightgrid_enabled = false;
	qboolean fog_lightgrid_has_data = false;
	const lightgrid_t *lightgrid = NULL;
	fogvol_visible_volume_t visible_volumes[MAX_FOGVOLUMES];
	fogvol_light_candidate_t frame_candidates[256];
	fogvol_light_stats_t light_stats;
	vec3_t fog_bounds_mins;
	vec3_t fog_bounds_maxs;
	int scissor_x0[MAX_FOGVOLUMES];
	int scissor_y0[MAX_FOGVOLUMES];
	int scissor_x1[MAX_FOGVOLUMES];
	int scissor_y1[MAX_FOGVOLUMES];
	qboolean scissor_valid[MAX_FOGVOLUMES];
	int visible_count = 0;
	int frame_candidate_count = 0;
	qboolean has_fog_bounds = false;
	const qboolean light_stats_enabled = r_fogvol_light_stats.value > 0.f;
	const fogvol_lighting_mode_t lighting_mode = R_FogVol_LightingMode ();
	const qboolean raymarch_lighting_enabled = (r_fogvol_light.value > 0.f)
		&& (R_FogVol_UseRaymarchLights (lighting_mode) || R_FogVol_UseRaymarchDetail (lighting_mode));
	const qboolean froxel_lighting_mode = R_FogVol_UseFroxelLights (lighting_mode);
	const qboolean froxel_injection_enabled = (r_fogvol_froxel.value > 0.f) && froxel_lighting_mode;
	const qboolean want_froxel_sun = froxel_injection_enabled && (r_fogvol_froxel_sun.value > 0.f);
	const qboolean want_froxel_static = froxel_injection_enabled && (r_fogvol_froxel_static.value > 0.f) && (r_num_fog_static_lights > 0);
	const qboolean want_froxel_dlights = froxel_injection_enabled && (r_fogvol_dlightscale.value > 0.f);
	const qboolean run_froxel_injection = froxel_injection_enabled && (want_froxel_dlights || want_froxel_sun || want_froxel_static);

	R_FogVol_WarnFroxelLightingConfig ();
	if (r_fogvol_static_probe_mode.value > 0.f && r_fog_static_field.valid && !r_fog_static_field.build_complete)
		R_FogVol_ContinueStaticFieldBuild (q_max (1, (int)Q_rint (r_fogvol_static_probe_budget_frame.value)));

	/* Per-frame validity: only expose a fogvol composite texture after this
	 * frame actually produced one. */
	r_fogvol_composite_valid = false;

	if (!glprogs.fogvol)
		return;
	if (r_fogvolume_count <= 0)
		return;
	if (framebufs.composite.color_tex == 0 || framebufs.fogvol.color_tex[0] == 0)
		return;
	if (framebufs.composite.depth_stencil_tex == 0)
		return;
	if (!Mat4_Inverse (r_matviewproj, inv_viewproj))
		return;

	dumpstate_always = (r_fogvol_testvolumes_dumpstate.value > 0.f);
	if (last_dumpstate != (int)Q_rint (r_fogvol_testvolumes_dumpstate.value))
	{
		Con_Printf ("FOGVOL_TEST dumpstate %s\n", dumpstate_always ? "enabled" : "disabled");
		last_dumpstate = (int)Q_rint (r_fogvol_testvolumes_dumpstate.value);
	}

	use_test_guard = (r_fogvol_testvolumes.value > 0.f);
	if (use_test_guard)
		R_FogVol_CaptureRestoreState (&restore_state);
	if (R_FogVol_TestDebugEnabled ())
	{
		R_GLStateDump ("before-fogvol");
		R_FogVol_LogPipelineState ("FOGVOL_BEGIN");
	}
	R_FogVol_LogStateSnapshot ("before-pass");
	if (use_test_guard)
		R_FogVol_LogBufferMarker ("pre");

	use_halfres = (r_fogvol_halfres.value > 0.f);
	fog_width = use_halfres ? framebufs.fogvol.width : glwidth;
	fog_height = use_halfres ? framebufs.fogvol.height : glheight;
	view_x = (float)(glx + r_refdef.vrect.x);
	view_y = (float)(gly + glheight - r_refdef.vrect.y - r_refdef.vrect.height);
	view_w = (float)r_refdef.vrect.width;
	view_h = (float)r_refdef.vrect.height;
	if (glwidth <= 0 || glheight <= 0 || fog_width <= 0 || fog_height <= 0 || view_w <= 0.f || view_h <= 0.f)
	{
		Con_DPrintf ("R_FogVol_Render: rejected dimensions gl=%dx%d fog=%dx%d view=%.1fx%.1f\n",
			glwidth, glheight, fog_width, fog_height, view_w, view_h);
		return;
	}
	depth_scale_x = (float)glwidth / (float)fog_width;
	depth_scale_y = (float)glheight / (float)fog_height;
	depth_near = 0.5f;
	depth_far = gl_farclip.value > depth_near ? gl_farclip.value : depth_near + 1.f;
	depth_sky_cutoff = gl_clipcontrol_able ? 0.001f : 0.999f;

	if (use_halfres && r_fogvol_steps_scale_halfres.value > 0.f)
		steps = (int)Q_rint (r_fogvol_steps.value * r_fogvol_steps_scale_halfres.value);
	else
		steps = (int)Q_rint (r_fogvol_steps.value);
	if (r_fogvol_stepsize.value > 0.f)
	{
		float step_size = q_max (1.f, r_fogvol_stepsize.value);
		steps = (int)Q_rint (depth_far / step_size);
	}
	steps = CLAMP (8, steps, q_max (8, (int)Q_rint (r_fogvol_maxsteps.value)));

	if (run_froxel_injection)
	{
		R_Froxel_BeginFrame (depth_near, depth_far);
		if (want_froxel_dlights)
			R_Froxel_InjectDlights ();
		if (want_froxel_sun)
			R_Froxel_InjectSun ();
		if (want_froxel_static)
			R_Froxel_InjectStaticLights ();
		R_Froxel_EndFrame ();
	}
	else
	{
		/* If froxel injection is skipped this frame, drop temporal history so
		 * the next injected frame cannot blend against stale, non-consecutive data. */
		r_froxel.valid = false;
		r_froxel.prev_valid = false;
	}

	if (raymarch_lighting_enabled)
		memset (&fog_lights, 0, sizeof (fog_lights));
	memset (&light_stats, 0, sizeof (light_stats));
	lightgrid = Lightgrid_Get ();
	fog_lightgrid_has_data = (lightgrid && lightgrid->octree && r_lightgrid.value > 0.f);
	fog_lightgrid_enabled = fog_lightgrid_has_data && (r_fogvol_lightgrid.value > 0.f);
	if (r_fogvol_lightgrid_scale.value <= 0.f)
		fog_lightgrid_enabled = false;

	if (raymarch_lighting_enabled)
	{
		const double broadphase_start = light_stats_enabled ? Sys_DoubleTime () : 0.0;
		int max_lights = CLAMP (0, (int)Q_rint (r_fogvol_light_max.value), MAX_FOGLIGHTS);
		int write_offset = 0;

		visible_count = R_FogVol_BuildVisibleVolumeCache (visible_volumes, countof (visible_volumes), fog_bounds_mins, fog_bounds_maxs, &has_fog_bounds);
		if (visible_count > 0 && max_lights > 0)
		{
			frame_candidate_count = R_FogVol_BuildFrameLightBroadphase (visible_volumes, visible_count, fog_bounds_mins, fog_bounds_maxs, has_fog_bounds, frame_candidates, countof (frame_candidates));
			light_stats.broadphase_candidates = frame_candidate_count;
		}
		if (light_stats_enabled)
			light_stats.broadphase_seconds = Sys_DoubleTime () - broadphase_start;

		for (int i = 0; i < r_fogvolume_count; ++i)
		{
			int remaining = (int)countof (fog_lights.lights) - write_offset;
			int count = 0;
			if (remaining > 0 && max_lights > 0 && frame_candidate_count > 0)
			{
				const double narrowphase_start = light_stats_enabled ? Sys_DoubleTime () : 0.0;
				int per_volume_cap = q_min (max_lights, remaining);
				count = R_FogVol_BuildLightListForVolume (&r_fogvolumes[i], frame_candidates, frame_candidate_count, &fog_lights.lights[write_offset], per_volume_cap, light_stats_enabled ? &light_stats : NULL);
				if (light_stats_enabled)
					light_stats.narrowphase_seconds += Sys_DoubleTime () - narrowphase_start;
			}

			fog_lights.volumes[i].offset_count[0] = write_offset;
			fog_lights.volumes[i].offset_count[1] = count;
			write_offset += count;
			fog_light_enabled = fog_light_enabled || (count > 0);
		}

	}

	if (fog_lightgrid_enabled)
	{
		const double lightgrid_start = light_stats_enabled ? Sys_DoubleTime () : 0.0;
		memset (&fog_lightgrid, 0, sizeof (fog_lightgrid));

		for (int i = 0; i < r_fogvolume_count; ++i)
		{
			const fog_volume_t *v = &r_fogvolumes[i];

			for (int c = 0; c < 8; ++c)
			{
				vec3_t probe_pos;
				const lightgrid_probe_t *probe;

				probe_pos[0] = v->mins[0] + (v->maxs[0] - v->mins[0]) * fog_lightgrid_corner_lut[c][0];
				probe_pos[1] = v->mins[1] + (v->maxs[1] - v->mins[1]) * fog_lightgrid_corner_lut[c][1];
				probe_pos[2] = v->mins[2] + (v->maxs[2] - v->mins[2]) * fog_lightgrid_corner_lut[c][2];
				probe = R_GetLightgridSample (probe_pos);
				if (probe)
				{
					fog_lightgrid[i].probe_rgb[c][0] = probe->rgb[0] * probe->ao;
					fog_lightgrid[i].probe_rgb[c][1] = probe->rgb[1] * probe->ao;
					fog_lightgrid[i].probe_rgb[c][2] = probe->rgb[2] * probe->ao;
				}
				if (light_stats_enabled)
					light_stats.lightgrid_probes++;
			}
		}

		if (light_stats_enabled)
			light_stats.lightgrid_seconds = Sys_DoubleTime () - lightgrid_start;
	}

	for (int i = 0; i < r_fogvolume_count; ++i)
	{
		fog_volume_t *v = &r_fogvolumes[i];
		fog_volume_gpu_t *gpu = &gpu_volumes[i];

		gpu->mins[0] = v->mins[0];
		gpu->mins[1] = v->mins[1];
		gpu->mins[2] = v->mins[2];
		gpu->mins[3] = 0.f;

		gpu->maxs[0] = v->maxs[0];
		gpu->maxs[1] = v->maxs[1];
		gpu->maxs[2] = v->maxs[2];
		gpu->maxs[3] = 0.f;

		gpu->sphere[0] = v->sphereCenter[0];
		gpu->sphere[1] = v->sphereCenter[1];
		gpu->sphere[2] = v->sphereCenter[2];
		gpu->sphere[3] = q_max (0.f, v->sphereRadius);

		gpu->color_density[0] = q_max (0.f, v->color[0]);
		gpu->color_density[1] = q_max (0.f, v->color[1]);
		gpu->color_density[2] = q_max (0.f, v->color[2]);
		gpu->color_density[3] = v->density;

		gpu->noise_params[0] = CLAMP (0.005f, v->noiseScale, 0.5f);
		gpu->noise_params[1] = CLAMP (0.f, v->noiseAmount, 1.f);
		gpu->noise_params[2] = v->noiseBias;
		gpu->noise_params[3] = v->maxDistance;

		gpu->velocity_windspeed[0] = v->velocity[0];
		gpu->velocity_windspeed[1] = v->velocity[1];
		gpu->velocity_windspeed[2] = v->velocity[2];
		gpu->velocity_windspeed[3] = v->windSpeed;

		gpu->wind_turbulence[0] = v->windDir[0];
		gpu->wind_turbulence[1] = v->windDir[1];
		gpu->wind_turbulence[2] = v->windDir[2];
		gpu->wind_turbulence[3] = v->turbulence;

		gpu->misc[0] = (float)v->priority;
		gpu->misc[1] = (float)v->enabled;
		gpu->misc[2] = v->falloff;
		gpu->misc[3] = v->height;

		gpu->extra[0] = (float)R_FogVol_NormalizeShape (v->shape);
		gpu->extra[1] = (float)((v->blendMode >= 0) ? v->blendMode : (int)Q_rint (r_fogvol_blendmode.value));
		gpu->extra[2] = v->emissiveStrength;
		gpu->extra[3] = v->heightScale;

		gpu->params2[0] = v->edgeSoftness;
		gpu->params2[1] = 0.f;
		gpu->params2[2] = 0.f;
		gpu->params2[3] = 0.f;
	}

	GL_Upload (GL_UNIFORM_BUFFER, gpu_volumes, sizeof (fog_volume_gpu_t) * r_fogvolume_count, &buf, &ofs);
	GL_BindBufferRange (GL_UNIFORM_BUFFER, 2, buf, (GLintptr)ofs, sizeof (fog_volume_gpu_t) * r_fogvolume_count);

	/* PERF FIX (C-02): glGetError() forces a GPU-CPU pipeline flush and is
	 * expensive in the per-frame hot path.  Guard with NDEBUG so release builds
	 * skip the sync entirely.  The lights UBO upload failure path becomes a
	 * no-op in release, which is acceptable: a failed UBO upload would simply
	 * leave fog lights disabled for one frame rather than crashing. */
#if !defined(NDEBUG)
	/* Clear any stale GL errors before the lights UBO upload so glGetError()
	 * below only catches errors from THIS call, not from earlier unrelated GL
	 * operations — a stale error would silently disable all fog lights. */
	while (glGetError () != GL_NO_ERROR) {}
#endif
	if (raymarch_lighting_enabled)
	{
		GL_Upload (GL_UNIFORM_BUFFER, &fog_lights, sizeof (fog_lights), &buf, &ofs);
		GL_BindBufferRange (GL_UNIFORM_BUFFER, 4, buf, (GLintptr)ofs, sizeof (fog_lights));
#if !defined(NDEBUG)
		if (glGetError () != GL_NO_ERROR)
		{
			fog_light_enabled = false;
			memset (&fog_lights, 0, sizeof (fog_lights));
			GL_Upload (GL_UNIFORM_BUFFER, &fog_lights, sizeof (fog_lights), &buf, &ofs);
			GL_BindBufferRange (GL_UNIFORM_BUFFER, 4, buf, (GLintptr)ofs, sizeof (fog_lights));
		}
#endif
	}

	if (fog_lightgrid_enabled)
	{
		const double lightgrid_upload_start = light_stats_enabled ? Sys_DoubleTime () : 0.0;

		/* Upload all per-volume 8-corner probes once per frame; shader indexes by FogVolumeIndex. */
		GL_Upload (GL_UNIFORM_BUFFER, fog_lightgrid, sizeof (fog_lightgrid_gpu_t) * r_fogvolume_count, &buf, &ofs);
		GL_BindBufferRange (GL_UNIFORM_BUFFER, 5, buf, (GLintptr)ofs, sizeof (fog_lightgrid_gpu_t) * r_fogvolume_count);

		if (light_stats_enabled)
			light_stats.lightgrid_upload_seconds = Sys_DoubleTime () - lightgrid_upload_start;
	}
	if (light_stats_enabled)
	{
		Con_DPrintf ("fogvol_light_stats: broadphase_candidates=%d narrowphase_candidates=%d accepted=%d broadphase_ms=%.3f narrowphase_ms=%.3f lightgrid_probe_ms=%.3f lightgrid_upload_ms=%.3f probes=%d gpu_dlight_sel_frames=%d gpu_dlight_sel_fallbacks=%d gpu_dlight_candidates=%d gpu_dlight_selected=%d\n",
			light_stats.broadphase_candidates,
			light_stats.narrowphase_candidates,
			light_stats.narrowphase_accepted,
			light_stats.broadphase_seconds * 1000.0,
			light_stats.narrowphase_seconds * 1000.0,
			light_stats.lightgrid_seconds * 1000.0,
			light_stats.lightgrid_upload_seconds * 1000.0,
			light_stats.lightgrid_probes,
			r_fogvol_gpu_light_select_state.gpu_frames,
			r_fogvol_gpu_light_select_state.gpu_fallbacks,
			r_fogvol_gpu_light_select_state.last_candidates,
			r_fogvol_gpu_light_select_state.last_selected);
	}

	GL_BeginGroup ("Fog volumes");
	R_FogVol_UseProgram (glprogs.fogvol);
	GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
	GL_BindNative (GL_TEXTURE6, GL_TEXTURE_3D, (r_froxel.valid ? r_froxel.light_tex : 0));
	GL_BindNative (GL_TEXTURE7, GL_TEXTURE_2D, 0);
	GL_SetScissorEnabled (false);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	R_FogVol_SetShaderUniforms (steps, mode, use_halfres,
		glwidth, glheight, depth_scale_x, depth_scale_y,
		inv_viewproj, view_x, view_y, view_w, view_h,
		depth_near, depth_far, depth_sky_cutoff,
		fog_light_enabled, fog_lightgrid_enabled);

	if (use_halfres)
		glViewport (0, 0, fog_width, fog_height);
	else
		glViewport ((int)view_x, (int)view_y, (int)view_w, (int)view_h);
	depth_tex = framebufs.composite.depth_stencil_tex;
	velocity_tex = 0;
	if (framebufs.scene.velocity_tex)
	{
		qboolean msaa = framebufs.scene.samples > 1;
		velocity_tex = msaa ? framebufs.resolved_scene.velocity_tex : framebufs.scene.velocity_tex;
		/* r_fogvol.c has no access to gl_rmain.c's internal framesetup state.
		 * Keep the available velocity texture and let TAA/motion resolve handle
		 * stale-history rejection through their normal confidence path. */
	}
	fog_tex[0] = framebufs.fogvol.color_tex[0];
	fog_tex[1] = framebufs.fogvol.color_tex[1];
	fog_fbo[0] = framebufs.fogvol.fbo[0];
	fog_fbo[1] = framebufs.fogvol.fbo[1];
	history_tex[0] = framebufs.fogvol.history_tex[0];
	history_tex[1] = framebufs.fogvol.history_tex[1];
	history_fbo[0] = framebufs.fogvol.history_fbo[0];
	history_fbo[1] = framebufs.fogvol.history_fbo[1];
	src_tex = framebufs.composite.color_tex;
	final_tex = 0;
	/* BUG FIX #3: fog_src must start at 0 and be maintained across iterations.
	 * The original never initialised fog_src before the loop's first use in
	 * `fog_dst = (i==0) ? 0 : (1 - fog_src)`, and never advanced fog_src after
	 * the first iteration for the i==0 case.  fog_src is now explicitly set to
	 * match fog_dst at the bottom of each iteration via `fog_src = fog_dst`
	 * (which already existed at line 1181) — the missing piece was initialising
	 * it consistently here so the first `(1 - fog_src)` is deterministic. */
	fog_src = 0;

	for (int i = 0; i < r_fogvolume_count; ++i)
	{
		fog_volume_t *v = &r_fogvolumes[i];
		int x0, y0, x1, y1;
		scissor_valid[i] = false;
		if (!v->enabled)
			continue;
		if (!R_FogVol_ProjectAABBToScreenRect (v, &x0, &y0, &x1, &y1, true))
			continue;
		if (use_halfres)
		{
			float scale_x = (float)fog_width / (float)glwidth;
			float scale_y = (float)fog_height / (float)glheight;
			int hx0 = (int)floorf ((float)x0 * scale_x);
			int hy0 = (int)floorf ((float)y0 * scale_y);
			int hx1 = (int)ceilf ((float)x1 * scale_x);
			int hy1 = (int)ceilf ((float)y1 * scale_y);
			x0 = CLAMP (0, hx0, fog_width);
			y0 = CLAMP (0, hy0, fog_height);
			x1 = CLAMP (0, hx1, fog_width);
			y1 = CLAMP (0, hy1, fog_height);
			if (x1 <= x0 || y1 <= y0)
				continue;
		}
		scissor_x0[i] = x0;
		scissor_y0[i] = y0;
		scissor_x1[i] = x1;
		scissor_y1[i] = y1;
		scissor_valid[i] = true;
	}

	for (int i = 0; i < r_fogvolume_count; ++i)
	{
		fog_volume_t *v = &r_fogvolumes[i];
		int x0, y0, x1, y1;
		GLuint src_fbo;

		if (!scissor_valid[i])
			continue;
		x0 = scissor_x0[i];
		y0 = scissor_y0[i];
		x1 = scissor_x1[i];
		y1 = scissor_y1[i];

		if (mode == 1)
		{
			vec3_t color;
			VectorCopy (v->color, color);
			R_DebugDrawWireBox (v->mins, v->maxs, color, true);
		}

		fog_dst = (i == 0) ? 0 : (1 - fog_src);
		dst_tex = fog_tex[fog_dst];
		dst_fbo = fog_fbo[fog_dst];

		if (i == 0)
		{
			src_tex = framebufs.composite.color_tex;
			src_fbo = framebufs.composite.fbo;
			if (use_halfres)
			{
				/* Half-res path needs one downsample copy of the scene color. */
				GL_SetScissorEnabled (false);
				R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, src_fbo);
				R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, framebufs.fogvol.finalcopy_fbo);
				R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "FINAL_COPY read=COLOR_ATTACHMENT0");
				R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "FINAL_COPY draw=COLOR_ATTACHMENT0");
				R_FogVol_LogHazardPass ("FINAL_COPY", src_tex, 0, 0);
				R_FogVol_AssertNoFeedbackHazard ("FINAL_COPY", framebufs.fogvol.finalcopy_tex, src_tex);
				GL_BlitFramebufferFunc (0, 0, glwidth, glheight,
					0, 0, fog_width, fog_height,
					GL_COLOR_BUFFER_BIT, GL_LINEAR);
				/* Update source to the downsampled texture after copy. */
				src_tex = framebufs.fogvol.finalcopy_tex;
				src_fbo = framebufs.fogvol.finalcopy_fbo;
			}
			/* Full-res path can sample composite directly; avoid extra full-screen blit. */
		}
		else
		{
			src_tex = fog_tex[fog_src];
			src_fbo = fog_fbo[fog_src];
		}

		R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, src_fbo);
		R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, dst_fbo);
		R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "ITER read=COLOR_ATTACHMENT0");
		R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "ITER draw=COLOR_ATTACHMENT0");
		R_FogVol_LogHazardPass ("ITER", src_tex, 0, src_tex);
		if (mode == 1)
			Con_DPrintf ("FOGVOL debug ITER idx=%d src_tex=%u depth_tex=%u dst_tex=%u src_fbo=%u dst_fbo=%u scissor=(%d %d %d %d)\n",
				i, src_tex, depth_tex, dst_tex, src_fbo, dst_fbo, x0, y0, x1 - x0, y1 - y0);
		R_FogVol_AssertNoFeedbackHazard ("ITER", dst_tex, src_tex);
		R_FogVol_AssertNoBoundFeedbackHazard ("ITER");
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, src_tex);
		GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, depth_tex);
		GL_SetScissorEnabled (true);
		glScissor (x0, y0, x1 - x0, y1 - y0);
		GL_Uniform1iFunc (FOGVOL_U_VOLUME_INDEX, i);
		glDrawArrays (GL_TRIANGLES, 0, 3);
		GL_SetScissorEnabled (false);

		fog_src = fog_dst;
		final_tex = fog_tex[fog_src];
		has_drawn = true;
	}
	GL_SetScissorEnabled (false);
	/* BUG FIX (C-08): final_fbo is derived from fog_src which is 0 here only
	 * when no iterations ran (has_drawn=false).  In that case the code jumps
	 * to `done` via the has_drawn check below and final_fbo is never used —
	 * but the declaration order made this non-obvious and fragile.  Declare
	 * final_fbo only after confirming has_drawn so the value is always valid
	 * when it reaches code that actually uses it.  This matches final_tex
	 * which was already set correctly inside the loop via fog_src=fog_dst. */
	{
		GLuint final_fbo = framebufs.fogvol.fbo[fog_src];

	if (!has_drawn)
	{
		R_FogVol_BindFramebuffer (GL_FRAMEBUFFER, framebufs.composite.fbo);
		R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "temporal draw=COLOR_ATTACHMENT0");
		R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "temporal read=COLOR_ATTACHMENT0");
		glViewport (glx, gly, glwidth, glheight);
		goto done;
	}

	if (r_fogvol_temporal_alpha.value > 0.f && glprogs.fogvol_temporal && final_tex)
	{
		int history_valid = (r_fogvol_history_width == fog_width && r_fogvol_history_height == fog_height);
		int history_src = r_fogvol_history_index;
		int history_dst = 1 - history_src;
		int composite_src = history_src;
		int composite_dst = 1 - composite_src;
		if (!history_valid)
		{
			r_fogvol_history_width = fog_width;
			r_fogvol_history_height = fog_height;
			/* BUG FIX #4: Reset history_index to 0 on invalidation so we always
			 * start from a known-clean slot after a resolution change, rather
			 * than inheriting whatever index was live from a previous frame
			 * size that no longer matches. */
			r_fogvol_history_index = 0;
			history_src = 0;
			history_dst = 1;
			composite_src = 0;
			composite_dst = 1;
		}

		R_FogVol_UseProgram (glprogs.fogvol_temporal);
		GL_SetState (GLS_BLEND_OPAQUE | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS (0));
		/* history_src and history_dst are always opposite values after the init
		 * block above; keep relying on that invariant. */
		R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, history_fbo[history_src]);
		R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, framebufs.fogvol.composite_fbo[composite_dst]);
		R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "COMPOSITE read=COLOR_ATTACHMENT0");
		R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "COMPOSITE draw=COLOR_ATTACHMENT0");
		glViewport (0, 0, fog_width, fog_height);
		R_FogVol_AssertNoFeedbackHazard ("COMPOSITE", framebufs.fogvol.composite_tex[composite_dst], final_tex);
		R_FogVol_AssertNoFeedbackHazard ("COMPOSITE", framebufs.fogvol.composite_tex[composite_dst], history_tex[history_src]);
		R_FogVol_AssertNoBoundFeedbackHazard ("COMPOSITE");
		GL_BindNative (GL_TEXTURE0, GL_TEXTURE_2D, final_tex);
		GL_BindNative (GL_TEXTURE1, GL_TEXTURE_2D, history_tex[history_src]);
		GL_BindNative (GL_TEXTURE2, GL_TEXTURE_2D, depth_tex);
		GL_BindNative (GL_TEXTURE3, GL_TEXTURE_2D, velocity_tex);
		R_FogVol_LogHazardPass ("COMPOSITE", final_tex, history_tex[history_src], final_tex);
		GL_Uniform1fFunc (0, r_fogvol_temporal_alpha.value);
		GL_Uniform1fFunc (1, r_fogvol_temporal_depth_reject.value);
		GL_Uniform1iFunc (2, mode);
		GL_UniformMatrix4fvFunc (3, 1, GL_FALSE, inv_viewproj);
		GL_Uniform4fFunc (4, (float)glwidth, (float)glheight, 1.f / (float)glwidth, 1.f / (float)glheight);
		GL_Uniform2fFunc (5, depth_scale_x, depth_scale_y);
		GL_Uniform1iFunc (6, history_valid ? 1 : 0);
		/* BUG FIX (halfres distortion): temporal shader also runs at viewport
		 * (0,0,fog_width,fog_height) in halfres, so view_x/view_y must not be
		 * subtracted — pass (0,0) as the origin, same reasoning as FogViewParams
		 * in the raymarcher pass above. */
		GL_Uniform4fFunc (7,
			use_halfres ? 0.f : view_x,
			use_halfres ? 0.f : view_y,
			1.f / view_w, 1.f / view_h);
		GL_Uniform4fFunc (8,
			CLAMP (0.f, r_fogvol_temporal_confidence_min_alpha.value, 1.f),
			q_max (0.f, r_fogvol_temporal_disocclusion_bias.value),
			q_max (0.1f, r_fogvol_temporal_clamp_strength.value),
			0.f);
		GL_Uniform1iFunc (9, velocity_tex != 0 ? 1 : 0);
		GL_Uniform1iFunc (10, r_fogvol_checkerboard.value > 0.f ? 1 : 0);
		if (mode == 1)
			Con_DPrintf ("FOGVOL debug COMPOSITE final_tex=%u history_tex=%u depth_tex=%u velocity_tex=%u dst_tex=%u conf[min=%.3f disocc=%.3f clamp=%.3f]\n",
				final_tex, history_tex[history_src], depth_tex, velocity_tex, framebufs.fogvol.composite_tex[composite_dst],
				r_fogvol_temporal_confidence_min_alpha.value, r_fogvol_temporal_disocclusion_bias.value, r_fogvol_temporal_clamp_strength.value);
		glDrawArrays (GL_TRIANGLES, 0, 3);

		R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, framebufs.fogvol.composite_fbo[composite_dst]);
		R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, history_fbo[history_dst]);
		R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY read=COLOR_ATTACHMENT0");
		R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY draw=COLOR_ATTACHMENT0");
		R_FogVol_AssertNoFeedbackHazard ("HISTORY", history_tex[history_dst], framebufs.fogvol.composite_tex[composite_dst]);
		R_FogVol_AssertNoBoundFeedbackHazard ("HISTORY");
		R_FogVol_LogHazardPass ("HISTORY", framebufs.fogvol.composite_tex[composite_dst], 0, framebufs.fogvol.composite_tex[composite_dst]);
		GL_BlitFramebufferFunc (0, 0, fog_width, fog_height,
			0, 0, fog_width, fog_height,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);

		composite_src_tex = framebufs.fogvol.composite_tex[composite_dst];
		composite_src_fbo = framebufs.fogvol.composite_fbo[composite_dst];
		final_tex = history_tex[history_dst];
		final_fbo = history_fbo[history_dst];
		r_fogvol_history_index = history_dst;
	}

	if (has_drawn)
	{
		R_FogVol_BindFramebuffer (GL_FRAMEBUFFER, framebufs.composite.fbo);
		R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY draw=COLOR_ATTACHMENT0");
		glViewport (glx, gly, glwidth, glheight);
		if (use_halfres)
		{
			if (composite_src_tex)
			{
				final_tex = composite_src_tex;
				final_fbo = composite_src_fbo;
			}
			R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, final_fbo);
			R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, framebufs.composite.fbo);
			R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY read=COLOR_ATTACHMENT0");
			R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY draw=COLOR_ATTACHMENT0");
			R_FogVol_LogHazardPass ("HISTORY", final_tex, 0, final_tex);
			R_FogVol_AssertNoFeedbackHazard ("HISTORY", framebufs.composite.color_tex, final_tex);
			glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
			GL_BlitFramebufferFunc (0, 0, fog_width, fog_height,
				0, 0, glwidth, glheight,
				GL_COLOR_BUFFER_BIT, GL_LINEAR);
			glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		}
		else
		{
			R_FogVol_BindFramebuffer (GL_READ_FRAMEBUFFER, final_fbo);
			R_FogVol_BindFramebuffer (GL_DRAW_FRAMEBUFFER, framebufs.composite.fbo);
			R_FogVol_SetReadBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY read=COLOR_ATTACHMENT0");
			R_FogVol_SetDrawBufferDebug (GL_COLOR_ATTACHMENT0, "HISTORY draw=COLOR_ATTACHMENT0");
			R_FogVol_LogHazardPass ("HISTORY", final_tex, 0, final_tex);
			R_FogVol_AssertNoFeedbackHazard ("HISTORY", framebufs.composite.color_tex, final_tex);
			/* BUG FIX (white screen): Protect composite FBO alpha channel.
			 * BUG FIX (grey washout): Only blit the viewrect region, not the
			 * full glwidth×glheight.  The fog FBO is cleared to (0,0,0,0)
			 * outside the viewrect; blitting that area would overwrite HUD
			 * and other non-fog pixels with black.  Blit only the view area
			 * that was actually rendered by the fog shader. */
			glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
			GL_BlitFramebufferFunc (
				(int)view_x, (int)view_y, (int)(view_x + view_w), (int)(view_y + view_h),
				(int)view_x, (int)view_y, (int)(view_x + view_w), (int)(view_y + view_h),
				GL_COLOR_BUFFER_BIT, GL_NEAREST);
			glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		}

		r_fogvol_composite_valid = true;
		R_FogVol_DebugCheckCompositeAlphaDynamics (fog_width, fog_height);
	}

	if (mode == 1)
		R_DebugFlushGeometry ();

	} /* end final_fbo block (BUG FIX C-08) */

done:
	R_FogVol_LogStateSnapshot ("after-pass");
	if (use_halfres)
	{
		GLint viewport[4] = {0};
		GLint scissor_box[4] = {0};
		GLboolean scissor_enabled = GL_FALSE;
		const GLint expected_vp[4] = { glx, gly, glwidth, glheight };

		glGetIntegerv (GL_VIEWPORT, viewport);
		scissor_enabled = glIsEnabled (GL_SCISSOR_TEST);
		glGetIntegerv (GL_SCISSOR_BOX, scissor_box);

		if (viewport[0] != expected_vp[0] || viewport[1] != expected_vp[1] || viewport[2] != expected_vp[2] || viewport[3] != expected_vp[3]
			|| (scissor_enabled && (scissor_box[0] != expected_vp[0] || scissor_box[1] != expected_vp[1] || scissor_box[2] != expected_vp[2] || scissor_box[3] != expected_vp[3])))
		{
			Con_DPrintf ("FOGVOL_STATE defensive-restore viewport=(%d %d %d %d) scissor=%d box=(%d %d %d %d)\n",
				viewport[0], viewport[1], viewport[2], viewport[3],
				scissor_enabled,
				scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
			glViewport (expected_vp[0], expected_vp[1], expected_vp[2], expected_vp[3]);
			GL_SetScissorEnabled (false);
			glScissor (expected_vp[0], expected_vp[1], expected_vp[2], expected_vp[3]);
		}
	}
	if (R_FogVol_TestDebugEnabled ())
	{
		R_FogVol_LogPipelineState ("FOGVOL_END");
		R_GLStateDump ("after-fogvol");
	}
	if (use_test_guard)
	{
		R_FogVol_Restore3DRenderState (&restore_state);
		if (dumpstate_always)
		{
			fogvol_test_state_t restored_state;
			R_FogVol_TestState_Capture (&restored_state);
			R_FogVol_TestState_Log ("restored-baseline", &restored_state);
		}
	}

	GL_EndGroup ();
}


void R_FogVol_LogEndFrameState (void)
{
	R_FogVol_LogPipelineState ("END_FRAME");
}


static qboolean R_FogVol_GridBoxOverlap (const vec3_t cell_mins, const vec3_t cell_maxs, const fog_volume_t *vol)
{
	return R_FogVol_BoundsOverlap (cell_mins, cell_maxs, vol->mins, vol->maxs);
}

static qboolean R_FogVol_GridSphereOverlap (const vec3_t cell_mins, const vec3_t cell_maxs, const fog_volume_t *vol)
{
	float dist2 = 0.f;
	for (int a = 0; a < 3; ++a)
	{
		float v = vol->sphereCenter[a];
		if (v < cell_mins[a])
		{
			float d = cell_mins[a] - v;
			dist2 += d * d;
		}
		else if (v > cell_maxs[a])
		{
			float d = v - cell_maxs[a];
			dist2 += d * d;
		}
	}
	return dist2 <= vol->sphereRadius * vol->sphereRadius;
}

static qboolean R_FogVol_GridVolumeOverlapsCell (const fog_volume_t *vol, const vec3_t cell_mins, const vec3_t cell_maxs)
{
	if (R_FogVol_NormalizeShape (vol->shape) == FOGVOL_SHAPE_SPHERE)
		return R_FogVol_GridSphereOverlap (cell_mins, cell_maxs, vol);
	return R_FogVol_GridBoxOverlap (cell_mins, cell_maxs, vol);
}

void R_FogVol_InjectIntoGrid (froxel_grid_t *grid, const fog_volume_t *vols, int num)
{
	vec3_t cell_size;
	const qboolean inject_debug = (r_fogvol_inject_debug.value > 0.f) || (r_fogvol_debug.value >= 7.f);

	if (!grid || !vols || num <= 0)
		return;
	if (grid->dims[0] <= 0 || grid->dims[1] <= 0 || grid->dims[2] <= 0)
		return;
	if (!grid->density || !grid->color || !grid->emissive)
		return;

	for (int a = 0; a < 3; ++a)
		cell_size[a] = (grid->maxs[a] - grid->mins[a]) / (float)grid->dims[a];

	for (int i = 0; i < num; ++i)
	{
		const fog_volume_t *vol = &vols[i];
		vec3_t bmins, bmaxs;
		int min_i[3], max_i[3];
		const float vol_density = q_max (0.f, vol->density) * q_max (0.f, r_fogvol_density_scale.value);
		const int blend_mode = (vol->blendMode >= 0) ? vol->blendMode : (int)Q_rint (r_fogvol_blendmode.value);

		if (!vol->enabled || vol_density <= 0.f)
			continue;

		R_FogVol_GetVolumeBounds (vol, bmins, bmaxs);
		if (!R_FogVol_BoundsOverlap (bmins, bmaxs, grid->mins, grid->maxs))
			continue;

		for (int a = 0; a < 3; ++a)
		{
			float cminf = floorf ((bmins[a] - grid->mins[a]) / q_max (cell_size[a], 1e-6f));
			float cmaxf = floorf ((bmaxs[a] - grid->mins[a]) / q_max (cell_size[a], 1e-6f));
			min_i[a] = CLAMP (0, (int)cminf, grid->dims[a] - 1);
			max_i[a] = CLAMP (0, (int)cmaxf, grid->dims[a] - 1);
		}

		for (int z = min_i[2]; z <= max_i[2]; ++z)
		for (int y = min_i[1]; y <= max_i[1]; ++y)
		for (int x = min_i[0]; x <= max_i[0]; ++x)
		{
			vec3_t cell_mins, cell_maxs;
			const int idx = x + grid->dims[0] * (y + grid->dims[1] * z);
			float *cell_color = &grid->color[idx * 3];
			float *cell_emissive = &grid->emissive[idx * 3];

			cell_mins[0] = grid->mins[0] + (float)x * cell_size[0];
			cell_mins[1] = grid->mins[1] + (float)y * cell_size[1];
			cell_mins[2] = grid->mins[2] + (float)z * cell_size[2];
			cell_maxs[0] = cell_mins[0] + cell_size[0];
			cell_maxs[1] = cell_mins[1] + cell_size[1];
			cell_maxs[2] = cell_mins[2] + cell_size[2];

			if (!R_FogVol_GridVolumeOverlapsCell (vol, cell_mins, cell_maxs))
				continue;

			if (blend_mode == 1)
			{
				grid->density[idx] = q_max (grid->density[idx], vol_density);
				for (int c = 0; c < 3; ++c)
				{
					cell_color[c] = q_max (cell_color[c], vol->color[c]);
					if (r_fogvol_emissive.value > 0.f && vol->emissiveStrength > 0.f)
						cell_emissive[c] = q_max (cell_emissive[c], vol->color[c] * vol->emissiveStrength);
				}
			}
			else
			{
				const float old_density = grid->density[idx];
				const float new_density = old_density + vol_density;
				const float w = (new_density > 1e-6f) ? (vol_density / new_density) : 1.f;
				grid->density[idx] = new_density;
				for (int c = 0; c < 3; ++c)
				{
					cell_color[c] = cell_color[c] * (1.f - w) + vol->color[c] * w;
					if (r_fogvol_emissive.value > 0.f && vol->emissiveStrength > 0.f)
						cell_emissive[c] += vol->color[c] * vol->emissiveStrength * vol_density;
				}
			}
		}

		if (inject_debug)
		{
			Con_DPrintf ("FOGVOL_GRID inject i=%d priority=%d shape=%d blend=%d bounds=[%d..%d,%d..%d,%d..%d]\n",
				i, vol->priority, vol->shape, blend_mode,
				min_i[0], max_i[0], min_i[1], max_i[1], min_i[2], max_i[2]);
		}
	}
}
