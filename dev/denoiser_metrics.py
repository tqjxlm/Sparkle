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
REBLUR_PREPASS_MAX_RADIUS = 4
REBLUR_BLUR_MAX_RADIUS = 8
REBLUR_TEMPORAL_DISOCCLUSION_THRESHOLD = 0.10
REBLUR_TEMPORAL_NORMAL_REJECT_COS = 0.85
REBLUR_TEMPORAL_FAST_HISTORY_MAX_FRAMES = 4


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


def _compute_prepass_neighbor_weight(
    center_x: int,
    center_y: int,
    neighbor_x: int,
    neighbor_y: int,
    view_z: np.ndarray,
    normals: np.ndarray,
    roughness: np.ndarray,
    radius: float,
    use_roughness_weight: bool,
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
    sigma = max(radius * 0.75, 1.0)
    distance_weight = float(np.exp(-distance_squared / (2.0 * sigma * sigma)))

    relative_depth_delta = abs(
        neighbor_view_z - center_view_z) / max(center_view_z, 1e-6)
    depth_weight = float(np.exp(-relative_depth_delta * 24.0))

    normal_cos = float(
        np.clip(np.dot(center_normal, neighbor_normal), 0.0, 1.0))
    normal_weight = normal_cos ** 16.0

    roughness_weight = 1.0
    if use_roughness_weight:
        roughness_delta = abs(float(roughness[neighbor_y, neighbor_x]) - float(
            roughness[center_y, center_x]))
        roughness_weight = float(np.exp(-roughness_delta * 12.0))

    return distance_weight * depth_weight * normal_weight * roughness_weight


def _is_valid_prepass_view_z(view_z: float, denoising_range: float) -> bool:
    return np.isfinite(view_z) and view_z > 0.0 and view_z <= denoising_range


def prepass_shader_equivalent(
    tile_mask: np.ndarray,
    packed_normal_roughness: np.ndarray,
    view_z: np.ndarray,
    diff_signal: np.ndarray,
    spec_signal: np.ndarray,
    denoising_range: float,
    diffuse_radius: float,
    specular_radius: float,
    spec_tracking_radius: float,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
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
    normals = _unpack_normal_from_packed_normal_roughness(
        packed_normal_roughness)
    roughness = np.clip(packed_normal_roughness[..., 3], 0.0, 1.0)

    diff_radius_i = int(np.clip(
        np.rint(diffuse_radius), 0, REBLUR_PREPASS_MAX_RADIUS))
    spec_radius_i = int(np.clip(
        np.rint(specular_radius), 0, REBLUR_PREPASS_MAX_RADIUS))
    tracking_radius_i = int(np.clip(
        np.rint(spec_tracking_radius), 0, REBLUR_PREPASS_MAX_RADIUS))

    out_diff = diff_signal.copy()
    out_spec = spec_signal.copy()
    spec_hit_dist_for_tracking = np.clip(
        spec_signal[..., 3], 0.0, 1.0).astype(np.float32, copy=True)

    for y in range(height):
        tile_y = y // REBLUR_TILE_SIZE
        for x in range(width):
            tile_x = x // REBLUR_TILE_SIZE
            if tile_mask[tile_y, tile_x] != 0:
                continue

            center_view_z = float(view_z[y, x])
            if not _is_valid_prepass_view_z(center_view_z, denoising_range):
                continue

            if diff_radius_i > 0:
                weighted_sum = np.zeros(4, dtype=np.float64)
                weight_sum = 0.0
                for offset_y in range(-diff_radius_i, diff_radius_i + 1):
                    for offset_x in range(-diff_radius_i, diff_radius_i + 1):
                        nx = x + offset_x
                        ny = y + offset_y
                        if nx < 0 or ny < 0 or nx >= width or ny >= height:
                            continue
                        neighbor_tile_x = nx // REBLUR_TILE_SIZE
                        neighbor_tile_y = ny // REBLUR_TILE_SIZE
                        if tile_mask[neighbor_tile_y, neighbor_tile_x] != 0:
                            continue
                        if not _is_valid_prepass_view_z(float(view_z[ny, nx]), denoising_range):
                            continue
                        weight = _compute_prepass_neighbor_weight(
                            x,
                            y,
                            nx,
                            ny,
                            view_z,
                            normals,
                            roughness,
                            float(max(diff_radius_i, 1)),
                            False,
                        )
                        if weight <= 0.0:
                            continue
                        weighted_sum += diff_signal[ny,
                                                    nx].astype(np.float64) * weight
                        weight_sum += weight
                if weight_sum > 0.0:
                    filtered = (weighted_sum / weight_sum).astype(np.float32)
                    filtered[3] = np.float32(np.clip(filtered[3], 0.0, 1.0))
                    out_diff[y, x] = filtered

            if spec_radius_i > 0:
                weighted_sum = np.zeros(4, dtype=np.float64)
                weight_sum = 0.0
                for offset_y in range(-spec_radius_i, spec_radius_i + 1):
                    for offset_x in range(-spec_radius_i, spec_radius_i + 1):
                        nx = x + offset_x
                        ny = y + offset_y
                        if nx < 0 or ny < 0 or nx >= width or ny >= height:
                            continue
                        neighbor_tile_x = nx // REBLUR_TILE_SIZE
                        neighbor_tile_y = ny // REBLUR_TILE_SIZE
                        if tile_mask[neighbor_tile_y, neighbor_tile_x] != 0:
                            continue
                        if not _is_valid_prepass_view_z(float(view_z[ny, nx]), denoising_range):
                            continue
                        weight = _compute_prepass_neighbor_weight(
                            x,
                            y,
                            nx,
                            ny,
                            view_z,
                            normals,
                            roughness,
                            float(max(spec_radius_i, 1)),
                            True,
                        )
                        if weight <= 0.0:
                            continue
                        weighted_sum += spec_signal[ny,
                                                    nx].astype(np.float64) * weight
                        weight_sum += weight
                if weight_sum > 0.0:
                    filtered = (weighted_sum / weight_sum).astype(np.float32)
                    filtered[3] = np.float32(np.clip(filtered[3], 0.0, 1.0))
                    out_spec[y, x] = filtered

            if tracking_radius_i > 0:
                weighted_sum = 0.0
                weight_sum = 0.0
                for offset_y in range(-tracking_radius_i, tracking_radius_i + 1):
                    for offset_x in range(-tracking_radius_i, tracking_radius_i + 1):
                        nx = x + offset_x
                        ny = y + offset_y
                        if nx < 0 or ny < 0 or nx >= width or ny >= height:
                            continue
                        neighbor_tile_x = nx // REBLUR_TILE_SIZE
                        neighbor_tile_y = ny // REBLUR_TILE_SIZE
                        if tile_mask[neighbor_tile_y, neighbor_tile_x] != 0:
                            continue
                        if not _is_valid_prepass_view_z(float(view_z[ny, nx]), denoising_range):
                            continue
                        neighbor_value = float(spec_signal[ny, nx, 3])
                        if not np.isfinite(neighbor_value):
                            continue
                        weight = _compute_prepass_neighbor_weight(
                            x,
                            y,
                            nx,
                            ny,
                            view_z,
                            normals,
                            roughness,
                            float(max(tracking_radius_i, 1)),
                            True,
                        )
                        if weight <= 0.0:
                            continue
                        weighted_sum += float(np.clip(neighbor_value,
                                              0.0, 1.0)) * weight
                        weight_sum += weight
                if weight_sum > 0.0:
                    spec_hit_dist_for_tracking[y, x] = np.float32(
                        np.clip(weighted_sum / weight_sum, 0.0, 1.0))

    return out_diff, out_spec, spec_hit_dist_for_tracking


def temporal_accumulation_shader_equivalent(
    tile_mask: np.ndarray,
    packed_normal_roughness: np.ndarray,
    view_z: np.ndarray,
    motion_vectors: np.ndarray,
    diff_signal: np.ndarray,
    spec_signal: np.ndarray,
    spec_hit_dist_for_tracking: np.ndarray,
    prev_view_z: np.ndarray,
    prev_normal_roughness: np.ndarray,
    prev_internal_data: np.ndarray,
    prev_diff_history: np.ndarray,
    prev_spec_history: np.ndarray,
    prev_diff_fast_history: np.ndarray,
    prev_spec_fast_history: np.ndarray,
    prev_spec_hit_dist_for_tracking: np.ndarray,
    denoising_range: float,
    max_history_frames: int,
    history_available: bool,
    disocclusion_threshold: float = REBLUR_TEMPORAL_DISOCCLUSION_THRESHOLD,
    normal_reject_cos: float = REBLUR_TEMPORAL_NORMAL_REJECT_COS,
    fast_history_max_frames: int = REBLUR_TEMPORAL_FAST_HISTORY_MAX_FRAMES,
) -> tuple[
    np.ndarray,
    np.ndarray,
    np.ndarray,
    np.ndarray,
    np.ndarray,
    np.ndarray,
    np.ndarray,
    np.ndarray,
]:
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
    if spec_hit_dist_for_tracking.shape != view_z.shape:
        raise ValueError(
            "spec_hit_dist_for_tracking shape must match view_z shape")
    if prev_view_z.shape != view_z.shape:
        raise ValueError("prev_view_z shape must match view_z shape")
    if prev_normal_roughness.shape != packed_normal_roughness.shape:
        raise ValueError(
            "prev_normal_roughness shape must match packed_normal_roughness")
    if prev_internal_data.shape != packed_normal_roughness.shape:
        raise ValueError("prev_internal_data shape must match HxWx4")
    if prev_diff_history.shape != diff_signal.shape:
        raise ValueError("prev_diff_history shape must match diff_signal")
    if prev_spec_history.shape != spec_signal.shape:
        raise ValueError("prev_spec_history shape must match spec_signal")
    if prev_diff_fast_history.shape != diff_signal.shape:
        raise ValueError(
            "prev_diff_fast_history shape must match diff_signal")
    if prev_spec_fast_history.shape != spec_signal.shape:
        raise ValueError(
            "prev_spec_fast_history shape must match spec_signal")
    if prev_spec_hit_dist_for_tracking.shape != view_z.shape:
        raise ValueError(
            "prev_spec_hit_dist_for_tracking shape must match view_z shape")
    if motion_vectors.shape[:2] != view_z.shape:
        raise ValueError("motion_vectors shape must match view_z shape")
    if motion_vectors.ndim != 3 or motion_vectors.shape[2] < 2:
        raise ValueError("motion_vectors must be HxWxN with N >= 2")

    height, width = view_z.shape
    max_history_frames_clamped = max(1.0, float(max_history_frames))
    max_fast_history_frames_clamped = max(1.0, float(fast_history_max_frames))
    disocclusion_threshold_clamped = max(0.0, float(disocclusion_threshold))
    normal_reject_cos_clamped = float(np.clip(normal_reject_cos, 0.0, 1.0))

    current_normals = _unpack_normal_from_packed_normal_roughness(
        packed_normal_roughness)
    previous_normals = _unpack_normal_from_packed_normal_roughness(
        prev_normal_roughness)

    out_data1 = np.zeros((height, width, 4), dtype=np.float32)
    out_data2 = np.zeros((height, width, 4), dtype=np.float32)
    out_internal_data = np.zeros((height, width, 4), dtype=np.float32)
    out_diff_history = diff_signal.astype(np.float32, copy=True)
    out_spec_history = spec_signal.astype(np.float32, copy=True)
    out_diff_fast_history = diff_signal.astype(np.float32, copy=True)
    out_spec_fast_history = spec_signal.astype(np.float32, copy=True)
    out_spec_hit_dist_for_tracking = np.clip(
        spec_hit_dist_for_tracking, 0.0, 1.0).astype(np.float32, copy=True)

    for y in range(height):
        tile_y = y // REBLUR_TILE_SIZE
        for x in range(width):
            tile_x = x // REBLUR_TILE_SIZE
            current_view_z = float(view_z[y, x])
            current_tracking = float(
                np.clip(spec_hit_dist_for_tracking[y, x], 0.0, 1.0))
            if tile_mask[tile_y, tile_x] != 0 or not _is_valid_prepass_view_z(current_view_z, denoising_range):
                out_data1[y, x, 3] = np.float32(current_tracking)
                out_data2[y, x, 3] = np.float32(1.0)
                continue

            valid_reprojection = False
            depth_delta_norm = 1.0
            prev_x = x
            prev_y = y
            if history_available:
                motion = motion_vectors[y, x, :2]
                reproj_x = int(np.rint(float(x) + float(motion[0])))
                reproj_y = int(np.rint(float(y) + float(motion[1])))
                if 0 <= reproj_x < width and 0 <= reproj_y < height:
                    prev_x = reproj_x
                    prev_y = reproj_y
                    previous_view_z = float(prev_view_z[prev_y, prev_x])
                    if _is_valid_prepass_view_z(previous_view_z, denoising_range):
                        depth_delta_norm = abs(
                            previous_view_z - current_view_z) / max(current_view_z, 1e-6)
                        normal_cos = float(np.clip(np.dot(current_normals[y, x], previous_normals[prev_y, prev_x]),
                                                   0.0, 1.0))
                        valid_reprojection = (
                            depth_delta_norm <= disocclusion_threshold_clamped
                            and normal_cos >= normal_reject_cos_clamped
                        )

            history_length = 1.0
            fast_history_length = 1.0
            prev_diff_value = diff_signal[y, x]
            prev_spec_value = spec_signal[y, x]
            prev_diff_fast_value = diff_signal[y, x]
            prev_spec_fast_value = spec_signal[y, x]
            previous_tracking = current_tracking
            if valid_reprojection:
                prev_internal = prev_internal_data[prev_y, prev_x]
                history_length = float(np.clip(
                    float(prev_internal[0]) + 1.0, 1.0, max_history_frames_clamped))
                fast_history_length = float(np.clip(
                    float(prev_internal[1]) + 1.0, 1.0, max_fast_history_frames_clamped))
                prev_diff_value = prev_diff_history[prev_y, prev_x]
                prev_spec_value = prev_spec_history[prev_y, prev_x]
                prev_diff_fast_value = prev_diff_fast_history[prev_y, prev_x]
                prev_spec_fast_value = prev_spec_fast_history[prev_y, prev_x]
                previous_tracking = float(np.clip(
                    prev_spec_hit_dist_for_tracking[prev_y, prev_x], 0.0, 1.0))

            history_alpha = 1.0 / \
                max(history_length, 1.0) if valid_reprojection else 1.0
            fast_history_alpha = 1.0 / \
                max(fast_history_length, 1.0) if valid_reprojection else 1.0

            accumulated_diff = prev_diff_value * \
                (1.0 - history_alpha) + diff_signal[y, x] * history_alpha
            accumulated_spec = prev_spec_value * \
                (1.0 - history_alpha) + spec_signal[y, x] * history_alpha
            accumulated_diff_fast = prev_diff_fast_value * \
                (1.0 - fast_history_alpha) + \
                diff_signal[y, x] * fast_history_alpha
            accumulated_spec_fast = prev_spec_fast_value * \
                (1.0 - fast_history_alpha) + \
                spec_signal[y, x] * fast_history_alpha
            accumulated_tracking = previous_tracking * \
                (1.0 - fast_history_alpha) + \
                current_tracking * fast_history_alpha

            accumulated_diff = accumulated_diff.astype(np.float32, copy=False)
            accumulated_spec = accumulated_spec.astype(np.float32, copy=False)
            accumulated_diff_fast = accumulated_diff_fast.astype(
                np.float32, copy=False)
            accumulated_spec_fast = accumulated_spec_fast.astype(
                np.float32, copy=False)
            accumulated_diff[3] = np.float32(
                np.clip(accumulated_diff[3], 0.0, 1.0))
            accumulated_spec[3] = np.float32(
                np.clip(accumulated_spec[3], 0.0, 1.0))
            accumulated_diff_fast[3] = np.float32(
                np.clip(accumulated_diff_fast[3], 0.0, 1.0))
            accumulated_spec_fast[3] = np.float32(
                np.clip(accumulated_spec_fast[3], 0.0, 1.0))
            accumulated_tracking = float(
                np.clip(accumulated_tracking, 0.0, 1.0))

            out_diff_history[y, x] = accumulated_diff
            out_spec_history[y, x] = accumulated_spec
            out_diff_fast_history[y, x] = accumulated_diff_fast
            out_spec_fast_history[y, x] = accumulated_spec_fast
            out_spec_hit_dist_for_tracking[y, x] = np.float32(
                accumulated_tracking)

            history_denom = max(max_history_frames_clamped - 1.0, 1.0)
            history_factor = float(
                np.clip((history_length - 1.0) / history_denom, 0.0, 1.0))
            if valid_reprojection:
                prev_uv_x = (float(prev_x) + 0.5) / float(width)
                prev_uv_y = (float(prev_y) + 0.5) / float(height)
            else:
                prev_uv_x = 0.0
                prev_uv_y = 0.0

            out_data1[y, x, 0] = np.float32(history_factor)
            out_data1[y, x, 1] = np.float32(1.0 if valid_reprojection else 0.0)
            out_data1[y, x, 2] = np.float32(
                history_length / max_history_frames_clamped)
            out_data1[y, x, 3] = np.float32(accumulated_tracking)
            out_data2[y, x, 0] = np.float32(prev_uv_x)
            out_data2[y, x, 1] = np.float32(prev_uv_y)
            out_data2[y, x, 2] = np.float32(
                np.clip(depth_delta_norm, 0.0, 1.0))
            out_data2[y, x, 3] = np.float32(0.0 if valid_reprojection else 1.0)
            out_internal_data[y, x, 0] = np.float32(history_length)
            out_internal_data[y, x, 1] = np.float32(fast_history_length)
            out_internal_data[y, x, 2] = np.float32(
                1.0 if valid_reprojection else 0.0)
            out_internal_data[y, x, 3] = np.float32(0.0)

    return (
        out_data1,
        out_data2,
        out_internal_data,
        out_diff_history,
        out_spec_history,
        out_diff_fast_history,
        out_spec_fast_history,
        out_spec_hit_dist_for_tracking,
    )


def _compute_blur_neighbor_weight(
    center_x: int,
    center_y: int,
    neighbor_x: int,
    neighbor_y: int,
    view_z: np.ndarray,
    normals: np.ndarray,
    roughness: np.ndarray,
    center_signal: np.ndarray,
    neighbor_signal: np.ndarray,
    radius: float,
    use_roughness_weight: bool,
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
    sigma = max(radius * 0.75, 1.0)
    distance_weight = float(np.exp(-distance_squared / (2.0 * sigma * sigma)))

    relative_depth_delta = abs(
        neighbor_view_z - center_view_z) / max(center_view_z, 1e-6)
    depth_weight = float(np.exp(-relative_depth_delta * 24.0))

    normal_cos = float(
        np.clip(np.dot(center_normal, neighbor_normal), 0.0, 1.0))
    normal_weight = normal_cos ** 16.0

    hit_distance_delta = abs(float(np.clip(center_signal[3], 0.0, 1.0)) -
                             float(np.clip(neighbor_signal[3], 0.0, 1.0)))
    hit_distance_weight = float(np.exp(-hit_distance_delta * 14.0))

    roughness_weight = 1.0
    if use_roughness_weight:
        roughness_delta = abs(float(roughness[neighbor_y, neighbor_x]) - float(
            roughness[center_y, center_x]))
        roughness_weight = float(np.exp(-roughness_delta * 8.0))

    return distance_weight * depth_weight * normal_weight * hit_distance_weight * roughness_weight


def _compute_blur_effective_radius(
    center_signal: np.ndarray,
    center_roughness: float,
    history_factor: float,
    min_blur_radius: float,
    max_blur_radius: float,
    is_specular: bool,
) -> float:
    min_radius = max(float(min_blur_radius), 0.0)
    max_radius = max(float(max_blur_radius), min_radius)
    normalized_history = float(np.clip(history_factor, 0.0, 1.0))

    hit_distance_factor = float(np.sqrt(np.clip(center_signal[3], 0.0, 1.0)))
    convergence_factor = 1.0 - normalized_history
    roughness_factor = float(
        np.clip(0.2 + 0.8 * center_roughness, 0.0, 1.0)) if is_specular else 1.0
    blend = float(np.clip(hit_distance_factor *
                  roughness_factor * convergence_factor, 0.0, 1.0))

    return float(np.clip(min_radius + (max_radius - min_radius) * blend, min_radius, max_radius))


def blur_shader_equivalent(
    tile_mask: np.ndarray,
    packed_normal_roughness: np.ndarray,
    view_z: np.ndarray,
    diff_signal: np.ndarray,
    spec_signal: np.ndarray,
    denoising_range: float,
    min_blur_radius: float,
    max_blur_radius: float,
    history_factor: float,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
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
    normals = _unpack_normal_from_packed_normal_roughness(
        packed_normal_roughness)
    roughness = np.clip(packed_normal_roughness[..., 3], 0.0, 1.0)

    out_prev_view_z = view_z.astype(np.float32, copy=True)
    out_diff = diff_signal.copy()
    out_spec = spec_signal.copy()
    diff_effective_radius = np.zeros((height, width), dtype=np.float32)
    spec_effective_radius = np.zeros((height, width), dtype=np.float32)

    clamped_min_radius = float(
        np.clip(min_blur_radius, 0.0, REBLUR_BLUR_MAX_RADIUS))
    clamped_max_radius = float(
        np.clip(max_blur_radius, 0.0, REBLUR_BLUR_MAX_RADIUS))
    if clamped_max_radius < clamped_min_radius:
        clamped_max_radius = clamped_min_radius

    for y in range(height):
        tile_y = y // REBLUR_TILE_SIZE
        for x in range(width):
            tile_x = x // REBLUR_TILE_SIZE
            if tile_mask[tile_y, tile_x] != 0:
                continue

            center_view_z = float(view_z[y, x])
            if not _is_valid_prepass_view_z(center_view_z, denoising_range):
                continue

            center_diff = diff_signal[y, x]
            center_spec = spec_signal[y, x]
            center_roughness = float(roughness[y, x])

            diff_radius = _compute_blur_effective_radius(
                center_diff,
                center_roughness,
                history_factor,
                clamped_min_radius,
                clamped_max_radius,
                False,
            )
            spec_radius = _compute_blur_effective_radius(
                center_spec,
                center_roughness,
                history_factor,
                clamped_min_radius,
                clamped_max_radius,
                True,
            )
            diff_effective_radius[y, x] = np.float32(diff_radius)
            spec_effective_radius[y, x] = np.float32(spec_radius)

            diff_radius_i = int(
                np.clip(np.rint(diff_radius), 0, REBLUR_BLUR_MAX_RADIUS))
            spec_radius_i = int(
                np.clip(np.rint(spec_radius), 0, REBLUR_BLUR_MAX_RADIUS))

            if diff_radius_i > 0:
                weighted_sum = np.zeros(4, dtype=np.float64)
                weight_sum = 0.0
                for offset_y in range(-diff_radius_i, diff_radius_i + 1):
                    for offset_x in range(-diff_radius_i, diff_radius_i + 1):
                        nx = x + offset_x
                        ny = y + offset_y
                        if nx < 0 or ny < 0 or nx >= width or ny >= height:
                            continue
                        neighbor_tile_x = nx // REBLUR_TILE_SIZE
                        neighbor_tile_y = ny // REBLUR_TILE_SIZE
                        if tile_mask[neighbor_tile_y, neighbor_tile_x] != 0:
                            continue
                        if not _is_valid_prepass_view_z(float(view_z[ny, nx]), denoising_range):
                            continue
                        neighbor_diff = diff_signal[ny, nx]
                        weight = _compute_blur_neighbor_weight(
                            x,
                            y,
                            nx,
                            ny,
                            view_z,
                            normals,
                            roughness,
                            center_diff,
                            neighbor_diff,
                            float(max(diff_radius_i, 1)),
                            False,
                        )
                        if weight <= 0.0:
                            continue
                        weighted_sum += neighbor_diff.astype(
                            np.float64) * weight
                        weight_sum += weight
                if weight_sum > 0.0:
                    filtered = (weighted_sum / weight_sum).astype(np.float32)
                    filtered[3] = np.float32(np.clip(filtered[3], 0.0, 1.0))
                    out_diff[y, x] = filtered

            if spec_radius_i > 0:
                weighted_sum = np.zeros(4, dtype=np.float64)
                weight_sum = 0.0
                for offset_y in range(-spec_radius_i, spec_radius_i + 1):
                    for offset_x in range(-spec_radius_i, spec_radius_i + 1):
                        nx = x + offset_x
                        ny = y + offset_y
                        if nx < 0 or ny < 0 or nx >= width or ny >= height:
                            continue
                        neighbor_tile_x = nx // REBLUR_TILE_SIZE
                        neighbor_tile_y = ny // REBLUR_TILE_SIZE
                        if tile_mask[neighbor_tile_y, neighbor_tile_x] != 0:
                            continue
                        if not _is_valid_prepass_view_z(float(view_z[ny, nx]), denoising_range):
                            continue
                        neighbor_spec = spec_signal[ny, nx]
                        weight = _compute_blur_neighbor_weight(
                            x,
                            y,
                            nx,
                            ny,
                            view_z,
                            normals,
                            roughness,
                            center_spec,
                            neighbor_spec,
                            float(max(spec_radius_i, 1)),
                            True,
                        )
                        if weight <= 0.0:
                            continue
                        weighted_sum += neighbor_spec.astype(
                            np.float64) * weight
                        weight_sum += weight
                if weight_sum > 0.0:
                    filtered = (weighted_sum / weight_sum).astype(np.float32)
                    filtered[3] = np.float32(np.clip(filtered[3], 0.0, 1.0))
                    out_spec[y, x] = filtered

    return out_diff, out_spec, out_prev_view_z, diff_effective_radius, spec_effective_radius


def normalized_rmse(a: np.ndarray, b: np.ndarray, value_range: float) -> float:
    if value_range <= 0.0:
        raise ValueError("value_range must be positive")
    return rmse(a, b) / value_range


def rmse(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.sqrt(np.mean((a - b) ** 2)))
