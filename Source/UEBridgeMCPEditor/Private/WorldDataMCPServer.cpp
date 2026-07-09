#include "WorldDataMCPServer.h"

#include "Algo/AllOf.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/Event.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/ThreadSafeBool.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Misc/Crc.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

#include "WorldDataMCPCommon.h"
#include "WorldDataMCPTools.h"
#include "UEBridgeMCPExtractedTools.h"

using namespace WorldDataMCP;

DEFINE_LOG_CATEGORY_STATIC(LogWorldDataMCP, Log, All);

TSharedPtr<IHttpRouter> FWorldDataMCPServer::HttpRouter = nullptr;
TArray<FHttpRouteHandle> FWorldDataMCPServer::RouteHandles;
int32 FWorldDataMCPServer::BoundPort = 0;
bool FWorldDataMCPServer::bRunning = false;
TArray<FString> FWorldDataMCPServer::SessionIds;
FString FWorldDataMCPServer::NegotiatedProtocolVersion(TEXT("2025-06-18"));
FString FWorldDataMCPServer::AccessToken;
FString FWorldDataMCPServer::LastError;

namespace
{
	static FCriticalSection GMcpStateMutex;

	// Newest first. The server advertises the newest version it supports but will
	// echo back a client's requested version when that version is also supported,
	// per the MCP initialize negotiation rules.
	static const TCHAR* const GSupportedProtocolVersions[] = {
		TEXT("2025-06-18"),
		TEXT("2025-03-26"),
		TEXT("2024-11-05")
	};

	static constexpr int32 GMaxMcpRequestBodyBytes = 1024 * 1024;

	// Multiple CLI clients (Codex, Cursor, Claude Code) may hold sessions against the
	// same editor concurrently. Keep a bounded set so a new initialize does not evict
	// every other client's session.
	static constexpr int32 GMaxConcurrentMcpSessions = 16;

	static FString GenerateAccessToken()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits)
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}

	static bool IsStrongAccessToken(const FString& Token)
	{
		return Token.Len() >= 32;
	}

	static bool ConstantTimeEquals(const FString& Expected, const FString& Actual)
	{
		const FTCHARToUTF8 ExpectedUtf8(*Expected);
		const FTCHARToUTF8 ActualUtf8(*Actual);
		const int32 ExpectedLen = ExpectedUtf8.Length();
		const int32 ActualLen = ActualUtf8.Length();
		const int32 MaxLen = FMath::Max(ExpectedLen, ActualLen);

		int32 Diff = ExpectedLen ^ ActualLen;
		for (int32 Index = 0; Index < MaxLen; ++Index)
		{
			const uint8 A = Index < ExpectedLen ? static_cast<uint8>(ExpectedUtf8.Get()[Index]) : 0;
			const uint8 B = Index < ActualLen ? static_cast<uint8>(ActualUtf8.Get()[Index]) : 0;
			Diff |= A ^ B;
		}
		return Diff == 0;
	}

	static TArray<TSharedPtr<FJsonValue>> MakeSupportedProtocolVersionsArray()
	{
		TArray<TSharedPtr<FJsonValue>> Versions;
		for (const TCHAR* const Version : GSupportedProtocolVersions)
		{
			Versions.Add(MakeShared<FJsonValueString>(Version));
		}
		return Versions;
	}

	static TSharedRef<FJsonObject> MakeAuthHeadersObject(const FString& Token)
	{
		TSharedRef<FJsonObject> Headers = MakeShared<FJsonObject>();
		Headers->SetStringField(FWorldDataMCPServer::GetAccessTokenHeaderName(), Token);
		return Headers;
	}

	static void RemoveManagedMcpServerEntries(const TSharedPtr<FJsonObject>& Servers)
	{
		if (!Servers.IsValid())
		{
			return;
		}

		TArray<FString> KeysToRemove;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Servers->Values)
		{
			TSharedPtr<FJsonObject> Entry = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
			FString GeneratedBy;
			const bool bGeneratedByThisPlugin = Entry.IsValid()
				&& Entry->TryGetStringField(TEXT("generatedBy"), GeneratedBy)
				&& GeneratedBy == TEXT("UEBridgeMCP");
			if (bGeneratedByThisPlugin || Pair.Key.StartsWith(TEXT("world_data_")))
			{
				KeysToRemove.Add(Pair.Key);
			}
		}

		for (const FString& Key : KeysToRemove)
		{
			Servers->RemoveField(Key);
		}
	}
}
FDateTime FWorldDataMCPServer::StartedAtUtc;
FDateTime FWorldDataMCPServer::LastRefreshAtUtc;

namespace
{
	static bool SaferReplaceWriteString(const FString& Content, const FString& TargetPath)
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

	static FString GetProjectFilePath()
	{
		FString ProjectFile = FPaths::GetProjectFilePath();
		if (!ProjectFile.IsEmpty())
		{
			ProjectFile = FPaths::ConvertRelativePathToFull(ProjectFile);
			FPaths::MakePlatformFilename(ProjectFile);
		}
		return ProjectFile;
	}

	static FString GetProjectDir()
	{
		FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::MakePlatformFilename(ProjectDir);
		return ProjectDir;
	}

	static FString SanitizeNamePart(const FString& InName)
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

	static uint32 GetProjectHash()
	{
		FString Identity = GetProjectFilePath();
		if (Identity.IsEmpty())
		{
			Identity = GetProjectDir();
		}
		Identity = Identity.ToLower();
		return FCrc::StrCrc32(*Identity);
	}

	static FString GetProjectHashString()
	{
		return FString::Printf(TEXT("%08x"), GetProjectHash());
	}

	static int32 GetDefaultPort()
	{
		return 5753 + static_cast<int32>(GetProjectHash() % 20000);
	}

	static FString GetConfigPath()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("config.json"));
	}

	static FString GetConnectionPath()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("mcp.json"));
	}

	static FString GetCursorClientConfigPath()
	{
		FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT(".cursor"), TEXT("mcp.json"));
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::MakePlatformFilename(Path);
		return Path;
	}

	static TSharedPtr<FJsonObject> LoadJsonFile(const FString& Path)
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

	static TSharedPtr<FJsonObject> ParseObject(const FString& JsonText)
	{
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
		{
			return nullptr;
		}
		return Parsed;
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
			OutTools.Add(ToolValue);
		}
	}

	static FString BuildCombinedToolDefinitionsJson(const FString& LocalToolsJson)
	{
		TArray<TSharedPtr<FJsonValue>> MergedTools;
		TSet<FString> SeenToolNames;
		AppendToolDefinitionsFromJson(LocalToolsJson, MergedTools, SeenToolNames);
		AppendToolDefinitionsFromJson(WorldDataMCP::ExtractedTools::GetToolDefinitionsJson(), MergedTools, SeenToolNames);

		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(MergedTools, Writer);
		return Out;
	}

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

	static FString GetCodexPolicySnapshotJson()
	{
		return JsonObjectToString(MakeCodexPolicySnapshotObject().ToSharedRef());
	}

	static FString GetFirstHeaderValue(const FHttpServerRequest& Request, const FString& HeaderName)
	{
		for (const TPair<FString, TArray<FString>>& Pair : Request.Headers)
		{
			if (Pair.Key.Equals(HeaderName, ESearchCase::IgnoreCase) && Pair.Value.Num() > 0)
			{
				return Pair.Value[0];
			}
		}
		return FString();
	}

	// Reduce a Host header or an Origin (scheme://host:port) to a bare host and decide
	// whether it is loopback. Used to defend against DNS-rebinding: a malicious web page
	// could otherwise point a hostname at 127.0.0.1 and drive editor mutations via the
	// user's browser.
	static bool IsLoopbackAuthority(FString Authority)
	{
		Authority.TrimStartAndEndInline();
		if (Authority.IsEmpty())
		{
			return false;
		}

		int32 SchemeIndex = Authority.Find(TEXT("://"));
		if (SchemeIndex != INDEX_NONE)
		{
			Authority.RightChopInline(SchemeIndex + 3);
		}

		int32 SlashIndex = INDEX_NONE;
		if (Authority.FindChar(TCHAR('/'), SlashIndex))
		{
			Authority.LeftInline(SlashIndex);
		}

		if (Authority.StartsWith(TEXT("[")))
		{
			int32 CloseIndex = INDEX_NONE;
			if (Authority.FindChar(TCHAR(']'), CloseIndex))
			{
				Authority = Authority.Mid(1, CloseIndex - 1);
			}
		}
		else
		{
			int32 ColonIndex = INDEX_NONE;
			if (Authority.FindChar(TCHAR(':'), ColonIndex))
			{
				Authority.LeftInline(ColonIndex);
			}
		}

		Authority.ToLowerInline();
		return Authority == TEXT("127.0.0.1") || Authority == TEXT("localhost") || Authority == TEXT("::1");
	}

	// Native MCP clients connect to 127.0.0.1 with a loopback Host or none at all.
	static bool IsLoopbackHostHeader(const FHttpServerRequest& Request)
	{
		const FString Host = GetFirstHeaderValue(Request, TEXT("Host"));
		return Host.IsEmpty() ? true : IsLoopbackAuthority(Host);
	}

	// Per the MCP local-server security guidance, validate Origin to block browser-based
	// DNS-rebinding. Native clients omit Origin (allowed); browsers always send it.
	static bool IsAllowedOriginHeader(const FHttpServerRequest& Request)
	{
		const FString Origin = GetFirstHeaderValue(Request, TEXT("Origin"));
		if (Origin.IsEmpty() || Origin.Equals(TEXT("null"), ESearchCase::IgnoreCase))
		{
			return true;
		}
		return IsLoopbackAuthority(Origin);
	}
}

void FWorldDataMCPServer::Start(int32 Port)
{
	if (bRunning)
	{
		LastError.Empty();
		return;
	}

	LastError.Empty();
	RouteHandles.Reset();
	BoundPort = 0;
	HttpRouter.Reset();
	IFileManager::Get().Delete(*GetConnectionPath(), false, true, true);
	{
		FScopeLock Lock(&GMcpStateMutex);
		SessionIds.Reset();
		NegotiatedProtocolVersion = GSupportedProtocolVersions[0];
		// Keep the token empty here; EnsureAccessToken() reuses the persisted token from
		// Saved/UEBridgeMCP/config.json when available so already-written CLI configs
		// (Codex config.toml, .mcp.json, .cursor/mcp.json) survive editor restarts.
		AccessToken.Empty();
	}
	EnsureAccessToken();

	FHttpServerModule& HttpModule = FHttpServerModule::Get();
	for (int32 Offset = 0; Offset < 10; ++Offset)
	{
		const int32 TryPort = Port + Offset;
		HttpRouter = HttpModule.GetHttpRouter(TryPort, true);
		if (HttpRouter.IsValid())
		{
			BoundPort = TryPort;
			break;
		}
	}

	if (!HttpRouter.IsValid())
	{
		LastError = FString::Printf(TEXT("Failed to bind MCP HTTP server on ports %d-%d."), Port, Port + 9);
		UE_LOG(LogWorldDataMCP, Error, TEXT("%s"), *LastError);
		return;
	}

	RouteHandles.Add(HttpRouter->BindRoute(FHttpPath(TEXT("/mcp")), EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateStatic(&FWorldDataMCPServer::HandleMCPPost)));
	RouteHandles.Add(HttpRouter->BindRoute(FHttpPath(TEXT("/mcp")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateStatic(&FWorldDataMCPServer::HandleMCPGet)));
	RouteHandles.Add(HttpRouter->BindRoute(FHttpPath(TEXT("/mcp")), EHttpServerRequestVerbs::VERB_OPTIONS,
		FHttpRequestHandler::CreateStatic(&FWorldDataMCPServer::HandleMCPOptions)));

	const bool bAllRoutesBound = RouteHandles.Num() == 3 && Algo::AllOf(RouteHandles, [](const FHttpRouteHandle& RouteHandle)
	{
		return RouteHandle.IsValid();
	});
	if (!bAllRoutesBound)
	{
		LastError = FString::Printf(TEXT("Failed to bind one or more /mcp routes on port %d."), BoundPort);
		UE_LOG(LogWorldDataMCP, Error, TEXT("%s"), *LastError);

		for (const FHttpRouteHandle& RouteHandle : RouteHandles)
		{
			if (HttpRouter.IsValid() && RouteHandle.IsValid())
			{
				HttpRouter->UnbindRoute(RouteHandle);
			}
		}
		RouteHandles.Reset();
		HttpRouter.Reset();
		BoundPort = 0;
		return;
	}

	HttpModule.StartAllListeners();

	bRunning = true;
	StartedAtUtc = FDateTime::UtcNow();
	RefreshConnectionFiles();

	UE_LOG(LogWorldDataMCP, Log, TEXT("WorldData MCP server '%s' listening at %s"), *GetServerName(), *GetMcpUrl());
}

void FWorldDataMCPServer::Stop()
{
	if (!bRunning)
	{
		{
			FScopeLock Lock(&GMcpStateMutex);
			SessionIds.Reset();
			AccessToken.Empty();
		}
		IFileManager::Get().Delete(*GetConnectionPath(), false, true, true);
		return;
	}

	if (HttpRouter.IsValid())
	{
		for (const FHttpRouteHandle& RouteHandle : RouteHandles)
		{
			if (RouteHandle.IsValid())
			{
				HttpRouter->UnbindRoute(RouteHandle);
			}
		}
	}

	RouteHandles.Reset();
	HttpRouter.Reset();
	BoundPort = 0;
	bRunning = false;
	StartedAtUtc = FDateTime();
	{
		FScopeLock Lock(&GMcpStateMutex);
		SessionIds.Reset();
		NegotiatedProtocolVersion = GSupportedProtocolVersions[0];
		AccessToken.Empty();
	}
	IFileManager::Get().Delete(*GetConnectionPath(), false, true, true);
}

bool FWorldDataMCPServer::IsRunning()
{
	return bRunning;
}

int32 FWorldDataMCPServer::GetPort()
{
	return BoundPort;
}

int32 FWorldDataMCPServer::LoadConfiguredPort()
{
	const TSharedPtr<FJsonObject> Json = LoadJsonFile(GetConfigPath());
	double SavedPort = 0.0;
	FString SavedProjectId;
	if (Json->TryGetNumberField(TEXT("mcpPort"), SavedPort)
		&& SavedPort > 0.0
		&& Json->TryGetStringField(TEXT("projectId"), SavedProjectId)
		&& SavedProjectId == GetProjectId())
	{
		return static_cast<int32>(SavedPort);
	}

	return GetDefaultPort();
}

FString FWorldDataMCPServer::GetServerName()
{
	return FString::Printf(TEXT("world_data_%s_%s"), *SanitizeNamePart(GetProjectName()), *GetProjectHashString());
}

FString FWorldDataMCPServer::GetProjectId()
{
	return FString::Printf(TEXT("%s_%s"), *SanitizeNamePart(GetProjectName()), *GetProjectHashString());
}

FString FWorldDataMCPServer::GetMcpUrl()
{
	return FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), BoundPort);
}

FString FWorldDataMCPServer::GetAccessTokenHeaderName()
{
	return TEXT("X-WorldData-MCP-Token");
}

void FWorldDataMCPServer::EnsureAccessToken()
{
	FScopeLock Lock(&GMcpStateMutex);
	if (!AccessToken.IsEmpty())
	{
		return;
	}

	const TSharedPtr<FJsonObject> Config = LoadJsonFile(GetConfigPath());
	FString SavedProjectId;
	FString SavedToken;
	if (Config->TryGetStringField(TEXT("projectId"), SavedProjectId)
		&& SavedProjectId == GetProjectId()
		&& Config->TryGetStringField(TEXT("accessToken"), SavedToken)
		&& IsStrongAccessToken(SavedToken))
	{
		AccessToken = SavedToken;
	}
	else
	{
		AccessToken = GenerateAccessToken();
	}
}

FString FWorldDataMCPServer::GetAccessToken()
{
	EnsureAccessToken();
	FScopeLock Lock(&GMcpStateMutex);
	return AccessToken;
}

FString FWorldDataMCPServer::GetProjectInfoJson()
{
	FString ProtocolSnapshot;
	{
		FScopeLock Lock(&GMcpStateMutex);
		ProtocolSnapshot = NegotiatedProtocolVersion;
	}

	TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
	Info->SetBoolField(TEXT("success"), true);
	Info->SetStringField(TEXT("projectName"), GetProjectName());
	Info->SetStringField(TEXT("projectId"), GetProjectId());
	Info->SetStringField(TEXT("serverName"), GetServerName());
	Info->SetStringField(TEXT("url"), GetMcpUrl());
	Info->SetNumberField(TEXT("port"), BoundPort);
	Info->SetNumberField(TEXT("pid"), static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
	Info->SetStringField(TEXT("protocolVersion"), ProtocolSnapshot);
	Info->SetArrayField(TEXT("supportedProtocolVersions"), MakeSupportedProtocolVersionsArray());
	Info->SetStringField(TEXT("accessTokenHeader"), GetAccessTokenHeaderName());
	Info->SetBoolField(TEXT("requiresAccessToken"), true);
	Info->SetBoolField(TEXT("requiresSessionHeader"), true);
	Info->SetStringField(TEXT("uproject"), GetProjectFilePath());
	Info->SetStringField(TEXT("projectDir"), GetProjectDir());
	Info->SetBoolField(TEXT("running"), bRunning);
	Info->SetStringField(TEXT("startedAtUtc"), StartedAtUtc.GetTicks() > 0 ? StartedAtUtc.ToIso8601() : TEXT(""));
	Info->SetStringField(TEXT("lastRefreshAtUtc"), LastRefreshAtUtc.GetTicks() > 0 ? LastRefreshAtUtc.ToIso8601() : TEXT(""));
	Info->SetStringField(TEXT("lastError"), LastError);
	Info->SetStringField(TEXT("clientConfigFile"), GetClientConfigFilePath());
	Info->SetStringField(TEXT("cursorClientConfigFile"), GetCursorClientConfigPath());
	Info->SetStringField(TEXT("savedConfigFile"), GetSavedConfigFilePath());
	Info->SetStringField(TEXT("connectionFile"), GetConnectionFilePath());
	return JsonObjectToString(Info);
}

FString FWorldDataMCPServer::GetStatusJson()
{
	FString ProtocolSnapshot;
	{
		FScopeLock Lock(&GMcpStateMutex);
		ProtocolSnapshot = NegotiatedProtocolVersion;
	}

	TSharedRef<FJsonObject> Status = MakeShared<FJsonObject>();
	Status->SetBoolField(TEXT("running"), bRunning);
	Status->SetStringField(TEXT("serverName"), GetServerName());
	Status->SetStringField(TEXT("projectId"), GetProjectId());
	Status->SetNumberField(TEXT("port"), BoundPort);
	Status->SetStringField(TEXT("url"), bRunning ? GetMcpUrl() : TEXT(""));
	Status->SetNumberField(TEXT("routeCount"), RouteHandles.Num());
	Status->SetStringField(TEXT("protocolVersion"), ProtocolSnapshot);
	Status->SetArrayField(TEXT("supportedProtocolVersions"), MakeSupportedProtocolVersionsArray());
	Status->SetStringField(TEXT("accessTokenHeader"), GetAccessTokenHeaderName());
	Status->SetBoolField(TEXT("requiresAccessToken"), true);
	Status->SetBoolField(TEXT("requiresSessionHeader"), true);
	Status->SetStringField(TEXT("startedAtUtc"), StartedAtUtc.GetTicks() > 0 ? StartedAtUtc.ToIso8601() : TEXT(""));
	Status->SetStringField(TEXT("lastRefreshAtUtc"), LastRefreshAtUtc.GetTicks() > 0 ? LastRefreshAtUtc.ToIso8601() : TEXT(""));
	Status->SetStringField(TEXT("lastError"), LastError);
	Status->SetStringField(TEXT("clientConfigFile"), GetClientConfigFilePath());
	Status->SetStringField(TEXT("cursorClientConfigFile"), GetCursorClientConfigPath());
	Status->SetStringField(TEXT("codexClientConfigFile"), GetCodexClientConfigFilePath());
	Status->SetStringField(TEXT("claudeSettingsFile"), GetClaudeSettingsFilePath());
	Status->SetStringField(TEXT("savedConfigFile"), GetSavedConfigFilePath());
	Status->SetStringField(TEXT("connectionFile"), GetConnectionFilePath());
	return JsonObjectToString(Status);
}

FString FWorldDataMCPServer::GetLastError()
{
	return LastError;
}

FDateTime FWorldDataMCPServer::GetStartedAtUtc()
{
	return StartedAtUtc;
}

FDateTime FWorldDataMCPServer::GetLastRefreshAtUtc()
{
	return LastRefreshAtUtc;
}

FString FWorldDataMCPServer::GetClientConfigFilePath()
{
	FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT(".mcp.json"));
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::MakePlatformFilename(Path);
	return Path;
}

FString FWorldDataMCPServer::GetCodexClientConfigFilePath()
{
	FString Path = GetCodexConfigPath();
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::MakePlatformFilename(Path);
	}
	return Path;
}

FString FWorldDataMCPServer::GetClaudeSettingsFilePath()
{
	FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT(".claude"), TEXT("settings.local.json"));
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::MakePlatformFilename(Path);
	return Path;
}

FString FWorldDataMCPServer::GetSavedConfigFilePath()
{
	FString Path = FPaths::ConvertRelativePathToFull(GetConfigPath());
	FPaths::MakePlatformFilename(Path);
	return Path;
}

FString FWorldDataMCPServer::GetConnectionFilePath()
{
	FString Path = FPaths::ConvertRelativePathToFull(GetConnectionPath());
	FPaths::MakePlatformFilename(Path);
	return Path;
}

void FWorldDataMCPServer::RefreshConnectionFiles()
{
	if (BoundPort <= 0)
	{
		if (LastError.IsEmpty())
		{
			LastError = TEXT("Cannot refresh MCP files because the server is not bound to a port.");
		}
		return;
	}

	SaveConfiguredPort(BoundPort);
	WriteClientConfig();
	WriteCodexClientConfig();
	WriteClaudeProjectSettings();
	WriteProjectConnectionFile();
	LastRefreshAtUtc = FDateTime::UtcNow();
	LastError.Empty();
}

FString FWorldDataMCPServer::GetToolDefinitionsJson()
{
	static const FString LocalToolsJson = FString(TEXT(R"JSON([
{"name":"get_current_project_info","description":"Return the UE project identity and MCP endpoint for this editor session.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get Project Info","readOnlyHint":true,"openWorldHint":false}},
{"name":"list_level_actors","description":"List actors in the currently loaded editor world with transforms, folder, mobility, and selection state.","inputSchema":{"type":"object","properties":{"classFilter":{"type":"string","description":"Optional case-insensitive class-name substring."},"nameContains":{"type":"string","description":"Optional case-insensitive actor name or label substring."},"selectedOnly":{"type":"boolean","description":"When true, only return currently selected actors."},"maxResults":{"type":"number","description":"Maximum returned actors. Default 200, capped at 1000."}}},"annotations":{"title":"List Level Actors","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_selected_actors","description":"Return the actors currently selected in the editor viewport/outliner.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Get Selected Actors","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_actor_details","description":"Read one actor in depth: transform, tags, and its components with classes and relative transforms.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string","description":"Alias for name."}},"required":["name"]},"annotations":{"title":"Get Actor Details","readOnlyHint":true,"openWorldHint":false}},
{"name":"find_assets","description":"Search assets under a content path without loading them.","inputSchema":{"type":"object","properties":{"searchTerm":{"type":"string","description":"Optional case-insensitive asset name or path substring."},"classFilter":{"type":"string","description":"Optional class-name substring such as StaticMesh, Blueprint, World, Material."},"path":{"type":"string","description":"Content root to search. Default /Game."},"maxResults":{"type":"number","description":"Maximum returned assets. Default 50, capped at 500."}}},"annotations":{"title":"Find Assets","readOnlyHint":true,"openWorldHint":false}},
{"name":"read_asset","description":"Read basic Asset Registry metadata for one asset.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string","description":"Asset object path or package path, for example /Game/Foo/Bar.Bar or /Game/Foo/Bar."}},"required":["assetPath"]},"annotations":{"title":"Read Asset","readOnlyHint":true,"openWorldHint":false}},
{"name":"get_content_summary","description":"Summarize the Asset Registry under a content path: total asset count and a histogram of asset counts by class.","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"Content root to summarize. Default /Game."},"maxClasses":{"type":"number","description":"Maximum class buckets returned. Default 30, capped at 200."}}},"annotations":{"title":"Get Content Summary","readOnlyHint":true,"openWorldHint":false}},
{"name":"select_actor","description":"Select an actor in the editor by name or label.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string","description":"Alias for name."}}},"annotations":{"title":"Select Actor","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"spawn_actor","description":"Spawn an actor into the current editor world. Use staticMeshPath to create a StaticMeshActor.","inputSchema":{"type":"object","properties":{"class":{"type":"string","description":"Actor class name/path. Default Actor. Blueprint asset paths are supported."},"staticMeshPath":{"type":"string","description":"Optional StaticMesh asset path. When supplied, spawns a StaticMeshActor."},"label":{"type":"string","description":"Optional editor label."},"location":{"type":"object","description":"Object {x,y,z} or array [x,y,z]."},"rotation":{"type":"object","description":"Object {pitch,yaw,roll} or array [pitch,yaw,roll]."},"scale":{"type":"object","description":"Object {x,y,z} or array [x,y,z]."}}},"annotations":{"title":"Spawn Actor","readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
)JSON")) + TEXT(R"JSON(
{"name":"transform_actor","description":"Set an actor world transform by name or label.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string","description":"Alias for name."},"location":{"type":"object","description":"Object {x,y,z} or array [x,y,z]."},"rotation":{"type":"object","description":"Object {pitch,yaw,roll} or array [pitch,yaw,roll]."},"scale":{"type":"object","description":"Object {x,y,z} or array [x,y,z]."}},"required":["name"]},"annotations":{"title":"Transform Actor","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"delete_actor","description":"Delete an actor from the current editor world by name or label.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string","description":"Alias for name."}},"required":["name"]},"annotations":{"title":"Delete Actor","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"attach_actor","description":"Attach one actor to another in the editor world.","inputSchema":{"type":"object","properties":{"child":{"type":"string","description":"Child actor name or label."},"childName":{"type":"string"},"parent":{"type":"string","description":"Parent actor name or label."},"parentName":{"type":"string"},"socket":{"type":"string"},"keepWorldTransform":{"type":"boolean","description":"Default true."}},"required":["child","parent"]},"annotations":{"title":"Attach Actor","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"set_actor_property","description":"Set an editable actor or component property using reflection. Supports primitive values, enum names, FVector, FRotator, FLinearColor, and UObject asset references.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Actor name or editor label."},"label":{"type":"string","description":"Alias for name."},"component":{"type":"string","description":"Optional component name or class name."},"property":{"type":"string","description":"Property name."},"value":{"description":"JSON value compatible with the property type."}},"required":["name","property","value"]},"annotations":{"title":"Set Actor Property","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"save_current_level","description":"Save the current level package to disk.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Save Current Level","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"create_asset","description":"Create a UObject asset under /Game with the supplied class. Useful for simple data assets and reflected asset classes such as PCGGraph.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string","description":"Long package path under /Game, for example /Game/MCP/NewAsset."},"path":{"type":"string","description":"Alias for assetPath."},"class":{"type":"string","description":"UObject class name or path. Default DataAsset."},"save":{"type":"boolean","description":"When true, save the created asset package to disk."}},"required":["assetPath"]},"annotations":{"title":"Create Asset","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"create_blueprint_asset","description":"Create a Blueprint asset under /Game using a reflected parent class. Graph node construction is not exposed yet.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string","description":"Long package path under /Game."},"path":{"type":"string","description":"Alias for assetPath."},"parentClass":{"type":"string","description":"Parent class name/path. Default Actor."},"save":{"type":"boolean","description":"When true, save the created asset package to disk."}},"required":["assetPath"]},"annotations":{"title":"Create Blueprint Asset","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"modify_material_instance","description":"Modify scalar and vector parameters on a MaterialInstanceConstant asset.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string","description":"MaterialInstanceConstant object path or package path."},"path":{"type":"string","description":"Alias for assetPath."},"scalarParameters":{"type":"object","additionalProperties":{"type":"number"}},"vectorParameters":{"type":"object","description":"Map of parameter name to {r,g,b,a} object or [r,g,b,a] array."},"save":{"type":"boolean","description":"When true, save the modified asset package to disk."}},"required":["assetPath"]},"annotations":{"title":"Modify Material Instance","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"create_pcg_graph_from_recipe","description":"Create a PCGGraph asset under /Game from recipe metadata. If sourceGraph/source_graph is supplied, duplicates that existing PCGGraph including nodes and edges; otherwise creates an empty PCGGraph container and reports recipeApplied=false.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string","description":"PCGGraph package path under /Game."},"path":{"type":"string","description":"Alias for assetPath."},"recipe_id":{"type":"string"},"id":{"type":"string"},"sourceGraph":{"type":"string","description":"Optional source PCGGraph asset path to duplicate."},"source_graph":{"type":"string","description":"Alias for sourceGraph."},"recipe":{"type":"object","description":"Optional recipe object containing recipe_id/id and source_graph."},"save":{"type":"boolean","description":"When true, save the created graph asset package to disk."}},"required":["assetPath"]},"annotations":{"title":"Create PCG Graph From Recipe","readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"get_codex_policy_snapshot","description":"Read a redacted snapshot of explicit local Codex config policy such as approval_policy, sandbox_mode, active profile, model, and MCP server blocks. Does not expose hidden prompts or env secrets.","inputSchema":{"type":"object","properties":{}},"annotations":{"title":"Codex Policy Snapshot","readOnlyHint":true,"openWorldHint":false}}
])JSON");
	return BuildCombinedToolDefinitionsJson(LocalToolsJson);
}

FString FWorldDataMCPServer::GetResourceListJson()
{
	TArray<TSharedPtr<FJsonValue>> Resources;
	auto AddResource = [&Resources](const FString& Uri, const FString& Name, const FString& Description)
	{
		TSharedRef<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), Uri);
		Resource->SetStringField(TEXT("name"), Name);
		Resource->SetStringField(TEXT("description"), Description);
		Resource->SetStringField(TEXT("mimeType"), TEXT("application/json"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	};

	AddResource(TEXT("worlddata://context/bootstrap"), TEXT("Bootstrap Context"), TEXT("Recommended first-read order and compact editor state."));
	AddResource(TEXT("worlddata://project/info"), TEXT("Project Info"), TEXT("Project identity, paths, MCP endpoint, and process details."));
	AddResource(TEXT("worlddata://codex/policy-snapshot"), TEXT("Codex Policy Snapshot"), TEXT("Redacted local Codex config policy and MCP server configuration."));
	AddResource(TEXT("worlddata://level/actors"), TEXT("Level Actors"), TEXT("Current editor-world actors, labels, classes, and transforms."));
	AddResource(TEXT("worlddata://editor/selection"), TEXT("Editor Selection"), TEXT("Actors currently selected in the editor."));
	AddResource(TEXT("worlddata://content/assets"), TEXT("Content Assets"), TEXT("Compact asset registry survey under /Game."));
	AddResource(TEXT("worlddata://content/summary"), TEXT("Content Summary"), TEXT("Asset counts by class under /Game."));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("recommendedFirstRead"), TEXT("worlddata://context/bootstrap"));
	Result->SetArrayField(TEXT("resources"), Resources);
	return JsonObjectToString(Result);
}

FString FWorldDataMCPServer::ReadResource(const FString& Uri)
{
	if (Uri == TEXT("worlddata://context/bootstrap"))
	{
		return WorldDataMCP::Tools::GetBootstrapContextJson();
	}
	if (Uri == TEXT("worlddata://project/info"))
	{
		return GetProjectInfoJson();
	}
	if (Uri == TEXT("worlddata://codex/policy-snapshot"))
	{
		return GetCodexPolicySnapshotJson();
	}
	if (Uri == TEXT("worlddata://level/actors"))
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetNumberField(TEXT("maxResults"), 300);
		return WorldDataMCP::Tools::ListLevelActors(Args);
	}
	if (Uri == TEXT("worlddata://editor/selection"))
	{
		return WorldDataMCP::Tools::GetSelectedActors(MakeShared<FJsonObject>());
	}
	if (Uri == TEXT("worlddata://content/assets"))
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("path"), TEXT("/Game"));
		Args->SetNumberField(TEXT("maxResults"), 300);
		return WorldDataMCP::Tools::FindAssets(Args);
	}
	if (Uri == TEXT("worlddata://content/summary"))
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("path"), TEXT("/Game"));
		return WorldDataMCP::Tools::GetContentSummary(Args);
	}

	return ErrorJson(FString::Printf(TEXT("Unknown resource: %s"), *Uri));
}

TUniquePtr<FHttpServerResponse> FWorldDataMCPServer::MakeJsonResponse(int32 Code, const FString& Body)
{
	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
	Response->Code = static_cast<EHttpServerResponseCodes>(Code);
	return Response;
}

bool FWorldDataMCPServer::HandleMCPOptions(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	OnComplete(MakeJsonResponse(200, TEXT("{}")));
	return true;
}

bool FWorldDataMCPServer::HandleMCPGet(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	OnComplete(MakeJsonResponse(405, TEXT("{\"error\":\"Use POST for MCP Streamable HTTP.\"}")));
	return true;
}

bool FWorldDataMCPServer::HandleMCPPost(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!IsLoopbackHostHeader(Request))
	{
		OnComplete(MakeJsonResponse(403, TEXT("{\"error\":\"Only loopback Host headers (127.0.0.1/localhost) are accepted.\"}")));
		return true;
	}

	if (!IsAllowedOriginHeader(Request))
	{
		OnComplete(MakeJsonResponse(403, TEXT("{\"error\":\"Only loopback Origin values are accepted.\"}")));
		return true;
	}

	const FString ExpectedToken = GetAccessToken();
	const FString ProvidedToken = GetFirstHeaderValue(Request, GetAccessTokenHeaderName());
	if (ExpectedToken.IsEmpty() || !ConstantTimeEquals(ExpectedToken, ProvidedToken))
	{
		OnComplete(MakeJsonResponse(401, TEXT("{\"error\":\"Missing or invalid MCP access token.\"}")));
		return true;
	}

	if (Request.Body.Num() > GMaxMcpRequestBodyBytes)
	{
		OnComplete(MakeJsonResponse(413, TEXT("{\"error\":\"MCP request body is too large.\"}")));
		return true;
	}

	FString Body;
	if (Request.Body.Num() > 0)
	{
		FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		Body = FString(Converter.Length(), Converter.Get());
	}

	if (Body.IsEmpty())
	{
		OnComplete(MakeJsonResponse(400, JsonObjectToString(MakeJsonRpcError(MakeShared<FJsonValueNull>(), -32700, TEXT("Empty request body")).ToSharedRef())));
		return true;
	}

	TSharedPtr<FJsonObject> JsonRequest = ParseObject(Body);
	if (!JsonRequest.IsValid())
	{
		OnComplete(MakeJsonResponse(400, JsonObjectToString(MakeJsonRpcError(MakeShared<FJsonValueNull>(), -32700, TEXT("Invalid JSON")).ToSharedRef())));
		return true;
	}

	FString Method;
	JsonRequest->TryGetStringField(TEXT("method"), Method);

	const FString ProvidedSessionId = GetFirstHeaderValue(Request, TEXT("Mcp-Session-Id"));
	if (Method != TEXT("initialize") && !ProvidedSessionId.IsEmpty())
	{
		bool bKnownSession = false;
		{
			FScopeLock Lock(&GMcpStateMutex);
			bKnownSession = SessionIds.Contains(ProvidedSessionId);
		}
		if (!bKnownSession)
		{
			OnComplete(MakeJsonResponse(404, TEXT("{\"error\":\"Unknown or expired MCP session. Re-initialize to obtain a new session.\"}")));
			return true;
		}
	}

	if (!JsonRequest->HasField(TEXT("id")))
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(FString(), TEXT("text/plain"));
		Response->Code = static_cast<EHttpServerResponseCodes>(202);
		OnComplete(MoveTemp(Response));
		return true;
	}

	TSharedPtr<FJsonObject> Result = ProcessJsonRpc(JsonRequest);
	FString ResponseBody = JsonObjectToString(Result.ToSharedRef());
	TUniquePtr<FHttpServerResponse> Response = MakeJsonResponse(200, ResponseBody);
	FString ProtocolSnapshot;
	{
		FScopeLock Lock(&GMcpStateMutex);
		ProtocolSnapshot = NegotiatedProtocolVersion;
	}
	// Sessions are per-client: initialize mints a fresh id for that client only, and
	// every other request just echoes the caller's own session back.
	const FString ResponseSessionId = Method == TEXT("initialize") ? RegisterSession() : ProvidedSessionId;
	Response->Headers.Add(TEXT("MCP-Protocol-Version"), { ProtocolSnapshot });
	if (!ResponseSessionId.IsEmpty())
	{
		Response->Headers.Add(TEXT("MCP-Session-Id"), { ResponseSessionId });
	}
	OnComplete(MoveTemp(Response));
	return true;
}

FString FWorldDataMCPServer::RegisterSession()
{
	FScopeLock Lock(&GMcpStateMutex);
	const FString NewSessionId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	SessionIds.Add(NewSessionId);
	if (SessionIds.Num() > GMaxConcurrentMcpSessions)
	{
		SessionIds.RemoveAt(0, SessionIds.Num() - GMaxConcurrentMcpSessions);
	}
	return NewSessionId;
}

TSharedPtr<FJsonObject> FWorldDataMCPServer::ProcessJsonRpc(const TSharedPtr<FJsonObject>& Request)
{
	TSharedPtr<FJsonValue> RequestId = Request->TryGetField(TEXT("id"));
	if (!RequestId.IsValid())
	{
		RequestId = MakeShared<FJsonValueNull>();
	}

	FString Method;
	if (!Request->TryGetStringField(TEXT("method"), Method))
	{
		return MakeJsonRpcError(RequestId, -32600, TEXT("Missing JSON-RPC method."));
	}

	const TSharedPtr<FJsonObject>* Params = nullptr;
	Request->TryGetObjectField(TEXT("params"), Params);
	TSharedPtr<FJsonObject> ParamsObj = Params && Params->IsValid() ? *Params : MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> ResultObj;
	if (Method == TEXT("initialize"))
	{
		// Session ids are minted by the HTTP layer (RegisterSession) so each client
		// gets its own; in-process callers (Codex ACP bridge) need no session.
		ResultObj = HandleInitialize(ParamsObj);
	}
	else if (Method == TEXT("tools/list"))
	{
		ResultObj = HandleToolsList(ParamsObj);
	}
	else if (Method == TEXT("tools/call"))
	{
		ResultObj = HandleToolsCall(ParamsObj);
	}
	else if (Method == TEXT("resources/list"))
	{
		ResultObj = HandleResourcesList(ParamsObj);
	}
	else if (Method == TEXT("resources/read"))
	{
		ResultObj = HandleResourcesRead(ParamsObj);
	}
	else if (Method == TEXT("ping"))
	{
		ResultObj = MakeShared<FJsonObject>();
	}
	else
	{
		return MakeJsonRpcError(RequestId, -32601, FString::Printf(TEXT("Method not found: %s"), *Method));
	}

	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetField(TEXT("id"), RequestId);
	Response->SetObjectField(TEXT("result"), ResultObj);
	return Response;
}

FString FWorldDataMCPServer::NegotiateProtocolVersion(const FString& RequestedVersion)
{
	if (!RequestedVersion.IsEmpty())
	{
		for (const TCHAR* const Supported : GSupportedProtocolVersions)
		{
			if (RequestedVersion.Equals(Supported))
			{
				return RequestedVersion;
			}
		}
	}

	// Fall back to the newest version we support so older/newer clients can still
	// proceed instead of failing the handshake.
	return GSupportedProtocolVersions[0];
}

FString FWorldDataMCPServer::GetServerInstructions()
{
	return FString::Printf(TEXT(
		"This MCP server exposes a live Unreal Engine editor session for project '%s'. "
		"Start by reading the resource worlddata://context/bootstrap for a compact, read-only orientation "
		"(engine/level state and a recommended read order). "
		"Use read-only tools (get_current_project_info, list_level_actors, get_selected_actors, get_actor_details, "
		"find_assets, read_asset, get_content_summary, get_codex_policy_snapshot) to gather context first, then use "
		"mutating tools (spawn_actor, transform_actor, delete_actor, attach_actor, set_actor_property, "
		"save_current_level, create_asset, create_blueprint_asset, modify_material_instance, create_pcg_graph_from_recipe, select_actor) "
		"only after you understand the scene. "
		"UEBridgeMCP also includes standalone extracted tools such as list_resources, "
		"read_resource, read_log, execute_python, project file tools, PIE controls, and PCG recipe tools. "
		"All tools act on the user's currently open editor world; prefer precise filters and small maxResults to keep responses compact."),
		*GetProjectName());
}

TSharedPtr<FJsonObject> FWorldDataMCPServer::HandleInitialize(const TSharedPtr<FJsonObject>& Params)
{
	FString RequestedVersion;
	Params->TryGetStringField(TEXT("protocolVersion"), RequestedVersion);
	const FString ProtocolVersion = NegotiateProtocolVersion(RequestedVersion);
	{
		FScopeLock Lock(&GMcpStateMutex);
		NegotiatedProtocolVersion = ProtocolVersion;
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("protocolVersion"), ProtocolVersion);
	Result->SetArrayField(TEXT("supportedProtocolVersions"), MakeSupportedProtocolVersionsArray());
	Result->SetStringField(TEXT("accessTokenHeader"), GetAccessTokenHeaderName());
	Result->SetBoolField(TEXT("requiresAccessToken"), true);
	Result->SetBoolField(TEXT("requiresSessionHeader"), true);

	// The tool/resource catalog is fixed for the editor session and we do not push
	// notifications over this transport, so advertise listChanged=false honestly.
	TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> ToolsCap = MakeShared<FJsonObject>();
	ToolsCap->SetBoolField(TEXT("listChanged"), false);
	Capabilities->SetObjectField(TEXT("tools"), ToolsCap);
	TSharedRef<FJsonObject> ResourcesCap = MakeShared<FJsonObject>();
	ResourcesCap->SetBoolField(TEXT("listChanged"), false);
	ResourcesCap->SetBoolField(TEXT("subscribe"), false);
	Capabilities->SetObjectField(TEXT("resources"), ResourcesCap);
	Result->SetObjectField(TEXT("capabilities"), Capabilities);

	TSharedRef<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
	ServerInfo->SetStringField(TEXT("name"), GetServerName());
	ServerInfo->SetStringField(TEXT("title"), FString::Printf(TEXT("WorldData (%s)"), *GetProjectName()));
	ServerInfo->SetStringField(TEXT("version"), TEXT("0.2.0"));
	Result->SetObjectField(TEXT("serverInfo"), ServerInfo);

	Result->SetStringField(TEXT("instructions"), GetServerInstructions());

	return Result;
}

TSharedPtr<FJsonObject> FWorldDataMCPServer::HandleToolsList(const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonValue>> Tools;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(GetToolDefinitionsJson());
	FJsonSerializer::Deserialize(Reader, Tools);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("tools"), Tools);
	return Result;
}

TSharedPtr<FJsonObject> FWorldDataMCPServer::HandleToolsCall(const TSharedPtr<FJsonObject>& Params)
{
	FString ToolName;
	if (!Params->TryGetStringField(TEXT("name"), ToolName))
	{
		ToolName = TEXT("<missing>");
	}

	FString ArgsJson = TEXT("{}");
	const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("arguments"), ArgsObj) && ArgsObj && ArgsObj->IsValid())
	{
		ArgsJson = JsonObjectToString(ArgsObj->ToSharedRef());
	}

	FString ToolResult;
	if (IsInGameThread())
	{
		ToolResult = DispatchTool(ToolName, ArgsJson);
	}
	else
	{
		// Marshal to the game thread, but never block the HTTP worker forever: if the
		// editor is busy (modal dialog, long operation, PIE transition) we time out and
		// return a structured error instead of hanging the request. The shared promise
		// keeps the async task safe even if we abandon the wait.
		static constexpr double ToolDispatchTimeoutSeconds = 60.0;

		const FString ToolNameCopy = ToolName;
		const FString ArgsCopy = ArgsJson;
		TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
		TSharedRef<FThreadSafeBool, ESPMode::ThreadSafe> bCanceled = MakeShared<FThreadSafeBool, ESPMode::ThreadSafe>();
		bCanceled->AtomicSet(false);
		TFuture<FString> Future = Promise->GetFuture();

		AsyncTask(ENamedThreads::GameThread, [Promise, bCanceled, ToolNameCopy, ArgsCopy]()
		{
			if (*bCanceled)
			{
				Promise->SetValue(ErrorJson(FString::Printf(
					TEXT("Tool '%s' was cancelled before dispatch because the HTTP worker timed out."),
					*ToolNameCopy)));
				return;
			}
			Promise->SetValue(DispatchTool(ToolNameCopy, ArgsCopy));
		});

		if (Future.WaitFor(FTimespan::FromSeconds(ToolDispatchTimeoutSeconds)))
		{
			ToolResult = Future.Get();
		}
		else
		{
			bCanceled->AtomicSet(true);
			ToolResult = ErrorJson(FString::Printf(
				TEXT("Tool '%s' timed out after %.0f seconds waiting for the editor game thread."),
				*ToolName, ToolDispatchTimeoutSeconds));
		}
	}

	// Surface tool-level failures via the MCP `isError` flag so clients can react,
	// while still returning the structured payload as text content.
	bool bIsError = false;
	if (TSharedPtr<FJsonObject> ParsedResult = ParseObject(ToolResult))
	{
		bool bSuccess = true;
		if (ParsedResult->TryGetBoolField(TEXT("success"), bSuccess))
		{
			bIsError = !bSuccess;
		}
	}

	TArray<TSharedPtr<FJsonValue>> Content;
	TSharedRef<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), ToolResult);
	Content.Add(MakeShared<FJsonValueObject>(TextContent));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("content"), Content);
	Result->SetBoolField(TEXT("isError"), bIsError);
	return Result;
}

TSharedPtr<FJsonObject> FWorldDataMCPServer::HandleResourcesList(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Parsed = ParseObject(GetResourceListJson());
	return Parsed.IsValid() ? Parsed : MakeShared<FJsonObject>();
}

TSharedPtr<FJsonObject> FWorldDataMCPServer::HandleResourcesRead(const TSharedPtr<FJsonObject>& Params)
{
	FString Uri;
	if (!Params->TryGetStringField(TEXT("uri"), Uri))
	{
		Uri = TEXT("");
	}

	const FString Text = Uri.IsEmpty()
		? ErrorJson(TEXT("Missing required resource uri."))
		: ReadResource(Uri);

	TSharedRef<FJsonObject> ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("uri"), Uri);
	ContentItem->SetStringField(TEXT("mimeType"), TEXT("application/json"));
	ContentItem->SetStringField(TEXT("text"), Text);

	TArray<TSharedPtr<FJsonValue>> Contents;
	Contents.Add(MakeShared<FJsonValueObject>(ContentItem));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("contents"), Contents);
	return Result;
}

FString FWorldDataMCPServer::DispatchTool(const FString& ToolName, const FString& ArgsJson)
{
	TSharedPtr<FJsonObject> Args = ParseObject(ArgsJson);
	if (!Args.IsValid())
	{
		return ErrorJson(TEXT("Invalid arguments JSON."));
	}

	if (ToolName == TEXT("get_current_project_info"))
	{
		return GetProjectInfoJson();
	}
	if (ToolName == TEXT("list_level_actors"))
	{
		return WorldDataMCP::Tools::ListLevelActors(Args);
	}
	if (ToolName == TEXT("get_selected_actors"))
	{
		return WorldDataMCP::Tools::GetSelectedActors(Args);
	}
	if (ToolName == TEXT("get_actor_details"))
	{
		return WorldDataMCP::Tools::GetActorDetails(Args);
	}
	if (ToolName == TEXT("find_assets"))
	{
		return WorldDataMCP::Tools::FindAssets(Args);
	}
	if (ToolName == TEXT("get_content_summary"))
	{
		return WorldDataMCP::Tools::GetContentSummary(Args);
	}
	if (ToolName == TEXT("read_asset"))
	{
		return WorldDataMCP::Tools::ReadAsset(Args);
	}
	if (ToolName == TEXT("select_actor"))
	{
		return WorldDataMCP::Tools::SelectActor(Args);
	}
	if (ToolName == TEXT("spawn_actor"))
	{
		return WorldDataMCP::Tools::SpawnActor(Args);
	}
	if (ToolName == TEXT("transform_actor"))
	{
		return WorldDataMCP::Tools::TransformActor(Args);
	}
	if (ToolName == TEXT("delete_actor"))
	{
		return WorldDataMCP::Tools::DeleteActor(Args);
	}
	if (ToolName == TEXT("attach_actor"))
	{
		return WorldDataMCP::Tools::AttachActor(Args);
	}
	if (ToolName == TEXT("set_actor_property"))
	{
		return WorldDataMCP::Tools::SetActorProperty(Args);
	}
	if (ToolName == TEXT("save_current_level"))
	{
		return WorldDataMCP::Tools::SaveCurrentLevel(Args);
	}
	if (ToolName == TEXT("create_asset"))
	{
		return WorldDataMCP::Tools::CreateAsset(Args);
	}
	if (ToolName == TEXT("create_blueprint_asset"))
	{
		return WorldDataMCP::Tools::CreateBlueprintAsset(Args);
	}
	if (ToolName == TEXT("modify_material_instance"))
	{
		return WorldDataMCP::Tools::ModifyMaterialInstance(Args);
	}
	if (ToolName == TEXT("create_pcg_graph_from_recipe"))
	{
		return WorldDataMCP::Tools::CreatePcgGraphFromRecipe(Args);
	}
	if (ToolName == TEXT("get_codex_policy_snapshot"))
	{
		return GetCodexPolicySnapshotJson();
	}

	FString ExtractedResult;
	if (WorldDataMCP::ExtractedTools::Dispatch(ToolName, Args, ExtractedResult))
	{
		return ExtractedResult;
	}

	return ErrorJson(FString::Printf(TEXT("Unknown UEBridgeMCP tool: %s"), *ToolName));
}

TSharedPtr<FJsonObject> FWorldDataMCPServer::MakeJsonRpcError(TSharedPtr<FJsonValue> Id, int32 Code, const FString& Message)
{
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetNumberField(TEXT("code"), Code);
	Error->SetStringField(TEXT("message"), Message);

	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
	Response->SetObjectField(TEXT("error"), Error);
	return Response;
}

void FWorldDataMCPServer::SaveConfiguredPort(int32 Port)
{
	const FString Token = GetAccessToken();
	FString ProtocolSnapshot;
	{
		FScopeLock Lock(&GMcpStateMutex);
		ProtocolSnapshot = NegotiatedProtocolVersion;
	}

	TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
	Config->SetNumberField(TEXT("mcpPort"), Port);
	Config->SetStringField(TEXT("projectId"), GetProjectId());
	Config->SetStringField(TEXT("projectName"), GetProjectName());
	Config->SetStringField(TEXT("serverName"), GetServerName());
	Config->SetStringField(TEXT("url"), GetMcpUrl());
	Config->SetNumberField(TEXT("pid"), static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
	Config->SetStringField(TEXT("protocol"), ProtocolSnapshot);
	Config->SetStringField(TEXT("protocolVersion"), ProtocolSnapshot);
	Config->SetArrayField(TEXT("supportedProtocolVersions"), MakeSupportedProtocolVersionsArray());
	Config->SetStringField(TEXT("accessToken"), Token);
	Config->SetStringField(TEXT("accessTokenHeader"), GetAccessTokenHeaderName());
	Config->SetObjectField(TEXT("headers"), MakeAuthHeadersObject(Token));
	Config->SetBoolField(TEXT("requiresAccessToken"), true);
	Config->SetBoolField(TEXT("requiresSessionHeader"), true);
	Config->SetStringField(TEXT("uproject"), GetProjectFilePath());
	Config->SetStringField(TEXT("projectDir"), GetProjectDir());
	Config->SetStringField(TEXT("writtenAtUtc"), FDateTime::UtcNow().ToIso8601());
	SaferReplaceWriteString(JsonObjectToString(Config, true), GetConfigPath());
}

void FWorldDataMCPServer::WriteClientConfig()
{
	if (BoundPort <= 0)
	{
		return;
	}

	const FString Token = GetAccessToken();
	FString ProtocolSnapshot;
	{
		FScopeLock Lock(&GMcpStateMutex);
		ProtocolSnapshot = NegotiatedProtocolVersion;
	}

	auto WriteConfigAtPath = [Token, ProtocolSnapshot](const FString& McpJsonPath)
	{
		TSharedPtr<FJsonObject> Root = LoadJsonFile(McpJsonPath);

		const TSharedPtr<FJsonObject>* ServersPtr = nullptr;
		TSharedPtr<FJsonObject> Servers;
		if (Root->TryGetObjectField(TEXT("mcpServers"), ServersPtr) && ServersPtr && ServersPtr->IsValid())
		{
			Servers = *ServersPtr;
		}
		else
		{
			Servers = MakeShared<FJsonObject>();
		}
		RemoveManagedMcpServerEntries(Servers);

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("type"), TEXT("http"));
		Entry->SetStringField(TEXT("url"), GetMcpUrl());
		Entry->SetObjectField(TEXT("headers"), MakeAuthHeadersObject(Token));
		Entry->SetStringField(TEXT("protocolVersion"), ProtocolSnapshot);
		Entry->SetNumberField(TEXT("tool_timeout_sec"), 120);
		Entry->SetStringField(TEXT("generatedBy"), TEXT("UEBridgeMCP"));
		Entry->SetStringField(TEXT("projectId"), GetProjectId());
		Entry->SetStringField(TEXT("projectName"), GetProjectName());
		Entry->SetStringField(TEXT("serverName"), GetServerName());
		Entry->SetStringField(TEXT("accessTokenHeader"), GetAccessTokenHeaderName());

		Servers->SetObjectField(GetServerName(), Entry);
		Root->SetObjectField(TEXT("mcpServers"), Servers);

		if (!SaferReplaceWriteString(JsonObjectToString(Root.ToSharedRef(), true), McpJsonPath))
		{
			UE_LOG(LogWorldDataMCP, Warning, TEXT("Failed to write MCP client config: %s"), *McpJsonPath);
		}
	};

	WriteConfigAtPath(GetClientConfigFilePath());
	WriteConfigAtPath(GetCursorClientConfigPath());
}

namespace
{
	// TOML escaping for values we control (URL, token, header name). These contain only
	// URL-safe and hex characters, but escape defensively anyway.
	static FString EscapeTomlString(const FString& Value)
	{
		FString Out = Value;
		Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Out.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return Out;
	}

	// Strip a TOML section header line down to the section name, tolerating quoted keys
	// like [mcp_servers."my name"].
	static bool TryParseTomlSectionName(const FString& Line, FString& OutSection)
	{
		FString Trimmed = Line;
		Trimmed.TrimStartAndEndInline();
		if (!Trimmed.StartsWith(TEXT("[")) || !Trimmed.EndsWith(TEXT("]")))
		{
			return false;
		}
		OutSection = Trimmed.Mid(1, Trimmed.Len() - 2).TrimStartAndEnd();
		OutSection.ReplaceInline(TEXT("\""), TEXT(""));
		return true;
	}

	static bool IsManagedCodexSection(const FString& Section, const FString& ManagedServerName)
	{
		if (!Section.StartsWith(TEXT("mcp_servers.")))
		{
			return false;
		}
		const FString ServerPart = Section.RightChop(12); // strip "mcp_servers."
		FString ServerName = ServerPart;
		int32 DotIndex = INDEX_NONE;
		if (ServerPart.FindChar(TCHAR('.'), DotIndex))
		{
			ServerName = ServerPart.Left(DotIndex); // covers sub-tables like mcp_servers.name.env
		}
		return ServerName == ManagedServerName || ServerName.StartsWith(TEXT("world_data_"));
	}
}

void FWorldDataMCPServer::WriteCodexClientConfig()
{
	if (BoundPort <= 0)
	{
		return;
	}

	const FString ConfigPath = GetCodexClientConfigFilePath();
	if (ConfigPath.IsEmpty())
	{
		UE_LOG(LogWorldDataMCP, Warning, TEXT("Cannot locate the user home directory; skipped Codex CLI config."));
		return;
	}

	const FString ManagedServerName = GetServerName();

	// Preserve everything the user wrote, drop only sections this plugin manages
	// (exact server name or any stale world_data_* entry), then append the fresh block.
	FString Existing;
	FFileHelper::LoadFileToString(Existing, *ConfigPath);

	TArray<FString> KeptLines;
	bool bSkippingManagedSection = false;
	TArray<FString> Lines;
	Existing.ParseIntoArrayLines(Lines, false);
	for (const FString& Line : Lines)
	{
		FString Section;
		if (TryParseTomlSectionName(Line, Section))
		{
			bSkippingManagedSection = IsManagedCodexSection(Section, ManagedServerName);
		}
		if (!bSkippingManagedSection)
		{
			KeptLines.Add(Line);
		}
	}

	// Trim trailing blank lines so repeated refreshes do not grow the file.
	while (KeptLines.Num() > 0 && KeptLines.Last().TrimStartAndEnd().IsEmpty())
	{
		KeptLines.RemoveAt(KeptLines.Num() - 1);
	}

	FString Output = FString::Join(KeptLines, TEXT("\n"));
	if (!Output.IsEmpty())
	{
		Output += TEXT("\n\n");
	}

	// Codex CLI streamable HTTP schema: url + http_headers. startup_timeout_sec covers
	// slow first responses while the editor is loading; tool_timeout_sec matches the
	// JSON client configs.
	Output += FString::Printf(
		TEXT("# Managed by UEBridgeMCP (%s). Do not edit this section; it is rewritten when the editor MCP server starts.\n")
		TEXT("[mcp_servers.%s]\n")
		TEXT("url = \"%s\"\n")
		TEXT("http_headers = { \"%s\" = \"%s\" }\n")
		TEXT("startup_timeout_sec = 30\n")
		TEXT("tool_timeout_sec = 120\n"),
		*GetProjectName(),
		*ManagedServerName,
		*EscapeTomlString(GetMcpUrl()),
		*EscapeTomlString(GetAccessTokenHeaderName()),
		*EscapeTomlString(GetAccessToken()));

	if (!SaferReplaceWriteString(Output, ConfigPath))
	{
		UE_LOG(LogWorldDataMCP, Warning, TEXT("Failed to write Codex CLI MCP config: %s"), *ConfigPath);
	}
}

void FWorldDataMCPServer::WriteClaudeProjectSettings()
{
	if (BoundPort <= 0)
	{
		return;
	}

	// Claude Code prompts per project before using .mcp.json servers. Pre-approving
	// project MCP servers makes the connection genuinely one-click. Only this single
	// key is merged; the rest of the user's settings file is preserved.
	const FString SettingsPath = GetClaudeSettingsFilePath();
	TSharedPtr<FJsonObject> Root = LoadJsonFile(SettingsPath);

	bool bAlreadyEnabled = false;
	if (Root->TryGetBoolField(TEXT("enableAllProjectMcpServers"), bAlreadyEnabled) && bAlreadyEnabled)
	{
		return;
	}

	Root->SetBoolField(TEXT("enableAllProjectMcpServers"), true);
	if (!SaferReplaceWriteString(JsonObjectToString(Root.ToSharedRef(), true), SettingsPath))
	{
		UE_LOG(LogWorldDataMCP, Warning, TEXT("Failed to write Claude Code project settings: %s"), *SettingsPath);
	}
}

FString FWorldDataMCPServer::GetCliSetupReportJson()
{
	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetBoolField(TEXT("serverRunning"), bRunning);
	Report->SetStringField(TEXT("serverName"), GetServerName());
	Report->SetStringField(TEXT("url"), bRunning ? GetMcpUrl() : TEXT(""));
	Report->SetStringField(TEXT("accessTokenHeader"), GetAccessTokenHeaderName());

	auto DescribeFile = [](const FString& Path, const FString& Purpose)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("path"), Path);
		Item->SetStringField(TEXT("purpose"), Purpose);
		Item->SetBoolField(TEXT("exists"), !Path.IsEmpty() && FPaths::FileExists(Path));
		return Item;
	};

	TSharedRef<FJsonObject> Clients = MakeShared<FJsonObject>();

	TSharedRef<FJsonObject> Claude = MakeShared<FJsonObject>();
	Claude->SetObjectField(TEXT("mcpConfig"), DescribeFile(GetClientConfigFilePath(), TEXT("Project-scoped MCP servers (.mcp.json).")));
	Claude->SetObjectField(TEXT("settings"), DescribeFile(GetClaudeSettingsFilePath(), TEXT("enableAllProjectMcpServers pre-approval.")));
	Claude->SetStringField(TEXT("usage"), TEXT("Run `claude` inside the project directory; the server is picked up automatically."));
	Clients->SetObjectField(TEXT("claudeCode"), Claude);

	TSharedRef<FJsonObject> Cursor = MakeShared<FJsonObject>();
	Cursor->SetObjectField(TEXT("mcpConfig"), DescribeFile(GetCursorClientConfigPath(), TEXT("Cursor project MCP servers (.cursor/mcp.json).")));
	Cursor->SetStringField(TEXT("usage"), TEXT("Run `cursor-agent` (or open Cursor) with this project as the workspace root."));
	Clients->SetObjectField(TEXT("cursor"), Cursor);

	TSharedRef<FJsonObject> Codex = MakeShared<FJsonObject>();
	Codex->SetObjectField(TEXT("mcpConfig"), DescribeFile(GetCodexClientConfigFilePath(), TEXT("Global Codex MCP servers (~/.codex/config.toml).")));
	Codex->SetStringField(TEXT("usage"), TEXT("Run `codex` anywhere; restart any running codex session after refresh so it reloads config.toml."));
	Clients->SetObjectField(TEXT("codex"), Codex);

	Report->SetObjectField(TEXT("clients"), Clients);

	TArray<TSharedPtr<FJsonValue>> Notes;
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Configs are rewritten automatically whenever the MCP server starts or connection files are refreshed.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("The access token persists across editor restarts, so existing CLI sessions keep working after a relaunch.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Multiple CLI clients can stay connected at the same time; each gets its own MCP session.")));
	Report->SetArrayField(TEXT("notes"), Notes);

	return JsonObjectToString(Report, true);
}

void FWorldDataMCPServer::WriteProjectConnectionFile()
{
	if (BoundPort <= 0)
	{
		return;
	}

	TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
	const FString Token = GetAccessToken();
	FString ProtocolSnapshot;
	{
		FScopeLock Lock(&GMcpStateMutex);
		ProtocolSnapshot = NegotiatedProtocolVersion;
	}
	Info->SetStringField(TEXT("projectName"), GetProjectName());
	Info->SetStringField(TEXT("projectId"), GetProjectId());
	Info->SetStringField(TEXT("serverName"), GetServerName());
	Info->SetStringField(TEXT("url"), GetMcpUrl());
	Info->SetNumberField(TEXT("port"), BoundPort);
	Info->SetNumberField(TEXT("pid"), static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
	Info->SetStringField(TEXT("protocol"), ProtocolSnapshot);
	Info->SetStringField(TEXT("protocolVersion"), ProtocolSnapshot);
	Info->SetArrayField(TEXT("supportedProtocolVersions"), MakeSupportedProtocolVersionsArray());
	Info->SetStringField(TEXT("accessToken"), Token);
	Info->SetStringField(TEXT("accessTokenHeader"), GetAccessTokenHeaderName());
	Info->SetObjectField(TEXT("headers"), MakeAuthHeadersObject(Token));
	Info->SetBoolField(TEXT("requiresAccessToken"), true);
	Info->SetBoolField(TEXT("requiresSessionHeader"), true);
	Info->SetBoolField(TEXT("running"), true);
	Info->SetStringField(TEXT("uproject"), GetProjectFilePath());
	Info->SetStringField(TEXT("projectDir"), GetProjectDir());
	Info->SetStringField(TEXT("writtenAtUtc"), FDateTime::UtcNow().ToIso8601());
	SaferReplaceWriteString(JsonObjectToString(Info, true), GetConnectionPath());
}
