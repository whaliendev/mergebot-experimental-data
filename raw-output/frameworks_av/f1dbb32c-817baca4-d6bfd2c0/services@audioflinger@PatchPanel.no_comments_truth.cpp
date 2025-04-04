#define LOG_TAG "AudioFlinger::PatchPanel"
#include "PatchPanel.h"
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
status_t PatchPanel::createAudioPatch(const struct audio_patch* patch,
                                   audio_patch_handle_t *handle,
                                   bool endpointPatch)
 NO_THREAD_SAFETY_ANALYSIS
{
    if (handle == NULL || patch == NULL) {
        return BAD_VALUE;
    }
    ALOGV("%s() num_sources %d num_sinks %d handle %d",
            __func__, patch->num_sources, patch->num_sinks, *handle);
    status_t status = NO_ERROR;
    audio_patch_handle_t halHandle = AUDIO_PATCH_HANDLE_NONE;
    if (!audio_patch_is_valid(patch) || (patch->num_sinks == 0 && patch->num_sources != 2)) {
        return BAD_VALUE;
    }
    if (patch->num_sources > 2) {
        return INVALID_OPERATION;
    }
    if (*handle != AUDIO_PATCH_HANDLE_NONE) {
        auto iter = mPatches.find(*handle);
        if (iter != mPatches.end()) {
            ALOGV("%s() removing patch handle %d", __func__, *handle);
            Patch &removedPatch = iter->second;
            if (removedPatch.isSoftware()) {
                removedPatch.clearConnections(this);
            }
            if (removedPatch.mHalHandle != AUDIO_PATCH_HANDLE_NONE) {
                audio_module_handle_t hwModule = AUDIO_MODULE_HANDLE_NONE;
                const struct audio_patch &oldPatch = removedPatch.mAudioPatch;
                if (oldPatch.sources[0].type == AUDIO_PORT_TYPE_DEVICE &&
                        (patch->sources[0].type != AUDIO_PORT_TYPE_DEVICE ||
                                oldPatch.sources[0].ext.device.hw_module !=
                                patch->sources[0].ext.device.hw_module)) {
                    hwModule = oldPatch.sources[0].ext.device.hw_module;
                } else if (patch->num_sinks == 0 ||
                        (oldPatch.sinks[0].type == AUDIO_PORT_TYPE_DEVICE &&
                                (patch->sinks[0].type != AUDIO_PORT_TYPE_DEVICE ||
                                        oldPatch.sinks[0].ext.device.hw_module !=
                                        patch->sinks[0].ext.device.hw_module))) {
                    hwModule = oldPatch.sinks[0].ext.device.hw_module;
                }
                sp<DeviceHalInterface> hwDevice = findHwDeviceByModule(hwModule);
                if (hwDevice != 0) {
                    hwDevice->releaseAudioPatch(removedPatch.mHalHandle);
                }
                halHandle = removedPatch.mHalHandle;
            }
            erasePatch(*handle);
        }
    }
    Patch newPatch{*patch, endpointPatch};
    audio_module_handle_t insertedModule = AUDIO_MODULE_HANDLE_NONE;
    switch (patch->sources[0].type) {
        case AUDIO_PORT_TYPE_DEVICE: {
            audio_module_handle_t srcModule = patch->sources[0].ext.device.hw_module;
            AudioHwDevice *audioHwDevice = findAudioHwDeviceByModule(srcModule);
            if (!audioHwDevice) {
                status = BAD_VALUE;
                goto exit;
            }
            for (unsigned int i = 0; i < patch->num_sinks; i++) {
                if ((patch->sinks[i].type == AUDIO_PORT_TYPE_MIX ||
                                (patch->sinks[i].type == AUDIO_PORT_TYPE_DEVICE &&
                                        patch->sinks[i].ext.device.hw_module != srcModule)) &&
                        patch->num_sinks > 1) {
                    ALOGW("%s() multiple sinks for mix or across modules not supported", __func__);
                    status = INVALID_OPERATION;
                    goto exit;
                }
                if (patch->sinks[i].type != patch->sinks[0].type) {
                    ALOGW("%s() different sink types in same patch not supported", __func__);
                    status = BAD_VALUE;
                    goto exit;
                }
            }
            if ((patch->num_sources == 2) ||
                ((patch->sinks[0].type == AUDIO_PORT_TYPE_DEVICE) &&
                 ((patch->sinks[0].ext.device.hw_module != srcModule) ||
                  !audioHwDevice->supportsAudioPatches()))) {
                audio_devices_t outputDevice = patch->sinks[0].ext.device.type;
                String8 outputDeviceAddress = String8(patch->sinks[0].ext.device.address);
                if (patch->num_sources == 2) {
                    if (patch->sources[1].type != AUDIO_PORT_TYPE_MIX ||
                            (patch->num_sinks != 0 && patch->sinks[0].ext.device.hw_module !=
                                    patch->sources[1].ext.mix.hw_module)) {
                        ALOGW("%s() invalid source combination", __func__);
                        status = INVALID_OPERATION;
                        goto exit;
                    }
                    const sp<IAfThreadBase> thread = mAfPatchPanelCallback->checkPlaybackThread_l(
                            patch->sources[1].ext.mix.handle);
                    if (thread == 0) {
                        ALOGW("%s() cannot get playback thread", __func__);
                        status = INVALID_OPERATION;
                        goto exit;
                    }
                    newPatch.mPlayback.setThread(
                            thread->asIAfPlaybackThread().get(), false );
                } else {
                    audio_config_t config = AUDIO_CONFIG_INITIALIZER;
                    audio_config_base_t mixerConfig = AUDIO_CONFIG_BASE_INITIALIZER;
                    audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
                    audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE;
                    if (patch->sinks[0].config_mask & AUDIO_PORT_CONFIG_SAMPLE_RATE) {
                        config.sample_rate = patch->sinks[0].sample_rate;
                    }
                    if (patch->sinks[0].config_mask & AUDIO_PORT_CONFIG_CHANNEL_MASK) {
                        config.channel_mask = patch->sinks[0].channel_mask;
                    }
                    if (patch->sinks[0].config_mask & AUDIO_PORT_CONFIG_FORMAT) {
                        config.format = patch->sinks[0].format;
                    }
                    if (patch->sinks[0].config_mask & AUDIO_PORT_CONFIG_FLAGS) {
                        flags = patch->sinks[0].flags.output;
                    }
                    const sp<IAfThreadBase> thread = mAfPatchPanelCallback->openOutput_l(
                                                            patch->sinks[0].ext.device.hw_module,
                                                            &output,
                                                            &config,
                                                            &mixerConfig,
                                                            outputDevice,
                                                            outputDeviceAddress,
                                                            flags);
                    ALOGV("mAfPatchPanelCallback->openOutput_l() returned %p", thread.get());
                    if (thread == 0) {
                        status = NO_MEMORY;
                        goto exit;
                    }
                    newPatch.mPlayback.setThread(thread->asIAfPlaybackThread().get());
                }
                audio_devices_t device = patch->sources[0].ext.device.type;
                String8 address = String8(patch->sources[0].ext.device.address);
                audio_config_t config = AUDIO_CONFIG_INITIALIZER;
                if (patch->sources[0].config_mask & AUDIO_PORT_CONFIG_SAMPLE_RATE) {
                    config.sample_rate = patch->sources[0].sample_rate;
                } else {
                    config.sample_rate = newPatch.mPlayback.thread()->sampleRate();
                }
                if (patch->sources[0].config_mask & AUDIO_PORT_CONFIG_CHANNEL_MASK) {
                    config.channel_mask = patch->sources[0].channel_mask;
                } else {
                    config.channel_mask = audio_channel_in_mask_from_count(
                            newPatch.mPlayback.thread()->channelCount());
                }
                if (patch->sources[0].config_mask & AUDIO_PORT_CONFIG_FORMAT) {
                    config.format = patch->sources[0].format;
                } else {
                    config.format = newPatch.mPlayback.thread()->format();
                }
                audio_input_flags_t flags =
                        patch->sources[0].config_mask & AUDIO_PORT_CONFIG_FLAGS ?
                        patch->sources[0].flags.input : AUDIO_INPUT_FLAG_NONE;
                audio_io_handle_t input = AUDIO_IO_HANDLE_NONE;
                audio_source_t source = AUDIO_SOURCE_MIC;
                if (patch->num_sources == 2
                        && patch->sources[1].ext.mix.usecase.stream
                                == AUDIO_STREAM_VOICE_CALL) {
                    source = AUDIO_SOURCE_VOICE_COMMUNICATION;
                }
                const sp<IAfThreadBase> thread = mAfPatchPanelCallback->openInput_l(srcModule,
                                                                    &input,
                                                                    &config,
                                                                    device,
                                                                    address,
                                                                    source,
                                                                    flags,
                                                                    outputDevice,
                                                                    outputDeviceAddress);
                ALOGV("mAfPatchPanelCallback->openInput_l() returned %p inChannelMask %08x",
                      thread.get(), config.channel_mask);
                if (thread == 0) {
                    status = NO_MEMORY;
                    goto exit;
                }
                newPatch.mRecord.setThread(thread->asIAfRecordThread().get());
                status = newPatch.createConnections(this);
                if (status != NO_ERROR) {
                    goto exit;
                }
                if (audioHwDevice->isInsert()) {
                    insertedModule = audioHwDevice->handle();
                }
            } else {
                if (patch->sinks[0].type == AUDIO_PORT_TYPE_MIX) {
                    sp<IAfThreadBase> thread = mAfPatchPanelCallback->checkRecordThread_l(
                                                              patch->sinks[0].ext.mix.handle);
                    if (thread == 0) {
                        thread = mAfPatchPanelCallback->checkMmapThread_l(
                                patch->sinks[0].ext.mix.handle);
                        if (thread == 0) {
                            ALOGW("%s() bad capture I/O handle %d",
                                    __func__, patch->sinks[0].ext.mix.handle);
                            status = BAD_VALUE;
                            goto exit;
                        }
                    }
                    mAfPatchPanelCallback->unlock();
                    status = thread->sendCreateAudioPatchConfigEvent(patch, &halHandle);
                    mAfPatchPanelCallback->lock();
                    if (status == NO_ERROR) {
                        newPatch.setThread(thread);
                    }
                    for (auto& iter : mPatches) {
                        if (iter.second.mAudioPatch.sinks[0].ext.mix.handle == thread->id()) {
                            erasePatch(iter.first);
                            break;
                        }
                    }
                } else {
                    sp<DeviceHalInterface> hwDevice = audioHwDevice->hwDevice();
                    status = hwDevice->createAudioPatch(patch->num_sources,
                                                        patch->sources,
                                                        patch->num_sinks,
                                                        patch->sinks,
                                                        &halHandle);
                    if (status == INVALID_OPERATION) goto exit;
                }
            }
        } break;
        case AUDIO_PORT_TYPE_MIX: {
            audio_module_handle_t srcModule = patch->sources[0].ext.mix.hw_module;
            ssize_t index = mAfPatchPanelCallback->getAudioHwDevs_l().indexOfKey(srcModule);
            if (index < 0) {
                ALOGW("%s() bad src hw module %d", __func__, srcModule);
                status = BAD_VALUE;
                goto exit;
            }
            DeviceDescriptorBaseVector devices;
            for (unsigned int i = 0; i < patch->num_sinks; i++) {
                if (patch->sinks[i].type != AUDIO_PORT_TYPE_DEVICE) {
                    ALOGW("%s() invalid sink type %d for mix source",
                            __func__, patch->sinks[i].type);
                    status = BAD_VALUE;
                    goto exit;
                }
                if (patch->sinks[i].ext.device.hw_module != srcModule) {
                    status = BAD_VALUE;
                    goto exit;
                }
                sp<DeviceDescriptorBase> device = new DeviceDescriptorBase(
                        patch->sinks[i].ext.device.type);
                device->setAddress(patch->sinks[i].ext.device.address);
                device->applyAudioPortConfig(&patch->sinks[i]);
                devices.push_back(device);
            }
            sp<IAfThreadBase> thread = mAfPatchPanelCallback->checkPlaybackThread_l(
                    patch->sources[0].ext.mix.handle);
            if (thread == 0) {
                thread = mAfPatchPanelCallback->checkMmapThread_l(
                        patch->sources[0].ext.mix.handle);
                if (thread == 0) {
                    ALOGW("%s() bad playback I/O handle %d",
                            __func__, patch->sources[0].ext.mix.handle);
                    status = BAD_VALUE;
                    goto exit;
                }
            }
            if (thread == mAfPatchPanelCallback->primaryPlaybackThread_l()) {
                mAfPatchPanelCallback->updateOutDevicesForRecordThreads_l(devices);
            }
            mAfPatchPanelCallback->unlock();
            status = thread->sendCreateAudioPatchConfigEvent(patch, &halHandle);
            mAfPatchPanelCallback->lock();
            if (status == NO_ERROR) {
                newPatch.setThread(thread);
            }
            if (!endpointPatch) {
                for (auto& iter : mPatches) {
                    if (iter.second.mAudioPatch.sources[0].ext.mix.handle == thread->id() &&
                            !iter.second.mIsEndpointPatch) {
                        erasePatch(iter.first);
                        break;
                    }
                }
            }
        } break;
        default:
            status = BAD_VALUE;
            goto exit;
    }
exit:
    ALOGV("%s() status %d", __func__, status);
    if (status == NO_ERROR) {
        *handle = static_cast<audio_patch_handle_t>(
                mAfPatchPanelCallback->nextUniqueId(AUDIO_UNIQUE_ID_USE_PATCH));
        newPatch.mHalHandle = halHandle;
        mAfPatchPanelCallback->getPatchCommandThread()->createAudioPatch(*handle, newPatch);
        if (insertedModule != AUDIO_MODULE_HANDLE_NONE) {
            addSoftwarePatchToInsertedModules(insertedModule, *handle, &newPatch.mAudioPatch);
        }
        mPatches.insert(std::make_pair(*handle, std::move(newPatch)));
    } else {
        newPatch.clearConnections(this);
    }
    return status;
}
PatchPanel::Patch::~Patch()
{
    ALOGE_IF(isSoftware(), "Software patch connections leaked %d %d",
            mRecord.handle(), mPlayback.handle());
}
status_t PatchPanel::Patch::createConnections(const sp<IAfPatchPanel>& panel)
{
    status_t status = panel->createAudioPatch(
            PatchBuilder().addSource(mAudioPatch.sources[0]).
                addSink(mRecord.thread(), { .source = AUDIO_SOURCE_MIC }).patch(),
            mRecord.handlePtr(),
            true );
    if (status != NO_ERROR) {
        *mRecord.handlePtr() = AUDIO_PATCH_HANDLE_NONE;
        return status;
    }
    if (mAudioPatch.num_sinks != 0) {
        status = panel->createAudioPatch(
                PatchBuilder().addSource(mPlayback.thread()).addSink(mAudioPatch.sinks[0]).patch(),
                mPlayback.handlePtr(),
                true );
        if (status != NO_ERROR) {
            *mPlayback.handlePtr() = AUDIO_PATCH_HANDLE_NONE;
            return status;
        }
    } else {
        *mPlayback.handlePtr() = AUDIO_PATCH_HANDLE_NONE;
    }
    uint32_t channelCount = mPlayback.thread()->channelCount();
    audio_channel_mask_t inChannelMask = audio_channel_in_mask_from_count(channelCount);
    audio_channel_mask_t outChannelMask = mPlayback.thread()->channelMask();
    uint32_t sampleRate = mPlayback.thread()->sampleRate();
    audio_format_t format = mPlayback.thread()->format();
    audio_format_t inputFormat = mRecord.thread()->format();
    if (!audio_is_linear_pcm(inputFormat)) {
        format = inputFormat;
    }
    audio_input_flags_t inputFlags = mAudioPatch.sources[0].config_mask & AUDIO_PORT_CONFIG_FLAGS ?
            mAudioPatch.sources[0].flags.input : AUDIO_INPUT_FLAG_NONE;
    if (sampleRate == mRecord.thread()->sampleRate() &&
            inChannelMask == mRecord.thread()->channelMask() &&
            mRecord.thread()->fastTrackAvailable() &&
            mRecord.thread()->hasFastCapture()) {
        inputFlags = (audio_input_flags_t) (inputFlags | AUDIO_INPUT_FLAG_FAST);
    } else {
        inputFlags = (audio_input_flags_t) (inputFlags & ~AUDIO_INPUT_FLAG_FAST);
    }
    audio_output_flags_t outputFlags = mAudioPatch.sinks[0].config_mask & AUDIO_PORT_CONFIG_FLAGS ?
            mAudioPatch.sinks[0].flags.output : AUDIO_OUTPUT_FLAG_NONE;
    audio_stream_type_t streamType = AUDIO_STREAM_PATCH;
    audio_source_t source = AUDIO_SOURCE_DEFAULT;
    if (mAudioPatch.num_sources == 2 && mAudioPatch.sources[1].type == AUDIO_PORT_TYPE_MIX) {
        streamType = mAudioPatch.sources[1].ext.mix.usecase.stream;
        if (streamType == AUDIO_STREAM_VOICE_CALL) {
            source = AUDIO_SOURCE_VOICE_COMMUNICATION;
        }
    }
    if (mPlayback.thread()->hasFastMixer()) {
        outputFlags = (audio_output_flags_t) (outputFlags | AUDIO_OUTPUT_FLAG_FAST);
    } else {
        outputFlags = (audio_output_flags_t) (outputFlags & ~AUDIO_OUTPUT_FLAG_FAST);
    }
    sp<IAfPatchRecord> tempRecordTrack;
    const bool usePassthruPatchRecord =
            (inputFlags & AUDIO_INPUT_FLAG_DIRECT) && (outputFlags & AUDIO_OUTPUT_FLAG_DIRECT);
    const size_t playbackFrameCount = mPlayback.thread()->frameCount();
    const size_t recordFrameCount = mRecord.thread()->frameCount();
    size_t frameCount = 0;
    if (usePassthruPatchRecord) {
        frameCount = std::max(playbackFrameCount, recordFrameCount);
        ALOGV("%s() playframeCount %zu recordFrameCount %zu frameCount %zu",
            __func__, playbackFrameCount, recordFrameCount, frameCount);
        tempRecordTrack = IAfPatchRecord::createPassThru(
                                                 mRecord.thread().get(),
                                                 sampleRate,
                                                 inChannelMask,
                                                 format,
                                                 frameCount,
                                                 inputFlags,
                                                 source);
    } else {
        int playbackShift = __builtin_ctz(playbackFrameCount);
        int shift = __builtin_ctz(recordFrameCount);
        if (playbackShift < shift) {
            shift = playbackShift;
        }
        frameCount = (playbackFrameCount * recordFrameCount) >> shift;
        ALOGV("%s() playframeCount %zu recordFrameCount %zu frameCount %zu",
            __func__, playbackFrameCount, recordFrameCount, frameCount);
        tempRecordTrack = IAfPatchRecord::create(
                                                 mRecord.thread().get(),
                                                 sampleRate,
                                                 inChannelMask,
                                                 format,
                                                 frameCount,
                                                 nullptr,
                                                 (size_t)0 ,
                                                 inputFlags,
                                                 {} ,
                                                 source);
    }
    status = mRecord.checkTrack(tempRecordTrack.get());
    if (status != NO_ERROR) {
        return status;
    }
    const bool isFmBridge = mAudioPatch.sources[0].ext.device.type == AUDIO_DEVICE_IN_FM_TUNER;
    const size_t frameCountToBeReady = isFmBridge && !usePassthruPatchRecord ? frameCount / 4 : 1;
    sp<IAfPatchTrack> tempPatchTrack = IAfPatchTrack::create(
                                           mPlayback.thread().get(),
                                           streamType,
                                           sampleRate,
                                           outChannelMask,
                                           format,
                                           frameCount,
                                           tempRecordTrack->buffer(),
                                           tempRecordTrack->bufferSize(),
                                           outputFlags,
                                           {} ,
                                           frameCountToBeReady);
    status = mPlayback.checkTrack(tempPatchTrack.get());
    if (status != NO_ERROR) {
        return status;
    }
    mRecord.setTrackAndPeer(tempRecordTrack, tempPatchTrack, !usePassthruPatchRecord);
    mPlayback.setTrackAndPeer(tempPatchTrack, tempRecordTrack, true );
    mRecord.track()->start(AudioSystem::SYNC_EVENT_NONE, AUDIO_SESSION_NONE);
    mPlayback.track()->start();
    return status;
}
void PatchPanel::Patch::clearConnections(const sp<IAfPatchPanel>& panel)
{
    ALOGV("%s() mRecord.handle %d mPlayback.handle %d",
            __func__, mRecord.handle(), mPlayback.handle());
    mRecord.stopTrack();
    mPlayback.stopTrack();
    mRecord.clearTrackPeer();
    mRecord.closeConnections(panel);
    mPlayback.closeConnections(panel);
}
status_t PatchPanel::Patch::getLatencyMs(double* latencyMs) const
{
    if (!isSoftware()) return INVALID_OPERATION;
    auto recordTrack = mRecord.const_track();
    if (recordTrack.get() == nullptr) return INVALID_OPERATION;
    auto playbackTrack = mPlayback.const_track();
    if (playbackTrack.get() == nullptr) return INVALID_OPERATION;
    if (audio_is_linear_pcm(recordTrack->format())) {
        double recordServerLatencyMs, playbackTrackLatencyMs;
        if (recordTrack->getServerLatencyMs(&recordServerLatencyMs) == OK
                && playbackTrack->getTrackLatencyMs(&playbackTrackLatencyMs) == OK) {
            *latencyMs = recordServerLatencyMs + playbackTrackLatencyMs;
            return OK;
        }
    }
    IAfTrack::FrameTime recordFT{}, playFT{};
    recordTrack->getKernelFrameTime(&recordFT);
    playbackTrack->getKernelFrameTime(&playFT);
    if (recordFT.timeNs > 0 && playFT.timeNs > 0) {
        const int64_t frameDiff = recordFT.frames - playFT.frames;
        const int64_t timeDiffNs = recordFT.timeNs - playFT.timeNs;
        constexpr int64_t maxValidTimeDiffNs = 200 * NANOS_PER_MILLISECOND;
        if (std::abs(timeDiffNs) < maxValidTimeDiffNs) {
            *latencyMs = frameDiff * 1e3 / recordTrack->sampleRate()
                   - timeDiffNs * 1e-6;
            return OK;
        }
    }
    return INVALID_OPERATION;
}
String8 PatchPanel::Patch::dump(audio_patch_handle_t myHandle) const
{
    String8 result = String8::format("Patch %d: %s (thread %p => thread %p)",
            myHandle, isSoftware() ? "Software bridge between" : "No software bridge",
            mRecord.const_thread().get(), mPlayback.const_thread().get());
    bool hasSinkDevice =
            mAudioPatch.num_sinks > 0 && mAudioPatch.sinks[0].type == AUDIO_PORT_TYPE_DEVICE;
    bool hasSourceDevice =
            mAudioPatch.num_sources > 0 && mAudioPatch.sources[0].type == AUDIO_PORT_TYPE_DEVICE;
    result.appendFormat(" thread %p %s (%d) first device type %08x", mThread.unsafe_get(),
            hasSinkDevice ? "num sinks" :
                (hasSourceDevice ? "num sources" : "no devices"),
            hasSinkDevice ? mAudioPatch.num_sinks :
                (hasSourceDevice ? mAudioPatch.num_sources : 0),
            hasSinkDevice ? mAudioPatch.sinks[0].ext.device.type :
                (hasSourceDevice ? mAudioPatch.sources[0].ext.device.type : 0));
    double latencyMs;
    if (getLatencyMs(&latencyMs) == OK) {
        result.appendFormat("  latency: %.2lf ms", latencyMs);
    }
    return result;
}
status_t PatchPanel::releaseAudioPatch(audio_patch_handle_t handle)
 NO_THREAD_SAFETY_ANALYSIS
 {
    ALOGV("%s handle %d", __func__, handle);
    status_t status = NO_ERROR;
    auto iter = mPatches.find(handle);
    if (iter == mPatches.end()) {
        return BAD_VALUE;
    }
    Patch &removedPatch = iter->second;
    const struct audio_patch &patch = removedPatch.mAudioPatch;
    const struct audio_port_config &src = patch.sources[0];
    switch (src.type) {
        case AUDIO_PORT_TYPE_DEVICE: {
            sp<DeviceHalInterface> hwDevice = findHwDeviceByModule(src.ext.device.hw_module);
            if (hwDevice == 0) {
                ALOGW("%s() bad src hw module %d", __func__, src.ext.device.hw_module);
                status = BAD_VALUE;
                break;
            }
            if (removedPatch.isSoftware()) {
                removedPatch.clearConnections(this);
                break;
            }
            if (patch.sinks[0].type == AUDIO_PORT_TYPE_MIX) {
                audio_io_handle_t ioHandle = patch.sinks[0].ext.mix.handle;
                sp<IAfThreadBase> thread = mAfPatchPanelCallback->checkRecordThread_l(ioHandle);
                if (thread == 0) {
                    thread = mAfPatchPanelCallback->checkMmapThread_l(ioHandle);
                    if (thread == 0) {
                        ALOGW("%s() bad capture I/O handle %d", __func__, ioHandle);
                        status = BAD_VALUE;
                        break;
                    }
                }
                mAfPatchPanelCallback->unlock();
                status = thread->sendReleaseAudioPatchConfigEvent(removedPatch.mHalHandle);
                mAfPatchPanelCallback->lock();
            } else {
                status = hwDevice->releaseAudioPatch(removedPatch.mHalHandle);
            }
        } break;
        case AUDIO_PORT_TYPE_MIX: {
            if (findHwDeviceByModule(src.ext.mix.hw_module) == 0) {
                ALOGW("%s() bad src hw module %d", __func__, src.ext.mix.hw_module);
                status = BAD_VALUE;
                break;
            }
            audio_io_handle_t ioHandle = src.ext.mix.handle;
            sp<IAfThreadBase> thread = mAfPatchPanelCallback->checkPlaybackThread_l(ioHandle);
            if (thread == 0) {
                thread = mAfPatchPanelCallback->checkMmapThread_l(ioHandle);
                if (thread == 0) {
                    ALOGW("%s() bad playback I/O handle %d", __func__, ioHandle);
                    status = BAD_VALUE;
                    break;
                }
            }
            mAfPatchPanelCallback->unlock();
            status = thread->sendReleaseAudioPatchConfigEvent(removedPatch.mHalHandle);
            mAfPatchPanelCallback->lock();
        } break;
        default:
            status = BAD_VALUE;
    }
    erasePatch(handle);
    return status;
}
void PatchPanel::erasePatch(audio_patch_handle_t handle) {
    mPatches.erase(handle);
    removeSoftwarePatchFromInsertedModules(handle);
    mAfPatchPanelCallback->getPatchCommandThread()->releaseAudioPatch(handle);
}
status_t PatchPanel::listAudioPatches(unsigned int* ,
                                  struct audio_patch *patches __unused)
{
    ALOGV(__func__);
    return NO_ERROR;
}
status_t PatchPanel::getDownstreamSoftwarePatches(
        audio_io_handle_t stream,
        std::vector<SoftwarePatch>* patches) const
{
    for (const auto& module : mInsertedModules) {
        if (module.second.streams.count(stream)) {
            for (const auto& patchHandle : module.second.sw_patches) {
                const auto& patch_iter = mPatches.find(patchHandle);
                if (patch_iter != mPatches.end()) {
                    const Patch &patch = patch_iter->second;
                    patches->emplace_back(sp<const IAfPatchPanel>::fromExisting(this),
                            patchHandle,
                            patch.mPlayback.const_thread()->id(),
                            patch.mRecord.const_thread()->id());
                } else {
                    ALOGE("Stale patch handle in the cache: %d", patchHandle);
                }
            }
            return OK;
        }
    }
    return BAD_VALUE;
}
void PatchPanel::notifyStreamOpened(
        AudioHwDevice *audioHwDevice, audio_io_handle_t stream, struct audio_patch *patch)
{
    if (audioHwDevice->isInsert()) {
        mInsertedModules[audioHwDevice->handle()].streams.insert(stream);
        if (patch != nullptr) {
            std::vector <SoftwarePatch> swPatches;
            getDownstreamSoftwarePatches(stream, &swPatches);
            if (swPatches.size() > 0) {
                auto iter = mPatches.find(swPatches[0].getPatchHandle());
                if (iter != mPatches.end()) {
                    *patch = iter->second.mAudioPatch;
                }
            }
        }
    }
}
void PatchPanel::notifyStreamClosed(audio_io_handle_t stream)
{
    for (auto& module : mInsertedModules) {
        module.second.streams.erase(stream);
    }
}
AudioHwDevice* PatchPanel::findAudioHwDeviceByModule(audio_module_handle_t module)
{
    if (module == AUDIO_MODULE_HANDLE_NONE) return nullptr;
    ssize_t index = mAfPatchPanelCallback->getAudioHwDevs_l().indexOfKey(module);
    if (index < 0) {
        ALOGW("%s() bad hw module %d", __func__, module);
        return nullptr;
    }
    return mAfPatchPanelCallback->getAudioHwDevs_l().valueAt(index);
}
sp<DeviceHalInterface> PatchPanel::findHwDeviceByModule(audio_module_handle_t module)
{
    AudioHwDevice *audioHwDevice = findAudioHwDeviceByModule(module);
    return audioHwDevice ? audioHwDevice->hwDevice() : nullptr;
}
void PatchPanel::addSoftwarePatchToInsertedModules(
        audio_module_handle_t module, audio_patch_handle_t handle,
        const struct audio_patch *patch)
{
    mInsertedModules[module].sw_patches.insert(handle);
    if (!mInsertedModules[module].streams.empty()) {
        mAfPatchPanelCallback->updateDownStreamPatches_l(patch, mInsertedModules[module].streams);
    }
}
void PatchPanel::removeSoftwarePatchFromInsertedModules(
        audio_patch_handle_t handle)
{
    for (auto& module : mInsertedModules) {
        module.second.sw_patches.erase(handle);
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
