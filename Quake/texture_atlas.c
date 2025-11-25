/*
 * CPU Texture Atlas Generator
 */

#include "quakedef.h"
#include "texture_atlas.h"
#include "stb_image_write.h"

static world_atlas_t g_world_atlas;

static void Atlas_FreeTextures(void)
{
	int i;
	for (i = 0; i < g_world_atlas.texture_count; i++)
	{
		if (g_world_atlas.textures[i].rgba)
		{
			free(g_world_atlas.textures[i].rgba);
			g_world_atlas.textures[i].rgba = NULL;
		}
	}
}

static void Atlas_BlitTexture(int dst_x, int dst_y, const atlas_texture_t *tex)
{
	int x, y;
	unsigned char *dst = g_world_atlas.pixels + ((dst_y * g_world_atlas.atlas_w) + dst_x) * 4;

	for (y = 0; y < tex->height; y++)
	{
		unsigned char *dst_row = dst + y * g_world_atlas.atlas_w * 4;
		const unsigned char *src_row = tex->rgba + y * tex->width * 4;
		for (x = 0; x < tex->width; x++)
		{
			int di = x * 4;
			int si = x * 4;
			dst_row[di + 0] = src_row[si + 0];
			dst_row[di + 1] = src_row[si + 1];
			dst_row[di + 2] = src_row[si + 2];
			dst_row[di + 3] = src_row[si + 3];
		}
	}
}

void Atlas_Init(void)
{
	memset(&g_world_atlas, 0, sizeof(g_world_atlas));
	g_world_atlas.atlas_w = ATLAS_SIZE;
	g_world_atlas.atlas_h = ATLAS_SIZE;
}

void Atlas_Reset(void)
{
	Atlas_FreeTextures();
	if (g_world_atlas.pixels)
	{
		free(g_world_atlas.pixels);
		g_world_atlas.pixels = NULL;
	}
	memset(g_world_atlas.textures, 0, sizeof(g_world_atlas.textures));
	memset(g_world_atlas.rects, 0, sizeof(g_world_atlas.rects));
	g_world_atlas.texture_count = 0;
	g_world_atlas.built = 0;
}

void Atlas_RegisterTexture(const char *name, int w, int h, const unsigned char *rgba)
{
	atlas_texture_t *tex;
	int size;

	if (!name || !rgba)
		return;

	if (g_world_atlas.texture_count >= ATLAS_MAX_TEXTURES)
	{
		Con_Printf("Atlas_RegisterTexture: max textures reached\n");
		return;
	}

	tex = &g_world_atlas.textures[g_world_atlas.texture_count];
	q_strlcpy(tex->name, name, sizeof(tex->name));
	tex->width = w;
	tex->height = h;

	size = w * h * 4;
	tex->rgba = (unsigned char *) malloc(size);
	if (!tex->rgba)
	{
		Con_Printf("Atlas_RegisterTexture: out of memory\n");
		return;
	}

	memcpy(tex->rgba, rgba, size);
	g_world_atlas.texture_count++;
}

void Atlas_Build(const char *mapname)
{
	int i;
	int x = 0, y = 0, row_h = 0;

	if (g_world_atlas.texture_count == 0)
		return;

	if (g_world_atlas.pixels)
	{
		free(g_world_atlas.pixels);
		g_world_atlas.pixels = NULL;
	}

	g_world_atlas.pixels = (unsigned char *) calloc(1, g_world_atlas.atlas_w * g_world_atlas.atlas_h * 4);
	if (!g_world_atlas.pixels)
	{
		Con_Printf("Atlas_Build: failed to allocate atlas buffer\n");
		return;
	}

	for (i = 0; i < g_world_atlas.texture_count; i++)
	{
		atlas_texture_t *tex = &g_world_atlas.textures[i];
		atlas_rect_t *rect = &g_world_atlas.rects[i];

		if (tex->width <= 0 || tex->height <= 0)
			continue;

		if (x + tex->width > g_world_atlas.atlas_w)
		{
			x = 0;
			y += row_h;
			row_h = 0;
		}

		if (y + tex->height > g_world_atlas.atlas_h)
		{
			Con_Printf("Atlas_Build: texture '%s' does not fit in atlas\n", tex->name);
			continue;
		}

		rect->x = x;
		rect->y = y;
		rect->w = tex->width;
		rect->h = tex->height;

		Atlas_BlitTexture(x, y, tex);

		x += tex->width;
		if (tex->height > row_h)
			row_h = tex->height;
	}

	g_world_atlas.built = 1;

	Con_Printf("Atlas_Build: built atlas for %s (%d textures)\n", mapname ? mapname : "<unknown>", g_world_atlas.texture_count);
}

void Atlas_SavePNG(const char *mapname)
{
	char dirname[MAX_OSPATH];
	char filename[MAX_OSPATH];

	if (!g_world_atlas.built || !g_world_atlas.pixels)
		return;

	q_snprintf(dirname, sizeof(dirname), "%s/atlas_dump", com_gamedir);
	Sys_mkdir(dirname);

	q_snprintf(filename, sizeof(filename), "%s/%s_atlas.png", dirname, mapname ? mapname : "map");

	stbi_write_png(filename, g_world_atlas.atlas_w, g_world_atlas.atlas_h, 4, g_world_atlas.pixels, g_world_atlas.atlas_w * 4);
	Con_Printf("Atlas_SavePNG: wrote %s\n", filename);
}

void Atlas_SaveJSON(const char *mapname)
{
    char dirname[MAX_OSPATH];
    char filename[MAX_OSPATH];
    FILE *f;
    int i;

    if (!g_world_atlas.built)
        return;

    q_snprintf(dirname, sizeof(dirname), "%s/atlas_dump", com_gamedir);
    Sys_mkdir(dirname);

    q_snprintf(filename, sizeof(filename), "%s/%s_atlas.json", dirname, mapname ? mapname : "map");

    f = fopen(filename, "w");
    if (!f)
    {
        Con_Printf("Atlas_SaveJSON: failed to open %s\n", filename);
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"atlas_width\": %d,\n", g_world_atlas.atlas_w);
    fprintf(f, "  \"atlas_height\": %d,\n", g_world_atlas.atlas_h);
    fprintf(f, "  \"textures\": [\n");

    for (i = 0; i < g_world_atlas.texture_count; i++)
    {
        atlas_texture_t *tex = &g_world_atlas.textures[i];
        atlas_rect_t *rect = &g_world_atlas.rects[i];
        fprintf(f, "    { \"name\": \"%s\", \"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d }%s\n",
            tex->name,
            rect->x, rect->y, rect->w, rect->h,
            (i + 1 < g_world_atlas.texture_count) ? "," : "");
    }

    fprintf(f, "  ]\n");
    fprintf(f, "}\n");

    fclose(f);
    Con_Printf("Atlas_SaveJSON: wrote %s\n", filename);
}

void Atlas_OnMapStart(const char *mapname)
{
	Atlas_Build(mapname);
	Atlas_SavePNG(mapname);
	Atlas_SaveJSON(mapname);
}

/*
 * Engine Integration Snippets (for reference)
 *
 * In GL_LoadTexture():
 * // === Texture Atlas Hook ===
 * // Atlas_RegisterTexture(name, width, height, rgba_buffer);
 *
 * In BSP-MIPTEX Load:
 * // === Texture Atlas Hook ===
 * // Atlas_RegisterTexture(mip->name, w, h, rgba);
 *
 * In Mod_LoadBrushModel():
 * // === Build CPU Atlas for this map ===
 * // Atlas_OnMapStart(loadmodel->name);
 */
