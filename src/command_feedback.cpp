#include "command_feedback.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>

#include "ui/ui.h"

extern SemaphoreHandle_t gui_mutex;

namespace {

constexpr uint32_t COLOR_PENDING = 0xB08D57;
constexpr uint32_t COLOR_SUCCESS = 0x39704A;
constexpr uint32_t COLOR_ERROR = 0xC8102E;
constexpr uint32_t RESULT_HOLD_MS = 1600;

lv_timer_t* g_power_clear_timer = nullptr;
lv_timer_t* g_steam_clear_timer = nullptr;

lv_obj_t* button_for(CommandFeedbackTarget target)
{
    return target == CommandFeedbackTarget::Power ? ui_powerButton : ui_steamButton;
}

lv_timer_t*& timer_for(CommandFeedbackTarget target)
{
    return target == CommandFeedbackTarget::Power ? g_power_clear_timer : g_steam_clear_timer;
}

CommandFeedbackTarget target_for_timer(lv_timer_t* timer)
{
    return timer == g_power_clear_timer
        ? CommandFeedbackTarget::Power
        : CommandFeedbackTarget::Steam;
}

void configure_feedback_states(lv_obj_t* button)
{
    if (!button) {
        return;
    }

    // State-specific styles leave the normal theme untouched and disappear as
    // soon as their state is cleared.
    lv_obj_set_style_outline_color(
        button, lv_color_hex(COLOR_PENDING), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_outline_opa(
        button, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_outline_width(
        button, 3, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_outline_pad(
        button, 1, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_opa(
        button, LV_OPA_70, LV_PART_MAIN | LV_STATE_DISABLED);

    lv_obj_set_style_outline_color(
        button, lv_color_hex(COLOR_SUCCESS), LV_PART_MAIN | LV_STATE_USER_1);
    lv_obj_set_style_outline_opa(
        button, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_USER_1);
    lv_obj_set_style_outline_width(
        button, 3, LV_PART_MAIN | LV_STATE_USER_1);
    lv_obj_set_style_outline_pad(
        button, 1, LV_PART_MAIN | LV_STATE_USER_1);

    lv_obj_set_style_outline_color(
        button, lv_color_hex(COLOR_ERROR), LV_PART_MAIN | LV_STATE_USER_2);
    lv_obj_set_style_outline_opa(
        button, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_USER_2);
    lv_obj_set_style_outline_width(
        button, 3, LV_PART_MAIN | LV_STATE_USER_2);
    lv_obj_set_style_outline_pad(
        button, 1, LV_PART_MAIN | LV_STATE_USER_2);
}

void delete_clear_timer(CommandFeedbackTarget target)
{
    lv_timer_t*& timer = timer_for(target);
    if (timer) {
        lv_timer_del(timer);
        timer = nullptr;
    }
}

void clear_result_timer_cb(lv_timer_t* timer)
{
    CommandFeedbackTarget target = target_for_timer(timer);
    lv_obj_t* button = button_for(target);
    if (button) {
        lv_obj_clear_state(
            button, static_cast<lv_state_t>(LV_STATE_USER_1 | LV_STATE_USER_2));
        lv_obj_invalidate(button);
    }

    timer_for(target) = nullptr;
    lv_timer_del(timer);
}

void show_pending_no_mutex(CommandFeedbackTarget target)
{
    lv_obj_t* button = button_for(target);
    if (!button) {
        return;
    }

    configure_feedback_states(button);
    delete_clear_timer(target);
    lv_obj_clear_state(
        button, static_cast<lv_state_t>(LV_STATE_USER_1 | LV_STATE_USER_2));
    lv_obj_add_state(button, LV_STATE_DISABLED);
    lv_obj_invalidate(button);
}

void show_result_no_mutex(CommandFeedbackTarget target, bool success)
{
    lv_obj_t* button = button_for(target);
    if (!button) {
        return;
    }

    configure_feedback_states(button);
    delete_clear_timer(target);
    lv_obj_clear_state(
        button,
        static_cast<lv_state_t>(
            LV_STATE_DISABLED | LV_STATE_USER_1 | LV_STATE_USER_2));
    lv_obj_add_state(button, success ? LV_STATE_USER_1 : LV_STATE_USER_2);
    lv_obj_invalidate(button);

    timer_for(target) =
        lv_timer_create(clear_result_timer_cb, RESULT_HOLD_MS, nullptr);
    if (!timer_for(target)) {
        // Do not leave a stale success/error outline behind if LVGL is too low
        // on memory to allocate the one-shot clear timer.
        lv_obj_clear_state(
            button,
            static_cast<lv_state_t>(LV_STATE_USER_1 | LV_STATE_USER_2));
        lv_obj_invalidate(button);
    }
}

}  // namespace

void command_feedback_show_pending_from_ui(CommandFeedbackTarget target)
{
    show_pending_no_mutex(target);
}

void command_feedback_show_error_from_ui(CommandFeedbackTarget target)
{
    show_result_no_mutex(target, false);
}

void command_feedback_complete_from_worker(
    CommandFeedbackTarget target,
    bool success)
{
    if (!gui_mutex) {
        return;
    }

    // This function is only called by the network worker after the blocking
    // request has finished. It is never called from an LVGL callback, so
    // waiting for the UI lock cannot recursively deadlock.
    if (xSemaphoreTake(gui_mutex, portMAX_DELAY) == pdTRUE) {
        show_result_no_mutex(target, success);
        xSemaphoreGive(gui_mutex);
    }
}
