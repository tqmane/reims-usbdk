// SPDX-License-Identifier: Apache-2.0
#include "stdafx.h"
#include "RawConfiguration.h"

CUsbDkRawConfiguration::~CUsbDkRawConfiguration()
{
    for (auto urb : m_Alternates)
    {
        if (urb != nullptr)
        {
            ExFreePool(urb);
        }
    }
    if (m_Config != nullptr)
    {
        ExFreePool(m_Config);
    }
}

NTSTATUS CUsbDkRawConfiguration::Send(PURB Urb)
{
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(5));
    auto status = WdfUsbTargetDeviceSendUrbSynchronously(m_Device, WDF_NO_HANDLE, &options, Urb);
    return NT_SUCCESS(status) && !USBD_SUCCESS(Urb->UrbHeader.Status) ? STATUS_IO_DEVICE_ERROR : status;
}

NTSTATUS CUsbDkRawConfiguration::ReadDescriptor(UCHAR Index, PVOID Buffer, USHORT Size)
{
    URB urb = {};
    UsbBuildGetDescriptorRequest(&urb, sizeof(_URB_CONTROL_DESCRIPTOR_REQUEST),
                                USB_CONFIGURATION_DESCRIPTOR_TYPE, Index, 0,
                                Buffer, nullptr, Size, nullptr);
    auto status = Send(&urb);
    if (NT_SUCCESS(status) && urb.UrbControlDescriptorRequest.TransferBufferLength != Size)
    {
        return STATUS_DEVICE_DATA_ERROR;
    }
    return status;
}

NTSTATUS CUsbDkRawConfiguration::Prepare(UCHAR Value)
{
    if (Value == 0)
    {
        return STATUS_SUCCESS;
    }
    USB_DEVICE_DESCRIPTOR device;
    WdfUsbTargetDeviceGetDeviceDescriptor(m_Device, &device);
    for (UCHAR index = 0; index < device.bNumConfigurations; ++index)
    {
        USB_CONFIGURATION_DESCRIPTOR header = {};
        auto status = ReadDescriptor(index, &header, sizeof(header));
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        if (header.bDescriptorType != USB_CONFIGURATION_DESCRIPTOR_TYPE ||
            header.bLength < sizeof(header) || header.wTotalLength < header.bLength)
        {
            return STATUS_DEVICE_DATA_ERROR;
        }
        if (header.bConfigurationValue != Value)
        {
            continue;
        }
        status = m_Descriptor.Create(header.wTotalLength, USBDK_NON_PAGED_POOL);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        status = ReadDescriptor(index, m_Descriptor.Ptr(), header.wTotalLength);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        auto bytes = static_cast<UCHAR*>(m_Descriptor.Ptr());
        if (!UsbDkParseConfiguration(bytes, m_Descriptor.Size(), Value, m_Layout))
        {
            return STATUS_DEVICE_DATA_ERROR;
        }
        for (USHORT i = 0; i < m_Layout.Count; ++i)
        {
            m_Entries[i].InterfaceDescriptor = reinterpret_cast<PUSB_INTERFACE_DESCRIPTOR>(bytes + m_Layout.InterfaceOffsets[i]);
        }
        m_Config = USBD_CreateConfigurationRequestEx(static_cast<PUSB_CONFIGURATION_DESCRIPTOR>(m_Descriptor.Ptr()), m_Entries);
        return m_Config == nullptr ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

bool CUsbDkRawConfiguration::ValidateInterface(PUSBD_INTERFACE_INFORMATION Info, size_t Capacity,
                                              const USB_INTERFACE_DESCRIPTOR &Descriptor) const
{
    size_t bytes = FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) +
                   Descriptor.bNumEndpoints * sizeof(USBD_PIPE_INFORMATION);
    if (Capacity < bytes || Info->Length != bytes || Info->InterfaceNumber != Descriptor.bInterfaceNumber ||
        Info->AlternateSetting != Descriptor.bAlternateSetting || Info->NumberOfPipes != Descriptor.bNumEndpoints)
    {
        return false;
    }
    ULONG seen = 0;
    for (ULONG i = 0; i < Info->NumberOfPipes; ++i)
    {
        auto &pipe = Info->Pipes[i];
        if ((pipe.EndpointAddress & 0x70) || !(pipe.EndpointAddress & 15) || pipe.PipeHandle == nullptr ||
            pipe.PipeType < UsbdPipeTypeIsochronous || pipe.PipeType > UsbdPipeTypeInterrupt)
        {
            return false;
        }
        ULONG bit = 1u << ((pipe.EndpointAddress & 15) + ((pipe.EndpointAddress & 0x80) ? 16 : 0));
        if (seen & bit)
        {
            return false;
        }
        seen |= bit;
    }
    return true;
}

NTSTATUS CUsbDkRawConfiguration::Select()
{
    if (m_Config == nullptr)
    {
        URB urb = {};
        UsbBuildSelectConfigurationRequest(&urb, sizeof(_URB_SELECT_CONFIGURATION), nullptr);
        return Send(&urb);
    }
    auto capacity = m_Config->UrbHeader.Length;
    auto status = Send(m_Config);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    auto begin = reinterpret_cast<ULONG_PTR>(m_Config);
    auto end = begin + capacity;
    for (USHORT i = 0; i < m_Layout.Count; ++i)
    {
        auto info = m_Entries[i].Interface;
        auto address = reinterpret_cast<ULONG_PTR>(info);
        if (address < begin || address > end ||
            !ValidateInterface(info, end - address, *m_Entries[i].InterfaceDescriptor))
        {
            return STATUS_DEVICE_DATA_ERROR;
        }
    }
    m_Ready = true;
    return STATUS_SUCCESS;
}

PUSBD_INTERFACE_INFORMATION CUsbDkRawConfiguration::Interface(USHORT Index) const
{
    return m_Alternates[Index] != nullptr ? &m_Alternates[Index]->UrbSelectInterface.Interface : m_Entries[Index].Interface;
}

PUSBD_PIPE_INFORMATION CUsbDkRawConfiguration::FindPipe(ULONG64 Address) const
{
    if (!m_Ready || Address > 255)
    {
        return nullptr;
    }
    for (USHORT i = 0; i < m_Layout.Count; ++i)
    {
        auto info = Interface(i);
        for (ULONG p = 0; p < info->NumberOfPipes; ++p)
        {
            if (info->Pipes[p].EndpointAddress == Address)
            {
                return &info->Pipes[p];
            }
        }
    }
    return nullptr;
}

NTSTATUS CUsbDkRawConfiguration::SetAltSetting(ULONG64 InterfaceIndex, ULONG64 SettingIndex)
{
    if (!m_Ready || InterfaceIndex >= m_Layout.Count || SettingIndex > 255)
    {
        return STATUS_INVALID_PARAMETER;
    }
    auto data = static_cast<UCHAR*>(m_Descriptor.Ptr());
    auto number = m_Entries[InterfaceIndex].InterfaceDescriptor->bInterfaceNumber;
    PUSB_INTERFACE_DESCRIPTOR selected = nullptr;
    ULONG64 ordinal = 0;
    for (size_t offset = data[0]; offset < m_Descriptor.Size(); offset += data[offset])
    {
        if (data[offset + 1] == USB_INTERFACE_DESCRIPTOR_TYPE && data[offset + 2] == number)
        {
            if (ordinal++ == SettingIndex)
            {
                selected = reinterpret_cast<PUSB_INTERFACE_DESCRIPTOR>(data + offset);
                break;
            }
        }
    }
    if (selected == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }
    unsigned proposedMask;
    if (!UsbDkInterfaceEndpointMask(data, m_Descriptor.Size(), reinterpret_cast<UCHAR*>(selected) - data, proposedMask))
    {
        return STATUS_DEVICE_DATA_ERROR;
    }
    unsigned otherMask = 0;
    for (USHORT i = 0; i < m_Layout.Count; ++i)
    {
        if (i == InterfaceIndex)
        {
            continue;
        }
        auto info = Interface(i);
        for (ULONG p = 0; p < info->NumberOfPipes; ++p)
        {
            auto address = info->Pipes[p].EndpointAddress;
            otherMask |= 1u << ((address & 15) + ((address & 0x80) ? 16 : 0));
        }
    }
    if (!UsbDkEndpointMasksCompatible(proposedMask, otherMask))
    {
        return STATUS_INVALID_PARAMETER;
    }
    size_t size = FIELD_OFFSET(_URB_SELECT_INTERFACE, Interface.Pipes) +
                  selected->bNumEndpoints * sizeof(USBD_PIPE_INFORMATION);
    auto urb = static_cast<PURB>(ExAllocatePoolWithTag(USBDK_NON_PAGED_POOL, size, 'CFHR'));
    if (urb == nullptr)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(urb, size);
    UsbBuildSelectInterfaceRequest(urb, static_cast<USHORT>(size), m_Config->UrbSelectConfiguration.ConfigurationHandle,
                                  number, selected->bAlternateSetting);
    urb->UrbSelectInterface.Interface.Length = static_cast<USHORT>(size - FIELD_OFFSET(_URB_SELECT_INTERFACE, Interface));
    auto status = Send(urb);
    if (NT_SUCCESS(status) && !ValidateInterface(&urb->UrbSelectInterface.Interface,
                                                size - FIELD_OFFSET(_URB_SELECT_INTERFACE, Interface), *selected))
    {
        status = STATUS_DEVICE_DATA_ERROR;
    }
    if (!NT_SUCCESS(status))
    {
        // A failed select may already have invalidated old pipe handles.
        m_Ready = false;
        ExFreePool(urb);
        return status;
    }
    if (m_Alternates[InterfaceIndex] != nullptr)
    {
        ExFreePool(m_Alternates[InterfaceIndex]);
    }
    m_Alternates[InterfaceIndex] = urb;
    return STATUS_SUCCESS;
}

NTSTATUS CUsbDkRawConfiguration::ClearPipe(ULONG64 Address, bool Reset)
{
    auto pipe = FindPipe(Address);
    if (pipe == nullptr)
    {
        return STATUS_NOT_FOUND;
    }
    URB urb = {};
    urb.UrbPipeRequest.Hdr.Length = sizeof(_URB_PIPE_REQUEST);
    urb.UrbPipeRequest.Hdr.Function = Reset ? URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL : URB_FUNCTION_ABORT_PIPE;
    urb.UrbPipeRequest.PipeHandle = pipe->PipeHandle;
    auto status = Send(&urb);
    if (!NT_SUCCESS(status))
    {
        m_Ready = false;
    }
    return status;
}

NTSTATUS CUsbDkRawConfiguration::Reset()
{
    if (!m_Ready)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    NTSTATUS result = STATUS_SUCCESS;
    for (USHORT i = 0; i < m_Layout.Count; ++i)
    {
        auto info = Interface(i);
        for (ULONG p = 0; p < info->NumberOfPipes; ++p)
        {
            auto status = ClearPipe(info->Pipes[p].EndpointAddress, false);
            if (NT_SUCCESS(status))
            {
                status = ClearPipe(info->Pipes[p].EndpointAddress, true);
            }
            if (!NT_SUCCESS(status))
            {
                result = status;
            }
        }
    }
    return result;
}

NTSTATUS CUsbDkRawConfiguration::FormatTransfer(WDFREQUEST Request, ULONG64 Address, USB_DK_TRANSFER_TYPE Type,
                                                bool Read, WDFMEMORY Buffer, PULONG64 PacketSizes,
                                                size_t PacketCount, WDFMEMORY &UrbMemory)
{
    auto pipe = FindPipe(Address);
    if (pipe == nullptr || !!(Address & 0x80) != Read)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if ((Type == BulkTransferType && pipe->PipeType != UsbdPipeTypeBulk) ||
        (Type == InterruptTransferType && pipe->PipeType != UsbdPipeTypeInterrupt) ||
        (Type == IsochronousTransferType && pipe->PipeType != UsbdPipeTypeIsochronous) ||
        (Type != BulkTransferType && Type != InterruptTransferType && Type != IsochronousTransferType))
    {
        return STATUS_INVALID_PARAMETER;
    }
    CPreAllocatedWdfMemoryBuffer buffer(Buffer);
    if (buffer.Size() > MAXULONG || (Type == IsochronousTransferType &&
        (PacketSizes == nullptr || PacketCount == 0 || PacketCount > (MAXUSHORT - sizeof(URB)) / sizeof(USBD_ISO_PACKET_DESCRIPTOR))))
    {
        return STATUS_INVALID_PARAMETER;
    }
    PURB urb;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Request;
    auto status = Type == IsochronousTransferType ?
        WdfUsbTargetDeviceCreateIsochUrb(m_Device, &attributes, static_cast<ULONG>(PacketCount), &UrbMemory, &urb) :
        WdfUsbTargetDeviceCreateUrb(m_Device, &attributes, &UrbMemory, &urb);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    ULONG flags = (Read ? USBD_TRANSFER_DIRECTION_IN : USBD_TRANSFER_DIRECTION_OUT) | USBD_SHORT_TRANSFER_OK;
    if (Type != IsochronousTransferType)
    {
        UsbBuildInterruptOrBulkTransferRequest(urb, sizeof(_URB_BULK_OR_INTERRUPT_TRANSFER), pipe->PipeHandle,
                                               buffer.Ptr(), nullptr, static_cast<ULONG>(buffer.Size()), flags, nullptr);
    }
    else
    {
        auto &iso = urb->UrbIsochronousTransfer;
        iso.Hdr.Length = static_cast<USHORT>(GET_ISO_URB_SIZE(PacketCount));
        iso.Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
        iso.PipeHandle = pipe->PipeHandle;
        iso.TransferFlags = flags | USBD_START_ISO_TRANSFER_ASAP;
        iso.TransferBuffer = buffer.Ptr();
        iso.TransferBufferMDL = nullptr;
        iso.NumberOfPackets = static_cast<ULONG>(PacketCount);
        size_t offset = 0;
        for (size_t i = 0; i < PacketCount; ++i)
        {
            if (PacketSizes[i] > buffer.Size() - offset)
            {
                return STATUS_INVALID_PARAMETER;
            }
            iso.IsoPacket[i].Offset = static_cast<ULONG>(offset);
            iso.IsoPacket[i].Length = Read ? 0 : static_cast<ULONG>(PacketSizes[i]);
            offset += static_cast<size_t>(PacketSizes[i]);
        }
        iso.TransferBufferLength = static_cast<ULONG>(offset);
    }
    return WdfUsbTargetDeviceFormatRequestForUrb(m_Device, Request, UrbMemory, nullptr);
}
