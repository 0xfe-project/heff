#!/usr/bin/env node
// The other side of the backends' network tests, by Node.js: an HTTP
// implementation of its own to hold heff's client and server to, which
// an integration test, given no network of its own, runs as a command.
//
//   node test/peers.mjs ports N     N ports nothing listens on, a line each
//   node test/peers.mjs routes      an HTTP server of the client's tests;
//                                   says `PORT REFUSED`, ends with its input
//   node test/peers.mjs slow MS     an HTTP server answering each request
//                                   after MS; says `PORT`, and, its input
//                                   ended, `most N`: how many it had in
//                                   hand at once at most
//   node test/peers.mjs client PORT a server's client: requests by
//                                   node:http, bytes written to sockets,
//                                   curl, then /stop; says what each got,
//                                   a line of JSON, `{ what, got }`
//   node test/peers.mjs burst PORT M N
//                                   a server's clients: M one after
//                                   another, then N, their connections
//                                   opened at once, a request each, then
//                                   /stop; says how many of the N were
//                                   answered
//   node test/peers.mjs idle PORT M N
//                                   a server's clients: M one after
//                                   another, then N connections opened
//                                   one after another and held open,
//                                   silent, then a request on each, then
//                                   /stop; says how many of the N were
//                                   reset before they were made (each
//                                   tried again), and how many answered
//   node test/peers.mjs boc BIN IDLE N
//                                   std.http.bocserver's server, BIN
//                                   PORT IDLE, started and driven: N
//                                   clients holding their connections
//                                   open, asked twice each; `/slow`; a
//                                   silent client; SIGTERM with a
//                                   request in flight; says what each
//                                   came to, and what the server said
import { spawn, spawnSync } from "node:child_process";
import { Agent, createServer, request } from "node:http";
import { connect } from "node:net";

const [mode, arg] = process.argv.slice(2);
const listening = (s) => new Promise((ok) => s.listen(0, "127.0.0.1", () => ok(s.address().port)));
// (`n` ports nothing listens on, as the system picked them: each held
// until all are picked, so that they are not the same)
const freePorts = async (n) => {
  const held = [];
  for (let i = 0; i < n; i++) { const s = createServer(); await listening(s); held.push(s); }
  const ports = held.map((s) => s.address().port);
  await Promise.all(held.map((s) => new Promise((ok) => s.close(ok))));
  return ports;
};
const sum = (b) => b.reduce((s, c) => s + c, 0);
// (a connection to `port` once it is made, or the error it failed by;
// one reset before it is made tried again, a little later: so macOS
// answers a connection past the listener's queue -- at most
// `kern.ipc.somaxconn` long, 128 by default -- where Linux drops it and
// the client's system tries again by itself)
const reached = (port, tries = 100) => new Promise((ok) => {
  const s = connect({ host: "127.0.0.1", port });
  const failed = (e) => {
    s.destroy();
    if (e.code === "ECONNRESET" && tries > 0) setTimeout(() => reached(port, tries - 1).then(ok), 50);
    else ok(e);
  };
  s.once("error", failed);
  s.once("connect", () => { s.off("error", failed); ok(s); });
});
// (until the input ends)
const inputEnded = () => new Promise((ok) => { process.stdin.on("data", () => {}); process.stdin.on("end", ok); process.stdin.resume(); });

if (mode === "ports") console.log((await freePorts(Number(arg))).join("\n"));

if (mode === "routes") {
  const big = Buffer.from(Array.from({ length: 1 << 18 }, (_, i) => (i * 7) % 251));
  const server = createServer((q, r) => {
    const say = (status, body, headers = {}) => { r.writeHead(status, headers); r.end(body); };
    switch (q.url) {
      case "/item": return say(200, "item 3\n".repeat(4));
      // (what it was sent, said back: its method, a header of the
      // test's, its type, how long its body, whether it waits to go on;
      // and the body)
      case "/echo": {
        const parts = [];
        q.on("data", (c) => parts.push(c));
        return q.on("end", () => {
          const body = Buffer.concat(parts);
          say(200, body, {
            "X-Method": q.method, "X-Heff": q.headers["x-heff"] ?? "none", "X-Type": q.headers["content-type"] ?? "none",
            "X-Length": String(body.length), "X-Expect": q.headers.expect ?? "none",
          });
        });
      }
      case "/nope": return say(404, "not here");
      case "/fails": return say(500, "broken");
      case "/moved": return say(302, "moving", { Location: "/item" });
      case "/loop": return say(302, "", { Location: "/loop" });
      case "/slow": return setTimeout(() => say(200, "late"), 500);
      case "/big": return say(200, big, { "Content-Length": big.length });
      case "/chunked": r.write("one,"); r.write("two,"); return r.end("three");
      case "/short": r.writeHead(200, { "Content-Length": 100 }); r.write("cut"); return setTimeout(() => r.socket.destroy(), 50);
      default: return say(500);
    }
  });
  const port = await listening(server);
  const [refused] = await freePorts(1);
  console.log(`${port} ${refused}`);
  await inputEnded();
  server.closeAllConnections();
  server.close();
}

if (mode === "slow") {
  let inHand = 0, most = 0;
  const server = createServer((q, r) => {
    const i = q.url.replace(/^\/item\//, "");
    most = Math.max(most, ++inHand);
    setTimeout(() => { inHand--; r.writeHead(200, { "Content-Type": "text/plain" }); r.end(`item ${i}\n`); }, Number(arg));
  });
  console.log(await listening(server));
  await inputEnded();
  console.log(`most ${most}`);
  server.closeAllConnections();
  server.close();
}

if (mode === "client") {
  const port = Number(arg);
  const said = (what, got) => console.log(JSON.stringify({ what, got }));
  const bytesOf = (n) => Buffer.from(Array.from({ length: 256 * n }, (_, i) => i % 256));
  const big = Buffer.concat(Array.from({ length: 4096 }, () => bytesOf(4)));
  // (bytes written to a connection, in parts -- each after a pause, or
  // after what it waits for has come -- and all that came back, until the
  // server closed it)
  const raw = (parts) => new Promise((ok) => {
    const s = connect({ host: "127.0.0.1", port });
    let got = "", next = 0;
    const timer = setTimeout(() => { s.destroy(); ok(`${got}(no end)`); }, 10000);
    const send = () => {
      if (next >= parts.length) return;
      const [after, text] = parts[next];
      if (typeof after === "number") { next++; setTimeout(() => { s.write(text); send(); }, after); }
      else if (got.includes(after)) { next++; s.write(text); send(); }
    };
    s.setEncoding("latin1");
    s.on("connect", send);
    s.on("data", (d) => { got += d; send(); });
    s.on("error", (e) => { got += `(${e.code})`; });
    s.on("close", () => { clearTimeout(timer); ok(got); });
  });
  // (the server up: connected to, the connection closed at once)
  const up = async () => {
    for (let i = 0; i < 600; i++) {
      if (await new Promise((ok) => {
        const s = connect({ host: "127.0.0.1", port });
        s.on("connect", () => { s.destroy(); ok(true); });
        s.on("error", () => ok(false));
      })) return true;
      await new Promise((ok) => setTimeout(ok, 50));
    }
    return false;
  };
  if (!(await up())) { said("up", "not listening"); process.exit(0); }
  const agent = new Agent({ keepAlive: true, maxSockets: 1 });
  // (a request by node:http, on the connection kept, or `via` another)
  const ask = (method, path, body, headers = {}, via = agent) => new Promise((ok) => {
    const q = request({ host: "127.0.0.1", port, method, path, agent: via, headers }, (r) => {
      const parts = [];
      r.on("data", (c) => parts.push(c));
      r.on("end", () => ok({ status: r.statusCode, headers: r.headers, body: Buffer.concat(parts), reused: q.reusedSocket }));
    });
    q.on("error", (e) => ok({ status: e.code, headers: {}, body: Buffer.alloc(0), reused: false }));
    q.end(body);
  });
  const posted = Buffer.from(Array.from({ length: 300000 }, (_, i) => (i * 13) % 256));
  let r = await ask("GET", "/hello");
  said("get", `${r.status} ${r.headers["content-length"]} ${r.body} ${r.reused}`);
  r = await ask("POST", "/echo", posted, { "X-Heff": "node" });
  said("post", `${r.status} ${r.headers["x-method"]} ${r.headers["x-version"]} ${r.headers["x-peer"]} ${r.headers["x-heff"]} ${r.body.equals(posted)} ${r.reused}`);
  r = await ask("HEAD", "/hello");
  said("head", `${r.status} ${r.headers["content-length"]} ${r.body.length} ${r.reused}`);
  r = await ask("GET", "/nope");
  said("404", `${r.status} ${r.body} ${r.reused}`);
  r = await ask("GET", "/big");
  said("big", `${r.status} ${r.body.equals(big)} ${r.reused}`);
  // (the connection kept, closed: the server, one connection at a time,
  // goes on to the next)
  agent.destroy();
  for (const [what, parts] of [
    ["one after another, in one write",
      [[0, "GET /hello HTTP/1.1\r\nHost: x\r\n\r\nGET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"]]],
    ["a head in parts", [[0, "GET /hel"], [50, "lo HTTP/1.1\r\nHost: x\r\nConnection: close\r"], [50, "\n\r\n"]]],
    ["HTTP/1.0", [[0, "GET /hello HTTP/1.0\r\n\r\n"]]],
    ["HTTP/1.0, kept alive", [[0, "GET /hello HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET /hello HTTP/1.0\r\n\r\n"]]],
    ["Expect: 100-continue",
      [[0, "POST /echo HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n"], ["100 Continue\r\n\r\n", "hello"]]],
    ["a chunked body", [[0, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"]]],
    // (refused, the client sending on after the response: drained, not
    // reset, so that it reads the response)
    ["a body too long, sent on",
      [[0, "POST /echo HTTP/1.1\r\nContent-Length: 100000000\r\n\r\n" + "x".repeat(32768)], ["Content Too Large\n", "y".repeat(524288)]]],
    ["a chunked body, sent on",
      [[0, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n8000\r\n" + "x".repeat(32768)], ["Not Implemented\n", "y".repeat(524288)]]],
    ["no request", [[0, "NONSENSE\r\n\r\n"]]],
  ]) said(what, await raw(parts));
  if (spawnSync("curl", ["--version"]).status === 0) {
    const at = `http://127.0.0.1:${port}`;
    const c = spawnSync("curl", ["-sS", "--http1.1", "-o", "/dev/null", "-o", "/dev/null", "-w", "%{http_code} %{num_connects} %{size_download}\n",
      `${at}/hello`, `${at}/big`], { encoding: "utf8", timeout: 30000 });
    said("curl, two requests on one connection", `${c.stdout}${c.stderr}`);
  }
  const stop = await ask("GET", "/stop", undefined, {}, false);
  said("stop", `${stop.status} ${stop.body}`);
}

if (mode === "burst") {
  const port = Number(arg), before = Number(process.argv[4]), n = Number(process.argv[5]);
  // (a request on a connection of its own, closed after: what came back,
  // or the error)
  const got = async (target) => {
    const s = await reached(port);
    if (s instanceof Error) return `(${s.code})`;
    return new Promise((ok) => {
      let back = "";
      const timer = setTimeout(() => { s.destroy(); ok(`${back}(no end)`); }, 20000);
      s.setEncoding("latin1");
      s.on("data", (d) => { back += d; });
      s.on("error", (e) => { back += `(${e.code})`; });
      s.on("close", () => { clearTimeout(timer); ok(back); });
      s.write(`GET ${target} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n`);
    });
  };
  // (the server up: a connection made, closed at once)
  for (let i = 0; i < 600 && !(await new Promise((ok) => {
    const s = connect({ host: "127.0.0.1", port });
    s.on("connect", () => { s.destroy(); ok(true); });
    s.on("error", () => ok(false));
  })); i++) await new Promise((ok) => setTimeout(ok, 50));
  for (let i = 0; i < before; i++) await got(`/before/${i}`);
  const all = await Promise.all(Array.from({ length: n }, (_, i) => got(`/${i}`)));
  const answered = all.filter((b, i) => b.startsWith("HTTP/1.1 200 ") && b.endsWith(`\r\n\r\n/${i}`)).length;
  console.log(`answered ${answered} of ${n}`);
  await got("/stop");
}

if (mode === "idle") {
  const port = Number(arg), before = Number(process.argv[4]), n = Number(process.argv[5]);
  // (a request on the connection, closed after: what came back)
  const asked = (s, target) => new Promise((ok) => {
    let back = "";
    const timer = setTimeout(() => { s.destroy(); ok(`${back}(no end)`); }, 20000);
    s.setEncoding("latin1");
    s.on("data", (d) => { back += d; });
    s.on("error", (e) => { back += `(${e.code})`; });
    s.on("close", () => { clearTimeout(timer); ok(back); });
    s.write(`GET ${target} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n`);
  });
  const got = async (target) => {
    const s = await reached(port);
    return s instanceof Error ? `(${s.code})` : asked(s, target);
  };
  for (let i = 0; i < 600 && !(await new Promise((ok) => {
    const s = connect({ host: "127.0.0.1", port });
    s.on("connect", () => { s.destroy(); ok(true); });
    s.on("error", () => ok(false));
  })); i++) await new Promise((ok) => setTimeout(ok, 50));
  for (let i = 0; i < before; i++) await got(`/before/${i}`);
  // (each made before the next is begun: a reset counted, and tried
  // again, as `reached` does)
  let resets = 0;
  const made = (tries = 100) => new Promise((ok) => {
    const s = connect({ host: "127.0.0.1", port });
    const failed = (e) => {
      s.destroy();
      if (e.code === "ECONNRESET" && tries > 0) { resets++; setTimeout(() => made(tries - 1).then(ok), 50); }
      else ok(e);
    };
    s.once("error", failed);
    s.once("connect", () => { s.off("error", failed); ok(s); });
  });
  const held = [];
  for (let i = 0; i < n; i++) held.push(await made());
  await new Promise((ok) => setTimeout(ok, 100));
  const all = await Promise.all(held.map((s, i) => s instanceof Error ? `(${s.code})` : asked(s, `/${i}`)));
  const answered = all.filter((b, i) => b.startsWith("HTTP/1.1 200 ") && b.endsWith(`\r\n\r\n/${i}`)).length;
  console.log(`reset before they were made: ${resets} of ${n}`);
  console.log(`answered ${answered} of ${n}`);
  await got("/stop");
}

if (mode === "boc") {
  const bin = arg, idle = Number(process.argv[4]), n = Number(process.argv[5]);
  const [port] = await freePorts(1);
  const server = spawn(bin, [String(port), String(idle)], { stdio: ["ignore", "pipe", "pipe"] });
  let said = "", saidErr = "";
  server.stdout.setEncoding("utf8").on("data", (d) => { said += d; });
  server.stderr.setEncoding("utf8").on("data", (d) => { saidErr += d; });
  const exited = new Promise((ok) => server.on("exit", (code, signal) => ok({ code, signal })));
  // (a connection kept: `ask(target)`, one request on it, its response's
  // body once it is all come, or null where the connection ended first;
  // `ended`, once the other side closed it)
  const opened = async () => {
    const s = await reached(port);
    if (s instanceof Error) return null;
    s.setEncoding("latin1");
    let buf = "", waiting = null, closed = false;
    const ended = new Promise((done) => s.on("close", () => { closed = true; if (waiting) { waiting(null); waiting = null; } done(); }));
    const take = () => {
      if (!waiting) return;
      const h = buf.indexOf("\r\n\r\n");
      if (h < 0) return;
      const m = /content-length: *(\d+)/i.exec(buf.slice(0, h));
      const len = m ? Number(m[1]) : 0;
      if (buf.length < h + 4 + len) return;
      const body = buf.slice(h + 4, h + 4 + len);
      buf = buf.slice(h + 4 + len);
      const w = waiting; waiting = null; w(body);
    };
    s.on("data", (d) => { buf += d; take(); });
    s.on("error", () => {});
    const ask = (target) => new Promise((back) => {
      if (closed) return back(null);
      waiting = back;
      s.write(`GET ${target} HTTP/1.1\r\nHost: x\r\n\r\n`);
      take();
    });
    return { ask, ended, end: () => s.end(), s };
  };
  let up = null;
  for (let i = 0; i < 600 && !up; i++) {
    up = await opened();
    if (!up) await new Promise((ok) => setTimeout(ok, 50));
  }
  if (!up) { console.log("no server"); server.kill("SIGKILL"); process.exit(0); }
  up.end();
  await up.ended;
  // (N connections at once, a request each, answered while all are held
  // open; then a second request each, on the same connection)
  const cs = await Promise.all(Array.from({ length: n }, () => opened()));
  const first = await Promise.all(cs.map((c, i) => (c ? c.ask(`/a/${i}`) : null)));
  console.log(`first requests answered, every connection held open: ${first.filter((b, i) => b === `/a/${i}`).length} of ${n}`);
  const second = await Promise.all(cs.map((c, i) => (c ? c.ask(`/b/${i}`) : null)));
  console.log(`second requests answered, on the same connections: ${second.filter((b, i) => b === `/b/${i}`).length} of ${n}`);
  await Promise.all(cs.map((c) => { if (c) { c.end(); return c.ended; } }));
  // (a handler that awaits a sleep of its own)
  const slow = await opened();
  const t0 = Date.now();
  const sb = await slow.ask("/slow");
  const ms = Date.now() - t0;
  console.log(`slow, its handler awaiting a sleep: ${sb === "slow" && ms >= 280 && ms < 2000 ? "answered, in time" : `${sb} in ${ms} ms`}`);
  // (a handler that throws: its connection closed at once, unanswered,
  // and the next client answered)
  const thrower = await opened();
  const t2 = Date.now();
  const tb = await thrower.ask("/throw");
  const thrownMs = Date.now() - t2;
  const next = await opened();
  const nb = next ? await next.ask("/after") : null;
  if (next) { next.end(); await next.ended; }
  console.log(`a handler that throws: ${tb === null && thrownMs < idle ? "closed at once, unanswered" : `${tb} in ${thrownMs} ms`}; the next: ${nb === "/after" ? "answered" : nb}`);
  // (a client that sends nothing: closed by the server in `idle`)
  const silent = await opened();
  const t1 = Date.now();
  await silent.ended;
  const idled = Date.now() - t1;
  console.log(`a silent client: ${idled >= idle - 50 && idled < idle + 1500 ? "closed in time" : `closed in ${idled} ms`}`);
  // (SIGTERM with a request in flight, and a connection kept open idle:
  // the request answered, the idle one closed, the server ended)
  const kept = await opened();
  await kept.ask("/kept");
  const flight = await opened();
  const answer = flight.ask("/slow");
  await new Promise((ok) => setTimeout(ok, 100));
  server.kill("SIGTERM");
  const fb = await answer;
  console.log(`SIGTERM, a request in flight: ${fb === "slow" ? "answered" : fb}`);
  const keptClosed = await Promise.race([kept.ended.then(() => true), new Promise((ok) => setTimeout(() => ok(false), 3000))]);
  console.log(`a connection kept open, idle: ${keptClosed ? "closed by the stop" : "still open"}`);
  const end = await Promise.race([exited, new Promise((ok) => setTimeout(() => ok(null), 5000))]);
  if (!end) server.kill("SIGKILL");
  console.log(`the server: ${end ? `ended, status ${end.code}` : "still running"}`);
  await exited;
  process.stdout.write(said.split("\n").filter((l) => l).map((l) => `the server said: ${l}\n`).join(""));
  if (saidErr) process.stdout.write(`the server's errors: ${saidErr}`);
}
