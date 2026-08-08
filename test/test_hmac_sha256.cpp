// Host-native test for src/hmac_sha256.h against known SHA-256/HMAC-SHA256
// test vectors (NIST + RFC 4231), plus the reboot2 auth-token wrapper in
// src/auth_token.h.
//
//   g++ -std=c++17 -Isrc test/test_hmac_sha256.cpp -o /tmp/t && /tmp/t

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "auth_token.h"
#include "hmac_sha256.h"

using reboot2::crypto::hmac_sha256;
using reboot2::crypto::sha256;

std::string hex(const uint8_t* data, size_t len) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0xf]);
  }
  return out;
}

int main() {
  // NIST: SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
  {
    auto d = sha256(reinterpret_cast<const uint8_t*>(""), 0);
    assert(hex(d.data(), d.size()) ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  }

  // NIST: SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
  {
    const char* msg = "abc";
    auto d = sha256(reinterpret_cast<const uint8_t*>(msg), std::strlen(msg));
    assert(hex(d.data(), d.size()) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  }

  // NIST: SHA-256 of a 56-byte message that straddles the padding boundary
  // (two-block message), catches off-by-one bugs in the padding logic.
  {
    const char* msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    auto d = sha256(reinterpret_cast<const uint8_t*>(msg), std::strlen(msg));
    assert(hex(d.data(), d.size()) ==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  }

  // RFC 4231 test case 1: HMAC-SHA-256(key=0x0b*20, "Hi There")
  {
    uint8_t key[20];
    std::memset(key, 0x0b, sizeof(key));
    const char* data = "Hi There";
    auto mac = hmac_sha256(key, sizeof(key), reinterpret_cast<const uint8_t*>(data),
                            std::strlen(data));
    assert(hex(mac.data(), mac.size()) ==
           "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  }

  // RFC 4231 test case 2: HMAC-SHA-256(key="Jefe", "what do ya want for nothing?")
  {
    const char* key = "Jefe";
    const char* data = "what do ya want for nothing?";
    auto mac = hmac_sha256(reinterpret_cast<const uint8_t*>(key), std::strlen(key),
                            reinterpret_cast<const uint8_t*>(data), std::strlen(data));
    assert(hex(mac.data(), mac.size()) ==
           "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  }

  // reboot2::auth::Token wrapper: make_token()/verify_and_accept() agree,
  // and context strings correctly domain-separate token streams.
  {
    using namespace reboot2::auth;
    const std::string secret = "shared-secret";
    uint64_t last_accepted = 0;

    auto t = make_token("ctx-a", 1000, secret);
    assert(verify_and_accept(t, "ctx-a", secret, /*now=*/1000, /*window=*/600, last_accepted));
    assert(last_accepted == 1000);

    // Same token, wrong context -> rejected (domain separation).
    uint64_t other_last_accepted = 0;
    assert(!verify_and_accept(t, "ctx-b", secret, 1000, 600, other_last_accepted));

    // Wrong secret -> rejected.
    uint64_t wrong_secret_last = 0;
    assert(!verify_and_accept(t, "ctx-a", "wrong-secret", 1000, 600, wrong_secret_last));
  }

  std::printf("test_hmac_sha256: all assertions passed\n");
  return 0;
}
