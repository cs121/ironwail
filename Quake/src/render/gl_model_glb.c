/*
Copyright (C) 2026 Ironwail contributors

Phase 1 GLB support: static mesh loading only.
Loads .glb files as non-animated models using the alias rendering pipeline.

Uses inline minimal glTF JSON parsing -- no third-party dependencies.
Only supports: positions, normals, UV0, indices, baseColor texture/factor.
*/

#include "quakedef.h"
#include "glquake.h"
#include "gl_model_glb.h"
#include "image.h"

extern cvar_t r_glb_debug;

static void GLB_DebugPrintf (const char *fmt, ...)
{
	va_list argptr;
	char message[2048];
	if (!r_glb_debug.value)
		return;
	va_start (argptr, fmt);
	q_vsnprintf (message, sizeof (message), fmt, argptr);
	va_end (argptr);
	Con_Printf ("%s", message);
}

#define GLB_MAGIC 0x46546C67u
#define GLB_JSON_CHUNK 0x4E4F534Au
#define GLB_BIN_CHUNK 0x004E4942u

#define GLTF_COMPONENT_FLOAT 5126
#define GLTF_COMPONENT_UNSIGNED_SHORT 5123
#define GLTF_COMPONENT_UNSIGNED_INT 5125
#define GLTF_COMPONENT_UNSIGNED_BYTE 5121

#define GLTF_TYPE_SCALAR 1
#define GLTF_TYPE_VEC2 2
#define GLTF_TYPE_VEC3 3
#define GLTF_TYPE_VEC4 4

#define GLTF_PRIMITIVE_TRIANGLES 4

typedef struct glb_accessor_s
{
	int buffer_view;
	int byte_offset;
	int component_type;
	int count;
	int type;
	qboolean sparse;
} glb_accessor_t;

typedef struct glb_bufferview_s
{
	int buffer;
	int byte_offset;
	int byte_length;
	int byte_stride;
} glb_bufferview_t;

typedef struct glb_primitive_s
{
	int mesh_index;
	int position_accessor;
	int normal_accessor;
	int uv0_accessor;
	int color0_accessor;
	int indices_accessor;
	int material_index;
	int mode;
} glb_primitive_t;

typedef struct glb_material_s
{
	float base_color_factor[4];
	int base_color_texture_index;
	int metallic_roughness_texture_index;
	int normal_texture_index;
	int occlusion_texture_index;
	int emissive_texture_index;
	float metallic_factor;
	float roughness_factor;
	float emissive_factor[3];
	int alpha_mode;
	float alpha_cutoff;
	qboolean double_sided;
	int has_texture;
	int has_emissive_texture;
} glb_material_t;

typedef struct glb_texture_s
{
	int source_index;
} glb_texture_t;

typedef struct glb_image_s
{
	char uri[256];
	int buffer_view;
	char mime_type[64];
	int has_uri;
	int has_buffer_view;
} glb_image_t;

typedef struct glb_buffer_s
{
	char uri[MAX_QPATH];
	int byte_length;
	const byte *data;
	int size;
	qboolean owned;
} glb_buffer_t;

#define MAX_GLB_ACCESSORS 256
#define MAX_GLB_BUFFERVIEWS 256
#define MAX_GLB_PRIMITIVES 256
#define MAX_GLB_MATERIALS 64
#define MAX_GLB_TEXTURES 64
#define MAX_GLB_IMAGES 64
#define MAX_GLB_BUFFERS 16

typedef struct glb_data_s
{
	glb_accessor_t accessors[MAX_GLB_ACCESSORS];
	int num_accessors;

	glb_bufferview_t bufferviews[MAX_GLB_BUFFERVIEWS];
	int num_bufferviews;

	glb_primitive_t primitives[MAX_GLB_PRIMITIVES];
	int num_primitives;

	glb_material_t materials[MAX_GLB_MATERIALS];
	int num_materials;

	glb_texture_t textures[MAX_GLB_TEXTURES];
	int num_textures;

	glb_image_t images[MAX_GLB_IMAGES];
	int num_images;

	glb_buffer_t buffers[MAX_GLB_BUFFERS];
	int num_buffers;

	const byte *bin_data;
	int bin_size;

	int num_meshes;
	int num_skins;
	int num_animations;
} glb_data_t;

static const byte *glb_json;
static int glb_json_size;

static int glb_json_pos;
static qboolean glb_json_error;

static void glb_json_skip_whitespace (void)
{
	while (glb_json_pos < glb_json_size)
	{
		char c = (char)glb_json[glb_json_pos];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			glb_json_pos++;
		else
			break;
	}
}

static qboolean glb_json_match_char (char c)
{
	glb_json_skip_whitespace ();
	if (glb_json_pos < glb_json_size && (char)glb_json[glb_json_pos] == c)
	{
		glb_json_pos++;
		return true;
	}
	return false;
}

static void glb_json_expect_char (char c)
{
	if (!glb_json_match_char (c))
	{
		glb_json_error = true;
		Con_Warning ("GLB JSON: expected '%c' at pos %d\n", c, glb_json_pos);
	}
}

static void glb_json_skip_string (void)
{
	if (glb_json_pos < glb_json_size && (char)glb_json[glb_json_pos] == '"')
		glb_json_pos++;
	while (glb_json_pos < glb_json_size)
	{
		char c = (char)glb_json[glb_json_pos];
		glb_json_pos++;
		if (c == '"' || c == '\0')
			break;
		if (c == '\\')
		{
			if (glb_json_pos < glb_json_size)
				glb_json_pos++;
		}
	}
}

static double glb_json_parse_number (void)
{
	char buf[64];
	int len = 0;
	glb_json_skip_whitespace ();
	while (glb_json_pos < glb_json_size && len < 63)
	{
		char c = (char)glb_json[glb_json_pos];
		if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
		{
			buf[len++] = c;
			glb_json_pos++;
		}
		else
			break;
	}
	buf[len] = '\0';
	if (!len)
	{
		glb_json_error = true;
		return 0.0;
	}
	return atof (buf);
}

static int glb_json_parse_int (void)
{
	return (int)glb_json_parse_number ();
}

static void glb_json_parse_string_buf (char *out, int maxsize)
{
	glb_json_skip_whitespace ();
	out[0] = '\0';
	if (glb_json_pos >= glb_json_size || (char)glb_json[glb_json_pos] != '"')
		return;
	glb_json_pos++;
	int len = 0;
	while (glb_json_pos < glb_json_size && len < maxsize - 1)
	{
		char c = (char)glb_json[glb_json_pos];
		if (c == '"')
		{
			glb_json_pos++;
			break;
		}
		if (c == '\\')
		{
			glb_json_pos++;
			if (glb_json_pos >= glb_json_size)
				break;
			c = (char)glb_json[glb_json_pos];
		}
		out[len++] = c;
		glb_json_pos++;
	}
	out[len] = '\0';
}

static void glb_json_skip_value (void)
{
	glb_json_skip_whitespace ();
	if (glb_json_pos >= glb_json_size)
		return;
	char c = (char)glb_json[glb_json_pos];
	if (c == '"')
	{
		glb_json_skip_string ();
	}
	else if (c == '{')
	{
		glb_json_pos++;
		int depth = 1;
		while (glb_json_pos < glb_json_size && depth > 0)
		{
			c = (char)glb_json[glb_json_pos];
			if (c == '"')
				glb_json_skip_string ();
			else
			{
				if (c == '{') depth++;
				else if (c == '}') depth--;
				glb_json_pos++;
			}
		}
	}
	else if (c == '[')
	{
		glb_json_pos++;
		int depth = 1;
		while (glb_json_pos < glb_json_size && depth > 0)
		{
			c = (char)glb_json[glb_json_pos];
			if (c == '"')
				glb_json_skip_string ();
			else
			{
				if (c == '[') depth++;
				else if (c == ']') depth--;
				glb_json_pos++;
			}
		}
	}
	else
	{
		while (glb_json_pos < glb_json_size)
		{
			c = (char)glb_json[glb_json_pos];
			if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\n' || c == '\r' || c == '\t')
				break;
			glb_json_pos++;
		}
	}
}

static qboolean glb_json_read_key (const char *expected)
{
	char key[256];
	glb_json_skip_whitespace ();
	glb_json_parse_string_buf (key, sizeof (key));
	if (strcmp (key, expected) != 0)
		return false;
	glb_json_expect_char (':');
	return true;
}

static qboolean glb_json_peek_key (char *out, int maxsize)
{
	int saved = glb_json_pos;
	glb_json_skip_whitespace ();
	glb_json_parse_string_buf (out, maxsize);
	glb_json_pos = saved;
	return true;
}

static qboolean glb_json_parse_object_key (char *out, int maxsize)
{
	glb_json_skip_whitespace ();
	if (glb_json_pos >= glb_json_size || (char)glb_json[glb_json_pos] != '"')
	{
		glb_json_error = true;
		return false;
	}
	glb_json_parse_string_buf (out, maxsize);
	glb_json_expect_char (':');
	return !glb_json_error;
}

static int glb_parse_component_size (int component_type)
{
	switch (component_type)
	{
	case GLTF_COMPONENT_UNSIGNED_BYTE: return 1;
	case GLTF_COMPONENT_UNSIGNED_SHORT: return 2;
	case GLTF_COMPONENT_UNSIGNED_INT: return 4;
	case GLTF_COMPONENT_FLOAT: return 4;
	default: return 0;
	}
}

static void glb_parse_accessor (glb_data_t *data)
{
	glb_accessor_t *a = &data->accessors[data->num_accessors];
	memset (a, 0, sizeof (*a));
	a->buffer_view = -1;
	a->byte_offset = 0;
	a->component_type = 0;
	a->count = 0;
	a->type = 0;

	glb_json_expect_char ('{');
	while (!glb_json_error && !glb_json_match_char ('}'))
	{
		char key[64];
		if (!glb_json_parse_object_key (key, sizeof (key)))
			break;
		if (!strcmp (key, "bufferView"))
			a->buffer_view = glb_json_parse_int ();
		else if (!strcmp (key, "byteOffset"))
			a->byte_offset = glb_json_parse_int ();
		else if (!strcmp (key, "componentType"))
			a->component_type = glb_json_parse_int ();
		else if (!strcmp (key, "count"))
			a->count = glb_json_parse_int ();
		else if (!strcmp (key, "type"))
		{
			char typestr[32];
			glb_json_parse_string_buf (typestr, sizeof (typestr));
			if (!strcmp (typestr, "SCALAR")) a->type = GLTF_TYPE_SCALAR;
			else if (!strcmp (typestr, "VEC2")) a->type = GLTF_TYPE_VEC2;
			else if (!strcmp (typestr, "VEC3")) a->type = GLTF_TYPE_VEC3;
			else if (!strcmp (typestr, "VEC4")) a->type = GLTF_TYPE_VEC4;
		}
		else if (!strcmp (key, "sparse"))
			a->sparse = true;
		else
			glb_json_skip_value ();

		glb_json_match_char (',');
	}
}

static void glb_parse_bufferview (glb_data_t *data)
{
	glb_bufferview_t *bv = &data->bufferviews[data->num_bufferviews];
	memset (bv, 0, sizeof (*bv));
	bv->buffer = 0;
	bv->byte_offset = 0;
	bv->byte_length = 0;
	bv->byte_stride = 0;

	glb_json_expect_char ('{');
	while (!glb_json_error && !glb_json_match_char ('}'))
	{
		char key[64];
		if (!glb_json_parse_object_key (key, sizeof (key)))
			break;
		if (!strcmp (key, "buffer"))
			bv->buffer = glb_json_parse_int ();
		else if (!strcmp (key, "byteOffset"))
			bv->byte_offset = glb_json_parse_int ();
		else if (!strcmp (key, "byteLength"))
			bv->byte_length = glb_json_parse_int ();
		else if (!strcmp (key, "byteStride"))
			bv->byte_stride = glb_json_parse_int ();
		else
			glb_json_skip_value ();

		glb_json_match_char (',');
	}
}

static void glb_parse_primitive (glb_data_t *data)
{
	glb_primitive_t *p = &data->primitives[data->num_primitives];
	memset (p, 0, sizeof (*p));
	p->position_accessor = -1;
	p->normal_accessor = -1;
	p->uv0_accessor = -1;
	p->color0_accessor = -1;
	p->indices_accessor = -1;
	p->material_index = -1;
	p->mode = GLTF_PRIMITIVE_TRIANGLES;

	glb_json_expect_char ('{');
	while (!glb_json_error && !glb_json_match_char ('}'))
	{
		char key[64];
		if (!glb_json_parse_object_key (key, sizeof (key)))
			break;

		if (!strcmp (key, "attributes"))
		{
			glb_json_expect_char ('{');
			while (!glb_json_error && !glb_json_match_char ('}'))
			{
				char attr[64];
				if (!glb_json_parse_object_key (attr, sizeof (attr)))
					break;
				int idx = glb_json_parse_int ();
				if (!strcmp (attr, "POSITION")) p->position_accessor = idx;
				else if (!strcmp (attr, "NORMAL")) p->normal_accessor = idx;
				else if (!strcmp (attr, "TEXCOORD_0")) p->uv0_accessor = idx;
				else if (!strcmp (attr, "COLOR_0")) p->color0_accessor = idx;
				glb_json_match_char (',');
			}
		}
		else if (!strcmp (key, "indices"))
			p->indices_accessor = glb_json_parse_int ();
		else if (!strcmp (key, "material"))
			p->material_index = glb_json_parse_int ();
		else if (!strcmp (key, "mode"))
			p->mode = glb_json_parse_int ();
		else
			glb_json_skip_value ();

		glb_json_match_char (',');
	}
}

static int glb_parse_texture_info (void)
{
	int index = -1;
	glb_json_expect_char ('{');
	while (!glb_json_error && !glb_json_match_char ('}'))
	{
		char key[64];
		if (!glb_json_parse_object_key (key, sizeof (key)))
			break;
		if (!strcmp (key, "index"))
			index = glb_json_parse_int ();
		else
			glb_json_skip_value ();
		glb_json_match_char (',');
	}
	return index;
}

static void glb_parse_pbr (glb_material_t *mat)
{
	glb_json_expect_char ('{');
	while (!glb_json_error && !glb_json_match_char ('}'))
	{
		char key[64];
		if (!glb_json_parse_object_key (key, sizeof (key)))
			break;
		if (!strcmp (key, "baseColorFactor"))
		{
			glb_json_expect_char ('[');
			mat->base_color_factor[0] = (float)glb_json_parse_number ();
			glb_json_expect_char (',');
			mat->base_color_factor[1] = (float)glb_json_parse_number ();
			glb_json_expect_char (',');
			mat->base_color_factor[2] = (float)glb_json_parse_number ();
			glb_json_expect_char (',');
			mat->base_color_factor[3] = (float)glb_json_parse_number ();
			glb_json_match_char (']');
		}
		else if (!strcmp (key, "baseColorTexture"))
		{
			mat->base_color_texture_index = glb_parse_texture_info ();
			mat->has_texture = (mat->base_color_texture_index >= 0);
		}
		else if (!strcmp (key, "metallicRoughnessTexture"))
			mat->metallic_roughness_texture_index = glb_parse_texture_info ();
		else if (!strcmp (key, "metallicFactor"))
			mat->metallic_factor = (float)glb_json_parse_number ();
		else if (!strcmp (key, "roughnessFactor"))
			mat->roughness_factor = (float)glb_json_parse_number ();
		else
			glb_json_skip_value ();

		glb_json_match_char (',');
	}
}

static void glb_parse_material (glb_data_t *data)
{
	glb_material_t *mat = &data->materials[data->num_materials];
	memset (mat, 0, sizeof (*mat));
	mat->base_color_factor[0] = 1.f;
	mat->base_color_factor[1] = 1.f;
	mat->base_color_factor[2] = 1.f;
	mat->base_color_factor[3] = 1.f;
	mat->base_color_texture_index = -1;
	mat->metallic_roughness_texture_index = -1;
	mat->normal_texture_index = -1;
	mat->occlusion_texture_index = -1;
	mat->emissive_texture_index = -1;
	mat->metallic_factor = 1.f;
	mat->roughness_factor = 1.f;
	mat->emissive_factor[0] = 0.f;
	mat->emissive_factor[1] = 0.f;
	mat->emissive_factor[2] = 0.f;
	mat->alpha_mode = 0;
	mat->alpha_cutoff = 0.5f;

	glb_json_expect_char ('{');
	while (!glb_json_error && !glb_json_match_char ('}'))
	{
		char key[64];
		if (!glb_json_parse_object_key (key, sizeof (key)))
			break;
		if (!strcmp (key, "pbrMetallicRoughness"))
			glb_parse_pbr (mat);
		else if (!strcmp (key, "normalTexture"))
			mat->normal_texture_index = glb_parse_texture_info ();
		else if (!strcmp (key, "occlusionTexture"))
			mat->occlusion_texture_index = glb_parse_texture_info ();
		else if (!strcmp (key, "emissiveTexture"))
		{
			mat->emissive_texture_index = glb_parse_texture_info ();
			mat->has_emissive_texture = (mat->emissive_texture_index >= 0);
		}
		else if (!strcmp (key, "emissiveFactor"))
		{
			glb_json_expect_char ('[');
			mat->emissive_factor[0] = (float)glb_json_parse_number ();
			glb_json_expect_char (',');
			mat->emissive_factor[1] = (float)glb_json_parse_number ();
			glb_json_expect_char (',');
			mat->emissive_factor[2] = (float)glb_json_parse_number ();
			glb_json_match_char (']');
		}
		else if (!strcmp (key, "alphaMode"))
		{
			char mode[32];
			glb_json_parse_string_buf (mode, sizeof (mode));
			mat->alpha_mode = !strcmp (mode, "MASK") ? 1 : (!strcmp (mode, "BLEND") ? 2 : 0);
		}
		else if (!strcmp (key, "alphaCutoff"))
			mat->alpha_cutoff = (float)glb_json_parse_number ();
		else if (!strcmp (key, "doubleSided"))
		{
			char v[8];
			glb_json_parse_string_buf (v, sizeof (v));
			if (!v[0])
			{
				glb_json_skip_whitespace ();
				mat->double_sided = glb_json_pos + 4 <= glb_json_size && !memcmp (glb_json + glb_json_pos, "true", 4);
				glb_json_skip_value ();
			}
		}
		else
			glb_json_skip_value ();

		glb_json_match_char (',');
	}
}

static void glb_parse_texture (glb_data_t *data)
{
	glb_texture_t *tex = &data->textures[data->num_textures];
	memset (tex, 0, sizeof (*tex));
	tex->source_index = -1;

	glb_json_expect_char ('{');
	while (!glb_json_error && !glb_json_match_char ('}'))
	{
		char key[64];
		if (!glb_json_parse_object_key (key, sizeof (key)))
			break;
		if (!strcmp (key, "source"))
			tex->source_index = glb_json_parse_int ();
		else
			glb_json_skip_value ();

		glb_json_match_char (',');
	}
}

static void glb_parse_image (glb_data_t *data)
{
	glb_image_t *img = &data->images[data->num_images];
	memset (img, 0, sizeof (*img));
	img->buffer_view = -1;

	glb_json_expect_char ('{');
	while (!glb_json_error && !glb_json_match_char ('}'))
	{
		char key[64];
		if (!glb_json_parse_object_key (key, sizeof (key)))
			break;
		if (!strcmp (key, "uri"))
		{
			glb_json_parse_string_buf (img->uri, sizeof (img->uri));
			img->has_uri = 1;
		}
		else if (!strcmp (key, "bufferView"))
		{
			img->buffer_view = glb_json_parse_int ();
			img->has_buffer_view = 1;
		}
		else if (!strcmp (key, "mimeType"))
			glb_json_parse_string_buf (img->mime_type, sizeof (img->mime_type));
		else
			glb_json_skip_value ();

		glb_json_match_char (',');
	}
}


static void glb_parse_buffer (glb_data_t *data)
{
	glb_buffer_t *buffer = &data->buffers[data->num_buffers];
	memset (buffer, 0, sizeof (*buffer));

	glb_json_expect_char ('{');
	while (!glb_json_error && !glb_json_match_char ('}'))
	{
		char key[64];
		if (!glb_json_parse_object_key (key, sizeof (key)))
			break;
		if (!strcmp (key, "uri"))
			glb_json_parse_string_buf (buffer->uri, sizeof (buffer->uri));
		else if (!strcmp (key, "byteLength"))
			buffer->byte_length = glb_json_parse_int ();
		else
			glb_json_skip_value ();
		glb_json_match_char (',');
	}
}

static qboolean glb_parse_json (glb_data_t *data)
{
	glb_json_expect_char ('{');
	while (!glb_json_error && !glb_json_match_char ('}'))
	{
		char key[64];
		if (!glb_json_parse_object_key (key, sizeof (key)))
			break;

		if (!strcmp (key, "buffers"))
		{
			glb_json_expect_char ('[');
			while (!glb_json_error && !glb_json_match_char (']'))
			{
				if (data->num_buffers < MAX_GLB_BUFFERS)
				{
					glb_parse_buffer (data);
					data->num_buffers++;
				}
				else
					glb_json_skip_value ();
				glb_json_match_char (',');
			}
		}
		else if (!strcmp (key, "meshes"))
		{
			glb_json_expect_char ('[');
			data->num_meshes = 0;
			while (!glb_json_error && !glb_json_match_char (']'))
			{
				int mesh_index = data->num_meshes++;
				glb_json_expect_char ('{');
				while (!glb_json_error && !glb_json_match_char ('}'))
				{
					char mk[64];
					if (!glb_json_parse_object_key (mk, sizeof (mk)))
						break;
					if (!strcmp (mk, "primitives"))
					{
						glb_json_expect_char ('[');
						while (!glb_json_error && !glb_json_match_char (']'))
						{
							if (data->num_primitives < MAX_GLB_PRIMITIVES)
							{
								glb_parse_primitive (data);
								data->primitives[data->num_primitives].mesh_index = mesh_index;
								data->num_primitives++;
							}
							else
							{
								glb_json_skip_value ();
								Con_Warning ("GLB: too many primitives, max %d\n", MAX_GLB_PRIMITIVES);
							}
							glb_json_match_char (',');
						}
					}
					else
						glb_json_skip_value ();

					glb_json_match_char (',');
				}
				glb_json_match_char (',');
			}
		}
		else if (!strcmp (key, "accessors"))
		{
			glb_json_expect_char ('[');
			while (!glb_json_error && !glb_json_match_char (']'))
			{
				if (data->num_accessors < MAX_GLB_ACCESSORS)
				{
					glb_parse_accessor (data);
					data->num_accessors++;
				}
				else
				{
					glb_json_skip_value ();
					Con_Warning ("GLB: too many accessors, max %d\n", MAX_GLB_ACCESSORS);
				}
				glb_json_match_char (',');
			}
		}
		else if (!strcmp (key, "bufferViews"))
		{
			glb_json_expect_char ('[');
			while (!glb_json_error && !glb_json_match_char (']'))
			{
				if (data->num_bufferviews < MAX_GLB_BUFFERVIEWS)
				{
					glb_parse_bufferview (data);
					data->num_bufferviews++;
				}
				else
				{
					glb_json_skip_value ();
					Con_Warning ("GLB: too many bufferViews, max %d\n", MAX_GLB_BUFFERVIEWS);
				}
				glb_json_match_char (',');
			}
		}
		else if (!strcmp (key, "materials"))
		{
			glb_json_expect_char ('[');
			while (!glb_json_error && !glb_json_match_char (']'))
			{
				if (data->num_materials < MAX_GLB_MATERIALS)
				{
					glb_parse_material (data);
					data->num_materials++;
				}
				else
				{
					glb_json_skip_value ();
					Con_Warning ("GLB: too many materials, max %d\n", MAX_GLB_MATERIALS);
				}
				glb_json_match_char (',');
			}
		}
		else if (!strcmp (key, "textures"))
		{
			glb_json_expect_char ('[');
			while (!glb_json_error && !glb_json_match_char (']'))
			{
				if (data->num_textures < MAX_GLB_TEXTURES)
				{
					glb_parse_texture (data);
					data->num_textures++;
				}
				else
					glb_json_skip_value ();
				glb_json_match_char (',');
			}
		}
		else if (!strcmp (key, "images"))
		{
			glb_json_expect_char ('[');
			while (!glb_json_error && !glb_json_match_char (']'))
			{
				if (data->num_images < MAX_GLB_IMAGES)
				{
					glb_parse_image (data);
					data->num_images++;
				}
				else
					glb_json_skip_value ();
				glb_json_match_char (',');
			}
		}
		else if (!strcmp (key, "skins"))
		{
			data->num_skins = 0;
			glb_json_expect_char ('[');
			while (!glb_json_error && !glb_json_match_char (']'))
			{
				data->num_skins++;
				glb_json_skip_value ();
				glb_json_match_char (',');
			}
		}
		else if (!strcmp (key, "animations"))
		{
			data->num_animations = 0;
			glb_json_expect_char ('[');
			while (!glb_json_error && !glb_json_match_char (']'))
			{
				data->num_animations++;
				glb_json_skip_value ();
				glb_json_match_char (',');
			}
		}
		else
			glb_json_skip_value ();

		glb_json_match_char (',');
	}

	return !glb_json_error;
}

static const byte *glb_get_accessor_data (const glb_data_t *data, const glb_accessor_t *acc, int *out_stride)
{
	long long total;
	int stride;
	int elem_size;

	if (acc->count <= 0 || acc->type <= 0)
		return NULL;
	if (acc->buffer_view < 0 || acc->buffer_view >= data->num_bufferviews)
		return NULL;
	const glb_bufferview_t *bv = &data->bufferviews[acc->buffer_view];
	const byte *buffer_data;
	int buffer_size;
	if (bv->buffer < 0 || bv->buffer >= data->num_buffers)
		return NULL;
	buffer_data = data->buffers[bv->buffer].data;
	buffer_size = data->buffers[bv->buffer].size;
	if (!buffer_data || buffer_size <= 0)
		return NULL;
	int comp_size = glb_parse_component_size (acc->component_type);
	if (!comp_size)
		return NULL;
	elem_size = comp_size * acc->type;
	if (elem_size <= 0)
		return NULL;
	stride = (bv->byte_stride > 0) ? bv->byte_stride : elem_size;
	if (stride < elem_size)
		return NULL;
	if (bv->byte_offset < 0 || acc->byte_offset < 0 || bv->byte_length < 0)
		return NULL;
	total = (long long)bv->byte_offset + (long long)acc->byte_offset;
	if (total < 0 || total >= buffer_size)
		return NULL;
	if (acc->count > 0)
	{
		long long last_end = total + (long long)(acc->count - 1) * stride + elem_size;
		if (last_end > buffer_size)
			return NULL;
		if ((long long)acc->byte_offset + (long long)(acc->count - 1) * stride + elem_size > bv->byte_length)
			return NULL;
	}
	if (out_stride)
	{
		*out_stride = stride;
	}
	return buffer_data + (int)total;
}

static qboolean glb_uri_is_safe (const char *uri)
{
	if (!uri || !uri[0])
		return false;
	if (uri[0] == '/' || uri[0] == '\\')
		return false;
	if (strstr (uri, "..") || strchr (uri, ':') || strchr (uri, '\\'))
		return false;
	return true;
}

static void glb_build_relative_path (const char *model_name, const char *uri, char *out, size_t outsize)
{
	char modelpath[MAX_QPATH];
	const char *slash;

	COM_StripExtension (model_name, modelpath, sizeof (modelpath));
	slash = strrchr (modelpath, '/');
	if (slash)
	{
		int dirlen = (int)(slash - modelpath + 1);
		q_snprintf (out, outsize, "%.*s%s", dirlen, modelpath, uri);
	}
	else
		q_snprintf (out, outsize, "%s", uri);
}

static void glb_free_owned_buffers (glb_data_t *data)
{
	int i;
	for (i = 0; i < data->num_buffers; i++)
	{
		if (data->buffers[i].owned && data->buffers[i].data)
			q_free ((void *)data->buffers[i].data);
		data->buffers[i].data = NULL;
		data->buffers[i].size = 0;
		data->buffers[i].owned = false;
	}
}

static qboolean glb_load_external_buffers (qmodel_t *mod, glb_data_t *data)
{
	int i;
	for (i = 0; i < data->num_buffers; i++)
	{
		glb_buffer_t *buffer = &data->buffers[i];
		if (buffer->data)
			continue;
		if (!buffer->uri[0])
		{
			Con_Warning ("GLB: buffer %d has no binary payload in %s\n", i, mod->name);
			return false;
		}
		if (!glb_uri_is_safe (buffer->uri))
		{
			Con_Warning ("GLB: unsafe buffer uri '%s' ignored for %s\n", buffer->uri, mod->name);
			return false;
		}
		if (!strncmp (buffer->uri, "data:", 5))
		{
			Con_Warning ("GLB: data URI buffers are not supported for %s\n", mod->name);
			return false;
		}
		{
			char path[MAX_QPATH];
			byte *loaded;
			glb_build_relative_path (mod->name, buffer->uri, path, sizeof (path));
			loaded = COM_LoadMallocFile (path, NULL);
			if (!loaded || com_filesize <= 0)
			{
				if (loaded) q_free (loaded);
				Con_Warning ("GLB: could not load external buffer '%s' for %s\n", buffer->uri, mod->name);
				return false;
			}
			if (buffer->byte_length > 0 && com_filesize < buffer->byte_length)
			{
				Con_Warning ("GLB: external buffer '%s' is shorter than declared for %s\n", buffer->uri, mod->name);
				q_free (loaded);
				return false;
			}
			buffer->data = loaded;
			buffer->size = (int)com_filesize;
			buffer->owned = true;
		}
	}
	return true;
}

static gltexture_t *glb_load_texture_index (qmodel_t *mod, const glb_data_t *data, int texture_index, const char *usage)
{
	if (texture_index >= 0 && texture_index < data->num_textures)
	{
		const glb_texture_t *tex = &data->textures[texture_index];
		if (tex->source_index >= 0 && tex->source_index < data->num_images)
		{
			const glb_image_t *img = &data->images[tex->source_index];
			if (img->has_uri && img->uri[0])
			{
				char texname[MAX_QPATH];
				char basename[MAX_QPATH];
				char modeldir[MAX_QPATH];

				if (!glb_uri_is_safe (img->uri))
				{
					Con_Warning ("GLB: unsafe texture uri '%s' ignored for %s\n", img->uri, mod->name);
					return NULL;
				}

				COM_StripExtension (mod->name, modeldir, sizeof (modeldir));
				const char *slash = strrchr (modeldir, '/');
				if (slash)
				{
					int dirlen = (int)(slash - modeldir + 1);
					q_snprintf (texname, sizeof (texname), "%.*s%s", dirlen, modeldir, img->uri);
				}
				else
					q_snprintf (texname, sizeof (texname), "%s", img->uri);

				int fwidth, fheight;
				enum srcformat fmt = SRC_RGBA;
				byte *imgdata = Image_LoadImage (texname, &fwidth, &fheight, &fmt);
				if (imgdata)
				{
					gltexture_t *gltex = TexMgr_LoadImage (mod, texname, fwidth, fheight, fmt, imgdata, texname, 0, TEXPREF_ALPHA | TEXPREF_NOBRIGHT | TEXPREF_MIPMAP);
					q_free (imgdata);
					return gltex;
				}

				COM_StripExtension (img->uri, basename, sizeof (basename));
				const char *urislash = strrchr (img->uri, '/');
				if (urislash)
					COM_StripExtension (urislash + 1, basename, sizeof (basename));

				q_snprintf (texname, sizeof (texname), "%s/%s", modeldir, basename);
				imgdata = Image_LoadImage (texname, &fwidth, &fheight, &fmt);
				if (imgdata)
				{
					gltexture_t *gltex = TexMgr_LoadImage (mod, texname, fwidth, fheight, fmt, imgdata, texname, 0, TEXPREF_ALPHA | TEXPREF_NOBRIGHT | TEXPREF_MIPMAP);
					q_free (imgdata);
					return gltex;
				}

				Con_Warning ("GLB: could not load texture '%s' for %s\n", img->uri, mod->name);
			}
			else if (img->has_buffer_view && img->buffer_view >= 0 && img->buffer_view < data->num_bufferviews)
			{
				const glb_bufferview_t *bv = &data->bufferviews[img->buffer_view];
				if (bv->buffer >= 0 && bv->buffer < data->num_buffers && data->buffers[bv->buffer].data &&
					bv->byte_offset >= 0 && bv->byte_length > 0 && bv->byte_offset + bv->byte_length <= data->buffers[bv->buffer].size)
				{
					int fwidth, fheight;
					enum srcformat fmt = SRC_RGBA;
					char texname[MAX_QPATH];
					const byte *imgdata_src = data->buffers[bv->buffer].data + bv->byte_offset;
					byte *imgdata;
					q_snprintf (texname, sizeof (texname), "%s:%s:%d", mod->name, usage ? usage : "image", tex->source_index);
					imgdata = Image_LoadImageBuffer (texname, imgdata_src, bv->byte_length, &fwidth, &fheight, &fmt);
					if (imgdata)
					{
						gltexture_t *gltex = TexMgr_LoadImage (mod, texname, fwidth, fheight, fmt, imgdata, texname, 0, TEXPREF_ALPHA | TEXPREF_NOBRIGHT | TEXPREF_MIPMAP);
						q_free (imgdata);
						return gltex;
					}
				}
			}
		}
	}

	return NULL;
}

static gltexture_t *glb_load_texture (qmodel_t *mod, const glb_data_t *data, int material_index)
{
	const glb_material_t *mat = NULL;
	if (material_index >= 0 && material_index < data->num_materials)
		mat = &data->materials[material_index];
	if (mat && mat->has_texture)
		return glb_load_texture_index (mod, data, mat->base_color_texture_index, "baseColor");
	return NULL;
}

static gltexture_t *glb_load_emissive_texture (qmodel_t *mod, const glb_data_t *data, int material_index)
{
	const glb_material_t *mat = NULL;
	if (material_index >= 0 && material_index < data->num_materials)
		mat = &data->materials[material_index];
	if (mat && mat->has_emissive_texture)
		return glb_load_texture_index (mod, data, mat->emissive_texture_index, "emissive");
	return NULL;
}

static void glb_compute_normals (float (*normals)[3], const float (*positions)[3], int numverts, const unsigned short *indices, int numindices)
{
	int i;
	memset (normals, 0, sizeof (float) * 3 * numverts);
	for (i = 0; i + 2 < numindices; i += 3)
	{
		int i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
		float e1[3], e2[3], n[3];
		e1[0] = positions[i1][0] - positions[i0][0];
		e1[1] = positions[i1][1] - positions[i0][1];
		e1[2] = positions[i1][2] - positions[i0][2];
		e2[0] = positions[i2][0] - positions[i0][0];
		e2[1] = positions[i2][1] - positions[i0][1];
		e2[2] = positions[i2][2] - positions[i0][2];
		CrossProduct (e1, e2, n);
		normals[i0][0] += n[0]; normals[i0][1] += n[1]; normals[i0][2] += n[2];
		normals[i1][0] += n[0]; normals[i1][1] += n[1]; normals[i1][2] += n[2];
		normals[i2][0] += n[0]; normals[i2][1] += n[1]; normals[i2][2] += n[2];
	}
	for (i = 0; i < numverts; i++)
	{
		float len = sqrt (normals[i][0] * normals[i][0] + normals[i][1] * normals[i][1] + normals[i][2] * normals[i][2]);
		if (len > 1e-8f)
		{
			normals[i][0] /= len;
			normals[i][1] /= len;
			normals[i][2] /= len;
		}
		else
		{
			normals[i][0] = 0.f;
			normals[i][1] = 1.f;
			normals[i][2] = 0.f;
		}
	}
}

void Mod_LoadGLBModel (qmodel_t *mod, void *buffer)
{
	glb_data_t gbldata;
	int i, j;
	unsigned int magic, version, total_length;
	int start, end, total;
	aliashdr_t *outhdr;
	aliashdr_t *surf;
	size_t hdrsize;
	int valid_primitives = 0;
	int surface_index = 0;
	boneinfo_t *outbones;
	bonepose_t *outposes;
	iqmvert_t *poutvert;
	unsigned short *poutindexes;
	int total_verts, total_indexes;
	float (*all_positions)[3] = NULL;
	float (*all_normals)[3] = NULL;
	float (*all_uvs)[2] = NULL;
	qboolean *has_normal_per_prim = NULL;

	GLB_DebugPrintf ("GLB: loading %s\n", mod->name);

	if (!buffer)
	{
		Con_Warning ("GLB: null buffer for %s\n", mod->name);
		return;
	}

	const byte *buf = (const byte *)buffer;
	const unsigned int file_size = (unsigned int)com_filesize;

	if (file_size <= 0)
	{
		Con_Warning ("GLB: buffer too small for %s\n", mod->name);
		return;
	}

	int offset = 0;
	const byte *json_data = NULL;
	int json_size = 0;
	const byte *bin_data = NULL;
	int bin_size = 0;

	magic = (file_size >= 4) ? (unsigned int)(buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24)) : 0;
	if (magic == GLB_MAGIC)
	{
		memcpy (&version, buf + 4, 4);
		memcpy (&total_length, buf + 8, 4);

		if (version != 2)
		{
			Con_Warning ("GLB: unsupported version %u for %s (only version 2)\n", version, mod->name);
			return;
		}

		if (total_length < 20)
		{
			Con_Warning ("GLB: file too small (%u bytes) for %s\n", total_length, mod->name);
			return;
		}
		if (total_length > file_size)
		{
			Con_Warning ("GLB: declared length %u exceeds file size %u for %s\n", total_length, file_size, mod->name);
			return;
		}

		offset = 12;
		while (offset + 8 <= (int)total_length)
		{
			unsigned int chunk_length, chunk_type;
			memcpy (&chunk_length, buf + offset, 4);
			memcpy (&chunk_type, buf + offset + 4, 4);
			offset += 8;

			if (offset + (int)chunk_length > (int)total_length)
			{
				Con_Warning ("GLB: chunk overflows file for %s\n", mod->name);
				return;
			}

			if (chunk_type == GLB_JSON_CHUNK && !json_data)
			{
				json_data = buf + offset;
				json_size = chunk_length;
			}
			else if (chunk_type == GLB_BIN_CHUNK && !bin_data)
			{
				bin_data = buf + offset;
				bin_size = chunk_length;
			}

			offset += chunk_length;
		}
	}
	else
	{
		json_data = buf;
		json_size = (int)file_size;
	}

	if (!json_data || json_size <= 0)
	{
		Con_Warning ("GLB: no JSON chunk in %s\n", mod->name);
		return;
	}

	memset (&gbldata, 0, sizeof (gbldata));
	gbldata.bin_data = bin_data;
	gbldata.bin_size = bin_size;

	glb_json = json_data;
	glb_json_size = json_size;
	glb_json_pos = 0;
	glb_json_error = false;

	if (!glb_parse_json (&gbldata))
	{
		Con_Warning ("GLB: failed to parse JSON for %s\n", mod->name);
		return;
	}

	if (bin_data)
	{
		if (gbldata.num_buffers == 0)
			gbldata.num_buffers = 1;
		gbldata.buffers[0].data = bin_data;
		gbldata.buffers[0].size = bin_size;
	}

	if (!glb_load_external_buffers (mod, &gbldata))
	{
		glb_free_owned_buffers (&gbldata);
		return;
	}

	if (gbldata.num_primitives == 0)
	{
		Con_Warning ("GLB: no primitives found in %s\n", mod->name);
		glb_free_owned_buffers (&gbldata);
		return;
	}

	if (gbldata.num_skins > 0)
		Con_Warning ("GLB: skins/joints ignored for %s (skeletal animation unsupported)\n", mod->name);
	if (gbldata.num_animations > 0)
		Con_Warning ("GLB: animations ignored for %s (animation unsupported)\n", mod->name);

	GLB_DebugPrintf ("  meshes: %d, primitives: %d, accessors: %d, bufferviews: %d\n",
		gbldata.num_meshes, gbldata.num_primitives, gbldata.num_accessors, gbldata.num_bufferviews);
	GLB_DebugPrintf ("  materials: %d, textures: %d, images: %d\n",
		gbldata.num_materials, gbldata.num_textures, gbldata.num_images);
	if (gbldata.num_skins) GLB_DebugPrintf ("  skins: %d (UNSUPPORTED)\n", gbldata.num_skins);
	if (gbldata.num_animations) GLB_DebugPrintf ("  animations: %d (UNSUPPORTED)\n", gbldata.num_animations);

	total_verts = 0;
	total_indexes = 0;
	for (i = 0; i < gbldata.num_primitives; i++)
	{
		glb_primitive_t *p = &gbldata.primitives[i];
		if (p->mode != 0 && p->mode != GLTF_PRIMITIVE_TRIANGLES)
		{
			Con_Warning ("GLB: non-triangle primitive mode %d skipped in %s\n", p->mode, mod->name);
			continue;
		}
		if (p->position_accessor < 0 || p->position_accessor >= gbldata.num_accessors)
		{
			Con_Warning ("GLB: primitive %d missing POSITION accessor in %s\n", i, mod->name);
			continue;
		}
		glb_accessor_t *acc = &gbldata.accessors[p->position_accessor];
		valid_primitives++;
		total_verts += acc->count;
		if (p->indices_accessor >= 0 && p->indices_accessor < gbldata.num_accessors)
			total_indexes += gbldata.accessors[p->indices_accessor].count;
		else
			total_indexes += acc->count;
	}

	if (total_verts == 0 || total_indexes == 0)
	{
		Con_Warning ("GLB: no vertex/index data in %s\n", mod->name);
		glb_free_owned_buffers (&gbldata);
		return;
	}

	if (total_verts > MAXALIASVERTS)
	{
		Con_Warning ("GLB: too many vertices (%d > %d) in %s\n", total_verts, MAXALIASVERTS, mod->name);
		glb_free_owned_buffers (&gbldata);
		return;
	}

	start = Hunk_LowMark ();

	hdrsize = sizeof (*outhdr);
	outhdr = (aliashdr_t *) Hunk_Alloc (hdrsize * valid_primitives);
	memset (outhdr, 0, hdrsize * valid_primitives);

	outbones = (boneinfo_t *) Hunk_Alloc (sizeof (*outbones));
	memset (outbones, 0, sizeof (*outbones));
	outbones->parent = -1;
	memset (&outbones->inverse, 0, sizeof (outbones->inverse));
	outbones->inverse.mat[0] = 1.f;
	outbones->inverse.mat[5] = 1.f;
	outbones->inverse.mat[10] = 1.f;

	outposes = (bonepose_t *) Hunk_Alloc (sizeof (*outposes));
	memset (outposes, 0, sizeof (*outposes));
	outposes->mat[0] = 1.f;
	outposes->mat[5] = 1.f;
	outposes->mat[10] = 1.f;

	for (i = 0; i < valid_primitives; i++)
	{
		surf = (aliashdr_t *)((byte *)outhdr + hdrsize * i);
		if (i + 1 < valid_primitives)
			surf->nextsurface = hdrsize;
		surf->poseverttype = PV_IQM;
		surf->numskins = 1;
		surf->numframes = 1;
		surf->synctype = ST_SYNC;
		surf->numposes = 1;
		surf->numbones = 1;
		surf->numboneposes = 1;
		for (j = 0; j < 3; j++)
		{
			surf->scale[j] = 1.f;
			surf->scale_origin[j] = 0.f;
			surf->eyeposition[j] = 0.f;
		}
		surf->flags = 0;
		surf->size = 1.f;
		surf->boundingradius = 0.f;
		surf->boneinfo = (byte *)outbones - (byte *)surf;
		surf->boneposedata = (byte *)outposes - (byte *)surf;
		surf->frames[0].firstpose = 0;
		surf->frames[0].numposes = 1;
		surf->frames[0].interval = 0.1f;
	}

	poutvert = (iqmvert_t *) Hunk_Alloc (sizeof (*poutvert) * total_verts);
	poutindexes = (unsigned short *) Hunk_Alloc (sizeof (*poutindexes) * total_indexes);

	all_positions = (float (*)[3]) q_calloc (total_verts, sizeof (float) * 3);
	all_normals = (float (*)[3]) q_calloc (total_verts, sizeof (float) * 3);
	all_uvs = (float (*)[2]) q_calloc (total_verts, sizeof (float) * 2);
	has_normal_per_prim = (qboolean *) q_calloc (gbldata.num_primitives, sizeof (qboolean));

	if (!all_positions || !all_normals || !all_uvs || !has_normal_per_prim)
	{
		Con_Warning ("GLB: allocation failed for %s\n", mod->name);
		if (all_positions) q_free (all_positions);
		if (all_normals) q_free (all_normals);
		if (all_uvs) q_free (all_uvs);
		if (has_normal_per_prim) q_free (has_normal_per_prim);
		Hunk_FreeToLowMark (start);
		glb_free_owned_buffers (&gbldata);
		return;
	}

	int vert_offset = 0;
	int idx_offset = 0;

	for (i = 0; i < gbldata.num_primitives; i++)
	{
		glb_primitive_t *p = &gbldata.primitives[i];
		if (p->mode != 0 && p->mode != GLTF_PRIMITIVE_TRIANGLES)
			continue;
		if (p->position_accessor < 0 || p->position_accessor >= gbldata.num_accessors)
			continue;

		glb_accessor_t *pos_acc = &gbldata.accessors[p->position_accessor];
		int stride;
		const byte *pos_data = glb_get_accessor_data (&gbldata, pos_acc, &stride);
		if (!pos_data)
		{
			Con_Warning ("GLB: could not read POSITION data for primitive %d in %s\n", i, mod->name);
			continue;
		}

		if (pos_acc->component_type != GLTF_COMPONENT_FLOAT || pos_acc->type != GLTF_TYPE_VEC3)
		{
			Con_Warning ("GLB: POSITION must be FLOAT VEC3 for primitive %d in %s\n", i, mod->name);
			continue;
		}

		int nverts = pos_acc->count;
		int prim_index_count = (p->indices_accessor >= 0 && p->indices_accessor < gbldata.num_accessors) ? gbldata.accessors[p->indices_accessor].count : nverts;
		int prim_vert_offset = vert_offset;
		int prim_idx_offset = idx_offset;
		gltexture_t *base_tex, *emissive_tex;
		extern gltexture_t *greytexture;

		if (surface_index >= valid_primitives)
			break;
		surf = (aliashdr_t *)((byte *)outhdr + hdrsize * surface_index);
		surf->vertexes = (byte *)(poutvert + prim_vert_offset) - (byte *)surf;
		surf->numverts_vbo = nverts;
		surf->numverts = nverts;
		surf->indexes = (byte *)(poutindexes + prim_idx_offset) - (byte *)surf;
		surf->numindexes = prim_index_count;
		surf->numtris = prim_index_count / 3;

		base_tex = glb_load_texture (mod, &gbldata, p->material_index);
		emissive_tex = glb_load_emissive_texture (mod, &gbldata, p->material_index);
		if (!base_tex)
			base_tex = greytexture;
		for (j = 0; j < 4; j++)
		{
			surf->gltextures[0][j] = base_tex;
			surf->fbtextures[0][j] = NULL;
			surf->emissivetextures[0][j] = emissive_tex;
		}
		surf->skinwidth = base_tex ? base_tex->width : 1;
		surf->skinheight = base_tex ? base_tex->height : 1;
		GLB_DebugPrintf ("  primitive %d material %d texture: %s (%dx%d)\n", i, p->material_index,
			base_tex ? base_tex->name : "<none>", surf->skinwidth, surf->skinheight);

		if (prim_index_count <= 0 || prim_index_count % 3)
			Con_Warning ("GLB: primitive %d index count %d is not a triangle list in %s\n", i, prim_index_count, mod->name);

		if (p->normal_accessor >= 0 && p->normal_accessor < gbldata.num_accessors)
		{
			glb_accessor_t *nrm_acc = &gbldata.accessors[p->normal_accessor];
			int nrm_stride;
			const byte *nrm_data = glb_get_accessor_data (&gbldata, nrm_acc, &nrm_stride);
			if (nrm_data && nrm_acc->component_type == GLTF_COMPONENT_FLOAT && nrm_acc->type == GLTF_TYPE_VEC3 && nrm_acc->count == nverts)
			{
				has_normal_per_prim[i] = true;
				for (j = 0; j < nverts; j++)
				{
					memcpy (all_normals[vert_offset + j], nrm_data + j * nrm_stride, sizeof (float) * 3);
				}
			}
		}

		if (p->uv0_accessor >= 0 && p->uv0_accessor < gbldata.num_accessors)
		{
			glb_accessor_t *uv_acc = &gbldata.accessors[p->uv0_accessor];
			int uv_stride;
			const byte *uv_data = glb_get_accessor_data (&gbldata, uv_acc, &uv_stride);
			if (uv_data && uv_acc->component_type == GLTF_COMPONENT_FLOAT && uv_acc->type == GLTF_TYPE_VEC2 && uv_acc->count == nverts)
			{
				for (j = 0; j < nverts; j++)
				{
					memcpy (all_uvs[vert_offset + j], uv_data + j * uv_stride, sizeof (float) * 2);
				}
			}
		}

		for (j = 0; j < nverts; j++)
		{
			memcpy (all_positions[vert_offset + j], pos_data + j * stride, sizeof (float) * 3);
		}

		if (p->indices_accessor >= 0 && p->indices_accessor < gbldata.num_accessors)
		{
			glb_accessor_t *idx_acc = &gbldata.accessors[p->indices_accessor];
			int idx_stride;
			const byte *idx_data = glb_get_accessor_data (&gbldata, idx_acc, &idx_stride);
			if (idx_data)
			{
				if (idx_offset + idx_acc->count > total_indexes)
				{
					Con_Warning ("GLB: index buffer overflow in primitive %d for %s\n", i, mod->name);
					continue;
				}
				if (idx_acc->component_type == GLTF_COMPONENT_UNSIGNED_SHORT)
				{
					for (j = 0; j < idx_acc->count; j++)
					{
						unsigned short idx;
						memcpy (&idx, idx_data + j * idx_stride, sizeof (unsigned short));
						if (idx >= (unsigned short)nverts)
						{
							Con_Warning ("GLB: out-of-range index %u (verts=%d) in %s\n", idx, nverts, mod->name);
							idx = 0;
						}
						poutindexes[idx_offset + j] = idx;
					}
					idx_offset += idx_acc->count;
				}
				else if (idx_acc->component_type == GLTF_COMPONENT_UNSIGNED_INT)
				{
					for (j = 0; j < idx_acc->count; j++)
					{
						unsigned int idx;
						memcpy (&idx, idx_data + j * idx_stride, sizeof (unsigned int));
						if (idx >= (unsigned int)nverts || idx > 65535)
						{
							Con_Warning ("GLB: out-of-range index %u (verts=%d) in %s\n", idx, nverts, mod->name);
							idx = 0;
						}
						poutindexes[idx_offset + j] = (unsigned short)idx;
					}
					idx_offset += idx_acc->count;
				}
				else if (idx_acc->component_type == GLTF_COMPONENT_UNSIGNED_BYTE)
				{
					for (j = 0; j < idx_acc->count; j++)
					{
						unsigned int idx = idx_data[j * idx_stride];
						if (idx >= (unsigned int)nverts)
						{
							Con_Warning ("GLB: out-of-range index %u (verts=%d) in %s\n", idx, nverts, mod->name);
							idx = 0;
						}
						poutindexes[idx_offset + j] = (unsigned short)idx;
					}
					idx_offset += idx_acc->count;
				}
				else
				{
					Con_Warning ("GLB: unsupported index component type %d in %s\n", idx_acc->component_type, mod->name);
				}
			}
		}
		else
		{
			if (idx_offset + nverts > total_indexes)
			{
				Con_Warning ("GLB: generated index buffer overflow in primitive %d for %s\n", i, mod->name);
				continue;
			}
			for (j = 0; j < nverts; j++)
				poutindexes[idx_offset + j] = j;
			idx_offset += nverts;
		}

		surf->numindexes = idx_offset - prim_idx_offset;
		surf->numtris = surf->numindexes / 3;
		vert_offset += nverts;
		surface_index++;
	}

	total_verts = vert_offset;
	total_indexes = idx_offset;
	if (total_verts <= 0 || total_indexes <= 0)
	{
		Con_Warning ("GLB: no valid geometry decoded in %s\n", mod->name);
		Hunk_FreeToLowMark (start);
		q_free (all_positions);
		q_free (all_normals);
		q_free (all_uvs);
		q_free (has_normal_per_prim);
		glb_free_owned_buffers (&gbldata);
		return;
	}

	for (i = 0; i < gbldata.num_primitives; i++)
	{
		if (!has_normal_per_prim[i])
		{
			Con_DPrintf ("GLB: generating normals for primitive %d in %s\n", i, mod->name);
		}
	}

	for (j = 0; j < total_verts; j++)
	{
		float len = sqrt (all_normals[j][0] * all_normals[j][0] + all_normals[j][1] * all_normals[j][1] + all_normals[j][2] * all_normals[j][2]);
		if (len < 1e-8f)
		{
			all_normals[j][0] = 0.f;
			all_normals[j][1] = 1.f;
			all_normals[j][2] = 0.f;
		}
	}

	{
		int need_generate = 0;
		for (i = 0; i < gbldata.num_primitives; i++)
			if (!has_normal_per_prim[i]) need_generate = 1;
		if (need_generate)
		{
			for (surf = outhdr; surf; surf = surf->nextsurface ? (aliashdr_t *)((byte *)surf + surf->nextsurface) : NULL)
			{
				int voff = (int)(((iqmvert_t *)((byte *)surf + surf->vertexes)) - poutvert);
				unsigned short *idx = (unsigned short *)((byte *)surf + surf->indexes);
				if (voff >= 0 && voff + surf->numverts <= total_verts)
					glb_compute_normals (all_normals + voff, all_positions + voff, surf->numverts, idx, surf->numindexes);
			}
		}
	}

	for (j = 0; j < total_verts; j++)
	{
		poutvert[j].xyz[0] = all_positions[j][0];
		poutvert[j].xyz[1] = all_positions[j][1];
		poutvert[j].xyz[2] = all_positions[j][2];
		poutvert[j].norm[0] = (int8_t) CLAMP (-127, (int)(all_normals[j][0] * 127.f), 127);
		poutvert[j].norm[1] = (int8_t) CLAMP (-127, (int)(all_normals[j][1] * 127.f), 127);
		poutvert[j].norm[2] = (int8_t) CLAMP (-127, (int)(all_normals[j][2] * 127.f), 127);
		poutvert[j].norm[3] = 0;
		poutvert[j].st[0] = all_uvs[j][0];
		poutvert[j].st[1] = all_uvs[j][1];
		poutvert[j].tangent[0] = 0;
		poutvert[j].tangent[1] = 0;
		poutvert[j].tangent[2] = 0;
		poutvert[j].tangent[3] = 127;
		poutvert[j].weight[0] = 255;
		poutvert[j].weight[1] = 0;
		poutvert[j].weight[2] = 0;
		poutvert[j].weight[3] = 0;
		poutvert[j].idx[0] = 0;
		poutvert[j].idx[1] = 0;
		poutvert[j].idx[2] = 0;
		poutvert[j].idx[3] = 0;
	}

	{
		float (*tangent_arr)[4] = (float (*)[4]) q_calloc (total_verts, sizeof (float) * 4);
		if (tangent_arr)
		{
			for (surf = outhdr; surf; surf = surf->nextsurface ? (aliashdr_t *)((byte *)surf + surf->nextsurface) : NULL)
			{
				int voff = (int)(((iqmvert_t *)((byte *)surf + surf->vertexes)) - poutvert);
				unsigned short *idx = (unsigned short *)((byte *)surf + surf->indexes);
				if (voff >= 0 && voff + surf->numverts <= total_verts)
					GLMesh_BuildTangents (tangent_arr + voff, all_positions + voff, all_normals + voff, all_uvs + voff, surf->numverts, idx, surf->numindexes);
			}
			for (j = 0; j < total_verts; j++)
			{
				poutvert[j].tangent[0] = (int8_t) CLAMP (-127, (int)(tangent_arr[j][0] * 127.f), 127);
				poutvert[j].tangent[1] = (int8_t) CLAMP (-127, (int)(tangent_arr[j][1] * 127.f), 127);
				poutvert[j].tangent[2] = (int8_t) CLAMP (-127, (int)(tangent_arr[j][2] * 127.f), 127);
				poutvert[j].tangent[3] = (int8_t) (tangent_arr[j][3] < 0.f ? -127 : 127);
			}
			q_free (tangent_arr);
		}
	}

	q_free (all_positions);
	q_free (all_normals);
	q_free (all_uvs);
	q_free (has_normal_per_prim);
	all_positions = NULL;
	all_normals = NULL;
	all_uvs = NULL;
	has_normal_per_prim = NULL;

	GLMesh_LoadVertexBuffer (mod, outhdr);

	mod->synctype = ST_SYNC;
	mod->type = mod_glb;
	mod->numframes = 1;
	mod->flags = 0;

	mod->mins[0] = mod->mins[1] = mod->mins[2] = FLT_MAX;
	mod->maxs[0] = mod->maxs[1] = mod->maxs[2] = -FLT_MAX;
	{
		float yawradius = 0, radius = 0;
		for (j = 0; j < total_verts; j++)
		{
			int k;
			for (k = 0; k < 3; k++)
			{
				mod->mins[k] = q_min (mod->mins[k], poutvert[j].xyz[k]);
				mod->maxs[k] = q_max (mod->maxs[k], poutvert[j].xyz[k]);
			}
			float dist = poutvert[j].xyz[0] * poutvert[j].xyz[0] + poutvert[j].xyz[1] * poutvert[j].xyz[1];
			if (yawradius < dist) yawradius = dist;
			dist += poutvert[j].xyz[2] * poutvert[j].xyz[2];
			if (radius < dist) radius = dist;
		}
		radius = sqrt (radius);
		mod->rmins[0] = mod->rmins[1] = mod->rmins[2] = -radius;
		mod->rmaxs[0] = mod->rmaxs[1] = mod->rmaxs[2] = radius;
		yawradius = sqrt (yawradius);
		mod->ymins[0] = mod->ymins[1] = -yawradius;
		mod->ymaxs[0] = mod->ymaxs[1] = yawradius;
		mod->ymins[2] = mod->mins[2];
		mod->ymaxs[2] = mod->maxs[2];
	}

	mod->sortkey = ((CRC_Block ((byte *)mod->name, strlen (mod->name)) >> 1) & MODSORT_FRAMEMASK) << MODSORT_FRAMEBITS;

	GLB_DebugPrintf ("  vertices: %d, indices: %d, triangles: %d\n", total_verts, total_indexes, total_indexes / 3);
	GLB_DebugPrintf ("  bounds: [%.2f %.2f %.2f] to [%.2f %.2f %.2f]\n",
		mod->mins[0], mod->mins[1], mod->mins[2],
		mod->maxs[0], mod->maxs[1], mod->maxs[2]);

	end = Hunk_LowMark ();
	total = end - start;

	Cache_Alloc (&mod->cache, total, mod->name);
	if (!mod->cache.data)
	{
		Hunk_FreeToLowMark (start);
		glb_free_owned_buffers (&gbldata);
		return;
	}
	memcpy (mod->cache.data, outhdr, total);

	Hunk_FreeToLowMark (start);
	glb_free_owned_buffers (&gbldata);

	GLB_DebugPrintf ("GLB: %s loaded OK\n", mod->name);
}

void R_DrawGLBModels (entity_t **ents, int count)
{
	extern void R_DrawAliasModels (entity_t **ents, int count);
	R_DrawAliasModels (ents, count);
}

void R_DrawGLBModels_ShowTris (entity_t **ents, int count)
{
	extern void R_DrawAliasModels_ShowTris (entity_t **ents, int count);
	R_DrawAliasModels_ShowTris (ents, count);
}
