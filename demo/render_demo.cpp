// Renders a dry WAV through one Cloud Seed program, tail included.
//
//   g++ -O2 -std=c++14 -I ../src render_demo.cpp ../src/cloudseed/*.cpp -o render
//   ./render "Medium Space" dry.wav wet.wav 12
//
// Input must be 48 kHz WAV (16-bit PCM or 32-bit float), mono or stereo; the
// output is 32-bit float stereo. The program runs at its own parameters, so
// the dry/wet balance is the preset's own.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "cloudseed/fast_sin.h"
#include "cloudseed/presets.h"
#include "cloudseed/reverb_controller.h"

namespace {

namespace cs = cloudseed::presets;

const cs::Preset* const kAll[] = {
    &cs::kSmallRoom,   &cs::kMediumSpace, &cs::kNoiseInTheHallway,
    &cs::kHyperplane,  &cs::kRubiKaFields, &cs::kThroughTheLookingGlass,
    &cs::kThe90sAreBack, &cs::kDullEchoes, &cs::kChorusDelay, &cs::kDarkPlate,
};

struct Audio {
  std::vector<float> left, right;
};

// A deliberately small RIFF reader: enough for what ffmpeg writes here.
bool ReadWav(const char* path, Audio& audio) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  char riff[12];
  if (std::fread(riff, 1, 12, f) != 12 || std::memcmp(riff, "RIFF", 4) ||
      std::memcmp(riff + 8, "WAVE", 4)) { std::fclose(f); return false; }
  int channels = 0, bits = 0, format = 0;
  uint32_t rate = 0;
  bool ok = false;
  for (;;) {
    char id[4];
    uint32_t size;
    if (std::fread(id, 1, 4, f) != 4 || std::fread(&size, 4, 1, f) != 1) break;
    const long next = std::ftell(f) + size + (size & 1);
    if (!std::memcmp(id, "fmt ", 4)) {
      uint16_t fmt, ch, block, bps;
      uint32_t sr, byte_rate;
      if (std::fread(&fmt, 2, 1, f) != 1 || std::fread(&ch, 2, 1, f) != 1 ||
          std::fread(&sr, 4, 1, f) != 1 || std::fread(&byte_rate, 4, 1, f) != 1 ||
          std::fread(&block, 2, 1, f) != 1 || std::fread(&bps, 2, 1, f) != 1) break;
      format = fmt; channels = ch; rate = sr; bits = bps;
    } else if (!std::memcmp(id, "data", 4)) {
      const uint32_t frames = channels ? size / (channels * (bits / 8)) : 0;
      audio.left.resize(frames);
      audio.right.resize(frames);
      std::vector<uint8_t> raw(size);
      if (std::fread(raw.data(), 1, size, f) != size) break;
      for (uint32_t i = 0; i < frames; i++) {
        for (int c = 0; c < channels; c++) {
          const uint8_t* p = raw.data() + (i * channels + c) * (bits / 8);
          float v = 0.f;
          if (format == 3 && bits == 32) std::memcpy(&v, p, 4);
          else if (bits == 16) { int16_t s; std::memcpy(&s, p, 2); v = s / 32768.f; }
          else if (bits == 24) {
            const int32_t s = (p[0] << 8) | (p[1] << 16) | (int32_t(int8_t(p[2])) << 24);
            v = s / 2147483648.f;
          }
          if (c == 0) audio.left[i] = v;
          if (c == 1 || channels == 1) audio.right[i] = v;
        }
      }
      ok = true;
    }
    if (std::fseek(f, next, SEEK_SET) != 0) break;
  }
  std::fclose(f);
  if (ok && rate != 48000) {
    std::fprintf(stderr, "%s is %u Hz; resample it to 48000 first\n", path, rate);
    return false;
  }
  return ok;
}

bool WriteWav(const char* path, const Audio& audio) {
  std::FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const uint32_t frames = uint32_t(audio.left.size());
  const uint32_t data = frames * 2 * 4;  // stereo, 32-bit float
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f);      u32(36 + data);
  std::fwrite("WAVEfmt ", 1, 8, f);  u32(16);
  u16(3);                            // IEEE float
  u16(2);                            // channels
  u32(48000);                        // sample rate
  u32(48000 * 2 * 4);                // byte rate
  u16(2 * 4);                        // block align
  u16(32);                           // bits per sample
  std::fwrite("data", 1, 4, f);      u32(data);
  for (uint32_t i = 0; i < frames; i++) {
    std::fwrite(&audio.left[i], 4, 1, f);
    std::fwrite(&audio.right[i], 4, 1, f);
  }
  std::fclose(f);
  return true;
}

const cs::Preset* Find(const std::string& name) {
  for (auto* preset : kAll)
    if (name == preset->name) return preset;
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: render <program> <in.wav> <out.wav> [tail_s]\n");
    for (auto* preset : kAll) std::fprintf(stderr, "  %s\n", preset->name);
    return 2;
  }
  const cs::Preset* preset = Find(argv[1]);
  if (!preset) { std::fprintf(stderr, "no program named %s\n", argv[1]); return 2; }

  Audio in;
  if (!ReadWav(argv[2], in)) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
  const double tail = argc > 4 ? std::atof(argv[4]) : 12.0;
  const size_t tail_frames = size_t(tail * 48000);

  cloudseed::FastSin::Init();
  std::vector<float> pool(cloudseed::ReverbController::RequiredPoolFloats(48000));
  cloudseed::MemoryPool memory;
  memory.Init(pool.data(), pool.size());
  cloudseed::ReverbController reverb;
  if (!reverb.Init(48000, memory)) { std::fprintf(stderr, "reverb init failed\n"); return 1; }
  reverb.LoadPreset(preset->values);
  reverb.ClearBuffers();

  Audio out;
  const size_t total = in.left.size() + tail_frames;
  out.left.resize(total, 0.f);
  out.right.resize(total, 0.f);
  constexpr int kBlock = 48;
  float bl[kBlock], br[kBlock], ol[kBlock], orr[kBlock];
  for (size_t i = 0; i < total; i += kBlock) {
    const int n = int(total - i < kBlock ? total - i : kBlock);
    for (int j = 0; j < n; j++) {
      const size_t k = i + j;
      bl[j] = k < in.left.size() ? in.left[k] : 0.f;
      br[j] = k < in.right.size() ? in.right[k] : 0.f;
    }
    reverb.Process(bl, br, ol, orr, n);
    for (int j = 0; j < n; j++) { out.left[i + j] = ol[j]; out.right[i + j] = orr[j]; }
  }
  if (!WriteWav(argv[3], out)) { std::fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
  std::printf("%-26s lines=%2d  %.1fs in, %.1fs out -> %s\n", preset->name,
              reverb.line_count(), in.left.size() / 48000.0, total / 48000.0, argv[3]);
  return 0;
}
