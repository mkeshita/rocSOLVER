/************************************************************************
 * Derived from the BSD3-licensed
 * LAPACK routine (version 3.1) --
 *     Univ. of Tennessee, Univ. of California Berkeley,
 *     Univ. of Colorado Denver and NAG Ltd..
 *     December 2016
 * Copyright (c) 2019-2022 Advanced Micro Devices, Inc.
 * ***********************************************************************/

#pragma once

#include "rocblas.hpp"
#include "roclapack_potf2.hpp"
#include "rocsolver.h"

template <typename U>
ROCSOLVER_KERNEL void
    chk_positive(rocblas_int* iinfo, rocblas_int* info, int j, rocblas_int batch_count)
{
    int id = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;

    if(id < batch_count && info[id] == 0 && iinfo[id] > 0)
        info[id] = iinfo[id] + j;
}

template <bool BATCHED, typename T>
void rocsolver_potrf_getMemorySize(const rocblas_int n,
                                   const rocblas_fill uplo,
                                   const rocblas_int batch_count,
                                   size_t* size_scalars,
                                   size_t* size_work1,
                                   size_t* size_work2,
                                   size_t* size_work3,
                                   size_t* size_work4,
                                   size_t* size_pivots,
                                   size_t* size_iinfo,
                                   bool* optim_mem)
{
    // if quick return no need of workspace
    if(n == 0 || batch_count == 0)
    {
        *size_scalars = 0;
        *size_work1 = 0;
        *size_work2 = 0;
        *size_work3 = 0;
        *size_work4 = 0;
        *size_pivots = 0;
        *size_iinfo = 0;
        *optim_mem = true;
        return;
    }

    rocblas_int jb = POTRF_BLOCKSIZE;
    if(n <= POTRF_POTF2_SWITCHSIZE)
    {
        // requirements for calling a single POTF2
        rocsolver_potf2_getMemorySize<T>(n, batch_count, size_scalars, size_work1, size_pivots);
        *size_work2 = 0;
        *size_work3 = 0;
        *size_work4 = 0;
        *size_iinfo = 0;
        *optim_mem = true;
    }
    else
    {
        size_t s1, s2;

        // size to store info about positiveness of each subblock
        *size_iinfo = sizeof(rocblas_int) * batch_count;

        // requirements for calling POTF2 for the subblocks
        rocsolver_potf2_getMemorySize<T>(jb, batch_count, size_scalars, &s1, size_pivots);

        // extra requirements for calling TRSM
        if(uplo == rocblas_fill_upper)
            rocblasCall_trsm_mem<BATCHED, T>(rocblas_side_left,
                                             rocblas_operation_conjugate_transpose, jb, n - jb,
                                             batch_count, &s2, size_work2, size_work3, size_work4);
        else
            rocblasCall_trsm_mem<BATCHED, T>(rocblas_side_right,
                                             rocblas_operation_conjugate_transpose, n - jb, jb,
                                             batch_count, &s2, size_work2, size_work3, size_work4);

        *size_work1 = max(s1, s2);

        // always allocate all required memory for TRSM optimal performance
        *optim_mem = true;
    }
}

template <bool BATCHED, typename T, typename S, typename U, bool COMPLEX = is_complex<T>>
rocblas_status rocsolver_potrf_template(rocblas_handle handle,
                                        const rocblas_fill uplo,
                                        const rocblas_int n,
                                        U A,
                                        const rocblas_int shiftA,
                                        const rocblas_int lda,
                                        const rocblas_stride strideA,
                                        rocblas_int* info,
                                        const rocblas_int batch_count,
                                        T* scalars,
                                        void* work1,
                                        void* work2,
                                        void* work3,
                                        void* work4,
                                        T* pivots,
                                        rocblas_int* iinfo,
                                        bool optim_mem)
{
    ROCSOLVER_ENTER("potrf", "uplo:", uplo, "n:", n, "shiftA:", shiftA, "lda:", lda,
                    "bc:", batch_count);

    // quick return
    if(batch_count == 0)
        return rocblas_status_success;

    hipStream_t stream;
    rocblas_get_stream(handle, &stream);

    rocblas_int blocksReset = (batch_count - 1) / BS1 + 1;
    dim3 gridReset(blocksReset, 1, 1);
    dim3 threads(BS1, 1, 1);

    // info=0 (starting with a positive definite matrix)
    ROCSOLVER_LAUNCH_KERNEL(reset_info, gridReset, threads, 0, stream, info, batch_count, 0);

    // quick return
    if(n == 0)
        return rocblas_status_success;

    // everything must be executed with scalars on the host
    rocblas_pointer_mode old_mode;
    rocblas_get_pointer_mode(handle, &old_mode);
    rocblas_set_pointer_mode(handle, rocblas_pointer_mode_host);

    // if the matrix is small, use the unblocked (BLAS-levelII) variant of the
    // algorithm
    rocblas_int nb = POTRF_BLOCKSIZE;
    if(n <= POTRF_POTF2_SWITCHSIZE)
        return rocsolver_potf2_template<T>(handle, uplo, n, A, shiftA, lda, strideA, info,
                                           batch_count, scalars, (T*)work1, pivots);

    // constants for rocblas functions calls
    T t_one = 1;
    S s_one = 1;
    S s_minone = -1;

    rocblas_int jb, j = 0;

    // (TODO: When the matrix is detected to be non positive definite, we need to
    //  prevent TRSM and HERK to modify further the input matrix; ideally with no
    //  synchronizations.)

    if(uplo == rocblas_fill_upper)
    {
        // Compute the Cholesky factorization A = U'*U.
        while(j < n - POTRF_POTF2_SWITCHSIZE)
        {
            // Factor diagonal and subdiagonal blocks
            jb = min(n - j, nb); // number of columns in the block
            ROCSOLVER_LAUNCH_KERNEL(reset_info, gridReset, threads, 0, stream, iinfo, batch_count, 0);
            rocsolver_potf2_template<T>(handle, uplo, jb, A, shiftA + idx2D(j, j, lda), lda,
                                        strideA, iinfo, batch_count, scalars, (T*)work1, pivots);

            // test for non-positive-definiteness.
            ROCSOLVER_LAUNCH_KERNEL(chk_positive<U>, gridReset, threads, 0, stream, iinfo, info, j,
                                    batch_count);

            if(j + jb < n)
            {
                // update trailing submatrix
                rocblasCall_trsm<BATCHED, T>(
                    handle, rocblas_side_left, uplo, rocblas_operation_conjugate_transpose,
                    rocblas_diagonal_non_unit, jb, (n - j - jb), &t_one, A,
                    shiftA + idx2D(j, j, lda), lda, strideA, A, shiftA + idx2D(j, j + jb, lda), lda,
                    strideA, batch_count, optim_mem, work1, work2, work3, work4);

                rocblasCall_syrk_herk<T>(
                    handle, uplo, rocblas_operation_conjugate_transpose, n - j - jb, jb, &s_minone,
                    A, shiftA + idx2D(j, j + jb, lda), lda, strideA, &s_one, A,
                    shiftA + idx2D(j + jb, j + jb, lda), lda, strideA, batch_count);
            }
            j += nb;
        }
    }
    else
    {
        // Compute the Cholesky factorization A = L*L'.
        while(j < n - POTRF_POTF2_SWITCHSIZE)
        {
            // Factor diagonal and subdiagonal blocks
            jb = min(n - j, nb); // number of columns in the block
            ROCSOLVER_LAUNCH_KERNEL(reset_info, gridReset, threads, 0, stream, iinfo, batch_count, 0);
            rocsolver_potf2_template<T>(handle, uplo, jb, A, shiftA + idx2D(j, j, lda), lda,
                                        strideA, iinfo, batch_count, scalars, (T*)work1, pivots);

            // test for non-positive-definiteness.
            ROCSOLVER_LAUNCH_KERNEL(chk_positive<U>, gridReset, threads, 0, stream, iinfo, info, j,
                                    batch_count);

            if(j + jb < n)
            {
                // update trailing submatrix
                rocblasCall_trsm<BATCHED, T>(
                    handle, rocblas_side_right, uplo, rocblas_operation_conjugate_transpose,
                    rocblas_diagonal_non_unit, (n - j - jb), jb, &t_one, A,
                    shiftA + idx2D(j, j, lda), lda, strideA, A, shiftA + idx2D(j + jb, j, lda), lda,
                    strideA, batch_count, optim_mem, work1, work2, work3, work4);

                rocblasCall_syrk_herk<T>(handle, uplo, rocblas_operation_none, n - j - jb, jb,
                                         &s_minone, A, shiftA + idx2D(j + jb, j, lda), lda, strideA,
                                         &s_one, A, shiftA + idx2D(j + jb, j + jb, lda), lda,
                                         strideA, batch_count);
            }
            j += nb;
        }
    }

    // factor last block
    if(j < n)
    {
        rocsolver_potf2_template<T>(handle, uplo, n - j, A, shiftA + idx2D(j, j, lda), lda, strideA,
                                    iinfo, batch_count, scalars, (T*)work1, pivots);
        ROCSOLVER_LAUNCH_KERNEL(chk_positive<U>, gridReset, threads, 0, stream, iinfo, info, j,
                                batch_count);
    }

    rocblas_set_pointer_mode(handle, old_mode);
    return rocblas_status_success;
}
