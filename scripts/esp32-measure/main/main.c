/*
 * Minimal app_main for nanortc size measurement.
 * We export sizeof(nanortc_t) as a global so it can be read from the ELF.
 */
#include "nanortc.h"

__attribute__((used)) const volatile uint32_t nanortc_sizeof = (uint32_t)sizeof(nanortc_t);

void app_main(void)
{
    (void)nanortc_sizeof;
}
