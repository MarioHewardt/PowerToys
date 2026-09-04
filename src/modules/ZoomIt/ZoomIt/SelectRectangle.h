//==============================================================================
//
// Zoomit
// Sysinternals - www.sysinternals.com
//
// Class to select a recording rectangle and show it while recording
//
//==============================================================================
#pragma once

#include "pch.h"
#include <functional>

class SelectRectangle
{
public:
    ~SelectRectangle() { Stop(); };

    void Alpha( BYTE alpha ) { m_alpha = alpha; }
    BYTE Alpha() const { return m_alpha; }
    void MinSize( int minSize ) { m_minSize = minSize; }
    int MinSize() const { return m_minSize; }
    void RetainDimmedExteriorAfterSelection( bool retain ) { m_retainDimmedExterior = retain; }
    RECT SelectedRect() const { return m_selectedRect; }
    bool IsActive() const { return m_window != nullptr; }

    //
    // Optional notifications raised while the user drags out a selection. They
    // are invoked on the thread that called Start, from within its message
    // loop, and are only used when set.
    //
    void OnDragStarted( std::function<void()> callback ) { m_onDragStarted = std::move( callback ); }
    void OnDragCompleted( std::function<void()> callback ) { m_onDragCompleted = std::move( callback ); }
    void OnCancelled( std::function<void()> callback ) { m_onCancelled = std::move( callback ); }

    //
    // Raised as the selection is dragged out, with the current selection in
    // screen coordinates, so that overlays can follow it.
    //
    void OnSelectionChanged( std::function<void( const RECT& )> callback ) { m_onSelectionChanged = std::move( callback ); }

    //
    // Raised when the user taps the dictation key while the selection is being
    // dragged. Only Escape is otherwise consumed during a drag, so this costs
    // no global hotkey.
    //
    void OnDictateRequested( std::function<void()> callback ) { m_onDictateRequested = std::move( callback ); }

    bool Start( HWND ownerWindow = nullptr, bool fullMonitor = false );
    void Stop();
    void UpdateOwner( HWND window );
    void Hide() { if( m_window ) ShowWindow( m_window.get(), SW_HIDE ); }
    void Show() { if( m_window ) ShowWindow( m_window.get(), SW_SHOWNA ); }
    void SetExcludeFromCapture( bool exclude ) { if( m_window ) SetWindowDisplayAffinity( m_window.get(), exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE ); }

private:
    BYTE m_alpha = 176;
    int m_minSize = 34;
    RECT m_selectedRect{};

    bool m_cancel = false;
    const wchar_t* m_className = L"ZoomitSelectRectangle";
    UINT m_dpi{};
    RECT m_oldClipRect{};
    bool m_selected{ false };
    bool m_setClip{ false };
    bool m_stopping{ false };
    bool m_dragStarted{ false };
    bool m_retainDimmedExterior{ false };
    POINT m_startPoint{};
    wil::unique_hwnd m_window;

    std::function<void()> m_onDragStarted;
    std::function<void()> m_onDragCompleted;
    std::function<void()> m_onCancelled;
    std::function<void()> m_onDictateRequested;
    std::function<void( const RECT& )> m_onSelectionChanged;

    void NotifyDragStarted();
    void NotifyDragCompleted();
    void NotifyCancelled();
    void NotifyDictateRequested();
    void NotifySelectionChanged();

    void ShowSelected();
    LRESULT WindowProc( HWND window, UINT message, WPARAM wordParam, LPARAM longParam );
};
