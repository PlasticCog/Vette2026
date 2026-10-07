// Puts the relay server on your Cloudflare account and prints its address. Run deploy.cmd (Windows) or
// deploy.sh (macOS, Linux), or: node deploy.mjs
//
// 1. Installs the tools (npm ci), the first time.
// 2. Logs in to Cloudflare in your browser, if you aren't yet.
// 3. Deploys (wrangler deploy) and prints the address to give the game.
//
// The address is also saved in server-address.txt, next to this file.

import { spawn, spawnSync } from "node:child_process";
import { existsSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const windows = process.platform === "win32";
const npm = windows ? "npm.cmd" : "npm";
const npx = windows ? "npx.cmd" : "npx";

function say(text = "") {
  console.log(text);
}

function fail(text) {
  say();
  say(`!! ${text}`);
  process.exit(1);
}

// Runs a command with its output on screen; resolves with its exit code and what it printed.
function run(command, args) {
  return new Promise((resolve) => {
    let output = "";
    const child = spawn(command, args, { cwd: here, shell: windows, stdio: ["inherit", "pipe", "pipe"] });
    child.stdout.on("data", (d) => {
      output += d;
      process.stdout.write(d);
    });
    child.stderr.on("data", (d) => {
      output += d;
      process.stderr.write(d);
    });
    child.on("close", (code) => resolve({ code, output }));
    child.on("error", () => resolve({ code: -1, output }));
  });
}

const [major, minor] = process.versions.node.split(".").map(Number);
if (major < 20 || (major === 20 && minor < 3)) {
  fail(`Node.js 20.3 or newer is needed (this is ${process.versions.node}). Get it from https://nodejs.org/`);
}

say("== VETTE! 2026 relay server: deploying to Cloudflare ==");
say();

if (!existsSync(join(here, "node_modules", "wrangler"))) {
  say("-- Installing the tools (once)...");
  if ((await run(npm, ["ci", "--no-fund", "--no-audit"])).code !== 0) {
    fail("Installing the tools failed (see above). Check your internet connection and try again.");
  }
}

say("-- Checking your Cloudflare login...");
const whoami = spawnSync(npx, ["wrangler", "whoami"], { cwd: here, shell: windows, encoding: "utf8" });
if (whoami.status !== 0 || /not authenticated/i.test(`${whoami.stdout}${whoami.stderr}`)) {
  say("-- Logging in: your browser opens; sign in to Cloudflare (or create a free account) and allow access.");
  if ((await run(npx, ["wrangler", "login"])).code !== 0) {
    fail("The login didn't finish. Run this again to retry.");
  }
}

say("-- Deploying...");
const deploy = await run(npx, ["wrangler", "deploy"]);
if (deploy.code !== 0) {
  if (/workers\.dev subdomain/i.test(deploy.output)) {
    fail("Your account needs a workers.dev subdomain first: open https://dash.cloudflare.com, go to " +
         "Workers & Pages, pick a subdomain, then run this again.");
  }
  fail("The deploy failed (see above).");
}
const https = deploy.output.match(/https:\/\/[a-z0-9.-]+\.workers\.dev/i)?.[0];
if (!https) {
  fail("Deployed, but the address wasn't in the output. Find it in the Cloudflare dashboard under " +
       "Workers & Pages > vette2026-relay.");
}
const wss = https.replace(/^https:/, "wss:");
writeFileSync(join(here, "server-address.txt"), `${wss}\n`);

const line = "=".repeat(Math.max(48, wss.length + 4));
say();
say(line);
say("  Your VETTE! 2026 server is running.");
say();
say("  Its address (the game's \"Server\" setting):");
say(`    ${wss}`);
say();
say("  Invite links look like:");
say(`    ${https}/join/VETTE-4KQ7`);
say();
say("  To make it the default in your own builds of the game:");
say(`    cmake -DVETTE_DEFAULT_SERVER=${wss} ...`);
say("  (or set the repository variable VETTE_DEFAULT_SERVER on GitHub; see README.md)");
say(line);
say(`(Saved in ${join(here, "server-address.txt")}.)`);
