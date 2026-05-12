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
#include "r_postfx.h"
#include "r_framegraph.h"
#include "screen.h"

extern cvar_t r_motionblur;
extern cvar_t r_color_saturation;
extern cvar_t r_debug_colorspace;
extern cvar_t r_lighting_debug_view;
extern cvar_t r_tonemap;
extern cvar_t r_bloom;
extern cvar_t r_color_contrast;
extern cvar_t r_color_midtone;
extern cvar_t r_film35_enable;
extern cvar_t r_srgb_framebuffer;
extern cvar_t r_ssao;
extern cvar_t r_godrays;
extern cvar_t r_ref_enable_postfx;
extern qboolean water_warp;

float GL_ConsoleVisibility (void)
{
	if (scr_con_current <= 0.f)
		return 0.f;

	if (glheight <= 0)
		return 1.f;

	return CLAMP (0.f, scr_con_current / glheight, 1.f);
}

qboolean GL_ShouldApplyMotionBlur (void)
{
	if (r_motionblur.value <= 0.f)
		return false;

	return GL_ConsoleVisibility () <= 0.f;
}

qboolean R_GodraysMediumEnabled (void)
{
	return true;
}

static qboolean GL_NeedsPostprocess_Internal (void)
{
	float saturation;
	qboolean godrays_medium;

	saturation = CLAMP (0.9f, r_color_saturation.value, 1.2f);
	if (softemu || R_GetEffectiveAlphaMode () == ALPHAMODE_OIT || R_PostFX_DoFEnabledEffective ())
		return true;
	if (r_debug_colorspace.value > 0.f)
		return true;
	if (r_lighting_debug_view.value > 0.f)
		return true;
	if (r_tonemap.value > 0.f || r_bloom.value > 0.f || r_color_contrast.value != 1.f || saturation != 1.f || r_color_midtone.value != 1.f || GL_ShouldApplyMotionBlur () || r_film35_enable.value > 0.f)
		return true;
	if (r_srgb_framebuffer.value <= 0.f)
		return true;
	if (r_ssao.value > 0.f)
		return true;
	godrays_medium = R_GodraysMediumEnabled ();
	if (r_godrays.value > 0.f && godrays_medium)
		return true;
	return false;
}

qboolean GL_NeedsSceneEffects (void)
{
	if (framebufs.scene.samples > 1 || water_warp || R_GetSceneRenderScale () != 1 || R_GetSceneResolutionRatio () < 0.999f)
		return true;

	/* Bloom enabled: keep scene-effects path active for a full-frame bloom extract/composite pass. */
	if ((r_ref_enable_postfx.value != 0.f && r_bloom.value > 0.f) || GL_PostFXBloomBoostActive ())
		return true;

	if (GL_ShouldApplyMotionBlur ())
		return true;

	if (r_ref_enable_postfx.value != 0.f && R_PostFX_DoFEnabledEffective ())
		return true;

	return false;
}

qboolean GL_NeedsPostprocess (void)
{
	if (r_ref_enable_postfx.value == 0.f)
		return false;
	/* Scene-effects path renders into HDR scene targets; it must run postprocess
	 * for correct tone-map/output transfer even when optional effects are off. */
	if (GL_NeedsSceneEffects ())
		return true;
	return GL_NeedsPostprocess_Internal ();
}

void R_GetFramePlanDecisions (qboolean *out_needs_scene_effects, qboolean *out_needs_postprocess)
{
	RenderFramePlan plan;
	qboolean needs_scene_effects;
	qboolean needs_postprocess;

	if (R_FrameGraph_GetRenderFramePlan (&plan))
	{
		needs_scene_effects = plan.needs_scene_effects;
		needs_postprocess = plan.needs_postprocess;
		/* Defensive fallback: keep runtime cvar/feature decisions authoritative
		 * if frame-plan flags lag behind backend state after renderer refactors. */
		needs_scene_effects = needs_scene_effects || GL_NeedsSceneEffects ();
		needs_postprocess = needs_postprocess || GL_NeedsPostprocess ();
	}
	else
	{
		needs_scene_effects = GL_NeedsSceneEffects ();
		needs_postprocess = GL_NeedsPostprocess ();
	}

	if (out_needs_scene_effects)
		*out_needs_scene_effects = needs_scene_effects;
	if (out_needs_postprocess)
		*out_needs_postprocess = needs_postprocess;
}
