"""Extract the official NYUv2 labelled *test* split (654 images, Eigen et al.) from the .mat.

Writes <out>/rgb/<idx>.png (uint8 640x480), <out>/depth/<idx>.npy (float32 metres, the
inpainted 'depths' field used by the standard protocol) and index.json (1-based .mat indices).
"""

import json
import sys
from pathlib import Path

import cv2
import h5py
import numpy as np
from scipy.io import loadmat

mat, splits, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
test_idx = [int(i) for i in loadmat(splits)["testNdxs"].ravel()]  # 1-based
(out / "rgb").mkdir(parents=True, exist_ok=True)
(out / "depth").mkdir(parents=True, exist_ok=True)
with h5py.File(mat, "r") as f:
    images, depths = f["images"], f["depths"]  # stored transposed: (N, 3, W, H) / (N, W, H)
    for i in test_idx:
        rgb = np.transpose(images[i - 1], (2, 1, 0))  # -> H, W, 3
        d = np.transpose(depths[i - 1], (1, 0)).astype(np.float32)
        cv2.imwrite(str(out / "rgb" / f"{i:04d}.png"), cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR))
        np.save(out / "depth" / f"{i:04d}.npy", d)
(out / "index.json").write_text(json.dumps({"split": "test", "n": len(test_idx), "mat_index_1based": test_idx}))
print(f"nyu test: {len(test_idx)} images -> {out}")
