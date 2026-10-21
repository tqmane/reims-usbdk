# Additive configuration-selection protocol (development)

This branch starts at UsbDk v1.00-22 (`84752c30037956149d53b498005b5368616f9257`).
It preserves the existing exports and default first-configuration path.
It adds `UsbDk_GetRedirectorCapabilities` and
`UsbDk_SetConfiguration(HANDLE, ULONG64)`. The latter takes the USB
`bConfigurationValue` (0 deconfigures), not a descriptor index.

The redirector capability IOCTL uses function 0x958, buffered/read access,
input length 0 and output length 16. Its four ULONG fields are Size (16),
ProtocolVersion (1), Capabilities (SET_CONFIGURATION bit 1) and Reserved (0).
SET_CONFIGURATION uses function 0x959, buffered/write access, input length 16
and output length 0: ULONG Size (16), ULONG ProtocolVersion (1), ULONG64
ConfigurationValue (0..255). The capability handshake binds these new
operations to the same file object. A different file object is refused.

The new helper queries and validates capabilities before selection. The
original signed 1.0.22 SYS does not implement these IOCTLs and is refused;
success is never inferred from its file version. The corresponding libusb
integration must return NOT_SUPPORTED for unsupported configuration changes
instead of treating the original driver's no-op as success. Existing uses of
the original driver and first configuration remain available.

KMDF's normal configuration selector supports only its first configuration.
Explicit selection therefore uses a real SELECT_CONFIGURATION URB and keeps
the USB stack's returned configuration/interface/pipe handles. Descriptor
bounds, required alternate-zero settings and active endpoint uniqueness are
checked first. Once physical selection is attempted, a failure does not
revive stale WDF pipe handles. Alternate selection, pipe abort/reset and data
URBs use the new handles. Failed USB completions report zero actual data.

The data queue stops accepting requests before sent transfers (including
control transfers) are cancelled and awaited. Queue purge completion precedes
replacement of pipe state. Request-owned WDF memory keeps raw asynchronous
URBs and locked data alive until completion. Close quiesces the same path.

## CPU-only build and fixtures

On Windows x64, use existing Visual C++ tools, SDK 10.0.26100.0 and an unpacked
official WDK NuGet package. Both output locations are caller-selected and must
be outside the source checkout. These scripts install and load nothing:

```powershell
./Tools/build-local-unsigned.ps1 -WdkRoot <unpacked-WDK> -Output <build-folder>
./Tools/test-configuration.ps1 -WdkRoot <unpacked-WDK> -BuildOutput <build-folder>
```

The fixtures cover authored descriptor/truncation cases, ownership refusal,
queue/target quiescence ordering, alternate endpoint collision and completion
length bounds. The helper fixture links the actual helper objects and replaces
their Win32 I/O imports: synchronous/asynchronous capability replies, short
byte counts, cancellation, unsupported and malformed replies are exercised
without a driver handle. This does not prove kernel scheduling on hardware.

The resulting SYS/DLL are **unsigned development artifacts**. Production
signing, HVCI compatibility, real device capture, mode re-enumeration,
configuration/data I/O and restoration to the original driver remain
unqualified. No Manager selection is shipped by this phase. In particular,
the observed phone vendor-mode failure preceded SET_CONFIGURATION; this
source change alone is not evidence that the phone works.
