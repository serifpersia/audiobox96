/* SPDX-License-Identifier: GPL-2.0 */
/**
 * DOC: PreSonus AudioBox USB 96 Kernel Driver Header
 */

#ifndef __AUDIOBOX96_H
#define __AUDIOBOX96_H

#include <linux/usb.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/ktime.h>
#include <linux/cache.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define DRIVER_NAME "audiobox96"

#define USB_VID_PRESONUS             0x194f
#define USB_PID_AUDIOBOX96           0x0303

#define AUDIOBOX_CONTROL_INTERFACE   0
#define AUDIOBOX_RECORD_INTERFACE    1
#define AUDIOBOX_PLAYBACK_INTERFACE  2

#define AUDIOBOX_RECORD_ENDPOINT     0x81
#define AUDIOBOX_PLAYBACK_ENDPOINT   0x01
#define AUDIOBOX_FEEDBACK_ENDPOINT   0x82
#define AUDIOBOX_CLOCK_SOURCE_ID     5

#define PLAYBACK_URB_COUNT           8
#define PLAYBACK_PACKET_COUNT        8
#define CAPTURE_URB_COUNT            8
#define CAPTURE_PACKET_COUNT         8

/* Accelerated 1000Hz Feedback Polling */
#define FEEDBACK_URB_COUNT           4
#define FEEDBACK_PACKET_COUNT        1

#define NUM_CHANNELS                 2
#define BYTES_PER_SAMPLE             4
#define PLAYBACK_FRAME_SIZE          (NUM_CHANNELS * BYTES_PER_SAMPLE)
#define CAPTURE_FRAME_SIZE           (NUM_CHANNELS * BYTES_PER_SAMPLE)

#define MAX_FRAMES_PER_PACKET        13
#define MAX_URB_PACKETS              16

/**
 * struct audiobox_card - Core device tracking structure
 * @dev: USB device pointer
 * @card: ALSA card instance
 * @pcm: ALSA PCM instance
 * @playback_substream: Active playback substream
 * @capture_substream: Active capture substream
 * @mutex: Mutex for sleeping context configuration operations
 * @playback_active: Playback stream state boolean
 * @capture_active: Capture stream state boolean
 * @active_playback_urbs: Counter for submitted playback URBs
 * @current_rate: Configured sample rate in Hz
 * @num_playback_urbs: Configured number of playback URBs
 * @playback_urb_packets: Configured packets per playback URB
 * @playback_urbs: Array of playback URB pointers
 * @playback_urb_alloc_size: Size of playback URB buffer
 * @feedback_urbs: Array of explicit feedback URB pointers
 * @feedback_urb_alloc_size: Size of feedback URB buffer
 * @num_capture_urbs: Configured number of capture URBs
 * @capture_urb_packets: Configured packets per capture URB
 * @capture_urbs: Array of capture URB pointers
 * @capture_alloc_size: Size of capture URB buffer
 * @playback_anchor: USB anchor for playback URBs
 * @feedback_anchor: USB anchor for feedback URBs
 * @capture_anchor: USB anchor for capture URBs
 * @playback_lock: Spinlock for IRQ-safe playback state transitions
 * @playback_frames_consumed: Total playback frames read by hardware
 * @playback_frames_submitted: Total playback frames prepared by driver
 * @playback_urb_frames: Array tracking frame counts per in-flight URB
 * @last_urb_completed_time: Timestamp of last playback URB completion (ns)
 * @expected_urb_duration_ns: Precalculated expected duration of an URB (ns)
 * @frames_per_ns_q32: Precalculated Q32 factor for fast nanosecond interpolation
 * @driver_playback_pos: Current stream position for playback
 * @last_playback_hw_ptr: Last reported pointer for monotonic safety checks
 * @period_elapsed_accum: Accumulator for ALSA period elapsed events
 * @phase_accum: Q16.16 phase accumulator for playback pacing
 * @freq_q16: Q16 normalized frequency metric
 * @nominal_q16: Precalculated base Q16 value for the selected sample rate
 * @freq_est_q32: Q32.32 filtered feedback frequency estimate
 * @feedback_synced: Boolean indicating lock onto hardware clock
 * @feedback_urb_skip_count: Counter to drop initial volatile feedback data
 * @freqshift: Bit shift offset required to normalize feedback values
 * @freqshift_candidate: Proposed bit shift offset under evaluation
 * @freqshift_match_count: Counter for consecutive identical shift proposals
 * @freqshift_detected: Boolean indicating successful shift normalization
 * @playback_xrun_count: Counter for playback underrun events
 * @playback_completion_count: Total playback URBs successfully processed
 * @playback_jitter_max_ns: Highest recorded latency delta against expected URB time
 * @playback_jitter_sum_ns: Accumulated latency deltas
 * @capture_lock: Spinlock for IRQ-safe capture state transitions
 * @capture_frames_consumed: Total capture frames received from hardware
 * @driver_capture_pos: Current stream position for capture
 * @last_capture_hw_ptr: Last reported pointer for monotonic safety checks
 * @capture_period_elapsed_accum: Accumulator for capture ALSA events
 * @capture_xrun_count: Counter for capture overrun events
 */
struct audiobox_card {
    /* Cold Section: Configuration and Initialization Variables */
    struct usb_device *dev;
    struct snd_card *card;
    struct snd_pcm *pcm;

    struct snd_pcm_substream *playback_substream;
    struct snd_pcm_substream *capture_substream;

    struct mutex mutex;

    atomic_t playback_active;
    atomic_t capture_active;
    atomic_t active_playback_urbs;
    int current_rate;

    int num_playback_urbs;
    int playback_urb_packets;
    struct urb **playback_urbs;
    size_t playback_urb_alloc_size;

    struct urb *feedback_urbs[FEEDBACK_URB_COUNT];
    size_t feedback_urb_alloc_size;

    int num_capture_urbs;
    int capture_urb_packets;
    struct urb **capture_urbs;
    size_t capture_alloc_size;

    struct usb_anchor playback_anchor;
    struct usb_anchor feedback_anchor;
    struct usb_anchor capture_anchor;

    /* Hot Section: Playback and Feedback Fast-Path (Cache-Line Aligned) */
    spinlock_t playback_lock ____cacheline_aligned;
    u64 playback_frames_consumed;
    u64 playback_frames_submitted;
    unsigned int *playback_urb_frames;
    u64 last_urb_completed_time;
    u64 expected_urb_duration_ns;
    u64 frames_per_ns_q32;
    snd_pcm_uframes_t driver_playback_pos;
    snd_pcm_uframes_t last_playback_hw_ptr;
    snd_pcm_uframes_t period_elapsed_accum;

    u32 phase_accum;
    u32 freq_q16;
    u32 nominal_q16;
    u64 freq_est_q32;
    bool feedback_synced;
    unsigned int feedback_urb_skip_count;

    int freqshift;
    int freqshift_candidate;
    int freqshift_match_count;
    bool freqshift_detected;

    u32 playback_xrun_count;
    u64 playback_completion_count;
    u64 playback_jitter_max_ns;
    u64 playback_jitter_sum_ns;

    /* Hot Section: Capture Fast-Path (Cache-Line Aligned) */
    spinlock_t capture_lock ____cacheline_aligned;
    u64 capture_frames_consumed;
    snd_pcm_uframes_t driver_capture_pos;
    snd_pcm_uframes_t last_capture_hw_ptr;
    snd_pcm_uframes_t capture_period_elapsed_accum;
    u32 capture_xrun_count;
    u64 last_capture_urb_completed_time;
};

#endif /* __AUDIOBOX96_H */
