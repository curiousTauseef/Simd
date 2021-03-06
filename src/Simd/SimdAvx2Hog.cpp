/*
* Simd Library (http://ermig1979.github.io/Simd).
*
* Copyright (c) 2011-2017 Yermalayeu Ihar.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy 
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell 
* copies of the Software, and to permit persons to whom the Software is 
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in 
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/
#include "Simd/SimdStore.h"
#include "Simd/SimdBase.h"
#include "Simd/SimdMemory.h"
#include "Simd/SimdEnable.h"
#include "Simd/SimdAllocator.hpp"

#include <vector>

namespace Simd
{
#ifdef SIMD_AVX2_ENABLE    
	namespace Avx2
	{
        namespace
        {
            struct Buffer
            {
                const int size;
                __m256 * cos, * sin;
                __m256i * pos, * neg; 
                int * index;
                float * value;

                Buffer(size_t width, size_t quantization)
                    : size((int)quantization/2)
                {
                    width = AlignHi(width, A/sizeof(float));
                    _p = Allocate(width*(sizeof(int) + sizeof(float)) + (sizeof(__m256i) + sizeof(__m256))*2*size);
                    index = (int*)_p - 1;
                    value = (float*)index + width;
                    cos = (__m256*)(value + width + 1);
                    sin = cos + size;
                    pos = (__m256i*)(sin + size);
                    neg = pos + size;
                    for(int i = 0; i < size; ++i)
                    {
                        cos[i] = _mm256_set1_ps((float)::cos(i*M_PI/size));
                        sin[i] = _mm256_set1_ps((float)::sin(i*M_PI/size));
                        pos[i] = _mm256_set1_epi32(i);
                        neg[i] = _mm256_set1_epi32(size + i);
                    }
                }

                ~Buffer()
                {
                    Free(_p);
                }

            private:
                void *_p;
            };
        }

        template <bool align> SIMD_INLINE void HogDirectionHistograms(const __m256 & dx, const __m256 & dy, Buffer & buffer, size_t col)
        {
            __m256 bestDot = _mm256_setzero_ps();
            __m256i bestIndex = _mm256_setzero_si256();
            for(int i = 0; i < buffer.size; ++i)
            {
                __m256 dot = _mm256_fmadd_ps(dx, buffer.cos[i], _mm256_mul_ps(dy, buffer.sin[i]));
                __m256 mask = _mm256_cmp_ps(dot, bestDot, _CMP_GT_OS);
                bestDot = _mm256_max_ps(dot, bestDot);
                bestIndex = _mm256_blendv_epi8(bestIndex, buffer.pos[i], _mm256_castps_si256(mask));

                dot = _mm256_sub_ps(_mm256_setzero_ps(), dot);
                mask = _mm256_cmp_ps(dot, bestDot, _CMP_GT_OS);
                bestDot = _mm256_max_ps(dot, bestDot);
                bestIndex = _mm256_blendv_epi8(bestIndex, buffer.neg[i], _mm256_castps_si256(mask));
            }
            Store<align>((__m256i*)(buffer.index + col), bestIndex);
            Avx::Store<align>(buffer.value + col, Avx::Sqrt<0>(_mm256_fmadd_ps(dx, dx, _mm256_mul_ps(dy, dy))));
        }

        template <bool align> SIMD_INLINE void HogDirectionHistograms(const __m256i & t, const __m256i & l, const __m256i & r, const __m256i & b, Buffer & buffer, size_t col)
        {
            HogDirectionHistograms<align>(
                _mm256_cvtepi32_ps(_mm256_sub_epi32(_mm256_unpacklo_epi16(r, K_ZERO), _mm256_unpacklo_epi16(l, K_ZERO))), 
                _mm256_cvtepi32_ps(_mm256_sub_epi32(_mm256_unpacklo_epi16(b, K_ZERO), _mm256_unpacklo_epi16(t, K_ZERO))), 
                buffer, col + 0);
            HogDirectionHistograms<align>(
                _mm256_cvtepi32_ps(_mm256_sub_epi32(_mm256_unpackhi_epi16(r, K_ZERO), _mm256_unpackhi_epi16(l, K_ZERO))), 
                _mm256_cvtepi32_ps(_mm256_sub_epi32(_mm256_unpackhi_epi16(b, K_ZERO), _mm256_unpackhi_epi16(t, K_ZERO))), 
                buffer, col + 8);
        }

        template <bool align> SIMD_INLINE void HogDirectionHistograms(const uint8_t * src, size_t stride, Buffer & buffer, size_t col)
        {
            const uint8_t * s = src + col;
            __m256i t = LoadPermuted<false>((__m256i*)(s - stride));
            __m256i l = LoadPermuted<false>((__m256i*)(s - 1));
            __m256i r = LoadPermuted<false>((__m256i*)(s + 1));
            __m256i b = LoadPermuted<false>((__m256i*)(s + stride));
            HogDirectionHistograms<align>(PermutedUnpackLoU8(t), PermutedUnpackLoU8(l), PermutedUnpackLoU8(r), PermutedUnpackLoU8(b), buffer, col + 0);
            HogDirectionHistograms<align>(PermutedUnpackHiU8(t), PermutedUnpackHiU8(l), PermutedUnpackHiU8(r), PermutedUnpackHiU8(b), buffer, col + 16);
        }

        namespace Custom_8x8_18
        {
            struct Buffer
            {
                __m256i pos[5];
                __m256 cos[5], sin[5];
                __m128 kx[8], ky[8];

                int * index;
                float * value;
                __m128 * hist;
                size_t hs;

                Buffer(size_t width)
                {
                    width = AlignHi(width, A / sizeof(float));
                    hs = (width / 8 + 1) * 18 * sizeof(__m128);
                    _p = Allocate(width*(sizeof(int) + sizeof(float)) + hs);
                    index = (int*)_p - 1;
                    value = (float*)index + width;
                    hist = (__m128*)(value + width + 1);

                    for (int i = 0; i < 5; ++i)
                    {
                        cos[i] = _mm256_set1_ps((float)::cos(i*M_PI / 9));
                        sin[i] = _mm256_set1_ps((float)::sin(i*M_PI / 9));
                        pos[i] = _mm256_set1_epi32(i);
                    }
                    for (int i = 0; i < 8; ++i)
                    {
                        float k0 = float((15 - i * 2) / 16.0f);
                        float k1 = 1.0f - k0;
                        kx[i] = _mm_setr_ps(k0, k1, k0, k1);
                        ky[i] = _mm_setr_ps(k0, k0, k1, k1);
                    }
                    ClearHist();
                }

                ~Buffer()
                {
                    Free(_p);
                }

                void ClearHist()
                {
                    memset(hist, 0, hs);
                }

            private:
                void *_p;
            };

            const __m256i K32_1 = SIMD_MM256_SET1_EPI32(1);
            const __m256i K32_9 = SIMD_MM256_SET1_EPI32(9);
            const __m256i K32_18 = SIMD_MM256_SET1_EPI32(18);

            template <bool align> SIMD_INLINE void HogDirectionHistograms(const __m256 & dx, const __m256 & dy, Buffer & buffer, size_t col)
            {
                __m256 bestDot = _mm256_setzero_ps();
                __m256i bestIndex = _mm256_setzero_si256();
                __m256 _0 = _mm256_set1_ps(-0.0f);
                __m256 adx = _mm256_andnot_ps(_0, dx);
                __m256 ady = _mm256_andnot_ps(_0, dy);
                for (int i = 0; i < 5; ++i)
                {
                    __m256 dot = _mm256_fmadd_ps(adx, buffer.cos[i], _mm256_mul_ps(ady, buffer.sin[i]));
                    __m256 mask = _mm256_cmp_ps(dot, bestDot, _CMP_GT_OS);
                    bestDot = _mm256_max_ps(dot, bestDot);
                    bestIndex = _mm256_blendv_epi8(bestIndex, buffer.pos[i], _mm256_castps_si256(mask));
                }
                __m256i maskDx = _mm256_castps_si256(_mm256_cmp_ps(dx, _mm256_setzero_ps(), _CMP_LT_OS));
                bestIndex = _mm256_blendv_epi8(bestIndex, _mm256_sub_epi32(K32_9, bestIndex), maskDx);

                __m256i maskDy = _mm256_castps_si256(_mm256_cmp_ps(dy, _mm256_setzero_ps(), _CMP_LT_OS));
                __m256i corr = _mm256_and_si256(_mm256_castps_si256(_mm256_cmp_ps(adx, _mm256_setzero_ps(), _CMP_EQ_OS)), K32_1);
                bestIndex = _mm256_blendv_epi8(bestIndex, _mm256_sub_epi32(K32_18, _mm256_add_epi32(bestIndex, corr)), maskDy);

                bestIndex = _mm256_andnot_si256(_mm256_cmpeq_epi32(bestIndex, K32_18), bestIndex);

                Store<align>((__m256i*)(buffer.index + col), bestIndex);
                Avx::Store<align>(buffer.value + col, Avx::Sqrt<0>(_mm256_fmadd_ps(adx, adx, _mm256_mul_ps(ady, ady))));
            }

            template <int part> SIMD_INLINE __m256 CovertDifference(const __m128i & a, const __m128i & b)
            {
                return _mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(Ssse3::SubUnpackedU8<part>(a, b)));
            }

            template <bool align> SIMD_INLINE void HogDirectionHistograms(const uint8_t * src, size_t stride, Buffer & buffer, size_t col)
            {
                const uint8_t * s = src + col;
                __m128i t = Sse2::Load<false>((__m128i*)(s - stride));
                __m128i l = Sse2::Load<false>((__m128i*)(s - 1));
                __m128i r = Sse2::Load<false>((__m128i*)(s + 1));
                __m128i b = Sse2::Load<false>((__m128i*)(s + stride));
                HogDirectionHistograms<align>(CovertDifference<0>(r, l), CovertDifference<0>(b, t), buffer, col + 0);
                HogDirectionHistograms<align>(CovertDifference<1>(r, l), CovertDifference<1>(b, t), buffer, col + 8);
            }

            void AddRowToBuffer(const uint8_t * src, size_t stride, Buffer & buffer, size_t row, size_t width, size_t aligned)
            {
                const uint8_t * s = src + stride*row;
                for (size_t col = 1; col < aligned; col += HA)
                    HogDirectionHistograms<true>(s, stride, buffer, col);
                HogDirectionHistograms<false>(s, stride, buffer, width - 1 - HA);

                __m128 ky = buffer.ky[(row + 4) & 7];
                __m128 * hist = buffer.hist;
                size_t cellEnd = width / 8;

                for (size_t col = 1; col < 4; ++col)
                {
                    int index = buffer.index[col];
                    __m128 value = _mm_set1_ps(buffer.value[col]);
                    __m128 kx = buffer.kx[(col + 4) & 7];
                    hist[index] = _mm_add_ps(hist[index], _mm_mul_ps(value, _mm_mul_ps(ky, kx)));
                }
                hist += 18;

                for (size_t cell = 1, col = 4; cell < cellEnd; ++cell)
                {
                    for (size_t i = 0; i < 8; ++i, ++col)
                    {
                        int index = buffer.index[col];
                        __m128 value = _mm_set1_ps(buffer.value[col]);
                        __m128 kx = buffer.kx[i];
                        hist[index] = _mm_add_ps(hist[index], _mm_mul_ps(value, _mm_mul_ps(ky, kx)));
                    }
                    hist += 18;
                }

                for (size_t col = width - 4; col < width - 1; ++col)
                {
                    int index = buffer.index[col];
                    __m128 value = _mm_set1_ps(buffer.value[col]);
                    __m128 kx = buffer.kx[(col + 4) & 7];
                    hist[index] = _mm_add_ps(hist[index], _mm_mul_ps(value, _mm_mul_ps(ky, kx)));
                }
            }

            void AddToHistogram(Buffer & buffer, size_t row, size_t width, size_t height, float * histograms)
            {
                typedef float f18_t[18];

                float * src = (float*)buffer.hist;
                f18_t * h0 = (f18_t*)histograms + row*width - width - 1;
                f18_t * h1 = h0 + width;

                if (row == 0)
                {
                    for (size_t i = 0; i < 18; ++i)
                        h1[1][i] += src[i * 4 + 3];
                    h1++;
                    src += 72;
                    for (size_t cell = 1; cell < width; ++cell)
                    {
                        for (size_t i = 0; i < 18; ++i)
                        {
                            h1[0][i] += src[i * 4 + 2];
                            h1[1][i] += src[i * 4 + 3];
                        }
                        h1++;
                        src += 72;
                    }
                    for (size_t i = 0; i < 18; ++i)
                        h1[0][i] += src[i * 4 + 2];
                }
                else if (row == height)
                {
                    for (size_t i = 0; i < 18; ++i)
                        h0[1][i] += src[i * 4 + 1];
                    h0++;
                    src += 72;
                    for (size_t cell = 1; cell < width; ++cell)
                    {
                        for (size_t i = 0; i < 18; ++i)
                        {
                            h0[0][i] += src[i * 4 + 0];
                            h0[1][i] += src[i * 4 + 1];
                        }
                        h0++;
                        src += 72;
                    }
                    for (size_t i = 0; i < 18; ++i)
                        h0[0][i] += src[i * 4 + 0];
                }
                else
                {
                    for (size_t i = 0; i < 18; ++i)
                    {
                        h0[1][i] += src[i * 4 + 1];
                        h1[1][i] += src[i * 4 + 3];
                    }
                    h0++;
                    h1++;
                    src += 72;
                    for (size_t cell = 1; cell < width; ++cell)
                    {
                        __m128 * ps = (__m128*)src;
                        for (size_t i = 0; i < 16; i += 4)
                        {
                            __m128 s00 = _mm_unpacklo_ps(ps[i + 0], ps[i + 2]);
                            __m128 s01 = _mm_unpacklo_ps(ps[i + 1], ps[i + 3]);
                            __m128 s10 = _mm_unpackhi_ps(ps[i + 0], ps[i + 2]);
                            __m128 s11 = _mm_unpackhi_ps(ps[i + 1], ps[i + 3]);

                            _mm_storeu_ps(h0[0] + i, _mm_add_ps(_mm_loadu_ps(h0[0] + i), _mm_unpacklo_ps(s00, s01)));
                            _mm_storeu_ps(h0[1] + i, _mm_add_ps(_mm_loadu_ps(h0[1] + i), _mm_unpackhi_ps(s00, s01)));
                            _mm_storeu_ps(h1[0] + i, _mm_add_ps(_mm_loadu_ps(h1[0] + i), _mm_unpacklo_ps(s10, s11)));
                            _mm_storeu_ps(h1[1] + i, _mm_add_ps(_mm_loadu_ps(h1[1] + i), _mm_unpackhi_ps(s10, s11)));
                        }
                        for (size_t i = 16; i < 18; ++i)
                        {
                            h0[0][i] += src[i * 4 + 0];
                            h0[1][i] += src[i * 4 + 1];
                            h1[0][i] += src[i * 4 + 2];
                            h1[1][i] += src[i * 4 + 3];
                        }
                        h0++;
                        h1++;
                        src += 72;
                    }
                    for (size_t i = 0; i < 18; ++i)
                    {
                        h0[0][i] += src[i * 4 + 0];
                        h1[0][i] += src[i * 4 + 2];
                    }
                }
                buffer.ClearHist();
            }

            void HogDirectionHistograms(const uint8_t * src, size_t stride, size_t width, size_t height, float * histograms)
            {
                const size_t quantization = 18;

                size_t sizeX = width / 8, sizeY = height / 8;

                memset(histograms, 0, quantization*sizeX*sizeY*sizeof(float));

                Buffer buffer(width);

                size_t aligned = AlignLo(width - 2, HA) + 1;

                for (size_t row = 1; row < 4; ++row)
                    AddRowToBuffer(src, stride, buffer, row, width, aligned);
                AddToHistogram(buffer, 0, sizeX, sizeY, histograms);
                for (size_t row = 4, cell = 1; row < height - 4; ++row)
                {
                    AddRowToBuffer(src, stride, buffer, row, width, aligned);
                    if ((row & 7) == 3)
                        AddToHistogram(buffer, cell++, sizeX, sizeY, histograms);
                }
                for (size_t row = height - 4; row < height - 1; ++row)
                    AddRowToBuffer(src, stride, buffer, row, width, aligned);
                AddToHistogram(buffer, sizeY, sizeX, sizeY, histograms);
            }
        }

        void HogDirectionHistograms(const uint8_t * src, size_t stride, size_t width, size_t height, 
            size_t cellX, size_t cellY, size_t quantization, float * histograms)
        {
            assert(width%cellX == 0 && height%cellY == 0 && quantization%2 == 0);

            if (cellX == 8 && cellY == 8 && quantization == 18)
                Custom_8x8_18::HogDirectionHistograms(src, stride, width, height, histograms);
            else
            {
                memset(histograms, 0, quantization*(width / cellX)*(height / cellY)*sizeof(float));

                Buffer buffer(width, quantization);

                size_t alignedWidth = AlignLo(width - 2, A) + 1;

                for (size_t row = 1; row < height - 1; ++row)
                {
                    const uint8_t * s = src + stride*row;
                    for (size_t col = 1; col < alignedWidth; col += A)
                        HogDirectionHistograms<true>(s, stride, buffer, col);
                    HogDirectionHistograms<false>(s, stride, buffer, width - 1 - A);
                    Base::AddRowToHistograms(buffer.index, buffer.value, row, width, height, cellX, cellY, quantization, histograms);
                }
            }
        }

        class HogFeatureExtractor
        {
            static const size_t C = 8;
            static const size_t Q = 9;
            static const size_t Q2 = 18;

            typedef std::vector<int, Simd::Allocator<int> > Vector32i;
            typedef std::vector<float, Simd::Allocator<float> > Vector32f;

            size_t _sx, _sy, _hs;

            __m256i _pos[5];
            __m256 _cos[5], _sin[5];
            __m128 _kx[8], _ky[8];
            __m256i _Q, _Q2;

            Vector32i _index;
            Vector32f _value;
            Vector32f _buffer;
            Vector32f _histogram;
            Vector32f _norm;

            void Init(size_t w, size_t h)
            {
                _sx = w / C;
                _hs = _sx + 2;
                _sy = h / C;
                for (int i = 0; i < 5; ++i)
                {
                    _cos[i] = _mm256_set1_ps((float)::cos(i*M_PI / Q));
                    _sin[i] = _mm256_set1_ps((float)::sin(i*M_PI / Q));
                    _pos[i] = _mm256_set1_epi32(i);
                }
                for (int i = 0; i < C; ++i)
                {
                    float k0 = float((15 - i * 2) / 16.0f);
                    float k1 = 1.0f - k0;
                    _kx[i] = _mm_setr_ps(k0, k1, k0, k1);
                    _ky[i] = _mm_setr_ps(k0, k0, k1, k1);
                }
                _Q = _mm256_set1_epi32(Q);
                _Q2 = _mm256_set1_epi32(Q2);

                _index.resize(w);
                _value.resize(w);
                _buffer.resize((_sx + 1) * 4 * Q2);
                _histogram.resize((_sx + 2)*(_sy + 2)*Q2);
                _norm.resize((_sx + 2)*(_sy + 2));
            }

            template <bool align> SIMD_INLINE void GetHistogram(const __m256 & dx, const __m256 & dy, size_t col)
            {
                __m256 bestDot = _mm256_setzero_ps();
                __m256i bestIndex = _mm256_setzero_si256();
                __m256 _0 = _mm256_set1_ps(-0.0f);
                __m256 adx = _mm256_andnot_ps(_0, dx);
                __m256 ady = _mm256_andnot_ps(_0, dy);
                for (int i = 0; i < 5; ++i)
                {
                    __m256 dot = _mm256_fmadd_ps(adx, _cos[i], _mm256_mul_ps(ady, _sin[i]));
                    __m256 mask = _mm256_cmp_ps(dot, bestDot, _CMP_GT_OS);
                    bestDot = _mm256_max_ps(dot, bestDot);
                    bestIndex = _mm256_blendv_epi8(bestIndex, _pos[i], _mm256_castps_si256(mask));
                }
                __m256i maskDx = _mm256_castps_si256(_mm256_cmp_ps(dx, _mm256_setzero_ps(), _CMP_LT_OS));
                bestIndex = _mm256_blendv_epi8(bestIndex, _mm256_sub_epi32(_Q, bestIndex), maskDx);

                __m256i maskDy = _mm256_castps_si256(_mm256_cmp_ps(dy, _mm256_setzero_ps(), _CMP_LT_OS));
                __m256i corr = _mm256_and_si256(_mm256_castps_si256(_mm256_cmp_ps(adx, _mm256_setzero_ps(), _CMP_EQ_OS)), K32_00000001);
                bestIndex = _mm256_blendv_epi8(bestIndex, _mm256_sub_epi32(_Q2, _mm256_add_epi32(bestIndex, corr)), maskDy);

                bestIndex = _mm256_andnot_si256(_mm256_cmpeq_epi32(bestIndex, _Q2), bestIndex);

                Store<align>((__m256i*)(_index.data() + col), bestIndex);
                Avx::Store<align>(_value.data() + col, Avx::Sqrt<0>(_mm256_fmadd_ps(adx, adx, _mm256_mul_ps(ady, ady))));
            }

            template <int part> SIMD_INLINE __m256 CovertDifference(const __m128i & a, const __m128i & b)
            {
                return _mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(Ssse3::SubUnpackedU8<part>(a, b)));
            }

            template <bool align> SIMD_INLINE void GetHistogram(const uint8_t * src, size_t stride, size_t col)
            {
                const uint8_t * s = src + col;
                __m128i t = Sse2::Load<false>((__m128i*)(s - stride));
                __m128i l = Sse2::Load<false>((__m128i*)(s - 1));
                __m128i r = Sse2::Load<false>((__m128i*)(s + 1));
                __m128i b = Sse2::Load<false>((__m128i*)(s + stride));
                GetHistogram<align>(CovertDifference<0>(r, l), CovertDifference<0>(b, t), col + 0);
                GetHistogram<align>(CovertDifference<1>(r, l), CovertDifference<1>(b, t), col + 8);
            }

            void AddRowToBuffer(const uint8_t * src, size_t stride, size_t row, size_t width, size_t aligned)
            {
                const uint8_t * s = src + stride*row;
                GetHistogram<false>(s, stride, 1);
                for (size_t col = HA; col < aligned; col += HA)
                    GetHistogram<false>(s, stride, col);
                GetHistogram<false>(s, stride, width - 1 - HA);

                __m128 ky = _ky[(row + 4) & 7];
                __m128 * buffer = (__m128*)_buffer.data();
                for (size_t col = 1, n = C, i = 5; col < width - 1; i = 0, n = Simd::Min<size_t>(C, width - col - 1))
                {
                    for (; i < n; ++i, ++col)
                    {
                        int index = _index[col];
                        __m128 value = _mm_set1_ps(_value[col]);
                        buffer[index] = _mm_add_ps(buffer[index], _mm_mul_ps(value, _mm_mul_ps(ky, _kx[i])));
                    }
                    buffer += Q2;
                }
            }

            void AddToHistogram(size_t row, size_t width, size_t height)
            {
                typedef float f18_t[18];

                float * src = _buffer.data();
                f18_t * h0 = (f18_t*)_histogram.data() + row*_hs;
                f18_t * h1 = h0 + _hs;

                for (size_t cell = 0; cell <= width; ++cell)
                {
                    __m128 * ps = (__m128*)src;
                    for (size_t i = 0; i < 16; i += 4)
                    {
                        __m128 s00 = _mm_unpacklo_ps(ps[i + 0], ps[i + 2]);
                        __m128 s01 = _mm_unpacklo_ps(ps[i + 1], ps[i + 3]);
                        __m128 s10 = _mm_unpackhi_ps(ps[i + 0], ps[i + 2]);
                        __m128 s11 = _mm_unpackhi_ps(ps[i + 1], ps[i + 3]);

                        _mm_storeu_ps(h0[0] + i, _mm_add_ps(_mm_loadu_ps(h0[0] + i), _mm_unpacklo_ps(s00, s01)));
                        _mm_storeu_ps(h0[1] + i, _mm_add_ps(_mm_loadu_ps(h0[1] + i), _mm_unpackhi_ps(s00, s01)));
                        _mm_storeu_ps(h1[0] + i, _mm_add_ps(_mm_loadu_ps(h1[0] + i), _mm_unpacklo_ps(s10, s11)));
                        _mm_storeu_ps(h1[1] + i, _mm_add_ps(_mm_loadu_ps(h1[1] + i), _mm_unpackhi_ps(s10, s11)));
                    }
                    __m128 s0 = _mm_add_ps(_mm_unpacklo_ps(ps[16], ps[17]), _mm_loadh_pi(_mm_loadl_pi(_mm_setzero_ps(), (__m64*)(h0[0] + 16)), (__m64*)(h0[1] + 16)));
                    __m128 s1 = _mm_add_ps(_mm_unpackhi_ps(ps[16], ps[17]), _mm_loadh_pi(_mm_loadl_pi(_mm_setzero_ps(), (__m64*)(h1[0] + 16)), (__m64*)(h1[1] + 16)));
                    _mm_storel_pi((__m64*)(h0[0] + 16), s0);
                    _mm_storeh_pi((__m64*)(h0[1] + 16), s0);
                    _mm_storel_pi((__m64*)(h1[0] + 16), s1);
                    _mm_storeh_pi((__m64*)(h1[1] + 16), s1);
                    h0++;
                    h1++;
                    src += 72;
                }
                SetZero(_buffer);
            }

            void EstimateHistogram(const uint8_t * src, size_t stride, size_t width, size_t height)
            {
                SetZero(_histogram);

                size_t aligned = AlignHi(width - 1, HA) - HA;

                SetZero(_buffer);
                for (size_t row = 1; row < 4; ++row)
                    AddRowToBuffer(src, stride, row, width, aligned);
                AddToHistogram(0, _sx, _sy);
                for (size_t row = 4, cell = 1; row < height - 4; ++row)
                {
                    AddRowToBuffer(src, stride, row, width, aligned);
                    if ((row & 7) == 3)
                        AddToHistogram(cell++, _sx, _sy);
                }
                for (size_t row = height - 4; row < height - 1; ++row)
                    AddRowToBuffer(src, stride, row, width, aligned);
                AddToHistogram(_sy, _sx, _sy);
            }

            SIMD_INLINE float GetNorm(const float * src)
            {
                __m256 norm = _mm256_add_ps(_mm256_loadu_ps(src), _mm256_loadu_ps(src + Q));
                norm = _mm256_mul_ps(norm, norm);
                norm = _mm256_hadd_ps(norm, norm);
                norm = _mm256_hadd_ps(norm, norm);
                float buf[8];
                _mm256_storeu_ps(buf, norm);
                return buf[0] + buf[4] + Simd::Square(src[Q - 1] + src[Q2 - 1]);
            }

            void EstimateNorm()
            {
                SetZero(_norm);
                for (size_t y = 0, i = 0; y < _sy; y++)
                {
                    const float * h = _histogram.data() + ((y + 1)*_hs + 1)*Q2;
                    float * n = _norm.data() + (y + 1)*_hs + 1;
                    for (size_t x = 0; x < _sx; x++, i++)
                        n[x] = GetNorm(h + x*Q2);
                }
            }

            void ExtractFeatures(float * features)
            {
                __m128 _02 = _mm_set1_ps(0.2f);
                __m128 _05 = _mm_set1_ps(0.5f);
                __m128 _02357 = _mm_set1_ps(0.2357f);
                __m128 eps = _mm_set1_ps(0.0001f);
                for (size_t y = 0; y < _sy; y++)
                {
                    float * ph = _histogram.data() + ((y + 1)*_hs + 1)*Q2;
                    for (size_t x = 0; x < _sx; x++)
                    {
                        float * dst = features + (y*_sx + x) * 31;

                        float * p0 = _norm.data() + y*_hs + x;
                        float * p1 = p0 + _hs;
                        float * p2 = p1 + _hs;

                        __m128 n = _mm_setr_ps(
                            p1[1] + p1[2] + p2[1] + p2[2],
                            p0[1] + p0[2] + p1[1] + p1[2],
                            p1[0] + p1[1] + p2[0] + p2[1],
                            p0[0] + p0[1] + p1[0] + p1[1]);

                        n = _mm_rsqrt_ps(_mm_add_ps(n, eps));

                        __m128 t = _mm_setzero_ps();

                        float * src = ph + x*Q2;
                        for (int o = 0; o < 16; o += 4)
                        {
                            __m128 s = _mm_loadu_ps(src);
                            __m128 h0 = _mm_min_ps(_mm_mul_ps(Sse2::Broadcast<0>(s), n), _02);
                            __m128 h1 = _mm_min_ps(_mm_mul_ps(Sse2::Broadcast<1>(s), n), _02);
                            __m128 h2 = _mm_min_ps(_mm_mul_ps(Sse2::Broadcast<2>(s), n), _02);
                            __m128 h3 = _mm_min_ps(_mm_mul_ps(Sse2::Broadcast<3>(s), n), _02);
                            t = _mm_add_ps(t, _mm_add_ps(_mm_add_ps(h0, h1), _mm_add_ps(h2, h3)));
                            _mm_storeu_ps(dst, _mm_mul_ps(_05, _mm_hadd_ps(_mm_hadd_ps(h0, h1), _mm_hadd_ps(h2, h3))));
                            dst += 4;
                            src += 4;
                        }
                        {
                            __m128 h0 = _mm_min_ps(_mm_mul_ps(_mm_set1_ps(*src++), n), _02);
                            __m128 h1 = _mm_min_ps(_mm_mul_ps(_mm_set1_ps(*src++), n), _02);
                            t = _mm_add_ps(t, _mm_add_ps(h0, h1));
                            __m128 h = _mm_hadd_ps(h0, h1);
                            _mm_storeu_ps(dst, _mm_mul_ps(_05, _mm_hadd_ps(h, h)));
                            dst += 2;
                        }

                        src = ph + x*Q2;
                        for (int o = 0; o < 8; o += 4)
                        {
                            __m128 s = _mm_add_ps(_mm_loadu_ps(src), _mm_loadu_ps(src + Q));
                            __m128 h0 = _mm_min_ps(_mm_mul_ps(Sse2::Broadcast<0>(s), n), _02);
                            __m128 h1 = _mm_min_ps(_mm_mul_ps(Sse2::Broadcast<1>(s), n), _02);
                            __m128 h2 = _mm_min_ps(_mm_mul_ps(Sse2::Broadcast<2>(s), n), _02);
                            __m128 h3 = _mm_min_ps(_mm_mul_ps(Sse2::Broadcast<3>(s), n), _02);
                            _mm_storeu_ps(dst, _mm_mul_ps(_05, _mm_hadd_ps(_mm_hadd_ps(h0, h1), _mm_hadd_ps(h2, h3))));
                            dst += 4;
                            src += 4;
                        }
                        {
                            __m128 s = _mm_set1_ps(src[0] + src[Q]);
                            __m128 h = _mm_min_ps(_mm_mul_ps(s, n), _02);
                            h = _mm_mul_ps(_05, h);
                            h = _mm_hadd_ps(h, h);
                            h = _mm_hadd_ps(h, h);
                            _mm_store_ss(dst++, h);
                        }
                        _mm_storeu_ps(dst, _mm_mul_ps(t, _02357));
                    }
                }
            }

        public:

            void Run(const uint8_t * src, size_t stride, size_t width, size_t height, float * features)
            {
                Init(width, height);

                EstimateHistogram(src, stride, width, height);

                EstimateNorm();

                ExtractFeatures(features);
            }
        };

        void HogExtractFeatures(const uint8_t * src, size_t stride, size_t width, size_t height, float * features)
        {
            assert(width % 8 == 0 && height % 8 == 0 && width >= 16 && height >= 16);

            HogFeatureExtractor extractor;
            extractor.Run(src, stride, width, height, features);
        }

        namespace HogSeparableFilter_Detail
        {
            template <int add, bool end> SIMD_INLINE void Set(float * dst, const __m256 & value, const __m256 & mask)
            {
                Avx::Store<false>(dst, value);
            }

            template <> SIMD_INLINE void Set<1, false>(float * dst, const __m256 & value, const __m256 & mask)
            {
                Avx::Store<false>(dst, _mm256_add_ps(Avx::Load<false>(dst), value));
            }

            template <> SIMD_INLINE void Set<1, true>(float * dst, const __m256 & value, const __m256 & mask)
            {
                Avx::Store<false>(dst, _mm256_add_ps(Avx::Load<false>(dst), _mm256_and_ps(value, mask)));
            }
        }

        class HogSeparableFilter
        {
            typedef std::vector<float, Simd::Allocator<float> > Vector32f;
            typedef std::vector<__m256, Simd::Allocator<__m256> > Vector256f;

            size_t _w, _h, _s;
            Vector32f _buffer;
            Vector256f _filter;

            void Init(size_t w, size_t h, size_t cs, size_t rs)
            {
                _w = w - cs + 1;
                _s = AlignHi(_w, F);
                _h = h - rs + 1;
                _buffer.resize(_s*h);
            }

            template <bool align> void FilterCols(const float * src, const __m256 * filter, size_t size, float * dst)
            {
                __m256 sum = _mm256_setzero_ps();
                for (size_t i = 0; i < size; ++i)
                    sum = _mm256_fmadd_ps(Avx::Load<false>(src + i), filter[i], sum);
                Avx::Store<align>(dst, sum);
            }

            void FilterCols(const float * src, size_t srcStride, size_t width, size_t height, const float * filter, size_t size, float * dst, size_t dstStride)
            {
                _filter.resize(size);
                for (size_t i = 0; i < size; ++i)
                    _filter[i] = _mm256_set1_ps(filter[i]);

                size_t alignedWidth = AlignLo(width, F);

                for (size_t row = 0; row < height; ++row)
                {
                    for (size_t col = 0; col < alignedWidth; col += F)
                        FilterCols<true>(src + col, _filter.data(), size, dst + col);
                    if (alignedWidth != width)
                        FilterCols<false>(src + width - F, _filter.data(), size, dst + width - F);
                    src += srcStride;
                    dst += dstStride;
                }
            }

            template <bool align> void FilterCols_10(const float * src, const __m256 * filter, float * dst)
            {
                __m256 sum0 = _mm256_setzero_ps(), sum1 = _mm256_setzero_ps();
                __m256  src0 = Avx::Load<false>(src + 0);
                __m256  src4 = Avx::Load<false>(src + 4);
                __m256  src8 = Avx::Load<false>(src + 8);
                sum0 = _mm256_fmadd_ps(src0, filter[0], sum0);
                sum1 = _mm256_fmadd_ps(Alignr<1>(src0, src4), filter[1], sum1);
                sum0 = _mm256_fmadd_ps(Alignr<2>(src0, src4), filter[2], sum0);
                sum1 = _mm256_fmadd_ps(Alignr<3>(src0, src4), filter[3], sum1);
                sum0 = _mm256_fmadd_ps(src4, filter[4], sum0);
                sum1 = _mm256_fmadd_ps(Alignr<1>(src4, src8), filter[5], sum1);
                sum0 = _mm256_fmadd_ps(Alignr<2>(src4, src8), filter[6], sum0);
                sum1 = _mm256_fmadd_ps(Alignr<3>(src4, src8), filter[7], sum1);
                sum0 = _mm256_fmadd_ps(src8, filter[8], sum0);
                sum1 = _mm256_fmadd_ps(Avx::Load<false>(src + 9), filter[9], sum1);
                Avx::Store<align>(dst, _mm256_add_ps(sum0, sum1));
            }

            void FilterCols_10(const float * src, size_t srcStride, size_t width, size_t height, const float * filter, float * dst, size_t dstStride)
            {
                __m256 _filter[10];
                for (size_t i = 0; i < 10; ++i)
                    _filter[i] = _mm256_set1_ps(filter[i]);

                size_t alignedWidth = AlignLo(width, F);

                for (size_t row = 0; row < height; ++row)
                {
                    for (size_t col = 0; col < alignedWidth; col += F)
                        FilterCols_10<true>(src + col, _filter, dst + col);
                    if (alignedWidth != width)
                        FilterCols_10<false>(src + width - F, _filter, dst + width - F);
                    src += srcStride;
                    dst += dstStride;
                }
            }

            template <int add, bool end> void FilterRows(const float * src, size_t stride, const __m256 * filter, size_t size, float * dst, const __m256 & mask)
            {
                __m256 sum = _mm256_setzero_ps();
                for (size_t i = 0; i < size; ++i, src += stride)
                    sum = _mm256_fmadd_ps(Avx::Load<!end>(src), filter[i], sum);
                HogSeparableFilter_Detail::Set<add, end>(dst, sum, mask);
            }

            template <int add, bool end> void FilterRows2x(const float * src, size_t stride, const __m256 * filter, size_t size, float * dst, const __m256 & mask)
            {
                __m256 sums[2] = { _mm256_setzero_ps(), _mm256_setzero_ps()};
                for (size_t i = 0; i < size; ++i, src += stride)
                {
                    __m256 f = filter[i];
                    sums[0] = _mm256_fmadd_ps(Avx::Load<!end>(src + 0), f, sums[0]);
                    sums[1] = _mm256_fmadd_ps(Avx::Load<!end>(src + F), f, sums[1]);
                }
                HogSeparableFilter_Detail::Set<add, end>(dst + 0, sums[0], mask);
                HogSeparableFilter_Detail::Set<add, end>(dst + F, sums[1], mask);
            }

            template <int add> void FilterRows(const float * src, size_t srcStride, size_t width, size_t height, const float * filter, size_t size, float * dst, size_t dstStride)
            {
                _filter.resize(size);
                for (size_t i = 0; i < size; ++i)
                    _filter[i] = _mm256_set1_ps(filter[i]);

                size_t fullAlignedWidth =  AlignLo(width, DF);
                size_t partialAlignedWidth = AlignLo(width, F);
                __m256 tailMask = RightNotZero(width - partialAlignedWidth);

                for (size_t row = 0; row < height; ++row)
                {
                    size_t col = 0;
                    for (; col < fullAlignedWidth; col += DF)
                        FilterRows2x<add, false>(src + col, srcStride, _filter.data(), size, dst + col, tailMask);
                    for (; col < partialAlignedWidth; col += F)
                        FilterRows<add, false>(src + col, srcStride, _filter.data(), size, dst + col, tailMask);
                    if (partialAlignedWidth != width)
                        FilterRows<add, true>(src + width - F, srcStride, _filter.data(), size, dst + width - F, tailMask);
                    src += srcStride;
                    dst += dstStride;
                }
            }

        public:

            void Run(const float * src, size_t srcStride, size_t width, size_t height,
                const float * colFilter, size_t colSize, const float * rowFilter, size_t rowSize, float * dst, size_t dstStride, int add)
            {
                Init(width, height, colSize, rowSize);

                if (colSize == 10)
                    FilterCols_10(src, srcStride, _w, height, colFilter, _buffer.data(), _s);
                else
                    FilterCols(src, srcStride, _w, height, colFilter, colSize, _buffer.data(), _s);

                if (add)
                    FilterRows<1>(_buffer.data(), _s, _w, _h, rowFilter, rowSize, dst, dstStride);
                else
                    FilterRows<0>(_buffer.data(), _s, _w, _h, rowFilter, rowSize, dst, dstStride);
            }
        };

        void HogFilterSeparable(const float * src, size_t srcStride, size_t width, size_t height,
            const float * colFilter, size_t colSize, const float * rowFilter, size_t rowSize, float * dst, size_t dstStride, int add)
        {
            assert(width >= F + colSize - 1 && height >= rowSize - 1);

            HogSeparableFilter filter;
            filter.Run(src, srcStride, width, height, colFilter, colSize, rowFilter, rowSize, dst, dstStride, add);
        }
	}
#endif
}
