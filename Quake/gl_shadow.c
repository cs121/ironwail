#include "quakedef.h"

#include "gl_shadow.h"

#include <math.h>

static const float r_shadow_identity_mat4[16] = {
	1.f, 0.f, 0.f, 0.f,
	0.f, 1.f, 0.f, 0.f,
	0.f, 0.f, 1.f, 0.f,
	0.f, 0.f, 0.f, 1.f
};

int R_Shadow_ClampMapSize (float value)
{
	int size = (int)Q_rint (value);
	size = CLAMP (128, size, 8192);
	return Q_nextPow2 (size);
}

void R_Shadow_MatrixIdentity (float m[16])
{
	memcpy (m, r_shadow_identity_mat4, sizeof (float) * 16);
}

void R_Shadow_MatrixMultiply (float out[16], const float a[16], const float b[16])
{
	int row, col;
	float r[16];
	for (col = 0; col < 4; ++col)
	{
		for (row = 0; row < 4; ++row)
		{
			r[col * 4 + row] =
				a[0 * 4 + row] * b[col * 4 + 0]
				+ a[1 * 4 + row] * b[col * 4 + 1]
				+ a[2 * 4 + row] * b[col * 4 + 2]
				+ a[3 * 4 + row] * b[col * 4 + 3];
		}
	}
	memcpy (out, r, sizeof (r));
}

void R_Shadow_MatrixPerspective (float m[16], float fovy_deg, float aspect, float znear, float zfar)
{
	float f = 1.f / tanf ((fovy_deg * (float)M_PI / 180.f) * 0.5f);
	R_Shadow_MatrixIdentity (m);
	m[0] = f / aspect;
	m[5] = f;
	m[10] = (zfar + znear) / (znear - zfar);
	m[11] = -1.f;
	m[14] = (2.f * zfar * znear) / (znear - zfar);
	m[15] = 0.f;
}

void R_Shadow_MatrixOrtho (float m[16], float left, float right, float bottom, float top, float znear, float zfar)
{
	R_Shadow_MatrixIdentity (m);
	m[0] = 2.f / (right - left);
	m[5] = 2.f / (top - bottom);
	m[10] = -2.f / (zfar - znear);
	m[12] = -(right + left) / (right - left);
	m[13] = -(top + bottom) / (top - bottom);
	m[14] = -(zfar + znear) / (zfar - znear);
}

void R_Shadow_MatrixLook (float m[16], const vec3_t eye, const vec3_t dir, const vec3_t up_hint)
{
	vec3_t fwd, up, side, pos;

	VectorCopy (dir, fwd);
	if (VectorLength (fwd) < 1e-6f)
	{
		VectorSet (fwd, 0.f, 0.f, -1.f);
	}
	VectorNormalize (fwd);

	VectorCopy (up_hint, up);
	if (VectorLength (up) < 1e-6f || fabsf (DotProduct (fwd, up)) > 0.999f)
	{
		VectorSet (up, 0.f, 0.f, 1.f);
		if (fabsf (DotProduct (fwd, up)) > 0.999f)
			VectorSet (up, 0.f, 1.f, 0.f);
	}
	VectorNormalize (up);

	CrossProduct (fwd, up, side);
	if (VectorLength (side) < 1e-6f)
	{
		VectorSet (side, 1.f, 0.f, 0.f);
	}
	VectorNormalize (side);
	CrossProduct (side, fwd, up);
	VectorNormalize (up);

	R_Shadow_MatrixIdentity (m);
	m[0] = side[0]; m[4] = side[1]; m[8] = side[2];
	m[1] = up[0];   m[5] = up[1];   m[9] = up[2];
	m[2] = -fwd[0]; m[6] = -fwd[1]; m[10] = -fwd[2];

	pos[0] = -DotProduct (side, eye);
	pos[1] = -DotProduct (up, eye);
	pos[2] = DotProduct (fwd, eye);
	m[12] = pos[0];
	m[13] = pos[1];
	m[14] = pos[2];
}

void R_Shadow_ExtractFrustumPlane (const float mvp[16], int axis, float ndcval, qboolean flip, mplane_t *out)
{
	float nx = (mvp[0 * 4 + axis] - ndcval * mvp[0 * 4 + 3]);
	float ny = (mvp[1 * 4 + axis] - ndcval * mvp[1 * 4 + 3]);
	float nz = (mvp[2 * 4 + axis] - ndcval * mvp[2 * 4 + 3]);
	float dist = -(mvp[3 * 4 + axis] - ndcval * mvp[3 * 4 + 3]);
	float denom = sqrtf (nx * nx + ny * ny + nz * nz);
	float scale;

	if (denom < 1e-6f)
		denom = 1e-6f;
	scale = (flip ? -1.f : 1.f) / denom;

	out->normal[0] = nx * scale;
	out->normal[1] = ny * scale;
	out->normal[2] = nz * scale;
	out->dist = dist * scale;
	out->type = PLANE_ANYZ;
	out->signbits = 0;
}

void R_Shadow_ExtractFrustum (const float viewproj[16], mplane_t out[4])
{
	R_Shadow_ExtractFrustumPlane (viewproj, 0, 1.f, true, &out[0]);
	R_Shadow_ExtractFrustumPlane (viewproj, 0, -1.f, false, &out[1]);
	R_Shadow_ExtractFrustumPlane (viewproj, 1, -1.f, false, &out[2]);
	R_Shadow_ExtractFrustumPlane (viewproj, 1, 1.f, true, &out[3]);
}
