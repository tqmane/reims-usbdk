// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Public.h"
#include "MemoryBuffer.h"
#include "ConfigurationDescriptor.h"

// KMDF's interface/pipe objects describe only its first configuration. After
// explicit selection, keep the USB stack's returned handles instead of using
// those stale framework pipe objects. All mutations require drained I/O.
class CUsbDkRawConfiguration : public CAllocatable<USBDK_NON_PAGED_POOL, 'CFHR'>
{
public:
    explicit CUsbDkRawConfiguration(WDFUSBDEVICE Device) : m_Device(Device) {}
    ~CUsbDkRawConfiguration();
    NTSTATUS Prepare(UCHAR Value);
    NTSTATUS Select();
    NTSTATUS SetAltSetting(ULONG64 InterfaceIndex, ULONG64 SettingIndex);
    NTSTATUS ClearPipe(ULONG64 Address, bool Reset);
    NTSTATUS Reset();
    NTSTATUS FormatTransfer(WDFREQUEST Request, ULONG64 Address, USB_DK_TRANSFER_TYPE Type,
                           bool Read, WDFMEMORY Buffer, PULONG64 PacketSizes,
                           size_t PacketCount, WDFMEMORY &UrbMemory);

private:
    NTSTATUS Send(PURB Urb);
    NTSTATUS ReadDescriptor(UCHAR Index, PVOID Buffer, USHORT Size);
    PUSBD_INTERFACE_INFORMATION Interface(USHORT Index) const;
    PUSBD_PIPE_INFORMATION FindPipe(ULONG64 Address) const;
    bool ValidateInterface(PUSBD_INTERFACE_INFORMATION Info, size_t Capacity,
                           const USB_INTERFACE_DESCRIPTOR &Descriptor) const;

    WDFUSBDEVICE m_Device;
    CWdmMemoryBuffer m_Descriptor;
    UsbDkConfigurationLayout m_Layout;
    USBD_INTERFACE_LIST_ENTRY m_Entries[256] = {};
    PURB m_Config = nullptr;
    PURB m_Alternates[256] = {};
    bool m_Ready = false;
    CUsbDkRawConfiguration(const CUsbDkRawConfiguration&) = delete;
    CUsbDkRawConfiguration& operator=(const CUsbDkRawConfiguration&) = delete;
};
