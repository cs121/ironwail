//
// stb_dxt - v1.13 - DXT1/DXT5 compressor - public domain
// original public domain implementation by Sean Barrett / RAD Game Tools
//
// This file has been extracted for use inside ironwail; only the functions
// required for DXT1/DXT5 compression are included.
//
#ifndef STB_DXT_INCLUDE
#define STB_DXT_INCLUDE

#ifdef __cplusplus
extern "C" {
#endif

#define STB_DXT_HIGHQUAL 1

void stb_compress_dxt_block(unsigned char *dest, const unsigned char *src_rgba_four_bytes_per_pixel, int alpha, int mode);

#ifdef __cplusplus
}
#endif

#ifdef STB_DXT_IMPLEMENTATION

#ifndef STBD_UINT8
#define STBD_UINT8 unsigned char
#endif
#ifndef STBD_UINT16
#define STBD_UINT16 unsigned short
#endif
#ifndef STBD_UINT32
#define STBD_UINT32 unsigned int
#endif

static STBD_UINT8 stb__Expand5[32];
static STBD_UINT8 stb__Expand6[64];
static STBD_UINT8 stb__OMatch5[512];
static STBD_UINT8 stb__OMatch6[512];
static STBD_UINT8 stb__QuantRBTab[256+16];
static STBD_UINT8 stb__QuantGTab[256+8];
static int stb__PrepareOptTable = 1;

static void stb__InitDXT()
{
   int i;
   for (i=0;i<32;i++) stb__Expand5[i] = (STBD_UINT8) ((i<<3)|(i>>2));
   for (i=0;i<64;i++) stb__Expand6[i] = (STBD_UINT8) ((i<<2)|(i>>4));
   for (i=0;i<256+16;i++) {
      int v = i-8 < 0 ? 0 : i-8 > 255 ? 255 : i-8;
      stb__QuantRBTab[i] = (STBD_UINT8)(stb__Expand5[v >> 3]);
   }
   for (i=0;i<256+8;i++) {
      int v = i-4 < 0 ? 0 : i-4 > 255 ? 255 : i-4;
      stb__QuantGTab[i] = (STBD_UINT8)(stb__Expand6[v >> 2]);
   }
   for (i=0;i<512;i++) {
      int v = (i+8)>>4;
      if (v < 0) v = 0; if (v > 31) v = 31;
      stb__OMatch5[i] = (STBD_UINT8) v;
      v = (i+4)>>3;
      if (v < 0) v = 0; if (v > 63) v = 63;
      stb__OMatch6[i] = (STBD_UINT8) v;
   }
}

static STBD_UINT32 stb__AssembleRGB(STBD_UINT8 r, STBD_UINT8 g, STBD_UINT8 b)
{
   return (r << 0) | (g << 8) | (b << 16);
}

static void stb__CompressColorBlock(unsigned char *dest, unsigned char *block, int mode)
{
   int i,j;
   int mn[3], mx[3];

   if (stb__PrepareOptTable) { stb__InitDXT(); stb__PrepareOptTable = 0; }

   for (i=0;i<3;i++) {
      int lo=block[i],hi=block[i];
      for (j=i;j<64;j+=4) {
         if (block[j] < lo) lo = block[j];
         if (block[j] > hi) hi = block[j];
      }
      mn[i] = lo; mx[i] = hi;
   }

   {
      STBD_UINT32 c0 = stb__AssembleRGB(stb__QuantRBTab[mx[0]+8], stb__QuantGTab[mx[1]+4], stb__QuantRBTab[mx[2]+8]);
      STBD_UINT32 c1 = stb__AssembleRGB(stb__QuantRBTab[mn[0]+8], stb__QuantGTab[mn[1]+4], stb__QuantRBTab[mn[2]+8]);
      STBD_UINT32 mask = 0;

      if (c0 == c1) {
         for (i=0;i<16;i++)
            dest[4+i/8] |= (1 << ((i*2)&7));
      } else {
         STBD_UINT32 d0[3],d1[3];
         int v_r = mx[0] - mn[0], v_g = mx[1] - mn[1], v_b = mx[2] - mn[2];
         int axis_len2 = v_r*v_r + v_g*v_g + v_b*v_b;

         if (mode == STB_DXT_HIGHQUAL && axis_len2 > 0)
         {
            float mid_r = (mx[0] + mn[0]) * 0.5f;
            float mid_g = (mx[1] + mn[1]) * 0.5f;
            float mid_b = (mx[2] + mn[2]) * 0.5f;
            int best = 0;
            float best_err = 1e30f;
            for (i=0;i<32;i++)
            for (j=0;j<32;j++)
            {
               STBD_UINT32 p0 = stb__Expand5[i];
               STBD_UINT32 p1 = stb__Expand5[j];
               float err = (p0 - mid_r)*(p0 - mid_r) + (p1 - mid_r)*(p1 - mid_r);
               if (err < best_err)
               { best = (i<<16)|j; best_err = err; }
            }
            c0 = stb__AssembleRGB(stb__Expand5[(best>>16)&31], stb__Expand6[stb__OMatch6[(int)(v_g*4+256)]], stb__Expand5[(best>>0)&31]);
            c1 = stb__AssembleRGB(stb__Expand5[stb__OMatch5[(int)(v_r*4+256)]], stb__Expand6[(best>>8)&63], stb__Expand5[stb__OMatch5[(int)(v_b*4+256)]]);
         }

         d0[0] = (c0 >> 0) & 0xff; d0[1] = (c0 >> 8) & 0xff; d0[2] = (c0 >> 16) & 0xff;
         d1[0] = (c1 >> 0) & 0xff; d1[1] = (c1 >> 8) & 0xff; d1[2] = (c1 >> 16) & 0xff;

         dest[0] = (STBD_UINT8)(c0 >> 0);
         dest[1] = (STBD_UINT8)(c0 >> 8);
         dest[2] = (STBD_UINT8)(c0 >> 16);
         dest[3] = (STBD_UINT8)(c0 >> 24);
         dest[4] = (STBD_UINT8)(c1 >> 0);
         dest[5] = (STBD_UINT8)(c1 >> 8);
         dest[6] = (STBD_UINT8)(c1 >> 16);
         dest[7] = (STBD_UINT8)(c1 >> 24);

         for (j=0;j<16;j++) {
            int r = block[j*4+0];
            int g = block[j*4+1];
            int b = block[j*4+2];
            int d0r = r - d0[0]; int d0g = g - d0[1]; int d0b = b - d0[2];
            int d1r = r - d1[0]; int d1g = g - d1[1]; int d1b = b - d1[2];
            int p0 = d0r*d0r + d0g*d0g + d0b*d0b;
            int p1 = d1r*d1r + d1g*d1g + d1b*d1b;
            if (p0 < p1)
               mask |= 1 << (j*2);
            else
               mask |= 2 << (j*2);
         }
      }
      dest[4] |= (STBD_UINT8)(mask >> 0);
      dest[5] |= (STBD_UINT8)(mask >> 8);
      dest[6] |= (STBD_UINT8)(mask >> 16);
      dest[7] |= (STBD_UINT8)(mask >> 24);
   }
}

static void stb__CompressAlphaBlock(unsigned char *dest, unsigned char *src, int mode)
{
   int i,j,dist,bits,
       alphas[8],
       indices[16];
   int mn,mx;

   mn = mx = src[3];
   for (i=4;i<64;i+=4) {
      if (src[i+3] < mn) mn = src[i+3];
      else if (src[i+3] > mx) mx = src[i+3];
   }

   alphas[0] = mx;
   alphas[1] = mn;
   if (mode) {
      alphas[2] = (6*mx + 1*mn)/7;
      alphas[3] = (5*mx + 2*mn)/7;
      alphas[4] = (4*mx + 3*mn)/7;
      alphas[5] = (3*mx + 4*mn)/7;
      alphas[6] = (2*mx + 5*mn)/7;
      alphas[7] = (1*mx + 6*mn)/7;
   } else {
      alphas[2] = (4*mx + 1*mn)/5;
      alphas[3] = (3*mx + 2*mn)/5;
      alphas[4] = (2*mx + 3*mn)/5;
      alphas[5] = (1*mx + 4*mn)/5;
      alphas[6] = 0;
      alphas[7] = 255;
   }

   dist = 0; bits = 0;
   for (i=0;i<16;i++) {
      int a = src[i*4+3];
      int bv = 0;
      int best = 1000000000;
      for (j=0;j<8;j++) {
         int d = (a - alphas[j]);
         d = d < 0 ? -d : d;
         if (d < best) { best = d; bv = j; }
      }
      indices[i] = bv;
      bits |= bv << dist;
      dist += 3;
      if (dist >= 24) {
         *((STBD_UINT32 *) dest) = bits;
         dest += 3;
         bits >>= 24;
         dist -= 24;
      }
   }
   *((STBD_UINT32 *) dest) = bits;
   dest += 3;

   dest[0] = (STBD_UINT8)alphas[0];
   dest[1] = (STBD_UINT8)alphas[1];
}

void stb_compress_dxt_block(unsigned char *dest, const unsigned char *src_rgba_four_bytes_per_pixel, int alpha, int mode)
{
   unsigned char data[16][4];
   int i; 

   for (i=0;i<16;i++) {
      data[i][0] = src_rgba_four_bytes_per_pixel[i*4+0];
      data[i][1] = src_rgba_four_bytes_per_pixel[i*4+1];
      data[i][2] = src_rgba_four_bytes_per_pixel[i*4+2];
      data[i][3] = src_rgba_four_bytes_per_pixel[i*4+3];
   }

   if (alpha)
   {
      stb__CompressAlphaBlock(dest, (unsigned char *) data, mode);
      dest += 8;
      if (mode != STB_DXT_HIGHQUAL)
         mode = 0;
   }

   stb__CompressColorBlock(dest, (unsigned char *) data, mode);
}

#endif // STB_DXT_IMPLEMENTATION

#endif // STB_DXT_INCLUDE
