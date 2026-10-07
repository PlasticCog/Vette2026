# Third-party software in VETTE! 2026

| Component | Use | License |
|---|---|---|
| [SDL 3](https://github.com/libsdl-org/SDL) | Window, rendering, audio, input | zlib (`licenses/SDL3.txt`) |
| [ymfm](https://github.com/aaronsgiles/ymfm) by Aaron Giles | YM3812 (AdLib) and YM2203 (PC-98) FM emulation | BSD 3-Clause (`licenses/ymfm.txt`) |
| [font8x8](https://github.com/dhepper/font8x8) by Daniel Hepper, from Marcel Sondaar's IBM public-domain VGA fonts | Launch menu text | Public domain |
| [libcurl](https://curl.se/) 8.14.1 by Daniel Stenberg and contributors | Online play: the TCP and TLS connection to the relay server | curl license, MIT-style (`licenses/curl.txt`) |
| [OpenSSL](https://www.openssl.org/) 3 (Linux packages only, linked from Ubuntu's libssl) | Online play: TLS on Linux | Apache License 2.0 (`licenses/OpenSSL.txt`) |
| [miniupnpc](https://miniupnp.tuxfamily.org/) 2.3.3 by Thomas Bernard | Online play: opening a port on the home router for direct codes (UPnP IGD) | BSD 3-Clause (`licenses/miniupnpc.txt`) |
| [libnatpmp](https://github.com/miniupnp/libnatpmp) by Thomas Bernard | Online play: the same with NAT-PMP, and finding the router for PCP | BSD 3-Clause (`licenses/libnatpmp.txt`) |

Online play's TLS uses the operating system's own on Windows (Schannel) and macOS (Secure Transport).
Direct connections use STUN servers (Cloudflare's, Google's) only to learn this computer's internet
address. The relay server in `server/` is deployed by whoever runs it; it is not part of the packages, and its
development tool, Wrangler (Cloudflare, MIT or Apache 2.0), is not distributed.

VETTE! 2026 contains no files from the original VETTE! releases. It reads them from the player's own
copy of the game.
