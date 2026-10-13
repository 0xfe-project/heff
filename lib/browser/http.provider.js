// The page's side of browser.http: a request sent by `fetch`.  A
// `Request` arrives as a record, `{ url, method, headers, body, timeout }`:
// its strings bytes (`$toJs` makes them text), its headers a list of
// pairs, its body a Uint8Array, its timeout a BigInt.  The answer is
// given to `k` when `fetch` is done -- the response read whole -- as
// `Right` a `Response` or `Left` an `HttpError`, which is none of a
// status: a 404 is a response.
export const $host = "page";
export const http = (world) => {
  const nil = { $: "Nil", a: [] };
  const pairs = (l) => { const r = []; for (; l.$ === "Cons"; l = l.a[1]) r.push([$toJs(l.a[0].a[0]), $toJs(l.a[0].a[1])]); return r; };
  const list = (hs) => { let l = nil; for (let i = hs.length - 1; i >= 0; i--) l = { $: "Cons", a: [{ $: "MkPair", a: [$fromJs(hs[i][0]), $fromJs(hs[i][1])] }, l] }; return l; };
  return (p, k) => {
    const a = p.a;
    switch (p.$) {
      case "Send": {
        const q = a[0], init = { method: $toJs(q.method), headers: pairs(q.headers) };
        if (q.body.length > 0) init.body = q.body;
        const ms = Number(q.timeout);
        let timer = null;
        if (ms > 0 && typeof AbortController === "function") {
          const c = new AbortController();
          init.signal = c.signal;
          timer = setTimeout(() => c.abort(), ms);
        }
        const done = () => { if (timer !== null) clearTimeout(timer); };
        fetch($toJs(q.url), init)
          .then((r) => r.arrayBuffer().then((b) => ({ status: BigInt(r.status), headers: list([...r.headers]), body: new Uint8Array(b) })))
          .then((res) => { done(); return { $: "Right", a: [res] }; },
                (e) => { done(); return { $: "Left", a: [{ message: $fromJs(String(e && e.message || e)) }] }; })
          .then((r) => k(a[1](r)));
        return;
      }
      default: throw new Error("heff: not an operation of http: " + (p && p.$));
    }
  };
};
