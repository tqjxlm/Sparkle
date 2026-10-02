"""Tests for the static render graph viewer and the graph shape projection built on its descriptions."""

import json
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(PROJECT_ROOT, "dev"))
sys.path.insert(0, os.path.join(PROJECT_ROOT, "tests", "render_graph"))
import graph_shape_test  # noqa: E402
import render_graph_viewer  # noqa: E402

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
    """The table body rows of a page, one string per pass."""
    return page.split("<tbody>")[1].split("</tbody>")[0].strip().split("</tr>\n")


class RenderGraphViewerTest(unittest.TestCase):
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
        self.assertEqual(page.count('class="badge"'), 1)

    def test_writes_the_page_next_to_the_dump_by_default(self):
        with tempfile.TemporaryDirectory() as directory:
            dump_path = os.path.join(directory, "render_graph.json")
            with open(dump_path, "w", encoding="utf-8") as dump_file:
                json.dump(DUMP, dump_file)

            with patch.object(sys, "argv", ["render_graph_viewer.py", dump_path]), patch("builtins.print"):
                render_graph_viewer.main()

            with open(os.path.join(directory, "render_graph.html"), encoding="utf-8") as page_file:
                self.assertIn("<title>Render graph: render_graph.json</title>", page_file.read())


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
