// Renders a dry WAV through one Cloud Seed program, tail included.
//
//   g++ -O2 -ffp-contract=off -std=c++14 -I ../src render_demo.cpp ../src/cloudseed/*.cpp -o render
//   ./render "Medium Space" dry.wav wet.wav 12
//
// Input must be 48 kHz WAV (16/24-bit PCM or 32-bit float), mono or stereo; the
// output is 32-bit float stereo. The program runs at its own parameters, so
// the dry/wet balance is the preset's own.
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
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

// RIFF integers and samples are little endian, independent of the host.
uint32_t U32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
struct CloseFile {
  void operator()(std::FILE* file) const { std::fclose(file); }
};
using File = std::unique_ptr<std::FILE, CloseFile>;
constexpr size_t kMaxFrames = (UINT32_MAX - 36u) / 8u;

// A small reader for uncompressed mono/stereo WAVs. Validate the complete
// RIFF container and format before allocating or decoding sample data.
bool ReadWav(const char* path, Audio& audio) {
  File file(std::fopen(path, "rb"));
  auto* f = file.get();
  if (!f || std::fseek(f, 0, SEEK_END) != 0) return false;
  const long length = std::ftell(f);
  if (length < 12 || std::fseek(f, 0, SEEK_SET) != 0) return false;
  uint8_t header[12];
  if (std::fread(header, 1, 12, f) != 12 || std::memcmp(header, "RIFF", 4) ||
      std::memcmp(header + 8, "WAVE", 4)) return false;
  const uint64_t end = uint64_t(U32(header + 4)) + 8;
  if (end < 12 || end > uint64_t(length)) return false;

  uint8_t fmt[16] = {};
  bool have_fmt = false, have_data = false;
  uint32_t data_size = 0;
  long data_offset = 0;
  for (uint64_t offset = 12; offset < end;) {
    uint8_t chunk[8];
    if (end - offset < 8 || std::fread(chunk, 1, 8, f) != 8) return false;
    const uint32_t size = U32(chunk + 4);
    const uint64_t next = offset + 8 + uint64_t(size) + (size & 1);
    if (next > end) return false;
    if (!std::memcmp(chunk, "fmt ", 4)) {
      if (have_fmt || size < sizeof(fmt) ||
          std::fread(fmt, 1, sizeof(fmt), f) != sizeof(fmt)) return false;
      have_fmt = true;
    } else if (!std::memcmp(chunk, "data", 4)) {
      if (have_data) return false;
      have_data = true;
      data_offset = std::ftell(f);
      if (data_offset < 0) return false;
      data_size = size;
    }
    // next is bounded by the successfully measured (long) file length.
    if (std::fseek(f, static_cast<long>(next), SEEK_SET) != 0) return false;
    offset = next;
  }
  if (!have_fmt || !have_data) return false;
  const unsigned format = U16(fmt), channels = U16(fmt + 2);
  const uint32_t rate = U32(fmt + 4), byte_rate = U32(fmt + 8);
  const unsigned block = U16(fmt + 12), bits = U16(fmt + 14);
  if ((channels != 1 && channels != 2) ||
      !((format == 1 && (bits == 16 || bits == 24)) ||
        (format == 3 && bits == 32)) ||
      rate != 48000 || block != channels * (bits / 8) ||
      byte_rate != rate * block || data_size % block != 0) return false;
  const size_t frames = data_size / block;
  if (frames > kMaxFrames || std::fseek(f, data_offset, SEEK_SET) != 0)
    return false;
  Audio decoded;
  decoded.left.resize(frames);
  decoded.right.resize(frames);
  for (size_t i = 0; i < frames; ++i) {
    uint8_t frame[8];
    if (std::fread(frame, 1, block, f) != block) return false;
    for (unsigned c = 0; c < channels; ++c) {
      const uint8_t* p = frame + c * (bits / 8);
      float value;
      if (format == 3) {
        const uint32_t word = U32(p);
        std::memcpy(&value, &word, sizeof(value));
        if (!std::isfinite(value)) return false;
      } else if (bits == 16) {
        const int32_t word = U16(p);
        value = (word >= 0x8000 ? word - 0x10000 : word) / 32768.f;
      } else {
        const int32_t word = p[0] | (p[1] << 8) | (p[2] << 16);
        value = (word >= 0x800000 ? word - 0x1000000 : word) / 8388608.f;
      }
      if (c == 0) decoded.left[i] = value;
      if (c == 1 || channels == 1) decoded.right[i] = value;
    }
  }
  audio = std::move(decoded);
  return true;
}

bool WriteWav(const char* path, const Audio& audio) {
  if (audio.left.size() != audio.right.size() ||
      audio.left.size() > kMaxFrames) return false;
  File file(std::fopen(path, "wb"));
  auto* f = file.get();
  if (!f) return false;
  const uint32_t data = static_cast<uint32_t>(audio.left.size() * 8);
  bool ok = true;
  auto bytes = [&](const void* p, size_t size) {
    if (std::fwrite(p, 1, size, f) != size) ok = false;
  };
  auto u32 = [&](uint32_t v) {
    const uint8_t p[] = {uint8_t(v), uint8_t(v >> 8),
                         uint8_t(v >> 16), uint8_t(v >> 24)};
    bytes(p, sizeof(p));
  };
  auto u16 = [&](uint16_t v) {
    const uint8_t p[] = {uint8_t(v), uint8_t(v >> 8)};
    bytes(p, sizeof(p));
  };
  bytes("RIFF", 4);      u32(36 + data);
  bytes("WAVEfmt ", 8);  u32(16);
  u16(3);               // IEEE float
  u16(2);               // channels
  u32(48000);           // sample rate
  u32(48000 * 2 * 4);    // byte rate
  u16(2 * 4);           // block align
  u16(32);              // bits per sample
  bytes("data", 4);      u32(data);
  for (size_t i = 0; i < audio.left.size() && ok; ++i) {
    for (float sample : {audio.left[i], audio.right[i]}) {
      uint32_t word;
      std::memcpy(&word, &sample, sizeof(word));
      u32(word);
    }
  }
  const bool closed = std::fclose(file.release()) == 0;
  return ok && closed;
}

const cs::Preset* Find(const std::string& name) {
  for (auto* preset : kAll)
    if (name == preset->name) return preset;
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc < 4 || argc > 5) {
    std::fprintf(stderr, "usage: render <program> <in.wav> <out.wav> [tail_s]\n");
    for (auto* preset : kAll) std::fprintf(stderr, "  %s\n", preset->name);
    return 2;
  }
  const cs::Preset* preset = Find(argv[1]);
  if (!preset) { std::fprintf(stderr, "no program named %s\n", argv[1]); return 2; }

  Audio in;
  if (!ReadWav(argv[2], in)) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
  char* tail_end = nullptr;
  errno = 0;
  const double tail = argc > 4 ? std::strtod(argv[4], &tail_end) : 12.0;
  if ((argc > 4 && (tail_end == argv[4] || *tail_end != '\0')) ||
      errno == ERANGE || !std::isfinite(tail) || tail < 0.0 ||
      tail * 48000 > double(kMaxFrames - in.left.size())) {
    std::fprintf(stderr, "invalid tail duration or output too large for RIFF\n");
    return 2;
  }
  const size_t tail_frames = static_cast<size_t>(tail * 48000);

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
} catch (const std::exception& error) {
  std::fprintf(stderr, "render failed: %s\n", error.what());
  return 1;
}
