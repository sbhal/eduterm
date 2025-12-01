# Terminal Emulator Mental Models for Expert Linux Engineers

## Table of Contents
1. [The Three-Layer Architecture](#the-three-layer-architecture)
2. [PTY Driver: The Smart Pipe](#pty-driver-the-smart-pipe)
3. [Sessions, Process Groups, and Job Control](#sessions-process-groups-and-job-control)
4. [Terminal Modes: Cooked vs Raw](#terminal-modes-cooked-vs-raw)
5. [The FD Dance: Master/Slave Lifecycle](#the-fd-dance-masterslave-lifecycle)
6. [Signal Flow and Controlling Terminal](#signal-flow-and-controlling-terminal)
7. [Escape Sequences: The Protocol Layer](#escape-sequences-the-protocol-layer)
8. [Comparison: PTY vs Pipe vs Socket](#comparison-pty-vs-pipe-vs-socket)

---

## The Three-Layer Architecture

### Mental Model: Network Stack Analogy

Think of a terminal emulator like a network stack:

```
┌─────────────────────────────────────────────────────────────┐
│ Application Layer: X11/GUI (presentation)                   │
│ - Renders glyphs, handles mouse, manages window             │
│ - Parses ANSI/VT100 escape sequences                        │
│ - Converts KeyPress events to byte sequences                │
└─────────────────────────────────────────────────────────────┘
                            ↕ (master FD)
┌─────────────────────────────────────────────────────────────┐
│ Transport Layer: PTY Driver (kernel)                        │
│ - Line discipline (canonical/raw mode)                      │
│ - Signal generation (^C → SIGINT)                           │
│ - Echo handling, line editing                               │
│ - Flow control (^S/^Q)                                      │
└─────────────────────────────────────────────────────────────┘
                            ↕ (slave FD)
┌─────────────────────────────────────────────────────────────┐
│ Application Layer: Shell/Programs                           │
│ - Read from stdin (FD 0)                                    │
│ - Write to stdout/stderr (FD 1/2)                           │
│ - Unaware they're talking to a PTY                          │
└─────────────────────────────────────────────────────────────┘
```

**Key Insight:** The PTY driver is like TCP - it provides reliable, ordered, bidirectional communication with additional semantics (signals, echo, line editing).

### Data Flow: Keyboard Input

```
User presses 'a'
    ↓
X11 KeyPress event (keycode=38, state=0)
    ↓
XLookupString() → 'a' (0x61)
    ↓
write(master, "a", 1)
    ↓
┌─────────────────────────────────────────┐
│ PTY Driver (kernel)                     │
│ - Check if echo enabled → yes           │
│ - Write 'a' back to master (echo)       │
│ - Check if canonical mode → yes         │
│ - Buffer 'a' in line buffer             │
│ - Wait for '\n' before making available │
└─────────────────────────────────────────┘
    ↓ (after '\n' received)
read(slave/stdin, buf, ...) → "a\n"
    ↓
Shell processes command
```

### Data Flow: Program Output

```
Program: write(1, "hello\n", 6)
    ↓
FD 1 → /dev/pts/5 (slave)
    ↓
┌─────────────────────────────────────────┐
│ PTY Driver (kernel)                     │
│ - Check output processing (OPOST)       │
│ - Convert '\n' to '\r\n' if ONLCR set   │
│ - Make data available on master         │
└─────────────────────────────────────────┘
    ↓
read(master, buf, ...) → "hello\r\n"
    ↓
Terminal parses bytes, renders to screen
```

---

## PTY Driver: The Smart Pipe

### Mental Model: Middleware with State

The PTY driver is **stateful middleware** between terminal and programs:

```c
struct pty_state {
    struct termios settings;     // Terminal modes
    char line_buffer[4096];      // Canonical mode buffer
    int foreground_pgid;         // For signal delivery
    struct winsize size;         // Terminal dimensions
    bool packet_mode;            // For master to detect slave state changes
};
```

### What PTY Driver Does (That Pipes Don't)

| Feature | Pipe | PTY | Why PTY Needs It |
|---------|------|-----|------------------|
| Bidirectional | No (need 2 pipes) | Yes | Single communication channel |
| Echo | No | Yes | User sees what they type |
| Line editing | No | Yes | Backspace, ^U work before Enter |
| Signal generation | No | Yes | ^C sends SIGINT to foreground group |
| Terminal modes | No | Yes | Raw mode for vim, cooked for shell |
| Window size | No | Yes | Programs need to know terminal size |
| Controlling terminal | No | Yes | Job control, session management |

### Canonical Mode (Cooked) vs Raw Mode

**Canonical Mode (default):**
```
User types: h e l l o Backspace Backspace p Enter
                                ↓
PTY buffers: h e l l o → h e l l → h e l → h e l p
                                ↓
Program reads: "help\n" (only after Enter)
```

**Raw Mode (vim, less, etc.):**
```
User types: h e l l o Backspace Backspace p Enter
                                ↓
Program reads: 'h' 'e' 'l' 'l' 'o' '\b' '\b' 'p' '\n'
(each keystroke delivered immediately)
```

**How programs switch modes:**
```c
struct termios tio;
tcgetattr(0, &tio);

// Enter raw mode
tio.c_lflag &= ~(ICANON | ECHO);  // Disable canonical, echo
tcsetattr(0, TCSANOW, &tio);

// ... do raw I/O ...

// Restore canonical mode
tio.c_lflag |= (ICANON | ECHO);
tcsetattr(0, TCSANOW, &tio);
```

---

## Sessions, Process Groups, and Job Control

### Mental Model: Hierarchical Process Organization

```
System
├─ Session 1 (SID=1000, TTY=tty1)
│  ├─ Process Group 1000 (PGID=1000) [Foreground]
│  │  └─ bash (PID=1000, session leader)
│  ├─ Process Group 1050 (PGID=1050) [Background]
│  │  └─ vim (PID=1050, suspended with ^Z)
│  └─ Process Group 1100 (PGID=1100) [Foreground]
│     ├─ ls (PID=1100, group leader)
│     └─ grep (PID=1101)
│
└─ Session 2 (SID=2000, TTY=pts/5)
   ├─ Process Group 2000 (PGID=2000) [Foreground]
   │  └─ bash (PID=2000, session leader)
   └─ Process Group 2050 (PGID=2050) [Background]
      └─ find (PID=2050)
```

### The Hierarchy

```
Session (login session)
  ├─ Has one controlling terminal (or none)
  ├─ Has one session leader (first process)
  └─ Contains multiple process groups
      ├─ One foreground group (receives keyboard signals)
      └─ Multiple background groups
```

### Signal Delivery Rules

**User presses ^C:**
```
1. Terminal emulator writes 0x03 to master FD
2. PTY driver intercepts 0x03
3. PTY driver looks up foreground PGID for this session
4. PTY driver sends SIGINT to all processes in foreground group
5. Foreground processes receive SIGINT (default: terminate)
```

**Key Point:** Background processes do NOT receive ^C signals!

```bash
# Foreground
$ sleep 100
^C  # ← SIGINT delivered, sleep terminates

# Background
$ sleep 100 &
^C  # ← SIGINT NOT delivered, sleep continues
```

### Session Leader Responsibilities

The session leader (usually shell) manages:
- Which process group is foreground (`tcsetpgrp()`)
- Job control (fg, bg, jobs commands)
- Cleanup on exit (send SIGHUP to all processes)

```c
// Shell puts job in foreground
tcsetpgrp(0, job_pgid);  // 0 = stdin = controlling terminal

// Shell puts itself back in foreground
tcsetpgrp(0, getpgrp());
```

### Why setsid() is Critical

```c
// Without setsid():
fork();
// Child inherits parent's session, process group, controlling terminal
// Child CANNOT become session leader
// Child CANNOT set controlling terminal

// With setsid():
fork();
if (child) {
    setsid();  // Creates new session, child becomes leader
    ioctl(slave, TIOCSCTTY, NULL);  // Now can set controlling terminal
}
```

---

## Terminal Modes: Cooked vs Raw

### The termios Structure

```c
struct termios {
    tcflag_t c_iflag;   // Input modes
    tcflag_t c_oflag;   // Output modes
    tcflag_t c_cflag;   // Control modes
    tcflag_t c_lflag;   // Local modes
    cc_t c_cc[NCCS];    // Control characters
};
```

### Common Mode Combinations

**1. Canonical Mode (Shell):**
```c
c_lflag: ICANON | ECHO | ECHOE | ISIG
// ICANON: Line buffering, edit with backspace
// ECHO: Echo input characters
// ECHOE: Erase character echoes as BS-SP-BS
// ISIG: Generate signals for ^C, ^Z
```

**2. Raw Mode (vim, less):**
```c
c_lflag: 0  // All flags off
c_iflag: 0  // No input processing
c_oflag: 0  // No output processing
// Every byte delivered immediately, no processing
```

**3. Password Input:**
```c
c_lflag: ICANON | ISIG  // Line buffering, signals
c_lflag &= ~ECHO        // But no echo!
// User types password, nothing displayed
```

**4. cbreak Mode (less common):**
```c
c_lflag: ISIG  // Signals enabled
c_lflag &= ~(ICANON | ECHO)  // No line buffering, no echo
// Like raw but signals still work
```

### Output Processing (OPOST)

```c
// With OPOST | ONLCR:
write(1, "hello\n", 6);
// PTY converts: "hello\n" → "hello\r\n"
// Terminal receives: "hello\r\n"

// Without OPOST:
write(1, "hello\n", 6);
// PTY passes through: "hello\n"
// Terminal receives: "hello\n"
// Result: Cursor moves down but not to column 0!
```

---

## The FD Dance: Master/Slave Lifecycle

### Complete Lifecycle

```c
// PARENT PROCESS (Terminal Emulator)
// ==================================

// 1. Create PTY pair
int master = posix_openpt(O_RDWR | O_NOCTTY);
grantpt(master);
unlockpt(master);
char *slave_name = ptsname(master);  // "/dev/pts/5"
int slave = open(slave_name, O_RDWR | O_NOCTTY);

// 2. Set terminal size
struct winsize ws = {25, 80, 0, 0};
ioctl(master, TIOCSWINSZ, &ws);

// 3. Fork
pid_t pid = fork();

if (pid == 0) {
    // CHILD PROCESS (Shell)
    // =====================
    
    // 4. Close master (child doesn't need it)
    close(master);
    
    // 5. Create new session
    setsid();  // PID becomes SID
    
    // 6. Make PTY the controlling terminal
    ioctl(slave, TIOCSCTTY, NULL);
    
    // 7. Redirect stdin/stdout/stderr
    dup2(slave, 0);  // stdin
    dup2(slave, 1);  // stdout
    dup2(slave, 2);  // stderr
    close(slave);    // Close original FD
    
    // 8. Exec shell
    execle("/bin/sh", "-sh", NULL, env);
    
} else {
    // PARENT PROCESS (continued)
    // ==========================
    
    // 9. Close slave (parent doesn't need it)
    close(slave);
    
    // 10. Event loop
    for (;;) {
        // Read from master → display
        // Read from X11 → write to master
    }
}
```

### FD Table Evolution

**Before fork():**
```
Parent Process (PID 1000)
FD Table:
  0 → /dev/pts/0 (original terminal)
  1 → /dev/pts/0
  2 → /dev/pts/0
  3 → socket (X11)
  5 → /dev/ptmx (master)
  6 → /dev/pts/5 (slave)
```

**After fork(), in child:**
```
Child Process (PID 1001)
FD Table (inherited):
  0 → /dev/pts/0
  1 → /dev/pts/0
  2 → /dev/pts/0
  3 → socket (X11)
  5 → /dev/ptmx (master)  ← close this
  6 → /dev/pts/5 (slave)
```

**After close(master) in child:**
```
Child Process (PID 1001)
FD Table:
  0 → /dev/pts/0
  1 → /dev/pts/0
  2 → /dev/pts/0
  3 → socket (X11)
  6 → /dev/pts/5 (slave)
```

**After dup2(slave, 0/1/2) in child:**
```
Child Process (PID 1001)
FD Table:
  0 → /dev/pts/5 (slave)  ← redirected
  1 → /dev/pts/5 (slave)  ← redirected
  2 → /dev/pts/5 (slave)  ← redirected
  3 → socket (X11)
  6 → /dev/pts/5 (slave)
```

**After close(slave) in child:**
```
Child Process (PID 1001)
FD Table:
  0 → /dev/pts/5 (slave)
  1 → /dev/pts/5 (slave)
  2 → /dev/pts/5 (slave)
  3 → socket (X11)
```

**After close(slave) in parent:**
```
Parent Process (PID 1000)
FD Table:
  0 → /dev/pts/0
  1 → /dev/pts/0
  2 → /dev/pts/0
  3 → socket (X11)
  5 → /dev/ptmx (master)
```

---

## Signal Flow and Controlling Terminal

### The Signal Path

```
User presses ^C in terminal
         ↓
X11 KeyPress event
         ↓
Terminal writes 0x03 to master FD
         ↓
┌──────────────────────────────────────────┐
│ PTY Driver (kernel)                      │
│                                          │
│ 1. Recognize 0x03 as VINTR character    │
│ 2. Look up controlling terminal's       │
│    foreground process group ID          │
│ 3. Send SIGINT to all processes in      │
│    that process group                   │
└──────────────────────────────────────────┘
         ↓
kill(-foreground_pgid, SIGINT)
         ↓
All processes in foreground group receive SIGINT
```

### Controlling Terminal Semantics

**A process has a controlling terminal if:**
1. It's in a session that has a controlling terminal
2. The session leader called `ioctl(TIOCSCTTY)` on a PTY slave

**What controlling terminal provides:**
- Keyboard signals (^C, ^Z, ^\, ^Q, ^S)
- Hangup signal (SIGHUP) when terminal disconnects
- Job control (foreground/background)

**Checking controlling terminal:**
```c
int ctty = open("/dev/tty", O_RDWR);
if (ctty == -1) {
    // No controlling terminal
} else {
    // Has controlling terminal
    // /dev/tty is a magic symlink to your controlling terminal
}
```

### Signal Character Mapping

```c
// Default control characters (c_cc array):
c_cc[VINTR]  = 0x03;  // ^C → SIGINT
c_cc[VQUIT]  = 0x1C;  // ^\ → SIGQUIT
c_cc[VSUSP]  = 0x1A;  // ^Z → SIGTSTP
c_cc[VSTART] = 0x11;  // ^Q → Resume output
c_cc[VSTOP]  = 0x13;  // ^S → Stop output
c_cc[VEOF]   = 0x04;  // ^D → EOF
```

**Customizing:**
```c
struct termios tio;
tcgetattr(0, &tio);
tio.c_cc[VINTR] = 0x18;  // Change ^C to ^X
tcsetattr(0, TCSANOW, &tio);
```

---

## Escape Sequences: The Protocol Layer

### Mental Model: In-Band Signaling

Escape sequences are **in-band control messages** embedded in the data stream:

```
Regular data: "Hello, world!"
Control data: "\033[1mHello\033[0m, world!"
              ^^^^^^^^      ^^^^^^^^
              Start bold    Reset
```

### Common Escape Sequence Categories

**1. Cursor Movement:**
```
\033[H        Move to home (0,0)
\033[5;10H    Move to row 5, column 10
\033[A        Move up 1 line
\033[B        Move down 1 line
\033[C        Move right 1 column
\033[D        Move left 1 column
```

**2. Text Attributes:**
```
\033[0m       Reset all attributes
\033[1m       Bold
\033[4m       Underline
\033[7m       Reverse video
\033[30-37m   Foreground colors
\033[40-47m   Background colors
```

**3. Screen Manipulation:**
```
\033[2J       Clear entire screen
\033[K        Clear to end of line
\033[L        Insert line
\033[M        Delete line
```

**4. Terminal Queries:**
```
\033[6n       Query cursor position
              Terminal responds: \033[row;colR
\033[c        Query terminal type
              Terminal responds: \033[?1;2c
```

### Parsing State Machine

```c
enum parse_state {
    STATE_NORMAL,      // Regular characters
    STATE_ESC,         // Saw '\033'
    STATE_CSI,         // Saw '\033['
    STATE_PARAMS,      // Reading parameters
};

void parse_byte(char c) {
    switch (state) {
    case STATE_NORMAL:
        if (c == '\033') state = STATE_ESC;
        else display_char(c);
        break;
    case STATE_ESC:
        if (c == '[') state = STATE_CSI;
        else state = STATE_NORMAL;
        break;
    case STATE_CSI:
        if (isdigit(c) || c == ';') {
            accumulate_param(c);
            state = STATE_PARAMS;
        } else {
            execute_csi(c);
            state = STATE_NORMAL;
        }
        break;
    case STATE_PARAMS:
        if (isdigit(c) || c == ';') {
            accumulate_param(c);
        } else {
            execute_csi(c);
            state = STATE_NORMAL;
        }
        break;
    }
}
```

---

## Comparison: PTY vs Pipe vs Socket

| Feature | Pipe | Socket | PTY |
|---------|------|--------|-----|
| **Bidirectional** | No (need 2) | Yes | Yes |
| **Network capable** | No | Yes | No |
| **Line discipline** | No | No | Yes |
| **Signal generation** | No | No | Yes |
| **Echo** | No | No | Yes |
| **Terminal modes** | No | No | Yes |
| **Controlling terminal** | No | No | Yes |
| **Window size** | No | No | Yes |
| **Use case** | IPC | Network | Terminal emulation |

### When to Use Each

**Pipe:**
```c
// Simple parent-child communication
int pipefd[2];
pipe(pipefd);
fork();
// Parent writes to pipefd[1], child reads from pipefd[0]
```

**Socket:**
```c
// Network communication or complex IPC
int sv[2];
socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
// Bidirectional, can pass file descriptors
```

**PTY:**
```c
// Terminal emulation, running interactive programs
int master = posix_openpt(...);
// Full terminal semantics, job control, signals
```

---

## Advanced Topics

### Packet Mode

```c
// Enable packet mode on master
int on = 1;
ioctl(master, TIOCPKT, &on);

// Now read() returns packet header + data
char buf[4096];
ssize_t n = read(master, buf, sizeof(buf));
if (n > 0 && (buf[0] & TIOCPKT_FLUSHWRITE)) {
    // Slave's output buffer was flushed
    // Terminal should discard pending output
}
```

**Use case:** Detecting when slave changes terminal modes (e.g., vim enters raw mode).

### Zero-Copy with splice()

```c
// Efficient data transfer (Linux-specific)
splice(master, NULL, stdout_fd, NULL, 4096, SPLICE_F_MOVE);
// Avoids copying data through userspace
```

### Multiple Sessions on One PTY

**Not possible!** Each PTY pair supports exactly one session. For multiple sessions, create multiple PTY pairs.

```c
// Wrong:
fork(); fork();  // Two children
// Both try to call setsid() + TIOCSCTTY on same slave
// Second one fails: EPERM

// Right:
for (int i = 0; i < 2; i++) {
    int master = posix_openpt(...);  // New PTY for each session
    fork();
    // Each child gets its own PTY
}
```

---

## Summary: The Complete Mental Model

```
┌─────────────────────────────────────────────────────────────┐
│ Terminal Emulator (GUI Process)                             │
│ - Manages X11 window, renders glyphs                        │
│ - Parses escape sequences                                   │
│ - Holds master FD                                           │
└────────────────────┬────────────────────────────────────────┘
                     │ write(master, "ls\n", 3)
                     │ read(master, buf, 4096)
                     ↓
┌─────────────────────────────────────────────────────────────┐
│ PTY Driver (Kernel)                                         │
│ - Line discipline (canonical/raw)                           │
│ - Echo, line editing                                        │
│ - Signal generation (^C → SIGINT)                           │
│ - Terminal modes (termios)                                  │
│ - Window size tracking                                      │
│ - Foreground process group tracking                         │
└────────────────────┬────────────────────────────────────────┘
                     │ read(0, buf, 4096)  // stdin
                     │ write(1, "output", 6)  // stdout
                     │ write(2, "error", 5)   // stderr
                     ↓
┌─────────────────────────────────────────────────────────────┐
│ Session (SID = shell PID)                                   │
│ ├─ Shell (session leader, holds slave as FD 0/1/2)         │
│ ├─ Process Group 1 (foreground): ls | grep                 │
│ └─ Process Group 2 (background): vim (suspended)           │
└─────────────────────────────────────────────────────────────┘
```

**Key Takeaways:**
1. PTY driver is smart middleware, not a dumb pipe
2. Master/slave separation enables kernel to provide terminal semantics
3. Sessions and process groups enable job control
4. Terminal modes control how data is processed
5. Escape sequences are in-band control protocol
6. Everything is designed to emulate 1970s hardware terminals
