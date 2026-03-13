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

#ifndef GLQUAKE_H
#define GLQUAKE_H

void GL_BeginRendering (int *x, int *y, int *width, int *height);
void GL_EndRendering (void);
void GL_Set2D (void);
void GL_SetScissorEnabled (qboolean enabled);

extern	int glx, gly, glwidth, glheight;

// r_local.h -- private refresh defs

#define ALIAS_BASE_SIZE_RATIO		(1.0 / 11.0)
					// normalizing factor so player model works out to about
					//  1 pixel per triangle
#define	MAX_LBM_HEIGHT		480

#define BACKFACE_EPSILON	0.01

typedef struct dlight_s dlight_t;

extern vec3_t *r_pointfile;

void R_TimeRefresh_f (void);
void R_ReadPointFile_f (void);
texture_t *R_TextureAnimation (texture_t *base, int frame);

typedef enum {
	pt_static, pt_grav, pt_slowgrav, pt_fire, pt_explode, pt_explode2, pt_blob, pt_blob2
} ptype_t;

// !!! if this is changed, it must be changed in d_ifacea.h too !!!
typedef struct particle_s
{
// driver-usable fields
	vec3_t		org;
	byte		color;
// drivers never touch the following fields
	byte		type;
	float		spawn;
	float		die;
	vec3_t		vel;
	float		ramp;
} particle_t;

typedef struct particle_debug_stats_s
{
	int	active_legacy;
	int	active_q3p;
	int	bucket_legacy[8];
	int	q3p_spawned;
	int	q3p_dropped;
	int	q3p_culled;
	int	q3p_gpu_sim_frames;
	int	q3p_gpu_sim_fallbacks;
	int	q3p_gpu_cullsort_frames;
	int	q3p_gpu_visible;
	int	q3p_gpu_culled;
	float	overdraw_score;
	qboolean has_bounds;
	vec3_t	bounds_mins;
	vec3_t	bounds_maxs;
} particle_debug_stats_t;


//====================================================

extern	int		r_visframecount;	// ??? what difs?
extern	int		r_framecount;
extern	mplane_t	frustum[4];

extern	qboolean use_simd;

//
// view origin
//
extern	vec3_t	vup;
extern	vec3_t	vpn;
extern	vec3_t	vright;
extern	vec3_t	r_origin;

//
// screen size info
//
extern	refdef_t	r_refdef;
extern	mleaf_t		*r_viewleaf, *r_oldviewleaf;
extern	int		d_lightstylevalue[256];	// 8.8 fraction of base light value

extern	cvar_t	r_norefresh;
extern	cvar_t	r_drawentities;
extern	cvar_t	r_drawworld;
extern	cvar_t	r_drawviewmodel;
extern	cvar_t	r_gl_state_validate;
extern	cvar_t	r_speeds;
extern	cvar_t	r_pos;
extern	cvar_t	r_waterwarp;
extern	cvar_t	r_fullbright;
extern	cvar_t	r_lightmap;
extern	cvar_t	r_rgblighting_enable;
extern	cvar_t	r_wateralpha;
extern	cvar_t	r_telealpha;
extern	cvar_t	r_slimealpha;
extern	cvar_t	r_litwater;
extern	cvar_t	r_dynamic;
extern	cvar_t	r_novis;
extern	cvar_t	r_scale;
extern	cvar_t	r_shadow;
extern	cvar_t	r_shadow_sun;
extern	cvar_t	r_shadow_dlight;
extern	cvar_t	r_shadow_dlight_max;
extern	cvar_t	r_shadow_sun_size;
extern	cvar_t	r_shadow_dlight_size;
extern	cvar_t	r_shadow_sun_distance;
extern	cvar_t	r_shadow_sun_bias;
extern	cvar_t	r_shadow_dlight_bias;
extern	cvar_t	r_shadow_sun_pcf;
extern	cvar_t	r_shadow_dlight_pcf;
extern	cvar_t	r_shadow_sun_snap;
extern	cvar_t	r_shadow_mark_mode;
extern	cvar_t	r_shadow_debug;
extern	cvar_t	r_shadow_log;
extern	cvar_t	r_rimlight;
extern	cvar_t	r_rimlight_world;
extern	cvar_t	r_rimlight_models;
extern	cvar_t	r_rimlight_intensity;
extern	cvar_t	r_rimlight_power;
extern	cvar_t	r_rimlight_sun;
extern	cvar_t	r_rimlight_dlight;
extern	cvar_t	r_rimlight_shadow;

extern	cvar_t	r_oit;
extern	cvar_t	r_alphasort;

extern	cvar_t	gl_clear;
extern	cvar_t	gl_polyblend;
extern	cvar_t	gl_nocolors;
extern	cvar_t	gl_finish;

extern	cvar_t	gl_playermip;

extern int		gl_stencilbits;
extern	qboolean	gl_buffer_storage_able;
extern	qboolean	gl_multi_bind_able;
extern	qboolean	gl_bindless_able;
extern	qboolean	gl_clipcontrol_able;

extern	const char	*gl_vendor;
extern	const char	*gl_renderer;
extern	const char	*gl_version;

//==============================================================================

#define QGL_CORE_FUNCTIONS(x)\
	x(void,			DrawArraysInstanced, (GLenum mode, GLint first, GLsizei count, GLsizei primcount))\
	x(void,			DrawElementsInstanced, (GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount))\
	x(void,			VertexAttribDivisor, (GLuint index, GLuint divisor))\
	x(void,			DrawElementsIndirect, (GLenum mode, GLenum type, const void *indirect))\
	x(void,			MultiDrawElementsIndirect, (GLenum mode, GLenum type, const void *indirect, GLsizei drawcount, GLsizei stride))\
	x(void,			GenBuffers, (GLsizei n, GLuint *buffers))\
	x(void,			DeleteBuffers, (GLsizei n, const GLuint *buffers))\
	x(void,			BindBuffer, (GLenum target, GLuint buffer))\
	x(void,			BindBufferRange, (GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size))\
	x(void,			BufferData, (GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage))\
	x(void,			BufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data))\
	x(GLvoid*,		MapBuffer, (GLenum target, GLenum access))\
	x(GLboolean,	UnmapBuffer, (GLenum target))\
	x(void*,		MapBufferRange, (GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access))\
	x(void,			FlushMappedBufferRange, (GLenum target, GLintptr offset, GLsizeiptr length))\
	x(GLsync,		FenceSync, (GLenum condition, GLbitfield flags))\
	x(void,			DeleteSync, (GLsync sync))\
	x(GLenum,		ClientWaitSync, (GLsync sync, GLbitfield flags, GLuint64 timeout))\
	x(void,			WaitSync, (GLsync sync, GLbitfield flags, GLuint64 timeout))\
	x(GLuint,		CreateProgram, (void))\
	x(void,			DeleteProgram, (GLuint program))\
	x(void,			GetProgramiv, (GLuint program, GLenum pname, GLint *params))\
	x(void,			UseProgram, (GLuint program))\
	x(void,			LinkProgram, (GLuint program))\
	x(void,			GetProgramInfoLog, (GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog))\
	x(GLuint,		CreateShader, (GLenum type))\
	x(void,			DeleteShader, (GLuint shader))\
	x(void,			ShaderSource, (GLuint shader, GLsizei count, const GLchar* const *string, const GLint *length))\
	x(void,			CompileShader, (GLuint shader))\
	x(void,			GetShaderInfoLog, (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog))\
	x(void,			GetShaderiv, (GLuint shader, GLenum pname, GLint *params))\
	x(void,			AttachShader, (GLuint program, GLuint shader))\
	x(void,			DetachShader, (GLuint program, GLuint shader))\
	x(void,			BindAttribLocation, (GLuint program, GLuint index, const GLchar *name))\
	x(void,			BindVertexArray, (GLuint array))\
	x(void,			GenVertexArrays, (GLsizei n, GLuint *arrays))\
	x(void,			DeleteVertexArrays, (GLsizei n, const GLuint *arrays))\
	x(void,			EnableVertexAttribArray, (GLuint index))\
	x(void,			DisableVertexAttribArray, (GLuint index))\
	x(void,			VertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const GLvoid *pointer))\
	x(void,			VertexAttribIPointer, (GLuint index, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer))\
	x(GLint,		GetUniformLocation, (GLuint program, const GLchar *name))\
	x(void,			GetUniformiv, (GLuint program, GLint location, GLint *params))\
	x(void,			GetIntegeri_v, (GLenum target, GLuint index, GLint *data))\
	x(void,			GetActiveUniform, (GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name))\
	x(void,			Uniform1i, (GLint location, GLint v0))\
	x(void,			Uniform1f, (GLint location, GLfloat v0))\
	x(void,			Uniform2f, (GLint location, GLfloat v0, GLfloat v1))\
	x(void,			Uniform3f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2))\
	x(void,			Uniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))\
	x(void,			Uniform3fv, (GLint location, GLsizei count, const GLfloat *value))\
	x(void,			Uniform4fv, (GLint location, GLsizei count, const GLfloat *value))\
	x(void,			UniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value))\
	x(void,			ActiveTexture, (GLenum texture))\
	x(void,			GenerateMipmap, (GLenum target))\
	x(void,			BindFramebuffer, (GLenum target, GLuint framebuffer))\
	x(void,			GenFramebuffers, (GLsizei n, GLuint *framebuffers))\
	x(void,			DeleteFramebuffers, (GLsizei n, const GLuint *framebuffers))\
	x(void,			FramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level))\
	x(void,			FramebufferTextureLayer, (GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer))\
	x(void,			GetFramebufferAttachmentParameteriv, (GLenum target, GLenum attachment, GLenum pname, GLint *params))\
	x(GLenum,		CheckFramebufferStatus, (GLenum target))\
	x(void,			BlitFramebuffer, (GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter))\
	x(void,			DrawBuffers, (GLsizei n, const GLenum *bufs))\
	x(void,			ClearBufferfv, (GLenum buffer, GLint drawbuffer, const GLfloat *value))\
	x(void,			BlendEquation, (GLenum mode))\
	x(void,			BlendFunci, (GLuint buf, GLenum sfactor, GLenum dfactor))\
	x(void,			DebugMessageCallback, (GLDEBUGPROC callback, const void *userParam))\
	x(void,			ObjectLabel, (GLenum identifier, GLuint name, GLsizei length, const GLchar *label))\
	x(void,			PushDebugGroup, (GLenum source, GLuint id, GLsizei length, const char * message))\
	x(void,			PopDebugGroup, (void))\
	x(const GLubyte*,GetStringi, (GLenum name, GLuint index))\
	x(void,			TexStorage2D, (GLenum target, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height))\
	x(void,			TexStorage3D, (GLenum target, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height, GLsizei depth))\
	x(void,			CompressedTexImage2D, (GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const GLvoid *data))\
	x(void,			TexStorage2DMultisample, (GLenum target, GLsizei samples, GLenum internalFormat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations))\
	x(void,			MinSampleShading, (GLfloat value))\
	x(void,			TexImage3D, (GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const GLvoid *pixels))\
	x(void,			TexSubImage3D, (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const GLvoid *pixels))\
	x(void,			BindImageTexture, (GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access, GLenum format))\
	x(void,			MemoryBarrier, (GLbitfield barriers))\
	x(void,			DispatchCompute, (GLuint num_groups_x, GLuint num_groups_y, GLuint num_groups_z))\
	x(void,			GenSamplers, (GLsizei n, GLuint *samplers))\
	x(void,			DeleteSamplers, (GLsizei n, const GLuint *samplers))\
	x(void,			SamplerParameteri, (GLuint sampler, GLenum pname, GLint param))\
	x(void,			SamplerParameterf, (GLuint sampler, GLenum pname, GLfloat param))\
	x(void,			BindSampler, (GLuint unit, GLuint sampler))\
	x(void,			GenQueries, (GLsizei n, GLuint *ids))\
	x(void,			DeleteQueries, (GLsizei n, const GLuint *ids))\
	x(void,			BeginQuery, (GLenum target, GLuint id))\
	x(void,			EndQuery, (GLenum target))\
	x(void,			GetQueryiv, (GLenum target, GLenum pname, GLint *params))\
	x(void,			GetQueryObjectiv, (GLuint id, GLenum pname, GLint *params))\
	x(void,			GetQueryObjectuiv, (GLuint id, GLenum pname, GLuint *params))\
	x(void,			QueryCounter, (GLuint id, GLenum target))\
	x(void,			GetQueryObjecti64v, (GLuint id, GLenum pname, GLint64 *params))\
	x(void,			GetQueryObjectui64v, (GLuint id, GLenum pname, GLuint64 *params))\

#define QGL_ARB_buffer_storage_FUNCTIONS(x)\
	x(void,			BufferStorage, (GLenum target, GLsizeiptr size, const void *data, GLbitfield flags))\

#define QGL_ARB_multi_bind_FUNCTIONS(x)\
	x(void,			BindBuffersRange, (GLenum target, GLuint first, GLsizei count, const GLuint *buffers, const GLintptr *offsets, const GLsizeiptr *sizes))\
	x(void,			BindTextures, (GLuint first, GLsizei count, const GLuint *textures))\
	x(void,			BindSamplers, (GLuint first, GLsizei count, const GLuint *samplers))\
	x(void,			BindImageTextures, (GLuint first, GLsizei count, const GLuint *textures))\

#define QGL_ARB_bindless_texture_FUNCTIONS(x)\
	x(GLuint64,		GetTextureHandleARB, (GLuint texture))\
	x(GLuint64,		GetTextureSamplerHandleARB, (GLuint texture, GLuint sampler))\
	x(void,			MakeTextureHandleResidentARB, (GLuint64 handle))\
	x(void,			MakeTextureHandleNonResidentARB, (GLuint64 handle))\

#define QGL_ARB_clip_control_FUNCTIONS(x)\
	x(void,			ClipControl, (GLenum origin, GLenum depth))\

#define GL_ZERO_TO_ONE		0x935F

#define QGL_ALL_FUNCTIONS(x)\
	QGL_CORE_FUNCTIONS(x)\
	QGL_ARB_buffer_storage_FUNCTIONS(x)\
	QGL_ARB_multi_bind_FUNCTIONS(x)\
	QGL_ARB_bindless_texture_FUNCTIONS(x)\
	QGL_ARB_clip_control_FUNCTIONS(x)\

#define QGL_DECLARE_FUNC(ret, name, args) extern ret (APIENTRYP GL_##name##Func) args;
QGL_ALL_FUNCTIONS(QGL_DECLARE_FUNC)
#undef QGL_DECLARE_FUNC

void GL_BeginGroup (const char *name);
void GL_EndGroup (void);

//==============================================================================

// Note: in order to simplify state management we impose a few restrictions:
// - if a shader uses N vertex attributes their indices must be [0..N-1]
// - instanced vertex attributes, if any, must come first

// Total number of vertex attributes used (instanced + non-instanced)
#define GLS_ATTRIBS(n)				((n) << GLS_ATTRIBS_SHIFT)

// Number of instanced vertex attributes used
#define GLS_INSTANCED_ATTRIBS(n)	((n) << GLS_INSTANCED_ATTRIBS_SHIFT)

typedef enum {
        GLS_NO_ZTEST                            = 1 << 0,
        GLS_NO_ZWRITE                           = 1 << 1,

        GLS_BLEND_OPAQUE                        = 0 << 2,
        GLS_BLEND_ALPHA                         = 1 << 2,
        GLS_BLEND_ALPHA_OIT                     = 2 << 2,
        GLS_BLEND_MULTIPLY                      = 3 << 2,
        GLS_BLEND_ADD                           = 4 << 2,
        GLS_MASK_BLEND                          = 7 << 2,

        GLS_CULL_BACK                           = 0 << 5,
        GLS_CULL_NONE                           = 1 << 5,
        GLS_CULL_FRONT                          = 2 << 5,
        GLS_MASK_CULL                           = 3 << 5,

        GLS_ATTRIBS_BITS                        = 3,
        GLS_ATTRIBS_SHIFT                       = 7,
        GLS_ATTRIBS_MAXCOUNT            = (1 << GLS_ATTRIBS_BITS) - 1,
        GLS_MASK_ATTRIBS                        = GLS_ATTRIBS_MAXCOUNT << GLS_ATTRIBS_SHIFT,

        GLS_INSTANCED_ATTRIBS_SHIFT     = GLS_ATTRIBS_SHIFT + GLS_ATTRIBS_BITS,
        GLS_MASK_INSTANCED_ATTRIBS      = GLS_ATTRIBS_MAXCOUNT << GLS_INSTANCED_ATTRIBS_SHIFT,

        GLS_DEFAULT_STATE                       = GLS_BLEND_OPAQUE | GLS_CULL_BACK | GLS_ATTRIBS (1),
} glstatebits_t;

extern unsigned glstate;
void GL_SetState (unsigned mask);
void GL_ResetState (void);

extern GLint ssbo_align; // SSBO alignment - 1
extern GLint ubo_align; // UBO alignment - 1

//johnfitz -- anisotropic filtering
#define	GL_TEXTURE_MAX_ANISOTROPY_EXT		0x84FE
#define	GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT	0x84FF
extern	float		gl_max_anisotropy;
extern	qboolean	gl_anisotropy_able;

//johnfitz -- polygon offset
#define OFFSET_BMODEL 1
#define OFFSET_NONE 0
#define OFFSET_DECAL -1
#define OFFSET_FOG -2
#define OFFSET_SHOWTRIS -3
void GL_PolygonOffset (int);

typedef enum
{
	ZRANGE_FULL,
	ZRANGE_VIEWMODEL,
	ZRANGE_NEAR,
} zrange_t;
void GL_DepthRange (zrange_t range);

typedef enum
{
	ALPHAMODE_BASIC,
	ALPHAMODE_SORTED,
	ALPHAMODE_OIT,

	ALPHAMODE_COUNT
} alphamode_t;

alphamode_t R_GetAlphaMode (void);
alphamode_t R_GetEffectiveAlphaMode (void);
void R_SetAlphaMode (alphamode_t mode);

//johnfitz -- rendering statistics
extern int rs_brushpolys, rs_aliaspolys, rs_skypolys;
extern int rs_dynamiclightmaps, rs_brushpasses, rs_aliaspasses, rs_skypasses;

//johnfitz -- track developer statistics that vary every frame
extern cvar_t devstats;
typedef struct {
	int		packetsize;
	int		edicts;
	int		visedicts;
	int		efrags;
	int		tempents;
	int		beams;
	int		dlights;
	int		gpu_upload;
} devstats_t;
extern devstats_t dev_stats, dev_peakstats;

//ohnfitz -- reduce overflow warning spam
typedef struct {
	double	packetsize;
	double	efrags;
	double	beams;
	double	varstring;
} overflowtimes_t;
extern overflowtimes_t dev_overflows; //this stores the last time overflow messages were displayed, not the last time overflows occured
#define CONSOLE_RESPAM_TIME 3 // seconds between repeated warning messages

//johnfitz -- moved here from r_brush.c
extern int gl_lightmap_format, gl_lightmap_type, lightmap_bytes;
extern int lightmap_block_width, lightmap_block_height;
extern cvar_t gl_lightmap_atlas_size, gl_lightmap_format_cvar;
void GL_OnLightmapAtlasSizeChanged (cvar_t *var);

typedef struct lightmap_s
{
	int			xofs;
	int			yofs;
} lightmap_t;
extern lightmap_t *lightmaps;
extern int lightmap_count;	//allocated lightmaps

extern qboolean r_fullbright_cheatsafe, r_lightmap_cheatsafe, r_drawworld_cheatsafe; //johnfitz

extern float	map_wateralpha, map_lavaalpha, map_telealpha, map_slimealpha; //ericw
extern float	map_fallbackalpha; //spike -- because we might want r_wateralpha to apply to teleporters while water itself wasn't watervised

#define NUMVERTEXNORMALS	162
extern const float	r_avertexnormals[NUMVERTEXNORMALS][3];

//johnfitz -- fog functions called from outside gl_fog.c
void Fog_ParseServerMessage (void);
float *Fog_GetColor (void);
float Fog_GetDensity (void);
void Fog_EnableGFog (void);
void Fog_DisableGFog (void);
void Fog_SetupFrame (void);
void Fog_NewMap (void);
void Fog_Init (void);

extern float r_matview[16];
extern float r_matproj[16];
extern float r_matviewproj[16];
extern float r_fovx, r_fovy;

void R_NewGame (void);

#define LIGHT_TILES_X			32
#define LIGHT_TILES_Y			16
#define LIGHT_TILES_Z			32

typedef struct gpulight_s {
	float	pos[3];
	float	radius;
	float	color[3];
	float	minlight;
} gpulight_t;

#define DLIGHT_GPU_MAX 64

#define SHADOW_DLIGHT_MAX 4

typedef struct gpulightbuffer_s {
	float		lightstyles[MAX_LIGHTSTYLES * 2];
	gpulight_t	lights[DLIGHT_GPU_MAX];
} gpulightbuffer_t;

typedef struct gpuframedata_s {
        // Fields are grouped into vec4-aligned blocks to match the std140 layout in frame_uniforms.glsl.
        float           viewproj[16];
        float           prev_viewproj[16];
        float           view[16];
        float           fogdata[4];
        float           skyfogdata[4];
        vec4_t          wind;           // xyz: winddir, w: windphase
        vec4_t          dither;         // x: screen, y: texture, z: overbright, w: padding
        vec4_t          eye;            // xyz: eyepos, w: time
        vec4_t          prev_eye;       // xyz: prev_eyepos, w: delta_time
        vec4_t          zparams;        // x: zlogscale, y: zlogbias, zw: padding
        float           lightmap_params[4];
        vec4_t          lightgrid_params; // x: enabled, yzw: unused
        vec4_t          dlight_params;  // x: style, y: debug view, z: pass selector, w: padding
        vec4_t          colorspace_params; // x: debug mode, y: manual gamma, z: output sRGB, w: unused
        vec4_t          shader_params;  // x: shader debug, y: tcgen debug, z: sun visibility attenuation, w: unused
        vec4_t          sun_dir_enabled; // xyz: sun dir (scene->sun), w: enabled
        vec4_t          sun_color_intensity; // rgb: sun color, w: intensity
        unsigned int    numlights;
        unsigned int    prev_frame_valid;
        unsigned int    _padding1;
        unsigned int    _padding2;
} gpuframedata_t;

typedef struct {
	vec3_t dir;
	vec3_t color;
	float intensity;

	// volumetrics
	float volumetric_intensity;
	float anisotropy;

	// shadows
	float shadow_bias;
	float shadow_strength;
	float shadow_distance;

	// godrays
	float ray_decay;
	float ray_density;
} sun_t;

COMPILE_TIME_ASSERT (gpuframedata_std140_size, sizeof (gpuframedata_t) == 432);

typedef enum
{
	TCGEN_BASE = 0,
	TCGEN_LIGHTMAP = 1,
	TCGEN_ENVIRONMENT = 2,
} tcgen_mode_t;

extern gpulightbuffer_t r_lightbuffer;
extern gpuframedata_t r_framedata;
extern float r_lightstyle_framefrac;
extern dlight_t *r_dlight_sources[DLIGHT_GPU_MAX];
extern sun_t r_sun_state;

qboolean R_WorldHasSun (void);
const sun_t* R_GetSun (void);
void R_SetSun (const sun_t *src);
void R_Sun_ResetToDefaults (sun_t *s);
void R_Sun_ApplyMapOverrides (sun_t *s, const char *worldspawn_entity_string);
void R_Sun_Finalize (sun_t *s);
void R_Sun_UpdateFromWorld (void);

void R_AnimateLight (void);
void R_MarkSurfaces (void);
void R_MarkSurfaces_Shadow (void);
void R_RestoreMarkedSurfaces (void);
qboolean R_CullBox (vec3_t emins, vec3_t emaxs);
qboolean R_CullModelForEntity (entity_t *e);
void R_GetEntityBounds (const entity_t *e, vec3_t mins, vec3_t maxs);
void R_EntityMatrix (float matrix[16], vec3_t origin, vec3_t angles, unsigned char scale);

void R_InitParticles (void);
void R_DrawParticles (qboolean alpha);
void R_DrawParticles_ShowTris (void);
void CL_RunParticles (void);
void R_ClearParticles (void);
void R_GetParticleDebugStats (particle_debug_stats_t *stats);

void R_TranslatePlayerSkin (int playernum);
void R_TranslateNewPlayerSkin (int playernum); //johnfitz -- this handles cases when the actual texture changes

void R_UploadFrameData (void);
void R_StorePrevFrameState (void);
qboolean R_PrevFrameValid (void);

void R_DrawBrushModels (entity_t **ents, int count);
void R_DrawBrushModels_Water (entity_t **ents, int count, qboolean translucent);
void R_DrawBrushModels_DLights (entity_t **ents, int count);
void R_DrawBrushModels_Godrays (entity_t **ents, int count);
void R_DrawBrushModels_SkyLayers (entity_t **ents, int count);
void R_DrawBrushModels_SkyCubemap (entity_t **ents, int count);
void R_DrawBrushModels_SkyStencil (entity_t **ents, int count);
void R_DrawBrushModels_Shadow (entity_t **ents, int count, qboolean dlight);
void R_DrawAliasModels (entity_t **ents, int count);
void R_DrawAliasModels_Shadow (entity_t **ents, int count, qboolean dlight);
void R_DrawSpriteModels (entity_t **ents, int count);
void R_DrawBrushModels_ShowTris (entity_t **ents, int count);
void R_DrawAliasModels_ShowTris (entity_t **ents, int count);
void R_DrawSpriteModels_ShowTris (entity_t **ents, int count);
void R_GLStateDump (const char *tag);
void R_RenderShadowMaps (void);
void R_RenderSunShadowMap (void);
void R_RenderDLightShadowMaps (void);
void R_Shadow_ApplyWorldReceiverUniforms (GLuint program);
void R_Shadow_ApplyAliasReceiverUniforms (GLuint program);
void R_Shadow_ApplyWorldCasterUniforms (GLuint program);
void R_Shadow_ApplyAliasCasterUniforms (GLuint program);

entity_t **R_GetVisEntities (modtype_t type, qboolean translucent, int *outcount);

void R_ClearBoundingBoxes (void);

#define MAX_BMODEL_DRAWS		4096
#define MAX_BMODEL_INSTANCES	1024

typedef struct bmodel_draw_indirect_s {
	GLuint		count;
	GLuint		instanceCount;
	GLuint		firstIndex;
	GLuint		baseVertex;
	GLuint		baseInstance;
} bmodel_draw_indirect_t;

typedef struct bmodel_gpu_marksurf_s {
	GLuint		packedleafsky; // bit 0=sky; bits 1..31=leafindex
	GLuint		surfindex;
} bmodel_gpu_marksurf_t;

typedef struct bmodel_gpu_surf_s {
	vec4_t		plane;
	GLuint		framecount;
	GLuint		texnum;
	GLuint		numedges;
	GLuint		firstvert;
	vec3_t		mins;
	GLuint		lightmap;
	vec3_t		maxs;
	GLuint		padding1;
} bmodel_gpu_surf_t;

void GL_BuildLightmaps (void);

void GL_DeleteBModelBuffers (void);
void GL_BuildBModelVertexBuffer (void);
void GL_BuildBModelMarkBuffers (void);
void R_ResetGodraysStabilization (void);
void GLMesh_LoadVertexBuffer (qmodel_t *m, aliashdr_t *hdr);
void GLMesh_LoadVertexBuffers (void);
void GLMesh_DeleteVertexBuffers (void);

int R_LightPoint (qmodel_t *model, vec3_t p, float ofs, lightcache_t *cache);
qboolean R_EntityStaticLight (entity_t *e, vec3_t out_color255, entity_lightinfo_t *info);
qboolean R_SampleLightmapAtPoint(const vec3_t pos, vec3_t out_rgb);
qboolean R_SampleLightmapAndDeluxemapAtPoint(const vec3_t pos, vec3_t out_rgb, vec3_t out_dir);
qboolean R_LightgridEnabled (void);
void R_LightgridLighting (const vec3_t pos, vec3_t out_color, float *out_ao);
void R_AddDynamicLights_Lightgrid (const vec3_t pos, vec3_t lightcolor);
void R_AccumulateEntityDLights (const vec3_t pos, vec3_t out_color, vec3_t out_dir);

extern cvar_t r_debug_itemlight;
extern cvar_t r_minlight_models;
extern cvar_t r_viewmodel_light_boost;
extern cvar_t r_viewmodel_minlight;
extern cvar_t r_model_lightgrid;
extern cvar_t r_model_light_multisample;
extern cvar_t r_model_light_smooth;
extern cvar_t r_dlight_models_directional;
extern cvar_t r_model_lightgrid_assist;
extern cvar_t r_model_lightgrid_assist_threshold;
extern cvar_t r_bmodel_relight;
extern cvar_t r_model_light_stats;
extern cvar_t r_model_light_samples_max;

#define WORLDSHADER_SOLID		0
#define WORLDSHADER_ALPHATEST	1
#define WORLDSHADER_WATER		2

#define ALIASSHADER_STANDARD	0
#define ALIASSHADER_DITHER		1
#define ALIASSHADER_NOPERSP		2

typedef struct glprogs_s {
	/* 2d */
	GLuint		gui;
	GLuint		viewblend;
	GLuint		warpscale[2];		// [warp]
	GLuint		postprocess[3];		// [palettize:off/dithered/direct]
	GLuint		bloom_extract;
	GLuint		bloom_blur;
	GLuint		ssao;
	GLuint		ssao_blur;
	GLuint		godrays_mask;
	GLuint		godrays;
	GLuint		godrays_source;
	GLuint		godrays_source_sky;
	GLuint		fogvol;
	GLuint		fogvol_global;
	GLuint		fogvol_temporal;
	GLuint		fogvol_froxel_inject;
	GLuint		oit_resolve[2];		// [msaa]

	/* 3d */
	GLuint		world[2][3][3];		// [OIT][standard/dithered/banded][solid/alpha test/water]
	GLuint		world_dlight[2];		// [alpha test]
	GLuint		world_shadow[2];		// [dlight:0=sun,1=point]
	GLuint		water[2][2];		// [OIT][dither]
	GLuint		teleport[2][2];		// [OIT][dither]
	GLuint		skystencil;
	GLuint		skylayers[2];		// [dither]
	GLuint		skycubemap[2][2];	// [anim][dither]
	GLuint		skyboxside[2];		// [dither]
	GLuint		alias[2][3][2][2];	// [OIT][mode:standard/dithered/noperspective][alpha test][md5]
	GLuint		alias_shadow[2];		// [md5]
	GLuint		sprites[2];			// [dither]
	GLuint		decal;
	GLuint		decal_instanced;
	GLuint		particles[2][2];	// [OIT][dither]
	GLuint		debug3d;
	GLuint		dlight_composite;

	/* compute */
	GLuint		clear_indirect;
	GLuint		gather_indirect;
	GLuint		cull_mark;
	GLuint		q3p_sim;
	GLuint		q3p_cull_key;
	GLuint		gpu_bitonic_pairs;
	GLuint		fogvol_dlight_score;
	GLuint		fogvol_static_build;
	GLuint		palette_init[3];	// [metric:naive/riemersma/oklab]
	GLuint		palette_postprocess;
} glprogs_t;

extern glprogs_t glprogs;

void GL_UseProgram (GLuint program);
GLuint GL_GetCurrentProgram (void);
void GL_ClearCachedProgram (void);
void GL_CreateShaders (void);
void GL_DeleteShaders (void);
void R_DebugDrawWireBox (const vec3_t mins, const vec3_t maxs, const vec3_t color, qboolean ztest);
void R_DebugFlushGeometry (void);

typedef struct glframebufs_s {
	GLint			max_color_tex_samples;
	GLint			max_depth_tex_samples;
	GLint			max_samples; // lowest of max_color_tex_samples/max_depth_tex_samples

	struct {
		GLint		samples;
		GLuint		color_tex;
		GLuint		velocity_tex;
		GLuint		depth_stencil_tex;
		GLuint		fbo;
	}				scene;

	struct {
		GLuint		color_tex;
		GLuint		velocity_tex;
		GLuint		fbo;
	}				resolved_scene;

	struct {
		GLuint		color_tex;
		GLuint		depth_stencil_tex;
		GLuint		fbo;
	}				composite;

	struct {
		GLuint		color_tex[2];
		GLuint		fbo[2];
		GLuint		history_tex[2];
		GLuint		history_fbo[2];
		GLuint		composite_tex[2];
		GLuint		composite_fbo[2];
		GLuint		finalcopy_tex;
		GLuint		finalcopy_fbo;
		int			width;
		int			height;
	}				fogvol;

	struct {
		GLuint		extract_tex;
		GLuint		pingpong_tex[2];
		GLuint		extract_fbo;
		GLuint		pingpong_fbo[2];
		int			width;
		int			height;
	}				bloom;

	struct {
		GLuint		tex;
		GLuint		fbo;
	}				dlight;

	struct {
		GLuint		source_tex;
		GLuint		mask_tex;
		GLuint		shafts_tex;
		GLuint		source_fbo;
		GLuint		mask_fbo;
		GLuint		shafts_fbo;
		int			width;
		int			height;
	}				godrays;

	struct {
		GLuint		noise_tex;
		GLuint		ao_tex[2];
		GLuint		blur_tex[2];
		GLuint		ao_fbo[2];
		GLuint		blur_fbo[2];
		int			width[2];
		int			height[2];
		qboolean	valid;
	}				ssao;

	struct {
		GLuint		tex;
		GLuint		fbo;
		GLuint		pbo[2];
		int			pbo_index;
		qboolean	pbo_ready;
		int			width;
		int			height;
	}				autoexposure;

	struct {
		union {
			GLuint			mrt[2];
			struct {
				GLuint		accum_tex;
				GLuint		revealage_tex;
			};
		};
		GLuint		fbo_scene;
		GLuint		fbo_composite;
	}				oit;

	struct {
		GLuint		sun_depth_tex;
		GLuint		sun_fbo;
		GLuint		dlight_depth_tex;
		GLuint		dlight_fbo;
		int			sun_size;
		int			dlight_size;
		qboolean	available;
	}				shadow;
} glframebufs_t;

extern glframebufs_t framebufs;

void GL_CreateFrameBuffers (void);
void GL_DeleteFrameBuffers (void);


void GLLight_CreateResources (void);
void GLLight_DeleteResources (void);

void GLPalette_CreateResources (void);
void GLPalette_DeleteResources (void);
void GLPalette_UpdateLookupTable (void);
int GLPalette_Postprocess (void);

void GL_MakeAliasModelDisplayLists (qmodel_t *m, aliashdr_t *hdr);

typedef struct skybox_s
{
	char			name[256];
	float			wind_dist;
	float			wind_yaw;
	float			wind_pitch;
	float			wind_period;
	gltexture_t		*textures[6];
	gltexture_t		*cubemap;
	byte			*cubemap_pixels;
	void			*cubemap_offsets[6];
} skybox_t;

extern skybox_t		*skybox;

void Sky_Init (void);
void Sky_ClearAll (void);
void Sky_DrawSky (void);
void Sky_NewMap (void);
void Sky_LoadTexture (qmodel_t *m, texture_t *mt);
void Sky_LoadTextureQ64 (qmodel_t *m, texture_t *mt);
void Sky_LoadSkyBox (const char *name);
void Sky_SetupFrame (void);
qboolean Sky_IsAnimated (void);

qboolean R_TextureEmitsGodrays (texture_t *t);
qboolean R_SurfaceEmitsGodrays (msurface_t *s);

void GL_BindBuffer (GLenum target, GLuint buffer);
void GL_BindBufferRange (GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
void GL_BindBuffersRange (GLenum target, GLuint first, GLsizei count, const GLuint *buffers, const GLintptr *offsets, const GLsizeiptr *sizes);
GLuint GL_CreateBuffer (GLenum target, GLenum usage, const char *name, size_t size, const void *data);
void GL_DeleteBuffer (GLuint buffer);
void GL_ClearBufferBindings (void);

void GL_CreateFrameResources (void);
void GL_DeleteFrameResources (void);
void GL_Upload (GLenum target, const void *data, size_t numbytes, GLuint *outbuf, GLbyte **outofs);
void GL_ReserveDeviceMemory (GLenum target, size_t numbytes, GLuint *outbuf, size_t *outofs);
void GL_AcquireFrameResources (void);
void GL_ReleaseFrameResources (void);
void GL_AddGarbageBuffer (GLuint handle);

qboolean GL_NeedsSceneEffects (void);
qboolean GL_NeedsPostprocess (void);
void GL_PostProcess (void);

float GL_WaterAlphaForTextureType (textype_t type);

#endif	/* GLQUAKE_H */
