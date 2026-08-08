// Host-native test for the pure NC/NO semantics mapping in
// src/contact_mapping.h. Build and run with plain g++, no PlatformIO/Arduino
// toolchain required:
//
//   g++ -std=c++17 -I../src test_contact_mapping.cpp -o /tmp/test_contact_mapping && /tmp/test_contact_mapping
//
// or from the repo root:
//
//   g++ -std=c++17 -Isrc test/test_contact_mapping.cpp -o /tmp/test_contact_mapping && /tmp/test_contact_mapping

#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "contact_mapping.h"

int main() {
  // NC (is_no = false): wire "powered" (true) means coil de-energized;
  // wire "unpowered" (false) means coil energized (cuts power).
  assert(map_load_coil(true, false) == false);
  assert(map_load_coil(false, false) == true);

  // NO (is_no = true): wire value and coil state track directly.
  assert(map_load_coil(true, true) == true);
  assert(map_load_coil(false, true) == false);

  // The mapping is its own inverse in both directions.
  for (bool is_no : {false, true}) {
    for (bool value : {false, true}) {
      assert(map_load_coil(map_load_coil(value, is_no), is_no) == value);
    }
  }

  // Boot invariant: all coils reset LOW (false). For NC relays that must
  // decode to the load being POWERED; for NO relays, unpowered.
  assert(map_load_coil(/*coil=*/false, /*is_no=*/false) == true);   // NC → powered
  assert(map_load_coil(/*coil=*/false, /*is_no=*/true) == false);   // NO → unpowered

  std::printf("test_contact_mapping: all assertions passed\n");
  return 0;
}
