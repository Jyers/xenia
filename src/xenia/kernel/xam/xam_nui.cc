/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/kinect_device.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/xbox.h"

#include <algorithm>
#include <cstring>

namespace xe {
namespace kernel {
namespace xam {

extern std::atomic<int> xam_dialogs_shown_;

struct X_NUI_DEVICE_STATUS {
  xe::be<uint32_t> unk0;
  xe::be<uint32_t> unk1;
  xe::be<uint32_t> unk2;
  xe::be<uint32_t> status;
  xe::be<uint32_t> unk4;
  xe::be<uint32_t> unk5;
};
static_assert(sizeof(X_NUI_DEVICE_STATUS) == 24, "Size matters");

// status field values observed on real hardware.
constexpr uint32_t kNuiDeviceStatusNotConnected = 0;
constexpr uint32_t kNuiDeviceStatusConnected = 1;

void XamNuiGetDeviceStatus_entry(pointer_t<X_NUI_DEVICE_STATUS> status_ptr) {
  status_ptr.Zero();
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  // Attempt lazy initialisation on first call.
  if (!kinect->IsConnected()) {
    kinect->Initialize();
  }
  if (kinect->IsConnected()) {
    status_ptr->status = kNuiDeviceStatusConnected;
    return;
  }
#endif  // XE_PLATFORM_WIN32
  status_ptr->status = kNuiDeviceStatusNotConnected;
}
DECLARE_XAM_EXPORT1(XamNuiGetDeviceStatus, kNone, kImplemented);

dword_result_t XamNuiIsDeviceReady_entry(dword_t unk) {
  // unk appears to always be 0.
  // Returns X_ERROR_SUCCESS (0) when a Kinect is ready, non-zero otherwise.
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  if (!kinect->IsConnected()) {
    kinect->Initialize();
  }
  if (kinect->IsReady()) {
    return X_ERROR_SUCCESS;
  }
#endif  // XE_PLATFORM_WIN32
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiIsDeviceReady, kNone, kImplemented);

dword_result_t XamNuiCameraElevationSetAngle_entry(int_t angle) {
  // angle is in degrees, range [-27, 27].
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  if (kinect->IsReady()) {
    kinect->SetCameraElevationAngle(static_cast<long>(angle));
    return X_ERROR_SUCCESS;
  }
#endif  // XE_PLATFORM_WIN32
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationSetAngle, kNone, kImplemented);

dword_result_t XamNuiCameraElevationGetAngle_entry(
    pointer_t<xe::be<int32_t>> angle_ptr) {
  if (!angle_ptr) {
    return X_ERROR_BAD_ARGUMENTS;
  }
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  if (kinect->IsReady()) {
    *angle_ptr = static_cast<int32_t>(kinect->GetCameraElevationAngle());
    return X_ERROR_SUCCESS;
  }
#endif  // XE_PLATFORM_WIN32
  *angle_ptr = 0;
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationGetAngle, kNone, kImplemented);

dword_result_t XamNuiGetDeviceSerialNumber_entry(lpvoid_t buffer_ptr,
                                                  dword_t buffer_size) {
  // The serial-number buffer on the 360 is 40 bytes (20 wide chars).
  if (!buffer_ptr || !buffer_size) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  std::memset(buffer_ptr, 0, buffer_size);
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  if (kinect->IsConnected()) {
    std::wstring id = kinect->GetDeviceConnectionId();
    if (!id.empty()) {
      // Write as null-terminated big-endian wide string into guest memory.
      // Each wchar_t is 2 bytes; store in big-endian order.
      uint16_t* dst = reinterpret_cast<uint16_t*>(static_cast<void*>(buffer_ptr));
      size_t max_chars =
          (buffer_size / sizeof(uint16_t)) - 1;  // leave null terminator
      size_t len = std::min(id.size(), max_chars);
      for (size_t i = 0; i < len; ++i) {
        // Byte-swap each 16-bit character to big-endian.
        dst[i] = xe::byte_swap(static_cast<uint16_t>(id[i]));
      }
      dst[len] = 0;
      return X_ERROR_SUCCESS;
    }
  }
#endif  // XE_PLATFORM_WIN32
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiGetDeviceSerialNumber, kNone, kImplemented);

// Returns the index (0-5) of the "best" skeleton to use as the engaged player,
// or 0xFF if no valid skeleton is tracked.
dword_result_t XamNuiSkeletonGetBestSkeletonIndex_entry() {
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  if (kinect->IsReady()) {
    X_NUI_SKELETON_FRAME frame{};
    if (kinect->GetSkeletonFrame(&frame)) {
      for (uint32_t i = 0; i < kNuiSkeletonCount; ++i) {
        uint32_t state =
            static_cast<uint32_t>(frame.skeleton_data[i].tracking_state);
        if (state == X_NUI_SKELETON_TRACKED ||
            state == X_NUI_SKELETON_POSITION_ONLY) {
          return i;
        }
      }
    }
  }
#endif  // XE_PLATFORM_WIN32
  return 0xFF;  // No skeleton found.
}
DECLARE_XAM_EXPORT1(XamNuiSkeletonGetBestSkeletonIndex, kNone, kImplemented);

dword_result_t XamShowNuiTroubleshooterUI_entry(unknown_t unk1, unknown_t unk2,
                                                unknown_t unk3) {
  // unk1 is 0xFF - possibly user index?
  // unk2, unk3 appear to always be zero.

  if (cvars::headless) {
    return 0;
  }

  const Emulator* emulator = kernel_state()->emulator();
  ui::Window* display_window = emulator->display_window();
  ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  if (display_window && imgui_drawer) {
    xe::threading::Fence fence;
    if (display_window->app_context().CallInUIThreadSynchronous([&]() {
          xe::ui::ImGuiDialog::ShowMessageBox(
              imgui_drawer, "NUI Troubleshooter",
              "The game has indicated there is a problem with NUI (Kinect).")
              ->Then(&fence);
        })) {
      ++xam_dialogs_shown_;
      fence.Wait();
      --xam_dialogs_shown_;
    }
  }

  return 0;
}
DECLARE_XAM_EXPORT1(XamShowNuiTroubleshooterUI, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(NUI);
