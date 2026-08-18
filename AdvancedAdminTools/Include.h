#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <Windows.h>
#include <shellapi.h>

#include <nlohmann/json.hpp>

#include "API/ARK/Ark.h"

#if __has_include(<mysql.h>)
#include <mysql.h>
#elif __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#elif __has_include(<mariadb/mysql.h>)
#include <mariadb/mysql.h>
#else
#error "MariaDB/MySQL C headers were not found. Ensure C:\\Repos\\MySQL\\Includes is available."
#endif

#ifndef PROJECT_NAME
#define PROJECT_NAME "AdvancedAdminTools"
#endif

using json = nlohmann::json;

struct MysqlConfig
{
	std::string Host;
	std::string User;
	std::string Password;
	std::string Database;
	int Port = 3306;
};

struct PluginConfig
{
	bool Debug = false;
	MysqlConfig Mysql;
	std::unordered_map<std::string, std::string> ServerNames;
	bool Loaded = false;
};

inline PluginConfig Config;
inline std::string g_ServerKey;

inline std::string GetDllPath()
{
	return fmt::format("{}\\ArkApi\\Plugins\\{}\\", AsaApi::Tools::GetCurrentDir(), PROJECT_NAME);
}

inline std::string JsonGetString(const json& j, const char* key, std::string default_val = {})
{
	const auto it = j.find(key);
	if (it == j.end() || it->is_null() || !it->is_string())
		return default_val;
	return it->get<std::string>();
}

inline bool JsonGetBool(const json& j, const char* key, bool default_val = false)
{
	const auto it = j.find(key);
	if (it == j.end() || it->is_null() || !it->is_boolean())
		return default_val;
	return it->get<bool>();
}

void InitServerKey(std::string& outServerKey);
bool LoadConfig();

std::string DefeatBossExecute(const FString& cmd);
void DefeatBossCmd(APlayerController* player_controller, FString* cmd, bool);
void DefeatBossRcon(RCONClientConnection* rcon_connection, RCONPacket* rcon_packet, UWorld*);

void ChatCmd_DumpBosses(AShooterPlayerController* pc, FString* message, int mode, int platform);

std::string CompleteMissionExecute(const FString& cmd);
void CompleteMissionCmd(APlayerController* player_controller, FString* cmd, bool);
void CompleteMissionRcon(RCONClientConnection* rcon_connection, RCONPacket* rcon_packet, UWorld*);

std::string CompleteGenesisMissionsExecute(const FString& cmd);
void CompleteGenesisMissionsCmd(APlayerController* player_controller, FString* cmd, bool);
void CompleteGenesisMissionsRcon(RCONClientConnection* rcon_connection, RCONPacket* rcon_packet, UWorld*);

void ChatCmd_DumpMissions(AShooterPlayerController* pc, FString* message, int mode, int platform);

std::string ListTribeDinosExecute(const FString& cmd);
void ListTribeDinosCmd(APlayerController* player_controller, FString* cmd, bool);
void ListTribeDinosRcon(RCONClientConnection* rcon_connection, RCONPacket* rcon_packet, UWorld*);

std::string TeleportDinoToPlayerExecute(const FString& cmd);
void TeleportDinoToPlayerCmd(APlayerController* player_controller, FString* cmd, bool);
void TeleportDinoToPlayerRcon(RCONClientConnection* rcon_connection, RCONPacket* rcon_packet, UWorld*);

std::string TeleportDinoToLocationExecute(const FString& cmd);
void TeleportDinoToLocationCmd(APlayerController* player_controller, FString* cmd, bool);
void TeleportDinoToLocationRcon(RCONClientConnection* rcon_connection, RCONPacket* rcon_packet, UWorld*);
