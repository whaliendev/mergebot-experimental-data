#ifndef _INIT_HOST_INIT_STUBS_H
#define _INIT_HOST_INIT_STUBS_H 
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <string>
#define __ANDROID_API_P__ 28
#define PROP_VALUE_MAX 92
int setgroups(size_t __size, const gid_t* __list);
namespace android {
namespace init {
extern std::string default_console;
bool CanReadProperty(const std::string& source_context, const std::string& name);
extern uint32_t (*property_set)(const std::string& name, const std::string& value);
uint32_t HandlePropertySet(const std::string& name, const std::string& value,
                           const std::string& source_context, const ucred& cr, std::string* error);
inline void SetFatalRebootTarget() {}
inline void __attribute__((noreturn)) InitFatalReboot() {
    abort();
}
int SelinuxGetVendorAndroidVersion();
void SelabelInitialize();
bool SelabelLookupFileContext(const std::string& key, int type, std::string* result);
}
}
#endif
