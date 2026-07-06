#include "WorldDataMCPServer.h"
#include "WorldDataMCPServerInternal.h"
#include "WorldDataMCPCommon.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

using namespace WorldDataMCP;
using namespace WorldDataMCP::ServerInternal;

DEFINE_LOG_CATEGORY_STATIC(LogWorldDataMCPConfig, Log, All);

FString FWorldDataMCPServer::GetClientConfigFilePath()
{
	FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT(".mcp.json"));
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
	WriteProjectConnectionFile();
	LastRefreshAtUtc = FDateTime::UtcNow();
	LastError.Empty();
}

FString FWorldDataMCPServer::GetAccessToken()
{
	return GetConfiguredAccessToken();
}

void FWorldDataMCPServer::SaveConfiguredPort(int32 Port)
{
	TSharedPtr<FJsonObject> ExistingConfig = LoadJsonFile(GetConfigPath());
	FString AccessToken;
	if (!TryGetConfigAccessToken(ExistingConfig, AccessToken))
	{
		AccessToken = GenerateAccessToken();
	}

	TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
	Config->SetNumberField(TEXT("mcpPort"), Port);
	Config->SetStringField(TEXT("projectId"), GetProjectId());
	Config->SetStringField(TEXT("projectName"), GetProjectName());
	Config->SetStringField(TEXT("serverName"), GetServerName());
	Config->SetStringField(TEXT("uproject"), GetProjectFilePath());
	Config->SetStringField(TEXT("projectDir"), GetProjectDir());
	Config->SetStringField(TEXT("accessToken"), AccessToken);
	Config->SetStringField(TEXT("accessTokenHeader"), TEXT("X-WorldData-MCP-Token"));
	SaferReplaceWriteString(JsonObjectToString(Config, true), GetConfigPath());
}

void FWorldDataMCPServer::WriteClientConfig()
{
	if (BoundPort <= 0)
	{
		return;
	}

	auto WriteConfigAtPath = [](const FString& McpJsonPath, bool bIncludeTrustedHeaders)
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

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("type"), TEXT("http"));
		Entry->SetStringField(TEXT("url"), GetMcpUrl());
		Entry->SetNumberField(TEXT("tool_timeout_sec"), 120);
		if (bIncludeTrustedHeaders)
		{
			Entry->SetObjectField(TEXT("headers"), MakeAccessTokenHeadersObject());
		}

		Servers->SetObjectField(GetServerName(), Entry);
		Root->SetObjectField(TEXT("mcpServers"), Servers);

		if (!SaferReplaceWriteString(JsonObjectToString(Root.ToSharedRef(), true), McpJsonPath))
		{
			UE_LOG(LogWorldDataMCPConfig, Warning, TEXT("Failed to write MCP client config: %s"), *McpJsonPath);
		}
	};

	// Keep project-root client config files shareable: they advertise the local endpoint but
	// do not store the trusted-client token. The tokenized connection file stays under Saved/.
	WriteConfigAtPath(GetClientConfigFilePath(), false);
	WriteConfigAtPath(GetCursorClientConfigPath(), false);
}

void FWorldDataMCPServer::WriteProjectConnectionFile()
{
	if (BoundPort <= 0)
	{
		return;
	}

	FString ProtocolVersionSnapshot;
	{
		FScopeLock Lock(&SessionMutex);
		ProtocolVersionSnapshot = NegotiatedProtocolVersion;
	}

	TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
	Info->SetStringField(TEXT("projectName"), GetProjectName());
	Info->SetStringField(TEXT("projectId"), GetProjectId());
	Info->SetStringField(TEXT("serverName"), GetServerName());
	Info->SetStringField(TEXT("url"), GetMcpUrl());
	Info->SetNumberField(TEXT("port"), BoundPort);
	Info->SetStringField(TEXT("protocolVersion"), ProtocolVersionSnapshot);
	Info->SetArrayField(TEXT("supportedProtocolVersions"), MakeSupportedProtocolVersionValues());
	Info->SetNumberField(TEXT("pid"), static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
	Info->SetStringField(TEXT("uproject"), GetProjectFilePath());
	Info->SetStringField(TEXT("projectDir"), GetProjectDir());
	Info->SetObjectField(TEXT("headers"), MakeAccessTokenHeadersObject());
	Info->SetBoolField(TEXT("requireSessionHeader"), GWorldDataMCPRequireSessionHeader != 0);
	Info->SetNumberField(TEXT("paginationPageSize"), GWorldDataMCPPaginationPageSize);
	Info->SetStringField(TEXT("writtenAtUtc"), FDateTime::UtcNow().ToIso8601());
	SaferReplaceWriteString(JsonObjectToString(Info, true), GetConnectionPath());
}
