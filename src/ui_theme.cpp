#include "ui_theme.h"

#include "ui/ui.h"

namespace {

constexpr uint32_t COLOR_PAPER = 0xFAF7EF;
constexpr uint32_t COLOR_PAPER_LOW = 0xE8E0CE;
constexpr uint32_t COLOR_INK = 0x1C1A17;
constexpr uint32_t COLOR_MUTED_INK = 0x6F6A60;
constexpr uint32_t COLOR_BRASS = 0xB08D57;
constexpr uint32_t COLOR_LM_RED = 0xC8102E;

void set_screen_background(lv_obj_t* screen)
{
    if (!screen) {
        return;
    }

    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_PAPER), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void set_text_color(lv_obj_t* object, uint32_t color)
{
    if (!object) {
        return;
    }

    lv_obj_set_style_text_color(object, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(object, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void style_boiler_arc(lv_obj_t* arc)
{
    if (!arc) {
        return;
    }

    lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_PAPER_LOW), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(arc, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_LM_RED), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(arc, 9, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    // The boiler values are read-only. Removing the knob makes the arcs read
    // like gauges instead of editable controls.
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB | LV_STATE_DEFAULT);
}

void style_icon_button(lv_obj_t* button)
{
    if (!button) {
        return;
    }

    lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_PAPER), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(button, lv_color_hex(COLOR_BRASS), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(button, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(button, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_PAPER_LOW), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(COLOR_LM_RED), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 2, LV_PART_MAIN | LV_STATE_PRESSED);
}

void style_main_screen(void)
{
    set_screen_background(ui_mainScreen);

    style_boiler_arc(ui_Arc2);
    style_boiler_arc(ui_Arc3);
    style_icon_button(ui_powerButton);
    style_icon_button(ui_steamButton);

    set_text_color(ui_timeLabel, COLOR_INK);
    set_text_color(ui_CoffeeCountLabel, COLOR_INK);
    set_text_color(ui_FlushCountLabel, COLOR_INK);
    set_text_color(ui_CoffeeLabel, COLOR_INK);
    set_text_color(ui_SteamLabel, COLOR_INK);
    set_text_color(ui_CoffeeTempLabel, COLOR_MUTED_INK);
    set_text_color(ui_BoilerTempLabel, COLOR_MUTED_INK);
    set_text_color(ui_SecValueLabel, COLOR_INK);
    set_text_color(ui_SecondsLabel, COLOR_INK);
    set_text_color(ui_waterAlarmLabel, COLOR_LM_RED);

    if (ui_SecPanel) {
        lv_obj_set_style_bg_color(ui_SecPanel, lv_color_hex(COLOR_PAPER), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ui_SecPanel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_SecPanel, lv_color_hex(COLOR_BRASS), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(ui_SecPanel, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ui_SecPanel, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_opa(ui_SecPanel, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    if (ui_SecondsLabel) {
        lv_label_set_text(ui_SecondsLabel, "SECONDS");
        lv_obj_set_style_text_letter_space(ui_SecondsLabel, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

void style_setup_screens(void)
{
    set_screen_background(ui_welcomeScreen);
    set_screen_background(ui_NoConnectionScreen);
    set_screen_background(ui_setupWifiScreen);

    set_text_color(ui_welcomeLabel, COLOR_INK);
    set_text_color(ui_ErrorLabel, COLOR_INK);
    set_text_color(ui_WifiSetupLabel, COLOR_INK);
    set_text_color(ui_NoWifiLabel1, COLOR_INK);
    set_text_color(ui_SSIDLabel, COLOR_INK);
    set_text_color(ui_URLLabel, COLOR_MUTED_INK);

    style_icon_button(ui_WifiSetupBtn);

    if (ui_Spinner1) {
        lv_obj_set_style_arc_color(ui_Spinner1, lv_color_hex(COLOR_PAPER_LOW), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_arc_color(ui_Spinner1, lv_color_hex(COLOR_LM_RED), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    }
}

}  // namespace

void ui_theme_apply(void)
{
    style_main_screen();
    style_setup_screens();
}
