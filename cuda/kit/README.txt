CUDA on macOS - test kit (nvrmcu 0.1, built 2026-11-05)

Run on the Hackintosh, in Terminal, with the desktop up (the NVIDIA driver must be working):

    sh /Volumes/OPENCORE/cuda/run

It runs two tests and saves the output as results-<date>.txt in this folder.

  1. nvrmcu-selftest  - the new driver layer on its own: opens the RTX 5090 through NullMoth's kernel driver,
                        allocates video memory (up to 8 GB), copies data up and back, memset, GPU-to-GPU copies.
  2. cuda-selftest    - the standard CUDA runtime and cuBLAS APIs on top: cudaMalloc/cudaMemcpy/streams,
                        cublasSgemm and fp16 tensor-core GEMM checked against the CPU, and a TFLOPS figure.

Nothing is installed and nothing on the system changes. The tests are ordinary programs: they use the GPU the way a
Metal app does, alongside the desktop, and release it when they exit. If a test hangs, close Terminal; the GPU
work it queued is abandoned with it. If the screen ever freezes, hold the power button - nothing here writes to disk
except the results file.

Then boot Windows and tell Claude "CUDA results are on the stick".

Files:
  nvrmcu-selftest   sha256 6be26460f75350fc112f71f188b8c1c05a1f97eb347853fbec69f1b8ded3089c
  cuda-selftest     sha256 f225056afe14f540505284a26484ef543c216eaeea1ead013aba49b3cd6867c0
  Source: github.com/aydinmrnv/aat2250-macos-port, folder cuda/.
