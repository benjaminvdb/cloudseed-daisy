// Register-level control-flow checks, not an emulation of the MDMA bus/FIFO.
#include <climits>
#include <cstdio>
#include <cstring>

#include "cloudseed_daisy/mdma_transport.h"

using cloudseed_daisy::MdmaStagingTransport;

MDMA_Channel_TypeDef test_channel{};
unsigned int test_barriers = 0, test_clean_calls = 0;
unsigned int test_irq_masked = 0, test_irq_mask_calls = 0;
uintptr_t test_clean_address = 0;
int32_t test_clean_bytes = 0;
uint32_t daisy::System::ticks = 0;
unsigned int daisy::System::reads = 0;
void (*daisy::System::on_tick)() = nullptr;

namespace {
int failures = 0;
MdmaStagingTransport transport;
MdmaStagingTransport::Nodes node_store;
uint32_t fake_clock = 0;
uint32_t FakeClock() { return fake_clock; }
// The registers hold 32-bit addresses; the host's are wider.
uint32_t Addr(const void* p) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
}
const float* const ring = reinterpret_cast<const float*>(0xC0001000u);
float* const tcm = reinterpret_cast<float*>(0x20000020u);

void Check(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

void Reset() {
  std::memset(&test_channel, 0, sizeof(test_channel));
  transport = MdmaStagingTransport();
  daisy::System::ticks = 0;
  daisy::System::reads = 0;
  daisy::System::on_tick = nullptr;
  test_barriers = test_clean_calls = 0;
  test_irq_masked = test_irq_mask_calls = 0;
  test_clean_address = 0;
  test_clean_bytes = 0;
  Check(transport.Init(&node_store), "idle channel initializes");
  Check(test_clean_calls == 1 && test_clean_bytes == sizeof(node_store) &&
            node_store.node[0].CLAR == Addr(&node_store.node[1]) &&
            node_store.node[MdmaStagingTransport::kMaxNodes - 1].CLAR == 0 &&
            node_store.node[7].CTCR == test_channel.CTCR &&
            node_store.node[7].CBRUR == 0,
        "initialization fills and links every node, then cleans them");
  test_clean_calls = 0;
  test_clean_bytes = 0;
  fake_clock = 0;
}

void Start() {
  transport.Begin();
  Check(transport.Copy(ring, tcm, 48) && transport.Commit(), "start list");
  Check(!transport.Idle(), "started transfer owns its buffers");
}

void Complete() {
  // Hardware clears EN and raises CTCIF after the final destination write.
  test_channel.CCR &= ~MDMA_CCR_EN;
  test_channel.CISR = MDMA_CISR_CTCIF;
}

void TestDescriptors() {
  Reset();
  Check(!transport.Copy(ring, tcm, 0) && !transport.Copy(ring, tcm, -1) &&
            !transport.Copy(ring, tcm, INT_MAX) &&
            !transport.Copy(ring, tcm, MDMA_CBNDTR_BNDT / 4 + 1),
        "reject invalid byte counts before creating nodes");
  transport.Begin();
  Check(transport.Copy(ring, tcm, 48), "ring-to-TCM node");
  Check(transport.Copy(tcm, const_cast<float*>(ring), 48), "TCM-to-ring node");
  Check(transport.Commit(), "commit two-node list");
  Check(test_clean_calls == 1 && test_clean_address % 32 == 0 &&
            test_clean_bytes == 2 * sizeof(MDMA_LinkNodeTypeDef),
        "clean aligned descriptors before DMA reads them");
  const auto* nodes =
      reinterpret_cast<const MDMA_LinkNodeTypeDef*>(test_clean_address);
  Check(test_channel.CSAR == 0xC0001000u && test_channel.CDAR == 0x20000020u &&
            test_channel.CBNDTR == 192 && test_channel.CTBR == MDMA_CTBR_DBUS &&
            nodes[1].CTBR == MDMA_CTBR_SBUS && nodes[1].CLAR == 0,
        "addresses, counts and TCM buses match each direction");
  Check(sizeof(MDMA_LinkNodeTypeDef) % 8 == 0 && test_channel.CLAR % 8 == 0 &&
            test_channel.CLAR ==
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nodes[1])),
        "linked nodes retain the HAL layout and 64-bit alignment");
  Check((test_channel.CTCR & MDMA_CTCR_TLEN) == (127u << MDMA_CTCR_TLEN_Pos) &&
            (test_channel.CTCR & MDMA_CTCR_BWM) == 0 && test_barriers >= 2 &&
            (test_channel.CCR & (MDMA_CCR_EN | MDMA_CCR_SWRQ)) ==
                (MDMA_CCR_EN | MDMA_CCR_SWRQ),
        "128-byte nonbufferable software-request transfer");
  Check(!transport.Commit() && !transport.Init(&node_store),
        "active channel cannot be recommitted or reinitialized");
  Complete();
  uint32_t polls = 99;
  Check(transport.Wait(&polls) && polls == 0 && transport.Idle() &&
            transport.late_lists() == 0,
        "already complete list returns without polling");
  Check(test_channel.CIFCR & MDMA_CIFCR_CCTCIF, "completion flag acknowledged");
  transport.Begin();
  Check(transport.Copy(ring, tcm, 8) && nodes[0].CLAR == Addr(&nodes[1]),
        "a node added to the next list is linked on again");
  Check(transport.Commit() && nodes[0].CLAR == 0 && test_channel.CBNDTR == 32,
        "a shorter list ends at its last node");
  Complete();
  Check(transport.Wait(&polls), "second list completes");

  Reset();
  transport.Begin();
  for (int i = 0; i < MdmaStagingTransport::kMaxNodes; ++i)
    Check(transport.Copy(ring, tcm, 48), "capacity node accepted");
  Check(!transport.Copy(ring, tcm, 48) &&
            transport.nodes_last_list() == MdmaStagingTransport::kMaxNodes,
        "list capacity checked before writing out of bounds");
}

void TestTiming() {
  Reset();
  transport.SetClock(FakeClock);
  fake_clock = 1000;
  Start();
  Check((test_channel.CCR & MDMA_CCR_CTCIE) != 0,
        "a timed list enables the transfer-complete interrupt");
  // An interrupt before the list is complete (a stale one, see below)
  // touches nothing.
  test_channel.CISR = 0;
  transport.OnComplete();
  Check((test_channel.CCR & MDMA_CCR_CTCIE) != 0 &&
            transport.timed_lists() == 0 && transport.stale_interrupts() == 1,
        "an interrupt without the completion flag is stale");
  // The interrupt runs first: it records the duration and disarms itself.
  Complete();
  fake_clock = 1300;
  transport.OnComplete();
  Check((test_channel.CCR & MDMA_CCR_CTCIE) == 0 &&
            transport.timed_lists() == 1 && transport.list_clock_sum() == 300 &&
            transport.list_clock_max() == 300,
        "the interrupt times the list once");
  fake_clock = 2000;
  uint32_t polls = 0;
  Check(transport.Wait(&polls) && transport.timed_lists() == 1 &&
            transport.list_clock_sum() == 300,
        "a list the interrupt timed is not timed again");
  // The wait sees the completion before the interrupt ran. (The hardware
  // clears the flags Wait() acknowledged; the stub does not.)
  test_channel.CISR = 0;
  fake_clock = 5000;
  Start();
  daisy::System::on_tick = [] {
    if (daisy::System::reads == 3) {
      Complete();
      fake_clock = 5450;
    }
  };
  Check(transport.Wait(&polls) && transport.timed_lists() == 2 &&
            transport.list_clock_sum() == 750 &&
            transport.list_clock_max() == 450 &&
            (test_channel.CCR & MDMA_CCR_CTCIE) == 0 &&
            transport.late_lists() == 1,
        "the wait times a list whose interrupt is still pending");
  // That interrupt runs after the wait cleared the flag (and after the
  // next list started): it leaves the new list's timing alone.
  test_channel.CISR = 0;
  fake_clock = 6000;
  Start();
  fake_clock = 9999;
  transport.OnComplete();
  Check(
      transport.timed_lists() == 2 && transport.list_clock_sum() == 750 &&
          transport.stale_interrupts() == 2 &&
          (test_channel.CCR & MDMA_CCR_CTCIE) != 0,
      "the late interrupt finds nothing to time and keeps the new list armed");
  Complete();
  fake_clock = 6100;
  transport.OnComplete();
  Check(transport.timed_lists() == 3 && transport.list_clock_sum() == 850 &&
            transport.list_clock_max() == 450,
        "the new list is timed by its own interrupt");
  daisy::System::on_tick = nullptr;
  Check(transport.Wait(&polls) && transport.timed_lists() == 3,
        "a list the interrupt timed is not timed by the wait");
  transport.ResetListClockMax();
  Check(transport.list_clock_max() == 0 && transport.list_clock_sum() == 850,
        "the longest list resets per report, the sum runs on");
  Check(test_irq_masked == 0 && test_irq_mask_calls >= 6,
        "the shared words are touched with interrupts masked, then unmasked");
}

void TestCompletionAndTimeout() {
  Reset();
  transport.Begin();
  Check(transport.Copy(ring, tcm, 48), "unexpected-channel test builds list");
  test_channel.CCR |= MDMA_CCR_EN;
  Check(!transport.Commit() && !transport.Idle(),
        "rejected commit retains ownership of unexpectedly active channel");
  uint32_t polls = 0;
  Check(!transport.Wait(&polls) && !transport.Idle(),
        "rejected commit cannot release an unacknowledged suspension");
  Complete();
  Check(!transport.Wait(&polls) && transport.Idle(),
        "rejected commit releases only after suspension acknowledgement");

  Reset();
  Start();
  daisy::System::on_tick = [] {
    if (daisy::System::reads == 4) Complete();
  };
  Check(transport.Wait(&polls) && polls == 3 && transport.Idle() &&
            transport.late_lists() == 1,
        "delayed completion waits for the final flag");

  Reset();
  Start();
  daisy::System::ticks = UINT32_MAX - 500u;
  Check(!transport.Wait(&polls) && polls == 10 && !transport.Idle() &&
            daisy::System::reads <= 22 &&
            (test_channel.CCR & MDMA_CCR_EN) == 0 && transport.errors() == 1,
        "timeout and suspension are bounded across timer wrap");
  Check(!transport.Copy(ring, tcm, 48) && !transport.Commit(),
        "unacknowledged abort retains memory ownership");
  Complete();
  Check(!transport.Wait(&polls) && transport.Idle() && transport.errors() == 1,
        "late suspension acknowledgement releases memory, preserves error");
  Check(!transport.Wait(&polls) && !transport.Commit(),
        "failed transport stays disabled until explicit reinitialization");

  Reset();
  Start();
  daisy::System::on_tick = [] {
    if (daisy::System::reads == 14) Complete();
  };
  Check(
      !transport.Wait(&polls) && transport.Idle() && daisy::System::reads == 14,
      "suspension completion is acknowledged within the abort budget");

  Reset();
  Start();
  test_channel.CESR = 0x1234;
  test_channel.CISR = MDMA_CISR_TEIF;
  daisy::System::on_tick = [] {
    if (daisy::System::reads == 4) Complete();
  };
  Check(!transport.Wait(&polls) && transport.Idle() &&
            transport.error_status() == 0x1234 && transport.errors() == 1,
        "transfer error preserves diagnostic status through suspension");
  Check(test_irq_masked == 0, "the failure paths unmask interrupts again");
}
}  // namespace

int main() {
  TestDescriptors();
  TestTiming();
  TestCompletionAndTimeout();
  std::printf("MDMA transport: %d failures\n", failures);
  return failures ? 1 : 0;
}
