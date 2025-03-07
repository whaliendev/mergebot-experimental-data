       
#include <sys/socket.h>
#include <string>
#include "epoll.h"
namespace android {
namespace init {
static constexpr const char kRestoreconProperty[] = "selinux.restorecon_recursive";
bool CanReadProperty(const std::string& source_context, const std::string& name);
void PropertyInit();
void StartPropertyService(int* epoll_socket);
void StartSendingMessages(); void StopSendingMessages();
}
}
