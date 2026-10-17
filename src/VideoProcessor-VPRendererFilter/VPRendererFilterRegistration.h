#pragma once

// VP Renderer DirectShow filter: identity and per-user (HKCU, no admin) COM +
// DirectShow registration.
//
// DllRegisterServer writes
//   HKCU\Software\Classes\CLSID\{clsid}                    (default = filter name)
//   HKCU\Software\Classes\CLSID\{clsid}\InprocServer32     (default = .ax path,
//                                                           ThreadingModel = Both)
// and registers the filter in the legacy DirectShow filter category through
// IFilterMapper2 with HKEY_CLASSES_ROOT redirected to HKCU\Software\Classes, so
//   HKCU\Software\Classes\CLSID\{083863F1-70DE-11D0-BD40-00A0C911CE86}\Instance\{clsid}
// receives FriendlyName / CLSID / FilterData as the system serializes them
// (and an application that captures DllRegisterServer through its own mapper,
// as MPC-HC/BE do for External Filters -> Add -> Browse, sees the same call).
// DllUnregisterServer removes exactly those keys (and the Instance/category
// keys only when they were left empty).

#include <Windows.h>
#include <strmif.h>

#include <string>
#include <vector>


namespace VPRendererFilter
{
	// {54DEA624-D69C-43BF-BAB2-6E04D51F1B95}: generated for the VP Renderer
	// filter; never reuse another renderer's CLSID.
	extern const CLSID kClsidVPRendererFilter;
	extern const wchar_t* const kFilterName;   // L"VP Renderer"

	// MERIT_UNLIKELY: never picked over the system renderers by intelligent
	// connect in other applications; MPC uses it through External Filters ->
	// Prefer, which outranks its built-in renderer regardless of merit.
	constexpr DWORD kFilterMerit = 0x00400000;

	std::wstring GuidString(const GUID& guid);   // "{XXXXXXXX-XXXX-...}" upper case
	std::wstring ClsidKeyPath();                 // relative to HKCU
	std::wstring InprocServer32KeyPath();
	std::wstring LegacyCategoryKeyPath();
	std::wstring LegacyCategoryInstanceKeyPath();
	std::wstring FilterInstanceKeyPath();

	// REGFILTER2 (version 2) for the one rendered input pin and every accepted
	// subtype. Holds the arrays the REGFILTER2 points into: not copyable.
	struct FilterRegistrationData
	{
		FilterRegistrationData();
		FilterRegistrationData(const FilterRegistrationData&) = delete;
		FilterRegistrationData& operator=(const FilterRegistrationData&) = delete;

		std::vector<GUID> minorTypes;
		std::vector<REGPINTYPES> pinTypes;
		REGFILTERPINS2 pin;
		REGFILTER2 filter;
	};

	HRESULT RegisterFilterForCurrentUser(const std::wstring& modulePath);
	HRESULT UnregisterFilterForCurrentUser();
}
