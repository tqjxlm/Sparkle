"""Tests for the static render graph viewer."""

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = importlib.util.spec_from_file_location(
    "render_graph_viewer", os.path.join(PROJECT_ROOT, "dev", "render_graph_viewer.py"))
render_graph_viewer = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = render_graph_viewer
SPEC.loader.exec_module(render_graph_viewer)

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
        {"name": "ShadowMap", "kind": "Transient", "format": "D32", "size_class": "Absolute", "width": 1024,
         "height": 1024, "first_use": "Shadow", "last_use": "Trace", "usage": "DepthStencilAttachment|Texture",
         "physical": 0},
        {"name": "Debug<1>", "kind": "Transient", "format": "RGBAFloat16", "size_class": "Scene"},
        {"name": "Counter", "kind": "Imported", "first_use": "Trace", "last_use": "Trace",
         "usage": "StorageRead|StorageWrite(Compute)"},
        {"name": "TLAS", "kind": "Imported", "first_use": "Trace", "last_use": "Trace",
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
        self.assertIn('<td class="w barrier" title="Shadow / ShadowMap\naccess DepthWrite clear\n'
                      "attachment depth: Clear (clear) / Store (read by Trace)\n"
                      'barrier Undefined -&gt; DepthStencilOutput: Sampled(Compute) -&gt; DepthWrite">'
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

        self.assertIn('<td class="w" title="Debug&lt;View&gt; / Debug&lt;1&gt;\naccess ColorWrite">W</td>'
                      '<td></td><td></td>', culled)
        self.assertIn('</td><td class="life"></td><td class="w"', culled)

    def test_buffer_cells_show_read_write_and_memory_barriers(self):
        trace = rows(render_graph_viewer.render_html(DUMP, "fixture"))[2]

        self.assertIn('<td class="rw barrier" title="Trace / Counter\naccess StorageRead|StorageWrite(Compute)\n'
                      'barrier: CopyDst -&gt; StorageRead|StorageWrite(Compute)">RW</td>', trace)
        self.assertIn('<td class="r" title="Trace / TLAS\naccess AccelerationStructureRead(Compute)">R</td>', trace)

    def test_renders_a_graph_without_timings_or_buffers(self):
        shadow, debug, trace = ({key: value for key, value in graph_pass.items() if key != "gpu_ms"}
                                for graph_pass in DUMP["passes"])
        trace.update(accesses=trace["accesses"][:1], barriers=trace["barriers"][:1])
        page = render_graph_viewer.render_html(
            {"passes": [shadow, debug, trace], "resources": DUMP["resources"][:2]}, "fixture")

        self.assertIn("3 passes (2 live, 1 culled), 2 barriers, 2 resources (2 transient)</p>", page)
        self.assertNotIn("GPU ms", page)
        self.assertIn('<td class="r barrier" title="Trace / ShadowMap', rows(page)[2])

    def test_writes_the_page_next_to_the_dump_by_default(self):
        with tempfile.TemporaryDirectory() as directory:
            dump_path = os.path.join(directory, "render_graph.json")
            with open(dump_path, "w", encoding="utf-8") as dump_file:
                json.dump(DUMP, dump_file)

            with patch.object(sys, "argv", ["render_graph_viewer.py", dump_path]), patch("builtins.print"):
                render_graph_viewer.main()

            with open(os.path.join(directory, "render_graph.html"), encoding="utf-8") as page_file:
                self.assertIn("<title>Render graph: render_graph.json</title>", page_file.read())


if __name__ == "__main__":
    unittest.main()
