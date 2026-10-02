"""Tests for the static render graph viewer and the graph shape projection built on its descriptions."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(PROJECT_ROOT, "dev"))
sys.path.insert(0, os.path.join(PROJECT_ROOT, "tests", "render_graph"))
import graph_shape_test  # noqa: E402
import render_graph_viewer  # noqa: E402

LOAD_ELK = render_graph_viewer.load_elk

READBACK = {
    "passes": [{"name": "Readback", "kind": "Copy", "culled": False, "physical_pass": 0, "step": 0,
                "accesses": [{"resource": "Staging", "access": "CopyDst"}], "barriers": [],
                "barriers_after": [{"resource": "Staging", "from": "CopyDst", "to": "HostRead"}]}],
    "physical_passes": [{"members": [0], "attachments": []}],
    "resources": [{"name": "Staging", "type": "Buffer", "kind": "Imported", "first_use": 0, "last_use": 0,
                   "usage": "CopyDst"}],
}

DUMP = {
    "passes": [
        {
            "name": "Shadow", "kind": "Raster", "culled": False, "physical_pass": 0, "step": 0,
            "accesses": [{"resource": "ShadowMap", "access": "DepthWrite", "clear": True}],
            "barriers": [{"resource": "ShadowMap", "from_layout": "Undefined", "to_layout": "DepthStencilOutput",
                          "from": "Sampled(Compute)", "to": "DepthWrite"}],
        },
        {
            "name": "Debug<View>", "kind": "Raster", "culled": True, "cull_reason": "unread outputs: Debug<1>",
            "accesses": [{"resource": "Debug<1>", "access": "ColorWrite"}],
            "barriers": [],
        },
        {
            "name": "Trace", "kind": "Compute", "culled": False, "physical_pass": 1, "step": 0,
            "accesses": [
                {"resource": "ShadowMap", "access": "Sampled(Compute)"},
                {"resource": "Counter", "access": "StorageRead|StorageWrite(Compute)"},
                {"resource": "TLAS", "access": "AccelerationStructureRead(Compute)"},
            ],
            "barriers": [
                {"resource": "ShadowMap", "from_layout": "DepthStencilOutput", "to_layout": "Read",
                 "from": "DepthWrite", "to": "Sampled(Compute)"},
                {"resource": "Counter", "from": "CopyDst", "to": "StorageRead|StorageWrite(Compute)"},
            ],
        },
    ],
    "physical_passes": [
        {"members": [0], "break_reason": "NonRasterPass", "breaks": ["NonRasterPass"], "gpu_ms": 0.25,
         "color_bytes_per_pixel": 0,
         "attachments": [{"resource": "ShadowMap", "slot": "depth", "load": "Clear", "load_reason": "clear",
                          "store": "Store", "store_reason": "read by Trace"}]},
        {"members": [2], "gpu_ms": 0.5, "attachments": []},
    ],
    "resources": [
        {"name": "ShadowMap", "type": "Texture", "kind": "Transient", "format": "D32", "size_class": "Absolute",
         "width": 1024, "height": 1024, "first_use": 0, "last_use": 2, "usage": "Texture|DepthStencilAttachment",
         "physical": 0},
        {"name": "Debug<1>", "type": "Texture", "kind": "Transient", "format": "RGBAFloat16", "size_class": "Scene"},
        {"name": "Counter", "type": "Buffer", "kind": "Imported", "first_use": 2, "last_use": 2,
         "usage": "StorageRead|StorageWrite(Compute)"},
        {"name": "TLAS", "type": "AccelerationStructure", "kind": "Imported", "first_use": 2, "last_use": 2,
         "usage": "AccelerationStructureRead(Compute)"},
    ],
    "totals": {"render_passes": 1, "barriers": 3, "load_bytes": 0, "store_bytes": 4194304, "transient_bytes": 4194304,
               "memoryless_bytes": 0},
    "opportunities": ["Shadow|Trace: NonRasterPass; Store ShadowMap 4.19 MB", "Trace"],
    "tile_budget": 32,
}


def rows(page):
    """The grid's body rows of a page, one string per pass."""
    return page.split('<table class="grid">')[1].split("<tbody>")[1].split("</tbody>")[0].strip().split("</tr>\n")


def stub_elk(test):
    """Pages render without the elkjs submodule, which CI test jobs fetch only where they render pages."""
    patcher = patch.object(render_graph_viewer, "load_elk", return_value="/* elk */")
    patcher.start()
    test.addCleanup(patcher.stop)


def graph_pass(name, accesses, physical_pass=0, step=0, barriers=(), **extra):
    return {"name": name, "kind": "Raster", "culled": False, "physical_pass": physical_pass, "step": step,
            "accesses": accesses, "barriers": list(barriers), **extra}


def culled_pass(name, accesses):
    return {"name": name, "kind": "Raster", "culled": True, "cull_reason": "unread outputs", "accesses": accesses,
            "barriers": []}


def texture(name, kind="Transient"):
    return {"name": name, "type": "Texture", "kind": kind}


def edges(model):
    """Each edge as (source name, target name, item labels)."""
    names = {node["id"]: node["name"] for node in model["nodes"]}
    return [(names[edge["source"]], names[edge["target"]], [item["label"] for item in edge["items"]])
            for edge in model["edges"]]


class RenderGraphViewerTest(unittest.TestCase):
    def setUp(self):
        stub_elk(self)

    def test_summary_counts_passes_barriers_and_transients(self):
        page = render_graph_viewer.render_html(DUMP, "fixture")

        self.assertIn("3 passes (2 live, 1 culled, 2 physical), 3 barriers, 4 resources (2 transient, 0 memoryless), 0.750 GPU ms",
                      page)
        self.assertIn("<p>1 render passes, attachment loads 0.00 MB, stores 4.19 MB, transients 4.19 MB (0.00 MB"
                      " memoryless), tile budget 32 B/pixel</p>", page)

    def test_opportunity_report_lists_the_dumped_lines_in_order(self):
        page = render_graph_viewer.render_html(DUMP, "fixture")

        self.assertIn("<h2>Opportunities</h2>\n<ol><li>Shadow|Trace: NonRasterPass; Store ShadowMap 4.19 MB</li>"
                      "<li>Trace</li></ol>", page)

    def test_attachment_cell_shows_load_and_store_with_its_barrier(self):
        shadow = rows(render_graph_viewer.render_html(DUMP, "fixture"))[0]

        self.assertIn('<td class="physical">0.250</td>', shadow)
        self.assertIn('<td class="w barrier" title="Shadow / ShadowMap\naccess ShadowMap DepthWrite clear\n'
                      "attachment ShadowMap slot depth: Clear (clear) / Store (read by Trace)\n"
                      'barrier ShadowMap Undefined-&gt;DepthStencilOutput [Sampled(Compute) -&gt; DepthWrite]">'
                      "D<small>clr/st</small></td>", shadow)

    def test_culled_pass_is_greyed_with_its_reason_and_names_are_escaped(self):
        page = render_graph_viewer.render_html(DUMP, "<fixture>")
        culled = rows(page)[1]

        self.assertTrue(culled.startswith('<tr class="culled"><td class="pass">Debug&lt;View&gt;</td>'
                                          "<td>culled: unread outputs: Debug&lt;1&gt;</td><td></td><td></td>"))
        self.assertIn("<title>Render graph: &lt;fixture&gt;</title>", page)
        self.assertNotIn("Debug<", page)

    def test_lifetime_shades_passes_between_first_and_last_use(self):
        culled = rows(render_graph_viewer.render_html(DUMP, "fixture"))[1]

        self.assertIn('<td class="w" title="Debug&lt;View&gt; / Debug&lt;1&gt;\n'
                      'access Debug&lt;1&gt; ColorWrite">W</td><td></td><td></td>', culled)
        self.assertIn('</td><td class="life"></td><td class="w"', culled)

    def test_lifetime_follows_pass_indices_when_pass_names_repeat(self):
        clear = {"name": "Clear", "kind": "Raster", "culled": False, "physical_pass": 0, "step": 0, "accesses": [],
                 "barriers": []}
        write = dict(clear, physical_pass=1, accesses=[{"resource": "Map", "access": "ColorWrite"}])
        read = dict(clear, name="Read", physical_pass=2, accesses=[{"resource": "Map", "access": "Sampled(Pixel)"}])
        page = render_graph_viewer.render_html(
            {"passes": [clear, write, read],
             "physical_passes": [{"members": [index], "attachments": []} for index in range(3)],
             "resources": [
                {"name": "Map", "type": "Texture", "kind": "Imported", "first_use": 1, "last_use": 2,
                 "usage": "Texture|ColorAttachment"}]}, "fixture")

        first, second, third = rows(page)
        self.assertTrue(first.endswith("<td></td>"))
        self.assertIn('<td class="w"', second)
        self.assertIn('<td class="r"', third)
        self.assertIn('title="Map\nImported Texture\nused Clear..Read\nusage Texture|ColorAttachment"', page)

    def test_resource_headers_name_their_type(self):
        page = render_graph_viewer.render_html(DUMP, "fixture")

        self.assertIn('title="Counter\nImported Buffer\nused Trace..Trace\n', page)
        self.assertIn('title="TLAS\nImported AccelerationStructure\nused Trace..Trace\n', page)
        self.assertIn('title="Debug&lt;1&gt;\nTransient Texture\nRGBAFloat16 Scene\nno image"', page)

    def test_buffer_cells_show_read_write_and_memory_barriers(self):
        trace = rows(render_graph_viewer.render_html(DUMP, "fixture"))[2]

        self.assertIn('<td class="rw barrier" title="Trace / Counter\n'
                      'access Counter StorageRead|StorageWrite(Compute)\n'
                      'barrier Counter [CopyDst -&gt; StorageRead|StorageWrite(Compute)]">RW</td>', trace)
        self.assertIn('<td class="r" title="Trace / TLAS\naccess TLAS AccelerationStructureRead(Compute)">R</td>',
                      trace)

    def test_renders_a_graph_without_timings_or_buffers(self):
        shadow, debug, trace = (dict(graph_pass) for graph_pass in DUMP["passes"])
        trace.update(accesses=trace["accesses"][:1], barriers=trace["barriers"][:1])
        physical_passes = [{key: value for key, value in physical.items() if key != "gpu_ms"}
                           for physical in DUMP["physical_passes"]]
        page = render_graph_viewer.render_html(
            {"passes": [shadow, debug, trace], "physical_passes": physical_passes,
             "resources": DUMP["resources"][:2]}, "fixture")

        self.assertIn("3 passes (2 live, 1 culled, 2 physical), 2 barriers, 2 resources (2 transient, 0 memoryless)</p>", page)
        self.assertNotIn("GPU ms", page)
        self.assertNotIn("render passes", page)
        self.assertNotIn("Opportunities", page)
        self.assertIn('<td class="r barrier" title="Trace / ShadowMap', rows(page)[2])

    def test_cell_marks_and_counts_a_barrier_after_the_pass(self):
        page = render_graph_viewer.render_html(READBACK, "fixture")

        self.assertIn("1 passes (1 live, 0 culled, 1 physical), 1 barriers, 1 resources (0 transient, 0 memoryless)</p>", page)
        self.assertIn('<td class="w barrier-after" title="Readback / Staging\naccess Staging CopyDst\n'
                      'barrier after Staging [CopyDst -&gt; HostRead]">W</td>', rows(page)[0])

    def test_physical_cell_names_members_breaks_and_color_bytes(self):
        shadow = rows(render_graph_viewer.render_html(DUMP, "fixture"))[0]

        self.assertTrue(shadow.startswith('<tr class="band-start">'))
        self.assertIn('<td class="physical" title="physical Shadow, break NonRasterPass\ncolor 0 B/pixel\n'
                      'attachment ShadowMap slot depth: Clear (clear) / Store (read by Trace)">P0</td>', shadow)

    def test_physical_pass_over_the_tile_budget_is_marked(self):
        physical_passes = [dict(DUMP["physical_passes"][0], color_bytes_per_pixel=48, over_tile_budget=True),
                           DUMP["physical_passes"][1]]
        shadow = rows(render_graph_viewer.render_html(dict(DUMP, physical_passes=physical_passes), "fixture"))[0]

        self.assertIn('<td class="physical over-budget" title="physical Shadow, break NonRasterPass\n'
                      "color 48 B/pixel, over the tile budget\n", shadow)

    def test_merged_members_share_their_physical_pass(self):
        base = {"name": "Base", "kind": "Raster", "culled": False, "physical_pass": 0, "step": 0,
                "accesses": [{"resource": "Color", "access": "ColorWrite", "clear": True}], "barriers": []}
        overlay = dict(base, name="Overlay", step=1, accesses=[{"resource": "Color", "access": "ColorWrite"},
                                                               {"resource": "Sky", "access": "Sampled(Pixel)"}])
        page = render_graph_viewer.render_html(
            {"passes": [base, overlay],
             "physical_passes": [{"members": [0, 1], "gpu_ms": 0.125,
                                  "attachments": [{"resource": "Color", "slot": 1, "load": "Clear",
                                                   "load_reason": "clear", "store": "Store",
                                                   "store_reason": "imported"}]}],
             "resources": [{"name": "Color", "type": "Texture", "kind": "Imported", "first_use": 0, "last_use": 1,
                            "usage": "ColorAttachment"},
                           {"name": "Sky", "type": "Texture", "kind": "Imported", "first_use": 1, "last_use": 1,
                            "usage": "Texture"}]}, "fixture")

        first, second = rows(page)
        self.assertIn('rowspan="2" title="physical Base+Overlay\n', first)
        self.assertIn('>P0</td><td class="physical" rowspan="2">0.125</td>', first)
        self.assertTrue(second.startswith('<tr><td class="pass">Overlay</td><td>Raster, step 1</td><td class="w"'))
        self.assertIn("C<small>clr/st</small>", second)
        self.assertIn('<td class="r" title="Overlay / Sky\naccess Sky Sampled(Pixel)">R</td>', second)

    def test_band_spans_a_culled_pass_between_members(self):
        base = {"name": "Base", "kind": "Raster", "culled": False, "physical_pass": 0, "step": 0, "accesses": [],
                "barriers": []}
        culled = dict(base, name="Unused", culled=True, cull_reason="no outputs")
        page = render_graph_viewer.render_html(
            {"passes": [base, culled, dict(base, name="Overlay")],
             "physical_passes": [{"members": [0, 2], "attachments": []}], "resources": []}, "fixture")

        first, second, third = rows(page)
        self.assertIn('rowspan="3"', first)
        self.assertEqual(second, '<tr class="culled"><td class="pass">Unused</td><td>culled: no outputs</td>')
        self.assertEqual(third, '<tr><td class="pass">Overlay</td><td>Raster</td></tr>')

    def test_memoryless_transient_has_a_badge_naming_its_backing(self):
        depth = {"name": "Depth", "type": "Texture", "kind": "Transient", "format": "D32", "size_class": "Scene",
                 "first_use": 0, "last_use": 0, "usage": "DepthStencilAttachment", "physical": 1, "memoryless": True,
                 "backing": "pooled"}
        page = render_graph_viewer.render_html(dict(DUMP, resources=DUMP["resources"] + [depth]), "fixture")

        self.assertIn("5 resources (3 transient, 1 memoryless)", page)
        self.assertIn('title="Depth\nTransient Texture\nD32 Scene\nphysical image 1\n'
                      'memoryless, backed by a pooled image\nused Shadow..Shadow\nusage DepthStencilAttachment">'
                      '<span>Depth</span><br><span class="badge">M</span></th>', page)
        self.assertEqual(page.split('<table class="grid">')[1].count('class="badge"'), 1)

    def test_writes_the_page_next_to_the_dump_by_default(self):
        with tempfile.TemporaryDirectory() as directory:
            dump_path = os.path.join(directory, "render_graph.json")
            with open(dump_path, "w", encoding="utf-8") as dump_file:
                json.dump(DUMP, dump_file)

            with patch.object(sys, "argv", ["render_graph_viewer.py", dump_path]), patch("builtins.print"):
                render_graph_viewer.main()

            with open(os.path.join(directory, "render_graph.html"), encoding="utf-8") as page_file:
                self.assertIn("<title>Render graph: render_graph.json</title>", page_file.read())


class PageTest(unittest.TestCase):
    def setUp(self):
        stub_elk(self)

    def test_page_has_graph_lifetimes_and_grid_tabs_with_the_elk_and_model_scripts(self):
        page = render_graph_viewer.render_html(DUMP, "fixture")

        self.assertIn('<nav><a href="#graph">Graph</a><a href="#lifetimes">Lifetimes</a><a href="#grid">Grid</a></nav>',
                      page)
        self.assertLess(page.index('<section id="graph">'), page.index('<section id="lifetimes" hidden>'))
        self.assertLess(page.index('<section id="lifetimes" hidden>'), page.index('<section id="grid" hidden>'))
        self.assertIn("<script>/* elk */</script>", page)
        self.assertIn("function elkGraph(model, textWidth, ordered)", page)

    def test_embedded_model_cannot_close_its_script(self):
        names = ["</script><b>&", "a\u2028b"]
        dump = {"passes": [graph_pass(name, [{"resource": "T", "access": "ColorWrite"}]) for name in names],
                "physical_passes": [{"members": [0, 1], "attachments": []}], "resources": [texture("T")]}
        page = render_graph_viewer.render_html(dump, "fixture")
        embedded = page.split('<script type="application/json" id="graph-model">')[1].split("</script>")[0]

        self.assertNotIn("<", embedded)
        self.assertNotIn("\u2028", embedded)
        self.assertEqual(json.loads(embedded), render_graph_viewer.graph_model(dump))

    def test_missing_elk_names_the_submodule_command(self):
        with patch.object(render_graph_viewer, "ELK_PATH", os.path.join(PROJECT_ROOT, "tmp", "missing.js")), \
                patch.object(render_graph_viewer, "load_elk", LOAD_ELK):
            with self.assertRaisesRegex(FileNotFoundError, "git submodule update --init --depth 1 thirdparty/elkjs"):
                render_graph_viewer.render_html(DUMP, "fixture")

    def test_lifetimes_chart_spans_first_to_last_use_with_format_bytes_and_image(self):
        page = render_graph_viewer.render_html(DUMP, "fixture")
        lifetimes = page.split('<table class="lifetimes">')[1].split("</table>")[0]
        shadow, debug = lifetimes.split("<tbody>")[1].split("</tr>")[:2]

        self.assertIn('<td class="text">D32 Absolute 1024x1024</td><td>4</td><td>4.19 MB</td><td>0</td>', shadow)
        self.assertIn('<td class="span-transient span-start used" title="Shadow / ShadowMap\n', shadow)
        self.assertIn('>W</td><td class="span-transient culled"></td><td class="span-transient span-end used"', shadow)
        self.assertTrue(debug.endswith('<td class="used culled" title="Debug&lt;View&gt; / Debug&lt;1&gt;\n'
                                       'access Debug&lt;1&gt; ColorWrite">W</td><td></td>'))

    def test_profile_shows_mean_min_max_and_samples(self):
        physical_passes = [dict(physical, profile={"gpu_ms": {"mean": 0.5, "min": 0.25, "max": 1.0, "samples": 9}})
                           for physical in DUMP["physical_passes"]]
        shadow = dict(DUMP["passes"][0], profile={"cpu_ms": {"mean": 0.02, "min": 0.01, "max": 0.04, "samples": 10}})
        dump = dict(DUMP, passes=[shadow] + DUMP["passes"][1:], physical_passes=physical_passes,
                    profile={"frames": 10})
        page = render_graph_viewer.render_html(dump, "fixture")
        model = render_graph_viewer.graph_model(dump)

        self.assertIn(", 1.000 GPU ms (means over 10 profiled frames)</p>", page)
        self.assertIn('<td class="physical">0.500<small>0.250–1.000, 9/10</small></td>', rows(page)[0])
        self.assertEqual(model["groups"][0]["time"], "0.500 [0.250–1.000] ms GPU, 9/10 frames")
        self.assertIn("GPU 0.500 ms [0.250–1.000], 9/10 frames", model["groups"][0]["details"])
        self.assertEqual(model["nodes"][2]["sub"], "Raster · 0.020 ms CPU")
        self.assertIn("CPU 0.020 ms [0.010–0.040], 10/10 frames", model["nodes"][2]["details"])


class GraphModelTest(unittest.TestCase):
    def test_edges_run_from_the_last_writer_and_imports_read_first_are_sources(self):
        model = render_graph_viewer.graph_model(DUMP)

        self.assertEqual([(node["id"], node["name"]) for node in model["nodes"]], [
            ("in2", "Counter"), ("in3", "TLAS"), ("pass0", "Shadow"), ("pass1", "Debug<View>"), ("pass2", "Trace"),
            ("out2", "Counter")])
        self.assertEqual(edges(model), [
            ("Shadow", "Trace", ["ShadowMap Sampled [st]"]),
            ("Counter", "Trace", ["Counter StorageRead|StorageWrite"]),
            ("TLAS", "Trace", ["TLAS AccelerationStructureRead"]),
            ("Trace", "Counter", ["Counter StorageRead|StorageWrite"]),
        ])
        shadow_map, counter, tlas, output = (edge["items"][0] for edge in model["edges"])
        self.assertEqual([shadow_map["barrier"], counter["barrier"], tlas["barrier"], output["barrier"]],
                         [True, True, False, False])
        self.assertEqual(shadow_map["details"], [
            "ShadowMap: Shadow -> Trace", "access ShadowMap Sampled(Compute)",
            "Shadow attachment ShadowMap slot depth: Clear (clear) / Store (read by Trace)",
            "barrier ShadowMap DepthStencilOutput->Read [DepthWrite -> Sampled(Compute)]"])
        self.assertEqual([shadow_map["style"], counter["style"]], ["transient", "imported"])

    def test_groups_hold_live_members_with_title_breaks_and_heat(self):
        model = render_graph_viewer.graph_model(DUMP)
        shadow, trace = model["groups"]

        self.assertEqual([shadow["members"], trace["members"]], [["pass0"], ["pass2"]])
        self.assertEqual([shadow["time"], shadow["heat"], trace["heat"]], ["0.250 ms GPU", 0.5, 1.0])
        self.assertEqual([shadow["breaks"], trace["breaks"]], [["break NonRasterPass"], []])
        self.assertEqual([node["group"] for node in model["nodes"] if node["type"] == "pass"],
                         ["physical0", None, "physical1"])

    def test_untimed_groups_have_no_heat(self):
        physical_passes = [{key: value for key, value in physical.items() if key != "gpu_ms"}
                           for physical in DUMP["physical_passes"]]
        groups = render_graph_viewer.graph_model(dict(DUMP, physical_passes=physical_passes))["groups"]

        self.assertEqual([(group["time"], group["heat"]) for group in groups], [("", None), ("", None)])

    def test_merged_members_depend_without_store_and_load(self):
        base = graph_pass("Base", [{"resource": "Color", "access": "ColorWrite", "clear": True}])
        overlay = graph_pass("Overlay", [{"resource": "Color", "access": "ColorWrite"}], step=1)
        attachment = {"resource": "Color", "slot": 0, "load": "Clear", "load_reason": "clear", "store": "Store",
                      "store_reason": "imported"}
        model = render_graph_viewer.graph_model(
            {"passes": [base, overlay], "physical_passes": [{"members": [0, 1], "attachments": [attachment]}],
             "resources": [texture("Color", "Imported")]})

        self.assertEqual(edges(model), [("Base", "Overlay", ["Color ColorWrite"]),
                                        ("Overlay", "Color", ["Color ColorWrite [st]"])])

    def test_an_attachment_its_physical_pass_loads_reads_the_import(self):
        attachment = {"resource": "Color", "slot": 0, "load": "Load", "load_reason": "imported", "store": "Store",
                      "store_reason": "imported"}
        model = render_graph_viewer.graph_model(
            {"passes": [graph_pass("Ui", [{"resource": "Color", "access": "ColorWrite"}])],
             "physical_passes": [{"members": [0], "attachments": [attachment]}],
             "resources": [texture("Color", "Imported")]})

        self.assertEqual(edges(model), [("Color", "Ui", ["Color ColorWrite [ld]"]),
                                        ("Ui", "Color", ["Color ColorWrite [st]"])])

    def test_reads_depend_on_the_writers_of_overlapping_subresources(self):
        def access(name, subresources=None):
            entry = {"resource": "Cube", "access": name}
            return dict(entry, subresources=subresources) if subresources else entry

        passes = [graph_pass("Mip0", [access("ColorWrite", "mip 0")]),
                  graph_pass("Mip1", [access("ColorWrite", "mip 1")], physical_pass=1),
                  graph_pass("ReadMip1", [access("Sampled(Pixel)", "mip 1")], physical_pass=2),
                  graph_pass("ReadAll", [access("Sampled(Pixel)")], physical_pass=3),
                  graph_pass("WriteAll", [access("ColorWrite")], physical_pass=4),
                  graph_pass("ReadMips", [access("Sampled(Pixel)", "mips 0-1")], physical_pass=5)]
        model = render_graph_viewer.graph_model(
            {"passes": passes, "physical_passes": [{"members": [index], "attachments": []} for index in range(6)],
             "resources": [texture("Cube")]})

        self.assertEqual(edges(model), [
            ("Mip1", "ReadMip1", ["Cube[mip 1] Sampled"]),
            ("Mip0", "ReadAll", ["Cube Sampled"]),
            ("Mip1", "ReadAll", ["Cube Sampled"]),
            ("Mip0", "WriteAll", ["Cube ColorWrite"]),
            ("Mip1", "WriteAll", ["Cube ColorWrite"]),
            ("WriteAll", "ReadMips", ["Cube[mips 0-1] Sampled"]),
        ])

    def test_subresource_ranges_parse_single_and_spanned_mips_and_layers(self):
        ranges = render_graph_viewer.subresource_ranges

        self.assertEqual(ranges({}), (None, None))
        self.assertEqual(ranges({"subresources": "mip 1 layer 2"}), ((1, 1), (2, 2)))
        self.assertEqual(ranges({"subresources": "mips 0-4"}), ((0, 4), None))
        self.assertEqual(ranges({"subresources": "layers 3-5"}), (None, (3, 5)))

    def test_culled_passes_depend_on_any_writer_and_live_ones_skip_culled_writers(self):
        write = [{"resource": "Color", "access": "ColorWrite"}]
        read = [{"resource": "Color", "access": "Sampled(Pixel)"}, {"resource": "Sky", "access": "Sampled(Pixel)"}]
        passes = [graph_pass("Live", write), culled_pass("CulledWrite", write), culled_pass("CulledRead", read[:1]),
                  graph_pass("LiveRead", read[:1], physical_pass=1), culled_pass("SkyRead", read[1:])]
        model = render_graph_viewer.graph_model(
            {"passes": passes, "physical_passes": [{"members": [0], "attachments": []},
                                                   {"members": [3], "attachments": []}],
             "resources": [texture("Color"), texture("Sky", "Imported")]})

        self.assertEqual([(source, target, edge["culled"]) for (source, target, _), edge
                          in zip(edges(model), model["edges"])], [
            ("Live", "CulledWrite", True), ("CulledWrite", "CulledRead", True), ("Live", "LiveRead", False),
            ("Sky", "SkyRead", True)])
        self.assertTrue(model["nodes"][0]["culled"])
        self.assertEqual([node["sub"] for node in model["nodes"][1:]],
                         ["Raster", "culled", "culled", "Raster", "culled"])

    def test_a_stored_transient_no_later_live_pass_uses_gets_an_unread_sink(self):
        stored = {"resource": "Color", "slot": 0, "load": "Clear", "load_reason": "clear", "store": "Store",
                  "store_reason": "read by Unused"}
        passes = [graph_pass("Draw", [{"resource": "Color", "access": "ColorWrite", "clear": True}]),
                  culled_pass("Unused", [{"resource": "Color", "access": "Sampled(Pixel)"}])]
        model = render_graph_viewer.graph_model(
            {"passes": passes, "physical_passes": [{"members": [0], "attachments": [stored]}],
             "resources": [texture("Color")]})

        sink = model["nodes"][-1]
        self.assertEqual((sink["id"], sink["type"], sink["style"]), ("out0", "out", "transient unread"))
        self.assertEqual(edges(model), [("Draw", "Unused", ["Color Sampled [st]"]),
                                        ("Draw", "Color", ["Color stored, unread"])])
        self.assertEqual(model["edges"][1]["style"], "unread")

    def test_a_read_back_import_marks_its_barrier_after_on_the_output(self):
        model = render_graph_viewer.graph_model(READBACK)

        self.assertEqual(edges(model), [("Readback", "Staging", ["Staging CopyDst"])])
        self.assertTrue(model["edges"][0]["items"][0]["barrier"])
        self.assertEqual(model["edges"][0]["items"][0]["details"][-1], "barrier after Staging [CopyDst -> HostRead]")


@unittest.skipUnless(shutil.which("node") and os.path.isfile(render_graph_viewer.ELK_PATH),
                     "needs node and the thirdparty/elkjs submodule")
class GraphLayoutTest(unittest.TestCase):
    """Lays the model out with elk under node, measuring text at a fixed width per character."""

    DRIVER = """
const ELK = require(process.argv[2]);
eval(require('fs').readFileSync(process.argv[3], 'utf8'));
const model = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const textWidth = (text, font) => text.length * 7;
new ELK().layout(elkGraph(model, textWidth, true)).then(layout => {
  const boxes = [];
  (function visit(node, parent) {
    for (const child of node.children || []) {
      boxes.push({ id: child.id, x: child.x, y: child.y, w: child.width, h: child.height, parent, group: !!child.children });
      visit(child, child);
    }
  })(layout, null);
  const labels = layout.edges.filter(edge => edge.labels).map(edge => ({ id: edge.id, x: edge.labels[0].x,
    y: edge.labels[0].y, w: edge.labels[0].width, h: edge.labels[0].height }));
  const overlap = (a, b) => a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
  const leaves = boxes.filter(box => !box.group), problems = [];
  for (const box of boxes.filter(box => box.parent)) {
    const p = box.parent;
    if (box.x < p.x || box.y < p.y || box.x + box.w > p.x + p.width || box.y + box.h > p.y + p.height) {
      problems.push(`${box.id} outside ${p.id}`);
    }
  }
  [...leaves, ...labels].forEach((a, index, all) => all.slice(index + 1).forEach(b => {
    if (overlap(a, b)) problems.push(`${a.id} overlaps ${b.id}`);
  }));
  console.log(JSON.stringify({ placed: boxes.map(box => box.id).sort(), problems }));
});
"""

    def layout(self, dump):
        with tempfile.TemporaryDirectory() as directory:
            driver, layout_script = (os.path.join(directory, name) for name in ("driver.js", "layout.js"))
            for path, source in ((driver, self.DRIVER), (layout_script, render_graph_viewer.LAYOUT_SCRIPT)):
                with open(path, "w", encoding="utf-8") as script:
                    script.write(source)
            result = subprocess.run(["node", driver, render_graph_viewer.ELK_PATH, layout_script],
                                    input=json.dumps(render_graph_viewer.graph_model(dump)), capture_output=True,
                                    text=True, check=True)
        return json.loads(result.stdout)

    def test_places_every_node_and_group_without_overlaps(self):
        layout = self.layout(DUMP)

        self.assertEqual(layout["placed"], sorted(["in2", "in3", "out2", "pass0", "pass1", "pass2", "physical0",
                                                   "physical1"]))
        self.assertEqual(layout["problems"], [])

    def test_members_stay_inside_their_merged_physical_pass(self):
        passes = [graph_pass(f"Pass{index}", [{"resource": "Color", "access": "ColorWrite"},
                                              {"resource": f"Input{index}", "access": "Sampled(Pixel)"}], step=index)
                  for index in range(4)]
        dump = {"passes": passes, "physical_passes": [{"members": [0, 1, 2, 3], "attachments": [],
                                                       "breaks": ["SlotConflict(BackBuffer)", "Disabled"]}],
                "resources": [texture("Color")] + [texture(f"Input{index}", "Imported") for index in range(4)]}

        self.assertEqual(self.layout(dump)["problems"], [])


class GraphShapeProjectionTest(unittest.TestCase):
    def test_projects_one_line_per_pass_access_barrier_physical_pass_attachment_and_resource(self):
        self.assertEqual(graph_shape_test.project(DUMP), [
            "Shadow: Raster",
            "  access ShadowMap DepthWrite clear",
            "  barrier ShadowMap Undefined->DepthStencilOutput [Sampled(Compute) -> DepthWrite]",
            "physical Shadow, break NonRasterPass",
            "  attachment ShadowMap slot depth: Clear (clear) / Store (read by Trace)",
            "Debug<View>: culled (unread outputs: Debug<1>)",
            "Trace: Compute",
            "  access ShadowMap Sampled(Compute)",
            "  access Counter StorageRead|StorageWrite(Compute)",
            "  access TLAS AccelerationStructureRead(Compute)",
            "  barrier ShadowMap DepthStencilOutput->Read [DepthWrite -> Sampled(Compute)]",
            "  barrier Counter [CopyDst -> StorageRead|StorageWrite(Compute)]",
            "physical Trace",
            "ShadowMap: Transient D32 Absolute 1024x1024, physical 0, Shadow..Trace, Texture|DepthStencilAttachment",
            "Debug<1>: Transient RGBAFloat16 Scene, no image",
            "Counter: Imported, Trace..Trace, StorageRead|StorageWrite(Compute)",
            "TLAS: Imported, Trace..Trace, AccelerationStructureRead(Compute)",
        ])

    def test_projects_memoryless_but_not_the_backing_the_device_gave(self):
        resources = [{"name": name, "type": "Texture", "kind": "Transient", "format": "D32", "size_class": "Scene",
                      "physical": index, "memoryless": memoryless, "backing": backing}
                     for index, (name, memoryless, backing) in enumerate([("Depth", True, "memoryless"),
                                                                          ("Other", True, "pooled"),
                                                                          ("Stored", False, "pooled")])]

        self.assertEqual(graph_shape_test.project({"passes": [], "physical_passes": [], "resources": resources}), [
            "Depth: Transient D32 Scene, physical 0, memoryless",
            "Other: Transient D32 Scene, physical 1, memoryless",
            "Stored: Transient D32 Scene, physical 2",
        ])

    def test_projects_the_barrier_after_the_pass_that_records_it(self):
        self.assertEqual(graph_shape_test.project(READBACK), [
            "Readback: Copy",
            "  access Staging CopyDst",
            "  barrier after Staging [CopyDst -> HostRead]",
            "physical Readback",
            "Staging: Imported, Readback..Readback, CopyDst",
        ])

    def test_names_the_subresources_of_partial_accesses(self):
        face = {"resource": "Cube", "subresources": "mip 1 layer 2"}
        graph_pass = {
            "name": "Face", "kind": "Raster", "culled": False, "physical_pass": 0, "step": 0,
            "accesses": [{**face, "access": "ColorWrite"}],
            "barriers": [{**face, "from_layout": "Undefined", "to_layout": "ColorOutput", "from": "None",
                          "to": "ColorWrite"}],
        }
        physical = {"members": [0], "attachments": [{**face, "slot": 0, "load": "Load", "load_reason": "imported",
                                                     "store": "Store", "store_reason": "imported"}]}

        self.assertEqual(graph_shape_test.project(
            {"passes": [graph_pass], "physical_passes": [physical], "resources": []}), [
            "Face: Raster",
            "  access Cube[mip 1 layer 2] ColorWrite",
            "  barrier Cube[mip 1 layer 2] Undefined->ColorOutput [None -> ColorWrite]",
            "physical Face",
            "  attachment Cube[mip 1 layer 2] slot 0: Load (imported) / Store (imported)",
        ])

    def test_projects_a_merged_physical_pass_after_its_last_member_with_steps_and_break(self):
        base = {"name": "Base", "kind": "Raster", "culled": False, "physical_pass": 0, "step": 0,
                "accesses": [{"resource": "Color", "access": "ColorWrite", "clear": True}], "barriers": []}
        sky = dict(base, name="Sky", step=1, accesses=[{"resource": "Depth", "access": "DepthTest"}])
        tone = dict(base, name="Tone", physical_pass=1, accesses=[{"resource": "Color", "access": "Sampled(Pixel)"}])
        physical_passes = [
            {"members": [0, 1], "break_reason": "NonLocalRead", "break_resource": "Color",
             "breaks": ["NonLocalRead(Color, Depth)", "Disabled"],
             "attachments": [{"resource": "Color", "slot": 1, "load": "Clear", "load_reason": "clear",
                              "store": "Store", "store_reason": "read by Tone"}]},
            {"members": [2], "attachments": []},
        ]

        self.assertEqual(graph_shape_test.project(
            {"passes": [base, sky, tone], "physical_passes": physical_passes, "resources": []}), [
            "Base: Raster",
            "  access Color ColorWrite clear",
            "Sky: Raster, step 1",
            "  access Depth DepthTest",
            "physical Base+Sky, break NonLocalRead(Color, Depth), Disabled",
            "  attachment Color slot 1: Clear (clear) / Store (read by Tone)",
            "Tone: Raster",
            "  access Color Sampled(Pixel)",
            "physical Tone",
        ])

    def test_projects_pixel_local_reads_kept_and_lowered_with_the_barrier_inside_the_rendering(self):
        write = {"name": "Write", "kind": "Raster", "culled": False, "physical_pass": 0, "step": 0,
                 "accesses": [{"resource": "Color", "access": "ColorWrite", "clear": True}], "barriers": []}
        local = dict(write, name="Local", step=1,
                     accesses=[{"resource": "Color", "access": "PixelLocalRead(Pixel)", "pixel_local_slot": 1}],
                     barriers=[{"resource": "Color", "from_layout": "LocalRead", "to_layout": "LocalRead",
                                "from": "ColorWrite", "to": "PixelLocalRead(Pixel)", "in_rendering": True}])
        lowered = dict(write, name="Lowered", physical_pass=1,
                       accesses=[{"resource": "Color", "access": "Sampled(Pixel)", "pixel_local_slot": 1,
                                  "lowered_reason": "Disabled"}])
        physical_passes = [{"members": [0, 1], "break_reason": "Disabled", "breaks": ["Disabled"], "attachments": []},
                           {"members": [2], "attachments": []}]

        self.assertEqual(graph_shape_test.project(
            {"passes": [write, local, lowered], "physical_passes": physical_passes, "resources": []}), [
            "Write: Raster",
            "  access Color ColorWrite clear",
            "Local: Raster, step 1",
            "  access Color PixelLocalRead(Pixel) slot 1",
            "  barrier in rendering Color LocalRead->LocalRead [ColorWrite -> PixelLocalRead(Pixel)]",
            "physical Write+Local, break Disabled",
            "Lowered: Raster",
            "  access Color Sampled(Pixel) lowered from PixelLocalRead slot 1: Disabled",
            "physical Lowered",
        ])


if __name__ == "__main__":
    unittest.main()
