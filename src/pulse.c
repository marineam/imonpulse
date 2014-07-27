#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <fftw3.h>
#include <pulse/pulseaudio.h>

#include "monitor.h"

/* Alias this because it is used constantly */
#define pao(o) pa_operation_unref(o)

#define APPLICATION_NAME "iMonPulse"

/* The iMon display doesn't respond too quickly, so we are using
 * 10 data sets per second, for a fundamental frequency of 10Hz.
 * The maximum frequency is half the sample rate, or 22.05kHz
 * for the standard CD sample rate of 44.1kHz. */
#define SAMPLE_RATE 44100
#define BUF_SAMPLES 4410
#define BUF_SIZE    (sizeof(float) * BUF_SAMPLES)
#define BAR_COUNT   16

/* Max range for each bar, by fft output index. The true frequency
 * is the index multiplied by the fundamental frequency. The range of
 * frequencies are split up between the bars on a logarithmic scale.
 * Index 0 (the DC) is skipped since we just want frequency info.
 *   MAX = (44100kHz / 2) / 10Hz = 2205
 *   BASE = 2205^(1/16) = 1.6179423283987682
 * So the indexes are computed by:
 *   for (i = 0; i <= 16; i++)
 *      BAR_RANGE[i] = round(BASE^i)
 */
static const int BAR_RANGE[BAR_COUNT+1] =
    {1, 2, 3, 4, 7, 11, 18, 29, 47, 76, 123, 199, 322, 521, 842, 1363, 2205};

static const char BAR_CHARS[2][17] = {
    {' ',' ',' ',' ',' ',' ',' ',' ',' ',0x0,0x1,0x2,0x3,0x4,0x5,0x6,0x7},
    {' ',0x0,0x1,0x2,0x3,0x4,0x5,0x6,0x7,0x7,0x7,0x7,0x7,0x7,0x7,0x7,0x7}};

/* TODO: auto-detect this */
#define PULSE_DEV "alsa_output.pci-0000_00_1b.0.analog-stereo.monitor"

/* fft buffers */
static float *input;
static float *output;
static fftwf_plan plan;

/* Pulse connection information */
static pa_mainloop_api *api;
static pa_context *context;
static pa_stream *stream;
static pa_time_event *volume_timeout;

struct volume {
    pa_volume_t value;
    int mute;
};

static int context_setup();

static void display_init()
{

    /* fftw_malloc ensures that the buffers are properly aligned for SSE */
    input = fftwf_malloc(BUF_SIZE);
    output = fftwf_malloc(BUF_SIZE);
    assert(input && output);

    /* The data will be returned in half complex format (see fftw docs) */
    plan = fftwf_plan_r2r_1d(BUF_SAMPLES,
            input, output,
            FFTW_R2HC, FFTW_MEASURE);
}

static void display_free()
{
    fftwf_destroy_plan(plan);
    fftwf_free(input);
    fftwf_free(output);
}

static void display_update()
{
    char display[2][BAR_COUNT];

    /* Apply a Hamming window to focus on the center of this data set */
    for (int i = 0; i < BUF_SAMPLES; i++)
        input[i] *= (0.54 - 0.46 * cosf((2*M_PI*i)/BUF_SAMPLES));

    fftwf_execute(plan);

    for (int i = 0; i < BAR_COUNT; i++) {
        float max = 0;
        int level;

        for (int j = BAR_RANGE[i]; j < BAR_RANGE[i+1]; j++) {
            float mag = hypotf(output[j], output[BUF_SAMPLES-j]);
            if (mag > max)
                max = mag;
        }

        /* Scale to a level between 0 and 16...
         * The 2.5 value is pretty arbitrary, just picked what looked ok. */
        level = (int)(logf(max) * 2.5);
        if (level < 0)
            level = 0;
        else if (level > 16)
            level = 16;

        display[0][i] = BAR_CHARS[0][level];
        display[1][i] = BAR_CHARS[1][level];
    }

    imon_write(display, sizeof(display));
}

static void stream_read(pa_stream *s, size_t length, void *nill)
{
    static uint8_t saved[BUF_SIZE];
    static int saved_length = 0;
    const void *buffer;
    int index = 0;

    assert(length > 0);

    if (pa_stream_peek(s, &buffer, &length) < 0) {
        fprintf(stderr, "Stream read failure: %s\n",
                pa_strerror(pa_context_errno(context)));
        api->quit(api, 1);
    }

    assert(length > 0);

    /* Yeah, this is ugly, is should be done differently, but whatever */
    while (1) {
        if (saved_length) {
            if (saved_length + length - index >= BUF_SIZE) {
                memcpy(input, saved, saved_length);
                memcpy(((uint8_t*)input) + saved_length,
                        buffer + index, BUF_SIZE - saved_length);
                index += BUF_SIZE - saved_length;
                saved_length = 0;
                display_update();
            }
            else {
                memcpy(saved+saved_length, buffer + index, length - index);
                saved_length += length - index;
                break;
            }
        }
        else {
            if (length - index >= BUF_SIZE) {
                memcpy(input, buffer + index, BUF_SIZE);
                index += BUF_SIZE;
                display_update();
            }
            else {
                memcpy(saved, buffer + index, length - index);
                saved_length = length - index;
                break;
            }
        }
    }

    pa_stream_drop(s);
}

static void stream_change(pa_stream *s, void *nill)
{
    switch (pa_stream_get_state(s)) {
        case PA_STREAM_CREATING:
        case PA_STREAM_TERMINATED:
        case PA_STREAM_UNCONNECTED:
        case PA_STREAM_READY:
            break;

        case PA_STREAM_FAILED:
            fprintf(stderr, "Stream failure: %s\n",
                    pa_strerror(pa_context_errno(context)));
            api->quit(api, 1);
            break;
    }
}

static void clear_volume(pa_mainloop_api *a, pa_time_event *e,
        const struct timeval *tv, void *nill)
{
    imon_clear();

    api->time_free(volume_timeout);
    volume_timeout = NULL;
    pao(pa_stream_cork(stream, 0, NULL, NULL));
}

static void show_volume(pa_stream *s, int success, void *data)
{
    struct volume *volume = data;
    char display[2][BAR_COUNT];
    int len, level, scaled;
    struct timeval tv;

    scaled = (volume->value * 100) / PA_VOLUME_NORM;
    level = ((float)volume->value / PA_VOLUME_NORM) * BAR_COUNT;

    if (!volume->mute)
        len = snprintf(display[0], BAR_COUNT - 1, "Volume: %d%%", scaled);
    else
        len = snprintf(display[0], BAR_COUNT - 1, "Muted");

    memset(display[0]+len, ' ', BAR_COUNT-len);

    for (int i = 0; i < BAR_COUNT; i++) {
        if (i < level)
            display[1][i] = i/2;
        else
            display[1][i] = ' ';
    }

    imon_write(display, sizeof(display));

    pa_gettimeofday(&tv);
    pa_timeval_add(&tv, 3000000);

    if (!volume_timeout) {
        volume_timeout = api->time_new(api, &tv, clear_volume, NULL);
    }
    else {
        api->time_restart(volume_timeout, &tv);
    }
}

static void check_volume(pa_context *c,
        const pa_sink_info *i, int eol, void *nill)
{
    static struct volume volume = {0, 0};
    struct volume *new;

    if (!i || (volume.value == i->volume.values[0] && volume.mute == i->mute))
        return;

    volume.mute = i->mute;
    volume.value = i->volume.values[0];

    new = malloc(sizeof(volume));
    assert(new);
    memcpy(new, &volume, sizeof(volume));

    pao(pa_stream_cork(stream, 1, show_volume, new));
}

static void context_event(pa_context *c,
        pa_subscription_event_type_t t, uint32_t idx, void *nill)
{
    // TODO: don't hard code the card number
    if (idx != 0 || t != PA_SUBSCRIPTION_EVENT_CHANGE)
        return;

    pao(pa_context_get_sink_info_by_index(c, idx, check_volume, NULL));
}

static void context_change(pa_context *c, void *nill)
{
    pa_sample_spec spec;

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_SETTING_NAME:
        case PA_CONTEXT_TERMINATED:
            break;

        case PA_CONTEXT_READY:
            /* To make the fft easy get data as mono and in floats */
            memset(&spec, 0, sizeof(spec));
            spec.format = PA_SAMPLE_FLOAT32NE;
            spec.channels = 1;
            spec.rate = SAMPLE_RATE;

            stream = pa_stream_new(c, "monitor", &spec, NULL);
            pa_stream_set_state_callback(stream, stream_change, NULL);
            pa_stream_set_read_callback(stream, stream_read, NULL);
            pa_stream_connect_record(stream, PULSE_DEV, NULL, 0);

            pao(pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK, NULL, NULL));

            break;

        case PA_CONTEXT_FAILED:
            fprintf(stderr, "Connection failure: %s\n",
                    pa_strerror(pa_context_errno(c)));

            /* Attempt to reconnect */
            if (context != c || context_setup(c))
                api->quit(api, 1);
            break;
    }
}

static int context_setup() {
    time_t timeout = time(NULL) + 30;

    if (context)
        pa_context_unref(context);

    context = pa_context_new(api, APPLICATION_NAME);
    assert(context);

    pa_context_set_state_callback(context, context_change, NULL);
    pa_context_set_subscribe_callback(context, context_event, NULL);

    do {
        if (pa_context_connect(context, NULL, 0, NULL)) {
            fprintf(stderr, "Connection failure: %s\n",
                pa_strerror(pa_context_errno(context)));
            sleep(1);
        }
        else
            return 0;
    } while (time(NULL) <= timeout);

    pa_context_unref(context);
    context = NULL;

    return 1;
}

static void signal_exit(pa_mainloop_api *api, pa_signal_event *e,
        int sig, void *data) {
    api->quit(api, 0);
}

int main(int argc, char * argv[])
{
    pa_mainloop *mainloop = NULL;
    int r, ret = 1;

    display_init();

    /* Start up the PulseAudio connection */
    mainloop = pa_mainloop_new();
    assert(mainloop);
    api = pa_mainloop_get_api(mainloop);

    /* Register some signal handlers */
    pa_signal_init(api);
    pa_signal_new(SIGINT, signal_exit, NULL);
    pa_signal_new(SIGTERM, signal_exit, NULL);

    if (context_setup())
        goto finish;

    if ((r = imon_open(api))) {
        fprintf(stderr, "Failed to open iMON display: %s\n", strerror(r));
        goto finish;
    }

    pa_mainloop_run(mainloop, &ret);
    imon_close(api);

finish:
    if (stream)
        pa_stream_unref(stream);

    pa_signal_done();
    if (context)
        pa_context_disconnect(context);
        pa_context_unref(context);
    pa_mainloop_free(mainloop);
    api = NULL;

    display_free();

    return ret;
}
