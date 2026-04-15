#include "quakedef.h"
#include "render.h"
#include "gl_model.h"
#include "glquake.h"
#include "r_skyvis.h"

extern cvar_t r_sun_visibility;
extern float skyflatcolor[3];

#define SKYVIS_MAX_CELLS 524288u
#define SKYVIS_TRACE_DIST_MAX 16384.f

typedef struct skyvis_grid_s {
	vec3_t mins;
	vec3_t spacing;
	vec3_t maxs;
	int size[3];
	float *values;
	unsigned int total_cells;
	unsigned int valid_cells;
	float average_visibility;
	qboolean used_lightgrid_header;
	qboolean has_sky_surfaces;
} skyvis_grid_t;

/* Chosen reuse strategy: keep sky visibility in a sibling runtime cache instead
 * of overloading authored RGB lightgrid probes. This reuses lightgrid bounds/
 * spacing when a BSPX octree exists, but still works on classic maps with no
 * BSPX payload and keeps the new scalar visibility data zero-safe. */
static skyvis_grid_t r_skyvis_grid;

cvar_t r_skyvis = { "r_skyvis", "1", CVAR_ARCHIVE };
cvar_t r_skyvis_debug = { "r_skyvis_debug", "0", CVAR_NONE };
/* Compatibility note: -1 keeps legacy r_sun_visibility tuning but routes it
 * through the new sky-visibility skylight path instead of the old global hack. */
cvar_t r_skyvis_scale = { "r_skyvis_scale", "-1", CVAR_ARCHIVE };
cvar_t r_skyvis_cap = { "r_skyvis_cap", "0.25", CVAR_ARCHIVE };
cvar_t r_skyvis_spacing_xy = { "r_skyvis_spacing_xy", "128", CVAR_ARCHIVE };
cvar_t r_skyvis_spacing_z = { "r_skyvis_spacing_z", "96", CVAR_ARCHIVE };
cvar_t r_skyvis_rays = { "r_skyvis_rays", "7", CVAR_ARCHIVE };

static const vec4_t skyvis_raydirs[] = {
	{  0.00f,  0.00f, 1.00f, 1.00f },
	{  0.45f,  0.00f, 1.00f, 0.82f },
	{ -0.45f,  0.00f, 1.00f, 0.82f },
	{  0.00f,  0.45f, 1.00f, 0.82f },
	{  0.00f, -0.45f, 1.00f, 0.82f },
	{  0.20f,  0.20f, 1.00f, 0.94f },
	{ -0.20f, -0.20f, 1.00f, 0.94f }
};

static float R_SkyVis_Lerp (float a, float b, float t)
{
	return a + (b - a) * t;
}

static void R_SkyVis_ResetGrid (void)
{
	q_free (r_skyvis_grid.values);
	memset (&r_skyvis_grid, 0, sizeof (r_skyvis_grid));
}

void R_SkyVis_Clear (void)
{
	R_SkyVis_ResetGrid ();
}

void R_SkyVis_Shutdown (void)
{
	R_SkyVis_ResetGrid ();
}

static qboolean R_SkyVis_WorldHasSky (const qmodel_t *world)
{
	int i;

	if (!world)
		return false;

	for (i = 0; i < world->numsurfaces; i++)
		if (world->surfaces[i].flags & SURF_DRAWSKY)
			return true;

	return false;
}

static qboolean R_SkyVis_AdjustPointForLeaf (qmodel_t *world, vec3_t point)
{
	mleaf_t *leaf = NULL;
	int i;

	for (i = 0; i < 8; i++)
	{
		leaf = Mod_PointInLeaf (point, world);
		if (!leaf || leaf->contents != CONTENTS_SOLID)
			return true;
		point[2] += 1.f;
	}

	return leaf && leaf->contents != CONTENTS_SOLID;
}

static qboolean R_SkyVis_TraceToSky (qmodel_t *world, const vec3_t start, const vec3_t dir, float trace_dist)
{
	trace_t trace;
	vec3_t start_copy;
	vec3_t end;
	int step;

	VectorCopy (start, start_copy);
	VectorMA (start, trace_dist, dir, end);
	memset (&trace, 0, sizeof (trace));
	trace.fraction = 1.f;
	trace.allsolid = true;
	VectorCopy (end, trace.endpos);

	SV_RecursiveHullCheck (&world->hulls[0], world->hulls[0].firstclipnode, 0.f, 1.f, start_copy, end, &trace);
	if (trace.startsolid || trace.allsolid)
		return false;

	if (trace.fraction < 1.f)
	{
		mleaf_t *leaf = Mod_PointInLeaf (trace.endpos, world);
		if (leaf && leaf->contents == CONTENTS_SKY)
			return true;
		for (step = 1; step < 8; step++)
		{
			vec3_t probe;
			float t = (float)step / 8.f;
			VectorLerp (start, trace.endpos, t, probe);
			leaf = Mod_PointInLeaf (probe, world);
			if (leaf && leaf->contents == CONTENTS_SKY)
				return true;
		}
		return false;
	}

	for (step = 1; step <= 8; step++)
	{
		vec3_t probe;
		mleaf_t *leaf;
		float t = (float)step / 8.f;
		VectorLerp (start, end, t, probe);
		leaf = Mod_PointInLeaf (probe, world);
		if (leaf && leaf->contents == CONTENTS_SKY)
			return true;
	}

	{
		mleaf_t *leaf = Mod_PointInLeaf (end, world);
		return leaf && leaf->contents == CONTENTS_SKY;
	}
}

static float R_SkyVis_PointVisibility (qmodel_t *world, const vec3_t pos, float trace_dist)
{
	float weighted_visible = 0.f;
	float weighted_total = 0.f;
	int ray_count = CLAMP (1, (int)r_skyvis_rays.value, (int)(sizeof (skyvis_raydirs) / sizeof (skyvis_raydirs[0])));
	int i;

	for (i = 0; i < ray_count; i++)
	{
		vec3_t dir = { skyvis_raydirs[i][0], skyvis_raydirs[i][1], skyvis_raydirs[i][2] };
		float weight = skyvis_raydirs[i][3];
		VectorNormalize (dir);
		weighted_total += weight;
		if (R_SkyVis_TraceToSky (world, pos, dir, trace_dist))
			weighted_visible += weight;
	}

	if (weighted_total <= 0.f)
		return 0.f;

	return weighted_visible / weighted_total;
}

static float R_SkyVis_EvaluateCell (qmodel_t *world, const vec3_t cell_center, const vec3_t spacing, float trace_dist, qboolean *out_valid)
{
	vec3_t sample_points[3];
	float accum = 0.f;
	int valid = 0;
	int i;
	const float zofs = spacing[2] * 0.35f;

	VectorCopy (cell_center, sample_points[0]);
	VectorCopy (cell_center, sample_points[1]);
	VectorCopy (cell_center, sample_points[2]);
	sample_points[1][2] += zofs;
	sample_points[2][2] -= zofs;

	for (i = 0; i < 3; i++)
	{
		vec3_t adjusted;
		mleaf_t *leaf;
		VectorCopy (sample_points[i], adjusted);
		if (!R_SkyVis_AdjustPointForLeaf (world, adjusted))
			continue;
		leaf = Mod_PointInLeaf (adjusted, world);
		if (!leaf || leaf->contents == CONTENTS_SOLID)
			continue;
		accum += R_SkyVis_PointVisibility (world, adjusted, trace_dist);
		valid++;
	}

	if (out_valid)
		*out_valid = (valid > 0);
	if (!valid)
		return 0.f;

	accum /= (float)valid;
	accum = CLAMP (0.f, accum, 1.f);
	return accum * accum;
}

static void R_SkyVis_InitGeneratedGrid (skyvis_grid_t *grid, const qmodel_t *world)
{
	vec3_t mins, maxs, spacing;
	unsigned int total_cells;
	int i;

	if (world->lightgrid_octree)
	{
		const lightgrid_octree_header_t *header = &world->lightgrid_octree->header;
		VectorCopy (header->grid_mins, mins);
		VectorCopy (header->grid_dist, spacing);
		for (i = 0; i < 3; i++)
		{
			spacing[i] = q_max (1.f, spacing[i]);
			grid->size[i] = q_max (1, header->grid_size[i]);
			maxs[i] = mins[i] + spacing[i] * (float)(grid->size[i] - 1);
		}
		grid->used_lightgrid_header = true;
	}
	else
	{
		spacing[0] = spacing[1] = q_max (32.f, r_skyvis_spacing_xy.value);
		spacing[2] = q_max (32.f, r_skyvis_spacing_z.value);
		for (i = 0; i < 3; i++)
		{
			mins[i] = floorf (world->mins[i] / spacing[i]) * spacing[i];
			maxs[i] = ceilf (world->maxs[i] / spacing[i]) * spacing[i];
		}
		grid->used_lightgrid_header = false;
	}

	for (;;)
	{
		total_cells = 1u;
		for (i = 0; i < 3; i++)
		{
			float extent = q_max (0.f, maxs[i] - mins[i]);
			grid->size[i] = q_max (1, (int)floorf (extent / spacing[i] + 0.5f) + 1);
			total_cells *= (unsigned int)grid->size[i];
		}
		if (total_cells <= SKYVIS_MAX_CELLS)
			break;
		spacing[0] *= 2.f;
		spacing[1] *= 2.f;
		spacing[2] *= 2.f;
		grid->used_lightgrid_header = false;
		if (r_skyvis_debug.value > 0.f)
			Con_Printf ("r_skyvis: increasing spacing to %.0f %.0f %.0f to cap grid size\n", spacing[0], spacing[1], spacing[2]);
	}

	VectorCopy (mins, grid->mins);
	VectorCopy (maxs, grid->maxs);
	VectorCopy (spacing, grid->spacing);
	grid->total_cells = total_cells;
}

static void R_SkyVis_LogStats (const skyvis_grid_t *grid)
{
	Con_Printf ("r_skyvis: %ux%ux%u cells spacing=(%.0f %.0f %.0f) total=%u valid=%u avg=%.3f source=%s\n",
		(unsigned int)grid->size[0], (unsigned int)grid->size[1], (unsigned int)grid->size[2],
		grid->spacing[0], grid->spacing[1], grid->spacing[2],
		grid->total_cells, grid->valid_cells, grid->average_visibility,
		grid->used_lightgrid_header ? "lightgrid header" : "runtime spacing");
}

void R_SkyVis_NewMap (void)
{
	qmodel_t *world = cl.worldmodel;
	float trace_dist;
	unsigned int index;
	unsigned int valid_cells = 0;
	float vis_sum = 0.f;

	R_SkyVis_ResetGrid ();
	if (!world || world->type != mod_brush)
		return;

	r_skyvis_grid.has_sky_surfaces = R_SkyVis_WorldHasSky (world);
	if (!r_skyvis_grid.has_sky_surfaces)
	{
		Con_Printf ("r_skyvis: map has no sky surfaces, unified skylight will remain disabled\n");
		return;
	}

	R_SkyVis_InitGeneratedGrid (&r_skyvis_grid, world);
	if (!r_skyvis_grid.total_cells)
		return;

	r_skyvis_grid.values = (float *)q_malloc (sizeof (*r_skyvis_grid.values) * r_skyvis_grid.total_cells);
	if (!r_skyvis_grid.values)
	{
		Con_Warning ("r_skyvis: failed to allocate %u cells, unified skylight disabled\n", r_skyvis_grid.total_cells);
		R_SkyVis_ResetGrid ();
		return;
	}

	memset (r_skyvis_grid.values, 0, sizeof (*r_skyvis_grid.values) * r_skyvis_grid.total_cells);
	trace_dist = q_min (SKYVIS_TRACE_DIST_MAX, VectorLength (world->maxs) + VectorLength (world->mins) + 4096.f);
	trace_dist = q_max (4096.f, trace_dist);

	for (index = 0; index < r_skyvis_grid.total_cells; index++)
	{
		int cell[3];
		vec3_t center;
		qboolean cell_valid = false;
		float vis;

		cell[0] = index % r_skyvis_grid.size[0];
		cell[1] = (index / r_skyvis_grid.size[0]) % r_skyvis_grid.size[1];
		cell[2] = index / (r_skyvis_grid.size[0] * r_skyvis_grid.size[1]);
		center[0] = r_skyvis_grid.mins[0] + r_skyvis_grid.spacing[0] * (float)cell[0];
		center[1] = r_skyvis_grid.mins[1] + r_skyvis_grid.spacing[1] * (float)cell[1];
		center[2] = r_skyvis_grid.mins[2] + r_skyvis_grid.spacing[2] * (float)cell[2];

		vis = R_SkyVis_EvaluateCell (world, center, r_skyvis_grid.spacing, trace_dist, &cell_valid);
		r_skyvis_grid.values[index] = CLAMP (0.f, vis, 1.f);
		if (cell_valid)
		{
			valid_cells++;
			vis_sum += r_skyvis_grid.values[index];
		}
	}

	r_skyvis_grid.valid_cells = valid_cells;
	r_skyvis_grid.average_visibility = valid_cells ? (vis_sum / (float)valid_cells) : 0.f;
	R_SkyVis_LogStats (&r_skyvis_grid);
	if (!valid_cells || r_skyvis_grid.average_visibility <= 0.f)
		Con_Warning ("r_skyvis: generated all-zero visibility, unified skylight fallback is zero\n");
	else if (r_skyvis_debug.value > 0.f)
		Con_Printf ("r_skyvis: legacy global sky/fill attenuation replaced by sky-visibility skylight\n");
}

qboolean R_SkyVis_Active (void)
{
	return r_skyvis_grid.values != NULL && r_skyvis_grid.total_cells > 0 && r_skyvis_grid.valid_cells > 0;
}

static float R_SkyVis_FetchCell (int x, int y, int z)
{
	size_t index;

	x = CLAMP (0, x, r_skyvis_grid.size[0] - 1);
	y = CLAMP (0, y, r_skyvis_grid.size[1] - 1);
	z = CLAMP (0, z, r_skyvis_grid.size[2] - 1);
	index = ((size_t)z * (size_t)r_skyvis_grid.size[1] + (size_t)y) * (size_t)r_skyvis_grid.size[0] + (size_t)x;
	return r_skyvis_grid.values[index];
}

float R_SkyVis_Sample (const vec3_t pos)
{
	vec3_t local;
	int base[3];
	vec3_t frac;
	float c000, c100, c010, c110, c001, c101, c011, c111;
	float c00, c10, c01, c11, c0, c1;
	int i;

	if (!R_SkyVis_Active ())
		return 0.f;

	for (i = 0; i < 3; i++)
	{
		local[i] = (pos[i] - r_skyvis_grid.mins[i]) / r_skyvis_grid.spacing[i];
		base[i] = (int)floorf (local[i]);
		frac[i] = local[i] - (float)base[i];
		if (base[i] < 0 || base[i] >= r_skyvis_grid.size[i])
			return 0.f;
	}

	c000 = R_SkyVis_FetchCell (base[0],     base[1],     base[2]);
	c100 = R_SkyVis_FetchCell (base[0] + 1, base[1],     base[2]);
	c010 = R_SkyVis_FetchCell (base[0],     base[1] + 1, base[2]);
	c110 = R_SkyVis_FetchCell (base[0] + 1, base[1] + 1, base[2]);
	c001 = R_SkyVis_FetchCell (base[0],     base[1],     base[2] + 1);
	c101 = R_SkyVis_FetchCell (base[0] + 1, base[1],     base[2] + 1);
	c011 = R_SkyVis_FetchCell (base[0],     base[1] + 1, base[2] + 1);
	c111 = R_SkyVis_FetchCell (base[0] + 1, base[1] + 1, base[2] + 1);

	c00 = R_SkyVis_Lerp (c000, c100, frac[0]);
	c10 = R_SkyVis_Lerp (c010, c110, frac[0]);
	c01 = R_SkyVis_Lerp (c001, c101, frac[0]);
	c11 = R_SkyVis_Lerp (c011, c111, frac[0]);
	c0 = R_SkyVis_Lerp (c00, c10, frac[1]);
	c1 = R_SkyVis_Lerp (c01, c11, frac[1]);
	return CLAMP (0.f, R_SkyVis_Lerp (c0, c1, frac[2]), 1.f);
}

void R_SkyVis_GetTint (vec3_t out_tint)
{
	vec3_t sky_tint;
	const sun_t *sun = R_GetSun ();
	float len;

	VectorCopy (skyflatcolor, sky_tint);
	len = VectorLength (sky_tint);
	if (len < 1e-4f)
		VectorSet (sky_tint, 0.5f, 0.55f, 0.6f);

	if (sun && R_WorldHasSun ())
	{
		vec3_t sun_tint;
		VectorCopy (sun->color, sun_tint);
		if (VectorLength (sun_tint) > 1e-4f)
		{
			VectorNormalize (sun_tint);
			for (int i = 0; i < 3; i++)
				sky_tint[i] = sky_tint[i] * 0.7f + sun_tint[i] * 0.3f;
		}
	}

	for (int i = 0; i < 3; i++)
		out_tint[i] = CLAMP (0.f, sky_tint[i], 1.f);
}

float R_SkyVis_GetResolvedScale (void)
{
	float scale = r_skyvis_scale.value;
	if (scale < 0.f)
		scale = r_sun_visibility.value;
	return CLAMP (0.f, scale, 1.f);
}

float R_SkyVis_GetResolvedCap (void)
{
	return CLAMP (0.f, r_skyvis_cap.value, 1.f);
}

void R_SkyVis_Init (void)
{
	Cvar_RegisterVariable (&r_skyvis);
	Cvar_RegisterVariable (&r_skyvis_debug);
	Cvar_RegisterVariable (&r_skyvis_scale);
	Cvar_RegisterVariable (&r_skyvis_cap);
	Cvar_RegisterVariable (&r_skyvis_spacing_xy);
	Cvar_RegisterVariable (&r_skyvis_spacing_z);
	Cvar_RegisterVariable (&r_skyvis_rays);
}
