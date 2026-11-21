"""Gaussian-splat training (Part 4, HANDOFF P9). Training needs CUDA (gsplat); everything else
(dataset, initialisation, PLY export/import) runs on CPU and is tested here."""

from recon.splat.data import SplatDataset, SplatView, init_gaussians, load_dataset
from recon.splat.plyio import read_gaussian_ply, write_gaussian_ply
from recon.splat.train import GpuRequired, SplatJob, cuda_available, train_splat

__all__ = [
    "GpuRequired",
    "SplatDataset",
    "SplatJob",
    "SplatView",
    "cuda_available",
    "init_gaussians",
    "load_dataset",
    "read_gaussian_ply",
    "train_splat",
    "write_gaussian_ply",
]
