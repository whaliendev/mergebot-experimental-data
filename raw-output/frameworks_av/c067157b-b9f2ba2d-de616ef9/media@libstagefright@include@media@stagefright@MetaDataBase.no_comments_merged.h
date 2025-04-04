#ifndef META_DATA_BASE_H_
#define META_DATA_BASE_H_ 
#include <sys/types.h>
#include <stdint.h>
#include <utils/RefBase.h>
#include <utils/String8.h>
namespace android {
enum {
    kKeyMIMEType = 'mime',
    kKeyWidth = 'widt',
    kKeyHeight = 'heig',
    kKeyDisplayWidth = 'dWid',
    kKeyDisplayHeight = 'dHgt',
    kKeySARWidth = 'sarW',
    kKeySARHeight = 'sarH',
    kKeyThumbnailWidth = 'thbW',
    kKeyThumbnailHeight = 'thbH',
    kKeyCropRect = 'crop',
    kKeyRotation = 'rotA',
    kKeyIFramesInterval = 'ifiv',
    kKeyStride = 'strd',
    kKeySliceHeight = 'slht',
    kKeyChannelCount = '#chn',
    kKeyChannelMask = 'chnm',
    kKeySampleRate = 'srte',
    kKeyPcmEncoding = 'PCMe',
    kKeyFrameRate = 'frmR',
    kKeyBitRate = 'brte',
    kKeyMaxBitRate = 'mxBr',
    kKeyBitsPerSample = 'bits',
    kKeyStreamHeader = 'stHd',
    kKeyESDS = 'esds',
    kKeyAACProfile = 'aacp',
    kKeyAVCC = 'avcc',
    kKeyHVCC = 'hvcc',
    kKeyDVCC = 'dvcc',
    kKeyAV1C = 'av1c',
    kKeyThumbnailHVCC = 'thvc',
    kKeyThumbnailAV1C = 'tav1',
    kKeyD263 = 'd263',
    kKeyOpusHeader = 'ohdr',
    kKeyOpusCodecDelay = 'ocod',
    kKeyOpusSeekPreRoll = 'ospr',
    kKeyVp9CodecPrivate = 'vp9p',
    kKeyIsSyncFrame = 'sync',
    kKeyIsCodecConfig = 'conf',
    kKeyIsMuxerData = 'muxd',
    kKeyIsEndOfStream = 'feos',
    kKeyTime = 'time',
    kKeyDecodingTime = 'decT',
    kKeyNTPTime = 'ntpT',
    kKeyTargetTime = 'tarT',
    kKeyDriftTime = 'dftT',
    kKeyAnchorTime = 'ancT',
    kKeyDuration = 'dura',
    kKeyPixelFormat = 'pixf',
    kKeyColorFormat = 'colf',
    kKeyColorSpace = 'cols',
    kKeyPlatformPrivate = 'priv',
    kKeyDecoderComponent = 'decC',
    kKeyBufferID = 'bfID',
    kKeyMaxInputSize = 'inpS',
    kKeyMaxWidth = 'maxW',
    kKeyMaxHeight = 'maxH',
    kKeyThumbnailTime = 'thbT',
    kKeyTrackID = 'trID',
    kKeyEncoderDelay = 'encd',
    kKeyEncoderPadding = 'encp',
    kKeyAlbum = 'albu',
    kKeyArtist = 'arti',
    kKeyAlbumArtist = 'aart',
    kKeyComposer = 'comp',
    kKeyGenre = 'genr',
    kKeyTitle = 'titl',
    kKeyYear = 'year',
    kKeyAlbumArt = 'albA',
    kKeyAuthor = 'auth',
    kKeyCDTrackNumber = 'cdtr',
    kKeyDiscNumber = 'dnum',
    kKeyDate = 'date',
    kKeyWriter = 'writ',
    kKeyCompilation = 'cpil',
    kKeyLocation = 'loc ',
    kKeyTimeScale = 'tmsl',
    kKeyCaptureFramerate = 'capF',
    kKeyVideoProfile = 'vprf',
    kKeyVideoLevel = 'vlev',
    kKey2ByteNalLength = '2NAL',
    kKeyFileType = 'ftyp',
    kKeyTrackTimeStatus = 'tktm',
    kKeyRealTimeRecording = 'rtrc',
    kKeyNumBuffers = 'nbbf',
    kKeyAutoLoop = 'autL',
    kKeyValidSamples = 'valD',
    kKeyIsUnreadable = 'unre',
    kKeyRendered = 'rend',
    kKeyMediaLanguage = 'lang',
    kKeyManufacturer = 'manu',
    kKeyTextFormatData = 'text',
    kKeyRequiresSecureBuffers = 'secu',
    kKeyIsADTS = 'adts',
    kKeyAACAOT = 'aaot',
    kKeyEncryptedSizes = 'encr',
    kKeyPlainSizes = 'plai',
    kKeyCryptoKey = 'cryK',
    kKeyCryptoIV = 'cryI',
    kKeyCryptoMode = 'cryM',
    kKeyCryptoDefaultIVSize = 'cryS',
    kKeyPssh = 'pssh',
    kKeyCASystemID = 'caid',
    kKeyCASessionID = 'seid',
    kKeyCAPrivateData = 'cadc',
    kKeyEncryptedByteBlock = 'cblk',
    kKeySkipByteBlock = 'sblk',
    kKeyTrackIsAutoselect = 'auto',
    kKeyTrackIsDefault = 'dflt',
    kKeyTrackIsForced = 'frcd',
    kKeySEI = 'sei ',
    kKeyMpegUserData = 'mpud',
    kKeyHdrStaticInfo = 'hdrS',
    kKeyHdr10PlusInfo = 'hdrD',
    kKeyColorRange = 'cRng',
    kKeyColorPrimaries = 'cPrm',
    kKeyTransferFunction = 'tFun',
    kKeyColorMatrix = 'cMtx',
    kKeyTemporalLayerId = 'iLyr',
    kKeyTemporalLayerCount = 'cLyr',
    kKeyTileWidth = 'tilW',
    kKeyTileHeight = 'tilH',
    kKeyGridRows = 'grdR',
    kKeyGridCols = 'grdC',
    kKeyIccProfile = 'prof',
    kKeyIsPrimaryImage = 'prim',
    kKeyFrameCount = 'nfrm',
    kKeyExifOffset = 'exof',
    kKeyExifSize = 'exsz',
    kKeyExifTiffOffset = 'thdr',
    kKeyXmpOffset = 'xmof',
    kKeyXmpSize = 'xmsz',
    kKeyPcmBigEndian = 'pcmb',
    kKeyAlacMagicCookie = 'almc',
    kKeyAudioPresentationInfo = 'audP',
    kKeyOpaqueCSD0 = 'csd0',
    kKeyOpaqueCSD1 = 'csd1',
    kKeyOpaqueCSD2 = 'csd2',
    kKeyHapticChannelCount = 'hapC',
    kKey4BitTrackIds = '4bid',
    kKeyEmptyTrackMalFormed = 'nemt',
<<<<<<< HEAD
    kKeyVps = 'sVps',
    kKeySps = 'sSps',
    kKeyPps = 'sPps',
||||||| de616ef923
    kKeySps = 'sSps',
    kKeyPps = 'sPps',
=======
    kKeyVps = 'sVps',
    kKeySps = 'sSps',
    kKeyPps = 'sPps',
>>>>>>> b9f2ba2d
    kKeySelfID = 'sfid',
    kKeyPayloadType = 'pTyp',
    kKeyRtpExtMap = 'extm',
    kKeyRtpCvoDegrees = 'cvod',
<<<<<<< HEAD
    kKeyRtpDscp = 'dscp',
    kKeySocketNetwork = 'sNet',
    kKeySlowMotionMarkers = 'slmo',
||||||| de616ef923
=======
    kKeyRtpDscp = 'dscp',
    kKeySocketNetwork = 'sNet',
>>>>>>> b9f2ba2d
};
enum {
    kTypeESDS = 'esds',
    kTypeAVCC = 'avcc',
    kTypeHVCC = 'hvcc',
    kTypeAV1C = 'av1c',
    kTypeDVCC = 'dvcc',
    kTypeD263 = 'd263',
};
enum {
    kCryptoModeUnencrypted = 0,
    kCryptoModeAesCtr = 1,
    kCryptoModeAesCbc = 2,
};
class Parcel;
class MetaDataBase {
public:
    MetaDataBase();
    MetaDataBase(const MetaDataBase &from);
    MetaDataBase& operator = (const MetaDataBase &);
    virtual ~MetaDataBase();
    enum Type {
        TYPE_NONE = 'none',
        TYPE_C_STRING = 'cstr',
        TYPE_INT32 = 'in32',
        TYPE_INT64 = 'in64',
        TYPE_FLOAT = 'floa',
        TYPE_POINTER = 'ptr ',
        TYPE_RECT = 'rect',
    };
    void clear();
    bool remove(uint32_t key);
    bool setCString(uint32_t key, const char *value);
    bool setInt32(uint32_t key, int32_t value);
    bool setInt64(uint32_t key, int64_t value);
    bool setFloat(uint32_t key, float value);
    bool setPointer(uint32_t key, void *value);
    bool setRect(
            uint32_t key,
            int32_t left, int32_t top,
            int32_t right, int32_t bottom);
    bool findCString(uint32_t key, const char **value) const;
    bool findInt32(uint32_t key, int32_t *value) const;
    bool findInt64(uint32_t key, int64_t *value) const;
    bool findFloat(uint32_t key, float *value) const;
    bool findPointer(uint32_t key, void **value) const;
    bool findRect(
            uint32_t key,
            int32_t *left, int32_t *top,
            int32_t *right, int32_t *bottom) const;
    bool setData(uint32_t key, uint32_t type, const void *data, size_t size);
    bool findData(uint32_t key, uint32_t *type,
                  const void **data, size_t *size) const;
    bool hasData(uint32_t key) const;
    String8 toString() const;
    void dumpToLog() const;
private:
    friend class BpMediaSource;
    friend class BnMediaSource;
    friend class BnMediaExtractor;
    friend class MetaData;
    struct typed_data;
    struct Rect;
    struct MetaDataInternal;
    MetaDataInternal *mInternalData;
#ifndef __ANDROID_VNDK__
    status_t writeToParcel(Parcel &parcel);
    status_t updateFromParcel(const Parcel &parcel);
#endif
};
}
#endif
