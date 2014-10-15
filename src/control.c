#include "control.h"

#include "relay_threads.h"

static volatile uint32_t __control = 0;

void set_control_bits(uint32_t v)
{
    RELAY_ATOMIC_OR(__control, v);
}

void unset_control_bits(uint32_t v)
{
    v = ~v;
    RELAY_ATOMIC_AND(__control, v);
}

void set_stopped(void)
{
    set_control_bits(RELAY_STOP);
}

uint32_t get_control_val(void)
{
    return RELAY_ATOMIC_READ(__control);
}

uint32_t not_stopped(void)
{
    uint32_t v = RELAY_ATOMIC_READ(__control);
    return (v & RELAY_STOP) == 0;
}

uint32_t is_stopped(void)
{
    uint32_t v = RELAY_ATOMIC_READ(__control);
    return (v & RELAY_STOP) == RELAY_STOP;
}