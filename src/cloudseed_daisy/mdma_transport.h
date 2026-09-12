#pragma once

// The staging transport of the Daisy Seed: the STM32H750's MDMA moves the
// delay-memory windows and blocks between the rings (SDRAM, AXI SRAM, D2
// SRAM) and the tightly coupled memories through the Cortex-M7's AHBS port,
// as a linked list of transfers that one software request runs to the end
// (RM0433, MDMA: linked-list mode, MDMA_CxTBR SBUS/DBUS; ST's MDMA training:
// TCM bursts need increment == data size <= 32 bits). The nodes use the
// HAL's layout (MDMA_LinkNodeTypeDef) and the channel is programmed as the
// HAL does it, from the registers directly.
//
// The nodes live in memory the MDMA reads over the AXI bus (RM0433, linked
// list: the link address "must address a memory mapped on the AXI system
// bus"; never a TCM), the firmware puts them in the D2 SRAM window that
// libDaisy's MPU setup keeps out of the data cache. The transport object
// itself stays in ordinary (cached) memory: the audio callback reads its
// bookkeeping and the fields of every node it builds, and a load from
// non-cacheable memory stalls the core for the bus round trip (the
// hardware profile of the first implementation, which copied a 40-byte
// template out of that window per node, spent ~200 cycles per node). Every
// field that never changes is written once by Init(), so a copy writes five
// words per node: the bus select, the byte count, the two addresses and
// the link to the next node, which Commit() cuts at a segment's last node
// and the next Copy() of that node restores.
//
// This is the transport of cloudseed::Staging (cloudseed/staging.h), which
// is a template on the transport's type: Copy() inlines into the list
// builder. The engine (engine.cpp) owns the one instance a firmware needs.
//
// The profiling build times every list from Commit() to the channel's
// transfer-complete interrupt (OnComplete(), from MDMA_IRQHandler). That
// interrupt runs below the audio interrupt, which calls Wait() and Commit():
// it can be held pending across a whole callback and find its list already
// handled, and the callback can start the next list meanwhile. The words
// the two sides share, and the channel's control register both write, are
// touched with interrupts masked (a few cycles each time), so an interrupt
// only ever times the list whose completion flag it sees.

#include <stdint.h>
#include <string.h>

#include "daisy_seed.h"

namespace cloudseed_daisy {

// The interrupt mask around the shared words; the host test supplies its
// own pair (test/mdma_stubs/daisy_seed.h).
#ifndef MDMA_STAGING_IRQ_MASK
static inline uint32_t MdmaStagingIrqMask() {
  const uint32_t mask = __get_PRIMASK();
  __disable_irq();
  return mask;
}
static inline void MdmaStagingIrqRestore(uint32_t mask) {
  if (mask == 0u) __enable_irq();
}
#define MDMA_STAGING_IRQ_MASK() MdmaStagingIrqMask()
#define MDMA_STAGING_IRQ_RESTORE(mask) MdmaStagingIrqRestore(mask)
#endif

class MdmaStagingTransport {
 public:
  // Copies per list: a write-back and a window per staged ring, two per
  // multitap tap, each split in two at a ring wrap at most (the planner
  // budgets its rings against this, see Staging::Plan).
  static constexpr int kMaxNodes = 1000;

  // The node storage: 64-bit aligned (RM0433 MDMA_CxLAR), on the AXI bus.
  struct Nodes {
    alignas(32) MDMA_LinkNodeTypeDef node[kMaxNodes];
  };

  // Programs channel 0 the way HAL_MDMA_Init() and
  // HAL_MDMA_LinkedList_CreateNode() do (stm32h7xx_hal_mdma.c: MDMA_Init,
  // CreateNode), with the same register values; the HAL's list functions
  // are meant for static lists and cost flash the profiling build lacks.
  // Fills the constant fields of every node and links them in order.
  bool Init(Nodes* nodes) {
    __HAL_RCC_MDMA_CLK_ENABLE();
    MDMA_Channel_TypeDef* const channel = MDMA_Channel0;
    timeout_ticks_ = daisy::System::GetTickFreq() / 1000u;
    // This firmware owns channel 0. Do not repurpose an active channel.
    if ((channel->CCR & MDMA_CCR_EN) != 0 || nodes == nullptr) return false;
    channel->CCR = MDMA_PRIORITY_VERY_HIGH | MDMA_LITTLE_ENDIANNESS_PRESERVE;
    channel->CTCR = kCtcr;
    channel->CBNDTR = 0;
    channel->CBRUR = 0;
    channel->CTBR = 0;
    channel->CLAR = 0;
    channel->CIFCR = kAllFlags;
    nodes_ = nodes;
    for (int i = 0; i < kMaxNodes; i++) {
      MDMA_LinkNodeTypeDef& n = nodes->node[i];
      memset(&n, 0, sizeof(n));
      n.CTCR = kCtcr;
      n.CLAR = i + 1 < kMaxNodes ? Address(&nodes->node[i + 1]) : 0u;
    }
    SCB_CleanDCache_by_Addr(reinterpret_cast<uint32_t*>(nodes),
                            static_cast<int32_t>(sizeof(*nodes)));
    __DSB();
    next_ = segment_ = &nodes->node[0];
    end_ = &nodes->node[kMaxNodes];
    pending_ = false;
    failed_ = false;
    timed_ = false;
    return true;
  }

  // The clock (any unit) that times the lists: Commit() reads it, and
  // OnComplete() from the channel's transfer-complete interrupt (enabled
  // per list when a clock is set) or Wait() records the list's duration.
  void SetClock(uint32_t (*clock)()) { clock_ = clock; }

  int Capacity() const { return kMaxNodes; }

  // A block's list: its segments' nodes follow each other. The block before
  // it is complete (Staging::Begin waits): its timing is folded in.
  void Begin() {
    if (pending_ || failed_) return;
    if (block_timed_) {
      block_sum_ += block_cycles_;
      block_count_++;
      if (block_cycles_ > block_max_) block_max_ = block_cycles_;
    }
    block_cycles_ = 0;
    block_timed_ = false;
    next_ = segment_ = &nodes_->node[0];
  }

  // Adds a node, also while a segment runs (the segment's nodes are the
  // ones before it). Links it to the node after it (a node that ended a
  // segment in an earlier block is linked on again this way). Inlined into
  // the staging's list builder, which calls it once per copy.
  bool Copy(const float* source, float* destination, int floats) {
    MDMA_LinkNodeTypeDef* const node = next_;
    if (failed_ || node == end_ || floats <= 0 ||
        static_cast<uint32_t>(floats) > MDMA_CBNDTR_BNDT / sizeof(float))
      return false;
    // The AHBS/TCM bus (SBUS/DBUS) for the side in the ITCM or DTCM, the
    // AXI bus for the other.
    node->CTBR = IsTcm(destination) ? MDMA_CTBR_DBUS : MDMA_CTBR_SBUS;
    node->CBNDTR = static_cast<uint32_t>(floats) * sizeof(float);
    node->CSAR = Address(source);
    node->CDAR = Address(destination);
    MDMA_LinkNodeTypeDef* const after = node + 1;
    node->CLAR = after != end_ ? Address(after) : 0u;
    next_ = after;
    return true;
  }

  // Starts the segment of the nodes added since the last commit: the first
  // node goes into the channel registers, the rest is linked from there.
  // The previous segment must be complete (Wait()).
  bool Commit() {
    if (pending_ || failed_) return false;
    MDMA_Channel_TypeDef* const channel = MDMA_Channel0;
    if ((channel->CCR & MDMA_CCR_EN) != 0) {
      pending_ = failed_ = true;
      return false;
    }
    MDMA_LinkNodeTypeDef* const first_node = segment_;
    if (next_ == first_node) return true;
    (next_ - 1)->CLAR = 0;
    segment_ = next_;
    // The MDMA reads the nodes and the block buffers as they are in memory.
    SCB_CleanDCache_by_Addr(
        reinterpret_cast<uint32_t*>(first_node),
        static_cast<int32_t>((next_ - first_node) * sizeof(*first_node)));
    __DSB();
    channel->CIFCR = kAllFlags;
    const MDMA_LinkNodeTypeDef& first = *first_node;
    channel->CTCR = first.CTCR;
    channel->CBNDTR = first.CBNDTR;
    channel->CSAR = first.CSAR;
    channel->CDAR = first.CDAR;
    channel->CBRUR = first.CBRUR;
    channel->CLAR = first.CLAR;
    channel->CTBR = first.CTBR;
    channel->CMAR = first.CMAR;
    channel->CMDR = first.CMDR;
    __DSB();
    // Arm the timing and start, atomically against the interrupt of the
    // previous list, which may still be pending (see OnComplete()).
    const uint32_t mask = MDMA_STAGING_IRQ_MASK();
    if (clock_ != nullptr) {
      submitted_ = clock_();
      timed_ = true;
      channel->CCR |= MDMA_CCR_CTCIE;
    }
    channel->CCR |= MDMA_CCR_EN;
    channel->CCR |= MDMA_CCR_SWRQ;
    MDMA_STAGING_IRQ_RESTORE(mask);
    pending_ = true;
    lists_++;
    return true;
  }

  // From the channel's interrupt (MDMA_IRQHandler): times the list whose
  // completion flag is set and disables its interrupt (the flag itself
  // stays for Wait()). An interrupt held pending across an audio callback
  // finds its list timed by Wait() and its flag cleared, and the next list
  // (started by that callback) running: it leaves that list alone and
  // counts as stale.
  void OnComplete() {
    const uint32_t mask = MDMA_STAGING_IRQ_MASK();
    if (!TimeCompletion()) stale_++;
    MDMA_STAGING_IRQ_RESTORE(mask);
  }

  // Polls for the end of the list. *spun receives the polls needed (0 when
  // the list was already complete). Wait at most one audio period (1 ms),
  // then allow another period for suspension. The hardware timer advances
  // inside the audio IRQ, unlike HAL's SysTick timeout. A stalled suspension
  // leaves Idle() false: main may retry, but must not reuse the memory.
  bool Wait(uint32_t* spun) {
    if (spun) *spun = 0;
    if (!pending_) return !failed_;
    MDMA_Channel_TypeDef* const channel = MDMA_Channel0;
    uint32_t polls = 0;
    uint32_t status;
    const uint32_t start = daisy::System::GetTick();
    while (((status = channel->CISR) & (MDMA_CISR_CTCIF | MDMA_CISR_TEIF)) ==
           0) {
      ++polls;
      if (daisy::System::GetTick() - start >= timeout_ticks_) break;
    }
    if (spun) *spun = polls;
    if (polls > max_polls_) max_polls_ = polls;
    if (polls > 0) late_++;
    // Complete before its interrupt ran (the audio interrupt outranks it):
    // time it here, the interrupt then finds nothing to do.
    {
      const uint32_t mask = MDMA_STAGING_IRQ_MASK();
      TimeCompletion();
      MDMA_STAGING_IRQ_RESTORE(mask);
    }
    const bool ok = !failed_ && (status & MDMA_CISR_CTCIF) != 0 &&
                    (status & MDMA_CISR_TEIF) == 0;
    if (!ok) {
      {
        const uint32_t mask = MDMA_STAGING_IRQ_MASK();
        timed_ = false;
        channel->CCR &= ~MDMA_CCR_CTCIE;
        MDMA_STAGING_IRQ_RESTORE(mask);
      }
      if (!failed_) {
        error_status_ = channel->CESR;
        errors_++;
        failed_ = true;
      }
      channel->CCR &= ~MDMA_CCR_EN;
      const uint32_t abort_start = daisy::System::GetTick();
      // RM0433 14.3.13-14 and CxCR.EN: CTCIF acknowledges that the
      // remaining FIFO data reached the destination after suspension.
      while ((channel->CISR & MDMA_CISR_CTCIF) == 0 ||
             (channel->CCR & MDMA_CCR_EN) != 0) {
        if (daisy::System::GetTick() - abort_start >= timeout_ticks_)
          return false;
      }
    }
    __DSB();
    channel->CIFCR = kAllFlags;
    pending_ = false;
    return ok;
  }

  bool Idle() const { return !pending_; }
  bool Done() const {
    return pending_ && (MDMA_Channel0->CISR &
                        (MDMA_CISR_CTCIF | MDMA_CISR_TEIF)) != 0;
  }

  uint32_t errors() const { return errors_; }
  uint32_t error_status() const { return error_status_; }
  uint32_t lists() const { return lists_; }
  uint32_t max_polls() const { return max_polls_; }
  // Lists still running when Wait() was called (a block waited for them).
  uint32_t late_lists() const { return late_; }
  // Interrupts that found their list already handled by Wait().
  uint32_t stale_interrupts() const { return stale_; }
  int nodes_last_list() const {
    return static_cast<int>(next_ - &nodes_->node[0]);
  }
  // Timed segments: how many, the clock units they took in total and at
  // most.
  uint32_t timed_lists() const { return list_count_; }
  uint32_t list_clock_sum() const { return list_cycles_sum_; }
  uint32_t list_clock_max() const { return list_cycles_max_; }
  void ResetListClockMax() { list_cycles_max_ = 0; }
  // The same per block (the segments of a block added up).
  uint32_t timed_blocks() const { return block_count_; }
  uint32_t block_clock_sum() const { return block_sum_; }
  uint32_t block_clock_max() const { return block_max_; }
  void ResetBlockClockMax() { block_max_ = 0; }

 private:
  // Word transfers incrementing by a word on both sides, 16-beat bursts
  // below the 128-byte buffer length, packing, a full transfer per
  // software request (SWRM). Writes are not bufferable (BWM 0, the HAL
  // allows either for software requests): the completion flag must mean
  // that the windows are in the memory, the callback reads them at once.
  // Addresses need only word alignment (RM0433).
  static constexpr uint32_t kCtcr =
      MDMA_SRC_INC_WORD | MDMA_DEST_INC_WORD | MDMA_SRC_DATASIZE_WORD |
      MDMA_DEST_DATASIZE_WORD | MDMA_DATAALIGN_PACKENABLE |
      MDMA_SOURCE_BURST_16BEATS | MDMA_DEST_BURST_16BEATS |
      ((128u - 1u) << MDMA_CTCR_TLEN_Pos) | MDMA_FULL_TRANSFER | MDMA_CTCR_SWRM;
  static constexpr uint32_t kAllFlags = MDMA_CIFCR_CTEIF | MDMA_CIFCR_CCTCIF |
                                        MDMA_CIFCR_CBRTIF | MDMA_CIFCR_CBTIF |
                                        MDMA_CIFCR_CLTCIF;
  static bool IsTcm(const void* p) {
    const uintptr_t region = reinterpret_cast<uintptr_t>(p) & 0xFF000000u;
    return region == 0x20000000u || region == 0x00000000u;
  }
  static uint32_t Address(const void* p) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
  }

  // With interrupts masked: if the channel's completion flag is set, disables
  // the transfer-complete interrupt and, for a list still to be timed,
  // records its duration. Returns whether a list was timed.
  bool TimeCompletion() {
    MDMA_Channel_TypeDef* const channel = MDMA_Channel0;
    if ((channel->CISR & MDMA_CISR_CTCIF) == 0) return false;
    channel->CCR &= ~MDMA_CCR_CTCIE;
    // The write sits in the core's write buffer while the handler returns,
    // and a level interrupt still asserted at the return re-enters the
    // handler (Cortex-M7 TRM 7.2.2): read the register back, which waits
    // for the write to reach the MDMA, and complete it before going on.
    (void)channel->CCR;
    __DSB();
    if (!timed_) return false;
    timed_ = false;
    const uint32_t cycles = clock_() - submitted_;
    list_cycles_sum_ += cycles;
    list_count_++;
    if (cycles > list_cycles_max_) list_cycles_max_ = cycles;
    block_cycles_ += cycles;
    block_timed_ = true;
    return true;
  }

  Nodes* nodes_ = nullptr;
  MDMA_LinkNodeTypeDef* next_ = nullptr;     // the node the next copy fills
  MDMA_LinkNodeTypeDef* segment_ = nullptr;  // the first node not committed
  MDMA_LinkNodeTypeDef* end_ = nullptr;      // one past the last node
  bool pending_ = false;
  bool failed_ = false;
  bool timed_ = false;
  uint32_t timeout_ticks_ = 1;
  uint32_t (*clock_)() = nullptr;
  uint32_t submitted_ = 0;
  uint32_t errors_ = 0, error_status_ = 0, lists_ = 0, max_polls_ = 0;
  uint32_t late_ = 0, stale_ = 0;
  uint32_t list_count_ = 0, list_cycles_sum_ = 0, list_cycles_max_ = 0;
  bool block_timed_ = false;
  uint32_t block_cycles_ = 0;
  uint32_t block_count_ = 0, block_sum_ = 0, block_max_ = 0;
};

}  // namespace cloudseed_daisy
