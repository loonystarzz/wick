#!/usr/bin/env python3
"""
wickwiki.py — Wikipedia → Wax converter for the Wick ecosystem

MODES:
  CLI:    python3 wickwiki.py "Article Title" [-o out.wax] [-l en]
  Server: python3 wickwiki.py --serve [--port 8421] [-l en]

In server mode, Den (or any browser) can navigate to:
  http://localhost:8421/Article_Title
  http://localhost:8421/Python_(programming_language)

Spaces in titles can be given as underscores or %20.
The root / serves a Wax home page for the server itself.
"""

import sys
import re
import argparse
import urllib.parse
import urllib.request
import json
from http.server import HTTPServer, BaseHTTPRequestHandler
from bs4 import BeautifulSoup, Tag, NavigableString

# ── constants ────────────────────────────────────────────────────────────────

USER_AGENT   = "wickwiki/1.0 (Wick ecosystem; https://github.com/wick)"
DEFAULT_PORT = 8421
DEFAULT_LANG = "en"

# Sections to drop entirely — noise, not content
SKIP_SECTIONS = {
    "references", "external links", "see also", "notes",
    "further reading", "footnotes", "bibliography", "citations",
    "sources", "works cited",
}

# ── Wikipedia fetch ──────────────────────────────────────────────────────────

def wiki_api_url(lang, article):
    encoded = urllib.parse.quote(article, safe="()_")
    return (
        f"https://{lang}.wikipedia.org/w/api.php"
        f"?action=parse&page={encoded}&prop=text|displaytitle"
        f"&formatversion=2&format=json"
    )

def fetch_article(lang, article):
    """
    Fetch article HTML and display title from Wikipedia API.
    Returns (title_str, html_str) or raises RuntimeError.
    """
    url = wiki_api_url(lang, article)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        raise RuntimeError(f"network error: {e}")

    if "error" in data:
        info = data["error"].get("info", "unknown error")
        raise RuntimeError(f"Wikipedia API: {info}")

    parse = data.get("parse", {})
    title = parse.get("displaytitle", article)
    # strip HTML tags from display title (e.g. italics)
    title = re.sub(r"<[^>]+>", "", title)
    html  = parse.get("text", "")
    if not html:
        raise RuntimeError("empty response from Wikipedia")
    return title, html

# ── HTML → Wax conversion ────────────────────────────────────────────────────

def node_to_text(node):
    """Recursively extract plain text from a BS4 node."""
    if isinstance(node, NavigableString):
        return str(node)
    parts = []
    for child in node.children:
        parts.append(node_to_text(child))
    return "".join(parts)

def wrap_lines(text, width=72):
    """Word-wrap a paragraph to `width` columns."""
    words = text.split()
    if not words:
        return []
    lines = []
    cur = []
    cur_len = 0
    for w in words:
        wl = len(w)
        if cur and cur_len + 1 + wl > width:
            lines.append(" ".join(cur))
            cur = [w]
            cur_len = wl
        else:
            cur.append(w)
            cur_len = cur_len + 1 + wl if cur_len else wl
    if cur:
        lines.append(" ".join(cur))
    return lines

def collect_paragraph_links(el, base_url):
    """Return list of (label, url) for wiki-internal links in a <p>."""
    result = []
    seen = set()
    for a in el.find_all("a", href=True):
        href = a["href"]
        label = a.get_text(" ", strip=True)
        if not label:
            continue
        # only internal wiki article links, skip special namespaces
        if href.startswith("/wiki/") and ":" not in href[6:]:
            article = href[6:]
            url = f"{base_url}/{article}"
            key = (label.lower(), url)
            if key not in seen:
                seen.add(key)
                result.append((label, url))
    return result

def convert_html_to_wax(html, title, base_url):
    """
    Convert Wikipedia article HTML to a Wax document string.
    base_url is used to build links to other articles
    (e.g. "http://localhost:8421" in server mode, or "" to skip links).
    """
    soup = BeautifulSoup(html, "html.parser")

    # Remove unwanted clutter before parsing
    for sel in [
        ".mw-editsection",      # [edit] buttons
        ".reference",           # inline citation numbers [1]
        ".reflist",             # reference list
        ".navbox",              # navigation boxes
        ".infobox",             # side infoboxes (tables)
        ".thumb",               # image thumbnails
        ".toc",                 # table of contents
        ".mw-empty-elt",
        "style", "script",
        "[role=note]",
    ]:
        for tag in soup.select(sel):
            tag.decompose()

    content = soup.find("div", class_="mw-parser-output")
    if not content:
        content = soup

    out   = []     # output lines
    links = []     # accumulated (url, label) for link section
    seen_links = set()
    skip = False   # True when inside a skipped section

    def emit(line=""):
        out.append(line)

    def add_link(url, label):
        key = (url, label)
        if key not in seen_links:
            seen_links.add(key)
            links.append((url, label))

    # ── title ────────────────────────────────────────────────────────────────
    emit(f"#{title}")
    emit()

    # ── walk top-level elements ──────────────────────────────────────────────
    for el in content.children:
        if not isinstance(el, Tag):
            continue
        tag = el.name

        # ── headings ─────────────────────────────────────────────────────────
        if tag in ("h2", "h3", "h4", "h5"):
            heading = el.get_text(" ", strip=True)
            heading = re.sub(r"\s*\[edit\]", "", heading).strip()
            if not heading:
                continue
            if heading.lower() in SKIP_SECTIONS:
                skip = True
                continue
            skip = False
            emit()
            if tag == "h2":
                emit(f"##{heading}")
            else:
                # indent sub-headings slightly within the subheader style
                emit(f"##  {heading}")
            emit()
            continue

        if skip:
            continue

        # ── paragraphs ────────────────────────────────────────────────────────
        if tag == "p":
            text = el.get_text(" ", strip=True)
            # clean up citation artifacts like [1] and extra spaces
            text = re.sub(r"\[\d+\]", "", text)
            text = re.sub(r" (['',;:.!?)])", r"\1", text)  # space before punctuation
            text = re.sub(r"\s{2,}", " ", text).strip()
            if not text:
                continue
            for line in wrap_lines(text):
                emit(line)
            emit()
            if base_url:
                for label, url in collect_paragraph_links(el, base_url):
                    add_link(url, label)

        # ── unordered / ordered lists ─────────────────────────────────────────
        elif tag in ("ul", "ol"):
            items = el.find_all("li", recursive=False)
            for li in items:
                text = li.get_text(" ", strip=True)
                text = re.sub(r"\[\d+\]", "", text)
                text = re.sub(r"\s{2,}", " ", text).strip()
                if text:
                    for i, line in enumerate(wrap_lines(text, width=68)):
                        emit(f"  {line}" if i == 0 else f"    {line}")
            emit()

        # ── definition lists (used a lot on Wikipedia) ────────────────────────
        elif tag == "dl":
            for child in el.children:
                if not isinstance(child, Tag):
                    continue
                text = child.get_text(" ", strip=True)
                text = re.sub(r"\[\d+\]", "", text).strip()
                if child.name == "dt" and text:
                    emit(f"$$f{text}$$r")
                elif child.name == "dd" and text:
                    for i, line in enumerate(wrap_lines(text, width=68)):
                        emit(f"  {line}" if i == 0 else f"    {line}")
            emit()

        # ── blockquotes ───────────────────────────────────────────────────────
        elif tag == "blockquote":
            text = el.get_text(" ", strip=True)
            text = re.sub(r"\[\d+\]", "", text).strip()
            if text:
                emit("$$7---")
                for line in wrap_lines(text, width=68):
                    emit(f"  $$7{line}$$r")
                emit("$$7---$$r")
                emit()

    # ── link section ─────────────────────────────────────────────────────────
    if links:
        emit()
        emit("##Links in this article")
        emit()
        for url, label in links:
            emit(f"=> {url} {label}")

    return "\n".join(out)

# ── Wikipedia search ─────────────────────────────────────────────────────────

def search_wikipedia(lang, query, limit=10):
    """
    Use Wikipedia's opensearch API to find articles matching query.
    Returns list of (title, description, url) tuples.
    """
    params = urllib.parse.urlencode({
        "action":    "opensearch",
        "search":    query,
        "limit":     limit,
        "namespace": 0,
        "format":    "json",
    })
    url = f"https://{lang}.wikipedia.org/w/api.php?{params}"
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        raise RuntimeError(f"search failed: {e}")

    # opensearch returns [query, [titles], [descriptions], [urls]]
    if not isinstance(data, list) or len(data) < 4:
        return []
    titles       = data[1]
    descriptions = data[2]
    urls         = data[3]
    results = []
    for t, d, u in zip(titles, descriptions, urls):
        results.append((t, d, u))
    return results

def search_results_to_wax(query, results, base_url):
    """Render search results as a Wax document."""
    out = []
    out.append(f"#Search: {query}")
    out.append("")
    if not results:
        out.append("No results found.")
        out.append("")
        out.append(f"=> {base_url}/ wickwiki home")
        return "\n".join(out)

    out.append(f"$$7{len(results)} results for $$f{query}$$r")
    out.append("")
    for title, desc, _url in results:
        article_key = title.replace(" ", "_")
        link_url    = f"{base_url}/{urllib.parse.quote(article_key)}"
        out.append(f"=> {link_url} {title}")
        if desc:
            # wrap description, indented
            for i, line in enumerate(wrap_lines(desc, width=66)):
                out.append(f"  $$7{line}$$r")
        out.append("")

    out.append(f"=> {base_url}/ wickwiki home")
    return "\n".join(out)


def cmd_fetch(article, lang, outfile):
    # normalise: underscores → spaces for display, keep underscores for API
    article_key = article.replace(" ", "_")
    print(f"fetching: {lang}.wikipedia.org/wiki/{article_key} ...", file=sys.stderr)
    title, html = fetch_article(lang, article_key)
    print(f"converting: {title}", file=sys.stderr)
    wax = convert_html_to_wax(html, title, base_url="")
    if outfile:
        with open(outfile, "w", encoding="utf-8") as f:
            f.write(wax)
        print(f"written: {outfile}", file=sys.stderr)
    else:
        print(wax)

# ── HTTP server mode ─────────────────────────────────────────────────────────

SERVER_HOME = """\
#wickwiki
##Wikipedia through the Wick lens

wickwiki is a local server that fetches Wikipedia articles and serves
them as Wax documents for Den.

##search

To search Wikipedia, add $$b?q=$$r and your query to the URL bar:

  {base_url}/?q=Ada+Lovelace
  {base_url}/?q=black+holes
  {base_url}/?q=python+programming

##read an article directly

If you know the title, navigate straight to it:

  {base_url}/Python_(programming_language)
  {base_url}/Ada_Lovelace
  {base_url}/Gopher_(protocol)

Spaces can be written as underscores or %20.
Links inside articles point back here automatically.

##language

This server is reading $$f{lang}$$r.wikipedia.org.
"""

def make_handler(port, lang):
    base_url = f"http://localhost:{port}"

    class WikiHandler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            # quieter logging
            print(f"  {self.address_string()} {fmt % args}", file=sys.stderr)

        def send_wax(self, body, status=200):
            data = body.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "text/x-wax; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def send_error_wax(self, msg, status=404):
            body = f"#Error\n\n{msg}\n\n=> {base_url}/ wickwiki home\n"
            self.send_wax(body, status)

        def do_GET(self):
            parsed   = urllib.parse.urlparse(self.path)
            path     = urllib.parse.unquote(parsed.path.lstrip("/")).strip()
            qs       = urllib.parse.parse_qs(parsed.query)

            # root with ?q= → search
            if not path or path == "/":
                query = qs.get("q", [""])[0].strip()
                if query:
                    try:
                        results = search_wikipedia(lang, query)
                        wax = search_results_to_wax(query, results, base_url)
                    except RuntimeError as e:
                        wax = f"#Search error\n\n{e}\n\n=> {base_url}/ wickwiki home\n"
                    self.send_wax(wax)
                else:
                    home = SERVER_HOME.format(base_url=base_url, lang=lang)
                    self.send_wax(home)
                return

            # path → article title
            article = path.replace(" ", "_")
            try:
                title, html = fetch_article(lang, article)
                wax = convert_html_to_wax(html, title, base_url)
                self.send_wax(wax)
            except RuntimeError as e:
                self.send_error_wax(str(e))

    return WikiHandler

def cmd_serve(port, lang):
    handler = make_handler(port, lang)
    server  = HTTPServer(("", port), handler)
    base    = f"http://localhost:{port}"
    print(f"wickwiki running on {base}", file=sys.stderr)
    print(f"language: {lang}.wikipedia.org", file=sys.stderr)
    print(f"open Den and navigate to {base}/Article_Name", file=sys.stderr)
    print(f"press Ctrl+C to stop", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped.", file=sys.stderr)

# ── entry point ───────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="wickwiki — Wikipedia to Wax converter for the Wick ecosystem",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "article", nargs="?",
        help="Article title to fetch (CLI mode)",
    )
    parser.add_argument(
        "-o", "--output", metavar="FILE",
        help="Write output to FILE instead of stdout (CLI mode)",
    )
    parser.add_argument(
        "-l", "--lang", default=DEFAULT_LANG, metavar="LANG",
        help=f"Wikipedia language code (default: {DEFAULT_LANG})",
    )
    parser.add_argument(
        "--serve", action="store_true",
        help="Run as local HTTP server",
    )
    parser.add_argument(
        "--port", type=int, default=DEFAULT_PORT, metavar="PORT",
        help=f"Port for server mode (default: {DEFAULT_PORT})",
    )

    args = parser.parse_args()

    if args.serve:
        cmd_serve(args.port, args.lang)
    elif args.article:
        cmd_fetch(args.article, args.lang, args.output)
    else:
        parser.print_help()
        sys.exit(1)

if __name__ == "__main__":
    main()
