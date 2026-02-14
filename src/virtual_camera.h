#pragma once

#include <cstdint>
#include <atomic>

// Virtual Camera module - writes frames directly to OBS Virtual Camera shared memory
// This works independently of OBS Studio - the driver just needs to be installed

// Start the virtual camera output
// Creates the shared memory queue for the virtual camera
// width/height: resolution of the video output
// Returns true if successful
bool StartVirtualCamera(uint32_t width, uint32_t height, int fps = 30);

// Stop the virtual camera output and clean up resources
void StopVirtualCamera();

// Write a frame to the virtual camera
// rgba_data: pointer to RGBA pixel data (width * height * 4 bytes)
// width/height: frame dimensions
// timestamp: frame timestamp in 100-nanosecond units (can use QueryPerformanceCounter-based value)
// Returns true if frame was written successfully
bool WriteVirtualCameraFrame(const uint8_t* rgba_data, uint32_t width, uint32_t height, uint64_t timestamp);

// Write a pre-converted NV12 frame to the virtual camera (GPU path)
// nv12_data must be width*height*3/2 bytes (NV12 format)
bool WriteVirtualCameraFrameNV12(const uint8_t* nv12_data, uint32_t width, uint32_t height, uint64_t timestamp);

// Check if virtual camera is currently active
bool IsVirtualCameraActive();

// Check if OBS Virtual Camera driver is installed
// Looks for the registry entry or DLL presence
bool IsVirtualCameraDriverInstalled();

// Check if the virtual camera shared memory is already in use (e.g., by OBS)
// Returns true if another process has the virtual camera queue open
bool IsVirtualCameraInUseByOBS();

// Get the last error message for virtual camera operations
const char* GetVirtualCameraError();

// Internal: state tracking
extern std::atomic<bool> g_virtualCameraActive;
