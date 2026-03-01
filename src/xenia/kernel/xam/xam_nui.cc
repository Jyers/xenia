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

// Returns whether the NUI HUD system is enabled (1 = enabled, 0 = disabled).
dword_result_t XamNuiHudIsEnabled_entry() {
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  if (kinect->IsConnected()) {
    return 1;
  }
#endif  // XE_PLATFORM_WIN32
  return 0;
}
DECLARE_XAM_EXPORT1(XamNuiHudIsEnabled, kNone, kImplemented);

// Processes a skeleton frame through the NUI HUD engagement system.
// The game provides the skeleton frame (input).  The HUD uses it to
// determine which player is "engaged" (i.e., interacting with the device).
// NOTE: We intentionally do not update engagement state here.  The background
// KinectDevice polling thread is the authoritative source: it receives frames
// directly from the Kinect SDK and calls ProcessHudFrame on every new frame.
// Forwarding the game-provided frame here caused the engagement state to be
// reset to 0 whenever the game passed an empty/uninitialized frame buffer
// (which happens when XamNuiNatalCameraUpdateStarting has no data yet).
dword_result_t XamNuiHudInterpretFrame_entry(
    pointer_t<X_NUI_SKELETON_FRAME> frame_ptr) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiHudInterpretFrame, kNone, kImplemented);

// Returns the tracking ID of the currently engaged player, or 0 if no one is
// engaged.
dword_result_t XamNuiHudGetEngagedTrackingID_entry() {
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  if (kinect->IsConnected()) {
    return kinect->GetEngagedTrackingId();
  }
#endif  // XE_PLATFORM_WIN32
  return 0;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetEngagedTrackingID, kNone, kImplemented);

// Allows the game to override the engaged tracking ID.
void XamNuiHudSetEngagedTrackingID_entry(dword_t tracking_id) {
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  if (kinect->IsConnected()) {
    kinect->SetEngagedTrackingId(tracking_id);
  }
#endif  // XE_PLATFORM_WIN32
}
DECLARE_XAM_EXPORT1(XamNuiHudSetEngagedTrackingID, kNone, kImplemented);

// Returns the enrollment (player slot) index for the engaged player, or 0xFF
// if no one is engaged.
dword_result_t XamNuiHudGetEngagedEnrollmentIndex_entry() {
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  if (kinect->IsConnected()) {
    return kinect->GetEngagedEnrollmentIndex();
  }
#endif  // XE_PLATFORM_WIN32
  return KinectDevice::kNoEnrolledPlayer;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetEngagedEnrollmentIndex, kNone, kImplemented);

// Called by the game at the start of a camera processing cycle.
// Fills the provided skeleton frame buffer with the latest sensor data so the
// game can process it directly.  Returns X_ERROR_SUCCESS on success or
// X_ERROR_DEVICE_NOT_CONNECTED when no data is available.
dword_result_t XamNuiNatalCameraUpdateStarting_entry(
    pointer_t<X_NUI_SKELETON_FRAME> frame_ptr) {
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  if (kinect->IsReady()) {
    if (frame_ptr) {
      X_NUI_SKELETON_FRAME frame{};
      if (kinect->GetSkeletonFrame(&frame)) {
        *frame_ptr = frame;
        return X_ERROR_SUCCESS;
      }
      // No frame yet — zero the output buffer and still return success so the
      // game doesn't bail out on startup.
      frame_ptr.Zero();
      return X_ERROR_SUCCESS;
    }
    return X_ERROR_SUCCESS;
  }
#endif  // XE_PLATFORM_WIN32
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiNatalCameraUpdateStarting, kNone, kImplemented);

// Called by the game at the end of a camera processing cycle.
void XamNuiNatalCameraUpdateComplete_entry() {
  // Nothing to do on the host side.
}
DECLARE_XAM_EXPORT1(XamNuiNatalCameraUpdateComplete, kNone, kImplemented);

// Allows the game to force the Kinect device off.  We intentionally keep the
// sensor active so it can be re-detected without re-initialisation overhead,
// and return success so the game does not interpret the call as a failure.
// The sensor will still appear connected on the next XamNuiGetDeviceStatus call.
dword_result_t XamNuiSetForceDeviceOff_entry(dword_t force_off) {
  if (force_off) {
    XELOGD("Kinect: game requested XamNuiSetForceDeviceOff (sensor kept active).");
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiSetForceDeviceOff, kNone, kImplemented);

// Notifies the system of a player engagement state change.  Used by the game
// to signal that a specific player index has engaged/disengaged.
void XamNuiPlayerEngagementUpdate_entry(dword_t user_index, dword_t engaged) {
  // No state we need to maintain on the host side.
}
DECLARE_XAM_EXPORT1(XamNuiPlayerEngagementUpdate, kNone, kImplemented);

// Returns version information for the NUI HUD system.
// The output format is two 32-bit version numbers (major, minor).
dword_result_t XamNuiHudGetVersions_entry(lpdword_t major_out,
                                           lpdword_t minor_out) {
  // Report a version that games will accept (version 2.0 is Kinect SDK v1.x).
  if (major_out) {
    *major_out = 2;
  }
  if (minor_out) {
    *minor_out = 0;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetVersions, kNone, kImplemented);

// Enables or disables an input filter in the NUI HUD.
dword_result_t XamNuiHudEnableInputFilter_entry(dword_t enable) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiHudEnableInputFilter, kNone, kImplemented);

// Returns the HUD initialisation flags.
dword_result_t XamNuiHudGetInitializeFlags_entry() {
  // Return 0 (default flags - no special modes).
  return 0;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetInitializeFlags, kNone, kImplemented);

// ---------------------------------------------------------------------------
// User-NUI binding: associates signed-in user slots with the Kinect sensor.
// These are required for the engagement pipeline to know which controller
// slot corresponds to the person standing in front of the sensor.
// We maintain a simple 1-to-1 mapping (user index == enrollment index) which
// is the most common case and sufficient for single-player Kinect games.
// ---------------------------------------------------------------------------

// Returns the controller slot (user index) that should be bound to the Kinect.
// The game calls this before XamUserNuiBind to find a free/suitable slot.
dword_result_t XamUserNuiGetUserIndexForBind_entry() {
  return 0;  // Default to user 0 (single-player Kinect assumption).
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndexForBind, kNone, kStub);

// Returns the controller slot that is recommended for sign-in via Kinect.
dword_result_t XamUserNuiGetUserIndexForSignin_entry() {
  return 0;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndexForSignin, kNone, kStub);

// Binds a signed-in user (controller slot) to the Kinect engagement system.
dword_result_t XamUserNuiBind_entry(dword_t user_index) {
  XELOGI("Kinect: XamUserNuiBind user_index={}", user_index.value());
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserNuiBind, kNone, kStub);

// Unbinds a user from the Kinect engagement system.
dword_result_t XamUserNuiUnbind_entry(dword_t user_index) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserNuiUnbind, kNone, kStub);

// Returns the controller slot (user index) for a given enrollment index.
// enrollment_index is the skeleton slot (0-5); out_user_index receives the
// controller slot (0-3) bound to that skeleton.
dword_result_t XamUserNuiGetUserIndex_entry(dword_t enrollment_index,
                                             lpdword_t out_user_index) {
  if (out_user_index) {
    // 1-to-1 mapping: enrollment index == user index.
    *out_user_index = enrollment_index.value();
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndex, kNone, kStub);

// Returns the enrollment index (skeleton slot) for a given controller slot.
dword_result_t XamUserNuiGetEnrollmentIndex_entry(dword_t user_index,
                                                   lpdword_t out_enrollment_index) {
  if (out_enrollment_index) {
    *out_enrollment_index = user_index.value();
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetEnrollmentIndex, kNone, kStub);

// Updates the engagement scoring for a skeleton slot.  The game provides
// per-skeleton scores that influence which person is selected as "engaged".
// We don't maintain a score table — engagement is determined by the first
// tracked skeleton found by the background polling thread.
dword_result_t XamNuiSkeletonScoreUpdate_entry(unknown_t unk1, unknown_t unk2) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiSkeletonScoreUpdate, kNone, kStub);

// Sets camera flags (e.g. near mode, seated tracking).
dword_result_t XamNuiCameraSetFlags_entry(dword_t flags) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraSetFlags, kNone, kStub);

// Stores the current floor plane for use in subsequent tracking.
dword_result_t XamNuiCameraRememberFloor_entry() {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraRememberFloor, kNone, kStub);

// Enables or disables NUI automation (testing/scripted input).
dword_result_t XamEnableNuiAutomation_entry(dword_t enable) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamEnableNuiAutomation, kNone, kStub);

// Enables or disables Natal playback mode.
dword_result_t XamEnableNatalPlayback_entry(dword_t enable) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamEnableNatalPlayback, kNone, kStub);

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
