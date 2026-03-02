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
#include <functional>
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

  // Callback type fired (from the poll thread) when the engagement state
  // changes.  The arguments are the new tracking_id (0 = nobody) and the new
  // enrollment_index (kNoEnrolledPlayer when nobody is engaged).
  using EngagementChangedCallback =
      std::function<void(uint32_t tracking_id, uint32_t enrollment_index)>;
  void SetEngagementChangedCallback(EngagementChangedCallback cb);

  // Camera elevation angle in degrees [-27, 27].
  long GetCameraElevationAngle() const;
  bool SetCameraElevationAngle(long degrees);

  // Returns the device connection-id string (serial-like identifier).
  std::wstring GetDeviceConnectionId() const;

 private:
  KinectDevice();
  ~KinectDevice();

  // Loads Kinect10.dll and resolves all required free-function entry points.
  bool LoadKinectDll();

  // Background thread that polls for new skeleton frames.
  void PollThread();

  // ---- Kinect10.dll free-function types ----------------------------------
  // These are the legacy global-function exports from Kinect10.dll.  Using
  // GetProcAddress for each one is far more robust than trying to replicate
  // the COM vtable layout, which differs subtly between SDK versions and
  // causes every subsequent slot to shift silently when one entry is wrong.
  typedef HRESULT(WINAPI* PFN_NuiGetSensorCount)(int* pCount);
  typedef HRESULT(WINAPI* PFN_NuiInitialize)(DWORD dwFlags);
  typedef void(WINAPI* PFN_NuiShutdown)();
  typedef HRESULT(WINAPI* PFN_NuiSkeletonTrackingEnable)(
      HANDLE hNextFrameEvent, DWORD dwFlags);
  typedef HRESULT(WINAPI* PFN_NuiSkeletonTrackingDisable)();
  typedef HRESULT(WINAPI* PFN_NuiSkeletonGetNextFrame)(
      DWORD dwMillisecondsToWait, void* pSkeletonFrame);
  typedef HRESULT(WINAPI* PFN_NuiCameraElevationGetAngle)(LONG* plAngleDegrees);
  typedef HRESULT(WINAPI* PFN_NuiCameraElevationSetAngle)(LONG lAngleDegrees);

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

  // Kinect NUI initialisation flags.
  // NUI_INITIALIZE_FLAG_USES_DEPTH_AND_PLAYER_INDEX = 0x00000001
  // NUI_INITIALIZE_FLAG_USES_SKELETON               = 0x00000008
  //
  // Both flags are required: some SDK/driver combinations will accept the
  // skeleton flag alone in NuiInitialize but then reject NuiSkeletonTrackingEnable
  // with E_INVALIDARG unless the depth-and-player-index pipeline was also
  // explicitly requested.  The SDK handles depth internally; we never open a
  // depth stream ourselves so there is no overhead from including this flag.
  static constexpr DWORD kNuiInitFlags = 0x00000001 | 0x00000008;

  // Skeleton tracking flags (none required).
  static constexpr DWORD kNuiSkeletonTrackingFlagDefault = 0;

  // ---- State ------------------------------------------------------------
  HMODULE kinect_dll_ = nullptr;
  PFN_NuiGetSensorCount fn_nui_get_sensor_count_ = nullptr;
  PFN_NuiInitialize fn_nui_initialize_ = nullptr;
  PFN_NuiShutdown fn_nui_shutdown_ = nullptr;
  PFN_NuiSkeletonTrackingEnable fn_nui_skeleton_tracking_enable_ = nullptr;
  PFN_NuiSkeletonTrackingDisable fn_nui_skeleton_tracking_disable_ = nullptr;
  PFN_NuiSkeletonGetNextFrame fn_nui_skeleton_get_next_frame_ = nullptr;
  PFN_NuiCameraElevationGetAngle fn_nui_camera_elevation_get_angle_ = nullptr;
  PFN_NuiCameraElevationSetAngle fn_nui_camera_elevation_set_angle_ = nullptr;

  HANDLE skeleton_event_ = nullptr;  // signalled by the SDK when a new frame is ready

  std::thread poll_thread_;
  std::atomic<bool> running_{false};

  mutable std::mutex frame_mutex_;
  X_NUI_SKELETON_FRAME latest_frame_{};
  bool has_frame_ = false;

  // Engagement state: updated by ProcessHudFrame.
  uint32_t engaged_tracking_id_ = 0;
  uint32_t engaged_enrollment_index_ = kNoEnrolledPlayer;

  // Optional callback fired whenever the engagement state changes.
  EngagementChangedCallback engagement_changed_callback_;

  bool connected_ = false;
  bool nui_initialized_ = false;  // true once NuiInitialize has succeeded
  bool ready_ = false;

  // ---- Helpers ----------------------------------------------------------
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
