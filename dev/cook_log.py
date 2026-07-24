"""Cook-activity patterns of the app log, shared by every log gate."""


def count_cook_activity(lines):
    """Returns (artifact hits, on-the-fly cooks). cooker_request fixtures cook
    at runtime by design and do not count, nor do usd_export assets: the round
    trip test reloads content it just exported, which was never packaged."""
    hits = 0
    cooked = 0
    for line in lines:
        cooked += ("] cooking " in line
                   and "cooking cooker_request_test" not in line
                   and "usd_export/" not in line)
        hits += "cook artifact hit" in line
    return hits, cooked
