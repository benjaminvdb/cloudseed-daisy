#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "allpass_diffuser.h"
#include "config.h"
#include "modulated_allpass.h"
#include "modulated_delay.h"
#include "multitap_diffuser.h"
#include "reverb_controller.h"
#include "staged_io.h"

#if CLOUDSEED_STAGED_MEMORY
namespace cloudseed {

// A transport executes the copies between the delay rings and the staging
// memory, in the order they are added. Staging (below) is a template on the
// transport's type and calls these members directly, so that Copy(), which
// the audio callback calls once per copy while it builds a block's list,
// inlines into the list builder (TECHNICAL.md, "DSP kernels"); a virtual
// interface used to cost most of the instructions per copy. A transport
// provides:
//
//   void Begin();                       starts a block's list
//   bool Copy(const float* source, float* destination, int floats);
//                                       adds one copy, which the transport
//                                       may run at once or queue; false when
//                                       the list is full (nothing was added)
//   bool Commit();                      runs the copies added since the last
//                                       commit (a segment of the list, in
//                                       order); the previous segment must be
//                                       complete (Idle())
//   bool Wait(uint32_t* spun);          blocks until the running segment is
//                                       complete; false on a transfer error;
//                                       *spun receives the cycles (or any
//                                       unit) spent waiting, for diagnostics
//   bool Idle() const;                  after a failed Commit() or Wait(),
//                                       memory must not be reused until true;
//                                       a hardware transport may still be
//                                       draining an aborted transfer
//   bool Done() const;                  whether the running segment has
//                                       completed, so that Wait() returns
//                                       without waiting
//   int Capacity() const;               copies one block's list can hold; the
//                                       planner stages no ring whose copies,
//                                       with every one split at a ring wrap,
//                                       would not fit
//
// Copies of one block run in order, so a window read after a block
// write-back sees the written samples. Copies may be added while a segment
// runs. The Daisy Seed's transport is MdmaStagingTransport (mdma_staging.h);
// the tests use QueuedTransport (test/queued_transport.h), which runs its
// copies at Wait() as the MDMA does in the background.

// Copies at once with the CPU: the host implementation, and the fallback on
// the Daisy Seed. Reading a ring sequentially into the staging memory keeps
// an SDRAM row open for the whole window, which the sample-by-sample ring
// access could not.
class CpuStagingTransport {
 public:
  void Begin() {}
  bool Copy(const float* source, float* destination, int floats) {
    memcpy(destination, source, static_cast<size_t>(floats) * sizeof(float));
    return true;
  }
  bool Commit() { return true; }
  bool Wait(uint32_t* spun) {
    if (spun) *spun = 0;
    return true;
  }
  bool Idle() const { return true; }
  bool Done() const { return true; }
  int Capacity() const { return 1 << 30; }
};

// Stages the delay memory of the loaded preset in tightly coupled memory
// (see staged_io.h): every Process() call of a staged ring reads a window
// copied from the ring and writes a block that is copied back, so the
// audio callback never waits for the SDRAM or the cache while a transport
// (the Daisy Seed's MDMA) moves the samples in the background.
//
//   Staging<Transport> staging;
//   staging.Init(&transport);
//   staging.AddMemory(itcm, floats);  // up to kMaxMemories areas
//   reverb.LoadPreset(...); reverb.ClearBuffers();
//   if (!staging.Plan(reverb)) recover();  // prime only valid windows
//   per block, process only if Begin(count) succeeds; discard the output
//   if End() fails, then recover after Unplan() returns true.
//
// The list of a block (the write-back of what the block wrote, and the
// windows of the next block) is streamed: the reverb reports each group of
// heads it has finished with (a channel's early section, then each of its
// lines, see StagedProgress), whose copies are built at once and committed
// as a segment whenever the transport is free, so the copies run while the
// rest of the block is processed and only the last segment runs after it.
// Since a head's block is written back within a few groups, the block
// buffers are shared: kPools groups' worth, a group reusing the buffers of
// the group kPools before it, which the streaming keeps complete. A ring is
// stageable when its shortest delay leaves the windows clear of the block
// still being written (plus the modulation spread); the rest keep their
// ring.
template <class Transport>
class Staging {
 public:
  static constexpr int kMaxMemories = 2;
  static constexpr int kMaxEntries = 2 * ReverbChannel::kMaxPlacements;
  static constexpr int kMaxTapWindows = 2 * kMaxTaps;
  static constexpr int kBlock = kMaxBlockSize;
  // Block buffers: for kPools groups, the most heads a group has (the early
  // section: the pre-delay, the multitap and the early stages).
  static constexpr int kPools = 4;
  static constexpr int kPoolBlocks = 2 + kMaxStages;

  void Init(Transport* transport);
  void AddMemory(float* base, size_t floats);

  // Plans the staging of the preset loaded in the reverb; call after
  // ReverbController::ClearBuffers() and before the next Process(). Fills
  // the windows of the first block from the rings (synchronously) and
  // installs the progress hook (StagedProgress) the reverb reports to.
  // Unplan before changing delay lengths, modulation, taps, stage/line
  // counts or buffer placement, then clear/replan. Decay, tone and freeze
  // do not change window geometry and may be changed while planned.
  bool Plan(ReverbController& reverb);
  // Returns every ring to its owner; call before the reverb changes its
  // buffers (LoadPreset, PlaceBuffers).
  // This is an ownership handoff, not a seamless tail-preserving switch:
  // reload/clear before resuming DSP on the rings.
  // False leaves the plan and its memory owned by a transport that has not
  // stopped yet. Retry outside the audio callback; do not touch the rings.
  bool Unplan();

  // Around every ReverbController::Process(count) call while planned.
  // Use the firmware's fixed kBlock sample blocks: the transport's list
  // budget counts one copy for the write-back of a ring whose size and
  // write index are multiples of kBlock (the placed rings, see
  // ReverbController::PlaceBuffers), which a shorter block would break.
  // Begin() waits for the last segment of the previous block; End() commits
  // what the block's last group left.
  // False means the block must be discarded and the reverb reloaded after
  // Unplan() succeeds. Never process a window after Begin() fails.
  bool Begin(int count);
  bool End();
  // From the reverb, through the hook: the heads of the group (and of the
  // groups before it) are done with the block.
  void Progress(int group);

  // Diagnostics.
  bool planned() const { return planned_; }
  int staged_buffers() const { return entries_; }
  int unstaged_buffers() const { return unstaged_; }
  int staged_taps() const { return staged_taps_; }
  // Copies a block's list needs at most (every window split at a wrap).
  int planned_copies() const { return nodes_; }
  size_t memory_used() const { return used_; }
  size_t memory_size() const { return total_; }
  int copies_last_block() const { return copies_; }
  uint32_t wait_cycles() const { return waited_; }
  bool failed() const { return failed_; }
  // Wait cycles accumulate; the caller resets them when it reports.
  void ResetWaitCycles() { waited_ = 0; }

 private:
  enum class Kind : uint8_t { kAllpass, kDelay, kMultitap };
  struct Entry {
    void* object;
    float* ring;
    int ring_size;
    Kind kind;
    uint8_t taps;   // multitap: active taps
    uint8_t order;  // placement index: the processing order
    uint8_t group;  // the group among those the program runs, in order
    int spread;     // heads: read-position spread over the list horizon
    int length;     // heads: window floats
    int lead;       // heads: the lead the window was built with
    float* window;  // window (heads) or tap windows (multitap)
    float* block;   // block buffer, from the pools
    StagedHead head;
    StagedTaps tap_io;
    const float** tap_windows;  // multitap: active_count pointers
  };

  static void ProgressHook(void* context, int group) {
    static_cast<Staging*>(context)->Progress(group);
  }
  static int WrapIndex(int index, int size);
  static void CopyRing(Transport* transport, const float* source_block,
                       float* ring, int ring_size, int start, int floats,
                       bool& ok, int& copies);
  static void CopyWindow(Transport* transport, const float* ring,
                         int ring_size, int start, float* window, int floats,
                         bool& ok, int& copies);
  float* Allocate(size_t floats);
  void Attach();
  void BuildRange(int first, int last, bool write_back, int ahead);
  bool Commit();

  Transport* transport_ = nullptr;
  ReverbController* reverb_ = nullptr;  // reports its progress while planned
  struct Memory {
    float* base;
    size_t size;
    size_t used;
  } memories_[kMaxMemories];
  int memory_count_ = 0;
  size_t used_ = 0, total_ = 0;

  Entry entries_data_[kMaxEntries];
  const float* tap_window_table_[kMaxTapWindows];
  int entries_ = 0;
  int unstaged_ = 0;
  int staged_taps_ = 0;
  int tap_windows_ = 0;
  int nodes_ = 0;
  int line_count_ = 0;

  bool planned_ = false;
  bool failed_ = false;
  bool list_pending_ = false;
  int block_count_ = kBlock;
  int copies_ = 0;
  int copies_committed_ = 0;
  int next_entry_ = 0;        // the first entry not built for this block
  int committed_group_ = -1;  // groups up to it are committed
  int complete_group_ = -1;   // groups up to it are written back
  uint32_t waited_ = 0;
};

// The implementation. Compiled where a Staging<Transport> is used: the
// firmware's audio callback (cloudseed.cpp) and the tests.

template <class Transport>
void Staging<Transport>::Init(Transport* transport) {
  transport_ = transport;
  memory_count_ = 0;
  used_ = total_ = 0;
  entries_ = unstaged_ = staged_taps_ = tap_windows_ = nodes_ = 0;
  planned_ = failed_ = list_pending_ = false;
}

template <class Transport>
void Staging<Transport>::AddMemory(float* base, size_t floats) {
  if (memory_count_ >= kMaxMemories || base == nullptr || floats == 0) return;
  memories_[memory_count_++] = {base, floats, 0};
  total_ += floats;
}

// Staging memory in whole cache lines (32 bytes), so that a transport
// working in bursts never shares a line between two windows.
template <class Transport>
float* Staging<Transport>::Allocate(size_t floats) {
  const size_t rounded = (floats + 7) & ~static_cast<size_t>(7);
  for (int i = 0; i < memory_count_; i++) {
    Memory& m = memories_[i];
    if (rounded <= m.size - m.used) {
      float* p = m.base + m.used;
      m.used += rounded;
      used_ += rounded;
      return p;
    }
  }
  return nullptr;
}

template <class Transport>
bool Staging<Transport>::Unplan() {
  if (list_pending_ && transport_ != nullptr) {
    uint32_t spun = 0;
    if (!transport_->Wait(&spun)) failed_ = true;
    if (!transport_->Idle()) return false;
    list_pending_ = false;
  }
  if (reverb_ != nullptr) reverb_->SetStagedProgress({nullptr, nullptr});
  reverb_ = nullptr;
  for (int i = 0; i < entries_; i++) {
    Entry& e = entries_data_[i];
    switch (e.kind) {
      case Kind::kAllpass:
        static_cast<ModulatedAllpass*>(e.object)->SetStaged(nullptr);
        break;
      case Kind::kDelay:
        static_cast<ModulatedDelay*>(e.object)->SetStaged(nullptr);
        break;
      case Kind::kMultitap:
        static_cast<MultitapDiffuser*>(e.object)->SetStaged(nullptr);
        break;
    }
  }
  entries_ = unstaged_ = staged_taps_ = tap_windows_ = nodes_ = 0;
  for (int i = 0; i < memory_count_; i++) memories_[i].used = 0;
  used_ = 0;
  planned_ = list_pending_ = false;
  return true;
}

namespace staging_detail {

// The read positions of a modulated head over the horizon of a list (the
// blocks between building the window and using it): the LFO moves the delay
// by at most 2 pi * rate * mod_amount samples per sample, and the integer
// delay truncates.
inline int Spread(float mod_amount, float mod_rate, int horizon_samples) {
  const float samples =
      6.2831853f * mod_rate * mod_amount * static_cast<float>(horizon_samples);
  return static_cast<int>(ceilf(samples)) + 1;
}

// Whether blocks of kMaxBlockSize samples written from index into a ring of
// size floats never cross the ring's end.
inline bool Aligned(int size, int index) {
  return size % kMaxBlockSize == 0 && index % kMaxBlockSize == 0;
}

}  // namespace staging_detail

template <class Transport>
bool Staging<Transport>::Plan(ReverbController& reverb) {
  if (!Unplan()) return false;
  failed_ = false;
  if (transport_ == nullptr || memory_count_ == 0) return true;
  line_count_ = reverb.line_count();

  // The candidates of both channels, most expensive memory first: the rings
  // that stayed in the Init() memory (the SDRAM), then the fast pools from
  // the last one added (the D2 SRAM behind the AXI SRAM on the Seed).
  constexpr int kMax = kMaxEntries;
  Placement list[kMax];
  int n = reverb.channel(0).CollectPlacements(list, kMax);
  n += reverb.channel(1).CollectPlacements(list + n, kMax - n);
  int tier[kMax];
  for (int i = 0; i < n; i++) {
    const float* memory = nullptr;
    switch (list[i].kind) {
      case Placement::Kind::kAllpass:
        memory = static_cast<ModulatedAllpass*>(list[i].object)->memory();
        break;
      case Placement::Kind::kDelay:
        memory = static_cast<ModulatedDelay*>(list[i].object)->memory();
        break;
      case Placement::Kind::kMultitap:
        memory = static_cast<MultitapDiffuser*>(list[i].object)->memory();
        break;
    }
    tier[i] = 0;
    for (int p = 0; p < reverb.fast_pool_count(); p++) {
      const MemoryPool& pool = reverb.fast_pool(p);
      // These are physical memory tiers, potentially distinct allocations;
      // C++ does not specify relational pointer ordering between them.
      const uintptr_t address = reinterpret_cast<uintptr_t>(memory);
      const uintptr_t base = reinterpret_cast<uintptr_t>(pool.base());
      if (address >= base && address - base < pool.size() * sizeof(float))
        tier[i] = reverb.fast_pool_count() - p;
    }
  }
  // Lowest tier (the Init() memory) first; within a tier the multitaps come
  // last: a staged allpass or delay saves more per byte of staging memory
  // than a tap window (kBlock floats for one read stream).
  for (int i = 0; i < n; i++) {
    tier[i] *= 2;
    if (list[i].kind == Placement::Kind::kMultitap) tier[i] += 1;
  }
  int order[kMax];
  for (int i = 0; i < n; i++) order[i] = i;
  for (int i = 1; i < n; i++) {  // stable insertion sort, ascending
    const int o = order[i];
    int j = i;
    while (j > 0 && tier[order[j - 1]] > tier[o]) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = o;
  }

  // The block buffers of every group, before the windows take the memory.
  float* const pool =
      Allocate(static_cast<size_t>(kPools) * kPoolBlocks * kBlock);
  if (pool == nullptr) {
    unstaged_ = n;
    planned_ = true;
    return true;
  }

  // Samples between building a window and the last sample that reads it:
  // the list horizon, plus the block the windows must stay clear of.
  const int horizon = 2 * kBlock;
  const int clear = kBlock;

  for (int k = 0; k < n; k++) {
    const Placement& p = list[order[k]];
    if (entries_ >= kMaxEntries) {
      unstaged_++;
      continue;
    }
    Entry& e = entries_data_[entries_];
    memset(&e, 0, sizeof(e));
    e.object = p.object;
    e.order = static_cast<uint8_t>(order[k]);
    e.group = p.group;
    bool ok = false;
    // Copies of a list for this ring, at worst: a window can split at the
    // ring's end; a block written at a multiple of kBlock into a ring of
    // a multiple of kBlock floats never does (a block per list, kBlock
    // samples each).
    int copies = 0;
    switch (p.kind) {
      case Placement::Kind::kAllpass: {
        auto* o = static_cast<ModulatedAllpass*>(p.object);
        e.kind = Kind::kAllpass;
        e.ring = const_cast<float*>(o->memory());
        e.ring_size = o->buffer_size();
        copies = 2 + (staging_detail::Aligned(e.ring_size, o->write_index())
                          ? 1
                          : 2);
        float mod = o->modulation_enabled ? o->mod_amount : 0.f;
        if (mod >= static_cast<float>(o->sample_delay))
          mod = static_cast<float>(o->sample_delay - 1);
        const int spread = o->modulation_enabled
                               ? staging_detail::Spread(mod, o->mod_rate, horizon)
                               : 0;
        const int min_delay = o->sample_delay - static_cast<int>(mod) - 1;
        if (min_delay - spread < clear + 1) break;
        e.spread = spread;
        e.length = kBlock + 2 * spread + 2;
        ok = true;
        break;
      }
      case Placement::Kind::kDelay: {
        auto* o = static_cast<ModulatedDelay*>(p.object);
        e.kind = Kind::kDelay;
        e.ring = const_cast<float*>(o->memory());
        e.ring_size = o->memory_size();
        copies = 2 + (staging_detail::Aligned(e.ring_size, o->write_index())
                          ? 1
                          : 2);
        if (o->crossfading()) break;
        const float mod = o->mod_amount;
        const int spread = staging_detail::Spread(mod, o->mod_rate, horizon);
        const int min_delay = o->delay() - static_cast<int>(mod) - 1;
        if (min_delay - spread < clear + 1) break;
        e.spread = spread;
        e.length = kBlock + 2 * spread + 2;
        ok = true;
        break;
      }
      case Placement::Kind::kMultitap: {
        auto* o = static_cast<MultitapDiffuser*>(p.object);
        e.kind = Kind::kMultitap;
        e.ring = const_cast<float*>(o->memory());
        e.ring_size = o->memory_size();
        e.taps = static_cast<uint8_t>(o->active_count());
        if (tap_windows_ + e.taps > kMaxTapWindows) break;
        // The multitap writes its ring backwards from its index.
        copies = staging_detail::Aligned(e.ring_size, o->write_index() + 1)
                     ? 1
                     : 2;
        ok = true;
        break;
      }
    }
    if (!ok) {
      unstaged_++;
      continue;
    }

    // Memory for the windows (the blocks come from the pool).
    size_t need = 0;
    int windows = 0;
    if (e.kind == Kind::kMultitap) {
      auto* o = static_cast<MultitapDiffuser*>(p.object);
      for (int t = 0; t < e.taps; t++)
        if (o->tap_position(t) >= clear) windows++;
      need = static_cast<size_t>(windows) * kBlock;
      copies += 2 * windows;
    } else {
      need = static_cast<size_t>(e.length);
    }
    if (nodes_ + copies > transport_->Capacity()) {
      unstaged_++;
      continue;
    }
    e.window = need ? Allocate(need) : nullptr;
    if (need && e.window == nullptr) {
      unstaged_++;
      continue;
    }
    if (e.kind == Kind::kMultitap) {
      auto* o = static_cast<MultitapDiffuser*>(p.object);
      e.tap_windows = &tap_window_table_[tap_windows_];
      int w = 0;
      for (int t = 0; t < e.taps; t++) {
        e.tap_windows[t] =
            o->tap_position(t) >= clear ? e.window + w++ * kBlock : nullptr;
      }
      tap_windows_ += e.taps;
      staged_taps_ += windows;
    }
    nodes_ += copies;
    entries_++;
  }

  // The entries in processing order (the groups in order), so that the
  // heads a group reports done are a prefix of what is left to build.
  uint8_t rank[kMaxEntries];
  for (int i = 0; i < entries_; i++) rank[i] = static_cast<uint8_t>(i);
  for (int i = 1; i < entries_; i++) {
    const uint8_t r = rank[i];
    int j = i;
    while (j > 0 && entries_data_[rank[j - 1]].order > entries_data_[r].order) {
      rank[j] = rank[j - 1];
      j--;
    }
    rank[j] = r;
  }
  // Place entry rank[i] at i, following the permutation's cycles.
  for (int i = 0; i < entries_; i++) {
    if (rank[i] == i) continue;
    Entry saved = entries_data_[i];
    int j = i;
    for (;;) {
      const int from = rank[j];
      rank[j] = static_cast<uint8_t>(j);
      if (from == i) {
        entries_data_[j] = saved;
        break;
      }
      entries_data_[j] = entries_data_[from];
      j = from;
    }
  }
  // Each group's heads take a block each from the group's pool.
  int previous = -1, slot = 0;
  for (int i = 0; i < entries_; i++) {
    Entry& e = entries_data_[i];
    // The groups the program runs, numbered in order across the channels.
    const int group = e.group < kStagedGroups
                          ? e.group
                          : e.group - kStagedGroups + line_count_ + 1;
    if (group != previous) slot = 0;
    previous = group;
    e.group = static_cast<uint8_t>(group);
    e.block = pool + ((group % kPools) * kPoolBlocks + slot++) * kBlock;
  }

  // Fill the windows of the first block from the rings, then hand the
  // staged rings over.
  block_count_ = kBlock;
  transport_->Begin();
  copies_ = copies_committed_ = 0;
  BuildRange(0, entries_, false, 0);
  if (failed_) return false;
  // A rejected commit may itself have encountered a channel still active.
  list_pending_ = true;
  if (!transport_->Commit()) {
    failed_ = true;
    return false;
  }
  uint32_t spun = 0;
  if (!transport_->Wait(&spun)) failed_ = true;
  list_pending_ = !transport_->Idle();
  if (failed_) return false;
  Attach();
  reverb_ = &reverb;
  reverb.SetStagedProgress({&Staging::ProgressHook, this});
  planned_ = true;
  return true;
}

// Hands every staged object its window and block.
template <class Transport>
void Staging<Transport>::Attach() {
  for (int i = 0; i < entries_; i++) {
    Entry& e = entries_data_[i];
    if (e.kind == Kind::kMultitap) {
      e.tap_io.windows = e.tap_windows;
      e.tap_io.block = e.block;
      static_cast<MultitapDiffuser*>(e.object)->SetStaged(&e.tap_io);
    } else {
      e.head.window = e.window;
      e.head.lead = e.lead;
      e.head.length = e.length;
      e.head.block = e.block;
      if (e.kind == Kind::kAllpass)
        static_cast<ModulatedAllpass*>(e.object)->SetStaged(&e.head);
      else
        static_cast<ModulatedDelay*>(e.object)->SetStaged(&e.head);
    }
  }
}

// The two copies of a list per staged ring, one or two nodes each: a block
// written back into the ring, a window read out of it, split where it
// crosses the ring's end. Inlined into BuildRange, with the transport's
// Copy(): the list is built every block, a copy at a time, and these keep
// their bookkeeping in registers (TECHNICAL.md, "DSP kernels").
template <class Transport>
inline int Staging<Transport>::WrapIndex(int index, int size) {
  while (index < 0) index += size;
  while (index >= size) index -= size;
  return index;
}

template <class Transport>
__attribute__((always_inline)) inline void Staging<Transport>::CopyRing(
    Transport* transport, const float* source_block, float* ring,
    int ring_size, int start, int floats, bool& ok, int& copies) {
  start = WrapIndex(start, ring_size);
  const int room = ring_size - start;
  if (floats <= room) {
    ok &= transport->Copy(source_block, ring + start, floats);
    copies += 1;
  } else {
    ok &= transport->Copy(source_block, ring + start, room);
    ok &= transport->Copy(source_block + room, ring, floats - room);
    copies += 2;
  }
}

template <class Transport>
__attribute__((always_inline)) inline void Staging<Transport>::CopyWindow(
    Transport* transport, const float* ring, int ring_size, int start,
    float* window, int floats, bool& ok, int& copies) {
  start = WrapIndex(start, ring_size);
  const int room = ring_size - start;
  if (floats <= room) {
    ok &= transport->Copy(ring + start, window, floats);
    copies += 1;
  } else {
    ok &= transport->Copy(ring + start, window, room);
    ok &= transport->Copy(ring, window + room, floats - room);
    copies += 2;
  }
}

// The copies of the entries [first, last): the write-back of the last block
// processed (its ring positions end at the current write index), then the
// windows for the block that starts ahead samples after the current write
// index (the next one: the copies of a block run after its heads).
template <class Transport>
void Staging<Transport>::BuildRange(int first, int last, bool write_back,
                                    int ahead) {
  Transport* const transport = transport_;
  const int count = block_count_;
  bool ok = true;
  int copies = 0;
  const Entry* const end = entries_data_ + last;
  for (Entry* e = entries_data_ + first; e != end; ++e) {
    float* const ring = e->ring;
    const int ring_size = e->ring_size;
    if (e->kind == Kind::kMultitap) {
      auto* o = static_cast<MultitapDiffuser*>(e->object);
      const int index = o->write_index();  // runs backwards
      if (write_back) {
        // The last block wrote ring[index + 1 .. index + count], held in the
        // top count floats of its block buffer.
        CopyRing(transport, e->block + (kBlock - count), ring, ring_size,
                 index + 1, count, ok, copies);
      }
      const int next = index - ahead;  // start index of the served block
      const float* const* windows = e->tap_windows;
      const int taps = e->taps;
      for (int t = 0; t < taps; t++) {
        float* w = const_cast<float*>(windows[t]);
        if (w == nullptr) continue;
        CopyWindow(transport, ring, ring_size,
                   next + o->tap_position(t) - (kBlock - 1), w, kBlock, ok,
                   copies);
      }
      continue;
    }
    int index, delay;
    if (e->kind == Kind::kAllpass) {
      auto* o = static_cast<ModulatedAllpass*>(e->object);
      index = o->write_index();
      delay = o->modulation_enabled ? o->current_delay() : o->sample_delay;
    } else {
      auto* o = static_cast<ModulatedDelay*>(e->object);
      index = o->write_index();
      delay = o->current_delay();
    }
    if (write_back) {
      // The last block wrote ring[index - count .. index).
      CopyRing(transport, e->block, ring, ring_size, index - count, count, ok,
               copies);
    }
    const int next = index + ahead;  // first write of the served block
    const int lead = delay + e->spread + 1;
    e->lead = lead;
    // The object keeps its window and gets the new lead here (before its
    // next block).
    if (e->kind == Kind::kAllpass)
      static_cast<ModulatedAllpass*>(e->object)->SetStagedLead(lead);
    else
      static_cast<ModulatedDelay*>(e->object)->SetStagedLead(lead);
    CopyWindow(transport, ring, ring_size, next - lead, e->window, e->length,
               ok, copies);
  }
  copies_ += copies;
  if (!ok) failed_ = true;
}

// Commits the copies built since the last commit as a segment; the
// transport must be idle.
template <class Transport>
bool Staging<Transport>::Commit() {
  if (copies_ > copies_committed_) {
    copies_committed_ = copies_;
    const bool ok = transport_->Commit();
    // A rejected commit can discover an active hardware channel. Preserve
    // its ownership exactly as for a successful submission until it stops.
    list_pending_ = !transport_->Idle();
    if (!ok) {
      failed_ = true;
      return false;
    }
  }
  return true;
}

template <class Transport>
bool Staging<Transport>::Begin(int count) {
  if (!planned_) return true;
  if (failed_) return false;
  block_count_ = count;
  if (list_pending_) {
    CLOUDSEED_PROFILE_SECTION(kProfileStagingWait);
    uint32_t spun = 0;
    if (!transport_->Wait(&spun)) failed_ = true;
    list_pending_ = !transport_->Idle();
    waited_ += spun;
    if (failed_) return false;
  }
  CLOUDSEED_PROFILE_SECTION(kProfileStaging);
  transport_->Begin();
  copies_ = copies_committed_ = 0;
  next_entry_ = 0;
  committed_group_ = complete_group_ = -1;
  return true;
}

template <class Transport>
void Staging<Transport>::Progress(int group) {
  if (!planned_ || failed_) return;
#if defined(CLOUDSEED_PROFILE) && CLOUDSEED_PROFILE
  const int caller = ProfileSection(kProfileStaging);
#endif
  // The groups the program runs, numbered in order across the channels
  // (as in Plan()).
  if (group >= kStagedGroups) group += line_count_ + 1 - kStagedGroups;
  // The copies of this group and of the groups before it not built yet.
  int last = next_entry_;
  while (last < entries_ && entries_data_[last].group <= group) last++;
  BuildRange(next_entry_, last, true, 0);
  next_entry_ = last;
  // The next group reuses the block buffers of the group kPools before it:
  // that group's write-back must be complete before the next group runs. A
  // completed segment is acknowledged; a running one that is not needed yet
  // keeps running, its successors wait built.
  const int required = group + 1 - kPools;
  while (!failed_) {
    if (transport_->Idle()) {
      complete_group_ = committed_group_;
      if (copies_ > copies_committed_) {
        if (Commit()) committed_group_ = group;
      } else {
        committed_group_ = complete_group_ = group;
      }
      break;
    }
    if (!transport_->Done() && complete_group_ >= required) break;
    {
      CLOUDSEED_PROFILE_SECTION(kProfileStagingWait);
      uint32_t spun = 0;
      if (!transport_->Wait(&spun)) failed_ = true;
      waited_ += spun;
      CLOUDSEED_PROFILE_SECTION(kProfileStaging);
    }
    list_pending_ = !transport_->Idle();
  }
#if defined(CLOUDSEED_PROFILE) && CLOUDSEED_PROFILE
  ProfileSection(caller);
#endif
}

template <class Transport>
bool Staging<Transport>::End() {
  if (!planned_) return true;
  if (failed_) return false;
  CLOUDSEED_PROFILE_SECTION(kProfileStaging);
  // What the last group left (every entry is built by then; a group the
  // reverb did not report would be built here).
  if (next_entry_ < entries_) {
    BuildRange(next_entry_, entries_, true, 0);
    next_entry_ = entries_;
  }
  if (!transport_->Idle()) {
    CLOUDSEED_PROFILE_SECTION(kProfileStagingWait);
    uint32_t spun = 0;
    if (!transport_->Wait(&spun)) failed_ = true;
    waited_ += spun;
    CLOUDSEED_PROFILE_SECTION(kProfileStaging);
  }
  if (failed_) return false;
  Commit();
  return !failed_;
}

}  // namespace cloudseed
#endif  // CLOUDSEED_STAGED_MEMORY
