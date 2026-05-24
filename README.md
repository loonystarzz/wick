# wick

A minimal terminal web ecosystem. No images, no scripts, no layout engines — just text, links, and color.

Wick is a quiet alternative to the modern web. A wick site is a collection of plain `.wax` files served over HTTP or read from disk. A wick browser is a terminal program that renders them. That's the whole thing.

---

## Components

| | |
|---|---|
| **wax** | The document format. Plain text with a small set of line types. |
| **wick** | The protocol and ecosystem name. Wax files over HTTP or local paths. |
| **den** | The reference browser. Written in C with ncurses and libcurl. |

---

## The wax format

A `.wax` file is a plain text file. Every line is one of four types, determined by how it starts:

```
#Header text              → bold, indented 4 spaces
##Subheader text          → bold, indented 2 spaces
=> URL label text         → a numbered link: [1] label text
anything else             → plain text
```

Links can point to local files or remote URLs:

```
=> notes.wax              My notes
=> https://example.com/   Example site
=> https://example.com/data.csv#data.csv   Download dataset
```

A link URL ending with `#filename` (no spaces) is a **download link** — the browser will prompt before saving the file to `~/Downloads/`.

### Inline color

Text lines support inline color codes with `$$x` syntax:

```
this is $$agreen$$r and this is $$cred$$r and back to normal
```

| Code | Color | Code | Color |
|------|-------|------|-------|
| `0` | Black | `8` | Dark gray |
| `1` | Dark blue | `9` | Blue |
| `2` | Dark green | `a` | Green |
| `3` | Dark cyan | `b` | Cyan |
| `4` | Dark red | `c` | Red |
| `5` | Dark magenta | `d` | Magenta |
| `6` | Gold/orange | `e` | Yellow |
| `7` | Gray | `f` | White |
| `r` | Reset | | |

Color codes only work in text lines — not in headers, subheaders, or link labels. Color state resets at the end of each line.

### MIME type

```
text/x-wax
```

---

## den — the reference browser

Den is the official wick browser. It runs in any standard terminal.

**Build:**

```sh
gcc -Wall -Wextra -o den den.c -lncurses -lcurl
```

**Run:**

```sh
./den                          # opens home page
./den path/to/file.wax         # open a local file
./den https://example.com/index.wax
```

**Controls:**

| Key | Action |
|-----|--------|
| `1`–`9` | Follow that numbered link |
| `Space` | Open URL bar |
| `j` / `k` or `↑` / `↓` | Scroll line by line |
| `PgUp` / `PgDn` | Scroll page by page |
| `g` / `G` or `Home` / `End` | Jump to top / bottom |
| `h` | Go to home (`den://home`) |
| `q` | Quit |

Links with `#filename` in the URL trigger a download prompt instead of navigation. Den shows the filename, file size, and destination before downloading.

---

## A wick site

A wick site is just `.wax` files on a web server, usually with an `index.wax` as the entry point. Hosting one is no different from hosting static files.

```
index.wax
about.wax
posts/
  first-post.wax
  second-post.wax
```

---

## Projects

### wickypedia

A Wikipedia-to-wax converter. Fetches Wikipedia articles and serves them as wax documents so you can browse Wikipedia through den.

**Requires:** Python 3, `beautifulsoup4`

```sh
pip install beautifulsoup4
```

**CLI — convert a single article:**

```sh
python3 wickypedia.py "Python (programming language)"
python3 wickypedia.py "Ada Lovelace" -o ada.wax
python3 wickypedia.py "Gopher protocol" -l en -o gopher.wax
```

**Server — browse Wikipedia live through den:**

```sh
python3 wickypedia.py --serve           # starts on port 8421
python3 wickypedia.py --serve --port 9000
```

Then in den, navigate to:

```
http://localhost:8421/Ada_Lovelace
http://localhost:8421/Python_(programming_language)
http://localhost:8421/?q=black+holes    ← search
```

Spaces in article titles can be written as underscores or `%20`. Links inside articles point back to the local server automatically.

---

## Design constraints

- **No binary formats.** Wax files are readable as plain text without a browser.
- **No scripting.** Documents are static.
- **No images.** Terminal cells are characters.
- **No custom protocol.** Wax is served over HTTP/HTTPS or read from local files.
- **Keyboard only.** No mouse support in den.
- **Lightweight.** Should run fine on any low-end machine from the past 15 years.

---

## What wick is not

Wick is not trying to be Gemini, Gopher, or a web replacement. It makes no claims about privacy, federation, or the future of the internet. It is a small, self-contained thing for people who like small, self-contained things.

---

## License

MIT — see [LICENSE](LICENSE).
