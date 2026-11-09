/*
 * doomgeneric on pizdos: the screen is ANSI half blocks on the terminal,
 * the keyboard is the terminal in raw mode. Every cell is a upper half
 * block whose foreground and background colors box-average the 320x200
 * palette frame, and only cells that changed reach the terminal. Not a
 * terminal, or DG_HEADLESS in the environment: no escapes, no keys, and a
 * hash of every 35th frame on stdout, which is how the test watches the
 * game play. Ctrl-C quits: the terminal in raw mode sends no SIGINT.
 *
 * A terminal sends bytes, not keys: escape sequences name the arrows and
 * friends, a capital letter carries its shift, and a control byte is a
 * letter with Ctrl held. What doom wants but the terminal cannot say
 * arrives by contract: z (or any control byte) holds doom's fire, a
 * capital holds the shift that runs, space is use, and , and . strafe.
 * Keys the terminal repeats are held until the repeats stop.
 */

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_video.h"

enum {
    maxCols = 160,
    maxRows = 100,
    keyQueueSize = 256,
    /* 160x100 cells of a cursor position, truecolor colors and the block */
    outSize = maxCols * maxRows * 48,
};

struct cell {
    uint32_t top;
    uint32_t bottom;
};

static struct termios savedTio;
static int tty;
static int cols = 80;
static int rows = 50;
static uint32_t frameCount;
static uint32_t palette[256];
static struct cell lastFrame[maxCols * maxRows];
static int lastValid;
static char out[outSize];
static char* outAt;

/* the terminal keeps the colors of the last cell we drew with */
static uint32_t drawnFg = 0xffffffff;
static uint32_t drawnBg = 0xffffffff;

static unsigned short keyQueue[keyQueueSize];
static int keyRead;
static int keyWrite;

/* the escape sequence being read: 1 a lone ESC, 2 CSI, 3 SS3 */
static int escState;
static unsigned escParam;

static void restoreTerminal(void) {
    if (!tty) {
        return;
    }

    tty = 0;
    write(1, "\x1b[?25h", 6);
    write(1, "\x1b[?1049l", 8);
    tcsetattr(0, TCSANOW, &savedTio);
}

static void quitGame(void) {
    restoreTerminal();
    exit(0);
}

static void queueKey(int pressed, unsigned char key) {
    keyQueue[keyWrite] = (unsigned short)((pressed << 8) | key);
    keyWrite = (keyWrite + 1) % keyQueueSize;

    if (keyWrite == keyRead) {
        /* the queue overflowed: the oldest event is lost */
        keyRead = (keyRead + 1) % keyQueueSize;
    }
}

/* a terminal sends only presses: doom also wants the release, and its own
 * auto-repeat sustains the key */
static void pressRelease(unsigned char key) {
    queueKey(1, key);
    queueKey(0, key);
}

/* keys held by the terminal's auto-repeat: the byte arrives again and
 * again while the finger stays on it, and a gap releases the key. Arrows
 * turn and walk, , and . strafe, z (or any control byte) is doom's fire
 * (no terminal sends the Ctrl key itself), space is use, and uppercase
 * rides a held shift. */
enum { heldMax = 6, holdMs = 180 };

static struct {
    unsigned char key;
    uint32_t at;
} held[heldMax];

static void sustainKey(unsigned char key) {
    for (int i = 0; i < heldMax; i++) {
        if (held[i].key == key) {
            held[i].at = DG_GetTicksMs();
            return;
        }
    }

    for (int i = 0; i < heldMax; i++) {
        if (held[i].key == 0) {
            held[i].key = key;
            held[i].at = DG_GetTicksMs();
            queueKey(1, key);
            return;
        }
    }
}

static void expireHeld(void) {
    uint32_t now = DG_GetTicksMs();

    for (int i = 0; i < heldMax; i++) {
        if (held[i].key != 0 && (int)(now - held[i].at) > holdMs) {
            queueKey(0, held[i].key);
            held[i].key = 0;
        }
    }
}

static void arrow(char final) {
    switch (final) {
    case 'A':
        sustainKey(KEY_UPARROW);
        break;
    case 'B':
        sustainKey(KEY_DOWNARROW);
        break;
    case 'C':
        sustainKey(KEY_RIGHTARROW);
        break;
    case 'D':
        sustainKey(KEY_LEFTARROW);
        break;
    default:
        return;
    }
}

static void keyByte(unsigned char c) {
    if (escState == 2) {
        if (c >= '0' && c <= '9') {
            escParam = escParam * 10 + (unsigned)(c - '0');
            return;
        }

        escState = 0;

        switch (c) {
        case 'A':
        case 'B':
        case 'C':
        case 'D':
        case 'H':
        case 'F':
            arrow(c);

            if (c == 'H') {
                queueKey(0, KEY_UPARROW);
            }

            if (c == 'F') {
                queueKey(0, KEY_DOWNARROW);
            }

            return;
        case '~':
            switch (escParam) {
            case 1:
            case 7:
                pressRelease(KEY_HOME);
                return;
            case 4:
            case 8:
                pressRelease(KEY_END);
                return;
            case 5:
                pressRelease(KEY_PGUP);
                return;
            case 6:
                pressRelease(KEY_PGDN);
                return;
            }

            return;
        default:
            return;
        }
    }

    if (escState == 3) {
        escState = 0;
        arrow(c);
        return;
    }

    if (c == 0x1b) {
        escState = 1;
        return;
    }

    if (escState == 1) {
        escState = 0;

        if (c == '[') {
            escState = 2;
            escParam = 0;
            return;
        }

        if (c == 'O') {
            escState = 3;
            return;
        }

        /* a lone ESC is the Escape key: sequences arrive in one read */
        pressRelease(KEY_ESCAPE);
        keyByte(c);
        return;
    }

    switch (c) {
    case '\r':
    case '\n':
        pressRelease(KEY_ENTER);
        return;
    case 0x7f:
    case 0x08:
        pressRelease(KEY_BACKSPACE);
        return;
    case '\t':
        pressRelease(KEY_TAB);
        return;
    case 0x03:
        quitGame();
        return;
    case ' ':
        /* the space bar opens doors and flips switches */
        sustainKey(KEY_USE);
        return;
    case 'z':
        /* fire on a plain letter: the Mac's menu and terminal own the
         * control bytes more often than not */
        sustainKey(KEY_FIRE);
        return;
    case ',':
        sustainKey(KEY_STRAFE_L);
        return;
    case '.':
        sustainKey(KEY_STRAFE_R);
        return;
    case '+':
    case '=':
        pressRelease(KEY_EQUALS);
        return;
    case '-':
        pressRelease(KEY_MINUS);
        return;
    default:
        break;
    }

    /* a control byte is a letter with Ctrl held: the terminal never sends
     * the modifier alone, so doom's fire key rides on any of them */
    if (c >= 0x01 && c <= 0x1a) {
        if (c == 0x10) {
            pressRelease(KEY_PAUSE);
            return;
        }

        sustainKey(KEY_FIRE);
        return;
    }

    if (c >= 'A' && c <= 'Z') {
        /* the shift key never arrives by itself: it rides the letter */
        unsigned char lower = (unsigned char)(c - 'A' + 'a');

        sustainKey(KEY_RSHIFT);

        if (lower == 'z') {
            sustainKey(KEY_FIRE);
        } else {
            pressRelease(lower);
        }

        return;
    }

    if (c >= 0x20 && c < 0x7f) {
        pressRelease(c);
    }
}

static void readKeys(void) {
    struct pollfd p;
    char buf[64];

    expireHeld();

    p.fd = 0;
    p.events = POLLIN;

    while (poll(&p, 1, 0) == 1 && (p.revents & POLLIN) != 0) {
        ssize_t n = read(0, buf, sizeof(buf));

        if (n <= 0) {
            return;
        }

        for (ssize_t i = 0; i < n; i++) {
            keyByte((unsigned char)buf[i]);
        }
    }

    if (escState == 1) {
        escState = 0;
        pressRelease(KEY_ESCAPE);
    }
}

static void put(const char* s, size_t n) {
    memcpy(outAt, s, n);
    outAt += n;
}

static void putInt(uint32_t v) {
    char digits[10];
    int n = 0;

    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);

    while (n > 0) {
        *outAt++ = digits[--n];
    }
}

/* the box of source pixels a cell half covers, averaged through the
 * palette: plain integer sums, no gamma */
static uint32_t average(int x0, int x1, int y0, int y1) {
    uint32_t r = 0;
    uint32_t g = 0;
    uint32_t b = 0;
    uint32_t rgb;

    for (int y = y0; y < y1; y++) {
        const uint8_t* line = DG_ScreenBuffer + y * SCREENWIDTH;

        for (int x = x0; x < x1; x++) {
            rgb = palette[line[x]];
            r += rgb >> 16;
            g += (rgb >> 8) & 0xff;
            b += rgb & 0xff;
        }
    }

    rgb = (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0);
    return ((r / rgb) << 16) | ((g / rgb) << 8) | (b / rgb);
}

static void putColor(const char* which, uint32_t rgb) {
    put(which, 5);
    putInt(rgb >> 16);
    *outAt++ = ';';
    putInt((rgb >> 8) & 0xff);
    *outAt++ = ';';
    putInt(rgb & 0xff);
    *outAt++ = ';';
}

static void drawCells(void) {
    /* the terminal sits at the cell after the last one we drew */
    int atCol = -1;
    int atRow = -1;

    outAt = out;

    for (int r = 0; r < rows; r++) {
        int y0 = r * SCREENHEIGHT / rows;
        int y1 = (r + 1) * SCREENHEIGHT / rows;
        int mid = (y0 + y1) / 2;

        for (int c = 0; c < cols; c++) {
            struct cell cell;
            int x0 = c * SCREENWIDTH / cols;
            int x1 = (c + 1) * SCREENWIDTH / cols;
            int i = r * cols + c;

            cell.top = average(x0, x1, y0, mid);
            cell.bottom = average(x0, x1, mid, y1);

            if (lastValid && cell.top == lastFrame[i].top && cell.bottom == lastFrame[i].bottom) {
                continue;
            }

            lastFrame[i] = cell;

            if (c != atCol || r != atRow) {
                put("\x1b[", 2);
                putInt((uint32_t)(r + 1));
                *outAt++ = ';';
                putInt((uint32_t)(c + 1));
                *outAt++ = 'H';
            }

            if (cell.top != drawnFg || cell.bottom != drawnBg) {
                put("\x1b[", 2);
                putColor("38;2;", cell.top);
                putColor("48;2;", cell.bottom);
                *outAt++ = 'm';
                drawnFg = cell.top;
                drawnBg = cell.bottom;
            }

            put("\xe2\x96\x80", 3);
            atCol = c + 1;
            atRow = r;
        }
    }
}

static void headlessFrame(void) {
    const uint8_t* p = (const uint8_t*)DG_ScreenBuffer;
    uint32_t hash = 2166136261u;

    if (frameCount % 35 != 1) {
        return;
    }

    for (int i = 0; i < SCREENWIDTH * SCREENHEIGHT; i++) {
        hash = (hash ^ p[i]) * 16777619u;
    }

    for (int i = 0; i < 256; i++) {
        hash = (hash ^ colors[i].r) * 16777619u;
        hash = (hash ^ colors[i].g) * 16777619u;
        hash = (hash ^ colors[i].b) * 16777619u;
    }

    printf("frame %u %08x\n", frameCount, hash);
}

void DG_Init() {
    struct winsize ws;
    struct termios raw;
    const char* env;

    tty = isatty(0) && isatty(1) && getenv("DG_HEADLESS") == NULL;

    if (!tty) {
        setvbuf(stdout, NULL, _IONBF, 0);
        return;
    }

    if (tcgetattr(0, &savedTio) != 0) {
        tty = 0;
        setvbuf(stdout, NULL, _IONBF, 0);
        return;
    }

    raw = savedTio;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);
    atexit(restoreTerminal);
    write(1, "\x1b[?1049h\x1b[?25l", 14);
    write(1, "\x1b[2J\x1b[H", 7);

    cols = 80;
    rows = 50;

    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }

    if ((env = getenv("DG_COLS")) != NULL && atoi(env) > 0) {
        cols = atoi(env);
    }

    if ((env = getenv("DG_ROWS")) != NULL && atoi(env) > 0) {
        rows = atoi(env);
    }

    if (cols > maxCols) {
        cols = maxCols;
    }

    if (cols < 20) {
        cols = 20;
    }

    if (rows > maxRows) {
        rows = maxRows;
    }

    if (rows < 12) {
        rows = 12;
    }
}

void DG_DrawFrame() {
    frameCount++;

    if (!tty) {
        headlessFrame();
        return;
    }

    if (palette_changed) {
        for (int i = 0; i < 256; i++) {
            palette[i] = (colors[i].r << 16) | (colors[i].g << 8) | colors[i].b;
        }

        palette_changed = false;
    }

    drawCells();
    write(1, out, (size_t)(outAt - out));
    lastValid = 1;
}

void DG_SleepMs(uint32_t ms) {
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;

    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

uint32_t DG_GetTicksMs() {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int DG_GetKey(int* pressed, unsigned char* key) {
    if (tty) {
        readKeys();
    }

    if (keyRead == keyWrite) {
        return 0;
    }

    unsigned short entry = keyQueue[keyRead];
    keyRead = (keyRead + 1) % keyQueueSize;
    *pressed = entry >> 8;
    *key = (unsigned char)(entry & 0xff);
    return 1;
}

void DG_SetWindowTitle(const char* title) {
    (void)title;
}

int main(int argc, char** argv) {
    doomgeneric_Create(argc, argv);

    for (;;) {
        doomgeneric_Tick();
    }
}
