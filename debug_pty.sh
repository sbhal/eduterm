#!/bin/bash
# PTY File Descriptor Inspector
# Run this script while eduterm is running to inspect PTY setup

set -e

echo "=========================================="
echo "PTY File Descriptor Inspector"
echo "=========================================="
echo ""

# Find eduterm process
PARENT_PID=$(pgrep eduterm 2>/dev/null || true)

if [ -z "$PARENT_PID" ]; then
    echo "ERROR: eduterm is not running!"
    echo "Please start eduterm first: ./eduterm"
    exit 1
fi

# Find child shell process
CHILD_PID=$(pgrep -P $PARENT_PID 2>/dev/null || true)

if [ -z "$CHILD_PID" ]; then
    echo "ERROR: Child process not found!"
    echo "Parent PID: $PARENT_PID"
    exit 1
fi

echo "✓ Found processes:"
echo "  Parent (eduterm): PID $PARENT_PID"
echo "  Child (shell):    PID $CHILD_PID"
echo ""

# Step 1: Parent's file descriptors
echo "=========================================="
echo "STEP 1: Parent's File Descriptors"
echo "=========================================="
echo "Looking for master FD (should point to /dev/ptmx)..."
echo ""
ls -l /proc/$PARENT_PID/fd/ 2>/dev/null | while read line; do
    if echo "$line" | grep -q "ptmx"; then
        echo "  ✓ MASTER FD: $line"
    elif echo "$line" | grep -q "socket"; then
        echo "    X11 conn: $line"
    fi
done
echo ""

# Step 2: Child's file descriptors
echo "=========================================="
echo "STEP 2: Child's File Descriptors"
echo "=========================================="
echo "Looking for slave FD (FDs 0,1,2 should point to same /dev/pts/N)..."
echo ""
ls -l /proc/$CHILD_PID/fd/ 2>/dev/null | grep -E "^l.*[012] ->" | while read line; do
    echo "  ✓ $line"
done
echo ""

# Verify all three point to same device
STDIN=$(readlink /proc/$CHILD_PID/fd/0 2>/dev/null)
STDOUT=$(readlink /proc/$CHILD_PID/fd/1 2>/dev/null)
STDERR=$(readlink /proc/$CHILD_PID/fd/2 2>/dev/null)

if [ "$STDIN" = "$STDOUT" ] && [ "$STDOUT" = "$STDERR" ]; then
    echo "  ✓ VERIFIED: stdin, stdout, stderr all point to: $STDIN"
else
    echo "  ✗ ERROR: stdin/stdout/stderr point to different devices!"
    echo "    stdin:  $STDIN"
    echo "    stdout: $STDOUT"
    echo "    stderr: $STDERR"
fi
echo ""

# Step 3: Process tree and controlling terminal
echo "=========================================="
echo "STEP 3: Process Tree & Controlling Terminal"
echo "=========================================="
ps -o pid,ppid,pgid,sid,tty,comm -p $PARENT_PID,$CHILD_PID 2>/dev/null
echo ""

# Verify session leader
CHILD_SID=$(ps -o sid= -p $CHILD_PID 2>/dev/null | tr -d ' ')
if [ "$CHILD_PID" = "$CHILD_SID" ]; then
    echo "  ✓ Child is session leader (PID == SID)"
else
    echo "  ✗ Child is NOT session leader (PID=$CHILD_PID, SID=$CHILD_SID)"
fi
echo ""

# Step 4: lsof output
echo "=========================================="
echo "STEP 4: Open PTY Devices (lsof)"
echo "=========================================="
echo "Showing which processes have PTY devices open..."
echo ""
lsof 2>/dev/null | grep -E "eduterm|$CHILD_PID" | grep -E "ptmx|pts" || echo "  (no lsof output)"
echo ""

# Step 5: Summary
echo "=========================================="
echo "SUMMARY"
echo "=========================================="
echo ""
echo "Expected Setup:"
echo "  ✓ Parent has master FD pointing to /dev/ptmx"
echo "  ✓ Child has FDs 0,1,2 all pointing to same /dev/pts/N"
echo "  ✓ Child's controlling terminal (TTY) is pts/N"
echo "  ✓ Child is session leader (PID == SID)"
echo ""

# Extract PTY number
PTY_NUM=$(echo "$STDIN" | grep -oP 'pts/\K\d+' || echo "?")
echo "PTY Number: $PTY_NUM"
echo "  Master: /dev/ptmx (held by parent PID $PARENT_PID)"
echo "  Slave:  /dev/pts/$PTY_NUM (held by child PID $CHILD_PID as FDs 0,1,2)"
echo ""

echo "=========================================="
echo "INTERACTIVE TEST"
echo "=========================================="
echo ""
echo "You can test the PTY connection by writing to the slave device:"
echo "  echo 'Hello from outside!' > /dev/pts/$PTY_NUM"
echo ""
echo "This should appear in the eduterm window!"
echo ""
