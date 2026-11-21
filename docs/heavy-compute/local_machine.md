# Local machine: the limits behind every "can't run locally" in the inventory

Measured 2026-11-13 (`lscpu`, `free`, `swapon`, `df`, `lspci`, Python imports). Re-measure
with `python scripts/heavy/check_machine.py`.

| Resource | Value |
|---|---|
| CPU | Intel Core Ultra 5 226V, 8 cores / 8 threads (1 thread per core), 1 socket |
| RAM | 15.2 GB total, **~3.5 GB available** in normal use (desktop + IDE running) |
| Swap | 30.3 GB (15.2 GB zram, 13.4 GB already in use, + 15.2 GB swapfile) |
| GPU | Intel Arc 130V/140V iGPU. **No NVIDIA GPU, `nvidia-smi` absent** |
| CUDA / VRAM | none / 0 GB (`nvcc` absent) |
| Disk (repo volume `/home`) | 110 GB, **9.7 GB free** |
| Disk `/tmp` | tmpfs 7.6 GB (RAM-backed), 7.4 GB free |
| OS | Omarchy (Arch-based), Linux 7.2.8 |
| Python | 3.12.14 (uv venv `reconstruction/.venv`) |
| COLMAP CLI / GLOMAP | not installed (pycolmap 4.2.1 wheel only, **CPU, `has_cuda=False`**) |
| OpenMVS | v2.4.0 in Docker image `room-openmvs:2.4.0` (CPU build, 594 MB); not installed natively |
| Open3D | 0.19.0 PyPI wheel, **built with CUDA 12.1** (`_build_config['BUILD_CUDA_MODULE']=True`); no device here, so `o3d.core.cuda.is_available() == False` |
| PyTorch | 2.14.1+cpu (`torch.cuda.is_available() == False`) |
| gsplat | 1.5.3 installed; CUDA kernels **cannot run** |
| nerfstudio | not installed (1.1.5 pins gsplat 1.4.0 + opencv-headless 4.10, which conflict with our pins) |
| Docker | 29.9.2, no NVIDIA container runtime |

Practical local limits used in the inventory: no GPU work at all; jobs must stay under
~3 GB RSS to avoid swapping; at most ~8 GB of new downloads + intermediates.
