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
// mathlib.c -- math primitives

#include "quakedef.h"

vec3_t vec3_origin = {0,0,0};
vec4_t vec4_origin = {0,0,0,0};

/*-----------------------------------------------------------------*/


void ProjectPointOnPlane( vec3_t dst, const vec3_t p, const vec3_t normal )
{
	float d;
	vec3_t n;
	float inv_denom;

	inv_denom = 1.0F / DotProduct( normal, normal );

	d = DotProduct( normal, p ) * inv_denom;

	n[0] = normal[0] * inv_denom;
	n[1] = normal[1] * inv_denom;
	n[2] = normal[2] * inv_denom;

	dst[0] = p[0] - d * n[0];
	dst[1] = p[1] - d * n[1];
	dst[2] = p[2] - d * n[2];
}

/*
** assumes "src" is normalized
*/
void PerpendicularVector( vec3_t dst, const vec3_t src )
{
	int	pos;
	int i;
	float minelem = 1.0F;
	vec3_t tempvec;

	/*
	** find the smallest magnitude axially aligned vector
	*/
	for ( pos = 0, i = 0; i < 3; i++ )
	{
		if ( fabs( src[i] ) < minelem )
		{
			pos = i;
			minelem = fabs( src[i] );
		}
	}
	tempvec[0] = tempvec[1] = tempvec[2] = 0.0F;
	tempvec[pos] = 1.0F;

	/*
	** project the point onto the plane defined by src
	*/
	ProjectPointOnPlane( dst, tempvec, src );

	/*
	** normalize the result
	*/
	VectorNormalize( dst );
}

//johnfitz -- removed RotatePointAroundVector() becuase it's no longer used and my compiler fucked it up anyway

/*-----------------------------------------------------------------*/


float	anglemod(float a)
{
#if 0
	if (a >= 0)
		a -= 360*(int)(a/360);
	else
		a += 360*( 1 + (int)(-a/360) );
#endif
	a = (360.0/65536) * ((int)(a*(65536/360.0)) & 65535);
	return a;
}

/*
==================
NormalizeAngle

Returns a value between -180 and 180
==================
*/
float NormalizeAngle (float degrees)
{
	degrees += 180.f;
	// Note: can't use fmod because of the way it handles negative values
	degrees -= floor (degrees * (1.f/360.f)) * 360.f;
	degrees -= 180.f;
	return degrees;
}

/*
==================
AngleDifference

Returns a value between -180 and 180
==================
*/
float AngleDifference (float dega, float degb)
{
	return NormalizeAngle (dega - degb);
}

/*
==================
LerpAngle

Returns a value between -180 and 180
==================
*/
float LerpAngle (float degfrom, float degto, float frac)
{
	return NormalizeAngle (degfrom + AngleDifference (degto, degfrom) * frac);
}


/*
==================
BoxOnPlaneSide

Returns 1, 2, or 1 + 2
==================
*/
int BoxOnPlaneSide (vec3_t emins, vec3_t emaxs, mplane_t *p)
{
	float	dist1, dist2;
	int		xneg, yneg, zneg;
	int		sides;

#if 0	// this is done by the BOX_ON_PLANE_SIDE macro before calling this
		// function
// fast axial cases
	if (p->type < 3)
	{
		if (p->dist <= emins[p->type])
			return 1;
		if (p->dist >= emaxs[p->type])
			return 2;
		return 3;
	}
#endif

	if (p->signbits & ~7)
		Sys_Error ("BoxOnPlaneSide:  Bad signbits");

	xneg = p->signbits & 1;
	yneg = (p->signbits >> 1) & 1;
	zneg = (p->signbits >> 2) & 1;

	dist1 = p->normal[0] * (xneg ? emins : emaxs)[0] +
			p->normal[1] * (yneg ? emins : emaxs)[1] +
			p->normal[2] * (zneg ? emins : emaxs)[2];
	dist2 = p->normal[0] * (xneg ? emaxs : emins)[0] +
			p->normal[1] * (yneg ? emaxs : emins)[1] +
			p->normal[2] * (zneg ? emaxs : emins)[2];

	sides = 0;
	if (dist1 >= p->dist)
		sides = 1;
	if (dist2 < p->dist)
		sides |= 2;

#ifdef PARANOID
	if (sides == 0)
		Sys_Error ("BoxOnPlaneSide: sides==0");
#endif

	return sides;
}

//johnfitz -- the opposite of AngleVectors.  this takes forward and generates pitch yaw roll
//TODO: take right and up vectors to properly set yaw and roll
void VectorAngles (const vec3_t forward, vec3_t angles)
{
	angles[PITCH] = -atan2f(forward[2], hypotf(forward[0], forward[1])) / M_PI_DIV_180;
	angles[YAW]   =  atan2f(forward[1], forward[0]) / M_PI_DIV_180;
	angles[ROLL]  = 0;
}

void AngleVectors (vec3_t angles, vec3_t forward, vec3_t right, vec3_t up)
{
	float		sr, sp, sy, cr, cp, cy;

	sy = sinf(angles[YAW]   * M_PI_DIV_180);
	cy = cosf(angles[YAW]   * M_PI_DIV_180);
	sp = sinf(angles[PITCH] * M_PI_DIV_180);
	cp = cosf(angles[PITCH] * M_PI_DIV_180);
	sr = sinf(angles[ROLL]  * M_PI_DIV_180);
	cr = cosf(angles[ROLL]  * M_PI_DIV_180);

	forward[0] = cp*cy;
	forward[1] = cp*sy;
	forward[2] = -sp;
	right[0] = cr*sy  - sr*sp*cy;
	right[1] = -cr*cy - sr*sp*sy;
	right[2] = -sr*cp;
	up[0] = cr*sp*cy + sr*sy;
	up[1] = cr*sp*sy - sr*cy;
	up[2] = cr*cp;
}

int VectorCompare (const vec3_t v1, const vec3_t v2)
{
	int		i;

	for (i=0 ; i<3 ; i++)
		if (v1[i] != v2[i])
			return 0;

	return 1;
}

void VectorMA (const vec3_t veca, float scale, const vec3_t vecb, vec3_t vecc)
{
#ifdef USE_SSE2
	if (use_simd)
	{
		__m128 a = _mm_loadu_ps (veca);
		__m128 b = _mm_loadu_ps (vecb);
		__m128 s = _mm_set_ps1 (scale);
		_mm_storeu_ps (vecc, _mm_add_ps (a, _mm_mul_ps (s, b)));
		return;
	}
#endif
	vecc[0] = veca[0] + scale*vecb[0];
	vecc[1] = veca[1] + scale*vecb[1];
	vecc[2] = veca[2] + scale*vecb[2];
}

void VectorLerp (const vec3_t veca, const vec3_t vecb, float frac, vec3_t dst)
{
#ifdef USE_SSE2
	if (use_simd)
	{
		__m128 a = _mm_loadu_ps (veca);
		__m128 b = _mm_loadu_ps (vecb);
		__m128 f = _mm_set_ps1 (frac);
		/* dst = a + frac*(b-a) */
		_mm_storeu_ps (dst, _mm_add_ps (a, _mm_mul_ps (f, _mm_sub_ps (b, a))));
		return;
	}
#endif
	dst[0] = LERP (veca[0], vecb[0], frac);
	dst[1] = LERP (veca[1], vecb[1], frac);
	dst[2] = LERP (veca[2], vecb[2], frac);
}

void VectorAverage (const vec3_t veca, const vec3_t vecb, vec3_t dst)
{
        dst[0] = 0.5f * (veca[0] + vecb[0]);
        dst[1] = 0.5f * (veca[1] + vecb[1]);
        dst[2] = 0.5f * (veca[2] + vecb[2]);
}


vec_t _DotProduct (const vec3_t v1, const vec3_t v2)
{
return v1[0]*v2[0] + v1[1]*v2[1] + v1[2]*v2[2];
}

void _VectorSubtract (const vec3_t veca, const vec3_t vecb, vec3_t out)
{
	out[0] = veca[0]-vecb[0];
	out[1] = veca[1]-vecb[1];
	out[2] = veca[2]-vecb[2];
}

void _VectorAdd (const vec3_t veca, const vec3_t vecb, vec3_t out)
{
	out[0] = veca[0]+vecb[0];
	out[1] = veca[1]+vecb[1];
	out[2] = veca[2]+vecb[2];
}

void _VectorCopy (const vec3_t in, vec3_t out)
{
	out[0] = in[0];
	out[1] = in[1];
	out[2] = in[2];
}

void CrossProduct (const vec3_t v1, const vec3_t v2, vec3_t cross)
{
#ifdef USE_SSE2
	if (use_simd)
	{
		/* Load v1 = (x1,y1,z1,0), v2 = (x2,y2,z2,0)
		   cross = (y1*z2 - z1*y2,  z1*x2 - x1*z2,  x1*y2 - y1*x2)
		   Achieved with two shuffles + mul + sub                    */
		__m128 a = _mm_loadu_ps (v1); /* x1 y1 z1 ? */
		__m128 b = _mm_loadu_ps (v2); /* x2 y2 z2 ? */

		__m128 a_yzx = _mm_shuffle_ps (a, a, _MM_SHUFFLE (3, 0, 2, 1)); /* y1 z1 x1 ? */
		__m128 b_yzx = _mm_shuffle_ps (b, b, _MM_SHUFFLE (3, 0, 2, 1)); /* y2 z2 x2 ? */
		__m128 a_zxy = _mm_shuffle_ps (a, a, _MM_SHUFFLE (3, 1, 0, 2)); /* z1 x1 y1 ? */
		__m128 b_zxy = _mm_shuffle_ps (b, b, _MM_SHUFFLE (3, 1, 0, 2)); /* z2 x2 y2 ? */

		__m128 result = _mm_sub_ps (_mm_mul_ps (a_yzx, b_zxy),
		                            _mm_mul_ps (a_zxy, b_yzx));
		/* result = (y1*z2-z1*y2, z1*x2-x1*z2, x1*y2-y1*x2, ?) */
		_mm_storeu_ps (cross, result); /* stores 4 floats; 4th is garbage but vec3_t has room */
		return;
	}
#endif
	cross[0] = v1[1]*v2[2] - v1[2]*v2[1];
	cross[1] = v1[2]*v2[0] - v1[0]*v2[2];
	cross[2] = v1[0]*v2[1] - v1[1]*v2[0];
}

vec_t VectorLength(const vec3_t v)
{
#ifdef USE_SSE41
	if (use_simd)
	{
		__m128 a = _mm_loadu_ps (v);
		/* _mm_dp_ps: dot product of xyz (mask 0x71 = use xyz, store in lane 0) */
		__m128 dot = _mm_dp_ps (a, a, 0x71);
		return _mm_cvtss_f32 (_mm_sqrt_ss (dot));
	}
#endif
	return sqrtf(DotProduct(v,v));
}

float VectorNormalize (vec3_t v)
{
#ifdef USE_SSE41
	if (use_simd)
	{
		__m128 a   = _mm_loadu_ps (v);
		__m128 dot = _mm_dp_ps (a, a, 0x77); /* dot in all lanes, xyz only */
		/* rsqrt estimate (~11-bit precision), refined once with Newton-Raphson:
		   nr = est * (1.5 - 0.5 * x * est^2)  -->  ~23-bit accuracy         */
		__m128 est  = _mm_rsqrt_ps (dot);
		__m128 half = _mm_set_ps1 (0.5f);
		__m128 three= _mm_set_ps1 (3.0f);
		__m128 nr   = _mm_mul_ps (est, _mm_sub_ps (three,
		                _mm_mul_ps (dot, _mm_mul_ps (est, est))));
		nr = _mm_mul_ps (nr, half); /* nr = refined 1/sqrt(dot) */
		__m128 norm = _mm_mul_ps (a, nr);
		_mm_storeu_ps (v, norm);
		/* return the actual length = 1 / nr[0] */
		return _mm_cvtss_f32 (_mm_rcp_ss (_mm_shuffle_ps (nr, nr, 0)));
	}
#endif
	{
		float length, ilength;
		length = sqrtf(DotProduct(v,v));
		if (length)
		{
			ilength = 1/length;
			v[0] *= ilength;
			v[1] *= ilength;
			v[2] *= ilength;
		}
		return length;
	}
}

float DistanceSquared (const vec3_t a, const vec3_t b)
{
	vec3_t ab;
	VectorSubtract (b, a, ab);
	return VectorLengthSquared (ab);
}

float Distance (const vec3_t a, const vec3_t b)
{
	return sqrtf (DistanceSquared (a, b));
}

void VectorInverse (vec3_t v)
{
	v[0] = -v[0];
	v[1] = -v[1];
	v[2] = -v[2];
}

void VectorScale (const vec3_t in, vec_t scale, vec3_t out)
{
#ifdef USE_SSE2
	if (use_simd)
	{
		__m128 v = _mm_loadu_ps (in);
		__m128 s = _mm_set_ps1 (scale);
		_mm_storeu_ps (out, _mm_mul_ps (v, s));
		return;
	}
#endif
	out[0] = in[0]*scale;
	out[1] = in[1]*scale;
	out[2] = in[2]*scale;
}


int Q_log2(int val)
{
#if defined(__GNUC__) || defined(__clang__)
	return (val > 1) ? (31 - __builtin_clz((unsigned int)val)) : 0;
#else
	int answer = 0;
	while (val >>= 1)
		answer++;
	return answer;
#endif
}

int Q_nextPow2(int val)
{
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
	return (val - minval) / (maxval - minval);
}

float GetClampedFraction (float val, float minval, float maxval)
{
	val = GetFraction (val, minval, maxval);
	return CLAMP (0.f, val, 1.f);
}

float Log2f (float val)
{
	return log2f (val);
}

float Exp2f (float val)
{
	return exp2f (val);
}

float GetLogFraction (float val, float minval, float maxval)
{
	return logf (val / minval) / logf (maxval / minval);
}

float GetClampedLogFraction (float val, float minval, float maxval)
{
	val = GetLogFraction (val, minval, maxval);
	return CLAMP (0.f, val, 1.f);
}

float Lerp (float minval, float maxval, float t)
{
	return minval + (maxval - minval) * t;
}

float LogLerp (float minval, float maxval, float t)
{
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
	ret = (ret ^ (ret << 8)) & 0x00FF00FF;
	ret = (ret ^ (ret << 4)) & 0x0F0F0F0F;
	ret = (ret ^ (ret << 2)) & 0x33333333;
	ret = (ret ^ (ret << 1)) & 0x55555555;
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
	return (uint16_t) x;
}

/*
==================
DecodeMortonIndex

Extracts 2 8-bit coordinates from a 16-bit Z-order index
==================
*/
void DecodeMortonIndex (uint16_t index, int *x, int *y)
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
#ifdef USE_SSE2
	if (use_simd)
	{
		/* Each row of out = dot(row of in1, cols of in2)
		   We process one row of in1 at a time.
		   in2 columns: col0=(in2[0][0],in2[1][0],in2[2][0]), etc.
		   But in2 is stored row-major [row][col], so in2[r][c].
		   col j of in2 = (in2[0][j], in2[1][j], in2[2][j])              */

		/* out[i][j] = dot(in1[i], col_j_of_in2) */

		int i, j;
		for (i = 0; i < 3; i++)
		{
			__m128 row1 = _mm_set_ps (0, in1[i][2], in1[i][1], in1[i][0]);
			for (j = 0; j < 3; j++)
			{
				__m128 col2;
				if      (j == 0) col2 = _mm_set_ps (0, in2[2][0], in2[1][0], in2[0][0]);
				else if (j == 1) col2 = _mm_set_ps (0, in2[2][1], in2[1][1], in2[0][1]);
				else             col2 = _mm_set_ps (0, in2[2][2], in2[1][2], in2[0][2]);
				__m128 prod = _mm_mul_ps (row1, col2);
				/* horizontal add xyz */
				__m128 shuf = _mm_shuffle_ps (prod, prod, _MM_SHUFFLE (2, 3, 0, 1));
				__m128 sums = _mm_add_ps (prod, shuf);
				shuf = _mm_movehl_ps (shuf, sums);
				sums = _mm_add_ss (sums, shuf);
				out[i][j] = _mm_cvtss_f32 (sums);
			}
		}
		return;
	}
#endif
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
#ifdef USE_SSE2
	if (use_simd)
	{
		/* in2 has an implicit 4th row [0 0 0 1].
		   out[i][0..2] = in1[i][0..2] dot cols 0..2 of in2 (3x3 part)
		   out[i][3]    = in1[i][0..2] dot in2_col3 + in1[i][3]         */
		int i;
		for (i = 0; i < 3; i++)
		{
			__m128 row1 = _mm_loadu_ps (in1[i]); /* x y z w */

			/* Compute 3x3 part: out[i][j] = dot(in1[i][0..2], col_j_in2) */
			/* col 0 of in2 (rotation part) */
			__m128 c0 = _mm_set_ps (0, in2[2][0], in2[1][0], in2[0][0]);
			__m128 c1 = _mm_set_ps (0, in2[2][1], in2[1][1], in2[0][1]);
			__m128 c2 = _mm_set_ps (0, in2[2][2], in2[1][2], in2[0][2]);
			/* col 3 of in2 (translation part) */
			__m128 c3 = _mm_set_ps (0, in2[2][3], in2[1][3], in2[0][3]);

			__m128 r   = _mm_and_ps (row1, _mm_castsi128_ps (_mm_set_epi32 (0, -1, -1, -1)));

			{ __m128 p = _mm_mul_ps (r, c0); __m128 s = _mm_shuffle_ps (p, p, _MM_SHUFFLE (2, 3, 0, 1)); p = _mm_add_ps (p, s); s = _mm_movehl_ps (s, p); p = _mm_add_ss (p, s); out[i][0] = _mm_cvtss_f32 (p); }
			{ __m128 p = _mm_mul_ps (r, c1); __m128 s = _mm_shuffle_ps (p, p, _MM_SHUFFLE (2, 3, 0, 1)); p = _mm_add_ps (p, s); s = _mm_movehl_ps (s, p); p = _mm_add_ss (p, s); out[i][1] = _mm_cvtss_f32 (p); }
			{ __m128 p = _mm_mul_ps (r, c2); __m128 s = _mm_shuffle_ps (p, p, _MM_SHUFFLE (2, 3, 0, 1)); p = _mm_add_ps (p, s); s = _mm_movehl_ps (s, p); p = _mm_add_ss (p, s); out[i][2] = _mm_cvtss_f32 (p); }
			{ __m128 p = _mm_mul_ps (r, c3); __m128 s = _mm_shuffle_ps (p, p, _MM_SHUFFLE (2, 3, 0, 1)); p = _mm_add_ps (p, s); s = _mm_movehl_ps (s, p); p = _mm_add_ss (p, s); out[i][3] = _mm_cvtss_f32 (p) + in1[i][3]; }
		}
		return;
	}
#endif
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

Returns mathematically correct (floor-based) quotient and remainder for
numer and denom, both of which should contain no fractional part. The
quotient must fit in 32 bits.
====================
*/

void FloorDivMod (double numer, double denom, int *quotient,
		int *rem)
{
	int		q, r;
	double	x;

#ifndef PARANOID
	if (denom <= 0.0)
		Sys_Error ("FloorDivMod: bad denominator %f\n", denom);

//	if ((floor(numer) != numer) || (floor(denom) != denom))
//		Sys_Error ("FloorDivMod: non-integer numer or denom %f %f\n",
//				numer, denom);
#endif

	if (numer >= 0.0)
	{

		x = floor(numer / denom);
		q = (int)x;
		r = (int)floor(numer - (x * denom));
	}
	else
	{
	//
	// perform operations with positive values, and fix mod to make floor-based
	//
		x = floor(-numer / denom);
		q = -(int)x;
		r = (int)floor(-numer - (x * denom));
		if (r != 0)
		{
			q--;
			r = (int)denom - r;
		}
	}

	*quotient = q;
	*rem = r;
}


/*
===================
GreatestCommonDivisor
====================
*/
int GreatestCommonDivisor (int i1, int i2)
{
	while (i2)
	{
		int t = i2;
		i2 = i1 % i2;
		i1 = t;
	}
	return i1;
}


/*
===================
Invert24To16

Inverts an 8.24 value to a 16.16 value
====================
*/

fixed16_t Invert24To16(fixed16_t val)
{
	if (val < 256)
		return (0xFFFFFFFF);

	return (fixed16_t)
			(((double)0x10000 * (double)0x1000000 / (double)val) + 0.5);
}

/*
===================
MatrixMultiply
====================
*/
void MatrixMultiply(float left[16], float right[16])
{
#ifdef USE_AVX
	if (use_simd)
	{
		/* Load all 4 left columns as 128-bit registers */
		__m128 lc0 = _mm_loadu_ps (left + 0);
		__m128 lc1 = _mm_loadu_ps (left + 4);
		__m128 lc2 = _mm_loadu_ps (left + 8);
		__m128 lc3 = _mm_loadu_ps (left + 12);

		/* Broadcast left cols into 256-bit: [col | col] */
		__m256 L0 = _mm256_set_m128 (lc0, lc0);
		__m256 L1 = _mm256_set_m128 (lc1, lc1);
		__m256 L2 = _mm256_set_m128 (lc2, lc2);
		__m256 L3 = _mm256_set_m128 (lc3, lc3);

		#define VBCAST256(v256, lane) \
			_mm256_set_m128 (_mm_shuffle_ps (_mm256_extractf128_ps(v256,1), _mm256_extractf128_ps(v256,1), _MM_SHUFFLE(lane,lane,lane,lane)), \
			                 _mm_shuffle_ps (_mm256_castps256_ps128(v256),  _mm256_castps256_ps128(v256),  _MM_SHUFFLE(lane,lane,lane,lane)))

		/* Process 2 right columns per iteration */
		int i;
		for (i = 0; i < 4; i += 2, left += 8, right += 8)
		{
			/* Load 2 right columns into one 256-bit register: [col+1 | col] */
			__m256 RC = _mm256_loadu_ps (right); /* right[0..7] = col_i and col_{i+1} */

			/* Broadcast each scalar from RC to all lanes within each 128-bit half */
			__m256 c0 = _mm256_mul_ps (L0, VBCAST256 (RC, 0));
			__m256 c1 = _mm256_mul_ps (L1, VBCAST256 (RC, 1));
			__m256 c2 = _mm256_mul_ps (L2, VBCAST256 (RC, 2));
			__m256 c3 = _mm256_mul_ps (L3, VBCAST256 (RC, 3));

			c0 = _mm256_add_ps (c0, c1);
			c2 = _mm256_add_ps (c2, c3);
			c0 = _mm256_add_ps (c0, c2);

			_mm256_storeu_ps (left, c0);
		}
		#undef VBCAST256
		_mm256_zeroupper ();
		return;
	}
#endif
#ifdef USE_SSE2
	if (use_simd)
	{
		int i;
		__m128 leftcol0 = _mm_loadu_ps (left + 0);
		__m128 leftcol1 = _mm_loadu_ps (left + 4);
		__m128 leftcol2 = _mm_loadu_ps (left + 8);
		__m128 leftcol3 = _mm_loadu_ps (left + 12);

		#define VBROADCAST(vec,col)		_mm_shuffle_ps (vec, vec, _MM_SHUFFLE (col, col, col, col))

		for (i = 0; i < 4; ++i, left+=4, right+=4)
		{
			__m128 rightcol = _mm_loadu_ps (right);

			__m128 c0 = _mm_mul_ps (leftcol0, VBROADCAST (rightcol, 0));
			__m128 c1 = _mm_mul_ps (leftcol1, VBROADCAST (rightcol, 1));
			__m128 c2 = _mm_mul_ps (leftcol2, VBROADCAST (rightcol, 2));
			__m128 c3 = _mm_mul_ps (leftcol3, VBROADCAST (rightcol, 3));
			c0 = _mm_add_ps (c0, c1);
			c2 = _mm_add_ps (c2, c3);
			c0 = _mm_add_ps (c0, c2);

			_mm_storeu_ps (left, c0);
		}

		#undef VBROADCAST
		return;
	}
#endif
	{
		float temp[16];
		int column, row, i;

		memcpy(temp, left, 16 * sizeof(float));
		for(row = 0; row < 4; ++row)
		{
			for(column = 0; column < 4; ++column)
			{
				float value = 0.0f;
				for (i = 0; i < 4; ++i)
					value += temp[i*4 + row] * right[column*4 + i];

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
void RotationMatrix(float matrix[16], float angle, int axis)
{
	const float c = cosf(angle);
	const float s = sinf(angle);
	int i = (axis + 1) % 3;
	int j = (axis + 2) % 3;

	IdentityMatrix(matrix);

	matrix[i*4 + i] = c;
	matrix[j*4 + j] = c;
	matrix[j*4 + i] = -s;
	matrix[i*4 + j] = s;
}

/*
=============
TranslationMatrix
=============
*/
void TranslationMatrix(float matrix[16], float x, float y, float z)
{
	memset(matrix, 0, 16 * sizeof(float));

	// First column
	matrix[0*4 + 0] = 1.0f;

	// Second column
	matrix[1*4 + 1] = 1.0f;

	// Third column
	matrix[2*4 + 2] = 1.0f;

	// Fourth column
	matrix[3*4 + 0] = x;
	matrix[3*4 + 1] = y;
	matrix[3*4 + 2] = z;
	matrix[3*4 + 3] = 1.0f;
}

/*
=============
ScaleMatrix
=============
*/
void ScaleMatrix(float matrix[16], float x, float y, float z)
{
	memset(matrix, 0, 16 * sizeof(float));

	// First column
	matrix[0*4 + 0] = x;

	// Second column
	matrix[1*4 + 1] = y;

	// Third column
	matrix[2*4 + 2] = z;

	// Fourth column
	matrix[3*4 + 3] = 1.0f;
}

/*
=============
IdentityMatrix
=============
*/
void IdentityMatrix(float matrix[16])
{
	static const float ident[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	memcpy(matrix, ident, 16 * sizeof(float));
}

/*
=============
ApplyScale
=============
*/
void ApplyScale(float matrix[16], float x, float y, float z)
{
#ifdef USE_SSE2
	if (use_simd)
	{
		__m128 sx = _mm_set_ps1 (x);
		__m128 sy = _mm_set_ps1 (y);
		__m128 sz = _mm_set_ps1 (z);
		_mm_storeu_ps (matrix + 0*4, _mm_mul_ps (_mm_loadu_ps (matrix + 0*4), sx));
		_mm_storeu_ps (matrix + 1*4, _mm_mul_ps (_mm_loadu_ps (matrix + 1*4), sy));
		_mm_storeu_ps (matrix + 2*4, _mm_mul_ps (_mm_loadu_ps (matrix + 2*4), sz));
		return;
	}
#endif
	matrix[0*4 + 0] *= x; matrix[0*4 + 1] *= x; matrix[0*4 + 2] *= x; matrix[0*4 + 3] *= x;
	matrix[1*4 + 0] *= y; matrix[1*4 + 1] *= y; matrix[1*4 + 2] *= y; matrix[1*4 + 3] *= y;
	matrix[2*4 + 0] *= z; matrix[2*4 + 1] *= z; matrix[2*4 + 2] *= z; matrix[2*4 + 3] *= z;
}

/*
=============
ApplyTranslation
=============
*/
void ApplyTranslation(float matrix[16], float x, float y, float z)
{
#ifdef USE_SSE2
	if (use_simd)
	{
		__m128 v0 = _mm_loadu_ps (matrix + 0*4);
		__m128 v1 = _mm_loadu_ps (matrix + 1*4);
		__m128 v2 = _mm_loadu_ps (matrix + 2*4);
		__m128 v3 = _mm_loadu_ps (matrix + 3*4);

		v3 = _mm_add_ps (v3, _mm_mul_ps (v0, _mm_set_ps1 (x)));
		v3 = _mm_add_ps (v3, _mm_mul_ps (v1, _mm_set_ps1 (y)));
		v3 = _mm_add_ps (v3, _mm_mul_ps (v2, _mm_set_ps1 (z)));

		_mm_storeu_ps (matrix + 3*4, v3);
	}
	else
#endif
	{
		matrix[3*4 + 0] += x*matrix[0*4 + 0] + y*matrix[1*4 + 0] + z*matrix[2*4 + 0];
		matrix[3*4 + 1] += x*matrix[0*4 + 1] + y*matrix[1*4 + 1] + z*matrix[2*4 + 1];
		matrix[3*4 + 2] += x*matrix[0*4 + 2] + y*matrix[1*4 + 2] + z*matrix[2*4 + 2];
		matrix[3*4 + 3] += x*matrix[0*4 + 3] + y*matrix[1*4 + 3] + z*matrix[2*4 + 3];
	}
}

/*
=============
ProjectVector
=============
*/
void ProjectVector(const vec3_t src, const float matrix[16], vec3_t dst)
{
#ifdef USE_SSE2
	if (use_simd)
	{
		/* proj = col3 + src[0]*col0 + src[1]*col1 + src[2]*col2 */
		__m128 col0 = _mm_loadu_ps (matrix + 0*4);
		__m128 col1 = _mm_loadu_ps (matrix + 1*4);
		__m128 col2 = _mm_loadu_ps (matrix + 2*4);
		__m128 col3 = _mm_loadu_ps (matrix + 3*4);

		__m128 proj = _mm_add_ps (col3,
		              _mm_add_ps (_mm_mul_ps (_mm_set_ps1 (src[0]), col0),
		              _mm_add_ps (_mm_mul_ps (_mm_set_ps1 (src[1]), col1),
		                          _mm_mul_ps (_mm_set_ps1 (src[2]), col2))));

		float buf[4];
		_mm_storeu_ps (buf, proj);
		float z = fabsf (buf[3]);
		dst[0] = buf[0] / z;
		dst[1] = buf[1] / z;
		dst[2] = buf[3];
		return;
	}
#endif
	{
		float z;
		vec4_t proj;
		proj[0] = matrix[3*4 + 0];
		proj[1] = matrix[3*4 + 1];
		proj[2] = matrix[3*4 + 2];
		proj[3] = matrix[3*4 + 3];
		proj[0] += src[0]*matrix[0*4 + 0];
		proj[1] += src[0]*matrix[0*4 + 1];
		proj[2] += src[0]*matrix[0*4 + 2];
		proj[3] += src[0]*matrix[0*4 + 3];
		proj[0] += src[1]*matrix[1*4 + 0];
		proj[1] += src[1]*matrix[1*4 + 1];
		proj[2] += src[1]*matrix[1*4 + 2];
		proj[3] += src[1]*matrix[1*4 + 3];
		proj[0] += src[2]*matrix[2*4 + 0];
		proj[1] += src[2]*matrix[2*4 + 1];
		proj[2] += src[2]*matrix[2*4 + 2];
		proj[3] += src[2]*matrix[2*4 + 3];
		z = fabs (proj[3]);
		dst[0] = proj[0] / z;
		dst[1] = proj[1] / z;
		dst[2] = proj[3];
	}
}

void MatrixTranspose4x3(const float src[16], float dst[12])
{
#ifdef USE_SSE2
	if (use_simd)
	{
		/* src is column-major 4x4, we want the transpose of the top 3 rows as 3x4 row-major
		   i.e. dst[row][col] = src[col][row] for row in 0..2, col in 0..3
		   Use SSE transpose trick with unpack + shuffle                                    */
		__m128 c0 = _mm_loadu_ps (src + 0);  /* col0: r0c0 r1c0 r2c0 r3c0 */
		__m128 c1 = _mm_loadu_ps (src + 4);  /* col1: r0c1 r1c1 r2c1 r3c1 */
		__m128 c2 = _mm_loadu_ps (src + 8);  /* col2: r0c2 r1c2 r2c2 r3c2 */
		__m128 c3 = _mm_loadu_ps (src + 12); /* col3: r0c3 r1c3 r2c3 r3c3 */

		__m128 t0 = _mm_unpacklo_ps (c0, c1); /* r0c0 r0c1 r1c0 r1c1 */
		__m128 t1 = _mm_unpacklo_ps (c2, c3); /* r0c2 r0c3 r1c2 r1c3 */
		__m128 t2 = _mm_unpackhi_ps (c0, c1); /* r2c0 r2c1 r3c0 r3c1 */
		__m128 t3 = _mm_unpackhi_ps (c2, c3); /* r2c2 r2c3 r3c2 r3c3 */

		/* Row 0: r0c0 r0c1 r0c2 r0c3 */
		_mm_storeu_ps (dst + 0*4, _mm_movelh_ps (t0, t1));
		/* Row 1: r1c0 r1c1 r1c2 r1c3 */
		_mm_storeu_ps (dst + 1*4, _mm_movehl_ps (t1, t0));
		/* Row 2: r2c0 r2c1 r2c2 r2c3 */
		_mm_storeu_ps (dst + 2*4, _mm_movelh_ps (t2, t3));
		return;
	}
#endif
	#define COPY_ROW(row)				\
		dst[row*4+0] = src[row+0],		\
		dst[row*4+1] = src[row+4],		\
		dst[row*4+2] = src[row+8],		\
		dst[row*4+3] = src[row+12]

	COPY_ROW (0);
	COPY_ROW (1);
	COPY_ROW (2);

	#undef COPY_ROW
}

qboolean RayVsBox (const vec3_t org, const vec3_t rcpdelta, const vec3_t mins, const vec3_t maxs, float *frac)
{
#ifdef USE_SSE41
	if (use_simd)
	{
		/* Load as 4-wide (w lane is harmless: rcpdelta[3]=0 gives t=±inf, clipped away) */
		__m128 vorg    = _mm_loadu_ps (org);
		__m128 vrcpd   = _mm_loadu_ps (rcpdelta);
		__m128 vmins   = _mm_loadu_ps (mins);
		__m128 vmaxs   = _mm_loadu_ps (maxs);

		__m128 t0 = _mm_mul_ps (_mm_sub_ps (vmins, vorg), vrcpd);
		__m128 t1 = _mm_mul_ps (_mm_sub_ps (vmaxs, vorg), vrcpd);

		__m128 tmin4 = _mm_min_ps (t0, t1);
		__m128 tmax4 = _mm_max_ps (t0, t1);

		/* Clamp entry to [0,1], exit to [0,1] */
		__m128 zero = _mm_setzero_ps ();
		__m128 one  = _mm_set_ps1 (1.f);

		/* enter = max(0, max(tmin.x, tmin.y, tmin.z)) */
		/* We need horizontal max of xyz lanes of tmin4 */
		__m128 tmp = _mm_shuffle_ps (tmin4, tmin4, _MM_SHUFFLE (2, 1, 0, 3));
		__m128 enter4 = _mm_max_ps (tmin4, _mm_shuffle_ps (tmin4, tmin4, _MM_SHUFFLE (1, 0, 3, 2)));
		enter4 = _mm_max_ps (enter4, tmp);
		enter4 = _mm_max_ps (enter4, zero);

		/* exit = min(1, min(tmax.x, tmax.y, tmax.z)) */
		__m128 tmp2  = _mm_shuffle_ps (tmax4, tmax4, _MM_SHUFFLE (2, 1, 0, 3));
		__m128 exit4 = _mm_min_ps (tmax4, _mm_shuffle_ps (tmax4, tmax4, _MM_SHUFFLE (1, 0, 3, 2)));
		exit4 = _mm_min_ps (exit4, tmp2);
		exit4 = _mm_min_ps (exit4, one);

		float enter_f = _mm_cvtss_f32 (enter4);
		float exit_f  = _mm_cvtss_f32 (exit4);

		if (frac) *frac = 1.f;
		if (enter_f > exit_f)
			return false;
		if (frac) *frac = enter_f;
		return true;
	}
#endif
	{
		int		i;
		float	enter, exit;

		if (frac)
			*frac = 1.f;

		enter = 0.f;
		exit = 1.f;

		for (i = 0; i < 3; i++)
		{
			float t0 = (mins[i] - org[i]) * rcpdelta[i];
			float t1 = (maxs[i] - org[i]) * rcpdelta[i];
			float tmin = q_min (t0, t1);
			float tmax = q_max (t0, t1);
			enter = q_max (enter, tmin);
			exit = q_min (exit, tmax);
		}

		if (enter > exit)
			return false;

		if (frac)
			*frac = enter;

		return true;
	}
}
