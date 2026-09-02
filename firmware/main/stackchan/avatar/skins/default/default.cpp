/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "default.h"

using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

void DefaultAvatar::init(lv_obj_t* parent, const lv_font_t* font)
{
    _pannel = std::make_unique<Container>(parent);
    _pannel->align(LV_ALIGN_CENTER, 0, 0);
    _pannel->setSize(320, 240);
    _pannel->setRadius(0);
    _pannel->setBorderWidth(0);
    _pannel->setBgColor(secondaryColor);
    _pannel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    if (kawaii.enabled) {
        // Blush marks sit under the eyes; create them first so eyes draw on top
        auto make_blush = [&](int x) {
            auto b = std::make_unique<Container>(_pannel->get());
            b->setSize(30, 12);
            b->setRadius(LV_RADIUS_CIRCLE);
            b->setBorderWidth(0);
            b->setBgColor(kawaii.blushColor);
            b->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
            b->removeFlag(LV_OBJ_FLAG_CLICKABLE);
            b->align(LV_ALIGN_CENTER, x, 16);
            return b;
        };
        _blush_left  = make_blush(-82);
        _blush_right = make_blush(82);
    }

    auto left_eye  = std::make_unique<DefaultEyes>(_pannel->get(), primaryColor, secondaryColor, true);
    auto right_eye = std::make_unique<DefaultEyes>(_pannel->get(), primaryColor, secondaryColor, false);
    if (kawaii.enabled) {
        left_eye->setKawaii(kawaii.eyeSizePx, kawaii.pupil, kawaii.pupilColor, kawaii.shineColor);
        right_eye->setKawaii(kawaii.eyeSizePx, kawaii.pupil, kawaii.pupilColor, kawaii.shineColor);
    }
    _key_elements.leftEye  = std::move(left_eye);
    _key_elements.rightEye = std::move(right_eye);
    auto mouth = std::make_unique<DefaultMouth>(_pannel->get(), primaryColor, secondaryColor);
    if (kawaii.enabled) {
        mouth->setKawaii(kawaii.mouthWidthPx);
    }
    _key_elements.mouth = std::move(mouth);
    _key_elements.speechBubble =
        std::make_unique<DefaultSpeechBubble>(_pannel->get(), primaryColor, secondaryColor, font);
}

Container* DefaultAvatar::getPanel() const
{
    if (_pannel) {
        return _pannel.get();
    }
    return NULL;
}
