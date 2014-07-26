#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <semaphore.h>
#include <libusb.h>
#include <pulse/pulseaudio.h>

#include "monitor.h"

#define IMON_VENDOR 0x15c2
#define IMON_PRODUCT 0xffdc
static uint8_t END_PACKET[] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff };

#define LOCK_MAX 2
static sem_t lock;
static libusb_context *context;
static libusb_device_handle *handle;
static uint8_t interface, endpoint;
static pa_io_event **pollio;

static void imon_handle_events(pa_mainloop_api *api, pa_io_event *e,
        int fd, pa_io_event_flags_t events, void *nill)
{
    struct timeval tv;
    memset(&tv, 0, sizeof(tv));
    libusb_handle_events_timeout(context, &tv);
}

int imon_open(pa_mainloop_api *api)
{
    libusb_device *device;
    struct libusb_config_descriptor *config;
    struct libusb_interface_descriptor *altsetting;
    const struct libusb_pollfd **pollfds;
    int r;

    sem_init(&lock, 0, LOCK_MAX);

    r = libusb_init(&context);
    assert(r == 0);
    handle = libusb_open_device_with_vid_pid(context,
            IMON_VENDOR, IMON_PRODUCT);
    assert(handle > 0);

    device = libusb_get_device(handle);
    assert(device);
    libusb_get_active_config_descriptor(device, &config);
    /* Assume the first interface and altsetting is correct */
    /* FIXME: figure out how why this was causing a warning */
    altsetting = (void*)&config->interface[0].altsetting[0];
    interface = altsetting->bInterfaceNumber;

    for (int i = 0; i < altsetting->bNumEndpoints; i++) {
        uint8_t ep = altsetting->endpoint[i].bEndpointAddress;

        if ((ep & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT) {
            endpoint = ep;
            break;
        }
    }

    libusb_free_config_descriptor(config);

    if (libusb_kernel_driver_active(handle, interface)) {
        r = libusb_detach_kernel_driver(handle, interface);
        assert(r == 0);
    }

    r = libusb_claim_interface(handle, interface);

    /* Register libusb into the pulse eventloop */
    pollfds = libusb_get_pollfds(context);
    for (int i = 0; ; i++) {
        int flags = 0;

        pollio = realloc(pollio, sizeof(void*) * (i+1));
        if (pollfds[i] == NULL) {
            pollio[i] = NULL;
            break;
        }

        if (pollfds[i]->events & POLLIN)
            flags |= PA_IO_EVENT_INPUT;
        if (pollfds[i]->events & POLLOUT)
            flags |= PA_IO_EVENT_OUTPUT;
        pollio[i] = api->io_new(api, pollfds[i]->fd, flags,
                imon_handle_events, NULL);
    }

    free(pollfds);

    return r == 0 ? 0 : -1;
}

void imon_close(pa_mainloop_api *api)
{
    int lock_val;

    for (int i = 0; pollio[i]; i++) {
        api->io_free(pollio[i]);
    }
    free(pollio);

    /* Wait for a slot to open */
    while (sem_trywait(&lock))
        libusb_handle_events(context);

    sem_post(&lock);
    imon_clear();

    /* Wait for all events to finish */
    while (sem_getvalue(&lock, &lock_val), lock_val != LOCK_MAX)
        libusb_handle_events(context);

    libusb_release_interface(handle, interface);
    libusb_close(handle);
    libusb_exit(context);
    handle = NULL;
    context = NULL;
}

static void imon_write_cb(struct libusb_transfer *tx_info)
{
    /* This randomly fails, but I don't know if there is any
     * proper way to really recover from it...
     * At this point all the packets have already been sent
     * so it's not like I can abort the rest of the write.
     * So we'll silently ignore this for now and see what
     * problems that causes...
    if (tx_info->status != LIBUSB_TRANSFER_COMPLETED) {
        assert(0);
    }
    */

    /* release lock if this was the last packet in a set */
    if (tx_info->user_data) {
        sem_post(&lock);
    }
}

void imon_write(const void *data, int length)
{
    uint8_t buf[42];

    assert(handle);

    if (sem_trywait(&lock)) {
        //printf("Skipping update...\n");
        return;
    }

    if (length > 32)
        length = 32;

    if (length)
        memcpy(buf, data, length);

    /* Pad end with spaces as needed */
    for (int i = length; i < 32; i++)
        buf[i] = ' ';

    /* Pad the last few bytes of the 5th packet with 0xFF */
    for (int i = 32; i < 35; i++)
        buf[i] = 0xFF;

    /* Fill in the magical last packet */
    memcpy(buf+35, END_PACKET, 7);

    /* Send the text is a series of 5 8-byte packets,
     * the first 7 bytes are data
     * the last byte is the sequence number that increments by 2 (why?)
     * The 6th packet is the data from END_PACKET (dunno what it means)
     */
    for (int i = 0; i < 6; i++) {
        struct libusb_transfer *tx_info = libusb_alloc_transfer(0);
        uint8_t *tx_buf = malloc(8);
        void *user_data;
        int r;

        assert(tx_info && tx_buf);

        memcpy(tx_buf, buf+(i*7), 7);
        tx_buf[7] = i*2;

        /* Since we only need one bit of information in the callback
         * use a pointer value of 0 for data, 1 for the end packet. */
        user_data = (i == 5) ? (void*)1UL : NULL;

        /* Write with a timeout of 0.1 second */
        libusb_fill_interrupt_transfer(tx_info, handle, endpoint,
                tx_buf, 8, imon_write_cb, user_data, 100);
        tx_info->flags = LIBUSB_TRANSFER_SHORT_NOT_OK |
                         LIBUSB_TRANSFER_FREE_BUFFER |
                         LIBUSB_TRANSFER_FREE_TRANSFER;
        r = libusb_submit_transfer(tx_info);
        assert(r == 0);
    }
}

void imon_clear()
{
    imon_write(NULL, 0);
}
