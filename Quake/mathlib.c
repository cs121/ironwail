/*
Optimized mathlib.c — Quake-derived math primitives (single-precision tuned)

This version focuses on:
- consistent float math (sinf/cosf/etc.)
- optional SIMD paths (SSE2, BMI2)
- reduced transcendental calls (sincosf wrapper)
- safer inlining hints and aliasing-friendly signatures
- small algorithmic tweaks (iterative GCD, fmaf usage)

Behavior remains compatible with the original where not otherwise
noted in comments.
*/

#include "quakedef.h"

#include <math.h>
#include <string.h>

#if defined(USE_SSE2)
  #include <xmmintrin.h>
  #include <emmintrin.h>
#endif
#if defined(__BMI2__)
  #include <immintrin.h>
#endif

// --- helpers ------------------------------------------------------

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_PI_DIV_180
#define M_PI_DIV_180 (M_PI/180.0)
#endif

// Portable sincosf compat (uses separate calls when system sincosf is unavailable)
static inline void sincosf_compat(float a, float *s, float *c) {
#if (defined(__GLIBC__) || defined(__BIONIC__)) && !defined(_MSC_VER)
    // Many libcs provide sincosf
    sincosf(a, s, c);
#else
    *s = sinf(a);
    *c = cosf(a);
#endif
}

static inline float wrap_deg_pm180(float d)
{
    d += 180.0f;
    d -= floorf(d * (1.0f/360.0f)) * 360.0f; // robust vs negatives
    return d - 180.0f;
}

static inline float fminf2(float a, float b) { return a < b ? a : b; }
static inline float fmaxf2(float a, float b) { return a > b ? a : b; }

// -----------------------------------------------------------------

vec3_t vec3_origin = {0,0,0};
vec4_t vec4_origin = {0,0,0,0};

/*-----------------------------------------------------------------*/

void ProjectPointOnPlane( vec3_t dst, const vec3_t p, const vec3_t normal )
{
    float d;
    vec3_t n;
    float inv_denom = 1.0f / DotProduct( normal, normal );

    d = DotProduct( normal, p ) * inv_denom;

    n[0] = normal[0] * inv_denom;
    n[1] = normal[1] * inv_denom;
    n[2] = normal[2] * inv_denom;

    dst[0] = p[0] - d * n[0];
    dst[1] = p[1] - d * n[1];
    dst[2] = p[2] - d * n[2];
}

/* assumes "src" is normalized */
void PerpendicularVector( vec3_t dst, const vec3_t src )
{
    int pos = 0;
    int i;
    float minelem = 1.0f;
    vec3_t tempvec;

    // find the smallest magnitude axially aligned vector
    for (i = 0; i < 3; i++)
    {
        float a = fabsf(src[i]);
        if (a < minelem) { pos = i; minelem = a; }
    }
    tempvec[0] = tempvec[1] = tempvec[2] = 0.0f;
    tempvec[pos] = 1.0f;

    // project the point onto the plane defined by src
    ProjectPointOnPlane( dst, tempvec, src );

    // normalize the result
    VectorNormalize( dst );
}

/*-----------------------------------------------------------------*/

float anglemod(float a)
{
    a = (360.0f/65536.0f) * ((int)(a*(65536.0f/360.0f)) & 65535);
    return a;
}

/*
==================
NormalizeAngle

Returns a value between -180 and 180
==================
*/
float NormalizeAngle (float degrees) { return wrap_deg_pm180(degrees); }

/*
==================
AngleDifference

Returns a value between -180 and 180
==================
*/
float AngleDifference (float dega, float degb)
{
    return wrap_deg_pm180 (dega - degb);
}

/*
==================
LerpAngle

Returns a value between -180 and 180
==================
*/
float LerpAngle (float degfrom, float degto, float frac)
{
    return wrap_deg_pm180 (degfrom + AngleDifference (degto, degfrom) * frac);
}

/*
==================
BoxOnPlaneSide

Returns 1, 2, or 1 + 2
==================
*/
int BoxOnPlaneSide (vec3_t emins, vec3_t emaxs, mplane_t *p)
{
    float dist1, dist2;
    int   xneg, yneg, zneg;
    int   sides;

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
    if (dist2 <  p->dist) sides |= 2;

#ifdef PARANOID
    if (sides == 0)
        Sys_Error ("BoxOnPlaneSide: sides==0");
#endif

    return sides;
}

// johnfitz -- opposite of AngleVectors. Takes forward and generates pitch yaw roll
// TODO: take right/up to properly set yaw and roll
void VectorAngles (const vec3_t forward, vec3_t angles)
{
    vec3_t temp = { forward[0], forward[1], 0.0f };
    angles[PITCH] = -atan2f(forward[2], VectorLength(temp)) / (float)M_PI_DIV_180;
    angles[YAW]   =  atan2f(forward[1], forward[0]) / (float)M_PI_DIV_180;
    angles[ROLL]  =  0.0f;
}

void AngleVectors (vec3_t angles, vec3_t forward, vec3_t right, vec3_t up)
{
    const float k = (float)M_PI * (2.0f/360.0f);
    float sr, sp, sy, cr, cp, cy;

    sincosf_compat(angles[YAW]   * k, &sy, &cy);
    sincosf_compat(angles[PITCH] * k, &sp, &cp);
    sincosf_compat(angles[ROLL]  * k, &sr, &cr);

    forward[0] =  cp*cy;
    forward[1] =  cp*sy;
    forward[2] = -sp;

    right[0] = (-sr*sp*cy - cr*-sy);
    right[1] = (-sr*sp*sy - cr* cy);
    right[2] = -sr*cp;

    up[0] = (cr*sp*cy + -sr*-sy);
    up[1] = (cr*sp*sy + -sr* cy);
    up[2] =  cr*cp;
}

int VectorCompare (const vec3_t v1, const vec3_t v2)
{
    for (int i=0; i<3; i++) if (v1[i] != v2[i]) return 0;
    return 1;
}

void VectorMA (const vec3_t veca, float scale, const vec3_t vecb, vec3_t vecc)
{
    vecc[0] = veca[0] + scale*vecb[0];
    vecc[1] = veca[1] + scale*vecb[1];
    vecc[2] = veca[2] + scale*vecb[2];
}

void VectorLerp (const vec3_t veca, const vec3_t vecb, float frac, vec3_t dst)
{
    dst[0] = LERP (veca[0], vecb[0], frac);
    dst[1] = LERP (veca[1], vecb[1], frac);
    dst[2] = LERP (veca[2], vecb[2], frac);
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
    cross[0] = v1[1]*v2[2] - v1[2]*v2[1];
    cross[1] = v1[2]*v2[0] - v1[0]*v2[2];
    cross[2] = v1[0]*v2[1] - v1[1]*v2[0];
}

vec_t VectorLength(const vec3_t v)
{
    return sqrtf(DotProduct(v,v));
}

float VectorNormalize (vec3_t v)
{
    float len2 = DotProduct(v,v);
    if (len2 <= 0.0f) return 0.0f;

#if defined(USE_SSE2)
    if (use_simd) {
        __m128 x = _mm_set_ss(len2);
        __m128 r = _mm_rsqrt_ss(x); // fast approx
        // Optionally refine once via Newton-Raphson for better precision:
        // r = _mm_mul_ss(r, _mm_sub_ss(_mm_set_ss(1.5f), _mm_mul_ss(_mm_mul_ss(_mm_set_ss(0.5f), x), _mm_mul_ss(r, r))));
        float inv = _mm_cvtss_f32(r);
        v[0] *= inv; v[1] *= inv; v[2] *= inv;
        return sqrtf(len2);
    }
#endif
    {
        float len = sqrtf(len2);
        float inv = 1.0f/len;
        v[0] *= inv; v[1] *= inv; v[2] *= inv;
        return len;
    }
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
    out[0] = in[0]*scale; out[1] = in[1]*scale; out[2] = in[2]*scale;
}

int Q_log2(int val)
{
    int answer=0; while (val>>=1) answer++; return answer;
}

int Q_nextPow2(int val)
{
    val--; val |= val>>1; val |= val>>2; val |= val>>4; val |= val>>8; val |= val>>16; val++; return val;
}

float GetFraction (float val, float minval, float maxval)
{ return (val - minval) / (maxval - minval); }

float GetClampedFraction (float val, float minval, float maxval)
{ val = GetFraction (val, minval, maxval); return CLAMP (0.f, val, 1.f); }

float Log2f (float val) { return log2f(val); }
float Exp2f (float val) { return exp2f(val); }

float GetLogFraction (float val, float minval, float maxval)
{ return GetFraction (logf (val), logf (minval), logf (maxval)); }

float GetClampedLogFraction (float val, float minval, float maxval)
{ val = GetLogFraction (val, minval, maxval); return CLAMP (0.f, val, 1.f); }

float LogLerp (float minval, float maxval, float t)
{ return minval * expf (t * logf (maxval / minval)); }

float EaseInOut (float t)
{ return t * t * (3.f - 2.f * t); }

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
#if defined(__BMI2__)
    return _pdep_u32(even, 0x55555555u) | _pdep_u32(odd, 0xAAAAAAAAu);
#else
    return Interleave0 (even) | (Interleave0 (odd) << 1);
#endif
}

/*
==================
DeinterleaveEven

Deinterleaves the even 16 bits of x (bits 0,2,4..28,30)
==================
*/
uint16_t DeinterleaveEven (uint32_t x)
{
#if defined(__BMI2__)
    return (uint16_t)_pext_u32(x, 0x55555555u);
#else
    x &= 0x55555555u;
    x = (x ^ (x >> 1u)) & 0x33333333u;
    x = (x ^ (x >> 2u)) & 0x0F0F0F0Fu;
    x = (x ^ (x >> 4u)) & 0x00FF00FFu;
    x = (x ^ (x >> 8u)) & 0x0000FFFFu;
    return (uint16_t) x;
#endif
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

Returns mathematically correct (floor-based) quotient and remainder for
numer and denom, both of which should contain no fractional part. The
quotient must fit in 32 bits.
====================
*/
void FloorDivMod (double numer, double denom, int *quotient, int *rem)
{
#ifndef PARANOID
    if (denom <= 0.0)
        Sys_Error ("FloorDivMod: bad denominator %f\n", denom);
#endif

    int q, r;
    double x;
    if (numer >= 0.0) {
        x = floor(numer / denom);
        q = (int)x;
        r = (int)floor(numer - (x * denom));
    } else {
        x = floor(-numer / denom);
        q = -(int)x;
        r = (int)floor(-numer - (x * denom));
        if (r != 0) { q--; r = (int)denom - r; }
    }
    *quotient = q; *rem = r;
}

/*
===================
GreatestCommonDivisor (iterative)
====================
*/
int GreatestCommonDivisor (int a, int b)
{
    if (a < 0) a = -a; if (b < 0) b = -b;
    while (b) { int r = a % b; a = b; b = r; }
    return a;
}

/*
===================
Invert24To16

Inverts an 8.24 value to a 16.16 value
====================
*/
fixed16_t Invert24To16(fixed16_t val)
{
    if (val < 256) return (0xFFFFFFFF);
    return (fixed16_t) (((double)0x10000 * (double)0x1000000 / (double)val) + 0.5);
}

/*
===================
MatrixMultiply
====================
*/
void MatrixMultiply(float left[16], const float right[16])
{
#if defined(USE_SSE2)
    if (use_simd)
    {
        // Original SSE2 path adapted to avoid pointer aliasing issues
        __m128 leftcol0 = _mm_loadu_ps (left + 0);
        __m128 leftcol1 = _mm_loadu_ps (left + 4);
        __m128 leftcol2 = _mm_loadu_ps (left + 8);
        __m128 leftcol3 = _mm_loadu_ps (left + 12);

        #define VBROADCAST(vec,col) _mm_shuffle_ps (vec, vec, _MM_SHUFFLE (col, col, col, col))

        for (int i = 0; i < 4; ++i)
        {
            __m128 rightcol = _mm_loadu_ps (right + i*4);
            __m128 c0 = _mm_mul_ps (leftcol0, VBROADCAST (rightcol, 0));
            __m128 c1 = _mm_mul_ps (leftcol1, VBROADCAST (rightcol, 1));
            __m128 c2 = _mm_mul_ps (leftcol2, VBROADCAST (rightcol, 2));
            __m128 c3 = _mm_mul_ps (leftcol3, VBROADCAST (rightcol, 3));
            c0 = _mm_add_ps (c0, c1);
            c2 = _mm_add_ps (c2, c3);
            c0 = _mm_add_ps (c0, c2);
            _mm_storeu_ps (left + i*4, c0);
        }
        #undef VBROADCAST
        return;
    }
#endif
    // Scalar fallback with fmaf and alias-friendly temp
    float temp[16];
    memcpy(temp, left, 16 * sizeof(float));
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
#if __STDC_VERSION__ >= 199901L
            float value = fmaf(temp[0*4 + row], right[column*4 + 0],
                          fmaf(temp[1*4 + row], right[column*4 + 1],
                          fmaf(temp[2*4 + row], right[column*4 + 2],
                               temp[3*4 + row] * right[column*4 + 3])));
#else
            float value = temp[0*4 + row] * right[column*4 + 0]
                        + temp[1*4 + row] * right[column*4 + 1]
                        + temp[2*4 + row] * right[column*4 + 2]
                        + temp[3*4 + row] * right[column*4 + 3];
#endif
            left[column * 4 + row] = value;
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
    matrix[0*4 + 0] = 1.0f;
    matrix[1*4 + 1] = 1.0f;
    matrix[2*4 + 2] = 1.0f;
    matrix[3*4 + 0] = x; matrix[3*4 + 1] = y; matrix[3*4 + 2] = z; matrix[3*4 + 3] = 1.0f;
}

/*
=============
ScaleMatrix
=============
*/
void ScaleMatrix(float matrix[16], float x, float y, float z)
{
    memset(matrix, 0, 16 * sizeof(float));
    matrix[0*4 + 0] = x;
    matrix[1*4 + 1] = y;
    matrix[2*4 + 2] = z;
    matrix[3*4 + 3] = 1.0f;
}

/*
=============
IdentityMatrix
=============
*/
void IdentityMatrix(float matrix[16])
{
    memset(matrix, 0, 16 * sizeof(float));
    matrix[0*4 + 0] = 1.0f;
    matrix[1*4 + 1] = 1.0f;
    matrix[2*4 + 2] = 1.0f;
    matrix[3*4 + 3] = 1.0f;
}

/*
=============
ApplyScale
=============
*/
void ApplyScale(float matrix[16], float x, float y, float z)
{
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
#if defined(USE_SSE2)
    __m128 v0 = _mm_loadu_ps (matrix + 0);
    __m128 v1 = _mm_loadu_ps (matrix + 4);
    __m128 v2 = _mm_loadu_ps (matrix + 8);
    __m128 v3 = _mm_loadu_ps (matrix + 12);

    v3 = _mm_add_ps (v3, _mm_mul_ps (v0, _mm_set_ps1 (x)));
    v3 = _mm_add_ps (v3, _mm_mul_ps (v1, _mm_set_ps1 (y)));
    v3 = _mm_add_ps (v3, _mm_mul_ps (v2, _mm_set_ps1 (z)));

    _mm_storeu_ps (matrix + 12, v3);
#else
    matrix[12] += x*matrix[0];  matrix[13] += x*matrix[1];  matrix[14] += x*matrix[2];  matrix[15] += x*matrix[3];
    matrix[12] += y*matrix[4];  matrix[13] += y*matrix[5];  matrix[14] += y*matrix[6];  matrix[15] += y*matrix[7];
    matrix[12] += z*matrix[8];  matrix[13] += z*matrix[9];  matrix[14] += z*matrix[10]; matrix[15] += z*matrix[11];
#endif
}

/*
=============
ProjectVector
=============
*/
void ProjectVector(const vec3_t src, const float matrix[16], vec3_t dst)
{
    float x = matrix[12], y = matrix[13], z = matrix[14], w = matrix[15];

#if __STDC_VERSION__ >= 199901L
    x = fmaf(src[0], matrix[0],  fmaf(src[1], matrix[4],  fmaf(src[2], matrix[8],  x)));
    y = fmaf(src[0], matrix[1],  fmaf(src[1], matrix[5],  fmaf(src[2], matrix[9],  y)));
    z = fmaf(src[0], matrix[2],  fmaf(src[1], matrix[6],  fmaf(src[2], matrix[10], z)));
    w = fmaf(src[0], matrix[3],  fmaf(src[1], matrix[7],  fmaf(src[2], matrix[11], w)));
#else
    x += src[0]*matrix[0]  + src[1]*matrix[4]  + src[2]*matrix[8];
    y += src[0]*matrix[1]  + src[1]*matrix[5]  + src[2]*matrix[9];
    z += src[0]*matrix[2]  + src[1]*matrix[6]  + src[2]*matrix[10];
    w += src[0]*matrix[3]  + src[1]*matrix[7]  + src[2]*matrix[11];
#endif

    float invw = 1.0f / fabsf (w);
    dst[0] = x * invw;
    dst[1] = y * invw;
    dst[2] = w; // preserves original behavior
}

void MatrixTranspose4x3(const float src[16], float dst[12])
{
    #define COPY_ROW(row) do { \
        dst[(row)*4+0] = src[(row)+0];  \
        dst[(row)*4+1] = src[(row)+4];  \
        dst[(row)*4+2] = src[(row)+8];  \
        dst[(row)*4+3] = src[(row)+12]; \
    } while(0)

    COPY_ROW (0);
    COPY_ROW (1);
    COPY_ROW (2);

    #undef COPY_ROW
}

qboolean RayVsBox (const vec3_t org, const vec3_t rcpdelta, const vec3_t mins, const vec3_t maxs, float *frac)
{
    float enter = 0.f;
    float exit  = 1.f;

    if (frac) *frac = 1.f;

    for (int i = 0; i < 3; i++)
    {
        float t0 = (mins[i] - org[i]) * rcpdelta[i];
        float t1 = (maxs[i] - org[i]) * rcpdelta[i];
        float tmin = fminf2 (t0, t1);
        float tmax = fmaxf2 (t0, t1);
        enter = fmaxf2 (enter, tmin);
        exit  = fminf2 (exit,  tmax);
    }

    if (enter > exit) return false;
    if (frac) *frac = enter;
    return true;
}
