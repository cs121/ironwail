#include "quakedef.h"
#include "renderer/r_envlight.h"
#include "opengl/gl_lightgrid.h"

qboolean R_EnvLight_SampleEntityAmbient (const entity_t *e, vec3_t out_rgb_linear, float *out_ao)
{
	const lightgrid_t *lg;
	lightgrid_probe_t probe;
	vec3_t sample_pos;
	float ao = 1.f;

	if (out_ao)
		*out_ao = 1.f;
	if (out_rgb_linear)
		VectorSet (out_rgb_linear, 0.f, 0.f, 0.f);

	if (!e || !out_rgb_linear)
		return false;

	if (!r_model_lightgrid.value)
		return false;

	lg = Lightgrid_Get ();
	if (!lg || !r_lightgrid.value)
		return false;

	VectorCopy (e->origin, sample_pos);

	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (!Lightgrid_SampleProbe (lg, sample_pos, &probe))
			break;

		VectorCopy (probe.rgb, out_rgb_linear);
		ao = CLAMP (0.f, probe.ao, 1.f);
		if (probe.intensity > 0.f || ao > 0.f)
			break;

		if (attempt == 1 || !e->model)
			break;

		const float ofs = e->model->maxs[2] * 0.5f;
		if (ofs <= 0.f)
			break;

		sample_pos[2] += ofs;
	}

	if (out_ao)
		*out_ao = ao;

	return (out_rgb_linear[0] + out_rgb_linear[1] + out_rgb_linear[2]) > 0.f || ao > 0.f;
}
