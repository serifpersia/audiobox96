#ifndef __AUDIOBOX96_H
#define __AUDIOBOX96_H

#include <linux/usb.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/ktime.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define DRIVER_NAME "audiobox96"

#define USB_VID_PRESONUS    0x194f
#define USB_PID_AUDIOBOX96   0x0303

#define AUDIOBOX_CONTROL_INTERFACE   0
#define AUDIOBOX_RECORD_INTERFACE    1
#define AUDIOBOX_PLAYBACK_INTERFACE  2

#define AUDIOBOX_RECORD_ENDPOINT     0x81
#define AUDIOBOX_PLAYBACK_ENDPOINT   0x01
#define AUDIOBOX_FEEDBACK_ENDPOINT   0x82
#define AUDIOBOX_CLOCK_SOURCE_ID     5

#define NUM_FEEDBACK_URBS       8
#define FEEDBACK_URB_PACKETS    4

#define AUDIOBOX_URBS           4
#define AUDIOBOX_PACKETS        2

#define NUM_CHANNELS            2
#define BYTES_PER_SAMPLE        4
#define PLAYBACK_FRAME_SIZE     (NUM_CHANNELS * BYTES_PER_SAMPLE)
#define CAPTURE_FRAME_SIZE      (NUM_CHANNELS * BYTES_PER_SAMPLE)

#define MAX_FRAMES_PER_PACKET   13

struct audiobox_card {
    struct usb_device *dev;
    struct snd_card *card;
    struct snd_pcm *pcm;

    struct snd_pcm_substream *playback_substream;
    struct snd_pcm_substream *capture_substream;

    int num_playback_urbs;
    int playback_urb_packets;
    struct urb **playback_urbs;
    size_t playback_urb_alloc_size;
    struct urb *feedback_urbs[NUM_FEEDBACK_URBS];
    size_t feedback_urb_alloc_size;

    int num_capture_urbs;
    int capture_urb_packets;
    struct urb **capture_urbs;
    size_t capture_alloc_size;

    struct usb_anchor playback_anchor;
    struct usb_anchor feedback_anchor;
    struct usb_anchor capture_anchor;

    spinlock_t lock;
    struct mutex mutex;
    atomic_t playback_active;
    atomic_t capture_active;
    atomic_t active_playback_urbs;
    int current_rate;

    u64 playback_frames_consumed;
    u64 playback_frames_submitted;
    unsigned int playback_urb_frames[32];
    ktime_t last_urb_completed_time;
    snd_pcm_uframes_t driver_playback_pos;
    snd_pcm_uframes_t period_elapsed_accum;

    u64 capture_frames_consumed;
    snd_pcm_uframes_t driver_capture_pos;
    snd_pcm_uframes_t capture_period_elapsed_accum;

    u32 phase_accum;
    u32 freq_q16;
    u64 freq_est_q24;
    bool feedback_synced;
    unsigned int feedback_urb_skip_count;

    int freqshift;
    bool freqshift_detected;
};

#endif
