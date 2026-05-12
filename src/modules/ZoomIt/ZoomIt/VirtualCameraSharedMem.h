//==============================================================================
//
// VirtualCameraSharedMem.h
//
// Shared memory layout for passing captured frames from ZoomIt (producer)
// to the virtual camera source DLL (consumer) running in FrameServer.
//
//==============================================================================
#pragma once

#include <windows.h>

// Named shared memory for frame transfer.
// The DLL (running as SYSTEM in FrameServer) creates in Global\ namespace.
// ZoomIt opens it — OpenFileMapping doesn't need SeCreateGlobalPrivilege, only CreateFileMapping does.
inline constexpr wchar_t kVCamSharedMemName[]  = L"Global\\ZoomItVirtualCameraFrame";
inline constexpr wchar_t kVCamMutexName[]      = L"Global\\ZoomItVirtualCameraFrameMutex";

// Frame dimensions — fixed at 1280x720 NV12 for now
inline constexpr UINT32 kVCamWidth  = 1280;
inline constexpr UINT32 kVCamHeight = 720;
// NV12: Y plane = w*h, UV plane = w*h/2
inline constexpr DWORD  kVCamNV12Size = kVCamWidth * kVCamHeight * 3 / 2;

// Header at the start of shared memory
struct VCamSharedFrameHeader
{
    volatile LONG  frameNumber;   // Incremented by producer each new frame
    UINT32         width;         // Frame width (always kVCamWidth for now)
    UINT32         height;        // Frame height (always kVCamHeight for now)
    UINT32         dataSize;      // Size of frame data after header
    UINT32         reserved[4];   // Padding for future use
};

// Total shared memory size
inline constexpr DWORD kVCamSharedMemSize = sizeof( VCamSharedFrameHeader ) + kVCamNV12Size;
