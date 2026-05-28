# ALSA Driver for PreSonus AudioBox USB 96

An unofficial, low-latency ALSA kernel module for the PreSonus AudioBox USB 96 audio interface. This driver is designed to provide high-performance full-duplex audio support by bypassing the overhead of the generic USB audio driver.

## Project Status

### Implemented Features
* Audio Playback
* Audio Capture/Recording
* Dynamic URB configuration for best hardware configuration

### To-Do & Known Limitations
* MIDI IN/OUT (not yet implemented but planned)

## Compatibility

This driver has been tested and works reliably with the following audio servers:
* **PipeWire**
* **PipeWire-JACK**
* **JACK2**

## Performance Optimization

To achieve the lowest possible latency without audio dropouts (xruns), the following system optimizations are highly recommended:

### 1. Recommended Kernels
Using a kernel with a high-frequency timer and "Preempt" enabled is beneficial for pro-audio tasks.
* **Arch Linux:** The `linux-zen` kernel is recommended.
* **Debian / Ubuntu / Pop!_OS:** The **Liquorix** kernel is highly recommended.

### 2. CPU Power Management
When running lower buffer configurations, modern CPU power-saving features can cause latency spikes. Set your CPU governor to performance mode.

### 3. Realtime Configuration
To achieve stable low-latency performance, your user account must have realtime privileges.
* **Groups:** Ensure your user is a member of the `realtime` and `audio` groups:
  ```bash
  sudo usermod -aG realtime,audio $USER
  ```
* **Limits:** You may also need to configure realtime limits (rtprio, memlock) in `/etc/security/limits.conf` or `/etc/security/limits.d/99-realtime.conf`. Many distributions handle this automatically when you install a pro-audio package or the `realtime-privileges` package.

## Installation and Usage

This is an out-of-tree kernel module. You must compile it against the headers for your specific kernel version.

### Step 1: Prevent Conflict with the Generic USB Audio Driver

The standard Linux kernel includes the `snd-usb-audio` driver, which will automatically claim the AudioBox 96. You must create a udev rule to unbind the device from the generic driver so this custom module can take control.

1. Create a new udev rule file:
   ```bash
   sudo nano /etc/udev/rules.d/99-audiobox96.rules
   ```

2. Paste the following line:
   ```text
   ACTION=="add", SUBSYSTEM=="usb", ATTRS{idVendor}=="194f", ATTRS{idProduct}=="0303", RUN+="/bin/sh -c 'echo -n %k > /sys/bus/usb/drivers/snd-usb-audio/unbind'"
   ```

3. Reload the rules:
   ```bash
   sudo udevadm control --reload-rules
   sudo udevadm trigger
   ```

### Step 2: Install Prerequisites

Ensure you have the build tools and headers for your kernel:

* **Arch Linux / Manjaro:**
  ```bash
  sudo pacman -S base-devel linux-headers
  ```

* **Debian / Ubuntu / Pop!_OS / Mint:**
  ```bash
  sudo apt update
  sudo apt install build-essential linux-headers-$(uname -r)
  ```

* **Fedora / CentOS Stream / RHEL:**
  ```bash
  sudo dnf install kernel-devel kernel-headers make gcc
  ```

### Step 3: Compile and Load the Driver

1. Clone the repository and navigate to the directory:
   ```bash
   git clone https://github.com/serifpersia/audiobox96.git
   cd audiobox96
   ```

2. Compile the module:
   ```bash
   make
   ```

3. Load the module for the current session:
   ```bash
   sudo insmod snd-usb-audiobox96.ko
   ```

### Step 4: Persistent Installation

To automate the compilation and installation so the driver remains available after a reboot, use the provided installation script:

1. Make the script executable:
   ```bash
   chmod +x install.sh
   ```

2. Run the script:
   ```bash
   sudo ./install.sh
   ```

## Reporting Issues & Feedback

When reporting issues, please include:
* Linux distribution and version
* Kernel version (`uname -r`)
* Your audio setup (e.g., PipeWire or JACK version)
* Buffer size and period settings used during the issue
* Terminal output from `dmesg | tail -n 50`

## License

This project is licensed under the **GPL-2.0** license. See the [LICENSE](LICENSE) file for details.
