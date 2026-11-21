"""CPU textured-mesh renderer: a textured quad rendered head-on must reproduce its texture
(known answer, no data needed), including > 32k hit pixels (OpenCV remap limit regression)."""

from __future__ import annotations

import numpy as np
import pytest
import trimesh
from PIL import Image

from recon.render import TexturedMesh, psnr


@pytest.mark.light
def test_head_on_quad_reproduces_texture(tmp_path):
    gx, gy = np.meshgrid(np.linspace(0, 1, 128), np.linspace(0, 1, 128))
    tex = np.stack([gx, gy, 0.5 + 0.5 * np.sin(6 * gx + 4 * gy)], -1)
    tex = (tex * 255).astype(np.uint8)  # smooth, asymmetric: flips are distinguishable
    V = np.array([[-1, -1, 2], [1, -1, 2], [1, 1, 2], [-1, 1, 2]], float)
    UV = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], float)  # v up; image row 0 = v 1
    mesh = trimesh.Trimesh(V, [[0, 1, 2], [0, 2, 3]], process=False)
    mesh.visual = trimesh.visual.TextureVisuals(uv=UV, image=Image.fromarray(tex))
    mesh.export(str(tmp_path / "q.obj"))  # OBJ + MTL + texture: what OpenMVS TextureMesh writes
    W = H = 256  # 65k pixels: more than the 32767 remap limit
    f = 260.0  # quad edge at +-1 m maps just outside the image border: every pixel hits
    K = np.array([[f, 0, W / 2 - 0.5], [0, f, H / 2 - 0.5], [0, 0, 1]])
    img, hit, depth = TexturedMesh(tmp_path / "q.obj").render(K, np.eye(4), W, H)
    assert hit.all() and np.allclose(depth, 2.0, atol=1e-4)
    ref = np.asarray(Image.fromarray(tex).resize((W, H), Image.BILINEAR))
    # camera x right / y down; uv v points up: the render is the texture flipped vertically
    scores = {
        "none": psnr(img, ref),
        "vflip": psnr(img, ref[::-1]),
        "hflip": psnr(img, ref[:, ::-1]),
        "both": psnr(img, ref[::-1, ::-1]),
    }
    assert max(scores, key=scores.get) == "vflip" and scores["vflip"] > 35.0, scores


@pytest.mark.light
def test_glb_export_matches_openmvs_obj(tmp_path):
    """An OpenMVS-style OBJ (v measured from the top of the atlas) exported to GLB must render the
    same as the OBJ: the GLB is what viewers (three.js, Blender, Quick Look) will show."""
    from recon.dense import export_glb

    gx, gy = np.meshgrid(np.linspace(0, 1, 128), np.linspace(0, 1, 128))
    tex = (np.stack([gx, gy, 0.5 + 0.5 * np.sin(6 * gx + 4 * gy)], -1) * 255).astype(np.uint8)
    V = np.array([[-1, -1, 2], [1, -1, 2], [1, 1, 2], [-1, 1, 2]], float)
    # v-down convention: texture row = v * height; the quad's top edge (y = -1) shows row 0
    UV = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], float)
    mesh = trimesh.Trimesh(V, [[0, 1, 2], [0, 2, 3]], process=False)
    mesh.visual = trimesh.visual.TextureVisuals(uv=UV, image=Image.fromarray(tex))
    mesh.export(str(tmp_path / "m.obj"))
    ply = tmp_path / "m.ply"
    mesh.export(str(ply))
    assert export_glb(tmp_path / "m.obj", ply, tmp_path / "m.glb") == "textured"
    W = H = 256
    K = np.array([[260.0, 0, W / 2 - 0.5], [0, 260.0, H / 2 - 0.5], [0, 0, 1]])
    from_obj, h1, _ = TexturedMesh(tmp_path / "m.obj", v_up=False).render(K, np.eye(4), W, H)
    from_glb, h2, _ = TexturedMesh(tmp_path / "m.glb", v_up=True).render(K, np.eye(4), W, H)
    assert h1.all() and h2.all()
    assert psnr(from_obj, from_glb) > 35.0
    # and the content is the texture upright (not flipped): top rows of the render = top of atlas
    ref = np.asarray(Image.fromarray(tex).resize((W, H), Image.BILINEAR))
    assert psnr(from_obj, ref) > 35.0 and psnr(from_obj, ref[::-1]) < 20.0
