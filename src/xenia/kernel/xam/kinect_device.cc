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

  return true;
}

bool KinectDevice::Initialize() {
  if (ready_) {
    return true;
  }

  if (!LoadKinectDll()) {
    XELOGD("Kinect: Kinect10.dll not found — Kinect support unavailable.");
    return false;
  }

  // Check how many sensors are attached.
  int sensor_count = 0;
  HRESULT hr = fn_nui_get_sensor_count_(&sensor_count);
  if (FAILED(hr) || sensor_count == 0) {
    XELOGD("Kinect: No Kinect sensors detected.");
    return false;
  }

  // Open the first sensor.
  hr = fn_nui_create_sensor_by_index_(0, &nui_sensor_);
  if (FAILED(hr) || !nui_sensor_) {
    XELOGD("Kinect: NuiCreateSensorByIndex failed (hr=0x{:08X}).", hr);
    return false;
  }
  connected_ = true;

  // Initialise with skeleton tracking only (colour and depth add overhead).
  hr = Vtbl()->NuiInitialize(nui_sensor_, kNuiInitFlagUseSkeleton);
  if (FAILED(hr)) {
    XELOGD("Kinect: NuiInitialize failed (hr=0x{:08X}).", hr);
    // Still considered connected but not ready.
    return false;
  }

  // Create an event that is signalled when a new skeleton frame is available.
  skeleton_event_ =
      CreateEventW(nullptr, TRUE /*manual-reset*/, FALSE, nullptr);
  if (!skeleton_event_) {
    Vtbl()->NuiShutdown(nui_sensor_);
    return false;
  }

  hr = Vtbl()->NuiSkeletonTrackingEnable(nui_sensor_, skeleton_event_,
                                          kNuiSkeletonTrackingFlagDefault);
  if (FAILED(hr)) {
    XELOGD("Kinect: NuiSkeletonTrackingEnable failed (hr=0x{:08X}).", hr);
    CloseHandle(skeleton_event_);
    skeleton_event_ = nullptr;
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
  if (skeleton_event_) {
    SetEvent(skeleton_event_);  // unblock WaitForSingleObject
  }
  if (poll_thread_.joinable()) {
    poll_thread_.join();
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
    fn_nui_create_sensor_by_index_ = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Background polling thread
// ---------------------------------------------------------------------------

void KinectDevice::PollThread() {
  while (running_) {
    DWORD wait_result =
        WaitForSingleObject(skeleton_event_, 100 /*ms timeout*/);

    if (!running_) {
      break;
    }

    if (wait_result == WAIT_OBJECT_0) {
      ResetEvent(skeleton_event_);

      NuiSkeletonFrame native_frame{};
      HRESULT hr = Vtbl()->NuiSkeletonGetNextFrame(
          nui_sensor_, 0 /*do not wait*/, &native_frame);

      if (SUCCEEDED(hr)) {
        X_NUI_SKELETON_FRAME guest_frame{};
        ConvertFrame(native_frame, &guest_frame);

        std::lock_guard<std::mutex> lock(frame_mutex_);
        latest_frame_ = guest_frame;
        has_frame_ = true;
      }
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
