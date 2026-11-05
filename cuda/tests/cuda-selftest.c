// cuda-selftest: the standard CUDA runtime and cuBLAS APIs, as any CUDA program calls them, on the RTX 5090 under
// macOS. Stack: this program -> macuda's libtinycudart + libtinycublas -> nvrmcu -> NullMoth's NVRM kext -> GPU.
// Plain C: the kernels are macuda's precompiled sm_120 ones (copy, GEMM), so no CUDA compiler is needed to build it.
#include <cuda_runtime_api.h>
#include <cublas_v2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int pass, fail;
static void check(int ok, const char *what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); ok ? pass++ : fail++; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static uint16_t f2h(float f) {   // round-to-nearest-even float -> IEEE half
  uint32_t x; memcpy(&x, &f, 4);
  uint32_t sign = (x >> 16) & 0x8000, mant = x & 0x7fffff; int exp = (int)((x >> 23) & 0xff) - 127 + 15;
  if (exp <= 0) return (uint16_t)sign;
  if (exp >= 31) return (uint16_t)(sign | 0x7c00);
  uint32_t h = sign | ((uint32_t)exp << 10) | (mant >> 13), rem = mant & 0x1fff;
  if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) h++;
  return (uint16_t)h;
}
static float h2f(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ff, x;
  if (!exp) x = sign; else x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
  float f; memcpy(&f, &x, 4); return f;
}

// C = A * B, column-major, as cuBLAS defines it
static void ref_gemm(int m, int n, int k, const float *A, const float *B, float *C) {
  for (int j = 0; j < n; j++)
    for (int i = 0; i < m; i++) {
      double s = 0;
      for (int l = 0; l < k; l++) s += (double)A[i + (size_t)l * m] * B[l + (size_t)j * k];
      C[i + (size_t)j * m] = (float)s;
    }
}

static double max_rel_err(const float *got, const float *want, size_t n) {
  double worst = 0;
  for (size_t i = 0; i < n; i++) {
    double e = fabs((double)got[i] - want[i]) / (fabs((double)want[i]) + 1.0);
    if (e > worst || isnan(e)) worst = isnan(e) ? 1e9 : e;
  }
  return worst;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("CUDA self-test on macOS\n");
  int count = 0, rv = 0, dv = 0;
  cudaError_t e = cudaGetDeviceCount(&count);
  check(e == cudaSuccess && count == 1, "cudaGetDeviceCount finds the GPU");
  if (e != cudaSuccess || !count) { printf("\nRESULT: %d passed, %d failed\n", pass, fail); return 1; }
  cudaRuntimeGetVersion(&rv); cudaDriverGetVersion(&dv);
  struct cudaDeviceProp p;
  check(cudaGetDeviceProperties(&p, 0) == cudaSuccess, "cudaGetDeviceProperties");
  printf("      %s, compute %d.%d, %d SMs, %.1f GB, runtime %d driver %d\n", p.name, p.major, p.minor,
         p.multiProcessorCount, p.totalGlobalMem / 1e9, rv, dv);
  size_t fr = 0, tot = 0;
  if (cudaMemGetInfo(&fr, &tot) == cudaSuccess) printf("      cudaMemGetInfo: %.1f GB free of %.1f GB\n", fr / 1e9, tot / 1e9);

  // memory and copies through the runtime
  const size_t nb = 32u << 20;
  unsigned char *h = malloc(nb), *r = malloc(nb);
  for (size_t i = 0; i < nb; i++) h[i] = (unsigned char)(i * 131 + 7);
  void *d1 = NULL, *d2 = NULL;
  check(cudaMalloc(&d1, nb) == cudaSuccess && cudaMalloc(&d2, nb) == cudaSuccess, "cudaMalloc x2");
  check(cudaMemcpy(d1, h, nb, cudaMemcpyHostToDevice) == cudaSuccess, "cudaMemcpy host to device");
  check(cudaMemcpy(d2, d1, nb, cudaMemcpyDeviceToDevice) == cudaSuccess, "cudaMemcpy device to device");
  memset(r, 0, nb);
  check(cudaMemcpy(r, d2, nb, cudaMemcpyDeviceToHost) == cudaSuccess && !memcmp(h, r, nb), "cudaMemcpy back, byte for byte");
  check(cudaMemset(d2, 0x33, nb) == cudaSuccess && cudaMemcpy(r, d2, nb, cudaMemcpyDeviceToHost) == cudaSuccess && r[0] == 0x33 &&
        r[nb - 1] == 0x33, "cudaMemset");
  cudaStream_t s;
  check(cudaStreamCreate(&s) == cudaSuccess, "cudaStreamCreate");
  check(cudaMemcpyAsync(d1, h, nb, cudaMemcpyHostToDevice, s) == cudaSuccess && cudaStreamSynchronize(s) == cudaSuccess,
        "cudaMemcpyAsync + cudaStreamSynchronize");
  cudaFree(d1); cudaFree(d2);

  // cuBLAS SGEMM (fp32 SIMT kernel) against a CPU reference
  cublasHandle_t hb;
  check(cublasCreate(&hb) == CUBLAS_STATUS_SUCCESS, "cublasCreate");
  const int m = 512, n = 384, k = 256;
  float *A = malloc(sizeof(float) * m * k), *B = malloc(sizeof(float) * k * n), *C = malloc(sizeof(float) * m * n),
        *R = malloc(sizeof(float) * m * n);
  srand(1);
  for (int i = 0; i < m * k; i++) A[i] = (float)(rand() % 2001 - 1000) / 1000.0f;
  for (int i = 0; i < k * n; i++) B[i] = (float)(rand() % 2001 - 1000) / 1000.0f;
  ref_gemm(m, n, k, A, B, R);
  float *dA, *dB, *dC;
  cudaMalloc((void **)&dA, sizeof(float) * m * k); cudaMalloc((void **)&dB, sizeof(float) * k * n); cudaMalloc((void **)&dC, sizeof(float) * m * n);
  cudaMemcpy(dA, A, sizeof(float) * m * k, cudaMemcpyHostToDevice);
  cudaMemcpy(dB, B, sizeof(float) * k * n, cudaMemcpyHostToDevice);
  const float one = 1.0f, zero = 0.0f;
  cublasSetMathMode(hb, CUBLAS_DEFAULT_MATH);
  cublasStatus_t bs = cublasSgemm(hb, CUBLAS_OP_N, CUBLAS_OP_N, m, n, k, &one, dA, m, dB, k, &zero, dC, m);
  cudaMemcpy(C, dC, sizeof(float) * m * n, cudaMemcpyDeviceToHost);
  double err = max_rel_err(C, R, (size_t)m * n);
  check(bs == CUBLAS_STATUS_SUCCESS && err < 1e-4, "cublasSgemm 512x384x256 matches the CPU");
  printf("      worst relative error %.2e\n", err);

  // cuBLAS GemmEx: fp16 inputs on the tensor cores, fp32 output
  uint16_t *Ah = malloc(2 * (size_t)m * k), *Bh = malloc(2 * (size_t)k * n);
  float *Ar = malloc(sizeof(float) * m * k), *Br = malloc(sizeof(float) * k * n);
  for (int i = 0; i < m * k; i++) { Ah[i] = f2h(A[i]); Ar[i] = h2f(Ah[i]); }
  for (int i = 0; i < k * n; i++) { Bh[i] = f2h(B[i]); Br[i] = h2f(Bh[i]); }
  ref_gemm(m, n, k, Ar, Br, R);
  void *dAh, *dBh;
  cudaMalloc(&dAh, 2 * (size_t)m * k); cudaMalloc(&dBh, 2 * (size_t)k * n);
  cudaMemcpy(dAh, Ah, 2 * (size_t)m * k, cudaMemcpyHostToDevice);
  cudaMemcpy(dBh, Bh, 2 * (size_t)k * n, cudaMemcpyHostToDevice);
  cudaMemset(dC, 0, sizeof(float) * m * n);
  bs = cublasGemmEx(hb, CUBLAS_OP_N, CUBLAS_OP_N, m, n, k, &one, dAh, CUDA_R_16F, m, dBh, CUDA_R_16F, k, &zero, dC, CUDA_R_32F, m,
                    CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT);
  cudaMemcpy(C, dC, sizeof(float) * m * n, cudaMemcpyDeviceToHost);
  err = max_rel_err(C, R, (size_t)m * n);
  check(bs == CUBLAS_STATUS_SUCCESS && err < 2e-3, "cublasGemmEx fp16 on the tensor cores matches the CPU");
  printf("      worst relative error %.2e (status %d)\n", err, (int)bs);

  // throughput: a big fp16 GEMM, timed
  const int N = 4096;
  void *x, *y, *z;
  if (cudaMalloc(&x, 2ull * N * N) == cudaSuccess && cudaMalloc(&y, 2ull * N * N) == cudaSuccess && cudaMalloc(&z, 4ull * N * N) == cudaSuccess) {
    cudaMemset(x, 0, 2ull * N * N); cudaMemset(y, 0, 2ull * N * N);
    cublasGemmEx(hb, CUBLAS_OP_N, CUBLAS_OP_N, N, N, N, &one, x, CUDA_R_16F, N, y, CUDA_R_16F, N, &zero, z, CUDA_R_32F, N,
                 CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT);
    cudaDeviceSynchronize();
    double t0 = now();
    int reps = 10;
    for (int i = 0; i < reps; i++)
      cublasGemmEx(hb, CUBLAS_OP_N, CUBLAS_OP_N, N, N, N, &one, x, CUDA_R_16F, N, y, CUDA_R_16F, N, &zero, z, CUDA_R_32F, N,
                   CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT);
    cudaError_t se = cudaDeviceSynchronize();
    double dt = now() - t0;
    check(se == cudaSuccess, "10 x 4096^3 fp16 GEMM");
    printf("      %.1f TFLOPS (macuda's tensor-core kernel, nvrmcu 0.1 launch path)\n", 2.0 * N * N * (double)N * reps / dt / 1e12);
  }
  cublasDestroy(hb);
  printf("\nRESULT: %d passed, %d failed\n", pass, fail);
  return fail ? 1 : 0;
}
