// vecadd: the smallest real CUDA program - cudaMalloc, cudaMemcpy, a <<<>>> launch, a check on the CPU.
// Built with the split toolchain: nvcc for the device code (fatbin), Apple clang for the host code, linked against
// macuda's CUDA runtime layer on nvrmcu. If this prints PASS, real CUDA source runs on the RTX 5090 under macOS.
#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

__global__ void vecadd(const float *a, const float *b, float *c, int n) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) c[i] = a[i] * 2.0f + b[i];
}

__global__ void saxpy_shared(float *y, const float *x, float alpha, int n) {
  __shared__ float tile[256];
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  tile[threadIdx.x] = i < n ? x[i] : 0.0f;
  __syncthreads();
  if (i < n) y[i] = alpha * tile[threadIdx.x] + y[i];
}

int main(void) {
  int count = 0;
  cudaError_t e = cudaGetDeviceCount(&count);
  if (e != cudaSuccess || count == 0) { printf("FAIL  no CUDA device (%s)\n", cudaGetErrorString(e)); return 1; }
  cudaDeviceProp p;
  cudaGetDeviceProperties(&p, 0);
  printf("device: %s, cc %d.%d, %d SMs, %.1f GB\n", p.name, p.major, p.minor, p.multiProcessorCount, p.totalGlobalMem / 1e9);

  const int n = 1 << 24;
  size_t bytes = (size_t)n * sizeof(float);
  float *a = (float *)malloc(bytes), *b = (float *)malloc(bytes), *c = (float *)malloc(bytes);
  for (int i = 0; i < n; i++) { a[i] = (float)i * 0.5f; b[i] = (float)(n - i); }
  float *da, *db, *dc;
  if (cudaMalloc(&da, bytes) || cudaMalloc(&db, bytes) || cudaMalloc(&dc, bytes)) { printf("FAIL  cudaMalloc\n"); return 1; }
  cudaMemcpy(da, a, bytes, cudaMemcpyHostToDevice);
  cudaMemcpy(db, b, bytes, cudaMemcpyHostToDevice);
  vecadd<<<(n + 255) / 256, 256>>>(da, db, dc, n);
  e = cudaGetLastError();
  if (e != cudaSuccess) { printf("FAIL  launch: %s\n", cudaGetErrorString(e)); return 1; }
  cudaMemcpy(c, dc, bytes, cudaMemcpyDeviceToHost);
  int bad = 0;
  for (int i = 0; i < n; i++) if (c[i] != a[i] * 2.0f + b[i]) { if (bad++ < 5) printf("      c[%d] = %f, want %f\n", i, c[i], a[i] * 2.0f + b[i]); }
  printf("%s  vecadd over %d elements%s\n", bad ? "FAIL" : "PASS", n, bad ? "" : " (every element checked)");

  saxpy_shared<<<(n + 255) / 256, 256>>>(dc, da, 3.0f, n);
  cudaMemcpy(c, dc, bytes, cudaMemcpyDeviceToHost);
  int bad2 = 0;
  for (int i = 0; i < n; i++) { float want = 3.0f * a[i] + (a[i] * 2.0f + b[i]); if (fabsf(c[i] - want) > 1e-3f * fabsf(want) + 1e-3f) bad2++; }
  printf("%s  shared-memory kernel with __syncthreads%s\n", bad2 ? "FAIL" : "PASS", bad2 ? "" : "");

  cudaEvent_t t0, t1;
  int launches = 2000;
  cudaDeviceSynchronize();
  struct timespec s, f;
  clock_gettime(CLOCK_MONOTONIC, &s);
  for (int i = 0; i < launches; i++) vecadd<<<64, 256>>>(da, db, dc, 64 * 256);
  cudaDeviceSynchronize();
  clock_gettime(CLOCK_MONOTONIC, &f);
  double us = ((f.tv_sec - s.tv_sec) * 1e9 + (f.tv_nsec - s.tv_nsec)) / 1e3 / launches;
  printf("      %d small launches, %.1f us each\n", launches, us);
  (void)t0; (void)t1;
  cudaFree(da); cudaFree(db); cudaFree(dc);
  printf("RESULT: %s\n", bad || bad2 ? "FAIL" : "PASS");
  return bad || bad2;
}
