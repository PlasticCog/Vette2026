# VETTE! 2026 online server

VETTE! 2026 players can race each other over the internet. This folder is the small server that helps
them find each other: the host gets a **room code** like **VETTE-4KQ7** (or an invite link,
`https://…/join/VETTE-4KQ7`) to send a friend, and the friend types or clicks it.

Once both are in the room, the two games first try to connect **straight to each other** (on the same
home network, or over the internet when the host's router allows it). If they can, the race doesn't use
the server at all. If they can't, the server passes the race along between them ("via server"). Friends
only: there's no lobby, and a room takes exactly two players.

Players can also race with **no server at all**, using a *direct code*. The game makes one when the host's
router lets it open a port. The server is only needed for room codes and invite links.

It runs on **Cloudflare's free plan**: a [Worker](https://developers.cloudflare.com/workers/) and one
[Durable Object](https://developers.cloudflare.com/durable-objects/) per room. `src/index.js` is the whole
server, and `src/invite.js` makes the invite pages.

## Putting it online (once, about 5 minutes)

You need:

- a computer with [Node.js](https://nodejs.org/), version 20.3 or newer (the "LTS" download is fine);
- a free Cloudflare account. You can create it during step 2. No payment details are needed.

1. Open this `server` folder.
2. Run the deploy script:
   - **Windows:** double-click `deploy.cmd`.
   - **macOS / Linux:** in a terminal, `sh deploy.sh`.

   It installs the tools it needs (into this folder only). Your browser then opens so you can sign in to
   Cloudflare (or sign up) and allow access. After that, it puts the server online.
3. At the end it prints the server's address, and saves it in `server-address.txt`:
   ```
     Your VETTE! 2026 server is running.

     Its address (the game's "Server" setting):
       wss://vette2026-relay.<your-name>.workers.dev
   ```
   Put that address in the game: **Online race → Server**. To make it the default for everyone who
   downloads your builds, see "Building the game with your server" below.

If the script says your account needs a **workers.dev subdomain**: open <https://dash.cloudflare.com>, go
to **Workers & Pages**, choose a subdomain (your name, for example; it becomes part of the address), and
run the script again.

To check that it works, open the address in a browser with `https://` instead of `wss://`. It should say
`VETTE! 2026 relay server, protocol 1.` To test a race, use `vette_netcheck`, which is built with the
game. Run it on two computers, or in two windows:
```
vette_netcheck --server wss://vette2026-relay.<your-name>.workers.dev host
vette_netcheck --server wss://vette2026-relay.<your-name>.workers.dev join VETTE-XXXX
```

**Later:**
- **Update the server** after changing it: run the deploy script again.
- **See its traffic:** the Cloudflare dashboard, under **Workers & Pages → vette2026-relay**.
- **Watch its log live:** `npx wrangler tail` (it logs nothing by itself).
- **Change the address:** rename `name` in `wrangler.toml`.

### Doing it by hand

The script runs these four commands:

```
npm ci
npx wrangler login
npx wrangler deploy
```

The address is in the deploy command's last lines (`https://vette2026-relay.<your-name>.workers.dev`).
The game takes it with `wss://` in front instead.

## Building the game with your server

The game's default server is set when it's built. Until one is set, players have to type a server address.

- **Your own build:** `cmake -DVETTE_DEFAULT_SERVER=wss://vette2026-relay.<your-name>.workers.dev ...`
  This writes `net/default_server.h` (`vette::net::kDefaultServer`).
- **GitHub's builds** (every push, and the releases): in the repository on GitHub, open **Settings →
  Secrets and variables → Actions → Variables** and add a repository variable:
  - name: `VETTE_DEFAULT_SERVER`
  - value: the `wss://…` address.

  The build workflow passes it to CMake. It's a variable, not a secret: the address is in every copy of
  the game anyway.

## Invite links

`https://<your server>/join/VETTE-4KQ7` shows a small page. It tries to open the game
(`vette2026://join/VETTE-4KQ7`), and it also has:
- an **Open VETTE! 2026** button;
- the code, with a copy button;
- where to get the game.

`/direct/<direct code>` does the same for direct codes. The page only shows the code: nothing about the
race goes through the server.

The pages load nothing from anywhere else. Their only script is their own (pinned in the
Content-Security-Policy), and they show a code only after checking it against the code alphabet.

## Trying it on your own computer

```
npm ci
npx wrangler dev --port 8787 --var CREATES_PER_10_MIN:1000 --var JOINS_PER_10_MIN:1000
npm test                    # in another terminal: the server's tests
vette_netcheck host         # the default server is ws://127.0.0.1:8787
vette_netcheck join VETTE-XXXX
```

`npm test` runs two test files:
- `test/relay.test.mjs`: create and join, the race settings, relaying in order, refusals (versions,
  codes, a full room), dropping and resuming, leaving;
- `test/invite.test.mjs`: the invite pages, and that nothing from the address gets onto them.

The two `--var` flags lift the per-address limits, which would otherwise stop the tests part way.

`vette_netcheck` has more test options (they're listed in `src/tools/vette_netcheck.cpp`):
- `--drop-at 3:2` drops the connection 3 s in and stays offline 2 s.
- `--no-upgrade` (on either side) keeps the race on the server.
- `--offer 192.0.2.1:26989` (host) offers an address nobody can reach, so you can watch the fallback to
  the server.

## How much the free plan allows

The limit that matters is **100,000 Durable Object requests a day**. It resets at midnight UTC.
Messages coming in to the server count 20 to a request. These aren't charged
([pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/)):
- messages going out;
- pings;
- the runtime's automatic `ping`/`pong` answers;
- **races that went direct**: they don't touch the server after the first few seconds.

| A race through the server (both games sending) | Per room |
|---|---|
| Messages to the server | each game sends at most **30 a second** (the client groups each burst of serial bytes into one message: `Batching` in `src/net/session.h`), so 60 a second |
| Requests | 60 / 20 = **3 a second = 10,800 an hour** |
| Duration | the room stays awake while messages flow: 128 MB x 3,600 s = 461 GB-s an hour (free: 13,000 GB-s a day) |

So the free plan carries about **9 hours of racing through the server a day**, summed over all rooms.
Direct races don't count against this. Everything else is small:
- Joining costs a few requests: the room, the per-address limiter, and a Worker request per connection.
  Workers have their own 100,000 requests a day, which the invite pages also count against.
- A room where nobody is sending (menus, waiting) hibernates and costs nothing.
- A room writes about 10 rows of storage over its life, and the limiter 2 per join (free: 100,000 a day).

If that's ever too little, `Batching::min_interval_us = 66'667` halves the messages (about 18 hours a
day), at the cost of up to 33 ms more delay per packet.

Beyond the allowance, rooms can't be created or joined until midnight UTC (the game shows why). Races
already running through the server may stop. The Workers Paid plan ($5 a month) raises the limits.

## The protocol (version 1)

A game connects with a WebSocket and says who it is in the query. It gives `proto` (the protocol
version), `app` (the VETTE! 2026 version) and `game` (the original game's version and build), and these
must match:

| Path | Who |
|---|---|
| `/v1/create?proto=1&app=…&game=…&settings=…` | the host: creates a room with a new code |
| `/v1/join/<CODE>?proto=1&app=…&game=…` | the friend: joins the room (codes are accepted as `VETTE-4KQ7`, `vette4kq7`, `4KQ7`) |
| `/v1/resume/<CODE>?proto=1&app=…&game=…&token=…` | either, after a dropped connection, within the grace period |
| `GET /join/<CODE>`, `GET /direct/<CODE>` | invite pages (above) |

**From the server** (JSON text):

- `{"t":"welcome","role":"host"|"guest","code":"4KQ7","token":…,"resumed":…,"peer":"none"|"here"|"away","grace":30,"expires":1800,"settings_gen":1,"settings":…}`:
  the first message on every connection.
  - `token` is the player's secret for resuming.
  - A guest's welcome always has the host's race settings, so they arrive before any relayed byte.
- `{"t":"peer","state":"joined"|"away"|"back"|"left","reason":…}`: the other player arrived, dropped
  (and may come back within the grace period), came back, or left for good.
- `{"t":"settings","gen":2,"data":"improved_driving=1"}`: sent to the guest when the host changes the race
  settings. Only newer generations are passed on, so nothing stale ever replaces them.
- `{"t":"error","code":…,"reason":…}`, then a close: a refusal, with a sentence to show the player.
  Codes: `version`, `app`, `game`, `no_room`, `full`, `rate`, `expired`, `busy`, `bad_request`.
- `{"t":"closed","reason":…}`, then a close: the room has closed.
- `pong`: the answer to `ping`, given by the runtime without waking the room.

**From a game:**
- Binary messages, relayed unchanged to the other player. They're dropped while the other player is
  away, and the clients resend. `MessageHeader` in `src/net/protocol.h` describes them.
- Some binary messages are "side messages" between the two games (`kSide`). The host uses one to offer
  its direct addresses, and the guest answers `direct` or `relay`. The server doesn't look at them.
- `ping` every 2 s; `bye` to leave for good.
- From the host only: `{"t":"settings","gen":<n>,"data":"key=value lines"}`.

**Rooms:**
- A code is 4 characters from `23456789ABCDEFGHJKLMNPQRSTUVWXYZ` (no 0/O or 1/I): about a million codes.
- A room that nobody joins closes after 30 minutes.
- A dropped player has 30 seconds to come back.
- When the host leaves (or doesn't come back), the room closes. When the guest leaves, someone else can
  join the host.
- Every room closes after 12 hours.

**Limits:**
- Binary messages up to 2,048 bytes, text up to 1,024.
- 120 messages a second per connection, in bursts of up to 300.
- Race settings up to 512 bytes.
- Per client address, in 10 minutes: 10 rooms created and 30 join attempts (`CREATES_PER_10_MIN` and
  `JOINS_PER_10_MIN` in `wrangler.toml`).
- A connection that goes 20 s without a `ping`, while the other player is sending, is closed as dead.

**What's stored:**
- A room's record (code, versions, race settings, the two tokens, deadlines) lives in its Durable Object
  while the room is open, and is deleted when the room closes.
- The limiter keeps the times of recent creations and joins under a hash of the client's address. They're
  deleted 10 minutes after the last one.
- Nothing is logged.
