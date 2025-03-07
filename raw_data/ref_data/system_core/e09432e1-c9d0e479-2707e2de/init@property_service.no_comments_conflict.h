       
#include <sys/socket.h>
#include <string>
#include "epoll.h"
namespace android {
namespace init {
static constexpr const char kRestoreconProperty[] = "selinux.restorecon_recursive";
bool CanReadProperty(const std::string& source_context, const std::string& name);
void PropertyInit();
void StartPropertyService(int* epoll_socket);
<<<<<<< HEAD
void StartSendingMessages();
void StopSendingMessages();
||||||| 2707e2de3
void ResumePropertyService();
void PausePropertyService();
=======
>>>>>>> c9d0e479
}
}
