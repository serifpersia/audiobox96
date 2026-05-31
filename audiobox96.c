/* SPDX-License-Identifier: GPL-2.0 */
/**
 * DOC: PreSonus AudioBox USB 96 ALSA Driver Implementation
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/minmax.h>
#include <linux/math64.h>
#include <linux/compiler.h>
#include <sound/info.h>
#include "audiobox96.h"

MODULE_AUTHOR("Šerif Rami <ramiserifpersia@gmail.com>");
MODULE_DESCRIPTION("ALSA Driver for PreSonus AudioBox USB 96");
MODULE_LICENSE("GPL");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = { 1, [1 ...(SNDRV_CARDS - 1)] = 0 };
static atomic_t dev_idx = ATOMIC_INIT(0);

static struct usb_driver audiobox_alsa_driver;

static const struct snd_pcm_hardware audiobox_playback_hw = {
    .info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
    SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID |
    SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME |
    SNDRV_PCM_INFO_BATCH | SNDRV_PCM_INFO_NO_PERIOD_WAKEUP |
    SNDRV_PCM_INFO_SYNC_APPLPTR),
    .formats = SNDRV_PCM_FMTBIT_S32_LE,
    .rates = (SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
    SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000),
    .rate_min = 44100,
    .rate_max = 96000,
    .channels_min = NUM_CHANNELS,
    .channels_max = NUM_CHANNELS,
    .buffer_bytes_max = 8192 * PLAYBACK_FRAME_SIZE,
    .period_bytes_min = 8 * PLAYBACK_FRAME_SIZE,
    .period_bytes_max = 4096 * PLAYBACK_FRAME_SIZE,
    .periods_min = 2,
    .periods_max = 128,
};

static const struct snd_pcm_hardware audiobox_capture_hw = {
    .info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
    SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID |
    SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME |
    SNDRV_PCM_INFO_BATCH | SNDRV_PCM_INFO_NO_PERIOD_WAKEUP |
    SNDRV_PCM_INFO_SYNC_APPLPTR),
    .formats = SNDRV_PCM_FMTBIT_S32_LE,
    .rates = (SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
    SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000),
    .rate_min = 44100,
    .rate_max = 96000,
    .channels_min = NUM_CHANNELS,
    .channels_max = NUM_CHANNELS,
    .buffer_bytes_max = 8192 * CAPTURE_FRAME_SIZE,
    .period_bytes_min = 8 * CAPTURE_FRAME_SIZE,
    .period_bytes_max = 4096 * CAPTURE_FRAME_SIZE,
    .periods_min = 2,
    .periods_max = 128,
};

/**
 * hw_rule_buffer_size_by_rate() - ALSA Hardware Rule
 * @params: Hardware parameters constraint structure
 * @rule: Evaluated rule
 *
 * Restricts the allowed buffer sizes for high sample rates (88.2k, 96k)
 * to prevent USB bandwidth saturation on extreme configurations.
 * Return: Zero on success, negative error code otherwise.
 */
static int hw_rule_buffer_size_by_rate(struct snd_pcm_hw_params *params,
                                       struct snd_pcm_hw_rule *rule)
{
    struct snd_interval *r = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
    struct snd_interval *b = hw_param_interval(params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
    struct snd_interval t;

    snd_interval_any(&t);
    if (r->min >= 88200) {
        t.min = 32;
        t.max = 2048;
        return snd_interval_refine(b, &t);
    }
    return 0;
}

/**
 * hw_rule_rate_by_buffer_size() - ALSA Hardware Rule
 * @params: Hardware parameters constraint structure
 * @rule: Evaluated rule
 *
 * Prevents ALSA from selecting high sample rates when the buffer size
 * is extremely low (<32 frames), matching hardware constraints.
 * Return: Zero on success, negative error code otherwise.
 */
static int hw_rule_rate_by_buffer_size(struct snd_pcm_hw_params *params,
                                       struct snd_pcm_hw_rule *rule)
{
    struct snd_interval *b = hw_param_interval(params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
    struct snd_interval *r = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
    struct snd_interval t;

    snd_interval_any(&t);
    if (b->max < 32) {
        t.min = 44100;
        t.max = 48000;
        return snd_interval_refine(r, &t);
    }
    return 0;
}

/**
 * audiobox_calc_urb_config() - Resolve optimal URB pipeline depth
 * @rate: Active sample rate in Hz.
 * @period_size: ALSA PCM period size in frames.
 * @buffer_size: Total ALSA PCM buffer size in frames.
 * @out_urbs: Calculated optimal URB count returned by reference.
 * @out_packets: Calculated optimal packets per URB returned by reference.
 */
static void audiobox_calc_urb_config(unsigned int rate,
                                     snd_pcm_uframes_t period_size,
                                     snd_pcm_uframes_t buffer_size,
                                     int *out_urbs, int *out_packets)
{
    int packets;
    int urbs;
    u64 queue_frames;
    u64 frames_per_packet = rate / 8000;

    if (rate <= 48000) {
        if (period_size <= 16)
            packets = 1;
        else if (period_size <= 64)
            packets = 2;
        else if (period_size <= 512)
            packets = 4;
        else
            packets = 8;
    } else {
        if (period_size <= 32)
            packets = 1;
        else if (period_size <= 128)
            packets = 2;
        else if (period_size <= 1024)
            packets = 4;
        else
            packets = 8;
    }

    if (period_size <= 16)
        urbs = 2;
    else if (period_size <= 32)
        urbs = 3;
    else
        urbs = 4;

    queue_frames = (u64)urbs * packets * frames_per_packet;

    while (queue_frames > buffer_size) {
        if (urbs > 2) {
            urbs--;
        } else if (packets > 1) {
            packets >>= 1;
        } else {
            urbs = 2;
            packets = 1;
            break;
        }
        queue_frames = (u64)urbs * packets * frames_per_packet;
    }

    *out_urbs = urbs;
    *out_packets = packets;
}

/**
 * audiobox96_set_rate() - Configure hardware internal clock rate
 * @chip: Driver context.
 * @rate: Target sample rate in Hz.
 *
 * Return: Zero on success, negative error code otherwise.
 */
static int audiobox96_set_rate(struct audiobox_card *chip, int rate)
{
    struct usb_device *dev = chip->dev;
    __le32 *rate_le;
    int err;

    rate_le = kmalloc(sizeof(__le32), GFP_KERNEL);
    if (!rate_le)
        return -ENOMEM;

    *rate_le = cpu_to_le32(rate);

    err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
                          0x01, 0x21,
                          0x0100, (AUDIOBOX_CLOCK_SOURCE_ID << 8) | 0,
                          rate_le, 4, 1000);

    kfree(rate_le);

    if (err < 0) {
        dev_err(&dev->dev, "UAC2 clock set failed for %d Hz: %d\n", rate, err);
        return err;
    }
    return 0;
}

/**
 * audiobox96_get_clock_valid() - Verify hardware clock stabilization
 * @chip: Driver context.
 * @out_valid: Pointer to store boolean result.
 *
 * Return: Zero on success, negative error code otherwise.
 */
static int audiobox96_get_clock_valid(struct audiobox_card *chip, u8 *out_valid)
{
    struct usb_device *dev = chip->dev;
    u8 *valid_buf;
    int err;

    valid_buf = kmalloc(1, GFP_KERNEL);
    if (!valid_buf)
        return -ENOMEM;

    err = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
                          0x01, 0xA1,
                          (0x02 << 8), (AUDIOBOX_CLOCK_SOURCE_ID << 8) | AUDIOBOX_CONTROL_INTERFACE,
                          valid_buf, 1, 1000);

    if (err >= 0) {
        *out_valid = *valid_buf;
        err = 0;
    }
    kfree(valid_buf);
    return err;
}

/**
 * copy_capture_samples() - Transfer recording data to ALSA buffer lock-free
 * @substream: Capture PCM substream.
 * @pos: Frame position in hardware buffer.
 * @src: Pointer to incoming URB payload.
 * @frames: Count of frames to copy.
 */
static void copy_capture_samples(struct snd_pcm_substream *substream,
                                 snd_pcm_uframes_t pos,
                                 const u8 *src,
                                 unsigned int frames)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    unsigned int bytes_to_copy = frames * CAPTURE_FRAME_SIZE;
    size_t byte_pos = frames_to_bytes(runtime, pos);

    if (pos + frames > runtime->buffer_size) {
        size_t part1_frames = runtime->buffer_size - pos;
        size_t part1_bytes = frames_to_bytes(runtime, part1_frames);

        memcpy(runtime->dma_area + byte_pos, src, part1_bytes);
        memcpy(runtime->dma_area, src + part1_bytes, bytes_to_copy - part1_bytes);
    } else {
        memcpy(runtime->dma_area + byte_pos, src, bytes_to_copy);
    }
}

/**
 * capture_urb_complete() - USB Interrupt completion handler for recording
 * @urb: USB request block instance.
 */
static void capture_urb_complete(struct urb *urb)
{
    struct audiobox_card *chip = urb->context;
    struct snd_pcm_substream *substream;
    struct snd_pcm_runtime *runtime;
    int i;
    unsigned long flags;
    int periods_elapsed = 0;
    bool trigger_xrun = false;

    snd_pcm_uframes_t copy_pos_array[MAX_URB_PACKETS];
    snd_pcm_uframes_t frames_array[MAX_URB_PACKETS];
    const u8 *src_array[MAX_URB_PACKETS];
    int valid_packets = 0;

    if (unlikely(urb->status)) {
        if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
            urb->status == -ESHUTDOWN || !chip)
            goto exit_clear;
    }

    if (unlikely(!atomic_read(&chip->capture_active)))
        goto exit_clear;

    substream = chip->capture_substream;
    if (unlikely(!substream || !substream->runtime))
        goto exit_clear;

    runtime = substream->runtime;

    spin_lock_irqsave(&chip->capture_lock, flags);
    chip->last_capture_urb_completed_time = ktime_get_ns();

    for (i = 0; i < urb->number_of_packets; i++) {
        int status = urb->iso_frame_desc[i].status;
        unsigned int actual_len = urb->iso_frame_desc[i].actual_length;
        unsigned int offset = urb->iso_frame_desc[i].offset;

        if (likely(status == 0 && actual_len >= CAPTURE_FRAME_SIZE)) {
            snd_pcm_uframes_t frames = actual_len / CAPTURE_FRAME_SIZE;
            snd_pcm_sframes_t used_space;
            snd_pcm_sframes_t avail_space;

            if (chip->capture_frames_consumed >= runtime->control->appl_ptr) {
                used_space = chip->capture_frames_consumed - runtime->control->appl_ptr;
            } else {
                used_space = (chip->capture_frames_consumed + runtime->boundary) - runtime->control->appl_ptr;
            }
            avail_space = runtime->buffer_size - used_space;

            if (unlikely(avail_space < (snd_pcm_sframes_t)frames)) {
                if (runtime->status->state == SNDRV_PCM_STATE_RUNNING) {
                    trigger_xrun = true;
                    chip->capture_xrun_count++;
                }
            }

            if (likely(valid_packets < MAX_URB_PACKETS)) {
                copy_pos_array[valid_packets] = chip->driver_capture_pos;
                frames_array[valid_packets] = frames;
                src_array[valid_packets] = (const u8 *)urb->transfer_buffer + offset;
                valid_packets++;
            }

            chip->driver_capture_pos += frames;
            if (chip->driver_capture_pos >= runtime->buffer_size)
                chip->driver_capture_pos -= runtime->buffer_size;

            chip->capture_frames_consumed += frames;
            if (chip->capture_frames_consumed >= runtime->boundary)
                chip->capture_frames_consumed -= runtime->boundary;

            chip->capture_period_elapsed_accum += frames;
            while (chip->capture_period_elapsed_accum >= runtime->period_size) {
                chip->capture_period_elapsed_accum -= runtime->period_size;
                periods_elapsed++;
            }
        }
    }

    spin_unlock_irqrestore(&chip->capture_lock, flags);

    if (unlikely(trigger_xrun)) {
        snd_pcm_stop_xrun(substream);
        goto resubmit;
    }

    for (i = 0; i < valid_packets; i++)
        copy_capture_samples(substream, copy_pos_array[i], src_array[i], frames_array[i]);

    while (unlikely(periods_elapsed > 0)) {
        snd_pcm_period_elapsed(substream);
        periods_elapsed--;
    }

    resubmit:
    for (i = 0; i < urb->number_of_packets; i++) {
        urb->iso_frame_desc[i].offset = i * (MAX_FRAMES_PER_PACKET * CAPTURE_FRAME_SIZE);
        urb->iso_frame_desc[i].length = MAX_FRAMES_PER_PACKET * CAPTURE_FRAME_SIZE;
        urb->iso_frame_desc[i].status = 0;
        urb->iso_frame_desc[i].actual_length = 0;
    }

    usb_anchor_urb(urb, &chip->capture_anchor);
    if (unlikely(usb_submit_urb(urb, GFP_ATOMIC) < 0))
        goto exit_clear;

    return;

    exit_clear:
    usb_unanchor_urb(urb);
}

/**
 * copy_play_samples() - Transfer playback data from ALSA buffer lock-free
 * @runtime: Playback PCM runtime.
 * @pos: Frame position in hardware buffer.
 * @dst: Pointer to outgoing URB payload buffer.
 * @frames: Count of frames to copy.
 */
static void copy_play_samples(struct snd_pcm_runtime *runtime,
                              snd_pcm_uframes_t pos,
                              u8 *dst,
                              unsigned int frames)
{
    unsigned int bytes_to_copy = frames * PLAYBACK_FRAME_SIZE;
    size_t byte_pos = frames_to_bytes(runtime, pos);

    if (pos + frames > runtime->buffer_size) {
        size_t part1_frames = runtime->buffer_size - pos;
        size_t part1_bytes = frames_to_bytes(runtime, part1_frames);

        memcpy(dst, runtime->dma_area + byte_pos, part1_bytes);
        memcpy(dst + part1_bytes, runtime->dma_area, bytes_to_copy - part1_bytes);
    } else {
        memcpy(dst, runtime->dma_area + byte_pos, bytes_to_copy);
    }
}

/**
 * playback_urb_complete() - USB Interrupt completion handler for playback
 * @urb: USB request block instance.
 */
static void playback_urb_complete(struct urb *urb)
{
    struct audiobox_card *chip = urb->context;
    struct snd_pcm_substream *substream;
    struct snd_pcm_runtime *runtime;
    size_t total_bytes_for_urb = 0;
    snd_pcm_uframes_t copy_pos;
    snd_pcm_uframes_t frames_to_copy;
    snd_pcm_sframes_t avail_frames;
    size_t bytes_to_copy = 0;
    size_t silence_bytes = 0;
    int i, urb_idx = -1;
    unsigned long flags;
    int periods_elapsed = 0;
    bool trigger_xrun = false;
    unsigned int new_frames = 0;
    u64 now_ns;

    if (unlikely(urb->status)) {
        if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
            urb->status == -ESHUTDOWN || !chip)
            goto exit_clear;
    }

    if (unlikely(!atomic_read(&chip->playback_active)))
        goto exit_clear;

    substream = chip->playback_substream;
    if (unlikely(!substream || !substream->runtime))
        goto exit_clear;

    runtime = substream->runtime;
    now_ns = ktime_get_ns();

    spin_lock_irqsave(&chip->playback_lock, flags);

    if (likely(chip->last_urb_completed_time)) {
        u64 delta_ns = (now_ns > chip->last_urb_completed_time) ?
        (now_ns - chip->last_urb_completed_time) : 0;
        u64 expected_ns = chip->expected_urb_duration_ns;
        u64 jitter_ns = (delta_ns > expected_ns) ? (delta_ns - expected_ns) : (expected_ns - delta_ns);

        if (unlikely(jitter_ns > chip->playback_jitter_max_ns))
            chip->playback_jitter_max_ns = jitter_ns;

        chip->playback_jitter_sum_ns += jitter_ns;
        chip->playback_completion_count++;
    }
    chip->last_urb_completed_time = now_ns;

    for (i = 0; i < chip->num_playback_urbs; i++) {
        if (chip->playback_urbs[i] == urb) {
            urb_idx = i;
            break;
        }
    }

    if (likely(urb_idx >= 0)) {
        chip->playback_frames_consumed += chip->playback_urb_frames[urb_idx];
        if (chip->playback_frames_consumed >= runtime->boundary)
            chip->playback_frames_consumed -= runtime->boundary;
    }

    for (i = 0; i < urb->number_of_packets; i++) {
        unsigned int frames_for_packet;

        chip->phase_accum += chip->freq_q16;
        frames_for_packet = chip->phase_accum >> 16;
        chip->phase_accum &= 0xFFFF;

        if (unlikely(frames_for_packet > MAX_FRAMES_PER_PACKET))
            frames_for_packet = MAX_FRAMES_PER_PACKET;

        urb->iso_frame_desc[i].offset = total_bytes_for_urb;
        urb->iso_frame_desc[i].length = frames_for_packet * PLAYBACK_FRAME_SIZE;
        total_bytes_for_urb += urb->iso_frame_desc[i].length;
    }
    urb->transfer_buffer_length = total_bytes_for_urb;

    frames_to_copy = bytes_to_frames(runtime, total_bytes_for_urb);

    if (runtime->control->appl_ptr >= chip->playback_frames_submitted) {
        avail_frames = runtime->control->appl_ptr - chip->playback_frames_submitted;
    } else {
        avail_frames = (runtime->control->appl_ptr + runtime->boundary) - chip->playback_frames_submitted;
    }

    copy_pos = chip->driver_playback_pos;

    if (unlikely(avail_frames < (snd_pcm_sframes_t)frames_to_copy)) {
        if (runtime->status->state == SNDRV_PCM_STATE_RUNNING) {
            trigger_xrun = true;
            chip->playback_xrun_count++;
        }

        if (avail_frames > 0) {
            bytes_to_copy = frames_to_bytes(runtime, avail_frames);
            silence_bytes = total_bytes_for_urb - bytes_to_copy;
            chip->driver_playback_pos += avail_frames;
            chip->period_elapsed_accum += avail_frames;
            new_frames = avail_frames;
        } else {
            bytes_to_copy = 0;
            silence_bytes = total_bytes_for_urb;
            new_frames = 0;
        }
    } else {
        bytes_to_copy = total_bytes_for_urb;
        silence_bytes = 0;
        chip->driver_playback_pos += frames_to_copy;
        chip->period_elapsed_accum += frames_to_copy;
        new_frames = frames_to_copy;
    }

    if (likely(urb_idx >= 0))
        chip->playback_urb_frames[urb_idx] = new_frames;

    chip->playback_frames_submitted += new_frames;
    if (chip->playback_frames_submitted >= runtime->boundary)
        chip->playback_frames_submitted -= runtime->boundary;

    if (chip->driver_playback_pos >= runtime->buffer_size)
        chip->driver_playback_pos -= runtime->buffer_size;

    while (chip->period_elapsed_accum >= runtime->period_size) {
        chip->period_elapsed_accum -= runtime->period_size;
        periods_elapsed++;
    }

    spin_unlock_irqrestore(&chip->playback_lock, flags);

    if (unlikely(trigger_xrun)) {
        snd_pcm_stop_xrun(substream);
        if (total_bytes_for_urb > 0)
            memset(urb->transfer_buffer, 0, total_bytes_for_urb);
    } else if (likely(total_bytes_for_urb > 0)) {
        u8 *dst_buf = urb->transfer_buffer;

        if (likely(bytes_to_copy > 0)) {
            unsigned int frames_copied = bytes_to_frames(runtime, bytes_to_copy);
            copy_play_samples(runtime, copy_pos, dst_buf, frames_copied);
        }

        if (unlikely(silence_bytes > 0))
            memset(dst_buf + bytes_to_copy, 0, silence_bytes);
    }

    while (unlikely(periods_elapsed > 0)) {
        snd_pcm_period_elapsed(substream);
        periods_elapsed--;
    }

    usb_anchor_urb(urb, &chip->playback_anchor);
    if (unlikely(usb_submit_urb(urb, GFP_ATOMIC) < 0))
        goto exit_clear;

    return;

    exit_clear:
    usb_unanchor_urb(urb);
    atomic_dec(&chip->active_playback_urbs);
}

/**
 * feedback_urb_complete() - URB completion handler for explicit synchronization
 * @urb: USB request block instance.
 */
static void feedback_urb_complete(struct urb *urb)
{
    struct audiobox_card *chip = urb->context;
    unsigned long flags;
    int p;

    if (unlikely(urb->status)) {
        if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
            urb->status == -ESHUTDOWN || !chip || !atomic_read(&chip->playback_active)) {
            usb_unanchor_urb(urb);
        return;
            }
            goto resubmit;
    }

    spin_lock_irqsave(&chip->playback_lock, flags);

    if (unlikely(chip->feedback_urb_skip_count > 0)) {
        chip->feedback_urb_skip_count--;
        spin_unlock_irqrestore(&chip->playback_lock, flags);
        goto resubmit;
    }

    for (p = 0; p < urb->number_of_packets; p++) {
        unsigned int len = urb->iso_frame_desc[p].actual_length;

        if (likely(urb->iso_frame_desc[p].status == 0 && len >= 3)) {
            u8 *data = (u8 *)urb->transfer_buffer + urb->iso_frame_desc[p].offset;
            u32 feedback_val = 0;

            if (len == 3)
                feedback_val = (data[0] | (data[1] << 8) | (data[2] << 16));
            else
                feedback_val = le32_to_cpup((__le32 *)data);

            if (likely(feedback_val > 0)) {
                u32 f_shifted = feedback_val;

                if (likely(feedback_val > (chip->nominal_q16 / 2))) {
                    if (unlikely(!chip->freqshift_detected)) {
                        int shift = 0;
                        while (f_shifted < chip->nominal_q16 - chip->nominal_q16 / 4) {
                            f_shifted <<= 1;
                            shift++;
                        }
                        while (f_shifted > chip->nominal_q16 + chip->nominal_q16 / 2) {
                            f_shifted >>= 1;
                            shift--;
                        }

                        if (shift == chip->freqshift_candidate) {
                            chip->freqshift_match_count++;
                            if (chip->freqshift_match_count >= 3) {
                                chip->freqshift = shift;
                                chip->freqshift_detected = true;
                            }
                        } else {
                            chip->freqshift_candidate = shift;
                            chip->freqshift_match_count = 1;
                        }
                    } else {
                        if (chip->freqshift > 0)
                            f_shifted <<= chip->freqshift;
                        else if (chip->freqshift < 0)
                            f_shifted >>= -chip->freqshift;
                    }

                    u32 diff = (f_shifted > chip->nominal_q16) ?
                    (f_shifted - chip->nominal_q16) :
                    (chip->nominal_q16 - f_shifted);

                    if (likely(diff < (chip->nominal_q16 / 4))) {
                        u64 f_val_q32 = (u64)f_shifted << 16;
                        chip->freq_est_q32 = ((chip->freq_est_q32 * 255) + f_val_q32) >> 8;
                        chip->freq_q16 = (u32)((chip->freq_est_q32 + 0x8000) >> 16);
                        chip->feedback_synced = true;
                    }
                }
            }
        }
    }
    spin_unlock_irqrestore(&chip->playback_lock, flags);

    resubmit:
    usb_anchor_urb(urb, &chip->feedback_anchor);
    if (unlikely(usb_submit_urb(urb, GFP_ATOMIC) < 0))
        usb_unanchor_urb(urb);
}

/**
 * audiobox_free_capture_urbs() - Release memory allocated for capture
 * @chip: Driver context.
 */
static void audiobox_free_capture_urbs(struct audiobox_card *chip)
{
    int i;

    usb_kill_anchored_urbs(&chip->capture_anchor);

    if (chip->capture_urbs) {
        for (i = 0; i < chip->num_capture_urbs; i++) {
            if (chip->capture_urbs[i]) {
                usb_free_coherent(chip->dev, chip->capture_alloc_size,
                                  chip->capture_urbs[i]->transfer_buffer,
                                  chip->capture_urbs[i]->transfer_dma);
                usb_free_urb(chip->capture_urbs[i]);
            }
        }
        kfree(chip->capture_urbs);
        chip->capture_urbs = NULL;
    }
}

/**
 * audiobox_free_urbs() - Release memory allocated for playback and feedback
 * @chip: Driver context.
 */
static void audiobox_free_urbs(struct audiobox_card *chip)
{
    int i;

    usb_kill_anchored_urbs(&chip->playback_anchor);
    usb_kill_anchored_urbs(&chip->feedback_anchor);

    if (chip->playback_urbs) {
        for (i = 0; i < chip->num_playback_urbs; i++) {
            if (chip->playback_urbs[i]) {
                usb_free_coherent(chip->dev, chip->playback_urb_alloc_size,
                                  chip->playback_urbs[i]->transfer_buffer,
                                  chip->playback_urbs[i]->transfer_dma);
                usb_free_urb(chip->playback_urbs[i]);
            }
        }
        kfree(chip->playback_urbs);
        chip->playback_urbs = NULL;
    }

    kfree(chip->playback_urb_frames);
    chip->playback_urb_frames = NULL;

    for (i = 0; i < FEEDBACK_URB_COUNT; i++) {
        if (chip->feedback_urbs[i]) {
            usb_free_coherent(chip->dev, chip->feedback_urb_alloc_size,
                              chip->feedback_urbs[i]->transfer_buffer,
                              chip->feedback_urbs[i]->transfer_dma);
            usb_free_urb(chip->feedback_urbs[i]);
            chip->feedback_urbs[i] = NULL;
        }
    }
}

/**
 * audiobox_alloc_capture_urbs() - Allocate DMA memory for capture
 * @chip: Driver context.
 *
 * Return: Zero on success, ENOMEM on failure.
 */
static int audiobox_alloc_capture_urbs(struct audiobox_card *chip)
{
    int i, u;

    chip->capture_alloc_size = chip->capture_urb_packets * (MAX_FRAMES_PER_PACKET * CAPTURE_FRAME_SIZE);

    chip->capture_urbs = kcalloc(chip->num_capture_urbs, sizeof(struct urb *), GFP_KERNEL);
    if (!chip->capture_urbs)
        return -ENOMEM;

    for (i = 0; i < chip->num_capture_urbs; i++) {
        struct urb *urb = usb_alloc_urb(chip->capture_urb_packets, GFP_KERNEL);
        if (!urb)
            return -ENOMEM;
        chip->capture_urbs[i] = urb;
        urb->transfer_buffer = usb_alloc_coherent(chip->dev, chip->capture_alloc_size,
                                                  GFP_KERNEL, &urb->transfer_dma);
        if (!urb->transfer_buffer)
            return -ENOMEM;
        urb->dev = chip->dev;
        urb->pipe = usb_rcvisocpipe(chip->dev, AUDIOBOX_RECORD_ENDPOINT);
        urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
        urb->interval = 1;
        urb->context = chip;
        urb->complete = capture_urb_complete;

        for (u = 0; u < chip->capture_urb_packets; u++) {
            urb->iso_frame_desc[u].offset = u * (MAX_FRAMES_PER_PACKET * CAPTURE_FRAME_SIZE);
            urb->iso_frame_desc[u].length = MAX_FRAMES_PER_PACKET * CAPTURE_FRAME_SIZE;
        }
        urb->transfer_buffer_length = chip->capture_alloc_size;
    }
    return 0;
}

/**
 * audiobox_alloc_urbs() - Allocate DMA memory for playback and feedback
 * @chip: Driver context.
 *
 * Return: Zero on success, ENOMEM on failure.
 */
static int audiobox_alloc_urbs(struct audiobox_card *chip)
{
    int i;

    chip->playback_urb_alloc_size = chip->playback_urb_packets * (MAX_FRAMES_PER_PACKET * PLAYBACK_FRAME_SIZE);

    chip->playback_urbs = kcalloc(chip->num_playback_urbs, sizeof(struct urb *), GFP_KERNEL);
    if (!chip->playback_urbs)
        return -ENOMEM;

    chip->playback_urb_frames = kcalloc(chip->num_playback_urbs, sizeof(unsigned int), GFP_KERNEL);
    if (!chip->playback_urb_frames)
        return -ENOMEM;

    for (i = 0; i < chip->num_playback_urbs; i++) {
        struct urb *urb = usb_alloc_urb(chip->playback_urb_packets, GFP_KERNEL);
        if (!urb)
            return -ENOMEM;
        chip->playback_urbs[i] = urb;
        urb->transfer_buffer = usb_alloc_coherent(chip->dev, chip->playback_urb_alloc_size,
                                                  GFP_KERNEL, &urb->transfer_dma);
        if (!urb->transfer_buffer)
            return -ENOMEM;
        urb->dev = chip->dev;
        urb->pipe = usb_sndisocpipe(chip->dev, AUDIOBOX_PLAYBACK_ENDPOINT);
        urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
        urb->interval = 1;
        urb->context = chip;
        urb->complete = playback_urb_complete;
    }

    chip->feedback_urb_alloc_size = FEEDBACK_PACKET_COUNT * 4;
    for (i = 0; i < FEEDBACK_URB_COUNT; i++) {
        struct urb *urb = usb_alloc_urb(FEEDBACK_PACKET_COUNT, GFP_KERNEL);
        if (!urb)
            return -ENOMEM;
        chip->feedback_urbs[i] = urb;
        urb->transfer_buffer = usb_alloc_coherent(chip->dev, chip->feedback_urb_alloc_size,
                                                  GFP_KERNEL, &urb->transfer_dma);
        if (!urb->transfer_buffer)
            return -ENOMEM;
        urb->dev = chip->dev;
        urb->pipe = usb_rcvisocpipe(chip->dev, AUDIOBOX_FEEDBACK_ENDPOINT);
        urb->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
        urb->interval = 8;
        urb->context = chip;
        urb->complete = feedback_urb_complete;
    }
    return 0;
}

/**
 * audiobox_playback_open() - Open callback for ALSA playback PCM
 * @substream: Target substream.
 *
 * Return: Zero on success, negative error code on failure.
 */
static int audiobox_playback_open(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    int err;

    runtime->hw = audiobox_playback_hw;

    err = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
    if (err < 0)
        return err;

    err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
                              hw_rule_buffer_size_by_rate, NULL,
                              SNDRV_PCM_HW_PARAM_RATE, -1);
    if (err < 0)
        return err;

    err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
                              hw_rule_rate_by_buffer_size, NULL,
                              SNDRV_PCM_HW_PARAM_BUFFER_SIZE, -1);
    if (err < 0)
        return err;

    chip->playback_substream = substream;
    atomic_set(&chip->playback_active, 0);
    return 0;
}

/**
 * audiobox_playback_close() - Close callback for ALSA playback PCM
 * @substream: Target substream.
 *
 * Return: Always zero.
 */
static int audiobox_playback_close(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);

    atomic_set(&chip->playback_active, 0);
    usb_kill_anchored_urbs(&chip->playback_anchor);
    usb_kill_anchored_urbs(&chip->feedback_anchor);

    mutex_lock(&chip->mutex);
    audiobox_free_urbs(chip);
    mutex_unlock(&chip->mutex);

    usb_set_interface(chip->dev, AUDIOBOX_PLAYBACK_INTERFACE, 0);
    chip->playback_substream = NULL;
    return 0;
}

/**
 * audiobox_playback_hw_params() - Handle dynamic URB allocation for playback
 * @substream: Target substream.
 * @params: Requested hardware parameters.
 *
 * Return: Zero on success, negative error code on failure.
 */
static int audiobox_playback_hw_params(struct snd_pcm_substream *substream,
                                       struct snd_pcm_hw_params *params)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    unsigned int rate = params_rate(params);
    snd_pcm_uframes_t period_size = params_period_size(params);
    snd_pcm_uframes_t buffer_size = params_buffer_size(params);
    unsigned long flags;
    int err = 0;

    mutex_lock(&chip->mutex);

    spin_lock_irqsave(&chip->playback_lock, flags);
    if (atomic_read(&chip->playback_active)) {
        spin_unlock_irqrestore(&chip->playback_lock, flags);
        err = -EBUSY;
        goto unlock;
    }
    spin_unlock_irqrestore(&chip->playback_lock, flags);

    usb_kill_anchored_urbs(&chip->playback_anchor);
    usb_kill_anchored_urbs(&chip->feedback_anchor);
    atomic_set(&chip->active_playback_urbs, 0);

    audiobox_free_urbs(chip);

    audiobox_calc_urb_config(rate, period_size, buffer_size,
                             &chip->num_playback_urbs, &chip->playback_urb_packets);

    if (chip->current_rate != rate) {
        if (atomic_read(&chip->capture_active)) {
            dev_err(&chip->dev->dev, "Cannot change rate while capture is active\n");
            err = -EBUSY;
            goto unlock;
        }
        err = audiobox96_set_rate(chip, rate);
        if (err < 0)
            goto unlock;
        chip->current_rate = rate;
    }

    err = audiobox_alloc_urbs(chip);
    if (err < 0)
        goto unlock;

    unlock:
    mutex_unlock(&chip->mutex);
    return err;
}

/**
 * audiobox_playback_hw_free() - Hardware parameter release callback
 * @substream: Target substream.
 *
 * Return: Always zero.
 */
static int audiobox_playback_hw_free(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    mutex_lock(&chip->mutex);
    audiobox_free_urbs(chip);
    mutex_unlock(&chip->mutex);
    return 0;
}

/**
 * audiobox_playback_prepare() - Pre-stream configuration callback
 * @substream: Target substream.
 *
 * Return: Zero on success, negative error code on failure.
 */
static int audiobox_playback_prepare(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    int i, u, err = 0;
    size_t nominal_bytes = (runtime->rate / 8000) * PLAYBACK_FRAME_SIZE;
    u8 is_clock_valid = 0;

    mutex_lock(&chip->mutex);

    usb_kill_anchored_urbs(&chip->playback_anchor);
    usb_kill_anchored_urbs(&chip->feedback_anchor);

    err = audiobox96_get_clock_valid(chip, &is_clock_valid);
    if (err < 0 || !is_clock_valid) {
        dev_err(&chip->dev->dev, "Hardware internal clock INVALID status: %d\n", err);
        err = -EIO;
        goto unlock;
    }

    err = usb_set_interface(chip->dev, AUDIOBOX_PLAYBACK_INTERFACE, 1);
    if (err < 0) {
        dev_err(&chip->dev->dev, "Failed to set altsetting 1: %d\n", err);
        goto unlock;
    }

    chip->driver_playback_pos = 0;
    chip->last_playback_hw_ptr = 0;
    chip->playback_frames_consumed = 0;
    chip->playback_frames_submitted = 0;
    for (i = 0; i < chip->num_playback_urbs; i++)
        chip->playback_urb_frames[i] = 0;
    chip->period_elapsed_accum = 0;
    chip->feedback_synced = false;
    chip->feedback_urb_skip_count = 100;
    chip->phase_accum = 0;

    chip->freq_est_q32 = div_u64((u64)runtime->rate << 32, 8000);
    chip->freq_q16 = (u32)((chip->freq_est_q32 + 0x8000) >> 16);
    chip->nominal_q16 = div_u64((u64)runtime->rate << 16, 8000);
    chip->expected_urb_duration_ns = (u64)chip->playback_urb_packets * 125000ULL;
    chip->frames_per_ns_q32 = div_u64((u64)runtime->rate << 32, 1000000000ULL);

    chip->freqshift = 0;
    chip->freqshift_candidate = 0;
    chip->freqshift_match_count = 0;
    chip->freqshift_detected = false;
    atomic_set(&chip->active_playback_urbs, 0);

    chip->playback_xrun_count = 0;
    chip->playback_completion_count = 0;
    chip->playback_jitter_max_ns = 0;
    chip->playback_jitter_sum_ns = 0;

    for (i = 0; i < FEEDBACK_URB_COUNT; i++) {
        struct urb *f_urb = chip->feedback_urbs[i];
        if (!f_urb) continue;
        f_urb->number_of_packets = FEEDBACK_PACKET_COUNT;
        f_urb->transfer_buffer_length = FEEDBACK_PACKET_COUNT * 4;
        for (u = 0; u < FEEDBACK_PACKET_COUNT; u++) {
            f_urb->iso_frame_desc[u].offset = u * 4;
            f_urb->iso_frame_desc[u].length = 4;
        }
    }

    for (u = 0; u < chip->num_playback_urbs; u++) {
        struct urb *urb = chip->playback_urbs[u];
        size_t total_bytes = 0;

        if (!urb) continue;
        urb->number_of_packets = chip->playback_urb_packets;
        for (i = 0; i < chip->playback_urb_packets; i++) {
            urb->iso_frame_desc[i].offset = i * nominal_bytes;
            urb->iso_frame_desc[i].length = nominal_bytes;
            total_bytes += nominal_bytes;
        }
        urb->transfer_buffer_length = total_bytes;
        memset(urb->transfer_buffer, 0, total_bytes);
    }

    unlock:
    mutex_unlock(&chip->mutex);
    return err;
}

/**
 * audiobox_playback_pointer() - Hardware position read callback
 * @substream: Target substream.
 *
 * Return: Current hardware execution position in frames.
 */
static snd_pcm_uframes_t audiobox_playback_pointer(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    unsigned long flags;
    snd_pcm_uframes_t pos;
    u64 last_completed;

    if (unlikely(!atomic_read(&chip->playback_active)))
        return 0;

    spin_lock_irqsave(&chip->playback_lock, flags);
    pos = chip->driver_playback_pos;
    last_completed = chip->last_urb_completed_time;
    spin_unlock_irqrestore(&chip->playback_lock, flags);

    if (likely(runtime)) {
        u64 now_ns = ktime_get_ns();
        u64 elapsed_ns = (now_ns > last_completed) ? (now_ns - last_completed) : 0;
        u64 est_frames = (elapsed_ns * chip->frames_per_ns_q32) >> 32;

        u64 max_est = (chip->playback_urb_packets * runtime->rate) / 8000;
        if (est_frames > max_est)
            est_frames = max_est;

        pos += est_frames;
        if (pos >= runtime->buffer_size)
            pos -= runtime->buffer_size;

        if (chip->last_playback_hw_ptr <= runtime->buffer_size) {
            if (pos < chip->last_playback_hw_ptr &&
                (chip->last_playback_hw_ptr - pos) < (runtime->buffer_size / 4)) {
                pos = chip->last_playback_hw_ptr;
                }
        }
        chip->last_playback_hw_ptr = pos;
    }

    return pos;
}

/**
 * audiobox_playback_trigger() - State transition callback for playback
 * @substream: Target substream.
 * @cmd: ALSA operation command.
 *
 * Return: Zero on success, negative error code on failure.
 */
static int audiobox_playback_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    int i, ret = 0;
    unsigned long flags;

    switch (cmd) {
        case SNDRV_PCM_TRIGGER_START:
        case SNDRV_PCM_TRIGGER_RESUME:
        case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
            spin_lock_irqsave(&chip->playback_lock, flags);
            if (atomic_read(&chip->playback_active)) {
                spin_unlock_irqrestore(&chip->playback_lock, flags);
                return 0;
            }
            atomic_set(&chip->playback_active, 1);
            chip->playback_frames_submitted = 0;
            chip->playback_frames_consumed = 0;
            for (i = 0; i < chip->num_playback_urbs; i++)
                chip->playback_urb_frames[i] = 0;
        chip->last_urb_completed_time = ktime_get_ns();
        chip->feedback_synced = false;
        chip->playback_xrun_count = 0;
        chip->playback_completion_count = 0;
        chip->playback_jitter_max_ns = 0;
        chip->playback_jitter_sum_ns = 0;
        spin_unlock_irqrestore(&chip->playback_lock, flags);

        for (i = 0; i < FEEDBACK_URB_COUNT; i++) {
            if (!chip->feedback_urbs[i]) continue;
            usb_anchor_urb(chip->feedback_urbs[i], &chip->feedback_anchor);
            if (usb_submit_urb(chip->feedback_urbs[i], GFP_ATOMIC) < 0) {
                usb_unanchor_urb(chip->feedback_urbs[i]);
                ret = -EIO;
                goto error;
            }
        }

        for (i = 0; i < chip->num_playback_urbs; i++) {
            if (!chip->playback_urbs[i]) continue;
            usb_anchor_urb(chip->playback_urbs[i], &chip->playback_anchor);
            if (usb_submit_urb(chip->playback_urbs[i], GFP_ATOMIC) < 0) {
                usb_unanchor_urb(chip->playback_urbs[i]);
                ret = -EIO;
                goto error;
            }
            atomic_inc(&chip->active_playback_urbs);
        }
        break;

        case SNDRV_PCM_TRIGGER_STOP:
        case SNDRV_PCM_TRIGGER_SUSPEND:
        case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
            atomic_set(&chip->playback_active, 0);
            for (i = 0; i < chip->num_playback_urbs; i++) {
                if (chip->playback_urbs && chip->playback_urbs[i])
                    usb_unlink_urb(chip->playback_urbs[i]);
            }
            for (i = 0; i < FEEDBACK_URB_COUNT; i++) {
                if (chip->feedback_urbs[i])
                    usb_unlink_urb(chip->feedback_urbs[i]);
            }
            break;

        default:
            return -EINVAL;
    }
    return 0;

    error:
    atomic_set(&chip->playback_active, 0);
    usb_kill_anchored_urbs(&chip->playback_anchor);
    usb_kill_anchored_urbs(&chip->feedback_anchor);
    return ret;
}

/**
 * audiobox_capture_open() - Open callback for ALSA capture PCM
 * @substream: Target substream.
 *
 * Return: Always zero.
 */
static int audiobox_capture_open(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    atomic_set(&chip->capture_active, 0);
    chip->capture_substream = substream;
    return 0;
}

/**
 * audiobox_capture_close() - Close callback for ALSA capture PCM
 * @substream: Target substream.
 *
 * Return: Always zero.
 */
static int audiobox_capture_close(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);

    atomic_set(&chip->capture_active, 0);
    usb_kill_anchored_urbs(&chip->capture_anchor);

    usb_set_interface(chip->dev, AUDIOBOX_RECORD_INTERFACE, 0);
    chip->capture_substream = NULL;
    return 0;
}

/**
 * audiobox_capture_hw_params() - Capture dynamic URB allocation
 * @substream: Target substream.
 * @params: Requested hardware parameters.
 *
 * Return: Zero on success, negative error code on failure.
 */
static int audiobox_capture_hw_params(struct snd_pcm_substream *substream,
                                      struct snd_pcm_hw_params *params)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    unsigned int rate = params_rate(params);
    snd_pcm_uframes_t period_size = params_period_size(params);
    snd_pcm_uframes_t buffer_size = params_buffer_size(params);
    unsigned long flags;
    int err = 0;

    mutex_lock(&chip->mutex);

    spin_lock_irqsave(&chip->capture_lock, flags);
    if (atomic_read(&chip->capture_active)) {
        spin_unlock_irqrestore(&chip->capture_lock, flags);
        err = -EBUSY;
        goto unlock;
    }
    spin_unlock_irqrestore(&chip->capture_lock, flags);

    usb_kill_anchored_urbs(&chip->capture_anchor);
    audiobox_free_capture_urbs(chip);

    audiobox_calc_urb_config(rate, period_size, buffer_size,
                             &chip->num_capture_urbs, &chip->capture_urb_packets);

    if (chip->current_rate != rate) {
        if (atomic_read(&chip->playback_active)) {
            dev_err(&chip->dev->dev, "Cannot change rate while playback is active\n");
            err = -EBUSY;
            goto unlock;
        }
        err = audiobox96_set_rate(chip, rate);
        if (err < 0)
            goto unlock;
        chip->current_rate = rate;
    }

    err = audiobox_alloc_capture_urbs(chip);
    if (err < 0)
        goto unlock;

    unlock:
    mutex_unlock(&chip->mutex);
    return err;
}

/**
 * audiobox_capture_hw_free() - Hardware parameter release callback for capture
 * @substream: Target substream.
 *
 * Return: Always zero.
 */
static int audiobox_capture_hw_free(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    mutex_lock(&chip->mutex);
    audiobox_free_capture_urbs(chip);
    mutex_unlock(&chip->mutex);
    return 0;
}

/**
 * audiobox_capture_prepare() - Pre-stream configuration callback for capture
 * @substream: Target substream.
 *
 * Return: Zero on success, negative error code on failure.
 */
static int audiobox_capture_prepare(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    int i, u, err = 0;

    mutex_lock(&chip->mutex);

    usb_kill_anchored_urbs(&chip->capture_anchor);

    err = usb_set_interface(chip->dev, AUDIOBOX_RECORD_INTERFACE, 1);
    if (err < 0) {
        dev_err(&chip->dev->dev, "Failed to set record altsetting 1: %d\n", err);
        goto unlock;
    }

    chip->driver_capture_pos = 0;
    chip->last_capture_hw_ptr = 0;
    chip->capture_frames_consumed = 0;
    chip->capture_period_elapsed_accum = 0;
    chip->capture_xrun_count = 0;

    chip->frames_per_ns_q32 = div_u64((u64)runtime->rate << 32, 1000000000ULL);

    for (u = 0; u < chip->num_capture_urbs; u++) {
        struct urb *urb = chip->capture_urbs[u];
        if (!urb) continue;
        urb->number_of_packets = chip->capture_urb_packets;
        for (i = 0; i < chip->capture_urb_packets; i++) {
            urb->iso_frame_desc[i].offset = i * (MAX_FRAMES_PER_PACKET * CAPTURE_FRAME_SIZE);
            urb->iso_frame_desc[i].length = MAX_FRAMES_PER_PACKET * CAPTURE_FRAME_SIZE;
            urb->iso_frame_desc[i].status = 0;
            urb->iso_frame_desc[i].actual_length = 0;
        }
        urb->transfer_buffer_length = chip->capture_alloc_size;
        memset(urb->transfer_buffer, 0, chip->capture_alloc_size);
    }

    unlock:
    mutex_unlock(&chip->mutex);
    return err;
}

/**
 * audiobox_capture_pointer() - Hardware position read callback
 * @substream: Target substream.
 *
 * Return: Current hardware write position in frames.
 */
static snd_pcm_uframes_t audiobox_capture_pointer(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    unsigned long flags;
    snd_pcm_uframes_t pos;
    u64 last_completed;

    if (unlikely(!atomic_read(&chip->capture_active)))
        return 0;

    spin_lock_irqsave(&chip->capture_lock, flags);
    pos = (snd_pcm_uframes_t)(chip->capture_frames_consumed % runtime->buffer_size);
    last_completed = chip->last_capture_urb_completed_time;
    spin_unlock_irqrestore(&chip->capture_lock, flags);

    if (likely(runtime)) {
        u64 now_ns = ktime_get_ns();
        u64 elapsed_ns = (now_ns > last_completed) ? (now_ns - last_completed) : 0;
        u64 est_frames = (elapsed_ns * chip->frames_per_ns_q32) >> 32;

        u64 max_est = (chip->capture_urb_packets * runtime->rate) / 8000;
        if (est_frames > max_est)
            est_frames = max_est;

        pos += est_frames;
        if (pos >= runtime->buffer_size)
            pos -= runtime->buffer_size;
    }

    if (chip->last_capture_hw_ptr <= runtime->buffer_size) {
        if (pos < chip->last_capture_hw_ptr &&
            (chip->last_capture_hw_ptr - pos) < (runtime->buffer_size / 4)) {
            pos = chip->last_capture_hw_ptr;
            }
    }
    chip->last_capture_hw_ptr = pos;

    return pos;
}

/**
 * audiobox_capture_trigger() - State transition callback for capture
 * @substream: Target substream.
 * @cmd: ALSA operation command.
 *
 * Return: Zero on success, negative error code on failure.
 */
static int audiobox_capture_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    int i, ret = 0;
    unsigned long flags;

    switch (cmd) {
        case SNDRV_PCM_TRIGGER_START:
        case SNDRV_PCM_TRIGGER_RESUME:
        case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
            spin_lock_irqsave(&chip->capture_lock, flags);
            if (atomic_read(&chip->capture_active)) {
                spin_unlock_irqrestore(&chip->capture_lock, flags);
                return 0;
            }
            atomic_set(&chip->capture_active, 1);
            chip->capture_xrun_count = 0;
            chip->last_capture_urb_completed_time = ktime_get_ns();
            spin_unlock_irqrestore(&chip->capture_lock, flags);

            for (i = 0; i < chip->num_capture_urbs; i++) {
                if (!chip->capture_urbs[i]) continue;
                usb_anchor_urb(chip->capture_urbs[i], &chip->capture_anchor);
                if (usb_submit_urb(chip->capture_urbs[i], GFP_ATOMIC) < 0) {
                    usb_unanchor_urb(chip->capture_urbs[i]);
                    ret = -EIO;
                    goto error;
                }
            }
            break;

        case SNDRV_PCM_TRIGGER_STOP:
        case SNDRV_PCM_TRIGGER_SUSPEND:
        case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
            atomic_set(&chip->capture_active, 0);
            for (i = 0; i < chip->num_capture_urbs; i++) {
                if (chip->capture_urbs && chip->capture_urbs[i])
                    usb_unlink_urb(chip->capture_urbs[i]);
            }
            break;

        default:
            return -EINVAL;
    }
    return 0;

    error:
    atomic_set(&chip->capture_active, 0);
    usb_kill_anchored_urbs(&chip->capture_anchor);
    return ret;
}

static const struct snd_pcm_ops audiobox_playback_ops = {
    .open = audiobox_playback_open,
    .close = audiobox_playback_close,
    .ioctl = snd_pcm_lib_ioctl,
    .hw_params = audiobox_playback_hw_params,
    .hw_free = audiobox_playback_hw_free,
    .prepare = audiobox_playback_prepare,
    .trigger = audiobox_playback_trigger,
    .pointer = audiobox_playback_pointer,
};

static const struct snd_pcm_ops audiobox_capture_ops = {
    .open = audiobox_capture_open,
    .close = audiobox_capture_close,
    .ioctl = snd_pcm_lib_ioctl,
    .hw_params = audiobox_capture_hw_params,
    .hw_free = audiobox_capture_hw_free,
    .prepare = audiobox_capture_prepare,
    .trigger = audiobox_capture_trigger,
    .pointer = audiobox_capture_pointer,
};

/**
 * audiobox_card_private_free() - ALSA destruction callback
 * @card: ALSA card instance.
 */
static void audiobox_card_private_free(struct snd_card *card)
{
    struct audiobox_card *chip = card->private_data;
    if (chip) {
        audiobox_free_urbs(chip);
        audiobox_free_capture_urbs(chip);
        if (chip->dev) {
            struct usb_interface *play_iface = usb_ifnum_to_if(chip->dev, AUDIOBOX_PLAYBACK_INTERFACE);
            if (play_iface)
                usb_driver_release_interface(&audiobox_alsa_driver, play_iface);

            struct usb_interface *rec_iface = usb_ifnum_to_if(chip->dev, AUDIOBOX_RECORD_INTERFACE);
            if (rec_iface)
                usb_driver_release_interface(&audiobox_alsa_driver, rec_iface);

            usb_put_dev(chip->dev);
            chip->dev = NULL;
        }
    }
}

/**
 * snd_audiobox96_proc_read() - ALSA Proc Interface Read Callback
 * @entry: Proc entry info.
 * @buffer: Target proc print buffer.
 */
static void snd_audiobox96_proc_read(struct snd_info_entry *entry, struct snd_info_buffer *buffer)
{
    struct audiobox_card *chip = entry->private_data;
    unsigned long flags;

    u64 playback_submitted, playback_consumed;
    u32 freq_q16;
    u64 freq_est_q32;
    bool feedback_synced;
    int freqshift;
    u64 jitter_max_ns, jitter_avg_ns = 0;
    u64 completion_count;

    u64 capture_consumed;
    u32 cap_xrun_count;

    int cur_rate = chip->current_rate;
    int pb_active = atomic_read(&chip->playback_active);
    int cap_active = atomic_read(&chip->capture_active);

    spin_lock_irqsave(&chip->playback_lock, flags);
    playback_submitted = chip->playback_frames_submitted;
    playback_consumed = chip->playback_frames_consumed;
    freq_q16 = chip->freq_q16;
    freq_est_q32 = chip->freq_est_q32;
    feedback_synced = chip->feedback_synced;
    freqshift = chip->freqshift;
    jitter_max_ns = chip->playback_jitter_max_ns;
    completion_count = chip->playback_completion_count;
    if (completion_count > 0)
        jitter_avg_ns = div64_u64(chip->playback_jitter_sum_ns, completion_count);
    spin_unlock_irqrestore(&chip->playback_lock, flags);

    spin_lock_irqsave(&chip->capture_lock, flags);
    capture_consumed = chip->capture_frames_consumed;
    cap_xrun_count = chip->capture_xrun_count;
    spin_unlock_irqrestore(&chip->capture_lock, flags);

    snd_iprintf(buffer, "Driver_Name: %s\n", DRIVER_NAME);
    snd_iprintf(buffer, "Current_Rate: %d\n", cur_rate);
    snd_iprintf(buffer, "Playback_Active: %d\n", pb_active);
    snd_iprintf(buffer, "Capture_Active: %d\n", cap_active);

    if (pb_active && chip->playback_substream && chip->playback_substream->runtime) {
        struct snd_pcm_runtime *runtime = chip->playback_substream->runtime;
        u64 in_flight = (playback_submitted >= playback_consumed) ?
        (playback_submitted - playback_consumed) :
        ((playback_submitted + runtime->boundary) - playback_consumed);

        snd_pcm_uframes_t alsa_unplayed;
        if (runtime->control->appl_ptr >= runtime->status->hw_ptr)
            alsa_unplayed = runtime->control->appl_ptr - runtime->status->hw_ptr;
        else
            alsa_unplayed = (runtime->control->appl_ptr + runtime->boundary) - runtime->status->hw_ptr;

        snd_iprintf(buffer, "Playback_Buffer_Size: %lu\n", (unsigned long)runtime->buffer_size);
        snd_iprintf(buffer, "Playback_Period_Size: %lu\n", (unsigned long)runtime->period_size);
        snd_iprintf(buffer, "Playback_In_Flight: %llu\n", (unsigned long long)in_flight);
        snd_iprintf(buffer, "Playback_ALSA_Unplayed: %lu\n", (unsigned long)alsa_unplayed);
        snd_iprintf(buffer, "Playback_Pos: %lu\n", (unsigned long)chip->driver_playback_pos);
    } else {
        snd_iprintf(buffer, "Playback_Buffer_Size: 0\n");
        snd_iprintf(buffer, "Playback_Period_Size: 0\n");
        snd_iprintf(buffer, "Playback_In_Flight: 0\n");
        snd_iprintf(buffer, "Playback_ALSA_Unplayed: 0\n");
        snd_iprintf(buffer, "Playback_Pos: 0\n");
    }

    if (cap_active && chip->capture_substream && chip->capture_substream->runtime) {
        struct snd_pcm_runtime *runtime = chip->capture_substream->runtime;
        u64 unread;
        if (capture_consumed >= runtime->control->appl_ptr) {
            unread = capture_consumed - runtime->control->appl_ptr;
        } else {
            unread = (capture_consumed + runtime->boundary) - runtime->control->appl_ptr;
        }
        snd_iprintf(buffer, "Capture_Buffer_Size: %lu\n", (unsigned long)runtime->buffer_size);
        snd_iprintf(buffer, "Capture_Period_Size: %lu\n", (unsigned long)runtime->period_size);
        snd_iprintf(buffer, "Capture_Unread: %llu\n", (unsigned long long)unread);
        snd_iprintf(buffer, "Capture_Pos: %lu\n", (unsigned long)(capture_consumed % runtime->buffer_size));
    } else {
        snd_iprintf(buffer, "Capture_Buffer_Size: 0\n");
        snd_iprintf(buffer, "Capture_Period_Size: 0\n");
        snd_iprintf(buffer, "Capture_Unread: 0\n");
        snd_iprintf(buffer, "Capture_Pos: 0\n");
    }

    snd_iprintf(buffer, "Feedback_Synced: %d\n", feedback_synced ? 1 : 0);
    snd_iprintf(buffer, "Feedback_Val_Q32: %llu\n", (unsigned long long)freq_est_q32);
    snd_iprintf(buffer, "Freq_Q16: %u\n", freq_q16);
    snd_iprintf(buffer, "Freq_Shift: %d\n", freqshift);
    snd_iprintf(buffer, "Playback_XRUNS: %u\n", chip->playback_xrun_count);
    snd_iprintf(buffer, "Capture_XRUNS: %u\n", cap_xrun_count);
    snd_iprintf(buffer, "Playback_Completion_Count: %llu\n", (unsigned long long)completion_count);
    snd_iprintf(buffer, "Playback_Jitter_Max_NS: %llu\n", (unsigned long long)jitter_max_ns);
    snd_iprintf(buffer, "Playback_Jitter_Avg_NS: %llu\n", (unsigned long long)jitter_avg_ns);
    snd_iprintf(buffer, "Playback_URB_Count: %d\n", chip->num_playback_urbs);
    snd_iprintf(buffer, "Playback_Packets_Per_URB: %d\n", chip->playback_urb_packets);
}

/**
 * audiobox_probe() - USB Device Probe Routine
 * @intf: Interface attached.
 * @usb_id: Matched device ID string.
 *
 * Return: Zero on success, negative error code on failure.
 */
static int audiobox_probe(struct usb_interface *intf, const struct usb_device_id *usb_id)
{
    struct usb_device *dev = interface_to_usbdev(intf);
    struct snd_card *card;
    struct audiobox_card *chip;
    struct usb_interface *play_iface;
    struct usb_interface *rec_iface;
    int err, idx;

    if (intf->cur_altsetting->desc.bInterfaceNumber != AUDIOBOX_CONTROL_INTERFACE)
        return -ENODEV;

    idx = atomic_fetch_inc(&dev_idx);
    if (idx >= SNDRV_CARDS || !enable[idx]) {
        atomic_dec(&dev_idx);
        return -ENODEV;
    }

    err = snd_card_new(&dev->dev, index[idx], id[idx], THIS_MODULE,
                       sizeof(struct audiobox_card), &card);
    if (err < 0) {
        atomic_dec(&dev_idx);
        return err;
    }

    chip = card->private_data;
    card->private_free = audiobox_card_private_free;
    chip->dev = usb_get_dev(dev);
    chip->card = card;

    chip->num_playback_urbs = PLAYBACK_URB_COUNT;
    chip->playback_urb_packets = PLAYBACK_PACKET_COUNT;
    atomic_set(&chip->active_playback_urbs, 0);

    chip->num_capture_urbs = CAPTURE_URB_COUNT;
    chip->capture_urb_packets = CAPTURE_PACKET_COUNT;
    atomic_set(&chip->capture_active, 0);

    spin_lock_init(&chip->playback_lock);
    spin_lock_init(&chip->capture_lock);
    mutex_init(&chip->mutex);
    init_usb_anchor(&chip->playback_anchor);
    init_usb_anchor(&chip->feedback_anchor);
    init_usb_anchor(&chip->capture_anchor);

    strscpy(card->driver, DRIVER_NAME, sizeof(card->driver));
    strscpy(card->shortname, "AudioBox USB 96", sizeof(card->shortname));
    snprintf(card->longname, sizeof(card->longname), "%s at %s",
             card->shortname, dev_name(&dev->dev));

    play_iface = usb_ifnum_to_if(dev, AUDIOBOX_PLAYBACK_INTERFACE);
    if (!play_iface) {
        dev_err(&dev->dev, "Playback interface %d not found\n", AUDIOBOX_PLAYBACK_INTERFACE);
        err = -ENODEV;
        goto free_card;
    }

    err = usb_driver_claim_interface(&audiobox_alsa_driver, play_iface, chip);
    if (err < 0) {
        dev_err(&dev->dev, "Could not claim playback interface: %d\n", err);
        goto free_card;
    }

    rec_iface = usb_ifnum_to_if(dev, AUDIOBOX_RECORD_INTERFACE);
    if (!rec_iface) {
        dev_err(&dev->dev, "Recording interface %d not found\n", AUDIOBOX_RECORD_INTERFACE);
        err = -ENODEV;
        goto free_card;
    }

    err = usb_driver_claim_interface(&audiobox_alsa_driver, rec_iface, chip);
    if (err < 0) {
        dev_err(&dev->dev, "Could not claim recording interface: %d\n", err);
        goto free_card;
    }

    err = snd_pcm_new(card, "AudioBox Playback/Capture", 0, 1, 1, &chip->pcm);
    if (err < 0)
        goto free_card;

    chip->pcm->private_data = chip;
    strscpy(chip->pcm->name, "AudioBox Playback/Capture", sizeof(chip->pcm->name));

    snd_pcm_set_ops(chip->pcm, SNDRV_PCM_STREAM_PLAYBACK, &audiobox_playback_ops);
    snd_pcm_set_ops(chip->pcm, SNDRV_PCM_STREAM_CAPTURE, &audiobox_capture_ops);
    snd_pcm_set_managed_buffer_all(chip->pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

    usb_set_interface(dev, AUDIOBOX_CONTROL_INTERFACE, 0);
    usb_set_interface(dev, AUDIOBOX_RECORD_INTERFACE, 0);
    usb_set_interface(dev, AUDIOBOX_PLAYBACK_INTERFACE, 0);

    if (audiobox96_set_rate(chip, 48000) < 0)
        dev_warn(&dev->dev, "Could not initialize device clock to 48kHz\n");
    else
        chip->current_rate = 48000;

    err = snd_card_ro_proc_new(card, "audiobox96_stats", chip, snd_audiobox96_proc_read);
    if (err < 0)
        dev_warn(&dev->dev, "Failed to create proc file entry: %d\n", err);

    err = snd_card_register(card);
    if (err < 0)
        goto free_card;

    usb_set_intfdata(intf, chip);
    return 0;

    free_card:
    snd_card_free(card);
    atomic_dec(&dev_idx);
    return err;
}

/**
 * audiobox_disconnect() - USB Device Disconnect Callback
 * @intf: Interface being removed.
 */
static void audiobox_disconnect(struct usb_interface *intf)
{
    struct audiobox_card *chip = usb_get_intfdata(intf);

    if (!chip)
        return;

    if (intf->cur_altsetting->desc.bInterfaceNumber == AUDIOBOX_CONTROL_INTERFACE) {
        atomic_set(&chip->playback_active, 0);
        atomic_set(&chip->capture_active, 0);

        usb_kill_anchored_urbs(&chip->playback_anchor);
        usb_kill_anchored_urbs(&chip->feedback_anchor);
        usb_kill_anchored_urbs(&chip->capture_anchor);

        snd_card_disconnect(chip->card);
        usb_set_intfdata(intf, NULL);

        snd_card_free_when_closed(chip->card);
        atomic_dec(&dev_idx);
    }
}

/**
 * audiobox_suspend() - USB Device Suspend Callback
 * @intf: Interface attached.
 * @message: Power state message.
 *
 * Return: Always zero.
 */
static int audiobox_suspend(struct usb_interface *intf, pm_message_t message)
{
    struct audiobox_card *chip = usb_get_intfdata(intf);
    if (!chip)
        return 0;

    snd_power_change_state(chip->card, SNDRV_CTL_POWER_D3hot);
    return 0;
}

/**
 * audiobox_resume() - USB Device Resume Callback
 * @intf: Interface attached.
 *
 * Return: Always zero.
 */
static int audiobox_resume(struct usb_interface *intf)
{
    struct audiobox_card *chip = usb_get_intfdata(intf);
    if (!chip)
        return 0;

    if (chip->current_rate > 0) {
        audiobox96_set_rate(chip, chip->current_rate);
    }

    snd_power_change_state(chip->card, SNDRV_CTL_POWER_D0);
    return 0;
}

static const struct usb_device_id audiobox_usb_ids[] = {
    { USB_DEVICE(USB_VID_PRESONUS, USB_PID_AUDIOBOX96) },
    { }
};
MODULE_DEVICE_TABLE(usb, audiobox_usb_ids);

static struct usb_driver audiobox_alsa_driver = {
    .name = DRIVER_NAME,
    .probe = audiobox_probe,
    .disconnect = audiobox_disconnect,
    .suspend = audiobox_suspend,
    .resume = audiobox_resume,
    .reset_resume = audiobox_resume,
    .id_table = audiobox_usb_ids,
};

module_usb_driver(audiobox_alsa_driver);
