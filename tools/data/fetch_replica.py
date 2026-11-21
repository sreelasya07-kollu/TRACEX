"""Fetch a frame subset of NICE-SLAM's rendered Replica scenes using HTTP range requests.

Replica.zip is 12.4 GB; we read only the members we need (remotezip). Writes
data/replica/<scene>/{results/frameXXXXXX.jpg, results/depthXXXXXX.png, traj.txt}
plus <scene>_mesh.ply, cam_params.json and SUBSET.json (exact frame ids kept).
Usage: python data/fetch_replica.py --scenes room0 office0 --stride 5
"""

import argparse
import json
import time
from pathlib import Path

from remotezip import RemoteZip

URL = "https://cvg-data.inf.ethz.ch/nice-slam/data/Replica.zip"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenes", nargs="+", default=["room0", "office0"])
    ap.add_argument("--stride", type=int, default=5)
    ap.add_argument("--out", type=Path, default=Path(__file__).parent / "replica")
    a = ap.parse_args()
    
    z = RemoteZip(URL, timeout=30)
    try:
        names = set(z.namelist())
        for scene in a.scenes:
            dst = a.out / scene
            n_frames = sum(1 for n in names if n.startswith(f"Replica/{scene}/results/frame"))
            keep = list(range(0, n_frames, a.stride))
            members = [f"Replica/{scene}/traj.txt", f"Replica/{scene}_mesh.ply",
                       "Replica/cam_params.json"]
            for i in keep:
                members += [f"Replica/{scene}/results/frame{i:06d}.jpg",
                            f"Replica/{scene}/results/depth{i:06d}.png"]
            for m in members:
                rel = Path(m).relative_to("Replica")
                target = a.out / rel if not rel.name.endswith("_mesh.ply") else a.out / rel.name
                if target.exists() and target.stat().st_size > 0:
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                data = None
                for attempt in range(5):
                    try:
                        data = z.read(m)
                        break
                    except Exception as e:
                        if attempt == 4:
                            raise
                        print(f"Retry {attempt+1}/5 for {m} due to {e}")
                        time.sleep(2 * (attempt + 1))
                        try:
                            z.close()
                        except Exception:
                            pass
                        z = RemoteZip(URL, timeout=30)
                target.write_bytes(data)
            (dst / "SUBSET.json").write_text(json.dumps(
                {"source": URL, "scene": scene, "stride": a.stride, "n_frames_total": n_frames,
                 "frame_ids": keep}, indent=1))
            print(f"replica {scene}: {len(keep)}/{n_frames} frames -> {dst}")
    finally:
        try:
            z.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
