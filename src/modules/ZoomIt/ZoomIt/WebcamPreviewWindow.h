//==============================================================================
//
// WebcamPreviewWindow.h
//
// Shows a live on-screen preview of the webcam overlay while recording.
// The window is marked WDA_EXCLUDEFROMCAPTURE so it never appears in the
// recorded video.  It reads pre-scaled BGRA pixels from WebcamCapture
// via GetLatestPixels() and blits them with SetDIBitsToDevice.
//
// Copyright (C) Mark Russinovich
// Sysinternals - www.sysinternals.com
//
//==============================================================================
#pragma once

#include "WebcamCapture.h"
#include <vector>

class WebcamPreviewWindow
{
public:
    WebcamPreviewWindow() = default;
    ~WebcamPreviewWindow();

    // Create and show the preview window.
    //   pCapture     – the active webcam capture (for GetLatestPixels).
    //   screenRect   – the recording region in screen coordinates.
    //                  For full-screen, pass the monitor rect.
    //   outputWidth  – the recording output width (after crop+scale).
    //   outputHeight – the recording output height (after crop+scale).
    bool Create( WebcamCapture* pCapture,
                 RECT screenRect,
                 UINT outputWidth,
                 UINT outputHeight );

    // Destroy the preview window and stop the refresh timer.
    void Destroy();

    // Returns true if the preview window is active.
    bool IsActive() const { return m_hwnd != nullptr; }

private:
    static LRESULT CALLBACK WndProc( HWND, UINT, WPARAM, LPARAM );
    void OnPaint();
    void OnTimer();
    RECT ComputeScreenRect() const;

    static constexpr UINT_PTR TIMER_ID = 1;
    static constexpr UINT     TIMER_MS = 33;   // ~30 fps refresh

    HWND             m_hwnd = nullptr;
    WebcamCapture*   m_capture = nullptr;
    RECT             m_screenRect = {};       // recording region on screen
    UINT             m_outputWidth = 0;
    UINT             m_outputHeight = 0;

    // Latest BGRA pixel buffer for painting.
    std::vector<BYTE> m_pixels;
    UINT              m_pixW = 0;
    UINT              m_pixH = 0;

    // Cached BITMAPINFO for SetDIBitsToDevice.
    BITMAPINFO        m_bmi = {};
};
