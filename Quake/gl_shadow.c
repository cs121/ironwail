#include "quakedef.h"
#include "gl_shadow.h"
#if R_SHADOWMAPS
#include "r_shadows.h"
#endif
#include "gl_texmgr.h"

#define SHADOW_POOL_SIZE 16
/* Optional Oberlimit für PCF und Softness/Bias, um Ausreißer zu verhindern */
#define SHADOW_PCF_SAMPLES_MAX 64
#define SHADOW_SOFTNESS_MAX 0.25f
#define SHADOW_BIAS_MAX 0.05f
#define SHADOW_NORMALBIAS_MAX 1.0f

cvar_t gl_shadows = { "gl_shadows", "1", CVAR_ARCHIVE };
cvar_t gl_shadow_mapsize = { "gl_shadow_mapsize", "512", CVAR_ARCHIVE };
cvar_t gl_shadow_softness = { "gl_shadow_softness", "0.02", CVAR_ARCHIVE };
cvar_t gl_shadow_pcf_samples = { "gl_shadow_pcf_samples", "8", CVAR_ARCHIVE };
cvar_t gl_shadow_maxlights = { "gl_shadow_maxlights", "4", CVAR_ARCHIVE };
cvar_t gl_shadow_bias = { "gl_shadow_bias", "0.002", CVAR_ARCHIVE };
cvar_t gl_shadow_normalbias = { "gl_shadow_normalbias", "0.02", CVAR_ARCHIVE };
cvar_t gl_shadow_update_static_interval = { "gl_shadow_update_static_interval", "2", CVAR_ARCHIVE };
cvar_t gl_shadow_debug = { "gl_shadow_debug", "0", CVAR_NONE };
cvar_t r_showshadows = { "r_showshadows", "0", CVAR_NONE };

typedef struct shadow_light_uniform_s {
	vec3_t pos;
	float radius;
	vec3_t color;
	float intensity;
	GLuint cube_tex;
} shadow_light_uniform_t;

static struct shadow_manager_s {
	qboolean initialized;
	int map_size;
	int max_lights;
	dlight_shadow_t pool[SHADOW_POOL_SIZE];
	int pool_size;
	GLuint fallback_cube_tex;
	shadow_light_uniform_t active_lights[MAX_SHADOW_LIGHTS];
	int active_light_count;
	/* Hardware-Budget */
	GLint max_combined_texture_units;
	GLint max_fragment_texture_units;
} shadow_manager;

typedef struct shadow_program_uniforms_s {
	GLuint program;
	GLint active_lights_loc;
	GLint show_shadows_loc;
	GLint light_pos_loc[MAX_SHADOW_LIGHTS];
	GLint light_radius_loc[MAX_SHADOW_LIGHTS];
	GLint light_color_loc[MAX_SHADOW_LIGHTS];
	GLint light_intensity_loc[MAX_SHADOW_LIGHTS];
	GLint light_bias_loc[MAX_SHADOW_LIGHTS];
	GLint light_normal_bias_loc[MAX_SHADOW_LIGHTS];
	GLint light_softness_loc[MAX_SHADOW_LIGHTS];
	GLint light_pcf_samples_loc[MAX_SHADOW_LIGHTS];
	GLint light_shadow_cube_loc[MAX_SHADOW_LIGHTS];
	qboolean sampler_units_initialized;
} shadow_program_uniforms_t;

#define SHADOW_PROGRAM_CACHE_MAX 32
#define SHADOW_TEXTURE_UNIT_BASE 8

static shadow_program_uniforms_t shadow_uniform_cache[SHADOW_PROGRAM_CACHE_MAX];
static int shadow_uniform_cache_count;

/* ---------- Debug helpers ---------- */
static void Shadow_CheckGLErrors (const char* where)
{
	if (!gl_shadow_debug.value)
		return;
	for (;;)
	{
		GLenum err = glGetError ();
		if (err == GL_NO_ERROR)
			break;
		Con_Printf ("GL error 0x%04x at %s\n", (unsigned)err, where ? where : "(unknown)");
	}
}

static void Shadow_QueryTextureUnitBudget (void)
{
	/* Einmalig zur Laufzeit ermitteln; konservative Defaults bei Fehlern */
	shadow_manager.max_combined_texture_units = 16;
	shadow_manager.max_fragment_texture_units = 16;
	glGetIntegerv (GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &shadow_manager.max_combined_texture_units);
	glGetIntegerv (GL_MAX_TEXTURE_IMAGE_UNITS, &shadow_manager.max_fragment_texture_units);
	if (shadow_manager.max_combined_texture_units <= 0) shadow_manager.max_combined_texture_units = 16;
	if (shadow_manager.max_fragment_texture_units <= 0) shadow_manager.max_fragment_texture_units = 16;
}

static int Shadow_ClampLightsToTextureBudget (int requested)
{
	/* Stelle sicher, dass wir nicht über die verfügbaren Texture Units hinaus binden */
	const int base = SHADOW_TEXTURE_UNIT_BASE;
	int max_by_combined = shadow_manager.max_combined_texture_units - base;
	int max_by_fragment = shadow_manager.max_fragment_texture_units - base;
	int budget = max_by_combined < max_by_fragment ? max_by_combined : max_by_fragment;
	if (budget < 0) budget = 0;
	if (requested > budget) requested = budget;
	if (requested > MAX_SHADOW_LIGHTS) requested = MAX_SHADOW_LIGHTS;
	if (requested < 0) requested = 0;
	return requested;
}

static qboolean Shadow_CreateFallbackCube (void)
{
	GLuint tex = 0;
	static const GLfloat white = 1.0f;
	int face;

	glGenTextures (1, &tex);
	if (!tex)
		return false;

	GL_BindNative (GL_TEXTURE0 + SHADOW_TEXTURE_UNIT_BASE, GL_TEXTURE_CUBE_MAP, tex);
	for (face = 0; face < MAX_SHADOW_CUBE_FACES; ++face)
	{
		glTexImage2D (GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_R16F, 1, 1, 0,
			GL_RED, GL_FLOAT, &white);
	}

	glTexParameteri (GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

	if (GL_ObjectLabelFunc)
		GL_ObjectLabelFunc (GL_TEXTURE, tex, -1, "shadow fallback cube");

	GL_BindNative (GL_TEXTURE0 + SHADOW_TEXTURE_UNIT_BASE, GL_TEXTURE_CUBE_MAP, 0);
	Shadow_CheckGLErrors ("Shadow_CreateFallbackCube");

	shadow_manager.fallback_cube_tex = tex;
	return true;
}

static void Shadow_DestroyCube (shadow_cube_t* cube)
{
	if (!cube)
		return;

	if (cube->color_cube_tex)
	{
		glDeleteTextures (1, &cube->color_cube_tex);
		cube->color_cube_tex = 0;
	}

	if (cube->depth_rb)
	{
		if (GL_DeleteRenderbuffersFunc)
			GL_DeleteRenderbuffersFunc (1, &cube->depth_rb);
		cube->depth_rb = 0;
	}

	if (cube->fbo)
	{
		if (GL_DeleteFramebuffersFunc)
			GL_DeleteFramebuffersFunc (1, &cube->fbo);
		cube->fbo = 0;
	}

	cube->size = 0;
	cube->dirty = true;
	cube->last_update_frame = -9999;
}

static void Shadow_ResetPool (void)
{
	int i;
	for (i = 0; i < shadow_manager.pool_size; ++i)
	{
		dlight_shadow_t* slot = &shadow_manager.pool[i];
		Shadow_DestroyCube (&slot->cube);
		slot->light_id = -1;
		slot->is_static = false;
	}
	shadow_manager.pool_size = 0;
	shadow_manager.active_light_count = 0;
}

static void Shadow_ClearUniformCache (void)
{
	shadow_uniform_cache_count = 0;
	memset (shadow_uniform_cache, 0, sizeof (shadow_uniform_cache));
}

static shadow_program_uniforms_t* Shadow_GetProgramUniforms (GLuint program)
{
	int i;
	shadow_program_uniforms_t* entry = NULL;

	for (i = 0; i < shadow_uniform_cache_count; ++i)
	{
		if (shadow_uniform_cache[i].program == program)
			return &shadow_uniform_cache[i];
	}

	if (shadow_uniform_cache_count >= SHADOW_PROGRAM_CACHE_MAX)
		return NULL;

	entry = &shadow_uniform_cache[shadow_uniform_cache_count++];
	memset (entry, 0, sizeof (*entry));
	entry->program = program;
	entry->active_lights_loc = -1;
	entry->show_shadows_loc = -1;
	for (i = 0; i < MAX_SHADOW_LIGHTS; ++i)
	{
		entry->light_pos_loc[i] = -1;
		entry->light_radius_loc[i] = -1;
		entry->light_color_loc[i] = -1;
		entry->light_intensity_loc[i] = -1;
		entry->light_bias_loc[i] = -1;
		entry->light_normal_bias_loc[i] = -1;
		entry->light_softness_loc[i] = -1;
		entry->light_pcf_samples_loc[i] = -1;
		entry->light_shadow_cube_loc[i] = -1;
	}

	entry->active_lights_loc = GL_GetUniformLocationFunc (program, "uActiveLights");
	entry->show_shadows_loc = GL_GetUniformLocationFunc (program, "uShowShadows");

	for (i = 0; i < MAX_SHADOW_LIGHTS; ++i)
	{
		char name[64];

		q_snprintf (name, sizeof (name), "uLights[%d].pos", i);
		entry->light_pos_loc[i] = GL_GetUniformLocationFunc (program, name);

		q_snprintf (name, sizeof (name), "uLights[%d].radius", i);
		entry->light_radius_loc[i] = GL_GetUniformLocationFunc (program, name);

		q_snprintf (name, sizeof (name), "uLights[%d].color", i);
		entry->light_color_loc[i] = GL_GetUniformLocationFunc (program, name);

		q_snprintf (name, sizeof (name), "uLights[%d].intensity", i);
		entry->light_intensity_loc[i] = GL_GetUniformLocationFunc (program, name);

		q_snprintf (name, sizeof (name), "uLights[%d].bias", i);
		entry->light_bias_loc[i] = GL_GetUniformLocationFunc (program, name);

		q_snprintf (name, sizeof (name), "uLights[%d].normalBias", i);
		entry->light_normal_bias_loc[i] = GL_GetUniformLocationFunc (program, name);

		q_snprintf (name, sizeof (name), "uLights[%d].softness", i);
		entry->light_softness_loc[i] = GL_GetUniformLocationFunc (program, name);

		q_snprintf (name, sizeof (name), "uLights[%d].pcfSamples", i);
		entry->light_pcf_samples_loc[i] = GL_GetUniformLocationFunc (program, name);

		q_snprintf (name, sizeof (name), "uLights[%d].shadowCube", i);
		entry->light_shadow_cube_loc[i] = GL_GetUniformLocationFunc (program, name);
	}

	return entry;
}

static void Shadow_RegisterCvars (void)
{
	static qboolean registered = false;
	if (registered)
		return;

	Cvar_RegisterVariable (&gl_shadows);
	Cvar_RegisterVariable (&gl_shadow_mapsize);
	Cvar_RegisterVariable (&gl_shadow_softness);
	Cvar_RegisterVariable (&gl_shadow_pcf_samples);
	Cvar_RegisterVariable (&gl_shadow_maxlights);
	Cvar_RegisterVariable (&gl_shadow_bias);
	Cvar_RegisterVariable (&gl_shadow_normalbias);
	Cvar_RegisterVariable (&gl_shadow_update_static_interval);
	Cvar_RegisterVariable (&gl_shadow_debug);
	Cvar_RegisterVariable (&r_showshadows);

	registered = true;
}

static int Shadow_ClampMapSize (int size)
{
	int clamped = 1;
	if (size <= 0)
		size = 1;

	while (clamped < size)
		clamped <<= 1;

	if (clamped < 16)
		clamped = 16;
	if (clamped > 4096)
		clamped = 4096;
	return clamped;
}

static void Shadow_UpdateSettings (void)
{
	shadow_manager.map_size = Shadow_ClampMapSize ((int)gl_shadow_mapsize.value);
	/* Clamps werden zusätzlich gegen Texture-Budget geprüft, sobald bekannt */
	shadow_manager.max_lights = CLAMP (0, (int)gl_shadow_maxlights.value, MAX_SHADOW_LIGHTS);
}

void R_InitShadow (void)
{
	memset (&shadow_manager, 0, sizeof (shadow_manager));
	Shadow_RegisterCvars ();
	Shadow_UpdateSettings ();
	Shadow_ClearUniformCache ();
	Shadow_QueryTextureUnitBudget ();
	shadow_manager.max_lights = Shadow_ClampLightsToTextureBudget (shadow_manager.max_lights);
	if (!Shadow_CreateFallbackCube ())
	{
		Con_Printf ("Failed to create fallback shadow cubemap. Point light shadows disabled.\n");
		shadow_manager.initialized = false;
		return;
	}
	shadow_manager.initialized = true;
	Shadow_CheckGLErrors ("R_InitShadow");
}

void R_ShutdownShadow (void)
{
	if (!shadow_manager.initialized)
		return;

	Shadow_ResetPool ();
	Shadow_ClearUniformCache ();
	if (shadow_manager.fallback_cube_tex)
	{
		GL_DeleteNativeTexture (shadow_manager.fallback_cube_tex);
		shadow_manager.fallback_cube_tex = 0;
	}
	shadow_manager.initialized = false;
}

void R_ResizeShadowMapIfNeeded (void)
{
	int new_size;
	if (!shadow_manager.initialized)
		return;

	new_size = Shadow_ClampMapSize ((int)gl_shadow_mapsize.value);
	if (new_size != shadow_manager.map_size)
	{
		shadow_manager.map_size = new_size;
		Shadow_ResetPool ();
	}

	shadow_manager.max_lights = Shadow_ClampLightsToTextureBudget (CLAMP (0, (int)gl_shadow_maxlights.value, MAX_SHADOW_LIGHTS));
}

void R_ShadowBeginFrame (int frame_num)
{
	(void)frame_num;
	if (!shadow_manager.initialized)
		return;

	shadow_manager.active_light_count = 0;
	Shadow_UpdateSettings ();
	shadow_manager.max_lights = Shadow_ClampLightsToTextureBudget (shadow_manager.max_lights);
}

void R_ShadowSyncWorldLights (const gpulight_t* lights, int numlights)
{
	int i;
	int count = 0;
	int max_upload_lights;

	if (!shadow_manager.initialized)
		return;

	shadow_manager.active_light_count = 0;

	if (!gl_shadows.value)
		return;

	max_upload_lights = Shadow_ClampLightsToTextureBudget (shadow_manager.max_lights);

	for (i = 0; i < numlights && count < max_upload_lights; ++i)
	{
		const gpulight_t* src = &lights[i];
		shadow_light_uniform_t* dst;

		if (src->radius <= 0.0f)
			continue;

		dst = &shadow_manager.active_lights[count];
		VectorCopy (src->pos, dst->pos);
		dst->radius = src->radius;
		VectorCopy (src->color, dst->color);
		dst->intensity = 1.0f;
		dst->cube_tex = shadow_manager.fallback_cube_tex;

#if R_SHADOWMAPS
		R_UploadShadowMatrices (NULL, i); // TODO: upload per-light view-projection matrices.
#endif

		++count;
	}

	shadow_manager.active_light_count = count;
	Shadow_CheckGLErrors ("R_ShadowSyncWorldLights");
}

void R_ShadowApplyWorldUniforms (GLuint program)
{
	shadow_program_uniforms_t* uniforms;
	int show = (r_showshadows.value != 0.f) ? 1 : 0;
	int i;
	int active_lights = 0;
	int max_upload_lights;
	float bias = gl_shadow_bias.value;
	float normal_bias = gl_shadow_normalbias.value;
	float softness = gl_shadow_softness.value;
	int pcf_samples = (int)gl_shadow_pcf_samples.value;

	if (!shadow_manager.initialized)
		return;

	/* harte Clamps gegen Ausreißer */
	if (pcf_samples < 1) pcf_samples = 1;
	if (pcf_samples > SHADOW_PCF_SAMPLES_MAX) pcf_samples = SHADOW_PCF_SAMPLES_MAX;
	if (softness < 0.0f) softness = 0.0f;
	if (softness > SHADOW_SOFTNESS_MAX) softness = SHADOW_SOFTNESS_MAX;
	if (bias < 0.0f) bias = 0.0f;
	if (bias > SHADOW_BIAS_MAX) bias = SHADOW_BIAS_MAX;
	if (normal_bias < 0.0f) normal_bias = 0.0f;
	if (normal_bias > SHADOW_NORMALBIAS_MAX) normal_bias = SHADOW_NORMALBIAS_MAX;

	uniforms = Shadow_GetProgramUniforms (program);
	if (!uniforms)
		return;

	if (!uniforms->sampler_units_initialized)
	{
		for (i = 0; i < MAX_SHADOW_LIGHTS; ++i)
		{
			if (uniforms->light_shadow_cube_loc[i] >= 0)
				GL_Uniform1iFunc (uniforms->light_shadow_cube_loc[i], SHADOW_TEXTURE_UNIT_BASE + i);
		}
		uniforms->sampler_units_initialized = true;
	}

	if (uniforms->show_shadows_loc >= 0)
		GL_Uniform1iFunc (uniforms->show_shadows_loc, show);

	max_upload_lights = Shadow_ClampLightsToTextureBudget (shadow_manager.max_lights);

	if (gl_shadows.value && shadow_manager.fallback_cube_tex)
	{
		int total = q_min (shadow_manager.active_light_count, max_upload_lights);
		for (i = 0; i < total && active_lights < max_upload_lights; ++i)
		{
			shadow_light_uniform_t* light = &shadow_manager.active_lights[i];
			GLuint cube_tex = light->cube_tex ? light->cube_tex : shadow_manager.fallback_cube_tex;
			int slot = active_lights;

			if (light->radius <= 0.0f)
				continue;

			if (uniforms->light_pos_loc[slot] >= 0)
				GL_Uniform3fFunc (uniforms->light_pos_loc[slot], light->pos[0], light->pos[1], light->pos[2]);

			if (uniforms->light_radius_loc[slot] >= 0)
				GL_Uniform1fFunc (uniforms->light_radius_loc[slot], light->radius);

			if (uniforms->light_color_loc[slot] >= 0)
				GL_Uniform3fFunc (uniforms->light_color_loc[slot], light->color[0], light->color[1], light->color[2]);

			if (uniforms->light_intensity_loc[slot] >= 0)
				GL_Uniform1fFunc (uniforms->light_intensity_loc[slot], light->intensity);

			if (uniforms->light_bias_loc[slot] >= 0)
				GL_Uniform1fFunc (uniforms->light_bias_loc[slot], bias);

			if (uniforms->light_normal_bias_loc[slot] >= 0)
				GL_Uniform1fFunc (uniforms->light_normal_bias_loc[slot], normal_bias);

			if (uniforms->light_softness_loc[slot] >= 0)
				GL_Uniform1fFunc (uniforms->light_softness_loc[slot], softness);

			if (uniforms->light_pcf_samples_loc[slot] >= 0)
				GL_Uniform1iFunc (uniforms->light_pcf_samples_loc[slot], pcf_samples);

			GL_BindNative (GL_TEXTURE0 + SHADOW_TEXTURE_UNIT_BASE + slot,
				GL_TEXTURE_CUBE_MAP, cube_tex);

			++active_lights;
		}
	}

	for (i = active_lights; i < max_upload_lights; ++i)
	{
		if (uniforms->light_radius_loc[i] >= 0)
			GL_Uniform1fFunc (uniforms->light_radius_loc[i], 0.0f);
		if (uniforms->light_intensity_loc[i] >= 0)
			GL_Uniform1fFunc (uniforms->light_intensity_loc[i], 0.0f);
		GL_BindNative (GL_TEXTURE0 + SHADOW_TEXTURE_UNIT_BASE + i,
			GL_TEXTURE_CUBE_MAP, shadow_manager.fallback_cube_tex);
	}

	if (uniforms->active_lights_loc >= 0)
		GL_Uniform1iFunc (uniforms->active_lights_loc, active_lights);
	Shadow_CheckGLErrors ("R_ShadowApplyWorldUniforms");
}

void R_ShadowEndFrame (void)
{
	/* Currently a placeholder for future per-frame cleanup. */
	if (!shadow_manager.initialized)
		return;
	Shadow_CheckGLErrors ("R_ShadowEndFrame");
}
