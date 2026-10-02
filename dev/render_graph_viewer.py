"""Render a render graph dump as one self-contained static HTML page.

The page has three tabs. Graph lays the passes out as a dependency graph with
elkjs, which the page inlines from the thirdparty/elkjs submodule so it works
offline: pass nodes inside the physical passes that record them, edges from the
pass that last wrote a resource to each later pass using it, imported inputs and
outputs as resource nodes. Lifetimes charts each resource's first to last use
over the passes. Grid is a pass x resource table of every access, attachment and
barrier. Below the grid, the opportunity report lists the physical passes by the
bytes their attachment loads and stores move. A dump with a profile shows each
time as its mean [min-max] over the profiled frames.
"""

import argparse
import html
import json
import os
from collections import defaultdict

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELK_PATH = os.path.join(PROJECT_ROOT, "thirdparty", "elkjs", "lib", "elk.bundled.js")

WRITES = {"ColorWrite", "DepthWrite", "StorageWrite", "CopyDst", "AccelerationStructureBuild"}
ATTACHMENTS = {"ColorWrite", "DepthWrite", "DepthTest"}
LOAD_OPS = {"Load": "ld", "Clear": "clr", "DontCare": "–"}
STORE_OPS = {"Store": "st", "DontCare": "–"}
BYTES_PER_PIXEL = {"B8G8R8A8Srgb": 4, "B8G8R8A8Unorm": 4, "R8G8B8A8Srgb": 4, "R8G8B8A8Unorm": 4, "R32UInt": 4,
                   "R32Float": 4, "D24S8": 4, "D32": 4, "RGBAFloat": 16, "RGBAFloat16": 8, "RGBAUInt32": 16,
                   "R10G10B10A2Unorm": 4, "R16Float": 2, "RGFloat16": 4}

STYLE = """
body { font: 13px system-ui, sans-serif; margin: 16px; color: #222; background: #fff; }
h1 { font-size: 20px; margin: 0 0 4px; }
p { margin: 4px 0; }
nav { margin: 10px 0 8px; border-bottom: 1px solid #ccc; }
nav a { display: inline-block; padding: 4px 14px; color: #335; text-decoration: none; border: 1px solid transparent;
        border-bottom: none; border-radius: 4px 4px 0 0; }
nav a.active { border-color: #ccc; background: #fff; margin-bottom: -1px; font-weight: 600; }
section[hidden] { display: none; }
table { border-collapse: collapse; }
th, td { border: 1px solid #ddd; padding: 2px 4px; text-align: center; white-space: nowrap; }
thead th { position: sticky; top: 0; background: #fff; vertical-align: bottom; }
th.resource span, th.pass-column span { writing-mode: vertical-rl; transform: rotate(180deg); }
.pass { text-align: left; }
.imported { font-style: italic; color: #666; }
.life { background: #eef3fb; }
.r { background: #cfe8cf; }
.w { background: #f7c9a9; }
.rw { background: #e3c8f0; }
.barrier { box-shadow: inset 0 3px #d33; }
.barrier-after { box-shadow: inset 0 -3px #d33; }
.barrier.barrier-after { box-shadow: inset 0 3px #d33, inset 0 -3px #d33; }
tr.culled, th.culled, td.culled { opacity: 0.45; }
tr.band-start > td { border-top: 2px solid #888; }
td.physical { background: #f4f4f4; }
td.over-budget { color: #c60; font-weight: bold; }
td small { display: block; font-size: 10px; }
th.resource span.badge { writing-mode: horizontal-tb; transform: none; display: inline-block; margin-top: 2px;
                         padding: 0 3px; }
.badge { border-radius: 3px; background: #2a7; color: #fff; font-size: 10px; padding: 0 3px; }
table.lifetimes td { min-width: 16px; }
table.lifetimes td.name, table.lifetimes td.text { text-align: left; }
td.span-transient { background: #d6e4f7; }
td.span-imported { background: #e6dcf5; }
td.span-memoryless { background: #cdebd9; }
td.span-start { border-left: 2px solid #666; }
td.span-end { border-right: 2px solid #666; }
td.used { font-weight: 600; }
#graph-view { position: relative; display: flex; height: calc(100vh - 150px); min-height: 480px;
              border: 1px solid #ccc; }
#graph-canvas { flex: 1; overflow: hidden; background: #fbfbfc; cursor: grab; }
#graph-canvas svg { width: 100%; height: 100%; display: block; }
#graph-panel { width: 360px; border-left: 1px solid #ccc; overflow: auto; padding: 8px 10px; background: #fff; }
#graph-panel h3 { margin: 0 0 6px; font-size: 14px; }
#graph-panel ul { margin: 0 0 8px; padding-left: 16px; }
#graph-panel pre { font-size: 11px; background: #f6f6f6; padding: 6px; overflow: auto; }
.toolbar { margin: 0 0 6px; display: flex; gap: 14px; align-items: center; flex-wrap: wrap; font-size: 12px; }
.toolbar button { font: inherit; }
.key { display: inline-block; width: 14px; height: 10px; border: 1px solid #888; vertical-align: middle;
       margin-right: 3px; }
svg text { font-family: system-ui, sans-serif; }
.node rect { fill: #fff; stroke: #5a6b85; stroke-width: 1.2; }
.node .name { font-size: 13px; font-weight: 600; fill: #1c2433; }
.node .sub { font-size: 11px; fill: #666; }
.node.resource text { font-size: 11px; }
.node.resource.transient rect, .key.transient { fill: #e3edfb; background: #e3edfb; stroke: #5b84c4; }
.node.resource.imported rect, .key.imported { fill: #efe7fa; background: #efe7fa; stroke: #8a63c4; }
.node.resource.memoryless rect, .key.memoryless { fill: #dcf3e6; background: #dcf3e6; stroke: #2a9a5a; }
.node.resource.imported text { font-style: italic; }
.node.resource.unread rect { fill: #fde2e2; stroke: #d33; }
.node.resource.unread text { fill: #b00; }
.culled { opacity: 0.4; }
.node.culled rect { stroke-dasharray: 4 3; }
.group rect { stroke: #aab3c2; stroke-width: 1; }
.group.over-budget rect { stroke: #c60; stroke-width: 2.5; }
.group text { paint-order: stroke; stroke-width: 4px; stroke-linejoin: round; }
.group .title { font-size: 12px; font-weight: 700; fill: #333; }
.group .breaks { font-size: 11px; fill: #555; }
.edge path { fill: none; stroke: #7d8797; stroke-width: 1.3; }
.edge.memoryless path { stroke: #2a9a5a; }
.edge.unread path { stroke: #d33; stroke-width: 2; }
.edge.culled path { stroke-dasharray: 5 4; }
.edge-label rect.background { fill: #fbfbfc; opacity: 0.9; }
.edge-label text { font-size: 11px; }
.edge-label .transient { fill: #1f4e8c; }
.edge-label .imported { fill: #6b3fa0; font-style: italic; }
.edge-label .memoryless { fill: #1e7b4a; }
.edge-label .unread { fill: #b00; font-weight: 600; }
.edge-label rect.barrier, .key.barrier { fill: #d33; background: #d33; stroke: none; border-color: #d33; }
.selected rect { stroke: #1a5fd0 !important; stroke-width: 2.5 !important; }
.dim { opacity: 0.12; }
"""

LEGEND = ("R read, W write, RW both; C color / D depth attachment with its physical pass's load (ld load, clr clear,"
          " – don't care) / store (st store, – don't care); red top edge: barrier before the pass, red bottom edge:"
          " barrier after it; shaded: resource lifetime; italic: imported; M: memoryless transient, kept in tile memory;"
          " P: physical pass, a band over its members, with its GPU time; bold orange: over the tile budget."
          " Hover a cell or header for details.")

LIFETIMES_LEGEND = ("One row per resource, one column per pass in execution order; the bar spans the resource's first"
                    " to last live use, blue for a transient, green for a memoryless one, purple for an import. A cell"
                    " shows the pass's access (R, W, RW). B/px is the format's bytes per pixel; bytes are known for"
                    " Absolute sizes. Image is the transient's physical image: transients sharing one alias it.")

GRAPH_LEGEND = """<div class="toolbar">
<button type="button" id="graph-fit">Fit</button>
<label><input type="checkbox" id="graph-ordered" checked>execution order</label>
<span><span class="key transient"></span>transient</span>
<span><span class="key memoryless"></span>memoryless</span>
<span><span class="key imported"></span><i>imported</i></span>
<span><span class="key barrier"></span>barrier before the reader</span>
<span style="color:#b00">red: stored, never read</span>
<span>dashed, faded: culled</span>
<span>[st] stored, [ld] loaded attachment; P&lt;i&gt;: physical pass, shaded by GPU time</span>
<span>drag to pan, wheel to zoom, click to trace</span>
</div>"""

LAYOUT_SCRIPT = r"""
const NAME_FONT = '600 13px', SUB_FONT = '11px', TITLE_FONT = '700 12px', LINE = 14;

function groupTitle(group) {
  return group.label + (group.time ? `  ${group.time}` : '');
}

function nodeBox(node, textWidth) {
  if (node.type !== 'pass') return { width: textWidth(node.name, SUB_FONT) + 20, height: 22 };
  return { width: Math.max(textWidth(node.name, NAME_FONT), textWidth(node.sub, SUB_FONT)) + 20, height: 38 };
}

// textWidth(text, font) measures a label. A physical pass is a compound node whose top padding holds its title and
// bottom padding its breaks; elk ignores a minimum size on compound nodes, so the right padding widens instead.
// ordered chains consecutive physical passes with hidden edges, so the layers follow execution order
function elkGraph(model, textWidth, ordered) {
  const roots = [], groups = new Map(), boxes = new Map();
  for (const node of model.nodes) boxes.set(node.id, nodeBox(node, textWidth));
  for (const group of model.groups) {
    const text = Math.max(textWidth(groupTitle(group), TITLE_FONT),
                          ...group.breaks.map(line => textWidth(line, SUB_FONT)));
    const members = Math.max(...group.members.map(id => boxes.get(id).width));
    const right = 10 + Math.max(0, text - members);
    groups.set(group.id, { id: group.id, children: [], layoutOptions: {
      'elk.padding': `[top=${LINE + 12},left=10,bottom=${group.breaks.length * LINE + 10},right=${right}]` } });
  }
  for (const node of model.nodes) {
    const elkNode = { id: node.id, ...boxes.get(node.id) };
    if (!node.group) { roots.push(elkNode); continue; }
    const group = groups.get(node.group);
    if (!group.children.length) roots.push(group);
    group.children.push(elkNode);
  }
  const edges = model.edges.map(edge => ({
    id: edge.id, sources: [edge.source], targets: [edge.target],
    labels: [{ text: edge.id, height: edge.items.length * LINE + 2,
               width: Math.max(...edge.items.map(item => textWidth(item.label, SUB_FONT))) + 14 }] }));
  if (ordered) {
    model.groups.slice(1).forEach((group, index) => edges.push({
      id: `order${index}`, sources: [model.groups[index].members.at(-1)], targets: [group.members[0]] }));
  }
  return { id: 'root', children: roots, edges, layoutOptions: {
    'elk.algorithm': 'layered', 'elk.direction': 'DOWN', 'elk.hierarchyHandling': 'INCLUDE_CHILDREN',
    'elk.edgeRouting': 'ORTHOGONAL', 'elk.edgeLabels.placement': 'CENTER',
    'elk.layered.considerModelOrder.strategy': 'NODES_AND_EDGES',
    'elk.layered.nodePlacement.strategy': 'LINEAR_SEGMENTS',
    'elk.spacing.nodeNode': '28', 'elk.layered.spacing.nodeNodeBetweenLayers': '28',
    'elk.spacing.edgeEdge': '12', 'elk.spacing.edgeNode': '14', 'elk.spacing.edgeLabel': '4',
    'elk.layered.spacing.edgeNodeBetweenLayers': '14', 'elk.layered.spacing.edgeEdgeBetweenLayers': '10',
    'elk.json.shapeCoords': 'ROOT', 'elk.json.edgeCoords': 'ROOT' } };
}
"""

VIEW_SCRIPT = r"""
(() => {
const model = JSON.parse(document.getElementById('graph-model').textContent);
const SVG = 'http://www.w3.org/2000/svg';
const canvas = document.getElementById('graph-canvas');
const panel = document.getElementById('graph-panel');
const tabs = ['graph', 'lifetimes', 'grid'];

function showTab() {
  const tab = tabs.includes(location.hash.slice(1)) ? location.hash.slice(1) : 'graph';
  for (const name of tabs) {
    document.getElementById(name).hidden = name !== tab;
    document.querySelector(`nav a[href="#${name}"]`).classList.toggle('active', name === tab);
  }
}
window.addEventListener('hashchange', showTab);
showTab();

const measure = document.createElement('canvas').getContext('2d');
function textWidth(text, font) {
  measure.font = font + ' system-ui, sans-serif';
  return Math.ceil(measure.measureText(text).width);
}

function element(tag, attributes, parent, text) {
  const result = document.createElementNS(SVG, tag);
  for (const [key, value] of Object.entries(attributes)) result.setAttribute(key, value);
  if (text !== undefined) result.textContent = text;
  if (parent) parent.appendChild(result);
  return result;
}

function tooltip(target, lines) {
  element('title', {}, target, lines.join('\n'));
}

function heat(value) {
  return value === null ? '#f2f4f7' : `hsl(${50 - 28 * value}, 90%, ${94 - 12 * value}%)`;
}

const byId = new Map([...model.nodes, ...model.groups, ...model.edges].map(entry => [entry.id, entry]));
const shapes = new Map();
let dragged = false;

function draw(layout) {
  const svg = element('svg', {}, canvas);
  element('defs', {}, svg).innerHTML =
    '<marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto">' +
    '<path d="M0,0 L10,5 L0,10 z" fill="#7d8797"/></marker>';
  const view = element('g', {}, svg);
  const [groupLayer, edgeLayer, labelLayer, titleLayer, nodeLayer] = [0, 1, 2, 3, 4].map(() => element('g', {}, view));
  (function visit(node) {
    for (const child of node.children || []) {
      const entry = byId.get(child.id);
      if (entry.members) drawGroup(groupLayer, titleLayer, child, entry); else drawNode(nodeLayer, child, entry);
      visit(child);
    }
  })(layout);
  for (const edge of layout.edges) {
    if (byId.has(edge.id)) drawEdge(edgeLayer, labelLayer, edge, byId.get(edge.id));
  }
  setupView(svg, view, layout);
}

function drawGroup(groupLayer, titleLayer, box, group) {
  const classes = 'group' + (group.over_tile_budget ? ' over-budget' : '');
  const g = element('g', { class: classes, transform: `translate(${box.x},${box.y})` }, groupLayer);
  element('rect', { width: box.width, height: box.height, rx: 8, fill: heat(group.heat) }, g);
  const titles = element('g', { class: classes, transform: `translate(${box.x},${box.y})` }, titleLayer);
  const halo = { stroke: heat(group.heat) };
  element('text', { x: 10, y: LINE + 3, class: 'title', ...halo }, titles, groupTitle(group));
  group.breaks.forEach((line, index) => element('text', {
    x: 10, y: box.height - (group.breaks.length - index - 1) * LINE - 8, class: 'breaks', ...halo }, titles, line));
  for (const shape of [g, titles]) {
    tooltip(shape, group.details);
    register(shape, group);
  }
}

function drawNode(layer, box, node) {
  const classes = ['node', node.type === 'pass' ? 'pass' : 'resource', node.style, node.culled ? 'culled' : ''];
  const g = element('g', { class: classes.join(' '), transform: `translate(${box.x},${box.y})` }, layer);
  element('rect', { width: box.width, height: box.height, rx: node.type === 'pass' ? 5 : 11 }, g);
  if (node.type === 'pass') {
    element('text', { x: 10, y: 16, class: 'name' }, g, node.name);
    element('text', { x: 10, y: 31, class: 'sub' }, g, node.sub);
  } else {
    element('text', { x: box.width / 2, y: 15, 'text-anchor': 'middle' }, g, node.name);
  }
  tooltip(g, node.details);
  register(g, node);
}

function drawEdge(edgeLayer, labelLayer, layout, edge) {
  const classes = ['edge', edge.style, edge.culled ? 'culled' : ''].join(' ');
  const g = element('g', { class: classes }, edgeLayer);
  for (const section of layout.sections || []) {
    const points = [section.startPoint, ...(section.bendPoints || []), section.endPoint];
    element('path', { d: 'M' + points.map(point => `${point.x},${point.y}`).join(' L'),
                      'marker-end': 'url(#arrow)' }, g);
  }
  const label = layout.labels[0];
  const text = element('g', { class: 'edge-label ' + classes, transform: `translate(${label.x},${label.y})` },
                       labelLayer);
  element('rect', { class: 'background', width: label.width, height: label.height, rx: 3 }, text);
  edge.items.forEach((item, index) => {
    const line = element('g', { transform: `translate(0,${index * LINE})` }, text);
    if (item.barrier) element('rect', { class: 'barrier', x: 2, y: 4, width: 4, height: 9 }, line);
    element('text', { x: 10, y: 12, class: item.style }, line, item.label);
    tooltip(line, item.details);
  });
  register(g, edge);
  register(text, edge);
}

function register(shape, entry) {
  if (!shapes.has(entry.id)) shapes.set(entry.id, []);
  shapes.get(entry.id).push(shape);
  shape.addEventListener('click', event => { event.stopPropagation(); if (!dragged) select(entry); });
}

function trace(start, forward) {
  const reached = new Set(start), edges = new Set();
  for (let changed = true; changed;) {
    changed = false;
    for (const edge of model.edges) {
      const [from, to] = forward ? [edge.source, edge.target] : [edge.target, edge.source];
      if (reached.has(from) && !edges.has(edge.id)) {
        edges.add(edge.id);
        reached.add(to);
        changed = true;
      }
    }
  }
  return [...reached, ...edges];
}

function select(entry) {
  for (const list of shapes.values()) for (const shape of list) shape.classList.remove('dim', 'selected');
  panel.innerHTML = '';
  if (!entry) {
    panel.textContent = 'Click a pass, physical pass, resource or edge to trace what it depends on and what depends' +
      ' on it.';
    return;
  }
  const start = entry.source ? [entry.source, entry.target] : entry.members || [entry.id];
  const related = new Set(entry.source ? [entry.id, ...start]
                                       : [entry.id, ...trace(start, true), ...trace(start, false)]);
  for (const [id, list] of shapes) {
    const members = byId.get(id).members || [];
    if (related.has(id) || members.some(member => related.has(member))) continue;
    for (const shape of list) shape.classList.add('dim');
  }
  for (const shape of shapes.get(entry.id)) shape.classList.add('selected');
  const heading = panel.appendChild(document.createElement('h3'));
  heading.textContent = entry.name || entry.label ||
    `${byId.get(entry.source).name} → ${byId.get(entry.target).name}`;
  const list = panel.appendChild(document.createElement('ul'));
  for (const line of entry.items ? entry.items.flatMap(item => item.details) : entry.details) {
    list.appendChild(document.createElement('li')).textContent = line;
  }
  if (entry.dump) panel.appendChild(document.createElement('pre')).textContent = JSON.stringify(entry.dump, null, 2);
}

const pan = { x: 0, y: 0, k: 1, view: null, layout: null, start: null };

function apply() {
  pan.view.setAttribute('transform', `translate(${pan.x},${pan.y}) scale(${pan.k})`);
}

// fits the whole graph, or its width when that leaves a tall graph too small to read
function fit(whole) {
  const margin = 16, width = canvas.clientWidth - 2 * margin, height = canvas.clientHeight - 2 * margin;
  const fitWidth = width / pan.layout.width, fitAll = Math.min(fitWidth, height / pan.layout.height);
  pan.k = Math.min(1, whole || fitAll >= 0.5 ? fitAll : fitWidth);
  pan.x = margin + (width - pan.layout.width * pan.k) / 2;
  pan.y = margin;
  apply();
}

function setupView(svg, view, layout) {
  Object.assign(pan, { view, layout });
  fit(false);
  svg.addEventListener('wheel', event => {
    event.preventDefault();
    const rect = svg.getBoundingClientRect(), scale = Math.exp(-event.deltaY * 0.0015);
    const px = event.clientX - rect.left, py = event.clientY - rect.top;
    Object.assign(pan, { x: px - (px - pan.x) * scale, y: py - (py - pan.y) * scale, k: pan.k * scale });
    apply();
  }, { passive: false });
  svg.addEventListener('pointerdown', event => {
    pan.start = { px: event.clientX, py: event.clientY, x: pan.x, y: pan.y };
    dragged = false;
  });
  svg.addEventListener('click', () => { if (!dragged) select(null); });
  select(null);
}

window.addEventListener('pointermove', event => {
  if (!pan.start) return;
  const dx = event.clientX - pan.start.px, dy = event.clientY - pan.start.py;
  dragged = dragged || Math.hypot(dx, dy) > 3;
  Object.assign(pan, { x: pan.start.x + dx, y: pan.start.y + dy });
  apply();
});
window.addEventListener('pointerup', () => { pan.start = null; });
document.getElementById('graph-fit').addEventListener('click', () => fit(true));

const ordered = document.getElementById('graph-ordered');
function layout() {
  canvas.textContent = '';
  shapes.clear();
  new ELK().layout(elkGraph(model, textWidth, ordered.checked)).then(draw).catch(error => {
    canvas.textContent = `Graph layout failed: ${error}`;
  });
}
ordered.addEventListener('change', layout);
layout();
})();
"""


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


def mean_ms(entry, key):
    """An entry's time in ms: the profile's mean when profiled, else the single frame's; None when untimed."""
    stat = entry.get("profile", {}).get(key)
    return stat["mean"] if stat else entry.get(key)


def describe_time(entry, key, frames):
    """An entry's time: the profile's mean [min–max] with the frames it was sampled in, else the single frame's."""
    stat = entry.get("profile", {}).get(key)
    if stat:
        return (f"{stat['mean']:.3f} ms [{stat['min']:.3f}–{stat['max']:.3f}],"
                f" {stat['samples']}/{frames} frames")
    return f"{entry[key]:.3f} ms" if key in entry else None


def access_flags(access):
    """The access bits of a dumped access, e.g. StorageRead|StorageWrite(Compute) -> {StorageRead, StorageWrite}."""
    return set(access.split("(")[0].split("|"))


def is_write(access):
    return bool(access_flags(access["access"]) & WRITES)


def access_kind(accesses):
    """R, W or RW: whether the accesses read, write or both."""
    flags = set().union(*(access_flags(access["access"]) for access in accesses))
    return ("R" if flags - WRITES else "") + ("W" if flags & WRITES else "")


def cell(graph_pass, name, uses):
    kind = access_kind(uses["accesses"])
    label = kind
    if uses["attachments"]:
        attachment = uses["attachments"][0]
        label = (f"{'D' if attachment['slot'] == 'depth' else 'C'}"
                 f"<small>{LOAD_OPS[attachment['load']]}/{STORE_OPS[attachment['store']]}</small>")

    tooltip = html.escape("\n".join(use_details(graph_pass, name, uses)))
    classes = kind.lower() + (" barrier" if uses["barriers"] else "")
    classes += " barrier-after" if uses["barriers_after"] else ""
    return f'<td class="{classes}" title="{tooltip}">{label}</td>'


def use_details(graph_pass, name, uses):
    details = [f"{graph_pass['name']} / {name}"]
    details += [describe_access(access) for access in uses["accesses"]]
    details += [describe_attachment(attachment) for attachment in uses["attachments"]]
    details += [describe_barrier(barrier) for barrier in uses["barriers"]]
    details += [describe_barrier_after(barrier) for barrier in uses["barriers_after"]]
    return details


def resource_details(resource, passes):
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
    return details


def resource_header(resource, passes):
    css = "resource imported" if resource["kind"] == "Imported" else "resource"
    tooltip = html.escape("\n".join(resource_details(resource, passes)))
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


def physical_cells(index, passes, physical_passes, frames):
    """The physical pass cells of the pass's row: spanning the rows of a physical pass from its first member to its
    last, detailed on hover, on the first member's row; none on the other rows of the span; empty on a culled row
    outside every span. frames is None for an untimed dump."""
    for physical_index, physical in enumerate(physical_passes):
        first, last = physical["members"][0], physical["members"][-1]
        if index == first:
            break
        if first < index <= last:
            return ""
    else:
        return "<td></td>" + ("<td></td>" if frames is not None else "")

    span = f' rowspan="{last - first + 1}"' if last > first else ""
    css = "physical over-budget" if physical.get("over_tile_budget") else "physical"
    tooltip = html.escape("\n".join(physical_details(physical, passes)))
    cells = f'<td class="{css}"{span} title="{tooltip}">P{physical_index}</td>'
    if frames is not None:
        cells += f'<td class="physical"{span}>{grid_time(physical, frames)}</td>'
    return cells


def physical_details(physical, passes):
    details = [describe_physical_pass(physical, passes)]
    if "color_bytes_per_pixel" in physical:
        over = ", over the tile budget" if physical.get("over_tile_budget") else ""
        details.append(f"color {physical['color_bytes_per_pixel']} B/pixel{over}")
    return details + [describe_attachment(attachment) for attachment in physical["attachments"]]


def grid_time(physical, frames):
    """A physical pass's GPU ms for its grid cell: the mean with its [min–max] and samples when profiled."""
    stat = physical.get("profile", {}).get("gpu_ms")
    if stat:
        return (f"{stat['mean']:.3f}<small>{stat['min']:.3f}–{stat['max']:.3f},"
                f" {stat['samples']}/{frames}</small>")
    return f"{physical['gpu_ms']:.3f}" if "gpu_ms" in physical else ""


def summarize_totals(dump):
    """The dump's totals, with byte counts in MB at the resolved size."""
    totals = dump["totals"]
    megabytes = {key: f"{totals[key] / 1e6:.2f} MB" for key in totals if key.endswith("_bytes")}
    budget = f", tile budget {dump['tile_budget']} B/pixel" if "tile_budget" in dump else ""
    return (f"{totals['render_passes']} render passes, attachment loads {megabytes['load_bytes']},"
            f" stores {megabytes['store_bytes']}, transients {megabytes['transient_bytes']}"
            f" ({megabytes['memoryless_bytes']} memoryless){budget}")


def grid_table(dump, frames):
    """The pass x resource table; frames is None for an untimed dump, 0 for a timed single frame."""
    passes, physical_passes, resources = dump["passes"], dump["physical_passes"], dump["resources"]
    timing = "<th>GPU ms</th>" if frames is not None else ""
    header = f'<th class="pass">Pass</th><th>Kind</th><th>Physical</th>{timing}'
    header += "".join(resource_header(resource, passes) for resource in resources)
    band_starts = {physical["members"][0] for physical in physical_passes}
    rows = []
    for index, graph_pass in enumerate(passes):
        uses = uses_by_resource(graph_pass, physical_passes)
        row = f'<td class="pass">{html.escape(graph_pass["name"])}</td><td>{html.escape(pass_kind(graph_pass))}</td>'
        row += physical_cells(index, passes, physical_passes, frames)
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
    return f'<table class="grid">\n<thead><tr>{header}</tr></thead>\n<tbody>\n{body}\n</tbody>\n</table>'


def pass_kind(graph_pass):
    if graph_pass["culled"]:
        return f"culled: {graph_pass['cull_reason']}"
    return graph_pass["kind"] + (f", step {graph_pass['step']}" if graph_pass["step"] else "")


def resource_style(resource):
    """How a resource is drawn: imported, memoryless or transient."""
    if resource is None or resource["kind"] == "Imported":
        return "imported"
    return "memoryless" if resource.get("memoryless") else "transient"


def lifetimes_table(dump):
    """One row per resource with its format, size and image, and a bar over the passes from its first to its last
    use."""
    passes, physical_passes, resources = dump["passes"], dump["physical_passes"], dump["resources"]
    header = "<th>Resource</th><th>Kind</th><th>Format</th><th>B/px</th><th>Bytes</th><th>Image</th>"
    header += "".join(f'<th class="pass-column{" culled" if graph_pass["culled"] else ""}">'
                      f'<span>{html.escape(graph_pass["name"])}</span></th>' for graph_pass in passes)
    uses = [uses_by_resource(graph_pass, physical_passes) for graph_pass in passes]
    rows = []
    for resource in resources:
        name, style = resource["name"], resource_style(resource)
        bytes_per_pixel = BYTES_PER_PIXEL.get(resource.get("format"))
        size = f" {resource['width']}x{resource['height']}" if "width" in resource else ""
        image_bytes = (f"{resource['width'] * resource['height'] * bytes_per_pixel / 1e6:.2f} MB"
                       if size and bytes_per_pixel else "")
        badge = ' <span class="badge">M</span>' if resource.get("memoryless") else ""
        tooltip = html.escape("\n".join(resource_details(resource, passes)))
        row = (f'<td class="name{" imported" if style == "imported" else ""}" title="{tooltip}">{html.escape(name)}'
               f'</td><td class="text">{resource["kind"]}{badge}</td>'
               f'<td class="text">{html.escape(resource.get("format", ""))} {resource.get("size_class", "")}{size}</td>'
               f'<td>{bytes_per_pixel or ""}</td><td>{image_bytes}</td><td>{resource.get("physical", "")}</td>')
        first, last = resource.get("first_use", -1), resource.get("last_use", -1)
        for index, graph_pass in enumerate(passes):
            classes = [f"span-{style}"] if first <= index <= last else []
            classes += ["span-start"] if index == first else []
            classes += ["span-end"] if index == last else []
            label, title = "", ""
            if name in uses[index]:
                label = access_kind(uses[index][name]["accesses"])
                title = f' title="{html.escape(chr(10).join(use_details(graph_pass, name, uses[index][name])))}"'
                classes += ["used"]
            classes += ["culled"] if graph_pass["culled"] else []
            css = f' class="{" ".join(classes)}"' if classes else ""
            row += f"<td{css}{title}>{label}</td>"
        rows.append(f"<tr>{row}</tr>")
    body = "\n".join(rows)
    return f'<table class="lifetimes">\n<thead><tr>{header}</tr></thead>\n<tbody>\n{body}\n</tbody>\n</table>'


def subresource_ranges(entry):
    """The inclusive (first, last) mips and layers an entry names; None for all of them."""
    ranges = {"mip": None, "layer": None}
    words = entry.get("subresources", "").split()
    for kind, span in zip(words[::2], words[1::2]):
        first, _, last = span.partition("-")
        ranges[kind.removesuffix("s")] = (int(first), int(last or first))
    return ranges["mip"], ranges["layer"]


def ranges_overlap(a, b):
    return all(x is None or y is None or (x[0] <= y[1] and y[0] <= x[1]) for x, y in zip(a, b))


def ranges_cover(a, b):
    return all(x is None or (y is not None and x[0] <= y[0] and y[1] <= x[1]) for x, y in zip(a, b))


def find_entry(entries, access):
    """The entry of a pass's attachments or barriers naming the access's resource and subresources."""
    return next((entry for entry in entries if resource_of(entry) == resource_of(access)), None)


def short_access(access):
    """An access without its stages, e.g. Sampled(Pixel) -> Sampled."""
    return access["access"].split("(")[0]


def graph_model(dump):
    """The Graph tab's model.

    nodes: every pass in execution order, in its physical pass's group unless culled; a source node per resource a
    pass reads before any pass writes it; a sink node per import a live pass writes, and per transient a physical pass
    stores that no later live pass uses (unread).
    groups: one per physical pass with its members, title, breaks and GPU time, heat the time relative to the slowest.
    edges: from the last pass that wrote the subresources an access names to the accessing pass, one per pair of
    passes with one item per resource. A live pass depends on live writers only, a culled one on any writer. An item
    marks the barrier recorded before its reader and, between physical passes, the attachment store and load.
    """
    passes, physical_passes, resources = dump["passes"], dump["physical_passes"], dump["resources"]
    frames = dump.get("profile", {}).get("frames")
    by_name = {resource["name"]: index for index, resource in enumerate(resources)}
    pass_ids = [f"pass{index}" for index in range(len(passes))]
    sources, sinks, edges = {}, {}, {}

    def resource_at(name):
        return resources[by_name[name]] if name in by_name else None

    def attachment(pass_index, access):
        if pass_index is None or passes[pass_index]["culled"]:
            return None
        return find_entry(physical_passes[passes[pass_index]["physical_pass"]]["attachments"], access)

    def resource_node(table, prefix, name, unread=False):
        if name not in table:
            resource = resource_at(name)
            table[name] = {"id": f"{prefix}{by_name.get(name, len(resources) + len(table))}", "type": prefix,
                           "name": name, "style": resource_style(resource) + (" unread" if unread else ""),
                           "details": resource_details(resource, passes) if resource else [name], "dump": resource}
        return table[name]["id"]

    def add_item(source, target, item):
        edge = edges.setdefault((source, target), {"id": f"edge{len(edges)}", "source": source, "target": target,
                                                   "items": []})
        if item not in edge["items"]:
            edge["items"].append(item)

    def dependency(writer, reader, access):
        """The item of an edge into the reader: its access, the writer's store and its load between physical passes,
        and its barrier."""
        writer_name = passes[writer]["name"] if writer is not None else "input"
        details = [f"{resource_of(access)}: {writer_name} -> {passes[reader]['name']}", describe_access(access)]
        crosses = writer is None or passes[writer].get("physical_pass") != passes[reader].get("physical_pass")
        ops = []
        for pass_index, op_names, op in ((writer, STORE_OPS, "store"), (reader, LOAD_OPS, "load")):
            found = attachment(pass_index, access) if crosses else None
            if found:
                ops.append(op_names[found[op]])
                details.append(f"{passes[pass_index]['name']} {describe_attachment(found)}")
        barrier = find_entry(passes[reader]["barriers"], access)
        if barrier:
            details.append(describe_barrier(barrier))
        label = f"{resource_of(access)} {short_access(access)}" + (f" [{'/'.join(ops)}]" if ops else "")
        return {"label": label, "style": resource_style(resource_at(access["resource"])),
                "barrier": barrier is not None, "details": details}

    def reads(pass_index, access):
        """Whether the access depends on earlier contents: it reads, or its physical pass loads the attachment."""
        loaded = attachment(pass_index, access)
        return bool(access_flags(access["access"]) - WRITES) or (loaded is not None and loaded["load"] == "Load")

    live_writers, all_writers = defaultdict(list), defaultdict(list)
    for index, graph_pass in enumerate(passes):
        writers = all_writers if graph_pass["culled"] else live_writers
        for access in graph_pass["accesses"]:
            ranges = subresource_ranges(access)
            found = [writer for writer, written, _ in writers[access["resource"]] if ranges_overlap(written, ranges)]
            for writer in found:
                add_item(pass_ids[writer], pass_ids[index], dependency(writer, index, access))
            if not found and reads(index, access):
                add_item(resource_node(sources, "in", access["resource"]), pass_ids[index],
                         dependency(None, index, access))
        for access in filter(is_write, graph_pass["accesses"]):
            ranges = subresource_ranges(access)
            for table in (all_writers,) if graph_pass["culled"] else (all_writers, live_writers):
                table[access["resource"]] = [writer for writer in table[access["resource"]]
                                             if not ranges_cover(ranges, writer[1])] + [(index, ranges, access)]

    for name, writers in live_writers.items():
        if resource_style(resource_at(name)) == "imported":
            for writer, _, access in writers:
                add_item(pass_ids[writer], resource_node(sinks, "out", name),
                         output_item(passes[writer], access, attachment(writer, access)))
    for physical in physical_passes:
        for stored in physical["attachments"]:
            if stored["store"] == "Store" and resource_style(resource_at(stored["resource"])) != "imported" and \
                    not used_after(passes, physical["members"][-1], stored):
                writer = max(member for member in physical["members"]
                             if find_entry(passes[member]["accesses"], stored))
                add_item(pass_ids[writer], resource_node(sinks, "out", stored["resource"], unread=True),
                         unread_item(passes[writer], stored))

    edge_list = list(edges.values())
    culled = {pass_ids[index] for index, graph_pass in enumerate(passes) if graph_pass["culled"]}
    for edge in edge_list:
        edge["culled"] = edge["source"] in culled or edge["target"] in culled
        styles = {item["style"] for item in edge["items"]}
        edge["style"] = "unread" if "unread" in styles else styles.pop() if len(styles) == 1 else "mixed"
    for source in sources.values():
        source["culled"] = all(edge["culled"] for edge in edge_list if edge["source"] == source["id"])

    nodes = [pass_node(pass_id, graph_pass, frames) for pass_id, graph_pass in zip(pass_ids, passes)]
    return {"nodes": list(sources.values()) + nodes + list(sinks.values()),
            "groups": physical_groups(physical_passes, passes, frames), "edges": edge_list}


def used_after(passes, index, entry):
    """Whether a live pass after the index accesses the subresources the entry names."""
    ranges = subresource_ranges(entry)
    return any(not graph_pass["culled"] and access["resource"] == entry["resource"]
               and ranges_overlap(subresource_ranges(access), ranges)
               for graph_pass in passes[index + 1:] for access in graph_pass["accesses"])


def output_item(writer, access, stored):
    """The item of an edge from a last live writer of an import to its sink."""
    details = [f"{resource_of(access)}: written by {writer['name']}, an output of the graph", describe_access(access)]
    label = f"{resource_of(access)} {short_access(access)}"
    if stored:
        label += f" [{STORE_OPS[stored['store']]}]"
        details.append(describe_attachment(stored))
    barrier = find_entry(writer.get("barriers_after", []), access)
    if barrier:
        details.append(describe_barrier_after(barrier))
    return {"label": label, "style": "imported", "barrier": barrier is not None, "details": details}


def unread_item(writer, stored):
    """The item of an edge to the sink of a transient that its physical pass stores and no later live pass uses."""
    return {"label": f"{resource_of(stored)} stored, unread", "style": "unread", "barrier": False,
            "details": [f"{resource_of(stored)}: stored after {writer['name']}, no later live pass uses it",
                        describe_attachment(stored)]}


def pass_node(pass_id, graph_pass, frames):
    cpu = describe_time(graph_pass, "cpu_ms", frames)
    sub = "culled" if graph_pass["culled"] else pass_kind(graph_pass).replace(", ", " · ")
    if cpu:
        sub += f" · {mean_ms(graph_pass, 'cpu_ms'):.3f} ms CPU"
    details = [graph_pass["name"], pass_kind(graph_pass)] + ([f"CPU {cpu}"] if cpu else [])
    details += [describe_access(access) for access in graph_pass["accesses"]]
    details += [describe_barrier(barrier) for barrier in graph_pass["barriers"]]
    details += [describe_barrier_after(barrier) for barrier in graph_pass.get("barriers_after", [])]
    group = None if graph_pass["culled"] else f"physical{graph_pass['physical_pass']}"
    return {"id": pass_id, "type": "pass", "name": graph_pass["name"], "sub": sub, "culled": graph_pass["culled"],
            "group": group, "details": details, "dump": graph_pass}


def group_time(physical, frames):
    """A physical pass's GPU time for its title: the mean [min–max], with the samples when some frames lack one."""
    stat = physical.get("profile", {}).get("gpu_ms")
    if stat:
        missing = f", {stat['samples']}/{frames} frames" if stat["samples"] != frames else ""
        return f"{stat['mean']:.3f} [{stat['min']:.3f}–{stat['max']:.3f}] ms GPU{missing}"
    return f"{physical['gpu_ms']:.3f} ms GPU" if "gpu_ms" in physical else ""


def physical_groups(physical_passes, passes, frames):
    times = [mean_ms(physical, "gpu_ms") for physical in physical_passes]
    slowest = max((time for time in times if time is not None), default=0)
    groups = []
    for index, (physical, time) in enumerate(zip(physical_passes, times)):
        gpu = describe_time(physical, "gpu_ms", frames)
        breaks = physical.get("breaks", [])
        groups.append({"id": f"physical{index}", "label": f"P{index}",
                       "members": [f"pass{member}" for member in physical["members"]],
                       "time": group_time(physical, frames),
                       "heat": time / slowest if time is not None and slowest > 0 else None,
                       "breaks": [("break " if line == 0 else "") + reason for line, reason in enumerate(breaks)],
                       "over_tile_budget": physical.get("over_tile_budget", False),
                       "details": [f"P{index}"] + ([f"GPU {gpu}"] if gpu else []) + physical_details(physical, passes),
                       "dump": physical})
    return groups


def script_json(value):
    """JSON that is safe inside a <script> element."""
    text = json.dumps(value, separators=(",", ":"))
    return text.replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026") \
        .replace("\u2028", "\\u2028").replace("\u2029", "\\u2029")


def load_elk():
    if not os.path.isfile(ELK_PATH):
        raise FileNotFoundError(f"{ELK_PATH} is missing; fetch it with"
                                " `git submodule update --init --depth 1 thirdparty/elkjs`")
    with open(ELK_PATH, encoding="utf-8") as elk_file:
        return elk_file.read().replace("</script", "<\\/script")


def render_html(dump, title):
    passes, physical_passes, resources = dump["passes"], dump["physical_passes"], dump["resources"]
    times = [mean_ms(physical, "gpu_ms") for physical in physical_passes]
    timed = any(time is not None for time in times)
    profiled = dump.get("profile", {}).get("frames")
    frames = (profiled or 0) if timed else None
    culled = sum(graph_pass["culled"] for graph_pass in passes)
    barriers = sum(len(graph_pass["barriers"]) + len(graph_pass.get("barriers_after", [])) for graph_pass in passes)
    transients = sum(resource["kind"] == "Transient" for resource in resources)
    memoryless = sum(resource.get("memoryless", False) for resource in resources)
    summary = (f"{len(passes)} passes ({len(passes) - culled} live, {culled} culled, {len(physical_passes)} physical),"
               f" {barriers} barriers, {len(resources)} resources ({transients} transient, {memoryless} memoryless)")
    if timed:
        summary += f", {sum(time or 0 for time in times):.3f} GPU ms"
    if profiled:
        summary += f" (means over {profiled} profiled frames)"

    totals = f"<p>{html.escape(summarize_totals(dump))}</p>\n" if "totals" in dump else ""
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
{totals}<nav><a href="#graph">Graph</a><a href="#lifetimes">Lifetimes</a><a href="#grid">Grid</a></nav>
<section id="graph">
{GRAPH_LEGEND}
<div id="graph-view"><div id="graph-canvas"></div><div id="graph-panel"></div></div>
</section>
<section id="lifetimes" hidden>
{lifetimes_table(dump)}
<p>{html.escape(LIFETIMES_LEGEND)}</p>
</section>
<section id="grid" hidden>
{grid_table(dump, frames)}
<p>{html.escape(LEGEND)}</p>
{report}</section>
<script type="application/json" id="graph-model">{script_json(graph_model(dump))}</script>
<script>{load_elk()}</script>
<script>{LAYOUT_SCRIPT}{VIEW_SCRIPT}</script>
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
    output_path = args.output or os.path.splitext(args.dump)[0] + ".html"
    with open(output_path, "w", encoding="utf-8") as html_file:
        html_file.write(render_html(dump, os.path.basename(args.dump)))
    print(f"Wrote {output_path}")


if __name__ == "__main__":
    main()
