#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>

#include <pulse/simple.h>
#include <pulse/error.h>
#include <fftw3.h>

/* This buffer should give us about 21 data sets per second
 * and thus a minimum frequency of about 21 Hz. The maximum
 * frequency is half the sample rate or about 22 kHz */
#define SAMPLE_RATE 44100
#define BUF_SAMPLES 2048
#define BUF_SIZE    (sizeof(float) * BUF_SAMPLES)
#define BAR_COUNT   16
#define BAR_MAX     75 // An arbitrary number...

/* This is the frequency range for each graph bar, by fft output index.
 * The actual frequency represented is about 21 times greater. These
 * were computed with:
 * for (i = 0; i <= 16; i++)
 *     round(1.54221083^i) */
const int BAR_RANGE[] =
    {1, 2, 3, 4, 6, 9, 13, 21, 32, 49, 76, 117, 181, 279, 431, 664, 1024};

const char BAR_CHARS[2][16] = {
    {' ',' ',' ',' ',' ',' ',' ',' ',0x0,0x1,0x2,0x3,0x4,0x5,0x6,0x7},
    {0x0,0x1,0x2,0x3,0x4,0x5,0x6,0x7,0x7,0x7,0x7,0x7,0x7,0x7,0x7,0x7}};

/* TODO: auto-detect this */
#define PULSE_DEV \
    "alsa_output.pci_8086_3a3e_sound_card_0_alsa_playback_0.monitor"
#define IMON_DEV "/dev/lcd0"

struct imon_display {
    /* imon worker thread control */
    pthread_mutex_t worker_waiting;
    int worker_done;

    /* imon frame buffer and device */
    char lcd_buffer[2][BAR_COUNT];
    int lcd_fd;

    /* fft buffers */
    float *input;
    float *output;
    fftwf_plan plan;
};

static int imon_init(struct imon_display *display)
{
    pthread_mutex_init(&display->worker_waiting, NULL);
    display->worker_done = 0;

    /* fftw_malloc ensures that the buffers are properly aligned for SSE */
    display->input = fftwf_malloc(BUF_SIZE);
    display->output = fftwf_malloc(BUF_SIZE);
    assert(display->input && display->output);

    /* The data will be returned in half complex format (see fftw docs) */
    display->plan = fftwf_plan_r2r_1d(BUF_SAMPLES,
            display->input, display->output,
            FFTW_R2HC, FFTW_MEASURE);

    if ((display->lcd_fd = open(IMON_DEV, O_WRONLY)) < 0) {
        fprintf(stderr, "LCD init failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static void imon_free(struct imon_display *display)
{
    pthread_mutex_destroy(&display->worker_waiting);

    fftwf_destroy_plan(display->plan);
    fftwf_free(display->input);
    fftwf_free(display->output);

    if (display->lcd_fd)
        close(display->lcd_fd);
}

static void imon_update_bar(struct imon_display *display,
        float level, int bar) {
    int i = (int)((level/BAR_MAX) * 16);

    if (i < 0)
        i = 0;
    else if (i >= 16)
        i = 15;

    display->lcd_buffer[0][bar] = BAR_CHARS[0][i];
    display->lcd_buffer[1][bar] = BAR_CHARS[1][i];
}

void imon_update(struct imon_display *display)
{
    /* Apply a Hamming window to focus on the center of this data set */
    for (int i = 0; i < BUF_SAMPLES; i++)
        display->input[i] *= (0.54 - 0.46 * cosf((2*M_PI*i)/BUF_SAMPLES));

    fftwf_execute(display->plan);

    for (int i = 0; i < BAR_COUNT; i++) {
        float max = 0;

        for (int j = BAR_RANGE[i]; j < BAR_RANGE[i+1]; j++) {
            float mag = sqrt(pow(display->output[j],2) +
                    pow(display->output[BUF_SAMPLES-j],2));
            if (mag > max)
                max = mag;
        }

        imon_update_bar(display, max, i);
    }

    if (write(display->lcd_fd, display->lcd_buffer,
                sizeof(display->lcd_buffer)) < 0) {
        fprintf(stderr, "LCD update failed: %s\n", strerror(errno));
        display->worker_done = 1;
    }
}

void* imon_worker(void *data)
{
    struct imon_display *display = data;

    while (!display->worker_done) {
        pthread_mutex_lock(&display->worker_waiting);

        imon_update(display);
    }

    return NULL;
}

int main(int argc, char * argv[])
{
    int error, ret = 1;
    pa_simple *pulse = NULL;
    pa_sample_spec spec;
    struct imon_display display;
    pthread_t worker;

    if (imon_init(&display))
        goto finish;

    /* To make the fft easy get data as mono and in floats */
    spec.format = PA_SAMPLE_FLOAT32NE;
    spec.channels = 1;
    spec.rate = SAMPLE_RATE;

    /* Start up the worker thread */
    pthread_create(&worker, NULL, imon_worker, &display);
    pthread_detach(worker);

    if (!(pulse = pa_simple_new(NULL, argv[0], PA_STREAM_RECORD, PULSE_DEV,
                    "pretty lcd thingy", &spec, NULL, NULL, &error))) {
        fprintf(stderr, "pa_simple_new() failed: %s\n", pa_strerror(error));
        goto finish;
    }

    for (;;) {
        uint8_t buffer[BUF_SIZE];

        /*
        pa_usec_t latency;

        if ((latency = pa_simple_get_latency(pulse, &error)) < 0) {
            fprintf(stderr, "Reading latency failed: %s\n",
                    pa_strerror(error));
            goto finish;
        }

        printf("%lld\n", latency);
        */

        if (pa_simple_read(pulse, &buffer, BUF_SIZE, &error) < 0) {
            fprintf(stderr, "Reading from Pulse failed: %s\n",
                    pa_strerror(error));
            goto finish;
        }

        if (pthread_mutex_trylock(&display.worker_waiting) == EBUSY) {
            /* Worker is waiting, update the input buffer */
            memcpy(display.input, &buffer, BUF_SIZE);
        }
        else {
            fprintf(stderr, "Skipping update...\n");
        }

        /* Clear the above lock or signal the worker to start */
        pthread_mutex_unlock(&display.worker_waiting);
    }

    ret = 0;

finish:
    display.worker_done = 1;

    if (pulse)
        pa_simple_free(pulse);

    imon_free(&display);

    return ret;
}
