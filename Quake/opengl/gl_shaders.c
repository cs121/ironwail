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
#include "q_ctype.h"

#define GLSL_PATH_PREFIX "shaders/"
#define GLSL_PATH(name)   GLSL_PATH_PREFIX name

typedef struct shader_cache_s
{
        char path[MAX_QPATH];
        char *data;
} shader_cache_t;

static shader_cache_t shader_cache[128];
static int shader_cache_count;

glprogs_t glprogs;
static GLuint gl_programs[128];
static char *gl_program_names[128];
static char *gl_program_defines[128];
static char *gl_program_vert_paths[128];
static char *gl_program_frag_paths[128];
static GLuint gl_current_program;
static int gl_num_programs;

static const char *GL_LookupProgramName (GLuint program)
{
	int i;

	if (!program)
		return "none";

	for (i = 0; i < gl_num_programs; i++)
	{
		if (gl_programs[i] == program)
			return gl_program_names[i] ? gl_program_names[i] : "unnamed";
	}

	return "unknown";
}
static const char *GL_LookupProgramValue (GLuint program, char *const *table, const char *fallback)
{
	int i;

	if (!program)
		return fallback;

	for (i = 0; i < gl_num_programs; i++)
	{
		if (gl_programs[i] == program)
			return table[i] ? table[i] : fallback;
	}

	return fallback;
}


static char *GL_CopyString (const char *src)
{
	size_t len;
	char *out;

	if (!src)
		return NULL;
	len = strlen (src) + 1;
	out = (char *) malloc (len);
	if (!out)
		Sys_Error ("GL_CopyString: out of memory");
	memcpy (out, src, len);
	return out;
}

/*
=============
GL_InitError
=============
*/
static void GL_InitError (const char *message, ...)
{
	const char *fmt;
	char buf[4096];
	size_t len;
	va_list argptr;

	va_start (argptr, message);
	q_vsnprintf (buf, sizeof (buf), message, argptr);
	va_end (argptr);

	len = strlen (buf);
	while (len && q_isspace (buf[len - 1]))
		buf[--len] = '\0';

	fmt = 
		"Your system appears to meet the minimum requirements,\n"
		"however an error was encountered during OpenGL initialization.\n"
		"This could be caused by a driver or an engine bug.\n"
		"Please report this issue, including the following details:\n"
		"\n"
		"%s\n"
		"\n"
		"Engine:	Ironwail " IRONWAIL_VER_STRING " (%d-bit)\n"
		"OpenGL:	%s\n"
		"GPU:   	%s\n"
		"Vendor:	%s\n"
#if defined(_WIN32)
		"\n"
		"(Note: you can press Ctrl+C to copy this text to clipboard)"
#endif
	;

	Sys_Error (
		fmt,
		buf,
		(int) sizeof (void *) * 8,
		gl_version,
		gl_renderer,
		gl_vendor
	);
}

/*
=============
AppendString
=============
*/
static qboolean AppendString (char **dst, const char *dstend, const char *str, int len)
{
	int avail = dstend - *dst;
	if (len < 0)
		len = Q_strlen (str);
	if (len + 1 > avail)
		return false;
	memcpy (*dst, str, len);
	(*dst)[len] = 0;
	*dst += len;
	return true;
}

/*
=============
GL_CreateShader
=============
*/
static GLuint GL_CreateShader (GLenum type, const char *source, const char *extradefs, const char *name)
{
	const char *strings[16];
	const char *typestr = NULL;
	char header[256];
	int numstrings = 0;
	GLint status;
	GLuint shader;

	switch (type)
	{
		case GL_VERTEX_SHADER:
			typestr = "vertex";
			break;
		case GL_FRAGMENT_SHADER:
			typestr = "fragment";
			break;
		case GL_COMPUTE_SHADER:
			typestr = "compute";
			break;
		default:
			Sys_Error ("GL_CreateShader: unknown type 0x%X for %s", type, name);
			break;
	}

	q_snprintf (header, sizeof (header),
		"#version 430\n"
		"\n"
		"#define BINDLESS %d\n"
		"#define REVERSED_Z %d\n",
		gl_bindless_able,
		gl_clipcontrol_able
	);
	strings[numstrings++] = header;

	if (extradefs && *extradefs)
		strings[numstrings++] = extradefs;
	strings[numstrings++] = source;

	shader = GL_CreateShaderFunc (type);
	GL_ObjectLabelFunc (GL_SHADER, shader, -1, name);
	GL_ShaderSourceFunc (shader, numstrings, strings, NULL);
	GL_CompileShaderFunc (shader);
	GL_GetShaderivFunc (shader, GL_COMPILE_STATUS, &status);

	if (status != GL_TRUE)
	{
		char infolog[1024];
		memset(infolog, 0, sizeof(infolog));
		GL_GetShaderInfoLogFunc (shader, sizeof(infolog), NULL, infolog);
		GL_InitError ("Error compiling %s %s shader:\n\n%s", name, typestr, infolog);
	}

	return shader;
}

/*
=============
GL_CreateProgramFromShaders
=============
*/
static GLuint GL_CreateProgramFromShaders (const GLuint *shaders, int numshaders, const char *name, const char *defines, const char *vert_path, const char *frag_path)
{
	GLuint program;
	GLint status;

	program = GL_CreateProgramFunc ();
	GL_ObjectLabelFunc (GL_PROGRAM, program, -1, name);

	while (numshaders-- > 0)
	{
		GL_AttachShaderFunc (program, *shaders);
		GL_DeleteShaderFunc (*shaders);
		++shaders;
	}

	GL_LinkProgramFunc (program);
	GL_GetProgramivFunc (program, GL_LINK_STATUS, &status);

	if (GL_GetUniformBlockIndexFunc && GL_UniformBlockBindingFunc)
	{
		GLuint frame_data_block = GL_GetUniformBlockIndexFunc (program, FRAME_DATA_UBO_NAME);
		if (frame_data_block != GL_INVALID_INDEX)
			GL_UniformBlockBindingFunc (program, frame_data_block, FRAME_UBO_BINDING);
	}

	if (status != GL_TRUE)
	{
		char infolog[1024];
		memset(infolog, 0, sizeof(infolog));
		GL_GetProgramInfoLogFunc (program, sizeof(infolog), NULL, infolog);
		GL_InitError ("Error linking %s program:\n\n%s", name, infolog);
	}

	if (gl_num_programs == countof(gl_programs))
		Sys_Error ("gl_programs overflow");
	gl_programs[gl_num_programs] = program;
	gl_program_names[gl_num_programs] = GL_CopyString (name ? name : "unnamed");
	gl_program_defines[gl_num_programs] = GL_CopyString ((defines && defines[0]) ? defines : "");
	gl_program_vert_paths[gl_num_programs] = GL_CopyString (vert_path ? vert_path : "none");
	gl_program_frag_paths[gl_num_programs] = GL_CopyString (frag_path ? frag_path : "none");
	gl_num_programs++;

	Con_DPrintf ("SHDLOG: PROGRAM id=%u name=%s vert=%s frag=%s defines=%s\n",
		(unsigned)program,
		name ? name : "unnamed",
		vert_path ? vert_path : "none",
		frag_path ? frag_path : "none",
		(defines && defines[0]) ? defines : "<none>");

	return program;
}

/*
====================
GL_CreateProgramFromSources
====================
*/
static char *GL_LoadShaderFile_Internal (const char *path, int depth)
{
        size_t capacity, result_len;
        char *result;
        char *source;
        const char *cursor;
        int i;

        for (i = 0; i < shader_cache_count; i++)
        {
                if (!strcmp (shader_cache[i].path, path))
                        return shader_cache[i].data;
        }

        if (depth >= 32)
                Sys_Error ("GL_LoadShaderFile: include depth overflow for %s", path);

        if (shader_cache_count == countof(shader_cache))
                Sys_Error ("GL_LoadShaderFile: shader cache overflow");

        source = (char *) COM_LoadMallocFile (path, NULL);
        if (!source)
                GL_InitError ("Unable to load shader file %s", path);

        capacity = strlen (source) + 1;
        if (capacity < 64)
                capacity = 64;
        result = (char *) malloc (capacity);
        if (!result)
                Sys_Error ("GL_LoadShaderFile: out of memory processing %s", path);
        result[0] = '\0';
        result_len = 0;

#define APPEND_STR(srcptr, srclen)                                                     \
        do                                                                             \
        {                                                                              \
                size_t _need = (srclen);                                               \
                while (result_len + _need + 1 > capacity)                              \
                {                                                                      \
                        size_t _newcap = capacity * 2;                                 \
                        char *_newbuf = (char *) realloc (result, _newcap);            \
                        if (!_newbuf)                                                  \
                        {                                                              \
                                free (result);                                         \
                                free (source);                                         \
                                Sys_Error ("GL_LoadShaderFile: realloc failed for %s", path); \
                        }                                                              \
                        result = _newbuf;                                              \
                        capacity = _newcap;                                            \
                }                                                                      \
                memcpy (result + result_len, (srcptr), _need);                         \
                result_len += _need;                                                   \
                result[result_len] = '\0';                                             \
        } while (0)

        cursor = source;
        while (*cursor)
        {
                const char *line_start = cursor;
                const char *line_end = strchr (cursor, '\n');
                size_t line_len = line_end ? (size_t) (line_end - cursor + 1) : strlen (cursor);
                const char *trim = line_start;

                while (trim < line_start + line_len && (*trim == ' ' || *trim == '\t' || *trim == '\r'))
                        trim++;

                if ((size_t) (line_start + line_len - trim) >= 8 && !strncmp (trim, "#include", 8))
                {
                        const char *ptr = trim + 8;
                        char delim = '\0';

                        while (ptr < line_start + line_len && (*ptr == ' ' || *ptr == '\t'))
                                ptr++;
                        if (ptr < line_start + line_len && (*ptr == '"' || *ptr == '<'))
                        {
                                delim = (*ptr == '<') ? '>' : '"';
                                ptr++;
                        }

                        if (delim)
                        {
                                const char *end = ptr;
                                while (end < line_start + line_len && *end != delim)
                                        end++;
                                if (end < line_start + line_len)
                                {
                                        char include_path[MAX_QPATH];
                                        char full_path[MAX_QPATH];
                                        char basedir[MAX_QPATH];
                                        size_t include_len = (size_t) (end - ptr);
                                        char *slash;
                                        char *included;

                                        if (include_len >= sizeof (include_path))
                                        {
                                                free (result);
                                                free (source);
                                                Sys_Error ("GL_LoadShaderFile: include path too long in %s", path);
                                        }

                                        memcpy (include_path, ptr, include_len);
                                        include_path[include_len] = '\0';

                                        q_strlcpy (basedir, path, sizeof (basedir));
                                        slash = strrchr (basedir, '/');
#if defined(_WIN32)
                                        if (!slash)
                                                slash = strrchr (basedir, '\\');
#endif
                                        if (slash)
                                                slash[1] = '\0';
                                        else
                                                basedir[0] = '\0';

                                        q_strlcpy (full_path, basedir, sizeof (full_path));
                                        if (q_strlcat (full_path, include_path, sizeof (full_path)) >= sizeof (full_path))
                                        {
                                                free (result);
                                                free (source);
                                                Sys_Error ("GL_LoadShaderFile: include path overflow in %s", path);
                                        }

                                        included = GL_LoadShaderFile_Internal (full_path, depth + 1);
                                        if (included)
                                        {
                                                APPEND_STR (included, strlen (included));
                                                if (line_end && (result_len == 0 || result[result_len - 1] != '\n'))
                                                        APPEND_STR ("\n", 1);
                                        }

                                        cursor = line_end ? line_end + 1 : cursor + line_len;
                                        continue;
                                }
                        }
                }

                APPEND_STR (line_start, line_len);
                cursor = line_end ? line_end + 1 : cursor + line_len;
        }

#undef APPEND_STR

        free (source);

        shader_cache[shader_cache_count].data = result;
        q_strlcpy (shader_cache[shader_cache_count].path, path, sizeof (shader_cache[shader_cache_count].path));
        shader_cache_count++;

        return result;
}

static char *GL_LoadShaderFile (const char *path)
{
        return GL_LoadShaderFile_Internal (path, 0);
}

static GLuint GL_CreateProgramFromFiles (int count, const char **paths, const GLenum *types, const char *name, va_list argptr)
{
        char macros[1024];
        char eval[256];
        char *pipe;
        int i, realcount;
        GLuint shaders[2];
        GLuint program;

        if (count <= 0 || count > 2)
                Sys_Error ("GL_CreateProgramFromFiles: invalid source count (%d)", count);

        q_vsnprintf (eval, sizeof (eval), name, argptr);
	macros[0] = 0;

	pipe = strchr (name, '|');
	if (pipe) // parse symbol list and generate #defines
	{
		char *dst = macros;
		char *dstend = macros + sizeof (macros);
		char *src = eval + 1 + (pipe - name);

		while (*src == ' ')
			src++;

		while (*src)
		{
			char *srcend = src + 1;
			while (*srcend && *srcend != ';')
				srcend++;

			if (!AppendString (&dst, dstend, "#define ", 8) ||
				!AppendString (&dst, dstend, src, srcend - src) ||
				!AppendString (&dst, dstend, "\n", 1))
				Sys_Error ("GL_CreateProgram: symbol overflow for %s", eval);

			src = srcend;
			while (*src == ';' || *src == ' ')
				src++;
		}

		AppendString (&dst, dstend, "\n", 1);
	}

	name = eval;

	realcount = 0;
        for (i = 0; i < count; i++)
        {
                if (paths[i])
                {
                        char *source = GL_LoadShaderFile (paths[i]);
                        shaders[realcount] = GL_CreateShader (types[i], source, macros, name);
                        realcount++;
                }
        }

        program = GL_CreateProgramFromShaders (shaders, realcount, name, macros, paths[0], (count > 1) ? paths[1] : NULL);

        return program;
}

/*
====================
GL_CreateProgram

Compiles and returns GLSL program.
====================
*/
static FUNC_PRINTF(3,4) GLuint GL_CreateProgram (const char *vertPath, const char *fragPath, const char *name, ...)
{
        const char *paths[2] = {vertPath, fragPath};
        GLenum types[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
        va_list argptr;
        GLuint program;

        va_start (argptr, name);
        program = GL_CreateProgramFromFiles (2, paths, types, name, argptr);
        va_end (argptr);

        return program;
}

/*
====================
GL_CreateComputeProgram

Compiles and returns GLSL program.
====================
*/
static FUNC_PRINTF(2,3) GLuint GL_CreateComputeProgram (const char *path, const char *name, ...)
{
        GLenum type = GL_COMPUTE_SHADER;
        va_list argptr;
        GLuint program;

        va_start (argptr, name);
        program = GL_CreateProgramFromFiles (1, &path, &type, name, argptr);
        va_end (argptr);

        return program;
}

/*
====================
GL_UseProgram
====================
*/
void GL_UseProgram (GLuint program)
{
	if (program == gl_current_program)
		return;
	gl_current_program = program;
	GL_UseProgramFunc (program);
}

/*
====================
GL_ClearCachedProgram

This must be called if you do anything that could make the cached program
invalid (e.g. manually binding, destroying the context).
====================
*/
void GL_ClearCachedProgram (void)
{
	gl_current_program = 0;
	GL_UseProgramFunc (0);
}

GLuint GL_GetCurrentProgramCached (void)
{
	return gl_current_program;
}

const char *GL_GetProgramDebugName (GLuint program)
{
	return GL_LookupProgramName (program);
}

const char *GL_GetProgramDebugDefines (GLuint program)
{
	return GL_LookupProgramValue (program, gl_program_defines, "");
}

const char *GL_GetProgramVertexShaderPath (GLuint program)
{
	return GL_LookupProgramValue (program, gl_program_vert_paths, "none");
}

const char *GL_GetProgramFragmentShaderPath (GLuint program)
{
	return GL_LookupProgramValue (program, gl_program_frag_paths, "none");
}

/*
=============
GL_CreateShaders
=============
*/
void GL_CreateShaders (void)
{
	int palettize, dither, mode, alphatest, warp, oit, md5;

	R_Shadow_ResetUBOBindings ();

	glprogs.gui = GL_CreateProgram (GLSL_PATH("gui.vert"), GLSL_PATH("gui.frag"), "gui");
	glprogs.viewblend = GL_CreateProgram (GLSL_PATH("viewblend.vert"), GLSL_PATH("viewblend.frag"), "viewblend");
	for (warp = 0; warp < 2; warp++)
            glprogs.warpscale[warp] = GL_CreateProgram (GLSL_PATH("warpscale.vert"), GLSL_PATH("warpscale.frag"), "view warp/scale|WARP %d", warp);
	for (palettize = 0; palettize < 3; palettize++)
            glprogs.postprocess[palettize] = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("postprocess.frag"), "postprocess|PALETTIZE %d", palettize);
	glprogs.filmgrain = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("filmgrain.frag"), "filmgrain");

	glprogs.bloom_extract = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("bloom_extract.frag"), "bloom extract");
    glprogs.bloom_blur = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("bloom_blur.frag"), "bloom blur");
	glprogs.ssao = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("ssao.frag"), "ssao");
	glprogs.ssao_blur = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("ssao_blur.frag"), "ssao blur");
	glprogs.ao_assao_main = GL_CreateComputeProgram (GLSL_PATH("ao_assao_main.comp"), "ao assao main");
	glprogs.ao_gtao_prefilter = GL_CreateComputeProgram (GLSL_PATH("ao_gtao_prefilter.comp"), "ao gtao prefilter");
	glprogs.ao_gtao_main = GL_CreateComputeProgram (GLSL_PATH("ao_gtao_main.comp"), "ao gtao main");
	glprogs.ao_denoise = GL_CreateComputeProgram (GLSL_PATH("ao_denoise.comp"), "ao denoise");
	glprogs.ao_bent_pack = GL_CreateComputeProgram (GLSL_PATH("ao_bent_pack.comp"), "ao bent pack");
	glprogs.godrays_mask = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("godrays_mask.frag"), "godrays mask");
	glprogs.godrays = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("godrays.frag"), "godrays");
	glprogs.atmos_froxel_build = GL_CreateComputeProgram (GLSL_PATH("atmos_froxel_build.comp"), "atmos froxel build");
	glprogs.atmos_froxel_integrate = GL_CreateComputeProgram (GLSL_PATH("atmos_froxel_integrate.comp"), "atmos froxel integrate");
	glprogs.atmos_froxel_temporal = GL_CreateComputeProgram (GLSL_PATH("atmos_froxel_temporal.comp"), "atmos froxel temporal");
        for (mode = 0; mode < 2; mode++)
                glprogs.oit_resolve[mode] = GL_CreateProgram (GLSL_PATH("oit_resolve.vert"), GLSL_PATH("oit_resolve.frag"), "oit resolve|MSAA %d", mode);

        for (oit = 0; oit < 2; oit++)
                for (dither = 0; dither < 3; dither++)
                        for (mode = 0; mode < 3; mode++)
                                glprogs.world[oit][dither][mode] = GL_CreateProgram (GLSL_PATH("world.vert"), GLSL_PATH("world.frag"), "world|OIT %d; DITHER %d; MODE %d", oit, dither, mode);

        glprogs.shadow_depth = GL_CreateProgram (GLSL_PATH("shadow_depth.vert"), GLSL_PATH("shadow_depth.frag"), "shadow depth");
	for (md5 = 0; md5 < 2; md5++)
		glprogs.shadow_depth_alias[md5] = GL_CreateProgram (GLSL_PATH("shadow_depth_alias.vert"), GLSL_PATH("shadow_depth.frag"), "shadow depth alias|MD5 %d", md5);

	for (dither = 0; dither < 2; dither++)
	{
		for (oit = 0; oit < 2; oit++)
		{
                        glprogs.water[oit][dither] = GL_CreateProgram (GLSL_PATH("water.vert"), GLSL_PATH("water.frag"), "water|OIT %d; DITHER %d", oit, dither);
                        glprogs.particles[oit][dither] = GL_CreateProgram (GLSL_PATH("particles.vert"), GLSL_PATH("particles.frag"), "particles|OIT %d; DITHER %d", oit, dither);
		}
                for (mode = 0; mode < 2; mode++)
                        glprogs.skycubemap[mode][dither] = GL_CreateProgram (GLSL_PATH("sky_cubemap.vert"), GLSL_PATH("sky_cubemap.frag"), "sky cubemap|ANIM %d; DITHER %d", mode, dither);
                glprogs.skylayers[dither] = GL_CreateProgram (GLSL_PATH("sky_layers.vert"), GLSL_PATH("sky_layers.frag"), "sky layers|DITHER %d", dither);
                glprogs.skyboxside[dither] = GL_CreateProgram (GLSL_PATH("sky_boxside.vert"), GLSL_PATH("sky_boxside.frag"), "skybox side|DITHER %d", dither);
                glprogs.sprites[dither] = GL_CreateProgram (GLSL_PATH("sprites.vert"), GLSL_PATH("sprites.frag"), "sprites|DITHER %d", dither);
                glprogs.decals[dither] = GL_CreateProgram (GLSL_PATH("decals.vert"), GLSL_PATH("decals.frag"), "decals|DITHER %d", dither);
        }
        glprogs.skystencil = GL_CreateProgram (GLSL_PATH("skystencil.vert"), NULL, "sky stencil");

        for (oit = 0; oit < 2; oit++)
                for (mode = 0; mode < 3; mode++)
                        for (alphatest = 0; alphatest < 2; alphatest++)
                                for (md5 = 0; md5 < 2; md5++)
                                        glprogs.alias[oit][mode][alphatest][md5] =
                                                GL_CreateProgram (GLSL_PATH("alias.vert"), GLSL_PATH("alias.frag"), "alias|OIT %d; MODE %d; ALPHATEST %d; MD5 %d", oit, mode, alphatest, md5);

        glprogs.debug3d = GL_CreateProgram (GLSL_PATH("debug3d.vert"), GLSL_PATH("debug3d.frag"), "debug3d");
        glprogs.shadow_debug = GL_CreateProgram (GLSL_PATH("shadow_debug.vert"), GLSL_PATH("shadow_debug.frag"), "shadow debug");

	glprogs.clear_indirect = GL_CreateComputeProgram (GLSL_PATH("clear_indirect.comp"), "clear indirect draw params");
	glprogs.gather_indirect = GL_CreateComputeProgram (GLSL_PATH("gather_indirect.comp"), "indirect draw gather");
	glprogs.cull_mark = GL_CreateComputeProgram (GLSL_PATH("cull_mark.comp"), "cull/mark");
	glprogs.cluster_lights = GL_CreateComputeProgram (GLSL_PATH("cluster_lights.comp"), "light cluster");
	glprogs.cluster_prefix = GL_CreateComputeProgram (GLSL_PATH("cluster_prefix.comp"), "light cluster prefix");
	for (mode = 0; mode < 3; mode++)
		glprogs.palette_init[mode] = GL_CreateComputeProgram (GLSL_PATH("palette_init.comp"), "palette init|MODE %d", mode);
        glprogs.palette_postprocess = GL_CreateComputeProgram (GLSL_PATH("palette_postprocess.comp"), "palette postprocess");
}

/*
=============
GL_DeleteShaders
=============
*/
void GL_DeleteShaders (void)
{
        int i;
        for (i = 0; i < gl_num_programs; i++)
        {
                GL_DeleteProgramFunc (gl_programs[i]);
                gl_programs[i] = 0;
                free (gl_program_names[i]);
                gl_program_names[i] = NULL;
                free (gl_program_defines[i]);
                gl_program_defines[i] = NULL;
                free (gl_program_vert_paths[i]);
                gl_program_vert_paths[i] = NULL;
                free (gl_program_frag_paths[i]);
                gl_program_frag_paths[i] = NULL;
        }
        gl_num_programs = 0;

        GL_UseProgramFunc (0);
        gl_current_program = 0;

        memset (&glprogs, 0, sizeof(glprogs));

        for (i = 0; i < shader_cache_count; i++)
        {
                free (shader_cache[i].data);
                shader_cache[i].data = NULL;
                shader_cache[i].path[0] = '\0';
        }
        shader_cache_count = 0;
}
