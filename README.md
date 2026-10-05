# rscpp

C++20 React Server Components (Flight) implementation with out-of-order
streaming and streamed server rendering.

**See [`docs/STATUS.md`](docs/STATUS.md) for what is verified working and what is
missing, and [`docs/DIFFERENCES.md`](docs/DIFFERENCES.md) for where this is not
React 19.** Earlier status documents in this repository contained claims that
were not true of the code; those were removed rather than left in place.

- **True out-of-order streaming.** A Suspense boundary that suspends does not hold
  back the page: the root row is written immediately with a `$L` reference, and
  the boundary's content row follows when the work completes. See
  [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md#how-out-of-order-streaming-works).
- **Streaming SSR.** The HTML response sends a paintable document first and
  delivers boundary content as it finishes, using nonce-scoped `$RC`/`$R` scripts
  rather than weakening the CSP to `unsafe-inline`.
- **Tight CSP.** No `unsafe-inline`, no `unsafe-eval`, no path traversal.
- **Zero client dependencies.** Vanilla JavaScript client runtime; cpp-httplib and
  nlohmann/json are vendored.

## Layout

```
include/rscpp/     header-only core runtime (no I/O)
  model/           Value, Node, Registry, Suspend
  flight/          row framing, decoding, tags
  html/            escaping and the tag/attribute allowlists
  runtime/         Context, Router, Renderer
  util/            small shared helpers
src/server/        HTTP + Flight server application
src/examples/      demo application
web/               client runtime assets served from disk
tests/             unit and end-to-end suites
docs/              architecture, verified status and protocol notes
```

Include `#include "rscpp/rsc.hpp"` for the whole core; the individual headers
under it are also self-contained.

## Build and test

```
cmake -S . -B build -DRSCPP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
```

Two CTest targets: 253 unit checks (an independently written framer plus golden
wire strings), and 163 end-to-end HTTP assertions including row arrival order,
boundary fulfilment, the CSP nonce contract, and execution of the generated
`$RC` script against a DOM stub.

`-C Debug` matches the multi-config generator CMake picks on Windows; on a
single-config generator it is unnecessary.

If `e2e` fails with "server exited during startup", a stale MSYS2 `/mingw64/bin`
on `PATH` is shadowing the MinGW runtime; the process dies before `main()`.
