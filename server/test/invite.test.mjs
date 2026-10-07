// Tests of the invite pages (/join/<code>, /direct/<code>) against `wrangler dev`:
//
//   npx wrangler dev --port 8787
//   node test/invite.test.mjs           (npm test runs it after relay.test.mjs)
//
// The page must open the game, show the code, run only its own script, and never put anything from
// the address on the page that isn't a valid code.

import { createHash } from "node:crypto";
import { PAGE_SCRIPT, directCode, roomCode } from "../src/invite.js";

const BASE = (process.env.RELAY_URL ?? "ws://127.0.0.1:8787").replace(/^ws/, "http");

let failures = 0;
let checks = 0;
function check(cond, what) {
  checks++;
  if (!cond) {
    failures++;
    console.log(`  FAIL ${what}`);
  }
}

async function get(path) {
  const r = await fetch(`${BASE}${path}`, { redirect: "manual" });
  return { status: r.status, headers: r.headers, body: await r.text() };
}

// Codes made by the game (src/net/invite.cpp): 192.168.1.241:26989, and 127.0.0.1:26995.
const SHORT = "G2P2-2Z4U-SZDQ";
const LONG = "MZ22-224U-DMVM-3D3U";

async function main() {
  // The code rules, as the game has them.
  check(roomCode("vette-4kq7") === "VETTE-4KQ7" && roomCode("4KQ7") === "VETTE-4KQ7", "room codes");
  check(roomCode("VETTE-40I7") === null && roomCode("<b>") === null, "not room codes");
  check(directCode(SHORT.toLowerCase().replaceAll("-", "")) === SHORT && directCode(LONG) === LONG, "direct codes");
  check(directCode("G2P2-2Z4U-SZDR") === null, "a direct code with a typo");

  // A room invite.
  let r = await get("/join/vette-4kq7");
  check(r.status === 200 && r.headers.get("content-type").startsWith("text/html"), `join page (${r.status})`);
  check(r.body.includes('href="vette2026://join/VETTE-4KQ7"'), "the Open button");
  check(r.body.includes('data-link="vette2026://join/VETTE-4KQ7"'), "opened on load");
  check(r.body.includes('<div id="code">VETTE-4KQ7</div>'), "the code, canonical");
  check(r.body.includes("Your friend invited you to an online race"), "the heading");
  check(r.body.includes("https://github.com/PlasticCog/Vette2026/releases") &&
        r.body.includes("you also need your own copy of DOS VETTE!"), "where to get the game");
  check(r.body.includes("Online race &gt; Join a race, paste the code"), "how to join in the game");
  const maxAge = Number((r.headers.get("cache-control") ?? "").match(/max-age=(\d+)/)?.[1]);
  check(maxAge > 0 && maxAge <= 600, `short caching (${r.headers.get("cache-control")})`);
  const csp = r.headers.get("content-security-policy") ?? "";
  const hash = createHash("sha256").update(PAGE_SCRIPT).digest("base64");
  check(csp.includes("default-src 'none'") && csp.includes(`script-src 'sha256-${hash}'`), `CSP (${csp})`);
  const scripts = [...r.body.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
  check(scripts.length === 1 && scripts[0] === PAGE_SCRIPT, "only the page's own script");
  check(!/\b(src|href)="(https?:)?\/\//.test(r.body.replace(/href="https:\/\/github\.com\/PlasticCog\/Vette2026\/releases"/g, "")),
        "no other external resources");
  check(r.headers.get("x-content-type-options") === "nosniff", "nosniff");

  // A direct invite: the page only shows it.
  r = await get(`/direct/${LONG.toLowerCase()}`);
  check(r.status === 200 && r.body.includes(`href="vette2026://direct/${LONG}"`) &&
        r.body.includes(`<div id="code">${LONG}</div>`), "direct page");
  r = await get(`/direct/${SHORT.replaceAll("-", "")}`);
  check(r.status === 200 && r.body.includes(`vette2026://direct/${SHORT}`), "direct page, short code");

  // Anything else: a page saying so, with nothing from the address on it, and no script.
  for (const bad of [
    "/join/%3Cscript%3Ealert(1)%3C%2Fscript%3E",
    "/join/VETTE-4KQ7%22%3E%3Cimg%20src%3Dx%3E",
    "/join/%E0%A4%A",
    "/direct/G2P2-2Z4U-SZDR",
    "/direct/javascript:alert(1)",
    "/join/" + "A".repeat(300),
  ]) {
    r = await get(bad);
    check(r.status === 404, `${bad}: 404 (${r.status})`);
    check(r.body.includes("That invite link doesn't work"), `${bad}: says so`);
    check(!/<script|<img|alert|javascript:|vette2026:\/\//i.test(r.body), `${bad}: nothing injected`);
  }
}

const started = Date.now();
await main().catch((e) => {
  failures++;
  console.log(`  FAIL ${e.stack}`);
});
console.log(`${checks} checks, ${failures} failure(s), ${Date.now() - started} ms`);
process.exit(failures ? 1 : 0);
