#!/usr/bin/env python3
"""Check all factory values and their positional parameter ordering."""
import json
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent
source = pathlib.Path(sys.argv[1]) / "Factory Programs"
enum = (root / "src/cloudseed/parameter.h").read_text().split("enum class Parameter {")[1].split("Count\n")[0]
order = re.findall(r"^\s+(\w+)(?: = 0)?,", enum, re.MULTILINE)
assert len(order) == 46
presets = re.findall(r'constexpr Preset \w+ = \{\s*"([^"]+)",\s*\{(.*?)\}\};',
                     (root / "src/cloudseed/presets.h").read_text(), re.DOTALL)
# The plugin's nine factory programs, and the successor's Dark Plate, which
# has no file in the checkout (presets.h says where it comes from).
assert len(presets) == 10
factory = 0
for name, body in presets:
    fields = re.findall(r"([\d.Ee+-]+),\s*// (\w+)", body)
    assert [key for _, key in fields] == order, name + ": parameter order"
    path = source / (name + ".json")
    if not path.exists():
        assert name == "Dark Plate", name + ": not a factory program"
        continue
    raw = path.read_text(encoding="utf-8-sig")
    # The upstream 90s preset uses unquoted JSON property names.
    raw = re.sub(r"(?m)^(\s*)(\w+)\s*:", r'\1"\2":', raw)
    expected = json.loads(raw)
    actual = {key: float(value) for value, key in fields}
    assert actual == expected, name + ": factory values"
    factory += 1
assert factory == 9
print("All 414 factory values and parameter positions match the checkout;"
      " Dark Plate's parameter positions match")
