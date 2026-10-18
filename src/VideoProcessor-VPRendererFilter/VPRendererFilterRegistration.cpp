#include "VPRendererFilterRegistration.h"

#include "VPRendererFilterMediaTypes.h"

#include <objbase.h>

#include <cwchar>


namespace VPRendererFilter
{
	const CLSID kClsidVPRendererFilter = { 0x54dea624, 0xd69c, 0x43bf,
		{ 0xba, 0xb2, 0x6e, 0x04, 0xd5, 0x1f, 0x1b, 0x95 } };
	const wchar_t* const kFilterName = L"VP Renderer";

	namespace
	{
		// Defined locally so the CPU tests need no strmiids symbols.
		const CLSID kClsidLegacyAmFilterCategory = { 0x083863f1, 0x70de, 0x11d0,
			{ 0xbd, 0x40, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86 } };
		const CLSID kClsidFilterMapper2 = { 0xcda42200, 0xbd88, 0x11d0,
			{ 0xbd, 0x4e, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86 } };

		HRESULT FromWin32(LSTATUS status)
		{
			return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
		}

		HRESULT SetStringValue(HKEY key, const wchar_t* name, const std::wstring& value)
		{
			return FromWin32(RegSetValueExW(key, name, 0, REG_SZ,
				reinterpret_cast<const BYTE*>(value.c_str()),
				static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))));
		}

		HRESULT CreateKeyWithDefault(const std::wstring& path, const std::wstring& value,
			HKEY* opened = nullptr)
		{
			HKEY key = nullptr;
			HRESULT hr = FromWin32(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0,
				nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr));
			if (FAILED(hr))
				return hr;
			hr = SetStringValue(key, nullptr, value);
			if (SUCCEEDED(hr) && opened)
				*opened = key;
			else
				RegCloseKey(key);
			return hr;
		}

		// Deletes HKCU\path only when it has neither subkeys nor values.
		void DeleteKeyIfEmpty(const std::wstring& path)
		{
			HKEY key = nullptr;
			if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
				return;
			DWORD subkeys = 0, values = 0;
			const LSTATUS status = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys,
				nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
			RegCloseKey(key);
			if (status == ERROR_SUCCESS && subkeys == 0 && values == 0)
				RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
		}

		// COM for the duration of a registration call (regsvr32 and MPC have
		// usually initialized it already; then this is a no-op pair).
		class ComScope
		{
		public:
			ComScope() : m_hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
			~ComScope() { if (SUCCEEDED(m_hr)) CoUninitialize(); }
		private:
			HRESULT m_hr;
		};

		// Calls fn(mapper) with HKEY_CLASSES_ROOT redirected to
		// HKCU\Software\Classes. The mapper is created before the redirect so
		// its own activation is unaffected.
		template<typename Fn>
		HRESULT WithUserClassesMapper(Fn fn)
		{
			ComScope com;
			IFilterMapper2* mapper = nullptr;
			HRESULT hr = CoCreateInstance(kClsidFilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
				__uuidof(IFilterMapper2), reinterpret_cast<void**>(&mapper));
			if (FAILED(hr))
				return hr;

			HKEY classes = nullptr;
			hr = FromWin32(RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, nullptr,
				REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &classes, nullptr));
			if (SUCCEEDED(hr))
			{
				hr = FromWin32(RegOverridePredefKey(HKEY_CLASSES_ROOT, classes));
				if (SUCCEEDED(hr))
				{
					hr = fn(mapper);
					RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
				}
				RegCloseKey(classes);
			}
			mapper->Release();
			return hr;
		}
	}


	std::wstring GuidString(const GUID& g)
	{
		wchar_t text[40];
		swprintf_s(text, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
			g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
			g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
		return text;
	}

	std::wstring ClsidKeyPath()
	{
		return L"Software\\Classes\\CLSID\\" + GuidString(kClsidVPRendererFilter);
	}

	std::wstring InprocServer32KeyPath()
	{
		return ClsidKeyPath() + L"\\InprocServer32";
	}

	std::wstring LegacyCategoryKeyPath()
	{
		return L"Software\\Classes\\CLSID\\" + GuidString(kClsidLegacyAmFilterCategory);
	}

	std::wstring LegacyCategoryInstanceKeyPath()
	{
		return LegacyCategoryKeyPath() + L"\\Instance";
	}

	std::wstring FilterInstanceKeyPath()
	{
		return LegacyCategoryInstanceKeyPath() + L"\\" + GuidString(kClsidVPRendererFilter);
	}


	FilterRegistrationData::FilterRegistrationData()
	{
		for (const SubtypeInfo& info : AcceptedSubtypes())
			minorTypes.push_back(info.subtype);
		for (const GUID& minor : minorTypes)
		{
			REGPINTYPES type;
			type.clsMajorType = &kMediaTypeVideo;
			type.clsMinorType = &minor;
			pinTypes.push_back(type);
		}

		pin = {};
		pin.dwFlags = REG_PINFLAG_B_RENDERER;
		pin.cInstances = 1;
		pin.nMediaTypes = static_cast<UINT>(pinTypes.size());
		pin.lpMediaType = pinTypes.data();
		pin.nMediums = 0;
		pin.lpMedium = nullptr;
		pin.clsPinCategory = nullptr;

		filter = {};
		filter.dwVersion = 2;
		filter.dwMerit = kFilterMerit;
		filter.cPins2 = 1;
		filter.rgPins2 = &pin;
	}


	HRESULT RegisterFilterForCurrentUser(const std::wstring& modulePath)
	{
		if (modulePath.empty())
			return E_INVALIDARG;

		HRESULT hr = CreateKeyWithDefault(ClsidKeyPath(), kFilterName);
		HKEY inproc = nullptr;
		if (SUCCEEDED(hr))
			hr = CreateKeyWithDefault(InprocServer32KeyPath(), modulePath, &inproc);
		if (SUCCEEDED(hr))
		{
			hr = SetStringValue(inproc, L"ThreadingModel", L"Both");
			RegCloseKey(inproc);
		}

		if (SUCCEEDED(hr))
		{
			FilterRegistrationData data;
			hr = WithUserClassesMapper([&data](IFilterMapper2* mapper)
			{
				return mapper->RegisterFilter(kClsidVPRendererFilter, kFilterName, nullptr,
					&kClsidLegacyAmFilterCategory, nullptr, &data.filter);
			});
		}

		if (FAILED(hr))
			UnregisterFilterForCurrentUser();
		return hr;
	}


	HRESULT UnregisterFilterForCurrentUser()
	{
		HRESULT hr = WithUserClassesMapper([](IFilterMapper2* mapper)
		{
			const HRESULT r = mapper->UnregisterFilter(&kClsidLegacyAmFilterCategory,
				nullptr, kClsidVPRendererFilter);
			// Not registered is not an error for unregistration.
			return r == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ? S_OK : r;
		});

		// Belt and braces: the instance key the mapper wrote, if it is still there.
		const LSTATUS instance = RegDeleteTreeW(HKEY_CURRENT_USER, FilterInstanceKeyPath().c_str());
		if (instance == ERROR_SUCCESS)
			RegDeleteKeyW(HKEY_CURRENT_USER, FilterInstanceKeyPath().c_str());
		DeleteKeyIfEmpty(LegacyCategoryInstanceKeyPath());
		DeleteKeyIfEmpty(LegacyCategoryKeyPath());

		const LSTATUS clsid = RegDeleteTreeW(HKEY_CURRENT_USER, ClsidKeyPath().c_str());
		if (clsid == ERROR_SUCCESS)
			RegDeleteKeyW(HKEY_CURRENT_USER, ClsidKeyPath().c_str());
		else if (clsid != ERROR_FILE_NOT_FOUND && SUCCEEDED(hr))
			hr = HRESULT_FROM_WIN32(clsid);
		return hr;
	}
}
