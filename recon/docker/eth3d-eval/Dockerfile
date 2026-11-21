# ETH3D official multi-view evaluation tool (accuracy / completeness / F-score vs GT laser scan).
# Used only to score our reconstructions; nothing from it is linked into our code.
# PCL 1.14 (Ubuntu 24.04) needs C++14+; the pinned ETH3D commit sets C++11, patched to 17 below.
# Build: docker build -t room-eth3d-eval reconstruction/docker/eth3d-eval
# Use:   docker run --rm -v "$PWD:/work" -w /work room-eth3d-eval \
#          --reconstruction_ply_path recon.ply --ground_truth_mlp_path scan_alignment.mlp \
#          --tolerances 0.01,0.02,0.05,0.1,0.2
FROM ubuntu:24.04
ARG ETH3D_COMMIT=0daa4f4de036d47d37a6ba054c51fd66642ef896
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates libboost-filesystem-dev libboost-system-dev \
      libeigen3-dev libpcl-dev \
    && rm -rf /var/lib/apt/lists/*
RUN git clone https://github.com/ETH3D/multi-view-evaluation.git /src/eval \
 && git -C /src/eval checkout -q ${ETH3D_COMMIT} \
 && sed -i -E 's/(-std=)(c|gnu)\+\+(0x|11)/\1c++17/g; s/CMAKE_CXX_STANDARD 11/CMAKE_CXX_STANDARD 17/g' /src/eval/CMakeLists.txt \
 && cmake -S /src/eval -B /src/eval/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 \
 && cmake --build /src/eval/build -j2 \
 && install -m755 /src/eval/build/ETH3DMultiViewEvaluation /usr/local/bin/
WORKDIR /work
ENTRYPOINT ["ETH3DMultiViewEvaluation"]
