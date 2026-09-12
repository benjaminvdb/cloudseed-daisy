// Exercises the engine's audio-callback and main-loop sides with the real
// DSP, a deterministic clock for the load meter, and a transport whose
// failures can be injected, while deliberately not running the main loop
// when the callback must cope on its own.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "queued_transport.h"

// The engine's staging transport on the host: the queued one, which runs
// its copies at Wait() as the MDMA does, and which the tests below replace
// with their own instances to inject failures.
#define CLOUDSEED_HOST_TRANSPORT QueuedTransport
#include "../src/cloudseed_daisy/engine.cpp"

uint32_t daisy::System::ticks = 0;
uint32_t daisy::System::ticks_per_read = 100;

namespace cloudseed_daisy {
// The tests' access to the engine's internals.
struct EngineTest {
  static int requested(const Engine& e) {
    return e.requested_program_.load();
  }
  static void set_staging_failures(Engine& e, unsigned int n) {
    e.staging_failures_ = n;
  }
};
}  // namespace cloudseed_daisy

namespace {
using namespace cloudseed;
using cloudseed_daisy::Engine;
using cloudseed_daisy::EngineTest;
using cloudseed_daisy::State;

int failures = 0;
void Check(bool value, const char* message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

const cloudseed_daisy::Program kPrograms[] = {
    {&presets::kSmallRoom, 3},         {&presets::kMediumSpace, 3},
    {&presets::kNoiseInTheHallway, 8}, {&presets::kHyperplane, 9},
    {&presets::kRubiKaFields, 4},      {&presets::kThroughTheLookingGlass, 12},
    {&presets::kThe90sAreBack, 9},     {&presets::kDullEchoes, 12},
    {&presets::kChorusDelay, 12},      {&presets::kDarkPlate, 12},
};
constexpr int kProgramCount = sizeof(kPrograms) / sizeof(kPrograms[0]);
constexpr int kBlock = kMaxBlockSize;

Engine engine;
int requested_program = 0;  // what the "pot" points at
float mix_gain = 0.7f;
unsigned int hook_calls = 0;

void OnProgramLoaded(ReverbController& reverb, void* context) {
  ++*static_cast<unsigned int*>(context);
  reverb.SetParameter(Parameter::DryOut, 0.0);
  reverb.SetParameter(Parameter::CutoffEnabled, 1.0);
}

void Reset(int program) {
#if CLOUDSEED_STAGING
  Check(cloudseed_daisy::staging.Unplan(), "previous transport stops");
#endif
  Engine::Config config;
  config.programs = kPrograms;
  config.program_count = kProgramCount;
  config.sample_rate = 48000.f;
  config.block_size = kBlock;
  config.on_program_loaded = OnProgramLoaded;
  config.context = &hook_calls;
  hook_calls = 0;
  Check(engine.Init(config), "engine initializes");
  daisy::System::ticks_per_read = 100;
  Check(engine.Start(program), "engine starts its first program");
  Check(hook_calls == 1, "the load hook ran once for the first program");
#if CLOUDSEED_STAGING
  Check(cloudseed_daisy::staging.planned() &&
            cloudseed_daisy::staging.staged_buffers() > 0 &&
            !cloudseed_daisy::staging.failed(),
        "engine stages the program's delay memory");
#endif
  requested_program = program;
  Check(engine.state() == State::kRunning && engine.program() == program,
        "engine runs the first program");
}

// One audio callback: the engine's block, the "controls", and a dry/wet
// mix like an application's.
void Callback(float signal = 0.f, bool expect_dry = false) {
  float input[kBlock], wet_l[kBlock], wet_r[kBlock], left[kBlock],
      right[kBlock];
  for (float& sample : input) sample = signal;
  engine.BeginBlock();
  engine.RequestProgram(requested_program);
  engine.SetParameter(Parameter::LineDecay, 0.5, 0.001f);
  engine.SetParameter(Parameter::PostCutoffFrequency, 0.8, 0.001f);
  const bool wet = engine.Process(input, input, wet_l, wet_r, kBlock);
  for (int i = 0; i < kBlock; i++) {
    left[i] = mix_gain * input[i] + (wet ? wet_l[i] : 0.f);
    right[i] = mix_gain * input[i] + (wet ? wet_r[i] : 0.f);
  }
  cloudseed_daisy::host_cycles += 120000;
  engine.EndBlock();
  for (int i = 0; i < kBlock; i++) {
    Check(std::isfinite(left[i]) && std::isfinite(right[i]),
          "output stays finite");
    if (expect_dry)
      Check(!wet && left[i] == signal * mix_gain && right[i] == signal * mix_gain,
            "recovery leaves the dry signal alone");
  }
}

void TestStarvedMain() {
  Reset(5);                                 // Through the Looking Glass
  daisy::System::ticks = UINT32_MAX - 500;  // timing also crosses timer wrap
  daisy::System::ticks_per_read = 1200;     // 1.2 ms in a 1 ms deadline
  Callback();
  Check(engine.state() == State::kOverloaded,
        "overloaded callback relinquishes reverb without main");
  std::vector<unsigned char> snapshot(sizeof(cloudseed_daisy::reverb_object));
  std::memcpy(snapshot.data(), &cloudseed_daisy::reverb_object, snapshot.size());
  requested_program = 0;
  // No main-loop execution. Even another high timing report must not retry
  // the reverb while main owns it or stop the selector from tracking.
  for (int i = 0; i < 20; i++) Callback(.1f, true);
  Check(std::memcmp(snapshot.data(), &cloudseed_daisy::reverb_object,
                    snapshot.size()) == 0,
        "recovery callback leaves the complete reverb object untouched");
  Check(EngineTest::requested(engine) == 0, "selector remains responsive");
  Check(engine.overloaded(), "the overload is reported for the LED");
  engine.Service();
  Check(engine.program() == 0 && engine.state() == State::kFadingIn,
        "main recovers directly to the newly requested light program");
  Check(engine.line_limit(5) == kMaxLines - 1 &&
            engine.line_limit(0) == (kMaxLines < 3 ? kMaxLines : 3),
        "overload reduces only the program that caused it");
  Check(hook_calls == 2, "the load hook ran for the recovery load");
}

void TestSelectorHandoff() {
  Reset(5);
  requested_program = 0;
  for (int i = 0; i < 20; i++) Callback();
  Check(engine.state() == State::kSwitching,
        "selector requests handoff without main initiating the fade");
  Callback(.1f, true);
  engine.Service();
  for (int i = 0; i < 20; i++) Callback();
  Check(engine.program() == 0 && engine.state() == State::kRunning,
        "new program fades in and completes the handoff");
  Check(!engine.overloaded(), "a program change is no overload");
}

void TestParameterThreshold() {
  Reset(1);
  Check(!engine.SetParameter(Parameter::LineDecay, 0.5, 0.001f) ||
            engine.SetParameter(Parameter::LineDecay, 0.5, 0.001f) == false,
        "a repeated value is not applied");
  engine.BeginBlock();
  Check(engine.SetParameter(Parameter::LineDecay, 0.7, 0.001f),
        "a changed value is applied");
  Check(!engine.SetParameter(Parameter::LineDecay, 0.7005, 0.001f),
        "a value within the threshold is not applied");
  Check(engine.SetParameter(Parameter::LineDecay, 0.702, 0.001f),
        "a value beyond the threshold is applied");
  Check(engine.SetParameter(Parameter::LineDecay, 0.702, 0.f) == false,
        "the same value is never applied twice");
  engine.EndBlock();
  Check(engine.reverb().GetParameter(Parameter::LineDecay) == 0.702,
        "the reverb holds the last applied value");
  // A program load resets the memory: the same value applies again.
  requested_program = 2;
  for (int i = 0; i < 20; i++) Callback();
  engine.Service();
  engine.BeginBlock();
  Check(engine.SetParameter(Parameter::LineDecay, 0.702, 0.001f),
        "a program load forgets the last applied values");
  engine.EndBlock();
  // Freeze follows the control and reaches the reverb only while the
  // callback owns it.
  engine.BeginBlock();
  engine.SetFrozen(true);
  engine.EndBlock();
  Check(engine.frozen() && engine.reverb().frozen(), "freeze reaches the reverb");
  engine.BeginBlock();
  engine.SetFrozen(false);
  engine.EndBlock();
  Check(!engine.frozen() && !engine.reverb().frozen(), "release reaches the reverb");
}

void TestAdaptiveBudget() {
  if (kMaxLines < 3) return;
  Reset(5);
  // Model a program whose processing exceeds the budget until two lines
  // are left. These are injected times, not claimed MCU measurements.
  while (engine.line_count() > 2) {
    const int previous = engine.line_count();
    daisy::System::ticks_per_read = 950;
    Callback();
    Check(engine.state() == State::kOverloaded,
          "reserve triggers recovery before the full deadline");
    engine.Service();
    Check(engine.line_count() == previous - 1,
          "reload removes one actual late line per channel");
  }
  daisy::System::ticks_per_read = 700;
  for (int i = 0; i < 30; i++) Callback();
  Check(engine.state() == State::kRunning && engine.line_count() == 2,
        "program remains running once measured load fits");
  for (int program : {0, 5}) {
    requested_program = program;
    for (int i = 0; i < 20; i++) Callback();
    engine.Service();
    for (int i = 0; i < 20; i++) Callback();
  }
  Check(engine.line_count() == 2 && engine.state() == State::kRunning,
        "revisiting a heavy program retains its learned line limit");
}

void TestMinimumBudget() {
  Reset(5);
  daisy::System::ticks_per_read = 1200;
  for (int lines = kMaxLines; lines >= 1; --lines) {
    Check(engine.line_count() == lines, "recovery line count stays in range");
    Callback();
    engine.Service();
  }
  Check(engine.state() == State::kBypassed && engine.line_limit(5) == 0,
        "overload at one line stays bypassed without an endless retry loop");
  for (int i = 0; i < 20; i++) Callback(.1f, true);
  Check(engine.overloaded(), "unavailable program retains the overload indication");
  requested_program = 0;
  Callback(.1f, true);
  Check(engine.state() == State::kSwitching,
        "selector can leave a bypassed program without main");
  engine.Service();
  daisy::System::ticks_per_read = 100;
  for (int i = 0; i < 20; i++) Callback();
  Check(engine.program() == 0 && engine.state() == State::kRunning,
        "a different program still runs after minimum-budget failure");
}

void TestLargerBlocks() {
  // A callback block of two engine blocks renders both, the fade included.
  Reset(1);
  float input[2 * kBlock], wet_l[2 * kBlock], wet_r[2 * kBlock];
  for (int i = 0; i < 2 * kBlock; i++) input[i] = i % 7 == 0 ? .1f : 0.f;
  bool finite = true;
  for (int block = 0; block < 50; block++) {
    engine.BeginBlock();
    engine.RequestProgram(1);
    Check(engine.Process(input, input, wet_l, wet_r, 2 * kBlock),
          "a double block renders");
    engine.EndBlock();
    for (int i = 0; i < 2 * kBlock; i++)
      finite &= std::isfinite(wet_l[i]) && std::isfinite(wet_r[i]);
  }
  Check(finite, "double blocks stay finite");
}

#if CLOUDSEED_STAGING
void TestTransportFailure() {
  for (auto failure : {QueuedTransport::Failure::kCopy,
                       QueuedTransport::Failure::kCommit,
                       QueuedTransport::Failure::kActiveCommit,
                       QueuedTransport::Failure::kWait}) {
    for (bool at_start : {false, true}) {
      Reset(1);
      daisy::System::ticks_per_read = 100;
      Check(cloudseed_daisy::staging.Unplan(), "engine staging detached");
      QueuedTransport transport;
      std::vector<float> tcm(24000, std::numeric_limits<float>::quiet_NaN());
      cloudseed_daisy::staging.Init(&transport);
      cloudseed_daisy::staging.AddMemory(tcm.data(), tcm.size());
      if (at_start) transport.failure = failure;
      const bool started = engine.Start(1);
      if (at_start) {
        Check(!started && engine.state() == State::kStagingFault,
              "engine rejects a failed initial window");
      } else {
        Check(started, "queued transport program starts");
        Callback(.1f);
        transport.failure = failure;
      }
      Callback(.1f, true);
      Check(engine.state() == State::kStagingFault && !engine.staging_enabled(),
            "transport fault enters dry recovery without consuming bad data");
      Check(engine.staging_failures() == 1, "transport failure counted once");
      std::vector<unsigned char> snapshot(sizeof(cloudseed_daisy::reverb_object));
      std::memcpy(snapshot.data(), &cloudseed_daisy::reverb_object, snapshot.size());
      for (int i = 0; i < 10; i++) Callback(.1f, true);
      Check(std::memcmp(snapshot.data(), &cloudseed_daisy::reverb_object,
                        snapshot.size()) == 0 &&
                engine.staging_failures() == 1,
            "dry recovery leaves DSP state and failure count alone");
      Check(engine.overloaded(), "a staging fault is reported for the LED");
      engine.Service();
      Check(engine.state() == State::kFadingIn &&
                !cloudseed_daisy::staging.planned() &&
                engine.line_limit(1) == (kMaxLines < 3 ? kMaxLines : 3),
            "fault reloads CPU rings without reducing program density");
      for (int i = 0; i < 25; i++) Callback(.1f);
      Check(engine.state() == State::kRunning, "CPU fallback resumes audio");
      Check(cloudseed_daisy::staging.Unplan() && !transport.bad_lifetime,
            "fault recovery ends with no transport ownership violation");
      // Release the test transport before its destruction.
      cloudseed_daisy::staging.Init(&cloudseed_daisy::staging_transport);
    }
  }
}

void TestUnfinishedAbort() {
  Reset(1);
  daisy::System::ticks_per_read = 100;
  Check(cloudseed_daisy::staging.Unplan(), "initial staging stops");
  QueuedTransport transport;
  std::vector<float> tcm(24000);
  cloudseed_daisy::staging.Init(&transport);
  cloudseed_daisy::staging.AddMemory(tcm.data(), tcm.size());
  Check(engine.Start(1), "abort test program starts");
  Callback(.1f);
  transport.hold_pending = true;
  Callback(.1f, true);
  std::vector<unsigned char> snapshot(sizeof(cloudseed_daisy::reverb_object));
  std::memcpy(snapshot.data(), &cloudseed_daisy::reverb_object, snapshot.size());
  requested_program = 0;
  for (int i = 0; i < 5; i++) {
    Callback(.1f, true);
    engine.Service();
  }
  Check(engine.state() == State::kStagingFault && !transport.Idle() &&
            std::memcmp(snapshot.data(), &cloudseed_daisy::reverb_object,
                        snapshot.size()) == 0,
        "unfinished abort preserves transport ownership across main retries");
  transport.hold_pending = false;
  engine.Service();
  Check(engine.state() == State::kFadingIn && engine.program() == 0 &&
            transport.Idle() && !transport.bad_lifetime,
        "completed abort releases memory and loads latest selector request");
  cloudseed_daisy::staging.Init(&cloudseed_daisy::staging_transport);
}
#endif

#if CLOUDSEED_PROFILE
std::string log;
void (*on_print)() = nullptr;
void Sink(const char* text) {
  log += text;
  if (on_print) on_print();
}

void TestProfileMailbox() {
  Reset(0);  // Small Room, 3 lines
  const float controls[] = {.5f, .6f};
  daisy::System::ticks_per_read = 250;  // a 25% load
  for (int block = 0; block < 1000; block++) {
    engine.SetProfileControls(controls, 2);
    Callback();
  }
  Check(cloudseed_daisy::profile_ready.load() == 1, "completed profile is published");
  EngineTest::set_staging_failures(engine, 2);
  daisy::System::ticks_per_read = 800;
  for (int block = 0; block < 1000; block++) Callback();
  Check(cloudseed_daisy::profile_report.program == 0 &&
            cloudseed_daisy::profile_report.avg_permille == 250,
        "producer preserves unread report when main is starved");
  cloudseed_daisy::ProfileReport report;
  Check(engine.TakeReport(&report) && !engine.TakeReport(&report),
        "profile is consumed exactly once");
  // Simulate the callback publishing a new report while the first USB
  // write is blocked. Every later field printed must still belong to the
  // old report.
  log.clear();
  on_print = [] {
    on_print = nullptr;
    for (int block = 0; block < 1000; block++) Callback();
  };
  engine.PrintReport(report, Sink);
  Check(report.program == 0 && report.avg_permille == 250 &&
            report.staging_failures == 0 && report.control_count == 2 &&
            cloudseed_daisy::profile_report.staging_failures == 2,
        "printing uses a private snapshot across callback preemption");
  Check(log.find("load program=0 lines=3 mixed=0 avg=25.0%") != std::string::npos &&
            log.find("controls=500 600") != std::string::npos &&
            log.find("staging copies=") != std::string::npos &&
            log.find("failures=0") != std::string::npos,
        "report prints measured identity and the control snapshot");
  Check(engine.TakeReport(&report) && report.avg_permille == 800 &&
            report.staging_failures == 2,
        "next profile is retained while the preceding report prints");
  // An interval that spans a freeze is marked mixed.
  for (int block = 0; block < 500; block++) Callback();
  engine.BeginBlock();
  engine.SetFrozen(true);
  engine.EndBlock();
  for (int block = 0; block < 500; block++) Callback();
  Check(engine.TakeReport(&report) && report.mixed,
        "profile marks an interval spanning a freeze");
  engine.BeginBlock();
  engine.SetFrozen(false);
  engine.EndBlock();
  // The program and build lines.
  log.clear();
  engine.PrintProgramIfChanged(Sink);
  Check(log.find("program 0 \"Small Room\" lines=3 limit=3") != std::string::npos,
        "the program line names the program and its lines");
  log.clear();
  engine.PrintProgramIfChanged(Sink);
  Check(log.empty(), "the program line is printed once per load");
  engine.PrintBuild(Sink);
  Check(log.find("build maxlines=") != std::string::npos, "the build line prints");
}
#endif
}  // namespace

int main() {
  TestStarvedMain();
  TestSelectorHandoff();
  TestParameterThreshold();
  TestAdaptiveBudget();
  TestMinimumBudget();
  TestLargerBlocks();
#if CLOUDSEED_STAGING
  TestTransportFailure();
  TestUnfinishedAbort();
#endif
#if CLOUDSEED_PROFILE
  TestProfileMailbox();
#endif
  std::printf("%d-line engine (staging=%d, profile=%d): %d failures\n",
              cloudseed::kMaxLines, CLOUDSEED_STAGING, CLOUDSEED_PROFILE,
              failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
