// VETTE! 2026 relay server: room codes and a byte relay for online two-player races.
//
// A Cloudflare Worker with one Durable Object per room (SQLite-backed, WebSocket Hibernation API),
// and one small Durable Object per client address that counts room creations and joins. Players
// connect with a WebSocket:
//
//   /v1/create?proto=1&app=<VETTE! 2026 version>&game=<original game build>&settings=<race settings>
//   /v1/join/<CODE>?proto=1&app=...&game=...
//   /v1/resume/<CODE>?proto=1&app=...&game=...&token=<the token from the welcome>
//
// The server speaks JSON text messages (welcome, peer, settings, error, closed). The players' binary
// messages are relayed to the other player unchanged and in order; the server never looks inside
// them. Text "ping" is answered "pong" by the runtime without waking the room. The full protocol is
// in README.md. A room keeps its state only while it's open: closing it deletes everything.

import { DurableObject } from "cloudflare:workers";

const PROTOCOL = 1;

const CODE_ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"; // no 0/O, 1/I: easy to read out and type
const CODE_LENGTH = 4;                                    // 32^4 = 1,048,576 codes
const CODE_PREFIX = "VETTE-";

const UNUSED_ROOM_MS = 30 * 60 * 1000;  // a room nobody has joined closes after 30 minutes
const GRACE_MS = 30 * 1000;             // a dropped player may reconnect within 30 seconds
const LIFETIME_MS = 12 * 3600 * 1000;   // every room closes after 12 hours
const STALE_MS = 20 * 1000;             // no "ping" for this long: the connection is presumed dead
const STALE_CHECK_MS = 2000;

const MAX_BINARY = 2048;   // bytes per relayed message (the game client sends at most 1044)
const MAX_TEXT = 1024;     // bytes per control message from a client
const MAX_SETTINGS = 512;  // the host's race settings (key=value lines)
const MAX_FIELD = 64;      // app and game version strings
const RATE_PER_SECOND = 120;  // messages per connection, sustained...
const RATE_BURST = 300;       // ...and in a burst (a reconnect resends what was missed)

const LIMIT_WINDOW_MS = 10 * 60 * 1000;  // per client address, within 10 minutes:
const DEFAULT_CREATES = 10;              //   rooms created (CREATES_PER_10_MIN overrides)
const DEFAULT_JOINS = 30;                //   join attempts (JOINS_PER_10_MIN overrides)

// WebSocket close codes for refusals (4000-4999 is the application range), mirroring HTTP.
const CLOSE = {
  bad_request: 4400, version: 4426, app: 4409, game: 4409, no_room: 4404, full: 4403,
  rate: 4429, expired: 4410, busy: 4503, closed: 4410, replaced: 4000, timeout: 4408,
};

const encoder = new TextEncoder();

function randomCode() {
  const bytes = crypto.getRandomValues(new Uint8Array(CODE_LENGTH));
  return Array.from(bytes, (b) => CODE_ALPHABET[b & 31]).join("");
}

function randomToken() {
  const bytes = crypto.getRandomValues(new Uint8Array(16));
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

// "vette-4kq7", "VETTE 4KQ7", "4kq7" -> "4KQ7"; null if it can't be a code.
function normalizeCode(typed) {
  let code = String(typed ?? "").toUpperCase().replace(/[^0-9A-Z]/g, "");
  if (code.startsWith("VETTE") && code.length > CODE_LENGTH) {
    code = code.slice(5);
  }
  if (code.length !== CODE_LENGTH || [...code].some((c) => !CODE_ALPHABET.includes(c))) {
    return null;
  }
  return code;
}

// Printable ASCII only (and newlines where allowed), within a length.
function cleanField(value, max, allowNewlines = false) {
  if (typeof value !== "string" || value.length > max) {
    return null;
  }
  const ok = allowNewlines ? /^[\x20-\x7E\n]*$/ : /^[\x20-\x7E]*$/;
  return ok.test(value) ? value : null;
}

// Accepts the WebSocket only to say why it's refused, then closes it: clients see a reason they
// can show instead of a bare HTTP error.
function refuse(code, reason) {
  const [client, server] = Object.values(new WebSocketPair());
  server.accept();
  server.send(JSON.stringify({ t: "error", code, reason }));
  server.close(CLOSE[code] ?? 4400, code);
  return new Response(null, { status: 101, webSocket: client });
}

function closeQuietly(ws, code, reason) {
  try {
    ws.close(code, reason);
  } catch {
    // already closed
  }
}

function sendQuietly(ws, message) {
  try {
    ws.send(typeof message === "string" || message instanceof ArrayBuffer ? message
                                                                          : JSON.stringify(message));
    return true;
  } catch {
    return false;
  }
}

const other = (role) => (role === "host" ? "guest" : "host");

async function addressKey(request) {
  const ip = request.headers.get("CF-Connecting-IP") ?? "local";
  const digest = await crypto.subtle.digest("SHA-256", encoder.encode(`vette2026:${ip}`));
  return Array.from(new Uint8Array(digest).slice(0, 16), (b) => b.toString(16).padStart(2, "0"))
    .join("");
}

export default {
  async fetch(request, env) {
    try {
      return await handle(request, env);
    } catch {
      // A Durable Object call failed: most likely the free plan's daily allowance is used up.
      return refuse("busy", "The online server can't take the connection right now (it may have used " +
                            "up today's free allowance, which resets at midnight UTC). Try again later.");
    }
  },
};

// Routes a connection: the hello, the abuse limits, then the room.
async function handle(request, env) {
  const url = new URL(request.url);
  const parts = url.pathname.split("/").filter(Boolean);
  if (parts.length === 0) {
    return new Response(`VETTE! 2026 relay server, protocol ${PROTOCOL}.\n`,
                        { headers: { "content-type": "text/plain; charset=utf-8" } });
  }
  const op = parts[1];
  if (parts[0] !== "v1" || !["create", "join", "resume"].includes(op) ||
      parts.length !== (op === "create" ? 2 : 3)) {
    return new Response("Not found.\n", { status: 404 });
  }
  if ((request.headers.get("Upgrade") ?? "").toLowerCase() !== "websocket") {
    return new Response("This address takes WebSocket connections from VETTE! 2026.\n",
                        { status: 426, headers: { Upgrade: "websocket" } });
  }

  // The hello: protocol version, then the game's version and build.
  const q = url.searchParams;
  const proto = Number.parseInt(q.get("proto") ?? "", 10);
  if (!Number.isInteger(proto)) {
    return refuse("bad_request", "The request has no protocol version.");
  }
  if (proto !== PROTOCOL) {
    return refuse("version", proto < PROTOCOL
      ? `This VETTE! 2026 is too old for the online server (protocol ${proto}, the server ` +
        `needs ${PROTOCOL}). Update the game to play online.`
      : `The online server is older than this VETTE! 2026 (protocol ${proto}, the server ` +
        `has ${PROTOCOL}). The server needs an update.`);
  }
  const app = cleanField(q.get("app"), MAX_FIELD);
  const game = cleanField(q.get("game"), MAX_FIELD);
  const settings = cleanField(q.get("settings") ?? "", MAX_SETTINGS, true);
  if (!app || !game) {
    return refuse("bad_request", "The request has no game version.");
  }
  if (settings === null) {
    return refuse("bad_request", `The race settings are longer than ${MAX_SETTINGS} bytes.`);
  }

  // Abuse limits, per client address: rooms created and join attempts (guessing codes).
  if (op !== "resume") {
    const limiter = env.LIMITER.get(env.LIMITER.idFromName(await addressKey(request)));
    const max = Number.parseInt(op === "create" ? env.CREATES_PER_10_MIN : env.JOINS_PER_10_MIN, 10);
    const limit = Number.isInteger(max) ? max : (op === "create" ? DEFAULT_CREATES : DEFAULT_JOINS);
    if (!(await limiter.allow(op, limit, LIMIT_WINDOW_MS))) {
      return refuse("rate", op === "create"
        ? "Too many rooms were created from your address. Try again in a few minutes."
        : "Too many attempts to join a room from your address. Try again in a few minutes.");
    }
  }

  const forward = (code) => {
    const target = new URL(`https://room/${op}`);
    target.searchParams.set("code", code);
    for (const [key, value] of [["app", app], ["game", game], ["settings", settings],
                                ["token", q.get("token") ?? ""]]) {
      target.searchParams.set(key, value);
    }
    return new Request(target, request);
  };

  if (op === "create") {
    // A new random code; the room refuses (409) if that code is in use, and another is tried.
    for (let attempt = 0; attempt < 8; attempt++) {
      const code = randomCode();
      const response = await env.ROOMS.get(env.ROOMS.idFromName(code)).fetch(forward(code));
      if (response.status !== 409) {
        return response;
      }
    }
    return refuse("busy", "The server couldn't find a free room code. Try again.");
  }
  const code = normalizeCode(parts[2]);
  if (!code) {
    return refuse("no_room", `${parts[2]} isn't a room code. Codes look like ${CODE_PREFIX}4KQ7.`);
  }
  return env.ROOMS.get(env.ROOMS.idFromName(code)).fetch(forward(code));
}

// One room: a host, then at most one guest. Its record (in storage, so it survives hibernation):
//   code, created, app, game     the room and the host's versions (a guest must match them)
//   settings, settingsGen        the host's race settings and their generation (1, 2, ...)
//   token.{host,guest}           each player's secret for reconnecting (null: nobody in that seat)
//   conn.{host,guest}            the number of each player's current connection (0: none)
//   away.{host,guest}            the reconnect deadline while a player's connection is down (0: not)
//   unusedSince                  while there's no guest: the room closes UNUSED_ROOM_MS after this
//   nextConn, alarmAt            connection counter; the alarm that's set
export class Room extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.room = null;
    this.sockets = { host: null, guest: null };  // cache of the current connections
    this.buckets = new WeakMap();                // message rate per connection (memory only)
    this.lastStaleCheck = 0;
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
    ctx.blockConcurrencyWhile(async () => {
      this.room = (await ctx.storage.get("room")) ?? null;
    });
  }

  async fetch(request) {
    const url = new URL(request.url);
    const q = url.searchParams;
    const op = url.pathname.slice(1);
    const code = q.get("code");
    const now = Date.now();

    if (op === "create") {
      if (this.room) {
        return new Response(null, { status: 409 });  // code in use: the Worker picks another
      }
      this.room = {
        code, created: now, app: q.get("app"), game: q.get("game"),
        settings: q.get("settings") ?? "", settingsGen: 1,
        token: { host: randomToken(), guest: null },
        conn: { host: 0, guest: 0 },
        away: { host: 0, guest: 0 },
        unusedSince: now,
        nextConn: 1,
        alarmAt: 0,
      };
      const client = this.admit("host", now, false);
      await this.save();
      return new Response(null, { status: 101, webSocket: client });
    }

    const room = this.room;
    if (!room) {
      return refuse(op === "resume" ? "expired" : "no_room", op === "resume"
        ? "The room has closed."
        : `There's no room ${CODE_PREFIX}${code}. Check the code with your friend.`);
    }

    if (op === "join") {
      if (room.token.guest) {
        return refuse("full", `Room ${CODE_PREFIX}${code} already has two players.`);
      }
      const app = q.get("app"), game = q.get("game");
      if (app !== room.app) {
        return refuse("app", `Your friend has VETTE! 2026 ${room.app} and you have ${app}. ` +
                             "You both need the same version.");
      }
      if (game !== room.game) {
        return refuse("game", `Your friend's copy of VETTE! is ${room.game} and yours is ${game}. ` +
                              "You both need the same version of the original game.");
      }
      room.token.guest = randomToken();
      room.unusedSince = 0;
      const client = this.admit("guest", now, false);
      this.sendTo("host", { t: "peer", state: "joined" });
      await this.save();
      return new Response(null, { status: 101, webSocket: client });
    }

    // resume: the same player again, after their connection dropped (or went quiet).
    const token = q.get("token");
    const role = token && token === room.token.host ? "host"
               : token && token === room.token.guest ? "guest" : null;
    if (!role) {
      return refuse("expired", "Your place in the room has expired.");
    }
    for (const ws of this.ctx.getWebSockets(role)) {
      closeQuietly(ws, CLOSE.replaced, "replaced");  // an older connection that still looks open
    }
    room.away[role] = 0;
    const client = this.admit(role, now, true);
    // Always "back", even if the peer never saw "away" (a half-open connection): whatever was
    // relayed to the old connection may be lost, and the peer resends it.
    this.sendTo(other(role), { t: "peer", state: "back" });
    await this.save();
    return new Response(null, { status: 101, webSocket: client });
  }

  // Accepts a player's WebSocket (hibernatable) and sends the welcome.
  admit(role, now, resumed) {
    const room = this.room;
    const [client, server] = Object.values(new WebSocketPair());
    const id = room.nextConn++;
    room.conn[role] = id;
    this.ctx.acceptWebSocket(server, [role]);
    server.serializeAttachment({ role, id, since: now });
    this.sockets[role] = server;
    const welcome = {
      t: "welcome", role, code: room.code, token: room.token[role], resumed,
      peer: this.peerState(other(role)), grace: GRACE_MS / 1000,
      expires: UNUSED_ROOM_MS / 1000, settings_gen: room.settingsGen,
    };
    if (role === "guest") {
      welcome.settings = room.settings;  // the guest has them before any relayed byte
    }
    server.send(JSON.stringify(welcome));
    return client;
  }

  peerState(role) {
    if (!this.room.token[role]) {
      return "none";
    }
    return this.room.away[role] ? "away" : "here";
  }

  // The player's current connection, or null.
  current(role) {
    const room = this.room;
    if (!room || !room.conn[role]) {
      return null;
    }
    const cached = this.sockets[role];
    if (cached && cached.readyState === WebSocket.OPEN) {
      return cached;
    }
    this.sockets[role] = null;
    for (const ws of this.ctx.getWebSockets(role)) {
      if (ws.readyState === WebSocket.OPEN && ws.deserializeAttachment()?.id === room.conn[role]) {
        this.sockets[role] = ws;
        return ws;
      }
    }
    return null;
  }

  sendTo(role, message) {
    const ws = this.current(role);
    return ws ? sendQuietly(ws, message) : false;
  }

  allowRate(ws, now) {
    let bucket = this.buckets.get(ws);
    if (!bucket) {
      bucket = { tokens: RATE_BURST, at: now };
      this.buckets.set(ws, bucket);
    }
    bucket.tokens = Math.min(RATE_BURST, bucket.tokens + ((now - bucket.at) * RATE_PER_SECOND) / 1000);
    bucket.at = now;
    if (bucket.tokens < 1) {
      return false;
    }
    bucket.tokens -= 1;
    return true;
  }

  async webSocketMessage(ws, message) {
    const att = ws.deserializeAttachment();
    if (!this.room || !att || att.id !== this.room.conn[att.role]) {
      closeQuietly(ws, CLOSE.closed, "closed");  // a replaced connection, or the room is gone
      return;
    }
    const now = Date.now();
    if (!this.allowRate(ws, now)) {
      return this.dropConnection(att.role, ws, 1008, "rate limit");
    }
    if (typeof message === "string") {
      return this.control(att.role, ws, message);
    }
    if (message.byteLength > MAX_BINARY) {
      return this.dropConnection(att.role, ws, 1009, "message too big");
    }
    const peer = this.current(other(att.role));
    if (!peer) {
      return;  // the peer is away (or not there yet): its client resends after "back"
    }
    if (now - this.lastStaleCheck >= STALE_CHECK_MS) {
      this.lastStaleCheck = now;
      // The peer's connection may be half-open (its network vanished without closing it): its
      // client pings every 2 s, and the runtime records when it last answered.
      const pinged = this.ctx.getWebSocketAutoResponseTimestamp(peer);
      const since = pinged ? pinged.getTime() : peer.deserializeAttachment().since;
      if (now - since > STALE_MS) {
        return this.dropConnection(other(att.role), peer, CLOSE.timeout, "no ping");
      }
    }
    sendQuietly(peer, message);
  }

  async control(role, ws, text) {
    if (text.length > MAX_TEXT) {
      return this.dropConnection(role, ws, 1009, "message too big");
    }
    if (text === "bye") {
      return this.leave(role, "left");
    }
    let message;
    try {
      message = JSON.parse(text);
    } catch {
      return;
    }
    // The host's race settings: newer generations only, so nothing stale ever replaces them.
    if (message?.t === "settings" && role === "host" && Number.isInteger(message.gen) &&
        message.gen > this.room.settingsGen) {
      const settings = cleanField(message.data, MAX_SETTINGS, true);
      if (settings === null) {
        return;
      }
      this.room.settingsGen = message.gen;
      this.room.settings = settings;
      this.sendTo("guest", { t: "settings", gen: message.gen, data: settings });
      await this.save();
    }
  }

  async webSocketClose(ws) {
    await this.connectionLost(ws);
  }

  async webSocketError(ws) {
    await this.connectionLost(ws);
  }

  async connectionLost(ws) {
    this.buckets.delete(ws);
    const att = ws.deserializeAttachment();
    if (this.room && att && att.id === this.room.conn[att.role]) {
      await this.markAway(att.role);
    }
  }

  // Closes a player's connection as if it had dropped: they may reconnect within the grace period.
  async dropConnection(role, ws, code, reason) {
    closeQuietly(ws, code, reason);
    if (this.room && ws.deserializeAttachment()?.id === this.room.conn[role]) {
      await this.markAway(role);
    }
  }

  async markAway(role) {
    const room = this.room;
    room.conn[role] = 0;
    room.away[role] = Date.now() + GRACE_MS;
    this.sockets[role] = null;
    this.sendTo(other(role), { t: "peer", state: "away" });
    await this.save();
  }

  // A player leaves for good ("bye", or the grace period ran out).
  async leave(role, why) {
    const lost = why === "timeout";
    if (role === "host") {
      return this.closeRoom({
        host: "You left the room.",
        guest: lost ? "Your friend's connection was lost." : "Your friend left the room.",
      });
    }
    const room = this.room;
    const ws = this.current("guest");
    room.token.guest = null;
    room.conn.guest = 0;
    room.away.guest = 0;
    room.unusedSince = Date.now();
    this.sockets.guest = null;
    if (ws) {
      closeQuietly(ws, 1000, "left");
    }
    this.sendTo("host", { t: "peer", state: "left",
                          reason: lost ? "Your friend's connection was lost."
                                       : "Your friend left the room." });
    await this.save();
  }

  // Tells everyone why, closes every connection and forgets the room.
  async closeRoom(reasons) {
    for (const ws of this.ctx.getWebSockets()) {
      const role = ws.deserializeAttachment()?.role;
      if (role === "guest" && reasons.guest) {
        sendQuietly(ws, { t: "peer", state: "left", reason: reasons.guest });
      }
      sendQuietly(ws, { t: "closed", reason: reasons[role] ?? "The room has closed." });
      closeQuietly(ws, CLOSE.closed, "room closed");
    }
    this.room = null;
    this.sockets = { host: null, guest: null };
    await this.ctx.storage.deleteAlarm();
    await this.ctx.storage.deleteAll();
  }

  async alarm() {
    const room = this.room;
    if (!room) {
      await this.ctx.storage.deleteAll();
      return;
    }
    room.alarmAt = 0;  // it has fired; save() sets the next one
    const now = Date.now();
    if (now >= room.created + LIFETIME_MS) {
      const reason = "The room has been open for 12 hours, so it closed.";
      return this.closeRoom({ host: reason, guest: reason });
    }
    if (room.away.host && now >= room.away.host) {
      return this.leave("host", "timeout");
    }
    if (room.away.guest && now >= room.away.guest) {
      await this.leave("guest", "timeout");
    }
    if (!room.token.guest && room.unusedSince && now >= room.unusedSince + UNUSED_ROOM_MS) {
      return this.closeRoom({ host: "Nobody joined the room within 30 minutes, so it closed." });
    }
    await this.save();
  }

  // Stores the record and keeps the alarm at the next deadline.
  async save() {
    const room = this.room;
    const next = Math.min(
      room.created + LIFETIME_MS,
      room.away.host || Infinity,
      room.away.guest || Infinity,
      !room.token.guest && room.unusedSince ? room.unusedSince + UNUSED_ROOM_MS : Infinity);
    if (next !== room.alarmAt) {
      room.alarmAt = next;
      await this.ctx.storage.setAlarm(next);
    }
    await this.ctx.storage.put("room", room);
  }
}

// Counts room creations and join attempts from one client address (keyed by a hash of it) in a
// sliding window. Everything is deleted when the window has passed.
export class Limiter extends DurableObject {
  async allow(kind, max, windowMs) {
    const now = Date.now();
    const times = ((await this.ctx.storage.get(kind)) ?? []).filter((t) => now - t < windowMs);
    if (times.length >= max) {
      return false;
    }
    times.push(now);
    await this.ctx.storage.put(kind, times);
    await this.ctx.storage.setAlarm(now + windowMs);
    return true;
  }

  async alarm() {
    await this.ctx.storage.deleteAll();
  }
}
