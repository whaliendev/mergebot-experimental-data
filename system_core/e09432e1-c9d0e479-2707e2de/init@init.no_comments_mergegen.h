       
#include <sys/types.h>
#include <string>
#include "action.h"
#include "action_manager.h"
#include "parser.h"
#include "service_list.h"
namespace android {
namespace init {
Parser CreateParser(ActionManager& action_manager, ServiceList& service_list);
Parser CreateServiceOnlyParser(ServiceList& service_list, bool from_apex);
bool start_waiting_for_property(const char *name, const char *value);
void DumpState();
void ResetWaitForProp();
void SendLoadPersistentPropertiesMessage();
void PropertyChanged(const std::string& name, const std::string& value); bool QueueControlMessage(const std::string& message, const std::string& name, pid_t pid, int fd);
int SecondStageMain(int argc, char** argv);
}
}
