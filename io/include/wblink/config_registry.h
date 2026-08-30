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
#include <vector>

#include "wblink/config.h"

namespace wblink {

// kStringOrNumber exists for exactly one key: streams[].stream_type, which
// parse_stream_type() (io/src/config.cpp:43) accepts as either the numeric id
// or its spelled name. Declaring it as either one alone would make a --strict
// type check reject a shape the loader takes.
//
// kArrayOrObject exists for exactly one key on the same reasoning:
// §15.2 `adapters` is an array of stanzas OR the Pass 195 `{"auto": {...}}`
// object, and the loader takes both. The published schema must say so —
// declaring it an array would tell a config generator that the auto form is
// invalid, which is the one thing a declared surface must never do.
enum class KeyType {
    kObject,
    kArray,
    kString,
    kNumber,
    kBool,
    kStringOrNumber,
    kArrayOrObject
};

struct KeyEntry {
    // Dotted path; see the header comment for "[]" semantics.
    const char* path;
    KeyType type;
    // Liveness predicate, or nullptr when the key is live on every node.
    //
    // Returns false when this node's mode does not read the key. It is a
    // DECLARED property, not one inferred from which branch the parse took:
    // the loader parses `policy.return.*` unconditionally (config.cpp:649)
    // and the gate that makes it dead is downstream (app/main.cpp:3846), so
    // no amount of watching the parse can see it.
    //
    // Each predicate encodes a gate that exists in the code, cited at its
    // definition in config_registry.cpp. A key group whose gate cannot be
    // pointed at stays unpredicated — a guessed predicate reports a live key
    // as dead, which is worse than not checking.
    bool (*live)(const Config&) = nullptr;
    // Why the key would be inert, phrased for an operator. Non-null exactly
    // when `live` is.
    const char* inert_reason = nullptr;
};

// What --strict says about one key found in a config.
enum class KeyVerdict {
    kUnknown,  // not in the registry: a typo, or a name from an older example
    kInert,    // registered, but this node's mode does not read it
};

struct KeyFinding {
    std::string path;
    KeyVerdict verdict;
    // For kInert, the matching entry's inert_reason. Empty for kUnknown.
    const char* reason;
};

// Every key in `config_json` that is unknown or inert for `cfg`, in path
// order. Keys with any `_`-prefixed segment are comments by convention and
// are never reported.
//
// `config_json` is the raw text the config was loaded from; `cfg` is the
// result of loading it, which is what the liveness predicates read.
std::vector<KeyFinding> check_config_keys(const std::string& config_json,
                                          const Config& cfg);

// The findings as a JSON object, for `--check --strict --json`. Reports paths
// and verdicts only — never a value, so no config content reaches it.
std::string check_report_json(const std::vector<KeyFinding>& findings);

// The registry, sorted by path. The sort is asserted in
// tests/config_schema_test.cpp, which also checks the serialiser's shape.
const KeyEntry* config_registry(std::size_t* count);

// The registry as JSON, exactly what `waybeam-link config-schema --json`
// prints. Trailing newline included.
std::string config_schema_json();

}  // namespace wblink

#endif  // WBLINK_CONFIG_REGISTRY_H
