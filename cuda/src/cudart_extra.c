// cudart_extra.c - CUDA runtime entry points ordinary programs use that macuda's runtime layer leaves out (ggml never
// calls them). Each is the documented behaviour expressed through a call the layer does implement.
#include <cuda_runtime_api.h>

cudaError_t cudaStreamCreate(cudaStream_t *s) { return cudaStreamCreateWithFlags(s, cudaStreamDefault); }
cudaError_t cudaEventCreate(cudaEvent_t *e) { return cudaEventCreateWithFlags(e, cudaEventDefault); }
// The CUDA 13.0 API is what the headers and the device code are built against.
cudaError_t cudaRuntimeGetVersion(int *v) { if (!v) return cudaErrorInvalidValue; *v = 13000; return cudaSuccess; }
cudaError_t cudaDriverGetVersion(int *v) { if (!v) return cudaErrorInvalidValue; *v = 13000; return cudaSuccess; }
cudaError_t cudaDeviceReset(void) { return cudaDeviceSynchronize(); }
cudaError_t cudaThreadSynchronize(void) { return cudaDeviceSynchronize(); }
