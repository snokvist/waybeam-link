// SPDX-License-Identifier: GPL-2.0-or-later
// Pass 180. Deliberately its own translation unit, compiled unconditionally:
// the answer has to exist in a receive-only build too, which is exactly the
// build most likely to be asked whether it has a transmitter.
#include "wblink/node/build_info_c.h"

// A missing definition is the one failure the value checks cannot see: `#if`
// on an undefined identifier is 0 with no diagnostic, and in a reduced build
// 0 is also the correct answer, so "the defines never arrived" and "the
// feature is off" are indistinguishable. -Wundef would catch it but cannot be
// enabled globally — ~20 pre-existing sites legitimately test macros that only
// exist when ON. Five #ifndefs cost nothing and cover every configuration.
#ifndef WBLINK_BI_FRAME_SHM
#error "WBLINK_BI_FRAME_SHM not defined - see target_compile_definitions(wblink_node)"
#endif
#ifndef WBLINK_BI_CONTROL_SERVER
#error "WBLINK_BI_CONTROL_SERVER not defined - see target_compile_definitions(wblink_node)"
#endif
#ifndef WBLINK_BI_VENC
#error "WBLINK_BI_VENC not defined - see target_compile_definitions(wblink_node)"
#endif
#ifndef WBLINK_BI_RADIO
#error "WBLINK_BI_RADIO not defined - see target_compile_definitions(wblink_node)"
#endif
#ifndef WBLINK_BI_NODE_TX
#error "WBLINK_BI_NODE_TX not defined - see target_compile_definitions(wblink_node)"
#endif

namespace {

// The WBLINK_BI_* macros come from CMake (see the target_compile_definitions
// beside this file's entry in CMakeLists.txt), NOT from the feature macros a
// consumer sees. That is deliberate and was measured: WBLINK_VENC is a CMake
// option that gates target_sources and is not a preprocessor macro at all, so
// `#if WBLINK_VENC` reads as OFF in a full build and the string reported a
// transmitter-capable archive as having neither venc nor a TX half. The test
// could not see it either, because the test used the same wrong macro — a
// self-consistent lie that only a cross-configuration comparison exposed.
//
// Written as one literal per field rather than assembled at runtime: this is
// a constant of the build, and a constant that formats itself is a constant
// that can be wrong in a way the compiler cannot see.
#if WBLINK_BI_FRAME_SHM
#define WB_BI_FRAME_SHM "true"
#else
#define WB_BI_FRAME_SHM "false"
#endif

#if WBLINK_BI_CONTROL_SERVER
#define WB_BI_CONTROL "true"
#else
#define WB_BI_CONTROL "false"
#endif

#if WBLINK_BI_VENC
#define WB_BI_VENC "true"
#else
#define WB_BI_VENC "false"
#endif

#if WBLINK_BI_RADIO
#define WB_BI_RADIO "true"
#else
#define WB_BI_RADIO "false"
#endif

// Taken from the variable CMake already computed for the source list, not
// re-derived here: two places deciding what "has a transmitter" means is how
// they come to disagree.
#if WBLINK_BI_NODE_TX
#define WB_BI_NODE_TX "true"
#else
#define WB_BI_NODE_TX "false"
#endif

constexpr const char* kBuildInfo =
    "{\"frame_shm\":" WB_BI_FRAME_SHM
    ",\"control_server\":" WB_BI_CONTROL
    ",\"venc\":" WB_BI_VENC
    ",\"radio\":" WB_BI_RADIO
    ",\"node_tx\":" WB_BI_NODE_TX "}";

}  // namespace

extern "C" const char* wblink_build_info(void) { return kBuildInfo; }
