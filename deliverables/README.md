# macOS port: deliverables index (2026-11-02)

Nothing on this machine was changed outside ``. No disk, EFI, boot entry, BIOS, security setting or driver was touched, and no project script or binary was executed.

| Deliverable | File |
|---|---|
| Compatibility matrix | `COMPATIBILITY-MATRIX.md` |
| Progress log (done vs blockers) | `PROGRESS-LOG.md` |
| Pinned source/dependency inventory | `a4-security/provenance-and-reproducibility.md`, `a3-io/kext-inventory.md`, `a2-platform/platform-analysis.md` |
| Reproducible build instructions (needs a macOS host) | `a4-security/build-instructions.md` |
| Security review | `a4-security/security-review.md` |
| NVIDIA architecture and 2B85 trace | `a1-nvidia/architecture.md`, `a1-nvidia/2b85-trace.md` |
| Patch, rationale, validation, review | `../patches/0001-*.patch`, `../patches/0002-*.patch`, `a1-nvidia/patch-rationale.md`, `review-a1-patch.md` (branch `port/a1-5090` @ `9c76447` in `../work/nvidia-a1`) |
| Tests needing macOS or this physical GPU | `a1-nvidia/macos-gpu-tests.md`, `a3-io/io-acceptance-tests.md` |
| OpenCore config draft (**NOT BOOT READY**, 23 UNRESOLVED) | `a2-platform/config.draft.plist`, `a2-platform/config-draft-notes.md`, `a2-platform/ocvalidate-output.md` |
| Separate-drive install, recovery and rollback | `a2-platform/install-recovery-rollback.md` |
| I/O analysis and USB map skeleton | `a3-io/io-matrix.md`, `a3-io/usb-topology.md` |
| Hardware evidence | `../evidence/hardware-verified.md` (dock correction in `PROGRESS-LOG.md`) |

Pinned upstreams: nvidia-macos-driver `470e391e8cf8d7e499cdabfa7f2daadb6e14bcfd` (v2.6.6), 1401 `f5a9254079601d69a2e165fcd9f659e4f882bab9`, OpenCorePkg `8d8d690cb543f63114340cdae958de4ce7a8296f` (1.0.8), open-gpu-kernel-modules tag 615.78.08. Kext commits are listed in `a3-io/kext-inventory.md`.

Upstreaming note: the driver repo says AI-written PRs are closed. Any upstream submission of the patch would have to be your own, after you have reviewed it line by line.
