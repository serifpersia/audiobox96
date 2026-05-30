#!/bin/sh
# Helper script to compile and run the AudioBox 96 monitor tool.

MONITOR_SRC="audiobox96-mon.c"
MONITOR_BIN="audiobox96-mon"

# Ensure the source file exists in the directory
if [ ! -f "$MONITOR_SRC" ]; then
    echo "Error: $MONITOR_SRC not found in the current directory."
    exit 1
fi

# Check if gcc is available
if ! command -v gcc >/dev/null 2>&1; then
    echo "Error: gcc is not installed. Please install a compiler to continue."
    exit 1
fi

echo "Compiling $MONITOR_SRC..."
gcc -O2 -Wall "$MONITOR_SRC" -o "$MONITOR_BIN"

if [ $? -ne 0 ]; then
    echo "Error: Compilation failed."
    exit 1
fi

echo "Compilation successful. Launching monitor..."
# Run the binary. If you need to clean up the binary on exit, 
# uncomment the trap line below.
# trap 'rm -f "$MONITOR_BIN"' EXIT INT TERM

./"$MONITOR_BIN"
