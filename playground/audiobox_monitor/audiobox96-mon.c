/**
 * audiobox96-mon.c - Diagnostic CLI Status Dashboard
 *
 * Reads stats from /proc/asound/cardX/audiobox96_stats and renders a
 * color-coded monitoring terminal interface.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <stdbool.h>
#include <time.h>

struct stats {
    char driver_name[64];
    int current_rate;
    int playback_active;
    int capture_active;
    unsigned long pb_buffer_size;
    unsigned long pb_period_size;
    unsigned long long pb_in_flight;
    unsigned long pb_alsa_unplayed;
    unsigned long pb_pos;
    unsigned long cap_buffer_size;
    unsigned long cap_period_size;
    unsigned long long cap_unread;
    unsigned long cap_pos;
    int feedback_synced;
    unsigned long long feedback_val_q32;
    unsigned int freq_q16;
    int freq_shift;
    unsigned int pb_xruns;
    unsigned int cap_xruns;
    unsigned long long pb_completion_count;
    unsigned long long jitter_max_ns;
    unsigned long long jitter_avg_ns;
    int pb_urb_count;
    int pb_packets_per_urb;
};

static char *find_stats_file(void) {
    static char path[512];
    DIR *dir = opendir("/proc/asound");
    if (!dir) return NULL;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "card", 4) == 0) {
            snprintf(path, sizeof(path), "/proc/asound/%s/audiobox96_stats", entry->d_name);
            if (access(path, F_OK) == 0) {
                closedir(dir);
                return path;
            }
        }
    }
    closedir(dir);
    return NULL;
}

static bool parse_stats(const char *filepath, struct stats *s) {
    FILE *f = fopen(filepath, "r");
    if (!f) return false;

    char line[256];
    memset(s, 0, sizeof(*s));

    while (fgets(line, sizeof(line), f)) {
        char key[64], val[128];
        if (sscanf(line, "%63[^:]: %127s", key, val) == 2) {
            if (strcmp(key, "Driver_Name") == 0) snprintf(s->driver_name, sizeof(s->driver_name), "%.63s", val);
            else if (strcmp(key, "Current_Rate") == 0) s->current_rate = atoi(val);
            else if (strcmp(key, "Playback_Active") == 0) s->playback_active = atoi(val);
            else if (strcmp(key, "Capture_Active") == 0) s->capture_active = atoi(val);
            else if (strcmp(key, "Playback_Buffer_Size") == 0) s->pb_buffer_size = strtoul(val, NULL, 10);
            else if (strcmp(key, "Playback_Period_Size") == 0) s->pb_period_size = strtoul(val, NULL, 10);
            else if (strcmp(key, "Playback_In_Flight") == 0) s->pb_in_flight = strtoull(val, NULL, 10);
            else if (strcmp(key, "Playback_ALSA_Unplayed") == 0) s->pb_alsa_unplayed = strtoul(val, NULL, 10);
            else if (strcmp(key, "Playback_Pos") == 0) s->pb_pos = strtoul(val, NULL, 10);
            else if (strcmp(key, "Capture_Buffer_Size") == 0) s->cap_buffer_size = strtoul(val, NULL, 10);
            else if (strcmp(key, "Capture_Period_Size") == 0) s->cap_period_size = strtoul(val, NULL, 10);
            else if (strcmp(key, "Capture_Unread") == 0) s->cap_unread = strtoull(val, NULL, 10);
            else if (strcmp(key, "Capture_Pos") == 0) s->cap_pos = strtoul(val, NULL, 10);
            else if (strcmp(key, "Feedback_Synced") == 0) s->feedback_synced = atoi(val);
            else if (strcmp(key, "Feedback_Val_Q32") == 0) s->feedback_val_q32 = strtoull(val, NULL, 10);
            else if (strcmp(key, "Freq_Q16") == 0) s->freq_q16 = strtoul(val, NULL, 10);
            else if (strcmp(key, "Freq_Shift") == 0) s->freq_shift = atoi(val);
            else if (strcmp(key, "Playback_XRUNS") == 0) s->pb_xruns = atoi(val);
            else if (strcmp(key, "Capture_XRUNS") == 0) s->cap_xruns = atoi(val);
            else if (strcmp(key, "Playback_Completion_Count") == 0) s->pb_completion_count = strtoull(val, NULL, 10);
            else if (strcmp(key, "Playback_Jitter_Max_NS") == 0) s->jitter_max_ns = strtoull(val, NULL, 10);
            else if (strcmp(key, "Playback_Jitter_Avg_NS") == 0) s->jitter_avg_ns = strtoull(val, NULL, 10);
            else if (strcmp(key, "Playback_URB_Count") == 0) s->pb_urb_count = atoi(val);
            else if (strcmp(key, "Playback_Packets_Per_URB") == 0) s->pb_packets_per_urb = atoi(val);
        }
    }
    fclose(f);
    return true;
}

static void get_bar(double ratio, int width, char *out) {
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    int filled = (int)(ratio * width);
    char *ptr = out;
    for (int i = 0; i < width; i++) {
        if (i < filled) {
            strcpy(ptr, "█");
            ptr += strlen("█");
        } else {
            strcpy(ptr, "░");
            ptr += strlen("░");
        }
    }
    *ptr = '\0';
}

static void render_dashboard(const struct stats *s) {
    // Clear screen from home to remove ghost characters
    printf("\033[H");

    /* 1. Software ALSA Buffer Occupancy Ratio */
    double pb_alsa_ratio = 0.0;
    if (s->pb_buffer_size > 0)
        pb_alsa_ratio = (double)s->pb_alsa_unplayed / s->pb_buffer_size;

    /* 2. Hardware USB Pipeline Saturated Ratio */
    int frames_per_packet = s->current_rate / 8000;
    int max_pipeline_frames = s->pb_urb_count * s->pb_packets_per_urb * frames_per_packet;
    double usb_pipeline_ratio = 0.0;
    if (max_pipeline_frames > 0)
        usb_pipeline_ratio = (double)s->pb_in_flight / max_pipeline_frames;

    /* 3. Capture Buffer Load */
    double cap_ratio = 0.0;
    if (s->cap_buffer_size > 0)
        cap_ratio = (double)s->cap_unread / s->cap_buffer_size;

    char pb_alsa_bar[128], usb_pipe_bar[128], cap_bar[128];
    get_bar(pb_alsa_ratio, 30, pb_alsa_bar);
    get_bar(usb_pipeline_ratio, 30, usb_pipe_bar);
    get_bar(cap_ratio, 30, cap_bar);

    /* Color Rules for Software ALSA Buffer Fill (Warning if empty) */
    const char *pb_alsa_color = "\033[1;32m"; // Green
    if (pb_alsa_ratio < 0.20) {
        pb_alsa_color = "\033[1;5;31m"; // Flashing Red
    } else if (pb_alsa_ratio < 0.45) {
        pb_alsa_color = "\033[1;33m"; // Yellow
    }

    /* Color Rules for Hardware USB Pipeline Fill (Warning if starved) */
    const char *usb_pipe_color = "\033[1;32m"; // Green
    if (usb_pipeline_ratio < 0.90) {
        usb_pipe_color = "\033[1;31m"; // Solid Red
    } else if (usb_pipeline_ratio < 0.98) {
        usb_pipe_color = "\033[1;33m"; // Yellow
    }

    /* Color Rules for Capture Buffer Fill (Warning if full) */
    const char *cap_color = "\033[1;32m"; // Green
    if (cap_ratio > 0.85) {
        cap_color = "\033[1;5;31m"; // Flashing Red
    } else if (cap_ratio > 0.60) {
        cap_color = "\033[1;33m"; // Yellow
    }

    double actual_rate = ((double)s->feedback_val_q32 * 8000.0) / 4294967296.0;
    double drift = actual_rate - s->current_rate;

    printf("\033[1;36m┌────────────────────────────────────────────────────────────────────────┐\033[0m\033[K\n");
    printf("\033[1;36m│\033[1;37m  PRESONUS AUDIOBOX USB 96 DIAGNOSTIC MONITOR                           \033[1;36m│\033[0m\033[K\n");
    printf("\033[1;36m├────────────────────────────────────────────────────────────────────────┤\033[0m\033[K\n");
    printf("\033[1;36m│\033[1;37m  Driver Name: \033[1;32m%-16s\033[1;37m        Configured Rate: \033[1;32m%-6d Hz\033[1;36m        │\033[0m\033[K\n", s->driver_name, s->current_rate);
    printf("\033[1;36m├────────────────────────────────────────────────────────────────────────┤\033[0m\033[K\n");

    // Playback section
    printf("\033[1;36m│\033[1;35m  [PLAYBACK STREAM]                                                     \033[1;36m│\033[0m\033[K\n");
    printf("\033[1;36m│\033[1;37m  State: %-16s        Buffer Size: %-5lu frames                \033[1;36m│\033[0m\033[K\n",
           s->playback_active ? "\033[1;32mRUNNING\033[1;37m" : "\033[1;30mSTOPPED\033[1;37m", s->pb_buffer_size);
    printf("\033[1;36m│\033[1;37m  Period Size: %-5lu frames          Unplayed: %-5lu frames                 \033[1;36m│\033[0m\033[K\n",
           s->pb_period_size, s->pb_alsa_unplayed);
    printf("\033[1;36m│\033[1;37m  ALSA Buffer: %s[%-30s]\033[0m %5.1f%%                             \033[1;36m│\033[0m\033[K\n",
           pb_alsa_color, pb_alsa_bar, pb_alsa_ratio * 100.0);
    printf("\033[1;36m│\033[1;37m  USB Pipeline: %s[%-30s]\033[0m %5.1f%% (%-3d/%-3d frames)        \033[1;36m│\033[0m\033[K\n",
           usb_pipe_color, usb_pipe_bar, usb_pipeline_ratio * 100.0, (int)s->pb_in_flight, max_pipeline_frames);
    printf("\033[1;36m│\033[1;37m  Config: %2d URBs x %2d Pkts        Underruns (XRUNs): \033[1;31m%-5u\033[1;37m             \033[1;36m│\033[0m\033[K\n",
           s->pb_urb_count, s->pb_packets_per_urb, s->pb_xruns);
    printf("\033[1;36m├────────────────────────────────────────────────────────────────────────┤\033[0m\033[K\n");

    // Capture section
    printf("\033[1;36m│\033[1;35m  [CAPTURE STREAM]                                                      \033[1;36m│\033[0m\033[K\n");
    printf("\033[1;36m│\033[1;37m  State: %-16s        Buffer Size: %-5lu frames                \033[1;36m│\033[0m\033[K\n",
           s->capture_active ? "\033[1;32mRUNNING\033[1;37m" : "\033[1;30mSTOPPED\033[1;37m", s->cap_buffer_size);
    printf("\033[1;36m│\033[1;37m  Period Size: %-5lu frames          Unread:   %-5llu frames                \033[1;36m│\033[0m\033[K\n",
           s->cap_period_size, s->cap_unread);
    printf("\033[1;36m│\033[1;37m  Buffer Load: %s[%-30s]\033[0m %5.1f%%                             \033[1;36m│\033[0m\033[K\n",
           cap_color, cap_bar, cap_ratio * 100.0);
    printf("\033[1;36m│\033[1;37m  Overruns (XRUNs): \033[1;31m%-5u\033[1;37m                                                \033[1;36m│\033[0m\033[K\n", s->cap_xruns);
    printf("\033[1;36m├────────────────────────────────────────────────────────────────────────┤\033[0m\033[K\n");

    // Clock Sync Section
    printf("\033[1;36m│\033[1;35m  [CLOCK SYNCHRONIZATION]                                               \033[1;36m│\033[0m\033[K\n");
    printf("\033[1;36m│\033[1;37m  Feedback Status: %-16s  Nominal Rate: %-11.2f Hz           \033[1;36m│\033[0m\033[K\n",
           s->feedback_synced ? "\033[1;32mSYNCED\033[1;37m" : "\033[1;31mUNSYNCED\033[1;37m", (double)s->current_rate);
    printf("\033[1;36m│\033[1;37m  Actual Rate:     %-11.2f Hz     Drift: \033[1;33m%+-7.2f Hz\033[1;37m (Shift: %2d)      \033[1;36m│\033[0m\033[K\n",
           actual_rate, drift, s->freq_shift);
    printf("\033[1;36m│\033[1;37m  Freq Q16 Value:  %-10u            Feedback Val Q32: %-16llu \033[1;36m│\033[0m\033[K\n",
           s->freq_q16, s->feedback_val_q32);
    printf("\033[1;36m├────────────────────────────────────────────────────────────────────────┤\033[0m\033[K\n");

    // Timing Jitter Analysis
    double expected_ms = (double)s->pb_packets_per_urb * 0.125;
    double max_jitter_us = (double)s->jitter_max_ns / 1000.0;
    double avg_jitter_us = (double)s->jitter_avg_ns / 1000.0;

    printf("\033[1;36m│\033[1;35m  [LATENCY & SCHEDULING TIMING]                                         \033[1;36m│\033[0m\033[K\n");
    printf("\033[1;36m│\033[1;37m  Completion Target Interval: %-5.2f ms (%-5d us)                        \033[1;36m│\033[0m\033[K\n",
           expected_ms, s->pb_packets_per_urb * 125);
    printf("\033[1;36m│\033[1;37m  Average Timing Jitter:      \033[1;32m%-7.2f us\033[1;37m                                   \033[1;36m│\033[0m\033[K\n", avg_jitter_us);
    printf("\033[1;36m│\033[1;37m  Maximum Peak Jitter:        \033[1;33m%-7.2f us\033[1;37m                                   \033[1;36m│\033[0m\033[K\n", max_jitter_us);
    printf("\033[1;36m└────────────────────────────────────────────────────────────────────────┘\033[0m\033[K\n");
}

int main(void) {
    char *stats_file = find_stats_file();
    if (!stats_file) {
        fprintf(stderr, "Error: AudioBox USB 96 stats proc entry not found!\n");
        fprintf(stderr, "Please verify the snd-usb-audiobox96 driver is compiled and loaded.\n");
        return 1;
    }

    setvbuf(stdout, NULL, _IOFBF, 4096);
    printf("\033[2J"); // Clear screen once at launch

    struct stats s;
    while (1) {
        if (!parse_stats(stats_file, &s)) {
            printf("\033[H\033[J");
            printf("Error: Driver interface disconnected.\n");
            fflush(stdout);
            sleep(1);
            continue;
        }

        render_dashboard(&s);
        fflush(stdout);
        usleep(100000); // 100ms updates
    }
    return 0;
}
