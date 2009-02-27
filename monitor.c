#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>

#include <pulse/simple.h>
#include <pulse/error.h>

/* This buffer should give us 8 reads per second */
#define BUF_SIZE 22050
/* TODO: auto-detect this */
#define DEV_NAME \
    "alsa_output.pci_8086_3a3e_sound_card_0_alsa_playback_0.monitor"


int main(int argc, char * argv[])
{
    pa_simple *pulse;
    pa_sample_spec spec;
    int error, ret = 1;

    spec.format = PA_SAMPLE_S16NE;
    spec.channels = 2;
    spec.rate = 44100;

    if (!(pulse = pa_simple_new(NULL, argv[0], PA_STREAM_RECORD, DEV_NAME,
                    "level monitor", &spec, NULL, NULL, &error))) {
        fprintf(stderr, "pa_simple_new() failed: %s\n", pa_strerror(error));
        goto finish;
    }

    for (;;) {
        unsigned char buf[BUF_SIZE];

        if (pa_simple_read(pulse, buf, sizeof(buf), &error) < 0) {
            fprintf(stderr, "pa_simple_read() failed: %s\n", pa_strerror(error));
            goto finish;
        }

        printf("Got %d bytes!\n", sizeof(buf));
    }

    ret = 0;

finish:
    pa_simple_free(pulse);
    return ret;
}
