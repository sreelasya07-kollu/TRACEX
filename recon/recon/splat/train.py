"""Gaussian-splat training worker (HANDOFF P9) around gsplat 1.5.3 `rasterization` +
`DefaultStrategy`. Initialised from the SpatialMap fused cloud, held-out every 8th image.

STATUS: written against the gsplat API but NOT executed on this machine (no CUDA). The CPU-side
pieces (dataset, init, PLY export) are tested; everything in `train_splat` is verified only on
a CUDA machine (tests/test_phase9_gpu.py, marked `gpu`). `SplatJob` is a plain JSON-serialisable
description so the same job can be shipped to a cloud GPU worker."""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np

from formats.spatial_map import load_spatial_map
from recon.splat.data import init_gaussians, load_dataset
from recon.splat.plyio import write_gaussian_ply


class GpuRequired(RuntimeError):
    """Raised instead of silently producing nothing when no CUDA device is available."""


@dataclass
class SplatJob:
    spatial_map: str
    out_dir: str
    device: str = "cuda"
    steps: int = 7000
    sh_degree: int = 3
    image_scale: float = 0.5
    heldout_every: int = 8
    ssim_lambda: float = 0.2
    max_init_points: int = 200_000
    seed: int = 0
    save_renders: bool = True

    def to_json(self) -> str:
        return json.dumps(asdict(self), indent=1)

    @classmethod
    def from_json(cls, text: str) -> SplatJob:
        return cls(**json.loads(text))


def cuda_available() -> bool:
    try:
        import torch

        return bool(torch.cuda.is_available())
    except ImportError:
        return False


def train_splat(job: SplatJob) -> dict:
    """Trains and exports `<out>/point_cloud.ply`, `cfg.json`, `train_log.jsonl`, `ckpt_last.pt`
    and `eval/metrics.json` (held-out PSNR / SSIM / LPIPS). Returns the metrics dict."""
    if not job.device.startswith("cuda") or not cuda_available():
        raise GpuRequired(
            f"splat training needs a CUDA GPU (device={job.device!r}, "
            f"cuda_available={cuda_available()}); run this job on the GPU machine"
        )
    import torch
    import torch.nn.functional as F
    from gsplat.rendering import rasterization
    from gsplat.strategy import DefaultStrategy
    from torchmetrics.functional import peak_signal_noise_ratio, structural_similarity_index_measure
    from torchmetrics.image.lpip import LearnedPerceptualImagePatchSimilarity

    out = Path(job.out_dir)
    (out / "eval").mkdir(parents=True, exist_ok=True)
    torch.manual_seed(job.seed)
    rng = np.random.default_rng(job.seed)
    dev = torch.device(job.device)
    sm = load_spatial_map(Path(job.spatial_map))
    ds = load_dataset(sm, job.image_scale, job.heldout_every, job.max_init_points)
    init = init_gaussians(ds.init_xyz, ds.init_rgb, job.sh_degree, seed=job.seed)
    params = torch.nn.ParameterDict(
        {k: torch.nn.Parameter(torch.from_numpy(v).to(dev)) for k, v in init.items()}
    )
    lrs = {
        "means": 1.6e-4 * ds.scene_scale,
        "scales": 5e-3,
        "quats": 1e-3,
        "opacities": 5e-2,
        "sh0": 2.5e-3,
        "shN": 2.5e-3 / 20,
    }
    optimizers = {
        k: torch.optim.Adam([{"params": params[k], "lr": lr, "name": k}], eps=1e-15)
        for k, lr in lrs.items()
    }
    strategy = DefaultStrategy(verbose=False)
    strategy.check_sanity(params, optimizers)
    state = strategy.initialize_state(scene_scale=ds.scene_scale)

    def to_t(v):
        return (
            torch.from_numpy(v.image).to(dev),
            torch.from_numpy(v.K).float().to(dev)[None],
            torch.from_numpy(v.viewmat).float().to(dev)[None],
        )

    def render(K, vm, H, W, sh):
        colors = torch.cat([params["sh0"], params["shN"]], 1)
        img, alpha, info = rasterization(
            means=params["means"],
            quats=params["quats"],
            scales=torch.exp(params["scales"]),
            opacities=torch.sigmoid(params["opacities"]),
            colors=colors,
            viewmats=vm,
            Ks=K,
            width=W,
            height=H,
            sh_degree=sh,
            packed=False,
        )
        return img.clamp(0, 1), alpha, info

    torch.cuda.reset_peak_memory_stats()
    t0 = time.perf_counter()
    log = open(out / "train_log.jsonl", "w")
    for step in range(job.steps):
        v = ds.train[int(rng.integers(len(ds.train)))]
        gt, K, vm = to_t(v)
        H, W = gt.shape[:2]
        sh = min(step // 1000, job.sh_degree)
        pred, _, info = render(K, vm, H, W, sh)
        strategy.step_pre_backward(params, optimizers, state, step, info)
        l1 = F.l1_loss(pred[0], gt)
        ssim = structural_similarity_index_measure(
            pred.permute(0, 3, 1, 2), gt[None].permute(0, 3, 1, 2), data_range=1.0
        )
        loss = (1 - job.ssim_lambda) * l1 + job.ssim_lambda * (1 - ssim)
        loss.backward()
        strategy.step_post_backward(params, optimizers, state, step, info, packed=False)
        for opt in optimizers.values():
            opt.step()
            opt.zero_grad(set_to_none=True)
        for g in optimizers["means"].param_groups:
            g["lr"] = lrs["means"] * (0.01 ** (step / max(1, job.steps)))
        if step % 100 == 0:
            log.write(
                json.dumps({"step": step, "loss": float(loss), "n": len(params["means"])}) + "\n"
            )
    train_s = time.perf_counter() - t0
    log.close()

    lp = LearnedPerceptualImagePatchSimilarity(net_type="alex", normalize=True).to(dev)
    ps, ss, ls = [], [], []
    with torch.no_grad():
        for v in ds.heldout:
            gt, K, vm = to_t(v)
            pred, _, _ = render(K, vm, gt.shape[0], gt.shape[1], job.sh_degree)
            a, b = pred.permute(0, 3, 1, 2), gt[None].permute(0, 3, 1, 2)
            ps.append(float(peak_signal_noise_ratio(a, b, data_range=1.0)))
            ss.append(float(structural_similarity_index_measure(a, b, data_range=1.0)))
            ls.append(float(lp(a, b)))
            if job.save_renders:
                import cv2

                cv2.imwrite(
                    str(out / "eval" / f"{v.frame_id}.png"),
                    cv2.cvtColor((pred[0].cpu().numpy() * 255).astype(np.uint8), cv2.COLOR_RGB2BGR),
                )
    export = {k: p.detach().cpu().numpy() for k, p in params.items()}
    write_gaussian_ply(out / "point_cloud.ply", export)
    torch.save({k: p.detach().cpu() for k, p in params.items()}, out / "ckpt_last.pt")
    (out / "cfg.json").write_text(job.to_json())
    metrics = {
        "psnr": float(np.mean(ps)),
        "ssim": float(np.mean(ss)),
        "lpips": float(np.mean(ls)),
        "n_heldout": len(ds.heldout),
        "n_train": len(ds.train),
        "n_gaussians": len(export["means"]),
        "train_time_s": train_s,
        "peak_vram_mb": torch.cuda.max_memory_allocated() / 2**20,
        "steps": job.steps,
        "device": torch.cuda.get_device_name(dev),
    }
    (out / "eval" / "metrics.json").write_text(json.dumps(metrics, indent=1))
    return metrics
