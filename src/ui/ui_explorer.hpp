#pragma once

#include <switch.h>

#include "render.hpp"
#include "ui/ui_common.hpp"

namespace sw::ui {

class Explorer: public Widget {
    public:
        Explorer(Renderer &renderer, Context &context);
        virtual ~Explorer();

        virtual bool update_state(PadState &pad, HidTouchScreenState &touch) override;

        virtual void render() override;

        static constexpr inline std::string_view path_from_entry_name(std::string_view name) {
            return name.substr(name.find("##")+2);
        }

        static constexpr inline std::string_view filename_from_entry_name(std::string_view name) {
            return name.substr(0, name.find("##"));
        }

    public:
        Context &context;

        Renderer::Texture file_texture, folder_texture,
            recent_texture, sd_texture, usb_texture, network_texture;

        bool is_focused = false;

        fs::Path path;
        fs::Path selection;

        std::vector<fs::Node> entries;
        std::size_t cur_focused_entry = -1;

        bool is_initial_scan     = true;
        bool need_directory_scan = true;
        bool want_focus_reset    = false;

        // Animated selection highlight (Tier 3): smoothly slides toward the focused row.
        float sel_x0 = 0.0f, sel_y0 = 0.0f, sel_x1 = 0.0f, sel_y1 = 0.0f;
        bool  sel_valid = false;
};

} // namespace sw::ui
