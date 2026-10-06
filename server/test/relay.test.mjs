// End-to-end tests of the relay server against `wrangler dev`:
//
//   npx wrangler dev --port 8787 --var CREATES_PER_10_MIN:1000 --var JOINS_PER_10_MIN:1000
//   npm test                                  (or: node test/relay.test.mjs)
//
// RELAY_URL picks another server (default ws://127.0.0.1:8787). With RATE_LIMIT_TEST=1 it checks
// only the per-address limit, against a server started with --var CREATES_PER_10_MIN:2.

// Node 20 has WebSocket only behind a flag (later versions have it built in).
if (typeof WebSocket === "undefined") {
  const { spawnSync } = await import("node:child_process");
  const run = spawnSync(process.execPath, ["--experimental-websocket", "--no-warnings", ...process.argv.slice(1)],
                        { stdio: "inherit" });
  process.exit(run.status ?? 1);
}

const BASE = process.env.RELAY_URL ?? "ws://127.0.0.1:8787";
const HELLO = "proto=1&app=0.1.6&game=DOS%201.1";

let failures = 0;
let checks = 0;
function check(cond, what) {
  checks++;
  if (!cond) {
    failures++;
    console.log(`  FAIL ${what}`);
  }
}

// A connection with a queue of received messages (JSON objects, or Uint8Array for binary).
function connect(path) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`${BASE}${path}`);
    ws.binaryType = "arraybuffer";
    const conn = { ws, queue: [], waiters: [], closed: null };
    ws.onmessage = (e) => {
      const msg = typeof e.data === "string"
        ? (e.data === "pong" ? { t: "pong" } : JSON.parse(e.data))
        : new Uint8Array(e.data);
      const waiter = conn.waiters.shift();
      if (waiter) waiter(msg);
      else conn.queue.push(msg);
    };
    ws.onclose = (e) => {
      conn.closed = { code: e.code, reason: e.reason };
      for (const w of conn.waiters.splice(0)) w(null);
    };
    ws.onopen = () => resolve(conn);
    ws.onerror = () => { if (ws.readyState !== WebSocket.OPEN) reject(new Error(`can't connect ${path}`)); };
  });
}

function next(conn, timeoutMs = 3000) {
  if (conn.queue.length) return Promise.resolve(conn.queue.shift());
  if (conn.closed) return Promise.resolve(null);
  return new Promise((resolve) => {
    const timer = setTimeout(() => {
      conn.waiters.splice(conn.waiters.indexOf(done), 1);
      resolve(undefined);  // nothing arrived
    }, timeoutMs);
    const done = (msg) => { clearTimeout(timer); resolve(msg); };
    conn.waiters.push(done);
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function expectError(path, code, what) {
  const c = await connect(path);
  const msg = await next(c);
  check(msg?.t === "error" && msg.code === code, `${what}: error ${code} (got ${JSON.stringify(msg)})`);
  if (msg?.t === "error") console.log(`  refused (${msg.code}): ${msg.reason}`);
  await next(c, 1000);
  check(c.closed !== null, `${what}: closed after the error`);
}

async function rateLimitTest() {
  const opened = [];
  for (let i = 0; i < 2; i++) {
    const c = await connect(`/v1/create?${HELLO}`);
    check((await next(c))?.t === "welcome", `create ${i + 1} is allowed`);
    opened.push(c);
  }
  await expectError(`/v1/create?${HELLO}`, "rate", "third create within 10 minutes");
  for (const c of opened) c.ws.close();
}

async function main() {
  if (process.env.RATE_LIMIT_TEST) {
    await rateLimitTest();
    return;
  }

  // The hello: protocol and versions.
  await expectError(`/v1/create?proto=2&app=0.1.6&game=x`, "version", "newer protocol");
  await expectError(`/v1/create?proto=0&app=0.1.6&game=x`, "version", "older protocol");
  await expectError(`/v1/create?proto=1&game=x`, "bad_request", "no app version");
  await expectError(`/v1/join/VETTE-0000?${HELLO}`, "no_room", "code with 0s");
  await expectError(`/v1/join/VETTE-ZZZZ?${HELLO}`, "no_room", "no such room");

  // Create, with the race settings.
  const host = await connect(`/v1/create?${HELLO}&settings=${encodeURIComponent("improved_driving=1")}`);
  const hw = await next(host);
  check(hw?.t === "welcome" && hw.role === "host" && /^[2-9A-HJ-NP-Z]{4}$/.test(hw.code),
        `host welcome (${JSON.stringify(hw)})`);
  check(hw.peer === "none" && hw.settings_gen === 1 && hw.token.length === 32, "host welcome fields");
  const code = hw.code;
  console.log(`  room VETTE-${code}`);

  await expectError(`/v1/join/vette-${code.toLowerCase()}?proto=1&app=0.1.7&game=DOS%201.1`, "app",
                    "join with another VETTE! 2026 version");
  await expectError(`/v1/join/${code}?proto=1&app=0.1.6&game=DOS%201.0`, "game",
                    "join with another original game build");

  // Join: the guest has the settings in its welcome, before anything is relayed.
  const guest = await connect(`/v1/join/VETTE-${code}?${HELLO}`);
  const gw = await next(guest);
  check(gw?.t === "welcome" && gw.role === "guest" && gw.peer === "here", "guest welcome");
  check(gw.settings === "improved_driving=1" && gw.settings_gen === 1, "settings in the guest welcome");
  check((await next(host))?.state === "joined", "host told the guest joined");
  await expectError(`/v1/join/${code}?${HELLO}`, "full", "third player");

  // Relay, both ways, in order.
  for (let i = 0; i < 100; i++) {
    host.ws.send(new Uint8Array([1, i]));
    guest.ws.send(new Uint8Array([2, i, i]));
  }
  let inOrder = true;
  for (let i = 0; i < 100; i++) {
    const a = await next(guest), b = await next(host);
    inOrder &&= a instanceof Uint8Array && a[0] === 1 && a[1] === i && a.length === 2;
    inOrder &&= b instanceof Uint8Array && b[0] === 2 && b[1] === i && b.length === 3;
  }
  check(inOrder, "100 messages each way, in order");

  // Ping is answered by the runtime.
  host.ws.send("ping");
  check((await next(host))?.t === "pong", "ping answered");

  // Settings updates: newer generations reach the guest; an old generation is ignored.
  host.ws.send(JSON.stringify({ t: "settings", gen: 2, data: "improved_driving=0\nlaps=3" }));
  const s2 = await next(guest);
  check(s2?.t === "settings" && s2.gen === 2 && s2.data === "improved_driving=0\nlaps=3", "settings update");
  host.ws.send(JSON.stringify({ t: "settings", gen: 1, data: "improved_driving=1" }));
  guest.ws.send(JSON.stringify({ t: "settings", gen: 9, data: "guest=can't" }));
  host.ws.send(new Uint8Array([7]));  // a marker: nothing may arrive before it
  const marker = await next(guest);
  check(marker instanceof Uint8Array && marker[0] === 7, "stale and guest settings are ignored");

  // The guest's connection drops: the host is told, the guest resumes and gets the current settings.
  guest.ws.close();
  check((await next(host))?.state === "away", "host told the guest is away");
  host.ws.send(new Uint8Array([8]));  // dropped: nobody to relay to (the client resends after "back")
  const back = await connect(`/v1/resume/${code}?${HELLO}&token=${gw.token}`);
  const bw = await next(back);
  check(bw?.t === "welcome" && bw.resumed === true && bw.role === "guest", "guest resumed");
  check(bw.settings_gen === 2 && bw.settings === "improved_driving=0\nlaps=3", "resume has the current settings");
  check((await next(host))?.state === "back", "host told the guest is back");
  host.ws.send(new Uint8Array([9]));
  const after = await next(back);
  check(after instanceof Uint8Array && after[0] === 9, "relay after resume");
  await expectError(`/v1/resume/${code}?${HELLO}&token=00000000000000000000000000000000`, "expired",
                    "resume with a wrong token");

  // A message over the size limit drops that connection (it may resume).
  back.ws.send(new Uint8Array(4096));
  await next(back, 1000);
  check(back.closed?.code === 1009, `oversized message closes the connection (${back.closed?.code})`);
  check((await next(host))?.state === "away", "host told after the oversized message");
  const back2 = await connect(`/v1/resume/${code}?${HELLO}&token=${gw.token}`);
  check((await next(back2))?.resumed === true, "resumed again");
  check((await next(host))?.state === "back", "back again");

  // The guest leaves for good: the seat is free for someone else.
  back2.ws.send("bye");
  const left = await next(host);
  check(left?.state === "left" && left.reason === "Your friend left the room.", "host told the guest left");
  const guest2 = await connect(`/v1/join/${code}?${HELLO}`);
  const g2w = await next(guest2);
  check(g2w?.t === "welcome" && g2w.settings_gen === 2, "a new guest joins the same room");
  check((await next(host))?.state === "joined", "host told the new guest joined");

  // The host leaves: the room closes for the guest too.
  host.ws.send("bye");
  const g2left = await next(guest2);
  check(g2left?.state === "left", "guest told the host left");
  const g2closed = await next(guest2);
  check(g2closed?.t === "closed", "guest told the room closed");
  // The server has closed the connection (4410). Under `wrangler dev` the local proxy may hold the
  // TCP connection open after the close handshake, so the guest closes it too, as the game does.
  await next(guest2, 500);
  if (!guest2.closed) guest2.ws.close();
  await expectError(`/v1/join/${code}?${HELLO}`, "no_room", "the closed room is gone");
}

const started = Date.now();
await main().catch((e) => { failures++; console.log(`  FAIL ${e.stack}`); });
console.log(`${checks} checks, ${failures} failure(s), ${Date.now() - started} ms`);
process.exit(failures ? 1 : 0);
