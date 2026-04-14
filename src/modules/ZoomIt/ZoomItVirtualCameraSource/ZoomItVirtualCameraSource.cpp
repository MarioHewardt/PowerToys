//==============================================================================
//
// ZoomItVirtualCameraSource.cpp
//
// In-proc COM media source for ZoomIt virtual camera.
// Implements IMFMediaSourceEx with a single IMFMediaStream2 that produces
// solid-green NV12 1280x720 frames at 30 fps.
//
// Registration: HKLM\SOFTWARE\Classes\CLSID\{...}\InprocServer32
// (FrameServerMonitor runs as SYSTEM — needs HKLM, not HKCU.)
//
//==============================================================================

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <ks.h>
#include <ksproxy.h>
#include <atomic>
#include <new>
#include <stdio.h>
#include <string>
#include <propvarutil.h>

#pragma comment( lib, "mfplat.lib" )
#pragma comment( lib, "mfuuid.lib" )
#pragma comment( lib, "propsys.lib" )
#pragma warning( disable : 26403 )

#include "..\ZoomIt\VirtualCameraIds.h"

namespace
{
    constexpr DWORD kStreamId = 0;
    constexpr UINT32 kWidth = 1280;
    constexpr UINT32 kHeight = 720;
    constexpr UINT32 kFrameRateN = 30;
    constexpr UINT32 kFrameRateD = 1;
    constexpr LONGLONG kFrameDuration = 10'000'000LL / kFrameRateN; // 100ns units

    constexpr GUID kPinNameVideoCapture =
    { 0xfb6c4281, 0x0353, 0x11d1, { 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba } };

    // ---- Trace ----
    std::wstring GetTracePath()
    {
        wchar_t tp[MAX_PATH]{};
        GetTempPathW( MAX_PATH, tp );
        return std::wstring( tp ) + kZoomItVirtualCameraSourceTraceFileName;
    }
    std::wstring GuidToStr( const GUID& g )
    {
        wchar_t b[64]{}; StringFromGUID2( g, b, 64 ); return b;
    }
    void DllLog( const wchar_t* fmt, ... )
    {
        va_list a; va_start( a, fmt );
        wchar_t b[1024]{}; _vsnwprintf_s( b, _TRUNCATE, fmt, a );
        va_end( a );
        OutputDebugStringW( b );
        FILE* f = nullptr;
        if( _wfopen_s( &f, GetTracePath().c_str(), L"a, ccs=UTF-8" ) == 0 && f )
        { fwprintf( f, L"%s", b ); fclose( f ); }
    }

    // ---- Forward declarations ----
    class ProbeMediaSource;

    //====================================================================
    // ProbeMediaStream — IMFMediaStream2
    //
    // Produces solid-green NV12 frames via RequestSample.
    //====================================================================
    class ProbeMediaStream final : public IMFMediaStream2
    {
    public:
        HRESULT Initialize( ProbeMediaSource* parent, IMFStreamDescriptor* sd )
        {
            m_parent = parent; // weak ref — parent outlives stream
            m_spSD = sd; sd->AddRef();
            HRESULT hr = MFCreateEventQueue( &m_spEQ );
            if( FAILED( hr ) ) return hr;

            // Get the negotiated media type from the stream descriptor
            IMFMediaTypeHandler* handler = nullptr;
            hr = sd->GetMediaTypeHandler( &handler );
            if( SUCCEEDED( hr ) )
            {
                handler->GetCurrentMediaType( &m_spMediaType );
                handler->Release();
            }
            return S_OK;
        }

        // IUnknown
        STDMETHODIMP QueryInterface( REFIID riid, void** o ) override
        {
            if( !o ) return E_POINTER; *o = nullptr;
            if( riid == __uuidof( IUnknown ) || riid == __uuidof( IMFMediaEventGenerator ) ||
                riid == __uuidof( IMFMediaStream ) || riid == __uuidof( IMFMediaStream2 ) )
                *o = static_cast<IMFMediaStream2*>( this );
            else return E_NOINTERFACE;
            AddRef(); return S_OK;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }
        STDMETHODIMP_(ULONG) Release() override { ULONG c = --m_ref; if( !c ) delete this; return c; }

        // IMFMediaEventGenerator
        STDMETHODIMP BeginGetEvent( IMFAsyncCallback* cb, IUnknown* st ) override
        { return m_spEQ ? m_spEQ->BeginGetEvent( cb, st ) : MF_E_SHUTDOWN; }
        STDMETHODIMP EndGetEvent( IMFAsyncResult* r, IMFMediaEvent** e ) override
        { return m_spEQ ? m_spEQ->EndGetEvent( r, e ) : MF_E_SHUTDOWN; }
        STDMETHODIMP GetEvent( DWORD f, IMFMediaEvent** e ) override
        { return m_spEQ ? m_spEQ->GetEvent( f, e ) : MF_E_SHUTDOWN; }
        STDMETHODIMP QueueEvent( MediaEventType t, REFGUID ext, HRESULT s, const PROPVARIANT* v ) override
        { return m_spEQ ? m_spEQ->QueueEventParamVar( t, ext, s, v ) : MF_E_SHUTDOWN; }

        // IMFMediaStream
        STDMETHODIMP GetMediaSource( IMFMediaSource** pp ) override;

        STDMETHODIMP GetStreamDescriptor( IMFStreamDescriptor** pp ) override
        {
            if( !pp ) return E_POINTER; *pp = nullptr;
            if( !m_spSD ) return E_UNEXPECTED;
            *pp = m_spSD; m_spSD->AddRef(); return S_OK;
        }

        STDMETHODIMP RequestSample( IUnknown* pToken ) override
        {
            if( m_streamState != MF_STREAM_STATE_RUNNING )
                return MF_E_INVALIDREQUEST;

            DllLog( L"[VCamStream] RequestSample\n" );

            // Create a sample with an NV12 buffer
            IMFSample* sample = nullptr;
            IMFMediaBuffer* buffer = nullptr;

            // NV12: Y plane = w*h, UV plane = w*h/2
            const DWORD nv12Size = kWidth * kHeight * 3 / 2;

            HRESULT hr = MFCreateSample( &sample );
            if( FAILED( hr ) ) return hr;

            hr = MFCreateMemoryBuffer( nv12Size, &buffer );
            if( FAILED( hr ) ) { sample->Release(); return hr; }

            // Fill with solid green: Y=149, U=43, V=21
            BYTE* data = nullptr;
            DWORD maxLen = 0;
            hr = buffer->Lock( &data, &maxLen, nullptr );
            if( SUCCEEDED( hr ) )
            {
                // Y plane
                memset( data, 149, kWidth * kHeight );
                // UV plane (interleaved U,V pairs)
                BYTE* uv = data + kWidth * kHeight;
                for( DWORD i = 0; i < kWidth * kHeight / 2; i += 2 )
                {
                    uv[i] = 43;      // U
                    uv[i + 1] = 21;  // V
                }
                buffer->Unlock();
            }
            buffer->SetCurrentLength( nv12Size );

            sample->AddBuffer( buffer );
            buffer->Release();

            sample->SetSampleTime( MFGetSystemTime() );
            sample->SetSampleDuration( kFrameDuration );

            if( pToken )
                sample->SetUnknown( MFSampleExtension_Token, pToken );

            // Queue MEMediaSample event with the sample
            if( m_spEQ )
                m_spEQ->QueueEventParamUnk( MEMediaSample, GUID_NULL, S_OK, sample );

            sample->Release();
            return S_OK;
        }

        // IMFMediaStream2
        STDMETHODIMP SetStreamState( MF_STREAM_STATE state ) override
        {
            DllLog( L"[VCamStream] SetStreamState %d\n", state );
            if( state == MF_STREAM_STATE_RUNNING )
            {
                m_streamState = MF_STREAM_STATE_RUNNING;
                if( m_spEQ )
                    m_spEQ->QueueEventParamVar( MEStreamStarted, GUID_NULL, S_OK, nullptr );
            }
            else if( state == MF_STREAM_STATE_STOPPED )
            {
                m_streamState = MF_STREAM_STATE_STOPPED;
                if( m_spEQ )
                    m_spEQ->QueueEventParamVar( MEStreamStopped, GUID_NULL, S_OK, nullptr );
            }
            else if( state == MF_STREAM_STATE_PAUSED )
            {
                m_streamState = MF_STREAM_STATE_PAUSED;
                if( m_spEQ )
                    m_spEQ->QueueEventParamVar( MEStreamPaused, GUID_NULL, S_OK, nullptr );
            }
            return S_OK;
        }
        STDMETHODIMP GetStreamState( MF_STREAM_STATE* pState ) override
        {
            if( !pState ) return E_POINTER;
            *pState = m_streamState; return S_OK;
        }

        // Internal
        void StartStream()
        {
            m_streamState = MF_STREAM_STATE_RUNNING;
            if( m_spEQ )
                m_spEQ->QueueEventParamVar( MEStreamStarted, GUID_NULL, S_OK, nullptr );
        }
        void StopStream()
        {
            m_streamState = MF_STREAM_STATE_STOPPED;
            if( m_spEQ )
                m_spEQ->QueueEventParamVar( MEStreamStopped, GUID_NULL, S_OK, nullptr );
        }
        void Shutdown()
        {
            if( m_spEQ ) { m_spEQ->Shutdown(); m_spEQ->Release(); m_spEQ = nullptr; }
            if( m_spSD ) { m_spSD->Release(); m_spSD = nullptr; }
            if( m_spMediaType ) { m_spMediaType->Release(); m_spMediaType = nullptr; }
            m_parent = nullptr;
        }

    private:
        ProbeMediaSource* m_parent = nullptr;
        IMFMediaEventQueue* m_spEQ = nullptr;
        IMFStreamDescriptor* m_spSD = nullptr;
        IMFMediaType* m_spMediaType = nullptr;
        MF_STREAM_STATE m_streamState = MF_STREAM_STATE_STOPPED;
        std::atomic<ULONG> m_ref{ 1 };
    };

    //====================================================================
    // ProbeMediaSource — IMFMediaSourceEx + IMFGetService + IKsControl
    //====================================================================
    class ProbeMediaSource final : public IMFMediaSourceEx, public IMFGetService, public IKsControl
    {
    public:
        HRESULT Initialize( IMFAttributes* pActivateAttributes )
        {
            DllLog( L"[VCamSrc] Initialize\n" );

            HRESULT hr = MFCreateAttributes( &m_spAttributes, 8 );
            if( FAILED( hr ) ) return hr;

            if( pActivateAttributes )
            {
                hr = pActivateAttributes->CopyAllItems( m_spAttributes );
                if( FAILED( hr ) ) return hr;
            }

            hr = MFCreateEventQueue( &m_spEventQueue );
            if( FAILED( hr ) ) return hr;

            // Build one NV12 1280x720 @ 30fps media type
            IMFMediaType* mt = nullptr;
            hr = MFCreateMediaType( &mt );
            if( FAILED( hr ) ) return hr;

            mt->SetGUID( MF_MT_MAJOR_TYPE, MFMediaType_Video );
            mt->SetGUID( MF_MT_SUBTYPE, MFVideoFormat_NV12 );
            MFSetAttributeSize( mt, MF_MT_FRAME_SIZE, kWidth, kHeight );
            MFSetAttributeRatio( mt, MF_MT_FRAME_RATE, kFrameRateN, kFrameRateD );
            MFSetAttributeRatio( mt, MF_MT_PIXEL_ASPECT_RATIO, 1, 1 );
            mt->SetUINT32( MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive );
            mt->SetUINT32( MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE );

            // Stream descriptor
            IMFStreamDescriptor* sd = nullptr;
            IMFMediaType* types[] = { mt };
            hr = MFCreateStreamDescriptor( kStreamId, 1, types, &sd );
            mt->Release();
            if( FAILED( hr ) ) return hr;

            // Stream descriptor attributes
            IMFAttributes* sa = nullptr;
            if( SUCCEEDED( sd->QueryInterface( IID_PPV_ARGS( &sa ) ) ) )
            {
                sa->SetUINT32( MF_DEVICESTREAM_STREAM_ID, kStreamId );
                sa->SetGUID( MF_DEVICESTREAM_STREAM_CATEGORY, kPinNameVideoCapture );
                sa->SetUINT32( MF_DEVICESTREAM_FRAMESERVER_SHARED, TRUE );
                sa->SetUINT32( MF_DEVICESTREAM_MAX_FRAME_BUFFERS, 2 );
                sa->Release();
            }

            // Set current media type on stream descriptor
            IMFMediaTypeHandler* handler = nullptr;
            if( SUCCEEDED( sd->GetMediaTypeHandler( &handler ) ) )
            {
                handler->SetCurrentMediaType( types[0] );
                handler->Release();
            }

            // Create the stream object
            m_spStream = new (std::nothrow) ProbeMediaStream();
            if( !m_spStream ) { sd->Release(); return E_OUTOFMEMORY; }
            hr = m_spStream->Initialize( this, sd );

            // Presentation descriptor
            IMFStreamDescriptor* descs[] = { sd };
            if( SUCCEEDED( hr ) )
                hr = MFCreatePresentationDescriptor( 1, descs, &m_spPD );
            sd->Release();
            if( FAILED( hr ) ) return hr;

            m_spPD->SelectStream( 0 );
            m_state = State::Stopped;
            return S_OK;
        }

        // IUnknown
        STDMETHODIMP QueryInterface( REFIID riid, void** obj ) override
        {
            if( !obj ) return E_POINTER; *obj = nullptr;
            if( riid == __uuidof( IUnknown ) || riid == __uuidof( IMFMediaEventGenerator ) ||
                riid == __uuidof( IMFMediaSource ) || riid == __uuidof( IMFMediaSourceEx ) )
                *obj = static_cast<IMFMediaSourceEx*>( this );
            else if( riid == __uuidof( IMFGetService ) )
                *obj = static_cast<IMFGetService*>( this );
            else if( riid == __uuidof( IKsControl ) )
                *obj = static_cast<IKsControl*>( this );
            else return E_NOINTERFACE;
            AddRef(); return S_OK;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }
        STDMETHODIMP_(ULONG) Release() override { ULONG c = --m_ref; if( !c ) delete this; return c; }

        // IMFMediaEventGenerator
        STDMETHODIMP BeginGetEvent( IMFAsyncCallback* cb, IUnknown* st ) override
        { return m_spEventQueue ? m_spEventQueue->BeginGetEvent( cb, st ) : MF_E_SHUTDOWN; }
        STDMETHODIMP EndGetEvent( IMFAsyncResult* r, IMFMediaEvent** e ) override
        { return m_spEventQueue ? m_spEventQueue->EndGetEvent( r, e ) : MF_E_SHUTDOWN; }
        STDMETHODIMP GetEvent( DWORD f, IMFMediaEvent** e ) override
        { return m_spEventQueue ? m_spEventQueue->GetEvent( f, e ) : MF_E_SHUTDOWN; }
        STDMETHODIMP QueueEvent( MediaEventType t, REFGUID ext, HRESULT s, const PROPVARIANT* v ) override
        { return m_spEventQueue ? m_spEventQueue->QueueEventParamVar( t, ext, s, v ) : MF_E_SHUTDOWN; }

        // IMFMediaSource
        STDMETHODIMP GetCharacteristics( DWORD* ch ) override
        { if( !ch ) return E_POINTER; *ch = MFMEDIASOURCE_IS_LIVE; return S_OK; }

        STDMETHODIMP CreatePresentationDescriptor( IMFPresentationDescriptor** ppPD ) override
        {
            if( !ppPD ) return E_POINTER; *ppPD = nullptr;
            DllLog( L"[VCamSrc] CreatePresentationDescriptor\n" );
            return m_spPD ? m_spPD->Clone( ppPD ) : E_UNEXPECTED;
        }

        STDMETHODIMP Start( IMFPresentationDescriptor* pd, const GUID* tf, const PROPVARIANT* sp ) override
        {
            DllLog( L"[VCamSrc] Start\n" );
            if( !pd || !sp ) return E_INVALIDARG;
            if( tf && *tf != GUID_NULL ) return MF_E_UNSUPPORTED_TIME_FORMAT;

            // Determine if stream was previously selected
            bool wasStarted = ( m_state == State::Started );
            m_state = State::Started;

            // Walk stream descriptors and start selected streams
            DWORD count = 0;
            pd->GetStreamDescriptorCount( &count );
            for( DWORD i = 0; i < count; i++ )
            {
                BOOL selected = FALSE;
                IMFStreamDescriptor* sd = nullptr;
                pd->GetStreamDescriptorByIndex( i, &selected, &sd );
                if( sd ) sd->Release();

                if( selected && m_spStream )
                {
                    // Queue MENewStream or MEUpdatedStream
                    IUnknown* streamUnk = nullptr;
                    m_spStream->QueryInterface( IID_PPV_ARGS( &streamUnk ) );
                    if( m_spEventQueue )
                    {
                        MediaEventType met = wasStarted ? MEUpdatedStream : MENewStream;
                        m_spEventQueue->QueueEventParamUnk( met, GUID_NULL, S_OK, streamUnk );
                    }
                    if( streamUnk ) streamUnk->Release();

                    // Start the stream
                    m_spStream->StartStream();
                }
            }

            // Source started event
            PROPVARIANT t; InitPropVariantFromInt64( MFGetSystemTime(), &t );
            if( m_spEventQueue )
                m_spEventQueue->QueueEventParamVar( MESourceStarted, GUID_NULL, S_OK, &t );
            PropVariantClear( &t );
            return S_OK;
        }

        STDMETHODIMP Stop() override
        {
            DllLog( L"[VCamSrc] Stop\n" );
            m_state = State::Stopped;
            if( m_spStream ) m_spStream->StopStream();
            PROPVARIANT t; InitPropVariantFromInt64( MFGetSystemTime(), &t );
            if( m_spEventQueue )
                m_spEventQueue->QueueEventParamVar( MESourceStopped, GUID_NULL, S_OK, &t );
            PropVariantClear( &t );
            return S_OK;
        }

        STDMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }

        STDMETHODIMP Shutdown() override
        {
            DllLog( L"[VCamSrc] Shutdown\n" );
            m_state = State::Shutdown;
            if( m_spStream ) { m_spStream->Shutdown(); m_spStream->Release(); m_spStream = nullptr; }
            if( m_spEventQueue ) { m_spEventQueue->Shutdown(); m_spEventQueue->Release(); m_spEventQueue = nullptr; }
            if( m_spPD ) { m_spPD->Release(); m_spPD = nullptr; }
            if( m_spAttributes ) { m_spAttributes->Release(); m_spAttributes = nullptr; }
            return S_OK;
        }

        // IMFMediaSourceEx
        STDMETHODIMP GetSourceAttributes( IMFAttributes** a ) override
        {
            if( !a ) return E_POINTER; *a = nullptr;
            if( !m_spAttributes ) return E_UNEXPECTED;
            *a = m_spAttributes; m_spAttributes->AddRef(); return S_OK;
        }
        STDMETHODIMP GetStreamAttributes( DWORD id, IMFAttributes** a ) override
        {
            if( !a ) return E_POINTER; *a = nullptr;
            if( !m_spPD ) return MF_E_NOT_FOUND;
            BOOL sel = FALSE; IMFStreamDescriptor* sd = nullptr;
            HRESULT hr = m_spPD->GetStreamDescriptorByIndex( 0, &sel, &sd );
            if( FAILED( hr ) ) return hr;
            hr = sd->QueryInterface( IID_PPV_ARGS( a ) );
            sd->Release(); return hr;
        }
        STDMETHODIMP SetD3DManager( IUnknown* ) override { return S_OK; }

        // IMFGetService
        STDMETHODIMP GetService( REFGUID, REFIID, LPVOID* o ) override
        { if( !o ) return E_POINTER; *o = nullptr; return MF_E_UNSUPPORTED_SERVICE; }

        // IKsControl
        STDMETHODIMP KsProperty( PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* ) override
        { return HRESULT_FROM_WIN32( ERROR_SET_NOT_FOUND ); }
        STDMETHODIMP KsMethod( PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* ) override
        { return HRESULT_FROM_WIN32( ERROR_SET_NOT_FOUND ); }
        STDMETHODIMP KsEvent( PKSEVENT, ULONG, LPVOID, ULONG, ULONG* ) override
        { return HRESULT_FROM_WIN32( ERROR_SET_NOT_FOUND ); }

    private:
        enum class State { Invalid, Stopped, Started, Shutdown };
        ProbeMediaStream* m_spStream = nullptr;
        IMFMediaEventQueue* m_spEventQueue = nullptr;
        IMFPresentationDescriptor* m_spPD = nullptr;
        IMFAttributes* m_spAttributes = nullptr;
        State m_state = State::Invalid;
        std::atomic<ULONG> m_ref{ 1 };
    };

    // Implement GetMediaSource after ProbeMediaSource is defined
    STDMETHODIMP ProbeMediaStream::GetMediaSource( IMFMediaSource** pp )
    {
        if( !pp ) return E_POINTER; *pp = nullptr;
        if( !m_parent ) return E_UNEXPECTED;
        *pp = static_cast<IMFMediaSource*>( m_parent );
        m_parent->AddRef();
        return S_OK;
    }

    //====================================================================
    // ProbeActivate — IMFActivate wrapper
    //====================================================================
    class ProbeActivate final : public IMFActivate
    {
    public:
        ProbeActivate() { MFCreateAttributes( &m_attr, 4 ); }
        ~ProbeActivate() { if( m_attr ) m_attr->Release(); }

        STDMETHODIMP QueryInterface( REFIID riid, void** o ) override
        {
            if( !o ) return E_POINTER; *o = nullptr;
            if( riid == __uuidof( IUnknown ) || riid == __uuidof( IMFAttributes ) || riid == __uuidof( IMFActivate ) )
            { *o = static_cast<IMFActivate*>( this ); AddRef(); return S_OK; }
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }
        STDMETHODIMP_(ULONG) Release() override { ULONG c = --m_ref; if( !c ) delete this; return c; }

        // IMFAttributes delegate
        STDMETHODIMP GetItem( REFGUID k, PROPVARIANT* v ) override { return m_attr ? m_attr->GetItem( k, v ) : E_UNEXPECTED; }
        STDMETHODIMP GetItemType( REFGUID k, MF_ATTRIBUTE_TYPE* t ) override { return m_attr ? m_attr->GetItemType( k, t ) : E_UNEXPECTED; }
        STDMETHODIMP CompareItem( REFGUID k, REFPROPVARIANT v, BOOL* r ) override { return m_attr ? m_attr->CompareItem( k, v, r ) : E_UNEXPECTED; }
        STDMETHODIMP Compare( IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r ) override { return m_attr ? m_attr->Compare( o, m, r ) : E_UNEXPECTED; }
        STDMETHODIMP GetUINT32( REFGUID k, UINT32* v ) override { return m_attr ? m_attr->GetUINT32( k, v ) : E_UNEXPECTED; }
        STDMETHODIMP GetUINT64( REFGUID k, UINT64* v ) override { return m_attr ? m_attr->GetUINT64( k, v ) : E_UNEXPECTED; }
        STDMETHODIMP GetDouble( REFGUID k, double* v ) override { return m_attr ? m_attr->GetDouble( k, v ) : E_UNEXPECTED; }
        STDMETHODIMP GetGUID( REFGUID k, GUID* v ) override { return m_attr ? m_attr->GetGUID( k, v ) : E_UNEXPECTED; }
        STDMETHODIMP GetStringLength( REFGUID k, UINT32* l ) override { return m_attr ? m_attr->GetStringLength( k, l ) : E_UNEXPECTED; }
        STDMETHODIMP GetString( REFGUID k, LPWSTR v, UINT32 s, UINT32* l ) override { return m_attr ? m_attr->GetString( k, v, s, l ) : E_UNEXPECTED; }
        STDMETHODIMP GetAllocatedString( REFGUID k, LPWSTR* v, UINT32* l ) override { return m_attr ? m_attr->GetAllocatedString( k, v, l ) : E_UNEXPECTED; }
        STDMETHODIMP GetBlobSize( REFGUID k, UINT32* s ) override { return m_attr ? m_attr->GetBlobSize( k, s ) : E_UNEXPECTED; }
        STDMETHODIMP GetBlob( REFGUID k, UINT8* b, UINT32 s, UINT32* o ) override { return m_attr ? m_attr->GetBlob( k, b, s, o ) : E_UNEXPECTED; }
        STDMETHODIMP GetAllocatedBlob( REFGUID k, UINT8** b, UINT32* s ) override { return m_attr ? m_attr->GetAllocatedBlob( k, b, s ) : E_UNEXPECTED; }
        STDMETHODIMP GetUnknown( REFGUID k, REFIID r, LPVOID* v ) override { return m_attr ? m_attr->GetUnknown( k, r, v ) : E_UNEXPECTED; }
        STDMETHODIMP SetItem( REFGUID k, REFPROPVARIANT v ) override { return m_attr ? m_attr->SetItem( k, v ) : E_UNEXPECTED; }
        STDMETHODIMP DeleteItem( REFGUID k ) override { return m_attr ? m_attr->DeleteItem( k ) : E_UNEXPECTED; }
        STDMETHODIMP DeleteAllItems() override { return m_attr ? m_attr->DeleteAllItems() : E_UNEXPECTED; }
        STDMETHODIMP SetUINT32( REFGUID k, UINT32 v ) override { return m_attr ? m_attr->SetUINT32( k, v ) : E_UNEXPECTED; }
        STDMETHODIMP SetUINT64( REFGUID k, UINT64 v ) override { return m_attr ? m_attr->SetUINT64( k, v ) : E_UNEXPECTED; }
        STDMETHODIMP SetDouble( REFGUID k, double v ) override { return m_attr ? m_attr->SetDouble( k, v ) : E_UNEXPECTED; }
        STDMETHODIMP SetGUID( REFGUID k, REFGUID v ) override { return m_attr ? m_attr->SetGUID( k, v ) : E_UNEXPECTED; }
        STDMETHODIMP SetString( REFGUID k, LPCWSTR v ) override { return m_attr ? m_attr->SetString( k, v ) : E_UNEXPECTED; }
        STDMETHODIMP SetBlob( REFGUID k, const UINT8* b, UINT32 s ) override { return m_attr ? m_attr->SetBlob( k, b, s ) : E_UNEXPECTED; }
        STDMETHODIMP SetUnknown( REFGUID k, IUnknown* v ) override { return m_attr ? m_attr->SetUnknown( k, v ) : E_UNEXPECTED; }
        STDMETHODIMP LockStore() override { return m_attr ? m_attr->LockStore() : E_UNEXPECTED; }
        STDMETHODIMP UnlockStore() override { return m_attr ? m_attr->UnlockStore() : E_UNEXPECTED; }
        STDMETHODIMP GetCount( UINT32* c ) override { return m_attr ? m_attr->GetCount( c ) : E_UNEXPECTED; }
        STDMETHODIMP GetItemByIndex( UINT32 i, GUID* k, PROPVARIANT* v ) override { return m_attr ? m_attr->GetItemByIndex( i, k, v ) : E_UNEXPECTED; }
        STDMETHODIMP CopyAllItems( IMFAttributes* d ) override { return m_attr ? m_attr->CopyAllItems( d ) : E_UNEXPECTED; }

        STDMETHODIMP ActivateObject( REFIID riid, void** obj ) override
        {
            if( !obj ) return E_POINTER; *obj = nullptr;
            DllLog( L"[VCamSrc] ActivateObject riid=%s\n", GuidToStr( riid ).c_str() );
            auto* src = new (std::nothrow) ProbeMediaSource();
            if( !src ) return E_OUTOFMEMORY;
            HRESULT hr = src->Initialize( m_attr );
            if( FAILED( hr ) ) { src->Release(); return hr; }
            hr = src->QueryInterface( riid, obj );
            src->Release(); return hr;
        }
        STDMETHODIMP ShutdownObject() override { return S_OK; }
        STDMETHODIMP DetachObject() override { return S_OK; }

    private:
        IMFAttributes* m_attr = nullptr;
        std::atomic<ULONG> m_ref{ 1 };
    };

    //====================================================================
    // Class factory
    //====================================================================
    class ProbeClassFactory final : public IClassFactory
    {
    public:
        STDMETHODIMP QueryInterface( REFIID riid, void** o ) override
        {
            if( !o ) return E_POINTER; *o = nullptr;
            if( riid == __uuidof( IUnknown ) || riid == __uuidof( IClassFactory ) )
            { *o = static_cast<IClassFactory*>( this ); AddRef(); return S_OK; }
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }
        STDMETHODIMP_(ULONG) Release() override { ULONG c = --m_ref; if( !c ) delete this; return c; }

        STDMETHODIMP CreateInstance( IUnknown* outer, REFIID riid, void** obj ) override
        {
            if( !obj ) return E_POINTER; *obj = nullptr;
            DllLog( L"[VCamSrc] ClassFactory::CreateInstance riid=%s\n", GuidToStr( riid ).c_str() );
            if( outer ) return CLASS_E_NOAGGREGATION;
            auto* act = new (std::nothrow) ProbeActivate();
            if( !act ) return E_OUTOFMEMORY;
            HRESULT hr = act->QueryInterface( riid, obj );
            act->Release(); return hr;
        }
        STDMETHODIMP LockServer( BOOL ) override { return S_OK; }

    private:
        std::atomic<ULONG> m_ref{ 1 };
    };
}

extern "C" BOOL WINAPI DllMain( HINSTANCE, DWORD, LPVOID ) { return TRUE; }

STDAPI DllCanUnloadNow() { return S_FALSE; }

STDAPI DllGetClassObject( REFCLSID clsid, REFIID riid, LPVOID* obj )
{
    if( !obj ) return E_POINTER; *obj = nullptr;
    DllLog( L"[VCamSrc] DllGetClassObject clsid=%s riid=%s\n",
            GuidToStr( clsid ).c_str(), GuidToStr( riid ).c_str() );
    if( clsid != CLSID_ZoomItVirtualCameraProbeSource )
        return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new (std::nothrow) ProbeClassFactory();
    if( !f ) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface( riid, obj );
    f->Release(); return hr;
}
