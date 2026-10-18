

(function () {
    'use strict';

    const FRAGMENT_TYPE = 60107;
    const REACT_SUSPENSE_TYPE = 'react.suspense';
    const KNOWN_TAGS = '0IETPHA';
    const MAX_ROW_ID_CHARS = 16;
    const MAX_TEXT_LEN_CHARS = 16;
    // A uint64 is 16 hex digits, so that is the width of ids and of lengths.
    // The digit count bounds the *encoding* of a length, never its value:
    // 16 digits reach 2^64-1. These bound the value and the total buffer.
    // Largest real row in this project is ~5.6 KB (/heavy), so ~175x headroom.
    const MAX_TEXT_LEN_BYTES = 1 << 20;   // 1 MiB per T row
    const MAX_BUFFER_BYTES = 4 << 20;     // 4 MiB buffered

    const TEXT_ENCODER = new TextEncoder();
    const TEXT_DECODER = new TextDecoder('utf-8', { fatal: true });
    const COLON = 58;   
    const COMMA = 44;   

    function indexOfByte(bytes, target, from) {
        for (let i = from; i < bytes.length; i++) if (bytes[i] === target) return i;
        return -1;
    }

    const ACTION_RESULT_ROW_ID = 0x9c40000000000000;

    class FlightDecoder {
        constructor() {
            this.reset();
        }

        reset() {
            this.buf = '';
            this.chunks = new Map();     
            this.rawRows = [];           
            this.error = null;           
            this.actionResult = undefined;
            this.postponed = new Set();  

            // `reset()` runs from the constructor, before `listeners` exists.
            // Read-then-replace only matters on a *re*-reset, which supersedes
            // any still-unsettled waits from a newer response.
            const pending = this.listeners;
            this.listeners = new Map();
            if (pending !== undefined && pending.size > 0) {
                for (const entry of pending.values()) {
                    for (const w of entry) w.reject(new Error('superseded by a newer response'));
                }
            }
        }

        get ok() { return this.error === null; }

        feed(data) {
            if (this.error !== null) return;
            // Bound total buffered size before the append. Without this, a
            // declared-but-undelivered length prefix made every feed append
            // forever while `buf` grew with no cap.
            if (this.buf.length + data.length > MAX_BUFFER_BYTES) {
                this._fail('stream buffer limit exceeded');
                return;
            }
            this.buf += data;
            this._drain();
        }

        _fail(msg) {
            this.error = msg;
            this.buf = '';
            for (const [, entry] of this.listeners) {
                for (const w of entry) w.reject(new Error(msg));
            }
            this.listeners.clear();
        }

        _drain() {
            for (;;) {
                const colon = this.buf.indexOf(':');
                if (colon === -1) {
                    if (this.buf.length > MAX_ROW_ID_CHARS) this._fail('row id is too long');
                    return;
                }
                if (colon === 0) { this._fail('row id is empty'); return; }
                if (colon + 1 >= this.buf.length) return;

                // MAX_ROW_ID_CHARS must bound the id on every path. parseInt on
                // a wider string wraps, so two distinct row ids would alias to
                // one Number and collide in the reference table.
                if (colon > MAX_ROW_ID_CHARS) { this._fail('row id is too long'); return; }

                const idStr = this.buf.slice(0, colon);
                if (!/^[0-9a-f]+$/.test(idStr)) { this._fail('invalid hexadecimal row id: ' + idStr); return; }
                const id = parseInt(idStr, 16);
                const tag = this.buf[colon + 1];

                if (tag === 'T') {
                    const bytes = TEXT_ENCODER.encode(this.buf);
                    const colonB = indexOfByte(bytes, COLON, 0);
                    const commaB = indexOfByte(bytes, COMMA, colonB + 1);
                    // The digit guard belongs here, NOT inside `commaB === -1`.
                    // It used to be, so once the comma landed the digit count was
                    // never validated and a megabyte of digits reached parseInt.
                    const digitEnd = commaB === -1 ? bytes.length : commaB;
                    if (digitEnd - (colonB + 2) > MAX_TEXT_LEN_CHARS) {
                        this._fail('text length prefix is too long');
                        return;
                    }
                    if (commaB === -1) return;
                    const lenStr = TEXT_DECODER.decode(bytes.subarray(colonB + 2, commaB));
                    if (!/^[0-9a-f]+$/.test(lenStr)) { this._fail('invalid hexadecimal text length: ' + lenStr); return; }
                    const len = parseInt(lenStr, 16);
                    // The digit count bounds the encoding of len, not len itself.
                    if (len > MAX_TEXT_LEN_BYTES) {
                        this._fail('text length exceeds limit');
                        return;
                    }
                    const bodyB = commaB + 1;
                    if (bytes.length - bodyB < len) return;  
                    this._resolve(id, TEXT_DECODER.decode(bytes.subarray(bodyB, bodyB + len)));
                    this.buf = TEXT_DECODER.decode(bytes.subarray(bodyB + len));
                    continue;
                }

                const nl = this.buf.indexOf('\n', colon);
                if (nl === -1) return;  

                const line = this.buf.slice(0, nl);
                this.buf = this.buf.slice(nl + 1);
                this._handleLine(id, line);
            }
        }

        _handleLine(id, line) {
            this._remember(line);
            const rest = line.slice(line.indexOf(':') + 1);
            if (rest === '') { this._fail('row has no payload'); return; }

            const tag = KNOWN_TAGS.indexOf(rest[0]) !== -1 ? rest[0] : 0;
            const body = tag ? rest.slice(1) : rest;

            if (tag === 'P') {
                this.postponed.add(id);
                return;
            }

            if (tag === 'E') {
                let msg = 'Server error';
                try { msg = JSON.parse(body).message || msg; } catch (e) {  }
                this._resolve(id, { __error: msg });
                return;
            }

            let payload;
            try {
                payload = JSON.parse(body);
            } catch (e) {
                this._fail('row ' + id.toString(16) + ' is not valid JSON');
                return;
            }

            if (tag === 'I') {
                this._resolve(id, {
                    __client: true,
                    module: payload.id,
                    exportName: payload.name || 'default',
                    refId: id
                });
                return;
            }

            if (tag === 'A') {
                this.actionResult = payload;
                this._resolve(id, payload);
                return;
            }
            this._resolve(id, payload);
        }

        getActionResult() {
            return this.actionResult === undefined ? null : this.actionResult;
        }

        _remember(line) {
            this.rawRows.push(line);
            if (this.rawRows.length > 400) this.rawRows.shift();
            window.dispatchEvent(new CustomEvent('rsc:row', { detail: line }));
        }

        _resolve(id, value) {
            this.chunks.set(id, value);
            this.postponed.delete(id);
            const entry = this.listeners.get(id);
            if (entry) {
                this.listeners.delete(id);
                for (const w of entry) w.resolve(value);
            }
        }

        has(id) { return this.chunks.has(id); }

        get(id) { return this.chunks.get(id); }

        when(id) {
            if (this.chunks.has(id)) return Promise.resolve(this.chunks.get(id));
            if (this.error !== null) return Promise.reject(new Error(this.error));
            return new Promise((resolve, reject) => {
                if (!this.listeners.has(id)) this.listeners.set(id, []);
                this.listeners.get(id).push({ resolve, reject });
            });
        }

        async whenOrNull(id) {
            if (this.error !== null) return null;
            try { return await this.when(id); } catch (e) { return null; }
        }
    }

    function isRefString(s) {
        if (typeof s !== 'string' || s.length < 2) return false;
        if (s.startsWith('$$')) return false;  // escaped literal, e.g. "$$L9" -> "$L9"
        if (s === '$undefined' || s === '$Infinity' || s === '$-Infinity' ||
            s === '$NaN' || s === '$-0') return true;
        // `$h` is React 19.3's server-reference prefix ($F before that); both
        // decode to a server action here.
        if (s.startsWith('$L') || s.startsWith('$@') || s.startsWith('$F') ||
            s.startsWith('$h') || s.startsWith('$S') || s.startsWith('$D') ||
            s.startsWith('$n')) return true;
        if (s[0] === '$' && /^[0-9a-f]+$/.test(s.slice(1))) return true;
        return false;
    }

    function resolveRef(s, decoder) {
        if (s === '$undefined') return undefined;
        if (s === '$Infinity') return Infinity;
        if (s === '$-Infinity') return -Infinity;
        if (s === '$NaN') return NaN;
        if (s === '$-0') return -0;
        if (s.startsWith('$S')) return { $$typeof: 'symbol', name: s.slice(2) };
        if (s.startsWith('$D')) return new Date(s.slice(2));
        if (s.startsWith('$n')) return BigInt(s.slice(2));
        if (s.startsWith('$L')) {
            const id = parseInt(s.slice(2), 16);
            const meta = decoder.chunks.get(id);
            if (meta !== undefined) {
                if (meta.__client) return meta;
                return resolveValue(meta, decoder);
            }
            return { __lazy: id };
        }
        if (s.startsWith('$@')) return { __promise: parseInt(s.slice(2), 16) };
        if (s.startsWith('$F')) return { __serverAction: parseInt(s.slice(2), 16) };
        if (s.startsWith('$h')) return { __serverAction: parseInt(s.slice(2), 16) };
        if (s[0] === '$') {
            const id = parseInt(s.slice(1), 16);
            const meta = decoder.chunks.get(id);
            if (meta === undefined) return { __ref: id };
            return resolveValue(meta, decoder);
        }
        return s;
    }

    function resolveValue(v, decoder) {
        if (typeof v === 'string') {
            if (v.startsWith('$$')) return v.slice(1);  // unescape literal
            return isRefString(v) ? resolveRef(v, decoder) : v;
        }
        if (Array.isArray(v)) {
            if (v.length > 0 && v[0] === '$') return resolveElement(v, decoder);
            return v.map((x) => resolveValue(x, decoder));
        }
        if (v && typeof v === 'object') {
            if (v.__resolved) return v;
            const out = {};
            for (const k in v) {
                if (v[k] == null) { out[k] = v[k]; continue; }
                out[k] = resolveValue(v[k], decoder);
            }
            return out;
        }
        return v;
    }

    function resolveType(type, decoder) {
        if (typeof type === 'string') {
            if (isRefString(type)) return resolveRef(type, decoder);
            return type;
        }
        return type;
    }

    // Walks a resolved value and collects the row ids it is still waiting on.
    // These are `$L`, `$@` and value-reference slots whose rows have not
    // arrived yet; a boundary shows its fallback until all of them land.
    function collectPending(v, out) {
        if (v == null || typeof v !== 'object') return;
        if (Array.isArray(v)) {
            for (const x of v) collectPending(x, out);
            return;
        }
        if (v.__lazy !== undefined || v.__promise !== undefined || v.__ref !== undefined) {
            const id = v.__lazy !== undefined ? v.__lazy
                : v.__promise !== undefined ? v.__promise : v.__ref;
            if (!out.includes(id)) out.push(id);
            return;
        }
        for (const k in v) collectPending(v[k], out);
    }

    // Re-resolves a value after its pending rows have arrived: markers become
    // the values their rows carry, recursively, so a slot that resolved to an
    // element containing another pending slot keeps working.
    function reResolveValue(v, decoder) {
        if (typeof v === 'string') return resolveValue(v, decoder);
        if (Array.isArray(v)) {
            if (v.length > 0 && v[0] === '$') {
                return ['$', reResolveValue(v[1], decoder), v[2], reResolveValue(v[3] || {}, decoder)];
            }
            return v.map((x) => reResolveValue(x, decoder));
        }
        if (v && typeof v === 'object') {
            if (v.__client || v.__serverAction !== undefined) return v;
            if (v.__lazy !== undefined || v.__promise !== undefined || v.__ref !== undefined) {
                const id = v.__lazy !== undefined ? v.__lazy
                    : v.__promise !== undefined ? v.__promise : v.__ref;
                const chunk = decoder.get(id);
                if (chunk === undefined) return v;
                if (v.__lazy !== undefined && chunk && chunk.__client) return chunk;
                return reResolveValue(chunk, decoder);
            }
            const out = {};
            for (const k in v) out[k] = reResolveValue(v[k], decoder);
            return out;
        }
        return v;
    }

    // The server encodes Suspense as the "$Sreact.suspense" symbol reference,
    // which resolves to { $$typeof: 'symbol', name: 'react.suspense' }.
    function isSuspenseType(type) {
        return !!type && typeof type === 'object' && type.$$typeof === 'symbol' &&
               type.name === REACT_SUSPENSE_TYPE;
    }

    function resolveElement(arr, decoder) {
        const typeRaw = arr[1];
        const keyRaw = arr[2];
        const propsRaw = arr[3] || {};
        const type = resolveType(typeRaw, decoder);
        const props = resolveValue(propsRaw, decoder);
        let children = props.children;
        if (children == null) children = [];
        else if (!Array.isArray(children)) children = [children];
        return { type, key: keyRaw, props, children };
    }

    function asElementArray(val) {
        if (Array.isArray(val) && val.length && val[0] === '$') return val;
        return ['$', FRAGMENT_TYPE, null, { children: val }];
    }

    const decoder = new FlightDecoder();

    const URL_ATTRS = new Set(['href', 'src', 'srcset', 'action', 'formaction', 'poster', 'cite', 'ping']);
    const SAFE_SCHEMES = new Set(['http:', 'https:', 'mailto:', 'tel:', 'ftp:']);

    function isSafeUrlAttr(name, value) {
        if (!URL_ATTRS.has(name)) return true;
        // Strip every C0 control and space, not just leading ones: browsers
        // ignore those characters *inside* a scheme, so 'java\nscript:' would
        // otherwise dodge the scheme regex below. This must agree with the
        // C++ renderer, which rejects such schemes outright.
        const v = String(value).replace(/[\u0000-\u0020]+/g, '');
        if (v === '') return true;
        if (v.startsWith('#') || v.startsWith('/') || v.startsWith('?')) return true;
        const m = /^([a-zA-Z][a-zA-Z0-9+.-]*):/.exec(v);
        if (!m) return true;  
        return SAFE_SCHEMES.has(m[1].toLowerCase() + ':');
    }

    function applyProps(el, props) {
        for (const k in props) {
            if (k === 'children' || k === 'key' || k === 'ref') continue;
            // `open` is a real HTML attribute (details/dialog); only event
            // handler props start with `on`.
            if (k !== 'open' && k.startsWith('on')) continue;
            if (k === 'dangerouslySetInnerHTML') continue; 
            if (k === 'style') continue;                  
            const v = props[k];
            if (v == null) continue;
            if (typeof v === 'object') continue; 
            if (!isSafeUrlAttr(k, typeof v === 'string' ? v : String(v))) continue;
            if (k === 'className') { el.setAttribute('class', String(v)); continue; }
            if (k === 'htmlFor') { el.setAttribute('for', String(v)); continue; }
            if (typeof v === 'boolean') { if (v) el.setAttribute(k, ''); continue; }
            el.setAttribute(k, String(v));
        }
    }

    async function mountChild(child, parent) {
        if (child == null) return;
        const t = typeof child;
        if (t === 'string' || t === 'number' || t === 'boolean') {
            parent.appendChild(document.createTextNode(String(child)));
            return;
        }
        if (t !== 'object') return;
        if (Array.isArray(child)) {
            const node = resolveElement(child, decoder);
            await mountNode(node, parent);
            return;
        }
        // A slot that is still waiting on a row mounts as soon as its row
        // lands, without holding up the siblings: the anchor span keeps the
        // sibling order while the content is mounted out of tree.
        if (child.__lazy !== undefined || child.__promise !== undefined || child.__ref !== undefined) {
            const id = child.__lazy !== undefined ? child.__lazy
                : child.__promise !== undefined ? child.__promise : child.__ref;
            const isLazy = child.__lazy !== undefined;
            const anchor = document.createElement('span');
            anchor.style.display = 'none';
            parent.appendChild(anchor);
            decoder.when(id).then(async () => {
                const resolved = reResolveValue(child, decoder);
                if (resolved === child) return;  // row landed but carried no value
                await mountChild(resolved, anchor);
                while (anchor.firstChild) parent.insertBefore(anchor.firstChild, anchor);
                anchor.remove();
            }).catch(() => {
                if (isLazy) mountError(parent, 'client reference never arrived');
                else mountError(parent, 'promised value never arrived');
            });
            return;
        }
        if (child.__serverAction !== undefined) return;
        if (child.__postponed !== undefined) return;
        if (child.__error !== undefined) {
            const span = document.createElement('span');
            span.className = 'rsc-error-fallback';
            span.textContent = '[server error] ' + child.__error;
            parent.appendChild(span);
            return;
        }
        await mountNode(child, parent);
    }

    function mountError(parent, message) {
        const span = document.createElement('span');
        span.className = 'rsc-error-fallback';
        span.textContent = message;
        parent.appendChild(span);
    }

    async function mountNode(node, parent) {
        if (node == null) return;

        if (node.type && typeof node.type === 'object' && node.type.__lazy !== undefined) {
            const meta = await decoder.whenOrNull(node.type.__lazy);
            if (meta === null) { mountError(parent, 'client reference never arrived'); return; }
            node = { ...node, type: meta };
        }

        if (node.__lazy !== undefined) { await mountChild(node, parent); return; }
        if (node.__promise !== undefined) { await mountChild(node, parent); return; }
        if (node.__error !== undefined) { await mountChild(node, parent); return; }

        if (node.type === FRAGMENT_TYPE) {
            for (const c of node.children) await mountChild(c, parent);
            return;
        }
        if (isSuspenseType(node.type)) {
            await mountSuspense(node, parent);
            return;
        }
        if (node.type && typeof node.type === 'object') {
            if (node.type.__client) { await mountClient(node, parent); return; }
            if (node.type.__serverAction !== undefined) return;
            if (node.type.__error !== undefined) return;
            if (node.type.$$typeof === 'symbol') return;
        }
        if (typeof node.type === 'number') return;
        if (typeof node.type !== 'string') return;

        const el = document.createElement(node.type);
        applyProps(el, node.props);
        for (const c of node.children) await mountChild(c, el);
        parent.appendChild(el);
    }

    async function mountSuspense(node, parent) {
        const raw = node.props.children;
        const pending = [];
        collectPending(raw, pending);

        if (!pending.length) {
            // Nothing is waiting: no boundary wrapper, no fallback flash.
            if (raw == null) {
                await mountNode(node.props.fallback, parent);
            } else {
                await mountNode(raw, parent);
            }
            return;
        }

        const fb = document.createElement('div');
        fb.className = 'rsc-fallback';
        const ct = document.createElement('div');
        ct.className = 'rsc-content';
        ct.style.display = 'none';
        const container = document.createElement('div');
        container.className = 'rsc-boundary';
        container.appendChild(fb);
        container.appendChild(ct);
        parent.appendChild(container);

        if (node.props.fallback) await mountNode(node.props.fallback, fb);

        // Real React suspends at whatever slot is pending, not at the
        // boundary: only the rows this subtree is actually waiting on gate
        // the swap. Siblings outside the boundary were already mounted.
        Promise.all(pending.map((id) => decoder.when(id).catch(() => null))).then(async () => {
            try {
                const resolved = reResolveValue(raw, decoder);
                fb.style.display = 'none';
                ct.style.display = '';
                await mountNode(resolved, ct);
            } catch (e) {
                mountError(fb, 'boundary failed: ' + (e && e.message ? e.message : e));
            }
        });
    }

    function createComponentController(node, slot, comp) {
        const instance = { hooks: [], cursor: 0 };
        const parent = slot.parentElement;

        function getHook() {
            const i = instance.cursor++;
            if (i >= instance.hooks.length) instance.hooks.push({});
            return instance.hooks[i];
        }

        const api = {
            rsc,
            useState(initial) {
                const h = getHook();
                if (h.state === undefined) h.state = initial;
                const set = (next) => {
                    const v = typeof next === 'function' ? next(h.state) : next;
                    if (Object.is(v, h.state)) return;
                    h.state = v;
                    render();
                };
                return [h.state, set];
            }
        };

        function commit(result) {
            const host = document.createElement('span');
            host.className = 'rsc-client';
            if (result instanceof Node || result instanceof HTMLElement) {
                host.appendChild(result);
            } else {
                host.textContent = String(result == null ? '' : result);
            }
            if (slot.isConnected) {
                slot.replaceChildren(host);
            } else if (parent && parent.isConnected) {
                const existing = parent.querySelector('.rsc-client');
                if (existing) existing.replaceWith(host); else parent.appendChild(host);
            } else {
                return;
            }
        }

        function render() {
            instance.cursor = 0;
            let result;
            try {
                result = comp(node.props || {}, api);
            } catch (e) {
                if (slot.isConnected) slot.replaceChildren(document.createTextNode('[client component error] ' + e.message));
                console.error('[rsc] client component failed:', e);
                return;
            }
            if (result && typeof result.then === 'function') {
                result.then(commit).catch((e) => {
                    if (slot.isConnected) slot.replaceChildren(document.createTextNode('[client component error] ' + e.message));
                    console.error('[rsc] client component failed:', e);
                });
            } else {
                commit(result);
            }
        }

        return render;
    }

    const moduleCache = new Map();
    async function mountClient(node, parent) {
        const slot = document.createElement('span');
        slot.className = 'rsc-client';
        slot.textContent = '[loading client component…]';
        parent.appendChild(slot);
        try {
            let mod = moduleCache.get(node.type.module);
            if (!mod) { mod = await import(node.type.module); moduleCache.set(node.type.module, mod); }
            const comp = mod[node.type.exportName || 'default'];
            if (typeof comp !== 'function') {
                throw new Error('export "' + (node.type.exportName || 'default') + '" not found in ' + node.type.module);
            }
            const render = createComponentController(node, slot, comp);
            render();
        } catch (e) {
            slot.replaceChildren(document.createTextNode('[client component error] ' + e.message));
            console.error('[rsc] client component failed:', e.message);
        }
    }

    function childrenOf(node) {
        if (!node) return [];
        const out = [];
        const visit = (v) => {
            if (v == null || typeof v !== 'object') return;
            if (Array.isArray(v)) {
                if (v.length && v[0] === '$') out.push(resolveElement(v, decoder));
                else for (const x of v) visit(x);
                return;
            }
            if (v.__lazy !== undefined || v.__promise !== undefined) return;  
            out.push(v);
        };
        for (const c of (node.children || [])) visit(c);
        return out;
    }

    // Hidden `S:` segment sources can hold leftover copies of a client
    // component's template (a boundary that resolved while the root was
    // pending, or a retried root that inlined content whose source segment
    // was never consumed). Only templates in visible markup belong to the
    // tree being hydrated.
    function findClientTemplate(refId) {
        const all = document.querySelectorAll('template[data-rsc-client="' + refId + '"]');
        for (const t of all) {
            if (t.isConnected && !t.closest('[hidden]')) return t;
        }
        return null;
    }

    function hydrateTree(node) {
        if (!node) return;
        if (node.type && typeof node.type === 'object' && node.type.__client) {
            const t = node.type;
            const slot = findClientTemplate(t.refId);
            if (slot && slot.parentNode) {
                const placeholder = document.createElement('span');
                slot.parentNode.replaceChild(placeholder, slot);
                mountClient(node, placeholder);
            }
            return;
        }
        if (node.type === FRAGMENT_TYPE || isSuspenseType(node.type) ||
            typeof node.type === 'string') {
            for (const c of childrenOf(node)) hydrateTree(c);
        }
    }

    async function consumeStream(response) {
        if (!response.ok) {
            const detail = await response.text().catch(() => '');
            throw new Error('HTTP ' + response.status +
                (detail ? ': ' + detail.slice(0, 200) : ''));
        }
        if (!response.body) {
            throw new Error('response has no readable body');
        }

        const reader = response.body.getReader();
        const td = new TextDecoder('utf-8', { fatal: false });
        try {
            for (;;) {
                const { done, value } = await reader.read();
                if (done) break;
                decoder.feed(td.decode(value, { stream: true }));
            }
            const tail = td.decode();
            if (tail) decoder.feed(tail);
        } catch (e) {
            reader.cancel().catch(() => {});
            throw e;
        }

        if (decoder.error !== null) {
            throw new Error('malformed Flight stream: ' + decoder.error);
        }
        if (!decoder.has(0)) {
            throw new Error('Flight stream ended without a root row');
        }
    }

    function reportFatal(err, context) {
        console.error('[rsc] ' + context + ':', err);
        const root = document.getElementById('root');
        if (!root) return;
        const box = document.createElement('div');
        box.className = 'rsc-error-fallback';
        const h = document.createElement('h1');
        h.textContent = 'Something went wrong';
        const p = document.createElement('p');
        p.textContent = context + ': ' + (err && err.message ? err.message : String(err));
        box.appendChild(h);
        box.appendChild(p);
        root.replaceChildren(box);
        window.dispatchEvent(new CustomEvent('rsc:error', { detail: { context, error: err } }));
    }

    async function mountRoot() {
        const rootPayload = await decoder.when(0);
        if (rootPayload == null) throw new Error('root row carried no content');
        if (rootPayload.__error !== undefined) throw new Error(rootPayload.__error);
        const rootEl = resolveElement(rootPayload, decoder);
        const container = document.getElementById('root') || document.body;
        container.replaceChildren();
        await mountNode(rootEl, container);
    }

    let navEpoch = 0;

    async function load(path, { historyMode = 'push', context = 'navigate' } = {}) {
        const epoch = ++navEpoch;
        // Scripts from the page being navigated away from may still be
        // streaming; bumping the generation makes their $RC/$R no-ops so they
        // cannot feed stale rows into the decoder for the new page.
        window.__RSC_GEN__ = (window.__RSC_GEN__ || 0) + 1;
        const url = new URL(path, location.origin);
        const flightPath = '/rsc' + url.pathname + url.search;

        decoder.reset();
        try {
            const resp = await fetch(flightPath, {
                headers: { Accept: 'text/x-component' },
                credentials: 'same-origin'
            });
            if (epoch !== navEpoch) return;  
            await consumeStream(resp);
            if (epoch !== navEpoch) return;
            await mountRoot();
        } catch (e) {
            if (epoch !== navEpoch) return;
            reportFatal(e, context);
            return;
        }

        if (historyMode === 'push') {
            window.history.pushState({ rsc: true }, '', url.pathname + url.search);
        } else {
            window.history.replaceState({ rsc: true }, '', url.pathname + url.search);
        }
        window.scrollTo(0, 0);
        window.dispatchEvent(new CustomEvent('rsc:nav', { detail: { path } }));
    }

    function navigate(path) {
        return load(path, { historyMode: 'push', context: 'navigate' });
    }

    async function runAction(actionId, payload) {
        const epoch = ++navEpoch;
        window.__RSC_GEN__ = (window.__RSC_GEN__ || 0) + 1;
        decoder.reset();
        try {
            const resp = await fetch('/rsc/action', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json', Accept: 'text/x-component' },
                credentials: 'same-origin',
                body: JSON.stringify({
                    action_id: actionId,
                    payload: payload || {},
                    path: location.pathname
                })
            });
            if (!resp.ok) {
                await consumeStream(resp).catch(() => {});
                const err = decoder.get(0);
                const message = err && err.__error ? err.__error : ('action failed: HTTP ' + resp.status);
                reportFatal(new Error(message), 'action "' + actionId + '"');
                return null;
            }
            await consumeStream(resp);
            if (epoch !== navEpoch) return null;  
            await mountRoot();
            window.dispatchEvent(new CustomEvent('rsc:action', {
                detail: { actionId, result: decoder.getActionResult() }
            }));
            return decoder.getActionResult();
        } catch (e) {
            if (epoch === navEpoch) reportFatal(e, 'action "' + actionId + '"');
            return null;
        }
    }

    function isSameOriginLink(href) {
        if (!href) return false;
        if (href.startsWith('#')) return false;
        let url;
        try { url = new URL(href, location.href); } catch (e) { return false; }
        if (url.origin !== location.origin) return false;
        return url.protocol === 'http:' || url.protocol === 'https:';
    }

    function install() {
        document.addEventListener('click', (e) => {
            if (e.defaultPrevented || e.button !== 0 || e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
            const a = e.target && e.target.closest ? e.target.closest('a[href]') : null;
            if (!a) return;
            if (a.target && a.target !== '_self') return;
            if (a.hasAttribute('download')) return;
            if (a.getAttribute('data-no-rsc') != null) return;
            if (!isSameOriginLink(a.getAttribute('href'))) return;
            e.preventDefault();
            navigate(a.getAttribute('href'));
        });

        document.addEventListener('submit', (e) => {
            const form = e.target;
            if (!(form instanceof HTMLFormElement)) return;
            if (form.hasAttribute('data-no-rsc')) return;
            const actionId = form.getAttribute('action');
            if (!actionId || actionId.startsWith('#')) return;
            e.preventDefault();
            const payload = {};
            for (const [k, v] of new FormData(form).entries()) payload[k] = v;
            runAction(actionId, payload);
        }, true);

        window.addEventListener('popstate', () => {
            load(location.pathname + location.search,
                 { historyMode: 'replace', context: 'popstate' });
        });
    }

    function drainQueuedRows() {
        const queued = window.__RSC_ROWS__;
        if (!queued || !queued.length) return;
        const rows = queued.splice(0, queued.length);
        for (const row of rows) decoder.feed(row);
    }

    function postponedToken() {
        const meta = document.querySelector('meta[name="rsc-postponed"]');
        return meta && meta.content ? meta.content : null;
    }

    // Patches every still-unpatched placeholder for a block id. Ids are minted
    // by the server in one pass and reused across a root retry, so the same
    // block id can legitimately appear more than once in the document (an
    // orphaned hidden segment plus the live retried content): patch them all.
    function patchBoundaryPlaceholders(id, source) {
        const dests = document.querySelectorAll('[id="B:' + id + '"][data-rsc-boundary]');
        for (const d of dests) {
            while (d.firstChild) d.removeChild(d.firstChild);
            for (const n of source.childNodes) d.appendChild(n.cloneNode(true));
            d.removeAttribute('data-rsc-boundary');
            d.classList.remove('rsc-boundary');
        }
        return dests.length;
    }

    // Segments that completed before their placeholder existed (a boundary
    // resolving while the root is still pending) wait here and are matched up
    // as soon as the parent content lands in the DOM.
    function flushPendingSegments() {
        const pending = window.__RSC_SEG_PENDING__;
        if (pending) {
            for (const id of Object.keys(pending)) {
                const y = pending[id];
                if (!y || !y.isConnected) { delete pending[id]; continue; }
                if (patchBoundaryPlaceholders(id, y) > 0) {
                    // Consumed: drop a connected hidden source so it cannot
                    // shadow live templates or duplicate markup.
                    if (y.parentNode) y.parentNode.removeChild(y);
                    delete pending[id];
                }
            }
        }
        applyChunkErrors();
        tryHydrateSegments();
    }
    window.__RSC_FLUSH_SEGMENTS__ = flushPendingSegments;

    // A failed boundary never gets a segment: the server replaces its row with
    // an `E` row. On the streaming/boot path nothing else swaps the fallback,
    // so patch every still-unpatched placeholder an error row belongs to.
    function applyChunkErrors() {
        for (const [id, chunk] of decoder.chunks) {
            if (!chunk || chunk.__error === undefined) continue;
            const dests = document.querySelectorAll('[id="B:' + id.toString(16) + '"][data-rsc-boundary]');
            for (const d of dests) {
                d.removeAttribute('data-rsc-boundary');
                d.classList.remove('rsc-boundary');
                d.replaceChildren();
                const span = document.createElement('span');
                span.className = 'rsc-error-fallback';
                span.textContent = '[server error] ' + chunk.__error;
                d.appendChild(span);
            }
        }
    }

    // Boundary ids whose resumed content may carry client components. A
    // segment's markup can land after its row (parked in the pending registry)
    // or appear inside a parent segment patched later, so hydration is retried
    // every time segments are flushed into the document.
    const resumeSegmentIds = new Set();

    function tryHydrateSegments() {
        if (!resumeSegmentIds.size) return;
        for (const id of [...resumeSegmentIds]) {
            const chunk = decoder.get(parseInt(id, 16));
            if (chunk === undefined) continue;
            let element;
            try {
                element = resolveElement(asElementArray(chunk), decoder);
            } catch (e) { resumeSegmentIds.delete(id); continue; }

            const refs = [];
            (function collect(n) {
                if (!n || typeof n !== 'object') return;
                if (n.type && typeof n.type === 'object' && n.type.__client) {
                    refs.push(n.type.refId);
                    return;
                }
                if (n.type === FRAGMENT_TYPE || isSuspenseType(n.type) ||
                    typeof n.type === 'string') {
                    for (const c of childrenOf(n)) collect(c);
                }
            })(element);

            if (!refs.length) { resumeSegmentIds.delete(id); continue; }
            const ready = refs.every((r) => findClientTemplate(r) !== null);
            if (!ready) continue;  // still parked; the next flush retries
            try {
                hydrateTree(element);
            } catch (e) { /* stays server-rendered */ }
            resumeSegmentIds.delete(id);
        }
    }

    // Applies a resume stream to the document. The stream carries row scripts
    // (the rows the shell was waiting on) and segment templates keyed by the
    // ids the pre-render already reserved. Nothing in the shell is re-rendered.
    async function applyResumeStream(html) {
        const doc = new DOMParser().parseFromString(html, 'text/html');

        for (const s of doc.querySelectorAll('script[data-rsc-row]')) {
            let row;
            try { row = JSON.parse(s.textContent); } catch (e) { continue; }
            decoder.feed(row);
        }
        if (decoder.error !== null) throw new Error('malformed resume stream: ' + decoder.error);
        drainQueuedRows();

        const errorIds = new Set();
        for (const [id, chunk] of decoder.chunks) {
            if (chunk && chunk.__error !== undefined) errorIds.add(id.toString(16));
        }

        const segments = [];
        for (const t of doc.querySelectorAll('template[data-rsc-segment]')) {
            const id = t.getAttribute('data-rsc-boundary-id');
            const holder = document.createElement('div');
            holder.appendChild(document.importNode(t.content, true));
            // An empty segment for an error row carries no content: keep the
            // placeholder so applyChunkErrors can render `[server error]` into
            // it instead of consuming it with nothing.
            if (!holder.firstChild && errorIds.has(id)) continue;
            segments.push(id);
            if (patchBoundaryPlaceholders(id, holder) === 0) {
                (window.__RSC_SEG_PENDING__ = window.__RSC_SEG_PENDING__ || {})[id] = holder;
            }
        }
        flushPendingSegments();
        return segments;
    }

    // PPR resumption: fetch the continuation for the shell's postponement and
    // splice it in. The pre-render never waited on the parked data, so this is
    // where "slow" content actually arrives.
    async function resumeNow() {
        const token = postponedToken();
        if (!token) return false;
        try {
            const resp = await fetch('/rsc/resume/' + encodeURIComponent(token), {
                headers: { Accept: 'text/html' },
                credentials: 'same-origin'
            });
            if (!resp.ok) throw new Error('HTTP ' + resp.status);
            const segments = await applyResumeStream(await resp.text());

            // The root row may live in the shell (root resolved during the
            // pre-render) or arrive in this stream (root was postponed too).
            if (!decoder.has(0)) throw new Error('resume stream did not deliver a root row');

            // Mount client components that arrived inside the new segments.
            // Segments whose markup is still parked self-retry on flush.
            for (const id of segments) resumeSegmentIds.add(id);
            tryHydrateSegments();
            window.dispatchEvent(new CustomEvent('rsc:resumed', { detail: { boundaries: segments } }));
            return true;
        } catch (e) {
            window.dispatchEvent(new CustomEvent('rsc:resume-error', { detail: { error: e } }));
            return false;
        }
    }

    // The only public client surface. `action` is called directly by client
    // components; `navigate` is wired to link clicks and popstate below.
    const rsc = {
        navigate,
        action: runAction,
        resume: resumeNow,
        version: 'rscpp-react19-flight'
    };
    window.rsc = rsc;

    function boot() {
        install();

        const dataEl = document.getElementById('__RSC_DATA__');
        if (dataEl) {
            decoder.reset();
            try {
                decoder.feed(JSON.parse(dataEl.textContent));
            } catch (e) {
                reportFatal(e, 'parse embedded Flight payload');
                return;
            }
            if (decoder.error !== null) {
                reportFatal(new Error(decoder.error), 'embedded Flight payload');
                return;
            }

            const token = postponedToken();
            window.__RSC_ROWS_PUSH__ = () => {
                drainQueuedRows();
                if (decoder.error !== null) {
                    reportFatal(new Error(decoder.error), 'streamed boundary rows');
                    return;
                }
                // Boundary rows can reveal client components; re-hydrating is
                // idempotent (consumed templates are already replaced).
                if (decoder.has(0)) {
                    const chunk = decoder.chunks.get(0);
                    // An error root has no element to hydrate: applyChunkErrors
                    // already rendered the fallback into its boundary.
                    if (chunk && chunk.__error === undefined) {
                        try {
                            hydrateTree(resolveElement(chunk, decoder));
                        } catch (e) { /* non-fatal */ }
                    }
                }
                flushPendingSegments();
            };
            drainQueuedRows();
            applyChunkErrors();
            if (decoder.error !== null) {
                // Rows queued by $RC before client.js loaded can be malformed
                // too; this path used to return silently and leave a blank page.
                reportFatal(new Error(decoder.error), 'streamed boundary rows');
                return;
            }

            if (!decoder.has(0)) {
                if (!token) {
                    reportFatal(new Error('no root row in the embedded payload'), 'hydrate');
                    return;
                }
                // The entire content was postponed; resumption carries the root.
                resumeNow();
                return;
            }

            const rootChunk = decoder.chunks.get(0);
            if (rootChunk == null) {
                reportFatal(new Error('root row carried no content'), 'hydrate');
                return;
            }
            if (rootChunk.__error !== undefined) {
                // The root render failed: applyChunkErrors turns the root
                // placeholder into an error fallback when one exists; if the
                // shell carried no placeholder, render the error into #root so
                // the page is never silently blank.
                const container = document.getElementById('root') || document.body;
                if (!container.querySelector('.rsc-error-fallback')) {
                    container.replaceChildren();
                    const span = document.createElement('span');
                    span.className = 'rsc-error-fallback';
                    span.textContent = '[server error] ' + rootChunk.__error;
                    container.appendChild(span);
                }
                window.dispatchEvent(new CustomEvent('rsc:hydrated', { detail: { rootId: 'root', error: rootChunk.__error } }));
                return;
            }
            try {
                hydrateTree(resolveElement(rootChunk, decoder));
            } catch (e) {
                reportFatal(e, 'hydrate');
                return;
            }
            window.dispatchEvent(new CustomEvent('rsc:hydrated', { detail: { rootId: 'root' } }));

            // A postponed shell hydrates instantly and then resumes: fallbacks
            // are visible from the first paint, slow data arrives after.
            if (token) resumeNow();
            return;
        }

        navigate(location.pathname + location.search);
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', boot);
    } else {
        boot();
    }
})();
