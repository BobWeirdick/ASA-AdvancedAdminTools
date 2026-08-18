#include "Include.h"

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "AsaApi.lib")
#pragma comment(lib, "mysqlclient.lib")

namespace
{
	constexpr const wchar_t* kDefeatBossCmd = L"AdvancedAdminTools.DefeatBoss";
	constexpr const wchar_t* kCompleteMissionCmd = L"AdvancedAdminTools.CompleteMission";
	constexpr const wchar_t* kCompleteGenesisCmd = L"AdvancedAdminTools.CompleteGenesisMissions";
	constexpr const wchar_t* kListTribeDinosCmd = L"AdvancedAdminTools.ListTribeDinos";
	constexpr const wchar_t* kTeleportDinoToPlayerCmd = L"AdvancedAdminTools.TeleportDinoToPlayer";
	constexpr const wchar_t* kTeleportDinoToLocationCmd = L"AdvancedAdminTools.TeleportDinoToLocation";
	constexpr const wchar_t* kDumpBossesCmd = L"/dumpbosses";
	constexpr const wchar_t* kDumpMissionsCmd = L"/dumpmissions";

	constexpr const char* kDefeatBossUsage =
		"Usage: AdvancedAdminTools.DefeatBoss <EOSID> <BossName> [Difficulty]\n"
		"Difficulty: 0=Gamma, 1=Beta, 2=Alpha\n"
		"Tekgrams apply immediately. Extra levels need respawn/relog. Implant needs death+respawn.";

	constexpr const char* kCompleteMissionUsage =
		"Usage: AdvancedAdminTools.CompleteMission <EOSID> <MissionTag> [Difficulty]\n"
		"Difficulty: 0=Gamma/Easy, 1=Beta/Medium, 2=Alpha/Hard\n"
		"Player must be online on a Genesis map.";

	constexpr const char* kCompleteGenesisUsage =
		"Usage: AdvancedAdminTools.CompleteGenesisMissions <EOSID> <Difficulty>\n"
		"Difficulty: 0=Gamma/Easy, 1=Beta/Medium, 2=Alpha/Hard";

	constexpr const char* kListTribeDinosUsage =
		"Usage: AdvancedAdminTools.ListTribeDinos <TribeID>";

	constexpr const char* kTeleportDinoToPlayerUsage =
		"Usage: AdvancedAdminTools.TeleportDinoToPlayer <DinoIndex> <EOSID>";

	constexpr const char* kTeleportDinoToLocationUsage =
		"Usage: AdvancedAdminTools.TeleportDinoToLocation <DinoIndex> <X> <Y> <Z>";

	using BossAscensionMap = TMap<FName, int, FDefaultSetAllocator, TDefaultMapHashableKeyFuncs<FName, int, 0>>;

	struct LocalMissionMetaData
	{
		FName MissionTag;
		TSubclassOf<UObject> MissionMetaDataClass;
		uint8_t Pad[112]{};
	};

	bool g_dumpBossesRegistered = false;
	bool g_dumpMissionsRegistered = false;

	bool IsDifficultyToken(const FString& token, int& out_difficulty)
	{
		if (token.Equals(L"0")) { out_difficulty = 0; return true; }
		if (token.Equals(L"1")) { out_difficulty = 1; return true; }
		if (token.Equals(L"2")) { out_difficulty = 2; return true; }
		return false;
	}

	const char* DifficultyLabel(int difficulty)
	{
		switch (difficulty)
		{
		case 0: return "Gamma/Easy";
		case 1: return "Beta/Medium";
		case 2: return "Alpha/Hard";
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

	std::string StripOkPrefix(const std::string& result, bool& out_ok)
	{
		static constexpr const char* kPrefix = "OK:";
		if (result.rfind(kPrefix, 0) == 0)
		{
			out_ok = true;
			return result.substr(std::char_traits<char>::length(kPrefix));
		}
		out_ok = false;
		return result;
	}

	void ReplyConsole(APlayerController* player_controller, const std::string& result)
	{
		auto* shooter = static_cast<AShooterPlayerController*>(player_controller);
		if (!shooter)
			return;

		bool ok = false;
		const std::string message = StripOkPrefix(result, ok);
		AsaApi::GetApiUtils().SendServerMessage(shooter, ok ? FColorList::Green : FColorList::Red, message.c_str());
		if (!ok)
			Log::GetLog()->error("{}: {}", PROJECT_NAME, message);
	}

	void ReplyRcon(RCONClientConnection* connection, int packet_id, const std::string& result)
	{
		bool ok = false;
		const std::string message = StripOkPrefix(result, ok);
		SendRconReply(connection, packet_id, FString(message.c_str()));
		if (!ok)
			Log::GetLog()->error("{}: {}", PROJECT_NAME, message);
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

	// BP fields on UPrimalPlayerDataBP_Base_C are missing from the AsaApi cache.
	template<typename T>
	T* FindUPropertyPtr(UObject* object, const char* field_name)
	{
		if (!object)
			return nullptr;

		UClass* cls = object->ClassPrivateField();
		if (!cls)
			return nullptr;

		const FName name(field_name, EFindName::FNAME_Add);
		FProperty* prop = cls->FindPropertyByName(name);
		if (!prop)
			return nullptr;

		return reinterpret_cast<T*>(reinterpret_cast<char*>(object) + prop->Offset_InternalField());
	}

	BossAscensionMap* GetBossAscensionMap(UPrimalPlayerData* data)
	{
		return FindUPropertyPtr<BossAscensionMap>(data, "BossDinoNameTagAscensionDataMap");
	}

	int* GetNumAscensions(UPrimalPlayerData* data)
	{
		return FindUPropertyPtr<int>(data, "NumAscensions");
	}

	int LookupBossMapDifficulty(BossAscensionMap* map, const FName& boss)
	{
		if (!map)
			return -1;
		for (const auto& pair : *map)
		{
			if (pair.Key == boss)
				return pair.Value;
		}
		return -1;
	}

	UShooterCheatManager* FindUsableCheatManager(AShooterPlayerController* preferred)
	{
		if (preferred)
		{
			if (auto* cm = static_cast<UShooterCheatManager*>(preferred->CheatManagerField().Get()))
				return cm;
		}

		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		if (!world)
			return nullptr;

		auto& controllers = world->PlayerControllerListField();
		for (int i = 0; i < controllers.Num(); ++i)
		{
			auto* pc = static_cast<AShooterPlayerController*>(controllers[i].Get());
			if (!pc || pc == preferred)
				continue;
			if (auto* cm = static_cast<UShooterCheatManager*>(pc->CheatManagerField().Get()))
				return cm;
		}
		return nullptr;
	}

	void PersistPlayerData(AShooterPlayerController* pc, UPrimalPlayerData* data)
	{
		if (!data)
			return;

		if (UWorld* world = AsaApi::GetApiUtils().GetWorld())
			data->SavePlayerData(world, false);

		if (pc)
			pc->ForceNetUpdate(false, true, false);
		if (AShooterCharacter* character = pc ? pc->GetPlayerCharacter() : nullptr)
			character->ForceNetUpdate(false, true, false);
	}

	std::string FNameToUtf8(const FName& name)
	{
		FString out;
		name.ToString(out);
		return out.ToString();
	}

	FString CanonicalMissionTag(const FString& raw)
	{
		const wchar_t* str = *raw;
		if (!str || !*str)
			return raw;

		const wchar_t* last_dot = nullptr;
		const wchar_t* last_slash = nullptr;
		for (const wchar_t* p = str; *p; ++p)
		{
			if (*p == L'.')
				last_dot = p;
			else if (*p == L'/')
				last_slash = p;
		}

		FString out = last_dot ? FString(last_dot + 1) : (last_slash ? FString(last_slash + 1) : raw);
		out.RemoveFromEnd(L"_C");
		return out;
	}

	FName CanonicalMissionFName(const FName& tag)
	{
		FString raw;
		tag.ToString(raw);
		const FString canon = CanonicalMissionTag(raw);
		if (canon.IsEmpty())
			return FName();
		return FName(canon.ToString().c_str(), EFindName::FNAME_Add);
	}

	std::string MissionTagLabel(const FName& tag)
	{
		const FName canon = CanonicalMissionFName(tag);
		return canon.IsNone() ? FNameToUtf8(tag) : FNameToUtf8(canon);
	}

	bool TagMatchesDifficulty(const FString& tag, int difficulty)
	{
		const FString canon = CanonicalMissionTag(tag);
		switch (difficulty)
		{
		case 0: return canon.EndsWith(L"_Easy") || canon.EndsWith(L"_Gamma");
		case 1: return canon.EndsWith(L"_Medium") || canon.EndsWith(L"_Beta");
		case 2: return canon.EndsWith(L"_Hard") || canon.EndsWith(L"_Alpha");
		default: return false;
		}
	}

	bool TagHasDifficultySuffix(const FString& tag)
	{
		const FString canon = CanonicalMissionTag(tag);
		return canon.EndsWith(L"_Easy") || canon.EndsWith(L"_Medium") || canon.EndsWith(L"_Hard")
			|| canon.EndsWith(L"_Gamma") || canon.EndsWith(L"_Beta") || canon.EndsWith(L"_Alpha");
	}

	const wchar_t* const* DifficultySuffixes(int difficulty, int& out_count)
	{
		static const wchar_t* kGamma[] = { L"_Easy", L"_Gamma" };
		static const wchar_t* kBeta[] = { L"_Medium", L"_Beta" };
		static const wchar_t* kAlpha[] = { L"_Hard", L"_Alpha" };
		switch (difficulty)
		{
		case 0: out_count = 2; return kGamma;
		case 1: out_count = 2; return kBeta;
		case 2: out_count = 2; return kAlpha;
		default: out_count = 0; return nullptr;
		}
	}

	APrimalWorldSettings* GetPrimalWorldSettings()
	{
		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		if (!world)
			return nullptr;
		AWorldSettings* ws = world->GetWorldSettings(false, true);
		return ws ? static_cast<APrimalWorldSettings*>(ws) : nullptr;
	}

	FString GetCurrentMapName()
	{
		FString name;
		if (UWorld* world = AsaApi::GetApiUtils().GetWorld())
			world->GetMapName(&name);
		return name;
	}

	bool IsSafeUObject(UObject* obj)
	{
		return obj && UVictoryCore::BPIsValidLowLevelFast(obj);
	}

	bool IsMissionTypeClass(UClass* cls)
	{
		if (!cls)
			return false;
		UClass* base = NativeCall<UClass*>(nullptr, "AMissionType.StaticClass()");
		return base && cls->IsChildOf(base);
	}

	FName MissionTagFromClassName(UClass* cls)
	{
		if (!cls)
			return FName();

		FString name;
		cls->NamePrivateField().ToString(name);
		name.RemoveFromEnd(L"_C");
		if (name.IsEmpty())
			return FName();
		return FName(name.ToString().c_str(), EFindName::FNAME_Add);
	}

	FName GetMissionTagFromClass(UClass* mission_class)
	{
		if (!IsSafeUObject(mission_class) || !IsMissionTypeClass(mission_class))
			return FName();

		TSubclassOf<AMissionType> sub(mission_class);
		FName tag = NativeCall<FName, TSubclassOf<AMissionType>*>(
			nullptr,
			"AMissionType.GetMissionTagForMission(TSubclassOf<AMissionType>)",
			&sub);
		if (!tag.IsNone())
			return tag;

		return MissionTagFromClassName(mission_class);
	}

	void AppendUniqueTag(std::vector<FName>& tags, const FName& tag)
	{
		if (tag.IsNone())
			return;
		for (const FName& existing : tags)
		{
			if (existing == tag)
				return;
		}
		tags.push_back(tag);
	}

	int GetScriptStructSize(const char* static_struct_key, const wchar_t* reflected_name, int fallback)
	{
		try
		{
			if (UScriptStruct* ss = NativeCall<UScriptStruct*>(nullptr, static_struct_key))
			{
				const int size = static_cast<UStruct*>(ss)->PropertiesSizeField();
				if (size > 0)
					return size;
			}
		}
		catch (...)
		{
		}

		if (UScriptStruct* script_struct = FindScriptStruct(reflected_name))
		{
			const int size = static_cast<UStruct*>(script_struct)->PropertiesSizeField();
			if (size > 0)
				return size;
		}

		return fallback;
	}

	int GetMissionMetaStride()
	{
		static int stride = 0;
		if (stride <= 0)
			stride = GetScriptStructSize("FMissionMetaData.StaticStruct()", L"FMissionMetaData", 16);
		return stride;
	}

	int GetAvailableMissionStride()
	{
		static int stride = 0;
		if (stride <= 0)
			stride = GetScriptStructSize("FAvailableMission.StaticStruct()", L"FAvailableMission", 0x30);
		return stride;
	}

	UClass* ReadAvailableMissionClass(uint8_t* elem)
	{
		if (!elem)
			return nullptr;

		try
		{
			if (auto* sub = GetNativePointerField<TSubclassOf<UObject>*>(elem, "FAvailableMission.MissionClass"))
				return sub->uClass;
		}
		catch (...)
		{
		}

		return *reinterpret_cast<UClass**>(elem);
	}

	void CollectTagsFromMissionMetaArray(
		TArray<FMissionMetaData, TSizedDefaultAllocator<32>>& arr,
		std::vector<FName>& tags)
	{
		uint8_t* raw = reinterpret_cast<uint8_t*>(arr.GetData());
		const int count = arr.Num();
		const int stride = GetMissionMetaStride();
		if (!raw || count <= 0 || stride < static_cast<int>(sizeof(FName)))
			return;

		for (int i = 0; i < count; ++i)
		{
			uint8_t* elem = raw + (static_cast<size_t>(i) * stride);
			FName tag;
			try
			{
				tag = *GetNativePointerField<FName*>(elem, "FMissionMetaData.MissionTag");
			}
			catch (...)
			{
				tag = *reinterpret_cast<FName*>(elem);
			}
			AppendUniqueTag(tags, tag);
		}
	}

	void CollectClassesFromAvailableMissions(
		TArray<FAvailableMission, TSizedDefaultAllocator<32>>& arr,
		std::vector<UClass*>& classes,
		std::vector<FName>* tags)
	{
		uint8_t* raw = reinterpret_cast<uint8_t*>(arr.GetData());
		const int count = arr.Num();
		const int stride = GetAvailableMissionStride();
		if (!raw || count <= 0 || stride < static_cast<int>(sizeof(void*)))
			return;

		classes.reserve(classes.size() + static_cast<size_t>(count));
		for (int i = 0; i < count; ++i)
		{
			UClass* cls = ReadAvailableMissionClass(raw + (static_cast<size_t>(i) * stride));
			if (!IsSafeUObject(cls) || !IsMissionTypeClass(cls))
				continue;

			classes.push_back(cls);
			if (tags)
				AppendUniqueTag(*tags, GetMissionTagFromClass(cls));
		}
	}

	void CollectWorldAvailableMissionClasses(std::vector<UClass*>& classes)
	{
		if (APrimalWorldSettings* ws = GetPrimalWorldSettings())
			CollectClassesFromAvailableMissions(ws->AvailableMissionsField(), classes, nullptr);
	}

	bool MissionClassMatchesTag(UClass* cls, const FName& tag)
	{
		if (!cls || tag.IsNone())
			return false;

		FString want;
		tag.ToString(want);
		const FString want_canon = CanonicalMissionTag(want);

		FString cls_str;
		GetMissionTagFromClass(cls).ToString(cls_str);
		if (CanonicalMissionTag(cls_str).Equals(want_canon, ESearchCase::IgnoreCase))
			return true;

		FString name_str;
		MissionTagFromClassName(cls).ToString(name_str);
		return CanonicalMissionTag(name_str).Equals(want_canon, ESearchCase::IgnoreCase);
	}

	UClass* FindAvailableMissionClassByTag(const FName& tag)
	{
		std::vector<UClass*> classes;
		CollectWorldAvailableMissionClasses(classes);
		for (UClass* cls : classes)
		{
			if (MissionClassMatchesTag(cls, tag))
				return cls;
		}
		return nullptr;
	}

	// Gen1 stores the list on APrimalWorldSettings.AvailableMissions. VictoryCore is often empty.
	std::vector<FName> GetAvailableMissionTags()
	{
		std::vector<FName> tags;
		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		if (!world)
			return tags;

		TArray<FName, TSizedDefaultAllocator<32>> victory;
		UVictoryCore::GetAllAvailableMissionsAsTags(&victory, world);
		for (int i = 0; i < victory.Num(); ++i)
			AppendUniqueTag(tags, victory[i]);

		if (APrimalWorldSettings* ws = GetPrimalWorldSettings())
		{
			CollectTagsFromMissionMetaArray(ws->AvailableMissionsMetaDataField(), tags);
			CollectTagsFromMissionMetaArray(ws->NonPlayerFacingMissionsMetaDataField(), tags);

			std::vector<UClass*> classes;
			CollectClassesFromAvailableMissions(ws->AvailableMissionsField(), classes, &tags);
		}

		if (UPrimalGameData* gd = AsaApi::GetApiUtils().GetGameData())
		{
			std::vector<UClass*> classes;
			CollectClassesFromAvailableMissions(gd->AvailableMissionsField(), classes, &tags);
		}

		if (Config.Debug)
		{
			Log::GetLog()->info(
				"{}: [missions] map={} count={}",
				PROJECT_NAME,
				GetCurrentMapName().ToString(),
				tags.size());
		}

		return tags;
	}

	UClass* FindMissionClassByTag(UObject* world_context, const FName& tag)
	{
		if (!world_context || tag.IsNone())
			return nullptr;

		TArray<TSubclassOf<AMissionType>, TSizedDefaultAllocator<32>> matches;
		NativeCall<void, UObject*, FName, TArray<TSubclassOf<AMissionType>, TSizedDefaultAllocator<32>>*>(
			nullptr,
			"AMissionType.FindMissionsMatchingTag(UObject*,FName,TArray<TSubclassOf<AMissionType>,TSizedDefaultAllocator<32>>&)",
			world_context,
			tag,
			&matches);

		for (int i = 0; i < matches.Num(); ++i)
		{
			UClass* cls = matches[i].uClass;
			if (IsMissionTypeClass(cls))
				return cls;
		}

		return nullptr;
	}

	UClass* TryLoadMissionClass(const FString& tag)
	{
		if (tag.IsEmpty())
			return nullptr;

		if (tag.StartsWith(L"/Game/"))
		{
			FString path = tag;
			path.RemoveFromEnd(L"_C");

			FString bp = L"Blueprint'";
			bp += path;
			bp += L"'";
			if (UClass* cls = UVictoryCore::BPLoadClass(bp); IsMissionTypeClass(cls))
				return cls;
			if (UClass* cls = UVictoryCore::BPLoadClass(path); IsMissionTypeClass(cls))
				return cls;
		}

		const FString short_name = tag + L"_C";
		if (UObject* found = Globals::StaticFindObject(nullptr, nullptr, *short_name, false))
		{
			auto* cls = static_cast<UClass*>(found);
			if (IsMissionTypeClass(cls))
				return cls;
		}

		static const wchar_t* kFolders[] = {
			L"/Game/Genesis/Missions/Race",
			L"/Game/Genesis/Missions/Race/Arctic",
			L"/Game/Genesis/Missions/Hunt/Arctic",
			L"/Game/Genesis/Missions/Hunt/Bog",
			L"/Game/Genesis/Missions/Hunt/Lunar",
			L"/Game/Genesis/Missions/Hunt/Ocean",
			L"/Game/Genesis/Missions/Hunt/Volcanic",
			L"/Game/Genesis/Missions/Escort",
			L"/Game/Genesis/Missions/GatherNodes",
			L"/Game/Genesis/Missions/GauntletWaves",
			L"/Game/Genesis/Missions/Retrieve",
			L"/Game/Genesis/Missions/Sports/DodoBall/Bog",
			L"/Game/Genesis/Missions/Sports/DodoBall/Lunar",
			L"/Game/Genesis/Missions/Fishing",
			L"/Game/Genesis/Missions/EelBossFight",
			L"/Game/Genesis/Missions/VRBattle",
			L"/Game/Genesis/Missions/VRBattle/GammaMissions",
			L"/Game/Genesis/Missions/VRBattle/BetaMissions",
			L"/Game/Genesis/Missions/VRBattle/AlphaMissions",
			L"/Game/Genesis/Missions",
			L"/Game/Genesis2/Missions/Hunt/HuntMissions",
			L"/Game/Genesis2/Missions/Race/RaceMissions",
			L"/Game/Genesis2/Missions/Race2",
			L"/Game/Genesis2/Missions/ModularMission",
			L"/Game/Genesis2/Missions",
		};

		for (const wchar_t* folder : kFolders)
		{
			FString path = L"Blueprint'";
			path += folder;
			path += L"/";
			path += tag;
			path += L".";
			path += tag;
			path += L"'";
			if (UClass* cls = UVictoryCore::BPLoadClass(path); IsMissionTypeClass(cls))
				return cls;
		}

		return nullptr;
	}

	UClass* ResolveMissionClass(const FName& tag)
	{
		if (UClass* cls = FindAvailableMissionClassByTag(tag))
			return cls;

		if (UClass* cls = FindMissionClassByTag(AsaApi::GetApiUtils().GetWorld(), tag))
			return cls;

		if (APrimalWorldSettings* ws = GetPrimalWorldSettings())
		{
			LocalMissionMetaData meta{};
			if (ws->GetMissionMetaData(tag, reinterpret_cast<FMissionMetaData*>(&meta)))
			{
				UClass* meta_cls = meta.MissionMetaDataClass.uClass;
				if (IsMissionTypeClass(meta_cls))
					return meta_cls;
			}
		}

		FString tag_str;
		tag.ToString(tag_str);
		return TryLoadMissionClass(CanonicalMissionTag(tag_str));
	}

	AMissionType* GetMissionCdo(UClass* mission_class)
	{
		if (!IsSafeUObject(mission_class) || !IsMissionTypeClass(mission_class))
			return nullptr;
		auto* cdo = static_cast<AMissionType*>(mission_class->GetDefaultObject(true));
		return IsSafeUObject(cdo) ? cdo : nullptr;
	}

	bool HasCompletedMission(UClass* mission_class, AShooterCharacter* character)
	{
		if (!mission_class || !character)
			return false;

		TSubclassOf<AMissionType> sub(mission_class);
		int version = 0;
		double complete_time = 0.0;
		return NativeCall<bool, TSubclassOf<AMissionType>*, AShooterCharacter*, int*, double*>(
			nullptr,
			"AMissionType.HasPlayerCompletedMission(TSubclassOf<AMissionType>,AShooterCharacter*,int&,double&)",
			&sub,
			character,
			&version,
			&complete_time);
	}

	bool GrantMission(UClass* mission_class, AShooterCharacter* character)
	{
		AMissionType* cdo = GetMissionCdo(mission_class);
		if (!cdo || !character)
			return false;

		NativeCall<void, AShooterCharacter*, bool, bool, int, bool, double>(
			cdo,
			"AMissionType.SetMissionCompletedStatus(AShooterCharacter*,bool,bool,int,bool,double)",
			character,
			true,
			false,
			0,
			false,
			0.0);
		return true;
	}

	bool MissionExists(const FName& tag)
	{
		return !tag.IsNone() && ResolveMissionClass(tag) != nullptr;
	}

	std::string ResolveMissionTag(
		const FString& mission_base,
		int difficulty,
		const std::vector<FName>& available_tags,
		FName& out_tag)
	{
		out_tag = FName();
		std::vector<FName> candidates;
		const FString base = CanonicalMissionTag(mission_base);

		for (const FName& tag : available_tags)
		{
			FString tag_str;
			tag.ToString(tag_str);
			if (CanonicalMissionTag(tag_str).Equals(base, ESearchCase::IgnoreCase))
			{
				candidates.push_back(tag);
				break;
			}
		}

		if (candidates.empty())
		{
			int suffix_count = 0;
			const wchar_t* const* suffixes = DifficultySuffixes(difficulty, suffix_count);
			for (int s = 0; s < suffix_count; ++s)
			{
				const FString wanted = base + suffixes[s];
				for (const FName& tag : available_tags)
				{
					FString tag_str;
					tag.ToString(tag_str);
					if (CanonicalMissionTag(tag_str).Equals(wanted, ESearchCase::IgnoreCase))
						candidates.push_back(tag);
				}
			}
		}

		if (candidates.empty())
		{
			const FString prefix = base + L"_";
			for (const FName& tag : available_tags)
			{
				FString tag_str;
				tag.ToString(tag_str);
				const FString canon = CanonicalMissionTag(tag_str);
				if (canon.StartsWith(prefix, ESearchCase::IgnoreCase) && TagMatchesDifficulty(canon, difficulty))
					candidates.push_back(tag);
			}
		}

		if (candidates.empty())
		{
			std::vector<FString> synthetic;
			if (TagHasDifficultySuffix(base))
			{
				synthetic.push_back(base);
			}
			else
			{
				int suffix_count = 0;
				const wchar_t* const* suffixes = DifficultySuffixes(difficulty, suffix_count);
				for (int s = 0; s < suffix_count; ++s)
					synthetic.push_back(base + suffixes[s]);
				synthetic.push_back(base);
			}

			for (const FString& candidate_str : synthetic)
			{
				const FName candidate(candidate_str.ToString().c_str(), EFindName::FNAME_Add);
				if (MissionExists(candidate))
					candidates.push_back(candidate);
			}
		}

		if (candidates.size() > 1 && !TagHasDifficultySuffix(base))
		{
			std::vector<FName> narrowed;
			for (const FName& candidate : candidates)
			{
				FString tag_str;
				candidate.ToString(tag_str);
				if (TagMatchesDifficulty(tag_str, difficulty))
					narrowed.push_back(candidate);
			}
			if (!narrowed.empty())
				candidates = std::move(narrowed);
		}

		if (candidates.empty())
		{
			return fmt::format(
				"Unknown mission '{}' at difficulty {} ({}) on {}. {} missions listed.",
				mission_base.ToString(),
				difficulty,
				DifficultyLabel(difficulty),
				GetCurrentMapName().ToString(),
				available_tags.size());
		}

		if (candidates.size() > 1)
		{
			std::string list;
			for (size_t i = 0; i < candidates.size(); ++i)
			{
				if (i)
					list += ", ";
				list += MissionTagLabel(candidates[i]);
			}
			return fmt::format("Ambiguous mission '{}': {}", mission_base.ToString(), list);
		}

		out_tag = candidates[0];
		return {};
	}

	struct OnlineTarget
	{
		AShooterPlayerController* Pc = nullptr;
		AShooterCharacter* Character = nullptr;
	};

	std::string ResolveOnlineTarget(const FString& eos_id, OnlineTarget& out)
	{
		out = {};
		out.Pc = AsaApi::GetApiUtils().FindPlayerFromEOSID(eos_id);
		if (!out.Pc)
			return "Target player is not online";

		out.Character = out.Pc->GetPlayerCharacter();
		if (!out.Character)
			return "Target player has no character (spawn in first)";

		return {};
	}

	struct ListedDinoId
	{
		unsigned int Id1 = 0;
		unsigned int Id2 = 0;
	};

	std::vector<ListedDinoId> g_listedDinos;

	FRotator GetActorRotator(AActor* actor)
	{
		if (!actor)
			return FRotator();

		USceneComponent* root = actor->RootComponentField().Get();
		if (!root)
			return FRotator();

		return root->RelativeRotationField();
	}

	std::string GetDinoSpecies(APrimalDinoCharacter* dino)
	{
		if (!dino)
			return "Dino";

		const std::string tag = FNameToUtf8(dino->DinoNameTagField());
		return tag.empty() ? "Dino" : tag;
	}

	std::string GetDinoDisplayName(APrimalDinoCharacter* dino, const std::string& species)
	{
		if (!dino)
			return species;

		const std::string tamed = dino->TamedNameField().ToString();
		return tamed.empty() ? species : tamed;
	}

	int GetDinoLevel(APrimalDinoCharacter* dino)
	{
		if (!dino)
			return 0;

		if (UPrimalCharacterStatusComponent* status = dino->MyCharacterStatusComponentField())
			return status->GetCharacterLevel();

		return 0;
	}

	void CollectTribeDinos(int tribe_id, std::vector<APrimalDinoCharacter*>& out)
	{
		out.clear();

		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		if (!world)
			return;

		UClass* dino_class = APrimalDinoCharacter::GetPrivateStaticClass();
		if (!dino_class)
			return;

		TArray<AActor*> actors;
		TSubclassOf<AActor> subclass(dino_class);
		UGameplayStatics::GetAllActorsOfClassInTribe(world, subclass, &actors, tribe_id);

		for (int i = 0; i < actors.Num(); ++i)
		{
			AActor* actor = actors[i];
			if (!actor)
				continue;

			auto* dino = static_cast<APrimalDinoCharacter*>(actor);
			if (dino->IsDead())
				continue;
			if (dino->TargetingTeamField() != tribe_id)
				continue;

			out.push_back(dino);
		}
	}

	APrimalDinoCharacter* ResolveListedDino(int index, std::string& err)
	{
		if (g_listedDinos.empty())
		{
			err = "No dino list. Run ListTribeDinos first.";
			return nullptr;
		}

		if (index < 1 || index > static_cast<int>(g_listedDinos.size()))
		{
			err = fmt::format("Unknown dino index {}. List has {}.", index, g_listedDinos.size());
			return nullptr;
		}

		UWorld* world = AsaApi::GetApiUtils().GetWorld();
		if (!world)
		{
			err = "World not ready";
			return nullptr;
		}

		const ListedDinoId& id = g_listedDinos[static_cast<size_t>(index) - 1];
		APrimalDinoCharacter* dino = APrimalDinoCharacter::FindDinoWithID(world, id.Id1, id.Id2);
		if (!dino || dino->IsDead())
		{
			err = "Dino is no longer in the world";
			return nullptr;
		}

		return dino;
	}

	bool TeleportDino(APrimalDinoCharacter* dino, const FVector& dest)
	{
		if (!dino)
			return false;

		FVector loc = dest;
		FRotator rot = GetActorRotator(dino);
		if (!dino->TeleportTo(&loc, &rot, false, true))
			return false;

		dino->ForceNetUpdate(false, true, false);
		return true;
	}

	bool ParseCccCoords(const TArray<FString>& parsed, int start, FVector& out)
	{
		std::string joined;
		for (int i = start; i < parsed.Num(); ++i)
		{
			if (!joined.empty())
				joined += ' ';
			joined += parsed[i].ToString();
		}

		for (char& c : joined)
		{
			if (c == ',')
				c = ' ';
		}

		std::istringstream iss(joined);
		std::vector<std::string> toks;
		for (std::string tok; iss >> tok; )
			toks.push_back(tok);

		size_t i = 0;
		if (!toks.empty())
		{
			std::string first = toks[0];
			for (char& c : first)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			if (first == "setplayerpos")
				++i;
		}

		if (toks.size() < i + 3)
			return false;

		try
		{
			out.X = std::stod(toks[i]);
			out.Y = std::stod(toks[i + 1]);
			out.Z = std::stod(toks[i + 2]);
		}
		catch (...)
		{
			return false;
		}

		return true;
	}

	bool ParseIntToken(const FString& token, int& out)
	{
		try
		{
			size_t idx = 0;
			const std::string raw = token.ToString();
			out = std::stoi(raw, &idx, 10);
			return idx == raw.size();
		}
		catch (...)
		{
			return false;
		}
	}

	bool ResolveDumpTarget(
		AShooterPlayerController* pc,
		FString* message,
		AShooterPlayerController*& target,
		FString& eos)
	{
		if (!Config.Debug)
		{
			AsaApi::GetApiUtils().SendChatMessage(pc, FString(PROJECT_NAME), L"Debug is disabled.");
			return false;
		}

		if (!IsAdmin(pc))
		{
			AsaApi::GetApiUtils().SendChatMessage(pc, FString(PROJECT_NAME), L"Admin only.");
			return false;
		}

		target = pc;
		eos = AsaApi::GetApiUtils().GetEOSIDFromController(pc);

		TArray<FString> parsed;
		if (message)
			message->ParseIntoArray(parsed, L" ", true);

		if (parsed.IsValidIndex(1) && !parsed[1].IsEmpty())
		{
			eos = parsed[1];
			target = AsaApi::GetApiUtils().FindPlayerFromEOSID(eos);
			if (!target)
			{
				AsaApi::GetApiUtils().SendChatMessage(pc, FString(PROJECT_NAME), L"Target player is not online.");
				return false;
			}
		}

		return true;
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

	UPrimalPlayerData* data = GetPlayerData(pc);
	if (!data)
		return "Failed to resolve player data (spawn in first)";

	const int linked_player_id = pc->GetLinkedPlayerID();
	if (linked_player_id == 0)
		return "Failed to resolve LinkedPlayerID for target";

	BossAscensionMap* boss_map = GetBossAscensionMap(data);
	const FName boss_fname(boss_name.ToString().c_str(), EFindName::FNAME_Add);
	const int before = LookupBossMapDifficulty(boss_map, boss_fname);

	data->BPForceDefeatedBoss(difficulty, boss_fname, pc);

	if (UShooterCheatManager* cheat_manager = FindUsableCheatManager(pc))
		cheat_manager->DefeatBoss(linked_player_id, boss_fname, static_cast<char>(difficulty));
	else
		Log::GetLog()->warn("{}: No CheatManager; applied BPForceDefeatedBoss only", PROJECT_NAME);

	PersistPlayerData(pc, data);

	const int after = LookupBossMapDifficulty(boss_map, boss_fname);
	if (boss_map && after < difficulty)
	{
		Log::GetLog()->warn(
			"{}: Boss map tag '{}' is {} after apply (wanted {})",
			PROJECT_NAME,
			boss_name.ToString(),
			after,
			difficulty);
	}

	const std::string success = fmt::format(
		"DefeatBoss applied for {} boss '{}' difficulty {} ({}){}",
		eos_id.ToString(),
		boss_name.ToString(),
		difficulty,
		DifficultyLabel(difficulty),
		(boss_map && after >= 0) ? fmt::format(" [{}->{}]", before, after) : "");

	Log::GetLog()->info("{}: {}", PROJECT_NAME, success);
	return std::string("OK:") + success;
}

void DefeatBossCmd(APlayerController* player_controller, FString* cmd, bool)
{
	ReplyConsole(player_controller, DefeatBossExecute(cmd ? *cmd : FString()));
}

void DefeatBossRcon(RCONClientConnection* rcon_connection, RCONPacket* rcon_packet, UWorld*)
{
	if (!rcon_packet)
		return;
	ReplyRcon(rcon_connection, rcon_packet->Id, DefeatBossExecute(rcon_packet->Body));
}

std::string CompleteMissionExecute(const FString& cmd)
{
	TArray<FString> parsed;
	cmd.ParseIntoArray(parsed, L" ", true);

	if (!parsed.IsValidIndex(2))
		return kCompleteMissionUsage;

	const FString& eos_id = parsed[1];

	int difficulty = 0;
	int mission_end = parsed.Num();
	if (parsed.Num() >= 4)
	{
		int parsed_difficulty = 0;
		if (IsDifficultyToken(parsed[parsed.Num() - 1], parsed_difficulty))
		{
			difficulty = parsed_difficulty;
			mission_end = parsed.Num() - 1;
		}
	}

	if (mission_end <= 2)
		return kCompleteMissionUsage;

	FString mission_base = parsed[2];
	for (int i = 3; i < mission_end; ++i)
	{
		mission_base += L"_";
		mission_base += parsed[i];
	}

	if (TagHasDifficultySuffix(mission_base) && mission_end == parsed.Num())
		difficulty = 0;

	OnlineTarget target;
	if (const std::string err = ResolveOnlineTarget(eos_id, target); !err.empty())
		return err;

	FName resolved_tag;
	if (const std::string err = ResolveMissionTag(mission_base, difficulty, GetAvailableMissionTags(), resolved_tag); !err.empty())
		return err;

	UPrimalPlayerData* data = GetPlayerData(target.Pc);
	if (!data)
		return "Failed to resolve player data (spawn in first)";

	UClass* mission_class = ResolveMissionClass(resolved_tag);
	if (!mission_class)
		return fmt::format("Could not load mission class for '{}'", MissionTagLabel(resolved_tag));

	if (HasCompletedMission(mission_class, target.Character))
	{
		return std::string("OK:") + fmt::format(
			"Mission '{}' already completed for {}",
			MissionTagLabel(resolved_tag),
			eos_id.ToString());
	}

	if (!GrantMission(mission_class, target.Character))
		return "Failed to complete mission";

	PersistPlayerData(target.Pc, data);

	const std::string success = fmt::format(
		"CompleteMission applied for {} '{}' difficulty {} ({})",
		eos_id.ToString(),
		MissionTagLabel(resolved_tag),
		difficulty,
		DifficultyLabel(difficulty));
	Log::GetLog()->info("{}: {}", PROJECT_NAME, success);
	return std::string("OK:") + success;
}

void CompleteMissionCmd(APlayerController* player_controller, FString* cmd, bool)
{
	ReplyConsole(player_controller, CompleteMissionExecute(cmd ? *cmd : FString()));
}

void CompleteMissionRcon(RCONClientConnection* rcon_connection, RCONPacket* rcon_packet, UWorld*)
{
	if (!rcon_packet)
		return;
	ReplyRcon(rcon_connection, rcon_packet->Id, CompleteMissionExecute(rcon_packet->Body));
}

std::string CompleteGenesisMissionsExecute(const FString& cmd)
{
	TArray<FString> parsed;
	cmd.ParseIntoArray(parsed, L" ", true);

	if (!parsed.IsValidIndex(2))
		return kCompleteGenesisUsage;

	const FString& eos_id = parsed[1];
	int difficulty = 0;
	if (!IsDifficultyToken(parsed[2], difficulty))
		return kCompleteGenesisUsage;

	OnlineTarget target;
	if (const std::string err = ResolveOnlineTarget(eos_id, target); !err.empty())
		return err;

	const std::vector<FName> available_tags = GetAvailableMissionTags();
	if (available_tags.empty())
		return "No missions on this map";

	UPrimalPlayerData* data = GetPlayerData(target.Pc);
	if (!data)
		return "Failed to resolve player data (spawn in first)";

	int credited = 0;
	int skipped = 0;
	int failed = 0;

	for (const FName& tag : available_tags)
	{
		FString tag_str;
		tag.ToString(tag_str);
		if (!TagMatchesDifficulty(tag_str, difficulty))
			continue;

		UClass* mission_class = ResolveMissionClass(tag);
		if (HasCompletedMission(mission_class, target.Character))
		{
			++skipped;
			continue;
		}

		if (!GrantMission(mission_class, target.Character))
		{
			++failed;
			if (Config.Debug)
				Log::GetLog()->warn("{}: failed to complete '{}'", PROJECT_NAME, MissionTagLabel(tag));
			continue;
		}

		++credited;
	}

	PersistPlayerData(target.Pc, data);

	const std::string summary = fmt::format(
		"CompleteGenesisMissions for {} difficulty {} ({}): credited={}, skipped={}, failed={}",
		eos_id.ToString(),
		difficulty,
		DifficultyLabel(difficulty),
		credited,
		skipped,
		failed);
	Log::GetLog()->info("{}: {}", PROJECT_NAME, summary);

	if (credited == 0 && skipped == 0)
		return fmt::format("No missions matched difficulty {} ({})", difficulty, DifficultyLabel(difficulty));

	return std::string("OK:") + summary;
}

void CompleteGenesisMissionsCmd(APlayerController* player_controller, FString* cmd, bool)
{
	ReplyConsole(player_controller, CompleteGenesisMissionsExecute(cmd ? *cmd : FString()));
}

void CompleteGenesisMissionsRcon(RCONClientConnection* rcon_connection, RCONPacket* rcon_packet, UWorld*)
{
	if (!rcon_packet)
		return;
	ReplyRcon(rcon_connection, rcon_packet->Id, CompleteGenesisMissionsExecute(rcon_packet->Body));
}

std::string ListTribeDinosExecute(const FString& cmd)
{
	TArray<FString> parsed;
	cmd.ParseIntoArray(parsed, L" ", true);

	int tribe_id = 0;
	if (!parsed.IsValidIndex(1) || !ParseIntToken(parsed[1], tribe_id))
		return kListTribeDinosUsage;

	std::vector<APrimalDinoCharacter*> dinos;
	CollectTribeDinos(tribe_id, dinos);

	g_listedDinos.clear();
	g_listedDinos.reserve(dinos.size());

	std::string body = fmt::format("{} dinos for tribe {}", dinos.size(), tribe_id);
	for (size_t i = 0; i < dinos.size(); ++i)
	{
		APrimalDinoCharacter* dino = dinos[i];
		g_listedDinos.push_back({ dino->DinoID1Field(), dino->DinoID2Field() });

		const std::string species = GetDinoSpecies(dino);
		const std::string name = GetDinoDisplayName(dino, species);
		const int level = GetDinoLevel(dino);
		const FVector loc = dino->GetLocation();

		body += fmt::format(
			"\n#{} {} (Level {} {}) {:.1f} {:.1f} {:.1f}",
			i + 1,
			name,
			level,
			species,
			loc.X,
			loc.Y,
			loc.Z);
	}

	Log::GetLog()->info("{}: ListTribeDinos tribe={} count={}", PROJECT_NAME, tribe_id, dinos.size());
	return std::string("OK:") + body;
}

void ListTribeDinosCmd(APlayerController* player_controller, FString* cmd, bool)
{
	ReplyConsole(player_controller, ListTribeDinosExecute(cmd ? *cmd : FString()));
}

void ListTribeDinosRcon(RCONClientConnection* rcon_connection, RCONPacket* rcon_packet, UWorld*)
{
	if (!rcon_packet)
		return;
	ReplyRcon(rcon_connection, rcon_packet->Id, ListTribeDinosExecute(rcon_packet->Body));
}

std::string TeleportDinoToPlayerExecute(const FString& cmd)
{
	TArray<FString> parsed;
	cmd.ParseIntoArray(parsed, L" ", true);

	int index = 0;
	if (!parsed.IsValidIndex(2) || !ParseIntToken(parsed[1], index))
		return kTeleportDinoToPlayerUsage;

	const FString& eos_id = parsed[2];

	std::string err;
	APrimalDinoCharacter* dino = ResolveListedDino(index, err);
	if (!dino)
		return err;

	OnlineTarget target;
	if (const std::string target_err = ResolveOnlineTarget(eos_id, target); !target_err.empty())
		return target_err;

	if (!TeleportDino(dino, target.Character->GetLocation()))
		return "Teleport failed";

	const std::string success = fmt::format(
		"Teleported dino #{} to player {}",
		index,
		eos_id.ToString());
	Log::GetLog()->info("{}: {}", PROJECT_NAME, success);
	return std::string("OK:") + success;
}

void TeleportDinoToPlayerCmd(APlayerController* player_controller, FString* cmd, bool)
{
	ReplyConsole(player_controller, TeleportDinoToPlayerExecute(cmd ? *cmd : FString()));
}

void TeleportDinoToPlayerRcon(RCONClientConnection* rcon_connection, RCONPacket* rcon_packet, UWorld*)
{
	if (!rcon_packet)
		return;
	ReplyRcon(rcon_connection, rcon_packet->Id, TeleportDinoToPlayerExecute(rcon_packet->Body));
}

std::string TeleportDinoToLocationExecute(const FString& cmd)
{
	TArray<FString> parsed;
	cmd.ParseIntoArray(parsed, L" ", true);

	int index = 0;
	if (!parsed.IsValidIndex(1) || !ParseIntToken(parsed[1], index))
		return kTeleportDinoToLocationUsage;

	FVector dest;
	if (!ParseCccCoords(parsed, 2, dest))
		return kTeleportDinoToLocationUsage;

	std::string err;
	APrimalDinoCharacter* dino = ResolveListedDino(index, err);
	if (!dino)
		return err;

	if (!TeleportDino(dino, dest))
		return "Teleport failed";

	const std::string success = fmt::format(
		"Teleported dino #{} to {:.1f} {:.1f} {:.1f}",
		index,
		dest.X,
		dest.Y,
		dest.Z);
	Log::GetLog()->info("{}: {}", PROJECT_NAME, success);
	return std::string("OK:") + success;
}

void TeleportDinoToLocationCmd(APlayerController* player_controller, FString* cmd, bool)
{
	ReplyConsole(player_controller, TeleportDinoToLocationExecute(cmd ? *cmd : FString()));
}

void TeleportDinoToLocationRcon(RCONClientConnection* rcon_connection, RCONPacket* rcon_packet, UWorld*)
{
	if (!rcon_packet)
		return;
	ReplyRcon(rcon_connection, rcon_packet->Id, TeleportDinoToLocationExecute(rcon_packet->Body));
}

void ChatCmd_DumpBosses(AShooterPlayerController* pc, FString* message, int /*mode*/, int /*platform*/)
{
	if (!pc)
		return;

	AShooterPlayerController* target = nullptr;
	FString target_eos;
	if (!ResolveDumpTarget(pc, message, target, target_eos))
		return;

	UPrimalPlayerData* data = GetPlayerData(target);
	if (!data)
	{
		AsaApi::GetApiUtils().SendChatMessage(pc, FString(PROJECT_NAME), L"Failed to resolve player data.");
		return;
	}

	BossAscensionMap* boss_map = GetBossAscensionMap(data);
	if (!boss_map)
	{
		AsaApi::GetApiUtils().SendChatMessage(pc, FString(PROJECT_NAME), L"Boss map not found.");
		return;
	}

	const int* num_ascensions = GetNumAscensions(data);
	const int entry_count = boss_map->Num();

	Log::GetLog()->info(
		"{}: [dumpbosses] EOS={} PlayerID={} NumAscensions={} entries={}",
		PROJECT_NAME,
		target_eos.ToString(),
		AsaApi::GetApiUtils().GetPlayerID(target),
		num_ascensions ? *num_ascensions : -1,
		entry_count);

	AsaApi::GetApiUtils().SendChatMessage(
		pc,
		FString(PROJECT_NAME),
		fmt::format("Boss dump for {} ({} entries). Full list in server log.", target_eos.ToString(), entry_count).c_str());

	if (entry_count == 0)
	{
		AsaApi::GetApiUtils().SendChatMessage(pc, FString(PROJECT_NAME), "No completed bosses.");
		return;
	}

	int index = 0;
	for (const auto& pair : *boss_map)
	{
		const std::string boss_name = FNameToUtf8(pair.Key);
		Log::GetLog()->info(
			"{}: [dumpbosses] [{}] {} = {} ({})",
			PROJECT_NAME,
			index,
			boss_name,
			pair.Value,
			DifficultyLabel(pair.Value));

		if (index < 12)
		{
			AsaApi::GetApiUtils().SendChatMessage(
				pc,
				FString(PROJECT_NAME),
				fmt::format("{} = {} ({})", boss_name, pair.Value, DifficultyLabel(pair.Value)).c_str());
		}
		++index;
	}

	if (entry_count > 12)
	{
		AsaApi::GetApiUtils().SendChatMessage(
			pc,
			FString(PROJECT_NAME),
			fmt::format("...and {} more in the server log.", entry_count - 12).c_str());
	}
}

void ChatCmd_DumpMissions(AShooterPlayerController* pc, FString* message, int /*mode*/, int /*platform*/)
{
	if (!pc)
		return;

	AShooterPlayerController* target = nullptr;
	FString target_eos;
	if (!ResolveDumpTarget(pc, message, target, target_eos))
		return;

	const std::vector<FName> available_tags = GetAvailableMissionTags();
	AShooterCharacter* character = target->GetPlayerCharacter();

	std::vector<UClass*> world_classes;
	CollectWorldAvailableMissionClasses(world_classes);

	Log::GetLog()->info(
		"{}: [dumpmissions] EOS={} map={} count={}",
		PROJECT_NAME,
		target_eos.ToString(),
		GetCurrentMapName().ToString(),
		available_tags.size());

	AsaApi::GetApiUtils().SendChatMessage(
		pc,
		FString(PROJECT_NAME),
		fmt::format("Mission dump for {} ({} tags). Full list in server log.", target_eos.ToString(), available_tags.size()).c_str());

	int chat_shown = 0;
	for (size_t i = 0; i < available_tags.size(); ++i)
	{
		const FName& tag = available_tags[i];
		const std::string label = MissionTagLabel(tag);

		UClass* cls = nullptr;
		for (UClass* candidate : world_classes)
		{
			if (MissionClassMatchesTag(candidate, tag))
			{
				cls = candidate;
				break;
			}
		}

		const bool done = HasCompletedMission(cls, character);
		Log::GetLog()->info("{}: [dumpmissions] [{}] {} {}", PROJECT_NAME, i, label, done ? "done" : "open");

		if (chat_shown < 12)
		{
			AsaApi::GetApiUtils().SendChatMessage(
				pc,
				FString(PROJECT_NAME),
				fmt::format("{} ({})", label, done ? "done" : "open").c_str());
			++chat_shown;
		}
	}

	if (available_tags.size() > 12)
	{
		AsaApi::GetApiUtils().SendChatMessage(
			pc,
			FString(PROJECT_NAME),
			fmt::format("...and {} more in the server log.", available_tags.size() - 12).c_str());
	}
	else if (available_tags.empty())
	{
		AsaApi::GetApiUtils().SendChatMessage(pc, FString(PROJECT_NAME), "No missions on this map.");
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

	AsaApi::GetCommands().AddConsoleCommand(kDefeatBossCmd, &DefeatBossCmd);
	AsaApi::GetCommands().AddRconCommand(kDefeatBossCmd, &DefeatBossRcon);
	AsaApi::GetCommands().AddConsoleCommand(kCompleteMissionCmd, &CompleteMissionCmd);
	AsaApi::GetCommands().AddRconCommand(kCompleteMissionCmd, &CompleteMissionRcon);
	AsaApi::GetCommands().AddConsoleCommand(kCompleteGenesisCmd, &CompleteGenesisMissionsCmd);
	AsaApi::GetCommands().AddRconCommand(kCompleteGenesisCmd, &CompleteGenesisMissionsRcon);
	AsaApi::GetCommands().AddConsoleCommand(kListTribeDinosCmd, &ListTribeDinosCmd);
	AsaApi::GetCommands().AddRconCommand(kListTribeDinosCmd, &ListTribeDinosRcon);
	AsaApi::GetCommands().AddConsoleCommand(kTeleportDinoToPlayerCmd, &TeleportDinoToPlayerCmd);
	AsaApi::GetCommands().AddRconCommand(kTeleportDinoToPlayerCmd, &TeleportDinoToPlayerRcon);
	AsaApi::GetCommands().AddConsoleCommand(kTeleportDinoToLocationCmd, &TeleportDinoToLocationCmd);
	AsaApi::GetCommands().AddRconCommand(kTeleportDinoToLocationCmd, &TeleportDinoToLocationRcon);

	if (Config.Debug)
	{
		AsaApi::GetCommands().AddChatCommand(kDumpBossesCmd, &ChatCmd_DumpBosses);
		g_dumpBossesRegistered = true;
		AsaApi::GetCommands().AddChatCommand(kDumpMissionsCmd, &ChatCmd_DumpMissions);
		g_dumpMissionsRegistered = true;
		Log::GetLog()->info("{}: debug commands: /dumpbosses, /dumpmissions", PROJECT_NAME);
	}

	Log::GetLog()->info("{}: loaded", PROJECT_NAME);
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
	AsaApi::GetCommands().RemoveConsoleCommand(kDefeatBossCmd);
	AsaApi::GetCommands().RemoveRconCommand(kDefeatBossCmd);
	AsaApi::GetCommands().RemoveConsoleCommand(kCompleteMissionCmd);
	AsaApi::GetCommands().RemoveRconCommand(kCompleteMissionCmd);
	AsaApi::GetCommands().RemoveConsoleCommand(kCompleteGenesisCmd);
	AsaApi::GetCommands().RemoveRconCommand(kCompleteGenesisCmd);
	AsaApi::GetCommands().RemoveConsoleCommand(kListTribeDinosCmd);
	AsaApi::GetCommands().RemoveRconCommand(kListTribeDinosCmd);
	AsaApi::GetCommands().RemoveConsoleCommand(kTeleportDinoToPlayerCmd);
	AsaApi::GetCommands().RemoveRconCommand(kTeleportDinoToPlayerCmd);
	AsaApi::GetCommands().RemoveConsoleCommand(kTeleportDinoToLocationCmd);
	AsaApi::GetCommands().RemoveRconCommand(kTeleportDinoToLocationCmd);

	g_listedDinos.clear();

	if (g_dumpBossesRegistered)
	{
		AsaApi::GetCommands().RemoveChatCommand(kDumpBossesCmd);
		g_dumpBossesRegistered = false;
	}

	if (g_dumpMissionsRegistered)
	{
		AsaApi::GetCommands().RemoveChatCommand(kDumpMissionsCmd);
		g_dumpMissionsRegistered = false;
	}

	Config.Loaded = false;
	Log::GetLog()->info("{}: unloaded", PROJECT_NAME);
}
