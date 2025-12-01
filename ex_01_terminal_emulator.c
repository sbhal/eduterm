#define _XOPEN_SOURCE 600
#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/*
 * ============================================================================
 * MENTAL MODEL: Understanding Terminal Emulators
 * ============================================================================
 *
 * FUNDAMENTAL CONCEPT:
 * A terminal emulator is a THREE-LAYER system that connects a graphical
 * display (X11 window) to command-line programs (shell, vim, etc.) through
 * a kernel-provided pseudoterminal driver.
 *
 *     ┌─────────────┐      ┌══════════════════┐      ┌──────────────────┐
 *     │ X11 Window  │ ←──→ │ Pseudoterminal   │ ←──→ │ Shell/Programs   │
 *     │ (Your Code) │      │ (Kernel Driver)  │      │ (Child Process)  │
 *     └─────────────┘      └══════════════════┘      └──────────────────┘
 *         Master FD                                        Slave FD
 *
 * KEY INSIGHT #1: The Pseudoterminal Does Most of the Work
 * --------------------------------------------------------
 * The kernel's PTY driver handles:
 * - Signal generation (^C → SIGINT, ^Z → SIGTSTP)
 * - Line editing in "cooked mode" (backspace, ^U to kill line)
 * - Echo control (password input with echo off)
 * - Terminal modes (raw vs cooked, canonical vs non-canonical)
 *
 * Your emulator ONLY needs to:
 * - Convert keyboard events to bytes → write to master FD
 * - Read bytes from master FD → display as text
 * - Parse escape sequences for colors/formatting (VT100, ANSI)
 *
 * KEY INSIGHT #2: Process Groups and Sessions
 * -------------------------------------------
 * When you fork a shell, you create a SESSION with PROCESS GROUPS:
 *
 *                          Session (controlled by PTY)
 *     ┌────────────────────────────────────────────────────────────┐
 *     │                                                              │
 *     │  ┌─────────────┐  ┌──────────────┐  ┌──────────────┐      │
 *     │  │   Shell     │  │  ls | grep   │  │  vim (bg)    │      │
 *     │  │  (leader)   │  │ (foreground) │  │              │      │
 *     │  └─────────────┘  └──────────────┘  └──────────────┘      │
 *     │   Process Group    Process Group     Process Group         │
 *     └────────────────────────────────────────────────────────────┘
 *
 * - setsid() creates a new session and makes the calling process the leader
 * - TIOCSCTTY ioctl makes the PTY the controlling terminal for the session
 * - Signals (^C, ^Z) are sent to the FOREGROUND process group only
 * - The kernel tracks which process group is foreground (via tcsetpgrp)
 *
 * KEY INSIGHT #3: Master/Slave File Descriptors - The Communication Channel
 * -------------------------------------------------------------------------
 * Think of the PTY as a bidirectional pipe with special powers. The kernel's
 * PTY driver sits between master and slave, intercepting and processing data.
 *
 * CONCRETE EXAMPLE - User types "ls" and presses Enter:
 *
 * 1. X11 KeyPress event → x11_key() converts to bytes: 'l', 's', '\n'
 * 2. write(master_fd, "ls\n", 3) → bytes go to PTY driver
 * 3. PTY driver processes:
 *    - Echoes "ls\n" back to master (if echo mode enabled)
 *    - Buffers the line until '\n' (if canonical mode enabled)
 *    - Makes bytes available on slave FD
 * 4. Shell reads from slave_fd (its stdin): read(0, buf, ...) gets "ls\n"
 * 5. Shell executes ls command
 * 6. ls writes output to stdout: write(1, "file1\nfile2\n", ...)
 * 7. Output goes through slave_fd → PTY driver → master_fd
 * 8. Terminal reads from master: read(master_fd, ...) gets "file1\nfile2\n"
 * 9. Terminal displays the text in X11 window
 *
 * MASTER FD (your emulator, parent process):
 *   write(master, "ls\n", 3)     → Sends keyboard input to child
 *   read(master, buf, 1024)      → Receives program output from child
 *
 * SLAVE FD (child process - shell, vim, etc.):
 *   read(slave, buf, 1024)       → Receives keyboard input (becomes stdin)
 *   write(slave, "hello", 5)     → Sends output to terminal (from stdout)
 *
 * The "child" is the shell (/bin/sh) or any program running inside it.
 * When you type "vim", the shell forks and execs vim, which inherits the
 * slave FD as its stdin/stdout/stderr.
 *
 * CRITICAL: The PTY driver is NOT just a dumb pipe. It:
 * - Converts '\n' to '\r\n' (or vice versa) based on terminal modes
 * - Generates signals: writing 0x03 (^C) to master → SIGINT to foreground group
 * - Implements line buffering in canonical mode
 * - Handles echo (typing shows on screen) without your code doing anything
 *
 * WHY TWO FDs? Why not just one bidirectional FD?
 * ================================================
 *
 * REASON #1: Process Isolation and Security
 * The master and slave live in DIFFERENT processes (parent vs child).
 *
 * CLARIFICATION: "Parent" and "Child" are Unix process terms:
 * - Parent = Terminal emulator (your GUI program)
 * - Child = Shell (/bin/sh) created by fork()
 * - Grandchildren = Programs shell runs (vim, ls, grep, etc.)
 *
 * Yes, the shell IS a child process of the terminal emulator!
 * When you run vim, it becomes a child of the shell (grandchild of terminal).
 *
 * Process tree:
 *   Terminal Emulator (PID 1000, has master FD)
 *   └─ Shell (PID 1001, has slave FD as stdin/stdout/stderr)
 *      ├─ vim (PID 1002, inherited slave FD as stdin/stdout/stderr)
 *      └─ ls (PID 1003, inherited slave FD as stdin/stdout/stderr)
 *
 * After fork(), the parent closes slave, child closes master. This ensures:
 * - Child can't interfere with terminal emulator's operations
 * - Terminal emulator can't accidentally write to wrong end
 * - Clean separation: each process only has the FD it needs
 *
 * REASON #2: The Kernel Needs to Know Direction
 * The PTY driver treats master and slave ASYMMETRICALLY:
 * - Data written to master → processed → appears on slave (input path)
 * - Data written to slave → processed → appears on master (output path)
 * - Different processing rules apply to each direction
 *
 * Example: In canonical mode with echo enabled:
 *   write(master, "a", 1) → PTY echoes "a" back to master AND sends to slave
 *   write(slave, "a", 1) → PTY sends "a" to master (no echo)
 *
 * REASON #3: Terminal Control Operations (ioctls)
 * Many terminal operations only make sense on one end:
 * - TIOCSWINSZ (set window size) → called by PARENT on master FD
 * - TIOCSCTTY (set controlling terminal) → called by CHILD on slave FD
 * - tcsetpgrp (set foreground process group) → called by CHILD on slave FD
 *
 * WHO CALLS WHAT:
 * Terminal emulator (parent) uses master FD for:
 *   - ioctl(master, TIOCSWINSZ, ...) to tell kernel the window size
 *   - write(master, ...) to send keyboard input
 *   - read(master, ...) to receive program output
 *
 * Shell/programs (child) use slave FD for:
 *   - ioctl(slave, TIOCSCTTY, ...) to make PTY the controlling terminal
 *   - read(slave, ...) to read keyboard input (this is their stdin)
 *   - write(slave, ...) to send output (this is their stdout/stderr)
 *
 * The kernel needs to know which end you're operating on.
 *
 * REASON #4: File Descriptor Inheritance and Redirection
 * The slave FD is DUPLICATED to become stdin/stdout/stderr in the child.
 *
 * CONCRETE EXAMPLE - What happens in spawn() function:
 *
 * Before fork():
 *   Terminal emulator has: master=5, slave=6
 *
 * After fork(), in CHILD process:
 *   1. close(master)           // Close FD 5, child doesn't need it
 *   2. dup2(slave, 0)          // Copy FD 6 to FD 0 (stdin)
 *   3. dup2(slave, 1)          // Copy FD 6 to FD 1 (stdout)
 *   4. dup2(slave, 2)          // Copy FD 6 to FD 2 (stderr)
 *   5. close(slave)            // Close FD 6, no longer needed
 *
 * Now child has: FD 0, 1, 2 all point to the SAME slave device
 *
 * When shell or vim does:
 *   read(0, buf, 10)           // Read from stdin (FD 0 = slave)
 *   write(1, "hello", 5)       // Write to stdout (FD 1 = slave)
 *   write(2, "error", 5)       // Write to stderr (FD 2 = slave)
 *
 * All three operations go through the SAME slave FD to the PTY driver.
 * The master FD is completely hidden from child (it was closed).
 *
 * WHY DUPLICATE TO 0, 1, 2?
 * Unix convention: programs expect stdin=0, stdout=1, stderr=2.
 * By duplicating slave to these FDs, programs work without modification.
 * They don't know they're talking to a PTY - they think it's a real terminal.
 *
 * REASON #5: Historical Compatibility
 * Real hardware terminals had two separate connections:
 * - Computer's serial port (like our master)
 * - Terminal's serial port (like our slave)
 * PTYs emulate this model. Programs written for real terminals expect this.
 *
 * WHAT IF WE USED ONE FD?
 * You could theoretically use a single bidirectional FD (like a socketpair),
 * but you'd lose:
 * - Automatic echo handling (you'd implement it yourself)
 * - Signal generation (you'd parse ^C and send SIGINT yourself)
 * - Line buffering (you'd implement canonical mode yourself)
 * - Terminal modes (raw/cooked, you'd handle all of it)
 * - Controlling terminal semantics (job control wouldn't work)
 *
 * The two-FD design lets the kernel's PTY driver do all this work for you.
 * Master is for the "outside" (GUI), slave is for the "inside" (programs).
 * The kernel mediates, providing terminal semantics automatically.
 *
 * KEY INSIGHT #4: What You Actually Have to Emulate
 * -------------------------------------------------
 * Despite the name "terminal emulator", the PTY driver does most terminal
 * behavior. You MUST implement:
 *
 * 1. Escape sequence parsing (ANSI/VT100):
 *    - \033[1m = bold text
 *    - \033[31m = red text
 *    - \033[2J = clear screen
 *    - \033[H = move cursor to home
 *    - Hundreds more...
 *
 * 2. Cursor movement and wrapping:
 *    - What happens at column 80 when you print character 81?
 *    - Does newline after auto-wrap create a blank line?
 *
 * 3. Special key encoding:
 *    - Arrow keys → \033[A (up), \033[B (down), etc.
 *    - Function keys → \033OP (F1), \033OQ (F2), etc.
 *    - No standard mapping exists; you choose what to send
 *
 * 4. Character encoding (UTF-8):
 *    - Multibyte sequences must be handled correctly
 *    - This example uses ASCII only for simplicity
 *
 * REAL-WORLD CONSIDERATIONS:
 * - Terminal databases (terminfo/termcap) describe capabilities
 * - Programs query TERM environment variable to know what you support
 * - Setting TERM=dumb means "no escape sequences supported"
 * - Setting TERM=xterm means "I understand XTerm escape sequences"
 * - Incomplete escape sequence parsing = garbled output
 * - Wrong key encodings = arrow keys don't work in vim
 *
 * PERFORMANCE IMPLICATIONS:
 * - Reading 1 byte at a time is inefficient but simple
 * - Production terminals buffer reads/writes
 * - X11 redraws are expensive; minimize them
 * - Double buffering prevents flicker
 *
 * GOTCHAS:
 * - Child process exit closes slave FD → master read() returns 0 or error
 * - Must close master in child, slave in parent (or deadlock)
 * - SIGCHLD handling needed to reap zombie processes
 * - Window resize requires TIOCSWINSZ ioctl + SIGWINCH to child
 */

#define SHELL "/bin/sh"

struct PTY
{
    int master;  /* File descriptor for emulator (parent process) */
    int slave;   /* File descriptor for shell (child process) */
};

struct X11
{
    int fd;              /* X11 connection file descriptor for select() */
    Display *dpy;        /* X11 display connection */
    int screen;          /* X11 screen number */
    Window root;         /* Root window */
    Window termwin;      /* Our terminal window */
    GC termgc;           /* Graphics context for drawing */
    
    unsigned long col_fg, col_bg;  /* Foreground/background colors */
    int w, h;                       /* Window dimensions in pixels */
    
    XFontStruct *xfont;             /* Font for rendering text */
    int font_width, font_height;    /* Character cell dimensions */
    
    char *buf;           /* Terminal buffer: buf_w × buf_h characters */
    int buf_w, buf_h;    /* Buffer dimensions in characters (e.g., 80×25) */
    int buf_x, buf_y;    /* Current cursor position in buffer */
};

/*
 * term_set_size: Inform the PTY driver of the terminal dimensions
 *
 * WHY THIS EXISTS:
 * Programs like vim, less, and top need to know the terminal size to format
 * output correctly. They query this via ioctl(TIOCGWINSZ). The kernel stores
 * this size in the PTY driver, and we set it here.
 *
 * WHEN TO CALL:
 * - After creating the PTY pair (before forking)
 * - After window resize (then send SIGWINCH to child)
 *
 * GOTCHA:
 * If a child process calls ioctl(TIOCSWINSZ), it changes the stored size,
 * but the kernel does NOT notify the terminal emulator. This is rarely done
 * in practice, so it's not a real problem.
 */
bool
term_set_size(struct PTY *pty, struct X11 *x11)
{
    struct winsize ws = {
        .ws_row = x11->buf_h,  /* Number of rows (e.g., 25) */
        .ws_col = x11->buf_w,  /* Number of columns (e.g., 80) */
        .ws_xpixel = 0,        /* Unused in practice */
        .ws_ypixel = 0,        /* Unused in practice */
    };

    if (ioctl(pty->master, TIOCSWINSZ, &ws) == -1)
    {
        perror("ioctl(TIOCSWINSZ)");
        return false;
    }

    return true;
}

/*
 * pt_pair: Create a pseudoterminal master/slave pair
 *
 * MENTAL MODEL:
 * A PTY pair is like creating a virtual serial cable with two ends:
 * - Master end: Your terminal emulator holds this
 * - Slave end: The shell/programs hold this
 * - Kernel PTY driver: Sits in the middle, processing data
 *
 * PHYSICAL ANALOGY:
 * Imagine an old computer connected to a VT100 terminal via RS-232 cable:
 *   [VT100 Terminal] <--RS-232 cable--> [Computer running Unix]
 *
 * With PTY, the kernel simulates this cable:
 *   [Your Emulator] <--master/slave--> [Shell/Programs]
 *        ↑                                      ↑
 *    Holds master FD                      Holds slave FD
 *
 * THE POSIX DANCE (5 steps):
 * 1. posix_openpt()  - Allocate a PTY pair, get master FD
 *                      Returns FD like 5, slave is /dev/pts/3 (not opened yet)
 * 2. grantpt()       - Change slave device ownership to your UID
 *                      Sets /dev/pts/3 permissions so you can open it
 * 3. unlockpt()      - Unlock the slave device
 *                      Allows open() to succeed (security mechanism)
 * 4. ptsname()       - Query the slave device path
 *                      Returns string like "/dev/pts/3"
 * 5. open()          - Open the slave device
 *                      Now you have both master and slave FDs
 *
 * WHY SO COMPLICATED?
 * Historical Unix security model. The steps ensure:
 * - Only the user who created the PTY can access it
 * - Race conditions are prevented (unlock after permission change)
 * - Multiple processes can't accidentally share the same PTY
 *
 * ALTERNATIVE APPROACH (BSD-style):
 * Use openpty() from <pty.h> (available on Linux, BSD, macOS):
 *     #include <pty.h>
 *     if (openpty(&pty->master, &pty->slave, NULL, NULL, NULL) == -1)
 *         return false;
 * This does all 5 steps in one call. Simpler but "not POSIX standard".
 * In practice, it's widely available and preferred by many (e.g., st terminal).
 *
 * AFTER THIS FUNCTION:
 * You have two FDs:
 * - pty->master (e.g., FD 5) - for parent process
 * - pty->slave (e.g., FD 6) - for child process
 * Both point to the same PTY pair, just different ends.
 *
 * O_NOCTTY FLAG:
 * "Don't make this my controlling terminal yet." We'll do that explicitly
 * in the child process with ioctl(TIOCSCTTY). Without this flag, opening
 * the PTY might automatically make it the controlling terminal, which we
 * don't want in the parent process.
 */
bool
pt_pair(struct PTY *pty)
{
    char *slave_name;

    /* STEP 1: Allocate a PTY pair and get the master FD
     * posix_openpt() asks kernel: "Give me a new PTY pair"
     * Kernel responds: "Here's master FD 5, slave is /dev/pts/3"
     * O_RDWR: Bidirectional (read/write)
     * O_NOCTTY: Don't make this the controlling terminal yet
     */
    pty->master = posix_openpt(O_RDWR | O_NOCTTY);
    if (pty->master == -1)
    {
        perror("posix_openpt");
        return false;
    }

    /* STEP 2: Fix ownership and permissions of slave device
     * grantpt() does: chown /dev/pts/3 to your UID, chmod to 0620
     * Security: Prevents other users from hijacking your PTY
     */
    if (grantpt(pty->master) == -1)
    {
        perror("grantpt");
        return false;
    }

    /* STEP 3: Unlock the slave device
     * Tells kernel: "I'm done setting up, allow open() now"
     * Prevents race conditions between grantpt() and open()
     */
    if (unlockpt(pty->master) == -1)
    {
        perror("unlockpt");
        return false;
    }

    /* STEP 4: Get the path to the slave device
     * ptsname() returns: "/dev/pts/3" (number assigned by kernel)
     * This is what appears in "w" command: pts/3
     */
    slave_name = ptsname(pty->master);
    if (slave_name == NULL)
    {
        perror("ptsname");
        return false;
    }

    /* STEP 5: Open the slave device
     * Now we have both ends:
     * - pty->master (FD 5) - parent uses this
     * - pty->slave (FD 6) - child uses this
     */
    pty->slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (pty->slave == -1)
    {
        perror("open(slave)");
        return false;
    }

    return true;
}

/*
 * x11_key: Handle keyboard input from X11
 *
 * MENTAL MODEL:
 * X11 gives us a KeyPress event with a keycode (hardware-specific) and
 * modifier state (Shift, Ctrl, Alt). We must convert this to bytes that
 * the shell understands.
 *
 * XLookupString() does the heavy lifting:
 * - Handles Shift (a → A)
 * - Handles basic Ctrl combinations (Ctrl+C → 0x03)
 * - Returns ASCII for printable characters
 *
 * LIMITATIONS:
 * - Doesn't handle arrow keys, function keys, etc.
 * - For those, you need to check ksym and generate escape sequences
 * - Example: XK_Up → write "\033[A" to master
 *
 * PRODUCTION APPROACH:
 * Check ksym for special keys:
 *     if (ksym == XK_Up)
 *         write(pty->master, "\033[A", 3);
 *     else if (ksym == XK_F1)
 *         write(pty->master, "\033OP", 3);
 *     else
 *         // Use XLookupString for normal keys
 */
void
x11_key(XKeyEvent *ev, struct PTY *pty)
{
    char buf[32];
    int i, num;
    KeySym ksym;

    /* XLookupString: Convert X11 KeyPress to ASCII
     * Input: ev (keycode + modifiers)
     * Output: buf (ASCII chars), ksym (symbolic name)
     * Examples:
     *   'a' key → buf="a", num=1
     *   Shift+'a' → buf="A", num=1
     *   Ctrl+'c' → buf="\003", num=1
     */
    num = XLookupString(ev, buf, sizeof buf, &ksym, 0);
    
    /* Write to master FD: Send keyboard input to child
     * Flow: write(master) → PTY driver → slave → child reads stdin
     * PTY driver intercepts special chars (^C → SIGINT)
     */
    for (i = 0; i < num; i++)
        write(pty->master, &buf[i], 1);
}

/*
 * x11_redraw: Render the terminal buffer to the X11 window
 *
 * SIMPLE APPROACH:
 * - Clear window with background color
 * - Draw each character from buffer
 * - Draw cursor as a filled rectangle
 *
 * PERFORMANCE ISSUES:
 * - Redraws entire window every time (inefficient)
 * - Production terminals use damage tracking
 * - XSync() forces immediate display (blocks until done)
 *
 * PRODUCTION IMPROVEMENTS:
 * - Double buffering (draw to pixmap, then copy to window)
 * - Dirty rectangle tracking (only redraw changed regions)
 * - Cursor blinking (timer-based)
 * - Expose event handling (only redraw exposed regions)
 */
void
x11_redraw(struct X11 *x11)
{
    int x, y;
    char buf[1];

    /* XSetForeground: Set drawing color in GC */
    XSetForeground(x11->dpy, x11->termgc, x11->col_bg);
    /* XFillRectangle: Draw filled rect (clears window)
     * Args: dpy, window, GC, x, y, width, height
     */
    XFillRectangle(x11->dpy, x11->termwin, x11->termgc, 0, 0, x11->w, x11->h);

    /* Draw each character from buffer */
    XSetForeground(x11->dpy, x11->termgc, x11->col_fg);
    for (y = 0; y < x11->buf_h; y++)
    {
        for (x = 0; x < x11->buf_w; x++)
        {
            buf[0] = x11->buf[y * x11->buf_w + x];
            
            /* Skip control chars (no glyphs) */
            if (!iscntrl(buf[0]))
            {
                /* XDrawString: Draw text at position
                 * Y is BASELINE, not top-left
                 * Add ascent to move down from cell top
                 */
                XDrawString(x11->dpy, x11->termwin, x11->termgc,
                            x * x11->font_width,
                            y * x11->font_height + x11->xfont->ascent,
                            buf, 1);
            }
        }
    }

    /* Draw cursor as filled rectangle */
    XSetForeground(x11->dpy, x11->termgc, x11->col_fg);
    XFillRectangle(x11->dpy, x11->termwin, x11->termgc,
                   x11->buf_x * x11->font_width,
                   x11->buf_y * x11->font_height,
                   x11->font_width, x11->font_height);

    /* XSync: Force immediate display (blocks until done) */
    XSync(x11->dpy, False);
}

/*
 * x11_setup: Initialize X11 display, window, and terminal buffer
 *
 * FIXED SIZE TERMINAL:
 * This example uses 80×25 characters (classic VT100 size). No resizing
 * is implemented. Production terminals handle ConfigureNotify events
 * to detect window resizes, then:
 * 1. Reallocate buffer
 * 2. Call ioctl(TIOCSWINSZ)
 * 3. Send SIGWINCH to child process
 *
 * FONT HANDLING:
 * Uses X11 core fonts (XLoadQueryFont). This is ancient but simple.
 * Modern terminals use Xft/fontconfig for TrueType fonts and better
 * Unicode support.
 *
 * BUFFER LAYOUT:
 * Linear array: buf[y * buf_w + x] = character at (x, y)
 * Initialized to all zeros (NUL characters, which we don't draw)
 */
bool
x11_setup(struct X11 *x11)
{
    Colormap cmap;
    XColor color;
    XSetWindowAttributes wa = {
        .background_pixmap = ParentRelative,
        .event_mask = KeyPressMask | KeyReleaseMask | ExposureMask,
    };

    /* XOpenDisplay: Connect to X11 server
     * NULL = use $DISPLAY (e.g., ":0")
     * Creates socket to /tmp/.X11-unix/X0
     */
    x11->dpy = XOpenDisplay(NULL);
    if (x11->dpy == NULL)
    {
        fprintf(stderr, "Cannot open display\n");
        return false;
    }

    /* DefaultScreen: Get screen number (usually 0) */
    x11->screen = DefaultScreen(x11->dpy);
    /* RootWindow: Get desktop background window */
    x11->root = RootWindow(x11->dpy, x11->screen);
    /* ConnectionNumber: Get FD for select() */
    x11->fd = ConnectionNumber(x11->dpy);

    /* XLoadQueryFont: Load font from X server
     * "fixed" = built-in monospace font
     * Returns metrics (width, height, ascent, descent)
     */
    x11->xfont = XLoadQueryFont(x11->dpy, "fixed");
    if (x11->xfont == NULL)
    {
        fprintf(stderr, "Could not load font\n");
        return false;
    }
    /* XTextWidth: Get pixel width of character */
    x11->font_width = XTextWidth(x11->xfont, "m", 1);
    /* Height = ascent (above baseline) + descent (below) */
    x11->font_height = x11->xfont->ascent + x11->xfont->descent;

    /* DefaultColormap: Get colormap for color allocation */
    cmap = DefaultColormap(x11->dpy, x11->screen);
    
    /* XAllocNamedColor: Convert "#000000" to pixel value
     * Pixel value is what we use in XSetForeground/XFillRectangle
     */
    if (!XAllocNamedColor(x11->dpy, cmap, "#000000", &color, &color))
    {
        fprintf(stderr, "Could not load bg color\n");
        return false;
    }
    x11->col_bg = color.pixel;

    if (!XAllocNamedColor(x11->dpy, cmap, "#aaaaaa", &color, &color))
    {
        fprintf(stderr, "Could not load fg color\n");
        return false;
    }
    x11->col_fg = color.pixel;

    /* Create terminal buffer: 80×25 characters
     * Layout: buf[y * buf_w + x] = character at (x, y)
     * calloc() initializes to 0 (NUL chars, not drawn)
     */
    x11->buf_w = 80;
    x11->buf_h = 25;
    x11->buf_x = 0;
    x11->buf_y = 0;
    x11->buf = calloc(x11->buf_w * x11->buf_h, 1);
    if (x11->buf == NULL)
    {
        perror("calloc");
        return false;
    }

    /* Calculate window size: 80 chars × font_width pixels/char */
    x11->w = x11->buf_w * x11->font_width;
    x11->h = x11->buf_h * x11->font_height;

    /* XCreateWindow: Create window
     * Args: dpy, parent, x, y, width, height, border,
     *       depth, class, visual, valuemask, attributes
     * Returns Window ID (integer handle)
     */
    x11->termwin = XCreateWindow(x11->dpy, x11->root,
                                 0, 0, x11->w, x11->h, 0,
                                 DefaultDepth(x11->dpy, x11->screen),
                                 CopyFromParent,
                                 DefaultVisual(x11->dpy, x11->screen),
                                 CWBackPixmap | CWEventMask, &wa);
    /* XStoreName: Set window title (in title bar) */
    XStoreName(x11->dpy, x11->termwin, "eduterm");
    /* XMapWindow: Make window visible */
    XMapWindow(x11->dpy, x11->termwin);
    
    /* XCreateGC: Create Graphics Context for drawing
     * GC holds state: colors, font, line width, etc.
     */
    x11->termgc = XCreateGC(x11->dpy, x11->termwin, 0, NULL);

    /* XSync: Flush requests and wait for completion */
    XSync(x11->dpy, False);
    return true;
}

/*
 * spawn: Fork a child process and connect it to the PTY slave
 *
 * THE CRITICAL SEQUENCE:
 * 1. fork() - Create child process
 * 2. setsid() - Create new session, become session leader
 * 3. ioctl(TIOCSCTTY) - Make PTY the controlling terminal
 * 4. dup2() - Redirect stdin/stdout/stderr to slave FD
 * 5. execle() - Replace process with shell
 *
 * WHY setsid()?
 * Creates a new session and process group. The calling process becomes
 * the session leader. This is required before TIOCSCTTY.
 *
 * WHY TIOCSCTTY?
 * Makes the PTY the controlling terminal for this session. This means:
 * - ^C sends SIGINT to foreground process group
 * - ^Z sends SIGTSTP to foreground process group
 * - Hangup sends SIGHUP to session leader
 *
 * WHY dup2()?
 * Redirects standard file descriptors to the slave:
 * - FD 0 (stdin) → slave (reads from terminal)
 * - FD 1 (stdout) → slave (writes to terminal)
 * - FD 2 (stderr) → slave (writes to terminal)
 *
 * WHY CLOSE MASTER IN CHILD?
 * The child doesn't need the master FD. Leaving it open can cause:
 * - File descriptor leaks
 * - Preventing proper EOF detection
 * - Security issues (child could write to master)
 *
 * ENVIRONMENT VARIABLES:
 * TERM=dumb means "no escape sequences". Use TERM=xterm or TERM=vt100
 * if you implement escape sequence parsing.
 *
 * SHELL INVOCATION:
 * "-" prefix makes it a login shell (reads ~/.profile, etc.)
 * Using SHELL macro allows easy customization
 */
bool
spawn(struct PTY *pty)
{
    pid_t p;
    char *env[] = { "TERM=dumb", NULL };

    p = fork();
    if (p == 0)  /* Child process */
    {
        /* Close master FD (child only needs slave) */
        close(pty->master);

        /* Create new session and become session leader */
        setsid();
        
        /* Make this PTY our controlling terminal */
        if (ioctl(pty->slave, TIOCSCTTY, NULL) == -1)
        {
            perror("ioctl(TIOCSCTTY)");
            return false;
        }

        /* Redirect stdin, stdout, stderr to slave */
        dup2(pty->slave, 0);
        dup2(pty->slave, 1);
        dup2(pty->slave, 2);
        
        /* Close original slave FD (now duplicated to 0, 1, 2) */
        close(pty->slave);

        /* Replace this process with a shell */
        execle(SHELL, "-" SHELL, (char *)NULL, env);
        
        /* If execle returns, it failed */
        perror("execle");
        return false;
    }
    else if (p > 0)  /* Parent process */
    {
        /* Close slave FD (parent only needs master) */
        close(pty->slave);
        return true;
    }

    /* fork() failed */
    perror("fork");
    return false;
}

/*
 * run: Main event loop
 *
 * ARCHITECTURE:
 * Use select() to wait for activity on two file descriptors:
 * 1. pty->master - Output from child process
 * 2. x11->fd - X11 events (keyboard, expose, etc.)
 *
 * WHY select()?
 * Blocks until at least one FD is ready, avoiding busy-waiting.
 * Alternative: poll() or epoll() (Linux-specific, more scalable)
 *
 * READING FROM PTY:
 * Read 1 byte at a time for simplicity. Production terminals read
 * larger chunks (e.g., 4096 bytes) for efficiency.
 *
 * CHARACTER PROCESSING:
 * - '\r' (carriage return) - Move cursor to column 0
 * - '\n' (line feed) - Move cursor down one line
 * - '\b' (backspace) - Move cursor left one column
 * - Printable characters - Store in buffer, advance cursor
 *
 * CURSOR WRAPPING:
 * When cursor reaches column 80, wrap to next line. The just_wrapped
 * flag handles the edge case: if we wrap at column 80 and immediately
 * receive '\n', should we create a blank line? This implementation
 * says no (just_wrapped prevents double line feed).
 *
 * SCROLLING:
 * When cursor reaches bottom of screen, scroll buffer up by one line.
 * This is done with memmove() to shift all lines up, then clear the
 * bottom line.
 *
 * MISSING FEATURES:
 * - No escape sequence parsing (colors, bold, cursor positioning)
 * - No UTF-8 support (assumes ASCII)
 * - No alternate screen buffer
 * - No scrollback history
 * - No mouse support
 *
 * ERROR HANDLING:
 * When read() returns 0 or -1, the child has exited or an error occurred.
 * This is normal when the shell exits (user types "exit" or ^D).
 */
int
run(struct PTY *pty, struct X11 *x11)
{
    int maxfd;
    fd_set readable;
    XEvent ev;
    char buf[1];
    bool just_wrapped = false;

    maxfd = pty->master > x11->fd ? pty->master : x11->fd;

    for (;;)
    {
        /* Set up file descriptor set for select() */
        FD_ZERO(&readable);
        FD_SET(pty->master, &readable);
        FD_SET(x11->fd, &readable);

        /* Wait for activity on either FD */
        if (select(maxfd + 1, &readable, NULL, NULL, NULL) == -1)
        {
            perror("select");
            return 1;
        }

        /* Check if child process has output */
        if (FD_ISSET(pty->master, &readable))
        {
            /* read(master): Get output from child
             * Flow: child writes stdout → slave → PTY driver → master → here
             * Returns: >0 (bytes read), 0 (EOF/child exited), -1 (error)
             */
            if (read(pty->master, buf, 1) <= 0)
            {
                /* Child exited (normal when user types "exit") */
                fprintf(stderr, "Nothing to read from child: ");
                perror(NULL);
                return 1;
            }

            /* Process the character */
            if (buf[0] == '\r')
            {
                /* Carriage return: move cursor to column 0 */
                x11->buf_x = 0;
            }
            else if (buf[0] == '\n')
            {
                /* Line feed: move cursor down one line */
                if (!just_wrapped)
                {
                    x11->buf_y++;
                    if (x11->buf_y >= x11->buf_h)
                    {
                        /* Scroll buffer up by one line */
                        memmove(x11->buf, x11->buf + x11->buf_w,
                                x11->buf_w * (x11->buf_h - 1));
                        memset(x11->buf + x11->buf_w * (x11->buf_h - 1),
                               0, x11->buf_w);
                        x11->buf_y = x11->buf_h - 1;
                    }
                }
                just_wrapped = false;
            }
            else if (buf[0] == '\b')
            {
                /* Backspace: move cursor left */
                if (x11->buf_x > 0)
                    x11->buf_x--;
            }
            else if (!iscntrl(buf[0]))
            {
                /* Printable character: store in buffer */
                x11->buf[x11->buf_y * x11->buf_w + x11->buf_x] = buf[0];
                x11->buf_x++;

                /* Handle cursor wrapping at right edge */
                if (x11->buf_x >= x11->buf_w)
                {
                    x11->buf_x = 0;
                    x11->buf_y++;
                    just_wrapped = true;

                    /* Scroll if necessary */
                    if (x11->buf_y >= x11->buf_h)
                    {
                        memmove(x11->buf, x11->buf + x11->buf_w,
                                x11->buf_w * (x11->buf_h - 1));
                        memset(x11->buf + x11->buf_w * (x11->buf_h - 1),
                               0, x11->buf_w);
                        x11->buf_y = x11->buf_h - 1;
                    }
                }
                else
                {
                    just_wrapped = false;
                }
            }

            /* Redraw the terminal */
            x11_redraw(x11);
        }

        /* Check for X11 events */
        if (FD_ISSET(x11->fd, &readable))
        {
            /* XPending: How many events in queue?
             * Loop to process all before returning to select()
             */
            while (XPending(x11->dpy))
            {
                /* XNextEvent: Get next event from queue */
                XNextEvent(x11->dpy, &ev);
                
                if (ev.type == KeyPress)
                {
                    /* User pressed key - send to PTY */
                    x11_key(&ev.xkey, pty);
                }
                else if (ev.type == Expose)
                {
                    /* Window uncovered - redraw */
                    x11_redraw(x11);
                }
            }
        }
    }

    return 0;
}

/*
 * main: Initialize everything and start the event loop
 *
 * INITIALIZATION ORDER:
 * 1. Create PTY pair (master/slave FDs)
 * 2. Set up X11 window and terminal buffer
 * 3. Set terminal size (inform PTY driver)
 * 4. Fork child process and exec shell
 * 5. Enter event loop
 *
 * CLEANUP:
 * This example doesn't clean up resources (X11 connection, allocated
 * memory, etc.) because the process exits immediately after the loop.
 * Production code should handle SIGCHLD to reap zombie processes and
 * clean up properly.
 */
int
main(void)
{
    struct PTY pty;
    struct X11 x11;

    if (!pt_pair(&pty))
        return 1;

    if (!x11_setup(&x11))
        return 1;

    if (!term_set_size(&pty, &x11))
        return 1;

    if (!spawn(&pty))
        return 1;

    return run(&pty, &x11);
}
