#ifndef __CORE_FS_MGR_PRIV_H
#define __CORE_FS_MGR_PRIV_H 
#include <cutils/klog.h>
#include <fs_mgr.h>
__BEGIN_DECLS
#define INFO(x...) KLOG_INFO("fs_mgr", x)
#define WARNING(x...) KLOG_WARNING("fs_mgr", x)
#define ERROR(x...) KLOG_ERROR("fs_mgr", x)
#define CRYPTO_TMPFS_OPTIONS "size=256m,mode=0771,uid=1000,gid=1000"
#define WAIT_TIMEOUT 20
#define MF_WAIT 0x1
#define MF_CHECK 0x2
#define MF_CRYPT 0x4
#define MF_NONREMOVABLE 0x8
#define MF_VOLDMANAGED 0x10
#define MF_LENGTH 0x20
#define MF_RECOVERYONLY 0x40
#define MF_SWAPPRIO 0x80
#define MF_ZRAMSIZE 0x100
#define MF_VERIFY 0x200
#define MF_FORCECRYPT 0x400
#define MF_NOEMULATEDSD \
  0x800
