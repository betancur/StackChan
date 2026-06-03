/*
 * RozSettingsWorker — Setup screen for Roz Voice Agent options
 */
#include "workers.h"
#include <mooncake_log.h>
#include <hal/hal.h>

using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace setup_workers;

static std::string _tag = "Setup-Roz";

RozSettingsWorker::RozSettingsWorker()
{
    mclog::info("RozSettingsWorker start");

    _config = GetHAL().getXiaozhiConfig();

    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setBorderWidth(0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->setPadding(0, 50, 24, 18);
    _panel->setScrollDir(LV_DIR_VER);
    _panel->setScrollbarMode(LV_SCROLLBAR_MODE_ACTIVE);

    _panel_boot = std::make_unique<Container>(_panel->get());
    _panel_boot->setSize(296, 120);
    _panel_boot->align(LV_ALIGN_TOP_MID, 0, 20);
    _panel_boot->setBgColor(lv_color_hex(0xD2E3FF));
    _panel_boot->setBorderWidth(0);
    _panel_boot->setRadius(18);
    _panel_boot->setPadding(0, 0, 0, 0);
    _panel_boot->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _label_boot_title = std::make_unique<Label>(_panel_boot->get());
    _label_boot_title->setText("Start Roz on boot:");
    _label_boot_title->setTextFont(&lv_font_montserrat_16);
    _label_boot_title->setTextColor(lv_color_hex(0x26206A));
    _label_boot_title->setWidth(260);
    _label_boot_title->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _label_boot_title->align(LV_ALIGN_TOP_MID, 0, 18);

    _switch_start_roz_on_boot = std::make_unique<Switch>(_panel_boot->get());
    _switch_start_roz_on_boot->setSize(64, 36);
    _switch_start_roz_on_boot->align(LV_ALIGN_TOP_MID, 0, 66);
    _switch_start_roz_on_boot->setBgColor(lv_color_hex(0xB8D3FD), LV_PART_MAIN);
    _switch_start_roz_on_boot->setBgColor(lv_color_hex(0x4A90D9), LV_PART_INDICATOR | LV_STATE_CHECKED);
    _switch_start_roz_on_boot->setBgColor(lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    if (_config.startRozOnBoot) {
        _switch_start_roz_on_boot->addState(LV_STATE_CHECKED);
    }

    _btn_confirm = std::make_unique<Button>(_panel->get());
    apply_button_common_style(*_btn_confirm);
    _btn_confirm->align(LV_ALIGN_TOP_MID, 0, 160);
    _btn_confirm->setSize(290, 50);
    _btn_confirm->label().setText("Confirm");
    _btn_confirm->onClick().connect([this]() { _confirm_flag = true; });
}

void RozSettingsWorker::update()
{
    if (_confirm_flag) {
        _confirm_flag              = false;
        _config.startRozOnBoot     = _switch_start_roz_on_boot->getValue();
        GetHAL().setXiaozhiConfig(_config);
        mclog::tagInfo(_tag, "roz config updated: startRozOnBoot={}", _config.startRozOnBoot);
        _is_done = true;
    }
}
