"""Utility metrics and packing helpers for standalone denoiser module tests."""

from __future__ import annotations

import numpy as np

REBLUR_HIT_DISTANCE_PARAMS = np.array(
    [3.0, 0.1, 20.0, -25.0], dtype=np.float32)


def _safe_normalize(vectors: np.ndarray) -> np.ndarray:
    lengths = np.linalg.norm(vectors, axis=-1, keepdims=True)
    lengths = np.maximum(lengths, 1e-12)
    return vectors / lengths


def pack_normal_roughness(normals: np.ndarray, roughness: np.ndarray) -> np.ndarray:
    safe_normals = _safe_normalize(normals)
    max_abs = np.max(np.abs(safe_normals), axis=-1, keepdims=True)
    max_abs = np.maximum(max_abs, 1e-12)
    encoded_normals = safe_normals / max_abs
    packed = np.concatenate(
        (encoded_normals * 0.5 + 0.5, np.clip(roughness, 0.0, 1.0)[..., None]),
        axis=-1,
    )
    return packed.astype(np.float32, copy=False)


def unpack_normal_roughness(packed: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    encoded_normals = packed[..., :3] * 2.0 - 1.0
    normals = _safe_normalize(encoded_normals)
    roughness = np.clip(packed[..., 3], 0.0, 1.0)
    return normals.astype(np.float32, copy=False), roughness.astype(np.float32, copy=False)


def linear_to_ycocg(radiance: np.ndarray) -> np.ndarray:
    y = np.dot(radiance, np.array([0.25, 0.5, 0.25], dtype=np.float32))
    co = np.dot(radiance, np.array([0.5, 0.0, -0.5], dtype=np.float32))
    cg = np.dot(radiance, np.array([-0.25, 0.5, -0.25], dtype=np.float32))
    return np.stack((y, co, cg), axis=-1).astype(np.float32, copy=False)


def ycocg_to_linear(ycocg: np.ndarray) -> np.ndarray:
    t = ycocg[..., 0] - ycocg[..., 2]
    r = t + ycocg[..., 1]
    g = ycocg[..., 0] + ycocg[..., 2]
    b = t - ycocg[..., 1]
    rgb = np.stack((r, g, b), axis=-1)
    return np.maximum(rgb, 0.0).astype(np.float32, copy=False)


def get_norm_hit_distance(
    hit_distance: np.ndarray,
    view_z: np.ndarray,
    roughness: np.ndarray,
    params: np.ndarray = REBLUR_HIT_DISTANCE_PARAMS,
) -> np.ndarray:
    roughness_factor = np.clip(
        np.exp2(params[3] * roughness * roughness), 0.0, 1.0)
    mixed_scale = 1.0 + (params[2] - 1.0) * roughness_factor
    normalization = (params[0] + np.abs(view_z) * params[1]) * mixed_scale
    normalization = np.maximum(normalization, 1e-12)
    return np.clip(hit_distance / normalization, 0.0, 1.0).astype(np.float32, copy=False)


def pack_radiance_and_norm_hit_dist(radiance: np.ndarray, norm_hit_distance: np.ndarray) -> np.ndarray:
    sanitized_radiance = np.where(np.isfinite(radiance), radiance, 0.0)
    sanitized_radiance = np.maximum(sanitized_radiance, 0.0)
    sanitized_norm_hit = np.where(np.isfinite(
        norm_hit_distance), norm_hit_distance, 0.0)
    sanitized_norm_hit = np.clip(sanitized_norm_hit, 0.0, 1.0)
    ycocg = linear_to_ycocg(sanitized_radiance)
    return np.concatenate((ycocg, sanitized_norm_hit[..., None]), axis=-1).astype(np.float32, copy=False)


def unpack_radiance_and_norm_hit_dist(packed: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    radiance = ycocg_to_linear(packed[..., :3])
    norm_hit_distance = packed[..., 3]
    return radiance.astype(np.float32, copy=False), norm_hit_distance.astype(np.float32, copy=False)


def project_world_to_pixel(
    world_positions: np.ndarray,
    view_projection: np.ndarray,
    width: int,
    height: int,
) -> np.ndarray:
    homogeneous = np.concatenate((world_positions, np.ones(
        (world_positions.shape[0], 1), dtype=np.float32)), axis=-1)
    clip = (view_projection @ homogeneous.T).T
    w = clip[:, 3:4]
    safe_w = np.where(np.abs(w) < 1e-12, np.where(w < 0.0, -1e-12, 1e-12), w)
    ndc = clip[:, :3] / safe_w
    uv = ndc[:, :2] * 0.5 + 0.5
    uv[:, 1] = 1.0 - uv[:, 1]
    pixels = uv * np.array([width, height], dtype=np.float32)
    return pixels.astype(np.float32, copy=False)


def compute_motion_vectors_pixels(
    world_positions: np.ndarray,
    current_view_projection: np.ndarray,
    previous_view_projection: np.ndarray,
    width: int,
    height: int,
    has_history: bool,
) -> np.ndarray:
    if not has_history:
        return np.zeros((world_positions.shape[0], 2), dtype=np.float32)

    current_pixels = project_world_to_pixel(
        world_positions, current_view_projection, width, height)
    previous_pixels = project_world_to_pixel(
        world_positions, previous_view_projection, width, height)
    return (previous_pixels - current_pixels).astype(np.float32, copy=False)


def rmse(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.sqrt(np.mean((a - b) ** 2)))
