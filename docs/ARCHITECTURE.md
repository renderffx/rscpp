# Architecture

RSC++ is a C++20 implementation of the React 19 RSC / Flight wire protocol,
including out-of-order streaming and streamed server rendering.

## Layering

Dependencies flow strictly downward. A layer may include the layers above it in
this list's order; it may never include the layers below it.

| Layer | Location | Depends on | Notes |
|---|---|---|---|
| 1. Core runtime | `include/rscpp/` | `third_party/nlohmann` | Header-only. No I/O, no HTTP. |
| 2. Transport | `third_party/httplib/` | — | Vendored cpp-httplib. |
| 3. Server | `src/server/` | 1, 2 | HTTP/RSC server application. |
| 4. App | `src/examples/` | 1 | Demo pages and actions wired up by `src/server/main.cpp`. |

Client runtime assets live in `web/`, which the server serves from disk. They are
**not** in `include/` because they are served, not compiled in.

## `include/rscpp/` — the core runtime

Every header is self-contained and may be included in any order. Headers are
grouped into layers that mirror the include graph, so the dependency direction
below is visible in the directory layout:

| Layer | Directory | Depends on | Provides |
|---|---|---|---|
| 1. Primitives | `util/` | — | `hash.hpp` — string hashing shared by registry and flight |
| 2. Model | `model/` | 1 | `value.hpp` (`Value`, `Ref`, `to_json`/`from_json`), `node.hpp` (`Node`, `NodeType`, `Props`, `Children`, `ServerComponent`, `ClientComponent`, `ServerAction`, `Element`, `Text`, `Fragment`), `registry.hpp` (`Registry`, thread-safe snapshots), `suspend.hpp` (`Suspend`) |
| 3. Wire | `flight/` | 1, 2 | `tags.hpp` (`classify_tag`, `LENGTH_PREFIXED_TAGS`, `ACTION_RESULT_ROW_ID`, `REACT_SUSPENSE_TYPE`, `hex`/`parse_hex`), `row.hpp` (`RowRecord`, `RawRow`, `parse_row`, `RowSink`, `make_element`), `decoder.hpp` (`StreamDecoder`) |
| 4. Output | `html/` | — | `escape.hpp`, `allowlist.hpp` (tag, attribute and URL-scheme allowlists) |
| 5. Engine | `runtime/` | 2, 3, 4 | `context.hpp` (`Context`, `RequestInfo`, `cache()`, `async_once()`, `await()`), `router.hpp` (`Route`, `Router`), `renderer.hpp` (`Renderer`, `HtmlSink`) |

`rsc.hpp` is an umbrella that includes all of the above; it declares no types of
its own.

`Value` and `Node` are mutually recursive (`Value` may hold a `NodePtr`; `Node`
holds `Props` of `Value`). `NodePtr`/`RefPtr` are forward-declared in
`model/value.hpp` and completed in `model/node.hpp`.

## How out-of-order streaming works

This is the part that distinguishes a Flight server from a template engine.

### Rows are pushed, not collected

`Renderer` never builds a page and hands it over at the end. It writes each row
to a `flight::RowSink` the moment that row is finished:

```
Renderer::render()   -> P row, then the root row, immediately
Renderer::drain_pending() -> content rows, each the moment its work completes
```

`drain_pending()` is called *after* `render()` returns, which is exactly why the
root row can be on the wire while a slow boundary is still working.

### A suspended boundary is postponed, not waited on

When `serialize_children` throws `Suspend` for a boundary, `postpone_boundary`:

1. reserves a chunk id (`reserve_chunk`, which increments `pending_` and leaves
   the id **unemitted**);
2. renders the fallback into
   `<div id="B:<hex>" class="rsc-boundary" data-rsc-boundary="<hex>">`;
3. returns a Suspense element whose `children` is `"$L<hex>"`;
4. calls `schedule_retry`, which starts a detached thread that blocks on the
   future and then **enqueues** a continuation;
5. `drain_pending()` runs that continuation on the render thread as
   `run_boundary`, which emits `<id>:<model>` (`fulfill_chunk`, decrementing
   `pending_`).

Step 4 is why the render thread never blocks on the future, and why renderer
state is still only ever touched by one thread. The wait happens on a thread that
is allowed to do nothing but wait, and it is only allowed to push onto the queue.

No `P` row is emitted for a boundary that suspends. React uses `P` for
prerender/postponed aborts, which this project does not perform; the client
instead creates a blocked chunk for the referenced id and resolves it when the
content row arrives. A boundary that suspends a second time (awaiting two things
in sequence) is rescheduled rather than failed, so it still produces exactly one
content row.

### Ordering

Row order is document order for the ready parts of the tree, then the postponed
chunks in completion order. A client never depends on a chunk arriving in
document order; that is what the `$L` indirection is for.

## Streaming server rendering

The HTML response cannot simply embed one buffered payload, because holding the
document until the slowest boundary finishes is the thing being fixed. So the
document is emitted in three phases:

| Phase | Written by | Contents |
|---|---|---|
| Head | `build_shell_head` | `<head>`, the `$RC` runtime, and the Flight payload produced so far, embedded in `__RSC_DATA__` |
| Body | `SsrStreamSink` | root markup, with each postponed boundary as a placeholder div |
| Late | `drain_pending()` | `<div hidden id="S:<hex>">content</div>` plus a `$RC` script, per boundary, as each completes |
| Tail | `build_shell_tail` | `client.js` and the closing tags |

`$RC(boundaryId, segmentId, row)` is defined in the head, not in the deferred
module, because a boundary can finish long before `client.js` executes. It moves
the delivered markup into the placeholder so the user sees the content without
waiting for hydration, and queues the row for the decoder.

### The nonce

Those scripts have to execute, so the CSP has to authorise them. Rather than
`script-src 'unsafe-inline'`, each HTML response gets a random per-response
nonce that appears both in the CSP header and on every generated script tag. It
is created in the pre-routing handler, before the security-headers middleware
runs, and passed by value into the body worker, because the body is produced on a
different thread than the one that built the headers. Flight and asset responses
get no nonce at all.

## `src/server/` — the server application

One responsibility per file, each a target in `CMakeLists.txt`:

| File | Responsibility |
|---|---|
| `server.hpp` / `server.cpp` | `RSCServer`, route table, request handlers, middleware, response construction |
| `stream.hpp` | `ChunkedPipe`, `ChunkSink`, `StreamResponse` (one type covering Flight and HTML), `SsrStreamSink` |
| `delivery.hpp` / `delivery.cpp` | httplib `Request` → `RequestInfo`, page/error node construction, the runtime script, static-file delivery and MIME lookup |
| `config.hpp` / `config.cpp` | `ServerConfig` and `rscpp.ini` parsing |
| `logging.hpp` / `logging.cpp` | `Logger` and log levels |
| `metrics.hpp` / `metrics.cpp` | `ServerMetrics`, `RateLimiter` |
| `main.cpp` | process entry point: read configuration, register the demo app, bind routes |

`StreamResponse` is header-only: its behaviour is entirely the choice of sink
(`FlightSink` or `SsrStreamSink`), so there is no `stream.cpp` to compile.

`src/examples/dashboard/` holds the demo app — `pages.hpp` (the node trees),
`components.hpp` (component definitions and client-component registration),
`store.hpp` (process-local state and server actions), `demo.hpp` (registration).

### Runtime-relative paths

Two paths are resolved **at runtime** relative to the process working directory,
and therefore constrain the repository layout:

- `rscpp.ini` — opened by `src/server/main.cpp` at startup. Only the keys in the
  table below are read; anything else in the file is ignored.
- `web/` — `ServerConfig::static_dir` defaults to `"web"`.

Both are kept at the repository root on purpose. Moving either would silently
break configuration loading or static asset serving.

| Key | Default | Meaning |
|---|---|---|
| `host` | `0.0.0.0` | Bind address |
| `port` | `3004` | Bind port |
| `log_level` | `info` | `trace`, `debug`, `info`, `warn`, `error` |
| `max_request_size` | `10485760` | Request body cap, bytes |
| `static_dir` | `web` | Client asset directory |
| `rate_limit_capacity` | `200` | Per-client-IP token bucket size |
| `rate_limit_refill` | `100` | Tokens refilled per second |
| `enable_csp` | `true` | Send `Content-Security-Policy` |
| `enable_hsts` | `true` | Send `Strict-Transport-Security` |

## Build and test

```
cmake -S . -B build -DRSCPP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
```

`-C Debug` matches the multi-config generator CMake selects on Windows; it is
unnecessary on a single-config generator such as Ninja.

Layer boundaries are expressed as interface targets so the build, not convention,
enforces them: `rsc_core` and `rsc_httplib`.

Two CTest targets cover the system:

| Target | What it proves |
|---|---|
| `unit_core` | Framing, chunk ordering, postponement, context, router, registry |
| `e2e` | Real streaming behaviour over HTTP, including arrival order and the SSR nonce |

The E2E suite asserts ordering rather than absence-of-references: it fails if the
root row is not the first row to arrive, if the content row arrives before it, or
if a `$L` reference is left without a row behind it.
