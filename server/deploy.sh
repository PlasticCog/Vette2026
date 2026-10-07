#!/bin/sh
# Puts the VETTE! 2026 relay server on your Cloudflare account and prints its address (deploy.mjs).
# Run: sh deploy.sh   (needs Node.js 20.3 or newer: https://nodejs.org/)
cd "$(dirname "$0")" || exit 1
if ! command -v node >/dev/null 2>&1; then
    echo "Node.js isn't installed. Get it from https://nodejs.org/ (the LTS version), then run this again."
    exit 1
fi
exec node deploy.mjs
