#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <shlobj.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
namespace logger = SKSE::log;

struct SKSELogsPaths {
    fs::path primary;
    fs::path secondary;
};

struct PluginConfig {
    struct {
        bool enabled = true;
        std::string questEditorID = "YW_Quest_MDF";
        std::string questPlugin = "YurianaWench.esp";
        int triggerStage = 21;
        int completionStage = 30;
        bool showNotification = true;
    } quest;

    struct {
        bool enabled = true;
        std::string itemID = "625C7C";
        std::string itemPlugin = "YurianaWench.esp";
        std::string itemName = "Lelyna's Remedy";
        bool removeOnDetection = true;
        bool showNotification = true;
    } item;

    struct {
        bool enabled = true;
        std::string triggerMessage = "You must craft Lelyna's Remedy to cure Elora. Check your inventory for the recipe.";
        std::string completionMessage = "You have crafted the potion and the temple girls are very happy. You have saved Elora!";
        bool showTriggerMessage = true;
        bool showCompletionMessage = true;
    } messages;

    struct {
        bool enabled = true;
        int checkIntervalMs = 1000;
        float maxDetectionDistance = 5000.0f;
    } monitoring;

    struct {
        bool enabled = true;
    } notification;
};

struct CachedFormIDs {
    RE::FormID questFormID = 0;
    RE::FormID itemFormID = 0;
    bool questResolved = false;
    bool itemResolved = false;
};

struct QuestStateData {
    bool questActive = false;
    bool triggerStageReached = false;
    bool itemDetected = false;
    bool completionDone = false;
    int currentStage = 0;
    std::chrono::steady_clock::time_point lastCheck;
};

static std::deque<std::string> g_actionLines;
static std::deque<std::string> g_questLines;
static std::deque<std::string> g_systemLines;
static std::string g_documentsPath;
static std::string g_gamePath;
static bool g_isInitialized = false;
static std::mutex g_logMutex;
static std::mutex g_questMutex;
static std::mutex g_configMutex;
static std::mutex g_cacheMutex;
static bool g_monitoringActive = false;
static std::thread g_monitorThread;
static int g_monitorCycles = 0;
static std::chrono::steady_clock::time_point g_monitoringStartTime;
static bool g_initialDelayComplete = false;
static std::atomic<bool> g_isShuttingDown(false);
static std::atomic<bool> g_isInGameTransition(false);
static SKSELogsPaths g_logPaths;
static PluginConfig g_config;
static CachedFormIDs g_cachedFormIDs;
static QuestStateData g_questState;

static std::atomic<bool> g_questMonitoringActive(false);
static std::chrono::steady_clock::time_point g_lastQuestCheck;
static std::chrono::steady_clock::time_point g_lastItemCheck;

static bool g_triggerMessageShown = false;
static bool g_completionMessageShown = false;
static bool g_potionDetectedMessageShown = false;
static std::chrono::steady_clock::time_point g_potionDetectedTime;
static bool g_waitingForPotionDelay = false;

void StartMonitoringThread();
void StopMonitoringThread();
void WriteToActionsLog(const std::string& message, int lineNumber = 0);
void WriteToQuestLog(const std::string& message, int lineNumber = 0);
void WriteToSystemLog(const std::string& message, int lineNumber = 0);
void CheckQuestState();
void CheckPlayerInventory();
void ProcessQuestTrigger();
void ProcessItemDetection();
void ProcessQuestCompletion();
void ResolveFormIDs();
void ValidatePluginsInINI();
bool LoadConfiguration();
void SaveDefaultConfiguration();
fs::path GetPluginINIPath();
RE::FormID GetFormIDFromPlugin(const std::string& pluginName, const std::string& localFormID);
RE::TESQuest* GetQuestByEditorID(const std::string& editorID);
int GetQuestCurrentStage(RE::TESQuest* quest);
bool SetQuestStage(RE::TESQuest* quest, int stage);
bool PlayerHasItem(RE::FormID itemFormID);
bool RemoveItemFromPlayer(RE::FormID itemFormID, int count = 1);
void ShowNotificationMessage(const std::string& message);
void ShowMessageBox(const std::string& message);

std::string SafeWideStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    try {
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
        if (size_needed <= 0) {
            size_needed = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
            if (size_needed <= 0) return std::string();
            std::string result(size_needed, 0);
            int converted = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), (int)wstr.size(), &result[0], size_needed, NULL, NULL);
            if (converted <= 0) return std::string();
            return result;
        }
        std::string result(size_needed, 0);
        int converted = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &result[0], size_needed, NULL, NULL);
        if (converted <= 0) return std::string();
        return result;
    } catch (...) {
        std::string result;
        result.reserve(wstr.size());
        for (wchar_t wc : wstr) {
            if (wc <= 127) {
                result.push_back(static_cast<char>(wc));
            } else {
                result.push_back('?');
            }
        }
        return result;
    }
}

std::string GetEnvVar(const std::string& key) {
    char* buf = nullptr;
    size_t sz = 0;
    if (_dupenv_s(&buf, &sz, key.c_str()) == 0 && buf != nullptr) {
        std::string value(buf);
        free(buf);
        return value;
    }
    return "";
}

std::string NormalizeName(const std::string& name) {
    std::string normalized = name;
    normalized.erase(0, normalized.find_first_not_of(" \t\r\n"));
    normalized.erase(normalized.find_last_not_of(" \t\r\n") + 1);
    return normalized;
}

std::string GetCurrentTimeString() {
    auto now = std::chrono::system_clock::now();
    std::time_t time_t = std::chrono::system_clock::to_time_t(now);
    std::tm buf;
    localtime_s(&buf, &time_t);
    std::stringstream ss;
    ss << std::put_time(&buf, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

std::string GetCurrentTimeStringWithMillis() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t time_t = std::chrono::system_clock::to_time_t(now);
    std::tm buf;
    localtime_s(&buf, &time_t);
    std::stringstream ss;
    ss << std::put_time(&buf, "%Y-%m-%d %H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

void WriteToDualPath(const fs::path& primaryPath, const fs::path& secondaryPath,
                     const std::string& content, std::ios::openmode mode) {
    try {
        if (!primaryPath.parent_path().empty() && !fs::exists(primaryPath.parent_path())) {
            fs::create_directories(primaryPath.parent_path());
        }
        std::ofstream primaryFile(primaryPath, mode);
        if (primaryFile.is_open()) {
            primaryFile << content << std::endl;
            primaryFile.close();
        }
    } catch (...) {}

    try {
        if (!secondaryPath.parent_path().empty() && !fs::exists(secondaryPath.parent_path())) {
            fs::create_directories(secondaryPath.parent_path());
        }
        std::ofstream secondaryFile(secondaryPath, mode);
        if (secondaryFile.is_open()) {
            secondaryFile << content << std::endl;
            secondaryFile.close();
        }
    } catch (...) {}
}

// ===== LOG SYSTEM - ACTIONS LOG =====
void WriteToActionsLog(const std::string& message, int lineNumber) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t time_t = std::chrono::system_clock::to_time_t(now);
    std::tm buf;
    localtime_s(&buf, &time_t);

    std::stringstream ss;
    ss << "[" << std::put_time(&buf, "%Y-%m-%d %H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count() << "] ";
    ss << "[log] [info] ";
    ss << "[plugin.cpp:" << lineNumber << "] ";
    ss << message;

    std::string newLine = ss.str();
    g_actionLines.push_back(newLine);

    if (g_actionLines.size() > 2000) {
        g_actionLines.pop_front();
    }

    static size_t lineCount = 0;
    lineCount++;

    fs::path primaryLogPath = g_logPaths.primary / "BWY-multi-Fix-NG-Actions.log";
    fs::path secondaryLogPath = g_logPaths.secondary / "BWY-multi-Fix-NG-Actions.log";

    if (lineCount >= 2500) {
        std::stringstream bulkContent;
        for (const auto& line : g_actionLines) {
            bulkContent << line << "\n";
        }
        WriteToDualPath(primaryLogPath, secondaryLogPath, bulkContent.str(), std::ios::trunc);
        lineCount = g_actionLines.size();
    } else {
        WriteToDualPath(primaryLogPath, secondaryLogPath, newLine, std::ios::app);
    }
}

// ===== LOG SYSTEM - QUEST LOG =====
void WriteToQuestLog(const std::string& message, int lineNumber) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t time_t = std::chrono::system_clock::to_time_t(now);
    std::tm buf;
    localtime_s(&buf, &time_t);

    std::stringstream ss;
    ss << "[" << std::put_time(&buf, "%Y-%m-%d %H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count() << "] ";
    ss << "[quest] [info] ";
    ss << "[plugin.cpp:" << lineNumber << "] ";
    ss << message;

    std::string newLine = ss.str();
    g_questLines.push_back(newLine);

    if (g_questLines.size() > 2000) {
        g_questLines.pop_front();
    }

    static size_t lineCount = 0;
    lineCount++;

    fs::path primaryLogPath = g_logPaths.primary / "BWY-multi-Fix-NG-Quest.log";
    fs::path secondaryLogPath = g_logPaths.secondary / "BWY-multi-Fix-NG-Quest.log";

    if (lineCount >= 2500) {
        std::stringstream bulkContent;
        for (const auto& line : g_questLines) {
            bulkContent << line << "\n";
        }
        WriteToDualPath(primaryLogPath, secondaryLogPath, bulkContent.str(), std::ios::trunc);
        lineCount = g_questLines.size();
    } else {
        WriteToDualPath(primaryLogPath, secondaryLogPath, newLine, std::ios::app);
    }
}

// ===== LOG SYSTEM - SYSTEM LOG =====
void WriteToSystemLog(const std::string& message, int lineNumber) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t time_t = std::chrono::system_clock::to_time_t(now);
    std::tm buf;
    localtime_s(&buf, &time_t);

    std::stringstream ss;
    ss << "[" << std::put_time(&buf, "%Y-%m-%d %H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count() << "] ";
    ss << "[system] [info] ";
    ss << "[plugin.cpp:" << lineNumber << "] ";
    ss << message;

    std::string newLine = ss.str();
    g_systemLines.push_back(newLine);

    if (g_systemLines.size() > 2000) {
        g_systemLines.pop_front();
    }

    static size_t lineCount = 0;
    lineCount++;

    fs::path primaryLogPath = g_logPaths.primary / "BWY-multi-Fix-NG-System.log";
    fs::path secondaryLogPath = g_logPaths.secondary / "BWY-multi-Fix-NG-System.log";

    if (lineCount >= 2500) {
        std::stringstream bulkContent;
        for (const auto& line : g_systemLines) {
            bulkContent << line << "\n";
        }
        WriteToDualPath(primaryLogPath, secondaryLogPath, bulkContent.str(), std::ios::trunc);
        lineCount = g_systemLines.size();
    } else {
        WriteToDualPath(primaryLogPath, secondaryLogPath, newLine, std::ios::app);
    }
}

fs::path GetPluginINIPath() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    fs::path gamePath = fs::path(exePath).parent_path();
    fs::path pluginConfigDir = gamePath / "Data" / "SKSE" / "Plugins";
    
    if (!fs::exists(pluginConfigDir)) {
        fs::create_directories(pluginConfigDir);
    }
    
    return pluginConfigDir / "BWY-multi-Fix-NG.ini";
}

bool IsDLCInstalled(const std::string& dlcName) {
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return false;
    
    auto* file = dataHandler->LookupModByName(dlcName);
    return (file != nullptr);
}

bool IsPluginLoaded(const std::string& pluginName) {
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return false;
    
    auto* file = dataHandler->LookupModByName(pluginName);
    return (file != nullptr);
}

RE::FormID GetFormIDFromPlugin(const std::string& pluginName, const std::string& localFormID) {
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        logger::error("Failed to get TESDataHandler");
        return 0;
    }

    auto* file = dataHandler->LookupModByName(pluginName);
    if (!file) {
        logger::error("Plugin not found: {}", pluginName);
        return 0;
    }

    std::string cleanID = localFormID;
    if (cleanID.length() >= 2 && cleanID.substr(0, 2) == "XX") {
        cleanID = cleanID.substr(2);
    }

    RE::FormID localID = 0;
    try {
        localID = std::stoul(cleanID, nullptr, 16);
    } catch (...) {
        logger::error("Failed to parse FormID: {}", cleanID);
        return 0;
    }

    uint8_t modIndex = file->compileIndex;
    if (modIndex == 0xFF) {
        modIndex = file->smallFileCompileIndex;
    }

    RE::FormID fullFormID = (static_cast<RE::FormID>(modIndex) << 24) | (localID & 0x00FFFFFF);
    
    return fullFormID;
}

RE::TESQuest* GetQuestByEditorID(const std::string& editorID) {
    auto* quest = RE::TESForm::LookupByEditorID<RE::TESQuest>(editorID);
    return quest;
}

int GetQuestCurrentStage(RE::TESQuest* quest) {
    if (!quest) return -1;
    return quest->GetCurrentStageID();
}

void ExecuteConsoleCommand(const std::string& command) {
    try {
        auto* scriptFactory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
        if (!scriptFactory) {
            WriteToActionsLog("ERROR: Failed to get Script factory", __LINE__);
            return;
        }
        
        auto* script = scriptFactory->Create();
        if (!script) {
            WriteToActionsLog("ERROR: Failed to create Script object", __LINE__);
            return;
        }
        
        script->SetCommand(command);
        script->CompileAndRun(nullptr);
        delete script;
        
        WriteToActionsLog("Console command executed: " + command, __LINE__);
    } catch (...) {
        WriteToActionsLog("ERROR: Exception executing console command: " + command, __LINE__);
    }
}

bool SetQuestStageViaConsole(const std::string& questEditorID, int stage) {
    std::string command = "setstage " + questEditorID + " " + std::to_string(stage);
    
    WriteToQuestLog("========================================", __LINE__);
    WriteToQuestLog("EXECUTING CONSOLE COMMAND", __LINE__);
    WriteToQuestLog("Command: " + command, __LINE__);
    WriteToQuestLog("========================================", __LINE__);
    
    ExecuteConsoleCommand(command);
    
    return true;
}

bool PlayerHasItem(RE::FormID itemFormID) {
    if (itemFormID == 0) return false;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return false;

    auto* itemForm = RE::TESForm::LookupByID(itemFormID);
    if (!itemForm) return false;

    auto* boundObject = itemForm->As<RE::TESBoundObject>();
    if (!boundObject) return false;

    auto inventory = player->GetInventory();
    auto it = inventory.find(boundObject);
    if (it != inventory.end()) {
        if (it->second.first > 0) {
            return true;
        }
    }

    return false;
}

bool RemoveItemFromPlayer(RE::FormID itemFormID, int count) {
    if (itemFormID == 0) return false;
    
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return false;
    
    auto* itemForm = RE::TESForm::LookupByID(itemFormID);
    if (!itemForm) return false;
    
    auto* boundObject = itemForm->As<RE::TESBoundObject>();
    if (!boundObject) return false;
    
    try {
        player->RemoveItem(boundObject, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        WriteToActionsLog("Removed " + std::to_string(count) + " item(s) from player inventory", __LINE__);
        return true;
    } catch (...) {
        WriteToActionsLog("Exception while removing item from player inventory", __LINE__);
        return false;
    }
}

void ShowNotificationMessage(const std::string& message) {
    if (message.empty()) return;
    RE::DebugNotification(message.c_str());
}

void ShowMessageBox(const std::string& message) {
    if (message.empty()) return;
    
    auto* messageQueue = RE::UIMessageQueue::GetSingleton();
    if (!messageQueue) {
        ShowNotificationMessage(message);
        return;
    }
    
    RE::DebugMessageBox(message.c_str());
}

std::string GetDocumentsPath() {
    try {
        wchar_t path[MAX_PATH] = {0};
        HRESULT result = SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, path);
        if (SUCCEEDED(result)) {
            std::wstring ws(path);
            std::string converted = SafeWideStringToString(ws);
            if (!converted.empty()) {
                return converted;
            }
        }
        std::string userProfile = GetEnvVar("USERPROFILE");
        if (!userProfile.empty()) {
            return userProfile + "\\Documents";
        }
        return "C:\\Users\\Default\\Documents";
    } catch (...) {
        return "C:\\Users\\Default\\Documents";
    }
}

bool IsValidPluginPath(const fs::path& pluginPath) {
    const std::vector<std::string> dllNames = {
        "BWY-multi-Fix-NG.dll"
    };
    
    for (const auto& dllName : dllNames) {
        fs::path dllPath = pluginPath / dllName;
        try {
            if (fs::exists(dllPath)) {
                return true;
            }
        } catch (...) {
            continue;
        }
    }
    return false;
}

fs::path BuildPathCaseInsensitive(const fs::path& basePath, const std::vector<std::string>& components) {
    try {
        fs::path currentPath = basePath;
        
        for (const auto& component : components) {
            fs::path testPath = currentPath / component;
            if (fs::exists(testPath)) {
                currentPath = testPath;
                continue;
            }
            
            std::string lowerComponent = component;
            std::transform(lowerComponent.begin(), lowerComponent.end(), 
                         lowerComponent.begin(), ::tolower);
            testPath = currentPath / lowerComponent;
            if (fs::exists(testPath)) {
                currentPath = testPath;
                continue;
            }
            
            std::string upperComponent = component;
            std::transform(upperComponent.begin(), upperComponent.end(), 
                         upperComponent.begin(), ::toupper);
            testPath = currentPath / upperComponent;
            if (fs::exists(testPath)) {
                currentPath = testPath;
                continue;
            }
            
            bool found = false;
            if (fs::exists(currentPath) && fs::is_directory(currentPath)) {
                for (const auto& entry : fs::directory_iterator(currentPath)) {
                    try {
                        std::string entryName = entry.path().filename().string();
                        std::string lowerEntryName = entryName;
                        std::transform(lowerEntryName.begin(), lowerEntryName.end(), 
                                     lowerEntryName.begin(), ::tolower);
                        
                        if (lowerEntryName == lowerComponent) {
                            currentPath = entry.path();
                            found = true;
                            break;
                        }
                    } catch (...) {
                        continue;
                    }
                }
            }
            
            if (!found) {
                currentPath = currentPath / component;
            }
        }
        
        return currentPath;
        
    } catch (...) {
        return basePath;
    }
}

fs::path GetDllDirectory() {
    try {
        HMODULE hModule = nullptr;
        static int dummyVariable = 0;

        if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | 
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&dummyVariable), 
            &hModule) && hModule != nullptr) {
            
            wchar_t dllPath[MAX_PATH] = {0};
            DWORD size = GetModuleFileNameW(hModule, dllPath, MAX_PATH);

            if (size > 0) {
                std::wstring wsDllPath(dllPath);
                std::string dllPathStr = SafeWideStringToString(wsDllPath);

                if (!dllPathStr.empty()) {
                    fs::path dllDir = fs::path(dllPathStr).parent_path();
                    return dllDir;
                }
            }
        }

        return fs::path();

    } catch (...) {
        return fs::path();
    }
}

std::string GetGamePath() {
    try {
        std::string mo2Path = GetEnvVar("MO2_MODS_PATH");
        if (!mo2Path.empty()) {
            fs::path testPath = BuildPathCaseInsensitive(
                fs::path(mo2Path), {"Data", "SKSE", "Plugins"}
            );
            if (IsValidPluginPath(testPath)) {
                WriteToSystemLog("Game path detected: MO2 Environment Variable", __LINE__);
                return mo2Path;
            }
        }

        std::string mo2Overwrite = GetEnvVar("MO_OVERWRITE_PATH");
        if (!mo2Overwrite.empty()) {
            fs::path testPath = BuildPathCaseInsensitive(
                fs::path(mo2Overwrite), {"SKSE", "Plugins"}
            );
            if (IsValidPluginPath(testPath)) {
                WriteToSystemLog("Game path detected: MO2 Overwrite Path", __LINE__);
                return mo2Overwrite;
            }
        }

        std::string vortexPath = GetEnvVar("VORTEX_MODS_PATH");
        if (!vortexPath.empty()) {
            fs::path testPath = BuildPathCaseInsensitive(
                fs::path(vortexPath), {"Data", "SKSE", "Plugins"}
            );
            if (IsValidPluginPath(testPath)) {
                WriteToSystemLog("Game path detected: Vortex Environment Variable", __LINE__);
                return vortexPath;
            }
        }

        std::string skyrimMods = GetEnvVar("SKYRIM_MODS_FOLDER");
        if (!skyrimMods.empty()) {
            fs::path testPath = BuildPathCaseInsensitive(
                fs::path(skyrimMods), {"Data", "SKSE", "Plugins"}
            );
            if (IsValidPluginPath(testPath)) {
                WriteToSystemLog("Game path detected: SKYRIM_MODS_FOLDER Variable", __LINE__);
                return skyrimMods;
            }
        }

        std::vector<std::string> registryKeys = {
            "SOFTWARE\\WOW6432Node\\Bethesda Softworks\\Skyrim Special Edition",
            "SOFTWARE\\Bethesda Softworks\\Skyrim Special Edition",
            "SOFTWARE\\WOW6432Node\\GOG.com\\Games\\1457087920",
            "SOFTWARE\\GOG.com\\Games\\1457087920",
            "SOFTWARE\\WOW6432Node\\Valve\\Steam\\Apps\\489830",
            "SOFTWARE\\WOW6432Node\\Valve\\Steam\\Apps\\611670"
        };

        HKEY hKey;
        char pathBuffer[MAX_PATH] = {0};
        DWORD pathSize = sizeof(pathBuffer);

        for (const auto& key : registryKeys) {
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
                if (RegQueryValueExA(hKey, "Installed Path", NULL, NULL, (LPBYTE)pathBuffer, &pathSize) == ERROR_SUCCESS) {
                    RegCloseKey(hKey);
                    std::string result(pathBuffer);
                    if (!result.empty()) {
                        fs::path testPath = BuildPathCaseInsensitive(
                            fs::path(result), {"Data", "SKSE", "Plugins"}
                        );
                        if (IsValidPluginPath(testPath)) {
                            WriteToSystemLog("Game path detected: Windows Registry", __LINE__);
                            return result;
                        }
                    }
                }
                RegCloseKey(hKey);
            }
            pathSize = sizeof(pathBuffer);
        }

        std::vector<std::string> commonPaths = {
            "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Skyrim Special Edition",
            "C:\\Program Files\\Steam\\steamapps\\common\\Skyrim Special Edition",
            "D:\\Steam\\steamapps\\common\\Skyrim Special Edition",
            "D:\\SteamLibrary\\steamapps\\common\\Skyrim Special Edition",
            "E:\\Steam\\steamapps\\common\\Skyrim Special Edition",
            "E:\\SteamLibrary\\steamapps\\common\\Skyrim Special Edition",
            "F:\\Steam\\steamapps\\common\\Skyrim Special Edition",
            "F:\\SteamLibrary\\steamapps\\common\\Skyrim Special Edition",
            "G:\\Steam\\steamapps\\common\\Skyrim Special Edition",
            "G:\\SteamLibrary\\steamapps\\common\\Skyrim Special Edition",
            "C:\\GOG Games\\Skyrim Special Edition",
            "D:\\GOG Games\\Skyrim Special Edition",
            "E:\\GOG Games\\Skyrim Special Edition",
            "C:\\Games\\Skyrim Special Edition",
            "D:\\Games\\Skyrim Special Edition"
        };

        for (const auto& pathCandidate : commonPaths) {
            try {
                if (fs::exists(pathCandidate) && fs::is_directory(pathCandidate)) {
                    fs::path testPath = BuildPathCaseInsensitive(
                        fs::path(pathCandidate), {"Data", "SKSE", "Plugins"}
                    );
                    if (IsValidPluginPath(testPath)) {
                        WriteToSystemLog("Game path detected: Common Installation Path", __LINE__);
                        return pathCandidate;
                    }
                }
            } catch (...) {
                continue;
            }
        }

        WriteToSystemLog("Attempting DLL Directory Detection (Wabbajack/MO2/Portable fallback)...", __LINE__);
        fs::path dllDir = GetDllDirectory();
        
        if (!dllDir.empty()) {
            if (IsValidPluginPath(dllDir)) {
                fs::path calculatedGamePath = dllDir.parent_path().parent_path().parent_path();
                WriteToSystemLog("Game path detected: DLL Directory Method (Wabbajack/Portable)", __LINE__);
                WriteToSystemLog("Calculated game path: " + calculatedGamePath.string(), __LINE__);
                return calculatedGamePath.string();
            }
        }

        WriteToSystemLog("WARNING: No valid game path detected, using default fallback", __LINE__);
        return "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Skyrim Special Edition";
        
    } catch (...) {
        return "";
    }
}

SKSELogsPaths GetAllSKSELogsPaths() {
    SKSELogsPaths paths;

    try {
        std::string docs = GetDocumentsPath();

        fs::path primaryBase = fs::path(docs) / "My Games" / "Skyrim Special Edition" / "SKSE";
        paths.primary = primaryBase;

        fs::path secondaryBase = fs::path(docs) / "My Games" / "Skyrim.INI" / "SKSE";
        paths.secondary = secondaryBase;

        if (fs::exists(paths.primary)) {
            logger::info("Primary path exists and is accessible");
        }

        if (fs::exists(paths.secondary)) {
            logger::info("Secondary path exists and is accessible");
        }

    } catch (const std::exception& e) {
        logger::error("Error detecting SKSE paths: {}", e.what());
    }

    return paths;
}

void SaveDefaultConfiguration() {
    fs::path iniPath = GetPluginINIPath();
    
    std::ofstream iniFile(iniPath, std::ios::trunc);
    if (!iniFile.is_open()) {
        logger::error("Failed to create default configuration file");
        return;
    }

    iniFile << "[Quest]" << std::endl;
    iniFile << "Enabled=true" << std::endl;
    iniFile << "QuestEditorID=YW_Quest_MDF" << std::endl;
    iniFile << "QuestPlugin=YurianaWench.esp" << std::endl;
    iniFile << "TriggerStage=21" << std::endl;
    iniFile << "CompletionStage=30" << std::endl;
    iniFile << "ShowNotification=true" << std::endl;
    iniFile << std::endl;

    iniFile << "[Item]" << std::endl;
    iniFile << "Enabled=true" << std::endl;
    iniFile << "ItemID=625C7C" << std::endl;
    iniFile << "ItemPlugin=YurianaWench.esp" << std::endl;
    iniFile << "ItemName=Lelyna's Remedy" << std::endl;
    iniFile << "RemoveOnDetection=true" << std::endl;
    iniFile << "ShowNotification=true" << std::endl;
    iniFile << std::endl;

    iniFile << "[Messages]" << std::endl;
    iniFile << "Enabled=true" << std::endl;
    iniFile << "TriggerMessage=You must craft Lelyna's Remedy to cure Elora. Check your inventory for the recipe." << std::endl;
    iniFile << "CompletionMessage=You have crafted the potion and the temple girls are very happy. You have saved Elora!" << std::endl;
    iniFile << "ShowTriggerMessage=true" << std::endl;
    iniFile << "ShowCompletionMessage=true" << std::endl;
    iniFile << std::endl;

    iniFile << "[Monitoring]" << std::endl;
    iniFile << "Enabled=true" << std::endl;
    iniFile << "CheckIntervalMs=1000" << std::endl;
    iniFile << "MaxDetectionDistance=5000.0" << std::endl;
    iniFile << std::endl;

    iniFile << "[Notification]" << std::endl;
    iniFile << "Enabled=true" << std::endl;

    iniFile.close();
}

bool LoadConfiguration() {
    std::lock_guard<std::mutex> lock(g_configMutex);

    fs::path iniPath = GetPluginINIPath();

    if (!fs::exists(iniPath)) {
        SaveDefaultConfiguration();
    }

    std::ifstream iniFile(iniPath);
    if (!iniFile.is_open()) {
        logger::error("Failed to open configuration file");
        return false;
    }

    std::string line;
    std::string currentSection;

    while (std::getline(iniFile, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        if (line[0] == '[' && line[line.length() - 1] == ']') {
            currentSection = line.substr(1, line.length() - 2);
            continue;
        }

        size_t equalPos = line.find('=');
        if (equalPos != std::string::npos) {
            std::string key = line.substr(0, equalPos);
            std::string value = line.substr(equalPos + 1);

            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            if (currentSection == "Quest") {
                if (key == "Enabled") {
                    g_config.quest.enabled = (value == "1" || value == "true" || value == "True");
                } else if (key == "QuestEditorID") {
                    g_config.quest.questEditorID = value;
                } else if (key == "QuestPlugin") {
                    g_config.quest.questPlugin = value;
                } else if (key == "TriggerStage") {
                    g_config.quest.triggerStage = std::stoi(value);
                } else if (key == "CompletionStage") {
                    g_config.quest.completionStage = std::stoi(value);
                } else if (key == "ShowNotification") {
                    g_config.quest.showNotification = (value == "1" || value == "true" || value == "True");
                }
            } else if (currentSection == "Item") {
                if (key == "Enabled") {
                    g_config.item.enabled = (value == "1" || value == "true" || value == "True");
                } else if (key == "ItemID") {
                    g_config.item.itemID = value;
                } else if (key == "ItemPlugin") {
                    g_config.item.itemPlugin = value;
                } else if (key == "ItemName") {
                    g_config.item.itemName = value;
                } else if (key == "RemoveOnDetection") {
                    g_config.item.removeOnDetection = (value == "1" || value == "true" || value == "True");
                } else if (key == "ShowNotification") {
                    g_config.item.showNotification = (value == "1" || value == "true" || value == "True");
                }
            } else if (currentSection == "Messages") {
                if (key == "Enabled") {
                    g_config.messages.enabled = (value == "1" || value == "true" || value == "True");
                } else if (key == "TriggerMessage") {
                    g_config.messages.triggerMessage = value;
                } else if (key == "CompletionMessage") {
                    g_config.messages.completionMessage = value;
                } else if (key == "ShowTriggerMessage") {
                    g_config.messages.showTriggerMessage = (value == "1" || value == "true" || value == "True");
                } else if (key == "ShowCompletionMessage") {
                    g_config.messages.showCompletionMessage = (value == "1" || value == "true" || value == "True");
                }
            } else if (currentSection == "Monitoring") {
                if (key == "Enabled") {
                    g_config.monitoring.enabled = (value == "1" || value == "true" || value == "True");
                } else if (key == "CheckIntervalMs") {
                    g_config.monitoring.checkIntervalMs = std::stoi(value);
                } else if (key == "MaxDetectionDistance") {
                    g_config.monitoring.maxDetectionDistance = std::stof(value);
                }
            } else if (currentSection == "Notification") {
                if (key == "Enabled") {
                    g_config.notification.enabled = (value == "1" || value == "true" || value == "True");
                }
            }
        }
    }

    iniFile.close();
    return true;
}

void ValidatePluginsInINI() {
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        return;
    }

    bool needsUpdate = false;

    if (g_config.quest.enabled) {
        auto* questPlugin = dataHandler->LookupModByName(g_config.quest.questPlugin);
        if (!questPlugin) {
            g_config.quest.enabled = false;
            needsUpdate = true;
            WriteToActionsLog("Plugin not found: " + g_config.quest.questPlugin + " - Disabled [Quest] in memory", __LINE__);
        }
    }

    if (g_config.item.enabled) {
        auto* itemPlugin = dataHandler->LookupModByName(g_config.item.itemPlugin);
        if (!itemPlugin) {
            g_config.item.enabled = false;
            needsUpdate = true;
            WriteToActionsLog("Plugin not found: " + g_config.item.itemPlugin + " - Disabled [Item] in memory", __LINE__);
        }
    }

    if (needsUpdate) {
        WriteToActionsLog("Plugin validation completed - Some features disabled in memory due to missing plugins", __LINE__);
        WriteToActionsLog("User INI files preserved - NO modifications made to configuration files", __LINE__);
    }
}

void ResolveFormIDs() {
    std::lock_guard<std::mutex> lock(g_cacheMutex);

    if (g_config.item.enabled && !g_cachedFormIDs.itemResolved) {
        g_cachedFormIDs.itemFormID = GetFormIDFromPlugin(g_config.item.itemPlugin, g_config.item.itemID);
        if (g_cachedFormIDs.itemFormID != 0) {
            g_cachedFormIDs.itemResolved = true;
            std::stringstream ss;
            ss << "Item (" << g_config.item.itemName << ") resolved successfully - FormID: 0x" 
               << std::hex << std::uppercase << g_cachedFormIDs.itemFormID;
            WriteToActionsLog(ss.str(), __LINE__);
        } else {
            WriteToActionsLog("WARNING: Item (" + g_config.item.itemName + ") FormID resolution failed", __LINE__);
        }
    }

    if (g_config.quest.enabled && !g_cachedFormIDs.questResolved) {
        auto* quest = GetQuestByEditorID(g_config.quest.questEditorID);
        if (quest) {
            g_cachedFormIDs.questFormID = quest->GetFormID();
            g_cachedFormIDs.questResolved = true;
            std::stringstream ss;
            ss << "Quest (" << g_config.quest.questEditorID << ") resolved successfully - FormID: 0x" 
               << std::hex << std::uppercase << g_cachedFormIDs.questFormID;
            WriteToQuestLog(ss.str(), __LINE__);
        } else {
            WriteToQuestLog("WARNING: Quest (" + g_config.quest.questEditorID + ") not found", __LINE__);
        }
    }
}

void CheckQuestState() {
    if (!g_config.quest.enabled) return;
    if (g_questState.completionDone) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastQuestCheck).count();
    
    if (elapsed < g_config.monitoring.checkIntervalMs) {
        return;
    }
    
    g_lastQuestCheck = now;

    auto* quest = GetQuestByEditorID(g_config.quest.questEditorID);
    if (!quest) {
        if (g_questState.questActive) {
            WriteToQuestLog("Quest no longer accessible: " + g_config.quest.questEditorID, __LINE__);
            g_questState.questActive = false;
        }
        return;
    }

    bool isRunning = quest->IsRunning();
    int currentStage = GetQuestCurrentStage(quest);

    if (!g_questState.questActive && isRunning) {
        g_questState.questActive = true;
        g_questState.currentStage = currentStage;
        WriteToQuestLog("========================================", __LINE__);
        WriteToQuestLog("QUEST ACTIVATED", __LINE__);
        WriteToQuestLog("Quest: " + g_config.quest.questEditorID, __LINE__);
        WriteToQuestLog("Current Stage: " + std::to_string(currentStage), __LINE__);
        WriteToQuestLog("Trigger Stage: " + std::to_string(g_config.quest.triggerStage), __LINE__);
        WriteToQuestLog("========================================", __LINE__);
    }

    if (g_questState.questActive && currentStage != g_questState.currentStage) {
        WriteToQuestLog("Quest stage changed: " + std::to_string(g_questState.currentStage) + 
                        " -> " + std::to_string(currentStage), __LINE__);
        g_questState.currentStage = currentStage;
    }

    if (g_questState.questActive && !g_questState.triggerStageReached) {
        if (currentStage >= g_config.quest.triggerStage) {
            ProcessQuestTrigger();
        }
    }

    if (!isRunning && g_questState.questActive) {
        g_questState.questActive = false;
        WriteToQuestLog("Quest is no longer running: " + g_config.quest.questEditorID, __LINE__);
    }
}

void ProcessQuestTrigger() {
    std::lock_guard<std::mutex> lock(g_questMutex);

    if (g_questState.triggerStageReached) return;

    g_questState.triggerStageReached = true;

    WriteToQuestLog("========================================", __LINE__);
    WriteToQuestLog("TRIGGER STAGE REACHED", __LINE__);
    WriteToQuestLog("Quest: " + g_config.quest.questEditorID, __LINE__);
    WriteToQuestLog("Stage: " + std::to_string(g_config.quest.triggerStage), __LINE__);
    WriteToQuestLog("========================================", __LINE__);

    if (g_config.messages.enabled && g_config.messages.showTriggerMessage && !g_triggerMessageShown) {
        g_triggerMessageShown = true;
        ShowMessageBox(g_config.messages.triggerMessage);
        WriteToActionsLog("Trigger message displayed to player", __LINE__);
    }

    if (g_config.notification.enabled && g_config.quest.showNotification) {
        ShowNotificationMessage("BWY-Fix - Quest stage " + std::to_string(g_config.quest.triggerStage) + " reached");
    }

    WriteToActionsLog("Now monitoring player inventory for: " + g_config.item.itemName, __LINE__);
}

void CheckPlayerInventory() {
    if (!g_config.item.enabled) return;
    if (!g_questState.triggerStageReached) return;
    if (g_questState.itemDetected) return;
    if (g_questState.completionDone) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastItemCheck).count();
    
    if (elapsed < g_config.monitoring.checkIntervalMs) {
        return;
    }
    
    g_lastItemCheck = now;

    if (!g_cachedFormIDs.itemResolved) {
        ResolveFormIDs();
        if (!g_cachedFormIDs.itemResolved) {
            return;
        }
    }

    if (PlayerHasItem(g_cachedFormIDs.itemFormID)) {
        ProcessItemDetection();
    }
}

void ProcessItemDetection() {
    std::lock_guard<std::mutex> lock(g_questMutex);

    if (g_questState.itemDetected) return;
    if (g_waitingForPotionDelay) return;

    g_questState.itemDetected = true;
    g_waitingForPotionDelay = true;
    g_potionDetectedTime = std::chrono::steady_clock::now();

    WriteToActionsLog("========================================", __LINE__);
    WriteToActionsLog("ITEM DETECTED IN PLAYER INVENTORY", __LINE__);
    WriteToActionsLog("Item: " + g_config.item.itemName, __LINE__);
    std::stringstream ss;
    ss << "FormID: 0x" << std::hex << std::uppercase << g_cachedFormIDs.itemFormID;
    WriteToActionsLog(ss.str(), __LINE__);
    WriteToActionsLog("Waiting 5 seconds before processing...", __LINE__);
    WriteToActionsLog("========================================", __LINE__);

    if (!g_potionDetectedMessageShown) {
        g_potionDetectedMessageShown = true;
        ShowNotificationMessage("BWY FIX: Potion detected to cure the priestess. Wait for Yulia to take it.");
    }
}

void ProcessDelayedQuestCompletion() {
    if (!g_waitingForPotionDelay) return;
    if (!g_questState.itemDetected) return;
    if (g_questState.completionDone) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - g_potionDetectedTime).count();

    if (elapsed < 5) {
        return;
    }

    g_waitingForPotionDelay = false;

    WriteToActionsLog("========================================", __LINE__);
    WriteToActionsLog("5 SECOND DELAY COMPLETE - PROCESSING QUEST", __LINE__);
    WriteToActionsLog("========================================", __LINE__);

    if (g_config.item.removeOnDetection) {
        if (RemoveItemFromPlayer(g_cachedFormIDs.itemFormID, 1)) {
            WriteToActionsLog("Item successfully removed from player inventory", __LINE__);
        } else {
            WriteToActionsLog("WARNING: Failed to remove item from player inventory", __LINE__);
        }
    }

    ProcessQuestCompletion();
}

void ProcessQuestCompletion() {
    if (g_questState.completionDone) return;

    WriteToQuestLog("========================================", __LINE__);
    WriteToQuestLog("PROCESSING QUEST COMPLETION VIA CONSOLE", __LINE__);
    WriteToQuestLog("Quest: " + g_config.quest.questEditorID, __LINE__);
    WriteToQuestLog("Target Stage: " + std::to_string(g_config.quest.completionStage), __LINE__);
    WriteToQuestLog("========================================", __LINE__);

    SetQuestStageViaConsole(g_config.quest.questEditorID, g_config.quest.completionStage);

    g_questState.completionDone = true;

    WriteToQuestLog("Console command sent successfully", __LINE__);

    if (g_config.messages.enabled && g_config.messages.showCompletionMessage && !g_completionMessageShown) {
        g_completionMessageShown = true;
        ShowNotificationMessage(g_config.messages.completionMessage);
        WriteToActionsLog("Completion notification displayed to player", __LINE__);
    }

    WriteToActionsLog("========================================", __LINE__);
    WriteToActionsLog("QUEST FIX COMPLETED SUCCESSFULLY", __LINE__);
    WriteToActionsLog("Quest: " + g_config.quest.questEditorID, __LINE__);
    WriteToActionsLog("Final Stage: " + std::to_string(g_config.quest.completionStage), __LINE__);
    WriteToActionsLog("========================================", __LINE__);
}

// ===== GAME EVENT PROCESSOR =====
class GameEventProcessor : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    GameEventProcessor() = default;
    ~GameEventProcessor() = default;
    GameEventProcessor(const GameEventProcessor&) = delete;
    GameEventProcessor(GameEventProcessor&&) = delete;
    GameEventProcessor& operator=(const GameEventProcessor&) = delete;
    GameEventProcessor& operator=(GameEventProcessor&&) = delete;

public:
    static GameEventProcessor& GetSingleton() {
        static GameEventProcessor singleton;
        return singleton;
    }

    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
                                          RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
        if (event) {
            std::stringstream msg;
            msg << "Menu " << event->menuName.c_str() << " " << (event->opening ? "opened" : "closed");
            WriteToActionsLog(msg.str(), __LINE__);
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

// ===== CONTAINER CHANGE EVENT SINK FOR INVENTORY MONITORING =====
class ContainerChangeEventSink : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
    ContainerChangeEventSink() = default;
    ~ContainerChangeEventSink() = default;
    ContainerChangeEventSink(const ContainerChangeEventSink&) = delete;
    ContainerChangeEventSink(ContainerChangeEventSink&&) = delete;
    ContainerChangeEventSink& operator=(const ContainerChangeEventSink&) = delete;
    ContainerChangeEventSink& operator=(ContainerChangeEventSink&&) = delete;

public:
    static ContainerChangeEventSink& GetSingleton() {
        static ContainerChangeEventSink singleton;
        return singleton;
    }

    RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* event,
                                          RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
        if (!event) {
            return RE::BSEventNotifyControl::kContinue;
        }

        if (!g_questState.triggerStageReached || g_questState.itemDetected || g_questState.completionDone) {
            return RE::BSEventNotifyControl::kContinue;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return RE::BSEventNotifyControl::kContinue;
        }

        RE::FormID playerFormID = player->GetFormID();

        if (event->newContainer == playerFormID) {
            if (g_cachedFormIDs.itemResolved && event->baseObj == g_cachedFormIDs.itemFormID) {
                WriteToActionsLog("========================================", __LINE__);
                WriteToActionsLog("CONTAINER CHANGE EVENT - ITEM ADDED TO PLAYER", __LINE__);
                std::stringstream ss;
                ss << "Item FormID: 0x" << std::hex << std::uppercase << event->baseObj;
                WriteToActionsLog(ss.str(), __LINE__);
                WriteToActionsLog("Item Count: " + std::to_string(event->itemCount), __LINE__);
                WriteToActionsLog("========================================", __LINE__);

                ProcessItemDetection();
            }
        }

        return RE::BSEventNotifyControl::kContinue;
    }
};

// ===== QUEST STAGE EVENT SINK =====
class QuestStageEventSink : public RE::BSTEventSink<RE::TESQuestStageEvent> {
    QuestStageEventSink() = default;
    ~QuestStageEventSink() = default;
    QuestStageEventSink(const QuestStageEventSink&) = delete;
    QuestStageEventSink(QuestStageEventSink&&) = delete;
    QuestStageEventSink& operator=(const QuestStageEventSink&) = delete;
    QuestStageEventSink& operator=(QuestStageEventSink&&) = delete;

public:
    static QuestStageEventSink& GetSingleton() {
        static QuestStageEventSink singleton;
        return singleton;
    }

    RE::BSEventNotifyControl ProcessEvent(const RE::TESQuestStageEvent* event,
                                          RE::BSTEventSource<RE::TESQuestStageEvent>*) override {
        if (!event) {
            return RE::BSEventNotifyControl::kContinue;
        }

        if (!g_config.quest.enabled) {
            return RE::BSEventNotifyControl::kContinue;
        }

        if (g_questState.completionDone) {
            return RE::BSEventNotifyControl::kContinue;
        }

        auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(event->formID);
        if (!quest) {
            return RE::BSEventNotifyControl::kContinue;
        }

        std::string questEditorID = quest->GetFormEditorID();
        if (questEditorID.empty()) {
            return RE::BSEventNotifyControl::kContinue;
        }

        if (questEditorID == g_config.quest.questEditorID) {
            int newStage = static_cast<int>(event->stage);

            WriteToQuestLog("========================================", __LINE__);
            WriteToQuestLog("QUEST STAGE EVENT RECEIVED", __LINE__);
            WriteToQuestLog("Quest: " + questEditorID, __LINE__);
            WriteToQuestLog("New Stage: " + std::to_string(newStage), __LINE__);
            WriteToQuestLog("========================================", __LINE__);

            g_questState.questActive = true;
            g_questState.currentStage = newStage;

            if (!g_questState.triggerStageReached && newStage >= g_config.quest.triggerStage) {
                ProcessQuestTrigger();
            }
        }

        return RE::BSEventNotifyControl::kContinue;
    }
};

// ===== MONITORING THREAD FUNCTION =====
void MonitoringThreadFunction() {
    WriteToSystemLog("Monitoring thread started - Watching quest state and player inventory", __LINE__);
    WriteToSystemLog("Monitoring on dual paths (Primary & Secondary)", __LINE__);
    WriteToSystemLog("Primary: " + g_logPaths.primary.string(), __LINE__);
    WriteToSystemLog("Secondary: " + g_logPaths.secondary.string(), __LINE__);
    WriteToSystemLog("Waiting 5 seconds before starting quest monitoring", __LINE__);

    g_monitoringStartTime = std::chrono::steady_clock::now();
    g_initialDelayComplete = false;

    while (g_monitoringActive && !g_isShuttingDown.load()) {
        
        if (g_isInGameTransition.load()) {
            WriteToSystemLog("Game transition detected - monitoring paused", __LINE__);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (!g_initialDelayComplete) {
            auto currentTime = std::chrono::steady_clock::now();
            auto elapsedSeconds =
                std::chrono::duration_cast<std::chrono::seconds>(currentTime - g_monitoringStartTime).count();
            if (elapsedSeconds < 5) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            } else {
                g_initialDelayComplete = true;
                WriteToSystemLog("5-second initial delay complete, starting quest monitoring", __LINE__);
                
                ResolveFormIDs();
            }
        }
        
        g_monitorCycles++;
        
        CheckQuestState();
        CheckPlayerInventory();
        ProcessDelayedQuestCompletion();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(g_config.monitoring.checkIntervalMs));
    }

    WriteToSystemLog("Monitoring thread stopped", __LINE__);
}

void StartMonitoringThread() {
    if (!g_monitoringActive) {
        g_monitoringActive = true;
        g_monitorCycles = 0;
        g_initialDelayComplete = false;
        
        g_questState.questActive = false;
        g_questState.triggerStageReached = false;
        g_questState.itemDetected = false;
        g_questState.completionDone = false;
        g_questState.currentStage = 0;
        
        g_triggerMessageShown = false;
        g_completionMessageShown = false;
        g_potionDetectedMessageShown = false;
        g_waitingForPotionDelay = false;
        
        g_cachedFormIDs.questFormID = 0;
        g_cachedFormIDs.itemFormID = 0;
        g_cachedFormIDs.questResolved = false;
        g_cachedFormIDs.itemResolved = false;
        
        g_lastQuestCheck = std::chrono::steady_clock::now();
        g_lastItemCheck = std::chrono::steady_clock::now();
        
        g_monitorThread = std::thread(MonitoringThreadFunction);

        WriteToSystemLog("MONITORING SYSTEM ACTIVATED", __LINE__);
    }
}

void StopMonitoringThread() {
    if (g_monitoringActive) {
        g_monitoringActive = false;
        if (g_monitorThread.joinable()) {
            g_monitorThread.join();
        }
        WriteToSystemLog("Monitoring thread stopped and joined", __LINE__);
    }
}

// ===== PLUGIN INITIALIZATION =====
void SetupLog() {
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) {
        SKSE::stl::report_and_fail("SKSE log_directory not provided, logs disabled.");
        return;
    }
    auto pluginName = SKSE::PluginDeclaration::GetSingleton()->GetName();
    auto logFilePath = *logsFolder / std::format("{}.log", pluginName);
    auto fileLoggerPtr = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
    auto loggerPtr = std::make_shared<spdlog::logger>("log", std::move(fileLoggerPtr));
    spdlog::set_default_logger(std::move(loggerPtr));
    spdlog::set_level(spdlog::level::trace);
    spdlog::flush_on(spdlog::level::info);
}

void InitializePlugin() {
    try {
        LoadConfiguration();

        g_documentsPath = GetDocumentsPath();
        g_gamePath = GetGamePath();
        g_logPaths = GetAllSKSELogsPaths();

        if (g_gamePath.empty()) {
            g_gamePath = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Skyrim Special Edition";
        }

        WriteToSystemLog("BWY-multi-Fix-NG Plugin - v6.2.2", __LINE__);
        WriteToActionsLog("BWY-multi-Fix-NG Actions Monitor - v6.2.2", __LINE__);
        WriteToQuestLog("BWY-multi-Fix-NG Quest Monitor - v6.2.2", __LINE__);

        WriteToSystemLog("========================================", __LINE__);
        WriteToSystemLog("PLUGIN CONFIGURATION LOADED", __LINE__);
        WriteToSystemLog("Quest Monitoring: " + std::string(g_config.quest.enabled ? "Enabled" : "Disabled"), __LINE__);
        WriteToSystemLog("Quest EditorID: " + g_config.quest.questEditorID, __LINE__);
        WriteToSystemLog("Quest Plugin: " + g_config.quest.questPlugin, __LINE__);
        WriteToSystemLog("Trigger Stage: " + std::to_string(g_config.quest.triggerStage), __LINE__);
        WriteToSystemLog("Completion Stage: " + std::to_string(g_config.quest.completionStage), __LINE__);
        WriteToSystemLog("Item Monitoring: " + std::string(g_config.item.enabled ? "Enabled" : "Disabled"), __LINE__);
        WriteToSystemLog("Item ID: " + g_config.item.itemID, __LINE__);
        WriteToSystemLog("Item Plugin: " + g_config.item.itemPlugin, __LINE__);
        WriteToSystemLog("Item Name: " + g_config.item.itemName, __LINE__);
        WriteToSystemLog("========================================", __LINE__);

        g_isInitialized = true;

        StartMonitoringThread();

    } catch (const std::exception& e) {
        logger::error("CRITICAL ERROR in Initialize: {}", e.what());
    }
}

void ShutdownPlugin() {
    WriteToSystemLog("PLUGIN SHUTTING DOWN", __LINE__);
    WriteToActionsLog("PLUGIN SHUTTING DOWN", __LINE__);
    WriteToQuestLog("PLUGIN SHUTTING DOWN", __LINE__);

    g_isShuttingDown = true;

    auto* eventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
    if (eventSourceHolder) {
        eventSourceHolder->RemoveEventSink(&ContainerChangeEventSink::GetSingleton());
        eventSourceHolder->RemoveEventSink(&QuestStageEventSink::GetSingleton());
        WriteToSystemLog("Event sinks unregistered", __LINE__);
    }

    StopMonitoringThread();

    WriteToSystemLog("========================================", __LINE__);
    WriteToSystemLog("Plugin shutdown complete at: " + GetCurrentTimeString(), __LINE__);
    WriteToSystemLog("========================================", __LINE__);
    
    WriteToQuestLog("========================================", __LINE__);
    WriteToQuestLog("Plugin shutdown complete at: " + GetCurrentTimeString(), __LINE__);
    WriteToQuestLog("========================================", __LINE__);

    WriteToActionsLog("========================================", __LINE__);
    WriteToActionsLog("Plugin shutdown complete at: " + GetCurrentTimeString(), __LINE__);
    WriteToActionsLog("========================================", __LINE__);
}

// ===== MODIFIED MAIN FUNCTION WITH IMPROVED DETECTION FOR WABBAJACK/MO2 =====
void MessageListener(SKSE::MessagingInterface::Message* message) {
    switch (message->type) {
        case SKSE::MessagingInterface::kNewGame:
        case SKSE::MessagingInterface::kPostLoadGame:
            {
                WriteToActionsLog("========================================", __LINE__);
                WriteToActionsLog("SESSION START (New/Load) - Resetting Logic State safely", __LINE__);
                
                g_isInGameTransition = false;
                g_isShuttingDown = false;
                
                g_initialDelayComplete = false;
                
                g_questState.questActive = false;
                g_questState.triggerStageReached = false;
                g_questState.itemDetected = false;
                g_questState.completionDone = false;
                g_questState.currentStage = 0;
                
                g_triggerMessageShown = false;
                g_completionMessageShown = false;
                g_potionDetectedMessageShown = false;
                g_waitingForPotionDelay = false;
                
                g_cachedFormIDs.questFormID = 0;
                g_cachedFormIDs.itemFormID = 0;
                g_cachedFormIDs.questResolved = false;
                g_cachedFormIDs.itemResolved = false;
                
                g_lastQuestCheck = std::chrono::steady_clock::now();
                g_lastItemCheck = std::chrono::steady_clock::now();
                
                WriteToActionsLog("Logic reset complete.", __LINE__);
                
                if (!g_monitoringActive) {
                    WriteToActionsLog("Monitoring thread logic: Starting...", __LINE__);
                    StartMonitoringThread();
                } else {
                    WriteToActionsLog("Monitoring thread logic: Already active, continuing.", __LINE__);
                }
                
                WriteToActionsLog("========================================", __LINE__);
            }
            break;

        case SKSE::MessagingInterface::kDataLoaded:
            {
                auto& eventProcessor = GameEventProcessor::GetSingleton();
                RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&eventProcessor);
                
                auto* eventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
                if (eventSourceHolder) {
                    eventSourceHolder->AddEventSink(&ContainerChangeEventSink::GetSingleton());
                    eventSourceHolder->AddEventSink(&QuestStageEventSink::GetSingleton());
                    WriteToSystemLog("Container and Quest event sinks registered", __LINE__);
                }
                
                if (!g_isInitialized) {
                    LoadConfiguration();

                    g_documentsPath = GetDocumentsPath();
                    g_gamePath = GetGamePath();
                    g_logPaths = GetAllSKSELogsPaths();

                    if (g_gamePath.empty()) {
                        g_gamePath = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Skyrim Special Edition";
                    }
                    
                    WriteToSystemLog("BWY-multi-Fix-NG Plugin - v6.2.2 (DataLoaded)", __LINE__);
                    g_isInitialized = true;
                }
                
                ValidatePluginsInINI();
                
                if (!g_monitoringActive) {
                    StartMonitoringThread();
                }

                WriteToSystemLog("========================================", __LINE__);
                WriteToSystemLog("DATA LOADED - Plugin fully initialized", __LINE__);
                WriteToSystemLog("Quest to monitor: " + g_config.quest.questEditorID, __LINE__);
                WriteToSystemLog("Trigger stage: " + std::to_string(g_config.quest.triggerStage), __LINE__);
                WriteToSystemLog("Completion stage: " + std::to_string(g_config.quest.completionStage), __LINE__);
                WriteToSystemLog("Item to detect: " + g_config.item.itemName + " (" + g_config.item.itemID + ")", __LINE__);
                WriteToSystemLog("========================================", __LINE__);
            }
            break;

        case SKSE::MessagingInterface::kPreLoadGame:
            {
                WriteToActionsLog("Pre-load game detected - preparing for state reset", __LINE__);
                g_isInGameTransition = true;
            }
            break;

        case SKSE::MessagingInterface::kPostLoad:
            {
                WriteToSystemLog("Post-load message received", __LINE__);
            }
            break;

        case SKSE::MessagingInterface::kInputLoaded:
            {
                WriteToSystemLog("Input loaded message received", __LINE__);
            }
            break;

        default:
            break;
    }
}

// ===== SKSE PLUGIN LOAD - TRUNCATE LOGS ON GAME START =====
SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    SetupLog();

    logger::info("BWY-multi-Fix-NG Plugin v6.2.2 - Starting");

    auto logsFolder = SKSE::log::log_directory();
    if (logsFolder) {
        std::ofstream(*logsFolder / "BWY-multi-Fix-NG-Actions.log", std::ios::trunc).close();
        std::ofstream(*logsFolder / "BWY-multi-Fix-NG-Quest.log", std::ios::trunc).close();
        std::ofstream(*logsFolder / "BWY-multi-Fix-NG-System.log", std::ios::trunc).close();
        logger::info("Custom log files truncated successfully");
    }

    SKSE::GetMessagingInterface()->RegisterListener(MessageListener);

    logger::info("BWY-multi-Fix-NG Plugin loaded successfully");
    logger::info("Monitoring quest: YW_Quest_MDF");
    logger::info("Trigger stage: 21, Completion stage: 30");
    logger::info("Target item: Lelyna's Remedy (625C7C from YurianaWench.esp)");

    return true;
}

// ===== PLUGIN VERSION DECLARATION =====
constinit auto SKSEPlugin_Version = []() {
    SKSE::PluginVersionData v;
    v.PluginVersion({6, 2, 2});
    v.PluginName("BWY-multi-Fix-NG Quest Monitor");
    v.AuthorName("John95AC");
    v.UsesAddressLibrary();
    v.UsesSigScanning();
    v.CompatibleVersions({SKSE::RUNTIME_SSE_LATEST, SKSE::RUNTIME_LATEST_VR});

    return v;
}();