/* Copyright (c), Niclas Meyer <niclas@countingsort.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>

#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <libgen.h>
#include <limits.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef __GNUC__
#define UNUSED(x) UNUSED_##x __attribute__((__unused__))
#else
#define UNUSED(x) UNUSED_##x
#endif /* __GNUC__ */

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 256
#endif /* HOST_NAME_MAX */

#ifndef SIGWINCH
#define SIGWINCH 28
#endif /* SIGWINCH */

#define CTRL_KEY(k) ((k) - 0x60)

#define ENT_ALLOC_NUM 64
#define LINE_READER_INITIAL_SIZE 64

enum key {
    KEY_LOWEST = 256,
    KEY_NOKEY,
    KEY_UP,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
};

struct buffer {
    char *start;
    char *gap;
    char *post;
    char *end;
    int relpos; // assumed to be vaild at all times
};

struct direlement {
    enum {
        TYPE_DIR,
        TYPE_SYML,
        TYPE_SYML_TO_DIR,
        TYPE_EXEC,
        TYPE_NORM,
    } type;

    char name[NAME_MAX + 1];
    bool is_selected;
};

static struct termios g_old_termios;
static volatile sig_atomic_t g_needs_redraw = false;
static volatile sig_atomic_t g_quit         = false;

/**
 * Deletes a file. Can be passed to nftw
 */
static int
delete_file(
    const char *fpath,
    const struct stat *UNUSED(sb),
    int UNUSED(typeflag),
    struct FTW *UNUSED(ftwbuf))
{
    return remove(fpath);
}

/**
 * Got too used to rust. This falls back to fallback, if name isn't set
 */
static const char *
getenv_or(const char *name, const char *fallback)
{
    const char *res = getenv(name);
    if (!res) {
        return fallback;
    }
    return res;
}

/**
 * Natural compare function respecting numbers, instead of just checking digits
 */
static int
strnatcmp(const char *s1, const char *s2)
{
    for (;;) {
        if (*s2 == '\0') {
            return *s1 != '\0';
        }

        if (*s1 == '\0') {
            return 1;
        }

        if (!(isdigit(*s1) && isdigit(*s2))) {
            if (*s1 != *s2) {
                return (int)*s1 - (int)*s2;
            }
            ++s1;
            ++s2;
        } else {
            const char *lim1;
            const char *lim2;
            unsigned long n1 = strtoul(s1, &lim1, 10);
            unsigned long n2 = strtoul(s2, &lim2, 10);
            if (n1 > n2) {
                return 1;
            } else if (n1 < n2) {
                return -1;
            }
            s1 = lim1;
            s2 = lim2;
        }
    }
}

/**
 * Comparison function for direlements
 */
static int
direlemcmp(const void *va, const void *vb)
{
    const struct direlement *a = va;
    const struct direlement *b = vb;

    bool a_is_dir = a->type == TYPE_DIR || a->type == TYPE_SYML_TO_DIR;
    bool b_is_dir = b->type == TYPE_DIR || b->type == TYPE_SYML_TO_DIR;

    if (a_is_dir != b_is_dir) {
        return a_is_dir ? -1 : 1;
    }

    return strnatcmp(a->name, b->name);
}

/**
 * Sets the terminal size on row
 */
static bool
get_term_size(int *row, int *col)
{
    struct winsize wsize;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsize) < 0) {
        perror("ioctl");
        return false;
    }

    *row = wsize.ws_row;
    *col = wsize.ws_col;

    return true;
}

/**
 * Used as SIGWINCH (terminal resize handler)
 */
static void
handle_winch(int UNUSED(sig))
{
    g_needs_redraw = true;
}

/**
 * Used for SIGINT and SIGTERM to pass the exit signal to main()
 */
static void
handle_exit(int UNUSED(sig))
{
    g_quit = true;
}

/**
 * Saves the current session (current path and selected file)
 * to /tmp/filet_dir and /tmp/filet_sel
 */
static void
save_session(const char *path, const char *sel_name)
{
    FILE *f = fopen("/tmp/filet_dir", "w");
    if (f) {
        fprintf(f, "%s\n", path);
        fclose(f);
    }
    f = fopen("/tmp/filet_sel", "w");
    if (f) {
        fprintf(f, "%s/%s\n", path, sel_name);
        fclose(f);
    }
}

/**
 * Resets the terminal to its prior state
 */
static void
restore_terminal(void)
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_old_termios) < 0) {
        perror("tcsetattr");
    }

    printf(
        "\033[?7h"    // enable line wrapping
        "\033[?25h"   // unhide cursor
        "\033[;r"     // reset scroll region
        "\033[?1049l" // restore main screen
    );
}

/**
 * Saves old terminal configuration to reset on SIGINT or SIGTERM
 * restore_terminal will use this to restore the settings.
 */
static bool
get_termios(void)
{
    if (tcgetattr(STDIN_FILENO, &g_old_termios) < 0) {
        perror("tcgetattr");
        return false;
    }

    return true;
}

/**
 * Sets up the terminal for TUI use (read every char, differentiate \r and \n,
 * don't echo, hide the cursor, fix a scroll region, switch to a second screen)
 */
static bool
setup_terminal(int row)
{
    setvbuf(stdout, NULL, _IOFBF, 0);

    struct termios raw = g_old_termios;
    raw.c_oflag &= ~OPOST;
    raw.c_lflag &= ~(ECHO | ICANON);

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
        perror("tcsetattr");
        return false;
    }

    printf(
        "\033[?1049h" // use alternative screen buffer
        "\033[?7l"    // diable line wrapping
        "\033[?25l"   // hide cursor
        "\033[2J"     // clear screen
        "\033[3;%dr", // limit scrolling to scrolling area
        row);

    return true;
}

/**
 * Read a directory into ents.
 *
 * Returns the number of elements in the dir.
 */
static size_t
read_dir(
    const char *path,
    struct direlement **ents,
    size_t *ents_size,
    bool show_hidden)
{
    size_t n = 0;
    DIR *dir;
    dir = opendir(path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir))) {
            const char *name = ent->d_name;
            int fd           = dirfd(dir);
            struct stat sb;

            if (name[0] == '.' &&
                (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
                continue;
            }

            if (!show_hidden && name[0] == '.') {
                continue;
            }

            if (fstatat(fd, name, &sb, AT_SYMLINK_NOFOLLOW) < 0) {
                continue;
            }

            if (n == *ents_size) {
                *ents_size += ENT_ALLOC_NUM;
                struct direlement *tmp =
                    realloc(*ents, *ents_size * sizeof(*tmp));
                if (!tmp) {
                    perror("realloc");
                    exit(EXIT_FAILURE);
                }
                *ents = tmp;
            }

            strcpy((*ents)[n].name, ent->d_name);
            (*ents)[n].is_selected = false;

            if (S_ISDIR(sb.st_mode)) {
                (*ents)[n].type = TYPE_DIR;
            } else if (S_ISLNK(sb.st_mode)) {
                if (!(fstatat(dirfd(dir), (*ents)[n].name, &sb, 0) < 0 ||
                      !S_ISDIR(sb.st_mode))) {
                    (*ents)[n].type = TYPE_SYML_TO_DIR;
                } else {
                    (*ents)[n].type = TYPE_SYML;
                }
            } else {
                if (sb.st_mode & S_IXUSR) {
                    (*ents)[n].type = TYPE_EXEC;
                } else {
                    (*ents)[n].type = TYPE_NORM;
                }
            }

            ++n;
        }
        qsort(*ents, n, sizeof(**ents), direlemcmp);
        closedir(dir);
    }

    return n;
}

/**
 * Spawns a new process, waits for it and returns
 */
static void
spawn(const char *path, const char *cmd, const char *argv1, int row)
{
    int status;
    pid_t pid = fork();

    if (pid < 0) {
        return;
    }

    restore_terminal();
    fflush(stdout);

    if (pid == 0) {
        if (chdir(path) < 0) {
            _exit(EXIT_FAILURE);
        }
        execlp(cmd, cmd, argv1, NULL);
        // NOTREACHED
        _exit(EXIT_FAILURE);
    } else {
        do {
            waitpid(pid, &status, WUNTRACED);
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
    }

    setup_terminal(row);
}

/**
 * Gets the parent dir of a given path (without trailing /) and writes it back
 */
static void
parent_dir(char *path)
{
    if (path[1] != '\0') {
        char *end = strrchr(path, '/');
        if (path == end) {
            ++end;
        }
        *end = '\0';
    }
}

/**
 * Draws a single directory entry in it's own line
 *
 * Assumes the cursor is at the beginning of the line
 */
static void
draw_line(const struct direlement *ent, bool is_sel)
{
    switch (ent->type) {
    case TYPE_DIR:
        printf("\033[34;1m");
        break;
    case TYPE_SYML: // FALLTHROUGH
    case TYPE_SYML_TO_DIR:
        printf("\033[36;1m");
        break;
    case TYPE_EXEC:
        printf("\033[32;1m");
        break;
    case TYPE_NORM:
        printf("\033[m");
        break;
    }

    if (is_sel) {
        printf("> %c%s", ent->is_selected ? '*' : ' ', ent->name);
    } else {
        printf(
            " %c%s ",
            ent->is_selected ? '*' : ' ',
            ent->name); // space to clear the last char on unindenting it
    }
}

/**
 * Redraws the whole screen. Avoid this if possible
 */
static void
redraw(
    const struct direlement *ents,
    const char *user_and_hostname,
    const char *path,
    size_t n,
    size_t sel,
    size_t offset,
    int row)
{
    // clear screen and redraw status
    printf(
        "\033[2J"      // clear screen
        "\033[H"       // go to 0,0
        "%s"           // print username@hostname
        "\033[34;1m%s" // print path
        " \033[m[%zu]" // number of entries
        "\033[3;%dr"   // limit scrolling to scrolling area
        "\r\n",        // enter scrolling region
        user_and_hostname,
        path,
        n,
        row);

    if (n == 0) {
        printf("\n\033[31;7mdirectory empty\033[27m");
    } else {
        for (size_t i = offset; i < n && i - offset < (size_t)row - 2; ++i) {
            printf("\n");
            draw_line(&ents[i], i == sel);
            printf("\r");
        }
    }
}

/**
 * Reads a key from stdin
 *
 * Acts as a getchar wrapper that transforms arrow keys to hjkl
 */

static int
getkey(void)
{
    char c[3];
    int n = read(STDIN_FILENO, c, sizeof(c));

    if (n < 0) {
        return KEY_NOKEY;
    }

    if (n < sizeof(c)) {
        return c[0];
    }

    if (!(c[0] == '\033' && c[1] == '[')) {
        return c[0];
    }

    switch (c[2]) {
    case 'A':
        return KEY_UP;
    case 'B':
        return KEY_DOWN;
    case 'C':
        return KEY_RIGHT;
    case 'D':
        return KEY_LEFT;
    default:
        return KEY_NOKEY;
    }
}


// Included from github.com/buffet/bread
static bool
buffer_init(struct buffer *b, size_t init_size)
{
    b->start = malloc(init_size);
    if (!b->start) {
        return false;
    }

    b->gap    = b->start;
    b->end    = b->start + init_size;
    b->post   = b->end;
    b->relpos = 0;

    return true;
}

static void
buffer_forwards(struct buffer *b)
{
    if (b->post + b->relpos < b->end) {
        ++b->relpos;
    }
}

static void
buffer_backwards(struct buffer *b)
{
    if (b->gap + b->relpos > b->start) {
        --b->relpos;
    }
}

static void
buffer_move_gap(struct buffer *b)
{
    if (b->relpos == 0) {
        return;
    }

    if (b->relpos < 0) {
        b->gap += b->relpos;
        b->post += b->relpos;
        memmove(b->post, b->gap, -b->relpos);
    } else {
        memmove(b->gap, b->post, b->relpos);
        b->gap += b->relpos;
        b->post += b->relpos;
    }

    b->relpos = 0;
}

static bool
buffer_insertch(struct buffer *b, char ch)
{
    buffer_move_gap(b);

    if (b->gap == b->post) {
        size_t newsize = (b->end - b->start) * 2;
        char *newbuf   = realloc(b->start, newsize);

        if (!newbuf) {
            return false;
        }

        b->gap   = newbuf + (b->gap - b->start);
        b->post  = newbuf + newsize + (b->end - b->post);
        b->start = newbuf;
        b->end   = newbuf + newsize;
    }

    *b->gap++ = ch;

    return true;
}

static void
buffer_delete_forwards(struct buffer *b)
{
    buffer_move_gap(b);

    if (b->post < b->end) {
        ++b->post;
    }
}

static void
buffer_delete_backwards(struct buffer *b)
{
    buffer_move_gap(b);

    if (b->gap > b->start) {
        --b->gap;
    }
}

char *
read_line(const char *prompt)
{
    printf("\033[?25h" // Show cursor
        "%s", prompt);
    fflush(stdout);

    size_t prompt_len = 0;
    bool counting     = true;

    for (const char *c = prompt; *c; ++c) {
        if (*c == '\001') {
            counting = false;
        } else if (*c == '\002') {
            counting = true;
        } else {
            if (counting) {
                ++prompt_len;
            }
        }
    }

    struct buffer buffer;
    if (!buffer_init(&buffer, LINE_READER_INITIAL_SIZE)) {
        return NULL;
    }

    for (;;) {
        int k = getkey();

        if (k == '\n') {
            break;
        }

        switch (k) {
        case KEY_NOKEY:
            // EMPTY
            break;
        case CTRL_KEY('b'):
        case KEY_LEFT:
            buffer_backwards(&buffer);
            break;
        case CTRL_KEY('f'):
        case KEY_RIGHT:
            buffer_forwards(&buffer);
            break;
        case KEY_UP:
        case KEY_DOWN:
            // TODO: handle
            break;
        case CTRL_KEY('a'):
            buffer.relpos = buffer.start - buffer.gap - buffer.relpos;
            break;
        case CTRL_KEY('e'):
            buffer.relpos = buffer.end - buffer.post;
            break;
        case CTRL_KEY('d'):
            buffer_delete_forwards(&buffer);
            break;
        case CTRL_KEY('u'):
            buffer.gap  = buffer.start;
            buffer.post = buffer.end;
            break;
        case CTRL_KEY('h'):
        case '\x7f':
            buffer_delete_backwards(&buffer);
            break;
        default:
            if (!buffer_insertch(&buffer, k)) {
                free(buffer.start);
                return NULL;
            }
        }

        size_t postsize = buffer.end - buffer.post;

        printf(
            "\r\033[%luC\033[K%.*s%.*s",
            prompt_len,
            (int)(buffer.gap - buffer.start),
            buffer.start,
            (int)postsize,
            buffer.post);

        if (postsize - buffer.relpos > 0) {
            printf("\033[%luD", postsize - buffer.relpos);
        }
        fflush(stdout);
    }

    size_t presize  = buffer.gap - buffer.start;
    size_t postsize = buffer.end - buffer.post;
    size_t size     = presize + postsize;

    char *line = malloc(size + 1);
    if (!line) {
        free(buffer.start);
        return NULL;
    }

    memcpy(line, buffer.start, presize);
    memcpy(line + presize, buffer.post, postsize);

    line[size] = '\0';

    printf(
            "\033[?25l" // Hide cursor
            "\033[2K" // Clear line
    );
    g_needs_redraw = true;
    return line;
}

int
main(int argc, char **argv)
{
    if (!(isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))) {
        fprintf(stderr, "isatty: not connected to a tty");
        exit(EXIT_FAILURE);
    }

    char *path = malloc(PATH_MAX);
    if (!path) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    if (argc > 1) {
        if (!realpath(argv[1], path)) {
            perror("realpath");
            exit(EXIT_FAILURE);
        }
    } else {
        if (!getcwd(path, PATH_MAX)) {
            perror("getcwd");
            exit(EXIT_FAILURE);
        }
    }

    const char *depth = getenv("FILET_DEPTH");
    if (depth) {
        int level = atoi(depth) + 1;
        char levelstr[20];
        snprintf(levelstr, sizeof(levelstr), "%d", level);
        setenv("FILET_DEPTH", levelstr, true);
    } else {
        setenv("FILET_DEPTH", "1", true);
    }

    const char *editor = getenv_or("EDITOR", "vi");
    const char *shell  = getenv_or("SHELL", "/bin/sh");
    const char *home   = getenv_or("HOME", "/");
    const char *opener = getenv_or("FILET_OPENER", "xdg-open");

    struct passwd *pwuid = getpwuid(geteuid());
    if (!pwuid) {
        perror("getpwuid");
        exit(EXIT_FAILURE);
    }

    const char *user = pwuid->pw_name;

    char *hostname = malloc(HOST_NAME_MAX);
    if (!hostname) {
        perror("malloc");
    }

    if (gethostname(hostname, HOST_NAME_MAX) < 0) {
        perror("gethostname");
        free(hostname);
        hostname[0] = '\0';
    }

    size_t ents_size        = ENT_ALLOC_NUM;
    struct direlement *ents = malloc(ents_size * sizeof(*ents));
    if (!ents) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    int row = 0;
    int col = 0;
    if (!get_term_size(&row, &col)) {
        exit(EXIT_FAILURE);
    }

    struct sigaction sa_winch = {.sa_handler = handle_winch};
    if (sigaction(SIGWINCH, &sa_winch, NULL) < 0) {
        perror("sigaction WINCH");
        exit(EXIT_FAILURE);
    }

    struct sigaction sa_exit = {.sa_handler = handle_exit};
    if (sigaction(SIGTERM, &sa_exit, NULL) < 0) {
        perror("sigaction TERM");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGINT, &sa_exit, NULL) < 0) {
        perror("sigaction INT");
        exit(EXIT_FAILURE);
    }

    if (!get_termios()) {
        exit(EXIT_FAILURE);
    }

    if (!setup_terminal(row)) {
        exit(EXIT_FAILURE);
    }

    atexit(restore_terminal);

    size_t user_and_host_size =
        strlen(user) + strlen(hostname) + strlen("\033[32;1m@\033[m:") + 1;
    char *user_and_hostname = malloc(user_and_host_size);
    ;
    if (!user_and_hostname) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    if (hostname[0] != '\0') {
        snprintf(
            user_and_hostname,
            user_and_host_size,
            "\033[32;1m%s@%s\033[m:",
            user,
            hostname);
    } else {
        snprintf(
            user_and_hostname, user_and_host_size, "\033[32;1m%s\033[m:", user);
    }

    bool show_hidden = false;
    bool fetch_dir   = true;
    size_t sel       = 0;
    size_t y         = 0;
    size_t n;

    for (;;) {
        if (g_quit) {
            save_session(path, ents[sel].name);
            exit(EXIT_SUCCESS);
        }

        if (fetch_dir) {
            fetch_dir      = false;
            sel            = 0;
            y              = 0;
            n              = read_dir(path, &ents, &ents_size, show_hidden);
            g_needs_redraw = true;
        }

        if (g_needs_redraw) {
            g_needs_redraw = false;
            get_term_size(&row, &col);
            size_t scroll_size = row - 3;

            int empty_space = -(n - (sel - y + scroll_size));
            if (y > scroll_size) {
                y = scroll_size;
            } else if (empty_space > 0) {
                y = n >= scroll_size ? y + empty_space + 1 : sel;
            }
            redraw(ents, user_and_hostname, path, n, sel, sel - y, row);

            // move cursor to selection
            printf("\033[%zuH", y + 3);
        }

        fflush(stdout);

        int k = getkey();

        switch (k) {
        case 'h':
        case KEY_LEFT:
            parent_dir(path);
            fetch_dir = true;
            break;
        case '~':
            strcpy(path, home);
            fetch_dir = true;
            break;
        case '/':
            strcpy(path, "/");
            fetch_dir = true;
            break;
        case '.':
            show_hidden = !show_hidden;
            fetch_dir   = true;
            break;
        case 'r':
            fetch_dir = true;
            break;
        case 's': {
            save_session(path, ents[sel].name);
            spawn(path, shell, NULL, row);
            fetch_dir = true;
            break;
        }
        case 'q': {
            save_session(path, ents[sel].name);
            exit(EXIT_SUCCESS);
            break;
        }
        }

        if (n == 0) {
            continue; // rest of the commands require at least one entry
        }

        switch (k) {
        case 'j':
        case KEY_DOWN:
            if (sel < n - 1) {
                draw_line(&ents[sel], false);
                printf("\r\n");
                ++sel;
                draw_line(&ents[sel], true);
                printf("\r");

                if (y < (size_t)row - 3) {
                    ++y;
                }
            }
            break;
        case 'k':
        case KEY_UP:
            if (sel > 0) {
                draw_line(&ents[sel], false);
                if (y == 0) {
                    printf("\r\033[L");
                } else {
                    printf("\r\033[A");
                    --y;
                }
                --sel;
                draw_line(&ents[sel], true);
                printf("\r");
            }
            break;
        case 'l':
        case '\n':
        case KEY_RIGHT:
            if (ents[sel].type == TYPE_DIR ||
                ents[sel].type == TYPE_SYML_TO_DIR) {
                // don't append to /
                if (path[1] != '\0') {
                    strcat(path, "/");
                }
                strcat(path, ents[sel].name);
                fetch_dir = true;
            } else {
                if (opener) {
                    spawn(path, opener, ents[sel].name, row);
                }
                fetch_dir = true;
            }
            break;
        case 'c':
            printf("\033[%i;0H", row);
            char *target = read_line("cd ");
            char *dirname = NULL;

            if (target[0] == '~') { 
                target++;
                size_t dirname_size = strlen(home) + strlen(target) + 2;
                dirname = malloc(dirname_size);
                strcpy(dirname, home);
                strcat(dirname, target);
            } else {
                dirname = target;
            }
            if (!opendir(dirname)) {
                printf("\033[%i;0H" // Return to start of line
                    "Invalid Path", row);
                fflush(stdout);
                getchar();
            } else {
                strcpy(path, dirname);
                fetch_dir = true;
            }
            free(dirname);
            break;
        case 'g':
            if (sel - y == 0) {
                draw_line(&ents[sel], false);
                printf("\033[3H");
                sel = 0;
                draw_line(&ents[sel], true);
                printf("\r");
            } else {
                // screen needs to be redrawn
                sel = 0;
                y   = 0;
                redraw(ents, user_and_hostname, path, n, sel, 0, row);
                printf("\033[3H");
            }
            break;
        case 'G':
            if (sel + row - 2 - y >= n) {
                draw_line(&ents[sel], false);
                printf(
                    "\033[%luH", 2 + (n < ((size_t)row - 3) ? n : (size_t)row));
                sel = n - 1;
                y   = row - 3;
                draw_line(&ents[sel], true);
                printf("\r");
            } else {
                // screen needs to be redrawn
                sel = n - 1;
                y   = row - 3;
                redraw(
                    ents, user_and_hostname, path, n, sel, n - (row - 2), row);
                printf("\033[%dH", row);
            }
            break;
        case 'e':
            spawn(path, editor, ents[sel].name, row);
            fetch_dir = true;
            break;
        case 'm':
            ents[sel].is_selected = !ents[sel].is_selected;
            draw_line(&ents[sel], true);
            printf("\r");
            break;
        case 'x': {
            int fd = open(path, 0);
            if (fd < 0) {
                continue;
            }
            for (size_t i = 0; i < n; ++i) {
                if (ents[i].is_selected) {
                    if (ents[i].type == TYPE_DIR) {
                        nftw(
                            ents[i].name,
                            delete_file,
                            32,
                            FTW_DEPTH | FTW_MOUNT | FTW_PHYS);
                    } else {
                        unlinkat(
                            fd,
                            ents[i].name,
                            ents[i].type == TYPE_DIR ? AT_REMOVEDIR : 0);
                    }
                }

                fetch_dir = true;
            }
            close(fd);
            break;
        }
        }
    }
}
