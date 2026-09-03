/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "default.h"
#include <algorithm>

using namespace uitk;
using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

static const Vector2i _mouth_pos        = Vector2i(0, 26);
static const Vector2i _mouth_min_offset = Vector2i(-16, -16);
static const Vector2i _mouth_max_offset = Vector2i(16, 16);
static const Vector2i _mouth_min_size   = Vector2i(90, 6);
static const Vector2i _mouth_max_size   = Vector2i(60, 50);
static const int _mouth_min_radius      = 0;
static const int _mouth_max_radius      = 16;

DefaultMouth::DefaultMouth(lv_obj_t* parent, lv_color_t primaryColor, lv_color_t secondaryColor)
{
    _primary_color = primaryColor;
    _mouth = std::make_unique<Container>(parent);
    _mouth->setAlign(LV_ALIGN_CENTER);
    _mouth->setBorderWidth(0);
    _mouth->setBgColor(primaryColor);
    _mouth->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    setPosition(_position);
    setWeight(0);
    setRotation(0);
}

DefaultMouth::~DefaultMouth()
{
    if (_cat_left)  { lv_obj_delete(_cat_left);  _cat_left  = nullptr; }
    if (_cat_right) { lv_obj_delete(_cat_right); _cat_right = nullptr; }
    _mouth.reset();
}

void DefaultMouth::setKawaii(int closedWidthPx)
{
    _closed_width = closedWidthPx;
    _open_width   = std::max(closedWidthPx, closedWidthPx * 110 / 100);
    _kawaii       = true;

    // "ω" cat mouth: two small bottom arcs, shown while Happy
    auto make_cat = [&]() {
        lv_obj_t* a = lv_arc_create(lv_obj_get_parent(_mouth->get()));
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_SCROLLABLE);
        lv_arc_set_bg_angles(a, 20, 160);  // bottom arc
        lv_arc_set_value(a, 0);
        lv_obj_set_style_arc_color(a, _primary_color, LV_PART_MAIN);
        lv_obj_set_style_arc_width(a, 5, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(a, 0, LV_PART_MAIN);
        lv_obj_set_size(a, 20, 20);
        lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
        return a;
    };
    _cat_left  = make_cat();
    _cat_right = make_cat();
    layout_cat();
    setWeight(getWeight());
}

void DefaultMouth::layout_cat()
{
    if (!_cat_left || !_cat_right) {
        return;
    }
    // Follow the bar's position; the two arcs touch at the centre
    int x = _mouth_pos.x + map_range(_position.x, -100, 100, _mouth_min_offset.x, _mouth_max_offset.x);
    int y = _mouth_pos.y + map_range(_position.y, -100, 100, _mouth_min_offset.y, _mouth_max_offset.y);
    lv_obj_align(_cat_left, LV_ALIGN_CENTER, x - 9, y - 4);
    lv_obj_align(_cat_right, LV_ALIGN_CENTER, x + 9, y - 4);
}

void DefaultMouth::setEmotion(const Emotion& emotion)
{
    if (getIgnoreEmotion() || !_kawaii || !_cat_left) {
        return;
    }
    bool want_cat = (emotion == Emotion::Happy);
    if (want_cat == _cat_mode) {
        return;
    }
    _cat_mode = want_cat;
    _mouth->setHidden(want_cat);
    if (want_cat) {
        lv_obj_remove_flag(_cat_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(_cat_right, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(_cat_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_cat_right, LV_OBJ_FLAG_HIDDEN);
    }
}

void DefaultMouth::setPosition(const Vector2i& position)
{
    Element::setPosition(position);

    auto pos_x = _mouth_pos.x + map_range(_position.x, -100, 100, _mouth_min_offset.x, _mouth_max_offset.x);
    auto pos_y = _mouth_pos.y + map_range(_position.y, -100, 100, _mouth_min_offset.y, _mouth_max_offset.y);

    _mouth->setPos(pos_x, pos_y);
    layout_cat();
}

void DefaultMouth::setWeight(int weight)
{
    Feature::setWeight(weight);

    auto size_x = map_range(_weight, 0, 100, _closed_width, _open_width);
    auto size_y = map_range(_weight, 0, 100, _mouth_min_size.y, _mouth_max_size.y);
    auto radius = map_range(_weight, 0, 100, _mouth_min_radius, _mouth_max_radius);

    _mouth->setSize(size_x, size_y);
    _mouth->setRadius(radius);
}

void DefaultMouth::setRotation(int rotation)
{
    Element::setRotation(rotation);

    _mouth->setTransformPivot(_mouth->getWidth() / 2, _mouth->getHeight() / 2);
    _mouth->setRotation(rotation);
}

void DefaultMouth::setVisible(bool visible)
{
    Element::setVisible(visible);

    _mouth->setHidden(!visible);
    if (_cat_left && _cat_right) {
        bool show_cat = visible && _cat_mode;
        if (show_cat) { lv_obj_remove_flag(_cat_left, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(_cat_right, LV_OBJ_FLAG_HIDDEN); }
        else          { lv_obj_add_flag(_cat_left, LV_OBJ_FLAG_HIDDEN);    lv_obj_add_flag(_cat_right, LV_OBJ_FLAG_HIDDEN); }
    }
}
