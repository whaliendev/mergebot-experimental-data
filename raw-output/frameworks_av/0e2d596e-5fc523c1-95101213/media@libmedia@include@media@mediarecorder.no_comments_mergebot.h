#ifndef ANDROID_MEDIARECORDER_H
#define ANDROID_MEDIARECORDER_H 
#include <utils/Log.h>
#include <utils/threads.h>
#include <utils/List.h>
#include <utils/Errors.h>
#include <media/IMediaRecorderClient.h>
#include <media/IMediaDeathNotifier.h>
#include <media/MicrophoneInfo.h>
namespace android {
class Surface;
class IMediaRecorder;
class ICameraRecordingProxy;
class IGraphicBufferProducer;
struct PersistentSurface;
class Surface;
namespace hardware {
class ICamera;
}
typedef void (*media_completion_f)(status_t status, void *cookie);
enum video_source {
VIDEO_SOURCE_DEFAULT = 0,VIDEO_SOURCE_CAMERA = 1,VIDEO_SOURCE_SURFACE = 2,VIDEO_SOURCE_LIST_END
};
enum output_format {
OUTPUT_FORMAT_DEFAULT = 0,OUTPUT_FORMAT_THREE_GPP = 1,OUTPUT_FORMAT_MPEG_4 = 2,OUTPUT_FORMAT_AUDIO_ONLY_START = 3,
OUTPUT_FORMAT_RAW_AMR = 3,
OUTPUT_FORMAT_AMR_NB = 3,OUTPUT_FORMAT_AMR_WB = 4,OUTPUT_FORMAT_AAC_ADIF = 5,OUTPUT_FORMAT_AAC_ADTS = 6,OUTPUT_FORMAT_AUDIO_ONLY_END = 7,
OUTPUT_FORMAT_RTP_AVP = 7,
OUTPUT_FORMAT_MPEG2TS = 8,
OUTPUT_FORMAT_WEBM = 9,
OUTPUT_FORMAT_HEIF = 10,
OUTPUT_FORMAT_OGG = 11,OUTPUT_FORMAT_LIST_END
};
enum audio_encoder {
AUDIO_ENCODER_DEFAULT = 0,AUDIO_ENCODER_AMR_NB = 1,AUDIO_ENCODER_AMR_WB = 2,AUDIO_ENCODER_AAC = 3,AUDIO_ENCODER_HE_AAC = 4,AUDIO_ENCODER_AAC_ELD = 5,AUDIO_ENCODER_VORBIS = 6,AUDIO_ENCODER_OPUS = 7,AUDIO_ENCODER_LIST_END
};
enum video_encoder {
VIDEO_ENCODER_DEFAULT = 0,VIDEO_ENCODER_H263 = 1,VIDEO_ENCODER_H264 = 2,VIDEO_ENCODER_MPEG_4_SP = 3,VIDEO_ENCODER_VP8 = 4,VIDEO_ENCODER_HEVC = 5,VIDEO_ENCODER_LIST_END
};
enum media_recorder_states {
MEDIA_RECORDER_IDLE = 1 << 0,
MEDIA_RECORDER_INITIALIZED = 1 << 1,
MEDIA_RECORDER_DATASOURCE_CONFIGURED = 1 << 2,
MEDIA_RECORDER_PREPARED = 1 << 3,
MEDIA_RECORDER_RECORDING = 1 << 4,
MEDIA_RECORDER_ERROR = 1 << 5,};
enum media_recorder_event_type {
MEDIA_RECORDER_EVENT_LIST_START = 1,MEDIA_RECORDER_EVENT_ERROR = 1,MEDIA_RECORDER_EVENT_INFO = 2,MEDIA_RECORDER_EVENT_LIST_END = 99,
MEDIA_RECORDER_TRACK_EVENT_LIST_START = 100,MEDIA_RECORDER_TRACK_EVENT_ERROR = 100,MEDIA_RECORDER_TRACK_EVENT_INFO = 101,MEDIA_RECORDER_TRACK_EVENT_LIST_END = 1000,MEDIA_RECORDER_AUDIO_ROUTING_CHANGED = 10000,};
enum media_recorder_error_type {
MEDIA_RECORDER_ERROR_UNKNOWN = 1,
MEDIA_RECORDER_TRACK_ERROR_LIST_START = 100,MEDIA_RECORDER_TRACK_ERROR_GENERAL = 100,MEDIA_RECORDER_ERROR_VIDEO_NO_SYNC_FRAME = 200,MEDIA_RECORDER_TRACK_ERROR_LIST_END = 1000,};
enum media_recorder_info_type {
MEDIA_RECORDER_INFO_UNKNOWN = 1,MEDIA_RECORDER_INFO_MAX_DURATION_REACHED = 800,MEDIA_RECORDER_INFO_MAX_FILESIZE_REACHED = 801,MEDIA_RECORDER_INFO_MAX_FILESIZE_APPROACHING = 802,MEDIA_RECORDER_INFO_NEXT_OUTPUT_FILE_STARTED = 803,
MEDIA_RECORDER_TRACK_INFO_LIST_START = 1000,MEDIA_RECORDER_TRACK_INFO_COMPLETION_STATUS = 1000,MEDIA_RECORDER_TRACK_INFO_PROGRESS_IN_TIME = 1001,MEDIA_RECORDER_TRACK_INFO_TYPE = 1002,MEDIA_RECORDER_TRACK_INFO_DURATION_MS = 1003,
MEDIA_RECORDER_TRACK_INFO_MAX_CHUNK_DUR_MS = 1004,MEDIA_RECORDER_TRACK_INFO_ENCODED_FRAMES = 1005,
MEDIA_RECORDER_TRACK_INTER_CHUNK_TIME_MS = 1006,
MEDIA_RECORDER_TRACK_INFO_INITIAL_DELAY_MS = 1007,
MEDIA_RECORDER_TRACK_INFO_START_OFFSET_MS = 1008,
MEDIA_RECORDER_TRACK_INFO_DATA_KBYTES = 1009,MEDIA_RECORDER_TRACK_INFO_LIST_END = 2000,};
class MediaRecorderListener: virtual public RefBase
{
public:
    virtual void notify(int msg, int ext1, int ext2) = 0;
};
class MediaRecorder : public BnMediaRecorderClient,
                      public virtual IMediaDeathNotifier
{
public:
    MediaRecorder(const String16& opPackageName);
    ~MediaRecorder();
    void died();
    status_t initCheck();
    status_t setCamera(const sp<hardware::ICamera>& camera,
            const sp<ICameraRecordingProxy>& proxy);
    status_t setPreviewSurface(const sp<IGraphicBufferProducer>& surface);
    status_t setVideoSource(int vs);
    status_t setAudioSource(int as);
    status_t setPrivacySensitive(bool privacySensitive);
    status_t isPrivacySensitive(bool *privacySensitive) const;
    status_t setOutputFormat(int of);
    status_t setVideoEncoder(int ve);
    status_t setAudioEncoder(int ae);
    status_t setOutputFile(int fd);
    status_t setNextOutputFile(int fd);
    status_t setVideoSize(int width, int height);
    status_t setVideoFrameRate(int frames_per_second);
    status_t setParameters(const String8& params);
    status_t setListener(const sp<MediaRecorderListener>& listener);
    status_t setClientName(const String16& clientName);
    status_t prepare();
    status_t getMaxAmplitude(int* max);
    status_t start();
    status_t stop();
    status_t reset();
    status_t pause();
    status_t resume();
    status_t init();
    status_t close();
    status_t release();
    void notify(int msg, int ext1, int ext2);
    status_t setInputSurface(const sp<PersistentSurface>& surface);
    sp<IGraphicBufferProducer> querySurfaceMediaSourceFromMediaServer();
    status_t getMetrics(Parcel *reply);
    status_t setInputDevice(audio_port_handle_t deviceId);
    status_t getRoutedDeviceId(audio_port_handle_t *deviceId);
    status_t enableAudioDeviceCallback(bool enabled);
    status_t getActiveMicrophones(std::vector<media::MicrophoneInfo>* activeMicrophones);
    status_t setPreferredMicrophoneDirection(audio_microphone_direction_t direction);
    status_t setPreferredMicrophoneFieldDimension(float zoom);
    status_t getPortId(audio_port_handle_t *portId) const;
private:
    void doCleanUp();
    status_t doReset();
    sp<IMediaRecorder> mMediaRecorder;
    sp<MediaRecorderListener> mListener;
    sp<IGraphicBufferProducer> mSurfaceMediaSource;
    media_recorder_states mCurrentState;
    bool mIsAudioSourceSet;
    bool mIsVideoSourceSet;
    bool mIsAudioEncoderSet;
    bool mIsVideoEncoderSet;
    bool mIsOutputFileSet;
    Mutex mLock;
    Mutex mNotifyLock;
    int mOutputFormat;
};
}
#endif
