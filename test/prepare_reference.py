#!/usr/bin/env python3
"""Prepare a disposable legacy reference; never edit the original checkout.

The reference needs portability/lifetime fixes and the same explicitly tested
one-pole and seed-dependency corrections as the port. This is a corrected-reference comparison,
not a claim of bit equivalence with the unmodified plugin. See TECHNICAL.md.
"""
import pathlib
import re
import shutil
import sys

source, dest = map(pathlib.Path, sys.argv[1:])
shutil.copytree(source / "CloudSeed.Native", dest)
arrays = {
    "delayBuffer", "output", "buffer", "tempBuffer", "mixedBuffer",
    "filterOutputBuffer", "lineOutBuffer", "outBuffer",
}
for path in dest.rglob("*"):
    if path.suffix not in (".h", ".cpp"):
        continue
    text = path.read_text()
    text = text.replace("AudioLib\\", "AudioLib/")
    text = text.replace("typedef unsigned long long ulong;", "")
    text = text.replace('#include "sha256.h"', '#include "Sha256.h"')
    if path.name == "MathDefs.h":
        # These constants are also provided by glibc's math.h.
        text = re.sub(r"(?m)^#define[ \t]+(M_\w+)[ \t]+(.*)$",
                      r"#ifndef \1\n#define \1 \2\n#endif", text)
    for name in arrays:
        text = re.sub(r"\bdelete " + name + r";", "delete[] " + name + ";", text)

    # Legacy constructors read several scalar members before initializing
    # them (seeds, enable flags, allpass delay/modRate, biquad settings).
    # Class fields have two tabs; locals have three. Initialize only fields.
    def initialize(match):
        kind, fields = match.groups()
        return "\t\t" + kind + " " + ", ".join(
            f.strip() + "{}" for f in fields.split(",")
        ) + ";"

    text = re.sub(
        r"^\t\t(int|unsigned int|double|bool|FilterType|ChannelLR) "
        r"([A-Za-z_][A-Za-z_0-9]*(?:,\s*[A-Za-z_][A-Za-z_0-9]*)*);",
        initialize, text, flags=re.MULTILINE,
    )
    if path.name == "Lp1.h":
        text = text.replace("Output < 0.000000000001", "std::fabs(Output) < 0.000000000001")
    if path.name == "Hp1.h":
        text = text.replace("lpOut < 0.000000000001", "std::fabs(lpOut) < 0.000000000001")
        text = text.replace("Output = 0;", "lpOut = 0; Output = 0;")
        text = text.replace("double Output{};", "double Output{};\n\t\tvoid ClearBuffers() { lpOut = Output = 0; }")
    if path.name == "ReverbChannel.h":
        text = text.replace("highPass.Output = 0;", "highPass.ClearBuffers();")
    if path.name == "AllpassDiffuser.h":
        text = text.replace("double modRate{};", "double modRate{};\n\t\tdouble modAmount{};")
        text = text.replace("void SetModAmount(double amount)\n\t\t{",
                            "void SetModAmount(double amount)\n\t\t{\n\t\t\tmodAmount = amount;")
        text = text.replace(
            "this->seedValues = AudioLib::ShaRandom::Generate(seed, MaxStageCount * 3, crossSeed);\n\t\t\tUpdate();",
            "this->seedValues = AudioLib::ShaRandom::Generate(seed, MaxStageCount * 3, crossSeed);\n\t\t\tUpdate();\n\t\t\tSetModAmount(modAmount);\n\t\t\tSetModRate(modRate);",
        )
    if path.name == "ModulatedAllpass.h":
        text = text.replace("ModAmount = SampleDelay - 1;", "amount = SampleDelay - 1;")
        text = text.replace("if (ModAmount >= SampleDelay)", "double amount = ModAmount;\n\t\t\tif (amount >= SampleDelay)")
        text = text.replace("SampleDelay + ModAmount * mod", "SampleDelay + amount * mod")
    path.write_text(text)
