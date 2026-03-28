//==============================================================================
//
// WebcamCapture.h
//
// Captures frames from a webcam using Media Foundation's IMFSourceReader.
// The capture thread stores raw BGRA pixel buffers; compositing onto the
// recording frame happens on the caller's thread using D3D11.
//
// Copyright (C) Mark Russinovich
// Sysinternals - www.sysinternals.com
//
//==============================================================================
#pragma once

#include <d3d11_4.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <winrt/base.h>

class WebcamCapture
{
public:
    // Position constants matching g_WebcamPosition values.
    enum Position { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3 };

    // Size constants matching g_WebcamSize values.
    enum Size { Small = 0, Medium = 1, Large = 2, XLarge = 3 };

    WebcamCapture(
        winrt::com_ptr<ID3D11Device> const& device,
        winrt::com_ptr<ID3D11DeviceContext> const& context,
        const wchar_t* deviceSymLink,
        UINT outputWidth,
        UINT outputHeight,
        Position position,
        Size size );
    ~WebcamCapture();

    // Start/stop the capture thread.
    bool Start();
    void Stop();

    // Composite the latest camera frame onto the given texture.
    // Must be called from the thread that owns m_d3dContext.
    // Returns true if a frame was composited.
    bool CompositeOnto( ID3D11Texture2D* target );

private:
    void CaptureThread();
    bool InitSourceReader();
    RECT ComputeDestRect() const;
    void ComputeOverlayDimensions();

    winrt::com_ptr<ID3D11Device>        m_d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
    winrt::com_ptr<IMFSourceReader>     m_sourceReader;
    std::wstring                        m_deviceSymLink;

    // Pre-scaled overlay pixels produced by the capture thread.
    // CompositeOnto consumes them and uploads to a cached GPU texture.
    std::mutex                          m_frameLock;
    std::vector<BYTE>                   m_pendingPixels;   // new frame waiting
    bool                                m_newFrameReady = false;

    // Cached GPU texture (owned by CompositeOnto's thread only).
    winrt::com_ptr<ID3D11Texture2D>     m_cachedOverlay;

    UINT                                m_overlayW = 0;
    UINT                                m_overlayH = 0;
    UINT                                m_camWidth = 0;
    UINT                                m_camHeight = 0;
    RECT                                m_destRect = {};

    // Output dimensions (recording output after crop+scale).
    UINT                                m_outputWidth = 0;
    UINT                                m_outputHeight = 0;
    Position                            m_position = BottomRight;
    Size                                m_size = Medium;

    // Capture thread.
    std::thread                         m_thread;
    std::atomic<bool>                   m_running{ false };
    bool                                m_mfStarted = false;

    // Signalled once the first webcam frame has been captured so
    // Start() can block until the overlay is ready.
    std::mutex                          m_readyMutex;
    std::condition_variable             m_readyCV;
    bool                                m_firstFrameCaptured = false;
};
