CanuckWeb v1.00

"History made on 512 kilobytes."


On February 20, 2026, a $30 microcontroller browsed the internet.
Not a Raspberry Pi. Not a Linux board. An ESP32-S3 — a chip smaller than a fingernail, with 512KB of RAM and no operating system — searched DuckDuckGo, fetched live web pages, followed links, and kept history. It ran a real browser. Nobody had done this before.
This is that browser.

What It Runs On
LilyGo T-Deck — ESP32-S3 dual-core @ 240MHz, 512KB SRAM, 8MB PSRAM, 16MB Flash, 2.8" 320×240 display, QWERTY keyboard, optical trackball. A device that fits in your pocket and costs less than a dinner out.

How It Works
The modern web was never meant to run on hardware like this. Direct page fetches crash the chip instantly — a single modern homepage can be 3MB of JavaScript and the ESP32 has 512KB total. Every major site blocks anything that isn't a real browser on contact.
CanuckWeb solves this with a three-layer pipeline. Searches hit lite.duckduckgo.com — a no-JavaScript plain HTML endpoint that responds to a raw encrypted POST request. When you open a result, the request goes through r.jina.ai, which fetches the page, strips everything except the actual content, and sends back a few kilobytes of clean readable text instead of megabytes of markup. If a page is paywalled or blocked, the Wayback Machine archive fills in automatically. The chip never touches a raw webpage. It only ever sees pre-processed text that fits in memory. That's what made it possible.

Controls
Type + ENTERSearchTrackballScroll results and pagesClick or ENTEROpen resultNGo to URL directlyBBackSNew searchQRestart

Flash It
Option 1 — VSCode + PlatformIO
Download the zip, open it in VSCode, connect your T-Deck, hit Upload.
Option 2 — Flash the bin directly
No setup needed. Go to espressoflash.com, connect your T-Deck via USB, upload the CanuckWeb_v1.00.bin from the Releases section. Done in 60 seconds.

