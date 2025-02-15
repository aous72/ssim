/*
 * Copyright (c) 2021, Romain Bailly
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include "ssim_internal.h"

#if RMGR_ARCH_IS_X86_ANY

#if !defined(__AVX512F__) || 1 // Disable AVX-152 until it has been tested

namespace rmgr { namespace ssim { namespace avx512
{
    const GaussianBlurFct g_gaussianBlurFct = NULL;
}}}

#else

#include <cassert>
#include <cstring>
#include <immintrin.h>

namespace rmgr { namespace ssim { namespace avx512
{

#if RMGR_SSIM_USE_DOUBLE
    typedef __m512d        Vector;
    #define VEC_SIZE       8
    #define VCOEFF(val)    val, val, val, val, val, val, val, val
    #define VLOADA(addr)   _mm512_load_pd((addr))
    #define VLOADU(addr)   _mm512_loadu_pd((addr))
    #define VADD(a,b)      _mm512_add_pd((a), (b))
    #define VSUB(a,b)      _mm512_sub_pd((a), (b))
    #define VMUL(a,b)      _mm512_mul_pd((a), (b))
    #define VFMADD(a,b,c)  _mm512_fmadd_pd((a), (b), (c))
    #define VSET1(val)     _mm512_set1_pd(val)
#else
    typedef __m512         Vector;
    #define VEC_SIZE       16
    #define VCOEFF(val)    val##f, val##f, val##f, val##f, val##f, val##f, val##f, val##f, val##f, val##f, val##f, val##f, val##f, val##f, val##f, val##f
    #define VLOADA(addr)   _mm512_load_ps((addr))
    #define VLOADU(addr)   _mm512_loadu_ps((addr))
    #define VADD(a,b)      _mm512_add_ps((a), (b))
    #define VSUB(a,b)      _mm512_sub_ps((a), (b))
    #define VMUL(a,b)      _mm512_mul_ps((a), (b))
    #define VFMADD(a,b,c)  _mm512_fmadd_ps((a), (b), (c))
    #define VSET1(val)     _mm512_set1_ps(val)
#endif

//=================================================================================================
// gaussian_blur()

static void gaussian_blur(Float* dest, ptrdiff_t destStride, const Float* srce, ptrdiff_t srceStride, int32_t width, int32_t height, const Float kernel[], int radius) RMGR_NOEXCEPT
{
    assert(width  > 0);
    assert(height > 0);
    assert(srceStride >= width + 2*radius);
    assert(srceStride % VEC_SIZE == 0);
    assert(destStride % VEC_SIZE == 0);
    assert(reinterpret_cast<uintptr_t>(srce) % (VEC_SIZE * sizeof(Float)) == 0);
    assert(reinterpret_cast<uintptr_t>(dest) % (VEC_SIZE * sizeof(Float)) == 0);

    const size_t rowSize = width * sizeof(Float);

#if 0
    // Generic AVX-512 implementation.
    // Note that we always process in batches of 8 because we know the buffer is large enough.

    const int32_t kernelStride = 2*radius + 1;

    for (int32_t yd=0; yd<height; ++yd)
    {
        memset(dest, 0, rowSize);
        for (int32_t yk=-radius; yk<=radius; ++yk)
        {
            const Float* kernelRow = kernel + yk * kernelStride;
            for (int32_t xk=-radius; xk<=radius; ++xk)
            {
                const Vector k  = VSET1(kernelRow[xk]);
                const Float* s  = srce + (yd+yk) * srceStride + xk;
                Vector*      vd = reinterpret_cast<Vector*>(dest);

                int32_t xd = width;

                // 8x unrolled FMA loop
                while ((xd -= 8*VEC_SIZE) >= 0)
                {
                    vd[0] = VFMADD(VLOADU(s), k, vd[0]);  s+=VEC_SIZE;
                    vd[1] = VFMADD(VLOADU(s), k, vd[1]);  s+=VEC_SIZE;
                    vd[2] = VFMADD(VLOADU(s), k, vd[2]);  s+=VEC_SIZE;
                    vd[3] = VFMADD(VLOADU(s), k, vd[3]);  s+=VEC_SIZE;
                    vd[4] = VFMADD(VLOADU(s), k, vd[4]);  s+=VEC_SIZE;
                    vd[5] = VFMADD(VLOADU(s), k, vd[5]);  s+=VEC_SIZE;
                    vd[6] = VFMADD(VLOADU(s), k, vd[6]);  s+=VEC_SIZE;
                    vd[7] = VFMADD(VLOADU(s), k, vd[7]);  s+=VEC_SIZE;
                    vd += 8;
                }
                xd += 8 * VEC_SIZE;
            }
        }

        dest += destStride;
    }

#else

    // Faster version, but tailored for the radius==5 case
    // Here too we don't care about buffer overrun as we know the buffer was allocated accordingly.

    assert(radius == 5);
    (void)kernel;

    // The precomputed 21 unique values of the Gaussian kernel
    static RMGR_ALIGNED_VAR(64, const Float, k21[21 * VEC_SIZE]) =
    {
        VCOEFF(7.07622393965721130e-02),
        VCOEFF(5.66619709134101868e-02), VCOEFF(4.53713610768318176e-02),
        VCOEFF(2.90912277996540070e-02), VCOEFF(2.32944320887327194e-02), VCOEFF(1.19597595185041428e-02),
        VCOEFF(9.57662798464298248e-03), VCOEFF(7.66836293041706085e-03), VCOEFF(3.93706932663917542e-03), VCOEFF(1.29605561960488558e-03),
        VCOEFF(2.02135881409049034e-03), VCOEFF(1.61857774946838617e-03), VCOEFF(8.31005279906094074e-04), VCOEFF(2.73561221547424793e-04), VCOEFF(5.77411265112459660e-05),
        VCOEFF(2.73561221547424793e-04), VCOEFF(2.19050692976452410e-04), VCOEFF(1.12464345875196159e-04), VCOEFF(3.70224843209143728e-05), VCOEFF(7.81441485742107034e-06), VCOEFF(1.05756600987660931e-06)
    };

    const Float* s = srce -  5*srceStride;
    Float*       d = dest - 10*destStride;

    #define k(x,y)  *reinterpret_cast<const Vector*>(&k21[((y) * ((y)+1)/2 + (x)) * VEC_SIZE])

    width = (width + (2*VEC_SIZE*1)) & ~(2*VEC_SIZE-1); // Round up width to a multiple of vector length

    int32_t yd = height + 2*radius;
    do
    {
        memset(d+10*destStride, 0, rowSize);
        int32_t xd = width;
        do
        {
            // Unrolled code that exploits all the symmetries in the kernel.
            // Vertical and horizontal symmetries allow us to save a lot of computation thanks to factorization.
            // The diagonal symmetries say that k(x,y) == k(y,x), we cannot exploit that for factorizing,
            // but we can load fewer coefficients by keeping x <= y (21 coefficients instead of 36).

            const Vector s0 = VLOADA(s);
            const Vector s1 = VADD(VLOADU(s+1), VLOADU(s-1)); // Factorization due to the vertical line of symmetry
            const Vector s2 = VADD(VLOADU(s+2), VLOADU(s-2)); // Factorization due to the vertical line of symmetry
            const Vector s3 = VADD(VLOADU(s+3), VLOADU(s-3)); // Factorization due to the vertical line of symmetry
            const Vector s4 = VADD(VLOADU(s+4), VLOADU(s-4)); // Factorization due to the vertical line of symmetry
            const Vector s5 = VADD(VLOADU(s+5), VLOADU(s-5)); // Factorization due to the vertical line of symmetry

            Vector sum0 = VMUL(s0, k(0,0));
            Vector sum1 = VMUL(s0, k(0,1));
            Vector sum2 = VMUL(s0, k(0,2));
            Vector sum3 = VMUL(s0, k(0,3));
            Vector sum4 = VMUL(s0, k(0,4));
            Vector sum5 = VMUL(s0, k(0,5));

            sum0 = VFMADD(s1, k(0,1), sum0);
            sum1 = VFMADD(s1, k(1,1), sum1);
            sum2 = VFMADD(s1, k(1,2), sum2);
            sum3 = VFMADD(s1, k(1,3), sum3);
            sum4 = VFMADD(s1, k(1,4), sum4);
            sum5 = VFMADD(s1, k(1,5), sum5);

            sum0 = VFMADD(s2, k(0,2), sum0);
            sum1 = VFMADD(s2, k(1,2), sum1);
            sum2 = VFMADD(s2, k(2,2), sum2);
            sum3 = VFMADD(s2, k(2,3), sum3);
            sum4 = VFMADD(s2, k(2,4), sum4);
            sum5 = VFMADD(s2, k(2,5), sum5);

            sum0 = VFMADD(s3, k(0,3), sum0);
            sum1 = VFMADD(s3, k(1,3), sum1);
            sum2 = VFMADD(s3, k(2,3), sum2);
            sum3 = VFMADD(s3, k(3,3), sum3);
            sum4 = VFMADD(s3, k(3,4), sum4);
            sum5 = VFMADD(s3, k(3,5), sum5);

            sum0 = VFMADD(s4, k(0,4), sum0);
            sum1 = VFMADD(s4, k(1,4), sum1);
            sum2 = VFMADD(s4, k(2,4), sum2);
            sum3 = VFMADD(s4, k(3,4), sum3);
            sum4 = VFMADD(s4, k(4,4), sum4);
            sum5 = VFMADD(s4, k(4,5), sum5);

            sum0 = VFMADD(s5, k(0,5), sum0);
            sum1 = VFMADD(s5, k(1,5), sum1);
            sum2 = VFMADD(s5, k(2,5), sum2);
            sum3 = VFMADD(s5, k(3,5), sum3);
            sum4 = VFMADD(s5, k(4,5), sum4);
            sum5 = VFMADD(s5, k(5,5), sum5);

            // Reuse sums thanks to horizontal line of symmetry
            Vector* vd = reinterpret_cast<Vector*>(d);
            *vd = VADD(sum5, *vd);  vd+=destStride/VEC_SIZE;
            *vd = VADD(sum4, *vd);  vd+=destStride/VEC_SIZE;
            *vd = VADD(sum3, *vd);  vd+=destStride/VEC_SIZE;
            *vd = VADD(sum2, *vd);  vd+=destStride/VEC_SIZE;
            *vd = VADD(sum1, *vd);  vd+=destStride/VEC_SIZE;
            *vd = VADD(sum0, *vd);  vd+=destStride/VEC_SIZE;
            *vd = VADD(sum1, *vd);  vd+=destStride/VEC_SIZE;
            *vd = VADD(sum2, *vd);  vd+=destStride/VEC_SIZE;
            *vd = VADD(sum3, *vd);  vd+=destStride/VEC_SIZE;
            *vd = VADD(sum4, *vd);  vd+=destStride/VEC_SIZE;
            *vd = VADD(sum5, *vd);

            s += VEC_SIZE;
            d += VEC_SIZE;
        }
        while ((xd -= VEC_SIZE) != 0);

        s += srceStride - width;
        d += destStride - width;
    }
    while (--yd);

    #undef k
#endif

    _mm256_zeroupper();
}


const GaussianBlurFct g_gaussianBlurFct = gaussian_blur;


}}} // namespace rmgr::ssim::avx512

#endif // __AVX512F__
#endif // RMGR_ARCH_IS_X86_ANY
