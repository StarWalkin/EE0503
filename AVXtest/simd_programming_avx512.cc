#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include <cmath>
#include <cstdlib>

#include "../matmul.h"
#include "common.h"


#include <immintrin.h>

namespace matmul {
void MatmulOperator::mat_mul_simd_programming(struct matmul_params *params) {  //还没开始写
    const struct matrix *A = &params->A, *B = &params->B, *C = &params->C;
    const int block_size = params->block_size;  // block_size = 32

    quantize_fp32_to_int8(A->data_ptr, A->int8_data_ptr, params->A_scales, A->row * A->column, block_size);

    int m = C->row, n = C->column, k = A->column;
    // A: m x k; B: n x k; C: m x n
    for (int row = 0; row < m; row++) {
        for (int col = 0; col < n; col++) {

            // order of weights with QM_x86:
            // origin order: (w0,w1), (w2,w3), (w4,w5), (w6,w7), (w8, w9), ... (w62,w63)
            // QM_ARM order: (w0,w32),(w1,w33),(w2,w34),(w3,w35),(w4, w36),... (w31,w63)
            //               |--|
            //               4 bits
            //               |------|
            //               8 bits (byte)
            //            low|----------------------------------------------------------|high
            //               0                         256 bit
            __m256 acc0 = _mm256_setzero_ps();
            // pointer of the int4 weights
            const __m256i *w_start = (__m256i *)&B->int4_data_ptr[col * k / 2];
            // pointer of the int8 activation
            const __m256i *a_start = (__m256i *)&A->int8_data_ptr[row * k];
            // scale of weight
            float *s_ptr = &params->scales[col * k / 32];
            // scale of activation
            float *sa_ptr = &params->A_scales[row * k / 32];

            const int num_block = k / block_size;
            // Compute two blocks in each iteration
            for (int q = 0; q < num_block; q += 2) {
                // lowbit mask
                const __m256i lowMask = _mm256_set1_epi8(0xF);

                /*
                   We will accelerate the program using x86 Intrinsics. You can check the documentation of operations
                   at: https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html#avxnewtechs=AVX2
                */
                // TODO: Unpack 64 4-bit (one __mm256i) weights into 64 8-bit (two __mm256i)
                // (1) load 256 bit from w_strat with _mm256_loadu_si256
                // (2) use `_mm256_and_si256` and lowMask to extract the lower half of wegihts
                // (3) use `_mm256_srli_epi16` and `_mm256_and_si256` with lowMask to extract the upper half of weights
                __m256i raw_w = _mm256_loadu_si256(w_start);

                // TODO: apply zero_point to weights and convert the range from (0, 15) to (-8, 7)
                // Hint: using `_mm256_sub_epi8` to the lower-half and upper-half vectors of weights
                // Note: Store the lower half and upper half of weights into `w_0` and `w_128`, respectively
                const __m256i zero_point = _mm256_set1_epi8(8);
                __m256i w_0, w_128;

                // Perform int8 dot product with _mm256_maddubs_epi16
                /* Syntax of _mm256_maddubs_epi16:
                   __m256i _mm256_maddubs_epi16(__m256i s1, __m256i s2): Multiplies vertically each unsigned byte of
                   source vector s1 with the corresponding signed byte of source vector s2, producing intermediate,
                   signed 16-bit integers. Each adjacent pair of signed words is added, and the saturated result is
                   packed to the destination vector.
                */
                // To utilize _mm256_maddubs_epi16 which only takes unsigned s1, we need to:
                // (1) Get the absolute values of weights (for both lower and upper halves)
                // (2) Change the sign of activation (a0-a31 and a32-a63) depending on the sign of corresponding weights
                // (stored as another variable) (3) Perform dot product with _mm256_maddubs_epi16 and store the lower
                // and upper halves sum in `dot` and `dot2`
                __m256i dot, dot2;
                // Get absolute values of x vectors
                const __m256i ax = _mm256_sign_epi8(w_0, w_0);
                const __m256i ax2 = _mm256_sign_epi8(w_128, w_128);
                // Load activation
                __m256i activation = a_start[0];
                __m256i activation2 = a_start[1];
                // Sign the values of the y vectors
                const __m256i sy = _mm256_sign_epi8(activation, w_0);
                const __m256i sy2 = _mm256_sign_epi8(activation2, w_128);

                // TODO: Perform int8 dot product with `_mm256_maddubs_epi16`
                // Hint: use `_mm256_maddubs_epi16` to complete the following computation
                // dot = ax * sy
                // dot2 = ax2 * sy2

                // Convert int32 vectors to floating point vectors
                const __m256i ones = _mm256_set1_epi16(1);
                const __m256i summed_pairs = _mm256_madd_epi16(ones, dot);
                const __m256i summed_pairs2 = _mm256_madd_epi16(ones, dot2);
                __m256 intermediate = _mm256_cvtepi32_ps(summed_pairs);
                __m256 intermediate2 = _mm256_cvtepi32_ps(summed_pairs2);

                // Create vectors for scales and apply them to intermediate results
                __m256 v_s = _mm256_set1_ps(s_ptr[0] * sa_ptr[0]);
                __m256 v_s2 = _mm256_set1_ps(s_ptr[1] * sa_ptr[1]);
                acc0 = _mm256_fmadd_ps(intermediate, v_s, acc0);
                acc0 = _mm256_fmadd_ps(intermediate2, v_s2, acc0);
                s_ptr += 2;
                sa_ptr += 2;
                w_start += 1;
                a_start += 2;
            }
            float *ptr = (float *)&acc0;
            C->data_ptr[row * n + col] = ptr[0] + ptr[1] + ptr[2] + ptr[3] + ptr[4] + ptr[5] + ptr[6] + ptr[7];

        }
    }
};
}  // namespace matmul
