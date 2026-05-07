# Wax — Simple Terminal Plaintext Language

A minimal plaintext document format designed for terminal browsing, part of the Wick ecosystem.
Files use the `.wax` extension.

---

## Document structure

A Wax document is a plain text file. Each line is one of four types:
**header**, **subheader**, **link**, or **text**. Line type is determined
by the first character(s) of the line.

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
=> URL label text
```

A line beginning with `=>` is a link. The format is:

```
=> <url> <label...>
```

- `url` — the destination, no spaces. Can be a local `.wax` file path or a full URL.
- `label` — everything after the first whitespace following the URL. May contain spaces.

Browsers render links as numbered buttons:

```
[1] label text
```

Numbers are assigned sequentially from 1 as links appear in the document.
The user activates a link by pressing its number. No cursor navigation is used.

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
```

Rendered output:

```
    #My Wax Page
  ##welcome to my little corner of the net
this is some regular text with a splash of color!
check out these links:
[1] Example Site
[2] My local notes
```

---

## Browser behavior

- No cursor or focus indicator. Navigation is number-key only.
- Links are numbered `[1]`, `[2]`, etc. in order of appearance.
- After rendering, browsers display a link summary and prompt for a number.
- Entering a number navigates to that link's URL.
- If the URL is a local path to a `.wax` file, it is rendered in-browser.
- If the URL is a remote address, the browser should hand it off to the
  system's default URL handler.

---

## File extension and MIME type

| Property  | Value             |
|-----------|-------------------|
| Extension | `.wax`           |
| MIME type | `text/x-wax`     |
