#!/usr/bin/env python3
"""Shared REBLUR test-setting helpers."""

import os
import re


def get_default_max_accumulated_frame_num(project_root):
    header_path = os.path.join(
        project_root,
        "libraries",
        "include",
        "renderer",
        "denoiser",
        "ReblurDenoisingPipeline.h",
    )
    with open(header_path, "r", encoding="utf-8") as handle:
        contents = handle.read()

    match = re.search(r"max_accumulated_frame_num\s*=\s*(\d+)", contents)
    if match is None:
        raise RuntimeError(
            f"Failed to parse max_accumulated_frame_num from {header_path}"
        )

    return float(match.group(1))
