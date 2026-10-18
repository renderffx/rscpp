# rscpp

a c++20 implementation of react server components. one render pass emits the
flight rows and the html together, suspense parks instead of blocking, and the
browser runtime has no dependencies.

## why this exists

i was a signals fanboy. first time i looked at rsc i thought it was weird and
i hated it. then i read the internals and the whole thing flipped: the flight
protocol, embedding component trees in rows, streaming those rows while html
paints around them. every detail is insane craft, and it is way cooler than
people give it credit for.

so i wanted to know if i actually understood it. the honest test of whether you
understand a spec is to rebuild it in a language that was never meant for it.
javascript renders components and ships bytes without thinking about any of it.
c++ has to think about all of it: lifetimes, futures, ownership, who the render
thread is, and what happens when a value resolves three seconds after you
already wrote the page around it.

that friction is the point. when the platform is not native to the idea, the
idea has to stand on its own. building this taught me more about rsc than
reading every article about it did. the wire format stops being magic and
becomes a sequence of decisions you have made yourself.

## one walk, two outputs

most renderers produce a model and then hand it off. this one walks the tree
exactly once, and the walk itself is the serializer. every node it visits emits
two things in the same breath: a flight row for the client, and html markup for
the first paint. there is no intermediate ast, no collect-then-send phase. the
moment a row is complete it goes to its sink, and the bytes leave.

that is the idea that made rsc click for me. server components are not a
template engine that happens to stream. they are a single traversal that has to
answer two questions at every step — what does the client need to reconstruct
this tree, and what should be on screen right now — and it answers both before
moving to the next node.

so the walk carries two buffers the whole way down. markup accumulates as
escaped html. the model accumulates as a json tree where component references
become row ids, client component boundaries become import references, and text
above a kilobyte is cut out of line entirely into its own length-framed row, so
a giant string never bloats the model that has to be parsed to find everything
else. the walk knows both representations are true at once.

## nothing blocks, everything parks

the classic renderer calls a component, the component wants data, and the whole
render sits there. rsc's answer is suspension, and the interesting part is what
the renderer does when it happens.

when a component asks for a value that is not ready, it does not wait. it
throws. the future it was waiting on rides out with the throw, and the renderer
catches it, reserves a row id, writes a reference where the content belongs,
and keeps walking. the slow subtree costs the page nothing. the id is a promise
that row will exist later, and the client holds that exact reference from the
first paint.

this happens at three different scale levels, and each one parks separately:
the root of the page, a suspense boundary, and a single value or child slot
inside the flight model. a slow sibling inside one boundary does not park its
neighbors. a slow value inside one payload does not park the payload. react
draws these lines the same way — only the suspended property is replaced and
retried, everything around it keeps its finished form.

the markup and the row have to agree on the identity, so both sides of the walk
write from the same reserved id: the html gets a placeholder div with that id
as its name, the model gets a lazy reference to that id, and when the future
finally resolves, both the segment and the row are filled in and the client
stitches them together. nothing rewinds, nothing re-renders the page.

## who wakes the walk

parked work needs a waker. the obvious design is a thread per suspension, each
one waiting on its future. that is also how you get a thread explosion on a
page with forty boundaries, and renderer state shared across threads, which
means locks on everything.

instead there is one poll loop. the render thread parks every unresolved future
on a wait list, then sleeps a few milliseconds at a time, checking that list.
the moment a future is ready it is promoted onto a task queue, the loop runs
it, and the loop continues until nothing is queued and nothing is parked. the
renderer's state — the id counter, the buffers, the row sinks — belongs to one
thread from first byte to last. no locks around the walk itself.

it is the same shape react uses: a work loop that polls rather than a waiter
per promise. the difference is that here, "the work loop" is a literal loop,
and you can read the whole scheduling story in one function.

## the same future, the same id

there is a subtle trap in retrying parked work. a component re-executes, it
rebuilds its children, and the node objects you parked around are brand new.
if you keyed parked work by node, the retry would mint a second row id for
content that already has one, and the client would receive the same segment
twice under two names.

so parked work is keyed by the suspension itself — the future. the data layer
hands out the same future for the same suspension point even after the nodes
around it have been rebuilt, and the renderer looks up that future before
reserving anything. the retry finds its original id. one piece of content, one
row, no matter how many times the component re-ran to get there. react keys
flight rows by promise identity for exactly this reason.

the same mechanism makes double suspensions safe. a boundary that suspends,
retries, and suspends again parks twice under a single id, and only one row
ever exists for it.

## html that paints before its data

the html half gets the same out-of-order treatment, which is where the real
craft lives.

the document goes out in phases. the head installs a tiny runtime — the code
that will know how to move segments into place — before any application script
exists, and flushes the rows known so far. the body paints with a placeholder
for every pending boundary, so the user sees the full shape of the page
immediately. when a boundary resolves, its markup goes out as a hidden segment
plus a small script that relocates it into its placeholder, and the boundary's
row rides in that same script. the tail finally pulls in the client bundle,
which hydrates a page whose data is already there.

that is the entire out-of-order story. content is movable, placeholders carry
identity, and a few lines of inline runtime do the wiring. no framework needed
on the browser side to make it work.

client components blur the line in a nice way. the server cannot run them, so
it emits their first-paint markup as plain html — real markup, not empty shells
— wrapped in a template that names the component. the client runtime picks the
template up, finds the module, and swaps the live component in on hydration.
the page looks right before javascript loads, and the server never guessed at
what the client component would do.

scripts have to run, so every response mints a nonce, signs it into the csp
header, and stamps it on every script it generates. no `unsafe-inline`, no
weakening the policy to make streaming work. the markup side runs through tag
and attribute allowlists on the way out, urls are scheme-checked, and text is
escaped at the same point it enters the html buffer. the html and the model are
two projections of the same walk, and the html projection is filtered as it is
written.

## prerender and resume

there is a mode where the renderer does not even wait for the poll loop. with
prerendering on, it serializes everything that is ready, parks everything else
without ever waking it, and ends the response with a token. if nothing
suspended, there is no token and the page is a perfectly ordinary static
response — one request, done.

if the page did suspend, the token points at the parked work: the same
renderer, the same id space, the same request context, still in memory. the
client fetches the resume endpoint with that token, and the server serializes
only the parked slots — never repeating a row it already sent — and streams
just the missing segments. the ids line up with what the first response
promised, so the client stitches the second stream into the page it already
holds.

tokens expire and are consumed on use, which keeps the parked state from
accumulating in a process that serves pages forever. this is partial
prerendering in the ppr sense: a static shell with a dynamic tail delivered on
demand. it is also, not coincidentally, the fastest possible first byte, since
the first response does zero waiting by construction.

## the wire

rows on the wire are `id:payload`. a handful of tags do the work: model rows
carry tree fragments, import rows carry client module references, error rows
carry digests for boundaries to catch, action rows carry the return value of a
server action, and text rows carry large strings framed by byte length — which
they must be, because a text row can contain anything, including newlines, and
byte-length framing removes the need to escape it.

references are the other half of the protocol. a value reference points at a
row holding a serialized value, a lazy reference points at a row that does not
exist yet or was postponed, an action reference points at a callable, and a
leading dollar sign in a literal string gets doubled so it can never be
confused with a reference. all of this exists because the model has to survive
a round trip through json and come back wired up identically on the other side.

server actions reuse the same machinery with a nice inversion. the function is
serialized as a reference with metadata, so a client component can hold it in
props like any other value. calling it posts the encoded arguments back to the
server, where the action mutates state, the page re-renders, and the reply
carries new rows plus the action's return value on a reserved row. one route
handles every action — the response is just another flight stream whose first
rows the client already expects.

## the client, for real this time

the browser runtime is deliberately the simplest thing that could possibly
work. it holds a queue of rows, decodes them in arrival order, resolves
references as their targets land, and mounts a tree. it exposes exactly three
entry points: navigate, which asks the server for a new page and swaps it in;
action, which posts to a server action and folds the response in; and resume,
which pulls the postponed half of a prerendered page.

client components are module functions that take props and a small toolkit and
return dom. on every update the runtime re-runs the function and swaps the
output. there is no virtual dom, no diffing, no reconciler — the component owns
its subtree and returns its replacement wholesale. for a runtime whose whole
job is to stitch server output into the page, that is not a limitation. it is
the correct amount of machinery.

## what is not here yet

the honest ledger. error boundaries exist in the tree model and emit error
rows, but the client-side recovery story is thin — a failed boundary currently
degrades more than it should. module preloading is minimal, so a client
component's module arrives when its row is decoded rather than when the page
hints it should. binary rows and typed arrays are recognised in the tag space
but not produced or decoded. and there is more of the flight dialect than this
implements, with every gap written down rather than papered over.

the status and differences documents in the docs directory keep the precise
list. earlier versions of them contained claims that were not true of the code,
and those were removed rather than left in place — which is the spirit of the
whole project. a spec implementation you cannot audit is a story, not a spec.

## build and run

    cmake -S . -B build
    cmake --build build
    ./build/Debug/rscpp_server.exe [port]

run from the repository root: the config file and the web assets resolve
against the working directory, and a port argument overrides the config. the
exe path shown is the visual studio generator's; a single-config build places
it directly under the build directory.

`-DRSCPP_BUILD_TESTS=ON` is accepted, but this tree contains no test directory
and the build says so out loud. the docs keep the ledger of what was verified
against the full tree, where this implementation differs from react 19, and
where the architecture notes predate the parking loop.
