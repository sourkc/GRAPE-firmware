#include "tusb.h"

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t buffer_size)
{
    (void)itf;
    (void)buffer;
    (void)buffer_size;
}
