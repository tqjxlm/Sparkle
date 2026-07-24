"""Cook-activity patterns of the app log, shared by every log gate."""


def count_cook_activity(lines):
    """Returns (artifact hits, on-the-fly cooks). cooker_request fixtures cook
    at runtime by design and do not count. A relocated sky source (usd export)
    re-derives only its cheap fp16 master under the new name; every derived
    artifact must alias to packaged content by hash."""
    hits = 0
    cooked = 0
    for line in lines:
        cooked += ("] cooking " in line
                   and "cooking cooker_request_test" not in line
                   and not ("] cooking skylight:" in line and "usd_export/" in line))
        hits += "cook artifact hit" in line
    return hits, cooked
