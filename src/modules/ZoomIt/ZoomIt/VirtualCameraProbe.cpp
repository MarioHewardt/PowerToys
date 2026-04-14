//==============================================================================
//
// VirtualCameraProbe.cpp
//
// Diagnostic probe for Windows 11 Media Foundation virtual camera support.
//
//==============================================================================

#include "pch.h"
#include "VirtualCameraProbe.h"
#include "VirtualCameraIds.h"

#if defined( NTDDI_VERSION ) && ( NTDDI_VERSION < NTDDI_WIN10_CO )
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10_CO
#endif

#include <mfvirtualcamera.h>

namespace
{
    constexpr wchar_t kProbeFriendlyName[] = L"ZoomIt Probe";
    constexpr wchar_t kProbeServerReadyEventName[] = L"Local\\ZoomItVirtualCamProbeReady";
    constexpr wchar_t kProbeServerStopEventName[] = L"Local\\ZoomItVirtualCamProbeStop";
    constexpr wchar_t kProbeSourceServerSwitch[] = L"/virtualcam-source-server";
    constexpr DWORD kProbeServerReadyTimeoutMs = 5000;
    constexpr DWORD kProbeServerShutdownTimeoutMs = 5000;
    constexpr DWORD kProbeServerIdleTimeoutMs = 30000;
    using MFIsVirtualCameraTypeSupportedFn = HRESULT (WINAPI*)( MFVirtualCameraType type, BOOL* supported );
    using MFCreateVirtualCameraFn = HRESULT (WINAPI*)( MFVirtualCameraType type,
                                                       MFVirtualCameraLifetime lifetime,
                                                       MFVirtualCameraAccess access,
                                                       LPCWSTR friendlyName,
                                                       LPCWSTR sourceId,
                                                       const GUID* categories,
                                                       ULONG categoryCount,
                                                       IMFVirtualCamera** virtualCamera );

    struct ProbeFunctions
    {
        MFIsVirtualCameraTypeSupportedFn isTypeSupported = nullptr;
        MFCreateVirtualCameraFn createVirtualCamera = nullptr;
    };

    void ProbeLog( const wchar_t* format, ... );
    std::wstring GuidToString( const GUID& guid );

    std::wstring GetCurrentModulePath()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW( nullptr, path, static_cast<DWORD>( std::size( path ) ) );
        return path;
    }

    std::wstring GetProbeRegistryKeyPath()
    {
        return L"Software\\Classes\\CLSID\\" + GuidToString( CLSID_ZoomItVirtualCameraProbeSource );
    }

    std::wstring GetProbeSourceServerCommandLine()
    {
        return L'"' + GetCurrentModulePath() + L'"' + std::wstring( L" " ) + kProbeSourceServerSwitch;
    }

    std::wstring GetProbeSourceDllPath()
    {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW( nullptr, modulePath, static_cast<DWORD>( std::size( modulePath ) ) );
        PathRemoveFileSpecW( modulePath );
        std::wstring path = modulePath;
        path += L"\\ZoomItVirtualCameraSource.dll";
        return path;
    }

    std::wstring GetProbeSourceTracePath()
    {
        wchar_t tempPath[MAX_PATH]{};
        GetTempPathW( static_cast<DWORD>( std::size( tempPath ) ), tempPath );
        std::wstring path = tempPath;
        path += kZoomItVirtualCameraSourceTraceFileName;
        return path;
    }

    class ProbeMediaSource final : public IMFMediaSource
    {
    public:
        STDMETHODIMP QueryInterface( REFIID riid, void** object ) override
        {
            if( object == nullptr )
            {
                return E_POINTER;
            }

            *object = nullptr;
            ProbeLog( L"[VirtualCamProbe] ProbeMediaSource::QueryInterface riid=%s\n", GuidToString( riid ).c_str() );
            if( riid == __uuidof( IUnknown ) ||
                riid == __uuidof( IMFMediaEventGenerator ) ||
                riid == __uuidof( IMFMediaSource ) )
            {
                *object = static_cast<IMFMediaSource*>( this );
                AddRef();
                return S_OK;
            }

            return E_NOINTERFACE;
        }

        STDMETHODIMP_(ULONG) AddRef() override
        {
            return ++m_refCount;
        }

        STDMETHODIMP_(ULONG) Release() override
        {
            const ULONG count = --m_refCount;
            if( count == 0 )
            {
                delete this;
            }
            return count;
        }

        STDMETHODIMP GetEvent( DWORD, IMFMediaEvent** ) override
        {
            return E_NOTIMPL;
        }

        STDMETHODIMP BeginGetEvent( IMFAsyncCallback*, IUnknown* ) override
        {
            return E_NOTIMPL;
        }

        STDMETHODIMP EndGetEvent( IMFAsyncResult*, IMFMediaEvent** ) override
        {
            return E_NOTIMPL;
        }

        STDMETHODIMP QueueEvent( MediaEventType, REFGUID, HRESULT, const PROPVARIANT* ) override
        {
            return E_NOTIMPL;
        }

        STDMETHODIMP GetCharacteristics( DWORD* characteristics ) override
        {
            if( characteristics == nullptr )
            {
                return E_POINTER;
            }

            *characteristics = MFMEDIASOURCE_IS_LIVE;
            return S_OK;
        }

        STDMETHODIMP CreatePresentationDescriptor( IMFPresentationDescriptor** descriptor ) override
        {
            if( descriptor == nullptr )
            {
                return E_POINTER;
            }

            *descriptor = nullptr;
            return E_NOTIMPL;
        }

        STDMETHODIMP Start( IMFPresentationDescriptor*, const GUID*, const PROPVARIANT* ) override
        {
            return S_OK;
        }

        STDMETHODIMP Stop() override
        {
            return S_OK;
        }

        STDMETHODIMP Pause() override
        {
            return E_NOTIMPL;
        }

        STDMETHODIMP Shutdown() override
        {
            return S_OK;
        }

    private:
        std::atomic<ULONG> m_refCount{ 1 };
    };

    class ProbeClassFactory final : public IClassFactory
    {
    public:
        STDMETHODIMP QueryInterface( REFIID riid, void** object ) override
        {
            if( object == nullptr )
            {
                return E_POINTER;
            }

            *object = nullptr;
            ProbeLog( L"[VirtualCamProbe] ProbeClassFactory::QueryInterface riid=%s\n", GuidToString( riid ).c_str() );
            if( riid == __uuidof( IUnknown ) || riid == __uuidof( IClassFactory ) )
            {
                *object = static_cast<IClassFactory*>( this );
                AddRef();
                return S_OK;
            }

            return E_NOINTERFACE;
        }

        STDMETHODIMP_(ULONG) AddRef() override
        {
            return ++m_refCount;
        }

        STDMETHODIMP_(ULONG) Release() override
        {
            const ULONG count = --m_refCount;
            if( count == 0 )
            {
                delete this;
            }
            return count;
        }

        STDMETHODIMP CreateInstance( IUnknown* outer, REFIID riid, void** object ) override
        {
            if( object == nullptr )
            {
                return E_POINTER;
            }

            *object = nullptr;
            ProbeLog( L"[VirtualCamProbe] ProbeClassFactory::CreateInstance riid=%s outer=%p\n",
                      GuidToString( riid ).c_str(),
                      outer );
            if( outer != nullptr )
            {
                return CLASS_E_NOAGGREGATION;
            }

            auto* mediaSource = new (std::nothrow) ProbeMediaSource();
            if( mediaSource == nullptr )
            {
                return E_OUTOFMEMORY;
            }

            const HRESULT hr = mediaSource->QueryInterface( riid, object );
            mediaSource->Release();
            return hr;
        }

        STDMETHODIMP LockServer( BOOL ) override
        {
            return S_OK;
        }

    private:
        std::atomic<ULONG> m_refCount{ 1 };
    };

    void EnsureConsole()
    {
        if( !AttachConsole( ATTACH_PARENT_PROCESS ) )
        {
            AllocConsole();
        }

        FILE* stream = nullptr;
        freopen_s( &stream, "CONOUT$", "w", stdout );
        freopen_s( &stream, "CONOUT$", "w", stderr );
    }

    void ProbeLog( const wchar_t* format, ... )
    {
        va_list args;
        va_start( args, format );

        wchar_t buffer[1024]{};
        _vsnwprintf_s( buffer, _TRUNCATE, format, args );

        va_end( args );

        OutputDebugStringW( buffer );
        wprintf( L"%s", buffer );
        fflush( stdout );
    }

    std::wstring FormatHresult( HRESULT hr )
    {
        wchar_t buffer[32]{};
        swprintf_s( buffer, L"0x%08X", static_cast<unsigned>( hr ) );
        return buffer;
    }

    std::wstring GuidToString( const GUID& guid )
    {
        wchar_t buffer[64]{};
        StringFromGUID2( guid, buffer, static_cast<int>( std::size( buffer ) ) );
        return buffer;
    }

    bool NameContainsProbe( const std::wstring& value )
    {
        return value.find( kProbeFriendlyName ) != std::wstring::npos;
    }

    const wchar_t* KnownHresultMessage( HRESULT hr )
    {
        switch( hr )
        {
        case REGDB_E_CLASSNOTREG:
            return L"source CLSID is not registered";
        case E_ACCESSDENIED:
            return L"camera privacy or access policy denied the operation";
        default:
            return L"";
        }
    }

    bool EnumerateProbeCamera( std::wstring& matchedName, HRESULT& enumHr )
    {
        enumHr = S_OK;
        IMFAttributes* attributes = nullptr;
        IMFActivate** devices = nullptr;
        UINT32 count = 0;

        enumHr = MFCreateAttributes( &attributes, 1 );
        if( FAILED( enumHr ) )
        {
            return false;
        }

        attributes->SetGUID( MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID );
        enumHr = MFEnumDeviceSources( attributes, &devices, &count );
        attributes->Release();

        if( FAILED( enumHr ) )
        {
            return false;
        }

        bool found = false;
        for( UINT32 index = 0; index < count; ++index )
        {
            WCHAR* friendlyName = nullptr;
            UINT32 nameLength = 0;

            if( SUCCEEDED( devices[index]->GetAllocatedString( MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendlyName, &nameLength ) ) )
            {
                std::wstring currentName = friendlyName;
                if( NameContainsProbe( currentName ) )
                {
                    matchedName = currentName;
                    found = true;
                }
            }

            if( friendlyName != nullptr )
            {
                CoTaskMemFree( friendlyName );
            }

            devices[index]->Release();
        }

        if( devices != nullptr )
        {
            CoTaskMemFree( devices );
        }

        return found;
    }

    bool WaitForProbeCamera( std::wstring& matchedName, HRESULT& enumHr )
    {
        constexpr int attempts = 20;
        constexpr DWORD delayMs = 100;

        for( int attempt = 0; attempt < attempts; ++attempt )
        {
            if( EnumerateProbeCamera( matchedName, enumHr ) )
            {
                return true;
            }

            Sleep( delayMs );
        }

        return false;
    }

    bool LoadProbeFunctions( ProbeFunctions& functions )
    {
        auto module = wil::unique_hmodule( LoadLibrarySafe( L"mfsensorgroup.dll", DLL_LOAD_LOCATION_SYSTEM ) );
        if( !module )
        {
            ProbeLog( L"[VirtualCamProbe] Failed to load mfsensorgroup.dll, error=%lu\n", GetLastError() );
            return false;
        }

        functions.isTypeSupported = reinterpret_cast<MFIsVirtualCameraTypeSupportedFn>(
            GetProcAddress( module.get(), "MFIsVirtualCameraTypeSupported" ) );
        functions.createVirtualCamera = reinterpret_cast<MFCreateVirtualCameraFn>(
            GetProcAddress( module.get(), "MFCreateVirtualCamera" ) );

        if( functions.isTypeSupported == nullptr || functions.createVirtualCamera == nullptr )
        {
            ProbeLog( L"[VirtualCamProbe] Missing MF virtual camera exports in mfsensorgroup.dll\n" );
            return false;
        }

        module.release();
        return true;
    }

    HRESULT RegisterProbeLocalServer()
    {
        const std::wstring keyPath = GetProbeRegistryKeyPath();
        const std::wstring commandLine = GetProbeSourceServerCommandLine();

        HKEY clsidKey = nullptr;
        LONG error = RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            keyPath.c_str(),
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE | KEY_CREATE_SUB_KEY,
            nullptr,
            &clsidKey,
            nullptr );
        if( error != ERROR_SUCCESS )
        {
            return HRESULT_FROM_WIN32( error );
        }

        error = RegSetValueExW(
            clsidKey,
            nullptr,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>( kProbeFriendlyName ),
            static_cast<DWORD>( ( wcslen( kProbeFriendlyName ) + 1 ) * sizeof( wchar_t ) ) );

        HKEY localServerKey = nullptr;
        if( error == ERROR_SUCCESS )
        {
            error = RegCreateKeyExW(
                clsidKey,
                L"LocalServer32",
                0,
                nullptr,
                REG_OPTION_NON_VOLATILE,
                KEY_SET_VALUE,
                nullptr,
                &localServerKey,
                nullptr );
        }

        if( error == ERROR_SUCCESS )
        {
            error = RegSetValueExW(
                localServerKey,
                nullptr,
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>( commandLine.c_str() ),
                static_cast<DWORD>( ( commandLine.size() + 1 ) * sizeof( wchar_t ) ) );
        }

        if( localServerKey != nullptr )
        {
            RegCloseKey( localServerKey );
        }
        RegCloseKey( clsidKey );

        return HRESULT_FROM_WIN32( error );
    }

    HRESULT RegisterProbeInprocServer()
    {
        const std::wstring dllPath = GetProbeSourceDllPath();
        if( GetFileAttributesW( dllPath.c_str() ) == INVALID_FILE_ATTRIBUTES )
        {
            return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
        }

        // The FrameServerMonitor service runs as SYSTEM and can only see HKLM.
        // Writing to HKLM requires elevation.  Use reg.exe with ShellExecute
        // runas verb so the user gets a single UAC prompt.
        const std::wstring clsidStr = GuidToString( CLSID_ZoomItVirtualCameraProbeSource );
        const std::wstring keyPath = L"HKLM\\SOFTWARE\\Classes\\CLSID\\" + clsidStr;
        const std::wstring inprocPath = keyPath + L"\\InprocServer32";

        // Create the key tree and set InprocServer32 default value + ThreadingModel
        std::wstring regArgs =
            L"/c reg add \"" + keyPath + L"\" /ve /d \"ZoomIt Probe\" /f && "
            L"reg add \"" + inprocPath + L"\" /ve /d \"" + dllPath + L"\" /f && "
            L"reg add \"" + inprocPath + L"\" /v ThreadingModel /d Both /f";

        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof( sei );
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.lpVerb = L"runas";
        sei.lpFile = L"cmd.exe";
        sei.lpParameters = regArgs.c_str();
        sei.nShow = SW_HIDE;

        if( !ShellExecuteExW( &sei ) )
        {
            return HRESULT_FROM_WIN32( GetLastError() );
        }

        if( sei.hProcess )
        {
            WaitForSingleObject( sei.hProcess, 10000 );
            DWORD exitCode = 1;
            GetExitCodeProcess( sei.hProcess, &exitCode );
            CloseHandle( sei.hProcess );
            if( exitCode != 0 )
            {
                return E_FAIL;
            }
        }

        return S_OK;
    }

    void UnregisterProbeLocalServer()
    {
        const std::wstring clsidStr = GuidToString( CLSID_ZoomItVirtualCameraProbeSource );
        const std::wstring keyPath = L"HKLM\\SOFTWARE\\Classes\\CLSID\\" + clsidStr;
        std::wstring regArgs = L"/c reg delete \"" + keyPath + L"\" /f";

        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof( sei );
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.lpVerb = L"runas";
        sei.lpFile = L"cmd.exe";
        sei.lpParameters = regArgs.c_str();
        sei.nShow = SW_HIDE;

        if( ShellExecuteExW( &sei ) && sei.hProcess )
        {
            WaitForSingleObject( sei.hProcess, 10000 );
            CloseHandle( sei.hProcess );
        }
    }

    bool StartProbeSourceServerProcess( wil::unique_handle& processHandle,
                                        wil::unique_handle& threadHandle,
                                        wil::unique_handle& readyEvent,
                                        wil::unique_handle& stopEvent )
    {
        readyEvent.reset( CreateEventW( nullptr, TRUE, FALSE, kProbeServerReadyEventName ) );
        stopEvent.reset( CreateEventW( nullptr, TRUE, FALSE, kProbeServerStopEventName ) );
        if( !readyEvent || !stopEvent )
        {
            ProbeLog( L"[VirtualCamProbe] Failed to create probe events, error=%lu\n", GetLastError() );
            return false;
        }

        ResetEvent( readyEvent.get() );
        ResetEvent( stopEvent.get() );

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof( startupInfo );
        PROCESS_INFORMATION processInfo{};
        std::wstring commandLine = GetProbeSourceServerCommandLine();

        const BOOL created = CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo );
        if( !created )
        {
            ProbeLog( L"[VirtualCamProbe] Failed to launch probe source server, error=%lu\n", GetLastError() );
            return false;
        }

        processHandle.reset( processInfo.hProcess );
        threadHandle.reset( processInfo.hThread );

        const DWORD waitResult = WaitForSingleObject( readyEvent.get(), kProbeServerReadyTimeoutMs );
        ProbeLog( L"[VirtualCamProbe] Source server ready wait result=%lu\n", waitResult );
        return waitResult == WAIT_OBJECT_0;
    }

    HRESULT RegisterProbeClassObject( DWORD& classCookie, ProbeClassFactory*& classFactory )
    {
        classCookie = 0;
        classFactory = new (std::nothrow) ProbeClassFactory();
        if( classFactory == nullptr )
        {
            return E_OUTOFMEMORY;
        }

        const HRESULT registerHr = CoRegisterClassObject(
            CLSID_ZoomItVirtualCameraProbeSource,
            static_cast<IClassFactory*>( classFactory ),
            CLSCTX_LOCAL_SERVER,
            REGCLS_MULTIPLEUSE | REGCLS_SUSPENDED,
            &classCookie );

        if( FAILED( registerHr ) )
        {
            classFactory->Release();
            classFactory = nullptr;
            return registerHr;
        }

        const HRESULT resumeHr = CoResumeClassObjects();
        if( FAILED( resumeHr ) )
        {
            CoRevokeClassObject( classCookie );
            classCookie = 0;
            classFactory->Release();
            classFactory = nullptr;
            return resumeHr;
        }

        return S_OK;
    }
}

bool RunVirtualCameraProbe()
{
    EnsureConsole();
    DeleteFileW( GetProbeSourceTracePath().c_str() );
    ProbeLog( L"[VirtualCamProbe] Starting probe\n" );

    const HRESULT coInitHr = CoInitializeEx( nullptr, COINIT_MULTITHREADED );
    const bool coInitialized = SUCCEEDED( coInitHr ) || coInitHr == RPC_E_CHANGED_MODE;
    if( !coInitialized )
    {
        ProbeLog( L"[VirtualCamProbe] CoInitializeEx failed: %s\n", FormatHresult( coInitHr ).c_str() );
        return false;
    }

    const HRESULT mfStartupHr = MFStartup( MF_VERSION, MFSTARTUP_LITE );
    if( FAILED( mfStartupHr ) )
    {
        ProbeLog( L"[VirtualCamProbe] MFStartup failed: %s\n", FormatHresult( mfStartupHr ).c_str() );
        if( SUCCEEDED( coInitHr ) )
        {
            CoUninitialize();
        }
        return false;
    }

    ProbeFunctions functions{};
    if( !LoadProbeFunctions( functions ) )
    {
        MFShutdown();
        if( SUCCEEDED( coInitHr ) )
        {
            CoUninitialize();
        }
        return false;
    }

    const HRESULT registerInprocHr = RegisterProbeInprocServer();
    ProbeLog( L"[VirtualCamProbe] RegisterProbeInprocServer hr=%s\n", FormatHresult( registerInprocHr ).c_str() );

    const bool useInprocServer = SUCCEEDED( registerInprocHr );
    if( !useInprocServer )
    {
        const HRESULT registerServerHr = RegisterProbeLocalServer();
        ProbeLog( L"[VirtualCamProbe] RegisterProbeLocalServer hr=%s\n", FormatHresult( registerServerHr ).c_str() );
        if( FAILED( registerServerHr ) )
        {
            UnregisterProbeLocalServer();
            MFShutdown();
            if( SUCCEEDED( coInitHr ) )
            {
                CoUninitialize();
            }
            return false;
        }
    }

    wil::unique_handle sourceServerProcess;
    wil::unique_handle sourceServerThread;
    wil::unique_handle readyEvent;
    wil::unique_handle stopEvent;
    if( !useInprocServer && !StartProbeSourceServerProcess( sourceServerProcess, sourceServerThread, readyEvent, stopEvent ) )
    {
        UnregisterProbeLocalServer();
        MFShutdown();
        if( SUCCEEDED( coInitHr ) )
        {
            CoUninitialize();
        }
        return false;
    }

    winrt::com_ptr<IMFMediaSource> activatedSource;
    const HRESULT coCreateLocalHr = CoCreateInstance(
        CLSID_ZoomItVirtualCameraProbeSource,
        nullptr,
        CLSCTX_LOCAL_SERVER,
        __uuidof( IMFMediaSource ),
        reinterpret_cast<void**>( activatedSource.put_void() ) );
    ProbeLog( L"[VirtualCamProbe] CoCreateInstance(IMFMediaSource, CLSCTX_LOCAL_SERVER) hr=%s\n",
              FormatHresult( coCreateLocalHr ).c_str() );
    activatedSource = nullptr;

    const HRESULT coCreateInprocHr = CoCreateInstance(
        CLSID_ZoomItVirtualCameraProbeSource,
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof( IMFMediaSource ),
        reinterpret_cast<void**>( activatedSource.put_void() ) );
    ProbeLog( L"[VirtualCamProbe] CoCreateInstance(IMFMediaSource, CLSCTX_INPROC_SERVER) hr=%s\n",
              FormatHresult( coCreateInprocHr ).c_str() );
    activatedSource = nullptr;

    const HRESULT coCreateAllHr = CoCreateInstance(
        CLSID_ZoomItVirtualCameraProbeSource,
        nullptr,
        CLSCTX_ALL,
        __uuidof( IMFMediaSource ),
        reinterpret_cast<void**>( activatedSource.put_void() ) );
    ProbeLog( L"[VirtualCamProbe] CoCreateInstance(IMFMediaSource, CLSCTX_ALL) hr=%s\n",
              FormatHresult( coCreateAllHr ).c_str() );
    activatedSource = nullptr;

    BOOL supported = FALSE;
    const HRESULT supportHr = functions.isTypeSupported( MFVirtualCameraType_SoftwareCameraSource, &supported );
    ProbeLog( L"[VirtualCamProbe] MFIsVirtualCameraTypeSupported hr=%s supported=%d\n",
              FormatHresult( supportHr ).c_str(), supported ? 1 : 0 );

    if( FAILED( supportHr ) || !supported )
    {
        MFShutdown();
        if( SUCCEEDED( coInitHr ) )
        {
            CoUninitialize();
        }
        return false;
    }

    const std::wstring sourceId = GuidToString( CLSID_ZoomItVirtualCameraProbeSource );
    ProbeLog( L"[VirtualCamProbe] Probe source CLSID=%s\n", sourceId.c_str() );

    winrt::com_ptr<IMFVirtualCamera> virtualCamera;
    const HRESULT createHr = functions.createVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_Session,
        MFVirtualCameraAccess_CurrentUser,
        kProbeFriendlyName,
        sourceId.c_str(),
        nullptr,
        0,
        virtualCamera.put() );

    ProbeLog( L"[VirtualCamProbe] MFCreateVirtualCamera hr=%s\n", FormatHresult( createHr ).c_str() );
    if( FAILED( createHr ) )
    {
        MFShutdown();
        if( SUCCEEDED( coInitHr ) )
        {
            CoUninitialize();
        }
        return false;
    }

    winrt::com_ptr<IMFMediaSource> virtualCameraSource;
    const HRESULT getMediaSourceHr = virtualCamera->GetMediaSource( virtualCameraSource.put() );
    ProbeLog( L"[VirtualCamProbe] IMFVirtualCamera::GetMediaSource hr=%s\n", FormatHresult( getMediaSourceHr ).c_str() );
    virtualCameraSource = nullptr;

    const HRESULT startHr = virtualCamera->Start( nullptr );
    ProbeLog( L"[VirtualCamProbe] IMFVirtualCamera::Start hr=%s\n", FormatHresult( startHr ).c_str() );
    if( FAILED( startHr ) && KnownHresultMessage( startHr )[0] != L'\0' )
    {
        ProbeLog( L"[VirtualCamProbe] Start detail: %s\n", KnownHresultMessage( startHr ) );
    }

    std::wstring matchedName;
    HRESULT enumHr = E_FAIL;
    const bool enumerated = SUCCEEDED( startHr ) && WaitForProbeCamera( matchedName, enumHr );
    ProbeLog( L"[VirtualCamProbe] enumeration hr=%s found=%d\n", FormatHresult( enumHr ).c_str(), enumerated ? 1 : 0 );
    if( enumerated )
    {
        ProbeLog( L"[VirtualCamProbe] enumerated device name=%s\n", matchedName.c_str() );
    }

    HRESULT stopHr = S_OK;
    if( SUCCEEDED( startHr ) )
    {
        stopHr = virtualCamera->Stop();
    }

    const HRESULT removeHr = virtualCamera->Remove();
    const HRESULT shutdownHr = virtualCamera->Shutdown();
    ProbeLog( L"[VirtualCamProbe] cleanup stop=%s remove=%s shutdown=%s\n",
              FormatHresult( stopHr ).c_str(),
              FormatHresult( removeHr ).c_str(),
              FormatHresult( shutdownHr ).c_str() );

    virtualCamera = nullptr;

    if( stopEvent )
    {
        SetEvent( stopEvent.get() );
    }
    if( sourceServerProcess )
    {
        WaitForSingleObject( sourceServerProcess.get(), kProbeServerShutdownTimeoutMs );
    }

    UnregisterProbeLocalServer();

    MFShutdown();
    if( SUCCEEDED( coInitHr ) )
    {
        CoUninitialize();
    }

    ProbeLog( L"[VirtualCamProbe] Result=%s\n", enumerated ? L"PASS" : L"FAIL" );
    return enumerated;
}

bool RunVirtualCameraSourceServer()
{
    EnsureConsole();
    ProbeLog( L"[VirtualCamProbeServer] Starting source server\n" );

    const HRESULT coInitHr = CoInitializeEx( nullptr, COINIT_MULTITHREADED );
    if( FAILED( coInitHr ) && coInitHr != RPC_E_CHANGED_MODE )
    {
        ProbeLog( L"[VirtualCamProbeServer] CoInitializeEx failed: %s\n", FormatHresult( coInitHr ).c_str() );
        return false;
    }

    wil::unique_handle readyEvent( CreateEventW( nullptr, TRUE, FALSE, kProbeServerReadyEventName ) );
    wil::unique_handle stopEvent( CreateEventW( nullptr, TRUE, FALSE, kProbeServerStopEventName ) );
    if( !readyEvent || !stopEvent )
    {
        ProbeLog( L"[VirtualCamProbeServer] Failed to open probe events, error=%lu\n", GetLastError() );
        if( SUCCEEDED( coInitHr ) )
        {
            CoUninitialize();
        }
        return false;
    }

    DWORD classCookie = 0;
    ProbeClassFactory* classFactory = nullptr;
    const HRESULT registerHr = RegisterProbeClassObject( classCookie, classFactory );
    ProbeLog( L"[VirtualCamProbeServer] CoRegisterClassObject/Resume hr=%s\n", FormatHresult( registerHr ).c_str() );
    if( FAILED( registerHr ) )
    {
        if( SUCCEEDED( coInitHr ) )
        {
            CoUninitialize();
        }
        return false;
    }

    SetEvent( readyEvent.get() );
    const DWORD waitResult = WaitForSingleObject( stopEvent.get(), kProbeServerIdleTimeoutMs );
    ProbeLog( L"[VirtualCamProbeServer] Stop wait result=%lu\n", waitResult );

    if( classCookie != 0 )
    {
        CoRevokeClassObject( classCookie );
    }
    if( classFactory != nullptr )
    {
        classFactory->Release();
    }

    if( SUCCEEDED( coInitHr ) )
    {
        CoUninitialize();
    }

    ProbeLog( L"[VirtualCamProbeServer] Exiting source server\n" );
    return true;
}