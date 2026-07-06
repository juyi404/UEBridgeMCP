#include "WorldDataMCPServerInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "WorldDataMCPServer.h"
#include "WorldDataMCPCommon.h"
#include "WorldDataMCPToolRegistry.h"

using namespace WorldDataMCP;

namespace WorldDataMCP
{
namespace ServerInternal
{
	// --- Shared CVar state (external linkage so other TUs can read it) ---
	int32 GWorldDataMCPRequireSessionHeader = 0;
	static FAutoConsoleVariableRef CVarWorldDataMCPRequireSessionHeader(
		TEXT("WorldDataMCP.RequireSessionHeader"),
		GWorldDataMCPRequireSessionHeader,
		TEXT("When non-zero, require Mcp-Session-Id on post-initialize MCP requests, matching Unreal's native MCP server."));

	int32 GWorldDataMCPPaginationPageSize = 0;
	static FAutoConsoleVariableRef CVarWorldDataMCPPaginationPageSize(
		TEXT("WorldDataMCP.PaginationPageSize"),
		GWorldDataMCPPaginationPageSize,
		TEXT("Maximum items returned by tools/list and resources/list before emitting nextCursor. 0 disables pagination."));

	TArray<TSharedPtr<FJsonValue>> MakeSupportedProtocolVersionValues()
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const TCHAR* const Version : GSupportedProtocolVersions)
		{
			Values.Add(MakeShared<FJsonValueString>(Version));
		}
		return Values;
	}

	bool IsSupportedProtocolVersion(const FString& Version)
	{
		for (const TCHAR* const Supported : GSupportedProtocolVersions)
		{
			if (Version.Equals(Supported))
			{
				return true;
			}
		}
		return false;
	}

	FString GetNewestProtocolVersion()
	{
		return GSupportedProtocolVersions[0];
	}

	// --- File IO ---
	bool SaferReplaceWriteString(const FString& Content, const FString& TargetPath)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(TargetPath), true);
		const FString TmpPath = TargetPath + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(Content, *TmpPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return false;
		}
		if (!IFileManager::Get().Move(*TargetPath, *TmpPath, true))
		{
			IFileManager::Get().Delete(*TmpPath);
			return false;
		}
		return true;
	}

	// --- Project identity & paths ---
	FString GetProjectFilePath()
	{
		FString ProjectFile = FPaths::GetProjectFilePath();
		if (!ProjectFile.IsEmpty())
		{
			ProjectFile = FPaths::ConvertRelativePathToFull(ProjectFile);
			FPaths::MakePlatformFilename(ProjectFile);
		}
		return ProjectFile;
	}

	FString GetProjectDir()
	{
		FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::MakePlatformFilename(ProjectDir);
		return ProjectDir;
	}

	FString SanitizeNamePart(const FString& InName)
	{
		FString Out;
		Out.Reserve(InName.Len());
		bool bLastWasUnderscore = false;
		for (TCHAR Ch : InName)
		{
			if (FChar::IsAlnum(Ch))
			{
				Out.AppendChar(FChar::ToLower(Ch));
				bLastWasUnderscore = false;
			}
			else if (!bLastWasUnderscore)
			{
				Out.AppendChar(TCHAR('_'));
				bLastWasUnderscore = true;
			}
		}
		while (Out.StartsWith(TEXT("_")))
		{
			Out.RightChopInline(1);
		}
		while (Out.EndsWith(TEXT("_")))
		{
			Out.LeftChopInline(1);
		}
		return Out.IsEmpty() ? TEXT("project") : Out;
	}

	uint32 GetProjectHash()
	{
		FString Identity = GetProjectFilePath();
		if (Identity.IsEmpty())
		{
			Identity = GetProjectDir();
		}
		Identity = Identity.ToLower();
		return FCrc::StrCrc32(*Identity);
	}

	FString GetProjectHashString()
	{
		return FString::Printf(TEXT("%08x"), GetProjectHash());
	}

	int32 GetDefaultPort()
	{
		return 5753 + static_cast<int32>(GetProjectHash() % 20000);
	}

	FString GetConfigPath()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("config.json"));
	}

	FString GetConnectionPath()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("mcp.json"));
	}

	FString GetCursorClientConfigPath()
	{
		FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT(".cursor"), TEXT("mcp.json"));
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::MakePlatformFilename(Path);
		return Path;
	}

	TSharedPtr<FJsonObject> LoadJsonFile(const FString& Path)
	{
		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *Path))
		{
			return MakeShared<FJsonObject>();
		}

		TSharedPtr<FJsonObject> Json;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
		if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
		{
			return MakeShared<FJsonObject>();
		}
		return Json;
	}

	// --- Access token ---
	FString GenerateAccessToken()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits) + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}

	bool TryGetConfigAccessToken(const TSharedPtr<FJsonObject>& Config, FString& OutToken)
	{
		FString SavedProjectId;
		if (!Config.IsValid()
			|| !Config->TryGetStringField(TEXT("projectId"), SavedProjectId)
			|| SavedProjectId != FWorldDataMCPServer::GetProjectId()
			|| !Config->TryGetStringField(TEXT("accessToken"), OutToken))
		{
			return false;
		}
		OutToken.TrimStartAndEndInline();
		return OutToken.Len() >= 32;
	}

	FString GetConfiguredAccessToken()
	{
		FString Token;
		TryGetConfigAccessToken(LoadJsonFile(GetConfigPath()), Token);
		return Token;
	}

	TSharedRef<FJsonObject> MakeAccessTokenHeadersObject()
	{
		TSharedRef<FJsonObject> Headers = MakeShared<FJsonObject>();
		const FString Token = GetConfiguredAccessToken();
		if (!Token.IsEmpty())
		{
			Headers->SetStringField(TEXT("X-WorldData-MCP-Token"), Token);
		}
		return Headers;
	}

	// --- Codex policy snapshot (TOML parsing helpers are file-local) ---
	static FString StripTomlComment(const FString& Line)
	{
		bool bInString = false;
		bool bEscape = false;
		for (int32 Index = 0; Index < Line.Len(); ++Index)
		{
			const TCHAR Ch = Line[Index];
			if (bEscape)
			{
				bEscape = false;
				continue;
			}
			if (bInString)
			{
				if (Ch == TEXT('\\'))
				{
					bEscape = true;
				}
				else if (Ch == TEXT('"'))
				{
					bInString = false;
				}
				continue;
			}
			if (Ch == TEXT('"'))
			{
				bInString = true;
				continue;
			}
			if (Ch == TEXT('#'))
			{
				return Line.Left(Index).TrimStartAndEnd();
			}
		}
		return Line.TrimStartAndEnd();
	}

	static FString ParseTomlValue(FString Value)
	{
		Value.TrimStartAndEndInline();
		if (Value.Len() >= 2 && Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
		{
			Value = Value.Mid(1, Value.Len() - 2);
			Value.ReplaceInline(TEXT("\\\""), TEXT("\""));
			Value.ReplaceInline(TEXT("\\\\"), TEXT("\\"));
		}
		return Value;
	}

	static FString GetHomeDir()
	{
		FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		if (Home.IsEmpty())
		{
			Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
		}
		return Home;
	}

	static FString GetCodexConfigPath()
	{
		const FString Home = GetHomeDir();
		return Home.IsEmpty() ? FString() : FPaths::Combine(Home, TEXT(".codex"), TEXT("config.toml"));
	}

	static bool IsSafeCodexPolicyKey(const FString& Key)
	{
		static const TSet<FString> SafeKeys = {
			TEXT("approval_policy"),
			TEXT("sandbox_mode"),
			TEXT("model"),
			TEXT("model_reasoning_effort"),
			TEXT("profile"),
			TEXT("enabled"),
			TEXT("command"),
			TEXT("args"),
			TEXT("cwd"),
			TEXT("url"),
			TEXT("type")
		};
		return SafeKeys.Contains(Key);
	}

	static TSharedPtr<FJsonObject> MakeCodexPolicySnapshotObject()
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);

		const FString ConfigPath = GetCodexConfigPath();
		Result->SetStringField(TEXT("configPath"), ConfigPath);
		Result->SetBoolField(TEXT("exists"), !ConfigPath.IsEmpty() && FPaths::FileExists(ConfigPath));
		Result->SetStringField(TEXT("source"), TEXT("~/.codex/config.toml"));

		if (ConfigPath.IsEmpty() || !FPaths::FileExists(ConfigPath))
		{
			Result->SetStringField(TEXT("message"), TEXT("Codex config.toml was not found."));
			return Result;
		}

		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *ConfigPath))
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Codex config.toml exists but could not be read."));
			return Result;
		}

		TSharedRef<FJsonObject> RootPolicy = MakeShared<FJsonObject>();
		TMap<FString, TSharedPtr<FJsonObject>> ProfilesByName;
		TMap<FString, TSharedPtr<FJsonObject>> McpServersByName;
		FString ActiveProfile;
		FString Section;

		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines, false);
		for (FString Line : Lines)
		{
			Line = StripTomlComment(Line);
			if (Line.IsEmpty())
			{
				continue;
			}

			if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
			{
				Section = Line.Mid(1, Line.Len() - 2).TrimStartAndEnd();
				continue;
			}

			FString Key;
			FString RawValue;
			if (!Line.Split(TEXT("="), &Key, &RawValue))
			{
				continue;
			}
			Key.TrimStartAndEndInline();
			if (!IsSafeCodexPolicyKey(Key))
			{
				continue;
			}

			const FString Value = ParseTomlValue(RawValue);
			if (Section.IsEmpty())
			{
				RootPolicy->SetStringField(Key, Value);
				if (Key == TEXT("profile"))
				{
					ActiveProfile = Value;
				}
				continue;
			}

			if (Section.StartsWith(TEXT("profiles.")))
			{
				const FString ProfileName = Section.RightChop(9);
				TSharedPtr<FJsonObject>& Profile = ProfilesByName.FindOrAdd(ProfileName);
				if (!Profile.IsValid())
				{
					Profile = MakeShared<FJsonObject>();
					Profile->SetStringField(TEXT("name"), ProfileName);
				}
				Profile->SetStringField(Key, Value);
				continue;
			}

			if (Section.StartsWith(TEXT("mcp_servers.")) && !Section.Contains(TEXT(".env")))
			{
				const FString ServerName = Section.RightChop(12);
				TSharedPtr<FJsonObject>& Server = McpServersByName.FindOrAdd(ServerName);
				if (!Server.IsValid())
				{
					Server = MakeShared<FJsonObject>();
					Server->SetStringField(TEXT("name"), ServerName);
				}
				Server->SetStringField(Key, Value);
			}
		}

		Result->SetObjectField(TEXT("rootPolicy"), RootPolicy);
		Result->SetStringField(TEXT("activeProfile"), ActiveProfile);

		TArray<TSharedPtr<FJsonValue>> Profiles;
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : ProfilesByName)
		{
			Profiles.Add(MakeShared<FJsonValueObject>(Pair.Value.ToSharedRef()));
		}
		Result->SetArrayField(TEXT("profiles"), Profiles);

		TSharedRef<FJsonObject> Effective = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : RootPolicy->Values)
		{
			Effective->SetField(Pair.Key, Pair.Value);
		}
		if (!ActiveProfile.IsEmpty())
		{
			if (const TSharedPtr<FJsonObject>* Profile = ProfilesByName.Find(ActiveProfile); Profile && Profile->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Profile)->Values)
				{
					if (Pair.Key != TEXT("name"))
					{
						Effective->SetField(Pair.Key, Pair.Value);
					}
				}
			}
		}
		Result->SetObjectField(TEXT("effectivePolicy"), Effective);

		TArray<TSharedPtr<FJsonValue>> McpServers;
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : McpServersByName)
		{
			McpServers.Add(MakeShared<FJsonValueObject>(Pair.Value.ToSharedRef()));
		}
		Result->SetArrayField(TEXT("mcpServers"), McpServers);

		TArray<TSharedPtr<FJsonValue>> Notes;
		Notes.Add(MakeShared<FJsonValueString>(TEXT("Only safe, non-secret Codex config keys are returned; env sections and unknown keys are intentionally omitted.")));
		Notes.Add(MakeShared<FJsonValueString>(TEXT("This snapshot reads local Codex configuration. It does not expose hidden system/developer prompts or private runtime policy from Codex.")));
		Result->SetArrayField(TEXT("notes"), Notes);

		return Result;
	}

	FString GetCodexPolicySnapshotJson()
	{
		return JsonObjectToString(MakeCodexPolicySnapshotObject().ToSharedRef());
	}

	// --- Tool registry & definitions ---
	const TArray<FMCPToolModule>& GetMCPToolModules()
	{
		// Forwards to the runtime registry (WorldDataMCPToolRegistry), populated once at
		// module startup via RegisterBuiltinMCPToolModules(). Ordering (e.g. PcgKnowledge
		// before Knowledge/PCG so it claims its prefixed names first) is encoded as the
		// registration Priority rather than this list's position.
		return GetRegisteredMCPToolModules();
	}

	static const TSet<FString>& GetHumanConfirmationToolNames()
	{
		static const TSet<FString> Names = {
			TEXT("execute_python"),
			TEXT("write_file"),
			TEXT("delete_file"),
			TEXT("rename_file"),
			TEXT("reparent_blueprint"),
			TEXT("execute_console_command"),
			TEXT("create_level"),
			TEXT("load_level"),
			TEXT("save_current_level"),
			TEXT("save_all_dirty"),
			TEXT("save_asset"),
			TEXT("undo"),
			TEXT("redo"),
			TEXT("remove_all_foliage_instances"),
			TEXT("delete_blueprint_node"),
			TEXT("disconnect_pcg_nodes"),
			TEXT("remove_pcg_node"),
			TEXT("set_object_property"),
			TEXT("delete_actor"),
			TEXT("import_data_table_json"),
			TEXT("remove_data_table_row"),
			TEXT("set_widget_property")
		};
		return Names;
	}

	bool ToolRequiresHumanConfirmation(const FString& ToolName)
	{
		return GetHumanConfirmationToolNames().Contains(ToolName);
	}

	bool ToolArgumentsIncludeHumanConfirmation(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutError)
	{
		if (!ToolRequiresHumanConfirmation(ToolName))
		{
			return true;
		}

		bool bConfirmed = false;
		if (!Args.IsValid() || !Args->TryGetBoolField(TEXT("confirmDangerousAction"), bConfirmed) || !bConfirmed)
		{
			OutError = FString::Printf(
				TEXT("Tool '%s' requires explicit human confirmation. Ask the user to approve this exact action, then retry with arguments.confirmDangerousAction=true and a confirmationReason."),
				*ToolName);
			return false;
		}

		FString ConfirmationReason;
		Args->TryGetStringField(TEXT("confirmationReason"), ConfirmationReason);
		ConfirmationReason.TrimStartAndEndInline();
		if (ConfirmationReason.Len() < 8)
		{
			OutError = FString::Printf(
				TEXT("Tool '%s' requires a non-empty confirmationReason describing the approved action."),
				*ToolName);
			return false;
		}

		return true;
	}

	static TSharedRef<FJsonObject> MakeSchemaProperty(const FString& Type, const FString& Description)
	{
		TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
		Property->SetStringField(TEXT("type"), Type);
		Property->SetStringField(TEXT("description"), Description);
		return Property;
	}

	static bool RequiredContains(const TArray<TSharedPtr<FJsonValue>>& Required, const FString& FieldName)
	{
		for (const TSharedPtr<FJsonValue>& Value : Required)
		{
			if (Value.IsValid() && Value->AsString() == FieldName)
			{
				return true;
			}
		}
		return false;
	}

	static void AddRequiredField(TSharedPtr<FJsonObject> InputSchema, const FString& FieldName)
	{
		TArray<TSharedPtr<FJsonValue>> Required;
		const TArray<TSharedPtr<FJsonValue>>* ExistingRequired = nullptr;
		if (InputSchema.IsValid() && InputSchema->TryGetArrayField(TEXT("required"), ExistingRequired) && ExistingRequired)
		{
			Required = *ExistingRequired;
		}
		if (!RequiredContains(Required, FieldName))
		{
			Required.Add(MakeShared<FJsonValueString>(FieldName));
		}
		InputSchema->SetArrayField(TEXT("required"), Required);
	}

	static TSharedPtr<FJsonObject> GetOrCreateAnnotations(TSharedPtr<FJsonObject> ToolObject)
	{
		if (!ToolObject.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* AnnotationsPtr = nullptr;
		if (ToolObject->TryGetObjectField(TEXT("annotations"), AnnotationsPtr) && AnnotationsPtr && AnnotationsPtr->IsValid())
		{
			return *AnnotationsPtr;
		}

		TSharedPtr<FJsonObject> Annotations = MakeShared<FJsonObject>();
		ToolObject->SetObjectField(TEXT("annotations"), Annotations);
		return Annotations;
	}

	static void ApplyHumanConfirmationPolicy(TSharedPtr<FJsonObject> ToolObject)
	{
		FString ToolName;
		if (!ToolObject.IsValid() || !ToolObject->TryGetStringField(TEXT("name"), ToolName) || !ToolRequiresHumanConfirmation(ToolName))
		{
			return;
		}

		const TSharedPtr<FJsonObject>* InputSchemaPtr = nullptr;
		TSharedPtr<FJsonObject> InputSchema;
		if (ToolObject->TryGetObjectField(TEXT("inputSchema"), InputSchemaPtr) && InputSchemaPtr && InputSchemaPtr->IsValid())
		{
			InputSchema = *InputSchemaPtr;
		}
		else
		{
			InputSchema = MakeShared<FJsonObject>();
			InputSchema->SetStringField(TEXT("type"), TEXT("object"));
			ToolObject->SetObjectField(TEXT("inputSchema"), InputSchema);
		}

		const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
		TSharedPtr<FJsonObject> Properties;
		if (InputSchema->TryGetObjectField(TEXT("properties"), PropertiesPtr) && PropertiesPtr && PropertiesPtr->IsValid())
		{
			Properties = *PropertiesPtr;
		}
		else
		{
			Properties = MakeShared<FJsonObject>();
			InputSchema->SetObjectField(TEXT("properties"), Properties);
		}

		Properties->SetObjectField(TEXT("confirmDangerousAction"), MakeSchemaProperty(
			TEXT("boolean"),
			TEXT("Must be true after an explicit human approval for this exact high-risk action.")));
		Properties->SetObjectField(TEXT("confirmationReason"), MakeSchemaProperty(
			TEXT("string"),
			TEXT("Short summary of the human-approved action, used as an audit trail.")));
		AddRequiredField(InputSchema, TEXT("confirmDangerousAction"));
		AddRequiredField(InputSchema, TEXT("confirmationReason"));

		TSharedPtr<FJsonObject> Annotations = GetOrCreateAnnotations(ToolObject);
		Annotations->SetBoolField(TEXT("worldDataRequiresHumanConfirmation"), true);
		Annotations->SetStringField(
			TEXT("worldDataConfirmationHint"),
			TEXT("This high-risk tool is blocked until the user explicitly approves the exact action; retry with confirmDangerousAction=true and confirmationReason."));
	}

	static void ApplyRiskLevelPolicy(TSharedPtr<FJsonObject> ToolObject)
	{
		FString ToolName;
		if (!ToolObject.IsValid() || !ToolObject->TryGetStringField(TEXT("name"), ToolName) || ToolName.IsEmpty())
		{
			return;
		}

		TSharedPtr<FJsonObject> Annotations = GetOrCreateAnnotations(ToolObject);
		bool bReadOnlyHint = false;
		const bool bHasReadOnlyHint = Annotations->TryGetBoolField(TEXT("readOnlyHint"), bReadOnlyHint);
		bool bDestructiveHint = false;
		Annotations->TryGetBoolField(TEXT("destructiveHint"), bDestructiveHint);
		bool bOpenWorldHint = false;
		Annotations->TryGetBoolField(TEXT("openWorldHint"), bOpenWorldHint);

		if (ToolRequiresHumanConfirmation(ToolName) || bDestructiveHint)
		{
			Annotations->SetStringField(TEXT("worldDataRiskLevel"), TEXT("high"));
			Annotations->SetStringField(TEXT("worldDataRiskReason"), TEXT("Requires explicit human confirmation or is marked destructive."));
		}
		else if (!bHasReadOnlyHint || !bReadOnlyHint || bOpenWorldHint)
		{
			Annotations->SetStringField(TEXT("worldDataRiskLevel"), TEXT("medium"));
			Annotations->SetStringField(TEXT("worldDataRiskReason"), TEXT("Can change project/editor/world state or access the open editor world."));
		}
		else
		{
			Annotations->SetStringField(TEXT("worldDataRiskLevel"), TEXT("low"));
			Annotations->SetStringField(TEXT("worldDataRiskReason"), TEXT("Read-only tool with no open-world side effects."));
		}
	}

	static void AppendToolDefinitionsFromJson(const FString& ToolsJson, TArray<TSharedPtr<FJsonValue>>& OutTools, TSet<FString>& SeenToolNames)
	{
		TArray<TSharedPtr<FJsonValue>> ParsedTools;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ToolsJson);
		if (!FJsonSerializer::Deserialize(Reader, ParsedTools))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& ToolValue : ParsedTools)
		{
			TSharedPtr<FJsonObject> ToolObject = ToolValue.IsValid() ? ToolValue->AsObject() : nullptr;
			if (!ToolObject.IsValid())
			{
				continue;
			}

			FString ToolName;
			if (!ToolObject->TryGetStringField(TEXT("name"), ToolName) || ToolName.IsEmpty() || SeenToolNames.Contains(ToolName))
			{
				continue;
			}

			SeenToolNames.Add(ToolName);
			ApplyHumanConfirmationPolicy(ToolObject);
			ApplyRiskLevelPolicy(ToolObject);
			OutTools.Add(ToolValue);
		}
	}

	FString BuildCombinedToolDefinitionsJson(const FString& LocalToolsJson)
	{
		TArray<TSharedPtr<FJsonValue>> MergedTools;
		TSet<FString> SeenToolNames;
		AppendToolDefinitionsFromJson(LocalToolsJson, MergedTools, SeenToolNames);
		for (const FMCPToolModule& Module : GetMCPToolModules())
		{
			AppendToolDefinitionsFromJson(Module.GetDefinitions(), MergedTools, SeenToolNames);
		}

		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(MergedTools, Writer);
		return Out;
	}

	// --- Trusted-tool gating (catalog is file-local, built once) ---
	struct FTrustedToolCatalog
	{
		TSet<FString> KnownTools;
		TSet<FString> TrustedOnlyTools;
		bool bValid = false;
	};

	static bool ToolDefinitionRequiresTrustedClient(const TSharedPtr<FJsonObject>& ToolObject)
	{
		const TSharedPtr<FJsonObject>* AnnotationsPtr = nullptr;
		if (!ToolObject.IsValid() || !ToolObject->TryGetObjectField(TEXT("annotations"), AnnotationsPtr) || !AnnotationsPtr || !AnnotationsPtr->IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonObject>& Annotations = *AnnotationsPtr;
		bool bReadOnlyHint = false;
		const bool bHasReadOnlyHint = Annotations->TryGetBoolField(TEXT("readOnlyHint"), bReadOnlyHint);

		bool bDestructiveHint = false;
		Annotations->TryGetBoolField(TEXT("destructiveHint"), bDestructiveHint);

		bool bOpenWorldHint = false;
		Annotations->TryGetBoolField(TEXT("openWorldHint"), bOpenWorldHint);

		return !bHasReadOnlyHint || !bReadOnlyHint || bDestructiveHint || bOpenWorldHint;
	}

	static FTrustedToolCatalog BuildTrustedToolCatalog()
	{
		FTrustedToolCatalog Catalog;
		TArray<TSharedPtr<FJsonValue>> Tools;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FWorldDataMCPServer::GetToolDefinitionsJson());
		if (!FJsonSerializer::Deserialize(Reader, Tools))
		{
			return Catalog;
		}

		Catalog.bValid = true;
		for (const TSharedPtr<FJsonValue>& ToolValue : Tools)
		{
			const TSharedPtr<FJsonObject> ToolObject = ToolValue.IsValid() ? ToolValue->AsObject() : nullptr;
			FString ToolName;
			if (!ToolObject.IsValid() || !ToolObject->TryGetStringField(TEXT("name"), ToolName) || ToolName.IsEmpty())
			{
				Catalog.bValid = false;
				continue;
			}

			Catalog.KnownTools.Add(ToolName);
			if (ToolDefinitionRequiresTrustedClient(ToolObject))
			{
				Catalog.TrustedOnlyTools.Add(ToolName);
			}
		}
		return Catalog;
	}

	bool ToolRequiresTrustedClient(const FString& ToolName)
	{
		const FTrustedToolCatalog Catalog = BuildTrustedToolCatalog();
		return !Catalog.bValid || !Catalog.KnownTools.Contains(ToolName) || Catalog.TrustedOnlyTools.Contains(ToolName);
	}
}
}
