/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_KINECT_DEVICE_H_
#define XENIA_KERNEL_XAM_KINECT_DEVICE_H_

#include "xenia/base/byte_order.h"
#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "xenia/base/platform_win.h"

namespace xe {
namespace kernel {
namespace xam {

// -------------------------------------------------------------------------
// Xbox 360 NUI structures stored in guest (big-endian) memory
// -------------------------------------------------------------------------

// A 4-component vector stored in big-endian byte order.
struct X_NUI_VECTOR4 {
  xe::be<float> x, y, z, w;
};

constexpr uint32_t kNuiSkeletonPositionCount = 20;
constexpr uint32_t kNuiSkeletonCount = 6;

// Tracking state for an individual skeleton.
enum X_NUI_SKELETON_TRACKING_STATE : uint32_t {
  X_NUI_SKELETON_NOT_TRACKED = 0,
  X_NUI_SKELETON_POSITION_ONLY = 1,
  X_NUI_SKELETON_TRACKED = 2,
};

// Per-joint tracking quality.
enum X_NUI_SKELETON_POSITION_TRACKING_STATE : uint32_t {
  X_NUI_SKELETON_POSITION_NOT_TRACKED = 0,
  X_NUI_SKELETON_POSITION_INFERRED = 1,
  X_NUI_SKELETON_POSITION_TRACKED = 2,
};

// Xbox 360 NUI_SKELETON_DATA in guest (big-endian) memory layout.
struct X_NUI_SKELETON_DATA {
  xe::be<uint32_t> tracking_state;  // X_NUI_SKELETON_TRACKING_STATE
  xe::be<uint32_t> tracking_id;
  xe::be<uint32_t> enrollment_index;
  xe::be<uint32_t> user_index;
  X_NUI_VECTOR4 position;
  X_NUI_VECTOR4 skeleton_positions[kNuiSkeletonPositionCount];
  xe::be<uint32_t>
      position_tracking_state[kNuiSkeletonPositionCount];  // per-joint state
  xe::be<uint32_t> quality_flags;
};

// Xbox 360 NUI_SKELETON_FRAME in guest (big-endian) memory layout.
struct X_NUI_SKELETON_FRAME {
  xe::be<int64_t> timestamp;
  xe::be<uint32_t> frame_number;
  xe::be<uint32_t> flags;
  X_NUI_VECTOR4 floor_clip_plane;
  X_NUI_VECTOR4 normal_to_gravity;
  X_NUI_SKELETON_DATA skeleton_data[kNuiSkeletonCount];
};

// -------------------------------------------------------------------------
// KinectDevice — wraps the Windows Kinect SDK v1 (Kinect10.dll)
// -------------------------------------------------------------------------

// KinectDevice is a lazy-initialised singleton.  It dynamically loads
// Kinect10.dll at first use so that Xenia starts successfully on machines
// that do not have the Kinect driver stack installed.
class KinectDevice {
 public:
  // Returns the process-wide singleton (never null).
  static KinectDevice* Get();

  // Attempts to open the first available Kinect sensor.  Safe to call
  // multiple times; subsequent calls are no-ops.  Returns true when a
  // sensor was successfully opened and is ready.
  bool Initialize();

  // Shuts down the sensor and releases all Kinect resources.
  void Shutdown();

  // True when a Kinect sensor has been opened.
  bool IsConnected() const;

  // True when the sensor has been initialised (NuiInitialize succeeded).
  bool IsReady() const;

  // Copy the most-recently-received skeleton frame into *out_frame.
  // Returns false if no frame has been received yet.
  bool GetSkeletonFrame(X_NUI_SKELETON_FRAME* out_frame);

  // Returns the last valid frame number, or 0 if none.
  uint32_t GetLastFrameNumber() const;

  // Process a skeleton frame to determine the currently engaged player.
  // Called by XamNuiHudInterpretFrame and the background poll thread.
  void ProcessHudFrame(const X_NUI_SKELETON_FRAME& frame);

  // Tracking ID of the currently engaged (interacting) player, or 0 if none.
  uint32_t GetEngagedTrackingId() const;
  void SetEngagedTrackingId(uint32_t tracking_id);

  // Enrollment index (signed-in player slot) of the engaged player, or
  // kNoEnrolledPlayer (0xFF) if nobody is engaged.
  static constexpr uint32_t kNoEnrolledPlayer = 0xFF;
  uint32_t GetEngagedEnrollmentIndex() const;

  // Camera elevation angle in degrees [-27, 27].
  long GetCameraElevationAngle() const;
  bool SetCameraElevationAngle(long degrees);

  // Returns the device connection-id string (serial-like identifier).
  std::wstring GetDeviceConnectionId() const;

 private:
  KinectDevice();
  ~KinectDevice();

  // Loads Kinect10.dll and resolves the two free-function entry points.
  bool LoadKinectDll();

  // Background thread that polls for new skeleton frames.
  void PollThread();

  // ---- Kinect10.dll free-function types ----------------------------------
  typedef HRESULT(WINAPI* PFN_NuiGetSensorCount)(int* pCount);
  typedef HRESULT(WINAPI* PFN_NuiCreateSensorByIndex)(int index,
                                                       void** ppNuiSensor);

  // ---- Minimal INuiSensor vtable (Kinect SDK v1.8) -----------------------
  // Only the methods we actually call are given explicit types; the rest are
  // represented as opaque void* placeholders so the vtable offsets remain
  // correct even when those entries are never used.
  struct NuiSensorVtbl {
    // IUnknown (indices 0-2)
    void* QueryInterface;
    void* AddRef;
    void* Release;
    // INuiSensor (indices 3+)
    HRESULT(STDMETHODCALLTYPE* NuiInitialize)(void* self, DWORD dwFlags);
    void(STDMETHODCALLTYPE* NuiShutdown)(void* self);
    void* NuiSetFrameEndEvent;
    void* NuiImageStreamOpen;
    void* NuiImageStreamSetImageFrameFlags;
    void* NuiImageStreamGetNextFrame;
    void* NuiImageStreamReleaseFrame;
    HRESULT(STDMETHODCALLTYPE* NuiSkeletonTrackingEnable)(
        void* self, HANDLE hNextFrameEvent, DWORD dwFlags);
    HRESULT(STDMETHODCALLTYPE* NuiSkeletonTrackingDisable)(void* self);
    void* NuiSkeletonSetTrackedSkeletons;
    HRESULT(STDMETHODCALLTYPE* NuiSkeletonGetNextFrame)(
        void* self, DWORD dwMillisecondsToWait, void* pSkeletonFrame);
    void* NuiTransformSmooth;
    void* NuiAccelerometerGetCurrentReading;
    HRESULT(STDMETHODCALLTYPE* NuiCameraElevationSetAngle)(void* self,
                                                            LONG lAngleDegrees);
    HRESULT(STDMETHODCALLTYPE* NuiCameraElevationGetAngle)(
        void* self, LONG* plAngleDegrees);
    // The two entries below were added in Kinect SDK 1.5 and appear in the
    // vtable between NuiCameraElevationGetAngle and NuiSetDepthFilter.
    void* NuiImageGetColorPixelCoordinatesFromDepthPixel;
    void* NuiImageGetColorPixelCoordinatesFromDepthPixelAtResolution;
    void* NuiSetDepthFilter;
    void* NuiGetDepthFilter;
    void* NuiGetCoordinateMapper;
    void* NuiDepthPixelToDepth;
    BSTR(STDMETHODCALLTYPE* NuiDeviceConnectionId)(void* self);
  };

  // Native (little-endian) Kinect SDK structures.
  struct NuiVector4 {
    float x, y, z, w;
  };

  enum NuiSkeletonTrackingState : DWORD {
    kNuiSkeletonNotTracked = 0,
    kNuiSkeletonPositionOnly = 1,
    kNuiSkeletonTracked = 2,
  };

  enum NuiSkeletonPositionTrackingState : DWORD {
    kNuiPositionNotTracked = 0,
    kNuiPositionInferred = 1,
    kNuiPositionTracked = 2,
  };

  struct NuiSkeletonData {
    DWORD eTrackingState;
    DWORD dwTrackingID;
    DWORD dwEnrollmentIndex;
    DWORD dwUserIndex;
    NuiVector4 Position;
    NuiVector4 SkeletonPositions[20];
    DWORD eSkeletonPositionTrackingState[20];
    DWORD dwQualityFlags;
  };

  struct NuiSkeletonFrame {
    LARGE_INTEGER liTimeStamp;
    DWORD dwFrameNumber;
    DWORD dwFlags;
    NuiVector4 vFloorClipPlane;
    NuiVector4 vNormalToGravity;
    NuiSkeletonData SkeletonData[6];
  };

  // Kinect NUI initialisation flags for skeleton tracking.
  // NUI_INITIALIZE_FLAG_USES_DEPTH_AND_PLAYER_INDEX = 0x00000001
  // NUI_INITIALIZE_FLAG_USES_SKELETON                = 0x00000008
  //
  // Both flags must be combined.  Skeleton tracking internally relies on the
  // depth processing pipeline; initialising with the skeleton flag alone
  // (0x08) causes NuiInitialize to return S_OK yet leaves the depth pipeline
  // unstarted, which in turn makes NuiSkeletonTrackingEnable return
  // E_INVALIDARG (0x80070057).  Every official Kinect SDK skeleton sample
  // uses this combined value (0x09).
  static constexpr DWORD kNuiInitFlagUseSkeleton = 0x00000009;

  // Skeleton tracking flags.
  static constexpr DWORD kNuiSkeletonTrackingFlagDefault = 0;

  // ---- State ------------------------------------------------------------
  HMODULE kinect_dll_ = nullptr;
  PFN_NuiGetSensorCount fn_nui_get_sensor_count_ = nullptr;
  PFN_NuiCreateSensorByIndex fn_nui_create_sensor_by_index_ = nullptr;

  void* nui_sensor_ = nullptr;  // raw INuiSensor* (COM ref held)
  HANDLE skeleton_event_ = INVALID_HANDLE_VALUE;  // signaled when a new skeleton frame is ready

  std::thread poll_thread_;
  std::atomic<bool> running_{false};

  mutable std::mutex frame_mutex_;
  X_NUI_SKELETON_FRAME latest_frame_{};
  bool has_frame_ = false;

  // Engagement state: updated by ProcessHudFrame.
  uint32_t engaged_tracking_id_ = 0;
  uint32_t engaged_enrollment_index_ = kNoEnrolledPlayer;

  bool connected_ = false;
  bool ready_ = false;

  // ---- Helpers ----------------------------------------------------------
  inline NuiSensorVtbl* Vtbl() const {
    return *reinterpret_cast<NuiSensorVtbl**>(nui_sensor_);
  }

  // Convert a native NuiSkeletonFrame to the Xbox 360 big-endian layout.
  static void ConvertFrame(const NuiSkeletonFrame& src,
                           X_NUI_SKELETON_FRAME* dst);
  static void ConvertVector4(const NuiVector4& src, X_NUI_VECTOR4* dst);
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XE_PLATFORM_WIN32

#endif  // XENIA_KERNEL_XAM_KINECT_DEVICE_H_
