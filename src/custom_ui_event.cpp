#include "ui/ui.h"
#include "lamarzocco_machine.h"
#include "command_feedback.h"
#include "display_power.h"
#include "web.h"

extern LaMarzoccoMachine* g_machine;

void wifiSetup(lv_event_t *e)
{
    (void)e;
    display_power_mark_user_activity();
    if (display_power_wake_if_dimmed()) {
        return;
    }

    requestWEBSetup();
    lv_label_set_text(ui_SSIDLabel, "SSID: " AP_SSID);
    lv_label_set_text(ui_URLLabel, "URL:  http://" AP_SSID ".local");
    lv_scr_load(ui_setupWifiScreen);
}

void turnOnMachine(lv_event_t * e)
{
  (void)e;
  display_power_mark_user_activity();
  if (display_power_wake_if_dimmed()) {
    return;
  }

  command_feedback_show_pending_from_ui(CommandFeedbackTarget::Power);

  if (!g_machine) {
    Serial.println("[COMMAND] Power toggle unavailable: machine is not initialized");
    command_feedback_show_error_from_ui(CommandFeedbackTarget::Power);
    return;
  }

  LaMarzoccoMachine::QueueResult result = g_machine->queue_power_toggle();
  if (result == LaMarzoccoMachine::QueueResult::Queued) {
    Serial.println("[COMMAND] Power toggle queued");
  } else if (result == LaMarzoccoMachine::QueueResult::AlreadyPending) {
    Serial.println("[COMMAND] Ignoring duplicate power toggle");
  } else {
    Serial.println("[COMMAND] Power toggle queue unavailable");
    command_feedback_show_error_from_ui(CommandFeedbackTarget::Power);
  }
}

void toggleSteamBoiler(lv_event_t * e)
{
  (void)e;
  display_power_mark_user_activity();
  if (display_power_wake_if_dimmed()) {
    return;
  }

  command_feedback_show_pending_from_ui(CommandFeedbackTarget::Steam);

  if (!g_machine) {
    Serial.println("[COMMAND] Steam toggle unavailable: machine is not initialized");
    command_feedback_show_error_from_ui(CommandFeedbackTarget::Steam);
    return;
  }

  LaMarzoccoMachine::QueueResult result = g_machine->queue_steam_toggle();
  if (result == LaMarzoccoMachine::QueueResult::Queued) {
    Serial.println("[COMMAND] Steam toggle queued");
  } else if (result == LaMarzoccoMachine::QueueResult::AlreadyPending) {
    Serial.println("[COMMAND] Ignoring duplicate steam toggle");
  } else {
    Serial.println("[COMMAND] Steam toggle queue unavailable");
    command_feedback_show_error_from_ui(CommandFeedbackTarget::Steam);
  }
}
