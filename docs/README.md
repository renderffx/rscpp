RSC++ — React Server Components (Flight) in C++20
==============================================

A C++20 implementation of the React Flight wire protocol: server-side component
rendering, out-of-order streaming, streamed SSR, server actions, and a
dependency-free JavaScript browser runtime.

Read `docs/STATUS.md` for what is verified working and what is missing. Read
`docs/ARCHITECTURE.md` for how the streaming actually works, and
`docs/DIFFERENCES.md` for the specific protocol divergences from React 19.

QUICK START
-----------

1. Build:

     cmake -S . -B build -DRSCPP_BUILD_TESTS=ON
     cmake --build build

2. Run, from the repository root:

     ./build/Debug/rscpp_server.exe

3. Open http://localhost:3004/

CONFIGURATION
-------------

`rscpp.ini` in the repository root is resolved relative to the process working
directory, as is the `web/` asset directory. Only these keys are read; anything
else is ignored:

    host = 0.0.0.0          port = 3004              log_level = info
    max_request_size = 10485760                    static_dir = web
    rate_limit_capacity = 200                      rate_limit_refill = 100
    enable_csp = true                             enable_hsts = true

ROUTES
------

Every page is available two ways: `/<path>` streams HTML, `/rsc/<path>` streams
the raw Flight payload. Fourteen routes are registered:

    /                 dashboard            /products            product list
    /about            about page           /products/:id        product detail + client component
    /stress           recursive nesting    /search?q=           search by param
    /posts            post list            /error               error boundary
    /post/:post_id    post + author        /suspense            postponed boundary
    /heavy            large payload        /userlist            list rendering
    /counter          counter + actions    /userlist/:data      list from a request param

Endpoints:

    POST /rsc/action          invoke a server action
    GET  /rsc/manifest.json   registered components, actions, routes, client modules
    GET  /_health             liveness
    GET  /_metrics            request counters
    GET  /rsc/modules/<name>  client-component modules, served from web/modules/

WHAT WORKS
----------

  • Flight row framing, including length-prefixed 'T' rows whose payloads
    contain newlines
  • Out-of-order streaming: the root row is written before slow boundaries
  • Streamed SSR with per-boundary $RC/$R scripts and a per-response CSP nonce
  • Server actions returning a value over a reserved 'A' row
  • Client components via '$L<hex>' lazy references and 'I' import rows, with a
    small hooks API on the client
  • Error boundaries, request-scoped Context, dynamic routes, form actions
  • Five emitted wire tags: 0, I, E, T, A. 'P' and 'H' are recognised by the
    framer but never emitted

NOT IMPLEMENTED
---------------

  • Binary/stream rows (R, X, C, D, typed arrays) and the remaining React 19 tags
  • React priority lanes (transition/idle/concurrent)
  • Concurrent server-action isolation
  • Any compatibility with a real React client; the browser runtime is
    hand-written vanilla JS, not React

TESTING
-------

    ctest --test-dir build -C Debug --output-on-failure

253 unit checks, 163 end-to-end HTTP assertions. `-C Debug` matches the
multi-config generator CMake selects on Windows and is unnecessary on Ninja.

If e2e reports "server exited during startup", a stale MSYS2 /mingw64/bin on
PATH is shadowing the MinGW runtime and the process dies before main().
See docs/STATUS.md.