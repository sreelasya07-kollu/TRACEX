"""All gate thresholds in one place (proposals: tune from measured rescan rates)."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Thresholds:
    reg_ratio_pass: float = 0.95
    reg_ratio_warn: float = 0.80
    reproj_pass_px: float = 1.0
    reproj_warn_px: float = 2.0
    scale_conf_pass: float = 0.7
    scale_conf_warn: float = 0.4
    min_walls: int = 3
    wall_floor_dev_pass_deg: float = 3.0
    wall_floor_dev_warn_deg: float = 6.0
    ceiling_height_pass_m: tuple[float, float] = (2.0, 4.5)
    ceiling_height_warn_m: tuple[float, float] = (1.8, 6.0)
    wall_rms_pass_m: float = 0.03
    wall_rms_warn_m: float = 0.05
    wall_coverage_pass: float = 0.95
    wall_coverage_warn: float = 0.80
    floor_coverage_pass: float = 0.90
    floor_coverage_warn: float = 0.70
    ceiling_coverage_warn: float = 0.50  # ceilings are often not scanned: warn, never fail
    depth_valid_pass: float = 0.40
    depth_valid_warn: float = 0.25
    mesh_psnr_pass_db: float = 18.0
    mesh_psnr_warn_db: float = 14.0
    splat_psnr: float = 24.0
    splat_ssim: float = 0.80
    splat_lpips: float = 0.30
    # coverage model
    cell_m: float = 0.25
    min_views: int = 2
    min_view_spread_deg: float = 5.0
    max_grazing_cos: float = 0.2  # |cos(view, normal)| below this does not count as seen
    min_region_area_m2: float = 0.25
