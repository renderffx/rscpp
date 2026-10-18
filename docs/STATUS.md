# RSC++ — verified status

Everything below was checked against the source and, where marked, against a
running server. Claims that could not be verified are not repeated here.

Build and test, from the repository root:

```
cmake -S . -B build -DRSCPP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
```

With the full test tree present: **2/2 CTest targets pass** — 253 unit checks,
163 end-to-end HTTP assertions. Those counts describe the full tree revision;
the `tests/` directory is not part of this checkout, and the CMake configure
step says so when `-DRSCPP_BUILD_TESTS=ON` is passed. The former `differential`
target was deleted: it compared the decoder against a second copy of itself
(see below).

`-C Debug` is required on Windows because CMake selects a multi-config
generator there; on a single-config generator it is unnecessary.

> If `e2e` fails with "server exited during startup", check `PATH`. A stale
> MSYS2 `/mingw64/bin` ahead of a newer MinGW makes the process die with
> `0xC0000139 STATUS_ENTRYPOINT_NOT_FOUND` before `main()`. This is an
> environment problem, not a code fault.

## What is real

**Flight framing** (`include/rscpp/flight/{row,decoder,tags}.hpp`). Rows are
`<hexId>:<tag><payload>`. `parse_row` detects a tag only when the byte after the
colon is a known tag, so `0:null`, `1:42` and `2:true` parse as models rather
than being rejected. `StreamDecoder` is incremental and handles `'T'` rows
correctly — those are framed on their byte-length prefix, not on a newline,
because the payload may itself contain newlines. Malformed input latches a
failure state instead of half-parsing.

**Tags: classification follows React, emission is a subset.** `classify_tag`
accepts the byte-length-prefixed set `TOobUSsLlGgMmV` (`flight/tags.hpp`) plus the
newline-terminated set `A`, `B`–`Z`, `#`, `r`, `x`. This project **emits** five
tags:

| Tag | Meaning | Emitted by |
|---|---|---|
| `0` | model | every content and root row (`renderer.hpp`) |
| `I` | client-module import | `Renderer::ensure_client_ref` |
| `E` | error | a throwing component, boundary, request, or action |
| `T` | text | text over the inline-text threshold |
| `A` | server-action result | `ACTION_RESULT_ROW_ID` (`0x9c40000000000000`) |

`P` and `H` are **recognised** by the framer but never emitted (`P` is for
prerender/postponed aborts, which this project does not perform; hints are not
implemented). Binary rows (`R`/`X`/`C`), typed arrays, `N`/`O`/`o`/`U`/`S`/`s`/
`L`/`l`/`G`/`g`/`M`/`m`/`V` are **not implemented**. Earlier documents in this repo
claimed 31 tags; that was false.

**Out-of-order streaming** — verified on the wire. From `GET /rsc/suspense`:

```
0:["$","div",null,{...,"children":"$L1",...},"loaded":false]     <- immediately
1:["$","div",null,{"children":[...,"Streamed content arrived"]}] <- after the work
```

The root row reaches the client before the slow subtree. This is real
postponement, not a claim: `reserve_chunk` allocates an id and leaves it
**unemitted**, `postpone_boundary` puts `$L<id>` in the parent and returns
immediately, and `drain_pending()` is a poll loop on the render thread — every
few milliseconds it promotes ready futures from the wait list onto a task
queue and runs them. There are no per-suspension waiter threads anywhere in
the renderer.

React does **not** emit a `P` row for an ordinary suspending boundary — `P` is
for prerender/postponed aborts, which this project does not perform. The client
creates a blocked chunk for the referenced id and resolves it when the content
row arrives. Two e2e assertions previously encoded the older, non-React design
(`postpone row arrives`, `boundary is announced postponed before its content`)
and were corrected; both had been passing against stale binaries.

**Streaming SSR** — the document emits head → body → per-boundary `$RC`/`$R`
scripts → tail, rather than buffering until the slowest boundary finishes.

**Client components** are implemented end to end — `ensure_client_ref` emits an
`I` import row, the model references it as `$L<hex>`, and `client.js` imports
the module and mounts it. This is covered by the unit suite (import row and
model row against goldens, plus import-row reuse for a repeated module) and by
the SSR hydration path. Unlike earlier revisions of this document, the demo
**does** exercise it: `web/modules/LikeButton.js` is registered as a client
component and rendered by `/products/:id`, and `/rsc/manifest.json` lists it
under `client_modules`.

**Server actions** work end to end: `POST /rsc/action`, handler invoked, page
re-rendered, and the return value emitted as a reserved `A` row
(`ACTION_RESULT_ROW_ID`). Action ids are **explicit stable strings**
(`"increment"`, `"like:toggle"`) passed at registration, not sequential
auto-generated ones — earlier docs claimed the opposite and called it the
top-priority bug.

**HTML safety** is above average for a project of this size: a 97-element tag
allowlist, scheme-validated URLs that reject `javascript:`/`data:` after
stripping control characters, `on*` and `style` props dropped from markup, and a
real CSP with a per-response nonce instead of `unsafe-inline`.

## Conformance

"Verified" below means a named test in this repository asserts it — not that it
was compared against React.

This implementation is Flight-protocol-compatible for the tag set it implements.
It is not React 19 RSC, and no test here establishes React equivalence, because
there is no React runtime in this repository to compare against.

| Area | Behaviour | Test | Status |
|---|---|---|---|
| Row framing | `<hexId>:<tag><payload>`; a byte after the colon is a tag only if it is a known tag, so `0:null` parses as a model | `unit_core` `parse_row_accepts_every_documented_tag` | Verified |
| `T` rows | Framed on the byte-length prefix, not a newline; payloads containing newlines survive | `stream_decoder_t_rows_are_length_prefixed_not_newline_framed`, `stream_decoder_accepts_multibyte_text` | Verified |
| Malformed input | Latching failure state; unknown tags, truncated text, bad hex ids and invalid JSON are all rejected | `parse_row_rejects_malformed_input`, `stream_decoder_latches_failure`, `stream_decoder_rejects_a_truncated_model_row` | Verified |
| Out-of-order streaming | Root row precedes the postponed boundary's content row | `e2e` `testSuspenseStreamsOutOfOrder` | Verified |
| Postpone protocol | No `P` row; parent carries `$L<id>` and the id stays unemitted until its content row | `unit_core` `renderer_leaves_a_suspended_boundary_unemitted`, `renderer_emits_the_root_row_before_a_suspended_boundary_resolves` | Verified |
| Streamed SSR | Document emits head → body → per-boundary scripts → tail | `e2e` `testSuspenseHtmlStreaming`, `testStreamingSsr` | Verified |
| Boundary markup swap | `$RC` clears the fallback, moves the streamed segment in, queues the row | `e2e` `testBoundaryRuntimeExecutes` (executes the generated script) | Verified |
| Server actions | `POST /rsc/action` → invoke → re-render → fresh Flight stream, plus an `A` row carrying the return value | `e2e` `testServerActions`, `testActionStateIsShared` | Verified |
| Client components | `I` import row + `$L<hex>` lazy reference; a repeated module reuses its import row; a component inside a postponed boundary still emits its `I` row | `unit_core` `renderer_reuses_one_import_row_per_client_module`, `renderer_emits_client_ref_rows_for_components_inside_a_postponed_boundary` | Verified |
| Error rows | `E` rows with `digest` and `message`; a thrown boundary renders its fallback | `renderer_turns_a_throwing_component_into_an_error_row` | Verified |
| HTML escaping / tag and URL safety | 97-tag allowlist; `javascript:`/`data:` rejected after control-char stripping; `on*` and `style` dropped | `html_drops_dangerous_urls_and_unsafe_tags`, `html_escapes_text_and_drops_event_handlers`, `html_drops_style_attributes` | Verified |
| CSP | Per-response nonce on generated scripts, no `unsafe-inline` | `e2e` `testSecurityHeaders`, `testServerRenderedHtml`, `testSsrNonceIsScopedToItsResponse` | Verified |
| Attribute correctness | Only known HTML attributes are emitted; `data-*`/`aria-*` allowed; component-only props stay in the model | `html_omits_closing_tags_for_void_elements`, `html_and_model_agree_for_nested_text` | Verified |
| Routes and dynamic params | 15 routes (`src/server/main.cpp`), each served as both HTML and `/rsc/…` Flight | `e2e` `testFlightRoutes` | Verified |
| Context | Request-scoped provide/consume, `cache()`, `async_once()`, `await()` | `context_caches_per_key`, `context_async_once_reuses_the_same_future`, `context_provide_consume_and_params` | Verified |
| Router | `:param` extraction, base path, partial-match rejection | `router_first_match_wins_and_extracts_params`, `router_applies_a_base_path`, `router_rejects_paths_that_only_partially_match` | Verified |
| Registry | Lookup by name; iteration snapshots are independent copies | `registry_lookup_by_name`, `registry_iteration_snapshots_are_independent` | Verified |
| Manifest | `/rsc/manifest.json` lists components, actions, routes and client modules | `e2e` | Verified |
| Static assets | `web/` is served from disk with MIME types | `e2e` `testStaticAssets` | Verified |
| Health and metrics | `/_health`, `/_metrics` counters advance under traffic | `e2e` `testHealth`, `testMetricsAfterTraffic` | Verified |

## What is missing

Stated plainly, because the previous status documents in this repo were mostly
fiction:

- **Not a React 19 client.** The browser runtime is vanilla JavaScript in
  `web/client.js`. There is no C++ client, and no test runs against a real
  React runtime.
- **No binary/stream rows.** `R`, `X`, `C`, `D`, typed arrays and the remaining
  React 19 tags are absent.
- **No priority lanes.** Boundaries resolve in completion order. React's
  transition/idle/concurrent model is not implemented.
- **Server actions are not concurrent-safe.** Simultaneous invocations are not
  isolated, and there is no pending-state tracking.
- **No golden output from real React.** Every check compares rscpp against its
  own encoder and framer. That proves internal consistency and protocol framing —
  not agreement with React itself.

## Bugs found and fixed

Each of these was reproduced first, fixed, and then covered by a test that was
confirmed to fail without the fix.

1. **The build did not compile.** `HTMLStreamResponse` was used by
   `SsrStreamSink` before its definition; a duplicate `RSCBoundaryRuntime(nonce)`
   call; `RSCBoundaryRuntime` used above its definition; and a 108-line
   anonymous-namespace block of helpers (`trim`, `request_info_from`,
   `not_found_node`, `error_page`, `page_title`) defined hundreds of lines after
   first use. The whole server target failed to build.

2. **A postponed boundary's Flight row never reached the HTML client.** The
   markup streamed in, but `run_boundary` called `write_boundary_content` —
   which snapshots unsent rows — *before* `fulfill_chunk` emitted the content
   row. The client received `$L1` with no row behind it and would have waited
   forever. Fixed by shipping rows as they are produced once the body is open
   (`$R`), and by keeping the queued-rows channel live after hydration instead of
   draining it once.

3. **`$RC` never cleared the stale fallback.** The generated runtime appended the
   streamed content into the boundary placeholder but left the fallback markup
   in place, so a resolved boundary displayed both.

4. **Client-component rows inside a postponed boundary were never emitted.**
   `ensure_client_ref` buffers `I` rows, but `flush_extra_rows()` only ran inside
   `render()` — which has already returned by the time a boundary is retried. A
   `$L<id>` could be emitted with no `I` row behind it.

5. **Component-only props leaked into markup as invented attributes.** The SSR
   attribute filter checked name charset but not HTML validity, so a counter
   rendering `<div count={n}>` produced `<div count="0">`. Replaced with a
   known-attribute allowlist plus `data-*`/`aria-*`, applied after the
   `className`→`class` mapping.

6. **The server action `A` row was mis-framed, breaking every action call.**
   `LENGTH_PREFIXED_TAGS` contained `'A'`, so `to_wire()` byte-length-prefixed
   the action row and emitted `9c40000000000000:A0,` — the `0` is the length,
   and the JSON return value was gone. The client, which reads `'A'` as a
   newline-terminated JSON row, reported `row 11258999068426240000:
   unterminated row` and every action request failed. In React `'A'` is a binary
   blob row, which collided with this project's JSON action row. Fixed by
   removing `'A'` from `LENGTH_PREFIXED_TAGS` (and from the `A`..`Z` range in
   `is_newline_tag`, so it is unambiguously newline-terminated); the byte now
   emits `9c40000000000000:A{"count":1,"ok":true}`. `NEWLINE_TAGS` also had a
   stray space, which classified `' '` as a tag byte.

7. **Two tests asserted a protocol React does not speak.** The unit test and the
   goldens both encoded a Suspense type as the numeric `60113`, but
   Flight writes React's suspense symbol as the string `react.suspense` and the
   encoder never used the number; the golden had been generated against a stale
   binary. Separately, two e2e assertions required a `P` postpone row for a
   suspending boundary, which React emits only for prerender aborts. All four
   were corrected to match verified wire output.

8. **`web/client.js` looked for a Suspense marker that cannot exist.** It
   compared an element type against the numeric `SUSPENSE_TYPE = 60113`, so a
   streamed Suspense element was never recognised. It now resolves
   `Symbol.for("react.suspense")`, which is what the `$S`-prefixed wire string
   denotes.

9. **A unit test raced the render thread.**
   `renderer_emits_client_ref_rows_for_components_inside_a_postponed_boundary`
   fulfilled its promise from the main thread *before* calling `render()`, so
   `drain_pending()` had nothing left to postpone and the boundary never
   suspended. It now blocks until `render()` has returned, which makes the
   pending count and the root row's `$L` reference deterministic.

## Tests

| Target | Checks | Proves |
|---|---|---|
| `unit_core` | 253 | framing, chunk ordering, postponement, context, router, registry, escaping |
| `e2e` | 163 | real HTTP: arrival order, action round-trips, security headers, CSP nonce, the `$RC` runtime actually executing |

The e2e suite is a second implementation of row framing on purpose
(`parseWire`), so a bug that makes server and client agree on a wrong format
still fails. It asserts *ordering* and *presence of referenced rows*, not merely
absence of error strings.

Files that were not wired into any target and encoded a wrong protocol were
removed: `tests/differential/differential-runner.js` (loaded a directory that
does not exist), `tests/e2e/verify-relational.js`, the orphaned
`tests/reference/*.js` fixtures (they encoded `E` as an element row and `H` as a
fragment), and `tests/fixtures/`. `tests/differential/` and the `differential`
CTest target are gone.

Two JS modules, `web/modules/InteractiveCounter.js` and
`web/modules/SearchBox.js`, plus `web/inspector.html` and `web/rsc-client.d.ts`
were removed: no route, module or page referenced them. The only client
component the demo registers is `web/modules/LikeButton.js`.