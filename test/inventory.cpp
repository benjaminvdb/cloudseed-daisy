// Prints, for every factory program at the firmware's line counts, the delay
// buffers the reverb processes: where they live after placement (the
// firmware's AXI and D2 pools or the SDRAM), their delays and modulation, and
// what staging them in tightly coupled memory would need. Host only.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "cloudseed/fast_sin.h"
#include "cloudseed/reverb_controller.h"
#include "cloudseed/staging.h"
#include "cloudseed/presets.h"

namespace {

const cloudseed::presets::Preset* const kPrograms[] = {
    &cloudseed::presets::kSmallRoom,
    &cloudseed::presets::kMediumSpace,
    &cloudseed::presets::kNoiseInTheHallway,
    &cloudseed::presets::kHyperplane,
    &cloudseed::presets::kRubiKaFields,
    &cloudseed::presets::kThroughTheLookingGlass,
    &cloudseed::presets::kThe90sAreBack,
    &cloudseed::presets::kDullEchoes,
    &cloudseed::presets::kChorusDelay,
    &cloudseed::presets::kDarkPlate,
};
constexpr int kProgramLines[] = {3, 3, 8, 9, 4, 12, 9, 12, 12, 12};

struct Row {
  const char* kind;
  int channel;
  int size;
  int pool;  // 0 AXI, 1 D2, 2 SDRAM
  int delay;
  float mod, rate;
  int window;  // floats of read window for one block, 0 = not stageable
  int taps;
};

int PoolOf(const float* p, const std::vector<float>& a,
           const std::vector<float>& b) {
  if (p >= a.data() && p < a.data() + a.size()) return 0;
  if (p >= b.data() && p < b.data() + b.size()) return 1;
  return 2;
}

// Read window of a modulated head over the next two blocks: the LFO moves
// the delay by at most mod * 2 pi * rate samples per sample.
int Window(int delay, float mod, float rate, int block) {
  const float spread = mod * 6.2831853f * rate * static_cast<float>(2 * block);
  const int s = static_cast<int>(std::ceil(spread)) + 2;
  const int dmin = delay - static_cast<int>(mod) - 1;
  if (dmin < 2 * block + 2) return 0;
  return block + 2 * s + 1;
}

}  // namespace

int main(int argc, char** argv) {
  const int lines_override = argc > 1 ? std::atoi(argv[1]) : 0;
  cloudseed::FastSin::Init();
  std::vector<float> pool(
      cloudseed::ReverbController::RequiredPoolFloats(48000));
  cloudseed::MemoryPool memory;
  memory.Init(pool.data(), pool.size());
  cloudseed::ReverbController reverb;
  if (!reverb.Init(48000, memory)) return 1;
  // The firmware's pools (448 KB of AXI SRAM, the cacheable D2 SRAM behind
  // libDaisy's DMA buffers and the MDMA list) and its staging memory (the
  // ITCM but its first 32 bytes, and 24 KB of the DTCM), one staging set.
  // The default MDMA build's map puts the entire D2 pool above the first
  // 32 KiB non-cacheable region; AddFastPools() therefore clips no bytes.
  std::vector<float> axi(448 * 1024 / 4), d2(232 * 1024 / 4);
  reverb.AddFastPool(axi.data(), axi.size());
  reverb.AddFastPool(d2.data(), d2.size());
  constexpr int kBlock = cloudseed::kMaxBlockSize;
  std::vector<float> itcm((65536 - 32) / 4), dtcm(24 * 1024 / 4);
  cloudseed::CpuStagingTransport transport;
  cloudseed::Staging<cloudseed::CpuStagingTransport> staging;
  staging.Init(&transport);
  staging.AddMemory(itcm.data(), itcm.size());
  staging.AddMemory(dtcm.data(), dtcm.size());

  for (size_t p = 0; p < sizeof(kPrograms) / sizeof(kPrograms[0]); p++) {
    if (!staging.Unplan()) return 1;
    reverb.LoadPreset(kPrograms[p]->values);
    reverb.SetParameter(cloudseed::Parameter::DryOut, 0.0);
    reverb.SetParameter(cloudseed::Parameter::CutoffEnabled, 1.0);
    int limit = lines_override ? lines_override : kProgramLines[p];
    if (limit > cloudseed::kMaxLines) limit = cloudseed::kMaxLines;
    if (reverb.line_count() > limit) {
      reverb.SetParameter(
          cloudseed::Parameter::LineCount,
          double(limit - 1) / (cloudseed::kPluginLineCount - 1));
      reverb.PlaceBuffers();
    }
    reverb.ClearBuffers();
    if (!staging.Plan(reverb)) return 1;

    std::vector<Row> rows;
    for (int c = 0; c < 2; c++) {
      cloudseed::ReverbChannel& ch = reverb.channel(c);
      cloudseed::Placement list[cloudseed::ReverbChannel::kMaxPlacements];
      const int n =
          ch.CollectPlacements(list, cloudseed::ReverbChannel::kMaxPlacements);
      for (int i = 0; i < n; i++) {
        Row r = {};
        r.channel = c;
        r.size = list[i].size;
        switch (list[i].kind) {
          case cloudseed::Placement::Kind::kAllpass: {
            auto* o = static_cast<cloudseed::ModulatedAllpass*>(list[i].object);
            r.kind = "allpass";
            r.pool = PoolOf(o->memory(), axi, d2);
            r.delay = o->sample_delay;
            r.mod = o->modulation_enabled ? o->mod_amount : 0.f;
            r.rate = o->modulation_enabled ? o->mod_rate : 0.f;
            r.window = Window(r.delay, r.mod, r.rate, kBlock);
            break;
          }
          case cloudseed::Placement::Kind::kDelay: {
            auto* o = static_cast<cloudseed::ModulatedDelay*>(list[i].object);
            r.kind = "delay";
            r.pool = PoolOf(o->memory(), axi, d2);
            r.delay = o->delay();
            r.mod = o->mod_amount;
            r.rate = o->mod_rate;
            r.window = Window(r.delay, r.mod, r.rate, kBlock);
            break;
          }
          case cloudseed::Placement::Kind::kMultitap: {
            auto* o = static_cast<cloudseed::MultitapDiffuser*>(list[i].object);
            r.kind = "multitap";
            r.pool = PoolOf(o->memory(), axi, d2);
            r.taps = o->active_count();
            int stageable = 0;
            for (int t = 0; t < o->active_count(); t++)
              if (o->tap_position(t) >= 2 * kBlock) stageable++;
            r.delay =
                o->active_count() ? o->tap_position(o->active_count() - 1) : 0;
            r.window = stageable * kBlock;
            break;
          }
        }
        rows.push_back(r);
      }
    }
    // Totals per pool: buffers, streams, staging floats (window + block).
    int count[3] = {}, floats[3] = {}, stage[3] = {}, nodes[3] = {},
        unstageable[3] = {};
    long bytes[3] = {};
    for (const Row& r : rows) {
      count[r.pool]++;
      floats[r.pool] += r.size;
      if (r.window == 0) {
        unstageable[r.pool]++;
        continue;
      }
      stage[r.pool] += r.window + kBlock;
      const int windows = r.taps ? r.window / kBlock : 1;
      nodes[r.pool] +=
          2 * (1 + windows);  // write-back + windows, each may split
      bytes[r.pool] += 4L * (r.window + kBlock);
    }
    std::printf(
        "program %zu \"%s\" lines=%d buffers=%zu staged=%d unstaged=%d"
        " taps=%d copies<=%d tcm=%zu/%zu KB prime_copies=%d\n",
        p, kPrograms[p]->name, reverb.line_count(), rows.size(),
        staging.staged_buffers(), staging.unstaged_buffers(),
        staging.staged_taps(), staging.planned_copies(),
        staging.memory_used() * 4 / 1024, staging.memory_size() * 4 / 1024,
        staging.copies_last_block());
    const char* names[3] = {"AXI", "D2", "SDRAM"};
    for (int q = 0; q < 3; q++) {
      std::printf(
          "  %-5s buffers=%3d (%3d unstageable) memory=%7d KB staging=%6d B"
          " nodes<=%3d mdma=%6ld B/block\n",
          names[q], count[q], unstageable[q], floats[q] * 4 / 1024,
          stage[q] * 4, nodes[q], bytes[q]);
    }
    if (argc > 3) {
      for (const Row& r : rows) {
        std::printf(
            "    %-8s ch=%d %-5s size=%6d delay=%6d mod=%6.1f rate=%.2e "
            "window=%d taps=%d\n",
            r.kind, r.channel, names[r.pool], r.size, r.delay, r.mod, r.rate,
            r.window, r.taps);
      }
    }
  }
  return 0;
}
