/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <libgsi/libgsi.h>

#include "fs_mgr_priv.h"

using android::base::ParseByteCount;
using android::base::ParseInt;
using android::base::Split;
using android::base::StartsWith;

namespace android {
namespace fs_mgr {
namespace {

const std::string kDefaultAndroidDtDir("/proc/device-tree/firmware/android");

struct FlagList {
    const char *name;
    uint64_t flag;
};

FlagList kMountFlagsList[] = {
        {"noatime", MS_NOATIME},
        {"noexec", MS_NOEXEC},
        {"nosuid", MS_NOSUID},
        {"nodev", MS_NODEV},
        {"nodiratime", MS_NODIRATIME},
        {"ro", MS_RDONLY},
        {"rw", 0},
        {"remount", MS_REMOUNT},
        {"bind", MS_BIND},
        {"rec", MS_REC},
        {"unbindable", MS_UNBINDABLE},
        {"private", MS_PRIVATE},
        {"slave", MS_SLAVE},
        {"shared", MS_SHARED},
        {"defaults", 0},
};

<<<<<<< HEAD
off64_t CalculateZramSize(int percentage) {
    off64_t total;
||||||| 83cda925e
static struct flag_list fs_mgr_flags[] = {
    {"wait", MF_WAIT},
    {"check", MF_CHECK},
    {"encryptable=", MF_CRYPT},
    {"forceencrypt=", MF_FORCECRYPT},
    {"fileencryption=", MF_FILEENCRYPTION},
    {"forcefdeorfbe=", MF_FORCEFDEORFBE},
    {"keydirectory=", MF_KEYDIRECTORY},
    {"nonremovable", MF_NONREMOVABLE},
    {"voldmanaged=", MF_VOLDMANAGED},
    {"length=", MF_LENGTH},
    {"recoveryonly", MF_RECOVERYONLY},
    {"swapprio=", MF_SWAPPRIO},
    {"zramsize=", MF_ZRAMSIZE},
    {"max_comp_streams=", MF_MAX_COMP_STREAMS},
    {"verifyatboot", MF_VERIFYATBOOT},
    {"verify", MF_VERIFY},
    {"avb", MF_AVB},
    {"noemulatedsd", MF_NOEMULATEDSD},
    {"notrim", MF_NOTRIM},
    {"formattable", MF_FORMATTABLE},
    {"slotselect", MF_SLOTSELECT},
    {"nofail", MF_NOFAIL},
    {"latemount", MF_LATEMOUNT},
    {"reservedsize=", MF_RESERVEDSIZE},
    {"quota", MF_QUOTA},
    {"eraseblk=", MF_ERASEBLKSIZE},
    {"logicalblk=", MF_LOGICALBLKSIZE},
    {"sysfs_path=", MF_SYSFS},
    {"defaults", 0},
    {0, 0},
};

#define EM_AES_256_XTS  1
#define EM_ICE          2
#define EM_AES_256_CTS  3
#define EM_AES_256_HEH  4
#define EM_ADIANTUM     5

static const struct flag_list file_contents_encryption_modes[] = {
    {"aes-256-xts", EM_AES_256_XTS},
    {"adiantum", EM_ADIANTUM},
    {"software", EM_AES_256_XTS}, /* alias for backwards compatibility */
    {"ice", EM_ICE}, /* hardware-specific inline cryptographic engine */
    {0, 0},
};

static const struct flag_list file_names_encryption_modes[] = {
    {"aes-256-cts", EM_AES_256_CTS},
    {"aes-256-heh", EM_AES_256_HEH},
    {"adiantum", EM_ADIANTUM},
    {0, 0},
};

static unsigned int encryption_mode_to_flag(const struct flag_list *list,
                                            const char *mode, const char *type)
{
    const struct flag_list *j;

    for (j = list; j->name; ++j) {
        if (!strcmp(mode, j->name)) {
            return j->flag;
        }
    }
    LERROR << "Unknown " << type << " encryption mode: " << mode;
    return 0;
}

static const char *flag_to_encryption_mode(const struct flag_list *list,
                                           unsigned int flag)
{
    const struct flag_list *j;

    for (j = list; j->name; ++j) {
        if (flag == j->flag) {
            return j->name;
        }
    }
    return nullptr;
}

static uint64_t calculate_zram_size(unsigned int percentage)
{
    uint64_t total;
=======
static struct flag_list fs_mgr_flags[] = {
    {"wait", MF_WAIT},
    {"check", MF_CHECK},
    {"encryptable=", MF_CRYPT},
    {"forceencrypt=", MF_FORCECRYPT},
    {"fileencryption=", MF_FILEENCRYPTION},
    {"forcefdeorfbe=", MF_FORCEFDEORFBE},
    {"keydirectory=", MF_KEYDIRECTORY},
    {"nonremovable", MF_NONREMOVABLE},
    {"voldmanaged=", MF_VOLDMANAGED},
    {"length=", MF_LENGTH},
    {"recoveryonly", MF_RECOVERYONLY},
    {"swapprio=", MF_SWAPPRIO},
    {"zramsize=", MF_ZRAMSIZE},
    {"max_comp_streams=", MF_MAX_COMP_STREAMS},
    {"verifyatboot", MF_VERIFYATBOOT},
    {"verify", MF_VERIFY},
    {"avb", MF_AVB},
    {"noemulatedsd", MF_NOEMULATEDSD},
    {"notrim", MF_NOTRIM},
    {"formattable", MF_FORMATTABLE},
    {"slotselect", MF_SLOTSELECT},
    {"nofail", MF_NOFAIL},
    {"latemount", MF_LATEMOUNT},
    {"reservedsize=", MF_RESERVEDSIZE},
    {"quota", MF_QUOTA},
    {"eraseblk=", MF_ERASEBLKSIZE},
    {"logicalblk=", MF_LOGICALBLKSIZE},
    {"sysfs_path=", MF_SYSFS},
    {"defaults", 0},
    {0, 0},
};

#define EM_AES_256_XTS  1
#define EM_ICE          2
#define EM_AES_256_CTS  3
#define EM_AES_256_HEH  4

static const struct flag_list file_contents_encryption_modes[] = {
    {"aes-256-xts", EM_AES_256_XTS},
    {"software", EM_AES_256_XTS}, /* alias for backwards compatibility */
    {"ice", EM_ICE}, /* hardware-specific inline cryptographic engine */
    {0, 0},
};

static const struct flag_list file_names_encryption_modes[] = {
    {"aes-256-cts", EM_AES_256_CTS},
    {"aes-256-heh", EM_AES_256_HEH},
    {0, 0},
};

static unsigned int encryption_mode_to_flag(const struct flag_list *list,
                                            const char *mode, const char *type)
{
    const struct flag_list *j;

    for (j = list; j->name; ++j) {
        if (!strcmp(mode, j->name)) {
            return j->flag;
        }
    }
    LERROR << "Unknown " << type << " encryption mode: " << mode;
    return 0;
}

static const char *flag_to_encryption_mode(const struct flag_list *list,
                                           unsigned int flag)
{
    const struct flag_list *j;

    for (j = list; j->name; ++j) {
        if (flag == j->flag) {
            return j->name;
        }
    }
    return nullptr;
}

static uint64_t calculate_zram_size(unsigned int percentage)
{
    uint64_t total;
>>>>>>> 5d1d32dc

    total  = sysconf(_SC_PHYS_PAGES);
    total *= percentage;
    total /= 100;

    total *= sysconf(_SC_PAGESIZE);

    return total;
}

// Fills 'dt_value' with the underlying device tree value string without the trailing '\0'.
// Returns true if 'dt_value' has a valid string, 'false' otherwise.
bool ReadDtFile(const std::string& file_name, std::string* dt_value) {
    if (android::base::ReadFileToString(file_name, dt_value)) {
        if (!dt_value->empty()) {
            // Trim the trailing '\0' out, otherwise the comparison will produce false-negatives.
            dt_value->resize(dt_value->size() - 1);
            return true;
        }
    }

    return false;
}

const std::array<const char*, 3> kFileContentsEncryptionMode = {
        "aes-256-xts",
        "adiantum",
        "ice",
};

const std::array<const char*, 3> kFileNamesEncryptionMode = {
        "aes-256-cts",
        "aes-256-heh",
        "adiantum",
};

void ParseFileEncryption(const std::string& arg, FstabEntry* entry) {
    // The fileencryption flag is followed by an = and the mode of contents encryption, then
    // optionally a and the mode of filenames encryption (defaults to aes-256-cts).  Get it and
    // return it.
    entry->fs_mgr_flags.file_encryption = true;

    auto parts = Split(arg, ":");
    if (parts.empty() || parts.size() > 2) {
        LWARNING << "Warning: fileencryption= flag malformed: " << arg;
        return;
    }

    // Alias for backwards compatibility.
    if (parts[0] == "software") {
        parts[0] = "aes-256-xts";
    }

<<<<<<< HEAD
    if (std::find(kFileContentsEncryptionMode.begin(), kFileContentsEncryptionMode.end(),
                  parts[0]) == kFileContentsEncryptionMode.end()) {
        LWARNING << "fileencryption= flag malformed, file contents encryption mode not found: "
                 << arg;
        return;
    }
||||||| 83cda925e
    p = strtok_r(flags, ",", &savep);
    while (p) {
        /* Look for the flag "p" in the flag list "fl"
         * If not found, the loop exits with fl[i].name being null.
         */
        for (i = 0; fl[i].name; i++) {
            if (!strncmp(p, fl[i].name, strlen(fl[i].name))) {
                f |= fl[i].flag;
                if ((fl[i].flag == MF_CRYPT) && flag_vals) {
                    /* The encryptable flag is followed by an = and the
                     * location of the keys.  Get it and return it.
                     */
                    flag_vals->key_loc = strdup(strchr(p, '=') + 1);
                } else if ((fl[i].flag == MF_VERIFY) && flag_vals) {
                    /* If the verify flag is followed by an = and the
                     * location for the verity state,  get it and return it.
                     */
                    char *start = strchr(p, '=');
                    if (start) {
                        flag_vals->verity_loc = strdup(start + 1);
                    }
                } else if ((fl[i].flag == MF_FORCECRYPT) && flag_vals) {
                    /* The forceencrypt flag is followed by an = and the
                     * location of the keys.  Get it and return it.
                     */
                    flag_vals->key_loc = strdup(strchr(p, '=') + 1);
                } else if ((fl[i].flag == MF_FORCEFDEORFBE) && flag_vals) {
                    /* The forcefdeorfbe flag is followed by an = and the
                     * location of the keys.  Get it and return it.
                     */
                    flag_vals->key_loc = strdup(strchr(p, '=') + 1);
                    flag_vals->file_contents_mode = EM_AES_256_XTS;
                    flag_vals->file_names_mode = EM_AES_256_CTS;
                } else if ((fl[i].flag == MF_FILEENCRYPTION) && flag_vals) {
                    /* The fileencryption flag is followed by an = and
                     * the mode of contents encryption, then optionally a
                     * : and the mode of filenames encryption (defaults
                     * to aes-256-cts).  Get it and return it.
                     */
                    char *mode = strchr(p, '=') + 1;
                    char *colon = strchr(mode, ':');
                    if (colon) {
                        *colon = '\0';
                    }
                    flag_vals->file_contents_mode =
                        encryption_mode_to_flag(file_contents_encryption_modes,
                                                mode, "file contents");
                    if (colon) {
                        flag_vals->file_names_mode =
                            encryption_mode_to_flag(file_names_encryption_modes,
                                                    colon + 1, "file names");
                    } else if (flag_vals->file_contents_mode == EM_ADIANTUM) {
                        flag_vals->file_names_mode = EM_ADIANTUM;
                    } else {
                        flag_vals->file_names_mode = EM_AES_256_CTS;
                    }
                } else if ((fl[i].flag == MF_KEYDIRECTORY) && flag_vals) {
                    /* The metadata flag is followed by an = and the
                     * directory for the keys.  Get it and return it.
                     */
                    flag_vals->key_dir = strdup(strchr(p, '=') + 1);
                } else if ((fl[i].flag == MF_LENGTH) && flag_vals) {
                    /* The length flag is followed by an = and the
                     * size of the partition.  Get it and return it.
                     */
                    flag_vals->part_length = strtoll(strchr(p, '=') + 1, NULL, 0);
                } else if ((fl[i].flag == MF_VOLDMANAGED) && flag_vals) {
                    /* The voldmanaged flag is followed by an = and the
                     * label, a colon and the partition number or the
                     * word "auto", e.g.
                     *   voldmanaged=sdcard:3
                     * Get and return them.
                     */
                    char *label_start;
                    char *label_end;
                    char *part_start;
=======
    p = strtok_r(flags, ",", &savep);
    while (p) {
        /* Look for the flag "p" in the flag list "fl"
         * If not found, the loop exits with fl[i].name being null.
         */
        for (i = 0; fl[i].name; i++) {
            if (!strncmp(p, fl[i].name, strlen(fl[i].name))) {
                f |= fl[i].flag;
                if ((fl[i].flag == MF_CRYPT) && flag_vals) {
                    /* The encryptable flag is followed by an = and the
                     * location of the keys.  Get it and return it.
                     */
                    flag_vals->key_loc = strdup(strchr(p, '=') + 1);
                } else if ((fl[i].flag == MF_VERIFY) && flag_vals) {
                    /* If the verify flag is followed by an = and the
                     * location for the verity state,  get it and return it.
                     */
                    char *start = strchr(p, '=');
                    if (start) {
                        flag_vals->verity_loc = strdup(start + 1);
                    }
                } else if ((fl[i].flag == MF_FORCECRYPT) && flag_vals) {
                    /* The forceencrypt flag is followed by an = and the
                     * location of the keys.  Get it and return it.
                     */
                    flag_vals->key_loc = strdup(strchr(p, '=') + 1);
                } else if ((fl[i].flag == MF_FORCEFDEORFBE) && flag_vals) {
                    /* The forcefdeorfbe flag is followed by an = and the
                     * location of the keys.  Get it and return it.
                     */
                    flag_vals->key_loc = strdup(strchr(p, '=') + 1);
                    flag_vals->file_contents_mode = EM_AES_256_XTS;
                    flag_vals->file_names_mode = EM_AES_256_CTS;
                } else if ((fl[i].flag == MF_FILEENCRYPTION) && flag_vals) {
                    /* The fileencryption flag is followed by an = and
                     * the mode of contents encryption, then optionally a
                     * : and the mode of filenames encryption (defaults
                     * to aes-256-cts).  Get it and return it.
                     */
                    char *mode = strchr(p, '=') + 1;
                    char *colon = strchr(mode, ':');
                    if (colon) {
                        *colon = '\0';
                    }
                    flag_vals->file_contents_mode =
                        encryption_mode_to_flag(file_contents_encryption_modes,
                                                mode, "file contents");
                    if (colon) {
                        flag_vals->file_names_mode =
                            encryption_mode_to_flag(file_names_encryption_modes,
                                                    colon + 1, "file names");
                    } else {
                        flag_vals->file_names_mode = EM_AES_256_CTS;
                    }
                } else if ((fl[i].flag == MF_KEYDIRECTORY) && flag_vals) {
                    /* The metadata flag is followed by an = and the
                     * directory for the keys.  Get it and return it.
                     */
                    flag_vals->key_dir = strdup(strchr(p, '=') + 1);
                } else if ((fl[i].flag == MF_LENGTH) && flag_vals) {
                    /* The length flag is followed by an = and the
                     * size of the partition.  Get it and return it.
                     */
                    flag_vals->part_length = strtoll(strchr(p, '=') + 1, NULL, 0);
                } else if ((fl[i].flag == MF_VOLDMANAGED) && flag_vals) {
                    /* The voldmanaged flag is followed by an = and the
                     * label, a colon and the partition number or the
                     * word "auto", e.g.
                     *   voldmanaged=sdcard:3
                     * Get and return them.
                     */
                    char *label_start;
                    char *label_end;
                    char *part_start;
>>>>>>> 5d1d32dc

    entry->file_contents_mode = parts[0];

    if (parts.size() == 2) {
        if (std::find(kFileNamesEncryptionMode.begin(), kFileNamesEncryptionMode.end(), parts[1]) ==
            kFileNamesEncryptionMode.end()) {
            LWARNING << "fileencryption= flag malformed, file names encryption mode not found: "
                     << arg;
            return;
        }

        entry->file_names_mode = parts[1];
    } else if (entry->file_contents_mode == "adiantum") {
        entry->file_names_mode = "adiantum";
    } else {
        entry->file_names_mode = "aes-256-cts";
    }
}

bool SetMountFlag(const std::string& flag, FstabEntry* entry) {
    for (const auto& [name, value] : kMountFlagsList) {
        if (flag == name) {
            entry->flags |= value;
            return true;
        }
    }
    return false;
}

void ParseMountFlags(const std::string& flags, FstabEntry* entry) {
    std::string fs_options;
    for (const auto& flag : Split(flags, ",")) {
        if (!SetMountFlag(flag, entry)) {
            // Unknown flag, so it must be a filesystem specific option.
            if (!fs_options.empty()) {
                fs_options.append(",");  // appends a comma if not the first
            }
            fs_options.append(flag);
        }
    }
    entry->fs_options = std::move(fs_options);
}

void ParseFsMgrFlags(const std::string& flags, FstabEntry* entry) {
    for (const auto& flag : Split(flags, ",")) {
        if (flag.empty() || flag == "defaults") continue;
        std::string arg;
        if (auto equal_sign = flag.find('='); equal_sign != std::string::npos) {
            arg = flag.substr(equal_sign + 1);
        }

        // First handle flags that simply set a boolean.
#define CheckFlag(flag_name, value)       \
    if (flag == flag_name) {              \
        entry->fs_mgr_flags.value = true; \
        continue;                         \
    }

        CheckFlag("wait", wait);
        CheckFlag("check", check);
        CheckFlag("nonremovable", nonremovable);
        CheckFlag("recoveryonly", recovery_only);
        CheckFlag("noemulatedsd", no_emulated_sd);
        CheckFlag("notrim", no_trim);
        CheckFlag("verify", verify);
        CheckFlag("formattable", formattable);
        CheckFlag("slotselect", slot_select);
        CheckFlag("latemount", late_mount);
        CheckFlag("nofail", no_fail);
        CheckFlag("verifyatboot", verify_at_boot);
        CheckFlag("quota", quota);
        CheckFlag("avb", avb);
        CheckFlag("logical", logical);
        CheckFlag("checkpoint=block", checkpoint_blk);
        CheckFlag("checkpoint=fs", checkpoint_fs);
        CheckFlag("first_stage_mount", first_stage_mount);
        CheckFlag("slotselect_other", slot_select_other);
        CheckFlag("fsverity", fs_verity);

#undef CheckFlag

        // Then handle flags that take an argument.
        if (StartsWith(flag, "encryptable=")) {
            // The encryptable flag is followed by an = and the  location of the keys.
            entry->fs_mgr_flags.crypt = true;
            entry->key_loc = arg;
        } else if (StartsWith(flag, "voldmanaged=")) {
            // The voldmanaged flag is followed by an = and the label, a colon and the partition
            // number or the word "auto", e.g. voldmanaged=sdcard:3
            entry->fs_mgr_flags.vold_managed = true;
            auto parts = Split(arg, ":");
            if (parts.size() != 2) {
                LWARNING << "Warning: voldmanaged= flag malformed: " << arg;
                continue;
            }

            entry->label = std::move(parts[0]);
            if (parts[1] == "auto") {
                entry->partnum = -1;
            } else {
                if (!ParseInt(parts[1], &entry->partnum)) {
                    entry->partnum = -1;
                    LWARNING << "Warning: voldmanaged= flag malformed: " << arg;
                    continue;
                }
            }
        } else if (StartsWith(flag, "length=")) {
            // The length flag is followed by an = and the size of the partition.
            if (!ParseInt(arg, &entry->length)) {
                LWARNING << "Warning: length= flag malformed: " << arg;
            }
        } else if (StartsWith(flag, "swapprio=")) {
            if (!ParseInt(arg, &entry->swap_prio)) {
                LWARNING << "Warning: length= flag malformed: " << arg;
            }
        } else if (StartsWith(flag, "zramsize=")) {
            if (!arg.empty() && arg.back() == '%') {
                arg.pop_back();
                int val;
                if (ParseInt(arg, &val, 0, 100)) {
                    entry->zram_size = CalculateZramSize(val);
                } else {
                    LWARNING << "Warning: zramsize= flag malformed: " << arg;
                }
            } else {
                if (!ParseInt(arg, &entry->zram_size)) {
                    LWARNING << "Warning: zramsize= flag malformed: " << arg;
                }
            }
        } else if (StartsWith(flag, "verify=")) {
            // If the verify flag is followed by an = and the location for the verity state.
            entry->fs_mgr_flags.verify = true;
            entry->verity_loc = arg;
        } else if (StartsWith(flag, "forceencrypt=")) {
            // The forceencrypt flag is followed by an = and the location of the keys.
            entry->fs_mgr_flags.force_crypt = true;
            entry->key_loc = arg;
        } else if (StartsWith(flag, "fileencryption=")) {
            ParseFileEncryption(arg, entry);
        } else if (StartsWith(flag, "forcefdeorfbe=")) {
            // The forcefdeorfbe flag is followed by an = and the location of the keys.  Get it and
            // return it.
            entry->fs_mgr_flags.force_fde_or_fbe = true;
            entry->key_loc = arg;
            entry->file_contents_mode = "aes-256-xts";
            entry->file_names_mode = "aes-256-cts";
        } else if (StartsWith(flag, "max_comp_streams=")) {
            if (!ParseInt(arg, &entry->max_comp_streams)) {
                LWARNING << "Warning: max_comp_streams= flag malformed: " << arg;
            }
        } else if (StartsWith(flag, "reservedsize=")) {
            // The reserved flag is followed by an = and the reserved size of the partition.
            uint64_t size;
            if (!ParseByteCount(arg, &size)) {
                LWARNING << "Warning: reservedsize= flag malformed: " << arg;
            } else {
                entry->reserved_size = static_cast<off64_t>(size);
            }
        } else if (StartsWith(flag, "eraseblk=")) {
            // The erase block size flag is followed by an = and the flash erase block size. Get it,
            // check that it is a power of 2 and at least 4096, and return it.
            off64_t val;
            if (!ParseInt(arg, &val) || val < 4096 || (val & (val - 1)) != 0) {
                LWARNING << "Warning: eraseblk= flag malformed: " << arg;
            } else {
                entry->erase_blk_size = val;
            }
        } else if (StartsWith(flag, "logicalblk=")) {
            // The logical block size flag is followed by an = and the flash logical block size. Get
            // it, check that it is a power of 2 and at least 4096, and return it.
            off64_t val;
            if (!ParseInt(arg, &val) || val < 4096 || (val & (val - 1)) != 0) {
                LWARNING << "Warning: logicalblk= flag malformed: " << arg;
            } else {
                entry->logical_blk_size = val;
            }
        } else if (StartsWith(flag, "avb_keys=")) {  // must before the following "avb"
            entry->avb_keys = arg;
        } else if (StartsWith(flag, "avb")) {
            entry->fs_mgr_flags.avb = true;
            entry->vbmeta_partition = arg;
        } else if (StartsWith(flag, "keydirectory=")) {
            // The metadata flag is followed by an = and the directory for the keys.
            entry->key_dir = arg;
        } else if (StartsWith(flag, "sysfs_path=")) {
            // The path to trigger device gc by idle-maint of vold.
            entry->sysfs_path = arg;
        } else if (StartsWith(flag, "zram_loopback_path=")) {
            // The path to use loopback for zram.
            entry->zram_loopback_path = arg;
        } else if (StartsWith(flag, "zram_loopback_size=")) {
            if (!ParseByteCount(arg, &entry->zram_loopback_size)) {
                LWARNING << "Warning: zram_loopback_size= flag malformed: " << arg;
            }
        } else if (StartsWith(flag, "zram_backing_dev_path=")) {
            entry->zram_backing_dev_path = arg;
        } else {
            LWARNING << "Warning: unknown flag: " << flag;
        }
    }
}

std::string InitAndroidDtDir() {
    std::string android_dt_dir;
    // The platform may specify a custom Android DT path in kernel cmdline
    if (!fs_mgr_get_boot_config_from_kernel_cmdline("android_dt_dir", &android_dt_dir)) {
        // Fall back to the standard procfs-based path
        android_dt_dir = kDefaultAndroidDtDir;
    }
    return android_dt_dir;
}

bool IsDtFstabCompatible() {
    std::string dt_value;
    std::string file_name = get_android_dt_dir() + "/fstab/compatible";

    if (ReadDtFile(file_name, &dt_value) && dt_value == "android,fstab") {
        // If there's no status property or its set to "ok" or "okay", then we use the DT fstab.
        std::string status_value;
        std::string status_file_name = get_android_dt_dir() + "/fstab/status";
        return !ReadDtFile(status_file_name, &status_value) || status_value == "ok" ||
               status_value == "okay";
    }

    return false;
}

std::string ReadFstabFromDt() {
    if (!is_dt_compatible() || !IsDtFstabCompatible()) {
        return {};
    }

    std::string fstabdir_name = get_android_dt_dir() + "/fstab";
    std::unique_ptr<DIR, int (*)(DIR*)> fstabdir(opendir(fstabdir_name.c_str()), closedir);
    if (!fstabdir) return {};

    dirent* dp;
    // Each element in fstab_dt_entries is <mount point, the line format in fstab file>.
    std::vector<std::pair<std::string, std::string>> fstab_dt_entries;
    while ((dp = readdir(fstabdir.get())) != NULL) {
        // skip over name, compatible and .
        if (dp->d_type != DT_DIR || dp->d_name[0] == '.') continue;

        // create <dev> <mnt_point>  <type>  <mnt_flags>  <fsmgr_flags>\n
        std::vector<std::string> fstab_entry;
        std::string file_name;
        std::string value;
        // skip a partition entry if the status property is present and not set to ok
        file_name = android::base::StringPrintf("%s/%s/status", fstabdir_name.c_str(), dp->d_name);
        if (ReadDtFile(file_name, &value)) {
            if (value != "okay" && value != "ok") {
                LINFO << "dt_fstab: Skip disabled entry for partition " << dp->d_name;
                continue;
            }
        }

        file_name = android::base::StringPrintf("%s/%s/dev", fstabdir_name.c_str(), dp->d_name);
        if (!ReadDtFile(file_name, &value)) {
            LERROR << "dt_fstab: Failed to find device for partition " << dp->d_name;
            return {};
        }
        fstab_entry.push_back(value);

        std::string mount_point;
        file_name =
            android::base::StringPrintf("%s/%s/mnt_point", fstabdir_name.c_str(), dp->d_name);
        if (ReadDtFile(file_name, &value)) {
            LINFO << "dt_fstab: Using a specified mount point " << value << " for " << dp->d_name;
            mount_point = value;
        } else {
            mount_point = android::base::StringPrintf("/%s", dp->d_name);
        }
        fstab_entry.push_back(mount_point);

        file_name = android::base::StringPrintf("%s/%s/type", fstabdir_name.c_str(), dp->d_name);
        if (!ReadDtFile(file_name, &value)) {
            LERROR << "dt_fstab: Failed to find type for partition " << dp->d_name;
            return {};
        }
        fstab_entry.push_back(value);

        file_name = android::base::StringPrintf("%s/%s/mnt_flags", fstabdir_name.c_str(), dp->d_name);
        if (!ReadDtFile(file_name, &value)) {
            LERROR << "dt_fstab: Failed to find type for partition " << dp->d_name;
            return {};
        }
        fstab_entry.push_back(value);

        file_name = android::base::StringPrintf("%s/%s/fsmgr_flags", fstabdir_name.c_str(), dp->d_name);
        if (!ReadDtFile(file_name, &value)) {
            LERROR << "dt_fstab: Failed to find type for partition " << dp->d_name;
            return {};
        }
        fstab_entry.push_back(value);
        // Adds a fstab_entry to fstab_dt_entries, to be sorted by mount_point later.
        fstab_dt_entries.emplace_back(mount_point, android::base::Join(fstab_entry, " "));
    }

    // Sort fstab_dt entries, to ensure /vendor is mounted before /vendor/abc is attempted.
    std::sort(fstab_dt_entries.begin(), fstab_dt_entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::string fstab_result;
    for (const auto& [_, dt_entry] : fstab_dt_entries) {
        fstab_result += dt_entry + "\n";
    }
    return fstab_result;
}

// Identify path to fstab file. Lookup is based on pattern fstab.<hardware>,
// fstab.<hardware.platform> in folders /odm/etc, vendor/etc, or /.
std::string GetFstabPath() {
    for (const char* prop : {"hardware", "hardware.platform"}) {
        std::string hw;

        if (!fs_mgr_get_boot_config(prop, &hw)) continue;

        for (const char* prefix : {"/odm/etc/fstab.", "/vendor/etc/fstab.", "/fstab."}) {
            std::string fstab_path = prefix + hw;
            if (access(fstab_path.c_str(), F_OK) == 0) {
                return fstab_path;
            }
        }
    }

    return "";
}

bool ReadFstabFile(FILE* fstab_file, bool proc_mounts, Fstab* fstab_out) {
    ssize_t len;
    size_t alloc_len = 0;
    char *line = NULL;
    const char *delim = " \t";
    char *save_ptr, *p;
    Fstab fstab;

    while ((len = getline(&line, &alloc_len, fstab_file)) != -1) {
        /* if the last character is a newline, shorten the string by 1 byte */
        if (line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }

        /* Skip any leading whitespace */
        p = line;
        while (isspace(*p)) {
            p++;
        }
        /* ignore comments or empty lines */
        if (*p == '#' || *p == '\0')
            continue;

        FstabEntry entry;

        if (!(p = strtok_r(line, delim, &save_ptr))) {
            LERROR << "Error parsing mount source";
            goto err;
        }
        entry.blk_device = p;

        if (!(p = strtok_r(NULL, delim, &save_ptr))) {
            LERROR << "Error parsing mount_point";
            goto err;
        }
        entry.mount_point = p;

        if (!(p = strtok_r(NULL, delim, &save_ptr))) {
            LERROR << "Error parsing fs_type";
            goto err;
        }
        entry.fs_type = p;

        if (!(p = strtok_r(NULL, delim, &save_ptr))) {
            LERROR << "Error parsing mount_flags";
            goto err;
        }

        ParseMountFlags(p, &entry);

        // For /proc/mounts, ignore everything after mnt_freq and mnt_passno
        if (proc_mounts) {
            p += strlen(p);
        } else if (!(p = strtok_r(NULL, delim, &save_ptr))) {
            LERROR << "Error parsing fs_mgr_options";
            goto err;
        }

        ParseFsMgrFlags(p, &entry);

        if (entry.fs_mgr_flags.logical) {
            entry.logical_partition_name = entry.blk_device;
        }

        fstab.emplace_back(std::move(entry));
    }

    if (fstab.empty()) {
        LERROR << "No entries found in fstab";
        goto err;
    }

    /* If an A/B partition, modify block device to be the real block device */
    if (!fs_mgr_update_for_slotselect(&fstab)) {
        LERROR << "Error updating for slotselect";
        goto err;
    }
    free(line);
    *fstab_out = std::move(fstab);
    return true;

err:
    free(line);
    return false;
}

/* Extracts <device>s from the by-name symlinks specified in a fstab:
 *   /dev/block/<type>/<device>/by-name/<partition>
 *
 * <type> can be: platform, pci or vbd.
 *
 * For example, given the following entries in the input fstab:
 *   /dev/block/platform/soc/1da4000.ufshc/by-name/system
 *   /dev/block/pci/soc.0/f9824900.sdhci/by-name/vendor
 * it returns a set { "soc/1da4000.ufshc", "soc.0/f9824900.sdhci" }.
 */
std::set<std::string> ExtraBootDevices(const Fstab& fstab) {
    std::set<std::string> boot_devices;

    for (const auto& entry : fstab) {
        std::string blk_device = entry.blk_device;
        // Skips blk_device that doesn't conform to the format.
        if (!android::base::StartsWith(blk_device, "/dev/block") ||
            android::base::StartsWith(blk_device, "/dev/block/by-name") ||
            android::base::StartsWith(blk_device, "/dev/block/bootdevice/by-name")) {
            continue;
        }
        // Skips non-by_name blk_device.
        // /dev/block/<type>/<device>/by-name/<partition>
        //                           ^ slash_by_name
        auto slash_by_name = blk_device.find("/by-name");
        if (slash_by_name == std::string::npos) continue;
        blk_device.erase(slash_by_name);  // erases /by-name/<partition>

        // Erases /dev/block/, now we have <type>/<device>
        blk_device.erase(0, std::string("/dev/block/").size());

        // <type>/<device>
        //       ^ first_slash
        auto first_slash = blk_device.find('/');
        if (first_slash == std::string::npos) continue;

        auto boot_device = blk_device.substr(first_slash + 1);
        if (!boot_device.empty()) boot_devices.insert(std::move(boot_device));
    }

    return boot_devices;
}

FstabEntry BuildGsiUserdataFstabEntry() {
    constexpr uint32_t kFlags = MS_NOATIME | MS_NOSUID | MS_NODEV;

    FstabEntry userdata = {
            .blk_device = "userdata_gsi",
            .mount_point = "/data",
            .fs_type = "ext4",
            .flags = kFlags,
            .reserved_size = 128 * 1024 * 1024,
    };
    userdata.fs_mgr_flags.wait = true;
    userdata.fs_mgr_flags.check = true;
    userdata.fs_mgr_flags.logical = true;
    userdata.fs_mgr_flags.quota = true;
    userdata.fs_mgr_flags.late_mount = true;
    userdata.fs_mgr_flags.formattable = true;
    return userdata;
}

bool EraseFstabEntry(Fstab* fstab, const std::string& mount_point) {
    auto iter = std::remove_if(fstab->begin(), fstab->end(),
                               [&](const auto& entry) { return entry.mount_point == mount_point; });
    if (iter != fstab->end()) {
        fstab->erase(iter, fstab->end());
        return true;
    }
    return false;
}

void TransformFstabForGsi(Fstab* fstab) {
    // Inherit fstab properties for userdata.
    FstabEntry userdata;
    if (FstabEntry* entry = GetEntryForMountPoint(fstab, "/data")) {
        userdata = *entry;
        userdata.blk_device = "userdata_gsi";
        userdata.fs_mgr_flags.logical = true;
        userdata.fs_mgr_flags.formattable = true;
        if (!userdata.key_dir.empty()) {
            userdata.key_dir += "/gsi";
        }
    } else {
        userdata = BuildGsiUserdataFstabEntry();
    }

    if (EraseFstabEntry(fstab, "/system")) {
        fstab->emplace_back(BuildGsiSystemFstabEntry());
    }

    if (EraseFstabEntry(fstab, "/data")) {
        fstab->emplace_back(userdata);
    }
}

}  // namespace

bool ReadFstabFromFile(const std::string& path, Fstab* fstab) {
    auto fstab_file = std::unique_ptr<FILE, decltype(&fclose)>{fopen(path.c_str(), "re"), fclose};
    if (!fstab_file) {
        PERROR << __FUNCTION__ << "(): cannot open file: '" << path << "'";
        return false;
    }

    bool is_proc_mounts = path == "/proc/mounts";

    if (!ReadFstabFile(fstab_file.get(), is_proc_mounts, fstab)) {
        LERROR << __FUNCTION__ << "(): failed to load fstab from : '" << path << "'";
        return false;
    }
    if (!is_proc_mounts && !access(android::gsi::kGsiBootedIndicatorFile, F_OK)) {
        TransformFstabForGsi(fstab);
    }

    return true;
}

// Returns fstab entries parsed from the device tree if they exist
bool ReadFstabFromDt(Fstab* fstab, bool log) {
    std::string fstab_buf = ReadFstabFromDt();
    if (fstab_buf.empty()) {
        if (log) LINFO << __FUNCTION__ << "(): failed to read fstab from dt";
        return false;
    }

    std::unique_ptr<FILE, decltype(&fclose)> fstab_file(
        fmemopen(static_cast<void*>(const_cast<char*>(fstab_buf.c_str())),
                 fstab_buf.length(), "r"), fclose);
    if (!fstab_file) {
        if (log) PERROR << __FUNCTION__ << "(): failed to create a file stream for fstab dt";
        return false;
    }

    if (!ReadFstabFile(fstab_file.get(), false, fstab)) {
        if (log) {
            LERROR << __FUNCTION__ << "(): failed to load fstab from kernel:" << std::endl
                   << fstab_buf;
        }
        return false;
    }

    return true;
}

// Loads the fstab file and combines with fstab entries passed in from device tree.
bool ReadDefaultFstab(Fstab* fstab) {
    Fstab dt_fstab;
    ReadFstabFromDt(&dt_fstab, false);

    *fstab = std::move(dt_fstab);

    std::string default_fstab_path;
    // Use different fstab paths for normal boot and recovery boot, respectively
    if (access("/system/bin/recovery", F_OK) == 0) {
        default_fstab_path = "/etc/recovery.fstab";
    } else {  // normal boot
        default_fstab_path = GetFstabPath();
    }

    Fstab default_fstab;
    if (!default_fstab_path.empty()) {
        ReadFstabFromFile(default_fstab_path, &default_fstab);
    } else {
        LINFO << __FUNCTION__ << "(): failed to find device default fstab";
    }

    for (auto&& entry : default_fstab) {
        fstab->emplace_back(std::move(entry));
    }

    return !fstab->empty();
}

FstabEntry* GetEntryForMountPoint(Fstab* fstab, const std::string& path) {
    if (fstab == nullptr) {
        return nullptr;
    }

    for (auto& entry : *fstab) {
        if (entry.mount_point == path) {
            return &entry;
        }
    }

    return nullptr;
}

std::set<std::string> GetBootDevices() {
    // First check the kernel commandline, then try the device tree otherwise
    std::string dt_file_name = get_android_dt_dir() + "/boot_devices";
    std::string value;
    if (fs_mgr_get_boot_config_from_kernel_cmdline("boot_devices", &value) ||
        ReadDtFile(dt_file_name, &value)) {
        auto boot_devices = Split(value, ",");
        return std::set<std::string>(boot_devices.begin(), boot_devices.end());
    }

    // Fallback to extract boot devices from fstab.
    Fstab fstab;
    if (!ReadDefaultFstab(&fstab)) {
        return {};
    }

    return ExtraBootDevices(fstab);
}

FstabEntry BuildGsiSystemFstabEntry() {
    // .logical_partition_name is required to look up AVB Hashtree descriptors.
    FstabEntry system = {.blk_device = "system_gsi",
                         .mount_point = "/system",
                         .fs_type = "ext4",
                         .flags = MS_RDONLY,
                         .fs_options = "barrier=1",
                         // could add more keys separated by ':'.
                         .avb_keys = "/avb/gsi.avbpubkey:",
                         .logical_partition_name = "system"};
    system.fs_mgr_flags.wait = true;
    system.fs_mgr_flags.logical = true;
    system.fs_mgr_flags.first_stage_mount = true;
    return system;
}

}  // namespace fs_mgr
}  // namespace android

// FIXME: The same logic is duplicated in system/core/init/
const std::string& get_android_dt_dir() {
    // Set once and saves time for subsequent calls to this function
    static const std::string kAndroidDtDir = android::fs_mgr::InitAndroidDtDir();
    return kAndroidDtDir;
}

bool is_dt_compatible() {
    std::string file_name = get_android_dt_dir() + "/compatible";
    std::string dt_value;
    if (android::fs_mgr::ReadDtFile(file_name, &dt_value)) {
        if (dt_value == "android,firmware") {
            return true;
        }
    }

    return false;
}
