#include "hip/hip_runtime.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <sstream>

#include <omp.h>
#include <hipcub/hipcub.hpp>
#define HAVE_CUB 1
#ifdef HAVE_CUB
#include <hipcub/rocprim/block/block_reduce.hpp>
#endif  // HAVE_CUB

#ifdef USE_NVTX
#include <nvToolsExt.h>

const uint32_t colors[] = {0x0000ff00, 0x000000ff, 0x00ffff00, 0x00ff00ff,
                           0x0000ffff, 0x00ff0000, 0x00ffffff};
const int num_colors = sizeof(colors) / sizeof(uint32_t);

#define PUSH_RANGE(name, cid)                              \
    {                                                      \
        int color_id = cid;                                \
        color_id = color_id % num_colors;                  \
        nvtxEventAttributes_t eventAttrib = {0};           \
        eventAttrib.version = NVTX_VERSION;                \
        eventAttrib.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;  \
        eventAttrib.colorType = NVTX_COLOR_ARGB;           \
        eventAttrib.color = colors[color_id];              \
        eventAttrib.messageType = NVTX_MESSAGE_TYPE_ASCII; \
        eventAttrib.message.ascii = name;                  \
        nvtxRangePushEx(&eventAttrib);                     \
    }
#define POP_RANGE nvtxRangePop();
#else
#define PUSH_RANGE(name, cid)
#define POP_RANGE
#endif

#define CUDA_RT_CALL(call)                                                                  \
    {                                                                                       \
        hipError_t cudaStatus = call;                                                      \
        if (hipSuccess != cudaStatus)                                                      \
            fprintf(stderr,                                                                 \
                    "ERROR: CUDA RT call \"%s\" in line %d of file %s failed "              \
                    "with "                                                                 \
                    "%s (%d).\n",                                                           \
                    #call, __LINE__, __FILE__, hipGetErrorString(cudaStatus), cudaStatus); \
    }

typedef float real;
constexpr real tol = 1.0e-8;

const real PI = 2.0 * std::asin(1.0);

__global__ void initialize_boundaries(real* __restrict__ const a_new, real* __restrict__ const a,
                                      const real pi, const int nx, const int ny) {
    for (int iy = blockIdx.x * blockDim.x + threadIdx.x; iy < ny; iy += blockDim.x * gridDim.x) {
        const real y0 = sin(2.0 * pi * iy / (ny - 1));
        a[iy * nx + 0] = y0;
        a[iy * nx + (nx - 1)] = y0;
        a_new[iy * nx + 0] = y0;
        a_new[iy * nx + (nx - 1)] = y0;
    }
}

template <int BLOCK_DIM_X, int BLOCK_DIM_Y>
__global__ void jacobi_kernel(real* __restrict__ const a_new, const real* __restrict__ const a,
                              real* __restrict__ const l2_norm, const int iy_start,
                              const int iy_end, const int nx) {
#ifdef HAVE_CUB
    typedef hipcub::BlockReduce<real, BLOCK_DIM_X, hipcub::BLOCK_REDUCE_WARP_REDUCTIONS, BLOCK_DIM_Y>
        BlockReduce;
    __shared__ typename BlockReduce::TempStorage temp_storage;
#endif  // HAVE_CUB
    const int iy = blockIdx.y * blockDim.y + threadIdx.y + 1;
    const int ix = blockIdx.x * blockDim.x + threadIdx.x;
    real local_l2_norm = 0.0;

    if (iy < iy_end) {
        if (ix >= 1 && ix < (nx - 1)) {
            const real new_val = 0.25 * (a[iy * nx + ix + 1] + a[iy * nx + ix - 1] +
                                         a[(iy + 1) * nx + ix] + a[(iy - 1) * nx + ix]);
            a_new[iy * nx + ix] = new_val;

            // apply boundary conditions
            if (iy_start == iy) {
                a_new[iy_end * nx + ix] = new_val;
            }

            if ((iy_end - 1) == iy) {
                a_new[(iy_start - 1) * nx + ix] = new_val;
            }

            real residue = new_val - a[iy * nx + ix];
            local_l2_norm = residue * residue;
        }
    }
#ifdef HAVE_CUB
    real block_l2_norm = BlockReduce(temp_storage).Sum(local_l2_norm);
    if (0 == threadIdx.y && 0 == threadIdx.x) atomicAdd(l2_norm, block_l2_norm);
#else
    atomicAdd(l2_norm, local_l2_norm);
#endif  // HAVE_CUB
}

double noopt(const int nx, const int ny, const int iter_max, real* const a_ref_h, const int nccheck,
             const bool print);

template <typename T>
T get_argval(char** begin, char** end, const std::string& arg, const T default_val) {
    T argval = default_val;
    char** itr = std::find(begin, end, arg);
    if (itr != end && ++itr != end) {
        std::istringstream inbuf(*itr);
        inbuf >> argval;
    }
    return argval;
}

bool get_arg(char** begin, char** end, const std::string& arg) {
    char** itr = std::find(begin, end, arg);
    if (itr != end) {
        return true;
    }
    return false;
}

struct l2_norm_buf {
    hipEvent_t copy_done;
    real* d;
    real* h;
};

int main(int argc, char* argv[]) {
    const int iter_max = get_argval<int>(argv, argv + argc, "-niter", 1000);
    const int nccheck = get_argval<int>(argv, argv + argc, "-nccheck", 1);
    const int nx = get_argval<int>(argv, argv + argc, "-nx", 7168);
    const int ny = get_argval<int>(argv, argv + argc, "-ny", 7168);
    const bool csv = get_arg(argv, argv + argc, "-csv");

    if (nccheck != 1) {
        fprintf(stderr, "Only nccheck = 1 is supported\n");
        return -1;
    }

    real* a;
    real* a_new;

    hipStream_t compute_stream;
    hipStream_t copy_l2_norm_stream;
    hipStream_t reset_l2_norm_stream;

    hipEvent_t compute_done;
    hipEvent_t reset_l2_norm_done[2];

    real l2_norms[2];
    l2_norm_buf l2_norm_bufs[2];

    int iy_start = 1;
    int iy_end = (ny - 1);

    CUDA_RT_CALL(hipSetDevice(0));
    CUDA_RT_CALL(hipFree(0));

    CUDA_RT_CALL(hipMalloc(&a, nx * ny * sizeof(real)));
    CUDA_RT_CALL(hipMalloc(&a_new, nx * ny * sizeof(real)));

    CUDA_RT_CALL(hipMemset(a, 0, nx * ny * sizeof(real)));
    CUDA_RT_CALL(hipMemset(a_new, 0, nx * ny * sizeof(real)));

    // Set diriclet boundary conditions on left and right boarder
    hipLaunchKernelGGL((initialize_boundaries), dim3(ny / 128 + 1), dim3(128), 0, 0, a, a_new, PI, nx, ny);
    CUDA_RT_CALL(hipGetLastError());
    CUDA_RT_CALL(hipDeviceSynchronize());

    CUDA_RT_CALL(hipStreamCreate(&compute_stream));
    CUDA_RT_CALL(hipStreamCreate(&copy_l2_norm_stream));
    CUDA_RT_CALL(hipStreamCreate(&reset_l2_norm_stream));
    CUDA_RT_CALL(hipEventCreateWithFlags(&compute_done, hipEventDisableTiming));
    CUDA_RT_CALL(hipEventCreateWithFlags(&reset_l2_norm_done[0], hipEventDisableTiming));
    CUDA_RT_CALL(hipEventCreateWithFlags(&reset_l2_norm_done[1], hipEventDisableTiming));

    for (int i = 0; i < 2; ++i) {
        CUDA_RT_CALL(hipEventCreateWithFlags(&l2_norm_bufs[i].copy_done, hipEventDisableTiming));
        CUDA_RT_CALL(hipMalloc(&l2_norm_bufs[i].d, sizeof(real)));
        CUDA_RT_CALL(hipMemset(l2_norm_bufs[i].d, 0, sizeof(real)));
        CUDA_RT_CALL(hipHostMalloc(&l2_norm_bufs[i].h, sizeof(real)));
        (*l2_norm_bufs[i].h) = 1.0;
    }

    CUDA_RT_CALL(hipDeviceSynchronize());

    if (!csv)
        printf(
            "Jacobi relaxation: %d iterations on %d x %d mesh with norm check "
            "every %d iterations\n",
            iter_max, ny, nx, nccheck);

    constexpr int dim_block_x = 32;
    constexpr int dim_block_y = 4;
    dim3 dim_grid((nx + dim_block_x - 1) / dim_block_x, (ny + dim_block_y - 1) / dim_block_y, 1);

    int iter = 0;
    for (int i = 0; i < 2; ++i) {
        l2_norms[i] = 0.0;
    }

    double start = omp_get_wtime();

    PUSH_RANGE("Jacobi solve", 0)

    bool l2_norm_greater_than_tol = true;
    while (l2_norm_greater_than_tol && iter < iter_max) {
        // on new iteration: old current vars are now previous vars, old
        // previous vars are no longer needed
        int prev = iter % 2;
        int curr = (iter + 1) % 2;

        // wait for memset from old previous iteration to complete
        CUDA_RT_CALL(hipStreamWaitEvent(compute_stream, reset_l2_norm_done[curr], 0));

        hipLaunchKernelGGL(HIP_KERNEL_NAME(jacobi_kernel<dim_block_x, dim_block_y>), dim3(dim_grid),dim3({dim_block_x, dim_block_y,1}), 0, compute_stream, a_new, a, l2_norm_bufs[curr].d, iy_start, iy_end, nx);
        CUDA_RT_CALL(hipGetLastError());
        CUDA_RT_CALL(hipEventRecord(compute_done, compute_stream));

        // perform L2 norm calculation
        if ((iter % nccheck) == 0 || (!csv && (iter % 100) == 0)) {
            CUDA_RT_CALL(hipStreamWaitEvent(copy_l2_norm_stream, compute_done, 0));
            CUDA_RT_CALL(hipMemcpyAsync(l2_norm_bufs[curr].h, l2_norm_bufs[curr].d, sizeof(real),
                                         hipMemcpyDeviceToHost, copy_l2_norm_stream));
            CUDA_RT_CALL(hipEventRecord(l2_norm_bufs[curr].copy_done, copy_l2_norm_stream));

            // make sure D2H copy is complete before using the data for
            // calculation
            CUDA_RT_CALL(hipEventSynchronize(l2_norm_bufs[prev].copy_done));

            l2_norms[prev] = *(l2_norm_bufs[prev].h);
            l2_norms[prev] = std::sqrt(l2_norms[prev]);
            l2_norm_greater_than_tol = (l2_norms[prev] > tol);

            if (!csv && (iter % 100) == 0) {
                printf("%5d, %0.6f\n", iter, l2_norms[prev]);
            }

            // reset everything for next iteration
            l2_norms[prev] = 0.0;
            *(l2_norm_bufs[prev].h) = 0.0;
            CUDA_RT_CALL(
                hipMemsetAsync(l2_norm_bufs[prev].d, 0, sizeof(real), reset_l2_norm_stream));
            CUDA_RT_CALL(hipEventRecord(reset_l2_norm_done[prev], reset_l2_norm_stream));
        }

        std::swap(a_new, a);
        iter++;
    }
    CUDA_RT_CALL(hipDeviceSynchronize());
    POP_RANGE
    double stop = omp_get_wtime();

    if (csv) {
        printf("single_gpu, %d, %d, %d, %d, %f\n", nx, ny, iter_max, nccheck, (stop - start));
    } else {
        printf("%dx%d: 1 GPU: %8.4f s\n", ny, nx, (stop - start));
    }

    for (int i = 0; i < 2; ++i) {
        CUDA_RT_CALL(hipHostFree(l2_norm_bufs[i].h));
        CUDA_RT_CALL(hipFree(l2_norm_bufs[i].d));
        CUDA_RT_CALL(hipEventDestroy(l2_norm_bufs[i].copy_done));
    }

    CUDA_RT_CALL(hipEventDestroy(reset_l2_norm_done[1]));
    CUDA_RT_CALL(hipEventDestroy(reset_l2_norm_done[0]));
    CUDA_RT_CALL(hipEventDestroy(compute_done));

    CUDA_RT_CALL(hipStreamDestroy(reset_l2_norm_stream));
    CUDA_RT_CALL(hipStreamDestroy(copy_l2_norm_stream));
    CUDA_RT_CALL(hipStreamDestroy(compute_stream));

    CUDA_RT_CALL(hipFree(a_new));
    CUDA_RT_CALL(hipFree(a));

    return 0;
}
