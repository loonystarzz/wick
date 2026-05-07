# Wick Browser Specification

This document defines the expected behavior and user interface conventions for Wick-compatible browsers.

---

## Core Principles

Wick browsers are mostly minimalist terminal browsers designed for the Wax web language format. They prioritize:

- **Simplicity** - No complex UI elements
- **Keyboard-only navigation** - No mouse interaction required
- **Predictable behavior** - Consistent interaction patterns across all Wax content
- **Performance** - Fast loading and rendering of plain text documents

---

## User Interface

### Layout (example)

```
┌─────────────────────────────────────────────────────────────────┐
│                    Document Content Area                        │
│  #Header text                                                    │
│  ##Subheader text                                                │
│  Regular text content                                            │
│  [1] Link label                                                  │
│  [2] Another link                                                │
│                                                                 │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
│ Status bar: current_url | status messages | download progress │
└─────────────────────────────────────────────────────────────────┘
```

### Rendering Rules

#### Headers and Subheaders
- **Headers** (`#text`): Bold, indented 4 spaces, include the `#` character
- **Subheaders** (`##text`): Bold, indented 2 spaces, include the `##` characters
- **Text**: No indentation, normal weight

#### Links
- Render as `[number] label` format
- Numbers assigned sequentially from 1
- Alternative way to focus elements using tab or another button may be implemented
- extra key for numbers above 9 recommended

#### Color Codes
- Support all Wax color codes (`$$x` syntax)
- Reset color state at end of each line
- No color persistence across lines

---

## Navigation

### Key Bindings

| Key | Action |
|-----|--------|
| `1-9` | Follow numbered link |
| `Space` | Open URL bar for manual navigation |
| `j/k` or `↑/↓` | Scroll line by line |
| `PgUp/PgDn` | Scroll page by page |
| `g/G` or `Home/End` | Jump to top/bottom |
| `h` | Go to home page (`wick://home`) |
| `q` | Quit browser |

### Link Navigation

1. **Numbered links**: Press `1-9` to follow the corresponding link
2. **Download links**: If URL ends with `#filename`, initiate download instead of navigation
3. **Local files**: `.wax` files are rendered in-browser
4. **Remote URLs**: Non-Wax URLs are handed to system default handler

### URL Bar

- Activated with `Space` key
- Pre-filled with current URL
- Supports:
  - HTTP/HTTPS URLs
  - Local file paths
  - Download URLs with `#filename` syntax
  - Wick protocol URLs (`wick://home`)

---

## Download Behavior

### Download Detection
Links ending with `#filename` (no spaces) trigger download mode:

```
=> http://example.com/data.csv#data.csv Download dataset
=> file://local/path/document.pdf#report.pdf Save report
```

### Download Process

1. **URL Parsing**: Strip `#filename` from URL before request
2. **Content Detection**: Attempt to get file size via HTTP HEAD request
3. **User Prompt**: Display download information:
   ```
   Download: data.csv
   Size: 1.2 MB
   Destination: ~/Downloads/
   Download? [y/n]
   ```
4. **Download Execution** (if confirmed):
   - Create `~/Downloads` directory if needed
   - Download file with progress indicator
   - Show progress in status bar: `Downloading: 45% (data.csv)`
5. **Completion**: Show success message and remain on current page

### Error Handling
- Network errors: Show error message, remain on current page
- File creation errors: Show error message, remain on current page
- User cancellation: Return to current page without downloading

---

## Performance Requirements

### Loading
- Local files: < 100ms for typical documents
- Remote URLs: Show "fetching..." status during load
- Progress indication for large downloads

### Rendering
- Word wrapping at terminal width
- Responsive scrolling with no lag
- Efficient color code processing

### Memory
- Handle documents up to 1MB efficiently
- Clean up resources when navigating away

basically, it has to run fine with no strong slowdowns on any low end pc
released in the past 15 years 
---

## Compatibility

### Wax Language Support
- All Wax syntax elements must be supported
- Graceful degradation for unknown syntax
- Backward compatibility with older Wax versions

### Terminal Compatibility
- Work in standard terminal 
- Support common terminal emulators
- Handle terminal resize gracefully (cant break to a point u'd need to exit and reopen browser)

### Platform Support
- Unix-like systems (Linux, macOS, BSD)
- Windows (via WSL or native terminal)
- No external dependencies beyond standard libraries (or extra features not in spec)

---

## Security Considerations

### File Access
- Only access files explicitly requested by user
- No automatic file execution or opening
- Sanitize file paths to prevent directory traversal

### Network Access
- Follow HTTP redirects safely (limit redirect chain)
- Respect timeout limits
- No automatic execution of downloaded content

### User Data
- No tracking or telemetry
- Clean temporary files on exit

---

## Extensions (Optional)

### Protocol Handlers
- `<tag>://home` - Built-in home page
- `file://` - Local file system access
- Custom protocols for specific content types
<tag>: whatever u want to use for internal browser pages. browser name recommended.

### Advanced Features
- u may add any extra features to ur browser as long as they dont interfere with basic usage.
