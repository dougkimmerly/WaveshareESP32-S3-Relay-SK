// Host-native test for src/commit_confirm.h with an accelerated fake clock.
//
//   g++ -std=c++17 -Isrc test/test_commit_confirm.cpp -o /tmp/t && /tmp/t

#include <cassert>
#include <cstdio>

#include "auth_token.h"
#include "commit_confirm.h"

using namespace reboot2::confirm;
using reboot2::auth::make_token;

const std::string kSecret = "bench-secret";

// A WAN-cutting command (relay 1, powered ON -> OFF) applies provisionally
// and, with no confirm, reverts at the 15-minute deadline.
void test_reverts_at_15min_without_confirm() {
  CommitConfirmGuard guard(kSecret);

  auto disp = guard.submit(/*relay_id=*/1, /*new_powered=*/false, /*prior_powered=*/true,
                            /*is_reboot=*/false, /*now_mono=*/0);
  assert(disp == Disposition::kAppliedProvisional);
  assert(guard.has_pending(1));

  // Before the deadline: nothing to revert yet.
  auto reverts = guard.tick(15 * 60 - 1);
  assert(reverts.empty());
  assert(guard.has_pending(1));

  // At the deadline: reverts to the prior (powered) state.
  reverts = guard.tick(15 * 60);
  assert(reverts.size() == 1);
  assert(reverts[0].relay_id == 1);
  assert(reverts[0].revert_to_powered_state == true);
  assert(!guard.has_pending(1));

  std::printf("  test_reverts_at_15min_without_confirm: ok\n");
}

// A confirmed WAN-cutting command sticks (no revert fires).
void test_confirmed_command_sticks() {
  CommitConfirmGuard guard(kSecret);
  guard.submit(2, false, true, false, 0);

  auto token = make_token(confirm_token_context(2), /*ts=*/300, kSecret);
  assert(guard.confirm(2, token, /*now_wall=*/300));
  assert(!guard.has_pending(2));

  auto reverts = guard.tick(15 * 60);
  assert(reverts.empty());

  std::printf("  test_confirmed_command_sticks: ok\n");
}

// Reboot commands are exempt: never tracked, never reverted, regardless of
// relay or power direction.
void test_reboot_commands_exempt() {
  CommitConfirmGuard guard(kSecret);
  auto disp = guard.submit(3, /*new_powered=*/false, /*prior_powered=*/true, /*is_reboot=*/true, 0);
  assert(disp == Disposition::kExempt);
  assert(!guard.has_pending(3));

  auto reverts = guard.tick(15 * 60);
  assert(reverts.empty());

  std::printf("  test_reboot_commands_exempt: ok\n");
}

// Non-WAN relays (5/6) and power-ON commands are exempt too.
void test_non_wan_and_power_on_exempt() {
  CommitConfirmGuard guard(kSecret);
  assert(guard.submit(5, false, true, false, 0) == Disposition::kExempt);   // relay 5, not WAN
  assert(guard.submit(1, true, false, false, 0) == Disposition::kExempt);   // powering ON, not a cut
  assert(!guard.has_pending(5));
  assert(!guard.has_pending(1));

  std::printf("  test_non_wan_and_power_on_exempt: ok\n");
}

// A confirm token scoped to one relay cannot confirm a different relay's
// pending command (domain separation via confirm_token_context()).
void test_confirm_token_scoped_to_relay() {
  CommitConfirmGuard guard(kSecret);
  guard.submit(1, false, true, false, 0);
  guard.submit(4, false, true, false, 0);

  auto token_for_1 = make_token(confirm_token_context(1), 100, kSecret);
  assert(!guard.confirm(4, token_for_1, 100));  // wrong relay
  assert(guard.has_pending(4));
  assert(guard.confirm(1, token_for_1, 100));   // right relay
  assert(!guard.has_pending(1));

  std::printf("  test_confirm_token_scoped_to_relay: ok\n");
}

// The 2026-08-07 incident: an override command cut power to the WAN chain
// and nothing ever confirmed it. Under commit-confirm, that command must
// self-heal — the relay reverts to powered at the 15-minute deadline with
// no human/software intervention required.
void test_2026_08_07_incident_self_heals() {
  CommitConfirmGuard guard(kSecret);

  // The incident: an unconfirmed override cuts power on the pepRouter relay
  // (relay 3) while it was powered.
  auto disp = guard.submit(/*relay_id=*/3, /*new_powered=*/false, /*prior_powered=*/true,
                            /*is_reboot=*/false, /*now_mono=*/0);
  assert(disp == Disposition::kAppliedProvisional);

  // Time passes with no confirm token ever arriving (that's exactly what
  // happened on 2026-08-07).
  assert(guard.tick(5 * 60).empty());
  assert(guard.tick(10 * 60).empty());

  // At 15 minutes, the guard self-heals: pepRouter is restored to powered
  // without any authenticated confirm, human, or higher software layer
  // having to notice or act.
  auto reverts = guard.tick(15 * 60);
  assert(reverts.size() == 1);
  assert(reverts[0].relay_id == 3);
  assert(reverts[0].revert_to_powered_state == true);

  std::printf("  test_2026_08_07_incident_self_heals: ok\n");
}

int main() {
  test_reverts_at_15min_without_confirm();
  test_confirmed_command_sticks();
  test_reboot_commands_exempt();
  test_non_wan_and_power_on_exempt();
  test_confirm_token_scoped_to_relay();
  test_2026_08_07_incident_self_heals();
  std::printf("test_commit_confirm: all assertions passed\n");
  return 0;
}
