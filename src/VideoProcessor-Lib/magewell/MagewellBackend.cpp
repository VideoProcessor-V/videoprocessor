#include "pch.h"
#include "MagewellBackend.h"

#ifdef VP_NO_MAGEWELL
// CI build without the proprietary MWCapture SDK headers (/p:VpCiNoMagewell=true,
// MagewellSdk.props). The backend reports no devices; the default build is unchanged.
namespace
{
    class NoMagewellDiscoverer final : public ACaptureDeviceDiscoverer
    {
    public:
        explicit NoMagewellDiscoverer(ICaptureDeviceDiscovererCallback& callback)
            : ACaptureDeviceDiscoverer(callback) {}
        void Start() override {}
        void Stop() override {}
        HRESULT QueryInterface(REFIID iid, LPVOID* ppv) override
        {
            if (!ppv)
                return E_INVALIDARG;
            *ppv = nullptr;
            if (iid != IID_IUnknown)
                return E_NOINTERFACE;
            *ppv = this;
            AddRef();
            return S_OK;
        }
        ULONG AddRef() override { return ++m_refCount; }
        ULONG Release() override
        {
            const ULONG count = --m_refCount;
            if (count == 0)
                delete this;
            return count;
        }
    private:
        std::atomic<ULONG> m_refCount{ 0 };
    };

    std::atomic<bool> s_recoverFaults{ true };
}

namespace MagewellBackend
{
    CComPtr<ACaptureDeviceDiscoverer> CreateDiscoverer(ICaptureDeviceDiscovererCallback& callback)
    {
        return CComPtr<ACaptureDeviceDiscoverer>(new NoMagewellDiscoverer(callback));
    }
    void SetRecoverFaults(bool on) { s_recoverFaults = on; }
    bool RecoverFaults() { return s_recoverFaults; }
    CString UnavailableMessage(const CString& configuredDeviceName)
    {
        CString lower(configuredDeviceName);
        lower.MakeLower();
        if (lower.Find(TEXT("magewell")) < 0 && lower.Find(TEXT("pro capture")) < 0)
            return CString();
        return CString(TEXT("Magewell unavailable. This build was compiled without the Magewell backend."));
    }
    void AppendDeviceNames(std::vector<std::wstring>&) {}
    std::vector<std::wstring> ConnectionNames(const std::wstring&) { return {}; }
}
#else
#include "MagewellCaptureDeviceDiscoverer.h"
#include "MagewellSdkInstance.h"
#include "MagewellCaptureDevice.h"

namespace
{
    // Reuse the capture discoverer for the editor, so names, usable-card filters,
    // connector IDs and SDK lifetime cannot diverge between the two front ends.
    class DiscoveryQuery final : public ICaptureDeviceDiscovererCallback
    {
    public:
        std::vector<ACaptureDeviceComPtr> devices;
        void OnCaptureDeviceFound(ACaptureDeviceComPtr& device) override
        {
            devices.push_back(device);
        }
        void OnCaptureDeviceLost(ACaptureDeviceComPtr&) override {}
    };

    std::vector<ACaptureDeviceComPtr> QueryDevices()
    {
        DiscoveryQuery query;
        CComPtr<ACaptureDeviceDiscoverer> discovery = MagewellBackend::CreateDiscoverer(query);
        try
        {
            discovery->Start();
            discovery->Stop();
        }
        catch (const std::exception& error)
        {
            DebugLog::Log("Magewell configuration discovery unavailable: %s", error.what());
            return {};
        }
        return std::move(query.devices);
    }
}

namespace MagewellBackend
{
    CComPtr<ACaptureDeviceDiscoverer> CreateDiscoverer(ICaptureDeviceDiscovererCallback& callback)
    {
        return CComPtr<ACaptureDeviceDiscoverer>(new MagewellCaptureDeviceDiscoverer(callback));
    }

    void SetRecoverFaults(bool on)
    {
        MagewellCaptureDevice::SetRecoverFaults(on);
    }

    bool RecoverFaults()
    {
        return MagewellCaptureDevice::RecoverFaults();
    }

    CString UnavailableMessage(const CString& configuredDeviceName)
    {
        CString lower(configuredDeviceName);
        lower.MakeLower();
        if (lower.Find(TEXT("magewell")) < 0 && lower.Find(TEXT("pro capture")) < 0)
            return CString();
        const auto reason = MagewellSdkInstance::UnavailableReason();
        if (reason.empty())
            return CString();
        CString message;
        message.Format(TEXT("Magewell unavailable. %s"), reason.c_str());
        return message;
    }
    void AppendDeviceNames(std::vector<std::wstring>& names)
    {
        for (const auto& device : QueryDevices())
            names.emplace_back(device->GetName().GetString());
    }

    std::vector<std::wstring> ConnectionNames(const std::wstring& deviceName)
    {
        std::vector<std::wstring> names;
        for (const auto& device : QueryDevices())
        {
            if (_wcsicmp(device->GetName().GetString(), deviceName.c_str()) != 0)
                continue;
            for (const auto& input : device->SupportedCaptureInputs())
                names.emplace_back(input.name.GetString());
            break;
        }
        return names;
    }
}
#endif // VP_NO_MAGEWELL
