#include "quakedef.h"
#include "r_fogvol.h"

static qboolean FogVol_BoundsOverlapLocal (const vec3_t mins_a, const vec3_t maxs_a, const vec3_t mins_b, const vec3_t maxs_b)
{
	for (int i = 0; i < 3; ++i)
	{
		if (maxs_a[i] < mins_b[i] || mins_a[i] > maxs_b[i])
			return false;
	}
	return true;
}

static int FogVol_NormalizeShapeLocal (int shape)
{
	return (shape == FOGVOL_SHAPE_SPHERE) ? FOGVOL_SHAPE_SPHERE : FOGVOL_SHAPE_BOX;
}

static void FogVol_GetVolumeBoundsLocal (const fog_volume_t *vol, vec3_t bmins, vec3_t bmaxs)
{
	if (FogVol_NormalizeShapeLocal (vol->shape) == FOGVOL_SHAPE_SPHERE)
	{
		for (int i = 0; i < 3; ++i)
		{
			bmins[i] = vol->sphereCenter[i] - vol->sphereRadius;
			bmaxs[i] = vol->sphereCenter[i] + vol->sphereRadius;
		}
		return;
	}

	VectorCopy (vol->mins, bmins);
	VectorCopy (vol->maxs, bmaxs);
}

static qboolean R_FogVol_GridBoxOverlap (const vec3_t cell_mins, const vec3_t cell_maxs, const fog_volume_t *vol)
{
	return FogVol_BoundsOverlapLocal (cell_mins, cell_maxs, vol->mins, vol->maxs);
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
	if (FogVol_NormalizeShapeLocal (vol->shape) == FOGVOL_SHAPE_SPHERE)
		return R_FogVol_GridSphereOverlap (cell_mins, cell_maxs, vol);
	return R_FogVol_GridBoxOverlap (cell_mins, cell_maxs, vol);
}

void R_FogVol_InjectIntoGrid (froxel_grid_t *grid, const fog_volume_t *vols, int num)
{
	vec3_t cell_size;
	const qboolean inject_debug = (r_fogvol_debug.value >= 7.f);

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
		const float vol_density = q_max (0.f, vol->density);
		const int blend_mode = (vol->blendMode >= 0) ? vol->blendMode : (int)Q_rint (r_fogvol_blendmode.value);

		if (!vol->enabled || vol_density <= 0.f)
			continue;

		FogVol_GetVolumeBoundsLocal (vol, bmins, bmaxs);
		if (!FogVol_BoundsOverlapLocal (bmins, bmaxs, grid->mins, grid->maxs))
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
