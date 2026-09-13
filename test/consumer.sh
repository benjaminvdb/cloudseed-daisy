#!/bin/bash
#
# The README's quick start, executed.
#
# Builds a throwaway firmware project that adds this library exactly as
# "Add the library" documents — a git submodule of the published repository,
# a Makefile that includes cloudseed.mk — and whose Makefile and main.cpp are
# the README's own code blocks, extracted from README.md. A drift between the
# documented integration and the library breaks this suite.
#
# The submodule is a *bare clone*, so only committed objects reach the
# fixture: an integration that depends on an untracked file fails here rather
# than in someone's checkout. The consumer is built outside the repository so
# that nothing of this working tree is on its include path.
#
# Usage: ./consumer.sh [ref]
#   ref   the commit, tag or branch to consume; default HEAD. A release
#         workflow passes the tag, which is what a user will check out.
#         Both documentation and sources come from it; local edits are not tested.
#
# Needs LIBDAISY_DIR (a built libDaisy), the Arm toolchain and git.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
REF=${1:-HEAD}
LIBDAISY_DIR=${LIBDAISY_DIR:?LIBDAISY_DIR must point at a built libDaisy checkout}
LIBDAISY_DIR=$(cd "$LIBDAISY_DIR" && pwd)

if [ ! -f "$LIBDAISY_DIR/build/libdaisy.a" ]; then
  echo "consumer: $LIBDAISY_DIR is not built (run make in it first)" >&2
  exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

COMMIT=$(git -C "$REPO" rev-parse --verify "$REF^{commit}")
echo "consumer: building the README's quick start against $REF ($COMMIT)"

# The library as a user obtains it: a clone that holds only committed objects.
git clone --quiet --bare "$REPO" "$WORK/cloudseed-daisy.git"
git --git-dir="$WORK/cloudseed-daisy.git" show "$COMMIT:README.md" > "$WORK/README.md"

# The README's own code blocks. One of each exists; the first is taken.
extract() {  # extract <fence> <file>
  awk -v fence="\`\`\`$1" '
    $0 == fence { inside = 1; next }
    $0 == "```" { if (inside) exit }
    inside { print }
  ' "$2"
}

mkdir "$WORK/consumer"
cd "$WORK/consumer"
git init --quiet -b main
git config user.name 'cloudseed consumer suite'
git config user.email 'consumer@invalid'

extract make "$WORK/README.md" > Makefile
extract cpp "$WORK/README.md" > main.cpp
for f in Makefile main.cpp; do
  [ -s "$f" ] || { echo "consumer: README.md has no $f code block" >&2; exit 1; }
done
# The first block of each kind is the quick start's. Check that it is, so
# that a code block added earlier in the README fails loudly here instead of
# quietly becoming the fixture.
grep -q 'cloudseed.mk' Makefile ||
  { echo "consumer: the first make block in README.md is not the quick start's" >&2; exit 1; }
grep -q 'int main()' main.cpp ||
  { echo "consumer: the first cpp block in README.md is not the quick start's" >&2; exit 1; }

git add Makefile main.cpp
git commit --quiet -m 'consumer fixture'

# git refuses the file transport for submodules since 2.38.1 (CVE-2022-39253);
# allow it for this command alone rather than for the machine.
git -c protocol.file.allow=always submodule add --quiet \
  "file://$WORK/cloudseed-daisy.git" lib/cloudseed-daisy
git -C lib/cloudseed-daisy checkout --quiet --detach "$COMMIT"

# The README's Makefile names lib/libDaisy; give the fixture the real one
# there, so its text is built unmodified.
mkdir -p lib
ln -s "$LIBDAISY_DIR" lib/libDaisy

echo "consumer: --- Makefile ---"
sed 's/^/consumer: | /' Makefile

make -s

test -f build/myreverb.bin || { echo "consumer: no image was produced" >&2; exit 1; }
echo "consumer: the documented integration builds ($(stat -c %s build/myreverb.bin) B image)"
