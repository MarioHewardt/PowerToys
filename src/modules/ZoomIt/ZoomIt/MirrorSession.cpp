//==============================================================================
//
// Zoomit
// Sysinternals - www.sysinternals.com
//
// Live desktop mirroring support.
//
//==============================================================================
#include "pch.h"

#include "MirrorSession.h"
#include "CaptureFrameWait.h"

namespace winrt
{
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
    using namespace Windows::Foundation;
}

namespace util
{
    using namespace robmikh::common::uwp;
    using namespace robmikh::common::desktop;
}

namespace
{
    constexpr wchar_t c_mirrorWindowClass[] = L"ZoomItMirrorWindow";

    RECT GetMonitorRect( HMONITOR monitor )
    {
        MONITORINFO info{ sizeof( info ) };
        if( monitor != nullptr && GetMonitorInfoW( monitor, &info ) )
        {
            return info.rcMonitor;
        }
        return RECT{};
    }

    bool IsEmptyRect( const RECT& rect )
    {
        return rect.right <= rect.left || rect.bottom <= rect.top;
    }
}

MirrorSession::MirrorSession( HMONITOR sourceMonitor, HMONITOR targetMonitor ) :
    m_sourceMonitor( sourceMonitor ),
    m_targetMonitor( targetMonitor )
{
    m_targetRect = GetMonitorRect( m_targetMonitor );
}

MirrorSession::~MirrorSession()
{
    Stop();
}

bool MirrorSession::Start()
{
    if( m_running.load() || m_thread.joinable() || m_sourceMonitor == nullptr || m_targetMonitor == nullptr || IsEmptyRect( m_targetRect ) )
    {
        return false;
    }

    m_stopRequested = false;
    m_thread = std::thread( [this] { ThreadMain(); } );
    return true;
}

void MirrorSession::Stop()
{
    m_stopRequested = true;

    HWND window = m_window.load();
    if( window != nullptr )
    {
        PostMessageW( window, WM_CLOSE, 0, 0 );
    }

    if( m_thread.joinable() )
    {
        m_thread.join();
    }
}

bool MirrorSession::IsRunning() const
{
    return m_running.load();
}

void MirrorSession::ThreadMain()
{
    winrt::init_apartment( winrt::apartment_type::multi_threaded );

    try
    {
        if( !CreateMirrorWindow() )
        {
            return;
        }

        m_running = true;

        auto d3dDevice = util::CreateD3D11Device();
        auto dxgiDevice = d3dDevice.as<IDXGIDevice>();
        auto direct3DDevice = CreateDirect3DDevice( dxgiDevice.get() );
        auto item = util::CreateCaptureItemForMonitor( m_sourceMonitor );
        auto itemSize = item.Size();

        CaptureFrameWait frameWait(
            direct3DDevice,
            item,
            winrt::SizeInt32{ itemSize.Width, itemSize.Height } );
        frameWait.EnableCursorCapture( true );
        frameWait.ShowCaptureBorder( false );

        while( !m_stopRequested.load() )
        {
            MSG msg{};
            while( PeekMessageW( &msg, nullptr, 0, 0, PM_REMOVE ) )
            {
                if( msg.message == WM_QUIT )
                {
                    m_stopRequested = true;
                    break;
                }
                TranslateMessage( &msg );
                DispatchMessageW( &msg );
            }

            if( m_stopRequested.load() )
            {
                break;
            }

            auto frame = frameWait.TryGetNextFrame( 16 );
            if( !frame )
            {
                continue;
            }

            if( CopyFrameToPixels(
                    frame->FrameTexture,
                    static_cast<UINT>( frame->ContentSize.Width ),
                    static_cast<UINT>( frame->ContentSize.Height ) ) )
            {
                HWND window = m_window.load();
                if( window != nullptr )
                {
                    InvalidateRect( window, nullptr, FALSE );
                }
            }
        }
    }
    catch( winrt::hresult_error const& error )
    {
        OutputDebugStringW( ( std::wstring( L"[Mirror] Capture failed: " ) + error.message().c_str() + L"\n" ).c_str() );
    }
    catch( ... )
    {
        OutputDebugStringW( L"[Mirror] Capture failed with an unknown exception\n" );
    }

    DestroyMirrorWindow();
    m_running = false;
    winrt::uninit_apartment();
}

bool MirrorSession::CreateMirrorWindow()
{
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = MirrorSession::WindowProc;
    windowClass.hInstance = GetModuleHandleW( nullptr );
    windowClass.hCursor = LoadCursorW( nullptr, IDC_ARROW );
    windowClass.hbrBackground = static_cast<HBRUSH>( GetStockObject( BLACK_BRUSH ) );
    windowClass.lpszClassName = c_mirrorWindowClass;

    if( RegisterClassW( &windowClass ) == 0 )
    {
        const DWORD error = GetLastError();
        if( error != ERROR_CLASS_ALREADY_EXISTS )
        {
            return false;
        }
    }

    const int width = m_targetRect.right - m_targetRect.left;
    const int height = m_targetRect.bottom - m_targetRect.top;
    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        c_mirrorWindowClass,
        L"ZoomIt Mirror",
        WS_POPUP | WS_CLIPSIBLINGS,
        m_targetRect.left,
        m_targetRect.top,
        width,
        height,
        nullptr,
        nullptr,
        GetModuleHandleW( nullptr ),
        this );

    if( window == nullptr )
    {
        return false;
    }

    m_window = window;
    ShowWindow( window, SW_SHOWNA );
    UpdateWindow( window );
    return true;
}

void MirrorSession::DestroyMirrorWindow()
{
    HWND window = m_window.exchange( nullptr );
    if( window != nullptr && IsWindow( window ) )
    {
        DestroyWindow( window );
    }
}

void MirrorSession::Paint( HWND hWnd )
{
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint( hWnd, &ps );
    if( hdc == nullptr )
    {
        return;
    }

    RECT clientRect{};
    GetClientRect( hWnd, &clientRect );
    FillRect( hdc, &clientRect, static_cast<HBRUSH>( GetStockObject( BLACK_BRUSH ) ) );

    std::vector<BYTE> pixels;
    UINT frameWidth = 0;
    UINT frameHeight = 0;
    {
        std::lock_guard<std::mutex> lock( m_frameLock );
        pixels = m_framePixels;
        frameWidth = m_frameWidth;
        frameHeight = m_frameHeight;
    }

    if( !pixels.empty() && frameWidth != 0 && frameHeight != 0 )
    {
        const int clientWidth = clientRect.right - clientRect.left;
        const int clientHeight = clientRect.bottom - clientRect.top;
        const double scale = min(
            static_cast<double>( clientWidth ) / static_cast<double>( frameWidth ),
            static_cast<double>( clientHeight ) / static_cast<double>( frameHeight ) );
        const int destWidth = max( 1, static_cast<int>( frameWidth * scale ) );
        const int destHeight = max( 1, static_cast<int>( frameHeight * scale ) );
        const int destX = ( clientWidth - destWidth ) / 2;
        const int destY = ( clientHeight - destHeight ) / 2;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof( bmi.bmiHeader );
        bmi.bmiHeader.biWidth = static_cast<LONG>( frameWidth );
        bmi.bmiHeader.biHeight = -static_cast<LONG>( frameHeight );
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode( hdc, HALFTONE );
        StretchDIBits(
            hdc,
            destX,
            destY,
            destWidth,
            destHeight,
            0,
            0,
            frameWidth,
            frameHeight,
            pixels.data(),
            &bmi,
            DIB_RGB_COLORS,
            SRCCOPY );
    }

    EndPaint( hWnd, &ps );
}

void MirrorSession::SetFramePixels( std::vector<BYTE>&& pixels, UINT width, UINT height )
{
    std::lock_guard<std::mutex> lock( m_frameLock );
    m_framePixels = std::move( pixels );
    m_frameWidth = width;
    m_frameHeight = height;
}

bool MirrorSession::CopyFrameToPixels( winrt::Direct3D11::IDirect3DSurface const& surface, UINT width, UINT height )
{
    auto texture = GetDXGIInterfaceFromObject<ID3D11Texture2D>( surface );
    winrt::com_ptr<ID3D11Device> device;
    texture->GetDevice( device.put() );

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc( &desc );
    if( width == 0 || height == 0 )
    {
        width = desc.Width;
        height = desc.Height;
    }

    desc.Width = width;
    desc.Height = height;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    winrt::com_ptr<ID3D11Texture2D> staging;
    if( FAILED( device->CreateTexture2D( &desc, nullptr, staging.put() ) ) )
    {
        return false;
    }

    winrt::com_ptr<ID3D11DeviceContext> context;
    device->GetImmediateContext( context.put() );

    D3D11_BOX sourceRegion{};
    sourceRegion.left = 0;
    sourceRegion.top = 0;
    sourceRegion.front = 0;
    sourceRegion.right = width;
    sourceRegion.bottom = height;
    sourceRegion.back = 1;

    context->CopySubresourceRegion( staging.get(), 0, 0, 0, 0, texture.get(), 0, &sourceRegion );

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if( FAILED( context->Map( staging.get(), 0, D3D11_MAP_READ, 0, &mapped ) ) )
    {
        return false;
    }

    std::vector<BYTE> pixels( static_cast<size_t>( width ) * static_cast<size_t>( height ) * 4 );
    const UINT rowBytes = width * 4;
    auto source = static_cast<const BYTE*>( mapped.pData );
    auto dest = pixels.data();
    for( UINT y = 0; y < height; ++y )
    {
        memcpy( dest, source, rowBytes );
        source += mapped.RowPitch;
        dest += rowBytes;
    }

    context->Unmap( staging.get(), 0 );
    SetFramePixels( std::move( pixels ), width, height );
    return true;
}

LRESULT CALLBACK MirrorSession::WindowProc( HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam )
{
    if( message == WM_NCCREATE )
    {
        auto createStruct = reinterpret_cast<LPCREATESTRUCTW>( lParam );
        SetWindowLongPtrW( hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( createStruct->lpCreateParams ) );
        return TRUE;
    }

    auto self = reinterpret_cast<MirrorSession*>( GetWindowLongPtrW( hWnd, GWLP_USERDATA ) );
    if( self == nullptr )
    {
        return DefWindowProcW( hWnd, message, wParam, lParam );
    }

    switch( message )
    {
    case WM_ERASEBKGND:
        return TRUE;

    case WM_PAINT:
        self->Paint( hWnd );
        return 0;

    case WM_CLOSE:
        self->m_stopRequested = true;
        return 0;

    case WM_DESTROY:
        self->m_window = nullptr;
        return 0;
    }

    return DefWindowProcW( hWnd, message, wParam, lParam );
}
