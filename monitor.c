#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <math.h>

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

/* TODO: auto-detect this */
#define PULSE_DEV \
    "alsa_output.pci_8086_3a3e_sound_card_0_alsa_playback_0.monitor"
#define IMON_DEV "/dev/lcd0"

struct imon_display {
    char data[2][16];
};

struct fft_context {
    float *input;
    float *output;
    fftwf_plan plan;
};

void imon_update(struct imon_display *display, float level, int bar) {
    const char bar_chars[2][16] = {
        {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
         0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7},
        {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
         0x7, 0x7, 0x7, 0x7, 0x7, 0x7, 0x7, 0x7}};
    int i = (int)((level/BAR_MAX) * 16);

    if (i < 0)
        i = 0;
    else if (i >= 16)
        i = 15;

    display->data[0][bar] = bar_chars[0][i];
    display->data[1][bar] = bar_chars[1][i];
}

void fft_init(struct fft_context *context)
{
    /* fftw_malloc ensures that the buffers are properly aligned for SSE */
    context->input = fftwf_malloc(BUF_SIZE);
    context->output = fftwf_malloc(BUF_SIZE);
    assert(context->input && context->output);

    /* The data will be returned in half complex format (see fftw docs) */
    context->plan = fftwf_plan_r2r_1d(BUF_SAMPLES,
            context->input, context->output,
            FFTW_R2HC, FFTW_MEASURE);
}

void fft_free(struct fft_context *context)
{
    fftwf_destroy_plan(context->plan);
    fftwf_free(context->input);
    fftwf_free(context->output);
}

void fft_compute(struct fft_context *context, struct imon_display *display)
{
    /* Apply a Hamming window to focus on the center of this data set */
    for (int i = 0; i < BUF_SAMPLES; i++)
        context->input[i] *= (0.54 - 0.46 * cosf((2*M_PI*i)/BUF_SAMPLES));

    fftwf_execute(context->plan);

    for (int i = 0; i < BAR_COUNT; i++) {
        float max = 0;

        for (int j = BAR_RANGE[i]; j < BAR_RANGE[i+1]; j++) {
            float mag = sqrt(pow(context->output[j],2) +
                    pow(context->output[BUF_SAMPLES-j],2));
            if (mag > max)
                max = mag;
        }

        imon_update(display, max, i);
    }
}

int main(int argc, char * argv[])
{
    int imon, error, ret = 1;
    pa_simple *pulse;
    pa_sample_spec spec;
    struct fft_context context;

    fft_init(&context);

    /* To make the fft easy get data as mono and in floats */
    spec.format = PA_SAMPLE_FLOAT32NE;
    spec.channels = 1;
    spec.rate = SAMPLE_RATE;

    if (!(pulse = pa_simple_new(NULL, argv[0], PA_STREAM_RECORD, PULSE_DEV,
                    "pretty lcd thingy", &spec, NULL, NULL, &error))) {
        fprintf(stderr, "pa_simple_new() failed: %s\n", pa_strerror(error));
        goto finish;
    }

    if ((imon = open(IMON_DEV, O_WRONLY)) < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", IMON_DEV, strerror(errno));
        goto finish;
    }

    for (;;) {
        struct imon_display display;

        if (pa_simple_read(pulse, context.input, BUF_SIZE, &error) < 0) {
            fprintf(stderr, "pa_simple_read() failed: %s\n", pa_strerror(error));
            goto finish;
        }

        fft_compute(&context, &display);

        if (write(imon, &display, sizeof(display)) < 0) {
            fprintf(stderr, "write() failed: %s\n", strerror(errno));
            goto finish;
        }
    }

    ret = 0;

finish:
    if (imon)
        close(imon);
    if (pulse)
        pa_simple_free(pulse);
    fft_free(&context);
    return ret;
}
