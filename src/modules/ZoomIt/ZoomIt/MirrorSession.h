//==============================================================================
//
// Zoomit
// Sysinternals - www.sysinternals.com
//
// Live desktop mirroring support.
//
//==============================================================================
#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

class MirrorSession
{
public:
    MirrorSession( HMONITOR sourceMonitor, HMONITOR targetMonitor );
    ~MirrorSession();

    MirrorSession( const MirrorSession& ) = delete;
    MirrorSession& operator=( const MirrorSession& ) = delete;

    bool Start();
    void Stop();
    bool IsRunning() const;

private:
    static LRESULT CALLBACK WindowProc( HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam );

    void ThreadMain();
    bool CreateMirrorWindow();
    void DestroyMirrorWindow();
    void Paint( HWND hWnd );
    void SetFramePixels( std::vector<BYTE>&& pixels, UINT width, UINT height );
    bool CopyFrameToPixels( winrt::Direct3D11::IDirect3DSurface const& surface, UINT width, UINT height );

private:
    HMONITOR m_sourceMonitor = nullptr;
    HMONITOR m_targetMonitor = nullptr;
    RECT m_targetRect{};
    std::atomic_bool m_stopRequested{ false };
    std::atomic_bool m_running{ false };
    std::atomic<HWND> m_window{ nullptr };
    std::thread m_thread;

    std::mutex m_frameLock;
    std::vector<BYTE> m_framePixels;
    UINT m_frameWidth = 0;
    UINT m_frameHeight = 0;
};
