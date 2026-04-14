//==============================================================================
//
// VirtualCameraIds.h
//
// Shared CLSIDs for ZoomIt virtual camera experiments.
//
//==============================================================================
#pragma once

#include <guiddef.h>

inline constexpr GUID CLSID_ZoomItVirtualCameraProbeSource =
{ 0x6d537913, 0x9ce7, 0x4aa4, { 0x90, 0x80, 0x65, 0x0b, 0x71, 0x49, 0x4a, 0x86 } };

inline constexpr wchar_t kZoomItVirtualCameraSourceTraceFileName[] = L"ZoomItVirtualCameraSource.log";