#!/usr/bin/env bash
# Builds one demo clip per Cloud Seed program.
#
#   ./make_demos.sh
#
# Every clip plays the same dry phrase, then that phrase through one program,
# so the clips compare. One gain is applied to all of them, so a program that
# is louder than another still sounds louder here. Working files land in
# build/; the published clip and its card are copied to clips/.
#
# Needs a C++14 compiler and ffmpeg. The dry sources in dry/ are CC0; see
# README.md in this directory.
set -euo pipefail
cd "$(dirname "$0")"

out=build
mkdir -p "$out" clips
font=$(fc-match -f '%{file}' "DejaVu Sans" 2>/dev/null || echo /usr/share/fonts/TTF/DejaVuSans.ttf)
accent=0x8a2be2
source_label='guitar chord, snare, piano note'

# program | seconds of tail to render
programs=(
  "Small Room|8"
  "Medium Space|12"
  "Noise in the Hallway|14"
  "Hyperplane|22"
  "Rubi-Ka Fields|14"
  "Through the Looking Glass|26"
  "The 90s Are Back|16"
  "Dull Echoes|22"
  "Chorus Delay|16"
  "Dark Plate|16"
)

slug_of() { echo "$1" | tr '[:upper:] ' '[:lower:]-'; }

echo "==> building the renderer"
g++ -O2 -std=c++14 -ffp-contract=off -I ../src render_demo.cpp ../src/cloudseed/*.cpp -o "$out/render"

# One dry phrase for every program: a strummed chord for the tone, a snare for
# the transient, a piano note to leave ringing, with gaps to hear into.
echo "==> building the dry phrase"
for n in guitar-chord snare piano; do
  [ -f "$out/$n.wav" ] || ffmpeg -v error -y -i "dry/$n.mp3" -ar 48000 -ac 2 -c:a pcm_s16le "$out/$n.wav"
done
ffmpeg -v error -y -i "$out/guitar-chord.wav" -i "$out/snare.wav" -i "$out/piano.wav" \
  -filter_complex "
    [0:a]atrim=0:3.2,afade=t=out:st=2.9:d=0.3[chord];
    anullsrc=r=48000:cl=stereo,atrim=0:0.5[g1];
    anullsrc=r=48000:cl=stereo,atrim=0:0.7[g2];
    [chord][g1][1:a][g2][2:a]concat=n=5:v=0:a=1[a]" \
  -map "[a]" -c:a pcm_s16le "$out/source.wav"
dry_s=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$out/source.wav")
printf '    %.1fs: %s\n' "$dry_s" "$source_label"

echo "==> rendering every program"
peak_max=-99
for entry in "${programs[@]}"; do
  IFS='|' read -r program tail <<< "$entry"
  slug=$(slug_of "$program")
  report=$("$out/render" "$program" "$out/source.wav" "$out/$slug-wet.wav" "$tail")
  echo "    $report"
  echo "$report" | sed -n 's/.*lines= *\([0-9]*\).*/\1/p' > "$out/$slug.lines"
  # the dry phrase, a breath, then the program
  ffmpeg -v error -y -i "$out/source.wav" -i "$out/$slug-wet.wav" -filter_complex \
    "anullsrc=r=48000:cl=stereo,atrim=0:0.7[gap];[0:a][gap][1:a]concat=n=3:v=0:a=1[a]" \
    -map "[a]" -c:a pcm_f32le "$out/$slug-raw.wav"
  peak=$(ffmpeg -v info -i "$out/$slug-raw.wav" -af volumedetect -f null - 2>&1 |
         sed -n 's/.*max_volume: \(-*[0-9.]*\) dB.*/\1/p' | tail -1)
  peak_max=$(awk -v a="$peak_max" -v b="$peak" 'BEGIN { print (b > a) ? b : a }')
done

# One gain for all of them, from the loudest program, so the differences in
# level between programs survive. A lossy encoder overshoots, so leave room and
# check what the decoder returns.
gain=$(awk -v p="$peak_max" 'BEGIN { printf "%.2f", -3.0 - p }')
for attempt in 1 2 3; do
  echo "==> encoding at ${gain} dB (pass $attempt)"
  worst=-99
  for entry in "${programs[@]}"; do
    IFS='|' read -r program tail <<< "$entry"
    slug=$(slug_of "$program")
    lines=$(cat "$out/$slug.lines")

    # Level, then trim the inaudible end of the tail, keeping a second of it.
    ffmpeg -v error -y -i "$out/$slug-raw.wav" -af \
      "volume=${gain}dB,areverse,silenceremove=start_periods=1:start_silence=1:start_threshold=-60dB:detection=peak,areverse" \
      -c:a pcm_s16le "$out/$slug.wav"

    all_s=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$out/$slug.wav")
    wet_x=$(awk -v d="$dry_s" -v a="$all_s" 'BEGIN { printf "%d", 50 + 1180 * (d + 0.7) / a }')
    ffmpeg -v error -y -i "$out/$slug.wav" -filter_complex \
      "showwavespic=s=1180x170:colors=$accent|$accent:split_channels=1:scale=cbrt" \
      -frames:v 1 "$out/$slug-wave.png"
    ffmpeg -v error -y -f lavfi -i "color=c=0x0d1117:s=1280x470" -i "$out/$slug-wave.png" \
      -filter_complex "[0][1]overlay=50:188,
        drawtext=fontfile=$font:text='$program':fontcolor=white:fontsize=56:x=50:y=62,
        drawtext=fontfile=$font:text='Cloud Seed on the Daisy Seed · $lines delay lines':fontcolor=0x7d8590:fontsize=25:x=52:y=136,
        drawtext=fontfile=$font:text='dry':fontcolor=0x7d8590:fontsize=24:x=50:y=384,
        drawtext=fontfile=$font:text='with reverb':fontcolor=$accent:fontsize=24:x=$wet_x:y=384,
        drawtext=fontfile=$font:text='$source_label':fontcolor=0x7d8590:fontsize=24:x=50:y=424" \
      -frames:v 1 "$out/$slug-card.png"

    ffmpeg -v error -y -loop 1 -i "$out/$slug-card.png" -i "$out/$slug.wav" \
      -c:v libx264 -tune stillimage -pix_fmt yuv420p -crf 30 \
      -c:a aac -b:a 192k -shortest -movflags +faststart "$out/$slug.mp4"
    decoded=$(ffmpeg -v info -i "$out/$slug.mp4" \
      -af astats=measure_perchannel=none:measure_overall=Peak_level -f null - 2>&1 |
      sed -n 's/.*Peak level dB: \(.*\)/\1/p' | tail -1)
    worst=$(awk -v a="$worst" -v b="$decoded" 'BEGIN { print (b > a) ? b : a }')
    printf '    %-28s %5.1fs  decoded %6.2f dB\n' "$program" "$all_s" "$decoded"
  done
  awk -v w="$worst" 'BEGIN { exit (w > -1.0) ? 0 : 1 }' || break
  gain=$(awk -v g="$gain" -v w="$worst" 'BEGIN { printf "%.2f", g - (w + 1.0) }')
  echo "    loudest clip decoded at $worst dB; lowering the common gain"
done

cp "$out"/*.mp4 clips/
for entry in "${programs[@]}"; do
  IFS='|' read -r program tail <<< "$entry"
  cp "$out/$(slug_of "$program")-card.png" clips/
done
echo "==> clips/ holds $(ls clips/*.mp4 | wc -l) clips, $(du -sh clips | cut -f1) in total"
