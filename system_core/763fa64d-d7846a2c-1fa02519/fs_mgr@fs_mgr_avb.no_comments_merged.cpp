#include "fs_mgr_avb.h"
#include <fcntl.h>
#include <libgen.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sstream>
#include <string>
#include <vector>
#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <libavb/libavb.h>
#include "fs_mgr.h"
#include "fs_mgr_priv.h"
#include "fs_mgr_priv_avb_ops.h"
#include "fs_mgr_priv_dm_ioctl.h"
#include "fs_mgr_priv_sha.h"
static inline bool nibble_value(const char& c, uint8_t* value) {
    FS_MGR_CHECK(value != nullptr);
    switch (c) {
        case '0' ... '9':
            *value = c - '0';
            break;
        case 'a' ... 'f':
            *value = c - 'a' + 10;
            break;
        case 'A' ... 'F':
            *value = c - 'A' + 10;
            break;
        default:
            return false;
    }
    return true;
}
static bool hex_to_bytes(uint8_t* bytes, size_t bytes_len, const std::string& hex) {
    FS_MGR_CHECK(bytes != nullptr);
    if (hex.size() % 2 != 0) {
        return false;
    }
    if (hex.size() / 2 > bytes_len) {
        return false;
    }
    for (size_t i = 0, j = 0, n = hex.size(); i < n; i += 2, ++j) {
        uint8_t high;
        if (!nibble_value(hex[i], &high)) {
            return false;
        }
        uint8_t low;
        if (!nibble_value(hex[i + 1], &low)) {
            return false;
        }
        bytes[j] = (high << 4) | low;
    }
    return true;
}
static std::string bytes_to_hex(const uint8_t* bytes, size_t bytes_len) {
    FS_MGR_CHECK(bytes != nullptr);
    static const char* hex_digits = "0123456789abcdef";
    std::string hex;
    for (size_t i = 0; i < bytes_len; i++) {
        hex.push_back(hex_digits[(bytes[i] & 0xF0) >> 4]);
        hex.push_back(hex_digits[bytes[i] & 0x0F]);
    }
    return hex;
}
template <typename Hasher>
static std::pair<size_t, bool> verify_vbmeta_digest(const AvbSlotVerifyData& verify_data,
                                                    const uint8_t* expected_digest) {
    size_t total_size = 0;
    Hasher hasher;
    for (size_t n = 0; n < verify_data.num_vbmeta_images; n++) {
        hasher.update(verify_data.vbmeta_images[n].vbmeta_data,
                      verify_data.vbmeta_images[n].vbmeta_size);
        total_size += verify_data.vbmeta_images[n].vbmeta_size;
    }
    bool matched = (memcmp(hasher.finalize(), expected_digest, Hasher::DIGEST_SIZE) == 0);
    return std::make_pair(total_size, matched);
}
class FsManagerAvbVerifier {
  public:
    static std::unique_ptr<FsManagerAvbVerifier> Create();
    bool VerifyVbmetaImages(const AvbSlotVerifyData& verify_data);
    bool IsDeviceUnlocked() { return is_device_unlocked_; }
  protected:
    FsManagerAvbVerifier() = default;
  private:
    enum HashAlgorithm {
        kInvalid = 0,
        kSHA256 = 1,
        kSHA512 = 2,
    };
    HashAlgorithm hash_alg_;
    uint8_t digest_[SHA512_DIGEST_LENGTH];
    size_t vbmeta_size_;
    bool is_device_unlocked_;
};
std::unique_ptr<FsManagerAvbVerifier> FsManagerAvbVerifier::Create() {
    std::string cmdline;
    if (!android::base::ReadFileToString("/proc/cmdline", &cmdline)) {
        PERROR << "Failed to read /proc/cmdline";
        return nullptr;
    }
    std::unique_ptr<FsManagerAvbVerifier> avb_verifier(new FsManagerAvbVerifier());
    if (!avb_verifier) {
        LERROR << "Failed to create unique_ptr<FsManagerAvbVerifier>";
        return nullptr;
    }
    std::string digest;
    std::string hash_alg;
    for (const auto& entry : android::base::Split(android::base::Trim(cmdline), " ")) {
        std::vector<std::string> pieces = android::base::Split(entry, "=");
        const std::string& key = pieces[0];
        const std::string& value = pieces[1];
        if (key == "androidboot.vbmeta.device_state") {
            avb_verifier->is_device_unlocked_ = (value == "unlocked");
        } else if (key == "androidboot.vbmeta.hash_alg") {
            hash_alg = value;
        } else if (key == "androidboot.vbmeta.size") {
            if (!android::base::ParseUint(value.c_str(), &avb_verifier->vbmeta_size_)) {
                return nullptr;
            }
        } else if (key == "androidboot.vbmeta.digest") {
            digest = value;
        }
    }
    size_t expected_digest_size = 0;
    if (hash_alg == "sha256") {
        expected_digest_size = SHA256_DIGEST_LENGTH * 2;
        avb_verifier->hash_alg_ = kSHA256;
    } else if (hash_alg == "sha512") {
        expected_digest_size = SHA512_DIGEST_LENGTH * 2;
        avb_verifier->hash_alg_ = kSHA512;
    } else {
        LERROR << "Unknown hash algorithm: " << hash_alg.c_str();
        return nullptr;
    }
    if (digest.size() != expected_digest_size) {
        LERROR << "Unexpected digest size: " << digest.size()
               << " (expected: " << expected_digest_size << ")";
        return nullptr;
    }
    if (!hex_to_bytes(avb_verifier->digest_, sizeof(avb_verifier->digest_), digest)) {
        LERROR << "Hash digest contains non-hexidecimal character: " << digest.c_str();
        return nullptr;
    }
    return avb_verifier;
}
bool FsManagerAvbVerifier::VerifyVbmetaImages(const AvbSlotVerifyData& verify_data) {
    if (verify_data.num_vbmeta_images == 0) {
        LERROR << "No vbmeta images";
        return false;
    }
    size_t total_size = 0;
    bool digest_matched = false;
    if (hash_alg_ == kSHA256) {
        std::tie(total_size, digest_matched) =
            verify_vbmeta_digest<SHA256Hasher>(verify_data, digest_);
    } else if (hash_alg_ == kSHA512) {
        std::tie(total_size, digest_matched) =
            verify_vbmeta_digest<SHA512Hasher>(verify_data, digest_);
    }
    if (total_size != vbmeta_size_) {
        LERROR << "total vbmeta size mismatch: " << total_size << " (expected: " << vbmeta_size_
               << ")";
        return false;
    }
    if (!digest_matched) {
        LERROR << "vbmeta digest mismatch";
        return false;
    }
    return true;
}
static std::string construct_verity_table(const AvbHashtreeDescriptor& hashtree_desc,
                                          const std::string& salt, const std::string& root_digest,
                                          const std::string& blk_device) {
    std::string verity_mode;
    if (!fs_mgr_get_boot_config("veritymode", &verity_mode)) {
        verity_mode = "enforcing";
    }
    std::string dm_verity_mode;
    if (verity_mode == "enforcing") {
        dm_verity_mode = "restart_on_corruption";
    } else if (verity_mode == "logging") {
        dm_verity_mode = "ignore_corruption";
    } else if (verity_mode != "eio") {
        LERROR << "Unknown androidboot.veritymode: " << verity_mode;
        return "";
    }
    std::ostringstream verity_table;
    verity_table << hashtree_desc.dm_verity_version << " " << blk_device << " " << blk_device << " "
                 << hashtree_desc.data_block_size << " " << hashtree_desc.hash_block_size << " "
                 << hashtree_desc.image_size / hashtree_desc.data_block_size << " "
                 << hashtree_desc.tree_offset / hashtree_desc.hash_block_size << " "
                 << hashtree_desc.hash_algorithm << " " << root_digest << " " << salt;
    int optional_argc = 0;
    std::ostringstream optional_args;
    if (hashtree_desc.fec_size > 0) {
        optional_argc += 8;
        optional_args << "use_fec_from_device " << blk_device
                      << " fec_roots " << hashtree_desc.fec_num_roots
                      << " fec_blocks " << hashtree_desc.fec_offset / hashtree_desc.data_block_size
                      << " fec_start " << hashtree_desc.fec_offset / hashtree_desc.data_block_size
                      << " ";
    }
    if (!dm_verity_mode.empty()) {
        optional_argc += 1;
        optional_args << dm_verity_mode << " ";
    }
    optional_argc += 1;
    optional_args << "ignore_zero_blocks";
    verity_table << " " << optional_argc << " " << optional_args.str();
    return verity_table.str();
}
static bool load_verity_table(struct dm_ioctl* io, const std::string& dm_device_name, int fd,
                              uint64_t image_size, const std::string& verity_table) {
    fs_mgr_verity_ioctl_init(io, dm_device_name, DM_STATUS_TABLE_FLAG);
    char* buffer = (char*)io;
    struct dm_target_spec* dm_target = (struct dm_target_spec*)&buffer[sizeof(struct dm_ioctl)];
    io->target_count = 1;
    dm_target->status = 0;
    dm_target->sector_start = 0;
    dm_target->length = image_size / 512;
    strcpy(dm_target->target_type, "verity");
    char* verity_params = buffer + sizeof(struct dm_ioctl) + sizeof(struct dm_target_spec);
    size_t bufsize = DM_BUF_SIZE - (verity_params - buffer);
    LINFO << "Loading verity table: '" << verity_table << "'";
    if (verity_table.size() > bufsize - 1) {
        LERROR << "Verity table size too large: " << verity_table.size()
               << " (max allowable size: " << bufsize - 1 << ")";
        return false;
    }
    memcpy(verity_params, verity_table.c_str(), verity_table.size() + 1);
    verity_params += verity_table.size() + 1;
    verity_params = (char*)(((unsigned long)verity_params + 7) & ~7);
    dm_target->next = verity_params - buffer;
    if (ioctl(fd, DM_TABLE_LOAD, io)) {
        PERROR << "Error loading verity table";
        return false;
    }
    return true;
}
static bool hashtree_dm_verity_setup(struct fstab_rec* fstab_entry,
                                     const AvbHashtreeDescriptor& hashtree_desc,
                                     const std::string& salt, const std::string& root_digest,
                                     bool wait_for_verity_dev) {
    android::base::unique_fd fd(open("/dev/device-mapper", O_RDWR));
    if (fd < 0) {
        PERROR << "Error opening device mapper";
        return false;
    }
    alignas(dm_ioctl) char buffer[DM_BUF_SIZE];
    struct dm_ioctl* io = (struct dm_ioctl*)buffer;
    const std::string mount_point(basename(fstab_entry->mount_point));
    if (!fs_mgr_create_verity_device(io, mount_point, fd)) {
        LERROR << "Couldn't create verity device!";
        return false;
    }
    std::string verity_blk_name;
    if (!fs_mgr_get_verity_device_name(io, mount_point, fd, &verity_blk_name)) {
        LERROR << "Couldn't get verity device number!";
        return false;
    }
    std::string verity_table =
        construct_verity_table(hashtree_desc, salt, root_digest, fstab_entry->blk_device);
    if (verity_table.empty()) {
        LERROR << "Failed to construct verity table.";
        return false;
    }
    if (!load_verity_table(io, mount_point, fd, hashtree_desc.image_size, verity_table)) {
        LERROR << "Couldn't load verity table!";
        return false;
    }
    if (!fs_mgr_resume_verity_table(io, mount_point, fd)) {
        return false;
    }
    fs_mgr_set_blk_ro(fstab_entry->blk_device);
    free(fstab_entry->blk_device);
    fstab_entry->blk_device = strdup(verity_blk_name.c_str());
    if (wait_for_verity_dev && fs_mgr_test_access(verity_blk_name.c_str()) < 0) {
        return false;
    }
    return true;
}
static bool get_hashtree_descriptor(const std::string& partition_name,
                                    const AvbSlotVerifyData& verify_data,
                                    AvbHashtreeDescriptor* out_hashtree_desc, std::string* out_salt,
                                    std::string* out_digest) {
    bool found = false;
    const uint8_t* desc_partition_name;
    for (size_t i = 0; i < verify_data.num_vbmeta_images && !found; i++) {
        size_t num_descriptors;
        std::unique_ptr<const AvbDescriptor* [], decltype(&avb_free)> descriptors(
            avb_descriptor_get_all(verify_data.vbmeta_images[i].vbmeta_data,
                                   verify_data.vbmeta_images[i].vbmeta_size, &num_descriptors),
            avb_free);
        if (!descriptors || num_descriptors < 1) {
            continue;
        }
        std::string vbmeta_partition_name(verify_data.vbmeta_images[i].partition_name);
        if (vbmeta_partition_name != "vbmeta" &&
            vbmeta_partition_name != "boot" &&
            vbmeta_partition_name != partition_name) {
            LWARNING << "Skip vbmeta image at " << verify_data.vbmeta_images[i].partition_name
                     << " for partition: " << partition_name.c_str();
            continue;
        }
        for (size_t j = 0; j < num_descriptors && !found; j++) {
            AvbDescriptor desc;
            if (!avb_descriptor_validate_and_byteswap(descriptors[j], &desc)) {
                LWARNING << "Descriptor[" << j << "] is invalid";
                continue;
            }
            if (desc.tag == AVB_DESCRIPTOR_TAG_HASHTREE) {
                desc_partition_name = (const uint8_t*)descriptors[j] + sizeof(AvbHashtreeDescriptor);
                if (!avb_hashtree_descriptor_validate_and_byteswap(
                        (AvbHashtreeDescriptor*)descriptors[j], out_hashtree_desc)) {
                    continue;
                }
                if (out_hashtree_desc->partition_name_len != partition_name.length()) {
                    continue;
                }
                std::string hashtree_partition_name((const char*)desc_partition_name,
                                                    out_hashtree_desc->partition_name_len);
                if (hashtree_partition_name == partition_name) {
                    found = true;
                }
            }
        }
    }
    if (!found) {
        LERROR << "Partition descriptor not found: " << partition_name.c_str();
        return false;
    }
    const uint8_t* desc_salt = desc_partition_name + out_hashtree_desc->partition_name_len;
    *out_salt = bytes_to_hex(desc_salt, out_hashtree_desc->salt_len);
    const uint8_t* desc_digest = desc_salt + out_hashtree_desc->salt_len;
    *out_digest = bytes_to_hex(desc_digest, out_hashtree_desc->root_digest_len);
    return true;
}
FsManagerAvbUniquePtr FsManagerAvbHandle::Open(const fstab& fstab) {
    FsManagerAvbOps avb_ops(fstab);
    return DoOpen(&avb_ops);
}
FsManagerAvbUniquePtr FsManagerAvbHandle::Open(ByNameSymlinkMap&& by_name_symlink_map) {
    if (by_name_symlink_map.empty()) {
        LERROR << "Empty by_name_symlink_map when opening FsManagerAvbHandle";
        return nullptr;
    }
    FsManagerAvbOps avb_ops(std::move(by_name_symlink_map));
    return DoOpen(&avb_ops);
}
FsManagerAvbUniquePtr FsManagerAvbHandle::DoOpen(FsManagerAvbOps* avb_ops) {
    std::unique_ptr<FsManagerAvbVerifier> avb_verifier = FsManagerAvbVerifier::Create();
    if (!avb_verifier) {
        LERROR << "Failed to create FsManagerAvbVerifier";
        return nullptr;
    }
    FsManagerAvbUniquePtr avb_handle(new FsManagerAvbHandle());
    if (!avb_handle) {
        LERROR << "Failed to allocate FsManagerAvbHandle";
        return nullptr;
    }
    AvbSlotVerifyFlags flags = avb_verifier->IsDeviceUnlocked()
                                   ? AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR
                                   : AVB_SLOT_VERIFY_FLAGS_NONE;
    AvbSlotVerifyResult verify_result =
        avb_ops->AvbSlotVerify(fs_mgr_get_slot_suffix(), flags, &avb_handle->avb_slot_data_);
    switch (verify_result) {
        case AVB_SLOT_VERIFY_RESULT_OK:
            avb_handle->status_ = kFsManagerAvbHandleSuccess;
            break;
        case AVB_SLOT_VERIFY_RESULT_ERROR_VERIFICATION:
            if (!avb_verifier->IsDeviceUnlocked()) {
                LERROR << "ERROR_VERIFICATION isn't allowed when the device is LOCKED";
                return nullptr;
            }
            avb_handle->status_ = kFsManagerAvbHandleErrorVerification;
            break;
        default:
            LERROR << "avb_slot_verify failed, result: " << verify_result;
            return nullptr;
    }
    if (!avb_verifier->VerifyVbmetaImages(*avb_handle->avb_slot_data_)) {
        LERROR << "VerifyVbmetaImages failed";
        return nullptr;
    }
    avb_handle->avb_version_ =
        android::base::StringPrintf("%d.%d", AVB_VERSION_MAJOR, AVB_VERSION_MINOR);
    AvbVBMetaImageHeader vbmeta_header;
    avb_vbmeta_image_header_to_host_byte_order(
        (AvbVBMetaImageHeader*)avb_handle->avb_slot_data_->vbmeta_images[0].vbmeta_data,
        &vbmeta_header);
    bool hashtree_disabled =
        ((AvbVBMetaImageFlags)vbmeta_header.flags & AVB_VBMETA_IMAGE_FLAGS_HASHTREE_DISABLED);
    if (hashtree_disabled) {
        avb_handle->status_ = kFsManagerAvbHandleHashtreeDisabled;
    }
    LINFO << "Returning avb_handle with status: " << avb_handle->status_;
    return avb_handle;
}
bool FsManagerAvbHandle::SetUpAvb(struct fstab_rec* fstab_entry, bool wait_for_verity_dev) {
    if (!fstab_entry) return false;
    if (!avb_slot_data_ || avb_slot_data_->num_vbmeta_images < 1) {
        return false;
    }
    if (status_ == kFsManagerAvbHandleUninitialized) return false;
    if (status_ == kFsManagerAvbHandleHashtreeDisabled) {
        LINFO << "AVB HASHTREE disabled on:" << fstab_entry->mount_point;
        return true;
    }
    std::string partition_name(basename(fstab_entry->mount_point));
    if (!avb_validate_utf8((const uint8_t*)partition_name.c_str(), partition_name.length())) {
        LERROR << "Partition name: " << partition_name.c_str() << " is not valid UTF-8.";
        return false;
    }
    AvbHashtreeDescriptor hashtree_descriptor;
    std::string salt;
    std::string root_digest;
    if (!get_hashtree_descriptor(partition_name, *avb_slot_data_, &hashtree_descriptor, &salt,
                                 &root_digest)) {
        return false;
    }
    if (!hashtree_dm_verity_setup(fstab_entry, hashtree_descriptor, salt, root_digest,
                                  wait_for_verity_dev)) {
        return false;
    }
    return true;
}
