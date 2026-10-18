# Known Differences from React 19 RSC

This is an honest list of where RSC++ is **not** React 19 RSC. Anything claimed
as parity elsewhere in this repository is testable; anything in this file is not,
and is listed so it is not mistaken for it.

## Wire protocol

| Area | React 19 | RSC++ | Notes |
|---|---|---|---|
| Suspense element | `["$","$Sreact.suspense",null,{...}]` | `["$","$Sreact.suspense",null,{...}]` | Identical. The element type is React's `Symbol.for("react.suspense")`, which Flight writes as the `$S`-prefixed string `react.suspense`. The numeric symbol id (`0xead1`, 60113) is an internal detail of React's symbol table and never appears on the wire. |
| Server action result | Returned as a root model row | Custom `A` tag, id `0x9c40000000000000` | `A` is not a React tag. This is a project-local extension, added to `is_newline_tag` in `flight/tags.hpp` explicitly rather than falling out of the `B`–`Z` range, and to the client decoder alongside React's tag classes. Note this makes the byte unusable for React's binary blob row, which also uses `A`; the JSON action meaning wins and the blob form is not reachable. |
| `P` (postpone) | Used for prerender/postponed aborts | Not emitted | RSC++ follows React here: an ordinary suspending boundary is *not* announced with `P`. `reserve_chunk` allocates an id, leaves it unemitted, and the parent references it as `$L<id>`; the client blocks that chunk until the content row lands. `P` is emitted for prerender aborts only, which this project does not do. |
| `H` (hint) | Emitted for debug hints | Not emitted | Recognised by the framer as a newline-terminated tag; nothing constructs an `H` record. |
| `$RC` script | Fizz signature `(boundaryId, segmentId)` | Fizz-shaped, but carries a third argument with the boundary's Flight row | The extra argument is what lets the streamed document resolve its `$L` references without a second request. |

Shared with React: `0:` root row, `T<hexlen>,` byte-length framing, `I` import
rows, `E` error rows, and the `$`/`$L`/`$@`/`$F`/`$<id>` reference forms. The unit
suite compares the encoder's output against the decoder, and the e2e suite
re-implements the framing in JavaScript (`parseWire`), so two framers written
separately from the encoder must agree on every row.

## Server runtime

| Area | React 19 | RSC++ |
|---|---|---|
| Work loop | Fiber scheduler with priority lanes | Single render thread; `drain_pending()` polls the wait list and promotes ready futures onto a task queue (no waiter threads) |
| Concurrency | Priority lanes, transitions, `startTransition` | None. Boundaries resolve in completion order. |
| Out-of-order | Every server component can be a task | Only a **Suspense boundary** outlines its content into a chunk |

That last row is the significant one. A large tree with no Suspense boundary
still serializes into a single model row, so time-to-first-byte is bounded by
render time for that row. Real React would let independent subtrees stream
separately. Outlining every subtree would multiply row count and change the
wire format, so it was deliberately not done.

## Client runtime

There is no React on the client. `web/client.js` is vanilla JavaScript with its
own element-mounting renderer, and client components receive exactly one hook —
`useState` — not React's.

| Area | React 19 | RSC++ |
|---|---|---|
| Renderer | Fiber, diffing, hooks | Imperative DOM mounting: the component's whole output is re-created and swapped in, so there is no diffing and no reconciliation |
| Hooks | Full reconciler hook model | Exactly one hook, `useState`, implemented by hand: per-component-instance state, keyed by call order, re-run from cursor 0 on every update. `useReducer`, `useRef`, `useMemo`, `useCallback`, `useEffect` and the rest do not exist |
| Scheduling | Lane-based, interruptible | None; there are no lanes and no interruption |
| State ownership | Server + client | Server actions mutate process memory; client state lives only in the mounted instance and is lost on re-fetch |
| Hydration | Reconciles against server markup | Mounts into `#root`, replacing it |
| Code splitting | Automatic | Manual, via `Registry::register_client_component` and a `web/modules/` script |

Server-rendered boundaries are swapped by the generated `$RC` script rather than
by React's Fizz machinery, and client hydration consumes the same rows.

## What the tests actually prove

Worth being precise, because the coverage is uneven:

- **Deleted as unsound:** the `differential` target compared `StreamDecoder`
  against a second framer written from the same reading of the spec. It
  re-used the implementation's own `RawRow` type and its tag set never
  mentioned `'A'`, so it could not have detected the tag-classification
  defect it existed to catch. Its runner also loaded a directory that did not
  exist. What remains in its place is the e2e suite's independent JavaScript
  framer, which shares no type with the C++ implementation.
- **Regression pins, not validation:** the golden wire strings in
  fixture files. They were derived from this
  encoder's output, so they catch drift; they do not independently prove the
  format is correct.
- **Real behaviour:** the E2E suite drives the server over HTTP and asserts
  arrival order — that the root row precedes a boundary's content row, that a
  `$L` reference always has a row behind it, that the placeholder precedes the
  content that replaces it, and that the streamed `$RC` script both moves the
  markup and queues its row.

There is no property-based or mutation testing in this repository. Earlier
versions of this document claimed both, along with a `CLAIMS.md` that does not
exist; those claims were not backed by code and have been removed.

## Not implemented

- Automatic client code splitting.
- A reconciling client renderer: client components are re-created and swapped,
  so unmounted DOM state is not preserved across updates.
- Server state persistence; application state is process memory.
- Streaming recovery after an error boundary activates mid-stream.
- Priority-based boundary resolution.
