"""Render a render graph dump as one self-contained static HTML page.

The page is a pass x resource grid: one row per pass in execution order, one
column per resource (texture, buffer or acceleration structure) in dump order. A
cell shows the pass's access to the resource and details it on hover: accesses,
attachment load/store with reasons, barriers with layouts. Shaded cells span
each resource's lifetime, from its first to its last use. A band spans the rows
of each physical pass, the passes recorded as one render pass, and details its
members, color bytes per pixel and every rule the next pass breaks on hover. A
badge marks memoryless transients. Below the grid, the opportunity report lists
the physical passes by the bytes their attachment loads and stores move.
"""

import argparse
import html
import json
import os
from collections import defaultdict

WRITES = {"ColorWrite", "DepthWrite", "StorageWrite", "CopyDst", "AccelerationStructureBuild"}
ATTACHMENTS = {"ColorWrite", "DepthWrite", "DepthTest"}
LOAD_OPS = {"Load": "ld", "Clear": "clr", "DontCare": "–"}
STORE_OPS = {"Store": "st", "DontCare": "–"}

STYLE = """
body { font: 13px system-ui, sans-serif; margin: 16px; color: #222; background: #fff; }
table { border-collapse: collapse; }
th, td { border: 1px solid #ddd; padding: 2px 4px; text-align: center; white-space: nowrap; }
thead th { position: sticky; top: 0; background: #fff; vertical-align: bottom; }
th.resource span { writing-mode: vertical-rl; transform: rotate(180deg); }
.pass { text-align: left; }
.imported { font-style: italic; color: #666; }
.life { background: #eef3fb; }
.r { background: #cfe8cf; }
.w { background: #f7c9a9; }
.rw { background: #e3c8f0; }
.barrier { box-shadow: inset 0 3px #d33; }
.barrier-after { box-shadow: inset 0 -3px #d33; }
.barrier.barrier-after { box-shadow: inset 0 3px #d33, inset 0 -3px #d33; }
tr.culled { opacity: 0.45; }
tr.band-start > td { border-top: 2px solid #888; }
td.physical { background: #f4f4f4; }
td.over-budget { color: #c60; font-weight: bold; }
td small { display: block; font-size: 10px; }
th.resource span.badge { writing-mode: horizontal-tb; transform: none; display: inline-block; margin-top: 2px; padding: 0 3px;
         border-radius: 3px; background: #2a7; color: #fff; font-size: 10px; }
"""

LEGEND = ("R read, W write, RW both; C color / D depth attachment with its physical pass's load (ld load, clr clear,"
          " – don't care) / store (st store, – don't care); red top edge: barrier before the pass, red bottom edge:"
          " barrier after it; shaded: resource lifetime; italic: imported; M: memoryless transient, kept in tile memory;"
          " P: physical pass, a band over its members, with its GPU time; bold orange: over the tile budget."
          " Hover a cell or header for details.")


def resource_of(entry):
    """The resource an entry names, with its subresources unless it covers every one."""
    subresources = entry.get("subresources")
    return f"{entry['resource']}[{subresources}]" if subresources else entry["resource"]


def describe_access(access):
    clear = " clear" if access.get("clear") else ""
    pixel_local = ""
    if "lowered_reason" in access:
        pixel_local = f" lowered from PixelLocalRead slot {access['pixel_local_slot']}: {access['lowered_reason']}"
    elif "pixel_local_slot" in access:
        pixel_local = f" slot {access['pixel_local_slot']}"
    return f"access {resource_of(access)} {access['access']}{clear}{pixel_local}"


def describe_barrier(barrier):
    # memory barriers have no layouts
    layouts = f" {barrier['from_layout']}->{barrier['to_layout']}" if "from_layout" in barrier else ""
    in_rendering = " in rendering" if barrier.get("in_rendering") else ""
    return f"barrier{in_rendering} {resource_of(barrier)}{layouts} [{barrier['from']} -> {barrier['to']}]"


def describe_barrier_after(barrier):
    """A memory barrier recorded after the pass, e.g. one that makes a buffer the host reads visible to it."""
    return f"barrier after {resource_of(barrier)} [{barrier['from']} -> {barrier['to']}]"


def describe_attachment(attachment):
    return (f"attachment {resource_of(attachment)} slot {attachment['slot']}:"
            f" {attachment['load']} ({attachment['load_reason']})"
            f" / {attachment['store']} ({attachment['store_reason']})")


def describe_physical_pass(physical, passes):
    """A physical pass's members joined by '+', and every rule the next live pass breaks, the first one why it does
    not join."""
    line = "physical " + "+".join(passes[member]["name"] for member in physical["members"])
    if "breaks" in physical:
        line += ", break " + ", ".join(physical["breaks"])
    return line


def access_flags(access):
    """The access bits of a dumped access, e.g. StorageRead|StorageWrite(Compute) -> {StorageRead, StorageWrite}."""
    return set(access.split("(")[0].split("|"))


def cell(graph_pass, name, uses):
    flags = set().union(*(access_flags(access["access"]) for access in uses["accesses"]))
    kind = ("R" if flags - WRITES else "") + ("W" if flags & WRITES else "")
    label = kind
    if uses["attachments"]:
        attachment = uses["attachments"][0]
        label = (f"{'D' if attachment['slot'] == 'depth' else 'C'}"
                 f"<small>{LOAD_OPS[attachment['load']]}/{STORE_OPS[attachment['store']]}</small>")

    details = [f"{graph_pass['name']} / {name}"]
    details += [describe_access(access) for access in uses["accesses"]]
    details += [describe_attachment(attachment) for attachment in uses["attachments"]]
    details += [describe_barrier(barrier) for barrier in uses["barriers"]]
    details += [describe_barrier_after(barrier) for barrier in uses["barriers_after"]]
    tooltip = html.escape("\n".join(details))
    classes = kind.lower() + (" barrier" if uses["barriers"] else "")
    classes += " barrier-after" if uses["barriers_after"] else ""
    return f'<td class="{classes}" title="{tooltip}">{label}</td>'


def resource_header(resource, passes):
    details = [resource["name"], f"{resource['kind']} {resource['type']}"]
    if "format" in resource:
        size = f" {resource['width']}x{resource['height']}" if "width" in resource else ""
        details.append(f"{resource['format']} {resource['size_class']}{size}")
    if "physical" in resource:
        details.append(f"physical image {resource['physical']}")
    elif resource["kind"] == "Transient":
        details.append("no image")
    if resource.get("memoryless"):
        details.append(f"memoryless, backed by a {resource['backing']} image")
    if "first_use" in resource:
        first, last = passes[resource["first_use"]]["name"], passes[resource["last_use"]]["name"]
        details += [f"used {first}..{last}", f"usage {resource['usage']}"]
    css = "resource imported" if resource["kind"] == "Imported" else "resource"
    tooltip = html.escape("\n".join(details))
    badge = '<br><span class="badge">M</span>' if resource.get("memoryless") else ""
    return f'<th class="{css}" title="{tooltip}"><span>{html.escape(resource["name"])}</span>{badge}</th>'


def pass_attachments(graph_pass, physical_passes):
    """The attachments of the pass's physical pass that the pass attaches."""
    if graph_pass["culled"]:
        return []
    attached = {resource_of(access) for access in graph_pass["accesses"]
                if access_flags(access["access"]) & ATTACHMENTS}
    return [attachment for attachment in physical_passes[graph_pass["physical_pass"]]["attachments"]
            if resource_of(attachment) in attached]


def uses_by_resource(graph_pass, physical_passes):
    """Groups a pass's accesses, attachments and barriers by the resource they name."""
    entries = {"accesses": graph_pass["accesses"], "attachments": pass_attachments(graph_pass, physical_passes),
               "barriers": graph_pass["barriers"], "barriers_after": graph_pass.get("barriers_after", [])}
    uses = defaultdict(lambda: {key: [] for key in entries})
    for key, values in entries.items():
        for entry in values:
            uses[entry["resource"]][key].append(entry)
    return uses


def physical_cells(index, passes, physical_passes, timed):
    """The physical pass cells of the pass's row: spanning the rows of a physical pass from its first member to its
    last, detailed on hover, on the first member's row; none on the other rows of the span; empty on a culled row
    outside every span."""
    for physical_index, physical in enumerate(physical_passes):
        first, last = physical["members"][0], physical["members"][-1]
        if index == first:
            break
        if first < index <= last:
            return ""
    else:
        return "<td></td>" + ("<td></td>" if timed else "")

    details = [describe_physical_pass(physical, passes)]
    if "color_bytes_per_pixel" in physical:
        over = ", over the tile budget" if physical.get("over_tile_budget") else ""
        details.append(f"color {physical['color_bytes_per_pixel']} B/pixel{over}")
    details += [describe_attachment(attachment) for attachment in physical["attachments"]]
    span = f' rowspan="{last - first + 1}"' if last > first else ""
    css = "physical over-budget" if physical.get("over_tile_budget") else "physical"
    cells = f'<td class="{css}"{span} title="{html.escape(chr(10).join(details))}">P{physical_index}</td>'
    if timed:
        gpu_ms = f"{physical['gpu_ms']:.3f}" if "gpu_ms" in physical else ""
        cells += f'<td class="physical"{span}>{gpu_ms}</td>'
    return cells


def summarize_totals(dump):
    """The dump's totals, with byte counts in MB at the resolved size."""
    totals = dump["totals"]
    megabytes = {key: f"{totals[key] / 1e6:.2f} MB" for key in totals if key.endswith("_bytes")}
    budget = f", tile budget {dump['tile_budget']} B/pixel" if "tile_budget" in dump else ""
    return (f"{totals['render_passes']} render passes, attachment loads {megabytes['load_bytes']},"
            f" stores {megabytes['store_bytes']}, transients {megabytes['transient_bytes']}"
            f" ({megabytes['memoryless_bytes']} memoryless){budget}")


def render_html(dump, title):
    passes, physical_passes, resources = dump["passes"], dump["physical_passes"], dump["resources"]
    timed = any("gpu_ms" in physical for physical in physical_passes)
    culled = sum(graph_pass["culled"] for graph_pass in passes)
    barriers = sum(len(graph_pass["barriers"]) + len(graph_pass.get("barriers_after", [])) for graph_pass in passes)
    transients = sum(resource["kind"] == "Transient" for resource in resources)
    memoryless = sum(resource.get("memoryless", False) for resource in resources)
    summary = (f"{len(passes)} passes ({len(passes) - culled} live, {culled} culled, {len(physical_passes)} physical),"
               f" {barriers} barriers, {len(resources)} resources ({transients} transient, {memoryless} memoryless)")
    if timed:
        summary += f", {sum(physical.get('gpu_ms', 0) for physical in physical_passes):.3f} GPU ms"

    totals = f"<p>{html.escape(summarize_totals(dump))}</p>\n" if "totals" in dump else ""
    header = '<th class="pass">Pass</th><th>Kind</th><th>Physical</th>' + ("<th>GPU ms</th>" if timed else "")
    header += "".join(resource_header(resource, passes) for resource in resources)
    band_starts = {physical["members"][0] for physical in physical_passes}
    rows = []
    for index, graph_pass in enumerate(passes):
        uses = uses_by_resource(graph_pass, physical_passes)
        if graph_pass["culled"]:
            kind = f"culled: {graph_pass['cull_reason']}"
        else:
            kind = graph_pass["kind"] + (f", step {graph_pass['step']}" if graph_pass["step"] else "")
        row = f'<td class="pass">{html.escape(graph_pass["name"])}</td><td>{html.escape(kind)}</td>'
        row += physical_cells(index, passes, physical_passes, timed)
        for resource in resources:
            name = resource["name"]
            first, last = resource.get("first_use", -1), resource.get("last_use", -1)
            if name in uses:
                row += cell(graph_pass, name, uses[name])
            else:
                row += '<td class="life"></td>' if first <= index <= last else "<td></td>"
        classes = " ".join(name for name, applies in (("culled", graph_pass["culled"]),
                                                      ("band-start", index in band_starts)) if applies)
        rows.append(f'<tr class="{classes}">{row}</tr>' if classes else f"<tr>{row}</tr>")

    body = "\n".join(rows)
    opportunities = "".join(f"<li>{html.escape(line)}</li>" for line in dump.get("opportunities", []))
    report = f"<h2>Opportunities</h2>\n<ol>{opportunities}</ol>\n" if opportunities else ""
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Render graph: {html.escape(title)}</title>
<style>{STYLE}</style>
</head>
<body>
<h1>Render graph: {html.escape(title)}</h1>
<p>{summary}</p>
{totals}<table>
<thead><tr>{header}</tr></thead>
<tbody>
{body}
</tbody>
</table>
<p>{html.escape(LEGEND)}</p>
{report}</body>
</html>
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", help="render graph dump (JSON)")
    parser.add_argument("-o", "--output", help="HTML file to write; defaults to the dump path with .html")
    args = parser.parse_args()

    with open(args.dump, encoding="utf-8") as dump_file:
        dump = json.load(dump_file)
    output = args.output or os.path.splitext(args.dump)[0] + ".html"
    with open(output, "w", encoding="utf-8") as html_file:
        html_file.write(render_html(dump, os.path.basename(args.dump)))
    print(f"Wrote {output}")


if __name__ == "__main__":
    main()
