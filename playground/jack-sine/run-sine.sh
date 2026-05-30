#!/bin/sh
# Helper script to compile and run the JACK sine tester.

SINE_SRC="jack-sine.c"
SINE_BIN="jack-sine"

# 1. Check for the source file
if [ ! -f "$SINE_SRC" ]; then
    echo "Error: $SINE_SRC not found."
    exit 1
fi

# 2. Check for pkg-config and jack libraries
if ! command -v pkg-config >/dev/null 2>&1; then
    echo "Error: pkg-config is required to find JACK library headers."
    exit 1
fi

if ! pkg-config --exists jack; then
    echo "Error: JACK development headers not found. Please install jack2 development packages."
    echo "On Arch Linux: sudo pacman -S jack2"
    echo "On Debian/Ubuntu: sudo apt install libjack-jackd2-dev"
    exit 1
fi

# 3. Compile the file
echo "Compiling $SINE_SRC with JACK library..."
gcc -O2 -Wall "$SINE_SRC" -o "$SINE_BIN" $(pkg-config --cflags --libs jack) -lm

if [ $? -ne 0 ]; then
    echo "Error: Compilation failed."
    exit 1
fi

echo "Compilation successful."
echo "--------------------------------------------------------"
echo "Usage: ./$SINE_BIN [Frequency in Hz] [Amplitude 0.0-1.0]"
echo "Example: ./$SINE_BIN 440 0.5"
echo "--------------------------------------------------------"
echo "Launching tone generator..."
echo ""

./"$SINE_BIN" 440 0.5
