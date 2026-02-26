"""Deterministic module-level tests for standalone ReBLUR integration."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from denoiser_metrics import (
    compute_motion_vectors_pixels,
    get_norm_hit_distance,
    pack_normal_roughness,
    pack_radiance_and_norm_hit_dist,
    project_world_to_pixel,
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


def main() -> int:
    results = run_module_a_tests()
    print(
        "Module A results: "
        f"A1(pass={results.a1_pass}, normal_rmse={results.a1_normal_rmse:.8f}, "
        f"roughness_rmse={results.a1_roughness_rmse:.8f}, "
        f"radiance_rmse={results.a1_radiance_rmse:.8f}, "
        f"norm_hit_rmse={results.a1_norm_hit_rmse:.8f}); "
        f"A2(pass={results.a2_pass}, mean_reprojection_error_px={results.a2_mean_reprojection_error_px:.6f}, "
        f"max_reprojection_error_px={results.a2_max_reprojection_error_px:.6f}); "
        f"A3(pass={results.a3_pass}, valid_ratio={results.a3_guide_valid_ratio:.6%})",
        flush=True,
    )
    return 0 if results.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
