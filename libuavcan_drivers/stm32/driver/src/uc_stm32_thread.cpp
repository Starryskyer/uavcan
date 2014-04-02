/*
 * Copyright (C) 2014 Pavel Kirienko <pavel.kirienko@gmail.com>
 */

#include <uavcan_stm32/thread.hpp>

namespace uavcan_stm32
{

#if UAVCAN_STM32_CHIBIOS

bool Event::wait(uavcan::MonotonicDuration duration)
{
    msg_t ret = msg_t();
    if (duration.isZero())
    {
        sem_.waitTimeout(TIME_IMMEDIATE);
    }
    else
    {
        sem_.waitTimeout(US2ST(duration.toUSec()));
    }
    return ret == RDY_OK;
}

void Event::signal()
{
    sem_.signal();
}

void Event::signalFromInterrupt()
{
    sem_.signalI();
}

#endif

}