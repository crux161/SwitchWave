#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <usbhsfs.h>

namespace sw::fs {

class UmsController {
    public:
        struct Device {
            UsbHsFsDeviceFileSystemType type;
            std::int32_t intf_id;
            std::string name, mount_name;

            bool operator==(const Device &) const = default;
        };

        using DevicesChangedCallback = void(*)(const std::vector<Device> &, void *);

    public:
        Result initialize() {
            usbHsFsSetFileSystemMountFlags(UsbHsFsMountFlags_ReadOnly);

            if (auto rc = usbHsFsInitialize(0); R_FAILED(rc))
                return rc;

            usbHsFsSetPopulateCallback(UmsController::usbhsfs_populate_cb, this);
            this->refresh_devices(false);

            return 0;
        }

        void finalize() {
            usbHsFsSetPopulateCallback(nullptr, nullptr);
            this->set_devices_changed_callback(nullptr, nullptr);

            auto devices = this->get_devices();
            for (auto &dev: devices)
                this->unmount_device(dev);
            usbHsFsExit();
        }

        void set_devices_changed_callback(DevicesChangedCallback cb, void *user = nullptr) {
            std::vector<Device> devices;
            DevicesChangedCallback cb_to_call = nullptr;
            void *user_to_call = nullptr;

            {
                auto lk = std::scoped_lock(this->devices_mtx);
                this->devices_changed_cb = cb, this->devices_changed_user = user;

                if (this->devices_changed_cb) {
                    devices = this->devices;
                    cb_to_call = this->devices_changed_cb;
                    user_to_call = this->devices_changed_user;
                }
            }

            if (cb_to_call)
                cb_to_call(devices, user_to_call);
        }

        std::vector<Device> get_devices() const {
            auto lk = std::scoped_lock(this->devices_mtx);
            return this->devices;
        }

        void refresh_devices(bool notify_changed = true) {
            auto device_count = usbHsFsGetMountedDeviceCount();

            std::vector<UsbHsFsDevice> devices(device_count);
            if (device_count) {
                device_count = usbHsFsListMountedDevices(devices.data(), devices.size());
                if (device_count > devices.size())
                    device_count = devices.size();
            }

            this->populate_devices(devices.data(), device_count, notify_changed);
        }

        bool unmount_device(const Device &dev) {
            UsbHsFsDevice d = {
                .usb_if_id = dev.intf_id,
            };

            {
                auto lk = std::scoped_lock(this->devices_mtx);
                std::erase_if(this->devices, [&dev](const auto &d) {
                    return d.mount_name == dev.mount_name;
                });
            }

            return usbHsFsUnmountDevice(&d, true);
        }

    private:
        static void usbhsfs_populate_cb(const UsbHsFsDevice *devices, u32 device_count, void *user_data) {
            auto *self = static_cast<UmsController *>(user_data);
            self->populate_devices(devices, device_count, true);
        }

        void populate_devices(const UsbHsFsDevice *devices, u32 device_count, bool notify_changed) {
            std::vector<Device> new_devices;
            new_devices.reserve(device_count);
            DevicesChangedCallback cb_to_call = nullptr;
            void *user_to_call = nullptr;

            for (u32 i = 0; i < device_count; ++i) {
                auto &d = devices[i];

                std::string name;
                if (auto sv = std::string_view(d.product_name); !sv.empty())
                    name = sv;
                else if (sv = std::string_view(d.manufacturer); !sv.empty())
                    name = sv;
                else if (sv = std::string_view(d.serial_number); !sv.empty())
                    name = sv;
                else
                    name = "Unnamed device";

                new_devices.emplace_back(UsbHsFsDeviceFileSystemType(d.fs_type), d.usb_if_id, std::move(name), d.name);
            }

            {
                auto lk = std::scoped_lock(this->devices_mtx);

                if (new_devices == this->devices)
                    return;

                this->devices = std::move(new_devices);
                new_devices = this->devices;

                if (notify_changed) {
                    cb_to_call = this->devices_changed_cb;
                    user_to_call = this->devices_changed_user;
                }
            }

            if (cb_to_call)
                cb_to_call(new_devices, user_to_call);
        }

    private:
        UEvent *status_event = nullptr;
        mutable std::mutex devices_mtx;
        std::vector<Device> devices;

        DevicesChangedCallback devices_changed_cb = nullptr;
        void *devices_changed_user = nullptr;
};

} // namespace sw::fs
