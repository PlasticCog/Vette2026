# VETTE! 2026 relay server

Online two-player races meet here. The host's game creates a **room** and shows a code like
**VETTE-4KQ7**; the friend types it in their game. From then on the server relays the two games'
serial-cable bytes between them, in order, until the race is over. Friends only: there's no lobby, and a
room takes exactly two players.

It's a [Cloudflare Worker](https://developers.cloudflare.com/workers/) with one
[Durable Object](https://developers.cloudflare.com/durable-objects/) per room, and it runs on Cloudflare's
**free plan**. `src/index.js` is the whole server.

## Deploying it (once)

You need [Node.js](https://nodejs.org/) 20.3 or newer and a free Cloudflare account. Nothing is
installed globally: the commands run the Wrangler version pinned in `package.json`.

1. **Create a Cloudflare account** at <https://dash.cloudflare.com/sign-up> and confirm your email
   address. The free plan is enough; no payment details are needed.
2. **Install the tools**, in this folder:
   ```
   cd server
   npm ci
   ```
3. **Log in**: `npx wrangler login` opens your browser; allow Wrangler access to your account.
4. **Deploy**: `npx wrangler deploy`. The first time, if your account has no `workers.dev` subdomain
   yet, Wrangler asks you to choose one (for example your name); it becomes part of the address. You can
   also pick it in the dashboard under **Workers & Pages**.
5. **The address** is printed at the end of the deploy:
   ```
   Deployed vette2026-relay triggers
     https://vette2026-relay.<your-subdomain>.workers.dev
   ```
   Open it in a browser: it should say `VETTE! 2026 relay server, protocol 1.`
6. **Use it in the game**: the online server setting is the same address with `wss://` instead of
   `https://`, e.g. `wss://vette2026-relay.<your-subdomain>.workers.dev`.
7. **Check it** from two computers (or two windows) with `vette_netcheck`, built with the game:
   ```
   vette_netcheck --server wss://vette2026-relay.<your-subdomain>.workers.dev create
   vette_netcheck --server wss://vette2026-relay.<your-subdomain>.workers.dev join VETTE-4KQ7
   ```
   Both print the round-trip time and check every byte that arrives.

Later: `npx wrangler deploy` again after changing the server; `npx wrangler tail` shows its log live
(it logs nothing by itself); the dashboard's **Workers & Pages → vette2026-relay** shows the request
counts. Renaming `name` in `wrangler.toml` changes the address.

## Trying it locally

```
npm ci
npx wrangler dev --port 8787
npm test                                   # in another terminal: the server's end-to-end tests
vette_netcheck create                      # the default server is ws://127.0.0.1:8787
vette_netcheck join VETTE-XXXX
```

`npm test` runs `test/relay.test.mjs`: create and join, the race settings, relaying in order, refusals
(versions, codes, a full room), dropping and resuming, leaving. Start `wrangler dev` for it with
`--var CREATES_PER_10_MIN:1000 --var JOINS_PER_10_MIN:1000`, or the per-address limits stop it part way.
`vette_netcheck --drop-at 3:2` drops its connection 3 s in and stays offline 2 s, to watch a reconnect.

## How much the free plan allows

The free plan's daily allowance (reset at 00:00 UTC) that matters here is **100,000 Durable Object
requests**. Incoming WebSocket messages count 20 to a request; outgoing messages, WebSocket protocol
pings and the runtime's automatic `ping`/`pong` answers aren't charged
([pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/)).

| While racing (both games sending) | Per room |
|---|---|
| Messages to the server | each game sends at most **30 a second** (the client groups each burst of serial bytes into one message: `Batching` in `src/net/session.h`), so 60 a second |
| Requests | 60 / 20 = **3 a second = 10,800 an hour** |
| Duration | the room stays awake while messages flow: 128 MB x 3,600 s = 461 GB-s an hour (free: 13,000 GB-s a day) |

So the free plan carries about **9 hours of racing a day**, summed over all rooms. Everything else is
small: joining costs a few requests (the room, the per-address limiter, a Worker request per connection,
of which the free plan has another 100,000 a day); a room where nobody is sending (menus, waiting)
hibernates and costs nothing; a room writes about 10 rows of SQLite storage over its life and the
limiter 2 per join (free: 100,000 a day).

Measured with `vette_netcheck` against `wrangler dev` (30 packets of 50 bytes a second each way): 29.3
messages a second per side, 24 bytes of header each, every byte intact through drops and reconnects.

If that's ever too little: `Batching::min_interval_us = 66'667` halves the messages (about 18 hours of
racing a day) at the cost of up to 33 ms more delay per packet. Beyond the allowance, rooms can't be
created or joined until midnight UTC (the game shows the server's explanation), and running races may
stop; the Workers Paid plan ($5 a month) raises the limits.

## The protocol (version 1)

A game connects with a WebSocket and says who it is in the query: `proto` (the protocol version),
`app` (the VETTE! 2026 version) and `game` (the original game's version and build). Versions must match:

| Path | Who |
|---|---|
| `/v1/create?proto=1&app=…&game=…&settings=…` | the host: creates a room with a new code |
| `/v1/join/<CODE>?proto=1&app=…&game=…` | the friend: joins the room (codes are accepted as `VETTE-4KQ7`, `vette4kq7`, `4KQ7`) |
| `/v1/resume/<CODE>?proto=1&app=…&game=…&token=…` | either, after a dropped connection, within the grace period |

**From the server** (JSON text):

- `{"t":"welcome","role":"host"|"guest","code":"4KQ7","token":…,"resumed":…,"peer":"none"|"here"|"away","grace":30,"expires":1800,"settings_gen":1,"settings":…}`:
  the first message on every connection. `token` is the player's secret for resuming. A guest's welcome
  always has the host's race settings, so they're there before any relayed byte.
- `{"t":"peer","state":"joined"|"away"|"back"|"left","reason":…}`: the other player arrived, dropped
  (and may come back within the grace period), came back, or left for good.
- `{"t":"settings","gen":2,"data":"improved_driving=1"}`: to the guest, when the host changes the race
  settings. Only newer generations are passed on, so nothing stale ever replaces them.
- `{"t":"error","code":…,"reason":…}` then a close: a refusal, with a sentence to show the player.
  Codes: `version`, `app`, `game`, `no_room`, `full`, `rate`, `expired`, `busy`, `bad_request`.
- `{"t":"closed","reason":…}` then a close: the room has closed.
- `pong`: the answer to `ping`, given by the runtime without waking the room.

**From a game**: binary messages, relayed unchanged to the other player (dropped while the other player
is away: the clients resend; see `MessageHeader` in `src/net/protocol.h`); `ping` every 2 s; `bye` to
leave for good; and from the host only, `{"t":"settings","gen":<n>,"data":"key=value lines"}`.

**Rooms**: a code is 4 characters from `23456789ABCDEFGHJKLMNPQRSTUVWXYZ` (no 0/O or 1/I), about a
million codes. A room that nobody joins closes after 30 minutes; a dropped player has 30 seconds to come
back; when the host leaves (or doesn't come back) the room closes; when the guest leaves, the host can
be joined again. Every room closes after 12 hours.

**Limits**: binary messages up to 2,048 bytes and text up to 1,024; 120 messages a second per
connection (bursts of 300); race settings up to 512 bytes; per client address, 10 rooms created and 30
join attempts in 10 minutes (`CREATES_PER_10_MIN`, `JOINS_PER_10_MIN` in `wrangler.toml`). A connection
that goes 20 s without a `ping` while the other player is sending is closed as dead.

**What's stored**: a room's record (code, versions, race settings, the two tokens, deadlines) lives in
its Durable Object while the room is open and is deleted when it closes. The limiter keeps the times of
recent creations and joins under a hash of the client's address, deleted 10 minutes after the last one.
Nothing is logged.
