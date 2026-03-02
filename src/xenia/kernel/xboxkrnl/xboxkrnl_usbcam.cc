/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

dword_result_t XUsbcamCreate_entry(dword_t buffer,
                                   dword_t buffer_size,  // 0x4B000 640x480?
                                   lpunknown_t unk3_ptr) {
  // This function should return success.
  // It looks like it only allocates space for usbcam support.
  // returning error code might cause games to initialize incorrectly.
  // "Carcassonne" initalization function checks for result from this
  // function. If value is different than 0 instead of loading
  // rest of the game it returns from initalization function and tries
  // to run game normally which causes crash, due to uninitialized data.
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamCreate, kNone, kStub);

dword_result_t XUsbcamGetState_entry() {
  // 0 = not connected.
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamGetState, kNone, kStub);

// PsCamDeviceRequest is the internal kernel entry point that the Kinect
// runtime calls to submit I/O requests to the Ps-Cam (Kinect) device driver.
// We stub it as E_NOTIMPL so the runtime falls back gracefully rather than
// crashing on an unresolved import.
dword_result_t PsCamDeviceRequest_entry(dword_t request_code,
                                         lpvoid_t input_buf,
                                         dword_t input_size,
                                         lpvoid_t output_buf,
                                         dword_t output_size,
                                         lpdword_t bytes_returned) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    XELOGI("Kinect: PsCamDeviceRequest request_code=0x{:08X} (first call, not implemented)",
           request_code.value());
  }
  if (bytes_returned) {
    *bytes_returned = 0;
  }
  return X_STATUS_NOT_IMPLEMENTED;
}
DECLARE_XBOXKRNL_EXPORT1(PsCamDeviceRequest, kNone, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Usbcam);
