#!/usr/bin/env bash
# Download exactly the inventory dataset subsets (docs/heavy-compute/inventory.yaml `datasets`).
# Resumable (curl -C -), prints sizes (HTTP HEAD) before downloading and asks for confirmation
# (-y to skip). Verification: each file's size must equal the server's Content-Length, then a
# sha256 is recorded in data/heavy/SHA256SUMS; later runs re-verify against it. Upstream does
# not publish checksums for these files, so the first download is the reference.
#   scripts/heavy/fetch_datasets.sh [-y] replica_full replica_cull arkitscenes_2 tum_fr1 \
#                                   nyu_test eth3d_office eth3d_train_all mipnerf360
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
YES=0; [ "${1:-}" = "-y" ] && { YES=1; shift; }
[ $# -gt 0 ] || { echo "usage: $0 [-y] <dataset key>..."; exit 1; }
D=data/heavy; mkdir -p "$D"; SUMS="$D/SHA256SUMS"; touch "$SUMS"
AK=https://docs-assets.developer.apple.com/ml-research/datasets/arkitscenes/v1

urls() {  # key -> "url dest" lines
  case "$1" in
    replica_full) echo "https://cvg-data.inf.ethz.ch/nice-slam/data/Replica.zip $D/Replica.zip" ;;
    replica_cull) echo "https://cvg-data.inf.ethz.ch/nice-slam/cull_replica_mesh.zip $D/cull_replica_mesh.zip" ;;
    tum_fr1) for s in desk xyz; do
               echo "https://cvg.cit.tum.de/rgbd/dataset/freiburg1/rgbd_dataset_freiburg1_$s.tgz $D/tum_fr1_$s.tgz"; done ;;
    nyu_test) echo "http://horatio.cs.nyu.edu/mit/silberman/nyu_depth_v2/nyu_depth_v2_labeled.mat $D/nyu_depth_v2_labeled.mat"
              echo "http://horatio.cs.nyu.edu/mit/silberman/indoor_seg_sup/splits.mat $D/nyu_splits.mat" ;;
    eth3d_office) for f in office_dslr_undistorted office_dslr_scan_eval; do
                    echo "https://www.eth3d.net/data/$f.7z $D/eth3d_$f.7z"; done ;;
    eth3d_train_all) echo "https://www.eth3d.net/data/multi_view_training_dslr_undistorted.7z $D/eth3d_multi_view_training_dslr_undistorted.7z" ;;
    mipnerf360) echo "http://storage.googleapis.com/gresearch/refraw360/360_v2.zip $D/360_v2.zip" ;;
    arkitscenes_2)
      for v in 42444946 42444966; do
        for a in lowres_wide lowres_depth confidence lowres_wide_intrinsics; do
          echo "$AK/raw/Validation/$v/$a.zip $D/arkitscenes/$v/$a.zip"; done
        echo "$AK/raw/Validation/$v/lowres_wide.traj $D/arkitscenes/$v/lowres_wide.traj"
        echo "$AK/raw/Validation/$v/${v}_3dod_mesh.ply $D/arkitscenes/$v/${v}_3dod_mesh.ply"
      done
      # Laser-scanner GT point clouds (visits 421337: 8 scans, 421383: 4 scans), ids from
      # $AK/raw/laser_scanner_point_clouds/laser_scanner_point_clouds_mapping.csv (2026-11-13).
      for p in 421337:170078 421337:170079 421337:170080 421337:170081 421337:170144 \
               421337:170148 421337:170149 421337:170151 421383:170103 421383:170104 \
               421383:170156 421383:170158; do
        echo "$AK/raw/laser_scanner_point_clouds/${p%%:*}/${p##*:}.ply $D/arkitscenes/laser/${p%%:*}/${p##*:}.ply"
      done ;;
    scanbundles_real) echo "ERROR scanbundles_real: no real Scan Bundles exist yet" >&2; return 1 ;;
    *) echo "unknown dataset key: $1" >&2; return 1 ;;
  esac
}

head_size() { curl -sIL --max-time 30 "$1" | tr -d '\r' | awk 'tolower($1)=="content-length:"{l=$2} END{print l+0}'; }

LIST=$(for k in "$@"; do urls "$k"; done)
total=0
echo "files to fetch:"
while read -r url dest; do
  s=$(head_size "$url"); total=$((total + s))
  have=$([ -f "$dest" ] && stat -c %s "$dest" || echo 0)
  awk -v s="$s" -v d="$dest" -v c="$([ "$have" = "$s" ] && echo '  (complete)')" 'BEGIN{printf "  %8.1f MB  %s%s\n", s/1e6, d, c}'
done <<< "$LIST"
awk -v t="$total" -v d="$D" -v f="$(df -h --output=avail "$D" | tail -1)" 'BEGIN{printf "TOTAL %.2f GB, free on %s: %s\n", t/1e9, d, f}'
if [ $YES -ne 1 ]; then read -r -p "Proceed? [y/N] " ans; [ "$ans" = y ] || exit 1; fi

while read -r url dest; do
  mkdir -p "$(dirname "$dest")"
  want=$(head_size "$url")
  if [ ! -f "$dest" ] || [ "$(stat -c %s "$dest")" != "$want" ]; then
    curl -fL --retry 5 -C - -o "$dest" "$url"
  fi
  got=$(stat -c %s "$dest")
  [ "$got" = "$want" ] || { echo "SIZE MISMATCH $dest: $got != $want"; exit 2; }
  if grep -q "  $dest\$" "$SUMS"; then
    grep "  $dest\$" "$SUMS" | sha256sum -c --quiet - || { echo "CHECKSUM CHANGED $dest"; exit 3; }
  else
    sha256sum "$dest" >> "$SUMS"
  fi
  echo "ok $dest"
done <<< "$LIST"
echo "done. Extraction: Replica -> data/fetch_replica.py-style layout via 'unzip -d data/replica'; NYU -> data/extract_nyu.py"
