"""Deterministic module-level tests for standalone ReBLUR integration."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass

import numpy as np

from denoiser_metrics import (
    REBLUR_HIT_DIST_RECONSTRUCTION_AREA_3X3,
    REBLUR_HIT_DIST_RECONSTRUCTION_AREA_5X5,
    REBLUR_TILE_SIZE,
    binary_precision_recall,
    blur_shader_equivalent,
    classify_tiles_reference,
    classify_tiles_shader_equivalent,
    compute_motion_vectors_pixels,
    get_norm_hit_distance,
    hash_tile_mask,
    history_fix_shader_equivalent,
    normalized_rmse,
    pack_normal_roughness,
    pack_radiance_and_norm_hit_dist,
    post_blur_shader_equivalent,
    prepass_shader_equivalent,
    project_world_to_pixel,
    reconstruct_hit_distance_shader_equivalent,
    rmse,
    split_screen_shader_equivalent,
    temporal_stabilization_shader_equivalent,
    temporal_accumulation_shader_equivalent,
    unpack_normal_roughness,
    unpack_radiance_and_norm_hit_dist,
    validation_shader_equivalent,
)


@dataclass
class ModuleATestResults:
    a1_pass: bool
    a1_normal_rmse: float
    a1_roughness_rmse: float
    a1_radiance_rmse: float
    a1_norm_hit_rmse: float
    a2_pass: bool
    a2_mean_reprojection_error_px: float
    a2_max_reprojection_error_px: float
    a3_pass: bool
    a3_guide_valid_ratio: float

    @property
    def passed(self) -> bool:
        return self.a1_pass and self.a2_pass and self.a3_pass


@dataclass
class ModuleBTestResults:
    b1_pass: bool
    b1_precision: float
    b1_recall: float
    b2_pass: bool
    b2_unique_hash_count: int
    b2_reference_hash: str

    @property
    def passed(self) -> bool:
        return self.b1_pass and self.b2_pass


@dataclass
class ModuleCTestResults:
    c1_pass: bool
    c1_invalid_rmse_norm: float
    c2_pass: bool
    c2_valid_luma_abs_error: float
    c3_pass: bool
    c3_rmse_3x3_norm: float
    c3_rmse_5x5_norm: float

    @property
    def passed(self) -> bool:
        return self.c1_pass and self.c2_pass and self.c3_pass


@dataclass
class ModuleDTestResults:
    d1_pass: bool
    d1_variance_reduction_ratio: float
    d2_pass: bool
    d2_edge_leakage: float
    d3_pass: bool
    d3_jitter_reduction_ratio: float
    d3_baseline_jitter: float
    d3_tracking_jitter: float

    @property
    def passed(self) -> bool:
        return self.d1_pass and self.d2_pass and self.d3_pass


@dataclass
class ModuleETestResults:
    e1_pass: bool
    e1_history_monotonic: bool
    e1_final_mean_history_length: float
    e1_history_cap: float
    e2_pass: bool
    e2_reset_ratio: float
    e3_pass: bool
    e3_ghosting_metric: float

    @property
    def passed(self) -> bool:
        return self.e1_pass and self.e2_pass and self.e3_pass


@dataclass
class ModuleFTestResults:
    f1_pass: bool
    f1_p99_suppression_ratio: float
    f2_pass: bool
    f2_median_drift_ratio: float
    f3_pass: bool
    f3_recovery_frame: int
    f3_recovery_psnr: float
    f3_recovery_ssim: float

    @property
    def passed(self) -> bool:
        return self.f1_pass and self.f2_pass and self.f3_pass


@dataclass
class ModuleGTestResults:
    g1_pass: bool
    g1_high_frequency_reduction_ratio: float
    g2_pass: bool
    g2_edge_mse: float
    g3_pass: bool
    g3_low_history_radius: float
    g3_high_history_radius: float

    @property
    def passed(self) -> bool:
        return self.g1_pass and self.g2_pass and self.g3_pass


@dataclass
class ModuleHTestResults:
    h1_pass: bool
    h1_frames_validated: int
    h1_last_read_index: int
    h1_last_history_checksum: str
    h2_pass: bool
    h2_max_abs_diff: float
    h2_rmse: float

    @property
    def passed(self) -> bool:
        return self.h1_pass and self.h2_pass


@dataclass
class ModuleITestResults:
    i1_pass: bool
    i1_flicker_reduction_ratio: float
    i2_pass: bool
    i2_trailing_error: float

    @property
    def passed(self) -> bool:
        return self.i1_pass and self.i2_pass


@dataclass
class ModuleJTestResults:
    j1_pass: bool
    j1_max_abs_diff: float
    j2_pass: bool
    j2_detection_ratio: float
    j2_false_positive_ratio: float

    @property
    def passed(self) -> bool:
        return self.j1_pass and self.j2_pass


def run_module_a_tests(seed: int = 7) -> ModuleATestResults:
    rng = np.random.default_rng(seed)

    # A1: roundtrip encoding error.
    sample_count = 20000
    normals = rng.normal(size=(sample_count, 3)).astype(np.float32)
    normals /= np.linalg.norm(normals, axis=1,
                              keepdims=True).astype(np.float32)
    roughness = rng.uniform(0.0, 1.0, size=sample_count).astype(np.float32)
    radiance = np.exp(rng.normal(loc=-0.25, scale=0.8,
                      size=(sample_count, 3))).astype(np.float32)
    view_z = rng.uniform(0.1, 100.0, size=sample_count).astype(np.float32)
    hit_distance = rng.uniform(0.0, 40.0, size=sample_count).astype(np.float32)
    norm_hit = get_norm_hit_distance(hit_distance, view_z, roughness)

    packed_nr = pack_normal_roughness(normals, roughness)
    unpacked_normals, unpacked_roughness = unpack_normal_roughness(packed_nr)

    packed_rad = pack_radiance_and_norm_hit_dist(radiance, norm_hit)
    unpacked_radiance, unpacked_norm_hit = unpack_radiance_and_norm_hit_dist(
        packed_rad)

    a1_normal_rmse = rmse(unpacked_normals, normals)
    a1_roughness_rmse = rmse(unpacked_roughness, roughness)
    a1_radiance_rmse = rmse(unpacked_radiance, radiance)
    a1_norm_hit_rmse = rmse(unpacked_norm_hit, norm_hit)
    a1_pass = (
        a1_normal_rmse <= 1e-5
        and a1_roughness_rmse <= 1e-6
        and a1_radiance_rmse <= 1e-5
        and a1_norm_hit_rmse <= 1e-6
    )

    # A2: motion-vector reprojection error.
    width = 1280
    height = 720
    grid_size = 120
    xs = np.linspace(-0.9, 0.9, grid_size, dtype=np.float32)
    ys = np.linspace(-0.9, 0.9, grid_size, dtype=np.float32)
    xx, yy = np.meshgrid(xs, ys, indexing="xy")
    world_positions = np.stack(
        (xx.ravel(), yy.ravel(), np.zeros_like(xx).ravel()), axis=-1)

    current_view_projection = np.eye(4, dtype=np.float32)
    previous_view_projection = np.eye(4, dtype=np.float32)
    previous_view_projection[0, 3] = 0.02
    previous_view_projection[1, 3] = -0.015

    current_pixels = project_world_to_pixel(
        world_positions, current_view_projection, width, height)
    previous_pixels = project_world_to_pixel(
        world_positions, previous_view_projection, width, height)
    motion_vectors = compute_motion_vectors_pixels(
        world_positions,
        current_view_projection,
        previous_view_projection,
        width,
        height,
        has_history=True,
    )
    reprojected_pixels = current_pixels + motion_vectors
    reprojection_error = np.linalg.norm(
        reprojected_pixels - previous_pixels, axis=1)
    a2_mean_reprojection_error_px = float(np.mean(reprojection_error))
    a2_max_reprojection_error_px = float(np.max(reprojection_error))
    a2_pass = a2_mean_reprojection_error_px < 0.25

    # A3: guide validity ratio.
    guide_count = 100000
    guide_view_z = rng.uniform(0.1, 100.0, size=guide_count).astype(np.float32)
    guide_roughness = rng.uniform(
        0.0, 1.0, size=guide_count).astype(np.float32)
    guide_hit_distance = rng.uniform(
        0.0, 40.0, size=guide_count).astype(np.float32)
    guide_norm_hit_distance = get_norm_hit_distance(
        guide_hit_distance, guide_view_z, guide_roughness)

    # Inject a tiny amount of invalid data to make the ratio check meaningful.
    invalid_count = max(1, guide_count // 2000)  # 0.05%
    guide_view_z[:invalid_count] = np.nan
    guide_norm_hit_distance[invalid_count: invalid_count * 2] = np.nan

    valid_mask = (
        np.isfinite(guide_view_z)
        & (guide_view_z > 0.0)
        & np.isfinite(guide_norm_hit_distance)
        & (guide_norm_hit_distance >= 0.0)
        & (guide_norm_hit_distance <= 1.0)
    )
    a3_guide_valid_ratio = float(np.mean(valid_mask))
    a3_pass = a3_guide_valid_ratio >= 0.999

    return ModuleATestResults(
        a1_pass=a1_pass,
        a1_normal_rmse=a1_normal_rmse,
        a1_roughness_rmse=a1_roughness_rmse,
        a1_radiance_rmse=a1_radiance_rmse,
        a1_norm_hit_rmse=a1_norm_hit_rmse,
        a2_pass=a2_pass,
        a2_mean_reprojection_error_px=a2_mean_reprojection_error_px,
        a2_max_reprojection_error_px=a2_max_reprojection_error_px,
        a3_pass=a3_pass,
        a3_guide_valid_ratio=a3_guide_valid_ratio,
    )


def run_module_b_tests(seed: int = 19) -> ModuleBTestResults:
    rng = np.random.default_rng(seed)

    width = 317
    height = 239
    denoising_range = 300.0

    view_z = rng.uniform(0.1, denoising_range * 0.95,
                         size=(height, width)).astype(np.float32)

    sky_pixel_mask = rng.random((height, width)) < 0.35
    far_pixel_mask = rng.random((height, width)) < 0.08
    nan_pixel_mask = rng.random((height, width)) < 0.01
    view_z[sky_pixel_mask] = 0.0
    view_z[far_pixel_mask] = denoising_range + \
        rng.uniform(1.0, 50.0, size=far_pixel_mask.sum())
    view_z[nan_pixel_mask] = np.nan

    tile_size = 16
    tile_width = (width + tile_size - 1) // tile_size
    tile_height = (height + tile_size - 1) // tile_size
    for tile_y in range(tile_height):
        y0 = tile_y * tile_size
        y1 = min(y0 + tile_size, height)
        for tile_x in range(tile_width):
            x0 = tile_x * tile_size
            x1 = min(x0 + tile_size, width)
            pattern = (tile_x * 13 + tile_y * 7) % 9
            if pattern == 0:
                view_z[y0:y1, x0:x1] = 0.0
            elif pattern == 1:
                view_z[y0:y1, x0:x1] = denoising_range + 10.0
            elif pattern == 2:
                view_z[y0:y1, x0:x1] = np.nan
            elif pattern == 3:
                view_z[y0:y1, x0:x1] = 0.0
                center_x = (x0 + x1 - 1) // 2
                center_y = (y0 + y1 - 1) // 2
                view_z[center_y, center_x] = denoising_range * 0.5

    reference_tiles = classify_tiles_reference(view_z, denoising_range)
    predicted_tiles = classify_tiles_shader_equivalent(view_z, denoising_range)

    reference_denoisable_mask = reference_tiles == 0
    predicted_denoisable_mask = predicted_tiles == 0
    b1_precision, b1_recall = binary_precision_recall(
        reference_denoisable_mask, predicted_denoisable_mask)
    b1_pass = b1_precision >= 0.999 and b1_recall >= 0.999

    expected_hash = hash_tile_mask(predicted_tiles)
    hash_values = set()
    repeat_count = 16
    for _ in range(repeat_count):
        rerun_tiles = classify_tiles_shader_equivalent(
            view_z.copy(), denoising_range)
        hash_values.add(hash_tile_mask(rerun_tiles))
    b2_unique_hash_count = len(hash_values)
    b2_pass = b2_unique_hash_count == 1 and expected_hash in hash_values

    return ModuleBTestResults(
        b1_pass=b1_pass,
        b1_precision=b1_precision,
        b1_recall=b1_recall,
        b2_pass=b2_pass,
        b2_unique_hash_count=b2_unique_hash_count,
        b2_reference_hash=expected_hash,
    )


def _build_module_c_fixture(
    seed: int,
    width: int,
    height: int,
    invalid_ratio: float,
    denoising_range: float,
) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(seed)
    x = np.linspace(0.0, 1.0, width, dtype=np.float32)
    y = np.linspace(0.0, 1.0, height, dtype=np.float32)
    xx, yy = np.meshgrid(x, y, indexing="xy")

    base_view_z = 12.0 + 48.0 * \
        (0.35 * xx + 0.65 * yy) + 2.0 * np.sin((xx + yy) * np.pi * 2.0)
    base_view_z = np.clip(
        base_view_z, 1.0, denoising_range * 0.9).astype(np.float32)

    nx = 0.15 * np.sin(xx * np.pi * 2.0)
    ny = 0.15 * np.cos(yy * np.pi * 2.0)
    nz = np.sqrt(np.maximum(1.0 - nx * nx - ny * ny, 1e-6)).astype(np.float32)
    normals = np.stack((nx, ny, nz), axis=-1).astype(np.float32)
    roughness = np.clip(0.2 + 0.6 * yy + 0.1 *
                        np.sin(xx * np.pi), 0.0, 1.0).astype(np.float32)
    packed_normal_roughness = pack_normal_roughness(normals, roughness)

    diff_hit_ground_truth = np.clip(
        0.16
        + 0.50 * xx
        + 0.14 * np.sin(yy * np.pi * 2.0)
        + 0.04 * np.sin((xx + yy) * np.pi * 4.0),
        0.02,
        0.98,
    ).astype(np.float32)
    spec_hit_ground_truth = np.clip(
        0.12
        + 0.42 * yy
        + 0.16 * np.cos(xx * np.pi * 2.0)
        + 0.04 * np.sin((xx - yy) * np.pi * 4.0),
        0.02,
        0.98,
    ).astype(np.float32)

    diff_rgb = np.stack(
        (
            0.25 + 0.50 * xx,
            0.10 + 0.30 * yy,
            0.05 + 0.20 * (1.0 - xx),
        ),
        axis=-1,
    ).astype(np.float32)
    spec_rgb = np.stack(
        (
            0.10 + 0.25 * (1.0 - yy),
            0.07 + 0.20 * xx,
            0.04 + 0.20 * yy,
        ),
        axis=-1,
    ).astype(np.float32)

    diff_signal = np.concatenate(
        (diff_rgb, diff_hit_ground_truth[..., None]), axis=-1).astype(np.float32)
    spec_signal = np.concatenate(
        (spec_rgb, spec_hit_ground_truth[..., None]), axis=-1).astype(np.float32)

    invalid_mask = (rng.random((height, width)) < invalid_ratio).astype(bool)
    invalid_mask[:2, :] = False
    invalid_mask[-2:, :] = False
    invalid_mask[:, :2] = False
    invalid_mask[:, -2:] = False

    diff_signal_with_invalid = diff_signal.copy()
    spec_signal_with_invalid = spec_signal.copy()
    diff_signal_with_invalid[invalid_mask, 3] = 0.0
    spec_signal_with_invalid[invalid_mask, 3] = 0.0

    tile_mask = classify_tiles_reference(base_view_z, denoising_range)

    return {
        "tile_mask": tile_mask,
        "packed_normal_roughness": packed_normal_roughness,
        "view_z": base_view_z,
        "diff_input": diff_signal_with_invalid,
        "spec_input": spec_signal_with_invalid,
        "diff_ground_truth": diff_signal,
        "spec_ground_truth": spec_signal,
        "invalid_mask": invalid_mask,
    }


def _tile_mask_to_pixel_mask(tile_mask: np.ndarray, width: int, height: int) -> np.ndarray:
    pixel_mask = np.ones((height, width), dtype=bool)
    for y in range(height):
        tile_y = y // REBLUR_TILE_SIZE
        for x in range(width):
            tile_x = x // REBLUR_TILE_SIZE
            pixel_mask[y, x] = tile_mask[tile_y, tile_x] == 0
    return pixel_mask


def run_module_c_tests(seed: int = 31) -> ModuleCTestResults:
    denoising_range = 300.0

    primary_fixture = _build_module_c_fixture(
        seed=seed,
        width=208,
        height=144,
        invalid_ratio=0.35,
        denoising_range=denoising_range,
    )

    primary_out_diff_3x3, primary_out_spec_3x3 = reconstruct_hit_distance_shader_equivalent(
        primary_fixture["tile_mask"],
        primary_fixture["packed_normal_roughness"],
        primary_fixture["view_z"],
        primary_fixture["diff_input"],
        primary_fixture["spec_input"],
        denoising_range,
        REBLUR_HIT_DIST_RECONSTRUCTION_AREA_3X3,
    )

    height, width = primary_fixture["view_z"].shape
    denoisable_mask = _tile_mask_to_pixel_mask(
        primary_fixture["tile_mask"], width, height)
    view_z_mask = (
        np.isfinite(primary_fixture["view_z"])
        & (primary_fixture["view_z"] > 0.0)
        & (primary_fixture["view_z"] <= denoising_range)
    )
    invalid_eval_mask = primary_fixture["invalid_mask"] & denoisable_mask & view_z_mask

    diff_value_range = float(
        np.max(primary_fixture["diff_ground_truth"][..., 3]) -
        np.min(primary_fixture["diff_ground_truth"][..., 3])
    )
    spec_value_range = float(
        np.max(primary_fixture["spec_ground_truth"][..., 3]) -
        np.min(primary_fixture["spec_ground_truth"][..., 3])
    )
    c1_diff_rmse_norm = normalized_rmse(
        primary_out_diff_3x3[..., 3][invalid_eval_mask],
        primary_fixture["diff_ground_truth"][..., 3][invalid_eval_mask],
        max(diff_value_range, 1e-6),
    )
    c1_spec_rmse_norm = normalized_rmse(
        primary_out_spec_3x3[..., 3][invalid_eval_mask],
        primary_fixture["spec_ground_truth"][..., 3][invalid_eval_mask],
        max(spec_value_range, 1e-6),
    )
    c1_invalid_rmse_norm = 0.5 * (c1_diff_rmse_norm + c1_spec_rmse_norm)
    c1_pass = c1_invalid_rmse_norm <= 0.12

    valid_input_mask = (
        ~primary_fixture["invalid_mask"]) & denoisable_mask & view_z_mask
    c2_diff_luma_abs_error = float(
        np.max(np.abs(primary_out_diff_3x3[..., 0][valid_input_mask] -
               primary_fixture["diff_input"][..., 0][valid_input_mask]))
    )
    c2_spec_luma_abs_error = float(
        np.max(np.abs(primary_out_spec_3x3[..., 0][valid_input_mask] -
               primary_fixture["spec_input"][..., 0][valid_input_mask]))
    )
    c2_valid_luma_abs_error = max(
        c2_diff_luma_abs_error, c2_spec_luma_abs_error)
    c2_pass = c2_valid_luma_abs_error <= 1e-4

    sparse_fixture = _build_module_c_fixture(
        seed=seed + 1,
        width=208,
        height=144,
        invalid_ratio=0.68,
        denoising_range=denoising_range,
    )
    sparse_out_diff_3x3, sparse_out_spec_3x3 = reconstruct_hit_distance_shader_equivalent(
        sparse_fixture["tile_mask"],
        sparse_fixture["packed_normal_roughness"],
        sparse_fixture["view_z"],
        sparse_fixture["diff_input"],
        sparse_fixture["spec_input"],
        denoising_range,
        REBLUR_HIT_DIST_RECONSTRUCTION_AREA_3X3,
    )
    sparse_out_diff_5x5, sparse_out_spec_5x5 = reconstruct_hit_distance_shader_equivalent(
        sparse_fixture["tile_mask"],
        sparse_fixture["packed_normal_roughness"],
        sparse_fixture["view_z"],
        sparse_fixture["diff_input"],
        sparse_fixture["spec_input"],
        denoising_range,
        REBLUR_HIT_DIST_RECONSTRUCTION_AREA_5X5,
    )

    sparse_height, sparse_width = sparse_fixture["view_z"].shape
    sparse_denoisable_mask = _tile_mask_to_pixel_mask(
        sparse_fixture["tile_mask"], sparse_width, sparse_height)
    sparse_view_z_mask = (
        np.isfinite(sparse_fixture["view_z"])
        & (sparse_fixture["view_z"] > 0.0)
        & (sparse_fixture["view_z"] <= denoising_range)
    )
    sparse_invalid_eval_mask = sparse_fixture["invalid_mask"] & sparse_denoisable_mask & sparse_view_z_mask

    sparse_diff_range = float(
        np.max(sparse_fixture["diff_ground_truth"][..., 3]) -
        np.min(sparse_fixture["diff_ground_truth"][..., 3])
    )
    sparse_spec_range = float(
        np.max(sparse_fixture["spec_ground_truth"][..., 3]) -
        np.min(sparse_fixture["spec_ground_truth"][..., 3])
    )

    c3_diff_rmse_3x3 = normalized_rmse(
        sparse_out_diff_3x3[..., 3][sparse_invalid_eval_mask],
        sparse_fixture["diff_ground_truth"][..., 3][sparse_invalid_eval_mask],
        max(sparse_diff_range, 1e-6),
    )
    c3_diff_rmse_5x5 = normalized_rmse(
        sparse_out_diff_5x5[..., 3][sparse_invalid_eval_mask],
        sparse_fixture["diff_ground_truth"][..., 3][sparse_invalid_eval_mask],
        max(sparse_diff_range, 1e-6),
    )
    c3_spec_rmse_3x3 = normalized_rmse(
        sparse_out_spec_3x3[..., 3][sparse_invalid_eval_mask],
        sparse_fixture["spec_ground_truth"][..., 3][sparse_invalid_eval_mask],
        max(sparse_spec_range, 1e-6),
    )
    c3_spec_rmse_5x5 = normalized_rmse(
        sparse_out_spec_5x5[..., 3][sparse_invalid_eval_mask],
        sparse_fixture["spec_ground_truth"][..., 3][sparse_invalid_eval_mask],
        max(sparse_spec_range, 1e-6),
    )
    c3_rmse_3x3_norm = 0.5 * (c3_diff_rmse_3x3 + c3_spec_rmse_3x3)
    c3_rmse_5x5_norm = 0.5 * (c3_diff_rmse_5x5 + c3_spec_rmse_5x5)
    c3_pass = c3_rmse_5x5_norm <= c3_rmse_3x3_norm + 1e-6

    return ModuleCTestResults(
        c1_pass=c1_pass,
        c1_invalid_rmse_norm=c1_invalid_rmse_norm,
        c2_pass=c2_pass,
        c2_valid_luma_abs_error=c2_valid_luma_abs_error,
        c3_pass=c3_pass,
        c3_rmse_3x3_norm=c3_rmse_3x3_norm,
        c3_rmse_5x5_norm=c3_rmse_5x5_norm,
    )


def _build_module_d_flat_fixture(seed: int, width: int, height: int) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(seed)

    view_z = np.full((height, width), 40.0, dtype=np.float32)
    normals = np.zeros((height, width, 3), dtype=np.float32)
    normals[..., 2] = 1.0
    roughness = np.full((height, width), 0.35, dtype=np.float32)
    packed_normal_roughness = pack_normal_roughness(normals, roughness)

    clean_diff = np.zeros((height, width, 4), dtype=np.float32)
    clean_spec = np.zeros((height, width, 4), dtype=np.float32)
    clean_diff[..., 0] = 0.42
    clean_diff[..., 1] = 0.03
    clean_diff[..., 2] = -0.01
    clean_diff[..., 3] = 0.30
    clean_spec[..., 0] = 0.28
    clean_spec[..., 1] = 0.02
    clean_spec[..., 2] = -0.02
    clean_spec[..., 3] = 0.55

    noisy_diff = clean_diff.copy()
    noisy_spec = clean_spec.copy()
    noisy_diff[..., :3] += rng.normal(0.0, 0.08,
                                      size=(height, width, 3)).astype(np.float32)
    noisy_spec[..., :3] += rng.normal(0.0, 0.09,
                                      size=(height, width, 3)).astype(np.float32)
    noisy_diff[..., 3] = np.clip(
        noisy_diff[..., 3] + rng.normal(0.0, 0.04, size=(height, width)), 0.0, 1.0)
    noisy_spec[..., 3] = np.clip(
        noisy_spec[..., 3] + rng.normal(0.0, 0.05, size=(height, width)), 0.0, 1.0)

    tile_mask = classify_tiles_reference(view_z, denoising_range=300.0)
    eval_mask = np.zeros((height, width), dtype=bool)
    eval_mask[height // 4: (height * 3) // 4, width //
              4: (width * 3) // 4] = True

    return {
        "tile_mask": tile_mask,
        "packed_normal_roughness": packed_normal_roughness,
        "view_z": view_z,
        "noisy_diff": noisy_diff.astype(np.float32),
        "noisy_spec": noisy_spec.astype(np.float32),
        "clean_diff": clean_diff,
        "clean_spec": clean_spec,
        "eval_mask": eval_mask,
    }


def _build_module_d_edge_fixture(seed: int, width: int, height: int) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(seed)
    mid = width // 2

    view_z = np.full((height, width), 30.0, dtype=np.float32)
    view_z[:, mid:] = 95.0

    normals = np.zeros((height, width, 3), dtype=np.float32)
    normals[..., 2] = 1.0
    right_normal = np.array([0.6, 0.0, 0.8], dtype=np.float32)
    right_normal /= np.linalg.norm(right_normal)
    normals[:, mid:, :] = right_normal

    roughness = np.full((height, width), 0.2, dtype=np.float32)
    roughness[:, mid:] = 0.75
    packed_normal_roughness = pack_normal_roughness(normals, roughness)

    clean_diff = np.zeros((height, width, 4), dtype=np.float32)
    clean_spec = np.zeros((height, width, 4), dtype=np.float32)
    clean_diff[:, :mid, 0] = 0.18
    clean_diff[:, mid:, 0] = 0.82
    clean_diff[..., 1] = 0.02
    clean_diff[..., 2] = -0.01
    clean_diff[:, :mid, 3] = 0.26
    clean_diff[:, mid:, 3] = 0.62

    clean_spec[:, :mid, 0] = 0.14
    clean_spec[:, mid:, 0] = 0.68
    clean_spec[..., 1] = 0.02
    clean_spec[..., 2] = -0.02
    clean_spec[:, :mid, 3] = 0.20
    clean_spec[:, mid:, 3] = 0.72

    noisy_diff = clean_diff.copy()
    noisy_spec = clean_spec.copy()
    noisy_diff[..., :3] += rng.normal(0.0, 0.05,
                                      size=(height, width, 3)).astype(np.float32)
    noisy_spec[..., :3] += rng.normal(0.0, 0.06,
                                      size=(height, width, 3)).astype(np.float32)
    noisy_diff[..., 3] = np.clip(
        noisy_diff[..., 3] + rng.normal(0.0, 0.03, size=(height, width)), 0.0, 1.0)
    noisy_spec[..., 3] = np.clip(
        noisy_spec[..., 3] + rng.normal(0.0, 0.03, size=(height, width)), 0.0, 1.0)

    tile_mask = classify_tiles_reference(view_z, denoising_range=300.0)
    return {
        "tile_mask": tile_mask,
        "packed_normal_roughness": packed_normal_roughness,
        "view_z": view_z,
        "noisy_diff": noisy_diff.astype(np.float32),
        "noisy_spec": noisy_spec.astype(np.float32),
        "clean_diff": clean_diff,
        "clean_spec": clean_spec,
        "mid": mid,
    }


def run_module_d_tests(seed: int = 43) -> ModuleDTestResults:
    denoising_range = 300.0

    flat_fixture = _build_module_d_flat_fixture(
        seed=seed, width=192, height=128)
    out_diff_flat, out_spec_flat, _ = prepass_shader_equivalent(
        flat_fixture["tile_mask"],
        flat_fixture["packed_normal_roughness"],
        flat_fixture["view_z"],
        flat_fixture["noisy_diff"],
        flat_fixture["noisy_spec"],
        denoising_range,
        diffuse_radius=2.0,
        specular_radius=2.0,
        spec_tracking_radius=2.0,
    )

    eval_mask = flat_fixture["eval_mask"]
    before_variance = 0.5 * (
        float(np.var(flat_fixture["noisy_diff"][..., 0][eval_mask])) +
        float(np.var(flat_fixture["noisy_spec"][..., 0][eval_mask]))
    )
    after_variance = 0.5 * (
        float(np.var(out_diff_flat[..., 0][eval_mask])) +
        float(np.var(out_spec_flat[..., 0][eval_mask]))
    )
    d1_variance_reduction_ratio = before_variance / max(after_variance, 1e-8)
    d1_pass = d1_variance_reduction_ratio >= 1.8

    edge_fixture = _build_module_d_edge_fixture(
        seed=seed + 1, width=256, height=128)
    out_diff_edge, out_spec_edge, _ = prepass_shader_equivalent(
        edge_fixture["tile_mask"],
        edge_fixture["packed_normal_roughness"],
        edge_fixture["view_z"],
        edge_fixture["noisy_diff"],
        edge_fixture["noisy_spec"],
        denoising_range,
        diffuse_radius=2.0,
        specular_radius=2.0,
        spec_tracking_radius=2.0,
    )

    mid = edge_fixture["mid"]
    edge_band_half_width = 3
    left_strip = slice(mid - edge_band_half_width, mid)
    right_strip = slice(mid, mid + edge_band_half_width)

    clean_reference_delta = 0.5 * (
        float(np.mean(edge_fixture["clean_diff"][:, right_strip, 0])) -
        float(np.mean(edge_fixture["clean_diff"][:, left_strip, 0]))
    ) + 0.5 * (
        float(np.mean(edge_fixture["clean_spec"][:, right_strip, 0])) -
        float(np.mean(edge_fixture["clean_spec"][:, left_strip, 0]))
    )
    filtered_delta = 0.5 * (
        float(np.mean(out_diff_edge[:, right_strip, 0])) -
        float(np.mean(out_diff_edge[:, left_strip, 0]))
    ) + 0.5 * (
        float(np.mean(out_spec_edge[:, right_strip, 0])) -
        float(np.mean(out_spec_edge[:, left_strip, 0]))
    )
    d2_edge_leakage = max(0.0, (clean_reference_delta -
                          filtered_delta) / max(clean_reference_delta, 1e-6))
    d2_pass = d2_edge_leakage <= 0.08

    jitter_seed = seed + 2
    rng = np.random.default_rng(jitter_seed)
    jitter_height = 96
    jitter_width = 160
    jitter_view_z = np.full(
        (jitter_height, jitter_width), 48.0, dtype=np.float32)
    jitter_normals = np.zeros(
        (jitter_height, jitter_width, 3), dtype=np.float32)
    jitter_normals[..., 2] = 1.0
    jitter_roughness = np.full(
        (jitter_height, jitter_width), 0.55, dtype=np.float32)
    jitter_packed_normal_roughness = pack_normal_roughness(
        jitter_normals, jitter_roughness)
    jitter_tile_mask = classify_tiles_reference(jitter_view_z, denoising_range)

    base_diff = np.zeros((jitter_height, jitter_width, 4), dtype=np.float32)
    base_diff[..., 0] = 0.33
    base_diff[..., 1] = 0.02
    base_diff[..., 2] = -0.01
    base_diff[..., 3] = 0.42
    base_spec = np.zeros((jitter_height, jitter_width, 4), dtype=np.float32)
    base_spec[..., 0] = 0.40
    base_spec[..., 1] = 0.03
    base_spec[..., 2] = -0.02
    base_spec[..., 3] = 0.58

    jitter_eval_mask = np.zeros((jitter_height, jitter_width), dtype=bool)
    jitter_eval_mask[24:72, 40:120] = True

    frame_count = 12
    baseline_frames = []
    tracking_frames = []
    for _ in range(frame_count):
        frame_diff = base_diff.copy()
        frame_spec = base_spec.copy()
        frame_diff[..., :3] += rng.normal(0.0, 0.03, size=(
            jitter_height, jitter_width, 3)).astype(np.float32)
        frame_spec[..., :3] += rng.normal(0.0, 0.03, size=(
            jitter_height, jitter_width, 3)).astype(np.float32)
        frame_spec[..., 3] = np.clip(frame_spec[..., 3] + rng.normal(0.0, 0.06, size=(jitter_height, jitter_width)),
                                     0.0, 1.0)

        _, _, spec_tracking = prepass_shader_equivalent(
            jitter_tile_mask,
            jitter_packed_normal_roughness,
            jitter_view_z,
            frame_diff,
            frame_spec,
            denoising_range,
            diffuse_radius=0.0,
            specular_radius=0.0,
            spec_tracking_radius=2.0,
        )

        baseline_frames.append(frame_spec[..., 3][jitter_eval_mask].copy())
        tracking_frames.append(spec_tracking[jitter_eval_mask].copy())

    baseline_stack = np.stack(baseline_frames, axis=0)
    tracking_stack = np.stack(tracking_frames, axis=0)
    d3_baseline_jitter = float(np.mean(np.std(baseline_stack, axis=0)))
    d3_tracking_jitter = float(np.mean(np.std(tracking_stack, axis=0)))
    d3_jitter_reduction_ratio = d3_tracking_jitter / \
        max(d3_baseline_jitter, 1e-8)
    d3_pass = d3_jitter_reduction_ratio <= 0.80

    return ModuleDTestResults(
        d1_pass=d1_pass,
        d1_variance_reduction_ratio=d1_variance_reduction_ratio,
        d2_pass=d2_pass,
        d2_edge_leakage=d2_edge_leakage,
        d3_pass=d3_pass,
        d3_jitter_reduction_ratio=d3_jitter_reduction_ratio,
        d3_baseline_jitter=d3_baseline_jitter,
        d3_tracking_jitter=d3_tracking_jitter,
    )


def run_module_e_tests(seed: int = 53) -> ModuleETestResults:
    rng = np.random.default_rng(seed)
    denoising_range = 300.0
    max_history_frames = 8

    height = 96
    width = 160
    static_view_z = np.full((height, width), 42.0, dtype=np.float32)
    static_normals = np.zeros((height, width, 3), dtype=np.float32)
    static_normals[..., 2] = 1.0
    static_roughness = np.full((height, width), 0.45, dtype=np.float32)
    static_packed_normal_roughness = pack_normal_roughness(
        static_normals, static_roughness)
    static_tile_mask = classify_tiles_reference(static_view_z, denoising_range)
    static_motion_vectors = np.zeros((height, width, 4), dtype=np.float32)
    static_eval_mask = _tile_mask_to_pixel_mask(
        static_tile_mask, width, height)

    base_diff = np.zeros((height, width, 4), dtype=np.float32)
    base_spec = np.zeros((height, width, 4), dtype=np.float32)
    base_diff[..., 0] = 0.26
    base_diff[..., 1] = 0.03
    base_diff[..., 2] = -0.01
    base_diff[..., 3] = 0.40
    base_spec[..., 0] = 0.31
    base_spec[..., 1] = 0.02
    base_spec[..., 2] = -0.02
    base_spec[..., 3] = 0.62

    prev_view_z = static_view_z.copy()
    prev_normal_roughness = static_packed_normal_roughness.copy()
    prev_internal_data = np.zeros((height, width, 4), dtype=np.float32)
    prev_diff_history = base_diff.copy()
    prev_spec_history = base_spec.copy()
    prev_diff_fast_history = base_diff.copy()
    prev_spec_fast_history = base_spec.copy()
    prev_spec_hit_tracking = base_spec[..., 3].copy()

    history_mean_trace: list[float] = []
    frame_count = 12
    for frame in range(frame_count):
        frame_diff = base_diff.copy()
        frame_spec = base_spec.copy()
        frame_diff[..., :3] += rng.normal(0.0, 0.01,
                                          size=(height, width, 3)).astype(np.float32)
        frame_spec[..., :3] += rng.normal(0.0, 0.01,
                                          size=(height, width, 3)).astype(np.float32)
        frame_diff[..., 3] = np.clip(frame_diff[..., 3] + rng.normal(0.0, 0.01, size=(height, width)),
                                     0.0, 1.0)
        frame_spec[..., 3] = np.clip(frame_spec[..., 3] + rng.normal(0.0, 0.01, size=(height, width)),
                                     0.0, 1.0)
        frame_tracking = np.clip(frame_spec[..., 3], 0.0, 1.0)

        (
            _,
            _,
            out_internal_data,
            out_diff_history,
            out_spec_history,
            out_diff_fast_history,
            out_spec_fast_history,
            out_spec_hit_tracking,
        ) = temporal_accumulation_shader_equivalent(
            static_tile_mask,
            static_packed_normal_roughness,
            static_view_z,
            static_motion_vectors,
            frame_diff,
            frame_spec,
            frame_tracking,
            prev_view_z,
            prev_normal_roughness,
            prev_internal_data,
            prev_diff_history,
            prev_spec_history,
            prev_diff_fast_history,
            prev_spec_fast_history,
            prev_spec_hit_tracking,
            denoising_range,
            max_history_frames=max_history_frames,
            history_available=frame > 0,
        )

        history_mean_trace.append(
            float(np.mean(out_internal_data[..., 0][static_eval_mask])))
        prev_internal_data = out_internal_data
        prev_diff_history = out_diff_history
        prev_spec_history = out_spec_history
        prev_diff_fast_history = out_diff_fast_history
        prev_spec_fast_history = out_spec_fast_history
        prev_spec_hit_tracking = out_spec_hit_tracking

    history_deltas = np.diff(np.asarray(history_mean_trace, dtype=np.float32))
    e1_history_monotonic = bool(np.all(history_deltas >= -1e-4))
    e1_final_mean_history_length = float(history_mean_trace[-1])
    e1_history_cap = float(max_history_frames)
    e1_pass = e1_history_monotonic and e1_final_mean_history_length >= e1_history_cap - 0.05

    motion_height = 96
    motion_width = 192
    motion_view_z = np.full(
        (motion_height, motion_width), 42.0, dtype=np.float32)
    prev_motion_view_z = motion_view_z.copy()

    object_y0 = 24
    object_y1 = 72
    prev_object_x0 = 40
    prev_object_x1 = 96
    object_shift = 24
    curr_object_x0 = prev_object_x0 + object_shift
    curr_object_x1 = prev_object_x1 + object_shift

    prev_object_mask = np.zeros((motion_height, motion_width), dtype=bool)
    prev_object_mask[object_y0:object_y1, prev_object_x0:prev_object_x1] = True
    curr_object_mask = np.zeros((motion_height, motion_width), dtype=bool)
    curr_object_mask[object_y0:object_y1, curr_object_x0:curr_object_x1] = True

    prev_motion_view_z[prev_object_mask] = 10.0
    motion_view_z[curr_object_mask] = 10.0

    motion_normals = np.zeros(
        (motion_height, motion_width, 3), dtype=np.float32)
    motion_normals[..., 2] = 1.0
    motion_roughness = np.full(
        (motion_height, motion_width), 0.35, dtype=np.float32)
    motion_packed_normal_roughness = pack_normal_roughness(
        motion_normals, motion_roughness)
    motion_tile_mask = classify_tiles_reference(motion_view_z, denoising_range)

    motion_vectors = np.zeros(
        (motion_height, motion_width, 4), dtype=np.float32)
    motion_vectors[curr_object_mask, 0] = -float(object_shift)

    current_diff = np.zeros((motion_height, motion_width, 4), dtype=np.float32)
    current_spec = np.zeros((motion_height, motion_width, 4), dtype=np.float32)
    current_diff[..., 0] = 0.10
    current_spec[..., 0] = 0.08
    current_diff[..., 1] = 0.01
    current_spec[..., 1] = 0.01
    current_diff[..., 2] = -0.01
    current_spec[..., 2] = -0.01
    current_diff[..., 3] = 0.30
    current_spec[..., 3] = 0.35

    current_diff[curr_object_mask, 0] = 0.95
    current_spec[curr_object_mask, 0] = 0.88
    current_diff[curr_object_mask, 3] = 0.75
    current_spec[curr_object_mask, 3] = 0.82
    current_tracking = current_spec[..., 3].copy()

    prev_diff_history = np.zeros(
        (motion_height, motion_width, 4), dtype=np.float32)
    prev_spec_history = np.zeros(
        (motion_height, motion_width, 4), dtype=np.float32)
    prev_diff_history[..., 0] = 0.10
    prev_spec_history[..., 0] = 0.08
    prev_diff_history[..., 1] = 0.01
    prev_spec_history[..., 1] = 0.01
    prev_diff_history[..., 2] = -0.01
    prev_spec_history[..., 2] = -0.01
    prev_diff_history[..., 3] = 0.30
    prev_spec_history[..., 3] = 0.35
    prev_diff_history[prev_object_mask, 0] = 0.95
    prev_spec_history[prev_object_mask, 0] = 0.88
    prev_diff_history[prev_object_mask, 3] = 0.75
    prev_spec_history[prev_object_mask, 3] = 0.82

    prev_diff_fast_history = prev_diff_history.copy()
    prev_spec_fast_history = prev_spec_history.copy()
    prev_spec_tracking = prev_spec_history[..., 3].copy()
    prev_internal_data = np.zeros(
        (motion_height, motion_width, 4), dtype=np.float32)
    prev_internal_data[..., 0] = float(max_history_frames)
    prev_internal_data[..., 1] = 4.0
    prev_internal_data[..., 2] = 1.0

    (
        _,
        _,
        out_internal_data,
        out_diff_history,
        out_spec_history,
        _,
        _,
        _,
    ) = temporal_accumulation_shader_equivalent(
        motion_tile_mask,
        motion_packed_normal_roughness,
        motion_view_z,
        motion_vectors,
        current_diff,
        current_spec,
        current_tracking,
        prev_motion_view_z,
        motion_packed_normal_roughness,
        prev_internal_data,
        prev_diff_history,
        prev_spec_history,
        prev_diff_fast_history,
        prev_spec_fast_history,
        prev_spec_tracking,
        denoising_range,
        max_history_frames=max_history_frames,
        history_available=True,
    )

    motion_denoisable_mask = _tile_mask_to_pixel_mask(
        motion_tile_mask, motion_width, motion_height)
    newly_uncovered_mask = prev_object_mask & (
        ~curr_object_mask) & motion_denoisable_mask
    reset_pixels = out_internal_data[..., 0][newly_uncovered_mask] <= 1.01
    e2_reset_ratio = float(np.mean(reset_pixels)
                           ) if reset_pixels.size > 0 else 1.0
    e2_pass = e2_reset_ratio >= 0.99

    e3_diff_error = float(np.mean(np.abs(out_diff_history[..., 0][newly_uncovered_mask] -
                                         current_diff[..., 0][newly_uncovered_mask])))
    e3_spec_error = float(np.mean(np.abs(out_spec_history[..., 0][newly_uncovered_mask] -
                                         current_spec[..., 0][newly_uncovered_mask])))
    e3_ghosting_metric = 0.5 * (e3_diff_error + e3_spec_error)
    e3_pass = e3_ghosting_metric <= 0.02

    return ModuleETestResults(
        e1_pass=e1_pass,
        e1_history_monotonic=e1_history_monotonic,
        e1_final_mean_history_length=e1_final_mean_history_length,
        e1_history_cap=e1_history_cap,
        e2_pass=e2_pass,
        e2_reset_ratio=e2_reset_ratio,
        e3_pass=e3_pass,
        e3_ghosting_metric=e3_ghosting_metric,
    )


def _compute_psnr(reference: np.ndarray, test: np.ndarray, max_value: float = 1.0) -> float:
    mse = float(np.mean((reference - test) ** 2))
    if mse <= 1e-12:
        return 120.0
    return float(10.0 * np.log10((max_value * max_value) / mse))


def _compute_global_ssim(reference: np.ndarray, test: np.ndarray) -> float:
    ref = reference.astype(np.float32, copy=False)
    dst = test.astype(np.float32, copy=False)
    c1 = 0.01 ** 2
    c2 = 0.03 ** 2

    mu_x = float(np.mean(ref))
    mu_y = float(np.mean(dst))
    sigma_x = float(np.var(ref))
    sigma_y = float(np.var(dst))
    sigma_xy = float(np.mean((ref - mu_x) * (dst - mu_y)))

    numerator = (2.0 * mu_x * mu_y + c1) * (2.0 * sigma_xy + c2)
    denominator = (mu_x * mu_x + mu_y * mu_y + c1) * (sigma_x + sigma_y + c2)
    if denominator <= 1e-12:
        return 1.0
    return float(np.clip(numerator / denominator, 0.0, 1.0))


def run_module_f_tests(seed: int = 61) -> ModuleFTestResults:
    rng = np.random.default_rng(seed)
    denoising_range = 300.0
    history_fix_frame_num = 3
    history_fix_base_pixel_stride = 2.0
    history_fix_sigma_scale = 1.6
    max_history_frames = 32

    height = 72
    width = 104
    view_z = np.full((height, width), 38.0, dtype=np.float32)
    normals = np.zeros((height, width, 3), dtype=np.float32)
    normals[..., 2] = 1.0
    roughness = np.full((height, width), 0.4, dtype=np.float32)
    packed_normal_roughness = pack_normal_roughness(normals, roughness)
    tile_mask = classify_tiles_reference(view_z, denoising_range)
    eval_mask = _tile_mask_to_pixel_mask(tile_mask, width, height)

    base_diff = np.zeros((height, width, 4), dtype=np.float32)
    base_spec = np.zeros((height, width, 4), dtype=np.float32)
    base_diff[..., 0] = 0.28
    base_diff[..., 1] = 0.03
    base_diff[..., 2] = -0.01
    base_diff[..., 3] = 0.40
    base_spec[..., 0] = 0.21
    base_spec[..., 1] = 0.02
    base_spec[..., 2] = -0.01
    base_spec[..., 3] = 0.64

    temporal_diff = base_diff.copy()
    temporal_spec = base_spec.copy()
    temporal_diff[..., :3] += rng.normal(0.0, 0.05,
                                         size=(height, width, 3)).astype(np.float32)
    temporal_spec[..., :3] += rng.normal(0.0, 0.06,
                                         size=(height, width, 3)).astype(np.float32)
    temporal_diff[..., 3] = np.clip(
        temporal_diff[..., 3] + rng.normal(0.0, 0.04, size=(height, width)), 0.0, 1.0)
    temporal_spec[..., 3] = np.clip(
        temporal_spec[..., 3] + rng.normal(0.0, 0.05, size=(height, width)), 0.0, 1.0)

    firefly_mask = rng.random((height, width)) < 0.015
    temporal_diff[firefly_mask,
                  :3] += np.array([8.0, 6.0, 4.0], dtype=np.float32)
    temporal_spec[firefly_mask,
                  :3] += np.array([10.0, 8.0, 6.0], dtype=np.float32)

    prev_diff_fast = base_diff.copy()
    prev_spec_fast = base_spec.copy()
    prev_diff_fast[..., :3] += rng.normal(0.0, 0.02,
                                          size=(height, width, 3)).astype(np.float32)
    prev_spec_fast[..., :3] += rng.normal(0.0, 0.025,
                                          size=(height, width, 3)).astype(np.float32)

    data1 = np.zeros((height, width, 4), dtype=np.float32)
    data1[..., 0] = 0.15
    data1[..., 1] = 1.0
    data1[..., 2] = 2.0 / float(max_history_frames)
    data1[..., 3] = temporal_spec[..., 3]
    spec_tracking = np.clip(temporal_spec[..., 3], 0.0, 1.0)

    out_diff, out_spec, _, _ = history_fix_shader_equivalent(
        tile_mask,
        packed_normal_roughness,
        data1,
        view_z,
        temporal_diff,
        temporal_spec,
        prev_diff_fast,
        prev_spec_fast,
        spec_tracking,
        denoising_range,
        history_fix_frame_num=history_fix_frame_num,
        history_fix_base_pixel_stride=history_fix_base_pixel_stride,
        fast_history_clamping_sigma_scale=history_fix_sigma_scale,
        enable_anti_firefly=True,
        max_history_frames=max_history_frames,
    )

    input_luma = 0.5 * (temporal_diff[..., 0] + temporal_spec[..., 0])
    output_luma = 0.5 * (out_diff[..., 0] + out_spec[..., 0])
    p99_input = float(np.percentile(input_luma[eval_mask], 99.0))
    p99_output = float(np.percentile(output_luma[eval_mask], 99.0))
    f1_p99_suppression_ratio = p99_input / max(p99_output, 1e-6)
    f1_pass = f1_p99_suppression_ratio >= 2.0

    reference_luma = 0.5 * (base_diff[..., 0] + base_spec[..., 0])
    reference_median = float(np.median(reference_luma[eval_mask]))
    output_median = float(np.median(output_luma[eval_mask]))
    f2_median_drift_ratio = abs(
        output_median - reference_median) / max(abs(reference_median), 1e-6)
    f2_pass = f2_median_drift_ratio <= 0.05

    ref_diff = base_diff.copy()
    ref_spec = base_spec.copy()
    reference_rgb = np.clip(ref_diff[..., :3] + ref_spec[..., :3], 0.0, 1.0)
    reference_frame = (
        0.2126 * reference_rgb[..., 0] + 0.7152 *
        reference_rgb[..., 1] + 0.0722 * reference_rgb[..., 2]
    ).astype(np.float32)

    prev_diff_fast_recovery = np.zeros_like(base_diff)
    prev_spec_fast_recovery = np.zeros_like(base_spec)
    psnr_trace: list[float] = []
    ssim_trace: list[float] = []

    frame_count = 6
    for frame_index in range(frame_count):
        frame_diff = base_diff.copy()
        frame_spec = base_spec.copy()
        frame_diff[..., :3] += rng.normal(0.0, 0.07,
                                          size=(height, width, 3)).astype(np.float32)
        frame_spec[..., :3] += rng.normal(0.0, 0.08,
                                          size=(height, width, 3)).astype(np.float32)
        if frame_index == 0:
            reset_firefly_mask = rng.random((height, width)) < 0.020
            frame_diff[reset_firefly_mask,
                       :3] += np.array([7.0, 5.0, 3.0], dtype=np.float32)
            frame_spec[reset_firefly_mask,
                       :3] += np.array([8.0, 6.0, 4.0], dtype=np.float32)

        frame_data1 = np.zeros((height, width, 4), dtype=np.float32)
        history_length = min(frame_index + 1, max_history_frames)
        frame_data1[..., 0] = np.clip(
            (history_length - 1) / max(max_history_frames - 1, 1), 0.0, 1.0)
        frame_data1[..., 1] = 1.0 if frame_index > 0 else 0.0
        frame_data1[..., 2] = history_length / float(max_history_frames)
        frame_data1[..., 3] = np.clip(frame_spec[..., 3], 0.0, 1.0)
        frame_tracking = np.clip(frame_spec[..., 3], 0.0, 1.0)

        frame_out_diff, frame_out_spec, next_diff_fast, next_spec_fast = history_fix_shader_equivalent(
            tile_mask,
            packed_normal_roughness,
            frame_data1,
            view_z,
            frame_diff,
            frame_spec,
            prev_diff_fast_recovery,
            prev_spec_fast_recovery,
            frame_tracking,
            denoising_range,
            history_fix_frame_num=history_fix_frame_num,
            history_fix_base_pixel_stride=history_fix_base_pixel_stride,
            fast_history_clamping_sigma_scale=history_fix_sigma_scale,
            enable_anti_firefly=True,
            max_history_frames=max_history_frames,
        )
        prev_diff_fast_recovery = next_diff_fast
        prev_spec_fast_recovery = next_spec_fast

        out_rgb = np.clip(
            frame_out_diff[..., :3] + frame_out_spec[..., :3], 0.0, 1.0)
        out_luma = (
            0.2126 * out_rgb[..., 0] + 0.7152 *
            out_rgb[..., 1] + 0.0722 * out_rgb[..., 2]
        ).astype(np.float32)

        frame_psnr = _compute_psnr(
            reference_frame[eval_mask], out_luma[eval_mask], max_value=1.0)
        frame_ssim = _compute_global_ssim(
            reference_frame[eval_mask], out_luma[eval_mask])
        psnr_trace.append(frame_psnr)
        ssim_trace.append(frame_ssim)

    psnr_target = max(psnr_trace[0] + 2.0, psnr_trace[-1] * 0.90)
    ssim_target = max(ssim_trace[0] + 0.04, ssim_trace[-1] * 0.90)

    recovery_frame = frame_count
    recovery_psnr = psnr_trace[-1]
    recovery_ssim = ssim_trace[-1]
    for frame_index, (frame_psnr, frame_ssim) in enumerate(zip(psnr_trace, ssim_trace)):
        if frame_psnr >= psnr_target and frame_ssim >= ssim_target:
            recovery_frame = frame_index
            recovery_psnr = frame_psnr
            recovery_ssim = frame_ssim
            break

    f3_recovery_frame = recovery_frame
    f3_recovery_psnr = recovery_psnr
    f3_recovery_ssim = recovery_ssim
    f3_pass = f3_recovery_frame <= 3

    return ModuleFTestResults(
        f1_pass=f1_pass,
        f1_p99_suppression_ratio=f1_p99_suppression_ratio,
        f2_pass=f2_pass,
        f2_median_drift_ratio=f2_median_drift_ratio,
        f3_pass=f3_pass,
        f3_recovery_frame=f3_recovery_frame,
        f3_recovery_psnr=f3_recovery_psnr,
        f3_recovery_ssim=f3_recovery_ssim,
    )


def _compute_laplacian_energy(channel: np.ndarray, mask: np.ndarray) -> float:
    if channel.ndim != 2:
        raise ValueError("channel must be 2D")
    if mask.shape != channel.shape:
        raise ValueError("mask shape must match channel shape")

    if channel.shape[0] < 3 or channel.shape[1] < 3:
        return 0.0

    laplacian = np.zeros_like(channel, dtype=np.float32)
    laplacian[1:-1, 1:-1] = (
        4.0 * channel[1:-1, 1:-1]
        - channel[:-2, 1:-1]
        - channel[2:, 1:-1]
        - channel[1:-1, :-2]
        - channel[1:-1, 2:]
    )

    interior_mask = mask.copy()
    interior_mask[0, :] = False
    interior_mask[-1, :] = False
    interior_mask[:, 0] = False
    interior_mask[:, -1] = False
    if not np.any(interior_mask):
        return 0.0
    return float(np.mean(np.abs(laplacian[interior_mask])))


def _estimate_effective_radius_from_impulse(signal: np.ndarray, center_x: int, center_y: int) -> float:
    if signal.ndim != 2:
        raise ValueError("signal must be 2D")

    height, width = signal.shape
    yy, xx = np.indices((height, width), dtype=np.float32)
    weights = np.clip(signal.astype(np.float32), 0.0, None)
    total_weight = float(np.sum(weights))
    if total_weight <= 1e-8:
        return 0.0

    distance_squared = (xx - float(center_x)) ** 2 + \
        (yy - float(center_y)) ** 2
    return float(np.sqrt(np.sum(weights * distance_squared) / total_weight))


def _build_module_g_flat_fixture(seed: int, width: int, height: int) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(seed)

    view_z = np.full((height, width), 36.0, dtype=np.float32)
    normals = np.zeros((height, width, 3), dtype=np.float32)
    normals[..., 2] = 1.0
    roughness = np.full((height, width), 0.45, dtype=np.float32)
    packed_normal_roughness = pack_normal_roughness(normals, roughness)

    clean_diff = np.zeros((height, width, 4), dtype=np.float32)
    clean_spec = np.zeros((height, width, 4), dtype=np.float32)
    clean_diff[..., 0] = 0.30
    clean_diff[..., 1] = 0.02
    clean_diff[..., 2] = -0.01
    clean_diff[..., 3] = 0.48
    clean_spec[..., 0] = 0.34
    clean_spec[..., 1] = 0.03
    clean_spec[..., 2] = -0.02
    clean_spec[..., 3] = 0.56

    noisy_diff = clean_diff.copy()
    noisy_spec = clean_spec.copy()
    noisy_diff[..., :3] += rng.normal(0.0, 0.14,
                                      size=(height, width, 3)).astype(np.float32)
    noisy_spec[..., :3] += rng.normal(0.0, 0.16,
                                      size=(height, width, 3)).astype(np.float32)
    noisy_diff[..., 3] = np.clip(
        noisy_diff[..., 3] + rng.normal(0.0, 0.08, size=(height, width)), 0.0, 1.0)
    noisy_spec[..., 3] = np.clip(
        noisy_spec[..., 3] + rng.normal(0.0, 0.10, size=(height, width)), 0.0, 1.0)

    tile_mask = classify_tiles_reference(view_z, denoising_range=300.0)
    eval_mask = np.zeros((height, width), dtype=bool)
    eval_mask[height // 4: (height * 3) // 4, width //
              4: (width * 3) // 4] = True

    return {
        "tile_mask": tile_mask,
        "packed_normal_roughness": packed_normal_roughness,
        "view_z": view_z,
        "noisy_diff": noisy_diff.astype(np.float32),
        "noisy_spec": noisy_spec.astype(np.float32),
        "clean_diff": clean_diff,
        "clean_spec": clean_spec,
        "eval_mask": eval_mask,
    }


def _build_module_g_edge_fixture(seed: int, width: int, height: int) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(seed)
    mid = width // 2

    view_z = np.full((height, width), 24.0, dtype=np.float32)
    view_z[:, mid:] = 88.0

    normals = np.zeros((height, width, 3), dtype=np.float32)
    normals[..., 2] = 1.0
    right_normal = np.array([0.55, 0.0, 0.84], dtype=np.float32)
    right_normal /= np.linalg.norm(right_normal)
    normals[:, mid:, :] = right_normal

    roughness = np.full((height, width), 0.18, dtype=np.float32)
    roughness[:, mid:] = 0.78
    packed_normal_roughness = pack_normal_roughness(normals, roughness)

    clean_diff = np.zeros((height, width, 4), dtype=np.float32)
    clean_spec = np.zeros((height, width, 4), dtype=np.float32)
    clean_diff[:, :mid, 0] = 0.16
    clean_diff[:, mid:, 0] = 0.86
    clean_diff[..., 1] = 0.02
    clean_diff[..., 2] = -0.01
    clean_diff[:, :mid, 3] = 0.24
    clean_diff[:, mid:, 3] = 0.66

    clean_spec[:, :mid, 0] = 0.12
    clean_spec[:, mid:, 0] = 0.72
    clean_spec[..., 1] = 0.03
    clean_spec[..., 2] = -0.02
    clean_spec[:, :mid, 3] = 0.20
    clean_spec[:, mid:, 3] = 0.74

    noisy_diff = clean_diff.copy()
    noisy_spec = clean_spec.copy()
    noisy_diff[..., :3] += rng.normal(0.0, 0.07,
                                      size=(height, width, 3)).astype(np.float32)
    noisy_spec[..., :3] += rng.normal(0.0, 0.08,
                                      size=(height, width, 3)).astype(np.float32)
    noisy_diff[..., 3] = np.clip(
        noisy_diff[..., 3] + rng.normal(0.0, 0.05, size=(height, width)), 0.0, 1.0)
    noisy_spec[..., 3] = np.clip(
        noisy_spec[..., 3] + rng.normal(0.0, 0.05, size=(height, width)), 0.0, 1.0)

    tile_mask = classify_tiles_reference(view_z, denoising_range=300.0)
    edge_mask = np.zeros((height, width), dtype=bool)
    edge_mask[:, max(mid - 3, 0): min(mid + 3, width)] = True

    return {
        "tile_mask": tile_mask,
        "packed_normal_roughness": packed_normal_roughness,
        "view_z": view_z,
        "noisy_diff": noisy_diff.astype(np.float32),
        "noisy_spec": noisy_spec.astype(np.float32),
        "clean_diff": clean_diff,
        "clean_spec": clean_spec,
        "edge_mask": edge_mask,
    }


def run_module_g_tests(seed: int = 59) -> ModuleGTestResults:
    denoising_range = 300.0

    flat_fixture = _build_module_g_flat_fixture(
        seed=seed, width=224, height=144)
    out_diff_flat, out_spec_flat, _, _, _ = blur_shader_equivalent(
        flat_fixture["tile_mask"],
        flat_fixture["packed_normal_roughness"],
        flat_fixture["view_z"],
        flat_fixture["noisy_diff"],
        flat_fixture["noisy_spec"],
        denoising_range,
        min_blur_radius=1.0,
        max_blur_radius=6.0,
        history_factor=0.0,
    )

    eval_mask = flat_fixture["eval_mask"]
    high_frequency_before = 0.5 * (
        _compute_laplacian_energy(
            flat_fixture["noisy_diff"][..., 0], eval_mask)
        + _compute_laplacian_energy(flat_fixture["noisy_spec"][..., 0], eval_mask)
    )
    high_frequency_after = 0.5 * (
        _compute_laplacian_energy(out_diff_flat[..., 0], eval_mask)
        + _compute_laplacian_energy(out_spec_flat[..., 0], eval_mask)
    )
    g1_high_frequency_reduction_ratio = high_frequency_before / \
        max(high_frequency_after, 1e-8)
    g1_pass = g1_high_frequency_reduction_ratio >= 1.8

    edge_fixture = _build_module_g_edge_fixture(
        seed=seed + 1, width=256, height=128)
    out_diff_edge, out_spec_edge, _, _, _ = blur_shader_equivalent(
        edge_fixture["tile_mask"],
        edge_fixture["packed_normal_roughness"],
        edge_fixture["view_z"],
        edge_fixture["noisy_diff"],
        edge_fixture["noisy_spec"],
        denoising_range,
        min_blur_radius=1.0,
        max_blur_radius=5.0,
        history_factor=0.25,
    )

    edge_mask = edge_fixture["edge_mask"]
    g2_edge_mse = 0.5 * (
        float(np.mean((out_diff_edge[..., 0][edge_mask] -
              edge_fixture["clean_diff"][..., 0][edge_mask]) ** 2))
        + float(np.mean((out_spec_edge[..., 0][edge_mask] -
                edge_fixture["clean_spec"][..., 0][edge_mask]) ** 2))
    )
    g2_pass = g2_edge_mse <= 0.020

    impulse_size = 129
    center = impulse_size // 2
    impulse_view_z = np.full(
        (impulse_size, impulse_size), 30.0, dtype=np.float32)
    impulse_normals = np.zeros(
        (impulse_size, impulse_size, 3), dtype=np.float32)
    impulse_normals[..., 2] = 1.0
    impulse_roughness = np.full(
        (impulse_size, impulse_size), 0.6, dtype=np.float32)
    impulse_packed_normal_roughness = pack_normal_roughness(
        impulse_normals, impulse_roughness)
    impulse_tile_mask = classify_tiles_reference(
        impulse_view_z, denoising_range)

    impulse_diff = np.zeros((impulse_size, impulse_size, 4), dtype=np.float32)
    impulse_spec = np.zeros((impulse_size, impulse_size, 4), dtype=np.float32)
    impulse_diff[center, center, 0] = 1.0
    impulse_spec[center, center, 0] = 1.0
    impulse_diff[..., 3] = 0.65
    impulse_spec[..., 3] = 0.80

    out_diff_low, out_spec_low, _, _, _ = blur_shader_equivalent(
        impulse_tile_mask,
        impulse_packed_normal_roughness,
        impulse_view_z,
        impulse_diff,
        impulse_spec,
        denoising_range,
        min_blur_radius=0.0,
        max_blur_radius=6.0,
        history_factor=0.0,
    )
    out_diff_high, out_spec_high, _, _, _ = blur_shader_equivalent(
        impulse_tile_mask,
        impulse_packed_normal_roughness,
        impulse_view_z,
        impulse_diff,
        impulse_spec,
        denoising_range,
        min_blur_radius=0.0,
        max_blur_radius=6.0,
        history_factor=1.0,
    )

    g3_low_history_radius = 0.5 * (
        _estimate_effective_radius_from_impulse(
            out_diff_low[..., 0], center, center)
        + _estimate_effective_radius_from_impulse(out_spec_low[..., 0], center, center)
    )
    g3_high_history_radius = 0.5 * (
        _estimate_effective_radius_from_impulse(
            out_diff_high[..., 0], center, center)
        + _estimate_effective_radius_from_impulse(out_spec_high[..., 0], center, center)
    )
    g3_pass = g3_high_history_radius <= g3_low_history_radius - 0.20

    return ModuleGTestResults(
        g1_pass=g1_pass,
        g1_high_frequency_reduction_ratio=g1_high_frequency_reduction_ratio,
        g2_pass=g2_pass,
        g2_edge_mse=g2_edge_mse,
        g3_pass=g3_pass,
        g3_low_history_radius=g3_low_history_radius,
        g3_high_history_radius=g3_high_history_radius,
    )


def _history_checksum(diff_history: np.ndarray, spec_history: np.ndarray) -> str:
    return hashlib.sha256(
        np.ascontiguousarray(diff_history).tobytes()
        + np.ascontiguousarray(spec_history).tobytes()
    ).hexdigest()


def run_module_h_tests(seed: int = 67) -> ModuleHTestResults:
    rng = np.random.default_rng(seed)
    denoising_range = 300.0
    height = 90
    width = 146
    frame_count = 6

    view_z = np.full((height, width), 38.0, dtype=np.float32)
    view_z[:8, :] = 0.0
    view_z[:, :6] = 0.0
    view_z[height - 7:, width - 9:] = denoising_range + 10.0

    normals = rng.normal(size=(height, width, 3)).astype(np.float32)
    normals[..., 2] += 1.5
    normals /= np.linalg.norm(normals, axis=2,
                              keepdims=True).astype(np.float32)
    roughness = rng.uniform(0.08, 0.92, size=(
        height, width)).astype(np.float32)
    packed_normal_roughness = pack_normal_roughness(normals, roughness)
    tile_mask = classify_tiles_reference(view_z, denoising_range)

    history_read_index = 0
    history_checksums = ["", ""]
    diff_history_ping = [
        np.zeros((height, width, 4), dtype=np.float32),
        np.zeros((height, width, 4), dtype=np.float32),
    ]
    spec_history_ping = [
        np.zeros((height, width, 4), dtype=np.float32),
        np.zeros((height, width, 4), dtype=np.float32),
    ]
    h1_pass = True

    h2_max_abs_diff = 0.0
    h2_rmse_sum = 0.0
    h2_frame_samples = 0

    for frame_index in range(frame_count):
        history_write_index = (history_read_index + 1) % 2
        preserved_index = history_read_index
        preserved_checksum = history_checksums[preserved_index]

        data1 = np.zeros((height, width, 4), dtype=np.float32)
        data1[..., 0] = np.clip(
            0.18 + 0.12 * frame_index +
            rng.normal(0.0, 0.03, size=(height, width)),
            0.0,
            1.0,
        ).astype(np.float32)

        diff_radiance = np.stack(
            (
                0.20 + 0.15 * np.sin((frame_index + 1) * 0.4) +
                rng.uniform(0.0, 0.45, size=(height, width)),
                0.08 + rng.uniform(0.0, 0.30, size=(height, width)),
                0.06 + rng.uniform(0.0, 0.22, size=(height, width)),
            ),
            axis=-1,
        ).astype(np.float32)
        spec_radiance = np.stack(
            (
                0.10 + rng.uniform(0.0, 0.35, size=(height, width)),
                0.05 + 0.10 * np.cos((frame_index + 1) * 0.7) +
                rng.uniform(0.0, 0.20, size=(height, width)),
                0.03 + rng.uniform(0.0, 0.20, size=(height, width)),
            ),
            axis=-1,
        ).astype(np.float32)
        diff_hit = rng.uniform(0.05, 0.95, size=(
            height, width)).astype(np.float32)
        spec_hit = rng.uniform(0.05, 0.95, size=(
            height, width)).astype(np.float32)

        blur_diff = pack_radiance_and_norm_hit_dist(diff_radiance, diff_hit)
        blur_spec = pack_radiance_and_norm_hit_dist(spec_radiance, spec_hit)

        (
            out_prev_normal_roughness,
            out_diff_history,
            out_spec_history,
            out_denoised_output,
        ) = post_blur_shader_equivalent(
            tile_mask,
            packed_normal_roughness,
            view_z,
            data1,
            blur_diff,
            blur_spec,
            denoising_range,
        )

        if not np.array_equal(out_prev_normal_roughness, packed_normal_roughness):
            h1_pass = False

        written_checksum = _history_checksum(
            out_diff_history, out_spec_history)
        readback_checksum = _history_checksum(
            out_diff_history, out_spec_history)
        if written_checksum != readback_checksum:
            h1_pass = False

        diff_history_ping[history_write_index] = out_diff_history.copy()
        spec_history_ping[history_write_index] = out_spec_history.copy()
        ping_readback_checksum = _history_checksum(
            diff_history_ping[history_write_index], spec_history_ping[history_write_index]
        )
        if ping_readback_checksum != written_checksum:
            h1_pass = False

        if frame_index >= 1:
            if preserved_checksum:
                preserved_readback_checksum = _history_checksum(
                    diff_history_ping[preserved_index], spec_history_ping[preserved_index]
                )
                if preserved_readback_checksum != preserved_checksum:
                    h1_pass = False

        history_checksums[history_write_index] = written_checksum
        history_read_index = history_write_index

        post_diff_radiance, _ = unpack_radiance_and_norm_hit_dist(
            out_diff_history)
        post_spec_radiance, _ = unpack_radiance_and_norm_hit_dist(
            out_spec_history)
        expected_rgb = np.maximum(post_diff_radiance + post_spec_radiance, 0.0)
        expected_output = np.concatenate(
            (expected_rgb, np.ones((height, width, 1), dtype=np.float32)), axis=-1
        )

        h2_abs_diff = np.abs(expected_output - out_denoised_output)
        h2_max_abs_diff = max(h2_max_abs_diff, float(np.max(h2_abs_diff)))
        h2_rmse_sum += float(np.sqrt(np.mean((expected_output -
                             out_denoised_output) ** 2)))
        h2_frame_samples += 1

    h1_last_history_checksum = history_checksums[history_read_index]
    h2_rmse = h2_rmse_sum / max(h2_frame_samples, 1)
    h2_pass = h2_max_abs_diff <= 1e-6 and h2_rmse <= 1e-7

    return ModuleHTestResults(
        h1_pass=h1_pass,
        h1_frames_validated=frame_count,
        h1_last_read_index=history_read_index,
        h1_last_history_checksum=h1_last_history_checksum,
        h2_pass=h2_pass,
        h2_max_abs_diff=h2_max_abs_diff,
        h2_rmse=h2_rmse,
    )


def run_module_i_tests(seed: int = 73) -> ModuleITestResults:
    rng = np.random.default_rng(seed)
    denoising_range = 300.0

    # I1: temporal flicker reduction on a static fixture.
    static_height = 72
    static_width = 112
    static_frame_count = 14
    static_max_frames = 32

    static_view_z = np.full(
        (static_height, static_width), 42.0, dtype=np.float32)
    static_normals = rng.normal(
        size=(static_height, static_width, 3)).astype(np.float32)
    static_normals[..., 2] += 2.0
    static_normals /= np.linalg.norm(
        static_normals, axis=2, keepdims=True).astype(np.float32)
    static_roughness = rng.uniform(
        0.08, 0.92, size=(static_height, static_width)).astype(np.float32)
    static_packed_normal_roughness = pack_normal_roughness(
        static_normals, static_roughness)
    static_tile_mask = classify_tiles_reference(static_view_z, denoising_range)
    static_spec_tracking = np.clip(
        0.35 + 0.55 * static_roughness, 0.0, 1.0).astype(np.float32)

    yy_static, xx_static = np.meshgrid(
        np.linspace(0.0, 1.0, static_height, dtype=np.float32),
        np.linspace(0.0, 1.0, static_width, dtype=np.float32),
        indexing="ij",
    )
    static_base_diff = np.stack(
        (
            0.16 + 0.24 * xx_static,
            0.05 + 0.14 * yy_static,
            0.04 + 0.12 * (1.0 - xx_static),
        ),
        axis=-1,
    ).astype(np.float32)
    static_base_spec = np.stack(
        (
            0.08 + 0.18 * (1.0 - yy_static),
            0.03 + 0.10 * xx_static,
            0.02 + 0.10 * yy_static,
        ),
        axis=-1,
    ).astype(np.float32)
    static_base_diff_hit = np.clip(
        0.28 + 0.22 * xx_static + 0.08 * yy_static, 0.04, 0.95).astype(np.float32)
    static_base_spec_hit = np.clip(
        0.34 + 0.18 * yy_static + 0.12 * (1.0 - xx_static), 0.04, 0.95).astype(np.float32)

    static_prev_diff_luma_ping = [
        np.zeros((static_height, static_width), dtype=np.float32),
        np.zeros((static_height, static_width), dtype=np.float32),
    ]
    static_prev_spec_luma_ping = [
        np.zeros((static_height, static_width), dtype=np.float32),
        np.zeros((static_height, static_width), dtype=np.float32),
    ]
    static_internal_ping = [
        np.zeros((static_height, static_width, 4), dtype=np.float32),
        np.zeros((static_height, static_width, 4), dtype=np.float32),
    ]
    static_history_read_index = 0

    static_flicker_off_frames = []
    static_flicker_on_frames = []

    for frame_index in range(static_frame_count):
        static_history_write_index = (static_history_read_index + 1) % 2

        diff_noise = rng.normal(
            0.0, 0.055, size=(static_height, static_width, 3)).astype(np.float32)
        spec_noise = rng.normal(
            0.0, 0.065, size=(static_height, static_width, 3)).astype(np.float32)
        diff_radiance = np.maximum(static_base_diff + diff_noise, 0.0)
        spec_radiance = np.maximum(static_base_spec + spec_noise, 0.0)
        diff_hit = np.clip(
            static_base_diff_hit +
            rng.normal(0.0, 0.035, size=(static_height, static_width)),
            0.0,
            1.0,
        ).astype(np.float32)
        spec_hit = np.clip(
            static_base_spec_hit +
            rng.normal(0.0, 0.040, size=(static_height, static_width)),
            0.0,
            1.0,
        ).astype(np.float32)
        diff_history = pack_radiance_and_norm_hit_dist(diff_radiance, diff_hit)
        spec_history = pack_radiance_and_norm_hit_dist(spec_radiance, spec_hit)

        data1 = np.zeros((static_height, static_width, 4), dtype=np.float32)
        history_length = min(frame_index + 1, static_max_frames)
        data1[..., 0] = np.clip(
            (history_length - 1) / max(static_max_frames - 1, 1), 0.0, 1.0)
        data1[..., 1] = 1.0 if frame_index > 0 else 0.0
        data1[..., 2] = history_length / float(static_max_frames)
        data1[..., 3] = static_spec_tracking

        data2 = np.zeros((static_height, static_width, 4), dtype=np.float32)
        xs = (np.arange(static_width, dtype=np.float32) + 0.5) / \
            float(static_width)
        ys = (np.arange(static_height, dtype=np.float32) + 0.5) / \
            float(static_height)
        data2[..., 0] = xs[None, :]
        data2[..., 1] = ys[:, None]
        data2[..., 2] = 0.0
        data2[..., 3] = 0.0 if frame_index > 0 else 1.0

        static_internal_ping[static_history_write_index][..., 0] = np.float32(
            history_length)
        static_internal_ping[static_history_write_index][..., 1] = np.float32(
            min(history_length, 4))
        static_internal_ping[static_history_write_index][...,
                                                         2] = np.float32(0.0)
        static_internal_ping[static_history_write_index][...,
                                                         3] = np.float32(0.0)

        (
            stabilized_diff_luma,
            stabilized_spec_luma,
            _,
            stabilized_output,
        ) = temporal_stabilization_shader_equivalent(
            static_tile_mask,
            static_packed_normal_roughness,
            static_view_z,
            data1,
            data2,
            diff_history,
            spec_history,
            static_spec_tracking,
            static_prev_diff_luma_ping[static_history_read_index],
            static_prev_spec_luma_ping[static_history_read_index],
            static_internal_ping[static_history_write_index],
            denoising_range,
            stabilization_strength=0.78,
            max_stabilized_frame_num=static_max_frames,
            history_available=frame_index > 0,
            enable_mv_patch=True,
        )

        static_diff_linear, _ = unpack_radiance_and_norm_hit_dist(diff_history)
        static_spec_linear, _ = unpack_radiance_and_norm_hit_dist(spec_history)
        static_baseline_rgb = np.maximum(
            static_diff_linear + static_spec_linear, 0.0)

        static_baseline_luma = (
            0.2126 * static_baseline_rgb[..., 0]
            + 0.7152 * static_baseline_rgb[..., 1]
            + 0.0722 * static_baseline_rgb[..., 2]
        ).astype(np.float32)
        static_stabilized_luma = (
            0.2126 * stabilized_output[..., 0]
            + 0.7152 * stabilized_output[..., 1]
            + 0.0722 * stabilized_output[..., 2]
        ).astype(np.float32)

        static_flicker_off_frames.append(static_baseline_luma)
        static_flicker_on_frames.append(static_stabilized_luma)

        static_prev_diff_luma_ping[static_history_write_index] = stabilized_diff_luma.copy(
        )
        static_prev_spec_luma_ping[static_history_write_index] = stabilized_spec_luma.copy(
        )
        static_history_read_index = static_history_write_index

    static_off_stack = np.stack(static_flicker_off_frames, axis=0)
    static_on_stack = np.stack(static_flicker_on_frames, axis=0)
    static_off_std = np.std(static_off_stack, axis=0)
    static_on_std = np.std(static_on_stack, axis=0)
    i1_flicker_reduction_ratio = float(
        np.mean(static_off_std) / max(float(np.mean(static_on_std)), 1e-8)
    )
    i1_pass = i1_flicker_reduction_ratio >= 1.35

    # I2: moving-object ghosting guard in trailing region.
    moving_height = 80
    moving_width = 144
    moving_frame_count = 10
    moving_velocity = 6
    moving_max_frames = 32

    moving_view_z = np.full(
        (moving_height, moving_width), 30.0, dtype=np.float32)
    moving_normals = np.zeros(
        (moving_height, moving_width, 3), dtype=np.float32)
    moving_normals[..., 2] = 1.0
    moving_roughness = np.full(
        (moving_height, moving_width), 0.16, dtype=np.float32)
    moving_packed_normal_roughness = pack_normal_roughness(
        moving_normals, moving_roughness)
    moving_tile_mask = classify_tiles_reference(moving_view_z, denoising_range)

    moving_prev_diff_luma_ping = [
        np.zeros((moving_height, moving_width), dtype=np.float32),
        np.zeros((moving_height, moving_width), dtype=np.float32),
    ]
    moving_prev_spec_luma_ping = [
        np.zeros((moving_height, moving_width), dtype=np.float32),
        np.zeros((moving_height, moving_width), dtype=np.float32),
    ]
    moving_internal_ping = [
        np.zeros((moving_height, moving_width, 4), dtype=np.float32),
        np.zeros((moving_height, moving_width, 4), dtype=np.float32),
    ]
    moving_history_read_index = 0

    trailing_errors = []
    previous_object_mask = np.zeros((moving_height, moving_width), dtype=bool)

    object_half_w = 9
    object_half_h = 8
    object_center_y = moving_height // 2
    object_start_x = 20

    for frame_index in range(moving_frame_count):
        moving_history_write_index = (moving_history_read_index + 1) % 2

        center_x = object_start_x + frame_index * moving_velocity
        x0 = max(center_x - object_half_w, 0)
        x1 = min(center_x + object_half_w, moving_width)
        y0 = max(object_center_y - object_half_h, 0)
        y1 = min(object_center_y + object_half_h, moving_height)

        object_mask = np.zeros((moving_height, moving_width), dtype=bool)
        object_mask[y0:y1, x0:x1] = True

        clean_diff = np.zeros(
            (moving_height, moving_width, 3), dtype=np.float32)
        clean_spec = np.zeros(
            (moving_height, moving_width, 3), dtype=np.float32)
        clean_diff[..., 0] = 0.06
        clean_diff[..., 1] = 0.04
        clean_diff[..., 2] = 0.03
        clean_spec[..., 0] = 0.02
        clean_spec[..., 1] = 0.02
        clean_spec[..., 2] = 0.02

        clean_diff[object_mask, 0] = 0.12
        clean_diff[object_mask, 1] = 0.08
        clean_diff[object_mask, 2] = 0.05
        clean_spec[object_mask, 0] = 2.20
        clean_spec[object_mask, 1] = 1.65
        clean_spec[object_mask, 2] = 1.20

        noisy_diff = np.maximum(
            clean_diff + rng.normal(0.0, 0.03,
                                    size=clean_diff.shape).astype(np.float32),
            0.0,
        )
        noisy_spec = np.maximum(
            clean_spec + rng.normal(0.0, 0.06,
                                    size=clean_spec.shape).astype(np.float32),
            0.0,
        )
        diff_hit = np.full((moving_height, moving_width),
                           0.22, dtype=np.float32)
        spec_hit = np.full((moving_height, moving_width),
                           0.18, dtype=np.float32)
        diff_hit[object_mask] = 0.50
        spec_hit[object_mask] = 0.72

        diff_history = pack_radiance_and_norm_hit_dist(noisy_diff, diff_hit)
        spec_history = pack_radiance_and_norm_hit_dist(noisy_spec, spec_hit)
        spec_tracking = np.where(object_mask, 0.90, 0.18).astype(np.float32)

        data1 = np.zeros((moving_height, moving_width, 4), dtype=np.float32)
        history_length = min(frame_index + 1, moving_max_frames)
        data1[..., 0] = np.clip(
            (history_length - 1) / max(moving_max_frames - 1, 1), 0.0, 1.0)
        data1[..., 1] = 1.0 if frame_index > 0 else 0.0
        data1[..., 2] = history_length / float(moving_max_frames)
        data1[..., 3] = spec_tracking

        xs = (np.arange(moving_width, dtype=np.float32) + 0.5) / \
            float(moving_width)
        ys = (np.arange(moving_height, dtype=np.float32) + 0.5) / \
            float(moving_height)
        data2 = np.zeros((moving_height, moving_width, 4), dtype=np.float32)
        data2[..., 0] = xs[None, :]
        data2[..., 1] = ys[:, None]
        data2[..., 2] = 0.01
        data2[..., 3] = 0.0 if frame_index > 0 else 1.0

        shifted_mask = object_mask | previous_object_mask
        pixel_shift = float(moving_velocity) / float(moving_width)
        data2[shifted_mask, 0] = np.clip(
            data2[shifted_mask, 0] - pixel_shift, 0.0, 1.0)
        data2[shifted_mask, 2] = np.where(
            previous_object_mask[shifted_mask], 0.36, 0.18).astype(np.float32)

        moving_internal_ping[moving_history_write_index][..., 0] = np.float32(
            history_length)
        moving_internal_ping[moving_history_write_index][..., 1] = np.float32(
            min(history_length, 4))
        moving_internal_ping[moving_history_write_index][...,
                                                         2] = np.float32(0.0)
        moving_internal_ping[moving_history_write_index][...,
                                                         3] = np.float32(0.0)

        (
            moving_stabilized_diff_luma,
            moving_stabilized_spec_luma,
            _,
            stabilized_output,
        ) = temporal_stabilization_shader_equivalent(
            moving_tile_mask,
            moving_packed_normal_roughness,
            moving_view_z,
            data1,
            data2,
            diff_history,
            spec_history,
            spec_tracking,
            moving_prev_diff_luma_ping[moving_history_read_index],
            moving_prev_spec_luma_ping[moving_history_read_index],
            moving_internal_ping[moving_history_write_index],
            denoising_range,
            stabilization_strength=0.82,
            max_stabilized_frame_num=moving_max_frames,
            history_available=frame_index > 0,
            enable_mv_patch=True,
        )

        clean_diff_packed = pack_radiance_and_norm_hit_dist(
            clean_diff, diff_hit)
        clean_spec_packed = pack_radiance_and_norm_hit_dist(
            clean_spec, spec_hit)
        clean_diff_linear, _ = unpack_radiance_and_norm_hit_dist(
            clean_diff_packed)
        clean_spec_linear, _ = unpack_radiance_and_norm_hit_dist(
            clean_spec_packed)
        clean_rgb = np.maximum(clean_diff_linear + clean_spec_linear, 0.0)
        clean_luma = (
            0.2126 * clean_rgb[..., 0]
            + 0.7152 * clean_rgb[..., 1]
            + 0.0722 * clean_rgb[..., 2]
        ).astype(np.float32)
        stabilized_luma = (
            0.2126 * stabilized_output[..., 0]
            + 0.7152 * stabilized_output[..., 1]
            + 0.0722 * stabilized_output[..., 2]
        ).astype(np.float32)

        trailing_mask = previous_object_mask & (~object_mask)
        if np.any(trailing_mask):
            trailing_error = float(
                np.mean(np.abs(stabilized_luma[trailing_mask] - clean_luma[trailing_mask])))
            trailing_errors.append(trailing_error)

        moving_prev_diff_luma_ping[moving_history_write_index] = moving_stabilized_diff_luma.copy(
        )
        moving_prev_spec_luma_ping[moving_history_write_index] = moving_stabilized_spec_luma.copy(
        )
        moving_history_read_index = moving_history_write_index
        previous_object_mask = object_mask

    i2_trailing_error = float(np.mean(trailing_errors)
                              ) if trailing_errors else 0.0
    i2_pass = i2_trailing_error <= 0.10

    return ModuleITestResults(
        i1_pass=i1_pass,
        i1_flicker_reduction_ratio=i1_flicker_reduction_ratio,
        i2_pass=i2_pass,
        i2_trailing_error=i2_trailing_error,
    )


def run_module_j_tests(seed: int = 79) -> ModuleJTestResults:
    rng = np.random.default_rng(seed)
    denoising_range = 300.0
    height = 84
    width = 132

    view_z = np.full((height, width), 36.0, dtype=np.float32)
    view_z[:4, :] = 0.0
    view_z[:, :3] = denoising_range + 5.0

    noisy_diff_radiance = np.stack(
        (
            0.12 + rng.uniform(0.0, 0.40, size=(height, width)),
            0.06 + rng.uniform(0.0, 0.25, size=(height, width)),
            0.04 + rng.uniform(0.0, 0.20, size=(height, width)),
        ),
        axis=-1,
    ).astype(np.float32)
    noisy_spec_radiance = np.stack(
        (
            0.08 + rng.uniform(0.0, 0.35, size=(height, width)),
            0.04 + rng.uniform(0.0, 0.22, size=(height, width)),
            0.03 + rng.uniform(0.0, 0.18, size=(height, width)),
        ),
        axis=-1,
    ).astype(np.float32)
    noisy_diff_hit = rng.uniform(0.05, 0.95, size=(height, width)).astype(np.float32)
    noisy_spec_hit = rng.uniform(0.05, 0.95, size=(height, width)).astype(np.float32)
    noisy_diff_signal = pack_radiance_and_norm_hit_dist(noisy_diff_radiance, noisy_diff_hit)
    noisy_spec_signal = pack_radiance_and_norm_hit_dist(noisy_spec_radiance, noisy_spec_hit)

    base_denoised_output = np.zeros((height, width, 4), dtype=np.float32)
    base_denoised_output[..., :3] = np.stack(
        (
            0.22 + rng.uniform(0.0, 0.50, size=(height, width)),
            0.14 + rng.uniform(0.0, 0.36, size=(height, width)),
            0.10 + rng.uniform(0.0, 0.30, size=(height, width)),
        ),
        axis=-1,
    ).astype(np.float32)
    base_denoised_output[..., 3] = 1.0

    noisy_diff_linear, _ = unpack_radiance_and_norm_hit_dist(noisy_diff_signal)
    noisy_spec_linear, _ = unpack_radiance_and_norm_hit_dist(noisy_spec_signal)
    noisy_rgb = np.maximum(noisy_diff_linear + noisy_spec_linear, 0.0).astype(np.float32)
    valid_view_mask = (
        np.isfinite(view_z) & (view_z > 0.0) & (view_z <= denoising_range)
    )

    # J1: split boundary correctness.
    j1_max_abs_diff = 0.0
    for split_ratio in (0.20, 0.50, 0.80):
        split_output = split_screen_shader_equivalent(
            view_z,
            noisy_diff_signal,
            noisy_spec_signal,
            base_denoised_output,
            denoising_range,
            split_ratio,
        )

        expected = base_denoised_output.copy()
        x_coords = (np.arange(width, dtype=np.float32) + 0.5) / float(width)
        split_mask = np.broadcast_to(x_coords[None, :] <= split_ratio, (height, width))
        split_valid = split_mask & valid_view_mask
        split_invalid = split_mask & (~valid_view_mask)

        expected[split_valid, :3] = noisy_rgb[split_valid]
        expected[split_invalid, :3] = 0.0
        expected[split_mask, 3] = 1.0

        j1_max_abs_diff = max(j1_max_abs_diff, float(np.max(np.abs(split_output - expected))))

    j1_pass = j1_max_abs_diff <= 1e-6

    # J2: validation non-regression for known-bad synthetic states.
    tile_mask = classify_tiles_reference(view_z, denoising_range)

    data1 = np.zeros((height, width, 4), dtype=np.float32)
    data2 = np.zeros((height, width, 4), dtype=np.float32)
    data1[..., 1] = 1.0
    data1[..., 2] = 0.80
    data2[..., 2] = 0.04

    diff_history_signal = noisy_diff_signal.copy()
    spec_history_signal = noisy_spec_signal.copy()
    bad_mask = np.zeros((height, width), dtype=bool)

    # Disocclusion alerts.
    data2[14:28, 18:40, 3] = 1.0
    bad_mask[14:28, 18:40] = True

    # Reprojection-invalid alerts with non-trivial history length.
    data1[34:50, 54:86, 1] = 0.0
    data1[34:50, 54:86, 2] = 0.95
    bad_mask[34:50, 54:86] = True

    # Invalid hit-distance alerts.
    diff_history_signal[52:70, 92:118, 3] = 1.35
    spec_history_signal[52:70, 92:118, 3] = -0.20
    bad_mask[52:70, 92:118] = True

    # Large depth-delta alerts.
    data2[20:36, 96:124, 2] = 0.72
    bad_mask[20:36, 96:124] = True

    validation_output = validation_shader_equivalent(
        tile_mask,
        view_z,
        data1,
        data2,
        diff_history_signal,
        spec_history_signal,
        denoising_range,
        disocclusion_threshold=0.5,
        depth_delta_threshold=0.2,
        history_threshold=0.15,
    )

    alert_strength = np.max(validation_output[..., :3], axis=-1)
    alert_mask = alert_strength > 0.05

    tile_pixel_mask = np.repeat(
        np.repeat(tile_mask, REBLUR_TILE_SIZE, axis=0), REBLUR_TILE_SIZE, axis=1
    )[:height, :width]
    eligible_mask = (tile_pixel_mask == 0) & valid_view_mask
    clean_mask = eligible_mask & (~bad_mask)

    bad_count = int(np.count_nonzero(bad_mask))
    clean_count = int(np.count_nonzero(clean_mask))
    detected_bad = int(np.count_nonzero(alert_mask & bad_mask))
    detected_clean = int(np.count_nonzero(alert_mask & clean_mask))

    j2_detection_ratio = float(detected_bad / max(bad_count, 1))
    j2_false_positive_ratio = float(detected_clean / max(clean_count, 1))
    j2_pass = j2_detection_ratio >= 0.99 and j2_false_positive_ratio <= 0.02

    return ModuleJTestResults(
        j1_pass=j1_pass,
        j1_max_abs_diff=j1_max_abs_diff,
        j2_pass=j2_pass,
        j2_detection_ratio=j2_detection_ratio,
        j2_false_positive_ratio=j2_false_positive_ratio,
    )


def main() -> int:
    module_a_results = run_module_a_tests()
    module_b_results = run_module_b_tests()
    module_c_results = run_module_c_tests()
    module_d_results = run_module_d_tests()
    module_e_results = run_module_e_tests()
    module_f_results = run_module_f_tests()
    module_g_results = run_module_g_tests()
    module_h_results = run_module_h_tests()
    module_i_results = run_module_i_tests()
    module_j_results = run_module_j_tests()

    print(
        "Module A results: "
        f"A1(pass={module_a_results.a1_pass}, normal_rmse={module_a_results.a1_normal_rmse:.8f}, "
        f"roughness_rmse={module_a_results.a1_roughness_rmse:.8f}, "
        f"radiance_rmse={module_a_results.a1_radiance_rmse:.8f}, "
        f"norm_hit_rmse={module_a_results.a1_norm_hit_rmse:.8f}); "
        f"A2(pass={module_a_results.a2_pass}, mean_reprojection_error_px={module_a_results.a2_mean_reprojection_error_px:.6f}, "
        f"max_reprojection_error_px={module_a_results.a2_max_reprojection_error_px:.6f}); "
        f"A3(pass={module_a_results.a3_pass}, valid_ratio={module_a_results.a3_guide_valid_ratio:.6%})",
        flush=True,
    )
    print(
        "Module B results: "
        f"B1(pass={module_b_results.b1_pass}, precision={module_b_results.b1_precision:.6f}, "
        f"recall={module_b_results.b1_recall:.6f}); "
        f"B2(pass={module_b_results.b2_pass}, unique_hash_count={module_b_results.b2_unique_hash_count}, "
        f"hash={module_b_results.b2_reference_hash})",
        flush=True,
    )
    print(
        "Module C results: "
        f"C1(pass={module_c_results.c1_pass}, invalid_rmse_norm={module_c_results.c1_invalid_rmse_norm:.6f}); "
        f"C2(pass={module_c_results.c2_pass}, valid_luma_abs_error={module_c_results.c2_valid_luma_abs_error:.8f}); "
        f"C3(pass={module_c_results.c3_pass}, rmse_3x3_norm={module_c_results.c3_rmse_3x3_norm:.6f}, "
        f"rmse_5x5_norm={module_c_results.c3_rmse_5x5_norm:.6f})",
        flush=True,
    )
    print(
        "Module D results: "
        f"D1(pass={module_d_results.d1_pass}, variance_reduction_ratio={module_d_results.d1_variance_reduction_ratio:.6f}); "
        f"D2(pass={module_d_results.d2_pass}, edge_leakage={module_d_results.d2_edge_leakage:.6f}); "
        f"D3(pass={module_d_results.d3_pass}, jitter_reduction_ratio={module_d_results.d3_jitter_reduction_ratio:.6f}, "
        f"baseline_jitter={module_d_results.d3_baseline_jitter:.6f}, "
        f"tracking_jitter={module_d_results.d3_tracking_jitter:.6f})",
        flush=True,
    )
    print(
        "Module E results: "
        f"E1(pass={module_e_results.e1_pass}, history_monotonic={module_e_results.e1_history_monotonic}, "
        f"final_mean_history_length={module_e_results.e1_final_mean_history_length:.6f}, "
        f"history_cap={module_e_results.e1_history_cap:.6f}); "
        f"E2(pass={module_e_results.e2_pass}, reset_ratio={module_e_results.e2_reset_ratio:.6%}); "
        f"E3(pass={module_e_results.e3_pass}, ghosting_metric={module_e_results.e3_ghosting_metric:.6f})",
        flush=True,
    )
    print(
        "Module F results: "
        f"F1(pass={module_f_results.f1_pass}, p99_suppression_ratio={module_f_results.f1_p99_suppression_ratio:.6f}); "
        f"F2(pass={module_f_results.f2_pass}, median_drift_ratio={module_f_results.f2_median_drift_ratio:.6%}); "
        f"F3(pass={module_f_results.f3_pass}, recovery_frame={module_f_results.f3_recovery_frame}, "
        f"recovery_psnr={module_f_results.f3_recovery_psnr:.6f}, "
        f"recovery_ssim={module_f_results.f3_recovery_ssim:.6f})",
        flush=True,
    )
    print(
        "Module G results: "
        f"G1(pass={module_g_results.g1_pass}, high_frequency_reduction_ratio={module_g_results.g1_high_frequency_reduction_ratio:.6f}); "
        f"G2(pass={module_g_results.g2_pass}, edge_mse={module_g_results.g2_edge_mse:.6f}); "
        f"G3(pass={module_g_results.g3_pass}, low_history_radius={module_g_results.g3_low_history_radius:.6f}, "
        f"high_history_radius={module_g_results.g3_high_history_radius:.6f})",
        flush=True,
    )
    print(
        "Module H results: "
        f"H1(pass={module_h_results.h1_pass}, frames_validated={module_h_results.h1_frames_validated}, "
        f"last_read_index={module_h_results.h1_last_read_index}, "
        f"last_history_checksum={module_h_results.h1_last_history_checksum}); "
        f"H2(pass={module_h_results.h2_pass}, max_abs_diff={module_h_results.h2_max_abs_diff:.8f}, "
        f"rmse={module_h_results.h2_rmse:.8f})",
        flush=True,
    )
    print(
        "Module I results: "
        f"I1(pass={module_i_results.i1_pass}, flicker_reduction_ratio={module_i_results.i1_flicker_reduction_ratio:.6f}); "
        f"I2(pass={module_i_results.i2_pass}, trailing_error={module_i_results.i2_trailing_error:.6f})",
        flush=True,
    )
    print(
        "Module J results: "
        f"J1(pass={module_j_results.j1_pass}, max_abs_diff={module_j_results.j1_max_abs_diff:.8f}); "
        f"J2(pass={module_j_results.j2_pass}, detection_ratio={module_j_results.j2_detection_ratio:.6%}, "
        f"false_positive_ratio={module_j_results.j2_false_positive_ratio:.6%})",
        flush=True,
    )
    return (
        0
        if module_a_results.passed
        and module_b_results.passed
        and module_c_results.passed
        and module_d_results.passed
        and module_e_results.passed
        and module_f_results.passed
        and module_g_results.passed
        and module_h_results.passed
        and module_i_results.passed
        and module_j_results.passed
        else 1
    )


if __name__ == "__main__":
    raise SystemExit(main())
