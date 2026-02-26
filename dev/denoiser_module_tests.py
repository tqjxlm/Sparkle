"""Deterministic module-level tests for standalone ReBLUR integration."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from denoiser_metrics import (
    REBLUR_HIT_DIST_RECONSTRUCTION_AREA_3X3,
    REBLUR_HIT_DIST_RECONSTRUCTION_AREA_5X5,
    REBLUR_TILE_SIZE,
    binary_precision_recall,
    classify_tiles_reference,
    classify_tiles_shader_equivalent,
    compute_motion_vectors_pixels,
    get_norm_hit_distance,
    hash_tile_mask,
    normalized_rmse,
    pack_normal_roughness,
    pack_radiance_and_norm_hit_dist,
    project_world_to_pixel,
    reconstruct_hit_distance_shader_equivalent,
    rmse,
    unpack_normal_roughness,
    unpack_radiance_and_norm_hit_dist,
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


def main() -> int:
    module_a_results = run_module_a_tests()
    module_b_results = run_module_b_tests()
    module_c_results = run_module_c_tests()

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
    return 0 if module_a_results.passed and module_b_results.passed and module_c_results.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
