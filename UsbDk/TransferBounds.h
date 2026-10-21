// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>

inline bool UsbDkActualTransferLength(bool IoSucceeded, bool UsbSucceeded,
                                     size_t Reported, size_t Capacity, size_t &Actual)
{
    Actual = 0;
    if (!IoSucceeded || !UsbSucceeded)
    {
        return true;
    }
    if (Reported > Capacity)
    {
        return false;
    }
    Actual = Reported;
    return true;
}
