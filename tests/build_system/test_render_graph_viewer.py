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
    "passes": [{"name": "Readback", "kind": "Copy", "culled": False,
                "accesses": [{"resource": "Staging", "access": "CopyDst"}], "barriers": [], "attachments": []}],
    "resources": [{"name": "Staging", "type": "Buffer", "kind": "Imported", "first_use": 0, "last_use": 0,
                   "usage": "CopyDst", "final_barrier": {"from": "CopyDst", "to": "HostRead"}}],
}

DUMP = {
    "passes": [
        {
            "name": "Shadow", "kind": "Raster", "culled": False, "gpu_ms": 0.25,
            "accesses": [{"resource": "ShadowMap", "access": "DepthWrite", "clear": True}],
            "barriers": [{"resource": "ShadowMap", "from_layout": "Undefined", "to_layout": "DepthStencilOutput",
                          "from": "Sampled(Compute)", "to": "DepthWrite"}],
            "attachments": [{"resource": "ShadowMap", "slot": "depth", "load": "Clear", "load_reason": "clear",
                             "store": "Store", "store_reason": "read by Trace"}],
        },
        {
            "name": "Debug<View>", "kind": "Raster", "culled": True, "cull_reason": "unread outputs: Debug<1>",
            "accesses": [{"resource": "Debug<1>", "access": "ColorWrite"}],
            "barriers": [],
            "attachments": [],
        },
        {
            "name": "Trace", "kind": "Compute", "culled": False, "gpu_ms": 0.5,
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
            "attachments": [],
        },
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
}


def rows(page):
    """The table body rows of a page, one string per pass."""
    return page.split("<tbody>")[1].split("</tbody>")[0].strip().split("</tr>\n")


class RenderGraphViewerTest(unittest.TestCase):
    def test_summary_counts_passes_barriers_and_transients(self):
        page = render_graph_viewer.render_html(DUMP, "fixture")

        self.assertIn("3 passes (2 live, 1 culled), 3 barriers, 4 resources (2 transient), 0.750 GPU ms", page)

    def test_attachment_cell_shows_load_and_store_with_its_barrier(self):
        shadow = rows(render_graph_viewer.render_html(DUMP, "fixture"))[0]

        self.assertIn("<td>0.250</td>", shadow)
        self.assertIn('<td class="w barrier" title="Shadow / ShadowMap\naccess ShadowMap DepthWrite clear\n'
                      "attachment ShadowMap slot depth: Clear (clear) / Store (read by Trace)\n"
                      'barrier ShadowMap Undefined-&gt;DepthStencilOutput [Sampled(Compute) -&gt; DepthWrite]">'
                      "D<small>clr/st</small></td>", shadow)

    def test_culled_pass_is_greyed_with_its_reason_and_names_are_escaped(self):
        page = render_graph_viewer.render_html(DUMP, "<fixture>")
        culled = rows(page)[1]

        self.assertTrue(culled.startswith('<tr class="culled"><td class="pass">Debug&lt;View&gt;</td>'
                                          "<td>culled: unread outputs: Debug&lt;1&gt;</td><td></td>"))
        self.assertIn("<title>Render graph: &lt;fixture&gt;</title>", page)
        self.assertNotIn("Debug<", page)

    def test_lifetime_shades_passes_between_first_and_last_use(self):
        culled = rows(render_graph_viewer.render_html(DUMP, "fixture"))[1]

        self.assertIn('<td class="w" title="Debug&lt;View&gt; / Debug&lt;1&gt;\naccess Debug&lt;1&gt; ColorWrite">W</td>'
                      '<td></td><td></td>', culled)
        self.assertIn('</td><td class="life"></td><td class="w"', culled)

    def test_lifetime_follows_pass_indices_when_pass_names_repeat(self):
        clear = {"name": "Clear", "kind": "Raster", "culled": False, "accesses": [], "barriers": [],
                 "attachments": []}
        write = dict(clear, accesses=[{"resource": "Map", "access": "ColorWrite"}])
        read = dict(clear, name="Read", accesses=[{"resource": "Map", "access": "Sampled(Pixel)"}])
        page = render_graph_viewer.render_html(
            {"passes": [clear, write, read], "resources": [
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

        self.assertIn('<td class="rw barrier" title="Trace / Counter\naccess Counter StorageRead|StorageWrite(Compute)\n'
                      'barrier Counter [CopyDst -&gt; StorageRead|StorageWrite(Compute)]">RW</td>', trace)
        self.assertIn('<td class="r" title="Trace / TLAS\naccess TLAS AccelerationStructureRead(Compute)">R</td>',
                      trace)

    def test_renders_a_graph_without_timings_or_buffers(self):
        shadow, debug, trace = ({key: value for key, value in graph_pass.items() if key != "gpu_ms"}
                                for graph_pass in DUMP["passes"])
        trace.update(accesses=trace["accesses"][:1], barriers=trace["barriers"][:1])
        page = render_graph_viewer.render_html(
            {"passes": [shadow, debug, trace], "resources": DUMP["resources"][:2]}, "fixture")

        self.assertIn("3 passes (2 live, 1 culled), 2 barriers, 2 resources (2 transient)</p>", page)
        self.assertNotIn("GPU ms", page)
        self.assertIn('<td class="r barrier" title="Trace / ShadowMap', rows(page)[2])

    def test_buffer_header_names_its_final_barrier(self):
        page = render_graph_viewer.render_html(READBACK, "fixture")

        self.assertIn('title="Staging\nImported Buffer\nused Readback..Readback\nusage CopyDst\n'
                      'final barrier [CopyDst -&gt; HostRead]"', page)

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
    def test_projects_one_line_per_pass_access_barrier_attachment_and_resource(self):
        self.assertEqual(graph_shape_test.project(DUMP), [
            "Shadow: Raster",
            "  access ShadowMap DepthWrite clear",
            "  barrier ShadowMap Undefined->DepthStencilOutput [Sampled(Compute) -> DepthWrite]",
            "  attachment ShadowMap slot depth: Clear (clear) / Store (read by Trace)",
            "Debug<View>: culled (unread outputs: Debug<1>)",
            "Trace: Compute",
            "  access ShadowMap Sampled(Compute)",
            "  access Counter StorageRead|StorageWrite(Compute)",
            "  access TLAS AccelerationStructureRead(Compute)",
            "  barrier ShadowMap DepthStencilOutput->Read [DepthWrite -> Sampled(Compute)]",
            "  barrier Counter [CopyDst -> StorageRead|StorageWrite(Compute)]",
            "ShadowMap: Transient D32 Absolute 1024x1024, physical 0, Shadow..Trace, Texture|DepthStencilAttachment",
            "Debug<1>: Transient RGBAFloat16 Scene, no image",
            "Counter: Imported, Trace..Trace, StorageRead|StorageWrite(Compute)",
            "TLAS: Imported, Trace..Trace, AccelerationStructureRead(Compute)",
        ])

    def test_projects_the_final_barrier_of_a_buffer_the_host_reads(self):
        self.assertEqual(graph_shape_test.project(READBACK), [
            "Readback: Copy",
            "  access Staging CopyDst",
            "Staging: Imported, Readback..Readback, CopyDst, final barrier [CopyDst -> HostRead]",
        ])

    def test_names_the_subresources_of_partial_accesses(self):
        face = {"resource": "Cube", "subresources": "mip 1 layer 2"}
        graph_pass = {
            "name": "Face", "kind": "Raster", "culled": False,
            "accesses": [{**face, "access": "ColorWrite"}],
            "barriers": [{**face, "from_layout": "Undefined", "to_layout": "ColorOutput", "from": "None",
                          "to": "ColorWrite"}],
            "attachments": [{**face, "slot": 0, "load": "Load", "load_reason": "imported", "store": "Store",
                             "store_reason": "imported"}],
        }

        self.assertEqual(graph_shape_test.project({"passes": [graph_pass], "resources": []}), [
            "Face: Raster",
            "  access Cube[mip 1 layer 2] ColorWrite",
            "  barrier Cube[mip 1 layer 2] Undefined->ColorOutput [None -> ColorWrite]",
            "  attachment Cube[mip 1 layer 2] slot 0: Load (imported) / Store (imported)",
        ])


if __name__ == "__main__":
    unittest.main()
