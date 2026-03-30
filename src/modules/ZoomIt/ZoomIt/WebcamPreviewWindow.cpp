//==============================================================================
//
// WebcamPreviewWindow.cpp
//
// On-screen webcam preview during recording.
//
// Copyright (C) Mark Russinovich
// Sysinternals - www.sysinternals.com
//
//==============================================================================
#include "pch.h"
#include "WebcamPreviewWindow.h"

// Defined in Zoomit.cpp; compiles to nothing in Release builds.
void OutputDebug( const TCHAR* format, ... );

static const wchar_t* const kClassName = L"ZoomItWebcamPreview";
static bool s_classRegistered = false;

//----------------------------------------------------------------------------
// WebcamPreviewWindow::~WebcamPreviewWindow
//----------------------------------------------------------------------------
WebcamPreviewWindow::~WebcamPreviewWindow()
{
    Destroy();
}

//----------------------------------------------------------------------------
// WebcamPreviewWindow::ComputeScreenRect
//
// Maps the webcam overlay's position from recording-output coordinates
// to screen coordinates within the recording region.
//----------------------------------------------------------------------------
RECT WebcamPreviewWindow::ComputeScreenRect() const
{
    RECT dest = m_capture->GetDestRect();

    int screenW = m_screenRect.right - m_screenRect.left;
    int screenH = m_screenRect.bottom - m_screenRect.top;
    int outW = static_cast<int>( m_outputWidth );
    int outH = static_cast<int>( m_outputHeight );

    if( outW <= 0 || outH <= 0 )
        return {};

    // Map from output coordinates to screen coordinates.
    RECT r;
    r.left   = m_screenRect.left + MulDiv( dest.left,   screenW, outW );
    r.top    = m_screenRect.top  + MulDiv( dest.top,    screenH, outH );
    r.right  = m_screenRect.left + MulDiv( dest.right,  screenW, outW );
    r.bottom = m_screenRect.top  + MulDiv( dest.bottom, screenH, outH );
    return r;
}

//----------------------------------------------------------------------------
// WebcamPreviewWindow::Create
//----------------------------------------------------------------------------
bool WebcamPreviewWindow::Create(
    WebcamCapture* pCapture,
    RECT screenRect,
    UINT outputWidth,
    UINT outputHeight )
{
    if( !pCapture )
        return false;

    m_capture = pCapture;
    m_screenRect = screenRect;
    m_outputWidth = outputWidth;
    m_outputHeight = outputHeight;

    // Register the window class once.
    if( !s_classRegistered )
    {
        WNDCLASSEXW wc = { sizeof( wc ) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW( nullptr );
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursor( nullptr, IDC_ARROW );
        wc.hbrBackground = static_cast<HBRUSH>( GetStockObject( BLACK_BRUSH ) );
        if( !RegisterClassExW( &wc ) )
            return false;
        s_classRegistered = true;
    }

    RECT r = ComputeScreenRect();
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if( w <= 0 || h <= 0 )
        return false;

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClassName,
        L"",
        WS_POPUP,
        r.left, r.top, w, h,
        nullptr,     // no owner — avoids hidden-owner issues
        nullptr,
        GetModuleHandleW( nullptr ),
        this );

    if( !m_hwnd )
        return false;

    // Exclude from screen capture so it doesn't appear in the recording.
    typedef BOOL( WINAPI* PSWA )( HWND, DWORD );
    auto pSetWindowDisplayAffinity = reinterpret_cast<PSWA>(
        GetProcAddress( GetModuleHandleW( L"user32.dll" ), "SetWindowDisplayAffinity" ) );
    if( pSetWindowDisplayAffinity )
        pSetWindowDisplayAffinity( m_hwnd, WDA_EXCLUDEFROMCAPTURE );

    ShowWindow( m_hwnd, SW_SHOWNA );
    SetTimer( m_hwnd, TIMER_ID, TIMER_MS, nullptr );

    OutputDebug( L"[WebcamPreview] Created: screen=(%d,%d)-(%d,%d) size=%dx%d\n",
                 r.left, r.top, r.right, r.bottom, w, h );
    return true;
}

//----------------------------------------------------------------------------
// WebcamPreviewWindow::Destroy
//----------------------------------------------------------------------------
void WebcamPreviewWindow::Destroy()
{
    if( m_hwnd )
    {
        KillTimer( m_hwnd, TIMER_ID );
        DestroyWindow( m_hwnd );
        m_hwnd = nullptr;
    }
    m_capture = nullptr;
}

//----------------------------------------------------------------------------
// WebcamPreviewWindow::OnTimer
//
// Fetches the latest webcam pixels and triggers a repaint.
//----------------------------------------------------------------------------
void WebcamPreviewWindow::OnTimer()
{
    if( !m_capture )
        return;

    // Re-assert topmost Z-order so the preview stays above the live zoom
    // magnification window during drawing mode.  SWP_NOACTIVATE keeps
    // keyboard focus where it belongs.
    SetWindowPos( m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                  SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE );

    UINT w = 0, h = 0;
    if( m_capture->GetLatestPixels( m_pixels, w, h ) )
    {
        if( w != m_pixW || h != m_pixH )
        {
            m_pixW = w;
            m_pixH = h;

            memset( &m_bmi, 0, sizeof( m_bmi ) );
            m_bmi.bmiHeader.biSize = sizeof( BITMAPINFOHEADER );
            m_bmi.bmiHeader.biWidth = static_cast<LONG>( w );
            // Negative height = top-down DIB (BGRA from webcam is top-down).
            m_bmi.bmiHeader.biHeight = -static_cast<LONG>( h );
            m_bmi.bmiHeader.biPlanes = 1;
            m_bmi.bmiHeader.biBitCount = 32;
            m_bmi.bmiHeader.biCompression = BI_RGB;
        }
        InvalidateRect( m_hwnd, nullptr, FALSE );
    }
}

//----------------------------------------------------------------------------
// WebcamPreviewWindow::OnPaint
//----------------------------------------------------------------------------
void WebcamPreviewWindow::OnPaint()
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint( m_hwnd, &ps );

    if( m_pixW > 0 && m_pixH > 0 && !m_pixels.empty() )
    {
        RECT rc;
        GetClientRect( m_hwnd, &rc );
        int clientW = rc.right - rc.left;
        int clientH = rc.bottom - rc.top;

        // Stretch-blit the webcam pixels to fill the preview window.
        StretchDIBits( hdc,
                       0, 0, clientW, clientH,
                       0, 0, static_cast<int>( m_pixW ), static_cast<int>( m_pixH ),
                       m_pixels.data(),
                       &m_bmi,
                       DIB_RGB_COLORS,
                       SRCCOPY );
    }
    else
    {
        // No frame yet — fill with black.
        RECT rc;
        GetClientRect( m_hwnd, &rc );
        FillRect( hdc, &rc, static_cast<HBRUSH>( GetStockObject( BLACK_BRUSH ) ) );
    }

    EndPaint( m_hwnd, &ps );
}

//----------------------------------------------------------------------------
// WebcamPreviewWindow::WndProc
//----------------------------------------------------------------------------
LRESULT CALLBACK WebcamPreviewWindow::WndProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    WebcamPreviewWindow* self = nullptr;

    if( msg == WM_NCCREATE )
    {
        auto cs = reinterpret_cast<CREATESTRUCTW*>( lParam );
        self = static_cast<WebcamPreviewWindow*>( cs->lpCreateParams );
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( self ) );
    }
    else
    {
        self = reinterpret_cast<WebcamPreviewWindow*>( GetWindowLongPtrW( hwnd, GWLP_USERDATA ) );
    }

    if( self )
    {
        switch( msg )
        {
        case WM_TIMER:
            if( wParam == TIMER_ID )
            {
                self->OnTimer();
                return 0;
            }
            break;

        case WM_PAINT:
            self->OnPaint();
            return 0;

        case WM_ERASEBKGND:
            return 1;  // Handled — avoid flicker.

        case WM_DESTROY:
            self->m_hwnd = nullptr;
            return 0;
        }
    }

    return DefWindowProcW( hwnd, msg, wParam, lParam );
}
