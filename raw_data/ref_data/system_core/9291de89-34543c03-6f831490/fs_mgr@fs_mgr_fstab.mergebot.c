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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>
#include "fs_mgr_priv.h"

struct fs_mgr_flag_values {
  char *key_loc;
  char *verity_loc;
  long long part_length;
  char *label;
  int partnum;
  int swap_prio;
  int max_comp_streams;
  unsigned int zram_size;
  uint64_t reserved_size;
  unsigned int file_encryption_mode;
};

struct flag_list {
  const char *name;
  unsigned int flag;
};

static struct flag_list mount_flags[] = {
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
    {0, 0},
};

static struct flag_list fs_mgr_flags[] = {
    {"wait", MF_WAIT},
    {"check", MF_CHECK},
    {"encryptable=", MF_CRYPT},
    {"forceencrypt=", MF_FORCECRYPT},
    {"fileencryption=", MF_FILEENCRYPTION},
    {"forcefdeorfbe=", MF_FORCEFDEORFBE},
    {"nonremovable", MF_NONREMOVABLE},
    {"voldmanaged=", MF_VOLDMANAGED},
    {"length=", MF_LENGTH},
    {"recoveryonly", MF_RECOVERYONLY},
    {"swapprio=", MF_SWAPPRIO},
    {"zramsize=", MF_ZRAMSIZE},
<<<<<<< HEAD
    {"max_comp_streams=", MF_MAX_COMP_STREAMS},
||||||| 
=======
    {"verifyatboot", MF_VERIFYATBOOT},
>>>>>>> 34543c03e6319d102bc0b1e3ca54808ff398e676
    {"verify", MF_VERIFY},
    {"noemulatedsd", MF_NOEMULATEDSD},
    {"notrim", MF_NOTRIM},
    {"formattable", MF_FORMATTABLE},
    {"slotselect", MF_SLOTSELECT},
    {"nofail", MF_NOFAIL},
    {"latemount", MF_LATEMOUNT},
    {"reservedsize=", MF_RESERVEDSIZE},
    {"defaults", 0},
    {0, 0},
};

#define EM_SOFTWARE 1
#define EM_ICE 2

static struct flag_list encryption_modes[] = {
    {"software", EM_SOFTWARE}, {"ice", EM_ICE}, {0, 0}};

static uint64_t calculate_zram_size(unsigned int percentage) {
  uint64_t total;

  total = sysconf(_SC_PHYS_PAGES);
  total *= percentage;
  total /= 100;

  total *= sysconf(_SC_PAGESIZE);

  return total;
}

static uint64_t parse_size(const char *arg) {
  char *endptr;
  uint64_t size = strtoull(arg, &endptr, 10);
  if (*endptr == 'k' || *endptr == 'K')
    size *= 1024LL;
  else if (*endptr == 'm' || *endptr == 'M')
    size *= 1024LL * 1024LL;
  else if (*endptr == 'g' || *endptr == 'G')
    size *= 1024LL * 1024LL * 1024LL;

  return size;
}

static int parse_flags(char *flags, struct flag_list *fl,
                       struct fs_mgr_flag_values *flag_vals, char *fs_options,
                       int fs_options_len) {
  int f = 0;
  int i;
  char *p;
  char *savep;

  /* initialize flag values.  If we find a relevant flag, we'll
     * update the value */
  if (flag_vals) {
    memset(flag_vals, 0, sizeof(*flag_vals));
    flag_vals->partnum = -1;
    flag_vals->swap_prio = -1; /* negative means it wasn't specified. */
  }

  /* initialize fs_options to the null string */
  if (fs_options && (fs_options_len > 0)) {
    fs_options[0] = '\0';
  }

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
          flag_vals->file_encryption_mode = EM_SOFTWARE;
        } else if ((fl[i].flag == MF_FILEENCRYPTION) && flag_vals) {
          /* The fileencryption flag is followed by an = and the
                     * type of the encryption.  Get it and return it.
                     */
          const struct flag_list *j;
          const char *mode = strchr(p, '=') + 1;
          for (j = encryption_modes; j->name; ++j) {
            if (!strcmp(mode, j->name)) {
              flag_vals->file_encryption_mode = j->flag;
            }
          }
          if (flag_vals->file_encryption_mode == 0) {
            ERROR("Unknown file encryption mode: %s\n", mode);
          }
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

          label_start = strchr(p, '=') + 1;
          label_end = strchr(p, ':');
          if (label_end) {
            flag_vals->label =
                strndup(label_start, (int)(label_end - label_start));
            part_start = strchr(p, ':') + 1;
            if (!strcmp(part_start, "auto")) {
              flag_vals->partnum = -1;
            } else {
              flag_vals->partnum = strtol(part_start, NULL, 0);
            }
          } else {
            ERROR("Warning: voldmanaged= flag malformed\n");
          }
        } else if ((fl[i].flag == MF_SWAPPRIO) && flag_vals) {
          flag_vals->swap_prio = strtoll(strchr(p, '=') + 1, NULL, 0);
        } else if ((fl[i].flag == MF_MAX_COMP_STREAMS) && flag_vals) {
          flag_vals->max_comp_streams = strtoll(strchr(p, '=') + 1, NULL, 0);
        } else if ((fl[i].flag == MF_ZRAMSIZE) && flag_vals) {
          int is_percent = !!strrchr(p, '%');
          unsigned int val = strtoll(strchr(p, '=') + 1, NULL, 0);
          if (is_percent)
            flag_vals->zram_size = calculate_zram_size(val);
          else
            flag_vals->zram_size = val;
        } else if ((fl[i].flag == MF_RESERVEDSIZE) && flag_vals) {
          /* The reserved flag is followed by an = and the
                     * reserved size of the partition.  Get it and return it.
                     */
          flag_vals->reserved_size = parse_size(strchr(p, '=') + 1);
        }
        break;
      }
    }

    if (!fl[i].name) {
      if (fs_options) {
        /* It's not a known flag, so it must be a filesystem specific
                 * option.  Add it to fs_options if it was passed in.
                 */
        strlcat(fs_options, p, fs_options_len);
        strlcat(fs_options, ",", fs_options_len);
      } else {
        /* fs_options was not passed in, so if the flag is unknown
                 * it's an error.
                 */
        ERROR("Warning: unknown flag %s\n", p);
      }
    }
    p = strtok_r(NULL, ",", &savep);
  }

  if (fs_options && fs_options[0]) {
    /* remove the last trailing comma from the list of options */
    fs_options[strlen(fs_options) - 1] = '\0';
  }

  return f;
}

struct fstab *fs_mgr_read_fstab_file(FILE *fstab_file) {
  int cnt, entries;
  ssize_t len;
  size_t alloc_len = 0;
  char *line = NULL;
  const char *delim = " \t";
  char *save_ptr, *p;
  struct fstab *fstab = NULL;
  struct fs_mgr_flag_values flag_vals;
#define FS_OPTIONS_LEN 1024
  char tmp_fs_options[FS_OPTIONS_LEN];

  entries = 0;
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
    if (*p == '#' || *p == '\0') continue;
    entries++;
  }

  if (!entries) {
    ERROR("No entries found in fstab\n");
    goto err;
  }

  /* Allocate and init the fstab structure */
  fstab = calloc(1, sizeof(struct fstab));
  fstab->num_entries = entries;
  fstab->recs = calloc(fstab->num_entries, sizeof(struct fstab_rec));

  fseek(fstab_file, 0, SEEK_SET);

  cnt = 0;
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
    if (*p == '#' || *p == '\0') continue;

    /* If a non-comment entry is greater than the size we allocated, give an
         * error and quit.  This can happen in the unlikely case the file changes
         * between the two reads.
         */
    if (cnt >= entries) {
      ERROR("Tried to process more entries than counted\n");
      break;
    }

    if (!(p = strtok_r(line, delim, &save_ptr))) {
      ERROR("Error parsing mount source\n");
      goto err;
    }
    fstab->recs[cnt].blk_device = strdup(p);

    if (!(p = strtok_r(NULL, delim, &save_ptr))) {
      ERROR("Error parsing mount_point\n");
      goto err;
    }
    fstab->recs[cnt].mount_point = strdup(p);

    if (!(p = strtok_r(NULL, delim, &save_ptr))) {
      ERROR("Error parsing fs_type\n");
      goto err;
    }
    fstab->recs[cnt].fs_type = strdup(p);

    if (!(p = strtok_r(NULL, delim, &save_ptr))) {
      ERROR("Error parsing mount_flags\n");
      goto err;
    }
    tmp_fs_options[0] = '\0';
    fstab->recs[cnt].flags =
        parse_flags(p, mount_flags, NULL, tmp_fs_options, FS_OPTIONS_LEN);

    /* fs_options are optional */
    if (tmp_fs_options[0]) {
      fstab->recs[cnt].fs_options = strdup(tmp_fs_options);
    } else {
      fstab->recs[cnt].fs_options = NULL;
    }

    if (!(p = strtok_r(NULL, delim, &save_ptr))) {
      ERROR("Error parsing fs_mgr_options\n");
      goto err;
    }
    fstab->recs[cnt].fs_mgr_flags =
        parse_flags(p, fs_mgr_flags, &flag_vals, NULL, 0);
    fstab->recs[cnt].key_loc = flag_vals.key_loc;
    fstab->recs[cnt].verity_loc = flag_vals.verity_loc;
    fstab->recs[cnt].length = flag_vals.part_length;
    fstab->recs[cnt].label = flag_vals.label;
    fstab->recs[cnt].partnum = flag_vals.partnum;
    fstab->recs[cnt].swap_prio = flag_vals.swap_prio;
    fstab->recs[cnt].max_comp_streams = flag_vals.max_comp_streams;
    fstab->recs[cnt].zram_size = flag_vals.zram_size;
    fstab->recs[cnt].reserved_size = flag_vals.reserved_size;
    fstab->recs[cnt].file_encryption_mode = flag_vals.file_encryption_mode;
    cnt++;
  }
  /* If an A/B partition, modify block device to be the real block device */
  if (fs_mgr_update_for_slotselect(fstab) != 0) {
    ERROR("Error updating for slotselect\n");
    goto err;
  }
  free(line);
  return fstab;

err:
  free(line);
  if (fstab) fs_mgr_free_fstab(fstab);
  return NULL;
}

struct fstab *fs_mgr_read_fstab(const char *fstab_path) {
  FILE *fstab_file;
  struct fstab *fstab;

  fstab_file = fopen(fstab_path, "r");
  if (!fstab_file) {
    ERROR("Cannot open file %s\n", fstab_path);
    return NULL;
  }
  fstab = fs_mgr_read_fstab_file(fstab_file);
  if (fstab) {
    fstab->fstab_filename = strdup(fstab_path);
  }
  fclose(fstab_file);
  return fstab;
}

void fs_mgr_free_fstab(struct fstab *fstab) {
  int i;

  if (!fstab) {
    return;
  }

  for (i = 0; i < fstab->num_entries; i++) {
    /* Free the pointers return by strdup(3) */
    free(fstab->recs[i].blk_device);
    free(fstab->recs[i].mount_point);
    free(fstab->recs[i].fs_type);
    free(fstab->recs[i].fs_options);
    free(fstab->recs[i].key_loc);
    free(fstab->recs[i].label);
  }

  /* Free the fstab_recs array created by calloc(3) */
  free(fstab->recs);

  /* Free the fstab filename */
  free(fstab->fstab_filename);

  /* Free fstab */
  free(fstab);
}

/* Add an entry to the fstab, and return 0 on success or -1 on error */
int fs_mgr_add_entry(struct fstab *fstab, const char *mount_point,
                     const char *fs_type, const char *blk_device) {
  struct fstab_rec *new_fstab_recs;
  int n = fstab->num_entries;

  new_fstab_recs = (struct fstab_rec *)realloc(
      fstab->recs, sizeof(struct fstab_rec) * (n + 1));

  if (!new_fstab_recs) {
    return -1;
  }

  /* A new entry was added, so initialize it */
  memset(&new_fstab_recs[n], 0, sizeof(struct fstab_rec));
  new_fstab_recs[n].mount_point = strdup(mount_point);
  new_fstab_recs[n].fs_type = strdup(fs_type);
  new_fstab_recs[n].blk_device = strdup(blk_device);
  new_fstab_recs[n].length = 0;

  /* Update the fstab struct */
  fstab->recs = new_fstab_recs;
  fstab->num_entries++;

  return 0;
}

/*
 * Returns the 1st matching fstab_rec that follows the start_rec.
 * start_rec is the result of a previous search or NULL.
 */
struct fstab_rec *fs_mgr_get_entry_for_mount_point_after(
    struct fstab_rec *start_rec, struct fstab *fstab, const char *path) {
  int i;
  if (!fstab) {
    return NULL;
  }

  if (start_rec) {
    for (i = 0; i < fstab->num_entries; i++) {
      if (&fstab->recs[i] == start_rec) {
        i++;
        break;
      }
    }
  } else {
    i = 0;
  }
  for (; i < fstab->num_entries; i++) {
    int len = strlen(fstab->recs[i].mount_point);
    if (strncmp(path, fstab->recs[i].mount_point, len) == 0 &&
        (path[len] == '\0' || path[len] == '/')) {
      return &fstab->recs[i];
    }
  }
  return NULL;
}

/*
 * Returns the 1st matching mount point.
 * There might be more. To look for others, use fs_mgr_get_entry_for_mount_point_after()
 * and give the fstab_rec from the previous search.
 */
struct fstab_rec *fs_mgr_get_entry_for_mount_point(struct fstab *fstab,
                                                   const char *path) {
  return fs_mgr_get_entry_for_mount_point_after(NULL, fstab, path);
}

int fs_mgr_is_voldmanaged(const struct fstab_rec *fstab) {
  return fstab->fs_mgr_flags & MF_VOLDMANAGED;
}

int fs_mgr_is_nonremovable(const struct fstab_rec *fstab) {
  return fstab->fs_mgr_flags & MF_NONREMOVABLE;
}

int fs_mgr_is_verified(const struct fstab_rec *fstab) {
  return fstab->fs_mgr_flags & MF_VERIFY;
}

int fs_mgr_is_encryptable(const struct fstab_rec *fstab) {
  return fstab->fs_mgr_flags & (MF_CRYPT | MF_FORCECRYPT | MF_FORCEFDEORFBE);
}

int fs_mgr_is_file_encrypted(const struct fstab_rec *fstab) {
  return fstab->fs_mgr_flags & MF_FILEENCRYPTION;
}

const char *fs_mgr_get_file_encryption_mode(const struct fstab_rec *fstab) {
  const struct flag_list *j;
  for (j = encryption_modes; j->name; ++j) {
    if (fstab->file_encryption_mode == j->flag) {
      return j->name;
    }
  }
  return NULL;
}

int fs_mgr_is_convertible_to_fbe(const struct fstab_rec *fstab) {
  return fstab->fs_mgr_flags & MF_FORCEFDEORFBE;
}

int fs_mgr_is_noemulatedsd(const struct fstab_rec *fstab) {
  return fstab->fs_mgr_flags & MF_NOEMULATEDSD;
}

int fs_mgr_is_notrim(struct fstab_rec *fstab) {
  return fstab->fs_mgr_flags & MF_NOTRIM;
}

int fs_mgr_is_formattable(struct fstab_rec *fstab) {
  return fstab->fs_mgr_flags & (MF_FORMATTABLE);
}

int fs_mgr_is_slotselect(struct fstab_rec *fstab) {
  return fstab->fs_mgr_flags & MF_SLOTSELECT;
}

int fs_mgr_is_nofail(struct fstab_rec *fstab) {
  return fstab->fs_mgr_flags & MF_NOFAIL;
}

int fs_mgr_is_latemount(struct fstab_rec *fstab) {
  return fstab->fs_mgr_flags & MF_LATEMOUNT;
}