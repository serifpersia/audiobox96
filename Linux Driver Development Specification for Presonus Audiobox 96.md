Technical Specification: PreSonus AudioBox USB 96

Document Version: 2.0
Subject: Hardware Protocol, Endpoint Architecture, and ASIO-to-USB Mapping Matrix
1. Device Identification & Interface Profile

    Vendor ID: 0x194F (PreSonus Audio Electronics)

    Product ID: 0x0303

    USB Class: USB Audio Class 2.0 (UAC2) / High-Speed (480 Mbps)

    Clock Source: Internal Crystal (ID: 5), programmable via Host.

    Power: Bus Powered (500mA maximum draw).

2. Endpoint Configuration Table

Based on hardware descriptors, the following endpoints are used for operation. All audio endpoints utilize Asynchronous Synchronization, requiring the host to adjust to the device's clock.
Endpoint	Direction	Transfer Type	Max Packet Size	Interval (Microframes)	Purpose
0x01	OUT	Isochronous	104 Bytes	1 (125

        
μ
μ

      

s)	Audio Playback
0x81	IN	Isochronous	104 Bytes	1 (125

        
μ
μ

      

s)	Audio Recording
0x82	IN	Isochronous	4 Bytes	8 (1.0 ms)	Explicit Feedback
0x02	OUT	Bulk	512 Bytes	0	MIDI Data Out
0x83	IN	Bulk	512 Bytes	0	MIDI Data In
3. Audio Data Payload Calculation

The device uses a fixed 32-bit subslot for 24-bit PCM data. One stereo frame (L+R) is exactly 8 bytes.
Sample Rate	Frequency	Nominal Samples/Packet	Nominal Bytes/Packet	Pattern
44.1 kHz	44,100 Hz	5.5125	44.1	Alternates 5 and 6 samples (40/48 bytes)
48.0 kHz	48,000 Hz	6.0	48	Constant 6 samples (48 bytes)
88.2 kHz	88,200 Hz	11.025	88.2	Alternates 11 and 12 samples (88/96 bytes)
96.0 kHz	96,000 Hz	12.0	96	Constant 12 samples (96 bytes)
4. ASIO Buffer & URB Mapping Matrix

The driver categorizes sample buffer sizes into three "Latency Groups." These groups determine the number of packets bundled into a single USB Request Block (URB). This logic optimizes the balance between CPU interrupt overhead and audio stability.
Group A: Ultra-Low Latency (1ms USB Window)

    URB Packets: 8 microframes (1.0 ms duration)

    Interrupt Frequency: 1000 Hz

    Suggested URB Count: 4 to 8 in flight

Sample Rate	Supported Buffer Sizes (Samples)
44.1 / 48 kHz	16, 32, 64
88.2 / 96 kHz	32, 64, 128
Group B: Balanced Latency (2ms USB Window)

    URB Packets: 16 microframes (2.0 ms duration)

    Interrupt Frequency: 500 Hz

    Suggested URB Count: 3 to 4 in flight

Sample Rate	Supported Buffer Sizes (Samples)
44.1 / 48 kHz	128
88.2 / 96 kHz	256
Group C: High Stability / Recording (4ms USB Window)

    URB Packets: 32 microframes (4.0 ms duration)

    Interrupt Frequency: 250 Hz

    Suggested URB Count: 2 to 3 in flight

Sample Rate	Supported Buffer Sizes (Samples)
44.1 / 48 kHz	256, 512, 1024, 2048
88.2 / 96 kHz	512, 1024, 2048
5. Pipeline Depth Recommendations

To prevent XRUNS (pops/clicks), the hardware pipeline must always be "primed." The number of URBs should scale to ensure the hardware always has at least 2x the ASIO buffer size queued in the USB controller at all times.

    Low Buffers (Group A): Use 8 URBs. This creates an 8ms "safety cushion" regardless of the small 16 or 32 sample application buffer.

    Medium Buffers (Group B): Use 4 URBs. This creates an 8ms "safety cushion."

    High Buffers (Group C): Use 3 URBs. This creates a 12ms "safety cushion" while keeping interrupt overhead extremely low.

6. Synchronization Strategy (Feedback Adapter)

Do we assume feedback sync is the same for every configuration?
Yes. The feedback mechanism on Endpoint 0x82 is a purely physical measurement provided by the hardware.

    Uniform Protocol: Regardless of whether you are at 44.1kHz or 96kHz, the device sends a 32-bit value representing its current internal oscillation frequency relative to the USB Start-of-Frame (SOF).

    Adaptation Logic: The host uses the same mathematical logic for all rates:

        Read the feedback value.

        Shift/Normalize the value to match the currently selected Nominal Sample Rate.

        Adjust the number of samples sent per packet (the "Fractional Rate") to match.

    Independence from Buffer Size: The feedback loop is strictly tied to the Sample Rate, not the Buffer Size. Whether the buffer is 16 or 2048 samples, the feedback remains a 1ms-interval constant measurement.

Final Technical Summary for Implementation:

    Bit Depth: Always 32-bit slots.

    Latency: Controlled by switching between 8, 16, or 32 packets per URB.

    Sync: Mandatory poll of 0x82 every 1ms; feedback values must be used to drive the data output pace.

    Constraint: 88.2/96kHz must not attempt buffers below 32 samples due to the high data rate per microframe.
