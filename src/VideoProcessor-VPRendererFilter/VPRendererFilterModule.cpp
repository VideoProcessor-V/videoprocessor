#include <pch.h>

// COM entry points of VPRendererFilter.ax (exported through
// VideoProcessor-VPRendererFilter.def). The DirectShow base classes' dllentry
// is not used: one class, one factory, and per-user registration.

#include "VPRendererFilter.h"
#include "VPRendererFilterLog.h"
#include "VPRendererFilterRegistration.h"
#include "VPRendererPlugin.h"

#include <DebugLog.h>

#include <atomic>


namespace
{
	using VPRendererFilter::CVPRendererFilter;

	std::atomic<long> g_serverLocks{ 0 };

	class ClassFactory final : public IClassFactory
	{
	public:
		STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
		{
			if (!ppv)
				return E_POINTER;
			if (riid == IID_IUnknown || riid == IID_IClassFactory)
			{
				*ppv = static_cast<IClassFactory*>(this);
				AddRef();
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}

		// Static object: the reference count only keeps the server alive.
		STDMETHODIMP_(ULONG) AddRef() override
		{
			++g_serverLocks;
			return 2;
		}

		STDMETHODIMP_(ULONG) Release() override
		{
			--g_serverLocks;
			return 1;
		}

		STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
		{
			if (!ppv)
				return E_POINTER;
			*ppv = nullptr;
			if (outer && riid != IID_IUnknown)
				return CLASS_E_NOAGGREGATION;

			HRESULT hr = S_OK;
			CUnknown* object = CVPRendererFilter::CreateInstance(outer, &hr);
			if (!object)
				return FAILED(hr) ? hr : E_OUTOFMEMORY;
			if (FAILED(hr))
			{
				delete object;
				return hr;
			}
			object->NonDelegatingAddRef();
			hr = object->NonDelegatingQueryInterface(riid, ppv);
			object->NonDelegatingRelease();
			return hr;
		}

		STDMETHODIMP LockServer(BOOL lock) override
		{
			if (lock)
				++g_serverLocks;
			else
				--g_serverLocks;
			return S_OK;
		}
	};

	ClassFactory g_classFactory;
}


// Exported as DllGetClassObject / DllCanUnloadNow / DllRegisterServer /
// DllUnregisterServer by the .def file; distinct names keep them apart from
// the base classes' dllentry.obj should anything ever pull it in.

extern "C" HRESULT STDAPICALLTYPE VPRendererFilterDllGetClassObject(REFCLSID clsid,
	REFIID riid, void** ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = nullptr;
	if (clsid != VPRendererFilter::kClsidVPRendererFilter)
		return CLASS_E_CLASSNOTAVAILABLE;
	return g_classFactory.QueryInterface(riid, ppv);
}


extern "C" HRESULT STDAPICALLTYPE VPRendererFilterDllCanUnloadNow()
{
	return g_serverLocks.load() == 0 && CBaseObject::ObjectsActive() == 0 ? S_OK : S_FALSE;
}


extern "C" HRESULT STDAPICALLTYPE VPRendererFilterDllRegisterServer()
{
	try
	{
		VPRendererFilterLog::Initialize();
		const std::wstring path = VPRendererFilter::FilterModulePath();
		const HRESULT hr = VPRendererFilter::RegisterFilterForCurrentUser(path);
		DebugLog::Log("VP Renderer filter: DllRegisterServer (HKCU) path=%ls hr=0x%08lX",
			path.c_str(), hr);
		return hr;
	}
	catch (...)
	{
		return E_UNEXPECTED;
	}
}


extern "C" HRESULT STDAPICALLTYPE VPRendererFilterDllUnregisterServer()
{
	try
	{
		VPRendererFilterLog::Initialize();
		const HRESULT hr = VPRendererFilter::UnregisterFilterForCurrentUser();
		DebugLog::Log("VP Renderer filter: DllUnregisterServer (HKCU) hr=0x%08lX", hr);
		return hr;
	}
	catch (...)
	{
		return E_UNEXPECTED;
	}
}
