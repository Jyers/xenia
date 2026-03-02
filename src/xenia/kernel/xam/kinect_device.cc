/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/kinect_device.h"

#if XE_PLATFORM_WIN32

#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace xam {

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

KinectDevice* KinectDevice::Get() {
  static KinectDevice instance;
  return &instance;
}

KinectDevice::KinectDevice() = default;

KinectDevice::~KinectDevice() { Shutdown(); }

// ---------------------------------------------------------------------------
// Initialisation / shutdown
// ---------------------------------------------------------------------------

bool KinectDevice::LoadKinectDll() {
  if (kinect_dll_) {
    return true;
  }

  kinect_dll_ = LoadLibraryW(L"Kinect10.dll");
  if (!kinect_dll_) {
    return false;
  }

  // Load all required free-function exports directly.  This avoids the fragile
  // COM-vtable approach: a single extra or reordered method in any SDK version
  // shifts every subsequent slot and causes the wrong function to be called.
#define LOAD_NUI(name, field)                                              \
  field = reinterpret_cast<decltype(field)>(GetProcAddress(kinect_dll_, name)); \
  if (!field) {                                                            \
    XELOGE("Kinect: {} not found in Kinect10.dll — SDK too old?", name);  \
    goto load_fail;                                                        \
  }

  LOAD_NUI("NuiGetSensorCount", fn_nui_get_sensor_count_)
  LOAD_NUI("NuiInitialize", fn_nui_initialize_)
  LOAD_NUI("NuiShutdown", fn_nui_shutdown_)
  LOAD_NUI("NuiSkeletonTrackingEnable", fn_nui_skeleton_tracking_enable_)
  LOAD_NUI("NuiSkeletonTrackingDisable", fn_nui_skeleton_tracking_disable_)
  LOAD_NUI("NuiSkeletonGetNextFrame", fn_nui_skeleton_get_next_frame_)

#undef LOAD_NUI

  // Optional functions: camera elevation.  Not present in very old SDK
  // releases; silently ignore if missing.
  fn_nui_camera_elevation_get_angle_ =
      reinterpret_cast<PFN_NuiCameraElevationGetAngle>(
          GetProcAddress(kinect_dll_, "NuiCameraElevationGetAngle"));
  fn_nui_camera_elevation_set_angle_ =
      reinterpret_cast<PFN_NuiCameraElevationSetAngle>(
          GetProcAddress(kinect_dll_, "NuiCameraElevationSetAngle"));

  {
    char dll_path[MAX_PATH] = {};
    if (GetModuleFileNameA(kinect_dll_, dll_path, MAX_PATH)) {
      XELOGI("Kinect: Loaded Kinect10.dll from: {}", dll_path);
    }
  }
  return true;

load_fail:
  FreeLibrary(kinect_dll_);
  kinect_dll_ = nullptr;
  fn_nui_get_sensor_count_ = nullptr;
  fn_nui_initialize_ = nullptr;
  fn_nui_shutdown_ = nullptr;
  fn_nui_skeleton_tracking_enable_ = nullptr;
  fn_nui_skeleton_tracking_disable_ = nullptr;
  fn_nui_skeleton_get_next_frame_ = nullptr;
  fn_nui_camera_elevation_get_angle_ = nullptr;
  fn_nui_camera_elevation_set_angle_ = nullptr;
  return false;
}

bool KinectDevice::Initialize() {
  if (ready_) {
    return true;
  }

  XELOGI("Kinect: Attempting to initialise Kinect sensor.");

  if (!LoadKinectDll()) {
    XELOGI("Kinect: Kinect10.dll not found — Kinect support unavailable.");
    return false;
  }

  // Check how many sensors are attached.
  int sensor_count = 0;
  HRESULT hr = fn_nui_get_sensor_count_(&sensor_count);
  if (FAILED(hr) || sensor_count == 0) {
    XELOGI("Kinect: No Kinect sensors detected (count={}, hr=0x{:08X}).",
           sensor_count, static_cast<uint32_t>(hr));
    return false;
  }
  XELOGI("Kinect: {} sensor(s) found.", sensor_count);
  connected_ = true;

  // Initialise the default sensor (sensor 0) for skeleton tracking.
  // NUI_INITIALIZE_FLAG_USES_DEPTH_AND_PLAYER_INDEX (0x01) is included
  // alongside NUI_INITIALIZE_FLAG_USES_SKELETON (0x08): some SDK/driver
  // combinations reject NuiSkeletonTrackingEnable with E_INVALIDARG unless
  // the depth-and-player-index pipeline was also explicitly requested here.
  // The SDK manages the depth stream internally; we never open one ourselves.
  XELOGI("Kinect: Calling NuiInitialize with flags=0x{:08X}.",
         static_cast<uint32_t>(kNuiInitFlags));
  hr = fn_nui_initialize_(kNuiInitFlags);
  if (FAILED(hr)) {
    XELOGE("Kinect: NuiInitialize failed (hr=0x{:08X}).",
           static_cast<uint32_t>(hr));
    return false;
  }
  nui_initialized_ = true;
  XELOGI("Kinect: NuiInitialize succeeded (hr=0x{:08X}).",
         static_cast<uint32_t>(hr));

  // Create the frame-ready event and enable skeleton tracking.
  skeleton_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!skeleton_event_) {
    XELOGW("Kinect: CreateEvent failed (err=0x{:08X}).",
           static_cast<uint32_t>(GetLastError()));
  }
  hr = fn_nui_skeleton_tracking_enable_(skeleton_event_,
                                        kNuiSkeletonTrackingFlagDefault);
  if (FAILED(hr)) {
    XELOGE("Kinect: NuiSkeletonTrackingEnable failed (hr=0x{:08X}) — "
           "skeleton tracking unavailable.",
           static_cast<uint32_t>(hr));
    return false;
  }

  ready_ = true;

  // Start the background polling thread.
  running_ = true;
  poll_thread_ = std::thread([this]() { PollThread(); });

  XELOGI("Kinect: Sensor initialised and skeleton tracking enabled.");
  return true;
}

void KinectDevice::Shutdown() {
  if (!connected_) {
    if (kinect_dll_) {
      FreeLibrary(kinect_dll_);
      kinect_dll_ = nullptr;
      fn_nui_get_sensor_count_ = nullptr;
      fn_nui_initialize_ = nullptr;
      fn_nui_shutdown_ = nullptr;
      fn_nui_skeleton_tracking_enable_ = nullptr;
      fn_nui_skeleton_tracking_disable_ = nullptr;
      fn_nui_skeleton_get_next_frame_ = nullptr;
      fn_nui_camera_elevation_get_angle_ = nullptr;
      fn_nui_camera_elevation_set_angle_ = nullptr;
    }
    return;
  }

  // Stop the polling thread first.
  running_ = false;
  if (poll_thread_.joinable()) {
    poll_thread_.join();
  }

  if (nui_initialized_) {
    if (ready_ && fn_nui_skeleton_tracking_disable_) {
      fn_nui_skeleton_tracking_disable_();
    }
    if (fn_nui_shutdown_) {
      fn_nui_shutdown_();
    }
    nui_initialized_ = false;
  }

  if (skeleton_event_) {
    CloseHandle(skeleton_event_);
    skeleton_event_ = nullptr;
  }

  ready_ = false;
  connected_ = false;

  if (kinect_dll_) {
    FreeLibrary(kinect_dll_);
    kinect_dll_ = nullptr;
    fn_nui_get_sensor_count_ = nullptr;
    fn_nui_initialize_ = nullptr;
    fn_nui_shutdown_ = nullptr;
    fn_nui_skeleton_tracking_enable_ = nullptr;
    fn_nui_skeleton_tracking_disable_ = nullptr;
    fn_nui_skeleton_get_next_frame_ = nullptr;
    fn_nui_camera_elevation_get_angle_ = nullptr;
    fn_nui_camera_elevation_set_angle_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Background polling thread
// ---------------------------------------------------------------------------

void KinectDevice::PollThread() {
  uint64_t poll_count = 0;
  uint64_t frame_count = 0;
  HRESULT last_hr = S_OK;

  while (running_) {
    // Wait for the SDK to signal that a new skeleton frame is ready.
    // This mirrors the official SkeletonBasics sample pattern:
    //   WaitForSingleObject(event, timeout) → NuiSkeletonGetNextFrame(0) →
    //   ResetEvent(event).
    // If no event was created (CreateEvent failed), fall back to a blocking
    // NuiSkeletonGetNextFrame with a 100 ms timeout.
    if (skeleton_event_) {
      DWORD wait_result = WaitForSingleObject(skeleton_event_, 100);
      if (wait_result != WAIT_OBJECT_0) {
        // Timeout or error; keep waiting.
        continue;
      }
    }

    NuiSkeletonFrame native_frame{};
    // Use 0 ms timeout when an event handle is available (frame is already
    // ready); otherwise block for up to 100 ms.
    DWORD timeout_ms = skeleton_event_ ? 0 : 100;
    HRESULT hr = fn_nui_skeleton_get_next_frame_(timeout_ms, &native_frame);
    ++poll_count;
    if (SUCCEEDED(hr)) {
      ++frame_count;
    }

    // Reset the event so we wait for the next frame notification.
    if (skeleton_event_) {
      ResetEvent(skeleton_event_);
    }

    if (!running_) {
      break;
    }

    // Log first call result, any HRESULT change, and every 500 polls (~50 s).
    const bool hr_changed = (hr != last_hr);
    const bool periodic = (poll_count % 500 == 0);
    if (poll_count == 1 || hr_changed || periodic) {
      if (SUCCEEDED(hr)) {
        XELOGI(
            "Kinect: NuiSkeletonGetNextFrame OK (hr=0x{:08X}) "
            "poll#{} frame#{}",
            static_cast<uint32_t>(hr), poll_count, frame_count);
      } else {
        XELOGW(
            "Kinect: NuiSkeletonGetNextFrame failed (hr=0x{:08X}) "
            "poll#{} frame#{}",
            static_cast<uint32_t>(hr), poll_count, frame_count);
      }
      last_hr = hr;
    }

    if (SUCCEEDED(hr)) {
      X_NUI_SKELETON_FRAME guest_frame{};
      ConvertFrame(native_frame, &guest_frame);

      {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        const bool first_frame = !has_frame_;
        latest_frame_ = guest_frame;
        has_frame_ = true;
        if (first_frame) {
          XELOGI("Kinect: First skeleton frame received (frame #{}).",
                 native_frame.dwFrameNumber);
          // Dump all 6 skeleton slots so we can see what the sensor sees.
          for (uint32_t i = 0; i < kNuiSkeletonCount; ++i) {
            XELOGI("Kinect:   slot[{}] tracking_state={} tracking_id={}",
                   i, native_frame.SkeletonData[i].eTrackingState,
                   native_frame.SkeletonData[i].dwTrackingID);
          }
        }
      }  // frame_mutex_ released before ProcessHudFrame

      // Update HUD engagement outside the lock (ProcessHudFrame also locks).
      ProcessHudFrame(guest_frame);
    }
  }

  XELOGI("Kinect: PollThread exiting (polls={} frames={}).", poll_count,
         frame_count);
}

// ---------------------------------------------------------------------------
// Public accessors
// ---------------------------------------------------------------------------

bool KinectDevice::IsConnected() const { return connected_; }
bool KinectDevice::IsReady() const { return ready_; }

bool KinectDevice::GetSkeletonFrame(X_NUI_SKELETON_FRAME* out_frame) {
  std::lock_guard<std::mutex> lock(frame_mutex_);
  if (!has_frame_) {
    return false;
  }
  *out_frame = latest_frame_;
  return true;
}

uint32_t KinectDevice::GetLastFrameNumber() const {
  std::lock_guard<std::mutex> lock(frame_mutex_);
  if (!has_frame_) {
    return 0;
  }
  return static_cast<uint32_t>(latest_frame_.frame_number);
}

void KinectDevice::ProcessHudFrame(const X_NUI_SKELETON_FRAME& frame) {
  // Pick the first fully-tracked skeleton as the engaged player.
  // If no fully-tracked skeleton is found, accept a position-only skeleton.
  uint32_t best_tracking_id = 0;
  uint32_t best_enrollment_index = kNoEnrolledPlayer;

  // First pass: prefer fully tracked skeletons.
  for (uint32_t i = 0; i < kNuiSkeletonCount; ++i) {
    uint32_t state =
        static_cast<uint32_t>(frame.skeleton_data[i].tracking_state);
    if (state == X_NUI_SKELETON_TRACKED) {
      uint32_t tid =
          static_cast<uint32_t>(frame.skeleton_data[i].tracking_id);
      if (tid != 0) {
        best_tracking_id = tid;
        // Use the enrollment_index that ConvertFrame assigned (0 for first
        // tracked player, 1 for second) — not the raw slot index.
        best_enrollment_index =
            static_cast<uint32_t>(frame.skeleton_data[i].enrollment_index);
        break;
      }
    }
  }

  // Second pass: fall back to position-only if no fully tracked skeleton.
  if (best_tracking_id == 0) {
    for (uint32_t i = 0; i < kNuiSkeletonCount; ++i) {
      uint32_t state =
          static_cast<uint32_t>(frame.skeleton_data[i].tracking_state);
      if (state == X_NUI_SKELETON_POSITION_ONLY) {
        uint32_t tid =
            static_cast<uint32_t>(frame.skeleton_data[i].tracking_id);
        if (tid != 0) {
          best_tracking_id = tid;
          best_enrollment_index =
              static_cast<uint32_t>(frame.skeleton_data[i].enrollment_index);
          break;
        }
      }
    }
  }

  bool engagement_changed = false;
  uint32_t new_enrollment = kNoEnrolledPlayer;
  EngagementChangedCallback cb;
  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (best_tracking_id != engaged_tracking_id_) {
      if (best_tracking_id != 0) {
        XELOGI("Kinect: Person detected — engaged tracking_id={} enrollment={}",
               best_tracking_id, best_enrollment_index);
      } else {
        XELOGI("Kinect: Person left — engagement cleared.");
      }
      engaged_tracking_id_ = best_tracking_id;
      engaged_enrollment_index_ =
          (best_tracking_id != 0) ? best_enrollment_index : kNoEnrolledPlayer;
      new_enrollment = engaged_enrollment_index_;
      engagement_changed = true;
      cb = engagement_changed_callback_;
    }
  }
  // Fire callback outside the lock to avoid potential deadlocks.
  if (engagement_changed && cb) {
    cb(best_tracking_id, new_enrollment);
  }
}

uint32_t KinectDevice::GetEngagedTrackingId() const {
  std::lock_guard<std::mutex> lock(frame_mutex_);
  return engaged_tracking_id_;
}

void KinectDevice::SetEngagedTrackingId(uint32_t tracking_id) {
  std::lock_guard<std::mutex> lock(frame_mutex_);
  engaged_tracking_id_ = tracking_id;
  if (tracking_id == 0) {
    engaged_enrollment_index_ = kNoEnrolledPlayer;
  } else {
    // Try to find the matching skeleton in the latest frame so the enrollment
    // index stays consistent with the tracking ID.  Default to 0 (first player)
    // if the skeleton is temporarily absent from the current frame.
    engaged_enrollment_index_ = 0;
    if (has_frame_) {
      for (uint32_t i = 0; i < kNuiSkeletonCount; ++i) {
        if (static_cast<uint32_t>(latest_frame_.skeleton_data[i].tracking_id) ==
            tracking_id) {
          engaged_enrollment_index_ =
              static_cast<uint32_t>(latest_frame_.skeleton_data[i].enrollment_index);
          break;
        }
      }
    }
  }
}

uint32_t KinectDevice::GetEngagedEnrollmentIndex() const {
  std::lock_guard<std::mutex> lock(frame_mutex_);
  return engaged_enrollment_index_;
}

void KinectDevice::SetEngagementChangedCallback(EngagementChangedCallback cb) {
  std::lock_guard<std::mutex> lock(frame_mutex_);
  engagement_changed_callback_ = std::move(cb);
}

long KinectDevice::GetCameraElevationAngle() const {
  if (!ready_ || !fn_nui_camera_elevation_get_angle_) {
    return 0;
  }
  LONG angle = 0;
  fn_nui_camera_elevation_get_angle_(&angle);
  return static_cast<long>(angle);
}

bool KinectDevice::SetCameraElevationAngle(long degrees) {
  if (!ready_ || !fn_nui_camera_elevation_set_angle_) {
    return false;
  }
  HRESULT hr = fn_nui_camera_elevation_set_angle_(static_cast<LONG>(degrees));
  return SUCCEEDED(hr);
}

std::wstring KinectDevice::GetDeviceConnectionId() const {
  // Device connection ID is only available through the COM interface, which
  // we no longer hold (free-function API is used instead).  Return empty.
  return {};
}

// ---------------------------------------------------------------------------
// Format conversion helpers
// ---------------------------------------------------------------------------

void KinectDevice::ConvertVector4(const NuiVector4& src, X_NUI_VECTOR4* dst) {
  dst->x = src.x;
  dst->y = src.y;
  dst->z = src.z;
  dst->w = src.w;
}

void KinectDevice::ConvertFrame(const NuiSkeletonFrame& src,
                                X_NUI_SKELETON_FRAME* dst) {
  dst->timestamp = src.liTimeStamp.QuadPart;
  dst->frame_number = src.dwFrameNumber;
  dst->flags = src.dwFlags;
  ConvertVector4(src.vFloorClipPlane, &dst->floor_clip_plane);
  ConvertVector4(src.vNormalToGravity, &dst->normal_to_gravity);

  // Assign enrollment indices sequentially: 0 for the first tracked skeleton,
  // 1 for the second, 0xFFFFFFFF for untracked slots.  The Xbox 360 SDK always
  // uses this scheme regardless of which array slot a skeleton occupies.  The
  // Windows SDK's dwEnrollmentIndex is only meaningful after a
  // NuiSkeletonSetTrackedSkeletons call (which we never make), so we ignore it
  // and compute our own sequential indices instead.  Games scan skeleton_data[]
  // looking for enrollment_index==0 to find the first player; writing the slot
  // index here means the game would find an untracked entry at index 0 and
  // see nobody.
  uint32_t next_enrollment = 0;
  for (uint32_t i = 0; i < kNuiSkeletonCount; ++i) {
    const NuiSkeletonData& ssrc = src.SkeletonData[i];
    X_NUI_SKELETON_DATA& sdst = dst->skeleton_data[i];

    sdst.tracking_state = ssrc.eTrackingState;
    sdst.tracking_id = ssrc.dwTrackingID;
    const bool is_tracked = (ssrc.eTrackingState != kNuiSkeletonNotTracked) &&
                            (ssrc.dwTrackingID != 0);
    sdst.enrollment_index = is_tracked ? next_enrollment++ : 0xFFFFFFFF;
    sdst.user_index = ssrc.dwUserIndex;
    ConvertVector4(ssrc.Position, &sdst.position);

    for (uint32_t j = 0; j < kNuiSkeletonPositionCount; ++j) {
      ConvertVector4(ssrc.SkeletonPositions[j], &sdst.skeleton_positions[j]);
      sdst.position_tracking_state[j] = ssrc.eSkeletonPositionTrackingState[j];
    }

    sdst.quality_flags = ssrc.dwQualityFlags;
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XE_PLATFORM_WIN32
