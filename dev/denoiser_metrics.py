"""Utility metrics and packing helpers for standalone denoiser module tests."""

from __future__ import annotations

import hashlib

import numpy as np

REBLUR_HIT_DISTANCE_PARAMS = np.array(
    [3.0, 0.1, 20.0, -25.0], dtype=np.float32)
REBLUR_TILE_SIZE = 16
REBLUR_HIT_DIST_RECONSTRUCTION_OFF = 0
REBLUR_HIT_DIST_RECONSTRUCTION_AREA_3X3 = 1
REBLUR_HIT_DIST_RECONSTRUCTION_AREA_5X5 = 2


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


def _is_denoisable_view_z(view_z: np.ndarray, denoising_range: float) -> np.ndarray:
    finite_mask = np.isfinite(view_z)
    return finite_mask & (view_z > 0.0) & (view_z <= denoising_range)


def classify_tiles_reference(
    view_z: np.ndarray,
    denoising_range: float,
    tile_size: int = REBLUR_TILE_SIZE,
) -> np.ndarray:
    if view_z.ndim != 2:
        raise ValueError("view_z must be a 2D array")

    height, width = view_z.shape
    tile_width = (width + tile_size - 1) // tile_size
    tile_height = (height + tile_size - 1) // tile_size
    tiles = np.zeros((tile_height, tile_width), dtype=np.uint32)

    for tile_y in range(tile_height):
        y0 = tile_y * tile_size
        y1 = min(y0 + tile_size, height)
        for tile_x in range(tile_width):
            x0 = tile_x * tile_size
            x1 = min(x0 + tile_size, width)
            tile_view_z = view_z[y0:y1, x0:x1]
            denoisable_mask = _is_denoisable_view_z(
                tile_view_z, denoising_range)
            is_sky_tile = not np.any(denoisable_mask)
            tiles[tile_y, tile_x] = 1 if is_sky_tile else 0

    return tiles


def classify_tiles_shader_equivalent(
    view_z: np.ndarray,
    denoising_range: float,
) -> np.ndarray:
    if view_z.ndim != 2:
        raise ValueError("view_z must be a 2D array")

    height, width = view_z.shape
    tile_width = (width + REBLUR_TILE_SIZE - 1) // REBLUR_TILE_SIZE
    tile_height = (height + REBLUR_TILE_SIZE - 1) // REBLUR_TILE_SIZE
    tiles = np.zeros((tile_height, tile_width), dtype=np.uint32)

    for tile_y in range(tile_height):
        for tile_x in range(tile_width):
            tile_origin_x = tile_x * REBLUR_TILE_SIZE
            tile_origin_y = tile_y * REBLUR_TILE_SIZE

            invalid_count = 0
            sample_count = 0
            for thread_y in range(4):
                for thread_x in range(8):
                    base_x = tile_origin_x + thread_x * 2
                    base_y = tile_origin_y + thread_y * 4
                    for sample_x in range(2):
                        for sample_y in range(4):
                            pixel_x = base_x + sample_x
                            pixel_y = base_y + sample_y
                            if pixel_x >= width or pixel_y >= height:
                                continue

                            sample_count += 1
                            value = view_z[pixel_y, pixel_x]
                            if not np.isfinite(value) or value <= 0.0 or value > denoising_range:
                                invalid_count += 1

            tiles[tile_y, tile_x] = 1 if sample_count > 0 and invalid_count == sample_count else 0

    return tiles


def binary_precision_recall(reference_positive_mask: np.ndarray, predicted_positive_mask: np.ndarray) -> tuple[float, float]:
    if reference_positive_mask.shape != predicted_positive_mask.shape:
        raise ValueError(
            "reference and prediction masks must have the same shape")

    reference_positive = reference_positive_mask.astype(bool, copy=False)
    predicted_positive = predicted_positive_mask.astype(bool, copy=False)

    true_positive = np.logical_and(
        reference_positive, predicted_positive).sum(dtype=np.int64)
    false_positive = np.logical_and(
        ~reference_positive, predicted_positive).sum(dtype=np.int64)
    false_negative = np.logical_and(
        reference_positive, ~predicted_positive).sum(dtype=np.int64)

    precision = float(true_positive / (true_positive + false_positive)
                      ) if (true_positive + false_positive) > 0 else 1.0
    recall = float(true_positive / (true_positive + false_negative)
                   ) if (true_positive + false_negative) > 0 else 1.0
    return precision, recall


def hash_tile_mask(tile_mask: np.ndarray) -> str:
    return hashlib.sha256(np.ascontiguousarray(tile_mask).tobytes()).hexdigest()


def _unpack_normal_from_packed_normal_roughness(packed_normal_roughness: np.ndarray) -> np.ndarray:
    unpacked = packed_normal_roughness[..., :3] * 2.0 - 1.0
    return _safe_normalize(unpacked)


def _compute_hit_dist_neighbor_weight(
    center_x: int,
    center_y: int,
    neighbor_x: int,
    neighbor_y: int,
    view_z: np.ndarray,
    normals: np.ndarray,
) -> float:
    center_view_z = float(view_z[center_y, center_x])
    neighbor_view_z = float(view_z[neighbor_y, neighbor_x])
    if not np.isfinite(neighbor_view_z) or neighbor_view_z <= 0.0:
        return 0.0

    center_normal = normals[center_y, center_x]
    neighbor_normal = normals[neighbor_y, neighbor_x]

    offset_x = float(neighbor_x - center_x)
    offset_y = float(neighbor_y - center_y)
    distance_squared = offset_x * offset_x + offset_y * offset_y
    distance_weight = float(np.exp(-distance_squared / 2.0))

    relative_depth_delta = abs(
        neighbor_view_z - center_view_z) / max(center_view_z, 1e-6)
    depth_weight = float(np.exp(-relative_depth_delta * 16.0))

    normal_cos = float(
        np.clip(np.dot(center_normal, neighbor_normal), 0.0, 1.0))
    normal_weight = normal_cos ** 8.0

    return distance_weight * depth_weight * normal_weight


def reconstruct_hit_distance_shader_equivalent(
    tile_mask: np.ndarray,
    packed_normal_roughness: np.ndarray,
    view_z: np.ndarray,
    diff_signal: np.ndarray,
    spec_signal: np.ndarray,
    denoising_range: float,
    mode: int,
) -> tuple[np.ndarray, np.ndarray]:
    if mode == REBLUR_HIT_DIST_RECONSTRUCTION_OFF:
        return diff_signal.copy(), spec_signal.copy()

    if mode not in (
        REBLUR_HIT_DIST_RECONSTRUCTION_AREA_3X3,
        REBLUR_HIT_DIST_RECONSTRUCTION_AREA_5X5,
    ):
        raise ValueError(
            f"Unsupported hit-distance reconstruction mode: {mode}")

    if view_z.ndim != 2:
        raise ValueError("view_z must be a 2D array")
    if tile_mask.ndim != 2:
        raise ValueError("tile_mask must be a 2D array")
    if diff_signal.shape != spec_signal.shape or diff_signal.shape[-1] != 4:
        raise ValueError("diff/spec signals must have matching HxWx4 shapes")
    if packed_normal_roughness.shape[:2] != view_z.shape:
        raise ValueError(
            "packed_normal_roughness shape must match view_z shape")
    if diff_signal.shape[:2] != view_z.shape:
        raise ValueError("diff/spec shapes must match view_z shape")

    height, width = view_z.shape
    radius = 1 if mode == REBLUR_HIT_DIST_RECONSTRUCTION_AREA_3X3 else 2

    out_diff = diff_signal.copy()
    out_spec = spec_signal.copy()
    normals = _unpack_normal_from_packed_normal_roughness(
        packed_normal_roughness)

    for y in range(height):
        for x in range(width):
            tile_x = x // REBLUR_TILE_SIZE
            tile_y = y // REBLUR_TILE_SIZE
            if tile_mask[tile_y, tile_x] != 0:
                continue

            center_view_z = view_z[y, x]
            if not np.isfinite(center_view_z) or center_view_z <= 0.0 or center_view_z > denoising_range:
                continue

            diff_center = diff_signal[y, x, 3]
            spec_center = spec_signal[y, x, 3]
            diff_valid = bool(np.isfinite(diff_center)
                              and diff_center > 0.0 and diff_center <= 1.0)
            spec_valid = bool(np.isfinite(spec_center)
                              and spec_center > 0.0 and spec_center <= 1.0)

            if not diff_valid:
                weighted_sum = 0.0
                weight_sum = 0.0
                for offset_y in range(-radius, radius + 1):
                    for offset_x in range(-radius, radius + 1):
                        if offset_x == 0 and offset_y == 0:
                            continue
                        nx = x + offset_x
                        ny = y + offset_y
                        if nx < 0 or ny < 0 or nx >= width or ny >= height:
                            continue
                        neighbor_value = diff_signal[ny, nx, 3]
                        if not np.isfinite(neighbor_value) or neighbor_value <= 0.0 or neighbor_value > 1.0:
                            continue
                        neighbor_view_z = view_z[ny, nx]
                        if (
                            not np.isfinite(neighbor_view_z)
                            or neighbor_view_z <= 0.0
                            or neighbor_view_z > denoising_range
                        ):
                            continue
                        weight = _compute_hit_dist_neighbor_weight(
                            x, y, nx, ny, view_z, normals)
                        if weight <= 0.0:
                            continue
                        weighted_sum += float(neighbor_value) * weight
                        weight_sum += weight
                out_diff[y, x, 3] = np.float32(
                    weighted_sum / weight_sum) if weight_sum > 0.0 else np.float32(0.0)

            if not spec_valid:
                weighted_sum = 0.0
                weight_sum = 0.0
                for offset_y in range(-radius, radius + 1):
                    for offset_x in range(-radius, radius + 1):
                        if offset_x == 0 and offset_y == 0:
                            continue
                        nx = x + offset_x
                        ny = y + offset_y
                        if nx < 0 or ny < 0 or nx >= width or ny >= height:
                            continue
                        neighbor_value = spec_signal[ny, nx, 3]
                        if not np.isfinite(neighbor_value) or neighbor_value <= 0.0 or neighbor_value > 1.0:
                            continue
                        neighbor_view_z = view_z[ny, nx]
                        if (
                            not np.isfinite(neighbor_view_z)
                            or neighbor_view_z <= 0.0
                            or neighbor_view_z > denoising_range
                        ):
                            continue
                        weight = _compute_hit_dist_neighbor_weight(
                            x, y, nx, ny, view_z, normals)
                        if weight <= 0.0:
                            continue
                        weighted_sum += float(neighbor_value) * weight
                        weight_sum += weight
                out_spec[y, x, 3] = np.float32(
                    weighted_sum / weight_sum) if weight_sum > 0.0 else np.float32(0.0)

    return out_diff, out_spec


def normalized_rmse(a: np.ndarray, b: np.ndarray, value_range: float) -> float:
    if value_range <= 0.0:
        raise ValueError("value_range must be positive")
    return rmse(a, b) / value_range


def rmse(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.sqrt(np.mean((a - b) ** 2)))
