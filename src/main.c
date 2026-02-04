#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <getopt.h>
#include <locale.h>
#include <errno.h>

#include <wchar.h>
#include <sys/ioctl.h>

#include <fcntl.h>

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int term_width_from_fd(int fd) {
    struct winsize ws;
    if (fd >= 0 && isatty(fd) &&
        ioctl(fd, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col > 0)
    {
        return (int)ws.ws_col;
    }
    return 0;
}

static int get_term_width(void) {
    // 1) Normal case
    int w = term_width_from_fd(STDOUT_FILENO);
    if (w) return clamp_int(w, 20, 1000);

    // 2) watch exports COLUMNS/LINES for the child
    const char *cols = getenv("COLUMNS");
    if (cols && *cols) {
        char *end = NULL;
        long v = strtol(cols, &end, 10);
        if (end != cols && v > 0) return clamp_int((int)v, 20, 1000);
    }

    // 3) Sometimes stdin/stderr are still TTYs
    w = term_width_from_fd(STDIN_FILENO);
    if (w) return clamp_int(w, 20, 1000);

    w = term_width_from_fd(STDERR_FILENO);
    if (w) return clamp_int(w, 20, 1000);

    // 4) Last resort: ask the controlling terminal
    int tty = open("/dev/tty", O_RDONLY);
    if (tty >= 0) {
        w = term_width_from_fd(tty);
        close(tty);
        if (w) return clamp_int(w, 20, 1000);
    }

    return 80; // safer fallback than 120
}
#define MAX_PATH 4096
#define MAX_ITEMS 2048

#define COLOR_RESET    "\033[0m"
#define COLOR_CYAN     "\033[36m"
#define COLOR_GREEN    "\033[32m"
#define COLOR_YELLOW   "\033[33m"
#define COLOR_BLUE     "\033[34m"
#define COLOR_MAGENTA  "\033[35m"
#define COLOR_RED      "\033[31m"
#define COLOR_BOLD     "\033[1m"
#define COLOR_BG_CYAN  "\033[46;30m"
#define COLOR_WHITE    "\033[97m"
#define COLOR_GRAY     "\033[37m"
#define COLOR_DIM      "\033[2m"

typedef struct {
    int show_hidden;
    int long_format;
    int human_readable;
    int omit_group;
    int add_slash;
    int show_inode;
    int recursive;        // existing -R (kept)
    int reverse;
    int sort_by_ext;
    int sort_by_time;
    int numeric_ids;
    int comma_separated;
    int quote_names;
    char *pattern;

    int depth;            // NEW: inline depth inside one box (0 = off)
} Options;

typedef struct {
    char name[256];
    char full_path[MAX_PATH];
    mode_t mode;
    off_t size;
    time_t mtime;
    uid_t uid;
    gid_t gid;
    ino_t inode;
    int is_dir;
    int is_hidden;
} FileItem;

typedef struct {
    FileItem items[MAX_ITEMS];
    int count;
    char cwd[MAX_PATH];
} FileList;

static Options opts = {0};

static int g_use_utf8 = 1;

#define U8_H  "\xE2\x94\x80"
#define U8_V  "\xE2\x94\x82"
#define U8_TL "\xE2\x94\x8C"
#define U8_TR "\xE2\x94\x90"
#define U8_BL "\xE2\x94\x94"
#define U8_BR "\xE2\x94\x98"
#define U8_LJ "\xE2\x94\x9C"
#define U8_RJ "\xE2\x94\xA4"

#define A_H  "-"
#define A_V  "|"
#define A_TL "+"
#define A_TR "+"
#define A_BL "+"
#define A_BR "+"
#define A_LJ "+"
#define A_RJ "+"

static const char *GLYPH_H  = U8_H;
static const char *GLYPH_V  = U8_V;
static const char *GLYPH_TL = U8_TL;
static const char *GLYPH_TR = U8_TR;
static const char *GLYPH_BL = U8_BL;
static const char *GLYPH_BR = U8_BR;
static const char *GLYPH_LJ = U8_LJ;
static const char *GLYPH_RJ = U8_RJ;

static void print_row_prefix(void) {
    printf("%s%s%s ", COLOR_WHITE, GLYPH_V, COLOR_RESET);
}
static void init_glyphs(void) {
    const char *lc = setlocale(LC_CTYPE, NULL);
    const char *force = getenv("LSX_ASCII");
    if (force && *force) g_use_utf8 = 0;
    if (!lc || !strstr(lc, "UTF-8")) g_use_utf8 = 0;

    if (!g_use_utf8) {
        GLYPH_H  = A_H;
        GLYPH_V  = A_V;
        GLYPH_TL = A_TL;
        GLYPH_TR = A_TR;
        GLYPH_BL = A_BL;
        GLYPH_BR = A_BR;
        GLYPH_LJ = A_LJ;
        GLYPH_RJ = A_RJ;
    }
}

static void print_repeat(const char *s, int n) {
    for (int i = 0; i < n; i++) fputs(s, stdout);
}

// Count printable columns in a string that may include ANSI CSI escapes like "\x1b[...m".
// Count printable terminal columns in a string that may include ANSI CSI escapes.
// UTF-8 aware: uses mbrtowc + wcwidth to count columns correctly.
static int visible_len_ansi(const char *s) {
    mbstate_t st;
    memset(&st, 0, sizeof(st));

    int cols = 0;
    size_t i = 0;

    while (s[i]) {
        // Skip ANSI escape sequences: ESC [ ... final
        if (s[i] == '\x1b' && s[i + 1] == '[') {
            i += 2;
            while (s[i] && !(s[i] >= '@' && s[i] <= '~')) i++;
            if (s[i]) i++; // consume final byte
            continue;
        }

        // Decode next UTF-8 sequence into a wide char
        wchar_t wc;
        size_t n = mbrtowc(&wc, s + i, MB_CUR_MAX, &st);

        if (n == (size_t)-2) {
            // Incomplete multibyte sequence; treat remaining bytes as 1 col each
            cols += 1;
            i += 1;
            memset(&st, 0, sizeof(st));
            continue;
        }
        if (n == (size_t)-1) {
            // Invalid byte sequence; count as 1 and move on
            cols += 1;
            i += 1;
            memset(&st, 0, sizeof(st));
            continue;
        }
        if (n == 0) {
            // NUL
            break;
        }

        int w = wcwidth(wc);
        if (w < 0) w = 1; // non-printables fall back to 1

        cols += w;
        i += n;
    }

    return cols;
}

static void print_row_content(int width, const char *content) {
    int inner = width - 2;
    if (inner < 1) inner = 1;

    int vis = visible_len_ansi(content);
    int padding = inner - (1 + vis); // +1 because print_row_prefix prints "│ " (space after)
    if (padding < 0) padding = 0;

    print_row_prefix();          // prints left border + space
    fputs(content, stdout);      // prints colored content
    for (int i = 0; i < padding; i++) putchar(' ');
    printf("%s%s%s\n", COLOR_WHITE, GLYPH_V, COLOR_RESET); // right border
}
static void print_border_top(int width) {
    printf("%s%s", COLOR_WHITE, GLYPH_TL);
    print_repeat(GLYPH_H, width - 2);
    printf("%s%s\n", GLYPH_TR, COLOR_RESET);
}

static void print_border_mid(int width) {
    printf("%s%s", COLOR_WHITE, GLYPH_LJ);
    print_repeat(GLYPH_H, width - 2);
    printf("%s%s\n", GLYPH_RJ, COLOR_RESET);
}

static void print_border_bottom(int width) {
    printf("%s%s", COLOR_WHITE, GLYPH_BL);
    print_repeat(GLYPH_H, width - 2);
    printf("%s%s\n", GLYPH_BR, COLOR_RESET);
}


static void print_row_suffix(int width, int used_visible_cols) {
    int inner = width - 2;
    int padding = inner - used_visible_cols;
    if (padding < 0) padding = 0;
    for (int i = 0; i < padding; i++) putchar(' ');
    printf("%s%s%s\n", COLOR_WHITE, GLYPH_V, COLOR_RESET);
}

static int matches_pattern(const char *name, const char *pattern) {
    if (!pattern) return 1;

    if (pattern[0] == '*' && pattern[1] == '.') {
        const char *ext = strrchr(name, '.');
        if (ext) return strcmp(ext, pattern + 1) == 0;
        return 0;
    }
    return 1;
}

static int load_directory(FileList *list, const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return -1;

    list->count = 0;
    snprintf(list->cwd, sizeof(list->cwd), "%s", path);

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && list->count < MAX_ITEMS) {
        if (!opts.show_hidden && entry->d_name[0] == '.') continue;
        if (!matches_pattern(entry->d_name, opts.pattern)) continue;

        FileItem *item = &list->items[list->count];
        memset(item, 0, sizeof(*item));

        snprintf(item->name, sizeof(item->name), "%s", entry->d_name);

        int n = snprintf(item->full_path, sizeof(item->full_path), "%s/%s", path, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(item->full_path)) continue;

        struct stat st;
        if (lstat(item->full_path, &st) == 0) {
            item->mode  = st.st_mode;
            item->size  = st.st_size;
            item->mtime = st.st_mtime;
            item->uid   = st.st_uid;
            item->gid   = st.st_gid;
            item->inode = st.st_ino;
            item->is_dir = S_ISDIR(st.st_mode);
        } else {
            item->mode = 0;
            item->is_dir = 0;
        }

        item->is_hidden = (entry->d_name[0] == '.');
        list->count++;
    }

    closedir(dir);
    return 0;
}

static int load_single_file(FileList *list, const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    list->count = 0;
    snprintf(list->cwd, sizeof(list->cwd), "%s", path);

    FileItem *item = &list->items[list->count];
    memset(item, 0, sizeof(*item));

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    snprintf(item->name, sizeof(item->name), "%s", base);
    snprintf(item->full_path, sizeof(item->full_path), "%s", path);

    item->mode  = st.st_mode;
    item->size  = st.st_size;
    item->mtime = st.st_mtime;
    item->uid   = st.st_uid;
    item->gid   = st.st_gid;
    item->inode = st.st_ino;
    item->is_dir = S_ISDIR(st.st_mode);
    item->is_hidden = (base[0] == '.');

    list->count++;
    return 0;
}

static int compare_by_name(const void *a, const void *b) {
    const FileItem *fa = (const FileItem *)a;
    const FileItem *fb = (const FileItem *)b;
    int cmp = strcmp(fa->name, fb->name);
    return opts.reverse ? -cmp : cmp;
}

static int compare_by_ext(const void *a, const void *b) {
    const FileItem *fa = (const FileItem *)a;
    const FileItem *fb = (const FileItem *)b;

    const char *ext_a = strrchr(fa->name, '.');
    const char *ext_b = strrchr(fb->name, '.');

    if (!ext_a) ext_a = "";
    if (!ext_b) ext_b = "";

    int cmp = strcmp(ext_a, ext_b);
    if (cmp == 0) cmp = strcmp(fa->name, fb->name);
    return opts.reverse ? -cmp : cmp;
}

static int compare_by_time(const void *a, const void *b) {
    const FileItem *fa = (const FileItem *)a;
    const FileItem *fb = (const FileItem *)b;

    if (fa->mtime < fb->mtime) return opts.reverse ? -1 : 1;
    if (fa->mtime > fb->mtime) return opts.reverse ? 1 : -1;
    return 0;
}

static void sort_list(FileList *list) {
    if (opts.sort_by_time) {
        qsort(list->items, list->count, sizeof(FileItem), compare_by_time);
    } else if (opts.sort_by_ext) {
        qsort(list->items, list->count, sizeof(FileItem), compare_by_ext);
    } else {
        qsort(list->items, list->count, sizeof(FileItem), compare_by_name);
    }
}

static void format_size(off_t size, char *str, size_t len) {
    if (opts.human_readable) {
        if (size < 1024) snprintf(str, len, "%lldB", (long long)size);
        else if (size < 1024 * 1024) snprintf(str, len, "%.1fK", size / 1024.0);
        else if (size < 1024 * 1024 * 1024) snprintf(str, len, "%.1fM", size / (1024.0 * 1024.0));
        else snprintf(str, len, "%.1fG", size / (1024.0 * 1024.0 * 1024.0));
    } else {
        snprintf(str, len, "%lld", (long long)size);
    }
}

static void format_time(time_t t, char *str, size_t len) {
    struct tm *tmv = localtime(&t);
    if (!tmv) { snprintf(str, len, "??? ?? ??:??"); return; }
    strftime(str, len, "%b %d %H:%M", tmv);
}


static void make_indent_prefix(char *out, size_t outsz, int level, int is_last) {
    // Simple tree-ish indent that still prints as plain text inside your box.
    // Example: "  ├─ " / "  └─ " repeated by level
    out[0] = '\0';
    if (level <= 0) return;

    size_t used = 0;
    for (int i = 0; i < level - 1; i++) {
        const char *seg = g_use_utf8 ? "  " : "  ";
        size_t seglen = strlen(seg);
        if (used + seglen + 1 >= outsz) break;
        memcpy(out + used, seg, seglen);
        used += seglen;
        out[used] = '\0';
    }

    const char *branch = g_use_utf8 ? (is_last ? "  └─ " : "  ├─ ") : (is_last ? "  `- " : "  |- ");
    size_t blen = strlen(branch);
    if (used + blen + 1 < outsz) {
        memcpy(out + used, branch, blen);
        used += blen;
        out[used] = '\0';
    }
}

static void print_item_simple_line(FileItem *item, int width, const char *prefix, int prefix_visible_unused) {
    (void)prefix_visible_unused;

    const char *name_col = COLOR_RESET;
    char icon = '-';

    if (item->is_dir) { icon = 'D'; name_col = COLOR_CYAN COLOR_BOLD; }
    else if (S_ISLNK(item->mode)) { icon = '@'; name_col = COLOR_MAGENTA COLOR_BOLD; }
    else if (item->mode & S_IXUSR) { icon = '*'; name_col = COLOR_GREEN COLOR_BOLD; }
    else if (item->is_hidden) { icon = '.'; name_col = COLOR_DIM COLOR_MAGENTA; }

    char row[8192];
    row[0] = '\0';

    if (prefix && *prefix) {
        snprintf(row + strlen(row), sizeof(row) - strlen(row),
                 "%s%s%s", COLOR_DIM COLOR_GRAY, prefix, COLOR_RESET);
    }

    snprintf(row + strlen(row), sizeof(row) - strlen(row),
             "%s%c%s ", COLOR_WHITE, icon, COLOR_RESET);

    if (opts.quote_names) {
        snprintf(row + strlen(row), sizeof(row) - strlen(row),
                 "%s\"%s\"%s", name_col, item->name, COLOR_RESET);
    } else {
        snprintf(row + strlen(row), sizeof(row) - strlen(row),
                 "%s%s%s", name_col, item->name, COLOR_RESET);
    }

    if (opts.add_slash && item->is_dir) {
        snprintf(row + strlen(row), sizeof(row) - strlen(row),
                 "%s/%s", COLOR_DIM COLOR_GRAY, COLOR_RESET);
    }

    print_row_content(width, row);
}

static void print_item_long_line(FileItem *item, int width, const char *prefix, int prefix_visible_unused) {
    (void)prefix_visible_unused;

    time_t now = time(NULL);

    char row[8192];
    row[0] = '\0';

    if (prefix && *prefix) {
        snprintf(row + strlen(row), sizeof(row) - strlen(row),
                 "%s%s%s", COLOR_DIM COLOR_GRAY, prefix, COLOR_RESET);
    }

    if (opts.show_inode) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s%-8llu%s ",
                 COLOR_MAGENTA, (unsigned long long)item->inode, COLOR_RESET);
        strncat(row, tmp, sizeof(row) - strlen(row) - 1);
    }

    // perms (write into row using a temp buffer)
    {
        char perm[128];
        char t = S_ISDIR(item->mode) ? 'd' : S_ISLNK(item->mode) ? 'l' : '-';
        const char *tcol = S_ISDIR(item->mode) ? (COLOR_CYAN COLOR_BOLD)
                        : S_ISLNK(item->mode) ? (COLOR_MAGENTA COLOR_BOLD)
                        : (COLOR_DIM COLOR_GRAY);

        snprintf(perm, sizeof(perm), "%s%c%s", tcol, t, COLOR_RESET);
        strncat(row, perm, sizeof(row) - strlen(row) - 1);

        const mode_t bits[9] = {
            S_IRUSR, S_IWUSR, S_IXUSR,
            S_IRGRP, S_IWGRP, S_IXGRP,
            S_IROTH, S_IWOTH, S_IXOTH
        };

        for (int i = 0; i < 9; i++) {
            char ch;
            if (item->mode & bits[i]) ch = (i % 3 == 0) ? 'r' : (i % 3 == 1) ? 'w' : 'x';
            else ch = '-';

            const char *c =
                (ch == 'r') ? COLOR_GREEN :
                (ch == 'w') ? COLOR_YELLOW :
                (ch == 'x') ? (COLOR_RED COLOR_BOLD) :
                (COLOR_DIM COLOR_GRAY);

            char one[32];
            snprintf(one, sizeof(one), "%s%c%s", c, ch, COLOR_RESET);
            strncat(row, one, sizeof(row) - strlen(row) - 1);
        }

        strncat(row, " ", sizeof(row) - strlen(row) - 1);
    }

    // owner / group
    if (opts.numeric_ids) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "%s%-8u%s ", COLOR_CYAN, item->uid, COLOR_RESET);
        strncat(row, tmp, sizeof(row) - strlen(row) - 1);

        if (!opts.omit_group) {
            snprintf(tmp, sizeof(tmp), "%s%-8u%s ", COLOR_CYAN, item->gid, COLOR_RESET);
            strncat(row, tmp, sizeof(row) - strlen(row) - 1);
        }
    } else {
        struct passwd *pw = getpwuid(item->uid);
        char owner[9];
        if (pw) snprintf(owner, sizeof(owner), "%-.8s", pw->pw_name);
        else snprintf(owner, sizeof(owner), "%u", item->uid);

        char tmp[128];
        snprintf(tmp, sizeof(tmp), "%s%-8s%s ", COLOR_CYAN, owner, COLOR_RESET);
        strncat(row, tmp, sizeof(row) - strlen(row) - 1);

        if (!opts.omit_group) {
            struct group *gr = getgrgid(item->gid);
            char group[9];
            if (gr) snprintf(group, sizeof(group), "%-.8s", gr->gr_name);
            else snprintf(group, sizeof(group), "%u", item->gid);

            snprintf(tmp, sizeof(tmp), "%s%-8s%s ", COLOR_CYAN, group, COLOR_RESET);
            strncat(row, tmp, sizeof(row) - strlen(row) - 1);
        }
    }

    // size
    {
        char size_str[32];
        const char *size_col = COLOR_RESET;
        if (item->is_dir) {
            snprintf(size_str, sizeof(size_str), "<DIR>");
            size_col = COLOR_CYAN COLOR_BOLD;
        } else {
            format_size(item->size, size_str, sizeof(size_str));
            if (item->size >= (off_t)1024 * 1024 * 1024) size_col = COLOR_RED COLOR_BOLD;
            else if (item->size >= (off_t)1024 * 1024 * 50) size_col = COLOR_YELLOW COLOR_BOLD;
            else size_col = COLOR_GREEN;
        }

        char tmp[128];
        snprintf(tmp, sizeof(tmp), "%s%10s%s  ", size_col, size_str, COLOR_RESET);
        strncat(row, tmp, sizeof(row) - strlen(row) - 1);
    }

    // time
    {
        char time_str[32];
        format_time(item->mtime, time_str, sizeof(time_str));
        double age = difftime(now, item->mtime);
        const char *tcol = (age < 60 * 60 * 24 * 2) ? (COLOR_GREEN COLOR_BOLD) : (COLOR_DIM COLOR_GRAY);

        char tmp[128];
        snprintf(tmp, sizeof(tmp), "%s%-12s%s  ", tcol, time_str, COLOR_RESET);
        strncat(row, tmp, sizeof(row) - strlen(row) - 1);
    }

    // icon + name
    {
        char icon = '-';
        const char *name_col = COLOR_RESET;

        if (item->is_dir) { icon = 'D'; name_col = COLOR_CYAN COLOR_BOLD; }
        else if (S_ISLNK(item->mode)) { icon = '@'; name_col = COLOR_MAGENTA COLOR_BOLD; }
        else if (item->mode & S_IXUSR) { icon = '*'; name_col = COLOR_GREEN COLOR_BOLD; }
        else if (item->is_hidden) { icon = '.'; name_col = COLOR_DIM COLOR_MAGENTA; }

        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s%c%s %s%s%s",
                 COLOR_WHITE, icon, COLOR_RESET,
                 name_col,
                 opts.quote_names ? "\"" : "",
                 item->name);
        strncat(row, tmp, sizeof(row) - strlen(row) - 1);

        if (opts.quote_names) strncat(row, "\"", sizeof(row) - strlen(row) - 1);

        if (opts.add_slash && item->is_dir) {
            strncat(row, COLOR_DIM COLOR_GRAY "/", sizeof(row) - strlen(row) - 1);
            strncat(row, COLOR_RESET, sizeof(row) - strlen(row) - 1);
        } else {
            strncat(row, COLOR_RESET, sizeof(row) - strlen(row) - 1);
        }
    }

    print_row_content(width, row);
}
static void emit_directory_children_inline(const char *dir_path, int level, int width) {
    if (opts.depth <= 0) return;
    if (level > opts.depth) return;   // also fixes -D 1 behavior

    FileList *list = calloc(1, sizeof(*list));
    if (!list) return;

    if (load_directory(list, dir_path) != 0) {
        free(list);
        return;
    }

    sort_list(list);

    for (int i = 0; i < list->count; i++) {
        FileItem *child = &list->items[i];

        if (strcmp(child->name, ".") == 0 || strcmp(child->name, "..") == 0) continue;

        int is_last = (i == list->count - 1);

        char prefix[256];
        make_indent_prefix(prefix, sizeof(prefix), level, is_last);
        int prefix_visible = (int)strlen(prefix);

        if (opts.long_format) print_item_long_line(child, width, prefix, prefix_visible);
        else                 print_item_simple_line(child, width, prefix, prefix_visible);

        if (child->is_dir) {
            emit_directory_children_inline(child->full_path, level + 1, width);
        }
    }

    free(list);
}

static void draw_header(FileList *list, int width) {
    print_border_top(width);

    printf("%s%s%s ", COLOR_WHITE, GLYPH_V, COLOR_RESET);
    printf("%s%slsx%s %s", COLOR_BG_CYAN, COLOR_BOLD, COLOR_RESET, list->cwd);
    int title_visible = 1 + (int)strlen("lsx ") + (int)strlen(list->cwd);
    print_row_suffix(width, title_visible);

    print_border_mid(width);
}

static void draw_long_header_row(int width) {
    print_row_prefix();
    int used = 1;

    // no prefix column in header; nested items will insert a prefix before inode/perms, etc.
    if (opts.show_inode) {
        printf("%s%-8s%s ", COLOR_YELLOW COLOR_BOLD, "INODE", COLOR_RESET);
        used += 9;
    }

    printf("%s%-10s%s ", COLOR_YELLOW COLOR_BOLD, "PERMS", COLOR_RESET);
    used += 11;

    if (opts.numeric_ids) {
        printf("%s%-8s%s ", COLOR_YELLOW COLOR_BOLD, "UID", COLOR_RESET);
        used += 9;
        if (!opts.omit_group) {
            printf("%s%-8s%s ", COLOR_YELLOW COLOR_BOLD, "GID", COLOR_RESET);
            used += 9;
        }
    } else {
        printf("%s%-8s%s ", COLOR_YELLOW COLOR_BOLD, "OWNER", COLOR_RESET);
        used += 9;
        if (!opts.omit_group) {
            printf("%s%-8s%s ", COLOR_YELLOW COLOR_BOLD, "GROUP", COLOR_RESET);
            used += 9;
        }
    }

    printf("%s%10s%s  %s%-12s%s  %s%s%s",
           COLOR_YELLOW COLOR_BOLD, "SIZE", COLOR_RESET,
           COLOR_YELLOW COLOR_BOLD, "MODIFIED", COLOR_RESET,
           COLOR_YELLOW COLOR_BOLD, "NAME", COLOR_RESET);

    used += 10 + 2 + 12 + 2 + 4;
    print_row_suffix(width, used);

    print_border_mid(width);
}

static void draw_single_box_listing(const char *target_path) {
    int width = get_term_width();
    FileList *list = (FileList *)calloc(1, sizeof(*list));
    if (!list) return;

    if (load_directory(list, target_path) != 0) {
        if (errno == ENOTDIR) {
            if (load_single_file(list, target_path) != 0) { free(list); return; }
        } else {
            if (load_single_file(list, target_path) != 0) { free(list); return; }
        }
    }

    sort_list(list);

    // COMMA MODE: keep your original behavior (no boxes); depth doesn't apply here.
    if (opts.comma_separated) {
        for (int i = 0; i < list->count; i++) {
            FileItem *item = &list->items[i];
            const char *color = COLOR_RESET;
            if (item->is_dir) color = COLOR_CYAN;
            else if (item->mode & S_IXUSR) color = COLOR_GREEN;
            else if (S_ISLNK(item->mode)) color = COLOR_MAGENTA;

            printf("%s", color);
            if (opts.quote_names) printf("\"%s\"", item->name);
            else printf("%s", item->name);
            printf("%s", COLOR_RESET);
            if (i < list->count - 1) printf(", ");
        }
        printf("\n");
        if (opts.pattern) { /* freed in main */ }
        free(list);
        return;
    }

    // Draw ONE box header
    draw_header(list, width);

    if (opts.long_format) {
        draw_long_header_row(width);

        for (int i = 0; i < list->count; i++) {
            FileItem *item = &list->items[i];
            print_item_long_line(item, width, "", 0);

            // Inline children (depth)
            if (opts.depth > 0 && item->is_dir &&
                strcmp(item->name, ".") != 0 && strcmp(item->name, "..") != 0) {
                emit_directory_children_inline(item->full_path, 1, width);
            }
        }

    } else {
        for (int i = 0; i < list->count; i++) {
            FileItem *item = &list->items[i];
            print_item_simple_line(item, width, "", 0);

            // Inline children (depth)
            if (opts.depth > 0 && item->is_dir &&
                strcmp(item->name, ".") != 0 && strcmp(item->name, "..") != 0) {
                emit_directory_children_inline(item->full_path, 1, width);
            }
        }
    }

    print_border_bottom(width);
    printf("%s  %d items total%s\n", COLOR_DIM COLOR_GRAY, list->count, COLOR_RESET);

    free(list);
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [OPTIONS] [DIRECTORY|FILE]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -a            Show all files including hidden\n");
    fprintf(stderr, "  -l            Long format (table)\n");
    fprintf(stderr, "  -h            Human readable sizes (with -l)\n");
    fprintf(stderr, "  -g            Omit group column\n");
    fprintf(stderr, "  -F            Add slash to directories\n");
    fprintf(stderr, "  -i            Show inode numbers\n");
    fprintf(stderr, "  -R            Recursive listing (infinite inline depth)\n");
    fprintf(stderr, "  -D N          Inline depth inside ONE box (like tree -L). Example: -D 5\n");
    fprintf(stderr, "  --depth N     Same as -D\n");
    fprintf(stderr, "  -r            Reverse sort order\n");
    fprintf(stderr, "  -X            Sort by extension\n");
    fprintf(stderr, "  -t            Sort by modification time\n");
    fprintf(stderr, "  -n            Show numeric UIDs/GIDs\n");
    fprintf(stderr, "  -m            Comma-separated output\n");
    fprintf(stderr, "  -Q            Quote filenames\n");
    fprintf(stderr, "\nEnvironment:\n");
    fprintf(stderr, "  LSX_ASCII=1   Force ASCII borders (no UTF-8 box drawing)\n");
}

static struct option long_opts[] = {
    {"depth", required_argument, 0, 'D'},
    {0, 0, 0, 0}
};

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");
    init_glyphs();

    int opt;
    char cwd[MAX_PATH];

    opts.depth = 0;

    while ((opt = getopt_long(argc, argv, "alhgFiRrXtnmQD:", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'a': opts.show_hidden = 1; break;
            case 'l': opts.long_format = 1; break;
            case 'h': opts.human_readable = 1; break;
            case 'g': opts.omit_group = 1; break;
            case 'F': opts.add_slash = 1; break;
            case 'i': opts.show_inode = 1; break;
            case 'R': opts.recursive = 1; break;
            case 'r': opts.reverse = 1; break;
            case 'X': opts.sort_by_ext = 1; break;
            case 't': opts.sort_by_time = 1; break;
            case 'n': opts.numeric_ids = 1; break;
            case 'm': opts.comma_separated = 1; break;
            case 'Q': opts.quote_names = 1; break;

            case 'D': {
                int d = atoi(optarg);
                if (d < 0) {
                    fprintf(stderr, "lsx: --depth must be >= 0\n");
                    return 1;
                }
                opts.depth = d;
                break;
            }

            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Make -R enable infinite inline depth
    if (opts.recursive && opts.depth == 0) {
        opts.depth = 999;
    }

    const char *target = ".";
    if (optind < argc) {
        target = argv[optind];
        if (strchr(target, '*')) {
            opts.pattern = strdup(target);
            target = ".";
        }
    } else if (!getcwd(cwd, sizeof(cwd))) {
        perror("getcwd");
        return 1;
    } else {
        target = cwd;
    }

    draw_single_box_listing(target);

    if (opts.pattern) free(opts.pattern);
    return 0;
}
