// Host-native test for src/internet_probe.h with injected fake probes.
//
//   g++ -std=c++17 -Isrc test/test_internet_probe.cpp -o /tmp/t && /tmp/t

#include <cassert>
#include <cstdio>

#include "internet_probe.h"

using namespace reboot2::netprobe;

// Consensus is "down" only when BOTH targets fail.
void test_consensus_requires_both_targets_down() {
  auto both_ok = check_consensus([] { return true; }, [] { return true; });
  assert(both_ok.target_a_ok && both_ok.target_b_ok);
  assert(!both_ok.internet_down());

  auto only_a_down = check_consensus([] { return false; }, [] { return true; });
  assert(!only_a_down.internet_down());

  auto only_b_down = check_consensus([] { return true; }, [] { return false; });
  assert(!only_b_down.internet_down());

  auto both_down = check_consensus([] { return false; }, [] { return false; });
  assert(both_down.internet_down());

  std::printf("  test_consensus_requires_both_targets_down: ok\n");
}

// ConfirmGate needs N consecutive "down" ticks before confirming, and any
// "up" tick in between resets the count — a single good tick undoes any
// amount of prior bad ticks.
void test_confirm_gate_requires_consecutive_downs() {
  ConfirmGate gate(3);

  assert(!gate.tick(true));   // 1/3
  assert(!gate.tick(true));   // 2/3
  assert(gate.consecutive_count() == 2);
  assert(!gate.tick(false));  // reset by an "up" tick
  assert(gate.consecutive_count() == 0);

  assert(!gate.tick(true));   // 1/3
  assert(!gate.tick(true));   // 2/3
  assert(gate.tick(true));    // 3/3 -> confirmed
  assert(gate.consecutive_count() == 3);

  // Stays confirmed while downs keep coming.
  assert(gate.tick(true));

  std::printf("  test_confirm_gate_requires_consecutive_downs: ok\n");
}

// Exponential hold-off doubles per prior action and is capped at max_secs.
void test_exponential_holdoff_doubles_and_caps() {
  ExponentialHoldoff holdoff(/*base_secs=*/600, /*max_secs=*/4 * 3600, /*max_actions=*/3);

  assert(holdoff.holdoff_secs(0) == 600u);         // 1st action: base
  assert(holdoff.holdoff_secs(1) == 1200u);        // 2nd action: base * 2
  assert(holdoff.holdoff_secs(2) == 2400u);        // 3rd action: base * 4
  assert(holdoff.holdoff_secs(5) == 4u * 3600u);   // capped, not 600*32

  std::printf("  test_exponential_holdoff_doubles_and_caps: ok\n");
}

// The hold-off's notion of "breaker open" never exceeds the pre-existing
// circuit breaker's action limit passed in at construction.
void test_holdoff_breaker_matches_max_actions() {
  ExponentialHoldoff holdoff(600, 4 * 3600, /*max_actions=*/3);

  assert(!holdoff.breaker_open(0));
  assert(!holdoff.breaker_open(2));
  assert(holdoff.breaker_open(3));
  assert(holdoff.breaker_open(4));  // never re-closes past the limit

  std::printf("  test_holdoff_breaker_matches_max_actions: ok\n");
}

int main() {
  test_consensus_requires_both_targets_down();
  test_confirm_gate_requires_consecutive_downs();
  test_exponential_holdoff_doubles_and_caps();
  test_holdoff_breaker_matches_max_actions();
  std::printf("test_internet_probe: all assertions passed\n");
  return 0;
}
