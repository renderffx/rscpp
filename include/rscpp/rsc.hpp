#pragma once

// rscpp — server components and RSC Flight encoding for C++20.
//
// Layout:
//   rscpp/model/    value, node, registry and suspension types
//   rscpp/flight/   Flight row framing, decoding and tags
//   rscpp/html/     HTML escaping and the attribute/tag allowlists
//   rscpp/runtime/  request context, router and renderer
//   rscpp/util/     small shared helpers

#include "rscpp/flight/decoder.hpp"
#include "rscpp/flight/row.hpp"
#include "rscpp/flight/tags.hpp"
#include "rscpp/html/allowlist.hpp"
#include "rscpp/html/escape.hpp"
#include "rscpp/model/node.hpp"
#include "rscpp/model/registry.hpp"
#include "rscpp/model/suspend.hpp"
#include "rscpp/model/value.hpp"
#include "rscpp/runtime/context.hpp"
#include "rscpp/runtime/renderer.hpp"
#include "rscpp/runtime/router.hpp"
#include "rscpp/util/hash.hpp"