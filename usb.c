#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <libusb.h>

#define IMON_VENDOR 0x15c2
#define IMON_PRODUCT 0xffdc
static uint8_t END_PACKET[] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff };

static libusb_context *context;
static libusb_device_handle *handle;
static uint8_t interface, endpoint;

int imon_open()
{
    libusb_device *device;
    struct libusb_config_descriptor *config;
    struct libusb_interface_descriptor *altsetting;
    int r;

    r = libusb_init(&context);
    assert(r == 0);
    handle = libusb_open_device_with_vid_pid(context,
            IMON_VENDOR, IMON_PRODUCT);
    assert(handle > 0);

    /*
    r = libusb_reset_device(handle);
    fprintf(stderr, "%d\n", r);
    assert(r == 0);
    */

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
    assert(r == 0);

    return 0;
    /* TODO: handle errors */
}

int imon_write(const void *data, int length) {
    uint8_t buf[42];
    uint8_t tx[8];
    int r, t;

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
        memcpy(tx, buf+(i*7), 7);
        tx[7] = i*2;

        /* Write with a timeout of 1 second */
        r = libusb_interrupt_transfer(handle, endpoint, tx, 8, &t, 100);
        fprintf(stderr, "status %d, bytes %d\n", r, t);
        assert(r == 0);
        assert(t == 8);
    }

    return 0;
}

void imon_clear()
{
    imon_write(NULL, 0);
}

void imon_close()
{
    assert(handle);
    libusb_release_interface(handle, interface);
    libusb_close(handle);
    handle = NULL;
}

int main(int argc, char* argv[])
{
    char test[] = "1       2       3       4";

    imon_open();
    //imon_write(test, strlen(test));
    imon_clear();
    imon_close();

    return 0;
}
