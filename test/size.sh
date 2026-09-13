#!/bin/bash
#
# The example's static footprint, against a budget.
#
# A region that overflows already fails the link; this suite exists for the
# margin *before* that. The library leaves an application under a kilobyte of
# D2 SRAM (README, "Memory and flash"), so a change that quietly takes another
# 512 bytes still links here and overflows in someone else's firmware. The
# budget in test/size_budget.txt is a ceiling per region and per build: when a
# change raises a figure, the figure and the README's table are what get
# reviewed, not the link.
#
# Usage: ./size.sh [--update]
#   --update   rewrite the budget to the measured figures (deliberate growth)
#
# Needs LIBDAISY_DIR (a built libDaisy) and the Arm toolchain. Writes a table
# to $GITHUB_STEP_SUMMARY when GitHub Actions sets it.
set -euo pipefail
export LC_ALL=C

HERE=$(cd "$(dirname "$0")" && pwd)
BUDGET=$HERE/size_budget.txt
LIBDAISY_DIR=${LIBDAISY_DIR:?LIBDAISY_DIR must point at a built libDaisy checkout}
LIBDAISY_DIR=$(cd "$LIBDAISY_DIR" && pwd)
UPDATE=0
case $#:${1:-} in
  0:) ;;
  1:--update) UPDATE=1 ;;
  *) echo 'usage: size.sh [--update]' >&2; exit 1 ;;
esac

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The regions the library occupies; the rest are the application's.
REGIONS='FLASH DTCMRAM SRAM RAM_D2 SDRAM'

# Headroom --update leaves above the measured figure, per region. The RAM
# regions are pools and objects the build options size, so they are the same
# whatever compiles them and their budgets are near exact; code size is not,
# so FLASH carries an allowance for a toolchain or a libDaisy revision other
# than the one the budget was taken with.
headroom() {  # headroom <region>
  case $1 in
    FLASH) echo 4096 ;;
    SDRAM) echo 0 ;;
    *)     echo 512 ;;
  esac
}

measure() {  # measure <build name> <make options...>
  local build=$1; shift
  make -C "$HERE/../examples/seed" -s \
    LIBDAISY_DIR="$LIBDAISY_DIR" BUILD_DIR="$WORK/$build" "$@" \
    > "$WORK/$build.log" 2>&1 || {
      echo "size: the $build build failed" >&2
      tail -20 "$WORK/$build.log" >&2
      exit 1
    }
  # libDaisy links with -Wl,--print-memory-usage; its table is the source.
  awk -v build="$build" -v required="$REGIONS" '
    function bytes(value, unit) {
      if (value !~ /^[0-9]+$/ || unit !~ /^(B|KB|MB|GB)$/) {
        print "size: invalid memory figure: " value " " unit > "/dev/stderr"
        invalid = 1
        return 0
      }
      return value * (unit == "GB" ? 1073741824 : unit == "MB" ? 1048576 :
                      unit == "KB" ? 1024 : 1)
    }
    BEGIN { count = split(required, regions, " ") }
    /^Memory region/ { table = 1; next }
    table && $1 ~ /:$/ {
      region = substr($1, 1, length($1) - 1)
      used = bytes($2, $3)
      size = bytes($4, $5)
      if (++seen[region] != 1 || size <= 0 || used > size) invalid = 1
      printf "%s %s %.0f %.0f\n", build, region, used, size
    }
    END {
      for (i = 1; i <= count; i++) {
        if (seen[regions[i]] != 1) {
          print "size: " build " needs exactly one " regions[i] " region" > "/dev/stderr"
          invalid = 1
        }
      }
      if (invalid) exit 1
    }
  ' "$WORK/$build.log" > "$WORK/$build.sizes"
  [ -s "$WORK/$build.sizes" ] || {
    echo "size: the $build link printed no memory usage" >&2; exit 1; }
}

measure default
measure profile CLOUDSEED_PROFILE=1

cat "$WORK"/default.sizes "$WORK"/profile.sizes > "$WORK/measured"

budget_for() {  # budget_for <build> <region>
  awk -v b="$1" -v r="$2" '
    $1 == b && $2 == r { max = $3; found++ }
    END { if (found == 1 && max ~ /^[0-9]+$/) print max }
  ' "$BUDGET"
}

if [ "$UPDATE" = 1 ]; then
  {
    echo '# The example firmware'"'"'s static footprint, as a ceiling per build'
    echo '# and region (bytes), each the measured figure plus the headroom'
    echo '# test/size.sh allows it. --update rewrites this file; a raised'
    echo '# figure belongs in the same commit as the change that raised it,'
    echo '# and in the README'"'"'s "Memory and flash" table.'
    echo '#'
    echo "# Measured with $(arm-none-eabi-gcc -dumpfullversion)."
    echo '# build   region   max bytes'
    while read -r build region used _; do
      case " $REGIONS " in
        *" $region "*)
          printf '%-8s %-9s %s\n' "$build" "$region" "$((used + $(headroom "$region")))" ;;
      esac
    done < "$WORK/measured"
  } > "$BUDGET"
  echo "size: budget rewritten from this build"
fi

printf '%-8s %-9s %10s %10s %10s %7s  %s\n' build region used capacity free used% verdict
status=0
report=$WORK/report
: > "$report"
while read -r build region used size; do
  case " $REGIONS " in *" $region "*) ;; *) continue ;; esac
  max=$(budget_for "$build" "$region")
  free=$((size - used))
  pct=$(awk -v u="$used" -v s="$size" 'BEGIN { printf "%.1f", 100 * u / s }')
  if [ -z "$max" ]; then
    verdict="missing, duplicate or invalid budget"; status=1
  elif [ "$used" -gt "$max" ]; then
    verdict="OVER by $((used - max)) B (budget $max)"; status=1
  else
    verdict="ok (budget $max)"
  fi
  printf '%-8s %-9s %10s %10s %10s %6s%%  %s\n' \
    "$build" "$region" "$used" "$size" "$free" "$pct" "$verdict"
  printf '| %s | %s | %s | %s | %s | %s%% | %s |\n' \
    "$build" "$region" "$used" "$size" "$free" "$pct" "$verdict" >> "$report"
done < "$WORK/measured"

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  {
    echo '### Example firmware footprint'
    echo
    echo '| Build | Region | Used | Capacity | Free | Used | Verdict |'
    echo '|---|---|---:|---:|---:|---:|---|'
    cat "$report"
  } >> "$GITHUB_STEP_SUMMARY"
fi

if [ "$status" != 0 ]; then
  echo
  echo 'size: a region grew past its budget. If the growth is intended, run' >&2
  echo '      test/size.sh --update and update the README'"'"'s table.' >&2
  exit 1
fi
echo
echo 'size: every region within budget'
