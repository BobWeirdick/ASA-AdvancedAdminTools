#include "Include.h"

#pragma comment(lib, "AsaApi.lib")
#pragma comment(lib, "mysqlclient.lib")

namespace
{
	constexpr const wchar_t* kDefeatBossCmdName = L"AdvancedAdminTools.DefeatBoss";
	constexpr const wchar_t* kDumpBossesChatCmd = L"/dumpbosses";
	constexpr const char* kDefeatBossUsage =
		"Usage: AdvancedAdminTools.DefeatBoss <EOSID> <BossName...> [Difficulty]\n"
		"Difficulty: 0=Gamma (default), 1=Beta, 2=Alpha\n"
		"Note: Tekgrams unlock immediately; implant/level visuals often require die/respawn.";

	using BossAscensionMap = TMap<FName, int, FDefaultSetAllocator, TDefaultMapHashableKeyFuncs<FName, int, 0>>;

	bool g_dumpBossesRegistered = false;

	bool IsDifficultyToken(const FString& token, int& out_difficulty)
	{
		if (token.Equals(L"0"))
		{
			out_difficulty = 0;
			return true;
		}
		if (token.Equals(L"1"))
		{
			out_difficulty = 1;
			return true;
		}
		if (token.Equals(L"2"))
		{
			out_difficulty = 2;
			return true;
		}
		return false;
	}

	const char* DifficultyLabel(int difficulty)
	{
		switch (difficulty)
		{
		case 0: return "Gamma";
		case 1: return "Beta";
		case 2: return "Alpha";
		default: return "Unknown";
		}
	}

	void SendRconReply(RCONClientConnection* connection, int packet_id, const FString& message)
	{
		if (!connection)
			return;
		FString reply = message;
		connection->SendMessageW(packet_id, 0, &reply);
	}

	bool IsAdmin(AShooterPlayerController* pc)
	{
		return pc && pc->bIsAdmin()();
	}

	UPrimalPlayerData* GetPlayerData(AShooterPlayerController* pc)
	{
		if (!pc)
			return nullptr;

		if (AShooterCharacter* character = pc->GetPlayerCharacter())
		{
			if (UPrimalPlayerData* data = character->GetPlayerData())
				return data;
		}

		if (AShooterPlayerState* ps = static_cast<AShooterPlayerState*>(pc->PlayerStateField().Get()))
			return ps->MyPlayerDataField();

		return nullptr;
	}

	std::string FNameToUtf8(const FName& name)
	{
		FString out;
		name.ToString(out);
		return out.ToString();
	}
}

void InitServerKey(std::string& outServerKey)
{
	outServerKey.clear();
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	FString param(L"-serverkey=");
	if (argv)
	{
		for (int i = 0; i < argc; i++)
		{
			FString arg(argv[i]);
			if (arg.Contains(param) && arg.RemoveFromStart(param))
			{
				outServerKey = arg.ToString();
				break;
			}
		}
		LocalFree(argv);
	}
}

bool LoadConfig()
{
	try
	{
		const std::string config_path = GetDllPath() + "config.json";
		std::ifstream file(config_path);
		if (!file.is_open())
		{
			Log::GetLog()->error("{}: Failed to open {}", PROJECT_NAME, config_path);
			return false;
		}

		json j;
		file >> j;

		Config.Debug = JsonGetBool(j, "Debug");

		const auto mysql_it = j.find("Mysql");
		if (mysql_it != j.end() && mysql_it->is_object())
		{
			const json& mysql = *mysql_it;
			Config.Mysql.Host = JsonGetString(mysql, "Host");
			Config.Mysql.User = JsonGetString(mysql, "User");
			Config.Mysql.Password = JsonGetString(mysql, "Password");
			Config.Mysql.Database = JsonGetString(mysql, "Database");
			Config.Mysql.Port = mysql.value("Port", 3306);
		}

		Config.ServerNames.clear();
		const auto names_it = j.find("ServerNames");
		if (names_it != j.end() && names_it->is_object())
		{
			for (auto it = names_it->begin(); it != names_it->end(); ++it)
			{
				if (it.value().is_string())
					Config.ServerNames[it.key()] = it.value().get<std::string>();
			}
		}

		Config.Loaded = true;
		return true;
	}
	catch (const std::exception& e)
	{
		Log::GetLog()->error("{}: Failed to load config: {}", PROJECT_NAME, e.what());
		return false;
	}
}

std::string DefeatBossExecute(const FString& cmd)
{
	TArray<FString> parsed;
	cmd.ParseIntoArray(parsed, L" ", true);

	// parsed[0]=command, [1]=EOSID, [2...]=BossName..., optional trailing Difficulty
	if (!parsed.IsValidIndex(2))
		return kDefeatBossUsage;

	const FString& eos_id = parsed[1];

	int difficulty = 0;
	int boss_end = parsed.Num();
	if (parsed.Num() >= 4)
	{
		int parsed_difficulty = 0;
		if (IsDifficultyToken(parsed[parsed.Num() - 1], parsed_difficulty))
		{
			difficulty = parsed_difficulty;
			boss_end = parsed.Num() - 1;
		}
	}

	if (boss_end <= 2)
		return kDefeatBossUsage;

	FString boss_name = parsed[2];
	for (int i = 3; i < boss_end; ++i)
	{
		boss_name += L" ";
		boss_name += parsed[i];
	}

	AShooterPlayerController* pc = AsaApi::GetApiUtils().FindPlayerFromEOSID(eos_id);
	if (!pc)
		return "Target player is not online";

	const uint64 player_id = AsaApi::GetApiUtils().GetPlayerID(pc);
	if (player_id == 0)
		return "Failed to resolve player ID for target";

	auto* cheat_manager = static_cast<UShooterCheatManager*>(pc->CheatManagerField().Get());
	if (!cheat_manager)
		return "Target player has no CheatManager";

	const std::string boss_name_utf8 = boss_name.ToString();
	const FName boss_fname(boss_name_utf8.c_str(), EFindName::FNAME_Add);
	cheat_manager->DefeatBoss(static_cast<int>(player_id), boss_fname, static_cast<char>(difficulty));

	const std::string success = fmt::format(
		"DefeatBoss applied for EOS {} (PlayerID {}) boss '{}' difficulty {}. Tekgrams unlock immediately; implant/level visuals often require die/respawn.",
		eos_id.ToString(),
		player_id,
		boss_name.ToString(),
		difficulty);

	Log::GetLog()->info("{}: {}", PROJECT_NAME, success);
	return {};
}

void DefeatBossCmd(APlayerController* player_controller, FString* cmd, bool)
{
	const std::string error = DefeatBossExecute(cmd ? *cmd : FString());
	auto* shooter = static_cast<AShooterPlayerController*>(player_controller);
	if (!shooter)
		return;

	if (!error.empty())
	{
		AsaApi::GetApiUtils().SendServerMessage(shooter, FColorList::Red, error.c_str());
		Log::GetLog()->error("{}: {}", PROJECT_NAME, error);
		return;
	}

	AsaApi::GetApiUtils().SendServerMessage(
		shooter,
		FColorList::Green,
		"DefeatBoss applied. Implant/level visuals often require die/respawn.");
}

void DefeatBossRcon(RCONClientConnection* rcon_connection, RCONPacket* rcon_packet, UWorld*)
{
	if (!rcon_packet)
		return;

	const std::string error = DefeatBossExecute(rcon_packet->Body);
	if (!error.empty())
	{
		SendRconReply(rcon_connection, rcon_packet->Id, FString(error.c_str()));
		Log::GetLog()->error("{}: {}", PROJECT_NAME, error);
		return;
	}

	SendRconReply(
		rcon_connection,
		rcon_packet->Id,
		FString("DefeatBoss applied. Implant/level visuals often require die/respawn."));
}

void ChatCmd_DumpBosses(AShooterPlayerController* pc, FString* message, int /*mode*/, int /*platform*/)
{
	if (!pc)
		return;

	if (!Config.Debug)
	{
		AsaApi::GetApiUtils().SendChatMessage(pc, FString(PROJECT_NAME), L"Debug is disabled.");
		return;
	}

	if (!IsAdmin(pc))
	{
		AsaApi::GetApiUtils().SendChatMessage(pc, FString(PROJECT_NAME), L"Admin only.");
		return;
	}

	AShooterPlayerController* target = pc;
	FString target_eos = AsaApi::GetApiUtils().GetEOSIDFromController(pc);

	TArray<FString> parsed;
	if (message)
		message->ParseIntoArray(parsed, L" ", true);

	if (parsed.IsValidIndex(1) && !parsed[1].IsEmpty())
	{
		target_eos = parsed[1];
		target = AsaApi::GetApiUtils().FindPlayerFromEOSID(target_eos);
		if (!target)
		{
			AsaApi::GetApiUtils().SendChatMessage(pc, FString(PROJECT_NAME), L"Target player is not online.");
			return;
		}
	}

	UPrimalPlayerData* data = GetPlayerData(target);
	if (!data)
	{
		AsaApi::GetApiUtils().SendChatMessage(pc, FString(PROJECT_NAME), L"Failed to resolve player data.");
		return;
	}

	const int* num_ascensions = GetNativePointerField<int*>(data, "UPrimalPlayerDataBP_Base_C.NumAscensions");
	BossAscensionMap* boss_map = GetNativePointerField<BossAscensionMap*>(
		data, "UPrimalPlayerDataBP_Base_C.BossDinoNameTagAscensionDataMap");

	if (!boss_map)
	{
		AsaApi::GetApiUtils().SendChatMessage(pc, FString(PROJECT_NAME), L"BossDinoNameTagAscensionDataMap not found.");
		return;
	}

	const int ascension_count = num_ascensions ? *num_ascensions : -1;
	const int entry_count = boss_map->Num();
	const uint64 player_id = AsaApi::GetApiUtils().GetPlayerID(target);

	Log::GetLog()->info(
		"{}: [dumpbosses] EOS={} PlayerID={} NumAscensions={} BossEntries={}",
		PROJECT_NAME,
		target_eos.ToString(),
		player_id,
		ascension_count,
		entry_count);

	AsaApi::GetApiUtils().SendChatMessage(
		pc,
		FString(PROJECT_NAME),
		fmt::format(
			"Dumping boss progress for {} ({} entries). See server log [dumpbosses].",
			target_eos.ToString(),
			entry_count).c_str());

	if (entry_count == 0)
	{
		Log::GetLog()->info("{}: [dumpbosses] (no completed bosses in BossDinoNameTagAscensionDataMap)", PROJECT_NAME);
		AsaApi::GetApiUtils().SendChatMessage(pc, FString(PROJECT_NAME), "No completed bosses found on this player.");
		return;
	}

	int index = 0;
	for (const auto& pair : *boss_map)
	{
		const std::string boss_name = FNameToUtf8(pair.Key);
		const int difficulty = pair.Value;
		Log::GetLog()->info(
			"{}: [dumpbosses]   [{}] '{}' difficulty={} ({})",
			PROJECT_NAME,
			index,
			boss_name,
			difficulty,
			DifficultyLabel(difficulty));

		// Keep chat readable; log has the full list.
		if (index < 12)
		{
			AsaApi::GetApiUtils().SendChatMessage(
				pc,
				FString(PROJECT_NAME),
				fmt::format("{} = {} ({})", boss_name, difficulty, DifficultyLabel(difficulty)).c_str());
		}
		++index;
	}

	if (entry_count > 12)
	{
		AsaApi::GetApiUtils().SendChatMessage(
			pc,
			FString(PROJECT_NAME),
			fmt::format("...and {} more (full list in server log).", entry_count - 12).c_str());
	}
}

extern "C" __declspec(dllexport) void Plugin_Init()
{
	Log::Get().Init(PROJECT_NAME);
	InitServerKey(g_ServerKey);

	if (!LoadConfig())
	{
		Log::GetLog()->error("{}: Failed to load config", PROJECT_NAME);
		return;
	}

	if (Config.Debug && !g_ServerKey.empty())
		Log::GetLog()->info("{}: -serverkey={}", PROJECT_NAME, g_ServerKey);

	AsaApi::GetCommands().AddConsoleCommand(kDefeatBossCmdName, &DefeatBossCmd);
	AsaApi::GetCommands().AddRconCommand(kDefeatBossCmdName, &DefeatBossRcon);

	g_dumpBossesRegistered = false;
	if (Config.Debug)
	{
		AsaApi::GetCommands().AddChatCommand(kDumpBossesChatCmd, &ChatCmd_DumpBosses);
		g_dumpBossesRegistered = true;
		Log::GetLog()->info("{}: Debug chat command enabled: /dumpbosses [EOSID]", PROJECT_NAME);
	}

	Log::GetLog()->info("{}: Plugin loaded successfully.", PROJECT_NAME);
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
	AsaApi::GetCommands().RemoveConsoleCommand(kDefeatBossCmdName);
	AsaApi::GetCommands().RemoveRconCommand(kDefeatBossCmdName);

	if (g_dumpBossesRegistered)
	{
		AsaApi::GetCommands().RemoveChatCommand(kDumpBossesChatCmd);
		g_dumpBossesRegistered = false;
	}

	Config.Loaded = false;
	Log::GetLog()->info("{}: Plugin unloaded.", PROJECT_NAME);
}
