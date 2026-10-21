// SPDX-License-Identifier: Apache-2.0
// Link the actual RedirectorAccess/DriverFile objects. Only their four Win32
// I/O imports are replaced; this executable cannot open/capture a USB device.
#include "../UsbDkHelper/stdafx.h"
#include "../UsbDkHelper/RedirectorAccess.h"
#include "../UsbDk/Public.h"
#include <cassert>

static int mode, calls, setCalls, eventCloses;
static DWORD completionBytes;
static BOOL WINAPI fakeIoctl(HANDLE device, DWORD code, LPVOID input, DWORD inputSize,
                            LPVOID output, DWORD outputSize, LPDWORD bytes, LPOVERLAPPED overlapped)
{
    assert(device == reinterpret_cast<HANDLE>(42) && overlapped != nullptr);
    assert(overlapped->Internal == 0 && overlapped->InternalHigh == 0 && overlapped->Offset == 0);
    ++calls;
    if (mode == 2)
    {
        SetLastError(ERROR_INVALID_FUNCTION); // Original signed 1.0.22 SYS.
        return FALSE;
    }
    if (code == IOCTL_USBDK_DEVICE_GET_CAPABILITIES)
    {
        assert(inputSize == 0 && outputSize == 16);
        auto cap = static_cast<PUSB_DK_REDIRECTOR_CAPABILITIES>(output);
        *cap = {16, USB_DK_REDIRECTOR_PROTOCOL_VERSION, USB_DK_CAP_SET_CONFIGURATION, 0};
        completionBytes = mode == 3 ? 0 : 16;
        if (mode == 4) cap->ProtocolVersion = 99;
        if (mode == 5) cap->Capabilities = 0;
        if (mode == 6) cap->Reserved = 1;
    }
    else
    {
        assert(code == IOCTL_USBDK_DEVICE_SET_CONFIGURATION && inputSize == 16 && outputSize == 0);
        auto request = static_cast<PUSB_DK_SET_CONFIGURATION>(input);
        assert(request->Size == 16 && request->ProtocolVersion == 1 && request->ConfigurationValue == 3);
        ++setCalls;
        completionBytes = 0;
    }
    if (mode != 0)
    {
        SetLastError(ERROR_IO_PENDING);
        return FALSE;
    }
    if (bytes != nullptr) *bytes = completionBytes;
    return TRUE;
}

static BOOL WINAPI fakeResult(HANDLE, LPOVERLAPPED, LPDWORD bytes, BOOL wait)
{
    assert(wait);
    if (mode == 7)
    {
        SetLastError(ERROR_OPERATION_ABORTED);
        return FALSE;
    }
    *bytes = completionBytes;
    return TRUE;
}
static HANDLE WINAPI fakeEvent(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR)
{
    return reinterpret_cast<HANDLE>(43);
}
static BOOL WINAPI fakeClose(HANDLE handle)
{
    assert(handle == reinterpret_cast<HANDLE>(42) || handle == reinterpret_cast<HANDLE>(43));
    if (handle == reinterpret_cast<HANDLE>(43)) ++eventCloses;
    return TRUE;
}

extern "C"
{
    decltype(&DeviceIoControl) __imp_DeviceIoControl = fakeIoctl;
    decltype(&GetOverlappedResult) __imp_GetOverlappedResult = fakeResult;
    decltype(&CreateEventW) __imp_CreateEventW = fakeEvent;
    decltype(&CloseHandle) __imp_CloseHandle = fakeClose;
}

int main()
{
    static_assert(sizeof(USB_DK_REDIRECTOR_CAPABILITIES) == 16, "capability ABI");
    static_assert(sizeof(USB_DK_SET_CONFIGURATION) == 16, "configuration ABI");
    UsbDkRedirectorAccess access(reinterpret_cast<HANDLE>(42));
    for (mode = 0; mode <= 7; ++mode)
    {
        calls = setCalls = eventCloses = 0;
        bool success = false;
        try { access.SetConfiguration(3); success = true; }
        catch (const UsbDkW32ErrorException &) {}
        assert(success == (mode == 0 || mode == 1));
        assert(setCalls == (success ? 1 : 0));
        assert(calls == (success ? 2 : 1) && eventCloses == calls);
    }
    calls = 0;
    try { access.SetConfiguration(256); assert(false); }
    catch (const UsbDkW32ErrorException &error) { assert(error.GetErrorCode() == ERROR_INVALID_PARAMETER); }
    assert(calls == 0);
    puts("actual helper sync/async capability bytes, original driver refusal, malformed response and failure: PASS");
}
