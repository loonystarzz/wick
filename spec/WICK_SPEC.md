# Wick — Ecosystem Specification

Wick is a minimal ecosystem for plaintext browsing in the terminal.
It exists as a quiet alternative to the modern web — no images, no scripts,
no layout engines. Just text, links, and color.

---

## Philosophy

The web is heavy. Even simple pages carry megabytes of markup, fonts,
and JavaScript just to display a few paragraphs. Wick is a rejection of that.

Wick is for people who want to write and read documents in a terminal,
navigate between them with a keypress, and not think about anything else.
It is not trying to replace the web. It is trying to ignore most of it.

---

## Components

Wick is made up of three parts:

**Wax** is the document format. A Wax file is a plain text file with
a small set of line types: headers, subheaders, links, and text.
Text lines support inline color codes. Nothing else.
Files use the `.wax` extension. See `WAX_SPEC.md` for the full format.

**Wick** is the protocol and ecosystem name. It defines how Wax documents
are served and linked — locally by file path, or remotely over HTTP.
There is no custom protocol. A Wick site is just Wax files on a server.

**Den** is the reference browser. It runs in a terminal, renders Wax documents,
and lets you navigate by pressing number keys to follow links or Space to
type a URL. It is built with ncurses and libcurl. See the Den source for details.

---

## Design constraints

- **No binary formats.** Everything is plain text. Wax files are readable
  without a browser.
- **No scripting.** Documents are static. There is no way to embed logic
  or dynamic content.
- **No images.** Terminal cells are characters, not pixels.
- **No custom protocol.** Wax documents are served over HTTP/HTTPS or
  read from local files. No new port, no new handshake.
- **Keyboard only.** Den has no mouse support. Navigation is number keys,
  scroll keys, and one key to quit.

---

## A Wick site

A Wick site is a collection of `.wax` files, usually with an `index.wax`
as the entry point. Files link to each other with `=>` lines. That's it.
Hosting one is the same as hosting any static files.

---

## What Wick is not

Wick is not trying to be Gemini, Gopher, or a web replacement.
It makes no claims about privacy, federation, or the future of the internet.
It is a small, self-contained thing for people who like small, self-contained things.
