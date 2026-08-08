#ifndef REBOOT2_AUTH_TOKEN_H_
#define REBOOT2_AUTH_TOKEN_H_

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "hmac_sha256.h"

// Authenticated "home contact" tokens (fixer ADR 0055 §4). A token is an
// HMAC-SHA256 MAC over (context || 8-byte big-endian unix timestamp), keyed
// with a shared secret injected at build time (see docs/command-loss-timer.md
// for the SOPS sourcing). `context` domain-separates token streams — the
// command-loss timer's contact tokens and each relay's commit-confirm
// tokens use distinct context strings, so a captured token from one stream
// can never be replayed against another.
//
// Verification enforces two independent properties before a token is
// trusted:
//   - freshness: |now - timestamp| <= window, so a captured token cannot be
//     replayed long after the fact (the ±10 min window from the spec)
//   - monotonicity: timestamp must be strictly newer than the last accepted
//     timestamp for that context, so a captured token cannot be replayed
//     even within the freshness window
namespace reboot2::auth {

using Mac = std::array<uint8_t, 32>;

struct Token {
  uint64_t unix_timestamp;
  Mac mac;
};

inline std::array<uint8_t, 8> encode_timestamp(uint64_t ts) {
  std::array<uint8_t, 8> out;
  for (int i = 0; i < 8; i++) out[i] = (ts >> (56 - 8 * i)) & 0xff;
  return out;
}

inline std::vector<uint8_t> build_payload(const std::string& context, uint64_t ts) {
  std::vector<uint8_t> payload(context.begin(), context.end());
  auto ts_bytes = encode_timestamp(ts);
  payload.insert(payload.end(), ts_bytes.begin(), ts_bytes.end());
  return payload;
}

// Builds a token for `ts` under `secret`/`context`. Exposed so tests (and,
// on the real deployment, the "home" signer) can construct valid tokens
// without duplicating the wire format.
inline Token make_token(const std::string& context, uint64_t ts, const std::string& secret) {
  auto payload = build_payload(context, ts);
  Token t;
  t.unix_timestamp = ts;
  t.mac = crypto::hmac_sha256(reinterpret_cast<const uint8_t*>(secret.data()), secret.size(),
                               payload.data(), payload.size());
  return t;
}

// Wire format for a token carried over a SignalK PUT string value:
// "<decimal unix_timestamp>:<64 lowercase hex chars, the 32-byte MAC>".
// Pure parsing only — does not verify the MAC, just decodes the wire shape,
// so it can be host-tested (test/test_auth_token.cpp) independent of any
// secret. Returns false (leaving *out unspecified) on any malformed input:
// missing separator, non-numeric timestamp, or a hex part that isn't
// exactly 64 characters of [0-9a-fA-F].
inline bool parse_token(const std::string& raw, Token* out) {
  auto sep = raw.find(':');
  if (sep == std::string::npos || sep == 0) return false;
  std::string ts_part = raw.substr(0, sep);
  std::string mac_part = raw.substr(sep + 1);
  if (mac_part.size() != 64) return false;

  for (char c : ts_part) {
    if (c < '0' || c > '9') return false;
  }
  for (char c : mac_part) {
    bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!is_hex) return false;
  }

  out->unix_timestamp = std::strtoull(ts_part.c_str(), nullptr, 10);
  for (size_t i = 0; i < 32; i++) {
    out->mac[i] = static_cast<uint8_t>(std::strtoul(mac_part.substr(i * 2, 2).c_str(), nullptr, 16));
  }
  return true;
}

inline bool constant_time_equal(const Mac& a, const Mac& b) {
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

// Verifies `token` against `secret`/`context` and the freshness/anti-replay
// rules described above. On success, updates `last_accepted_timestamp` and
// returns true. On failure, `last_accepted_timestamp` is left untouched.
inline bool verify_and_accept(const Token& token, const std::string& context,
                               const std::string& secret, uint64_t now, uint64_t window_secs,
                               uint64_t& last_accepted_timestamp) {
  auto payload = build_payload(context, token.unix_timestamp);
  auto expected = crypto::hmac_sha256(reinterpret_cast<const uint8_t*>(secret.data()),
                                       secret.size(), payload.data(), payload.size());
  if (!constant_time_equal(expected, token.mac)) return false;

  uint64_t age = now >= token.unix_timestamp ? now - token.unix_timestamp
                                              : token.unix_timestamp - now;
  if (age > window_secs) return false;

  if (token.unix_timestamp <= last_accepted_timestamp) return false;  // repeat/replay

  last_accepted_timestamp = token.unix_timestamp;
  return true;
}

}  // namespace reboot2::auth

#endif  // REBOOT2_AUTH_TOKEN_H_
