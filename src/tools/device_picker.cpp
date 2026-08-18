/*---------------------------------------------------------------------------*\

  device_picker.cpp

  Minimal terminal UI for choosing an audio device from a list.

\*---------------------------------------------------------------------------*/

#include "device_picker.h"

#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace device_picker {

namespace {

/* ── Raw terminal mode ─────────────────────────────────────────────────── */

class RawMode {
public:
    bool enter() {
        if (tcgetattr(STDIN_FILENO, &saved_) != 0) return false;
        struct termios raw = saved_;
        /* Character-at-a-time, no echo.  ISIG is cleared too so that Ctrl-C
           comes through as a byte and we can restore the terminal ourselves. */
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;
        active_ = true;
        return true;
    }

    void leave() {
        if (active_) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_);
            active_ = false;
        }
    }

    ~RawMode() { leave(); }

private:
    struct termios saved_ {};
    bool           active_ = false;
};

/* ── Key input ─────────────────────────────────────────────────────────── */

enum Key { KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_ENTER, KEY_QUIT, KEY_OTHER };

/* Read one byte, retrying on interrupted reads.  Returns false at EOF. */
bool read_byte(unsigned char* c) {
    for (;;) {
        ssize_t n = read(STDIN_FILENO, c, 1);
        if (n == 1) return true;
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
}

/* Read one byte, giving up after ~100 ms.  Used to tell a lone Esc from the
   start of an arrow-key escape sequence. */
bool read_byte_timeout(unsigned char* c) {
    struct termios before, tmp;
    if (tcgetattr(STDIN_FILENO, &before) != 0) return false;
    tmp = before;
    tmp.c_cc[VMIN]  = 0;
    tmp.c_cc[VTIME] = 1;                     /* tenths of a second */
    tcsetattr(STDIN_FILENO, TCSANOW, &tmp);

    ssize_t n;
    do { n = read(STDIN_FILENO, c, 1); } while (n < 0 && errno == EINTR);

    tcsetattr(STDIN_FILENO, TCSANOW, &before);
    return n == 1;
}

Key read_key() {
    unsigned char c;
    if (!read_byte(&c)) return KEY_QUIT;         /* stdin closed */

    switch (c) {
    case '\r': case '\n': case ' ': return KEY_ENTER;
    case 'k': case 'K':             return KEY_UP;
    case 'j': case 'J':             return KEY_DOWN;
    case 'q': case 'Q':             return KEY_QUIT;
    case 3:   case 4:               return KEY_QUIT;   /* Ctrl-C, Ctrl-D */
    default: break;
    }

    if (c != 0x1b) return KEY_OTHER;

    /* Escape: either a bare Esc or the start of a CSI/SS3 sequence */
    unsigned char b1;
    if (!read_byte_timeout(&b1)) return KEY_QUIT;    /* bare Esc = cancel */
    if (b1 != '[' && b1 != 'O')  return KEY_OTHER;

    unsigned char b2;
    if (!read_byte_timeout(&b2)) return KEY_OTHER;

    switch (b2) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    /* "\x1b[1~" / "\x1b[4~" style Home/End: swallow the trailing '~' */
    case '1': case '7': { unsigned char t; read_byte_timeout(&t); return KEY_HOME; }
    case '4': case '8': { unsigned char t; read_byte_timeout(&t); return KEY_END;  }
    default:  return KEY_OTHER;
    }
}

/* ── Terminal geometry ─────────────────────────────────────────────────── */

void term_size(int* rows, int* cols) {
    struct winsize ws;
    *rows = 24;
    *cols = 80;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) *rows = ws.ws_row;
        if (ws.ws_col > 0) *cols = ws.ws_col;
    }
}

/* Truncate to at most `max_cols` bytes without splitting a UTF-8 character,
   appending an ellipsis when anything was dropped. */
std::string fit(const std::string& s, int max_cols) {
    if (max_cols <= 0) return std::string();
    if ((int)s.size() <= max_cols) return s;

    size_t cut = (size_t)(max_cols > 3 ? max_cols - 3 : max_cols);
    while (cut > 0 && (((unsigned char)s[cut]) & 0xC0) == 0x80) cut--;
    return s.substr(0, cut) + (max_cols > 3 ? "..." : "");
}

/* ── Frame rendering ───────────────────────────────────────────────────── */

const char* const RESET   = "\x1b[0m";
const char* const BOLD    = "\x1b[1m";
const char* const DIM     = "\x1b[2m";
const char* const REVERSE = "\x1b[7m";
const char* const CLR_EOL = "\x1b[2K";

class Frame {
public:
    /* Rewind over the previous frame so the new one overwrites it. */
    void begin() {
        buf_.clear();
        if (lines_ > 0) buf_ += "\x1b[" + std::to_string(lines_) + "A";
        pending_ = 0;
    }

    void line(const std::string& text) {
        buf_ += CLR_EOL;
        buf_ += text;
        buf_ += "\r\n";
        pending_++;
    }

    void end() {
        lines_ = pending_;
        fputs(buf_.c_str(), stderr);
        fflush(stderr);
    }

    /* Erase the frame and leave a single summary line in its place. */
    void collapse(const std::string& summary) {
        std::string out;
        if (lines_ > 0) out += "\x1b[" + std::to_string(lines_) + "A";
        out += CLR_EOL;
        out += summary;
        out += "\r\n";
        for (int i = 1; i < lines_; i++) out += std::string(CLR_EOL) + "\r\n";
        if (lines_ > 1) out += "\x1b[" + std::to_string(lines_ - 1) + "A";
        lines_ = 0;
        fputs(out.c_str(), stderr);
        fflush(stderr);
    }

private:
    std::string buf_;
    int         lines_   = 0;
    int         pending_ = 0;
};

}  // namespace

/* ── Public interface ──────────────────────────────────────────────────── */

bool available() {
    return isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
}

int pick(const std::string& title, const std::string& hint,
         const std::vector<AudioDevice>& devices) {
    RawMode raw;
    if (!raw.enter()) return PICK_CANCEL;

    /* The list is the devices followed by a "None" entry. */
    const int count = (int)devices.size() + 1;
    const int none_index = count - 1;

    int selected = 0;
    int top      = 0;
    Frame frame;
    int   result = PICK_CANCEL;

    fputs("\x1b[?25l", stderr);          /* hide cursor */

    for (;;) {
        int rows, cols;
        term_size(&rows, &cols);

        /* header (2) + footer (3) + a little breathing room */
        int visible = rows - 6;
        if (visible < 1)     visible = 1;
        if (visible > count) visible = count;

        /* keep the selection inside the viewport */
        if (selected < top)            top = selected;
        if (selected >= top + visible) top = selected - visible + 1;
        if (top > count - visible)     top = count - visible;
        if (top < 0)                   top = 0;

        frame.begin();
        frame.line(std::string(BOLD) + fit(title, cols) + RESET);
        frame.line(std::string(DIM) + fit(hint, cols) + RESET);

        for (int i = top; i < top + visible; i++) {
            bool is_none = (i == none_index);
            std::string label = is_none ? "None" : devices[i].name;
            if (label.empty()) label = devices[i].hw_id;

            std::string row = (i == selected ? " > " : "   ") + fit(label, cols - 4);
            if (i == selected) row = std::string(REVERSE) + row + RESET;
            else if (is_none)  row = std::string(DIM) + row + RESET;
            frame.line(row);
        }

        /* footer: the hw_id of the highlighted entry, plus key help */
        std::string detail = (selected == none_index)
                                 ? "leave this setting unconfigured"
                                 : "id: " + devices[selected].hw_id;
        frame.line(std::string(DIM) + fit("   " + detail, cols) + RESET);

        char pos[64];
        snprintf(pos, sizeof(pos), "%d/%d", selected + 1, count);
        frame.line(std::string(DIM) +
                   fit("   Up/Down move   Enter select   q cancel   (" +
                       std::string(pos) + ")", cols) +
                   RESET);
        frame.end();

        Key k = read_key();
        if (k == KEY_UP) {
            selected = (selected == 0) ? count - 1 : selected - 1;
        } else if (k == KEY_DOWN) {
            selected = (selected == count - 1) ? 0 : selected + 1;
        } else if (k == KEY_HOME) {
            selected = 0;
        } else if (k == KEY_END) {
            selected = count - 1;
        } else if (k == KEY_ENTER) {
            result = (selected == none_index) ? PICK_NONE : selected;
            break;
        } else if (k == KEY_QUIT) {
            result = PICK_CANCEL;
            break;
        }
    }

    std::string chosen = (result == PICK_NONE || result == PICK_CANCEL)
                             ? "none"
                             : devices[result].name;
    int rows, cols;
    term_size(&rows, &cols);
    frame.collapse(std::string(BOLD) + fit(title + ": " + chosen, cols) + RESET);

    fputs("\x1b[?25h", stderr);          /* show cursor */
    fflush(stderr);
    raw.leave();
    return result;
}

}  // namespace device_picker
