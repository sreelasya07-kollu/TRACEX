# OpenMVS v2.4.0 (CPU build) for Room Scanner Part 4 dense reconstruction / meshing / texturing.
# Dependencies come from Ubuntu 26.04 packages instead of vcpkg (much faster, smaller).
# 26.04 is required: OpenMVS 2.4 needs OpenCV >= 4.10 (Ubuntu 24.04 has 4.6).
# Build:  docker build -t room-openmvs:2.4.0 reconstruction/docker/openmvs
# Use:    docker run --rm -v "$PWD:/work" -w /work room-openmvs:2.4.0 DensifyPointCloud scene.mvs
# Binaries: InterfaceCOLMAP DensifyPointCloud ReconstructMesh RefineMesh TextureMesh TransformScene
ARG UBUNTU=26.04
FROM ubuntu:${UBUNTU} AS build

ARG OPENMVS_TAG=v2.4.0
ARG OPENMVS_COMMIT=58117204c86bbb11a0b25b26a8987676cf11274d
ARG VCG_COMMIT=658ba36d0a5666650da6e066b4794efc5a463407
ARG JOBS=2

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates pkg-config \
      libboost-iostreams-dev libboost-program-options-dev libboost-system-dev \
      libboost-serialization-dev libboost-filesystem-dev \
      libeigen3-dev libopencv-dev libcgal-dev libceres-dev libnanoflann-dev \
      libjpeg-dev libpng-dev libtiff-dev zlib1g-dev libjxl-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git clone https://github.com/cdcseacave/VCG.git /src/vcglib \
 && git -C /src/vcglib checkout -q ${VCG_COMMIT}
RUN git clone --branch ${OPENMVS_TAG} --depth 1 --recurse-submodules --shallow-submodules \
      https://github.com/cdcseacave/openMVS.git /src/openMVS \
 && test "$(git -C /src/openMVS rev-parse HEAD)" = "${OPENMVS_COMMIT}" \
 && sed -i 's/COMPONENTS iostreams program_options system serialization/COMPONENTS iostreams program_options serialization/' \
      /src/openMVS/CMakeLists.txt \
 && sed -i 's/cv::IMWRITE_JPEGXL_QUALITY/640 \/* cv::IMWRITE_JPEGXL_QUALITY (OpenCV>=4.11) *\//' \
      /src/openMVS/libs/Common/Types.inl
# Source patches needed against Ubuntu 26.04 packages:
# 1. Boost >= 1.89 (26.04 ships 1.90) has no boost_system CMake package:
#    Boost.System is header-only, so the component is dropped from find_package.
# 2. cv::IMWRITE_JPEGXL_QUALITY first appears in OpenCV 4.11; 26.04 ships 4.10. It is only
#    used when *saving .jxl* images (never done by our pipeline); replaced by its value 640.

RUN cmake -S /src/openMVS -B /src/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DVCG_ROOT=/src/vcglib \
      -DOpenMVS_USE_CUDA=OFF \
      -DOpenMVS_USE_PYTHON=OFF \
      -DOpenMVS_BUILD_VIEWER=OFF \
      -DCMAKE_INSTALL_PREFIX=/opt/openmvs \
 && cmake --build /src/build -j${JOBS} \
 && cmake --install /src/build
# Collect exactly the shared libraries the binaries need, so the runtime stage
# doesn't depend on distro-specific package names.
RUN mkdir -p /deps && find /opt/openmvs -type f -perm -u+x -exec ldd {} + 2>/dev/null \
      | awk '$2=="=>" && $3 ~ /^\// {print $3}' | sort -u \
      | while read -r f; do echo "$(readlink -f "$(dirname "$f")")/$(basename "$f")"; done \
      | sort -u | xargs -r -I{} cp -L --parents {} /deps
# ^ dirname is canonicalised because /lib -> usr/lib is a symlink in Ubuntu images,
#   and COPY cannot write a real /lib directory over it.

FROM ubuntu:${UBUNTU}
COPY --from=build /deps/ /
COPY --from=build /opt/openmvs /opt/openmvs
ENV PATH=/opt/openmvs/bin/OpenMVS:/opt/openmvs/bin:${PATH} \
    LD_LIBRARY_PATH=/opt/openmvs/lib:/opt/openmvs/lib/OpenMVS
WORKDIR /work
CMD ["DensifyPointCloud", "--help"]
