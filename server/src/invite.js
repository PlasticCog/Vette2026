// Invite links: https://<server>/join/VETTE-4KQ7 and https://<server>/direct/7K3M-QX9P-2HDA. A small page
// that opens the game (vette2026://join/..., vette2026://direct/...) and shows the code to paste. Nothing
// goes through the server for a direct code: the page only displays it. Codes are checked against the
// alphabet (and a direct code's check character) before they're put on the page, and escaped anyway.

const ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
const RELEASES = "https://github.com/PlasticCog/Vette2026/releases";

// "vette-4kq7" -> "VETTE-4KQ7"; null if it isn't a room code.
export function roomCode(typed) {
  let c = String(typed ?? "").toUpperCase().replace(/[\s_-]/g, "");
  if (c.startsWith("VETTE") && c.length > 4) {
    c = c.slice(5);
  }
  return c.length === 4 && [...c].every((ch) => ALPHABET.includes(ch)) ? `VETTE-${c}` : null;
}

// A direct code (12 or 16 characters, the last a Luhn mod 32 check character) in groups of 4; null if it
// isn't one. Same rules as src/net/invite.cpp.
export function directCode(typed) {
  const c = String(typed ?? "").toUpperCase().replace(/[\s_-]/g, "");
  if ((c.length !== 12 && c.length !== 16) || ![...c].every((ch) => ALPHABET.includes(ch))) {
    return null;
  }
  const values = [...c].map((ch) => ALPHABET.indexOf(ch));
  const check = values.pop();
  let factor = 2;
  let sum = 0;
  for (let i = values.length - 1; i >= 0; i--) {
    const addend = factor * values[i];
    factor = factor === 2 ? 1 : 2;
    sum += Math.floor(addend / 32) + (addend % 32);
  }
  if ((32 - (sum % 32)) % 32 !== check) {
    return null;
  }
  return c.match(/.{4}/g).join("-");
}

function escapeHtml(text) {
  return String(text).replace(/[&<>"']/g, (ch) => `&#${ch.charCodeAt(0)};`);
}

// The page's only script: opens the game once, and copies the code. It reads the code from the page, so
// it never changes, and its hash goes in the Content-Security-Policy.
export const PAGE_SCRIPT =
  "const b=document.body,c=document.getElementById('copy');" +
  "if(b.dataset.link)setTimeout(()=>{location.href=b.dataset.link},300);" +
  "if(c)c.addEventListener('click',()=>{const done=()=>{c.textContent='Copied'};" +
  "navigator.clipboard?navigator.clipboard.writeText(b.dataset.code).then(done,()=>{}):0;" +
  "const r=document.createRange();r.selectNodeContents(document.getElementById('code'));" +
  "const s=getSelection();s.removeAllRanges();s.addRange(r)});";

let scriptHash = null;
async function pageScriptHash() {
  if (!scriptHash) {
    const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(PAGE_SCRIPT));
    scriptHash = btoa(String.fromCharCode(...new Uint8Array(digest)));
  }
  return scriptHash;
}

const STYLE = `
:root{color-scheme:dark light;--bg:#101114;--panel:#1b1d22;--text:#f2f2f2;--dim:#a9adb6;--red:#d0182f;--line:#30333b}
@media (prefers-color-scheme:light){:root{--bg:#f4f4f6;--panel:#fff;--text:#16171a;--dim:#5b5f68;--line:#dcdde2}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:560px;margin:0 auto;padding:40px 16px}
h1{font-size:15px;letter-spacing:.2em;text-transform:uppercase;color:var(--red);margin:0 0 8px}
h2{font-size:26px;line-height:1.25;margin:0 0 28px}
.open{display:block;text-align:center;background:var(--red);color:#fff;text-decoration:none;font-weight:700;font-size:20px;padding:16px;border-radius:10px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:20px;margin:24px 0;text-align:center}
.label{color:var(--dim);font-size:14px;margin-bottom:6px}
#code{font:700 clamp(26px,8vw,40px)/1.2 ui-monospace,"Cascadia Mono",Consolas,monospace;letter-spacing:.06em;word-break:break-all;user-select:all}
button{margin-top:14px;font:inherit;font-weight:600;padding:8px 18px;border-radius:8px;border:1px solid var(--line);background:transparent;color:var(--text);cursor:pointer}
p{color:var(--dim);margin:10px 0}a{color:inherit}`;

// `heading` is one of the fixed texts below; everything else is escaped.
function page({ title, heading, code, link, valid }) {
  const body = valid
    ? `<a class="open" href="${escapeHtml(link)}">Open VETTE! 2026</a>
<div class="card"><div class="label">The code</div><div id="code">${escapeHtml(code)}</div>
<button id="copy" type="button">Copy code</button></div>
<p>Don't have it? Get VETTE! 2026 at <a href="${RELEASES}">${RELEASES}</a> (you also need your own copy of DOS VETTE!).</p>
<p>In the game: Online race &gt; Join a race, paste the code.</p>`
    : `<p>This invite link isn't complete or has a typo. Ask your friend for the code, or for the link again.</p>
<p>Get VETTE! 2026 at <a href="${RELEASES}">${RELEASES}</a>.</p>`;
  const data = valid ? ` data-code="${escapeHtml(code)}" data-link="${escapeHtml(link)}"` : "";
  return `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="robots" content="noindex"><title>${escapeHtml(title)}</title><style>${STYLE}</style></head>
<body${data}><main><h1>VETTE! 2026</h1><h2>${heading}</h2>
${body}</main>${valid ? `<script>${PAGE_SCRIPT}</script>` : ""}</body></html>`;
}

// The page for /join/<code> ("join") or /direct/<code> ("direct").
export async function invitePage(kind, typed) {
  const code = kind === "join" ? roomCode(typed) : directCode(typed);
  const valid = code !== null;
  const html = page({
    title: valid ? `VETTE! 2026: join ${code}` : "VETTE! 2026: invite",
    heading: valid ? "Your friend invited you to an online race" : "That invite link doesn't work",
    code,
    link: valid ? `vette2026://${kind}/${code}` : null,
    valid,
  });
  return new Response(html, {
    status: valid ? 200 : 404,
    headers: {
      "content-type": "text/html; charset=utf-8",
      // Short: the page is the same for every visit, but codes come and go.
      "cache-control": "public, max-age=300",
      "content-security-policy":
        `default-src 'none'; style-src 'unsafe-inline'; script-src 'sha256-${await pageScriptHash()}'; ` +
        "base-uri 'none'; form-action 'none'; frame-ancestors 'none'",
      "x-content-type-options": "nosniff",
      "referrer-policy": "no-referrer",
    },
  });
}
