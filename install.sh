#!/bin/bash

LOG_FILE=$(mktemp)
MODULE_NAME="snd_usb_audiobox96"
KO_FILE="snd-usb-audiobox96.ko"
TARGET_DIR="/lib/modules/$(uname -r)/extra/audiobox96"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
GRAY='\033[0;90m'
NC='\033[0m'
BOLD='\033[1m'

DETECTED_AUDIO=""
PIPEWIRE_PAUSED=false
PULSE_PAUSED=false
JACK_PAUSED=false

REAL_USER="${SUDO_USER:-$USER}"
REAL_UID=$(id -u "$REAL_USER")

cleanup() {
    tput cnorm
    rm -f "$LOG_FILE"
}
trap cleanup EXIT

header() {
    clear
    echo -e "${GREEN}${BOLD}"
    echo "  PRESONUS AUDIOBOX USB 96 DRIVER INSTALLER"
    echo "  ========================================="
    echo -e "${NC}"
}

init_sudo() {
    if [ "$EUID" -ne 0 ]; then
        echo -e "${GRAY}  Creating sudo session...${NC}"
        if ! sudo -v; then
            echo -e "\n  ${RED}✖ Authentication failed.${NC}"
            exit 1
        fi
    fi
}

user_systemctl() {
    sudo -u "$REAL_USER" XDG_RUNTIME_DIR="/run/user/$REAL_UID" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$REAL_UID/bus" systemctl --user "$@"
}

detect_audio_system() {
    if pgrep -x "pipewire" >/dev/null || user_systemctl is-active pipewire >/dev/null 2>&1; then
        DETECTED_AUDIO="PipeWire"
    elif pgrep -x "pulseaudio" >/dev/null || user_systemctl is-active pulseaudio >/dev/null 2>&1; then
        DETECTED_AUDIO="PulseAudio"
    fi

    if pgrep -x "jackd" >/dev/null || pgrep -x "jackdbus" >/dev/null; then
        if [ -n "$DETECTED_AUDIO" ]; then
            DETECTED_AUDIO="$DETECTED_AUDIO + JACK"
        else
            DETECTED_AUDIO="JACK"
        fi
    fi

    if [ -z "$DETECTED_AUDIO" ]; then
        DETECTED_AUDIO="None / Unknown ALSA"
    fi
}

pause_audio_system() {
    if [[ "$DETECTED_AUDIO" == *"PipeWire"* ]]; then
        if user_systemctl is-active pipewire >/dev/null 2>&1; then
            user_systemctl stop wireplumber.service pipewire-pulse.socket pipewire-pulse.service pipewire.socket pipewire.service >/dev/null 2>&1
            PIPEWIRE_PAUSED=true
        fi
    fi

    if [[ "$DETECTED_AUDIO" == *"PulseAudio"* ]]; then
        if user_systemctl is-active pulseaudio >/dev/null 2>&1; then
            user_systemctl stop pulseaudio.socket pulseaudio.service >/dev/null 2>&1
            PULSE_PAUSED=true
        fi
    fi

    if [[ "$DETECTED_AUDIO" == *"JACK"* ]]; then
        killall -9 jackd jackdbus >/dev/null 2>&1
        JACK_PAUSED=true
    fi
}

resume_audio_system() {
    if [ "$PIPEWIRE_PAUSED" = true ] || [[ "$DETECTED_AUDIO" == *"PipeWire"* ]]; then
        user_systemctl unmask pipewire.socket pipewire.service pipewire-pulse.socket pipewire-pulse.service wireplumber.service >/dev/null 2>&1
        user_systemctl start pipewire.socket pipewire.service pipewire-pulse.socket pipewire-pulse.service wireplumber.service >/dev/null 2>&1
    fi

    if [ "$PULSE_PAUSED" = true ] || [[ "$DETECTED_AUDIO" == *"PulseAudio"* ]]; then
        user_systemctl unmask pulseaudio.socket pulseaudio.service >/dev/null 2>&1
        user_systemctl start pulseaudio.socket pulseaudio.service >/dev/null 2>&1
    fi

    if [ "$JACK_PAUSED" = true ] && [[ "$DETECTED_AUDIO" != *"PipeWire"* ]]; then
        if command -v jack_control >/dev/null 2>&1; then
            sudo -u "$REAL_USER" jack_control start >/dev/null 2>&1
        fi
    fi
}

run() {
    local cmd="$1"
    local msg="$2"
    local allow_fail="${3:-false}"

    tput civis
    eval "$cmd" > "$LOG_FILE" 2>&1 &
    local pid=$!

    local spin='⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏'
    local i=0

    while kill -0 "$pid" 2>/dev/null; do
        i=$(((i + 1) % ${#spin}))
        printf "\r  ${GREEN}${spin:$i:1}${NC} ${GRAY}%s...${NC}" "$msg"
        sleep 0.08
    done

    wait $pid
    local exit_code=$?

    if [ $exit_code -eq 0 ]; then
        printf "\r\033[K  ${GREEN}✔${NC} ${BOLD}%s${NC}\n" "$msg"
    else
        if [ "$allow_fail" = "true" ]; then
            printf "\r\033[K  ${GREEN}✔${NC} ${BOLD}%s${NC} ${GRAY}(Skipped)${NC}\n" "$msg"
        else
            printf "\r\033[K  ${RED}✖${NC} ${BOLD}%s${NC}\n" "$msg"
            echo -e "\n${RED}  Build Log:${NC}"
            echo "  ----------------------------------------"
            sed 's/^/  /' "$LOG_FILE"
            echo "  ----------------------------------------"
            resume_audio_system
            exit 1
        fi
    fi
}

header
init_sudo

echo -e "  ${GRAY}Detecting audio system context...${NC}"
detect_audio_system
echo -e "  ${GREEN}✔${NC} ${BOLD}Detected audio engine:${NC} ${YELLOW}$DETECTED_AUDIO${NC}\n"

run "pause_audio_system" "Stopping audio services" "true"

run "make clean" "Cleaning build directory"
run "make" "Compiling driver source"

run "sudo mkdir -p \"$TARGET_DIR\"" "Verifying module directory"
run "sudo cp \"$KO_FILE\" \"$TARGET_DIR\"" "Installing kernel module"
run "sudo depmod -a" "Updating dependency map"

run "sudo rmmod $MODULE_NAME" "Unloading previous driver" "true"
run "sudo modprobe $MODULE_NAME" "Loading new driver"

run "resume_audio_system" "Unmasking and resuming audio services" "true"

echo -e "\n  ${GREEN}${BOLD}Success!${NC}"
echo -e "  The driver has been installed and loaded into the system."
echo -e "  ${YELLOW}Note: A system restart is recommended.${NC}\n"
