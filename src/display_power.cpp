#include "display_power.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

SemaphoreHandle_t state_mutex = nullptr;
portMUX_TYPE mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

LilyGo_Class *display = nullptr;
uint8_t active_brightness = 0;
uint8_t dim_brightness = 0;
uint32_t idle_timeout_ms = 0;
uint32_t last_user_activity_ms = 0;
uint32_t last_machine_activity_ms = 0;
bool dimmed = false;
bool initialized = false;

SemaphoreHandle_t get_state_mutex()
{
    portENTER_CRITICAL(&mutex_init_lock);
    SemaphoreHandle_t mutex = state_mutex;
    portEXIT_CRITICAL(&mutex_init_lock);
    return mutex;
}

SemaphoreHandle_t ensure_state_mutex()
{
    SemaphoreHandle_t mutex = get_state_mutex();
    if (mutex != nullptr) {
        return mutex;
    }

    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    if (candidate == nullptr) {
        return nullptr;
    }

    portENTER_CRITICAL(&mutex_init_lock);
    if (state_mutex == nullptr) {
        state_mutex = candidate;
        candidate = nullptr;
    }
    mutex = state_mutex;
    portEXIT_CRITICAL(&mutex_init_lock);

    if (candidate != nullptr) {
        vSemaphoreDelete(candidate);
    }
    return mutex;
}

bool lock_state(SemaphoreHandle_t &mutex)
{
    mutex = get_state_mutex();
    return mutex != nullptr &&
           xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
}

void unlock_state(SemaphoreHandle_t mutex)
{
    xSemaphoreGive(mutex);
}

bool timeout_elapsed(uint32_t now_ms, uint32_t activity_ms)
{
    // Unsigned subtraction remains correct when millis() wraps.
    return static_cast<uint32_t>(now_ms - activity_ms) >= idle_timeout_ms;
}

void set_dimmed_locked(bool should_dim)
{
    if (!initialized || dimmed == should_dim) {
        return;
    }

    display->setBrightness(should_dim ? dim_brightness : active_brightness);
    dimmed = should_dim;
}

} // namespace

bool display_power_init(LilyGo_Class *new_display,
                        uint8_t new_active_brightness,
                        uint8_t new_dim_brightness,
                        uint32_t new_idle_timeout_ms)
{
    if (new_display == nullptr) {
        return false;
    }

    SemaphoreHandle_t mutex = ensure_state_mutex();
    if (mutex == nullptr ||
        xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    const uint32_t now_ms = static_cast<uint32_t>(millis());
    display = new_display;
    active_brightness = new_active_brightness;
    dim_brightness = new_dim_brightness;
    idle_timeout_ms = new_idle_timeout_ms;
    last_user_activity_ms = now_ms;
    last_machine_activity_ms = now_ms;
    dimmed = false;
    initialized = true;
    display->setBrightness(active_brightness);

    unlock_state(mutex);
    return true;
}

void display_power_mark_user_activity()
{
    SemaphoreHandle_t mutex;
    if (!lock_state(mutex)) {
        return;
    }

    if (initialized) {
        last_user_activity_ms = static_cast<uint32_t>(millis());
    }

    unlock_state(mutex);
}

void display_power_mark_machine_activity()
{
    SemaphoreHandle_t mutex;
    if (!lock_state(mutex)) {
        return;
    }

    if (initialized) {
        last_machine_activity_ms = static_cast<uint32_t>(millis());
    }

    unlock_state(mutex);
}

void display_power_loop()
{
    SemaphoreHandle_t mutex;
    if (!lock_state(mutex)) {
        return;
    }

    if (initialized) {
        bool should_dim = false;
        if (idle_timeout_ms != 0) {
            const uint32_t now_ms = static_cast<uint32_t>(millis());
            should_dim = timeout_elapsed(now_ms, last_user_activity_ms) &&
                         timeout_elapsed(now_ms, last_machine_activity_ms);
        }
        set_dimmed_locked(should_dim);
    }

    unlock_state(mutex);
}

void display_power_wake()
{
    SemaphoreHandle_t mutex;
    if (!lock_state(mutex)) {
        return;
    }

    set_dimmed_locked(false);
    unlock_state(mutex);
}

bool display_power_wake_if_dimmed()
{
    SemaphoreHandle_t mutex;
    if (!lock_state(mutex)) {
        return false;
    }

    const bool was_dimmed = initialized && dimmed;
    set_dimmed_locked(false);
    unlock_state(mutex);
    return was_dimmed;
}

bool display_power_is_dimmed()
{
    SemaphoreHandle_t mutex;
    if (!lock_state(mutex)) {
        return false;
    }

    const bool result = initialized && dimmed;
    unlock_state(mutex);
    return result;
}
