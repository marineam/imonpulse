#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>

#include <pulse/pulseaudio.h>
#include <fftw3.h>

//#define VERBOSE

/* This buffer should give us about 10 data sets per second
 * and thus a minimum frequency of about 10 Hz. The maximum
 * frequency is half the sample rate or about 22 kHz */
#define SAMPLE_RATE 44100
#define BUF_SAMPLES 4096
#define BUF_SIZE    (sizeof(float) * BUF_SAMPLES)
#define BAR_COUNT   16

/* This is the frequency range for each graph bar, by fft output index.
 * The actual frequency represented is about 20 times greater. These
 * were computed with:
 * for (i = 0; i <= 16; i++)
 *     round(1.54221083^i) */
static const int BAR_RANGE[] =
    {1, 2, 3, 4, 6, 9, 13, 21, 32, 49, 76, 117, 181, 279, 431, 664, 1024};

static const char BAR_CHARS[2][16] = {
    {' ',' ',' ',' ',' ',' ',' ',' ',0x0,0x1,0x2,0x3,0x4,0x5,0x6,0x7},
    {0x0,0x1,0x2,0x3,0x4,0x5,0x6,0x7,0x7,0x7,0x7,0x7,0x7,0x7,0x7,0x7}};

/* TODO: auto-detect this */
#define PULSE_DEV \
    "alsa_output.pci_8086_3a3e_sound_card_0_alsa_playback_0.monitor"
#define IMON_DEV "/dev/lcd0"

/* imon worker thread control */
static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_waiting = PTHREAD_COND_INITIALIZER;
static int worker_done, worker_error, worker_trigger;

/* imon frame buffer and device */
static char lcd_buffer[2][2][BAR_COUNT];
static int lcd_fd, lcd_flip;

/* fft buffers */
static float *input;
static float *output;
static fftwf_plan plan;

/* Pulse connection information */
static pa_mainloop_api *mainloop_api;
static pa_context *context;
static pa_stream *stream;
static pa_time_event *volume_timeout;

struct volume {
    pa_volume_t value;
    int mute;
};

static void* imon_worker(void *nill)
{
    char write_buffer[2][BAR_COUNT];

    while (1) {
        pthread_mutex_lock(&worker_lock);

        if (!worker_trigger)
            pthread_cond_wait(&worker_waiting, &worker_lock);

        worker_trigger = 0;

        if (worker_done)
            break;

        memcpy(write_buffer, lcd_buffer[!lcd_flip], sizeof(write_buffer));
        pthread_mutex_unlock(&worker_lock);

        if (write(lcd_fd, write_buffer, sizeof(write_buffer)) < 0) {
            fprintf(stderr, "LCD update failed: %s\n", strerror(errno));
            pthread_mutex_lock(&worker_lock);
            worker_error = 1;
            worker_done = 1;
            break;
        }

    }

    pthread_mutex_unlock(&worker_lock);
    return NULL;
}

static void imon_worker_stop()
{
    pthread_mutex_lock(&worker_lock);

    if (!worker_done) {
        worker_done = 1;
        pthread_cond_signal(&worker_waiting);
    }

    pthread_mutex_unlock(&worker_lock);
}

static void imon_worker_trigger(int force)
{
    pthread_mutex_lock(&worker_lock);

    if (!worker_trigger || force) {
        worker_trigger = 1;
        lcd_flip = !lcd_flip;
        pthread_cond_signal(&worker_waiting);
    }
#ifdef VERBOSE
    else
        fprintf(stderr, "Skipping update...\n");
#endif

    pthread_mutex_unlock(&worker_lock);
}

static int imon_init()
{

    /* fftw_malloc ensures that the buffers are properly aligned for SSE */
    input = fftwf_malloc(BUF_SIZE);
    output = fftwf_malloc(BUF_SIZE);
    assert(input && output);

    /* The data will be returned in half complex format (see fftw docs) */
    plan = fftwf_plan_r2r_1d(BUF_SAMPLES,
            input, output,
            FFTW_R2HC, FFTW_MEASURE);

    memset(lcd_buffer, ' ', sizeof(lcd_buffer));

    if ((lcd_fd = open(IMON_DEV, O_WRONLY|O_NONBLOCK)) < 0) {
        fprintf(stderr, "LCD init failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static void imon_free()
{
    pthread_mutex_destroy(&worker_lock);
    pthread_cond_destroy(&worker_waiting);

    fftwf_destroy_plan(plan);
    fftwf_free(input);
    fftwf_free(output);

    if (lcd_fd)
        close(lcd_fd);
}

static void imon_update_bar(float level, int bar) {
    int i = (int)((logf(level)/7) * 16);

    if (i < 0)
        i = 0;
    else if (i >= 16)
        i = 15;

    lcd_buffer[lcd_flip][0][bar] = BAR_CHARS[0][i];
    lcd_buffer[lcd_flip][1][bar] = BAR_CHARS[1][i];
}

static void imon_update()
{
    /* Apply a Hamming window to focus on the center of this data set */
    for (int i = 0; i < BUF_SAMPLES; i++)
        input[i] *= (0.54 - 0.46 * cosf((2*M_PI*i)/BUF_SAMPLES));

    fftwf_execute(plan);

    for (int i = 0; i < BAR_COUNT; i++) {
        float max = 0;

        for (int j = BAR_RANGE[i]; j < BAR_RANGE[i+1]; j++) {
            float mag = sqrt(pow(output[j],2) +
                    pow(output[BUF_SAMPLES-j],2));
            if (mag > max)
                max = mag;
        }

        imon_update_bar(max, i);
    }

    imon_worker_trigger(0);
}

static void pulse_stream_read(pa_stream *s, size_t length, void *nill)
{
    static uint8_t saved[BUF_SIZE];
    static int saved_length = 0;
    const void *buffer;
    int index = 0;

    /* Abort if the worker thread stopped already */
    if (worker_done)
        mainloop_api->quit(mainloop_api, 1);

    assert(length > 0);

    if (pa_stream_peek(s, &buffer, &length) < 0) {
        fprintf(stderr, "Stream read failure: %s\n",
                pa_strerror(pa_context_errno(context)));
        mainloop_api->quit(mainloop_api, 1);
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
                imon_update();
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
                imon_update();
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

static void pulse_stream_change(pa_stream *s, void *nill)
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
            mainloop_api->quit(mainloop_api, 1);
            break;
    }
}

static void pulse_clear_volume(pa_mainloop_api *a, pa_time_event *e,
        const struct timeval *tv, void *nill)
{
    memset(lcd_buffer[lcd_flip], ' ', sizeof(lcd_buffer[0]));
    imon_worker_trigger(1);

    mainloop_api->time_free(volume_timeout);
    volume_timeout = NULL;
    pa_operation_unref(pa_stream_cork(stream, 0, NULL, NULL));
}

static void pulse_show_volume(pa_stream *s, int success, void *data)
{
    const char mute[] = "Muted";
    struct volume *volume = data;
    char volbuf[BAR_COUNT+1];
    int len, level, scaled;
    struct timeval tv;

    scaled = (volume->value * 100) / PA_VOLUME_NORM;
    level = ((float)volume->value / PA_VOLUME_NORM) * BAR_COUNT;

    if (!volume->mute) {
        len = snprintf(volbuf, sizeof(volbuf), "Volume: %d%%", scaled);
        memcpy(lcd_buffer[lcd_flip][0], volbuf, len);
        memset(lcd_buffer[lcd_flip][0]+len, ' ', BAR_COUNT-len);
    }
    else {
        len = strlen(mute);
        memcpy(lcd_buffer[lcd_flip][0], mute, len);
        memset(lcd_buffer[lcd_flip][0]+len, ' ', BAR_COUNT-len);
    }

    for (int i = 0; i < BAR_COUNT; i++) {
        if (i < level)
            lcd_buffer[lcd_flip][1][i] = i/2;
        else
            lcd_buffer[lcd_flip][1][i] = ' ';
    }

    imon_worker_trigger(1);

    pa_gettimeofday(&tv);
    pa_timeval_add(&tv, 3000000);

    if (!volume_timeout) {
        volume_timeout = mainloop_api->time_new(mainloop_api,
                &tv, pulse_clear_volume, NULL);
    }
    else {
        mainloop_api->time_restart(volume_timeout, &tv);
    }
}

static void pulse_check_volume(pa_context *c,
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

    pa_operation_unref(pa_stream_cork(stream, 1, pulse_show_volume, new));
}

static void pulse_context_event(pa_context *c,
        pa_subscription_event_type_t t, uint32_t idx, void *nill)
{
    // TODO: don't hard code the card number
    if (idx != 0 || t != PA_SUBSCRIPTION_EVENT_CHANGE)
        return;

    pa_operation_unref(
            pa_context_get_sink_info_by_index(
                c, idx, pulse_check_volume, NULL));
}

static void pulse_context_chage(pa_context *c, void *nill)
{
    pa_sample_spec spec;

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY:
            /* To make the fft easy get data as mono and in floats */
            memset(&spec, 0, sizeof(spec));
            spec.format = PA_SAMPLE_FLOAT32NE;
            spec.channels = 1;
            spec.rate = SAMPLE_RATE;

            stream = pa_stream_new(c, "monitor", &spec, NULL);
            pa_stream_set_state_callback(stream, pulse_stream_change, NULL);
            pa_stream_set_read_callback(stream, pulse_stream_read, NULL);
            pa_stream_connect_record(stream, PULSE_DEV, NULL, 0);

            pa_operation_unref(pa_context_subscribe(c,
                        PA_SUBSCRIPTION_MASK_SINK, NULL, NULL));

            break;

        case PA_CONTEXT_TERMINATED:
            mainloop_api->quit(mainloop_api, 0);
            break;

        case PA_CONTEXT_FAILED:
            fprintf(stderr, "Connection failure: %s\n",
                    pa_strerror(pa_context_errno(c)));
            mainloop_api->quit(mainloop_api, 1);
            break;
    }
}

int main(int argc, char * argv[])
{
    pthread_t worker;
    pa_mainloop *mainloop = NULL;
    int r, ret = 1;

    if (imon_init())
        goto finish;

    /* Start up the worker thread */
    if ((r = pthread_create(&worker, NULL, imon_worker, NULL))) {
        fprintf(stderr, "thread create failed: %s\n", strerror(r));
        goto finish;
    }

    /* Start up the PulseAudio connection */
    mainloop = pa_mainloop_new();
    assert(mainloop);
    mainloop_api = pa_mainloop_get_api(mainloop);

    context = pa_context_new(mainloop_api, argv[0]);
    assert(context);
    pa_context_set_state_callback(context, pulse_context_chage, NULL);
    pa_context_set_subscribe_callback(context, pulse_context_event, NULL);

    if (pa_context_connect(context, NULL, 0, NULL)) {
        fprintf(stderr, "Connection failure: %s\n",
                pa_strerror(pa_context_errno(context)));
        goto finish;
    }

    pa_mainloop_run(mainloop, &ret);

finish:
    imon_worker_stop();
    pthread_join(worker, NULL);

    if (stream)
        pa_stream_unref(stream);

    if (context)
        pa_context_unref(context);

    if (mainloop) {
        pa_signal_done();
        pa_mainloop_free(mainloop);
    }

    imon_free();

    return ret;
}
