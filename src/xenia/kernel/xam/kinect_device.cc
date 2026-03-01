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

#include <oleauto.h>  // SysFreeString

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

  fn_nui_get_sensor_count_ = reinterpret_cast<PFN_NuiGetSensorCount>(
      GetProcAddress(kinect_dll_, "NuiGetSensorCount"));
  fn_nui_create_sensor_by_index_ =
      reinterpret_cast<PFN_NuiCreateSensorByIndex>(
          GetProcAddress(kinect_dll_, "NuiCreateSensorByIndex"));

  if (!fn_nui_get_sensor_count_ || !fn_nui_create_sensor_by_index_) {
    FreeLibrary(kinect_dll_);
    kinect_dll_ = nullptr;
    fn_nui_get_sensor_count_ = nullptr;
    fn_nui_create_sensor_by_index_ = nullptr;
    return false;
  }

  char dll_path[MAX_PATH] = {};
  if (GetModuleFileNameA(kinect_dll_, dll_path, MAX_PATH)) {
    XELOGI("Kinect: Loaded Kinect10.dll from: {}", dll_path);
  }

  return true;
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

  // Open the first sensor.
  hr = fn_nui_create_sensor_by_index_(0, &nui_sensor_);
  if (FAILED(hr) || !nui_sensor_) {
    XELOGE("Kinect: NuiCreateSensorByIndex failed (hr=0x{:08X}).",
           static_cast<uint32_t>(hr));
    return false;
  }
  connected_ = true;

  // Initialise with depth+player-index AND skeleton tracking.
  // The depth flag is required even if we never read depth frames: skeleton
  // tracking internally uses the depth pipeline, and without it
  // NuiSkeletonTrackingEnable returns E_INVALIDARG (0x80070057).
  XELOGI("Kinect: Calling NuiInitialize with flags=0x{:08X}.",
         static_cast<uint32_t>(kNuiInitFlagUseSkeleton));
  hr = Vtbl()->NuiInitialize(nui_sensor_, kNuiInitFlagUseSkeleton);
  if (FAILED(hr)) {
    XELOGE("Kinect: NuiInitialize failed (hr=0x{:08X}).",
           static_cast<uint32_t>(hr));
    // Still considered connected but not ready.
    return false;
  }
  XELOGI("Kinect: NuiInitialize succeeded (hr=0x{:08X}).",
         static_cast<uint32_t>(hr));

  // Create the skeleton-ready event required by NuiSkeletonTrackingEnable.
  // The official Kinect SDK samples always pass a real event handle; some
  // SDK/driver versions reject NULL (0) as E_INVALIDARG because they
  // distinguish it from INVALID_HANDLE_VALUE as the "no event" sentinel.
  // Manual-reset (TRUE) so the signal is not auto-cleared between frames;
  // initially non-signaled (FALSE) since no frame is ready yet.
  skeleton_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!skeleton_event_) {
    XELOGW("Kinect: CreateEventW failed (GLE={}); falling back to "
           "INVALID_HANDLE_VALUE.",
           GetLastError());
    skeleton_event_ = INVALID_HANDLE_VALUE;
  }

  hr = Vtbl()->NuiSkeletonTrackingEnable(nui_sensor_, skeleton_event_,
                                         kNuiSkeletonTrackingFlagDefault);
  if (FAILED(hr)) {
    XELOGE("Kinect: NuiSkeletonTrackingEnable failed (hr=0x{:08X}).",
           static_cast<uint32_t>(hr));
    if (skeleton_event_ != INVALID_HANDLE_VALUE) {
      CloseHandle(skeleton_event_);
      skeleton_event_ = INVALID_HANDLE_VALUE;
    }
    Vtbl()->NuiShutdown(nui_sensor_);
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
      fn_nui_create_sensor_by_index_ = nullptr;
    }
    return;
  }

  // Stop the polling thread first.
  running_ = false;
  if (poll_thread_.joinable()) {
    poll_thread_.join();
  }

  // Close the skeleton frame event.
  if (skeleton_event_ != INVALID_HANDLE_VALUE) {
    CloseHandle(skeleton_event_);
    skeleton_event_ = INVALID_HANDLE_VALUE;
  }

  if (nui_sensor_) {
    if (ready_) {
      Vtbl()->NuiSkeletonTrackingDisable(nui_sensor_);
      Vtbl()->NuiShutdown(nui_sensor_);
    }
    // Release the COM reference.
    reinterpret_cast<IUnknown*>(nui_sensor_)->Release();
    nui_sensor_ = nullptr;
  }

  ready_ = false;
  connected_ = false;

  if (kinect_dll_) {
    FreeLibrary(kinect_dll_);
    kinect_dll_ = nullptr;
    fn_nui_get_sensor_count_ = nullptr;
    fn_nui_create_sensor_by_index_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Background polling thread
// ---------------------------------------------------------------------------

void KinectDevice::PollThread() {
  while (running_) {
    // Poll for the next skeleton frame, blocking for up to 100 ms.
    // The skeleton_event_ is signaled by the SDK when a new frame is ready,
    // but we use NuiSkeletonGetNextFrame's built-in timeout instead so the
    // thread wakes promptly when running_ is cleared.
    NuiSkeletonFrame native_frame{};
    HRESULT hr = Vtbl()->NuiSkeletonGetNextFrame(
        nui_sensor_, 100 /*ms timeout*/, &native_frame);

    if (!running_) {
      break;
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
        }
      }  // frame_mutex_ released before ProcessHudFrame

      // Update HUD engagement outside the lock (ProcessHudFrame also locks).
      ProcessHudFrame(guest_frame);
    }
  }
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
        best_enrollment_index = i;
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
          best_enrollment_index = i;
          break;
        }
      }
    }
  }

  std::lock_guard<std::mutex> lock(frame_mutex_);
  if (best_tracking_id != engaged_tracking_id_) {
    if (best_tracking_id != 0) {
      XELOGI("Kinect: Person detected — engaged tracking_id={} slot={}",
             best_tracking_id, best_enrollment_index);
    } else {
      XELOGI("Kinect: Person left — engagement cleared.");
    }
  }
  engaged_tracking_id_ = best_tracking_id;
  engaged_enrollment_index_ =
      (best_tracking_id != 0) ? best_enrollment_index : kNoEnrolledPlayer;
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
    // index stays consistent with the tracking ID.
    engaged_enrollment_index_ = 0;  // default to slot 0 if not found
    if (has_frame_) {
      for (uint32_t i = 0; i < kNuiSkeletonCount; ++i) {
        if (static_cast<uint32_t>(latest_frame_.skeleton_data[i].tracking_id) ==
            tracking_id) {
          engaged_enrollment_index_ = i;
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

long KinectDevice::GetCameraElevationAngle() const {
  if (!ready_) {
    return 0;
  }
  LONG angle = 0;
  Vtbl()->NuiCameraElevationGetAngle(nui_sensor_, &angle);
  return static_cast<long>(angle);
}

bool KinectDevice::SetCameraElevationAngle(long degrees) {
  if (!ready_) {
    return false;
  }
  HRESULT hr = Vtbl()->NuiCameraElevationSetAngle(
      nui_sensor_, static_cast<LONG>(degrees));
  return SUCCEEDED(hr);
}

std::wstring KinectDevice::GetDeviceConnectionId() const {
  if (!connected_ || !nui_sensor_) {
    return {};
  }
  BSTR bstr = Vtbl()->NuiDeviceConnectionId(nui_sensor_);
  if (!bstr) {
    return {};
  }
  std::wstring result(bstr);
  SysFreeString(bstr);
  return result;
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

  for (uint32_t i = 0; i < kNuiSkeletonCount; ++i) {
    const NuiSkeletonData& ssrc = src.SkeletonData[i];
    X_NUI_SKELETON_DATA& sdst = dst->skeleton_data[i];

    sdst.tracking_state = ssrc.eTrackingState;
    sdst.tracking_id = ssrc.dwTrackingID;
    sdst.enrollment_index = ssrc.dwEnrollmentIndex;
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
