       
#include <functional>
#include <string>
#include <vector>
class FastbootDevice;
bool GetVersion(FastbootDevice* device, const std::vector<std::string>& args, std::string* message);
bool GetBootloaderVersion(FastbootDevice* device, const std::vector<std::string>& args,
                          std::string* message);
bool GetBasebandVersion(FastbootDevice* device, const std::vector<std::string>& args,
                        std::string* message);
bool GetProduct(FastbootDevice* device, const std::vector<std::string>& args, std::string* message);
bool GetSerial(FastbootDevice* device, const std::vector<std::string>& args, std::string* message);
bool GetSecure(FastbootDevice* device, const std::vector<std::string>& args, std::string* message);
bool GetCurrentSlot(FastbootDevice* device, const std::vector<std::string>& args,
                    std::string* message);
bool GetSlotCount(FastbootDevice* device, const std::vector<std::string>& args,
                  std::string* message);
bool GetSlotSuccessful(FastbootDevice* device, const std::vector<std::string>& args,
                       std::string* message);
bool GetSlotUnbootable(FastbootDevice* device, const std::vector<std::string>& args,
                       std::string* message);
bool GetMaxDownloadSize(FastbootDevice* device, const std::vector<std::string>& args,
                        std::string* message);
bool GetUnlocked(FastbootDevice* device, const std::vector<std::string>& args,
                 std::string* message);
bool GetHasSlot(FastbootDevice* device, const std::vector<std::string>& args, std::string* message);
bool GetPartitionSize(FastbootDevice* device, const std::vector<std::string>& args,
                      std::string* message);
bool GetPartitionType(FastbootDevice* device, const std::vector<std::string>& args,
                      std::string* message);
bool GetPartitionIsLogical(FastbootDevice* device, const std::vector<std::string>& args,
                           std::string* message);
bool GetIsUserspace(FastbootDevice* device, const std::vector<std::string>& args,
                    std::string* message);
bool GetHardwareRevision(FastbootDevice* device, const std::vector<std::string>& args,
                         std::string* message);
<<<<<<< HEAD
bool GetVariant(FastbootDevice* device, const std::vector<std::string>& args, std::string* message);
bool GetOffModeChargeState(FastbootDevice* device, const std::vector<std::string>& args,
                           std::string* message);
||||||| e80c90f0f
=======
bool GetVariant(FastbootDevice* device, const std::vector<std::string>& args, std::string* message);
>>>>>>> e97756b4
std::vector<std::vector<std::string>> GetAllPartitionArgsWithSlot(FastbootDevice* device);
std::vector<std::vector<std::string>> GetAllPartitionArgsNoSlot(FastbootDevice* device);
