# roomscan-recon CUDA image for the big machine (inventory item P0-01).
# UNVERIFIED: written on a machine without an NVIDIA GPU; build + smoke-test on the target.
#
# Build (from reconstruction/):
#   docker build -f docker/cuda.Dockerfile -t roomscan-cuda:dev --build-arg CUDA_ARCHS="86;89;90" .
# Run:
#   docker run --rm --gpus all -v "$PWD:/work" -w /work roomscan-cuda:dev .venv/bin/python -m pytest -m gpu
#
# Contents vs inventory needs:
#   COLMAP 4.2.1 + CUDA (SIFT GPU, PatchMatch stereo, incremental AND global mapper; GLOMAP is
#     archived and merged into COLMAP as the "global" mapper)                 -> P2-01..P2-05, P7-01
#   pycolmap-cuda12 4.2.1 (same API as our pinned pycolmap, CUDA build)          -> P2-*, P6-01
#   PyTorch 2.14.1 cu126 + gsplat 1.5.3 (JIT-compiled with this image's nvcc)    -> P0-02, P8-*
#   Open3D 0.19.0 PyPI wheel (BUILD_CUDA_MODULE=True, CUDA 12.1)                 -> P3-08, P3-12
#   transformers + Depth-Anything-V2 (weights fetched at runtime into HF cache)  -> P3-01..P3-05
#   nerfstudio 1.1.5 (optional, separate venv: pins gsplat 1.4.0)                -> P8-01 baseline
# NOT included (by design / not implemented): OpenMVS -> use docker/openmvs (needs OpenCV >= 4.10,
#   i.e. Ubuntu 26.04; this CUDA base is 24.04); ETH3D eval -> docker/eth3d-eval;
#   hloc / SuperPoint / LightGlue (P2-03), UniDepth / Depth Pro (P3-03): not integrated yet.
ARG CUDA_BASE=nvidia/cuda:12.6.3-cudnn-devel-ubuntu24.04
FROM ${CUDA_BASE}

ARG COLMAP_TAG=4.2.1
ARG CUDA_ARCHS="86;89;90"
ARG JOBS=8
ARG WITH_NERFSTUDIO=0
ENV DEBIAN_FRONTEND=noninteractive TORCH_CUDA_ARCH_LIST="8.6;8.9;9.0"

# COLMAP build dependencies: colmap/doc/install.rst @4.2.1 (GUI and MKL omitted: headless).
RUN apt-get update && apt-get install -y --no-install-recommends \
      git cmake ninja-build build-essential ca-certificates curl \
      libboost-program-options-dev libboost-graph-dev libboost-system-dev \
      libeigen3-dev libopenimageio-dev openimageio-tools libmetis-dev libgoogle-glog-dev \
      libgtest-dev libgmock-dev libsqlite3-dev libglew-dev libcgal-dev libceres-dev \
      libsuitesparse-dev libcurl4-openssl-dev libssl-dev \
      python3.12 python3.12-dev p7zip-full unzip psmisc \
    && mkdir -p /usr/include/opencv4 \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --branch ${COLMAP_TAG} --depth 1 https://github.com/colmap/colmap.git /src/colmap \
 && cmake -S /src/colmap -B /src/colmap/build -GNinja -DCMAKE_BUILD_TYPE=Release \
      -DGUI_ENABLED=OFF -DCUDA_ENABLED=ON -DCMAKE_CUDA_ARCHITECTURES="${CUDA_ARCHS}" \
 && cmake --build /src/colmap/build -j${JOBS} && cmake --install /src/colmap/build \
 && rm -rf /src/colmap

# Python env from the committed lockfile (cu126 torch flavour).
COPY --from=ghcr.io/astral-sh/uv:0.12.18 /uv /usr/local/bin/uv
WORKDIR /opt/roomscan
COPY pyproject.toml uv.lock ./
RUN uv sync --locked --extra cu126 --no-install-project \
 && uv pip uninstall --python .venv pycolmap \
 && uv pip install --python .venv pycolmap-cuda12==4.2.1
ENV PATH=/opt/roomscan/.venv/bin:${PATH}

# Optional nerfstudio baseline (conflicting pins -> own venv).
RUN if [ "${WITH_NERFSTUDIO}" = "1" ]; then \
      uv venv /opt/ns && uv pip install --python /opt/ns nerfstudio==1.1.5 \
        --extra-index-url https://download.pytorch.org/whl/cu126 ; fi

# Smoke checks that need no GPU at build time.
RUN colmap -h > /dev/null && colmap help | grep -q global_mapper \
 && python -c "import open3d as o; assert o._build_config['BUILD_CUDA_MODULE']" \
 && python -c "import torch, gsplat, pycolmap; assert torch.version.cuda.startswith('12.6'); print(pycolmap.__version__)"
WORKDIR /work
