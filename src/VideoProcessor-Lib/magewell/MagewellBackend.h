#pragma once

// The complete host/configuration integration boundary. Vendor SDK headers and
// all capture/conversion implementation remain private to the magewell folder.
#include <ACaptureDeviceDiscoverer.h>
#include <string>
#include <vector>

namespace MagewellBackend
{
    CComPtr<ACaptureDeviceDiscoverer> CreateDiscoverer(ICaptureDeviceDiscovererCallback& callback);
    // Empty unless a saved Magewell selection needs an actionable runtime explanation.
    CString UnavailableMessage(const CString& configuredDeviceName);
    void AppendDeviceNames(std::vector<std::wstring>& names);
    std::vector<std::wstring> ConnectionNames(const std::wstring& deviceName);
    // [general] magewell_recover_faults (restore switch, bug hunt 2026-10-17,
    // default on): recoverable capture faults restart the device internally.
    // Off = they end capture (a manual Restart), as before. Process-wide.
    void SetRecoverFaults(bool on);
    bool RecoverFaults();
}