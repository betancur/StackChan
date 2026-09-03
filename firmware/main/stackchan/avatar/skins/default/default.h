/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../../avatar/avatar.h"
#include "../../avatar/elements/feature.h"
#include <lvgl.h>
#include <smooth_lvgl.hpp>
#include <memory>

namespace stackchan::avatar {

/**
 * @brief
 *
 */
/**
 * @brief Optional "kawaii" styling: bigger eyes with two specular highlights
 * and permanent blush marks. Meant for an inverted palette (dark features on a
 * light face), which is what makes black eyes with white shines possible.
 */
struct KawaiiStyle {
    bool enabled          = false;
    int eyeSizePx         = 44;                       // open-eye diameter (default skin: 20)
    bool pupil            = true;                     // dark pupil inside a light eye (for dark faces)
    lv_color_t pupilColor = lv_color_black();
    lv_color_t shineColor = lv_color_white();
    lv_color_t blushColor = lv_color_hex(0xF5A0AF);
    int mouthWidthPx      = 40;                       // closed-mouth width (default skin: 90)
};

class DefaultAvatar : public Avatar {
public:
    lv_color_t primaryColor   = lv_color_white();
    lv_color_t secondaryColor = lv_color_black();
    KawaiiStyle kawaii;  // set before init()

    void init(lv_obj_t* parent, const lv_font_t* font = &lv_font_montserrat_16);
    uitk::lvgl_cpp::Container* getPanel() const;

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _pannel;
    std::unique_ptr<uitk::lvgl_cpp::Container> _blush_left;
    std::unique_ptr<uitk::lvgl_cpp::Container> _blush_right;
};

/**
 * @brief
 *
 */
class DefaultEyes : public Feature {
public:
    DefaultEyes(lv_obj_t* parent, lv_color_t primaryColor, lv_color_t secondaryColor, bool isLeftEye);
    ~DefaultEyes();

    void setPosition(const uitk::Vector2i& position) override;
    void setWeight(int weight) override;
    void setRotation(int rotation) override;
    void setEmotion(const Emotion& emotion) override;
    void setVisible(bool visible) override;
    void setSize(int size) override;

    /// Kawaii look: open-eye diameter in px, optional pupil, two highlight dots.
    void setKawaii(int eyeSizePx, bool pupil, lv_color_t pupilColor, lv_color_t shineColor);

    /// Kawaii: where the pupil looks (-100..100 each axis, 0 = centre)
    void setGaze(int x, int y);

private:
    bool _is_left_eye    = false;
    int _eyelid_offset_y = 0;
    int _max_eye_px      = 32;  // container size / largest eye
    bool _kawaii         = false;
    bool _arch_mode      = false;  // Happy: "^" arch instead of the round eye
    int _gaze_x          = 0;
    int _gaze_y          = 0;
    lv_color_t _primary_color;
    lv_obj_t* _arch      = nullptr;

    std::unique_ptr<uitk::lvgl_cpp::Container> _container;
    std::unique_ptr<uitk::lvgl_cpp::Container> _eye;
    std::unique_ptr<uitk::lvgl_cpp::Container> _pupil;
    std::unique_ptr<uitk::lvgl_cpp::Container> _shine_big;
    std::unique_ptr<uitk::lvgl_cpp::Container> _shine_small;
    std::unique_ptr<uitk::lvgl_cpp::Container> _eyelid;

    void layout_shines(int eye_size);
    void layout_arch(int eye_size);
};

/**
 * @brief
 *
 */
class DefaultMouth : public Feature {
public:
    DefaultMouth(lv_obj_t* parent, lv_color_t primaryColor, lv_color_t secondaryColor);
    ~DefaultMouth();

    void setPosition(const uitk::Vector2i& position) override;
    void setWeight(int weight) override;
    void setRotation(int rotation) override;
    void setVisible(bool visible) override;

    /// Kawaii look: narrower mouth (closed width in px; open width follows)
    void setKawaii(int closedWidthPx);
    void setEmotion(const Emotion& emotion) override;

private:
    int _closed_width = 90;
    int _open_width   = 60;
    bool _kawaii      = false;
    bool _cat_mode    = false;  // Happy: "ω" cat mouth instead of the bar
    lv_color_t _primary_color;
    lv_obj_t* _cat_left  = nullptr;
    lv_obj_t* _cat_right = nullptr;
    std::unique_ptr<uitk::lvgl_cpp::Container> _mouth;

    void layout_cat();
};

/**
 * @brief
 *
 */
class DefaultSpeechBubble : public SpeechBubble {
public:
    DefaultSpeechBubble(lv_obj_t* parent, lv_color_t primaryColor, lv_color_t secondaryColor, const lv_font_t* font);
    ~DefaultSpeechBubble();

    void setSpeech(std::string_view text) override;
    void clearSpeech() override;
    void setVisible(bool visible) override;
    void setTextFont(void* font) override;

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _container;
    std::unique_ptr<uitk::lvgl_cpp::Image> _arrow;
    std::unique_ptr<uitk::lvgl_cpp::Container> _bubble;
    std::unique_ptr<uitk::lvgl_cpp::Label> _text;
};

}  // namespace stackchan::avatar
