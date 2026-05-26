#include <linux/module.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/minmax.h>
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
    SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
    .formats = SNDRV_PCM_FMTBIT_S32_LE,
    .rates = (SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
    SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000),
    .rate_min = 44100,
    .rate_max = 96000,
    .channels_min = NUM_CHANNELS,
    .channels_max = NUM_CHANNELS,
    .buffer_bytes_max = 512 * 1024,
    .period_bytes_min = 128,
    .period_bytes_max = 256 * 1024,
    .periods_min = 2,
    .periods_max = 1024,
};

static const struct snd_pcm_hardware audiobox_capture_hw = {
    .info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
    SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID |
    SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
    .formats = SNDRV_PCM_FMTBIT_S32_LE,
    .rates = (SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
    SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000),
    .rate_min = 44100,
    .rate_max = 96000,
    .channels_min = NUM_CHANNELS,
    .channels_max = NUM_CHANNELS,
    .buffer_bytes_max = 512 * 1024,
    .period_bytes_min = 128,
    .period_bytes_max = 256 * 1024,
    .periods_min = 2,
    .periods_max = 1024,
};

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

static void capture_urb_complete(struct urb *urb)
{
    struct audiobox_card *chip = urb->context;
    struct snd_pcm_substream *substream;
    struct snd_pcm_runtime *runtime;
    int i;
    unsigned long flags;
    bool need_period_elapsed = false;

    if (urb->status) {
        if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
            urb->status == -ESHUTDOWN || !chip) {
            goto exit_clear;
            }
    }

    if (!atomic_read(&chip->capture_active))
        goto exit_clear;

    substream = chip->capture_substream;
    if (!substream || !substream->runtime)
        goto exit_clear;

    runtime = substream->runtime;

    spin_lock_irqsave(&chip->lock, flags);

    for (i = 0; i < urb->number_of_packets; i++) {
        int status = urb->iso_frame_desc[i].status;
        unsigned int actual_len = urb->iso_frame_desc[i].actual_length;
        unsigned int offset = urb->iso_frame_desc[i].offset;

        if (status == 0 && actual_len >= CAPTURE_FRAME_SIZE) {
            u8 *src = (u8 *)urb->transfer_buffer + offset;
            snd_pcm_uframes_t frames = actual_len / CAPTURE_FRAME_SIZE;
            size_t byte_pos = frames_to_bytes(runtime, chip->driver_capture_pos);

            // Copy incoming samples to ALSA DMA ring buffer
            if (chip->driver_capture_pos + frames > runtime->buffer_size) {
                size_t part1 = runtime->buffer_size - chip->driver_capture_pos;
                size_t part1_bytes = frames_to_bytes(runtime, part1);
                size_t part2_bytes = actual_len - part1_bytes;

                memcpy(runtime->dma_area + byte_pos, src, part1_bytes);
                memcpy(runtime->dma_area, src + part1_bytes, part2_bytes);
            } else {
                memcpy(runtime->dma_area + byte_pos, src, actual_len);
            }

            chip->driver_capture_pos += frames;
            if (chip->driver_capture_pos >= runtime->buffer_size)
                chip->driver_capture_pos -= runtime->buffer_size;

            chip->capture_frames_consumed += frames;
            chip->capture_period_elapsed_accum += frames;
        }
    }

    while (chip->capture_period_elapsed_accum >= runtime->period_size) {
        chip->capture_period_elapsed_accum -= runtime->period_size;
        need_period_elapsed = true;
    }

    spin_unlock_irqrestore(&chip->lock, flags);

    if (need_period_elapsed)
        snd_pcm_period_elapsed(substream);

    for (i = 0; i < urb->number_of_packets; i++) {
        urb->iso_frame_desc[i].offset = i * (MAX_FRAMES_PER_PACKET * CAPTURE_FRAME_SIZE);
        urb->iso_frame_desc[i].length = MAX_FRAMES_PER_PACKET * CAPTURE_FRAME_SIZE;
        urb->iso_frame_desc[i].status = 0;
        urb->iso_frame_desc[i].actual_length = 0;
    }

    usb_anchor_urb(urb, &chip->capture_anchor);
    if (usb_submit_urb(urb, GFP_ATOMIC) < 0)
        goto exit_clear;

    return;

    exit_clear:
    usb_unanchor_urb(urb);
}

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
    int i;
    unsigned long flags;
    bool need_period_elapsed = false;

    if (urb->status) {
        if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
            urb->status == -ESHUTDOWN || !chip) {
            goto exit_clear;
            }
    }

    if (!atomic_read(&chip->playback_active))
        goto exit_clear;

    substream = chip->playback_substream;
    if (!substream || !substream->runtime)
        goto exit_clear;

    runtime = substream->runtime;

    spin_lock_irqsave(&chip->lock, flags);

    for (i = 0; i < urb->number_of_packets; i++) {
        unsigned int frames_for_packet;

        chip->phase_accum += chip->freq_q16;
        frames_for_packet = chip->phase_accum >> 16;
        chip->phase_accum &= 0xFFFF;

        if (frames_for_packet > MAX_FRAMES_PER_PACKET)
            frames_for_packet = MAX_FRAMES_PER_PACKET;

        urb->iso_frame_desc[i].offset = total_bytes_for_urb;
        urb->iso_frame_desc[i].length = frames_for_packet * PLAYBACK_FRAME_SIZE;
        total_bytes_for_urb += urb->iso_frame_desc[i].length;
    }
    urb->transfer_buffer_length = total_bytes_for_urb;

    frames_to_copy = bytes_to_frames(runtime, total_bytes_for_urb);
    avail_frames = snd_pcm_playback_hw_avail(runtime);
    copy_pos = chip->driver_playback_pos;

    if (avail_frames < (snd_pcm_sframes_t)frames_to_copy) {
        if (avail_frames > 0) {
            bytes_to_copy = frames_to_bytes(runtime, avail_frames);
            silence_bytes = total_bytes_for_urb - bytes_to_copy;
            chip->driver_playback_pos += avail_frames;
            chip->playback_frames_consumed += avail_frames;
            chip->period_elapsed_accum += avail_frames;
        } else {
            bytes_to_copy = 0;
            silence_bytes = total_bytes_for_urb;
        }
    } else {
        bytes_to_copy = total_bytes_for_urb;
        silence_bytes = 0;
        chip->driver_playback_pos += frames_to_copy;
        chip->playback_frames_consumed += frames_to_copy;
        chip->period_elapsed_accum += frames_to_copy;
    }

    if (chip->driver_playback_pos >= runtime->buffer_size)
        chip->driver_playback_pos -= runtime->buffer_size;

    while (chip->period_elapsed_accum >= runtime->period_size) {
        chip->period_elapsed_accum -= runtime->period_size;
        need_period_elapsed = true;
    }

    spin_unlock_irqrestore(&chip->lock, flags);

    if (total_bytes_for_urb > 0) {
        u8 *dst_buf = urb->transfer_buffer;

        if (bytes_to_copy > 0) {
            size_t ptr_bytes = frames_to_bytes(runtime, copy_pos);
            snd_pcm_uframes_t frames_copied = bytes_to_frames(runtime, bytes_to_copy);

            if (copy_pos + frames_copied > runtime->buffer_size) {
                size_t part1 = runtime->buffer_size - copy_pos;
                size_t part1_bytes = frames_to_bytes(runtime, part1);

                memcpy(dst_buf, runtime->dma_area + ptr_bytes, part1_bytes);
                memcpy(dst_buf + part1_bytes, runtime->dma_area, bytes_to_copy - part1_bytes);
            } else {
                memcpy(dst_buf, runtime->dma_area + ptr_bytes, bytes_to_copy);
            }
        }

        if (silence_bytes > 0) {
            memset(dst_buf + bytes_to_copy, 0, silence_bytes);
        }
    }

    if (need_period_elapsed)
        snd_pcm_period_elapsed(substream);

    usb_anchor_urb(urb, &chip->playback_anchor);
    if (usb_submit_urb(urb, GFP_ATOMIC) < 0)
        goto exit_clear;

    return;

    exit_clear:
    usb_unanchor_urb(urb);
    atomic_dec(&chip->active_playback_urbs);
}

static void feedback_urb_complete(struct urb *urb)
{
    struct audiobox_card *chip = urb->context;
    unsigned long flags;
    int p;

    if (urb->status) {
        if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
            urb->status == -ESHUTDOWN || !chip || !atomic_read(&chip->playback_active)) {
            usb_unanchor_urb(urb);
        return;
            }
            goto resubmit;
    }

    spin_lock_irqsave(&chip->lock, flags);

    if (chip->feedback_urb_skip_count > 0) {
        chip->feedback_urb_skip_count--;
        spin_unlock_irqrestore(&chip->lock, flags);
        goto resubmit;
    }

    for (p = 0; p < urb->number_of_packets; p++) {
        unsigned int len = urb->iso_frame_desc[p].actual_length;

        if (urb->iso_frame_desc[p].status == 0 && len >= 3) {
            u8 *data = (u8 *)urb->transfer_buffer + urb->iso_frame_desc[p].offset;
            u32 feedback_val = 0;

            if (len == 3) {
                feedback_val = (data[0] | (data[1] << 8) | (data[2] << 16));
            } else {
                feedback_val = le32_to_cpup((__le32 *)data) & 0x0fffffff;
            }

            if (feedback_val > 0) {
                u32 nominal_q16 = div_u64((u64)chip->current_rate << 16, 8000);
                u32 f_shifted = feedback_val;

                if (!chip->freqshift_detected) {
                    int shift = 0;
                    while (f_shifted < nominal_q16 - nominal_q16 / 4) {
                        f_shifted <<= 1;
                        shift++;
                    }
                    while (f_shifted > nominal_q16 + nominal_q16 / 2) {
                        f_shifted >>= 1;
                        shift--;
                    }
                    chip->freqshift = shift;
                    chip->freqshift_detected = true;
                } else {
                    if (chip->freqshift > 0)
                        f_shifted <<= chip->freqshift;
                    else if (chip->freqshift < 0)
                        f_shifted >>= -chip->freqshift;
                }

                u32 diff = (f_shifted > nominal_q16) ? (f_shifted - nominal_q16) : (nominal_q16 - f_shifted);

                if (diff < (nominal_q16 / 4)) {
                    chip->freq_q16 = (chip->freq_q16 * 7 + f_shifted) / 8;
                    chip->feedback_synced = true;
                }
            }
        }
    }
    spin_unlock_irqrestore(&chip->lock, flags);

    resubmit:
    usb_anchor_urb(urb, &chip->feedback_anchor);
    if (usb_submit_urb(urb, GFP_ATOMIC) < 0) {
        usb_unanchor_urb(urb);
    }
}

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

    for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
        if (chip->feedback_urbs[i]) {
            usb_free_coherent(chip->dev, chip->feedback_urb_alloc_size,
                              chip->feedback_urbs[i]->transfer_buffer,
                              chip->feedback_urbs[i]->transfer_dma);
            usb_free_urb(chip->feedback_urbs[i]);
            chip->feedback_urbs[i] = NULL;
        }
    }
}

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

static int audiobox_alloc_urbs(struct audiobox_card *chip)
{
    int i;

    chip->playback_urb_alloc_size = chip->playback_urb_packets * (MAX_FRAMES_PER_PACKET * PLAYBACK_FRAME_SIZE);

    chip->playback_urbs = kcalloc(chip->num_playback_urbs, sizeof(struct urb *), GFP_KERNEL);
    if (!chip->playback_urbs)
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

    chip->feedback_urb_alloc_size = FEEDBACK_URB_PACKETS * 4;
    for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
        struct urb *urb = usb_alloc_urb(FEEDBACK_URB_PACKETS, GFP_KERNEL);
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
        urb->interval = 4;
        urb->context = chip;
        urb->complete = feedback_urb_complete;
    }
    return 0;
}

static int audiobox_playback_open(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);

    substream->runtime->hw = audiobox_playback_hw;
    chip->playback_substream = substream;
    atomic_set(&chip->playback_active, 0);
    return 0;
}

static int audiobox_playback_close(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);

    atomic_set(&chip->playback_active, 0);
    usb_kill_anchored_urbs(&chip->playback_anchor);
    usb_kill_anchored_urbs(&chip->feedback_anchor);

    usb_set_interface(chip->dev, AUDIOBOX_PLAYBACK_INTERFACE, 0);
    chip->playback_substream = NULL;
    return 0;
}

static int audiobox_playback_hw_params(struct snd_pcm_substream *substream,
                                       struct snd_pcm_hw_params *params)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    unsigned int rate = params_rate(params);
    unsigned long flags;
    int err, fpp, pkts_period, pkts_buffer;

    spin_lock_irqsave(&chip->lock, flags);
    if (atomic_read(&chip->playback_active)) {
        spin_unlock_irqrestore(&chip->lock, flags);
        return -EBUSY;
    }
    spin_unlock_irqrestore(&chip->lock, flags);

    usb_kill_anchored_urbs(&chip->playback_anchor);
    usb_kill_anchored_urbs(&chip->feedback_anchor);
    atomic_set(&chip->active_playback_urbs, 0);

    audiobox_free_urbs(chip);

    fpp = rate / 8000;
    if (fpp == 0)
        fpp = 6;

    pkts_period = params_period_size(params) / fpp;
    pkts_buffer = params_buffer_size(params) / fpp;

    chip->playback_urb_packets = max(1, pkts_period / 2);
    if (chip->playback_urb_packets > 16)
        chip->playback_urb_packets = 16;

    chip->num_playback_urbs = max(2, pkts_buffer / chip->playback_urb_packets);
    if (chip->num_playback_urbs > 32)
        chip->num_playback_urbs = 32;

    if (chip->num_playback_urbs == 0 || chip->playback_urb_packets == 0) {
        chip->num_playback_urbs = 8;
        chip->playback_urb_packets = 4;
    }

    dev_info(&chip->dev->dev, "Dynamic URB config: rate=%u period=%lu buffer=%lu -> urbs=%d packets=%d\n",
             rate, (unsigned long)params_period_size(params), (unsigned long)params_buffer_size(params),
             chip->num_playback_urbs, chip->playback_urb_packets);

    if (chip->current_rate != rate) {
        err = audiobox96_set_rate(chip, rate);
        if (err < 0)
            return err;
        chip->current_rate = rate;
    }

    err = audiobox_alloc_urbs(chip);
    if (err < 0)
        return err;

    return 0;
}

static int audiobox_playback_hw_free(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    audiobox_free_urbs(chip);
    return 0;
}

static int audiobox_playback_prepare(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    int i, u, err;
    size_t nominal_bytes = (runtime->rate / 8000) * PLAYBACK_FRAME_SIZE;

    usb_kill_anchored_urbs(&chip->playback_anchor);
    usb_kill_anchored_urbs(&chip->feedback_anchor);

    err = usb_set_interface(chip->dev, AUDIOBOX_PLAYBACK_INTERFACE, 1);
    if (err < 0) {
        dev_err(&chip->dev->dev, "Failed to set altsetting 1: %d\n", err);
        return err;
    }

    chip->driver_playback_pos = 0;
    chip->playback_frames_consumed = 0;
    chip->period_elapsed_accum = 0;
    chip->feedback_synced = false;
    chip->feedback_urb_skip_count = 4;
    chip->phase_accum = 0;
    chip->freq_q16 = div_u64(((u64)runtime->rate << 16), 8000);

    chip->freqshift = 0;
    chip->freqshift_detected = false;
    atomic_set(&chip->active_playback_urbs, 0);

    for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
        struct urb *f_urb = chip->feedback_urbs[i];
        if (!f_urb) continue;
        f_urb->number_of_packets = FEEDBACK_URB_PACKETS;
        f_urb->transfer_buffer_length = FEEDBACK_URB_PACKETS * 4;
        for (u = 0; u < FEEDBACK_URB_PACKETS; u++) {
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
    return 0;
}

static snd_pcm_uframes_t audiobox_playback_pointer(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    unsigned long flags;
    u64 pos;
    int active_pb;

    if (!atomic_read(&chip->playback_active))
        return 0;

    spin_lock_irqsave(&chip->lock, flags);
    pos = chip->playback_frames_consumed;
    active_pb = atomic_read(&chip->active_playback_urbs);
    spin_unlock_irqrestore(&chip->lock, flags);

    if (runtime) {
        snd_pcm_uframes_t delay = (active_pb * chip->playback_urb_packets * runtime->rate) / 8000;
        runtime->delay = delay;
    }

    return (snd_pcm_uframes_t)(pos % runtime->buffer_size);
}

static int audiobox_playback_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    int i, ret = 0;
    unsigned long flags;

    switch (cmd) {
        case SNDRV_PCM_TRIGGER_START:
        case SNDRV_PCM_TRIGGER_RESUME:
            spin_lock_irqsave(&chip->lock, flags);
            if (atomic_read(&chip->playback_active)) {
                spin_unlock_irqrestore(&chip->lock, flags);
                return 0;
            }
            atomic_set(&chip->playback_active, 1);
            chip->feedback_synced = false;
            spin_unlock_irqrestore(&chip->lock, flags);

            for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
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
            for (i = 0; i < NUM_FEEDBACK_URBS; i++) {
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

static int audiobox_capture_open(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);

    substream->runtime->hw = audiobox_capture_hw;
    chip->capture_substream = substream;
    atomic_set(&chip->capture_active, 0);
    return 0;
}

static int audiobox_capture_close(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);

    atomic_set(&chip->capture_active, 0);
    usb_kill_anchored_urbs(&chip->capture_anchor);

    usb_set_interface(chip->dev, AUDIOBOX_RECORD_INTERFACE, 0);
    chip->capture_substream = NULL;
    return 0;
}

static int audiobox_capture_hw_params(struct snd_pcm_substream *substream,
                                      struct snd_pcm_hw_params *params)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    unsigned int rate = params_rate(params);
    unsigned long flags;
    int err, fpp, pkts_period, pkts_buffer;

    spin_lock_irqsave(&chip->lock, flags);
    if (atomic_read(&chip->capture_active)) {
        spin_unlock_irqrestore(&chip->lock, flags);
        return -EBUSY;
    }
    spin_unlock_irqrestore(&chip->lock, flags);

    usb_kill_anchored_urbs(&chip->capture_anchor);
    audiobox_free_capture_urbs(chip);

    fpp = rate / 8000;
    if (fpp == 0)
        fpp = 6;

    pkts_period = params_period_size(params) / fpp;
    pkts_buffer = params_buffer_size(params) / fpp;

    chip->capture_urb_packets = max(1, pkts_period / 2);
    if (chip->capture_urb_packets > 16)
        chip->capture_urb_packets = 16;

    chip->num_capture_urbs = max(2, pkts_buffer / chip->capture_urb_packets);
    if (chip->num_capture_urbs > 32)
        chip->num_capture_urbs = 32;

    if (chip->num_capture_urbs == 0 || chip->capture_urb_packets == 0) {
        chip->num_capture_urbs = 8;
        chip->capture_urb_packets = 4;
    }

    dev_info(&chip->dev->dev, "Dynamic Capture URB config: rate=%u period=%lu buffer=%lu -> urbs=%d packets=%d\n",
             rate, (unsigned long)params_period_size(params), (unsigned long)params_buffer_size(params),
             chip->num_capture_urbs, chip->capture_urb_packets);

    if (chip->current_rate != rate) {
        err = audiobox96_set_rate(chip, rate);
        if (err < 0)
            return err;
        chip->current_rate = rate;
    }

    err = audiobox_alloc_capture_urbs(chip);
    if (err < 0)
        return err;

    return 0;
}

static int audiobox_capture_hw_free(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    audiobox_free_capture_urbs(chip);
    return 0;
}

static int audiobox_capture_prepare(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    int i, u, err;

    usb_kill_anchored_urbs(&chip->capture_anchor);

    err = usb_set_interface(chip->dev, AUDIOBOX_RECORD_INTERFACE, 1);
    if (err < 0) {
        dev_err(&chip->dev->dev, "Failed to set record altsetting 1: %d\n", err);
        return err;
    }

    chip->driver_capture_pos = 0;
    chip->capture_frames_consumed = 0;
    chip->capture_period_elapsed_accum = 0;

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
    return 0;
}

static snd_pcm_uframes_t audiobox_capture_pointer(struct snd_pcm_substream *substream)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    unsigned long flags;
    u64 pos;

    if (!atomic_read(&chip->capture_active))
        return 0;

    spin_lock_irqsave(&chip->lock, flags);
    pos = chip->capture_frames_consumed;
    spin_unlock_irqrestore(&chip->lock, flags);

    return (snd_pcm_uframes_t)(pos % runtime->buffer_size);
}

static int audiobox_capture_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct audiobox_card *chip = snd_pcm_substream_chip(substream);
    int i, ret = 0;
    unsigned long flags;

    switch (cmd) {
        case SNDRV_PCM_TRIGGER_START:
        case SNDRV_PCM_TRIGGER_RESUME:
            spin_lock_irqsave(&chip->lock, flags);
            if (atomic_read(&chip->capture_active)) {
                spin_unlock_irqrestore(&chip->lock, flags);
                return 0;
            }
            atomic_set(&chip->capture_active, 1);
            spin_unlock_irqrestore(&chip->lock, flags);

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

static void audiobox_card_private_free(struct snd_card *card)
{
    struct audiobox_card *chip = card->private_data;
    if (chip) {
        audiobox_free_urbs(chip);
        audiobox_free_capture_urbs(chip);
        if (chip->dev) {
            struct usb_interface *play_iface = usb_ifnum_to_if(chip->dev, AUDIOBOX_PLAYBACK_INTERFACE);
            if (play_iface) {
                usb_driver_release_interface(&audiobox_alsa_driver, play_iface);
            }
            struct usb_interface *rec_iface = usb_ifnum_to_if(chip->dev, AUDIOBOX_RECORD_INTERFACE);
            if (rec_iface) {
                usb_driver_release_interface(&audiobox_alsa_driver, rec_iface);
            }
            usb_put_dev(chip->dev);
            chip->dev = NULL;
        }
    }
}

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

    chip->num_playback_urbs = 8;
    chip->playback_urb_packets = 4;
    atomic_set(&chip->active_playback_urbs, 0);

    chip->num_capture_urbs = 8;
    chip->capture_urb_packets = 4;
    atomic_set(&chip->capture_active, 0);

    spin_lock_init(&chip->lock);
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

static int audiobox_suspend(struct usb_interface *intf, pm_message_t message)
{
    struct audiobox_card *chip = usb_get_intfdata(intf);
    if (!chip)
        return 0;

    snd_power_change_state(chip->card, SNDRV_CTL_POWER_D3hot);
    return 0;
}

static int audiobox_resume(struct usb_interface *intf)
{
    struct audiobox_card *chip = usb_get_intfdata(intf);
    if (!chip)
        return 0;

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
