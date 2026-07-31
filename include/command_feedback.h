#pragma once

#include <stdint.h>

enum class CommandFeedbackTarget : uint8_t {
    Power,
    Steam
};

// UI-thread variants must only be called from an LVGL event callback. The
// caller already owns the GUI mutex, so these deliberately do not take it.
void command_feedback_show_pending_from_ui(CommandFeedbackTarget target);
void command_feedback_show_error_from_ui(CommandFeedbackTarget target);

// Worker-thread completion safely acquires the GUI mutex before touching LVGL.
void command_feedback_complete_from_worker(CommandFeedbackTarget target, bool success);
