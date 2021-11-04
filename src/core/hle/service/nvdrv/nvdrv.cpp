// SPDX-FileCopyrightText: 2021 yuzu emulator team and Skyline Team and Contributors
// (https://github.com/skyline-emu/)
// SPDX-License-Identifier: GPL-3.0-or-later Licensed under GPLv3
// or any later version Refer to the license.txt file included.

#include <bit>
#include <utility>

#include <fmt/format.h>
#include "core/core.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_writable_event.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/devices/nvdevice.h"
#include "core/hle/service/nvdrv/devices/nvdisp_disp0.h"
#include "core/hle/service/nvdrv/devices/nvhost_as_gpu.h"
#include "core/hle/service/nvdrv/devices/nvhost_ctrl.h"
#include "core/hle/service/nvdrv/devices/nvhost_ctrl_gpu.h"
#include "core/hle/service/nvdrv/devices/nvhost_gpu.h"
#include "core/hle/service/nvdrv/devices/nvhost_nvdec.h"
#include "core/hle/service/nvdrv/devices/nvhost_nvjpg.h"
#include "core/hle/service/nvdrv/devices/nvhost_vic.h"
#include "core/hle/service/nvdrv/devices/nvmap.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
#include "core/hle/service/nvdrv/nvmemp.h"
#include "core/hle/service/nvflinger/nvflinger.h"
#include "video_core/gpu.h"

namespace Service::Nvidia {

EventInterface::EventInterface(Module& module_) : module{module_} {
    events_mask = 0;
    for (u32 i = 0; i < MaxNvEvents; i++) {
        status[i] = EventState::Available;
        events[i] = nullptr;
        registered[i] = false;
    }
}

EventInterface::~EventInterface() {
    auto lk = Lock();
    for (u32 i = 0; i < MaxNvEvents; i++) {
        if (registered[i]) {
            module.service_context.CloseEvent(events[i]);
            events[i] = nullptr;
            registered[i] = false;
        }
    }
    for (auto* event : basic_events) {
        module.service_context.CloseEvent(event);
    }
}

std::unique_lock<std::mutex> EventInterface::Lock() {
    return std::unique_lock<std::mutex>(events_mutex);
}

void EventInterface::Signal(u32 event_id) {
    if (status[event_id].exchange(EventState::Signalling, std::memory_order_acq_rel) ==
        EventState::Waiting) {
        events[event_id]->GetWritableEvent().Signal();
    }
    status[event_id].store(EventState::Signalled, std::memory_order_release);
}

void EventInterface::Create(u32 event_id) {
    ASSERT(!events[event_id]);
    ASSERT(!registered[event_id]);
    ASSERT(!IsBeingUsed(event_id));
    events[event_id] =
        module.service_context.CreateEvent(fmt::format("NVDRV::NvEvent_{}", event_id));
    status[event_id] = EventState::Available;
    registered[event_id] = true;
    const u64 mask = 1ULL << event_id;
    fails[event_id] = 0;
    events_mask |= mask;
    assigned_syncpt[event_id] = 0;
}

void EventInterface::Free(u32 event_id) {
    ASSERT(events[event_id]);
    ASSERT(registered[event_id]);
    ASSERT(!IsBeingUsed(event_id));
    module.service_context.CloseEvent(events[event_id]);
    events[event_id] = nullptr;
    status[event_id] = EventState::Available;
    registered[event_id] = false;
    const u64 mask = ~(1ULL << event_id);
    events_mask &= mask;
}

u32 EventInterface::FindFreeEvent(u32 syncpoint_id) {
    u32 slot{MaxNvEvents};
    u32 free_slot{MaxNvEvents};
    for (u32 i = 0; i < MaxNvEvents; i++) {
        if (registered[i]) {
            if (!IsBeingUsed(i)) {
                slot = i;
                if (assigned_syncpt[i] == syncpoint_id) {
                    return slot;
                }
            }
        } else if (free_slot == MaxNvEvents) {
            free_slot = i;
        }
    }
    if (free_slot < MaxNvEvents) {
        Create(free_slot);
        return free_slot;
    }

    if (slot < MaxNvEvents) {
        return slot;
    }

    LOG_CRITICAL(Service_NVDRV, "Failed to allocate an event");
    return 0;
}

Kernel::KEvent* EventInterface::CreateNonCtrlEvent(std::string name) {
    Kernel::KEvent* new_event = module.service_context.CreateEvent(std::move(name));
    basic_events.push_back(new_event);
    return new_event;
}

void InstallInterfaces(SM::ServiceManager& service_manager, NVFlinger::NVFlinger& nvflinger,
                       Core::System& system) {
    auto module_ = std::make_shared<Module>(system);
    std::make_shared<NVDRV>(system, module_, "nvdrv")->InstallAsService(service_manager);
    std::make_shared<NVDRV>(system, module_, "nvdrv:a")->InstallAsService(service_manager);
    std::make_shared<NVDRV>(system, module_, "nvdrv:s")->InstallAsService(service_manager);
    std::make_shared<NVDRV>(system, module_, "nvdrv:t")->InstallAsService(service_manager);
    std::make_shared<NVMEMP>(system)->InstallAsService(service_manager);
    nvflinger.SetNVDrvInstance(module_);
}

Module::Module(Core::System& system)
    : service_context{system, "nvdrv"}, events_interface{*this}, container{system.GPU()} {
    devices["/dev/nvhost-as-gpu"] = std::make_shared<Devices::nvhost_as_gpu>(system, container);
    devices["/dev/nvhost-gpu"] =
        std::make_shared<Devices::nvhost_gpu>(system, events_interface, container);
    devices["/dev/nvhost-ctrl-gpu"] =
        std::make_shared<Devices::nvhost_ctrl_gpu>(system, events_interface);
    devices["/dev/nvmap"] = std::make_shared<Devices::nvmap>(system, container);
    devices["/dev/nvdisp_disp0"] = std::make_shared<Devices::nvdisp_disp0>(system, container);
    devices["/dev/nvhost-ctrl"] =
        std::make_shared<Devices::nvhost_ctrl>(system, events_interface, container);
    devices["/dev/nvhost-nvdec"] = std::make_shared<Devices::nvhost_nvdec>(system, container);
    devices["/dev/nvhost-nvjpg"] = std::make_shared<Devices::nvhost_nvjpg>(system);
    devices["/dev/nvhost-vic"] = std::make_shared<Devices::nvhost_vic>(system, container);
}

Module::~Module() = default;

NvResult Module::VerifyFD(DeviceFD fd) const {
    if (fd < 0) {
        LOG_ERROR(Service_NVDRV, "Invalid DeviceFD={}!", fd);
        return NvResult::InvalidState;
    }

    if (open_files.find(fd) == open_files.end()) {
        LOG_ERROR(Service_NVDRV, "Could not find DeviceFD={}!", fd);
        return NvResult::NotImplemented;
    }

    return NvResult::Success;
}

DeviceFD Module::Open(const std::string& device_name) {
    if (devices.find(device_name) == devices.end()) {
        LOG_ERROR(Service_NVDRV, "Trying to open unknown device {}", device_name);
        return INVALID_NVDRV_FD;
    }

    auto device = devices[device_name];
    const DeviceFD fd = next_fd++;

    device->OnOpen(fd);

    open_files[fd] = std::move(device);

    return fd;
}

NvResult Module::Ioctl1(DeviceFD fd, Ioctl command, const std::vector<u8>& input,
                        std::vector<u8>& output) {
    if (fd < 0) {
        LOG_ERROR(Service_NVDRV, "Invalid DeviceFD={}!", fd);
        return NvResult::InvalidState;
    }

    const auto itr = open_files.find(fd);

    if (itr == open_files.end()) {
        LOG_ERROR(Service_NVDRV, "Could not find DeviceFD={}!", fd);
        return NvResult::NotImplemented;
    }

    return itr->second->Ioctl1(fd, command, input, output);
}

NvResult Module::Ioctl2(DeviceFD fd, Ioctl command, const std::vector<u8>& input,
                        const std::vector<u8>& inline_input, std::vector<u8>& output) {
    if (fd < 0) {
        LOG_ERROR(Service_NVDRV, "Invalid DeviceFD={}!", fd);
        return NvResult::InvalidState;
    }

    const auto itr = open_files.find(fd);

    if (itr == open_files.end()) {
        LOG_ERROR(Service_NVDRV, "Could not find DeviceFD={}!", fd);
        return NvResult::NotImplemented;
    }

    return itr->second->Ioctl2(fd, command, input, inline_input, output);
}

NvResult Module::Ioctl3(DeviceFD fd, Ioctl command, const std::vector<u8>& input,
                        std::vector<u8>& output, std::vector<u8>& inline_output) {
    if (fd < 0) {
        LOG_ERROR(Service_NVDRV, "Invalid DeviceFD={}!", fd);
        return NvResult::InvalidState;
    }

    const auto itr = open_files.find(fd);

    if (itr == open_files.end()) {
        LOG_ERROR(Service_NVDRV, "Could not find DeviceFD={}!", fd);
        return NvResult::NotImplemented;
    }

    return itr->second->Ioctl3(fd, command, input, output, inline_output);
}

NvResult Module::Close(DeviceFD fd) {
    if (fd < 0) {
        LOG_ERROR(Service_NVDRV, "Invalid DeviceFD={}!", fd);
        return NvResult::InvalidState;
    }

    const auto itr = open_files.find(fd);

    if (itr == open_files.end()) {
        LOG_ERROR(Service_NVDRV, "Could not find DeviceFD={}!", fd);
        return NvResult::NotImplemented;
    }

    itr->second->OnClose(fd);

    open_files.erase(itr);

    return NvResult::Success;
}

void Module::SignalSyncpt(const u32 syncpoint_id, const u32 value) {
    const u32 max = MaxNvEvents - std::countl_zero(events_interface.events_mask);
    const u32 min = std::countr_zero(events_interface.events_mask);
    for (u32 i = min; i < max; i++) {
        if (events_interface.assigned_syncpt[i] == syncpoint_id &&
            events_interface.assigned_value[i] == value) {
            events_interface.Signal(i);
        }
    }
}

NvResult Module::QueryEvent(DeviceFD fd, u32 event_id, Kernel::KEvent*& event) {
    if (fd < 0) {
        LOG_ERROR(Service_NVDRV, "Invalid DeviceFD={}!", fd);
        return NvResult::InvalidState;
    }

    const auto itr = open_files.find(fd);

    if (itr == open_files.end()) {
        LOG_ERROR(Service_NVDRV, "Could not find DeviceFD={}!", fd);
        return NvResult::NotImplemented;
    }

    event = itr->second->QueryEvent(event_id);
    if (!event) {
        return NvResult::BadParameter;
    }
    return NvResult::Success;
}

} // namespace Service::Nvidia
