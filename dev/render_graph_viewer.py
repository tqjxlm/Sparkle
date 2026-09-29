"""Render a render graph dump as one self-contained static HTML page.

The page is a pass x resource grid: one row per pass in execution order, one
column per resource (texture, buffer or acceleration structure) in dump order. A
cell shows the pass's access to the resource and details it on hover: accesses,
attachment load/store with reasons, barriers with layouts. Shaded cells span
each resource's lifetime, from its first to its last use.
"""

import argparse
import html
import json
import os
from collections import defaultdict

WRITES = {"ColorWrite", "DepthWrite", "StorageWrite", "CopyDst", "AccelerationStructureBuild"}
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
td small { display: block; font-size: 10px; }
"""

LEGEND = ("R read, W write, RW both; C color / D depth attachment with load (ld load, clr clear, – don't care)"
          " / store (st store, – don't care); red top edge: barrier before the pass, red bottom edge: barrier after it;"
          " shaded: resource lifetime; italic: imported. Hover a cell or header for details.")


def resource_of(entry):
    """The resource an entry names, with its subresources unless it covers every one."""
    subresources = entry.get("subresources")
    return f"{entry['resource']}[{subresources}]" if subresources else entry["resource"]


def describe_access(access):
    clear = " clear" if access.get("clear") else ""
    return f"access {resource_of(access)} {access['access']}{clear}"


def describe_barrier(barrier):
    # memory barriers have no layouts
    layouts = f" {barrier['from_layout']}->{barrier['to_layout']}" if "from_layout" in barrier else ""
    return f"barrier {resource_of(barrier)}{layouts} [{barrier['from']} -> {barrier['to']}]"


def describe_barrier_after(barrier):
    """A memory barrier recorded after the pass, e.g. one that makes a buffer the host reads visible to it."""
    return f"barrier after {resource_of(barrier)} [{barrier['from']} -> {barrier['to']}]"


def describe_attachment(attachment):
    return (f"attachment {resource_of(attachment)} slot {attachment['slot']}:"
            f" {attachment['load']} ({attachment['load_reason']})"
            f" / {attachment['store']} ({attachment['store_reason']})")


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
    if "first_use" in resource:
        first, last = passes[resource["first_use"]]["name"], passes[resource["last_use"]]["name"]
        details += [f"used {first}..{last}", f"usage {resource['usage']}"]
    css = "resource imported" if resource["kind"] == "Imported" else "resource"
    tooltip = html.escape("\n".join(details))
    return f'<th class="{css}" title="{tooltip}"><span>{html.escape(resource["name"])}</span></th>'


def uses_by_resource(graph_pass):
    """Groups a pass's accesses, attachments and barriers by the resource they name."""
    keys = ("accesses", "attachments", "barriers", "barriers_after")
    uses = defaultdict(lambda: {key: [] for key in keys})
    for key in keys:
        for entry in graph_pass.get(key, []):
            uses[entry["resource"]][key].append(entry)
    return uses


def render_html(dump, title):
    passes, resources = dump["passes"], dump["resources"]
    timed = any("gpu_ms" in graph_pass for graph_pass in passes)
    culled = sum(graph_pass["culled"] for graph_pass in passes)
    barriers = sum(len(graph_pass["barriers"]) + len(graph_pass.get("barriers_after", [])) for graph_pass in passes)
    transients = sum(resource["kind"] == "Transient" for resource in resources)
    summary = (f"{len(passes)} passes ({len(passes) - culled} live, {culled} culled), {barriers} barriers,"
               f" {len(resources)} resources ({transients} transient)")
    if timed:
        summary += f", {sum(graph_pass.get('gpu_ms', 0) for graph_pass in passes):.3f} GPU ms"

    header = '<th class="pass">Pass</th><th>Kind</th>' + ("<th>GPU ms</th>" if timed else "")
    header += "".join(resource_header(resource, passes) for resource in resources)
    rows = []
    for index, graph_pass in enumerate(passes):
        uses = uses_by_resource(graph_pass)
        kind = f"culled: {graph_pass['cull_reason']}" if graph_pass["culled"] else graph_pass["kind"]
        row = f'<td class="pass">{html.escape(graph_pass["name"])}</td><td>{html.escape(kind)}</td>'
        if timed:
            row += f"<td>{graph_pass['gpu_ms']:.3f}</td>" if "gpu_ms" in graph_pass else "<td></td>"
        for resource in resources:
            name = resource["name"]
            first, last = resource.get("first_use", -1), resource.get("last_use", -1)
            if name in uses:
                row += cell(graph_pass, name, uses[name])
            else:
                row += '<td class="life"></td>' if first <= index <= last else "<td></td>"
        rows.append(f'<tr class="culled">{row}</tr>' if graph_pass["culled"] else f"<tr>{row}</tr>")

    body = "\n".join(rows)
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
<table>
<thead><tr>{header}</tr></thead>
<tbody>
{body}
</tbody>
</table>
<p>{html.escape(LEGEND)}</p>
</body>
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
