// i2c_lock.h - one lock for the CoreS3's internal I2C bus.   MIT licensed.
//
// Touch, the IMU, the power chip and the camera's sensor-control (SCCB) lines all share ONE I2C bus. M5GFX/M5Unified drive
// it with their own register-level code and say plainly that they must not run at the same time as another driver
// (M5GFX common.inl); esp32-camera's SCCB code talks to the same bus from the camera task on the other core.
// So: take an I2cGuard around ANYTHING that touches the bus from a task other than the one that owns M5.update():
//   loop task   : I2cGuard g; M5.update();  I2cGuard g; M5.Imu.update();       (main.cpp, input.cpp)
//   camera task : I2cGuard g; esp_camera_init(...); sensor->set_*(...); esp_camera_deinit();
// It is recursive (the same task may nest guards). On the PC (emulator, native tests) it does nothing.
// Call scribble::i2cLockInit() once from setup() BEFORE any other task exists (it creates the mutex).
#pragma once
#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
namespace scribble {
inline SemaphoreHandle_t& i2cMutexRef() { static SemaphoreHandle_t m = nullptr; return m; }
inline void i2cLockInit() { if (!i2cMutexRef()) i2cMutexRef() = xSemaphoreCreateRecursiveMutex(); }
struct I2cGuard {
  I2cGuard() { if (i2cMutexRef()) xSemaphoreTakeRecursive(i2cMutexRef(), portMAX_DELAY); }
  ~I2cGuard() { if (i2cMutexRef()) xSemaphoreGiveRecursive(i2cMutexRef()); }
  I2cGuard(const I2cGuard&) = delete; I2cGuard& operator=(const I2cGuard&) = delete;
};
}  // namespace scribble
#else
namespace scribble {
inline void i2cLockInit() {}
struct I2cGuard { I2cGuard() {} ~I2cGuard() {} };
}  // namespace scribble
#endif
