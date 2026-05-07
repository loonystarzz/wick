/*
 * den.c — Den, the Wick ecosystem browser (Wax document format)
 * compile: gcc -Wall -Wextra -o den den.c -lncurses -lcurl
 *
 * space   → open URL bar (type URL, Enter to go, Esc to cancel)
 * ^v/jk   → scroll line by line
 * PgUp/Dn → scroll page
 * g/G     → top / bottom
 * 1-9     → follow link
 * h       → home (wick://home)
 * q       → quit
 */

#include <ncurses.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* ── limits ─────────────────────────────────────────────────────────────── */
#define MAX_PARSED   4096
#define MAX_DLINES  16384
#define MAX_LINKS     128
#define MAX_URL       512
#define MAX_LABEL     256
#define MAX_RAW      1024

/* ── colour pairs ────────────────────────────────────────────────────────── */
#define CP_DEFAULT   1
#define CP_BLACK     2
#define CP_DKBLUE    3
#define CP_DKGREEN   4
#define CP_DKCYAN    5
#define CP_DKRED     6
#define CP_DKMAG     7
#define CP_GOLD      8
#define CP_GRAY      9
#define CP_DKGRAY   10
#define CP_BLUE     11
#define CP_GREEN    12
#define CP_CYAN     13
#define CP_RED      14
#define CP_MAGENTA  15
#define CP_YELLOW   16
#define CP_WHITE    17
#define CP_LINK     18
#define CP_BAR      19
#define CP_HEADER   20
#define CP_RULE     21
#define CP_URLBAR   22
#define CP_URLLABEL 23
#define CP_ERROR    24

static int code_to_pair(char c) {
    switch (c) {
        case '0': return CP_BLACK;   case '1': return CP_DKBLUE;
        case '2': return CP_DKGREEN; case '3': return CP_DKCYAN;
        case '4': return CP_DKRED;   case '5': return CP_DKMAG;
        case '6': return CP_GOLD;    case '7': return CP_GRAY;
        case '8': return CP_DKGRAY;  case '9': return CP_BLUE;
        case 'a': return CP_GREEN;   case 'b': return CP_CYAN;
        case 'c': return CP_RED;     case 'd': return CP_MAGENTA;
        case 'e': return CP_YELLOW;  case 'f': return CP_WHITE;
        default:  return 0;
    }
}

/* ── types ───────────────────────────────────────────────────────────────── */
typedef enum { LT_TEXT, LT_HEADER, LT_SUBHEADER, LT_LINK } LineType;

typedef struct {
    LineType type;
    int      link_index;
    char     raw[MAX_RAW];
} SLine;

typedef struct {
    LineType type;
    int      link_index;
    int      indent;
    char     text[MAX_RAW];
    int      is_cont;
} DLine;

typedef struct {
    char url[MAX_URL];
    char label[MAX_LABEL];
} Link;

/* ── globals ─────────────────────────────────────────────────────────────── */
static SLine slines[MAX_PARSED];
static int   sline_count = 0;
static DLine dlines[MAX_DLINES];
static int   dline_count = 0;
static Link  lnks[MAX_LINKS];
static int   link_count  = 0;
static int   last_page_w = 0;

/* ── curl write buffer ───────────────────────────────────────────────────── */
typedef struct { char *data; size_t size; } CurlBuf;

static size_t curl_write(void *ptr, size_t sz, size_t nmemb, void *userdata) {
    CurlBuf *b = (CurlBuf *)userdata;
    size_t added = sz * nmemb;
    char *tmp = realloc(b->data, b->size + added + 1);
    if (!tmp) return 0;
    b->data = tmp;
    memcpy(b->data + b->size, ptr, added);
    b->size += added;
    b->data[b->size] = '\0';
    return added;
}

/* ── fetch URL (http/https) into a heap string; caller frees ─────────────── */
/* returns NULL and fills errbuf on failure */
static char *fetch_url(const char *url, char *errbuf, size_t errbuf_sz) {
    CURL *curl = curl_easy_init();
    if (!curl) { snprintf(errbuf, errbuf_sz, "curl init failed"); return NULL; }

    CurlBuf buf = { NULL, 0 };
    char curl_err[CURL_ERROR_SIZE] = "";

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER,    curl_err);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "den/1.0 (Wick)");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        snprintf(errbuf, errbuf_sz, "%s",
                 curl_err[0] ? curl_err : curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    errbuf[0] = '\0';
    return buf.data; /* caller must free() */
}

/* ── helpers ─────────────────────────────────────────────────────────────── */
static int visible_len(const char *s) {
    int n = 0;
    while (*s) {
        if (s[0]=='$' && s[1]=='$' && s[2]!='\0') { s+=3; continue; }
        n++; s++;
    }
    return n;
}

static int is_remote_url(const char *s) {
    return strncmp(s, "http://",  7) == 0 ||
           strncmp(s, "https://", 8) == 0;
}

/* ── parse a single raw line into slines[] ───────────────────────────────── */
static void parse_line(const char *raw) {
    if (sline_count >= MAX_PARSED) return;

    char buf[MAX_RAW];
    strncpy(buf, raw, MAX_RAW-1); buf[MAX_RAW-1] = '\0';
    int len = (int)strlen(buf);
    while (len > 0 && (buf[len-1]=='\n' || buf[len-1]=='\r')) buf[--len] = '\0';

    SLine *sl = &slines[sline_count++];
    memset(sl, 0, sizeof(*sl));

    if (buf[0]=='#' && buf[1]=='#') {
        sl->type = LT_SUBHEADER;
        strncpy(sl->raw, buf, MAX_RAW-1);
    } else if (buf[0]=='#') {
        sl->type = LT_HEADER;
        strncpy(sl->raw, buf, MAX_RAW-1);
    } else if (buf[0]=='=' && buf[1]=='>') {
        if (link_count >= MAX_LINKS) { sl->type = LT_TEXT; return; }
        sl->type       = LT_LINK;
        sl->link_index = link_count;
        const char *p = buf+2;
        while (*p==' '||*p=='\t') p++;
        const char *us = p;
        while (*p && *p!=' ' && *p!='\t') p++;
        int ul = (int)(p-us); if (ul>=MAX_URL) ul=MAX_URL-1;
        strncpy(lnks[link_count].url, us, ul); lnks[link_count].url[ul]='\0';
        while (*p==' '||*p=='\t') p++;
        strncpy(lnks[link_count].label, p, MAX_LABEL-1);
        lnks[link_count].label[MAX_LABEL-1]='\0';
        strncpy(sl->raw, lnks[link_count].label, MAX_RAW-1);
        link_count++;
    } else {
        sl->type = LT_TEXT;
        strncpy(sl->raw, buf, MAX_RAW-1);
    }
}

/* ── load from file path ─────────────────────────────────────────────────── */
static void load_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { endwin(); perror(path); exit(1); }
    sline_count = 0; link_count = 0;
    char buf[MAX_RAW];
    while (fgets(buf, sizeof(buf), f)) parse_line(buf);
    fclose(f);
    last_page_w = 0;
}

/* ── load from a string (fetched content) ────────────────────────────────── */
static void load_from_string(const char *content) {
    sline_count = 0; link_count = 0;
    const char *p = content;
    char buf[MAX_RAW];
    while (*p) {
        int i = 0;
        while (*p && *p != '\n' && i < MAX_RAW-1) buf[i++] = *p++;
        if (*p == '\n') p++;
        buf[i++] = '\n'; buf[i] = '\0';
        parse_line(buf);
    }
    last_page_w = 0;
}

/* ── header width ────────────────────────────────────────────────────────── */
static int min_header_width(void) {
    int w = 0;
    for (int i = 0; i < sline_count; i++) {
        SLine *sl = &slines[i];
        int indent = (sl->type==LT_HEADER)?4:(sl->type==LT_SUBHEADER)?2:0;
        if (!indent) continue;
        int needed = indent + visible_len(sl->raw);
        if (needed > w) w = needed;
    }
    return w;
}

/* ── word-wrap into dlines[] ─────────────────────────────────────────────── */
static void wrap_text(const char *text, LineType type, int link_index,
                      int indent, int avail) {
    if (avail <= 0) avail = 1;

    const char *p = text;
    char line_buf[MAX_RAW];
    int  line_vis=0, line_len=0, first=1;

#define FLUSH_LINE(cont) do {                               \
    if (dline_count < MAX_DLINES) {                         \
        DLine *dl = &dlines[dline_count++];                 \
        dl->type=type; dl->link_index=link_index;          \
        dl->indent=indent; dl->is_cont=(cont);             \
        line_buf[line_len]='\0';                            \
        strncpy(dl->text, line_buf, MAX_RAW-1);            \
    }                                                       \
    line_len=0; line_vis=0; line_buf[0]='\0';              \
} while(0)

    char word[MAX_RAW]; int word_vis=0, word_len=0;

    while (1) {
        word_len=0; word_vis=0;
        int leading_space=0;
        while (*p==' ') { p++; leading_space=1; }
        if (!*p) break;

        while (*p && *p!=' ') {
            if (p[0]=='$'&&p[1]=='$'&&p[2]!='\0') {
                if (word_len+3<MAX_RAW){word[word_len++]=p[0];word[word_len++]=p[1];word[word_len++]=p[2];}
                p+=3;
            } else {
                if (word_len+1<MAX_RAW) word[word_len++]=*p;
                word_vis++; p++;
            }
        }
        word[word_len]='\0';

        int need = word_vis + (line_vis>0&&leading_space?1:0);

        if (line_vis>0 && line_vis+need>avail) {
            FLUSH_LINE(!first); first=0;
            while (word_vis>avail) {
                int tv=0,tl=0; const char *wp=word; char chunk[MAX_RAW]; int cl=0;
                while (*wp&&tv<avail) {
                    if (wp[0]=='$'&&wp[1]=='$'&&wp[2]!='\0'){
                        if(cl+3<MAX_RAW){chunk[cl++]=wp[0];chunk[cl++]=wp[1];chunk[cl++]=wp[2];}
                        tl+=3; wp+=3;
                    } else { if(cl+1<MAX_RAW)chunk[cl++]=*wp; tv++;tl++;wp++; }
                }
                chunk[cl]='\0';
                strncpy(line_buf,chunk,MAX_RAW-1); line_vis=tv; line_len=cl;
                FLUSH_LINE(1);
                memmove(word,word+tl,word_len-tl+1); word_len-=tl; word_vis-=tv;
            }
            strncpy(line_buf,word,MAX_RAW-1); line_len=word_len; line_vis=word_vis;
        } else {
            if (line_vis>0&&leading_space){if(line_len+1<MAX_RAW)line_buf[line_len++]=' ';line_vis++;}
            if (line_len+word_len<MAX_RAW){memcpy(line_buf+line_len,word,word_len);line_len+=word_len;}
            line_vis+=word_vis;
        }
    }
    FLUSH_LINE(!first);
#undef FLUSH_LINE
}

static void build_dlines(int page_w) {
    if (page_w == last_page_w) return;
    last_page_w = page_w; dline_count = 0;
    for (int i = 0; i < sline_count; i++) {
        SLine *sl = &slines[i];
        if (sl->type==LT_HEADER||sl->type==LT_SUBHEADER) {
            if (dline_count<MAX_DLINES){
                DLine *dl=&dlines[dline_count++];
                dl->type=sl->type; dl->link_index=0;
                dl->indent=(sl->type==LT_HEADER)?4:2;
                dl->is_cont=0;
                strncpy(dl->text,sl->raw,MAX_RAW-1);
            }
        } else if (sl->type==LT_LINK) {
            int num_w=(sl->link_index>=9)?4:3;
            int avail=page_w-num_w-1; if(avail<1)avail=1;
            wrap_text(sl->raw,LT_LINK,sl->link_index,0,avail);
        } else {
            wrap_text(sl->raw,LT_TEXT,0,0,page_w);
        }
    }
}

/* ── colour init ─────────────────────────────────────────────────────────── */
static void init_colors(void) {
    start_color(); use_default_colors();
    init_pair(CP_DEFAULT,  -1,            -1);
    init_pair(CP_BLACK,    COLOR_BLACK,   -1);
    init_pair(CP_DKBLUE,   COLOR_BLUE,    -1);
    init_pair(CP_DKGREEN,  COLOR_GREEN,   -1);
    init_pair(CP_DKCYAN,   COLOR_CYAN,    -1);
    init_pair(CP_DKRED,    COLOR_RED,     -1);
    init_pair(CP_DKMAG,    COLOR_MAGENTA, -1);
    init_pair(CP_GOLD,     COLOR_YELLOW,  -1);
    init_pair(CP_GRAY,     COLOR_WHITE,   -1);
    init_pair(CP_DKGRAY,   COLOR_BLACK,   -1);
    init_pair(CP_BLUE,     COLOR_BLUE,    -1);
    init_pair(CP_GREEN,    COLOR_GREEN,   -1);
    init_pair(CP_CYAN,     COLOR_CYAN,    -1);
    init_pair(CP_RED,      COLOR_RED,     -1);
    init_pair(CP_MAGENTA,  COLOR_MAGENTA, -1);
    init_pair(CP_YELLOW,   COLOR_YELLOW,  -1);
    init_pair(CP_WHITE,    COLOR_WHITE,   -1);
    init_pair(CP_LINK,     COLOR_GREEN,   -1);
    init_pair(CP_BAR,      COLOR_WHITE,   COLOR_BLACK);
    init_pair(CP_HEADER,   COLOR_WHITE,   -1);
    init_pair(CP_RULE,     COLOR_BLACK,   -1);
    init_pair(CP_URLBAR,   COLOR_WHITE,   COLOR_BLACK);
    init_pair(CP_URLLABEL, COLOR_CYAN,    COLOR_BLACK);
    init_pair(CP_ERROR,    COLOR_WHITE,   COLOR_RED);
}

/* ── draw coloured text ──────────────────────────────────────────────────── */
static void draw_colored_text(int row, int col, int max_col,
                               const char *text, attr_t base, int base_pair) {
    int cur_pair=base_pair; attr_t cur_attr=base;
    move(row,col); attron(COLOR_PAIR(cur_pair)|cur_attr);
    const char *p=text; int c=col;
    while (*p&&c<max_col) {
        if (p[0]=='$'&&p[1]=='$'&&p[2]!='\0') {
            char code=(char)tolower((unsigned char)p[2]);
            attroff(A_BOLD|A_DIM|COLOR_PAIR(cur_pair));
            if (code=='r'){cur_pair=base_pair;cur_attr=base;}
            else {int np=code_to_pair(code);if(np){cur_pair=np;cur_attr=base|(strchr("9abcdef",code)?A_BOLD:0);}}
            attron(COLOR_PAIR(cur_pair)|cur_attr); p+=3; continue;
        }
        addch((unsigned char)*p); c++; p++;
    }
    attroff(A_BOLD|A_DIM|COLOR_PAIR(cur_pair));
}

/* ── draw one display line ───────────────────────────────────────────────── */
static void draw_dline(int row, int page_x, int page_w, const DLine *dl) {
    int max_col = page_x+page_w;
    switch (dl->type) {
        case LT_HEADER: case LT_SUBHEADER:
            draw_colored_text(row,page_x+dl->indent,max_col,dl->text,A_BOLD,CP_HEADER);
            break;
        case LT_LINK: {
            int num_w=(dl->link_index>=9)?4:3;
            if (!dl->is_cont) {
                move(row,page_x);
                attron(COLOR_PAIR(CP_LINK)|A_BOLD);
                printw("[%d]",dl->link_index+1);
                attroff(COLOR_PAIR(CP_LINK)|A_BOLD);
            }
            draw_colored_text(row,page_x+num_w+1,max_col,dl->text,A_NORMAL,CP_DEFAULT);
            break;
        }
        default:
            draw_colored_text(row,page_x,max_col,dl->text,A_NORMAL,CP_DEFAULT);
            break;
    }
}

/* ── draw URL bar in bottom row ──────────────────────────────────────────── */
static void draw_urlbar(int row, int cols, const char *input, int cursor) {
    attron(COLOR_PAIR(CP_URLBAR));
    mvhline(row, 0, ' ', cols);
    attroff(COLOR_PAIR(CP_URLBAR));

    attron(COLOR_PAIR(CP_URLLABEL)|A_BOLD);
    mvprintw(row, 0, " go: ");
    attroff(COLOR_PAIR(CP_URLLABEL)|A_BOLD);

    int label_w = 5;
    if (cols <= label_w) return;
    int field_w = cols - label_w;

    int offset = 0;
    if (cursor >= field_w) offset = cursor - field_w + 1;

    int ilen = (int)strlen(input);

    attron(COLOR_PAIR(CP_URLBAR));
    for (int i = 0; i < field_w; i++) {
        int src = offset + i;
        char c = (src < ilen) ? input[src] : ' ';
        if (src == cursor) {
            attroff(COLOR_PAIR(CP_URLBAR));
            attron(COLOR_PAIR(CP_URLBAR)|A_REVERSE);
            mvaddch(row, label_w + i, (unsigned char)c);
            attroff(COLOR_PAIR(CP_URLBAR)|A_REVERSE);
            attron(COLOR_PAIR(CP_URLBAR));
        } else {
            mvaddch(row, label_w + i, (unsigned char)c);
        }
    }
    attroff(COLOR_PAIR(CP_URLBAR));
}

/* ── normal status bar ───────────────────────────────────────────────────── */
static void draw_bar(int rows, int cols, int scroll,
                     const char *current_url, const char *msg) {
    int y=rows-1;
    attron(COLOR_PAIR(CP_BAR)|A_BOLD);
    mvhline(y,0,' ',cols);
    if (msg&&msg[0]) {
        mvprintw(y,1,"%.*s",cols-2,msg);
    } else {
        /* percentage indicator (centred) */
        int pct=(dline_count<=1)?100:scroll*100/(dline_count-1);
        if(pct>100)pct=100;
        char pos[16]; snprintf(pos,sizeof(pos),"%d%%",pct);
        int pos_len=(int)strlen(pos);

        /* tiered hint strings — pure ASCII, no multi-byte chars */
        const char *hints_full  = "space:URL  ^v/jk  PgUp/Dn  [1-9]link  q quit";
        const char *hints_short = "spc:URL jk PgUpDn [1-9] q quit";
        const char *hints_min   = "spc jk q";
        const char *hints = "";
        if (cols >= 80)      hints = hints_full;
        else if (cols >= 55) hints = hints_short;
        else if (cols >= 30) hints = hints_min;
        int hlen=(int)strlen(hints);

        /* URL: leave room for pos in centre and hints on right */
        int right_used = (hlen>0) ? hlen+1 : 0;
        int url_max = (cols/2) - pos_len - 2;
        if (url_max < 0) url_max = 0;
        if (url_max > 50) url_max = 50;
        if (url_max > 0)
            mvprintw(y, 1, "%.*s", url_max, current_url);

        /* centred percentage */
        int pos_x = (cols - pos_len) / 2;
        if (pos_x < 1) pos_x = 1;
        mvprintw(y, pos_x, "%s", pos);

        /* right-aligned hints — only if they fit without overlapping pos */
        if (hlen > 0) {
            int hint_x = cols - hlen - 1;
            if (hint_x > pos_x + pos_len)
                mvprintw(y, hint_x, "%s", hints);
        }
    }
    attroff(COLOR_PAIR(CP_BAR)|A_BOLD);
}

/* ── show a brief error overlay on the bar ───────────────────────────────── */
static void show_error(int rows, int cols, const char *msg) {
    attron(COLOR_PAIR(CP_ERROR)|A_BOLD);
    mvhline(rows-1,0,' ',cols);
    mvprintw(rows-1,1,"error: %.100s",msg);
    attroff(COLOR_PAIR(CP_ERROR)|A_BOLD);
    refresh();
    napms(2000); /* show for 2 seconds */
}

/* ── built-in Wick homepage ──────────────────────────────────────────────── */
static const char *DEN_HOME =
"#Wick\n"
"##a minimal ecosystem for plaintext browsing in the terminal\n"
"\n"
"Wick is a quiet alternative to the modern web. No images, no scripts,\n"
"no layout engines. Just text, links, and a little color.\n"
"\n"
"##the pieces\n"
"\n"
"$$fWax$$r  — the document format. Plain text files with headers, links,\n"
"and inline color codes. Files use the $$b.wax$$r extension.\n"
"\n"
"$$fWick$$r — the ecosystem. Wax documents served over HTTP/HTTPS or read\n"
"from local files. No custom protocol, no new ports.\n"
"\n"
"$$fDen$$r  — this browser. Navigate with number keys, scroll with j/k,\n"
"open a URL with space.\n"
"\n"
"##navigation\n"
"\n"
"$$b[space]$$r  open URL bar\n"
"$$b[1-9]$$r    follow a numbered link\n"
"$$b[j/k]$$r    scroll down / up\n"
"$$b[PgDn/PgUp]$$r  scroll by page\n"
"$$b[g/G]$$r    jump to top / bottom\n"
"$$b[h]$$r      return to this page\n"
"$$b[q]$$r      quit\n"
"\n"
"##writing wax\n"
"\n"
"Lines beginning with $$f#$$r are headers, $$f##$$r are subheaders.\n"
"Lines beginning with $$f=>$$r are links: $$b=> url label text$$r\n"
"Inline color codes use $$f$$$$x$$r syntax — $$a$$$$a$$r for green, $$c$$$$c$$r for red, $$r$$$$r$$r to reset.\n"
"\n"
"##to get started\n"
"\n"
"Press $$b[space]$$r and type a URL or local .wax file path.\n";

/* ── check if fetched content looks like a wax document ─────────────────── */
/* heuristic: has at least one wax line type, not obviously HTML */
static int looks_like_wax(const char *content) {
    if (!content || !content[0]) return 0;
    /* reject obvious HTML */
    if (strncasecmp(content, "<!DOCTYPE", 9) == 0) return 0;
    if (strncasecmp(content, "<html",     5) == 0) return 0;
    /* accept if it has any wax-style line */
    const char *p = content;
    while (*p) {
        if (p[0]=='#' || (p[0]=='=' && p[1]=='>'))
            return 1;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 1; /* plain text with no wax markers is still valid wax */
}

/* ── try appending index.wax to a bare URL ───────────────────────────────── */
/* returns heap string (caller frees) or NULL */

/* ── navigate to a URL or file path ─────────────────────────────────────── */
/* fills current_url, loads content; returns 1 on success, 0 on error */
static int navigate(const char *target, char *current_url, size_t url_sz,
                    int rows, int cols) {
    /* built-in home page */
    if (strcmp(target, "wick://home") == 0 || strcmp(target, "home") == 0) {
        load_from_string(DEN_HOME);
        strncpy(current_url, "wick://home", url_sz-1);
        current_url[url_sz-1] = '\0';
        return 1;
    }

    if (is_remote_url(target)) {
        /* show "fetching…" in bar while loading */
        attron(COLOR_PAIR(CP_BAR)|A_BOLD);
        mvhline(rows-1,0,' ',cols);
        mvprintw(rows-1,1,"fetching: %.80s",target);
        attroff(COLOR_PAIR(CP_BAR)|A_BOLD);
        refresh();

        char errbuf[256] = "";
        char *content = fetch_url(target, errbuf, sizeof(errbuf));

        /* if fetch failed or content doesn't look like wax, try index.wax */
        if (!content || !looks_like_wax(content)) {
            free(content); content = NULL;
            char errbuf2[256] = "";
            attron(COLOR_PAIR(CP_BAR)|A_BOLD);
            mvhline(rows-1,0,' ',cols);
            mvprintw(rows-1,1,"trying index.wax: %.70s",target);
            attroff(COLOR_PAIR(CP_BAR)|A_BOLD);
            refresh();
            char fallback_url[MAX_URL];
            int len = (int)strlen(target);
            if (target[len-1] == '/')
                snprintf(fallback_url, sizeof(fallback_url), "%sindex.wax", target);
            else
                snprintf(fallback_url, sizeof(fallback_url), "%s/index.wax", target);
            content = fetch_url(fallback_url, errbuf2, sizeof(errbuf2));
            if (content) {
                load_from_string(content);
                free(content);
                strncpy(current_url, fallback_url, url_sz-1);
                current_url[url_sz-1] = '\0';
                return 1;
            }
            /* both failed — report original error or fallback error */
            show_error(rows, cols, errbuf[0] ? errbuf : errbuf2);
            return 0;
        }

        load_from_string(content);
        free(content);
        strncpy(current_url, target, url_sz-1);
        current_url[url_sz-1]='\0';
    } else {
        /* local file */
        FILE *f = fopen(target,"r");
        if (!f) {
            char msg[300]; snprintf(msg,sizeof(msg),"cannot open: %s",target);
            show_error(rows,cols,msg);
            return 0;
        }
        fclose(f);
        load_from_file(target);
        strncpy(current_url, target, url_sz-1);
        current_url[url_sz-1]='\0';
    }
    return 1;
}

/* ── URL bar input loop ──────────────────────────────────────────────────── */
/* returns 1 if user pressed Enter (result in out_url), 0 if Esc/cancel */
static int run_urlbar(int rows, int cols, const char *prefill,
                      char *out_url, size_t out_sz) {
    char input[MAX_URL] = "";
    int  ilen = 0;
    int  cursor = 0;

    if (prefill) {
        strncpy(input, prefill, MAX_URL-1);
        ilen = (int)strlen(input);
        cursor = ilen;
    }

    curs_set(1); /* show cursor while typing */

    for (;;) {
        draw_urlbar(rows-1, cols, input, cursor);
        refresh();

        int ch = getch();

        if (ch == 27) { /* Esc — cancel */
            curs_set(0);
            return 0;
        }
        if (ch == '\n' || ch == KEY_ENTER) {
            curs_set(0);
            strncpy(out_url, input, out_sz-1);
            out_url[out_sz-1] = '\0';
            return 1;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            if (cursor > 0) {
                memmove(input+cursor-1, input+cursor, ilen-cursor+1);
                ilen--; cursor--;
            }
        } else if (ch == KEY_DC) { /* delete forward */
            if (cursor < ilen) {
                memmove(input+cursor, input+cursor+1, ilen-cursor);
                ilen--;
            }
        } else if (ch == KEY_LEFT) {
            if (cursor > 0) cursor--;
        } else if (ch == KEY_RIGHT) {
            if (cursor < ilen) cursor++;
        } else if (ch == KEY_HOME) {
            cursor = 0;
        } else if (ch == KEY_END) {
            cursor = ilen;
        } else if (ch >= 32 && ch < 256 && ilen < MAX_URL-1) {
            /* printable character — insert at cursor */
            memmove(input+cursor+1, input+cursor, ilen-cursor+1);
            input[cursor] = (char)ch;
            ilen++; cursor++;
        }
    }
}

/* ── main browser loop ───────────────────────────────────────────────────── */
static void browse(const char *initial) {
    char current_url[MAX_URL];
    strncpy(current_url, initial, MAX_URL-1);
    current_url[MAX_URL-1] = '\0';

    char status_msg[256] = "";
    int  status_ttl = 0;
    int  scroll = 0;

    /* initial load */
    {
        int rows, cols; getmaxyx(stdscr, rows, cols);
        if (!navigate(initial, current_url, sizeof(current_url), rows, cols)) {
            /* if given arg failed, fall back to home rather than exit */
            if (strcmp(initial, "wick://home") != 0)
                navigate("wick://home", current_url, sizeof(current_url), rows, cols);
            else
                return;
        }
    }

    for (;;) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);

        int page_w = cols/2;
        int min_w  = min_header_width();
        if (page_w < min_w) page_w = min_w;
        if (page_w > cols)  page_w = cols;
        if (page_w < 10)    page_w = 10;
        int page_x  = (cols-page_w)/2;
        int visible = rows-1;

        build_dlines(page_w);

        int max_scroll = dline_count-visible;
        if (max_scroll < 0)      max_scroll = 0;
        if (scroll > max_scroll) scroll = max_scroll;
        if (scroll < 0)          scroll = 0;

        erase();

        /* vertical rules */
        attron(COLOR_PAIR(CP_RULE)|A_BOLD);
        for (int r=0;r<visible;r++) {
            if (page_x>0)             mvaddch(r,page_x-1,     ACS_VLINE);
            if (page_x+page_w<cols)   mvaddch(r,page_x+page_w,ACS_VLINE);
        }
        attroff(COLOR_PAIR(CP_RULE)|A_BOLD);

        /* content */
        for (int r=0;r<visible;r++) {
            int li=scroll+r;
            if (li>=dline_count) break;
            draw_dline(r,page_x,page_w,&dlines[li]);
        }

        draw_bar(rows,cols,scroll,current_url,
                 status_ttl>0?status_msg:"");
        if (status_ttl>0) status_ttl--;

        refresh();

        int ch = getch();

        switch (ch) {
            case 'q': case 'Q': return;

            case ' ': {
                /* open URL bar, prefilled with current URL */
                char new_url[MAX_URL]="";
                if (run_urlbar(rows,cols,current_url,new_url,sizeof(new_url))
                        && new_url[0]) {
                    int ok = navigate(new_url,current_url,sizeof(current_url),
                                      rows,cols);
                    if (ok) { scroll=0; last_page_w=0; }
                }
                break;
            }

            case KEY_UP:   case 'k': scroll--;              break;
            case KEY_DOWN: case 'j': scroll++;              break;
            case KEY_PPAGE:          scroll -= visible-1;   break;
            case KEY_NPAGE:          scroll += visible-1;   break;
            case KEY_HOME: case 'g': scroll = 0;            break;
            case KEY_END:  case 'G': scroll = max_scroll;   break;
            case 'h': {
                int ok = navigate("wick://home",current_url,sizeof(current_url),rows,cols);
                if (ok){scroll=0;last_page_w=0;}
                break;
            }

            default:
                if (ch>='1'&&ch<='9') {
                    int idx=ch-'1';
                    if (idx<link_count) {
                        const char *url=lnks[idx].url;
                        int ok=navigate(url,current_url,sizeof(current_url),
                                        rows,cols);
                        if (ok){scroll=0;last_page_w=0;}
                    }
                }
                break;
        }
    }
}

/* ── entry ───────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    const char *initial = (argc >= 2) ? argv[1] : "wick://home";

    curl_global_init(CURL_GLOBAL_DEFAULT);
    initscr(); cbreak(); noecho();
    keypad(stdscr,TRUE); curs_set(0);
    init_colors();

    browse(initial);

    endwin();
    curl_global_cleanup();
    return 0;
}
