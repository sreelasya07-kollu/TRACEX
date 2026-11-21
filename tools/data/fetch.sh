#!/usr/bin/env bash
# Fetch benchmark datasets into data/ (contents gitignored except scripts + DATASETS.md).
# See data/DATASETS.md for licences and the exact subsets. Never put real user scans here.
#   data/fetch.sh [replica] [tum] [nyu] [eth3d]      (default: replica tum nyu)
# ARKitScenes is fetched only on the GPU test machine: data/fetch.sh arkitscenes (TODO there).
set -euo pipefail
D="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="${PY:-$D/../.venv/bin/python}"
T=("$@"); [ ${#T[@]} -gt 0 ] || T=(replica tum nyu)
has() { [[ " ${T[*]} " == *" $1 "* ]]; }
dl() {  # url dest
  [ -s "$2" ] && return 0
  mkdir -p "$(dirname "$2")"; curl -fL --retry 3 -o "$2.part" "$1"; mv "$2.part" "$2"
}

if has replica; then
  "$PY" "$D/fetch_replica.py" --scenes ${REPLICA_SCENES:-room0 office0} --stride "${REPLICA_STRIDE:-5}"
  if [ ! -d "$D/replica/cull_replica_mesh" ]; then
    dl https://cvg-data.inf.ethz.ch/nice-slam/cull_replica_mesh.zip "$D/_dl/cull_replica_mesh.zip"
    (cd "$D/replica" && unzip -q -o "$D/_dl/cull_replica_mesh.zip")
  fi
fi

if has tum; then
  for s in freiburg1_desk; do
    if [ ! -d "$D/tum/rgbd_dataset_$s" ]; then
      dl "https://cvg.cit.tum.de/rgbd/dataset/${s%%_*}/rgbd_dataset_$s.tgz" "$D/_dl/$s.tgz"
      mkdir -p "$D/tum"; tar -xzf "$D/_dl/$s.tgz" -C "$D/tum"
    fi
  done
fi

if has nyu; then
  if [ ! -f "$D/nyu/test/index.json" ]; then
    dl http://horatio.cs.nyu.edu/mit/silberman/nyu_depth_v2/nyu_depth_v2_labeled.mat "$D/_dl/nyu_labeled.mat"
    dl http://horatio.cs.nyu.edu/mit/silberman/indoor_seg_sup/splits.mat "$D/_dl/nyu_splits.mat"
    "$PY" "$D/extract_nyu.py" "$D/_dl/nyu_labeled.mat" "$D/_dl/nyu_splits.mat" "$D/nyu/test"
    rm -f "$D/_dl/nyu_labeled.mat"   # 3 GB; the extracted test split is all we use
  fi
fi

if has eth3d; then
  if [ ! -d "$D/eth3d/office/images" ]; then
    dl https://www.eth3d.net/data/office_dslr_undistorted.7z "$D/_dl/office_dslr_undistorted.7z"
    dl https://www.eth3d.net/data/office_dslr_scan_eval.7z "$D/_dl/office_dslr_scan_eval.7z"
    mkdir -p "$D/eth3d"
    7z x -y -o"$D/eth3d" "$D/_dl/office_dslr_undistorted.7z" >/dev/null
    7z x -y -o"$D/eth3d" "$D/_dl/office_dslr_scan_eval.7z" >/dev/null
  fi
fi

( cd "$D" && find . -path ./_dl -prune -o -type f \( -name "*.ply" -o -name "*.txt" -o -name "*.json" \) -newer "$0" -print0 2>/dev/null | sort -z | xargs -0 -r sha256sum ) > "$D/SHA256SUMS.txt" || true
[ "${KEEP_DOWNLOADS:-0}" = 1 ] || rm -rf "$D/_dl"
echo "datasets ready in $D"
