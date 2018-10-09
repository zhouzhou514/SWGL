#ifndef TEST_SIMDCPPBLOCKLINE_H
#define TEST_SIMDCPPBLOCKLINE_H

#include "TriRaster.h"
#include "RasterOperations.h"


SIMDPP_ALIGN(64) static const float g_XPixOffets_8[8] = {0.0f, 1.0f, 2.0f, 3.0f,
                                                         4.0f, 5.0f, 6.0f, 7.0f};


template<int DIM>
static inline simdpp::float32<DIM> PixOffsetX()
{
  simdpp::float32<4> r = simdpp::make_float(0.0f, 1.0f, 2.0f, 3.0f);
  return r;
}

template<>
inline simdpp::float32<2> PixOffsetX<2>()
{
  simdpp::float32<2> r = simdpp::make_float(0.0f, 1.0f);
  return r;
}

template<>
inline simdpp::float32<8> PixOffsetX<8>()
{
  simdpp::float32<8> r = simdpp::load(g_XPixOffets_8);
  return r;
}


inline static unsigned int RealColorToUint32_BGRA_SIMD(const simdpp::float32<4>& real_color)
{
  static const simdpp::float32<4> const_255 = simdpp::make_float(255.0f);
  static const simdpp::uint32<4>  shiftmask = simdpp::make_int(16,8,0,24);

  const simdpp::float32<4> rgbaf = real_color*const_255;
  const simdpp::uint32<4>  rgbai = simdpp::to_int32(rgbaf);

  return simdpp::reduce_or(simdpp::shift_l(rgbai, shiftmask)); // return blue | (green << 8) | (red << 16) | (alpha << 24);
}


template<typename TriangleType, const int lineSize, typename ROP, typename ROP_SCALAR>
void RasterizeTriHalfSpace2D_BlockLine(const TriangleType& tri, int tileMinX, int tileMinY,
                                       FrameBuffer* frameBuf)
{
  const float tileMinX_f = float(tileMinX);
  const float tileMinY_f = float(tileMinY);

  const float y1 = tri.v3.y - tileMinY_f;
  const float y2 = tri.v2.y - tileMinY_f;
  const float y3 = tri.v1.y - tileMinY_f;

  const float x1 = tri.v3.x - tileMinX_f;
  const float x2 = tri.v2.x - tileMinX_f;
  const float x3 = tri.v1.x - tileMinX_f;

  // Deltas
  const float Dx12 = x1 - x2;
  const float Dx23 = x2 - x3;
  const float Dx31 = x3 - x1;

  const float Dy12 = y1 - y2;
  const float Dy23 = y2 - y3;
  const float Dy31 = y3 - y1;

  // Bounding rectangle
  const int minx = std::max(tri.bb_iminX - tileMinX, 0);
  const int miny = std::max(tri.bb_iminY - tileMinY, 0);
  const int maxx = std::min(tri.bb_imaxX - tileMinX, frameBuf->w - 1);
  const int maxy = std::min(tri.bb_imaxY - tileMinY, frameBuf->h - 1);

  int* cbuff = frameBuf->cbuffer;

  // Constant part of half-edge functions
  const float C1 = Dy12 * x1 - Dx12 * y1;
  const float C2 = Dy23 * x2 - Dx23 * y2;
  const float C3 = Dy31 * x3 - Dx31 * y3;

  const float areaInv = 1.0f / fabs(Dy31*Dx12 - Dx31*Dy12); // edgeFunction(v0, v1, v2);

  float Cy1_b = C1 + Dx12 * miny - Dy12 * minx;
  float Cy2_b = C2 + Dx23 * miny - Dy23 * minx;
  float Cy3_b = C3 + Dx31 * miny - Dy31 * minx;

  constexpr float blockSizeF = float(lineSize);

  ///////////////////////////////////////////////////////////////////////////////// vectorized per triangle variables
  //
  const simdpp::float32<4> blockSizeF_4v = simdpp::splat(blockSizeF);

  const auto tri_c1 = VROP<lineSize>::splat_v4(tri.c1);
  const auto tri_c2 = VROP<lineSize>::splat_v4(tri.c2);
  const auto tri_c3 = VROP<lineSize>::splat_v4(tri.c3);

  const simdpp::float32<lineSize> areaInvV = simdpp::splat(areaInv);
  const simdpp::float32<lineSize> Dx12v    = simdpp::splat(Dx12);
  const simdpp::float32<lineSize> Dx23v    = simdpp::splat(Dx23);
  const simdpp::float32<lineSize> Dx31v    = simdpp::splat(Dx31);

  const simdpp::float32<lineSize> Dy12v    = simdpp::splat(Dy12);
  const simdpp::float32<lineSize> Dy23v    = simdpp::splat(Dy23);
  const simdpp::float32<lineSize> Dy31v    = simdpp::splat(Dy31);

  const auto pixOffsX = PixOffsetX<lineSize>();

  SIMDPP_ALIGN(16) const float Cx1_va[4] = {0.f, - Dy12*blockSizeF, Dx12*blockSizeF, Dx12*blockSizeF - Dy12*blockSizeF};
  SIMDPP_ALIGN(16) const float Cx2_va[4] = {0.f, - Dy23*blockSizeF, Dx23*blockSizeF, Dx23*blockSizeF - Dy23*blockSizeF};
  SIMDPP_ALIGN(16) const float Cx3_va[4] = {0.f, - Dy31*blockSizeF, Dx31*blockSizeF, Dx31*blockSizeF - Dy31*blockSizeF};

  const simdpp::float32<4> hs_eps_v = simdpp::splat(HALF_SPACE_EPSILON);

  SIMDPP_ALIGN(16) float Dy_a[4]   = {Dy12,  Dy23,  Dy31,  0.0f};
  SIMDPP_ALIGN(16) float Dx_a[4]   = {Dx12,  Dx23,  Dx31,  0.0f};
  SIMDPP_ALIGN(16) float Cy1_ba[4] = {Cy1_b, Cy2_b, Cy3_b, 0.0f};

  const simdpp::float32<4> Dy_abc = simdpp::load(Dy_a);
  const simdpp::float32<4> Dx_abc = simdpp::load(Dx_a);
  simdpp::float32<4>       Cy_abc = simdpp::load(Cy1_ba);

  //
  ///////////////////////////////////////////////////////////////////////////////// vectorized per triangle variables
  SIMDPP_ALIGN(lineSize*4) int pixelsTemp[lineSize];

  // Scan through bounding rectangle
  for (int by = miny; by <= maxy; by += lineSize)
  {
    simdpp::float32<4> Cx_abc = Cy_abc;

    for (int bx = minx; bx <= maxx; bx+= lineSize)
    {
      const simdpp::float32<4> Cx1_v = simdpp::splat<0>(Cx_abc) + ((const simdpp::float32<4>)simdpp::load(Cx1_va));
      const simdpp::float32<4> Cx2_v = simdpp::splat<1>(Cx_abc) + ((const simdpp::float32<4>)simdpp::load(Cx2_va));
      const simdpp::float32<4> Cx3_v = simdpp::splat<2>(Cx_abc) + ((const simdpp::float32<4>)simdpp::load(Cx3_va));

      const auto vInside_v4u = simdpp::bit_cast< simdpp::uint32<4>, simdpp::float32<4> >(
          ( (Cx1_v > hs_eps_v) & (Cx2_v > hs_eps_v) & (Cx3_v > hs_eps_v) ).eval().unmask()
      );

      if(simdpp::reduce_and(vInside_v4u) != 0)
      {
        // store all pixels
        //
        #pragma unroll (lineSize)
        for (int iy = 0; iy < lineSize; iy++)
        {
          const int y1 = by + iy;

          if(y1 <= maxy)
          {
            const simdpp::float32<lineSize> pixOffsY = simdpp::splat(float(iy));

            const simdpp::float32<lineSize> Cx1_bv = simdpp::splat<0>(Cx_abc);
            const simdpp::float32<lineSize> Cx2_bv = simdpp::splat<1>(Cx_abc);
            const simdpp::float32<lineSize> Cx3_bv = simdpp::splat<2>(Cx_abc);

            const simdpp::float32<lineSize> w1 = areaInvV*( Cx1_bv + Dx12v*pixOffsX - Dy12v*pixOffsY );
            const simdpp::float32<lineSize> w2 = areaInvV*( Cx3_bv + Dx31v*pixOffsX - Dy31v*pixOffsY );
            const simdpp::float32<lineSize> w3 = areaInvV*( Cx2_bv + Dx23v*pixOffsX - Dy23v*pixOffsY );

            const auto color   = ROP::DrawPixel(tri_c1, tri_c2, tri_c3, w1, w2, w3);
            const auto pixData = VROP<lineSize>::RealColorToUint32_BGRA(color);

            if(bx + lineSize < maxx)
            {
              simdpp::store_u(cbuff + frameBuf->w * y1 + bx, pixData);
            }
            else
            {
              simdpp::store(pixelsTemp, pixData);

              #pragma unroll (lineSize)
              for (int ix = 0; ix < lineSize; ix++)
                if(bx + ix <= maxx)
                  cbuff[frameBuf->w*y1 + bx + ix] = pixelsTemp[ix];
            }

          } //  if(y1 <= maxy)

        }
      }
      else if (simdpp::reduce_or(vInside_v4u) != 0)
      {
        SIMDPP_ALIGN(16) float Cx1234[4];
        simdpp::float32<4> Cy_123 = simdpp::make_float(0.0f);

        for (int iy = 0; iy < lineSize; iy++)
        {
          const int          y1     = by + iy;
          simdpp::float32<4> Cx_123 = Cy_123 + Cx_abc;

          #pragma unroll (blockSize)
          for (int ix = 0; ix < lineSize; ix++)
          {
            const auto vInside_123 = simdpp::bit_cast< simdpp::uint32<4>, simdpp::float32<4> >(
                ((Cx_123 > hs_eps_v)).eval().unmask()
            );

            const int x1 = bx + ix;
            if (x1 <= maxx && y1 <= maxy && (simdpp::reduce_and(vInside_123) != 0))
            {
              //simdpp::store(Cx1234, Cx_123);
              //const float4 color           = ROP_SCALAR::DrawPixel(tri, areaInv*float3(Cx1234[0], Cx1234[2], Cx1234[1]));
              //cbuff[frameBuf->w * y1 + x1] = RealColorToUint32_BGRA(color);
              //const simdpp::float32<lineSize> pixOffsX1 = simdpp::splat(float(ix));

              const simdpp::float32<4> w1234  = areaInvV*Cx_123;
              const simdpp::float32<4> tv1 = simdpp::load_u(&tri.c1);
              const simdpp::float32<4> tv2 = simdpp::load_u(&tri.c2);
              const simdpp::float32<4> tv3 = simdpp::load_u(&tri.c3);
              const simdpp::float32<4> color2 = tv1*simdpp::splat<0>(w1234) +
                                                tv2*simdpp::splat<2>(w1234) +
                                                tv3*simdpp::splat<1>(w1234);

              cbuff[frameBuf->w * y1 + x1] = RealColorToUint32_BGRA_SIMD(color2);
            }

            Cx_123 = Cx_123 - Dy_abc;
          }

          Cy_123 = Cy_123 + Dx_abc;
        }
      }

      Cx_abc = Cx_abc - Dy_abc*blockSizeF_4v;
    }

    Cy_abc = Cy_abc + Dx_abc*blockSizeF_4v;
  }
}




#endif