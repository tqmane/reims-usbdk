// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>

// Checked before any configuration is sent to the USB stack. Offsets index the
// caller-owned descriptor; no pointers survive its allocation.
struct UsbDkConfigurationLayout
{
    unsigned short InterfaceOffsets[256] = {};
    unsigned short Count = 0;
};

inline bool UsbDkInterfaceEndpointMask(const unsigned char *Data, size_t Size,
                                      size_t Offset, unsigned &Mask)
{
    Mask = 0;
    if (Data == nullptr || Offset >= Size || Size - Offset < 9 || Data[Offset] < 9 ||
        Data[Offset] > Size - Offset || Data[Offset + 1] != 4)
    {
        return false;
    }
    for (Offset += Data[Offset]; Offset < Size; Offset += Data[Offset])
    {
        if (Size - Offset < 2 || Data[Offset] < 2 || Data[Offset] > Size - Offset)
        {
            return false;
        }
        if (Data[Offset + 1] == 4)
        {
            break;
        }
        if (Data[Offset + 1] == 5)
        {
            if (Data[Offset] < 7 || (Data[Offset + 2] & 0x70) || !(Data[Offset + 2] & 15))
            {
                return false;
            }
            unsigned bit = 1u << ((Data[Offset + 2] & 15) + ((Data[Offset + 2] & 0x80) ? 16 : 0));
            if (Mask & bit)
            {
                return false;
            }
            Mask |= bit;
        }
    }
    return true;
}

inline bool UsbDkEndpointMasksCompatible(unsigned Proposed, unsigned OtherActive)
{
    return (Proposed & OtherActive) == 0;
}

inline bool UsbDkParseConfiguration(const unsigned char *data, size_t size,
                                   unsigned char value, UsbDkConfigurationLayout &layout)
{
    layout = {};
    if (data == nullptr || size < 9 || size > 65535 || data[0] < 9 ||
        data[1] != 2 || (data[2] | (data[3] << 8)) != size ||
        data[5] != value || value == 0 || data[4] == 0)
    {
        return false;
    }
    bool numbers[256] = {};
    bool altZero[256] = {};
    unsigned count = 0, endpoints = 0, expected = 0;
    unsigned endpointMask = 0, defaultEndpointMask = 0;
    bool inInterface = false, isDefault = false;
    for (size_t offset = data[0]; offset < size;)
    {
        if (size - offset < 2 || data[offset] < 2 || data[offset] > size - offset)
        {
            return false;
        }
        const auto *descriptor = data + offset;
        if (descriptor[1] == 4)
        {
            if (descriptor[0] < 9 || (inInterface && endpoints != expected))
            {
                return false;
            }
            unsigned number = descriptor[2];
            if (!numbers[number])
            {
                numbers[number] = true;
                ++count;
            }
            isDefault = descriptor[3] == 0;
            if (isDefault)
            {
                if (altZero[number])
                {
                    return false;
                }
                altZero[number] = true;
                layout.InterfaceOffsets[layout.Count++] = static_cast<unsigned short>(offset);
            }
            endpoints = 0;
            expected = descriptor[4];
            endpointMask = 0;
            inInterface = true;
        }
        else if (descriptor[1] == 5)
        {
            if (!inInterface || descriptor[0] < 7 || (descriptor[2] & 0x70) ||
                !(descriptor[2] & 15) || !(descriptor[3] & 3))
            {
                return false;
            }
            unsigned bit = 1u << ((descriptor[2] & 15) + ((descriptor[2] & 0x80) ? 16 : 0));
            if ((endpointMask & bit) || (isDefault && (defaultEndpointMask & bit)))
            {
                return false;
            }
            endpointMask |= bit;
            if (isDefault)
            {
                defaultEndpointMask |= bit;
            }
            ++endpoints;
        }
        else if (descriptor[1] == 2)
        {
            return false;
        }
        offset += descriptor[0];
    }
    return inInterface && endpoints == expected && count == data[4] && layout.Count == count;
}
