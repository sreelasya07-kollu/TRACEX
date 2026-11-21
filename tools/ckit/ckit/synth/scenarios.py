"""Golden-scenario stand-ins (ADR 0002): deterministic synthetic raw sessions.

Writes the Phase 0 raw layout (docs/program-spec/recording-protocol.md) with ARKit pose
convention, so the edge conversion is exercised exactly as for real recordings.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

import cv2
import numpy as np

from ckit.rot import F_FLIP, look_rotation, mat_to_quat
from ckit.synth.render import Cam, render
from ckit.synth.scene import Box, Room, build_room

POSE_HZ = 30.0
IMU_HZ = 200.0
G = 9.81


@dataclass
class Scenario:
    name: str
    room: tuple[float, float, float]
    path: str = "perimeter"  # perimeter | corridor
    speed: float = 0.35
    sweep_deg: float = 30.0
    sweep_period: float = 6.0
    boxes: list[Box] = field(default_factory=list)
    blank_walls: bool = False
    light: float = 1.0
    noise: float = 0.01
    exposure_ms: float = 12.0
    subframes: int = 1
    loss: list[tuple[float, float]] = field(default_factory=list)
    inset: float = 0.9


SCENARIOS: dict[str, Scenario] = {
    "G1": Scenario("small_room", (3.6, 3.2, 2.5)),
    "G2": Scenario("large_room", (7.0, 5.5, 2.7), speed=0.5, inset=1.1),
    "G3": Scenario("corridor", (1.8, 9.0, 2.5), path="corridor", sweep_deg=65, sweep_period=5),
    "G4": Scenario(
        "cluttered",
        (5.0, 4.5, 2.6),
        boxes=[
            Box(2.0, 1.8, 3.0, 2.7, 0.75),
            Box(0.0, 1.2, 0.55, 3.0, 0.85),
            Box(4.45, 3.9, 5.0, 4.5, 1.8),
            Box(1.6, 3.0, 2.1, 3.4, 0.9),
        ],
        inset=1.0,
    ),
    "G5": Scenario(
        "low_light", (3.6, 3.2, 2.5), light=0.22, noise=0.035, exposure_ms=33, subframes=2
    ),
    "G6": Scenario("blank_walls", (3.6, 3.2, 2.5), blank_walls=True),
    "G7": Scenario(
        "fast_motion",
        (3.6, 3.2, 2.5),
        speed=0.9,
        sweep_deg=45,
        sweep_period=2.5,
        exposure_ms=16,
        subframes=5,
    ),
    "G8": Scenario("tracking_loss", (3.6, 3.2, 2.5), loss=[(12.0, 15.0), (28.0, 31.0)]),
}


def _waypoints(sc: Scenario) -> tuple[np.ndarray, np.ndarray]:
    """Polyline positions (N,2 xz) and base yaw (N,) at each vertex."""
    W, D, _ = sc.room
    if sc.path == "corridor":
        a, b = 0.8, D - 0.8
        pts = np.array([[W / 2, a], [W / 2, b], [W / 2 + 0.01, b], [W / 2, a]])
        yaw = np.array([0.0, 0.0, np.pi, np.pi])
        return pts, yaw
    i = sc.inset
    corners = np.array([[i, i], [W - i, i], [W - i, D - i], [i, D - i], [i, i]])
    # Facing outward: edge z=i -> -Z (yaw=pi), x=W-i -> +X, z=D-i -> +Z, x=i -> -X.
    edge_yaw = [np.pi, np.pi / 2, 0.0, -np.pi / 2]
    pts, yaw = [], []
    for k in range(4):
        pts += [corners[k], corners[k + 1]]
        yaw += [edge_yaw[k], edge_yaw[k]]
    # Second loop facing inward at the same inset to see objects and the opposite walls.
    for k in range(4):
        pts += [corners[k], corners[k + 1]]
        yaw += [edge_yaw[k] + np.pi, edge_yaw[k] + np.pi]
    return np.array(pts), np.unwrap(np.array(yaw))


def trajectory(sc: Scenario, duration: float | None, seed: int):
    """Returns times t (s), positions (N,3), R_world_from_camera (N,3,3) at POSE_HZ / IMU_HZ."""
    pts, yaw_v = _waypoints(sc)
    seg = np.linalg.norm(np.diff(pts, axis=0), axis=1)
    # Turning in place costs time too (about 1 s per 90 degrees).
    turn = np.abs(np.diff(yaw_v)) / (np.pi / 2)
    seg_t = seg / sc.speed + turn * (1.0 if sc.speed < 0.6 else 0.5)
    t_v = np.concatenate([[0], np.cumsum(seg_t)])
    total = t_v[-1] if duration is None else min(duration, t_v[-1])

    def at(t):
        t = np.clip(t, 0, t_v[-1])
        x = np.interp(t, t_v, pts[:, 0])
        z = np.interp(t, t_v, pts[:, 1])
        yaw = np.interp(t, t_v, yaw_v)
        return x, z, yaw

    rng = np.random.default_rng(seed + 7)
    phase = rng.uniform(0, 2 * np.pi)
    sw = np.deg2rad(sc.sweep_deg)

    def pose(t):
        # Smooth by averaging the piecewise-linear path over a 1 s window (no corner kinks).
        k = np.linspace(-0.5, 0.5, 11)
        x, z, yaw = (np.mean(a, axis=0) for a in at(t[None, :] + k[:, None]))
        yaw = yaw + sw * np.sin(2 * np.pi * t / sc.sweep_period + phase)
        pitch = np.deg2rad(-12 + 14 * np.sin(2 * np.pi * t / (sc.sweep_period * 1.7) + 1.0))
        roll = np.deg2rad(2.0 * np.sin(2 * np.pi * t / 3.1))
        y = 1.45 + 0.03 * np.sin(2 * np.pi * t / 0.9)  # walking bob
        P = np.stack([x, y, z], -1)
        R = np.stack([look_rotation(a, b, c) for a, b, c in zip(yaw, pitch, roll, strict=True)])
        return P, R

    return total, pose


def _imu(pose, t: np.ndarray, rng) -> tuple[np.ndarray, np.ndarray]:
    """Gyro (rad/s) and accel (specific force, m/s^2) in the ARKit camera frame (imu frame)."""
    dt = 1e-3
    P0, R0 = pose(t - dt)
    P1, R1 = pose(t)
    P2, R2 = pose(t + dt)
    from ckit.rot import log_so3

    w = np.stack([log_so3(a.T @ b) / (2 * dt) for a, b in zip(R0, R2, strict=True)])
    acc_w = (P2 - 2 * P1 + P0) / (dt * dt)
    f_w = acc_w + np.array([0, G, 0])
    f_c = np.einsum("nji,nj->ni", R1, f_w)
    # Express in the ARKit camera frame (imu frame): v_imu = F v_cv.
    gyro = w @ F_FLIP + rng.normal(0, 0.002, w.shape) + 0.001
    accel = f_c @ F_FLIP + rng.normal(0, 0.02, f_c.shape) + 0.01
    return gyro, accel


def generate(
    scenario_id: str,
    out: Path,
    seed: int = 0,
    duration: float | None = None,
    frame_hz: float = 5.0,
    cam: Cam | None = None,
) -> Path:
    sc = SCENARIOS[scenario_id]
    cam = cam or Cam()
    room: Room = build_room(*sc.room, seed=seed, boxes=sc.boxes, blank_walls=sc.blank_walls)
    total, pose = trajectory(sc, duration, seed)
    rng = np.random.default_rng(seed)
    out.mkdir(parents=True, exist_ok=True)
    (out / "frames").mkdir(exist_ok=True)
    t0_ns = 1_000_000_000_000  # arbitrary boot-relative origin

    def ns(t: float) -> int:
        return t0_ns + int(round(t * 1e9))

    def lost(t: float) -> bool:
        return any(a <= t < b for a, b in sc.loss)

    # Reported pose = true pose + slow random-walk drift (AR tracking is not perfect).
    tp = np.arange(0, total, 1 / POSE_HZ)
    drift = np.cumsum(rng.normal(0, 0.0004, (len(tp), 3)), axis=0)
    P, R = pose(tp)
    with open(out / "poses.jsonl", "w") as f:
        last = None
        for i, t in enumerate(tp):
            if lost(t):
                state, reason = "NOT_AVAILABLE", "INSUFFICIENT_FEATURES"
                p, Rr = last if last else (P[i], R[i])
            else:
                state = "LIMITED" if t < 1.0 else "NORMAL"
                reason = "INITIALIZING" if t < 1.0 else "NONE"
                p, Rr = P[i] + drift[i], R[i]
                last = (p, Rr)
            q = mat_to_quat(Rr @ F_FLIP)  # ARKit camera convention on disk
            f.write(
                json.dumps(
                    {
                        "t_ns": ns(t),
                        "p": [round(v, 6) for v in p],
                        "q": [round(v, 8) for v in q],
                        "tracking": state,
                        "reason": reason,
                    }
                )
                + "\n"
            )

    ti = np.arange(0, total, 1 / IMU_HZ)
    gyro, accel = _imu(pose, ti, rng)
    with open(out / "imu.jsonl", "w") as f:
        for t, g, a in zip(ti, gyro, accel, strict=True):
            f.write(
                json.dumps({"t_ns": ns(t), "type": "gyro", "v": [round(v, 6) for v in g]}) + "\n"
            )
            f.write(
                json.dumps({"t_ns": ns(t), "type": "accel", "v": [round(v, 5) for v in a]}) + "\n"
            )

    with open(out / "intrinsics.jsonl", "w") as f:
        f.write(
            json.dumps(
                {
                    "t_ns": ns(0),
                    "w": cam.w,
                    "h": cam.h,
                    "fx": cam.fx,
                    "fy": cam.fy,
                    "cx": cam.cx,
                    "cy": cam.cy,
                }
            )
            + "\n"
        )

    seen: dict[int, list[np.ndarray]] = {}
    plane_lines, point_lines, frame_lines = [], [], []
    tf = np.arange(0.1, total, 1 / frame_hz)
    exp = sc.exposure_ms / 1000
    for i, t in enumerate(tf):
        sub = np.linspace(t - exp / 2, t + exp / 2, sc.subframes) if sc.subframes > 1 else [t]
        acc = None
        for ts in sub:
            Ps, Rs = pose(np.array([ts]))
            h = render(room, cam, Rs[0], Ps[0], light=sc.light)
            acc = h.rgb if acc is None else acc + h.rgb
        rgb = acc / len(sub)
        if lost(t):
            rgb = rgb * 0.03  # lens covered
        rgb = rgb + rng.normal(0, sc.noise, rgb.shape)
        img = np.clip(rgb * 255, 0, 255).astype(np.uint8)[..., ::-1]
        name = f"frames/{i:06d}.jpg"
        cv2.imwrite(str(out / name), img, [cv2.IMWRITE_JPEG_QUALITY, 92])
        frame_lines.append({"i": i, "t_ns": ns(t), "file": name, "exposure_ns": int(exp * 1e9)})
        if lost(t):
            continue
        # AR feature points: textured pixels only (blank walls give few).
        if len(sub) > 1:
            Pc, Rc = pose(np.array([t]))
            h = render(room, cam, Rc[0], Pc[0])
        gray = cv2.cvtColor((h.rgb * 255).astype(np.uint8), cv2.COLOR_RGB2GRAY).astype(np.float32)
        grad = np.abs(cv2.Laplacian(gray, cv2.CV_32F, ksize=3))
        cand = np.argwhere((grad > 25) & (h.surf >= 0))
        if len(cand):
            pick = cand[rng.choice(len(cand), size=min(120, len(cand)), replace=False)]
            pts = h.xyz[pick[:, 0], pick[:, 1]] + rng.normal(0, 0.01, (len(pick), 3))
            point_lines.append({"t_ns": ns(t), "pts": np.round(pts, 4).tolist()})
            for r, c in pick:
                seen.setdefault(int(h.surf[r, c]), []).append(h.xyz[r, c])
        if i % max(1, int(frame_hz)) == 0:
            for si, plist in sorted(seen.items()):
                s = room.surfaces[si]
                if len(plist) < 15 or (s.kind == "object" and not s.name.endswith("_top")):
                    continue
                a = np.array(plist) - s.origin
                ua, va = a @ s.u, a @ s.v
                ctr = s.origin + s.u * (ua.min() + ua.max()) / 2 + s.v * (va.min() + va.max()) / 2
                kind = s.kind if s.kind != "object" else "table"
                plane_lines.append(
                    {
                        "t_ns": ns(t),
                        "id": si,
                        "kind": kind,
                        "center": np.round(ctr, 4).tolist(),
                        "normal": np.round(s.normal, 6).tolist(),
                        "axis_u": np.round(s.u, 6).tolist(),
                        "extent": [round(float(np.ptp(ua)), 4), round(float(np.ptp(va)), 4)],
                    }
                )
    for fname, lines in (
        ("frames.jsonl", frame_lines),
        ("points.jsonl", point_lines),
        ("planes.jsonl", plane_lines),
    ):
        with open(out / fname, "w") as f:
            for ln in lines:
                f.write(json.dumps(ln) + "\n")
    device = {
        "platform": "synthetic",
        "model": f"synth-{scenario_id}-{sc.name}",
        "os": "none",
        "app_version": "0.0.1",
        "pose_convention": "arkit",
        "imu_frame": "arkit_camera",
        "q_camera_from_imu": [0.0, 1.0, 0.0, 0.0],
        "clock_base": "synthetic",
        "clock_offset_ns": 0,
        "seed": seed,
        "scenario": scenario_id,
        "ground_truth": {
            "room": list(sc.room),
            "boxes": [[b.x0, b.z0, b.x1, b.z1, b.h] for b in sc.boxes],
        },
    }
    (out / "device.json").write_text(json.dumps(device, indent=2) + "\n")
    return out
