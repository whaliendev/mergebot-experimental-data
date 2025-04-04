#define LOG_TAG "AudioFlinger::PatchPanel"
#include "Configuration.h"
#include "PatchPanel.h"
#include "AudioFlinger.h"
#include "PatchCommandThread.h"
#include <audio_utils/primitives.h>
#include <media/AudioParameter.h>
#include <media/AudioValidator.h>
#include <media/DeviceDescriptorBase.h>
#include <media/PatchBuilder.h>
#include <mediautils/ServiceUtilities.h>
#include <utils/Log.h>
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif
namespace android {
status_t AudioFlinger::listAudioPorts(unsigned int *num_ports, struct audio_port* ports) const
{
    Mutex::Autolock _l(mLock);
    return mPatchPanel->listAudioPorts(num_ports, ports);
}
status_t AudioFlinger::listAudioPatches(unsigned int* num_patches, struct audio_patch* patches) const
{
    Mutex::Autolock _l(mLock);
    return mPatchPanel->listAudioPatches(num_patches, patches);
}
sp<IAfPatchPanel> IAfPatchPanel::create(const sp<IAfPatchPanelCallback>& afPatchPanelCallback) {
    return sp<PatchPanel>::make(afPatchPanelCallback);
}
status_t SoftwarePatch::getLatencyMs_l(double* latencyMs) const {
    return mPatchPanel->getLatencyMs_l(mPatchHandle, latencyMs);
}
status_t PatchPanel::getLatencyMs_l(
        audio_patch_handle_t patchHandle, double* latencyMs) const
{
    const auto& iter = mPatches.find(patchHandle);
    if (iter != mPatches.end()) {
        return iter->second.getLatencyMs(latencyMs);
    } else {
        return BAD_VALUE;
    }
}
void PatchPanel::closeThreadInternal_l(const sp<IAfThreadBase>& thread) const
{
    if (const auto recordThread = thread->asIAfRecordThread();
            recordThread) {
        mAfPatchPanelCallback->closeThreadInternal_l(recordThread);
    } else if (const auto playbackThread = thread->asIAfPlaybackThread();
            playbackThread) {
        mAfPatchPanelCallback->closeThreadInternal_l(playbackThread);
    } else {
        LOG_ALWAYS_FATAL("%s: Endpoints only accept IAfPlayback and IAfRecord threads, "
                "invalid thread, id: %d  type: %d",
                __func__, thread->id(), thread->type());
    }
}
status_t PatchPanel::listAudioPorts(unsigned int* ,
                                struct audio_port *ports __unused)
{
    ALOGV(__func__);
    return NO_ERROR;
}
status_t PatchPanel::getAudioPort(struct audio_port_v7* port)
{
    if (port->type != AUDIO_PORT_TYPE_DEVICE) {
        return INVALID_OPERATION;
    }
    AudioHwDevice* hwDevice = findAudioHwDeviceByModule(port->ext.device.hw_module);
    if (hwDevice == nullptr) {
        ALOGW("%s cannot find hw module %d", __func__, port->ext.device.hw_module);
        return BAD_VALUE;
    }
    if (!hwDevice->supportsAudioPatches()) {
        return INVALID_OPERATION;
    }
    return hwDevice->getAudioPort(port);
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
PatchPanel::Patch::~Patch()
{
    ALOGE_IF(isSoftware(), "Software patch connections leaked %d %d",
            mRecord.handle(), mPlayback.handle());
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
void PatchPanel::dump(int fd) const
{
    String8 patchPanelDump;
    const char *indent = "  ";
    bool headerPrinted = false;
    for (const auto& iter : mPatches) {
        if (!headerPrinted) {
            patchPanelDump += "\nPatches:\n";
            headerPrinted = true;
        }
        patchPanelDump.appendFormat("%s%s\n", indent, iter.second.dump(iter.first).c_str());
    }
    headerPrinted = false;
    for (const auto& module : mInsertedModules) {
        if (!module.second.streams.empty() || !module.second.sw_patches.empty()) {
            if (!headerPrinted) {
                patchPanelDump += "\nTracked inserted modules:\n";
                headerPrinted = true;
            }
            String8 moduleDump = String8::format("Module %d: I/O handles: ", module.first);
            for (const auto& stream : module.second.streams) {
                moduleDump.appendFormat("%d ", stream);
            }
            moduleDump.append("; SW Patches: ");
            for (const auto& patch : module.second.sw_patches) {
                moduleDump.appendFormat("%d ", patch);
            }
            patchPanelDump.appendFormat("%s%s\n", indent, moduleDump.c_str());
        }
    }
    if (!patchPanelDump.isEmpty()) {
        write(fd, patchPanelDump.c_str(), patchPanelDump.size());
    }
}
}
