"""Held-out-view evaluation of a textured mesh (HANDOFF P8): render the textured mesh at the
poses of images that were NOT used to build/texture it, compare with those photographs."""

from __future__ import annotations

from pathlib import Path

import cv2
import numpy as np
import pycolmap
from torchmetrics.functional import structural_similarity_index_measure

from recon.render import TexturedMesh, psnr
from recon.sparse import SparseScene


def split_names(names: list[str], every: int = 5) -> tuple[list[str], list[str]]:
    names = sorted(names)
    held = [n for k, n in enumerate(names) if k % every == every - 1]
    return [n for n in names if n not in held], held


def heldout_render_metrics(
    textured: Path,
    scene: SparseScene,
    held_names: list[str],
    image_scale: float,
    out: Path | None,
    v_up: bool = False,  # OpenMVS TextureMesh OBJ: v measured from the top (see recon/render.py)
) -> dict:
    """PSNR/SSIM per held-out view. `psnr_covered` is over pixels the mesh covers; `coverage`
    is the covered fraction; `psnr_all` fills uncovered pixels with black. All three are
    reported so a mesh cannot score well by covering almost nothing."""
    import torch

    mesh = TexturedMesh(textured, v_up=v_up)
    rec = pycolmap.Reconstruction(scene.model_dir)
    by_name = {im.name: im for im in rec.images.values()}
    rows = []
    for name in held_names:
        im = by_name[name]
        cam = rec.cameras[im.camera_id]
        K = np.asarray(cam.calibration_matrix())
        W, H = round(cam.width * image_scale), round(cam.height * image_scale)
        K2 = K.copy()
        K2[:2] *= image_scale
        K2[0, 2] = (K[0, 2] + 0.5) * image_scale - 0.5
        K2[1, 2] = (K[1, 2] + 0.5) * image_scale - 0.5
        T_wc = np.eye(4)
        T_wc[:3, :] = im.cam_from_world().inverse().matrix()
        gt = cv2.cvtColor(
            cv2.resize(
                cv2.imread(str(scene.images_dir / name)), (W, H), interpolation=cv2.INTER_AREA
            ),
            cv2.COLOR_BGR2RGB,
        )
        rgb, hit, _ = mesh.render(K2, T_wc, W, H)
        t = lambda x: torch.from_numpy(x).permute(2, 0, 1)[None].float() / 255  # noqa: E731
        ssim = float(structural_similarity_index_measure(t(rgb), t(gt), data_range=1.0))
        rows.append(
            {
                "image": name,
                "coverage": float(hit.mean()),
                "psnr_covered": psnr(rgb, gt, hit),
                "psnr_all": psnr(rgb, gt),
                "ssim_all": ssim,
            }
        )
        if out is not None:
            out.mkdir(parents=True, exist_ok=True)
            side = np.concatenate([rgb, gt], 1)
            cv2.imwrite(str(out / f"{Path(name).stem}_render_vs_gt.jpg"), side[..., ::-1])
    keys = ("coverage", "psnr_covered", "psnr_all", "ssim_all")
    return {"per_image": rows, **{f"mean_{k}": float(np.mean([r[k] for r in rows])) for k in keys}}
