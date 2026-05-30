// Copyright (c) 2024 averne <averne381@gmail.com>
//
// This file is part of SwitchWave.
//
// SwitchWave is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// SwitchWave is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SwitchWave.  If not, see <http://www.gnu.org/licenses/>.

#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <array>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <switch.h>

// Borealis does not fully reset its static Application state after quit.
// During this transition SwitchWave opens a Borealis shell between playback
// sessions, so this TU gets narrow access to reset those fields cleanly.
#define private public
#include <borealis.hpp>
#undef private

#include "context.hpp"
#include "fs/fs_common.hpp"
#include "fs/fs_recent.hpp"
#include "utils.hpp"
#include "ui/ui_borealis.hpp"

namespace sw::ui {

namespace {

using namespace std::string_view_literals;

constexpr float PagePaddingX = 56.0f;
constexpr float PagePaddingY = 34.0f;
constexpr float RowHeight    = 58.0f;
constexpr float RowGap       = 8.0f;
constexpr std::size_t ConfigTextLimit = 4095;

std::string_view app_version_str = "v" APP_VERSION,
    app_build_date_str = __DATE__ " " __TIME__;

std::string_view filename_from_entry_name(std::string_view name) {
    return name.substr(0, name.find("##"));
}

std::string_view path_from_entry_name(std::string_view name) {
    return name.substr(name.find("##") + 2);
}

void clear_children(brls::Box *box) {
    brls::Application::currentFocus = nullptr;

    auto &children = box->getChildren();
    while (!children.empty())
        box->removeView(children.front());
}

void focus_default(brls::Box *box) {
    if (auto *focus = box->getDefaultFocus())
        brls::Application::giveFocus(focus);
}

brls::Label *make_label(std::string text, float font_size = 22.0f, NVGcolor color = brls::TRANSPARENT) {
    auto *label = new brls::Label();
    label->setText(std::move(text));
    label->setFontSize(font_size);
    label->setWidth(brls::View::AUTO);
    label->setHeight(brls::View::AUTO);
    label->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    label->setVerticalAlign(brls::VerticalAlign::CENTER);
    if (color.a != 0.0f)
        label->setTextColor(color);
    return label;
}

brls::Label *make_section(std::string text) {
    auto *label = make_label(std::move(text), 20.0f, nvgRGB(105, 105, 115));
    label->setMarginTop(18.0f);
    label->setMarginBottom(6.0f);
    return label;
}

brls::Button *make_button(std::string text, brls::ActionListener action) {
    auto *button = new brls::Button();
    button->setText(std::move(text));
    button->setStyle(&brls::BUTTONSTYLE_DEFAULT);
    button->setWidth(brls::View::AUTO);
    button->setHeight(RowHeight);
    button->setMarginBottom(RowGap);
    button->registerClickAction(std::move(action));
    return button;
}

brls::Box *make_page() {
    auto *page = new brls::Box(brls::Axis::COLUMN);
    page->setWidth(brls::View::AUTO);
    page->setHeight(brls::View::AUTO);
    page->setGrow(1.0f);
    page->setPadding(PagePaddingY, PagePaddingX, PagePaddingY, PagePaddingX);
    return page;
}

brls::ScrollingFrame *make_scroller(brls::View *content) {
    auto *scroller = new brls::ScrollingFrame();
    scroller->setWidth(brls::View::AUTO);
    scroller->setHeight(brls::View::AUTO);
    scroller->setGrow(1.0f);
    scroller->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroller->setContentView(content);
    return scroller;
}

std::string bool_text(bool value) {
    return value ? "On" : "Off";
}

std::string filesystems_signature(const Context &context) {
    std::string signature;
    for (auto &filesystem: context.filesystems) {
        signature += filesystem->mount_name;
        signature += '\n';
    }
    return signature;
}

void notify_error(Context &context, std::string_view prefix, int rc, Context::ErrorType type) {
    context.set_error(rc, type);
    brls::Application::notify(std::string(prefix) + ": " + std::to_string(rc));
}

void open_keyboard(std::string header, std::string initial, std::size_t max_len,
        SwkbdType type, std::function<void(std::string)> after) {
#ifdef __SWITCH__
    SwkbdConfig config = {};
    if (R_FAILED(swkbdCreate(&config, 0))) {
        brls::Application::notify("Failed to open keyboard");
        return;
    }
    SW_SCOPEGUARD([&config] { swkbdClose(&config); });

    if (initial.size() > max_len)
        initial.resize(max_len);

    swkbdConfigMakePresetDefault(&config);
    swkbdConfigSetHeaderText(&config, header.c_str());
    swkbdConfigSetInitialText(&config, initial.c_str());
    swkbdConfigSetStringLenMax(&config, max_len);
    swkbdConfigSetStringLenMax(&config, 1);
    swkbdConfigSetKeySetDisableBitmask(&config, 0);
    swkbdConfigSetBlurBackground(&config, true);
    swkbdConfigSetType(&config, type);

    std::vector<char> buffer(std::max<std::size_t>((max_len + 1) * 4, 0x100), '\0');
    if (R_SUCCEEDED(swkbdShow(&config, buffer.data(), buffer.size())) && buffer[0] != '\0')
        after(buffer.data());
#else
    SW_UNUSED(header, type);
    after(std::move(initial));
#endif
}

void edit_text(std::string header, utils::StaticString32 &target,
        std::function<void()> after = [] {}) {
    open_keyboard(std::move(header), target.c_str(), target.capacity(), SwkbdType_Normal, [&target, after](std::string value) {
        target = value;
        after();
    });
}

void edit_number(std::string header, std::size_t &target, std::function<void()> after = [] {}) {
    open_keyboard(std::move(header), std::to_string(target), 8, SwkbdType_NumPad, [&target, after](std::string value) {
        target = std::strtoull(value.c_str(), nullptr, 10);
        after();
    });
}

void reset_borealis_static_state() {
    brls::Application::inited        = false;
    brls::Application::quitRequested = false;
    brls::Application::platform      = nullptr;
    brls::Application::title.clear();
    brls::Application::fontStash.clear();
    brls::Application::activitiesStack.clear();
    brls::Application::focusStack.clear();
    brls::Application::currentFocus = nullptr;
    brls::Application::blockInputsTokens = 0;
    brls::Application::commonFooter.clear();
    brls::Application::globalQuitEnabled      = false;
    brls::Application::gloablQuitIdentifier   = ACTION_NONE;
    brls::Application::globalFPSToggleEnabled = false;
    brls::Application::gloablFPSToggleIdentifier = brls::ACTION_NONE;
    brls::Application::repetitionOldFocus = nullptr;
    brls::Application::xmlViewsRegister.clear();
}

void apply_latitude_theme() {
    auto light = brls::getLightTheme();
    auto dark  = brls::getDarkTheme();

    auto accent    = nvgRGB(0, 194, 201);
    auto accent_hi = nvgRGB(33, 217, 224);

    for (auto theme: { light, dark }) {
        theme.addColor("brls/click_pulse", nvgRGBA(0, 194, 201, 38));
        theme.addColor("brls/highlight/color1", accent);
        theme.addColor("brls/highlight/color2", accent_hi);
        theme.addColor("brls/sidebar/active_item", accent);
        theme.addColor("brls/button/primary_enabled_background", accent);
    }

    brls::getStyle().addMetric("sw/page_padding_x", PagePaddingX);
    brls::getStyle().addMetric("sw/page_padding_y", PagePaddingY);
}

class ExplorerTab final: public brls::Box {
    public:
        explicit ExplorerTab(Context &context): brls::Box(brls::Axis::COLUMN), context(context) {
            this->setWidth(brls::View::AUTO);
            this->setHeight(brls::View::AUTO);
            this->setGrow(1.0f);
            this->known_filesystems = filesystems_signature(this->context);
            this->rebuild(true);

            this->registerAction("Back", brls::BUTTON_B, [this](brls::View *) {
                if (!this->path.empty() && !fs::Path(this->path).is_root()) {
                    this->path = std::string(fs::Path(this->path).parent());
                    this->request_rebuild(true);
                    return true;
                }
                return false;
            });
        }

    private:
        void draw(NVGcontext *vg, float x, float y, float width, float height,
                brls::Style style, brls::FrameContext *ctx) override {
            auto filesystems = filesystems_signature(this->context);
            if (filesystems != this->known_filesystems) {
                this->known_filesystems = std::move(filesystems);
                this->request_rebuild(true);
            }

            if (this->needs_rebuild) {
                auto rescan = this->pending_rescan;
                this->needs_rebuild = this->pending_rescan = false;
                this->rebuild(rescan);
                focus_default(this);
            }

            brls::Box::draw(vg, x, y, width, height, style, ctx);
        }

        void request_rebuild(bool rescan) {
            this->pending_rescan |= rescan;
            this->needs_rebuild = true;
        }

        void scan_directory() {
            if (!this->context.cur_fs && !this->context.filesystems.empty())
                this->context.cur_fs = this->context.filesystems.front();

            if (!this->context.cur_fs)
                return;

            if (this->path.empty()) {
                this->path = !this->context.cur_path.empty()
                    ? this->context.cur_path
                    : std::string(this->context.cur_fs->mount_name) + "/";
            }

            this->context.cur_path = fs::Path(this->path).base();
            this->entries.clear();

            auto *dir = opendir(this->path.c_str());
            if (!dir) {
                notify_error(this->context, "Failed to open directory", errno, Context::ErrorType::Io);
                return;
            }
            SW_SCOPEGUARD([dir] { closedir(dir); });

            auto *reent    = __syscall_getreent();
            auto *devoptab = devoptab_list[dir->dirData->device];

            std::array<char, 4096 + 1 + 0x20> fname = {};
            struct stat st = {};
            while (true) {
                fname.fill('\0');

                reent->deviceData = devoptab->deviceData;
                if (devoptab->dirnext_r(reent, dir->dirData, fname.data(), &st))
                    break;

                auto path = fs::Path(this->path) / fname.data();
                if (this->context.cur_fs->type == fs::Filesystem::Type::Recent)
                    path = path.internal().substr(1);

                auto name = std::string(path.filename()) + "##" + path.base();

                if (S_ISDIR(st.st_mode))
                    this->entries.emplace_back(fs::Node{fs::Node::Type::Directory, std::move(name)});
                else
                    this->entries.emplace_back(fs::Node{fs::Node::Type::File, std::move(name), std::size_t(st.st_size)});
            }

            if (this->context.cur_fs->type != fs::Filesystem::Type::Recent) {
                std::sort(this->entries.begin(), this->entries.end(), [](const fs::Node &lhs, const fs::Node &rhs) {
                    if (lhs.type != rhs.type)
                        return lhs.type < rhs.type;
                    return strcasecmp(lhs.name.c_str(), rhs.name.c_str()) < 0;
                });
            }
        }

        void rebuild(bool rescan) {
            if (rescan)
                this->scan_directory();

            clear_children(this);

            auto *page = make_page();
            this->addView(page);

            auto *header = new brls::Box(brls::Axis::ROW);
            header->setWidth(brls::View::AUTO);
            header->setHeight(brls::View::AUTO);
            header->setMarginBottom(18.0f);
            page->addView(header);

            for (auto &filesystem: this->context.filesystems) {
                auto *button = make_button(std::string(filesystem->name), [this, filesystem](brls::View *) {
                    this->context.cur_fs = filesystem;
                    this->path = std::string(filesystem->mount_name) + "/";
                    this->request_rebuild(true);
                    return true;
                });

                button->setWidth(150.0f);
                button->setMarginRight(10.0f);
                button->setMarginBottom(0.0f);
                header->addView(button);
            }

            auto *path_label = make_label(this->path.empty() ? "No filesystem mounted" : this->path, 20.0f);
            path_label->setMarginBottom(12.0f);
            page->addView(path_label);

            auto *list = new brls::Box(brls::Axis::COLUMN);
            list->setWidth(brls::View::AUTO);
            list->setHeight(brls::View::AUTO);

            if (!fs::Path(this->path).is_root()) {
                list->addView(make_button("..", [this](brls::View *) {
                    this->path = std::string(fs::Path(this->path).parent());
                    this->request_rebuild(true);
                    return true;
                }));
            }

            for (auto &entry: this->entries) {
                std::string name(filename_from_entry_name(entry.name));
                std::string row = (entry.type == fs::Node::Type::Directory) ? "[Folder] " : "[File] ";
                row += name;

                if (entry.type == fs::Node::Type::File) {
                    auto [size, suffix] = utils::to_human_size(entry.size);
                    char buf[64] = {};
                    std::snprintf(buf, sizeof(buf), "   %.2f%s", size, suffix.data());
                    row += buf;
                }

                list->addView(make_button(std::move(row), [this, entry](brls::View *) {
                    if (entry.type == fs::Node::Type::Directory) {
                        this->path = std::string(path_from_entry_name(entry.name));
                        this->request_rebuild(true);
                    } else {
                        this->context.cur_file = std::string(path_from_entry_name(entry.name));
                        brls::Application::notify("Opening media");
                        brls::Application::quit();
                    }
                    return true;
                }));
            }

            if (this->entries.empty())
                list->addView(make_label("This folder is empty.", 24.0f));

            page->addView(make_scroller(list));
        }

    private:
        Context &context;
        std::string path;
        std::vector<fs::Node> entries;
        std::string known_filesystems;
        bool needs_rebuild = false;
        bool pending_rescan = false;
};

class NetworkTab final: public brls::Box {
    public:
        explicit NetworkTab(Context &context): brls::Box(brls::Axis::COLUMN), context(context) {
            this->setWidth(brls::View::AUTO);
            this->setHeight(brls::View::AUTO);
            this->setGrow(1.0f);
            this->rebuild();
        }

    private:
        void draw(NVGcontext *vg, float x, float y, float width, float height,
                brls::Style style, brls::FrameContext *ctx) override {
            if (this->needs_rebuild) {
                this->needs_rebuild = false;
                this->rebuild();
                focus_default(this);
            }

            brls::Box::draw(vg, x, y, width, height, style, ctx);
        }

        void request_rebuild() {
            this->needs_rebuild = true;
        }

        void rebuild() {
            clear_children(this);

            auto *page = make_page();
            this->addView(page);

            auto *content = new brls::Box(brls::Axis::COLUMN);
            content->setWidth(brls::View::AUTO);
            content->setHeight(brls::View::AUTO);

            content->addView(make_button("New network source", [this](brls::View *) {
                auto info = std::make_unique<Context::NetworkFsInfo>();
                auto id = this->context.network_infos.size() + 1;

                info->protocol = fs::NetworkFilesystem::Protocol::Smb;
                info->fs_name  = std::string("network") + std::to_string(id);
                info->host     = "";
                info->port     = "445";
                info->share    = "";

                this->context.network_infos.emplace_back(std::move(info));
                this->request_rebuild();
                return true;
            }));

            for (std::size_t i = 0; i < this->context.network_infos.size(); ++i) {
                auto *info = this->context.network_infos[i].get();
                auto connected = info->fs && info->fs->connected();
                std::string title = std::string(info->fs_name.length() == 0 ? "<unnamed>" : info->fs_name.c_str());

                content->addView(make_section(title + (connected ? " (connected)" : " (disconnected)")));

                content->addView(make_button("Protocol: " + std::string(fs::NetworkFilesystem::protocol_name(info->protocol)), [this, info](brls::View *) {
                    if (info->fs)
                        this->context.unregister_network_fs(*info);

                    auto next = (static_cast<int>(info->protocol) + 1) %
                        static_cast<int>(fs::NetworkFilesystem::Protocol::ProtocolMax);
                    info->protocol = static_cast<fs::NetworkFilesystem::Protocol>(next);
                    this->request_rebuild();
                    return true;
                }));

                content->addView(make_button("Name: " + std::string(info->fs_name.c_str()), [this, info](brls::View *) {
                    if (info->fs)
                        this->context.unregister_network_fs(*info);
                    edit_text("Source name", info->fs_name, [this] { this->request_rebuild(); });
                    return true;
                }));

                content->addView(make_button("Host: " + std::string(info->host.c_str()), [this, info](brls::View *) {
                    edit_text("Host", info->host, [this] { this->request_rebuild(); });
                    return true;
                }));

                content->addView(make_button("Port: " + std::string(info->port.c_str()), [this, info](brls::View *) {
                    open_keyboard("Port", info->port.c_str(), info->port.capacity(), SwkbdType_NumPad,
                        [this, info](std::string value) {
                            info->port = value;
                            this->request_rebuild();
                        });
                    return true;
                }));

                content->addView(make_button("Share/path: " + std::string(info->share.c_str()), [this, info](brls::View *) {
                    edit_text("Share or path", info->share, [this] { this->request_rebuild(); });
                    return true;
                }));

                content->addView(make_button("Username: " + std::string(info->username.c_str()), [this, info](brls::View *) {
                    edit_text("Username", info->username, [this] { this->request_rebuild(); });
                    return true;
                }));

                content->addView(make_button("Password: " + std::string(info->password.length() == 0 ? "" : "********"), [this, info](brls::View *) {
                    edit_text("Password", info->password, [this] { this->request_rebuild(); });
                    return true;
                }));

                content->addView(make_button(connected ? "Disconnect" : "Connect", [this, info](brls::View *) {
                    int rc = 0;
                    if (info->fs && info->fs->connected())
                        rc = this->context.unregister_network_fs(*info);
                    else
                        rc = this->context.register_network_fs(*info);

                    if (rc)
                        notify_error(this->context, "Network source failed", rc, Context::ErrorType::Network);

                    this->request_rebuild();
                    return true;
                }));

                content->addView(make_button("Delete source", [this, i](brls::View *) {
                    auto &info = this->context.network_infos[i];
                    if (auto rc = this->context.unregister_network_fs(*info); rc)
                        notify_error(this->context, "Network source failed", rc, Context::ErrorType::Network);

                    this->context.network_infos.erase(this->context.network_infos.begin() + i);
                    this->request_rebuild();
                    return true;
                }));
            }

            page->addView(make_scroller(content));
        }

    private:
        Context &context;
        bool needs_rebuild = false;
};

class ConfigTab final: public brls::Box {
    public:
        explicit ConfigTab(Context &context): brls::Box(brls::Axis::COLUMN), context(context) {
            this->setWidth(brls::View::AUTO);
            this->setHeight(brls::View::AUTO);
            this->setGrow(1.0f);
            this->load_text();
            this->rebuild();
        }

    private:
        void draw(NVGcontext *vg, float x, float y, float width, float height,
                brls::Style style, brls::FrameContext *ctx) override {
            if (this->needs_rebuild) {
                this->needs_rebuild = false;
                this->rebuild();
                focus_default(this);
            }

            brls::Box::draw(vg, x, y, width, height, style, ctx);
        }

        const fs::Path &selected_path() const {
            return this->config_files[this->config_index];
        }

        std::string selected_name() const {
            return std::string(this->selected_path().filename());
        }

        void request_rebuild() {
            this->needs_rebuild = true;
        }

        bool create_empty_file() {
            auto *fp = std::fopen(this->selected_path().c_str(), "ab");
            if (!fp) {
                notify_error(this->context, "Failed to create configuration", errno, Context::ErrorType::Io);
                return false;
            }

            std::fclose(fp);
            return true;
        }

        void load_text() {
            auto *fp = std::fopen(this->selected_path().c_str(), "rb");
            if (!fp) {
                if (errno == ENOENT && this->create_empty_file()) {
                    this->config_text.clear();
                    this->has_unsaved_changes = false;
                    this->load_failed = false;
                } else {
                    notify_error(this->context, "Failed to read configuration", errno, Context::ErrorType::Io);
                    this->load_failed = true;
                }
                return;
            }
            SW_SCOPEGUARD([fp] { std::fclose(fp); });

            if (std::fseek(fp, 0, SEEK_END)) {
                notify_error(this->context, "Failed to read configuration", errno, Context::ErrorType::Io);
                this->load_failed = true;
                return;
            }

            auto size = std::ftell(fp);
            if (size < 0) {
                notify_error(this->context, "Failed to read configuration", errno, Context::ErrorType::Io);
                this->load_failed = true;
                return;
            }

            std::rewind(fp);
            this->config_text.resize(size);

            if (auto read = std::fread(this->config_text.data(), 1, this->config_text.size(), fp);
                    read != this->config_text.size()) {
                notify_error(this->context, "Failed to read configuration", errno, Context::ErrorType::Io);
                this->load_failed = true;
                return;
            }

            this->has_unsaved_changes = false;
            this->load_failed = false;
        }

        bool save_text() {
            auto *fp = std::fopen(this->selected_path().c_str(), "wb");
            if (!fp) {
                notify_error(this->context, "Failed to save configuration", errno, Context::ErrorType::Io);
                return false;
            }
            SW_SCOPEGUARD([fp] { std::fclose(fp); });

            if (auto written = std::fwrite(this->config_text.data(), 1, this->config_text.size(), fp);
                    written != this->config_text.size()) {
                notify_error(this->context, "Failed to save configuration", errno, Context::ErrorType::Io);
                return false;
            }

            this->has_unsaved_changes = false;
            brls::Application::notify("Configuration saved");
            return true;
        }

        void append_line(std::string line) {
            if (!this->config_text.empty() && this->config_text.back() != '\n')
                this->config_text += '\n';

            this->config_text += line;
            this->config_text += '\n';
            this->has_unsaved_changes = true;
            this->request_rebuild();
        }

        void remove_last_line() {
            if (this->config_text.empty())
                return;

            auto end = this->config_text.size();
            if (end > 0 && this->config_text[end - 1] == '\n')
                --end;

            auto pos = this->config_text.rfind('\n', end == 0 ? 0 : end - 1);
            if (pos == std::string::npos)
                this->config_text.clear();
            else
                this->config_text.erase(pos + 1);

            this->has_unsaved_changes = true;
            this->request_rebuild();
        }

        void rebuild() {
            clear_children(this);

            auto *page = make_page();
            this->addView(page);

            auto *content = new brls::Box(brls::Axis::COLUMN);
            content->setWidth(brls::View::AUTO);
            content->setHeight(brls::View::AUTO);

            content->addView(make_section("File"));
            content->addView(make_button("Selected: " + this->selected_name(), [this](brls::View *) {
                this->config_index = (this->config_index + 1) % this->config_files.size();
                this->load_text();
                this->request_rebuild();
                return true;
            }));

            if (this->has_unsaved_changes)
                content->addView(make_label("Unsaved changes", 22.0f, nvgRGB(210, 56, 56)));

            content->addView(make_button("Reload", [this](brls::View *) {
                this->load_text();
                this->request_rebuild();
                return true;
            }));

            content->addView(make_button("Save", [this](brls::View *) {
                this->save_text();
                this->request_rebuild();
                return true;
            }));

            if (!this->load_failed) {
                content->addView(make_section("Edit"));
                content->addView(make_button("Edit contents", [this](brls::View *) {
                    if (this->config_text.size() > ConfigTextLimit) {
                        brls::Application::notify("File is too large for the keyboard");
                        return true;
                    }

                    open_keyboard("Edit " + this->selected_name(), this->config_text, ConfigTextLimit,
                        SwkbdType_Normal, [this](std::string value) {
                            this->config_text = std::move(value);
                            this->has_unsaved_changes = true;
                            this->request_rebuild();
                        });
                    return true;
                }));

                content->addView(make_button("Append line", [this](brls::View *) {
                    open_keyboard("Append line", "", 255, SwkbdType_Normal, [this](std::string value) {
                        if (!value.empty())
                            this->append_line(std::move(value));
                    });
                    return true;
                }));

                content->addView(make_button("Remove last line", [this](brls::View *) {
                    this->remove_last_line();
                    return true;
                }));

                content->addView(make_button("Clear file", [this](brls::View *) {
                    this->config_text.clear();
                    this->has_unsaved_changes = true;
                    this->request_rebuild();
                    return true;
                }));

                content->addView(make_section("Preview"));

                auto preview = this->config_text.empty() ? std::string("<empty>") : this->config_text;
                if (preview.size() > 1400) {
                    preview.resize(1400);
                    preview += "\n...";
                }

                auto *preview_label = make_label(std::move(preview), 18.0f);
                preview_label->setVerticalAlign(brls::VerticalAlign::TOP);
                content->addView(preview_label);
            }

            page->addView(make_scroller(content));
        }

    private:
        Context &context;
        std::array<fs::Path, 2> config_files = {
            fs::Path(Context::AppDirectory) / "mpv.conf",
            fs::Path(Context::AppDirectory) / Context::SettingsFilename,
        };
        std::size_t config_index = 0;
        std::string config_text;
        bool has_unsaved_changes = false;
        bool load_failed = false;
        bool needs_rebuild = false;
};

class SettingsTab final: public brls::Box {
    public:
        explicit SettingsTab(Context &context): brls::Box(brls::Axis::COLUMN), context(context) {
            this->setWidth(brls::View::AUTO);
            this->setHeight(brls::View::AUTO);
            this->setGrow(1.0f);
            this->rebuild();
        }

    private:
        void draw(NVGcontext *vg, float x, float y, float width, float height,
                brls::Style style, brls::FrameContext *ctx) override {
            if (this->needs_rebuild) {
                this->needs_rebuild = false;
                this->rebuild();
                focus_default(this);
            }

            brls::Box::draw(vg, x, y, width, height, style, ctx);
        }

        void request_rebuild() {
            this->needs_rebuild = true;
        }

        void rebuild() {
            clear_children(this);

            auto *page = make_page();
            this->addView(page);

            auto *content = new brls::Box(brls::Axis::COLUMN);
            content->setWidth(brls::View::AUTO);
            content->setHeight(brls::View::AUTO);

            content->addView(make_section("Configuration"));
            content->addView(make_button("Read from file", [this](brls::View *) {
                if (this->context.read_from_file())
                    brls::Application::notify("Failed to read configuration");
                else
                    brls::Application::notify("Configuration loaded");
                this->request_rebuild();
                return true;
            }));
            content->addView(make_button("Save to file", [this](brls::View *) {
                if (this->context.write_to_file())
                    brls::Application::notify("Failed to save configuration");
                else
                    brls::Application::notify("Configuration saved");
                return true;
            }));

            content->addView(make_section("Playback"));
            this->add_toggle(content, "Use fast presentation", this->context.use_fast_presentation);
            this->add_toggle(content, "Disable screensaver", this->context.disable_screensaver);
            this->add_toggle(content, "Override screenshot button", this->context.override_screenshot_button);

            content->addView(make_section("System"));
            this->add_toggle(content, "Quit to home menu", this->context.quit_to_home_menu);

            content->addView(make_section("History"));
            content->addView(make_button("Max entries: " + std::to_string(this->context.history_size), [this](brls::View *) {
                edit_number("History entries", this->context.history_size, [this] { this->request_rebuild(); });
                return true;
            }));

            content->addView(make_button("Clear history", [this](brls::View *) {
                for (auto &filesystem: this->context.filesystems) {
                    if (filesystem->type == fs::Filesystem::Type::Recent)
                        reinterpret_cast<fs::RecentFs *>(filesystem.get())->clear();
                }
                brls::Application::notify("History cleared");
                return true;
            }));

            content->addView(make_button("Clear playback positions", [this](brls::View *) {
                auto path = fs::Path(Context::AppDirectory) / "watch_later";
                if (auto rc = fsdevDeleteDirectoryRecursively(path.c_str()); R_FAILED(rc))
                    this->context.set_error(EIO);
                else
                    brls::Application::notify("Playback positions cleared");
                return true;
            }));

            page->addView(make_scroller(content));
        }

        void add_toggle(brls::Box *content, std::string label, bool &value) {
            content->addView(make_button(label + ": " + bool_text(value), [this, &value](brls::View *) {
                value = !value;
                this->request_rebuild();
                return true;
            }));
        }

    private:
        Context &context;
        bool needs_rebuild = false;
};

class StorageTab final: public brls::Box {
    public:
        explicit StorageTab(Context &context): brls::Box(brls::Axis::COLUMN), context(context) {
            this->setWidth(brls::View::AUTO);
            this->setHeight(brls::View::AUTO);
            this->setGrow(1.0f);
            this->known_filesystems = filesystems_signature(this->context);
            this->known_usb_count = this->context.ums.get_devices().size();
            this->rebuild();
        }

    private:
        void draw(NVGcontext *vg, float x, float y, float width, float height,
                brls::Style style, brls::FrameContext *ctx) override {
            auto filesystems = filesystems_signature(this->context);
            auto usb_count   = this->context.ums.get_devices().size();
            if ((filesystems != this->known_filesystems) || (usb_count != this->known_usb_count)) {
                this->known_filesystems = std::move(filesystems);
                this->known_usb_count   = usb_count;
                this->request_rebuild();
            }

            if (this->needs_rebuild) {
                this->needs_rebuild = false;
                this->rebuild();
                focus_default(this);
            }

            brls::Box::draw(vg, x, y, width, height, style, ctx);
        }

        void request_rebuild() {
            this->needs_rebuild = true;
        }

        void rebuild() {
            clear_children(this);

            auto *page = make_page();
            this->addView(page);

            auto *content = new brls::Box(brls::Axis::COLUMN);
            content->setWidth(brls::View::AUTO);
            content->setHeight(brls::View::AUTO);

            content->addView(make_section("Mounted filesystems"));
            for (auto &filesystem: this->context.filesystems) {
                content->addView(make_label(std::string(filesystem->name) + "   " + std::string(filesystem->mount_name), 22.0f));
            }

            content->addView(make_section("USB devices"));
            auto &devices = this->context.ums.get_devices();
            if (devices.empty()) {
                content->addView(make_label("No USB device mounted.", 22.0f));
            } else {
                for (auto &device: devices) {
                    content->addView(make_button(std::string("Unmount ") + device.name, [this, device](brls::View *) {
                        std::erase_if(this->context.filesystems, [&device](const auto &filesystem) {
                            return device.mount_name == filesystem->mount_name;
                        });

                        if (!this->context.filesystems.empty())
                            this->context.cur_fs = this->context.filesystems.front();

                        this->context.ums.unmount_device(device);
                        this->request_rebuild();
                        return true;
                    }));
                }
            }

            page->addView(make_scroller(content));
        }

    private:
        Context &context;
        std::string known_filesystems;
        std::size_t known_usb_count = 0;
        bool needs_rebuild = false;
};

class AboutTab final: public brls::Box {
    public:
        explicit AboutTab(Context &context): brls::Box(brls::Axis::COLUMN), context(context) {
            this->setWidth(brls::View::AUTO);
            this->setHeight(brls::View::AUTO);
            this->setGrow(1.0f);
            this->rebuild();
        }

    private:
        void draw(NVGcontext *vg, float x, float y, float width, float height,
                brls::Style style, brls::FrameContext *ctx) override {
            if (this->needs_rebuild) {
                this->needs_rebuild = false;
                this->rebuild();
                focus_default(this);
            }

            brls::Box::draw(vg, x, y, width, height, style, ctx);
        }

        void rebuild() {
            clear_children(this);

            auto *page = make_page();
            this->addView(page);

            page->addView(make_label("SwitchWave", 34.0f));
            page->addView(make_label(std::string(app_version_str) + "   " + std::string(app_build_date_str), 20.0f, nvgRGB(105, 105, 115)));

            page->addView(make_section("Status"));
            if (this->context.last_error) {
                page->addView(make_label("Last error: " + std::to_string(this->context.last_error), 24.0f, nvgRGB(210, 56, 56)));
                page->addView(make_button("Dismiss error", [this](brls::View *) {
                    this->context.last_error = 0;
                    brls::Application::notify("Error dismissed");
                    this->needs_rebuild = true;
                    return true;
                }));
            } else {
                page->addView(make_label("No active error.", 24.0f));
            }

            page->addView(make_section("Frontend"));
            page->addView(make_label("Borealis", 22.0f));
        }

        Context &context;
        bool needs_rebuild = false;
};

class MainMenuActivity final: public brls::Activity {
    public:
        explicit MainMenuActivity(Context &context): context(context) { }

        brls::View *createContentView() override {
            auto *frame = new brls::TabFrame();
            frame->setTitle("SwitchWave");

            frame->addTab("Explorer", [this] { return new ExplorerTab(this->context); });
            frame->addTab("Network",  [this] { return new NetworkTab (this->context); });
            frame->addTab("Config",   [this] { return new ConfigTab  (this->context); });
            frame->addTab("Settings", [this] { return new SettingsTab(this->context); });
            frame->addTab("Storage",  [this] { return new StorageTab (this->context); });
            frame->addTab("About",    [this] { return new AboutTab   (this->context); });

            return frame;
        }

        void onContentAvailable() override {
            this->registerAction("Exit", brls::BUTTON_START, [this](brls::View *) {
                this->context.want_quit = true;
                brls::Application::quit();
                return true;
            });
        }

    private:
        Context &context;
};

} // namespace

int run_borealis_menu(Context &context, std::function<void()> frame_callback) {
    reset_borealis_static_state();

    brls::Logger::setLogLevel(brls::LogLevel::INFO);
    if (!brls::Application::init()) {
        std::printf("Failed to initialize Borealis\n");
        return 1;
    }

    brls::Application::createWindow("SwitchWave");
    apply_latitude_theme();
    brls::Application::setCommonFooter("SwitchWave");
    brls::Application::pushActivity(new MainMenuActivity(context));

    if (context.last_error)
        brls::Application::notify("Previous error is available in About");

    bool app_closed = false;
    while (true) {
        if (frame_callback)
            frame_callback();

        if (!brls::Application::mainLoop()) {
            app_closed = true;
            break;
        }

        if (!context.cur_file.empty() || context.want_quit)
            brls::Application::quit();
    }

    if (app_closed && context.cur_file.empty() && !context.want_quit)
        context.want_quit = true;

    reset_borealis_static_state();
    return 0;
}

} // namespace sw::ui
