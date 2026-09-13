#!/usr/bin/env python3
"""Check a release tag against the checked-out commit and release metadata."""
import os
import pathlib
import re
import subprocess
import sys


def check(tag):
    if not re.fullmatch(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", tag):
        raise ValueError(f"{tag} is not vMAJOR.MINOR.PATCH (no leading zeroes)")
    version = tag[1:]
    tagged = subprocess.check_output(
        ["git", "rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}"], text=True
    ).strip()
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    if tagged != head:
        raise ValueError(f"{tag} names {tagged}, but HEAD is {head}")

    header_path = pathlib.Path("src/cloudseed_daisy/version.h")
    header = header_path.read_text()
    for name, expected in zip(("MAJOR", "MINOR", "PATCH"), version.split(".")):
        values = re.findall(rf"^#define CLOUDSEED_DAISY_VERSION_{name} ([0-9]+)$",
                            header, re.MULTILINE)
        if values != [expected]:
            raise ValueError(f"{header_path}: {name} must be {expected}, got {values}")
    strings = re.findall(r'^#define CLOUDSEED_DAISY_VERSION_STRING "(.*)"$',
                         header, re.MULTILINE)
    if strings != [version]:
        raise ValueError(f"VERSION_STRING must be {version}, got {strings}")

    headings = re.findall(rf"^## \[{re.escape(version)}\](?: - [^\n]+)?$",
                          pathlib.Path("CHANGELOG.md").read_text(), re.MULTILINE)
    if len(headings) != 1:
        raise ValueError(f"CHANGELOG.md needs exactly one '## [{version}]' section")

    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a") as output:
            output.write(f"version={version}\n")
    print(f"{tag} is {head}, and the header and changelog agree")


if __name__ == "__main__":
    try:
        if len(sys.argv) != 2:
            raise ValueError("usage: verify-release.py vMAJOR.MINOR.PATCH")
        check(sys.argv[1])
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(f"release: {error}")
