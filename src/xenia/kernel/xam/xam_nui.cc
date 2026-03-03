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
#include <atomic>
#include <cstring>

namespace xe {
namespace kernel {
namespace xam {

extern std::atomic<int> xam_dialogs_shown_;

struct X_NUI_DEVICE_STATUS {
  // The connection status is written to both field 0 and field 3 because the
  // exact layout varies by SDK version.  Games that check either offset will
  // see the correct connected/not-connected value.
  xe::be<uint32_t> status0;  // field 0 — written for safety
  xe::be<uint32_t> unk1;
  xe::be<uint32_t> unk2;
  xe::be<uint32_t> status3;  // field 3 — original observed-on-hardware field
  xe::be<uint32_t> unk4;
  xe::be<uint32_t> unk5;
};
static_assert(sizeof(X_NUI_DEVICE_STATUS) == 24, "Size matters");

// Status values used in both field 0 and field 3.
constexpr uint32_t kNuiDeviceStatusNotConnected = 0;
constexpr uint32_t kNuiDeviceStatusConnected = 1;

// Enrollment index used when no player is enrolled (matches
// KinectDevice::kNoEnrolledPlayer on Win32).
constexpr uint32_t kNoEnrolledPlayer = 0xFF;

// NUI notifications are broadcast directly by KinectDevice::ProcessHudFrame
// when the engagement state changes.  The notification IDs are defined in
// kinect_device.cc.  Games that want NUI events create listeners with bit 5
// set in their mask (0x20).

#if XE_PLATFORM_WIN32
// Enables NUI notification broadcasting on the KinectDevice singleton.
// Safe to call multiple times; the second and subsequent calls are no-ops.
static void EnsureNuiNotificationsEnabled() {
  static std::atomic<bool> enabled{false};
  if (enabled.exchange(true)) {
    return;
  }
  KinectDevice::Get()->EnableNotificationBroadcast();
}
#endif  // XE_PLATFORM_WIN32

void XamNuiGetDeviceStatus_entry(pointer_t<X_NUI_DEVICE_STATUS> status_ptr) {
  status_ptr.Zero();
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  // Attempt lazy initialisation on first call.
  if (!kinect->IsConnected()) {
    kinect->Initialize();
  }
  // Ensure the engagement-changed callback is registered so player-detection
  // events are forwarded to the game's notification listeners.
  EnsureNuiNotificationsEnabled();
  if (kinect->IsConnected()) {
    // Write connected status to both field 0 and field 3 to handle games that
    // check either offset.
    status_ptr->status0 = kNuiDeviceStatusConnected;
    status_ptr->status3 = kNuiDeviceStatusConnected;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      XELOGI("Kinect: XamNuiGetDeviceStatus → Connected.");
    }
    return;
  }
  {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      XELOGI("Kinect: XamNuiGetDeviceStatus → Not connected.");
    }
  }
#endif  // XE_PLATFORM_WIN32
  status_ptr->status0 = kNuiDeviceStatusNotConnected;
  status_ptr->status3 = kNuiDeviceStatusNotConnected;
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
  // Register the engagement callback here too, because the game may call
  // XamNuiIsDeviceReady without ever calling XamNuiGetDeviceStatus.  If the
  // callback is not registered, engagement notifications are never broadcast.
  EnsureNuiNotificationsEnabled();
  if (kinect->IsReady()) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      XELOGI("Kinect: XamNuiIsDeviceReady → Ready (first call).");
    }
    return X_ERROR_SUCCESS;
  }
  {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      XELOGW("Kinect: XamNuiIsDeviceReady → Not ready (first call).");
    }
  }
#endif  // XE_PLATFORM_WIN32
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiIsDeviceReady, kNone, kImplemented);

dword_result_t XamNuiCameraElevationSetAngle_entry(int_t angle) {
  // angle is in degrees, range [-27, 27].
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiCameraElevationSetAngle angle={} (first call)",
           angle.value());
  }
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
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiCameraElevationGetAngle (first call)");
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
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiGetDeviceSerialNumber (first call)");
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
          static std::atomic<uint32_t> last_best{kNoEnrolledPlayer};
          const uint32_t prev = last_best.exchange(i);
          if (prev != i) {
            XELOGI("Kinect: XamNuiSkeletonGetBestSkeletonIndex → {} "
                   "(state={})",
                   i, state);
          }
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
  uint32_t result = 0;
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  if (kinect->IsConnected()) {
    result = 1;
  }
#endif  // XE_PLATFORM_WIN32
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiHudIsEnabled → {} (first call)", result);
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamNuiHudIsEnabled, kNone, kImplemented);

// Processes a skeleton frame through the NUI HUD engagement system.
// Fills frame_ptr with the latest sensor data so games that call only this
// function (not NatalCameraUpdateStarting) still receive valid joint data.
dword_result_t XamNuiHudInterpretFrame_entry(
    pointer_t<X_NUI_SKELETON_FRAME> frame_ptr) {
#if XE_PLATFORM_WIN32
  if (frame_ptr) {
    KinectDevice* kinect = KinectDevice::Get();
    if (kinect->IsReady()) {
      X_NUI_SKELETON_FRAME frame{};
      if (kinect->GetSkeletonFrame(&frame)) {
        *frame_ptr = frame;
      }
    }
  }
#endif  // XE_PLATFORM_WIN32
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiHudInterpretFrame, kNone, kImplemented);

// Returns the tracking ID of the currently engaged player, or 0 if no one is
// engaged.
dword_result_t XamNuiHudGetEngagedTrackingID_entry() {
  uint32_t result = 0;
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  if (kinect->IsConnected()) {
    result = kinect->GetEngagedTrackingId();
  }
#endif  // XE_PLATFORM_WIN32
  static std::atomic<uint32_t> last_result{0};
  const uint32_t prev = last_result.exchange(result);
  if (prev != result) {
    XELOGI("Kinect: XamNuiHudGetEngagedTrackingID → {}", result);
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetEngagedTrackingID, kNone, kImplemented);

// Allows the game to override the engaged tracking ID.
void XamNuiHudSetEngagedTrackingID_entry(dword_t tracking_id) {
  XELOGI("Kinect: XamNuiHudSetEngagedTrackingID tracking_id={}",
         tracking_id.value());
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
  uint32_t result = kNoEnrolledPlayer;
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  if (kinect->IsConnected()) {
    result = kinect->GetEngagedEnrollmentIndex();
  }
#endif  // XE_PLATFORM_WIN32
  static std::atomic<uint32_t> last_result{kNoEnrolledPlayer};
  const uint32_t prev = last_result.exchange(result);
  if (prev != result) {
    XELOGI("Kinect: XamNuiHudGetEngagedEnrollmentIndex → {}", result);
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetEngagedEnrollmentIndex, kNone, kImplemented);

// Called by the game at the start of a camera processing cycle.
// Fills the provided skeleton frame buffer with the latest sensor data so the
// game can process it directly.
dword_result_t XamNuiNatalCameraUpdateStarting_entry(
    pointer_t<X_NUI_SKELETON_FRAME> frame_ptr) {
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  // Trigger lazy initialisation here so games that call this function before
  // XamNuiGetDeviceStatus / XamNuiIsDeviceReady still get skeleton data.
  if (!kinect->IsConnected()) {
    kinect->Initialize();
    EnsureNuiNotificationsEnabled();
  }
  if (kinect->IsReady()) {
    if (frame_ptr) {
      X_NUI_SKELETON_FRAME frame{};
      if (kinect->GetSkeletonFrame(&frame)) {
        *frame_ptr = frame;
        return X_ERROR_SUCCESS;
      }
      // No frame yet — zero the output buffer.
      frame_ptr.Zero();
    }
    return X_ERROR_SUCCESS;
  }
#endif  // XE_PLATFORM_WIN32
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XAM_EXPORT1(XamNuiNatalCameraUpdateStarting, kNone, kImplemented);

// Called by the game at the end of a camera processing cycle.
void XamNuiNatalCameraUpdateComplete_entry() {}
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
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiPlayerEngagementUpdate user_index={} engaged={} (first call)",
           user_index.value(), engaged.value());
  }
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
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiHudGetVersions → major=2 minor=0 (first call)");
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetVersions, kNone, kImplemented);

// Enables or disables an input filter in the NUI HUD.
dword_result_t XamNuiHudEnableInputFilter_entry(dword_t enable) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiHudEnableInputFilter enable={} (first call)",
           enable.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiHudEnableInputFilter, kNone, kImplemented);

// Returns the HUD initialisation flags.
dword_result_t XamNuiHudGetInitializeFlags_entry() {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiHudGetInitializeFlags → 0 (first call)");
  }
  // Return 0 (default flags - no special modes).
  return 0;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetInitializeFlags, kNone, kImplemented);

// ---------------------------------------------------------------------------
// User-NUI binding: associates signed-in user slots with the Kinect sensor.
// These are required for the engagement pipeline to know which controller
// slot corresponds to the person standing in front of the sensor.
// ConvertFrame assigns sequential enrollment indices (0, 1, ...) to tracked
// skeletons, so user 0 maps to enrollment 0 (first tracked player) regardless
// of which array slot the skeleton occupies.
// ---------------------------------------------------------------------------

// Returns the controller slot (user index) that should be bound to the Kinect.
// The game calls this before XamUserNuiBind to find a free/suitable slot.
dword_result_t XamUserNuiGetUserIndexForBind_entry() {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamUserNuiGetUserIndexForBind → 0 (first call)");
  }
  return 0;  // Default to user 0 (single-player Kinect assumption).
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndexForBind, kNone, kStub);

// Returns the controller slot that is recommended for sign-in via Kinect.
dword_result_t XamUserNuiGetUserIndexForSignin_entry() {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamUserNuiGetUserIndexForSignin → 0 (first call)");
  }
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
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamUserNuiUnbind user_index={} (first call)",
           user_index.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserNuiUnbind, kNone, kStub);

// Returns the controller slot (user index) for a given enrollment index.
// enrollment_index is the sequential enrollment index (0 for first tracked
// player, 1 for second) assigned by ConvertFrame.  We map enrollment 0 to
// user 0 (single-player assumption).
dword_result_t XamUserNuiGetUserIndex_entry(dword_t enrollment_index,
                                             lpdword_t out_user_index) {
  if (out_user_index) {
    uint32_t result = kNoEnrolledPlayer;
#if XE_PLATFORM_WIN32
    KinectDevice* kinect = KinectDevice::Get();
    uint32_t engaged = kinect->GetEngagedEnrollmentIndex();
    if (engaged != kNoEnrolledPlayer &&
        enrollment_index.value() == engaged) {
      result = 0;  // engaged player maps to user 0
    }
#endif
    static std::atomic<bool> first_logged{false};
    if (!first_logged.exchange(true)) {
      XELOGI("Kinect: XamUserNuiGetUserIndex first call enrollment_index={} → user=0x{:02X}",
             enrollment_index.value(), result);
    }
    static std::atomic<uint32_t> last_result{kNoEnrolledPlayer};
    const uint32_t prev = last_result.exchange(result);
    if (prev != result) {
      XELOGI("Kinect: XamUserNuiGetUserIndex enrollment_index={} → user=0x{:02X}",
             enrollment_index.value(), result);
    }
    *out_user_index = result;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndex, kNone, kStub);

// Returns the enrollment index for a given controller slot.
// Enrollment indices are sequential (0 = first tracked player, 1 = second).
// For user 0, we return the enrollment index of the currently engaged player.
dword_result_t XamUserNuiGetEnrollmentIndex_entry(dword_t user_index,
                                                   lpdword_t out_enrollment_index) {
  if (out_enrollment_index) {
    uint32_t result = kNoEnrolledPlayer;
#if XE_PLATFORM_WIN32
    KinectDevice* kinect = KinectDevice::Get();
    // Return the engaged enrollment index for user 0 (and as a best-effort
    // for any user when a player is engaged, since we only support one Kinect
    // player at a time).
    if (user_index.value() == 0) {
      result = kinect->GetEngagedEnrollmentIndex();
    }
#endif
    static std::atomic<bool> first_logged{false};
    if (!first_logged.exchange(true)) {
      XELOGI("Kinect: XamUserNuiGetEnrollmentIndex first call user_index={} → enrollment=0x{:02X}",
             user_index.value(), result);
    }
    static std::atomic<uint32_t> last_result{kNoEnrolledPlayer};
    const uint32_t prev = last_result.exchange(result);
    if (prev != result) {
      XELOGI("Kinect: XamUserNuiGetEnrollmentIndex user_index={} → enrollment=0x{:02X}",
             user_index.value(), result);
    }
    *out_enrollment_index = result;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetEnrollmentIndex, kNone, kStub);

// Updates the engagement scoring for a skeleton slot.  The game provides
// per-skeleton scores that influence which person is selected as "engaged".
// We don't maintain a score table — engagement is determined by the first
// tracked skeleton found by the background polling thread.
dword_result_t XamNuiSkeletonScoreUpdate_entry(unknown_t unk1, unknown_t unk2) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiSkeletonScoreUpdate (first call)");
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiSkeletonScoreUpdate, kNone, kStub);

// Sets camera flags (e.g. near mode, seated tracking).
dword_result_t XamNuiCameraSetFlags_entry(dword_t flags) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiCameraSetFlags flags=0x{:08X} (first call)",
           flags.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraSetFlags, kNone, kStub);

// Stores the current floor plane for use in subsequent tracking.
dword_result_t XamNuiCameraRememberFloor_entry() {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiCameraRememberFloor (first call)");
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraRememberFloor, kNone, kStub);

// Enables or disables NUI automation (testing/scripted input).
dword_result_t XamEnableNuiAutomation_entry(dword_t enable) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamEnableNuiAutomation enable={} (first call)",
           enable.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamEnableNuiAutomation, kNone, kStub);

// Enables or disables Natal playback mode.
dword_result_t XamEnableNatalPlayback_entry(dword_t enable) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamEnableNatalPlayback enable={} (first call)",
           enable.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamEnableNatalPlayback, kNone, kStub);

// ---------------------------------------------------------------------------
// Camera tilt / motor control stubs.
// These control the Kinect's physical tilt motor.  We don't have a real
// motor to drive, so we stub them all as success.
// ---------------------------------------------------------------------------

// Registers a callback that the system calls when the tilt status changes.
dword_result_t XamNuiCameraTiltSetCallback_entry(unknown_t callback,
                                                  unknown_t context) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiCameraTiltSetCallback (first call, callback ignored)");
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltSetCallback, kNone, kStub);

// Returns the current tilt status.  We report "completed" (0) so the
// game does not wait for a motor move that will never happen.
dword_result_t XamNuiCameraTiltGetStatus_entry(lpdword_t status_out) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiCameraTiltGetStatus → 0 (first call)");
  }
  if (status_out) {
    *status_out = 0;  // 0 = not moving / completed
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltGetStatus, kNone, kStub);

// Reports the tilt status back to the system.
dword_result_t XamNuiCameraTiltReportStatus_entry(dword_t status) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiCameraTiltReportStatus status={} (first call)",
           status.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltReportStatus, kNone, kStub);

// Stops any in-progress motor movement.
dword_result_t XamNuiCameraElevationStopMovement_entry() {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiCameraElevationStopMovement (first call)");
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationStopMovement, kNone, kStub);

// Starts an automatic tilt to the optimal angle.
dword_result_t XamNuiCameraElevationAutoTilt_entry() {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiCameraElevationAutoTilt (first call)");
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationAutoTilt, kNone, kStub);

// Registers a callback for elevation-change events.
dword_result_t XamNuiCameraElevationSetCallback_entry(unknown_t callback,
                                                       unknown_t context) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiCameraElevationSetCallback (first call, callback ignored)");
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationSetCallback, kNone, kStub);

// Auto-tilts in the opposite direction.
dword_result_t XamNuiCameraElevationReverseAutoTilt_entry() {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiCameraElevationReverseAutoTilt (first call)");
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationReverseAutoTilt, kNone, kStub);

// Adjusts the tilt by a relative amount.
dword_result_t XamNuiCameraAdjustTilt_entry(int_t delta) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiCameraAdjustTilt delta={} (first call)",
           delta.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraAdjustTilt, kNone, kStub);

// Returns the type of tilt controller (motor vs. manual).
// 0 = no motor, 1 = motorised.  We report motorised so games don't try
// to prompt the user to physically tilt the sensor.
dword_result_t XamNuiCameraGetTiltControllerType_entry(lpdword_t type_out) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiCameraGetTiltControllerType → 1 (motorised, first call)");
  }
  if (type_out) {
    *type_out = 1;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraGetTiltControllerType, kNone, kStub);

// ---------------------------------------------------------------------------
// Biometric / identity stubs.
// These back the Kinect's face-recognition sign-in system.  We don't
// implement real biometric matching; all calls succeed immediately.
// ---------------------------------------------------------------------------

// Returns a session ID for the current NUI identity session.
// Games use this to correlate face-recognition results with a session.
// Returns a non-zero ID only while a player is actively engaged (tracked),
// so the game can tell when someone walks into view.
dword_result_t XamNuiIdentityGetSessionId_entry(lpdword_t session_id_out) {
  uint32_t session_id = 0;
#if XE_PLATFORM_WIN32
  KinectDevice* kinect = KinectDevice::Get();
  if (kinect->IsConnected() &&
      kinect->GetEngagedEnrollmentIndex() != kNoEnrolledPlayer) {
    session_id = 1;
  }
#endif  // XE_PLATFORM_WIN32
  if (session_id_out) {
    static std::atomic<uint32_t> last_session_id{0xFFFFFFFF};
    const uint32_t prev = last_session_id.exchange(session_id);
    if (prev != session_id) {
      XELOGI("Kinect: XamNuiIdentityGetSessionId → {} (changed)", session_id);
    }
    *session_id_out = session_id;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityGetSessionId, kNone, kStub);

// Returns enrollment information for a given enrollment slot.
dword_result_t XamNuiIdentityGetEnrollmentInfo_entry(dword_t slot,
                                                      lpvoid_t info_out) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiIdentityGetEnrollmentInfo slot={} (first call)",
           slot.value());
  }
  if (info_out) {
    // Zero the output buffer.  We don't know the exact size of the structure
    // on the 360 side, but zeroing a safe minimum (32 bytes) satisfies games
    // that check individual fields for non-zero values.
    std::memset(info_out, 0, 32);
  }
  return X_ERROR_NOT_FOUND;  // no enrolled faces
}
DECLARE_XAM_EXPORT1(XamNuiIdentityGetEnrollmentInfo, kNone, kStub);

// Removes a face enrollment entry.
dword_result_t XamNuiIdentityUnenroll_entry(dword_t slot) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiIdentityUnenroll slot={} (first call)", slot.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityUnenroll, kNone, kStub);

// Returns a colour texture generated from the depth image used during
// face recognition.  We return a null texture pointer.
dword_result_t XamNuiIdentityGetColorTexture_entry(lpdword_t texture_out) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiIdentityGetColorTexture (first call)");
  }
  if (texture_out) {
    *texture_out = 0;
  }
  return X_ERROR_NOT_FOUND;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityGetColorTexture, kNone, kStub);

// Returns a bitmask of quality flags for the current recognition frame.
dword_result_t XamNuiIdentityGetQualityFlags_entry(lpdword_t flags_out) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiIdentityGetQualityFlags → 0 (first call)");
  }
  if (flags_out) {
    *flags_out = 0;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityGetQualityFlags, kNone, kStub);

// Fills a string buffer with a human-readable description of the quality flags.
dword_result_t XamNuiIdentityGetQualityFlagsMessage_entry(dword_t flags,
                                                           lpvoid_t buf_out,
                                                           dword_t buf_len) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiIdentityGetQualityFlagsMessage (first call)");
  }
  if (buf_out && buf_len >= 2) {
    // Write a null wide-char (2 bytes) so the game gets an empty string.
    *buf_out.as<uint16_t*>() = 0;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityGetQualityFlagsMessage, kNone, kStub);

// Starts an enrollment-for-sign-in session.
dword_result_t XamNuiIdentityEnrollForSignIn_entry(dword_t user_index,
                                                    dword_t unk) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiIdentityEnrollForSignIn user_index={} (first call)",
           user_index.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityEnrollForSignIn, kNone, kStub);

// Attempts to identify the person using biometric (face) data.
// Returns X_ERROR_NOT_FOUND — no enrolled face to match against.
dword_result_t XamNuiIdentityIdentifyWithBiometric_entry(lpdword_t result_out) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiIdentityIdentifyWithBiometric → NOT_FOUND (first call)");
  }
  if (result_out) {
    *result_out = 0;
  }
  return X_ERROR_NOT_FOUND;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityIdentifyWithBiometric, kNone, kStub);

// Aborts any in-progress identity operation.
dword_result_t XamNuiIdentityAbort_entry() {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamNuiIdentityAbort (first call)");
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityAbort, kNone, kStub);

// ---------------------------------------------------------------------------
// Biometric data persistence (per-user face-model store).
// We return success but supply no data so the game falls back to its
// unenrolled-user code path rather than crashing.
// ---------------------------------------------------------------------------

// Reads previously-stored biometric (face model) data for a user.
dword_result_t XamReadBiometricData_entry(dword_t user_index,
                                          lpvoid_t buf_out,
                                          dword_t buf_size,
                                          lpdword_t bytes_read_out) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamReadBiometricData user_index={} (first call, no data)",
           user_index.value());
  }
  if (bytes_read_out) {
    *bytes_read_out = 0;
  }
  return X_ERROR_NOT_FOUND;  // no stored biometric data
}
DECLARE_XAM_EXPORT1(XamReadBiometricData, kNone, kStub);

// Stores biometric (face model) data for a user.
dword_result_t XamWriteBiometricData_entry(dword_t user_index,
                                           lpvoid_t buf,
                                           dword_t buf_size) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamWriteBiometricData user_index={} size={} (first call, data discarded)",
           user_index.value(), buf_size.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamWriteBiometricData, kNone, kStub);

// Enables or disables biometric (face-recognition) sign-in for a user.
dword_result_t XamUserNuiEnableBiometric_entry(dword_t user_index,
                                                dword_t enable) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamUserNuiEnableBiometric user_index={} enable={} (first call)",
           user_index.value(), enable.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserNuiEnableBiometric, kNone, kStub);

// ---------------------------------------------------------------------------
// NUI overlay UI functions.
// These display Kinect-specific system overlays (sign-in, guide, etc.).
// We log on first call (under the Kinect category so the user can filter)
// and immediately return success so the game continues.
// ---------------------------------------------------------------------------

dword_result_t XamShowNuiGuideUI_entry(dword_t user_index) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamShowNuiGuideUI user_index={} (first call, dismissed)",
           user_index.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiGuideUI, kNone, kStub);

dword_result_t XamShowNuiSigninUI_entry(dword_t user_index, dword_t unk) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamShowNuiSigninUI user_index={} (first call, dismissed)",
           user_index.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiSigninUI, kNone, kStub);

dword_result_t XamShowNuiControllerRequiredUI_entry(dword_t user_index) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamShowNuiControllerRequiredUI user_index={} (first call, dismissed)",
           user_index.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiControllerRequiredUI, kNone, kStub);

dword_result_t XamShowNuiHardwareRequiredUI_entry(dword_t user_index) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamShowNuiHardwareRequiredUI user_index={} (first call, dismissed)",
           user_index.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiHardwareRequiredUI, kNone, kStub);

dword_result_t XamShowNuiFriendsUI_entry(dword_t user_index) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamShowNuiFriendsUI user_index={} (first call, dismissed)",
           user_index.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiFriendsUI, kNone, kStub);

dword_result_t XamShowNuiGamerCardUIForXUID_entry(dword_t user_index,
                                                   lpqword_t xuid_ptr) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamShowNuiGamerCardUIForXUID user_index={} (first call, dismissed)",
           user_index.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiGamerCardUIForXUID, kNone, kStub);

dword_result_t XamShowNuiAchievementsUI_entry(dword_t user_index) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamShowNuiAchievementsUI user_index={} (first call, dismissed)",
           user_index.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiAchievementsUI, kNone, kStub);

dword_result_t XamShowNuiMarketplaceUI_entry(dword_t user_index,
                                              dword_t offer_id) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamShowNuiMarketplaceUI user_index={} (first call, dismissed)",
           user_index.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiMarketplaceUI, kNone, kStub);

dword_result_t XamShowNuiDeviceSelectorUI_entry(dword_t user_index) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamShowNuiDeviceSelectorUI user_index={} (first call, dismissed)",
           user_index.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiDeviceSelectorUI, kNone, kStub);

dword_result_t XamShowNuiDirtyDiscErrorUI_entry(dword_t user_index) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: XamShowNuiDirtyDiscErrorUI user_index={} (first call, dismissed)",
           user_index.value());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiDirtyDiscErrorUI, kNone, kStub);

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
