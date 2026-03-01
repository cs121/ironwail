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

static qboolean GL_IsShaderIncludePathSafe (const char *path, const char **reason)
{
	const unsigned char *s = (const unsigned char *)path;

	if (!path || !*path)
	{
		if (reason)
			*reason = "empty path";
		return false;
	}

	if (path[0] == '/' || path[0] == '\\')
	{
		if (reason)
			*reason = "absolute paths are not allowed";
		return false;
	}

	if ((q_isalpha (s[0]) && path[1] == ':') || strstr (path, "://"))
	{
		if (reason)
			*reason = "device/protocol paths are not allowed";
		return false;
	}

	while (*s)
	{
		if (*s < 32)
		{
			if (reason)
				*reason = "control characters are not allowed";
			return false;
		}
		s++;
	}

	return true;
}

static qboolean GL_NormalizeShaderIncludePath (const char *basepath, const char *includepath, char *out, size_t outsize, const char **reason)
{
	char temp[MAX_QPATH];
	char basedir[MAX_QPATH];
	const char *src;
	char *dst;
	char *slash;

	if (!GL_IsShaderIncludePathSafe (includepath, reason))
		return false;

	q_strlcpy (basedir, basepath, sizeof (basedir));
	slash = strrchr (basedir, '/');
	if (!slash)
		slash = strrchr (basedir, '\\');
	if (slash)
		slash[1] = '\0';
	else
		basedir[0] = '\0';

	if (q_strlcpy (temp, basedir, sizeof (temp)) >= sizeof (temp)
		|| q_strlcat (temp, includepath, sizeof (temp)) >= sizeof (temp))
	{
		if (reason)
			*reason = "path too long";
		return false;
	}

	dst = out;
	src = temp;
	if (outsize == 0)
		return false;
	out[0] = '\0';

	while (*src)
	{
		char segment[MAX_QPATH];
		size_t seglen = 0;

		while (*src == '/' || *src == '\\')
			src++;
		if (!*src)
			break;

		while (*src && *src != '/' && *src != '\\')
		{
			if (seglen + 1 >= sizeof (segment))
			{
				if (reason)
					*reason = "path segment too long";
				return false;
			}
			segment[seglen++] = *src++;
		}
		segment[seglen] = '\0';

		if (!strcmp (segment, "."))
			continue;
		if (!strcmp (segment, ".."))
		{
			if (reason)
				*reason = "parent directory traversal is not allowed";
			return false;
		}

		if (dst != out)
		{
			if ((size_t)(dst - out + 1) >= outsize)
			{
				if (reason)
					*reason = "normalized path too long";
				return false;
			}
			*dst++ = '/';
		}

		if ((size_t)(dst - out + seglen) >= outsize)
		{
			if (reason)
				*reason = "normalized path too long";
			return false;
		}
		memcpy (dst, segment, seglen);
		dst += seglen;
		*dst = '\0';
	}

	if (strncmp (out, GLSL_PATH_PREFIX, strlen (GLSL_PATH_PREFIX)) != 0)
	{
		if (reason)
			*reason = "include resolved outside shaders/ root";
		return false;
	}

	if (!out[0])
	{
		if (reason)
			*reason = "empty normalized path";
		return false;
	}

	return true;
}

glprogs_t glprogs;
static GLuint gl_programs[128];
static GLuint gl_current_program;
static int gl_num_programs;

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

static qboolean GL_IsValidMacroIdentifier (const char *name, size_t len)
{
	size_t i;

	if (!len)
		return false;

	if (!(q_isalpha ((unsigned char)name[0]) || name[0] == '_'))
		return false;

	for (i = 1; i < len; i++)
	{
		if (!(q_isalnum ((unsigned char)name[i]) || name[i] == '_'))
			return false;
	}

	return true;
}

#ifndef NDEBUG
static void GL_DebugTestMacroIdentifierParser (void)
{
	static qboolean ran;

	if (ran)
		return;
	ran = true;

	assert (GL_IsValidMacroIdentifier ("FOO", 3));
	assert (GL_IsValidMacroIdentifier ("_BAR9", 5));
	assert (!GL_IsValidMacroIdentifier ("", 0));
	assert (!GL_IsValidMacroIdentifier ("9BAD", 4));
	assert (!GL_IsValidMacroIdentifier ("BAD-NAME", 8));
}
#endif

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
static GLuint GL_CreateProgramFromShaders (const GLuint *shaders, int numshaders, const char *name)
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
	gl_num_programs++;

	return program;
}

/*
====================
GL_CreateProgramFromSources
====================
*/
static char *GL_LoadShaderFile_Internal (const char *path, int depth, const char **include_stack, int include_stack_size)
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

        if (include_stack_size >= 32)
                Sys_Error ("GL_LoadShaderFile: include stack overflow for %s", path);

        include_stack[include_stack_size++] = path;

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
								size_t include_len = (size_t) (end - ptr);
								const char *reason = NULL;
								char *included;

                                        if (include_len >= sizeof (include_path))
                                        {
                                                free (result);
                                                free (source);
                                                Sys_Error ("GL_LoadShaderFile: include path too long in %s", path);
                                        }

                                        memcpy (include_path, ptr, include_len);
                                        include_path[include_len] = '\0';

								if (!GL_NormalizeShaderIncludePath (path, include_path, full_path, sizeof (full_path), &reason))
								{
									free (result);
									free (source);
									GL_InitError ("Shader include rejected in %s: '%s' (%s)",
										path, include_path, reason ? reason : "invalid path");
								}

                                        for (i = 0; i < include_stack_size; i++)
                                        {
                                                if (!strcmp (include_stack[i], full_path))
                                                {
                                                        char cycle[4096];
                                                        char *dst = cycle;
                                                        size_t remaining = sizeof (cycle);
                                                        int j;

                                                        cycle[0] = '\0';
                                                        for (j = i; j < include_stack_size; j++)
                                                        {
                                                                int written = q_snprintf (dst, remaining, "%s -> ", include_stack[j]);
                                                                if (written < 0 || (size_t) written >= remaining)
                                                                        break;
                                                                dst += written;
                                                                remaining -= (size_t) written;
                                                        }
                                                        if (remaining > 0)
                                                                q_snprintf (dst, remaining, "%s", full_path);

                                                        free (result);
                                                        free (source);
                                                        GL_InitError ("Shader include cycle detected: %s", cycle);
                                                }
                                        }

                                        included = GL_LoadShaderFile_Internal (full_path, depth + 1, include_stack, include_stack_size);
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
        const char *include_stack[32];
        return GL_LoadShaderFile_Internal (path, 0, include_stack, 0);
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

#ifndef NDEBUG
		GL_DebugTestMacroIdentifierParser ();
#endif

		while (*src == ' ')
			src++;

		while (*src)
		{
			char *srcend = src;
			char *tokstart, *tokend;
			char *namestart, *nameend;
			char *valuestart;
			while (*srcend && *srcend != ';')
				srcend++;

			tokstart = src;
			while (tokstart < srcend && q_isspace ((unsigned char)*tokstart))
				tokstart++;
			tokend = srcend;
			while (tokend > tokstart && q_isspace ((unsigned char)tokend[-1]))
				tokend--;

			if (tokstart == tokend)
				GL_InitError ("GL_CreateProgram '%s': empty macro token in '%s'", name, eval);

			namestart = tokstart;
			nameend = namestart;
			while (nameend < tokend && !q_isspace ((unsigned char)*nameend))
				nameend++;

			if (!GL_IsValidMacroIdentifier (namestart, nameend - namestart))
				GL_InitError ("GL_CreateProgram '%s': invalid macro identifier '%.*s' in '%s'",
					name, (int)(nameend - namestart), namestart, eval);

			valuestart = nameend;
			while (valuestart < tokend && q_isspace ((unsigned char)*valuestart))
				valuestart++;

			if (!AppendString (&dst, dstend, "#define ", 8) ||
				!AppendString (&dst, dstend, namestart, nameend - namestart) ||
				(valuestart < tokend && (!AppendString (&dst, dstend, " ", 1) ||
					!AppendString (&dst, dstend, valuestart, tokend - valuestart))) ||
				!AppendString (&dst, dstend, "\n", 1))
				Sys_Error ("GL_CreateProgram: symbol overflow for %s", eval);

			src = srcend;
			if (*src == ';')
			{
				src++;
				while (*src == ' ')
					src++;
				if (!*src)
					GL_InitError ("GL_CreateProgram '%s': empty trailing macro token in '%s'", name, eval);
			}
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

        program = GL_CreateProgramFromShaders (shaders, realcount, name);

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

GLuint GL_GetCurrentProgram (void)
{
	return gl_current_program;
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

/*
=============
GL_CreateShaders
=============
*/
void GL_CreateShaders (void)
{
	int palettize, dither, mode, alphatest, warp, oit, md5;

	glprogs.gui = GL_CreateProgram (GLSL_PATH("gui.vert"), GLSL_PATH("gui.frag"), "gui");
	glprogs.viewblend = GL_CreateProgram (GLSL_PATH("viewblend.vert"), GLSL_PATH("viewblend.frag"), "viewblend");
	for (warp = 0; warp < 2; warp++)
            glprogs.warpscale[warp] = GL_CreateProgram (GLSL_PATH("warpscale.vert"), GLSL_PATH("warpscale.frag"), "view warp/scale|WARP %d", warp);
	for (palettize = 0; palettize < 3; palettize++)
            glprogs.postprocess[palettize] = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("postprocess.frag"), "postprocess|PALETTIZE %d", palettize);
	glprogs.bloom_extract = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("bloom_extract.frag"), "bloom extract");
    glprogs.bloom_blur = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("bloom_blur.frag"), "bloom blur");
	glprogs.ssao = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("ssao.frag"), "ssao");
	glprogs.ssao_blur = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("ssao_blur.frag"), "ssao blur");
	glprogs.godrays_mask = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("godrays_mask.frag"), "godrays mask");
	glprogs.godrays = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("godrays.frag"), "godrays");
	glprogs.godrays_source = GL_CreateProgram (GLSL_PATH("world.vert"), GLSL_PATH("godrays_source.frag"), "world godrays|MODE 0; DITHER 0");
	glprogs.godrays_source_sky = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("godrays_source_sky.frag"), "godrays source sky");
	glprogs.fogvol = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("fogvol.frag"), "fog volumes");
	glprogs.fogvol_upsample = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("fogvol_upsample.frag"), "fog volumes upsample");
	glprogs.fogvol_temporal = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("fogvol_temporal.frag"), "fog volumes temporal");
        for (mode = 0; mode < 2; mode++)
                glprogs.oit_resolve[mode] = GL_CreateProgram (GLSL_PATH("oit_resolve.vert"), GLSL_PATH("oit_resolve.frag"), "oit resolve|MSAA %d", mode);

        for (oit = 0; oit < 2; oit++)
                for (dither = 0; dither < 3; dither++)
                        for (mode = 0; mode < 3; mode++)
                                glprogs.world[oit][dither][mode] = GL_CreateProgram (GLSL_PATH("world.vert"), GLSL_PATH("world.frag"), "world|OIT %d; DITHER %d; MODE %d", oit, dither, mode);

        for (alphatest = 0; alphatest < 2; alphatest++)
                glprogs.world_dlight[alphatest] = GL_CreateProgram (GLSL_PATH("world_dlight.vert"), GLSL_PATH("world_dlight.frag"), "world dlight|ALPHATEST %d", alphatest);

	glprogs.dlight_composite = GL_CreateProgram (GLSL_PATH("postprocess.vert"), GLSL_PATH("dlight_composite.frag"), "dlight composite");

	for (dither = 0; dither < 2; dither++)
	{
		for (oit = 0; oit < 2; oit++)
		{
                        glprogs.water[oit][dither] = GL_CreateProgram (GLSL_PATH("water.vert"), GLSL_PATH("water.frag"), "water|OIT %d; DITHER %d", oit, dither);
                        glprogs.teleport[oit][dither] = GL_CreateProgram (GLSL_PATH("water.vert"), GLSL_PATH("glsl/teleport_fbm.frag"), "teleport fbm|OIT %d; DITHER %d", oit, dither);
                        glprogs.particles[oit][dither] = GL_CreateProgram (GLSL_PATH("particles.vert"), GLSL_PATH("particles.frag"), "particles|OIT %d; DITHER %d", oit, dither);
		}
                for (mode = 0; mode < 2; mode++)
                        glprogs.skycubemap[mode][dither] = GL_CreateProgram (GLSL_PATH("sky_cubemap.vert"), GLSL_PATH("sky_cubemap.frag"), "sky cubemap|ANIM %d; DITHER %d", mode, dither);
                glprogs.skylayers[dither] = GL_CreateProgram (GLSL_PATH("sky_layers.vert"), GLSL_PATH("sky_layers.frag"), "sky layers|DITHER %d", dither);
                glprogs.skyboxside[dither] = GL_CreateProgram (GLSL_PATH("sky_boxside.vert"), GLSL_PATH("sky_boxside.frag"), "skybox side|DITHER %d", dither);
                glprogs.sprites[dither] = GL_CreateProgram (GLSL_PATH("sprites.vert"), GLSL_PATH("sprites.frag"), "sprites|DITHER %d", dither);
        glprogs.decal = GL_CreateProgram (GLSL_PATH("decal.vert"), GLSL_PATH("decal.frag"), "decal");
        }
        glprogs.skystencil = GL_CreateProgram (GLSL_PATH("skystencil.vert"), NULL, "sky stencil");

        for (oit = 0; oit < 2; oit++)
                for (mode = 0; mode < 3; mode++)
                        for (alphatest = 0; alphatest < 2; alphatest++)
                                for (md5 = 0; md5 < 2; md5++)
                                        glprogs.alias[oit][mode][alphatest][md5] =
                                                GL_CreateProgram (GLSL_PATH("alias.vert"), GLSL_PATH("alias.frag"), "alias|OIT %d; MODE %d; ALPHATEST %d; MD5 %d", oit, mode, alphatest, md5);

        glprogs.debug3d = GL_CreateProgram (GLSL_PATH("debug3d.vert"), GLSL_PATH("debug3d.frag"), "debug3d");

        glprogs.clear_indirect = GL_CreateComputeProgram (GLSL_PATH("clear_indirect.comp"), "clear indirect draw params");
        glprogs.gather_indirect = GL_CreateComputeProgram (GLSL_PATH("gather_indirect.comp"), "indirect draw gather");
        glprogs.cull_mark = GL_CreateComputeProgram (GLSL_PATH("cull_mark.comp"), "cull/mark");
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
