# Wax — Simple Terminal Plaintext Language

A minimal plaintext document format designed for terminal browsing, part of the Wick ecosystem.
Files use the `.wax` extension.

---

## Document structure

A Wax document is a plain text file. Each line is one of four types:
**header**, **subheader**, **link**, or **text**. Line type is determined
by the first character(s) of the line.

Optionally, a document may begin with one or both **page metadata directives**
(`--bgcol` and `--txcol`) before any content lines. These are described in the
[Page Metadata Directives](#page-metadata-directives) section below.

---

## Line types

### Header

```
#Header text here
```

A line beginning with `#` (and not `##`) is a header.
Browsers render it **bold** and indented **4 spaces** from the left margin.
The `#` character is included in the rendered output.

### Subheader

```
##Subheader text here
```

A line beginning with `##` is a subheader.
Browsers render it **bold** and indented **2 spaces** from the left margin.
The `##` characters are included in the rendered output.

### Link

```
=> URL label 
=> URL#filename label 
```

A line beginning with `=>` is a link. The format is:

```
=> <url> <label...>
=> <url>#<filename> <label...>
```

- `url` — the destination, no spaces. Can be a local `.wax` file path or a full URL.
- `filename` — optional filename for download (after `#`, no spaces). When present, browsers should initiate a download instead of navigation.
- `label` — everything after the first whitespace following the URL (or `#filename`). May contain spaces.

**Navigation links:**
Browsers render links as numbered buttons:

```
[1] label text
```

Numbers are assigned sequentially from 1 as links appear in the document.
The user activates a link by pressing its number. No cursor navigation is used.

**Download links:**
If a link URL ends with `#filename` (no spaces between `#` and filename), browsers should:
1. Strip the `#filename` part from the URL before making the request
2. Prompt the user with download information (filename, file size, destination path)
3. If user confirms, download the content to the user's Downloads folder with the specified filename
4. Show download progress and remain on the current page after completion

After rendering, browsers should show a summary list of all links with their
numbers, URLs, and labels so the user can review before navigating.

### Text

Any line that does not start with `#`, `##`, or `=>` is plain text.
Text lines support inline color codes (see below).
Blank lines are rendered as empty lines.

---

## Color codes

Color codes may appear anywhere in a **text** line (not in headers, subheaders,
or link labels). The syntax is:

```
$$x
```

Where `x` is a single character from the table below. The code applies to all
text that follows it on the line until another code overrides it or `$$r` resets
it. Color state does not carry across lines.

| Code | Color        | Code | Color       |
|------|--------------|------|-------------|
| `0`  | Black        | `8`  | Dark gray   |
| `1`  | Dark blue    | `9`  | Blue        |
| `2`  | Dark green   | `a`  | Green       |
| `3`  | Dark cyan    | `b`  | Cyan        |
| `4`  | Dark red     | `c`  | Red         |
| `5`  | Dark magenta | `d`  | Magenta     |
| `6`  | Gold/orange  | `e`  | Yellow      |
| `7`  | Gray         | `f`  | White       |
| `r`  | Reset        |      |             |

Example:

```
this is $$agreen text$$r and this is $$cred text$$r back to normal
```

---

## Example document

```
#My Wax Page
##welcome to my little corner of the net
this is some $$eregular text$$r with a splash of color!
check out these links:
=> http://example.com/ Example Site
=> notes.wax My local notes
=> http://example.com/data.csv#data.csv Download dataset
=> http://localhost:8421/article.pdf#article.pdf Save article as PDF
```

Rendered output:

```
    #My Wax Page
  ##welcome to my little corner of the net
this is some regular text with a splash of color!
check out these links:
[1] Example Site
[2] My local notes
[3] Download dataset
[4] Save article as PDF
```

---

## Page metadata directives

Page metadata directives are optional special lines that **must appear at the
very start of the file**, before any content lines. They declare preferred
colors for the page background and default text. Browsers that support them
may use these values to style the page; browsers that do not support them
(for example, basic terminal browsers that cannot set a custom background)
**must silently ignore these lines** — they must never appear as visible
content in the rendered page.

There are two directives:

### `--bgcol` — background color

```
--bgcol <color>
```

Declares the author's preferred background color for the page. `<color>` is a
single color token from the standard Wax color table (see [Color codes](#color-codes)).

### `--txcol` — default text color

```
--txcol <color>
```

Declares the author's preferred default text color (i.e. the color used for
text lines before any inline `$$x` code). `<color>` is a single color token
from the standard Wax color table.

### Rules

- Both directives are **optional**. Either, both, or neither may be present.
- When present, they **must appear before any other lines** in the file.
  A browser encountering them anywhere else may treat them as plain text.
- The order of the two directives relative to each other does not matter,
  as long as both precede all content lines.
- Each directive occupies exactly one line.
- The color token is the same single-character code used in inline color
  codes (e.g. `0`–`9`, `a`–`f`). The `r` reset token is not valid here.
- Browsers **must not render these lines as page content** under any
  circumstances — they are metadata only.

### Browser handling

Browsers fall into two categories:

**Supporting browsers** (graphical or color-capable terminal browsers) should
apply the declared colors when rendering the page. They may display the color
values in a separate info panel, status bar, or similar out-of-band location
for user awareness, but the lines themselves must not appear inline in the
page body.

**Non-supporting browsers** (e.g. basic terminal browsers with no custom
background support) must simply skip these lines and apply no special
treatment. The page renders exactly as if the directives were absent.

### Example

```
--bgcol 0
--txcol f
#My Wax Page
##welcome to my little corner of the net
this is some $$eregular text$$r with a splash of color!
```

In this example the author requests a black background (`0`) with white text
(`f`). A supporting browser applies those colors; a non-supporting browser
ignores both lines and renders the page normally.

---

## File extension and MIME type

| Property  | Value             |
|-----------|-------------------|
| Extension | `.wax`           |
| MIME type | `text/x-wax`     |
