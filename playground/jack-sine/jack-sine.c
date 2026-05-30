/**
 * jack-sine.c - Lightweight, minimal JACK client for driver testing
 * Generates a clean, low-overhead sine wave inside the RT context.
 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <jack/jack.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Configuration Parameters */
#define DEFAULT_FREQ 440.0
#define DEFAULT_AMP  0.5

jack_port_t *output_port1;
jack_port_t *output_port2;
jack_client_t *client;

double phase = 0.0;
double phase_step = 0.0;
double sample_rate = 48000.0;
double target_frequency = DEFAULT_FREQ;
double amplitude = DEFAULT_AMP;

/* Signal handler to exit cleanly */
static void signal_handler(int sig) {
    fprintf(stderr, "\nSignal %d received, exiting cleanly...\n", sig);
    if (client) {
        jack_client_close(client);
    }
    exit(0);
}

/**
 * Real-time process callback executed by the JACK engine
 */
int process(jack_nframes_t nframes, void *arg) {
    jack_default_audio_sample_t *out1, *out2;
    jack_nframes_t i;

    out1 = jack_port_get_buffer(output_port1, nframes);
    out2 = jack_port_get_buffer(output_port2, nframes);

    for (i = 0; i < nframes; i++) {
        float sample = (float)(sin(phase) * amplitude);
        out1[i] = sample;
        out2[i] = sample;

        phase += phase_step;
        if (phase >= 2.0 * M_PI) {
            phase -= 2.0 * M_PI;
        }
    }
    return 0;
}

/**
 * Handle changes to the system sample rate dynamically
 */
int sample_rate_callback(jack_nframes_t nval, void *arg) {
    sample_rate = (double)nval;
    phase_step = (2.0 * M_PI * target_frequency) / sample_rate;
    printf("[JACK] Sample rate changed to: %.1f Hz (Phase Step: %f)\n", sample_rate, phase_step);
    return 0;
}

void jack_shutdown(void *arg) {
    fprintf(stderr, "[JACK] Server shut down or client disconnected.\n");
    exit(1);
}

int main(int argc, char *argv[]) {
    const char **ports;
    const char *client_name = "jack-sine-tester";
    jack_options_t options = JackNullOption;
    jack_status_t status;

    if (argc >= 2) {
        target_frequency = atof(argv[1]);
        if (target_frequency <= 0.0 || target_frequency > 22000.0) {
            target_frequency = DEFAULT_FREQ;
        }
    }
    if (argc >= 3) {
        amplitude = atof(argv[2]);
        if (amplitude < 0.0) amplitude = 0.0;
        if (amplitude > 1.0) amplitude = 1.0;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Open connection to JACK server */
    client = jack_client_open(client_name, options, &status, NULL);
    if (client == NULL) {
        fprintf(stderr, "jack_client_open() failed, status = 0x%2.0x\n", status);
        if (status & JackServerFailed) {
            fprintf(stderr, "Unable to connect to JACK server daemon.\n");
        }
        return 1;
    }

    if (status & JackNameNotUnique) {
        client_name = jack_get_client_name(client);
        fprintf(stderr, "Unique name '%s' assigned.\n", client_name);
    }

    /* Configure callbacks */
    jack_set_process_callback(client, process, 0);
    jack_set_sample_rate_callback(client, sample_rate_callback, 0);
    jack_on_shutdown(client, jack_shutdown, 0);

    /* Initialize initial sample rate state */
    sample_rate = (double)jack_get_sample_rate(client);
    phase_step = (2.0 * M_PI * target_frequency) / sample_rate;

    printf("[JACK] Connected. Target: %.2f Hz @ %.1f Hz (Amp: %.2f)\n", 
           target_frequency, sample_rate, amplitude);

    /* Register two mono output ports */
    output_port1 = jack_port_register(client, "output_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    output_port2 = jack_port_register(client, "output_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

    if ((output_port1 == NULL) || (output_port2 == NULL)) {
        fprintf(stderr, "Error: Could not register JACK output ports.\n");
        jack_client_close(client);
        return 1;
    }

    /* Activate client */
    if (jack_activate(client)) {
        fprintf(stderr, "Error: Could not activate JACK client.\n");
        jack_client_close(client);
        return 1;
    }

    /* Auto-connect ports to physical audio playback interfaces */
    ports = jack_get_ports(client, NULL, NULL, JackPortIsPhysical | JackPortIsInput);
    if (ports != NULL) {
        if (ports[0] != NULL) {
            jack_connect(client, jack_port_name(output_port1), ports[0]);
        }
        if (ports[1] != NULL) {
            jack_connect(client, jack_port_name(output_port2), ports[1]);
        }
        free(ports);
    } else {
        printf("Warning: No physical playback ports found to connect to.\n");
    }

    printf("Generating tone. Press Ctrl+C to stop.\n");
    while (1) {
        sleep(1);
    }

    jack_client_close(client);
    return 0;
}
