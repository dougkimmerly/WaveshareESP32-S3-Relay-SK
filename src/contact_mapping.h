#ifndef CONTACT_MAPPING_H_
#define CONTACT_MAPPING_H_

// Pure logic for the v2 "load powered" wire semantics (fixer #1224, ADR
// 0055 §4). Has no Arduino/SensESP dependencies so it can be exercised by a
// plain g++ host test — see test/test_contact_mapping.cpp.
//
// Every relay has two states that must be kept distinct:
//   - the SignalK wire value, which always means "is the downstream device
//     POWERED", regardless of contact type
//   - the physical coil state, which is energized/de-energized
//
// For a normally-open (NO) contact, the load is powered only while the coil
// is energized, so wire value and coil state track directly.
// For a normally-closed (NC) contact, the load is powered by default (coil
// de-energized) and the coil is energized only to CUT power (e.g. during a
// reboot pulse), so wire value and coil state are inverted.
//
// The mapping is its own inverse (negation is self-inverse, identity is
// self-inverse), so the same function converts load->coil and coil->load.
inline bool map_load_coil(bool value, bool is_no) {
  return is_no ? value : !value;
}

#endif  // CONTACT_MAPPING_H_
