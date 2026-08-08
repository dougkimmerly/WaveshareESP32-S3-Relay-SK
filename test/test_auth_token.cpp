// Host-native test for reboot2::auth::parse_token (src/auth_token.h) — the
// wire-format decoder for tokens carried over a SignalK PUT string value
// (fixer ADR 0055 §4, job 5/4 follow-up: main.cpp wiring). Pure parsing
// logic, no secret/MAC verification involved, so it's the extractable piece
// of the main.cpp integration seam worth a host test on its own.
//
//   g++ -std=c++17 -Isrc test/test_auth_token.cpp -o /tmp/t && /tmp/t

#include <cassert>
#include <cstdio>

#include "auth_token.h"

using namespace reboot2::auth;

void test_round_trips_a_made_token() {
  Token made = make_token("ctx", /*ts=*/1234567890ULL, "secret");
  char hex[65];
  for (int i = 0; i < 32; i++) std::snprintf(hex + i * 2, 3, "%02x", made.mac[i]);
  hex[64] = '\0';
  std::string wire = "1234567890:" + std::string(hex);

  Token parsed;
  assert(parse_token(wire, &parsed));
  assert(parsed.unix_timestamp == 1234567890ULL);
  assert(parsed.mac == made.mac);

  std::printf("  test_round_trips_a_made_token: ok\n");
}

void test_rejects_missing_separator() {
  Token parsed;
  assert(!parse_token(std::string(74, '0'), &parsed));
  std::printf("  test_rejects_missing_separator: ok\n");
}

void test_rejects_wrong_length_mac() {
  Token parsed;
  assert(!parse_token("100:abcd", &parsed));
  std::printf("  test_rejects_wrong_length_mac: ok\n");
}

void test_rejects_non_numeric_timestamp() {
  Token parsed;
  std::string wire = "notanumber:" + std::string(64, 'a');
  assert(!parse_token(wire, &parsed));
  std::printf("  test_rejects_non_numeric_timestamp: ok\n");
}

void test_rejects_non_hex_mac() {
  Token parsed;
  std::string wire = "100:" + std::string(63, 'a') + "z";
  assert(!parse_token(wire, &parsed));
  std::printf("  test_rejects_non_hex_mac: ok\n");
}

void test_rejects_empty_timestamp() {
  Token parsed;
  std::string wire = ":" + std::string(64, 'a');
  assert(!parse_token(wire, &parsed));
  std::printf("  test_rejects_empty_timestamp: ok\n");
}

int main() {
  test_round_trips_a_made_token();
  test_rejects_missing_separator();
  test_rejects_wrong_length_mac();
  test_rejects_non_numeric_timestamp();
  test_rejects_non_hex_mac();
  test_rejects_empty_timestamp();
  std::printf("test_auth_token: all tests passed\n");
  return 0;
}
