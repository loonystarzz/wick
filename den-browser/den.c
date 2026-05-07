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

/* ── forward declarations ───────────────────────────────────────────────── */
static void show_download_progress(int rows, int cols, const char *filename,
                                  curl_off_t downloaded, long total);
static int perform_download(const char *url, const char *filename,
                           const char *path, int rows, int cols);
static void reset_focus(void);

/* ── limits ─────────────────────────────────────────────────────────────── */
#define MAX_PARSED   4096
#define MAX_DLINES  16384
#define MAX_LINKS     128
#define MAX_URL       512
#define MAX_LABEL     256
#define MAX_RAW      1024
#define MAX_FILENAME  256
#define MAX_PATH      512

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
#define CP_DOWNLOAD 25
#define CP_PROMPT   26

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

typedef struct {
    char url[MAX_URL];
    char filename[MAX_FILENAME];
    char path[MAX_PATH];
    long content_length;
    int active;
    curl_off_t downloaded;
    CURL *curl;
    FILE *file;
} DownloadInfo;

/* ── globals ─────────────────────────────────────────────────────────────── */
static SLine slines[MAX_PARSED];
static int   sline_count = 0;
static DLine dlines[MAX_DLINES];
static int   dline_count = 0;
static Link  lnks[MAX_LINKS];
static int   link_count  = 0;
static int   last_page_w = 0;
static int   focused_link = -1;  /* -1 means no focus */
static DownloadInfo current_download = {0};

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

/* ── download progress callback ────────────────────────────────────────────── */
static int download_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t ultotal, curl_off_t ulnow) {
    DownloadInfo *dl = (DownloadInfo *)clientp;
    dl->downloaded = dlnow;
    
    /* Update progress display */
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    show_download_progress(rows, cols, dl->filename, dlnow, dl->content_length);
    
    return 0;
}

/* ── curl write callback for downloads ───────────────────────────────────────── */
static size_t download_write(void *ptr, size_t sz, size_t nmemb, void *userdata) {
    DownloadInfo *dl = (DownloadInfo *)userdata;
    size_t written = fwrite(ptr, sz, nmemb, dl->file);
    return written;
}

/* ── check if URL has #download fragment ─────────────────────────────────────── */
static int is_download_url(const char *url, char *clean_url, size_t clean_len, 
                           char *filename, size_t fn_len) {
    /* Find the # character */
    const char *hash_pos = strchr(url, '#');
    if (!hash_pos) {
        /* No #, not a download URL */
        strncpy(clean_url, url, clean_len - 1);
        clean_url[clean_len - 1] = '\0';
        return 0;
    }
    
    /* Extract everything after # as filename */
    const char *filename_start = hash_pos + 1;
    if (*filename_start == '\0') {
        /* Empty filename, not a download URL */
        strncpy(clean_url, url, clean_len - 1);
        clean_url[clean_len - 1] = '\0';
        return 0;
    }
    
    /* Copy clean URL (everything before #) */
    size_t clean_url_len = hash_pos - url;
    if (clean_url_len >= clean_len) clean_url_len = clean_len - 1;
    strncpy(clean_url, url, clean_url_len);
    clean_url[clean_url_len] = '\0';
    
    /* Ensure URL has proper scheme for curl */
    char temp_url[MAX_URL];
    strncpy(temp_url, clean_url, sizeof(temp_url) - 1);
    temp_url[sizeof(temp_url) - 1] = '\0';
    
    /* If URL doesn't start with http:// or https://, add http:// */
    if (strncmp(temp_url, "http://", 7) != 0 && strncmp(temp_url, "https://", 8) != 0) {
        snprintf(clean_url, clean_len, "http://%s", temp_url);
    }
    
    /* Copy filename (everything after #) */
    strncpy(filename, filename_start, fn_len - 1);
    filename[fn_len - 1] = '\0';
    
    return 1;
}

/* ── get content length of URL ──────────────────────────────────────────────── */
static long get_content_length(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "den/1.0 (Wick)");
    
    CURLcode res = curl_easy_perform(curl);
    long content_length = -1;
    
    if (res == CURLE_OK) {
        curl_off_t content_length_t;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length_t);
        content_length = (long)content_length_t;
    }
    
    curl_easy_cleanup(curl);
    return content_length;
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
    reset_focus();  /* Reset focus when loading new content */
    char buf[MAX_RAW];
    while (fgets(buf, sizeof(buf), f)) parse_line(buf);
    fclose(f);
    last_page_w = 0;
}

/* ── load from a string (fetched content) ────────────────────────────────── */
static void load_from_string(const char *content) {
    sline_count = 0; link_count = 0;
    reset_focus();  /* Reset focus when loading new content */
    const char *p = content;
    char buf[MAX_RAW];
    while (*p) {
        const char *nl = strchr(p, '\n');
        if (!nl) {
            strncpy(buf, p, MAX_RAW-1);
            buf[MAX_RAW-1] = '\0';
            parse_line(buf);
            break;
        } else {
            int len = nl - p;
            if (len >= MAX_RAW) len = MAX_RAW-1;
            strncpy(buf, p, len);
            buf[len] = '\0';
            parse_line(buf);
            p = nl + 1;
        }
    }
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

/* ── focus navigation ───────────────────────────────────────────────────── */
static int find_next_link(int current) {
    if (link_count == 0) return -1;
    if (current < 0 || current >= link_count) return 0;
    return (current + 1) % link_count;
}

static int find_prev_link(int current) {
    if (link_count == 0) return -1;
    if (current < 0 || current >= link_count) return link_count - 1;
    return (current - 1 + link_count) % link_count;
}

static void reset_focus(void) {
    focused_link = -1;
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
    init_pair(CP_DOWNLOAD, COLOR_GREEN,   COLOR_BLACK);
    init_pair(CP_PROMPT,   COLOR_CYAN,    COLOR_BLACK);
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
            int is_focused = (dl->link_index == focused_link);
            if (!dl->is_cont) {
                move(row,page_x);
                if (is_focused) {
                    attron(COLOR_PAIR(CP_LINK)|A_BOLD|A_REVERSE);
                } else {
                    attron(COLOR_PAIR(CP_LINK)|A_BOLD);
                }
                printw("[%d]",dl->link_index+1);
                if (is_focused) {
                    attroff(COLOR_PAIR(CP_LINK)|A_BOLD|A_REVERSE);
                } else {
                    attroff(COLOR_PAIR(CP_LINK)|A_BOLD);
                }
            }
            int color = is_focused ? CP_LINK : CP_DEFAULT;
            draw_colored_text(row,page_x+num_w+1,max_col,dl->text,A_NORMAL,color);
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
        const char *hints_full  = "space:URL  ^v/jk  PgUp/Dn  [1-9]link  Tab/Shift+Tab focus  q quit";
        const char *hints_short = "spc:URL jk PgUpDn [1-9] Tab/Shift+Tab q quit";
        const char *hints_min   = "spc jk Tab q";
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

/* ── show download prompt ───────────────────────────────────────────────────── */
static int show_download_prompt(int rows, int cols, const char *filename,
                               long size, const char *path) {
    char size_str[64];
    if (size > 0) {
        if (size < 1024) {
            snprintf(size_str, sizeof(size_str), "%ld B", size);
        } else if (size < 1024*1024) {
            snprintf(size_str, sizeof(size_str), "%.1f KB", size/1024.0);
        } else {
            snprintf(size_str, sizeof(size_str), "%.1f MB", size/(1024.0*1024.0));
        }
    } else {
        strncpy(size_str, "unknown size", sizeof(size_str)-1);
        size_str[sizeof(size_str)-1] = '\0';
    }
    
    /* Clear screen and show prompt */
    erase();
    
    int prompt_y = rows/2 - 2;
    
    attron(COLOR_PAIR(CP_PROMPT)|A_BOLD);
    mvprintw(prompt_y,     2, "Download file?");
    attroff(COLOR_PAIR(CP_PROMPT)|A_BOLD);
    
    attron(COLOR_PAIR(CP_DOWNLOAD));
    mvprintw(prompt_y + 1, 2, "Filename: %s", filename);
    mvprintw(prompt_y + 2, 2, "Size:     %s", size_str);
    mvprintw(prompt_y + 3, 2, "Path:     %s", path);
    attroff(COLOR_PAIR(CP_DOWNLOAD));
    
    attron(COLOR_PAIR(CP_PROMPT)|A_BOLD);
    mvprintw(prompt_y + 5, 2, "Download? [y/n]: ");
    attroff(COLOR_PAIR(CP_PROMPT)|A_BOLD);
    
    refresh();
    
    /* Wait for y/n response */
    for (;;) {
        int ch = getch();
        if (ch == 'y' || ch == 'Y') return 1;
        if (ch == 'n' || ch == 'N' || ch == 27) return 0;
    }
}

/* ── show download progress ─────────────────────────────────────────────────── */
static void show_download_progress(int rows, int cols, const char *filename,
                                  curl_off_t downloaded, long total) {
    int bar_y = rows - 2;
    int bar_width = cols - 20;
    
    if (bar_width < 10) bar_width = 10;
    
    /* Clear progress area */
    attron(COLOR_PAIR(CP_BAR));
    mvhline(bar_y, 0, ' ', cols);
    attroff(COLOR_PAIR(CP_BAR));
    
    /* Calculate percentage */
    int percent = 0;
    if (total > 0) {
        percent = (int)((downloaded * 100) / total);
        if (percent > 100) percent = 100;
    }
    
    /* Draw progress bar */
    int filled = (percent * bar_width) / 100;
    
    attron(COLOR_PAIR(CP_DOWNLOAD)|A_BOLD);
    mvprintw(bar_y, 1, "Downloading %s: %d%%", filename, percent);
    attroff(COLOR_PAIR(CP_DOWNLOAD)|A_BOLD);
    
    /* Progress bar */
    attron(COLOR_PAIR(CP_BAR));
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) {
            attron(COLOR_PAIR(CP_DOWNLOAD)|A_REVERSE);
            mvaddch(bar_y, 15 + i, ' ');
            attroff(COLOR_PAIR(CP_DOWNLOAD)|A_REVERSE);
        } else {
            mvaddch(bar_y, 15 + i, '-');
        }
    }
    attroff(COLOR_PAIR(CP_BAR));
    
    refresh();
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

/* ── perform download with fallback mechanism ─────────────────────────────────── */
static int perform_download_with_fallback(const char *url, const char *filename,
                                          const char *path, int rows, int cols) {
    /* First try the original URL */
    int result = perform_download(url, filename, path, rows, cols);
    if (result) return 1; /* Success */
    
    /* If original failed, try the index.wax fallback like the browser does */
    char fallback_url[MAX_URL];
    int len = (int)strlen(url);
    if (url[len-1] == '/') {
        snprintf(fallback_url, sizeof(fallback_url), "%sindex.wax", url);
    } else {
        snprintf(fallback_url, sizeof(fallback_url), "%s/index.wax", url);
    }
    
    /* Try downloading from fallback URL */
    char errbuf[256] = "";
    char *content = fetch_url(fallback_url, errbuf, sizeof(errbuf));
    
    if (content) {
        /* Write the fallback content to file */
        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, filename);
        
        FILE *f = fopen(full_path, "wb");
        if (f) {
            size_t written = fwrite(content, 1, strlen(content), f);
            fclose(f);
            
            if (written == strlen(content)) {
                char success_msg[300];
                snprintf(success_msg, sizeof(success_msg), "downloaded: %s", full_path);
                show_error(rows, cols, success_msg);
                free(content);
                return 1;
            }
        }
        free(content);
    }
    
    /* Both failed */
    show_error(rows, cols, "download failed");
    return 0;
}
static int perform_download(const char *url, const char *filename,
                           const char *path, int rows, int cols) {
    /* Construct full file path */
    char full_path[MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s/%s", path, filename);
    
    /* Open file for writing */
    FILE *f = fopen(full_path, "wb");
    if (!f) {
        char msg[300];
        snprintf(msg, sizeof(msg), "cannot create file: %s", full_path);
        show_error(rows, cols, msg);
        return 0;
    }
    
    /* Initialize download info */
    memset(&current_download, 0, sizeof(current_download));
    strncpy(current_download.url, url, sizeof(current_download.url)-1);
    strncpy(current_download.filename, filename, sizeof(current_download.filename)-1);
    strncpy(current_download.path, path, sizeof(current_download.path)-1);
    current_download.file = f;
    current_download.active = 1;
    
    /* Initialize curl */
    current_download.curl = curl_easy_init();
    if (!current_download.curl) {
        fclose(f);
        show_error(rows, cols, "curl init failed for download");
        return 0;
    }
    
    /* Configure curl for download */
    curl_easy_setopt(current_download.curl, CURLOPT_URL, url);
    curl_easy_setopt(current_download.curl, CURLOPT_WRITEFUNCTION, download_write);
    curl_easy_setopt(current_download.curl, CURLOPT_WRITEDATA, &current_download);
    curl_easy_setopt(current_download.curl, CURLOPT_XFERINFOFUNCTION, download_progress);
    curl_easy_setopt(current_download.curl, CURLOPT_XFERINFODATA, &current_download);
    curl_easy_setopt(current_download.curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(current_download.curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(current_download.curl, CURLOPT_TIMEOUT, 300L); /* 5 minute timeout */
    curl_easy_setopt(current_download.curl, CURLOPT_USERAGENT, "den/1.0 (Wick)");
    
    /* Get content length for progress */
    current_download.content_length = get_content_length(url);
    
    /* Perform download with progress display */
    CURLcode res = curl_easy_perform(current_download.curl);
    
    /* Cleanup */
    curl_easy_cleanup(current_download.curl);
    fclose(f);
    current_download.active = 0;
    
    if (res != CURLE_OK) {
        char msg[300];
        snprintf(msg, sizeof(msg), "download failed: %s", curl_easy_strerror(res));
        show_error(rows, cols, msg);
        return 0;
    }
    
    /* Show success message */
    char success_msg[300];
    snprintf(success_msg, sizeof(success_msg), "downloaded: %s", full_path);
    show_error(rows, cols, success_msg);
    return 1;
}

/* ── try appending index.wax to a bare URL ───────────────────────────────── */
/* returns heap string (caller frees) or NULL */

/* ── navigate to a URL or file path ─────────────────────────────────────── */
/* fills current_url, loads content; returns 1 on success, 0 on error */
static int navigate(const char *target, char *current_url, size_t url_sz,
                    int rows, int cols) {
    /* Check if this is a download URL */
    char clean_url[MAX_URL];
    char filename[MAX_FILENAME];
    if (is_download_url(target, clean_url, sizeof(clean_url), filename, sizeof(filename))) {
        /* This is a download request */
        if (!is_remote_url(clean_url)) {
            show_error(rows, cols, "download only supported for remote URLs");
            return 0;
        }
        
        /* Get file size */
        long file_size = get_content_length(clean_url);
        
        /* Show download prompt */
        int download_path = show_download_prompt(rows, cols, filename, file_size, "~/Downloads");
        
        if (download_path) {
            /* User chose to download - create Downloads directory if needed */
            char home_downloads[MAX_PATH];
            const char *home = getenv("HOME");
            if (!home) home = ".";
            snprintf(home_downloads, sizeof(home_downloads), "%s/Downloads", home);
            
            /* Create directory if it doesn't exist (mkdir may fail, that's ok) */
            char mkdir_cmd[MAX_PATH + 10];
            snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", home_downloads);
            system(mkdir_cmd);
            
            /* Perform download with fallback mechanism */
            int result = perform_download_with_fallback(clean_url, filename, home_downloads, rows, cols);
            
            /* After download, go back to previous page by not changing current_url */
            return result ? 2 : 0; /* 2 = download completed, stay on current page */
        } else {
            /* User chose not to download - go back to previous page */
            return 2; /* Stay on current page */
        }
    }
    
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
                    if (ok == 1) { scroll=0; last_page_w=0; }
                    /* ok == 2 means download completed or cancelled, stay on current page */
                    /* ok == 0 means error, stay on current page */
                }
                break;
            }

            case KEY_UP:   case 'k': scroll--;              break;
            case KEY_DOWN: case 'j': scroll++;              break;
            case KEY_PPAGE:          scroll -= visible-1;   break;
            case KEY_NPAGE:          scroll += visible-1;   break;
            case KEY_HOME: case 'g': scroll = 0;            break;
            case KEY_END:  case 'G': scroll = max_scroll;   break;
            case '\t': /* Tab - move focus to next link */
                focused_link = find_next_link(focused_link);
                break;
            case KEY_BTAB: /* Shift+Tab - move focus to previous link */
                focused_link = find_prev_link(focused_link);
                break;
            case '\n': case '\r': case KEY_ENTER: /* Enter - activate focused link */
                if (focused_link >= 0 && focused_link < link_count) {
                    const char *url = lnks[focused_link].url;
                    int ok = navigate(url,current_url,sizeof(current_url),rows,cols);
                    if (ok == 1) {scroll=0;last_page_w=0;}
                    /* ok == 2 means download completed or cancelled, stay on current page */
                    /* ok == 0 means error, stay on current page */
                }
                break;
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
                        if (ok == 1) {scroll=0;last_page_w=0;}
                        /* ok == 2 means download completed or cancelled, stay on current page */
                        /* ok == 0 means error, stay on current page */
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
