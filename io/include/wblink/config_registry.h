// SPDX-License-Identifier: GPL-2.0-or-later
// The declared key surface of the §15.2 node config.
//
// `io/src/config.cpp` reads every key through value()/at()/contains(), so a key
// it does not recognise is silently ignored — a typo or a name carried over
// from an older example loads clean and flies wrong (#106). This table is the
// authority on which keys exist, and `config-schema --json` publishes it.
//
// The table is checked against the loader by tests/config_registry_test.py,
// which reconstructs the dotted path of every accessor site in config.cpp and
// asserts the two sets are equal in BOTH directions. Adding an accessor
// without adding an entry here fails that test.
//
// Path syntax: dotted, with "[]" marking an array element — "adapters[].bus"
// is the `bus` key of an element of the top-level `adapters` array. Arrays of
// scalars (e.g. "scout.channels") have no "[]" entries of their own.
//
// On `policy.csa.psk`: the registry carries the key's NAME and type, and must.
// It is a name, not a secret — it is already in PROTOCOL.md §15.2, in
// CLAUDE.md and in examples/config.radio-*.sample.json — and omitting it would
// make the published surface wrong and make a future --strict reject a config
// that legitimately sets it. Nothing here reads a config, so no value can
// reach this output; the redaction that matters stays in dump_config_summary()
// (io/src/config.cpp), which prints "(set, redacted)".
#ifndef WBLINK_CONFIG_REGISTRY_H
#define WBLINK_CONFIG_REGISTRY_H

#include <cstddef>
#include <string>

namespace wblink {

// kStringOrNumber exists for exactly one key: streams[].stream_type, which
// parse_stream_type() (io/src/config.cpp:43) accepts as either the numeric id
// or its spelled name. Declaring it as either one alone would make a --strict
// type check reject a shape the loader takes.
enum class KeyType { kObject, kArray, kString, kNumber, kBool, kStringOrNumber };

struct KeyEntry {
    // Dotted path; see the header comment for "[]" semantics.
    const char* path;
    KeyType type;
};

// No liveness field here yet, deliberately. `--strict` (#106 item 1 PR B)
// needs to say that a key is *inert* — registered, but not read on this node's
// mode: §15.2 withholds return/ARQ from an uplink-free node, so
// `policy.return.*` does nothing there even though the loader parses it
// unconditionally (config.cpp:649; the real gate is downstream in
// app/main.cpp:3846). That cannot be inferred from which branch the parse
// took, because the parse only sees loader-level gates and the ones that
// matter live past the loader — so it will have to be declared, as a member
// added here with a `= nullptr` default. Adding it then costs nothing: an
// aggregate initialiser that omits the member still compiles, so only the
// entries that gain a predicate change. Carrying a field that is null in all
// 263 entries today would just be flexibility nothing uses.

// The registry, sorted by path. The sort is asserted in
// tests/config_schema_test.cpp, which also checks the serialiser's shape.
const KeyEntry* config_registry(std::size_t* count);

// The registry as JSON, exactly what `waybeam-link config-schema --json`
// prints. Trailing newline included.
std::string config_schema_json();

}  // namespace wblink

#endif  // WBLINK_CONFIG_REGISTRY_H
