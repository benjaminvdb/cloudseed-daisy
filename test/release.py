#!/usr/bin/env python3
"""Release gates and firmware-check failures, without an Arm compiler."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
REPORT = """Memory region         Used Size  Region Size  %age Used
           FLASH:      110392 B       128 KB     84.22%
         DTCMRAM:      106240 B       128 KB     81.05%
            SRAM:      487416 B       512 KB     92.97%
          RAM_D2:      293952 B       288 KB     99.67%
           SDRAM:      15600 KB        64 MB     23.80%
"""


class ReleaseChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = pathlib.Path(self.temp.name)
        self.repo = self.work / "repo"
        self.repo.mkdir()
        self.bin = self.work / "bin"
        self.bin.mkdir()
        self.lib = self.work / "libDaisy"
        (self.lib / "build").mkdir(parents=True)
        (self.lib / "build/libdaisy.a").touch()
        self.env = dict(os.environ, LIBDAISY_DIR=str(self.lib),
                        PATH=str(self.bin) + os.pathsep + os.environ["PATH"])
        self.env.pop("GITHUB_OUTPUT", None)
        self.env.pop("GITHUB_STEP_SUMMARY", None)

    def run_command(self, *args, success=True):
        result = subprocess.run(args, cwd=self.repo, env=self.env,
                                text=True, capture_output=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def executable(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/bash\nset -eu\n" + body)
        path.chmod(0o755)

    def init_git(self):
        self.run_command("git", "init", "-q", "-b", "main")
        self.run_command("git", "config", "user.name", "release fixture")
        self.run_command("git", "config", "user.email", "fixture@invalid")

    def commit(self):
        self.run_command("git", "add", ".")
        self.run_command("git", "-c", "commit.gpgsign=false", "commit", "-qm", "fixture")

    def metadata(self):
        (self.repo / "src/cloudseed_daisy").mkdir(parents=True)
        (self.repo / "src/cloudseed_daisy/version.h").write_text(
            '#define CLOUDSEED_DAISY_VERSION_MAJOR 0\n'
            '#define CLOUDSEED_DAISY_VERSION_MINOR 1\n'
            '#define CLOUDSEED_DAISY_VERSION_PATCH 0\n'
            '#define CLOUDSEED_DAISY_VERSION_STRING "0.1.0"\n')
        (self.repo / "CHANGELOG.md").write_text("## [0.1.0] - 2026-09-13\n")
        self.init_git()
        self.commit()
        self.run_command("git", "tag", "v0.1.0")

    def verify(self, tag="v0.1.0", success=True):
        return self.run_command("python3", str(ROOT / "scripts/verify-release.py"),
                                tag, success=success)

    def test_release_metadata(self):
        self.metadata()
        self.env["GITHUB_OUTPUT"] = str(self.work / "output")
        self.verify()
        self.assertEqual((self.work / "output").read_text(), "version=0.1.0\n")
        for heading in ("## [0a1b0]", "## [0.1.0]suffix", "## [0.1.0-rc.1]",
                        "## [0.1.0]\n## [0.1.0]", "## [Unreleased]"):
            with self.subTest(heading=heading):
                (self.repo / "CHANGELOG.md").write_text(heading + "\n")
                self.verify(success=False)

    def test_wrong_commit_and_annotated_tag(self):
        self.metadata()
        self.run_command("git", "-c", "tag.gpgsign=false", "tag", "-afm", "release", "v0.1.0")
        self.verify()
        (self.repo / "another-file").touch()
        self.commit()
        self.verify(success=False)

    def test_bad_versions(self):
        self.metadata()
        for tag in ("v00.1.0", "v0.01.0", "v0.1.00", "v0.1.0-rc.1", "v0.1.0+build", "0.1.0"):
            with self.subTest(tag=tag):
                self.verify(tag, success=False)
        path = self.repo / "src/cloudseed_daisy/version.h"
        original = path.read_text()
        for header in (original.replace('"0.1.0"', '"0.1.1"'),
                       original.replace("VERSION_PATCH 0", "VERSION_PATCH 1"),
                       original + "\n#define CLOUDSEED_DAISY_VERSION_PATCH 0\n"):
            path.write_text(header)
            self.verify(success=False)

    def size_fixture(self):
        (self.repo / "test").mkdir()
        (self.repo / "examples/seed").mkdir(parents=True)
        shutil.copy(ROOT / "test/size.sh", self.repo / "test/size.sh")
        # An independent fixture budget, unaffected by future footprint updates.
        (self.repo / "test/size_budget.txt").write_text("".join(
            f"{build:8s} {region:9s} {maximum}\n"
            for build in ("default", "profile")
            for region, maximum in (("FLASH", 114448), ("DTCMRAM", 106752),
                                    ("SRAM", 487928), ("RAM_D2", 294464),
                                    ("SDRAM", 15974400))))
        self.executable("make", 'printf "%s" "$MEMORY_REPORT"\n')
        self.executable("arm-none-eabi-gcc", 'echo 15.3.1\n')
        self.env["MEMORY_REPORT"] = REPORT

    def size(self, *args, success=True):
        return self.run_command("bash", "test/size.sh", *args, success=success)

    def test_size_exact_bytes_and_update(self):
        self.size_fixture()
        self.env["GITHUB_STEP_SUMMARY"] = str(self.work / "summary")
        self.size()
        self.assertIn("15974400", (self.work / "summary").read_text())
        self.size("--update")
        self.size()
        self.size("--udpate", success=False)

    def test_size_rejects_incomplete_or_malformed_reports_without_updating(self):
        self.size_fixture()
        budget = self.repo / "test/size_budget.txt"
        original = budget.read_bytes()
        for report in ("", REPORT.splitlines()[0] + "\nITCMRAM: 0 B 64 KB 0.0%\n",
                       REPORT.replace("RAM_D2", "RENAMED"),
                       REPORT + "RAM_D2: 293952 B 288 KB 99.67%\n",
                       REPORT.replace("110392 B", "110392 XB"),
                       REPORT.replace("110392 B", "110.392 KB")):
            with self.subTest(report=report):
                self.env["MEMORY_REPORT"] = report
                self.size(success=False)
                self.size("--update", success=False)
                self.assertEqual(budget.read_bytes(), original)

    def test_size_growth_and_invalid_budgets(self):
        self.size_fixture()
        self.env["MEMORY_REPORT"] = REPORT.replace("293952", "294465")
        self.size(success=False)
        self.env["MEMORY_REPORT"] = REPORT
        budget = self.repo / "test/size_budget.txt"
        original = budget.read_text()
        for text in (original + "default FLASH 999999\n",
                     original.replace("default  FLASH", "other    FLASH"),
                     original.replace("294464", "invalid")):
            budget.write_text(text)
            self.size(success=False)

    def test_consumer_uses_readme_from_selected_commit(self):
        (self.repo / "test").mkdir()
        shutil.copy(ROOT / "test/consumer.sh", self.repo / "test/consumer.sh")
        readme = self.repo / "README.md"
        readme.write_text("```make\ninclude lib/cloudseed-daisy/cloudseed.mk\n```\n"
                          "```cpp\nint main() { return 0; }\n```\n")
        self.init_git()
        self.commit()
        self.run_command("git", "tag", "v0.1.0")
        readme.write_text("The current documentation has no quick start.\n")
        self.commit()
        # A further uncommitted edit must not affect the selected tag either.
        readme.write_text("```cpp\nint main() { invalid; }\n```\n")
        self.executable("make", "grep -qx 'int main() { return 0; }' main.cpp\n"
                        "mkdir build\ntouch build/myreverb.bin\n")
        self.run_command("bash", "test/consumer.sh", "v0.1.0")
        self.run_command("bash", "test/consumer.sh", success=False)

    def test_all_passes_default_libdaisy_to_children(self):
        (self.repo / "test").mkdir()
        shutil.copy(ROOT / "test/all.sh", self.repo / "test/all.sh")
        for name in ("build", "regression", "engine", "mdma", "trig", "benchmark", "consumer", "size"):
            (self.repo / f"test/{name}.sh").write_text(
                'test -f "$LIBDAISY_DIR/build/libdaisy.a"\n')
        for name in ("render", "release"):
            (self.repo / f"test/{name}.py").touch()
        self.executable("arm-none-eabi-gcc", "exit 0\n")
        del self.env["LIBDAISY_DIR"]
        result = self.run_command("bash", "test/all.sh")
        self.assertNotIn("skipping", result.stdout)


if __name__ == "__main__":
    unittest.main()
