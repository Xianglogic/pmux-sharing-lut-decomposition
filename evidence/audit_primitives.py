"""Reject mapped primitive modes not covered by formal_primitives.v."""
import json
from pathlib import Path


def audit(path, top):
    module = json.loads(Path(path).read_text(encoding='utf-8'))["modules"][top]
    uses = set()
    for port in module["ports"].values():
        if port["direction"] == "output":
            uses.update(port["bits"])
    for cell in module["cells"].values():
        for name, direction in cell["port_directions"].items():
            if direction == "input":
                uses.update(cell["connections"][name])
    counts = {}
    for name, cell in module["cells"].items():
        typ = cell["type"]
        counts[typ] = counts.get(typ, 0) + 1
        p, c = cell["parameters"], cell["connections"]
        if typ == "$not":
            continue
        if typ == "cycloneiv_lcell_comb":
            assert set(p) <= {"lut_mask", "sum_lutc_input", "dont_touch", "lpm_type"}, name
            assert p.get("sum_lutc_input", "datac") == "datac", name
            assert len(p["lut_mask"]) == 16 and set(p["lut_mask"]) <= {"0", "1"}, name
            assert c.get("cin", []) in ([], ["0"], ["x"]), name
            assert not any(bit in uses for bit in c.get("cout", [])), (name, "carry output used")
            assert all(len(c[key]) == 1 for key in ("dataa", "datab", "datac", "datad", "combout")), name
        elif typ == "dffeas":
            assert set(p) <= {"power_up", "is_wysiwyg"}, name
            assert p.get("power_up", "dontcare") in ("low", "high", "dontcare"), name
            for pin, value in {"prn": "1", "aload": "0"}.items():
                assert c[pin] == [value], (name, pin, c[pin])
            assert all(len(c[key]) == 1 for key in ("d", "q", "clk", "ena", "clrn", "sclr", "sload", "asdata")), name
        else:
            raise ValueError(f"unmodeled cell: {name}: {typ}")
    return {"pass": True, "cell_counts": counts,
            "boundary": "LUT datac mode, unused carry; DFF positive clock, asynchronous active-low clear, enable before synchronous clear, declared power_up"}
