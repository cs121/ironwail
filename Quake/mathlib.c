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
// mathlib.c -- math primitives (gehärtet & leicht optimiert)

#include "quakedef.h"

/*
 * Diese Fassung fügt defensive Checks (NaN/Inf/Division-durch-Null),
 * klarere Kommentare und kleine Mikro-Optimierungen hinzu, behält aber
 * alle öffentlichen Signaturen bei, um Binärkompatibilität sicherzustellen.
 */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef EPSILON
#define EPSILON 1e-6f
#endif

 // Hilfs-Makros für sichere Divisionen
#define SAFE_DIV(a,b,fb) (((b) != 0.0f) ? ((a) / (b)) : (fb))

// Hinweis: Einige Makros (LERP, CLAMP, q_min, q_max, DotProduct, VectorSubtract, etc.)
// werden aus quakedef.h erwartet.

vec3_t vec3_origin = { 0,0,0 };
vec4_t vec4_origin = { 0,0,0,0 };

/*-----------------------------------------------------------------*/

static qboolean vec_is_finite3 (const vec3_t v)
{
	return isfinite (v[0]) && isfinite (v[1]) && isfinite (v[2]);
}

static qboolean mat_is_finite16 (const float m[16])
{
	int i; for (i = 0; i < 16; ++i) if (!isfinite (m[i])) return false; return true;
}

void ProjectPointOnPlane (vec3_t dst, const vec3_t p, const vec3_t normal)
{
	float nn = DotProduct (normal, normal);
	if (nn <= EPSILON) // degenerierter Normalvektor
	{
		// Identität: keine Projektion möglich -> Punkt zurückgeben
		_VectorCopy (p, dst);
		return;
	}

	// d = (n·p) / (n·n)
	const float inv_denom = 1.0f / nn;
	const float d = DotProduct (normal, p) * inv_denom;

	vec3_t n;
	n[0] = normal[0] * inv_denom;
	n[1] = normal[1] * inv_denom;
	n[2] = normal[2] * inv_denom;

	dst[0] = p[0] - d * n[0];
	dst[1] = p[1] - d * n[1];
	dst[2] = p[2] - d * n[2];
}

/*
** assumes "src" is normalized (|src| ~= 1)
** Fällt andernfalls robust auf ein sinnvolles Ergebnis zurück.
*/
void PerpendicularVector (vec3_t dst, const vec3_t src)
{
	int pos = 0, i;
	float minelem = 1.0f;
	vec3_t tempvec;

	// Wenn src degeneriert ist, nutze eine feste Achse
	if (VectorLength ((vec3_t) { src[0], src[1], src[2] }) <= EPSILON || !vec_is_finite3 (src))
	{
		dst[0] = 1.0f; dst[1] = 0.0f; dst[2] = 0.0f;
		return;
	}

	/* finde die Achse mit kleinstem Betrag */
	for (i = 0; i < 3; i++)
	{
		const float a = fabsf (src[i]);
		if (a < minelem) { pos = i; minelem = a; }
	}
	tempvec[0] = tempvec[1] = tempvec[2] = 0.0f;
	tempvec[pos] = 1.0f;

	/* projiziere auf die Ebene, die durch src definiert ist */
	ProjectPointOnPlane (dst, tempvec, src);

	/* normalisiere das Ergebnis */
	VectorNormalize (dst);
}

/*-----------------------------------------------------------------*/

float anglemod (float a)
{
	// Bewahrt das klassische Verhalten (16-bit Winkelwrap)
	return (360.0f / 65536.0f) * ((int)(a * (65536.0f / 360.0f)) & 65535);
}

/* === Winkel-Utilities === */
float NormalizeAngle (float degrees)
{
	degrees += 180.f;
	degrees -= floorf (degrees * (1.f / 360.f)) * 360.f; // floorf: korrekt bei negativen
	degrees -= 180.f;
	return degrees;
}

float AngleDifference (float dega, float degb)
{
	return NormalizeAngle (dega - degb);
}

float LerpAngle (float degfrom, float degto, float frac)
{
	return NormalizeAngle (degfrom + AngleDifference (degto, degfrom) * frac);
}

/*-----------------------------------------------------------------*/

/*
==================
BoxOnPlaneSide

Returns 1, 2, or 1 + 2
==================
*/
int BoxOnPlaneSide (vec3_t emins, vec3_t emaxs, mplane_t* p)
{
	float dist1, dist2;
	int xneg, yneg, zneg;
	int sides;

	xneg = p->signbits & 1;
	yneg = (p->signbits >> 1) & 1;
	zneg = (p->signbits >> 2) & 1;

	dist1 = p->normal[0] * (xneg ? emins : emaxs)[0] +
		p->normal[1] * (yneg ? emins : emaxs)[1] +
		p->normal[2] * (zneg ? emins : emaxs)[2];
	dist2 = p->normal[0] * (xneg ? emaxs : emins)[0] +
		p->normal[1] * (yneg ? emaxs : emins)[1] +
		p->normal[2] * (zneg ? emaxs : emins)[2];

	if (p->signbits & ~7)
		Sys_Error ("BoxOnPlaneSide:  Bad signbits");

	sides = 0;
	if (dist1 >= p->dist) sides = 1;
	if (dist2 < p->dist) sides |= 2;
	return sides;
}

// johnfitz -- inverse von AngleVectors. Nimmt forward und erzeugt pitch/yaw/roll.
// TODO: right/up einbeziehen, um yaw/roll korrekt zu bestimmen.
void VectorAngles (const vec3_t forward, vec3_t angles)
{
	vec3_t temp;

	temp[0] = forward[0];
	temp[1] = forward[1];
	temp[2] = 0;
	angles[PITCH] = -atan2f (forward[2], VectorLength (temp)) / M_PI_DIV_180;
	angles[YAW] = atan2f (forward[1], forward[0]) / M_PI_DIV_180;
	angles[ROLL] = 0;
}

void AngleVectors (vec3_t angles, vec3_t forward, vec3_t right, vec3_t up)
{
	float angle;
	float sr, sp, sy, cr, cp, cy;

	angle = angles[YAW] * (M_PI * 2.0f / 360.0f);
	sy = sinf (angle); cy = cosf (angle);
	angle = angles[PITCH] * (M_PI * 2.0f / 360.0f);
	sp = sinf (angle); cp = cosf (angle);
	angle = angles[ROLL] * (M_PI * 2.0f / 360.0f);
	sr = sinf (angle); cr = cosf (angle);

	forward[0] = cp * cy;
	forward[1] = cp * sy;
	forward[2] = -sp;
	right[0] = (-sr * sp * cy + -cr * -sy);
	right[1] = (-sr * sp * sy + -cr * cy);
	right[2] = -sr * cp;
	up[0] = (cr * sp * cy + -sr * -sy);
	up[1] = (cr * sp * sy + -sr * cy);
	up[2] = cr * cp;
}

int VectorCompare (const vec3_t v1, const vec3_t v2)
{
	int i;
	for (i = 0; i < 3; i++) if (v1[i] != v2[i]) return 0; // bitgenau wie Original
	return 1;
}

void VectorMA (const vec3_t veca, float scale, const vec3_t vecb, vec3_t vecc)
{
	vecc[0] = veca[0] + scale * vecb[0];
	vecc[1] = veca[1] + scale * vecb[1];
	vecc[2] = veca[2] + scale * vecb[2];
}

void VectorLerp (const vec3_t veca, const vec3_t vecb, float frac, vec3_t dst)
{
	dst[0] = LERP (veca[0], vecb[0], frac);
	dst[1] = LERP (veca[1], vecb[1], frac);
	dst[2] = LERP (veca[2], vecb[2], frac);
}

vec_t _DotProduct (const vec3_t v1, const vec3_t v2)
{
	return v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2];
}

void _VectorSubtract (const vec3_t veca, const vec3_t vecb, vec3_t out)
{
	out[0] = veca[0] - vecb[0];
	out[1] = veca[1] - vecb[1];
	out[2] = veca[2] - vecb[2];
}

void _VectorAdd (const vec3_t veca, const vec3_t vecb, vec3_t out)
{
	out[0] = veca[0] + vecb[0];
	out[1] = veca[1] + vecb[1];
	out[2] = veca[2] + vecb[2];
}

void _VectorCopy (const vec3_t in, vec3_t out)
{
	out[0] = in[0];
	out[1] = in[1];
	out[2] = in[2];
}

void CrossProduct (const vec3_t v1, const vec3_t v2, vec3_t cross)
{
	cross[0] = v1[1] * v2[2] - v1[2] * v2[1];
	cross[1] = v1[2] * v2[0] - v1[0] * v2[2];
	cross[2] = v1[0] * v2[1] - v1[1] * v2[0];
}

vec_t VectorLength (const vec3_t v)
{
	return sqrtf (DotProduct (v, v));
}

float VectorNormalize (vec3_t v)
{
	float length = sqrtf (DotProduct (v, v));
	if (length > EPSILON)
	{
		float ilength = 1.0f / length;
		v[0] *= ilength; v[1] *= ilength; v[2] *= ilength;
	}
	else
	{
		v[0] = v[1] = 0.0f; v[2] = 1.0f; // fallback-Einheitsvektor
		length = 0.0f;
	}
	return length;
}

float DistanceSquared (const vec3_t a, const vec3_t b)
{
	vec3_t ab; VectorSubtract (b, a, ab); return VectorLengthSquared (ab);
}

float Distance (const vec3_t a, const vec3_t b)
{
	return sqrtf (DistanceSquared (a, b));
}

void VectorInverse (vec3_t v)
{
	v[0] = -v[0]; v[1] = -v[1]; v[2] = -v[2];
}

void VectorScale (const vec3_t in, vec_t scale, vec3_t out)
{
	out[0] = in[0] * scale; out[1] = in[1] * scale; out[2] = in[2] * scale;
}

int Q_log2 (int val)
{
	int answer = 0; if (val <= 0) return 0; while (val >>= 1) answer++; return answer;
}

int Q_nextPow2 (int val)
{
	if (val <= 1) return 1; // robust für val<=0
	val--;
	val |= val >> 1;
	val |= val >> 2;
	val |= val >> 4;
	val |= val >> 8;
	val |= val >> 16;
	val++;
	return val;
}

float GetFraction (float val, float minval, float maxval)
{
	const float den = (maxval - minval);
	return (fabsf (den) > EPSILON) ? ((val - minval) / den) : 0.0f;
}

float GetClampedFraction (float val, float minval, float maxval)
{
	val = GetFraction (val, minval, maxval);
	return CLAMP (0.f, val, 1.f);
}

float Log2f (float val)
{
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
	return log2f (val);
#else
	return logf (val) * 1.4426950408889634f; // 1/ln(2)
#endif
}

float Exp2f (float val)
{
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
	return exp2f (val);
#else
	return expf (val * 0.6931471805599453f); // ln(2)
#endif
}

float GetLogFraction (float val, float minval, float maxval)
{
	if (val <= 0.0f || minval <= 0.0f || maxval <= 0.0f || fabsf (maxval - minval) <= EPSILON)
		return 0.0f;
	return GetFraction (logf (val), logf (minval), logf (maxval));
}

float GetClampedLogFraction (float val, float minval, float maxval)
{
	val = GetLogFraction (val, minval, maxval);
	return CLAMP (0.f, val, 1.f);
}

float LogLerp (float minval, float maxval, float t)
{
	if (minval <= 0.0f || maxval <= 0.0f) return 0.0f;
	return minval * expf (t * logf (maxval / minval));
}

float EaseInOut (float t)
{
	return t * t * (3.f - 2.f * t);
}

/*
==================
Interleave0

Interleaves x with 16 0 bits
==================
*/
uint32_t Interleave0 (uint16_t x)
{
	uint32_t ret = x;
	ret = (ret ^ (ret << 8)) & 0x00FF00FFu;
	ret = (ret ^ (ret << 4)) & 0x0F0F0F0Fu;
	ret = (ret ^ (ret << 2)) & 0x33333333u;
	ret = (ret ^ (ret << 1)) & 0x55555555u;
	return ret;
}

/*
==================
Interleave

Interleaves 2 16-bit integers
==================
*/
uint32_t Interleave (uint16_t even, uint16_t odd)
{
	return Interleave0 (even) | (Interleave0 (odd) << 1);
}

/*
==================
DeinterleaveEven

Deinterleaves the even 16 bits of x (bits 0,2,4..28,30)
==================
*/
uint16_t DeinterleaveEven (uint32_t x)
{
	x &= 0x55555555u;
	x = (x ^ (x >> 1u)) & 0x33333333u;
	x = (x ^ (x >> 2u)) & 0x0F0F0F0Fu;
	x = (x ^ (x >> 4u)) & 0x00FF00FFu;
	x = (x ^ (x >> 8u)) & 0x0000FFFFu;
	return (uint16_t)x;
}

/*
==================
DecodeMortonIndex

Extracts 2 8-bit coordinates from a 16-bit Z-order index
==================
*/
void DecodeMortonIndex (uint16_t index, int* x, int* y)
{
	uint32_t evenodd = index | ((index >> 1) << 16);
	index = DeinterleaveEven (evenodd);
	*x = index & 255;
	*y = index >> 8;
}

/*
================
R_ConcatRotations
================
*/
void R_ConcatRotations (float in1[3][3], float in2[3][3], float out[3][3])
{
	out[0][0] = in1[0][0] * in2[0][0] + in1[0][1] * in2[1][0] + in1[0][2] * in2[2][0];
	out[0][1] = in1[0][0] * in2[0][1] + in1[0][1] * in2[1][1] + in1[0][2] * in2[2][1];
	out[0][2] = in1[0][0] * in2[0][2] + in1[0][1] * in2[1][2] + in1[0][2] * in2[2][2];
	out[1][0] = in1[1][0] * in2[0][0] + in1[1][1] * in2[1][0] + in1[1][2] * in2[2][0];
	out[1][1] = in1[1][0] * in2[0][1] + in1[1][1] * in2[1][1] + in1[1][2] * in2[2][1];
	out[1][2] = in1[1][0] * in2[0][2] + in1[1][1] * in2[1][2] + in1[1][2] * in2[2][2];
	out[2][0] = in1[2][0] * in2[0][0] + in1[2][1] * in2[1][0] + in1[2][2] * in2[2][0];
	out[2][1] = in1[2][0] * in2[0][1] + in1[2][1] * in2[1][1] + in1[2][2] * in2[2][1];
	out[2][2] = in1[2][0] * in2[0][2] + in1[2][1] * in2[1][2] + in1[2][2] * in2[2][2];
}

/*
================
R_ConcatTransforms
================
*/
void R_ConcatTransforms (float in1[3][4], float in2[3][4], float out[3][4])
{
	out[0][0] = in1[0][0] * in2[0][0] + in1[0][1] * in2[1][0] + in1[0][2] * in2[2][0];
	out[0][1] = in1[0][0] * in2[0][1] + in1[0][1] * in2[1][1] + in1[0][2] * in2[2][1];
	out[0][2] = in1[0][0] * in2[0][2] + in1[0][1] * in2[1][2] + in1[0][2] * in2[2][2];
	out[0][3] = in1[0][0] * in2[0][3] + in1[0][1] * in2[1][3] + in1[0][2] * in2[2][3] + in1[0][3];
	out[1][0] = in1[1][0] * in2[0][0] + in1[1][1] * in2[1][0] + in1[1][2] * in2[2][0];
	out[1][1] = in1[1][0] * in2[0][1] + in1[1][1] * in2[1][1] + in1[1][2] * in2[2][1];
	out[1][2] = in1[1][0] * in2[0][2] + in1[1][1] * in2[1][2] + in1[1][2] * in2[2][2];
	out[1][3] = in1[1][0] * in2[0][3] + in1[1][1] * in2[1][3] + in1[1][2] * in2[2][3] + in1[1][3];
	out[2][0] = in1[2][0] * in2[0][0] + in1[2][1] * in2[1][0] + in1[2][2] * in2[2][0];
	out[2][1] = in1[2][0] * in2[0][1] + in1[2][1] * in2[1][1] + in1[2][2] * in2[2][1];
	out[2][2] = in1[2][0] * in2[0][2] + in1[2][1] * in2[1][2] + in1[2][2] * in2[2][2];
	out[2][3] = in1[2][0] * in2[0][3] + in1[2][1] * in2[1][3] + in1[2][2] * in2[2][3] + in1[2][3];
}

/*
===================
FloorDivMod
====================
*/
void FloorDivMod (double numer, double denom, int* quotient, int* rem)
{
	int q, r; double x;
	if (denom <= 0.0)
		Sys_Error ("FloorDivMod: bad denominator %f\n", denom);

	if (numer >= 0.0)
	{
		x = floor (numer / denom);
		q = (int)x;
		r = (int)floor (numer - (x * denom));
	}
	else
	{
		x = floor (-numer / denom);
		q = -(int)x;
		r = (int)floor (-numer - (x * denom));
		if (r != 0)
		{
			q--;
			r = (int)denom - r;
		}
	}
	*quotient = q; *rem = r;
}

/*
===================
GreatestCommonDivisor
====================
*/
int GreatestCommonDivisor (int i1, int i2)
{
	if (i1 < 0) i1 = -i1; if (i2 < 0) i2 = -i2;
	if (i1 > i2)
	{
		if (i2 == 0) return i1;
		return GreatestCommonDivisor (i2, i1 % i2);
	}
	else
	{
		if (i1 == 0) return i2;
		return GreatestCommonDivisor (i1, i2 % i1);
	}
}

/*
===================
Invert24To16

Inverts an 8.24 value to a 16.16 value
====================
*/
fixed16_t Invert24To16 (fixed16_t val)
{
	if (val < 256) return (fixed16_t)0xFFFFFFFFu;
	return (fixed16_t)(((double)0x10000 * (double)0x1000000 / (double)val) + 0.5);
}

/*
===================
MatrixMultiply
====================
*/
void MatrixMultiply (float left[16], float right[16])
{
#ifdef USE_SSE2
	if (use_simd)
	{
		// In-Place mit Zwischenspeicher für linken Operanden, um Aliasing zu vermeiden
		float lcopy[16]; memcpy (lcopy, left, sizeof (lcopy));
		__m128 leftcol0 = _mm_loadu_ps (lcopy + 0);
		__m128 leftcol1 = _mm_loadu_ps (lcopy + 4);
		__m128 leftcol2 = _mm_loadu_ps (lcopy + 8);
		__m128 leftcol3 = _mm_loadu_ps (lcopy + 12);

#define VBROADCAST(vec,col) _mm_shuffle_ps (vec, vec, _MM_SHUFFLE (col, col, col, col))
		int i;
		for (i = 0; i < 4; ++i)
		{
			__m128 rightcol = _mm_loadu_ps (right + i * 4);
			__m128 c0 = _mm_mul_ps (leftcol0, VBROADCAST (rightcol, 0));
			__m128 c1 = _mm_mul_ps (leftcol1, VBROADCAST (rightcol, 1));
			__m128 c2 = _mm_mul_ps (leftcol2, VBROADCAST (rightcol, 2));
			__m128 c3 = _mm_mul_ps (leftcol3, VBROADCAST (rightcol, 3));
			c0 = _mm_add_ps (c0, c1);
			c2 = _mm_add_ps (c2, c3);
			c0 = _mm_add_ps (c0, c2);
			_mm_storeu_ps (left + i * 4, c0);
		}
#undef VBROADCAST
	}
	else
#endif
	{
		float temp[16];
		int column, row, i;
		memcpy (temp, left, sizeof (temp));
		for (row = 0; row < 4; ++row)
		{
			for (column = 0; column < 4; ++column)
			{
				float value = 0.0f;
				for (i = 0; i < 4; ++i)
					value += temp[i * 4 + row] * right[column * 4 + i];
				left[column * 4 + row] = value;
			}
		}
	}
}

/*
=============
RotationMatrix
=============
*/
void RotationMatrix (float matrix[16], float angle, int axis)
{
	const float c = cosf (angle);
	const float s = sinf (angle);
	const int i = (axis + 1) % 3;
	const int j = (axis + 2) % 3;

	IdentityMatrix (matrix);
	matrix[i * 4 + i] = c;
	matrix[j * 4 + j] = c;
	matrix[j * 4 + i] = -s;
	matrix[i * 4 + j] = s;
}

/*
=============
TranslationMatrix
=============
*/
void TranslationMatrix (float matrix[16], float x, float y, float z)
{
	memset (matrix, 0, 16 * sizeof (float));
	matrix[0 * 4 + 0] = 1.0f;
	matrix[1 * 4 + 1] = 1.0f;
	matrix[2 * 4 + 2] = 1.0f;
	matrix[3 * 4 + 0] = x;
	matrix[3 * 4 + 1] = y;
	matrix[3 * 4 + 2] = z;
	matrix[3 * 4 + 3] = 1.0f;
}

/*
=============
ScaleMatrix
=============
*/
void ScaleMatrix (float matrix[16], float x, float y, float z)
{
	memset (matrix, 0, 16 * sizeof (float));
	matrix[0 * 4 + 0] = x;
	matrix[1 * 4 + 1] = y;
	matrix[2 * 4 + 2] = z;
	matrix[3 * 4 + 3] = 1.0f;
}

/*
=============
IdentityMatrix
=============
*/
void IdentityMatrix (float matrix[16])
{
	memset (matrix, 0, 16 * sizeof (float));
	matrix[0 * 4 + 0] = 1.0f;
	matrix[1 * 4 + 1] = 1.0f;
	matrix[2 * 4 + 2] = 1.0f;
	matrix[3 * 4 + 3] = 1.0f;
}

/*
=============
ApplyScale
=============
*/
void ApplyScale (float matrix[16], float x, float y, float z)
{
	matrix[0 * 4 + 0] *= x; matrix[0 * 4 + 1] *= x; matrix[0 * 4 + 2] *= x; matrix[0 * 4 + 3] *= x;
	matrix[1 * 4 + 0] *= y; matrix[1 * 4 + 1] *= y; matrix[1 * 4 + 2] *= y; matrix[1 * 4 + 3] *= y;
	matrix[2 * 4 + 0] *= z; matrix[2 * 4 + 1] *= z; matrix[2 * 4 + 2] *= z; matrix[2 * 4 + 3] *= z;
}

/*
=============
ApplyTranslation
=============
*/
void ApplyTranslation (float matrix[16], float x, float y, float z)
{
#ifdef USE_SSE2
	if (use_simd)
	{
		__m128 v0 = _mm_loadu_ps (matrix + 0 * 4);
		__m128 v1 = _mm_loadu_ps (matrix + 1 * 4);
		__m128 v2 = _mm_loadu_ps (matrix + 2 * 4);
		__m128 v3 = _mm_loadu_ps (matrix + 3 * 4);

		v3 = _mm_add_ps (v3, _mm_mul_ps (v0, _mm_set1_ps (x)));
		v3 = _mm_add_ps (v3, _mm_mul_ps (v1, _mm_set1_ps (y)));
		v3 = _mm_add_ps (v3, _mm_mul_ps (v2, _mm_set1_ps (z)));

		_mm_storeu_ps (matrix + 3 * 4, v3);
	}
	else
#endif
	{
		matrix[3 * 4 + 0] += x * matrix[0 * 4 + 0];
		matrix[3 * 4 + 1] += x * matrix[0 * 4 + 1];
		matrix[3 * 4 + 2] += x * matrix[0 * 4 + 2];
		matrix[3 * 4 + 3] += x * matrix[0 * 4 + 3];

		matrix[3 * 4 + 0] += y * matrix[1 * 4 + 0];
		matrix[3 * 4 + 1] += y * matrix[1 * 4 + 1];
		matrix[3 * 4 + 2] += y * matrix[1 * 4 + 2];
		matrix[3 * 4 + 3] += y * matrix[1 * 4 + 3];

		matrix[3 * 4 + 0] += z * matrix[2 * 4 + 0];
		matrix[3 * 4 + 1] += z * matrix[2 * 4 + 1];
		matrix[3 * 4 + 2] += z * matrix[2 * 4 + 2];
		matrix[3 * 4 + 3] += z * matrix[2 * 4 + 3];
	}
}

/*
=============
ProjectVector
=============
*/
void ProjectVector (const vec3_t src, const float matrix[16], vec3_t dst)
{
	float w;
	vec4_t proj;

	proj[0] = matrix[3 * 4 + 0];
	proj[1] = matrix[3 * 4 + 1];
	proj[2] = matrix[3 * 4 + 2];
	proj[3] = matrix[3 * 4 + 3];

	proj[0] += src[0] * matrix[0 * 4 + 0];
	proj[1] += src[0] * matrix[0 * 4 + 1];
	proj[2] += src[0] * matrix[0 * 4 + 2];
	proj[3] += src[0] * matrix[0 * 4 + 3];

	proj[0] += src[1] * matrix[1 * 4 + 0];
	proj[1] += src[1] * matrix[1 * 4 + 1];
	proj[2] += src[1] * matrix[1 * 4 + 2];
	proj[3] += src[1] * matrix[1 * 4 + 3];

	proj[0] += src[2] * matrix[2 * 4 + 0];
	proj[1] += src[2] * matrix[2 * 4 + 1];
	proj[2] += src[2] * matrix[2 * 4 + 2];
	proj[3] += src[2] * matrix[2 * 4 + 3];

	w = fabsf (proj[3]);
	if (w <= EPSILON)
	{
		// Punkt liegt auf/nahe der Projektionsebene -> setze X/Y auf 0, gebe w zurück
		dst[0] = 0.0f; dst[1] = 0.0f; dst[2] = proj[3];
		return;
	}

	dst[0] = proj[0] / w;
	dst[1] = proj[1] / w;
	dst[2] = proj[3]; // Bewahrt bestehendes Verhalten (w in Z ablegen)
}

void MatrixTranspose4x3 (const float src[16], float dst[12])
{
#define COPY_ROW(row) \
		dst[row*4+0] = src[row+0], \
		dst[row*4+1] = src[row+4], \
		dst[row*4+2] = src[row+8], \
		dst[row*4+3] = src[row+12]

	COPY_ROW (0);
	COPY_ROW (1);
	COPY_ROW (2);

#undef COPY_ROW
}

qboolean RayVsBox (const vec3_t org, const vec3_t rcpdelta, const vec3_t mins, const vec3_t maxs, float* frac)
{
	int i; float enter = 0.f, exit = 1.f;
	if (frac) *frac = 1.f;

	for (i = 0; i < 3; i++)
	{
		const float invd = rcpdelta[i];
		float t0 = (mins[i] - org[i]) * invd;
		float t1 = (maxs[i] - org[i]) * invd;
		if (!isfinite (t0) || !isfinite (t1))
		{
			// Parallel zur Achse (delta ~ 0 -> rcpdelta ~ inf). Wenn org außerhalb, kein Hit.
			if (org[i] < mins[i] - EPSILON || org[i] > maxs[i] + EPSILON)
				return false;
			// sonst ignorieren und nächste Achse prüfen
			continue;
		}
		const float tmin = q_min (t0, t1);
		const float tmax = q_max (t0, t1);
		enter = q_max (enter, tmin);
		exit = q_min (exit, tmax);
	}

	if (enter > exit) return false;
	if (frac) *frac = enter;
	return true;
}
