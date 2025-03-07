#define LOG_TAG "IAudioPolicyService"
#include <utils/Log.h>
#include <stdint.h>
#include <math.h>
#include <sys/types.h>
#include <binder/IPCThreadState.h>
#include <binder/Parcel.h>
#include <media/AudioEffect.h>
#include <media/IAudioPolicyService.h>
#include <mediautils/ServiceUtilities.h>
#include <mediautils/TimeCheck.h>
#include <system/audio.h>
namespace android {
enum {
  SET_DEVICE_CONNECTION_STATE = IBinder::FIRST_CALL_TRANSACTION,
  GET_DEVICE_CONNECTION_STATE,
  HANDLE_DEVICE_CONFIG_CHANGE,
  SET_PHONE_STATE,
  SET_RINGER_MODE,
  SET_FORCE_USE,
  GET_FORCE_USE,
  GET_OUTPUT,
  START_OUTPUT,
  STOP_OUTPUT,
  RELEASE_OUTPUT,
  GET_INPUT_FOR_ATTR,
  START_INPUT,
  STOP_INPUT,
  RELEASE_INPUT,
  INIT_STREAM_VOLUME,
  SET_STREAM_VOLUME,
  GET_STREAM_VOLUME,
  SET_VOLUME_ATTRIBUTES,
  GET_VOLUME_ATTRIBUTES,
  GET_MIN_VOLUME_FOR_ATTRIBUTES,
  GET_MAX_VOLUME_FOR_ATTRIBUTES,
  GET_STRATEGY_FOR_STREAM,
  GET_OUTPUT_FOR_EFFECT,
  REGISTER_EFFECT,
  UNREGISTER_EFFECT,
  IS_STREAM_ACTIVE,
  IS_SOURCE_ACTIVE,
  GET_DEVICES_FOR_STREAM,
  QUERY_DEFAULT_PRE_PROCESSING,
  SET_EFFECT_ENABLED,
  IS_STREAM_ACTIVE_REMOTELY,
  IS_OFFLOAD_SUPPORTED,
  IS_DIRECT_OUTPUT_SUPPORTED,
  LIST_AUDIO_PORTS,
  GET_AUDIO_PORT,
  CREATE_AUDIO_PATCH,
  RELEASE_AUDIO_PATCH,
  LIST_AUDIO_PATCHES,
  SET_AUDIO_PORT_CONFIG,
  REGISTER_CLIENT,
  GET_OUTPUT_FOR_ATTR,
  ACQUIRE_SOUNDTRIGGER_SESSION,
  RELEASE_SOUNDTRIGGER_SESSION,
  GET_PHONE_STATE,
  REGISTER_POLICY_MIXES,
  START_AUDIO_SOURCE,
  STOP_AUDIO_SOURCE,
  SET_AUDIO_PORT_CALLBACK_ENABLED,
  SET_AUDIO_VOLUME_GROUP_CALLBACK_ENABLED,
  SET_MASTER_MONO,
  GET_MASTER_MONO,
  GET_STREAM_VOLUME_DB,
  GET_SURROUND_FORMATS,
  SET_SURROUND_FORMAT_ENABLED,
  ADD_STREAM_DEFAULT_EFFECT,
  REMOVE_STREAM_DEFAULT_EFFECT,
  ADD_SOURCE_DEFAULT_EFFECT,
  REMOVE_SOURCE_DEFAULT_EFFECT,
  SET_ASSISTANT_UID,
  SET_A11Y_SERVICES_UIDS,
  IS_HAPTIC_PLAYBACK_SUPPORTED,
  SET_UID_DEVICE_AFFINITY,
  REMOVE_UID_DEVICE_AFFINITY,
  GET_OFFLOAD_FORMATS_A2DP,
  LIST_AUDIO_PRODUCT_STRATEGIES,
  GET_STRATEGY_FOR_ATTRIBUTES,
  LIST_AUDIO_VOLUME_GROUPS,
  GET_VOLUME_GROUP_FOR_ATTRIBUTES,
  SET_ALLOWED_CAPTURE_POLICY,
  MOVE_EFFECTS_TO_IO,
  SET_RTT_ENABLED,
  IS_CALL_SCREEN_MODE_SUPPORTED,
  SET_PREFERRED_DEVICE_FOR_PRODUCT_STRATEGY,
  REMOVE_PREFERRED_DEVICE_FOR_PRODUCT_STRATEGY,
  GET_PREFERRED_DEVICE_FOR_PRODUCT_STRATEGY,
};
#define MAX_ITEMS_PER_LIST 1024
class BpAudioPolicyService : public BpInterface<IAudioPolicyService> {
 public:
  explicit BpAudioPolicyService(const sp<IBinder> &impl)
      : BpInterface<IAudioPolicyService>(impl) {}
  virtual status_t setDeviceConnectionState(audio_devices_t device,
                                            audio_policy_dev_state_t state,
                                            const char *device_address,
                                            const char *device_name,
                                            audio_format_t encodedFormat) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(static_cast<uint32_t>(device));
    data.writeInt32(static_cast<uint32_t>(state));
    data.writeCString(device_address);
    data.writeCString(device_name);
    data.writeInt32(static_cast<uint32_t>(encodedFormat));
    remote()->transact(SET_DEVICE_CONNECTION_STATE, data, &reply);
    return static_cast<status_t>(reply.readInt32());
  }
  virtual audio_policy_dev_state_t getDeviceConnectionState(
      audio_devices_t device, const char *device_address) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(static_cast<uint32_t>(device));
    data.writeCString(device_address);
    remote()->transact(GET_DEVICE_CONNECTION_STATE, data, &reply);
    return static_cast<audio_policy_dev_state_t>(reply.readInt32());
  }
  virtual status_t handleDeviceConfigChange(audio_devices_t device,
                                            const char *device_address,
                                            const char *device_name,
                                            audio_format_t encodedFormat) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(static_cast<uint32_t>(device));
    data.writeCString(device_address);
    data.writeCString(device_name);
    data.writeInt32(static_cast<uint32_t>(encodedFormat));
    remote()->transact(HANDLE_DEVICE_CONFIG_CHANGE, data, &reply);
    return static_cast<status_t>(reply.readInt32());
  }
  virtual status_t setPhoneState(audio_mode_t state) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(state);
    remote()->transact(SET_PHONE_STATE, data, &reply);
    return static_cast<status_t>(reply.readInt32());
  }
  virtual status_t setForceUse(audio_policy_force_use_t usage,
                               audio_policy_forced_cfg_t config) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(static_cast<uint32_t>(usage));
    data.writeInt32(static_cast<uint32_t>(config));
    remote()->transact(SET_FORCE_USE, data, &reply);
    return static_cast<status_t>(reply.readInt32());
  }
  virtual audio_policy_forced_cfg_t getForceUse(
      audio_policy_force_use_t usage) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(static_cast<uint32_t>(usage));
    remote()->transact(GET_FORCE_USE, data, &reply);
    return static_cast<audio_policy_forced_cfg_t>(reply.readInt32());
  }
  virtual audio_io_handle_t getOutput(audio_stream_type_t stream) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(static_cast<uint32_t>(stream));
    remote()->transact(GET_OUTPUT, data, &reply);
    return static_cast<audio_io_handle_t>(reply.readInt32());
  }
  status_t getOutputForAttr(
      audio_attributes_t *attr, audio_io_handle_t *output,
      audio_session_t session, audio_stream_type_t *stream, pid_t pid,
      uid_t uid, const audio_config_t *config, audio_output_flags_t flags,
      audio_port_handle_t *selectedDeviceId, audio_port_handle_t *portId,
      std::vector<audio_io_handle_t> *secondaryOutputs) override {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    if (attr == nullptr) {
      ALOGE("%s NULL audio attributes", __func__);
      return BAD_VALUE;
    }
    if (output == nullptr) {
      ALOGE("%s NULL output - shouldn't happen", __func__);
      return BAD_VALUE;
    }
    if (selectedDeviceId == nullptr) {
      ALOGE("%s NULL selectedDeviceId - shouldn't happen", __func__);
      return BAD_VALUE;
    }
    if (portId == nullptr) {
      ALOGE("%s NULL portId - shouldn't happen", __func__);
      return BAD_VALUE;
    }
    if (secondaryOutputs == nullptr) {
      ALOGE("%s NULL secondaryOutputs - shouldn't happen", __func__);
      return BAD_VALUE;
    }
    data.write(attr, sizeof(audio_attributes_t));
    data.writeInt32(session);
    if (stream == NULL) {
      data.writeInt32(0);
    } else {
      data.writeInt32(1);
      data.writeInt32(*stream);
    }
    data.writeInt32(pid);
    data.writeInt32(uid);
    data.write(config, sizeof(audio_config_t));
    data.writeInt32(static_cast<uint32_t>(flags));
    data.writeInt32(*selectedDeviceId);
    data.writeInt32(*portId);
    status_t status = remote()->transact(GET_OUTPUT_FOR_ATTR, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = (status_t)reply.readInt32();
    if (status != NO_ERROR) {
      return status;
    }
    status = (status_t)reply.read(&attr, sizeof(audio_attributes_t));
    if (status != NO_ERROR) {
      return status;
    }
    *output = (audio_io_handle_t)reply.readInt32();
    audio_stream_type_t lStream = (audio_stream_type_t)reply.readInt32();
    if (stream != NULL) {
      *stream = lStream;
    }
    *selectedDeviceId = (audio_port_handle_t)reply.readInt32();
    *portId = (audio_port_handle_t)reply.readInt32();
    secondaryOutputs->resize(reply.readInt32());
    return reply.read(secondaryOutputs->data(),
                      secondaryOutputs->size() * sizeof(audio_io_handle_t));
  }
  virtual status_t startOutput(audio_port_handle_t portId) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32((int32_t)portId);
    remote()->transact(START_OUTPUT, data, &reply);
    return static_cast<status_t>(reply.readInt32());
  }
  virtual status_t stopOutput(audio_port_handle_t portId) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32((int32_t)portId);
    remote()->transact(STOP_OUTPUT, data, &reply);
    return static_cast<status_t>(reply.readInt32());
  }
  virtual void releaseOutput(audio_port_handle_t portId) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32((int32_t)portId);
    remote()->transact(RELEASE_OUTPUT, data, &reply);
  }
  virtual status_t getInputForAttr(
      const audio_attributes_t *attr, audio_io_handle_t *input,
      audio_unique_id_t riid, audio_session_t session, pid_t pid, uid_t uid,
      const String16 &opPackageName, const audio_config_base_t *config,
      audio_input_flags_t flags, audio_port_handle_t *selectedDeviceId,
      audio_port_handle_t *portId) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    if (attr == NULL) {
      ALOGE("getInputForAttr NULL attr - shouldn't happen");
      return BAD_VALUE;
    }
    if (input == NULL) {
      ALOGE("getInputForAttr NULL input - shouldn't happen");
      return BAD_VALUE;
    }
    if (selectedDeviceId == NULL) {
      ALOGE("getInputForAttr NULL selectedDeviceId - shouldn't happen");
      return BAD_VALUE;
    }
    if (portId == NULL) {
      ALOGE("getInputForAttr NULL portId - shouldn't happen");
      return BAD_VALUE;
    }
    data.write(attr, sizeof(audio_attributes_t));
    data.writeInt32(*input);
    data.writeInt32(riid);
    data.writeInt32(session);
    data.writeInt32(pid);
    data.writeInt32(uid);
    data.writeString16(opPackageName);
    data.write(config, sizeof(audio_config_base_t));
    data.writeInt32(flags);
    data.writeInt32(*selectedDeviceId);
    data.writeInt32(*portId);
    status_t status = remote()->transact(GET_INPUT_FOR_ATTR, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = reply.readInt32();
    if (status != NO_ERROR) {
      return status;
    }
    *input = (audio_io_handle_t)reply.readInt32();
    *selectedDeviceId = (audio_port_handle_t)reply.readInt32();
    *portId = (audio_port_handle_t)reply.readInt32();
    return NO_ERROR;
  }
  virtual status_t startInput(audio_port_handle_t portId) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(portId);
    remote()->transact(START_INPUT, data, &reply);
    status_t status = static_cast<status_t>(reply.readInt32());
    return status;
  }
  virtual status_t stopInput(audio_port_handle_t portId) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(portId);
    remote()->transact(STOP_INPUT, data, &reply);
    return static_cast<status_t>(reply.readInt32());
  }
  virtual void releaseInput(audio_port_handle_t portId) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(portId);
    remote()->transact(RELEASE_INPUT, data, &reply);
  }
  virtual status_t initStreamVolume(audio_stream_type_t stream, int indexMin,
                                    int indexMax) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(static_cast<uint32_t>(stream));
    data.writeInt32(indexMin);
    data.writeInt32(indexMax);
    remote()->transact(INIT_STREAM_VOLUME, data, &reply);
    return static_cast<status_t>(reply.readInt32());
  }
  virtual status_t setStreamVolumeIndex(audio_stream_type_t stream, int index,
                                        audio_devices_t device) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(static_cast<uint32_t>(stream));
    data.writeInt32(index);
    data.writeInt32(static_cast<uint32_t>(device));
    remote()->transact(SET_STREAM_VOLUME, data, &reply);
    return static_cast<status_t>(reply.readInt32());
  }
  virtual status_t getStreamVolumeIndex(audio_stream_type_t stream, int *index,
                                        audio_devices_t device) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(static_cast<uint32_t>(stream));
    data.writeInt32(static_cast<uint32_t>(device));
    remote()->transact(GET_STREAM_VOLUME, data, &reply);
    int lIndex = reply.readInt32();
    if (index) *index = lIndex;
    return static_cast<status_t>(reply.readInt32());
  }
  virtual status_t setVolumeIndexForAttributes(const audio_attributes_t &attr,
                                               int index,
                                               audio_devices_t device) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.write(&attr, sizeof(audio_attributes_t));
    data.writeInt32(index);
    data.writeInt32(static_cast<uint32_t>(device));
    status_t status = remote()->transact(SET_VOLUME_ATTRIBUTES, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    return static_cast<status_t>(reply.readInt32());
  }
  virtual status_t getVolumeIndexForAttributes(const audio_attributes_t &attr,
                                               int &index,
                                               audio_devices_t device) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.write(&attr, sizeof(audio_attributes_t));
    data.writeInt32(static_cast<uint32_t>(device));
    status_t status = remote()->transact(GET_VOLUME_ATTRIBUTES, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = static_cast<status_t>(reply.readInt32());
    if (status != NO_ERROR) {
      return status;
    }
    index = reply.readInt32();
    return NO_ERROR;
  }
  virtual status_t getMinVolumeIndexForAttributes(
      const audio_attributes_t &attr, int &index) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.write(&attr, sizeof(audio_attributes_t));
    status_t status =
        remote()->transact(GET_MIN_VOLUME_FOR_ATTRIBUTES, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = static_cast<status_t>(reply.readInt32());
    if (status != NO_ERROR) {
      return status;
    }
    index = reply.readInt32();
    return NO_ERROR;
  }
  virtual status_t getMaxVolumeIndexForAttributes(
      const audio_attributes_t &attr, int &index) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.write(&attr, sizeof(audio_attributes_t));
    status_t status =
        remote()->transact(GET_MAX_VOLUME_FOR_ATTRIBUTES, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = static_cast<status_t>(reply.readInt32());
    if (status != NO_ERROR) {
      return status;
    }
    index = reply.readInt32();
    return NO_ERROR;
  }
  virtual uint32_t getStrategyForStream(audio_stream_type_t stream) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(static_cast<uint32_t>(stream));
    remote()->transact(GET_STRATEGY_FOR_STREAM, data, &reply);
    return reply.readUint32();
  }
  virtual audio_devices_t getDevicesForStream(audio_stream_type_t stream) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(static_cast<uint32_t>(stream));
    remote()->transact(GET_DEVICES_FOR_STREAM, data, &reply);
    return (audio_devices_t)reply.readInt32();
  }
  virtual audio_io_handle_t getOutputForEffect(
      const effect_descriptor_t *desc) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.write(desc, sizeof(effect_descriptor_t));
    remote()->transact(GET_OUTPUT_FOR_EFFECT, data, &reply);
    return static_cast<audio_io_handle_t>(reply.readInt32());
  }
  virtual status_t registerEffect(const effect_descriptor_t *desc,
                                  audio_io_handle_t io, uint32_t strategy,
                                  audio_session_t session, int id) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.write(desc, sizeof(effect_descriptor_t));
    data.writeInt32(io);
    data.writeInt32(strategy);
    data.writeInt32(session);
    data.writeInt32(id);
    remote()->transact(REGISTER_EFFECT, data, &reply);
    return static_cast<status_t>(reply.readInt32());
  }
  virtual status_t unregisterEffect(int id) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(id);
    remote()->transact(UNREGISTER_EFFECT, data, &reply);
    return static_cast<status_t>(reply.readInt32());
  }
  virtual status_t setEffectEnabled(int id, bool enabled) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(id);
    data.writeInt32(enabled);
    remote()->transact(SET_EFFECT_ENABLED, data, &reply);
    return static_cast<status_t>(reply.readInt32());
  }
  status_t moveEffectsToIo(const std::vector<int> &ids,
                           audio_io_handle_t io) override {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(ids.size());
    for (auto id : ids) {
      data.writeInt32(id);
    }
    data.writeInt32(io);
    status_t status = remote()->transact(MOVE_EFFECTS_TO_IO, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    return static_cast<status_t>(reply.readInt32());
  }
  virtual bool isStreamActive(audio_stream_type_t stream,
                              uint32_t inPastMs) const {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32((int32_t)stream);
    data.writeInt32(inPastMs);
    remote()->transact(IS_STREAM_ACTIVE, data, &reply);
    return reply.readInt32();
  }
  virtual bool isStreamActiveRemotely(audio_stream_type_t stream,
                                      uint32_t inPastMs) const {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32((int32_t)stream);
    data.writeInt32(inPastMs);
    remote()->transact(IS_STREAM_ACTIVE_REMOTELY, data, &reply);
    return reply.readInt32();
  }
  virtual bool isSourceActive(audio_source_t source) const {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32((int32_t)source);
    remote()->transact(IS_SOURCE_ACTIVE, data, &reply);
    return reply.readInt32();
  }
  virtual status_t queryDefaultPreProcessing(audio_session_t audioSession,
                                             effect_descriptor_t *descriptors,
                                             uint32_t *count) {
    if (descriptors == NULL || count == NULL) {
      return BAD_VALUE;
    }
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(audioSession);
    data.writeInt32(*count);
    status_t status =
        remote()->transact(QUERY_DEFAULT_PRE_PROCESSING, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = static_cast<status_t>(reply.readInt32());
    uint32_t retCount = reply.readInt32();
    if (retCount != 0) {
      uint32_t numDesc = (retCount < *count) ? retCount : *count;
      reply.read(descriptors, sizeof(effect_descriptor_t) * numDesc);
    }
    *count = retCount;
    return status;
  }
  status_t setAllowedCapturePolicy(uid_t uid,
                                   audio_flags_mask_t flags) override {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(uid);
    data.writeInt32(flags);
    remote()->transact(SET_ALLOWED_CAPTURE_POLICY, data, &reply);
    return reply.readInt32();
  }
  virtual bool isOffloadSupported(const audio_offload_info_t &info) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.write(&info, sizeof(audio_offload_info_t));
    remote()->transact(IS_OFFLOAD_SUPPORTED, data, &reply);
    return reply.readInt32();
  }
  virtual bool isDirectOutputSupported(const audio_config_base_t &config,
                                       const audio_attributes_t &attributes) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.write(&config, sizeof(audio_config_base_t));
    data.write(&attributes, sizeof(audio_attributes_t));
    status_t status =
        remote()->transact(IS_DIRECT_OUTPUT_SUPPORTED, data, &reply);
    return status == NO_ERROR ? static_cast<bool>(reply.readInt32()) : false;
  }
  virtual status_t listAudioPorts(audio_port_role_t role,
                                  audio_port_type_t type,
                                  unsigned int *num_ports,
                                  struct audio_port *ports,
                                  unsigned int *generation) {
    if (num_ports == NULL || (*num_ports != 0 && ports == NULL) ||
        generation == NULL) {
      return BAD_VALUE;
    }
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    unsigned int numPortsReq = (ports == NULL) ? 0 : *num_ports;
    data.writeInt32(role);
    data.writeInt32(type);
    data.writeInt32(numPortsReq);
    status_t status = remote()->transact(LIST_AUDIO_PORTS, data, &reply);
    if (status == NO_ERROR) {
      status = (status_t)reply.readInt32();
      *num_ports = (unsigned int)reply.readInt32();
    }
    if (status == NO_ERROR) {
      if (numPortsReq > *num_ports) {
        numPortsReq = *num_ports;
      }
      if (numPortsReq > 0) {
        reply.read(ports, numPortsReq * sizeof(struct audio_port));
      }
      *generation = reply.readInt32();
    }
    return status;
  }
  virtual status_t getAudioPort(struct audio_port *port) {
    if (port == NULL) {
      return BAD_VALUE;
    }
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.write(port, sizeof(struct audio_port));
    status_t status = remote()->transact(GET_AUDIO_PORT, data, &reply);
    if (status != NO_ERROR ||
        (status = (status_t)reply.readInt32()) != NO_ERROR) {
      return status;
    }
    reply.read(port, sizeof(struct audio_port));
    return status;
  }
  virtual status_t createAudioPatch(const struct audio_patch *patch,
                                    audio_patch_handle_t *handle) {
    if (patch == NULL || handle == NULL) {
      return BAD_VALUE;
    }
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.write(patch, sizeof(struct audio_patch));
    data.write(handle, sizeof(audio_patch_handle_t));
    status_t status = remote()->transact(CREATE_AUDIO_PATCH, data, &reply);
    if (status != NO_ERROR ||
        (status = (status_t)reply.readInt32()) != NO_ERROR) {
      return status;
    }
    reply.read(handle, sizeof(audio_patch_handle_t));
    return status;
  }
  virtual status_t releaseAudioPatch(audio_patch_handle_t handle) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.write(&handle, sizeof(audio_patch_handle_t));
    status_t status = remote()->transact(RELEASE_AUDIO_PATCH, data, &reply);
    if (status != NO_ERROR) {
      status = (status_t)reply.readInt32();
    }
    return status;
  }
  virtual status_t listAudioPatches(unsigned int *num_patches,
                                    struct audio_patch *patches,
                                    unsigned int *generation) {
    if (num_patches == NULL || (*num_patches != 0 && patches == NULL) ||
        generation == NULL) {
      return BAD_VALUE;
    }
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    unsigned int numPatchesReq = (patches == NULL) ? 0 : *num_patches;
    data.writeInt32(numPatchesReq);
    status_t status = remote()->transact(LIST_AUDIO_PATCHES, data, &reply);
    if (status == NO_ERROR) {
      status = (status_t)reply.readInt32();
      *num_patches = (unsigned int)reply.readInt32();
    }
    if (status == NO_ERROR) {
      if (numPatchesReq > *num_patches) {
        numPatchesReq = *num_patches;
      }
      if (numPatchesReq > 0) {
        reply.read(patches, numPatchesReq * sizeof(struct audio_patch));
      }
      *generation = reply.readInt32();
    }
    return status;
  }
  virtual status_t setAudioPortConfig(const struct audio_port_config *config) {
    if (config == NULL) {
      return BAD_VALUE;
    }
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.write(config, sizeof(struct audio_port_config));
    status_t status = remote()->transact(SET_AUDIO_PORT_CONFIG, data, &reply);
    if (status != NO_ERROR) {
      status = (status_t)reply.readInt32();
    }
    return status;
  }
  virtual void registerClient(const sp<IAudioPolicyServiceClient> &client) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeStrongBinder(IInterface::asBinder(client));
    remote()->transact(REGISTER_CLIENT, data, &reply);
  }
  virtual void setAudioPortCallbacksEnabled(bool enabled) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(enabled ? 1 : 0);
    remote()->transact(SET_AUDIO_PORT_CALLBACK_ENABLED, data, &reply);
  }
  virtual void setAudioVolumeGroupCallbacksEnabled(bool enabled) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(enabled ? 1 : 0);
    remote()->transact(SET_AUDIO_VOLUME_GROUP_CALLBACK_ENABLED, data, &reply);
  }
  virtual status_t acquireSoundTriggerSession(audio_session_t *session,
                                              audio_io_handle_t *ioHandle,
                                              audio_devices_t *device) {
    if (session == NULL || ioHandle == NULL || device == NULL) {
      return BAD_VALUE;
    }
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    status_t status =
        remote()->transact(ACQUIRE_SOUNDTRIGGER_SESSION, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = (status_t)reply.readInt32();
    if (status == NO_ERROR) {
      *session = (audio_session_t)reply.readInt32();
      *ioHandle = (audio_io_handle_t)reply.readInt32();
      *device = (audio_devices_t)reply.readInt32();
    }
    return status;
  }
  virtual status_t releaseSoundTriggerSession(audio_session_t session) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(session);
    status_t status =
        remote()->transact(RELEASE_SOUNDTRIGGER_SESSION, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    return (status_t)reply.readInt32();
  }
  virtual audio_mode_t getPhoneState() {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    status_t status = remote()->transact(GET_PHONE_STATE, data, &reply);
    if (status != NO_ERROR) {
      return AUDIO_MODE_INVALID;
    }
    return (audio_mode_t)reply.readInt32();
  }
  virtual status_t registerPolicyMixes(const Vector<AudioMix> &mixes,
                                       bool registration) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(registration ? 1 : 0);
    size_t size = mixes.size();
    if (size > MAX_MIXES_PER_POLICY) {
      size = MAX_MIXES_PER_POLICY;
    }
    size_t sizePosition = data.dataPosition();
    data.writeInt32(size);
    size_t finalSize = size;
    for (size_t i = 0; i < size; i++) {
      size_t position = data.dataPosition();
      if (mixes[i].writeToParcel(&data) != NO_ERROR) {
        data.setDataPosition(position);
        finalSize--;
      }
    }
    if (size != finalSize) {
      size_t position = data.dataPosition();
      data.setDataPosition(sizePosition);
      data.writeInt32(finalSize);
      data.setDataPosition(position);
    }
    status_t status = remote()->transact(REGISTER_POLICY_MIXES, data, &reply);
    if (status == NO_ERROR) {
      status = (status_t)reply.readInt32();
    }
    return status;
  }
  virtual status_t startAudioSource(const struct audio_port_config *source,
                                    const audio_attributes_t *attributes,
                                    audio_port_handle_t *portId) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    if (source == NULL || attributes == NULL || portId == NULL) {
      return BAD_VALUE;
    }
    data.write(source, sizeof(struct audio_port_config));
    data.write(attributes, sizeof(audio_attributes_t));
    status_t status = remote()->transact(START_AUDIO_SOURCE, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = (status_t)reply.readInt32();
    if (status != NO_ERROR) {
      return status;
    }
    *portId = (audio_port_handle_t)reply.readInt32();
    return status;
  }
  virtual status_t stopAudioSource(audio_port_handle_t portId) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(portId);
    status_t status = remote()->transact(STOP_AUDIO_SOURCE, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = (status_t)reply.readInt32();
    return status;
  }
  virtual status_t setMasterMono(bool mono) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(static_cast<int32_t>(mono));
    status_t status = remote()->transact(SET_MASTER_MONO, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    return static_cast<status_t>(reply.readInt32());
  }
  virtual status_t getMasterMono(bool *mono) {
    if (mono == nullptr) {
      return BAD_VALUE;
    }
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    status_t status = remote()->transact(GET_MASTER_MONO, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = static_cast<status_t>(reply.readInt32());
    if (status == NO_ERROR) {
      *mono = static_cast<bool>(reply.readInt32());
    }
    return status;
  }
  virtual float getStreamVolumeDB(audio_stream_type_t stream, int index,
                                  audio_devices_t device) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(static_cast<int32_t>(stream));
    data.writeInt32(static_cast<int32_t>(index));
    data.writeUint32(static_cast<uint32_t>(device));
    status_t status = remote()->transact(GET_STREAM_VOLUME_DB, data, &reply);
    if (status != NO_ERROR) {
      return NAN;
    }
    return reply.readFloat();
  }
  virtual status_t getSurroundFormats(unsigned int *numSurroundFormats,
                                      audio_format_t *surroundFormats,
                                      bool *surroundFormatsEnabled,
                                      bool reported) {
    if (numSurroundFormats == NULL ||
        (*numSurroundFormats != 0 &&
         (surroundFormats == NULL || surroundFormatsEnabled == NULL))) {
      return BAD_VALUE;
    }
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    unsigned int numSurroundFormatsReq = *numSurroundFormats;
    data.writeUint32(numSurroundFormatsReq);
    data.writeBool(reported);
    status_t status = remote()->transact(GET_SURROUND_FORMATS, data, &reply);
    if (status == NO_ERROR &&
        (status = (status_t)reply.readInt32()) == NO_ERROR) {
      *numSurroundFormats = reply.readUint32();
    }
    if (status == NO_ERROR) {
      if (numSurroundFormatsReq > *numSurroundFormats) {
        numSurroundFormatsReq = *numSurroundFormats;
      }
      if (numSurroundFormatsReq > 0) {
        status = reply.read(surroundFormats,
                            numSurroundFormatsReq * sizeof(audio_format_t));
        if (status != NO_ERROR) {
          return status;
        }
        status = reply.read(surroundFormatsEnabled,
                            numSurroundFormatsReq * sizeof(bool));
      }
    }
    return status;
  }
  virtual status_t setSurroundFormatEnabled(audio_format_t audioFormat,
                                            bool enabled) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(audioFormat);
    data.writeBool(enabled);
    status_t status =
        remote()->transact(SET_SURROUND_FORMAT_ENABLED, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    return reply.readInt32();
  }
  virtual status_t getHwOffloadEncodingFormatsSupportedForA2DP(
      std::vector<audio_format_t> *formats) {
    if (formats == NULL) {
      return BAD_VALUE;
    }
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    status_t status =
        remote()->transact(GET_OFFLOAD_FORMATS_A2DP, data, &reply);
    if (status != NO_ERROR ||
        (status = (status_t)reply.readInt32()) != NO_ERROR) {
      return status;
    }
    size_t list_size = reply.readUint32();
    for (size_t i = 0; i < list_size; i++) {
      formats->push_back(static_cast<audio_format_t>(reply.readInt32()));
    }
    return NO_ERROR;
  }
  virtual status_t addStreamDefaultEffect(const effect_uuid_t *type,
                                          const String16 &opPackageName,
                                          const effect_uuid_t *uuid,
                                          int32_t priority, audio_usage_t usage,
                                          audio_unique_id_t *id) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.write(type, sizeof(effect_uuid_t));
    data.writeString16(opPackageName);
    data.write(uuid, sizeof(effect_uuid_t));
    data.writeInt32(priority);
    data.writeInt32((int32_t)usage);
    status_t status =
        remote()->transact(ADD_STREAM_DEFAULT_EFFECT, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = static_cast<status_t>(reply.readInt32());
    *id = reply.readInt32();
    return status;
  }
  virtual status_t removeStreamDefaultEffect(audio_unique_id_t id) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(id);
    status_t status =
        remote()->transact(REMOVE_STREAM_DEFAULT_EFFECT, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    return static_cast<status_t>(reply.readInt32());
  }
  virtual status_t addSourceDefaultEffect(const effect_uuid_t *type,
                                          const String16 &opPackageName,
                                          const effect_uuid_t *uuid,
                                          int32_t priority,
                                          audio_source_t source,
                                          audio_unique_id_t *id) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.write(type, sizeof(effect_uuid_t));
    data.writeString16(opPackageName);
    data.write(uuid, sizeof(effect_uuid_t));
    data.writeInt32(priority);
    data.writeInt32((int32_t)source);
    status_t status =
        remote()->transact(ADD_SOURCE_DEFAULT_EFFECT, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = static_cast<status_t>(reply.readInt32());
    *id = reply.readInt32();
    return status;
  }
  virtual status_t removeSourceDefaultEffect(audio_unique_id_t id) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(id);
    status_t status =
        remote()->transact(REMOVE_SOURCE_DEFAULT_EFFECT, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    return static_cast<status_t>(reply.readInt32());
  }
  virtual status_t setAssistantUid(uid_t uid) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(uid);
    status_t status = remote()->transact(SET_ASSISTANT_UID, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    return static_cast<status_t>(reply.readInt32());
  }
  virtual status_t setA11yServicesUids(const std::vector<uid_t> &uids) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(uids.size());
    for (auto uid : uids) {
      data.writeInt32(uid);
    }
    status_t status = remote()->transact(SET_A11Y_SERVICES_UIDS, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    return static_cast<status_t>(reply.readInt32());
  }
  virtual bool isHapticPlaybackSupported() {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    status_t status =
        remote()->transact(IS_HAPTIC_PLAYBACK_SUPPORTED, data, &reply);
    if (status != NO_ERROR) {
      return false;
    }
    return reply.readBool();
  }
  virtual status_t setUidDeviceAffinities(
      uid_t uid, const Vector<AudioDeviceTypeAddr> &devices) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32((int32_t)uid);
    size_t size = devices.size();
    size_t sizePosition = data.dataPosition();
    data.writeInt32((int32_t)size);
    size_t finalSize = size;
    for (size_t i = 0; i < size; i++) {
      size_t position = data.dataPosition();
      if (devices[i].writeToParcel(&data) != NO_ERROR) {
        data.setDataPosition(position);
        finalSize--;
      }
    }
    if (size != finalSize) {
      size_t position = data.dataPosition();
      data.setDataPosition(sizePosition);
      data.writeInt32(finalSize);
      data.setDataPosition(position);
    }
    status_t status = remote()->transact(SET_UID_DEVICE_AFFINITY, data, &reply);
    if (status == NO_ERROR) {
      status = (status_t)reply.readInt32();
    }
    return status;
  }
  virtual status_t removeUidDeviceAffinities(uid_t uid) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32((int32_t)uid);
    status_t status =
        remote()->transact(REMOVE_UID_DEVICE_AFFINITY, data, &reply);
    if (status == NO_ERROR) {
      status = (status_t)reply.readInt32();
    }
    return status;
  }
  virtual status_t listAudioProductStrategies(
      AudioProductStrategyVector &strategies) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    status_t status =
        remote()->transact(LIST_AUDIO_PRODUCT_STRATEGIES, data, &reply);
    if (status != NO_ERROR) {
      ALOGE("%s: permission denied", __func__);
      return status;
    }
    status = static_cast<status_t>(reply.readInt32());
    if (status != NO_ERROR) {
      return status;
    }
    uint32_t numStrategies = static_cast<uint32_t>(reply.readInt32());
    for (size_t i = 0; i < numStrategies; i++) {
      AudioProductStrategy strategy;
      status = strategy.readFromParcel(&reply);
      if (status != NO_ERROR) {
        ALOGE("%s: failed to read strategies", __FUNCTION__);
        strategies.clear();
        return status;
      }
      strategies.push_back(strategy);
    }
    return NO_ERROR;
  }
  virtual status_t getProductStrategyFromAudioAttributes(
      const AudioAttributes &aa, product_strategy_t &productStrategy) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    status_t status = aa.writeToParcel(&data);
    if (status != NO_ERROR) {
      return status;
    }
    status = remote()->transact(GET_STRATEGY_FOR_ATTRIBUTES, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = static_cast<status_t>(reply.readInt32());
    if (status != NO_ERROR) {
      return status;
    }
    productStrategy = static_cast<product_strategy_t>(reply.readInt32());
    return NO_ERROR;
  }
  virtual status_t listAudioVolumeGroups(AudioVolumeGroupVector &groups) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    status_t status =
        remote()->transact(LIST_AUDIO_VOLUME_GROUPS, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = static_cast<status_t>(reply.readInt32());
    if (status != NO_ERROR) {
      return status;
    }
    uint32_t numGroups = static_cast<uint32_t>(reply.readInt32());
    for (size_t i = 0; i < numGroups; i++) {
      AudioVolumeGroup group;
      status = group.readFromParcel(&reply);
      if (status != NO_ERROR) {
        ALOGE("%s: failed to read volume groups", __FUNCTION__);
        groups.clear();
        return status;
      }
      groups.push_back(group);
    }
    return NO_ERROR;
  }
  virtual status_t getVolumeGroupFromAudioAttributes(
      const AudioAttributes &aa, volume_group_t &volumeGroup) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    status_t status = aa.writeToParcel(&data);
    if (status != NO_ERROR) {
      return status;
    }
    status = remote()->transact(GET_VOLUME_GROUP_FOR_ATTRIBUTES, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = static_cast<status_t>(reply.readInt32());
    if (status != NO_ERROR) {
      return status;
    }
    volumeGroup = static_cast<volume_group_t>(reply.readInt32());
    return NO_ERROR;
  }
  virtual status_t setRttEnabled(bool enabled) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeInt32(static_cast<int32_t>(enabled));
    status_t status = remote()->transact(SET_RTT_ENABLED, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    return static_cast<status_t>(reply.readInt32());
  }
  virtual bool isCallScreenModeSupported() {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    status_t status =
        remote()->transact(IS_CALL_SCREEN_MODE_SUPPORTED, data, &reply);
    if (status != NO_ERROR) {
      return false;
    }
    return reply.readBool();
  }
  virtual status_t setPreferredDeviceForStrategy(
      product_strategy_t strategy, const AudioDeviceTypeAddr &device) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeUint32(static_cast<uint32_t>(strategy));
    status_t status = device.writeToParcel(&data);
    if (status != NO_ERROR) {
      return BAD_VALUE;
    }
    status = remote()->transact(SET_PREFERRED_DEVICE_FOR_PRODUCT_STRATEGY, data,
                                &reply);
    if (status != NO_ERROR) {
      return status;
    }
    return static_cast<status_t>(reply.readInt32());
  }
  virtual status_t removePreferredDeviceForStrategy(
      product_strategy_t strategy) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeUint32(static_cast<uint32_t>(strategy));
    status_t status = remote()->transact(
        REMOVE_PREFERRED_DEVICE_FOR_PRODUCT_STRATEGY, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    return static_cast<status_t>(reply.readInt32());
  }
  virtual status_t getPreferredDeviceForStrategy(product_strategy_t strategy,
                                                 AudioDeviceTypeAddr &device) {
    Parcel data, reply;
    data.writeInterfaceToken(IAudioPolicyService::getInterfaceDescriptor());
    data.writeUint32(static_cast<uint32_t>(strategy));
    status_t status = remote()->transact(
        GET_PREFERRED_DEVICE_FOR_PRODUCT_STRATEGY, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    status = device.readFromParcel(&reply);
    if (status != NO_ERROR) {
      return status;
    }
    return static_cast<status_t>(reply.readInt32());
  }
};
status_t BnAudioPolicyService::sanitizeAudioPortConfig(
    struct audio_port_config *config) {
  if (config->type == AUDIO_PORT_TYPE_DEVICE &&
      preventStringOverflow(config->ext.device.address)) {
    return BAD_VALUE;
  }
  return NO_ERROR;
}
template <size_t size>
static bool preventStringOverflow(char (&s)[size]) {
  if (strnlen(s, size) < size) return false;
  s[size - 1] = '\0';
  return true;
}
status_t BnAudioPolicyService::sanitizeAudioPortConfig(
    struct audio_port_config *config) {
  if (config->type == AUDIO_PORT_TYPE_DEVICE &&
      preventStringOverflow(config->ext.device.address)) {
    return BAD_VALUE;
  }
  return NO_ERROR;
}
status_t BnAudioPolicyService::sanitizeAudioPortConfig(
    struct audio_port_config *config) {
  if (config->type == AUDIO_PORT_TYPE_DEVICE &&
      preventStringOverflow(config->ext.device.address)) {
    return BAD_VALUE;
  }
  return NO_ERROR;
}
status_t BnAudioPolicyService::sanitizeAudioPortConfig(
    struct audio_port_config *config) {
  if (config->type == AUDIO_PORT_TYPE_DEVICE &&
      preventStringOverflow(config->ext.device.address)) {
    return BAD_VALUE;
  }
  return NO_ERROR;
}
}
