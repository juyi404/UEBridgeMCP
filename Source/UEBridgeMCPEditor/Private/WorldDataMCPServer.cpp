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
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/ThreadSafeBool.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Misc/Base64.h"
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
#include "WorldDataMCPServerInternal.h"
#include "WorldDataMCPTools.h"
#include "UEBridgeMCPExtractedTools.h"
#include "WorldDataMCPEditorTools.h"
#include "WorldDataMCPSceneTools.h"
#include "WorldDataMCPAuthoringTools.h"
#include "WorldDataMCPGraphTools.h"
#include "WorldDataMCPAIAnimTools.h"
#include "WorldDataMCPKnowledgeTools.h"
#include "WorldDataMCPStateTreeTools.h"
#include "WorldDataMCPBlueprintTools.h"
#include "WorldDataMCPAnimTools.h"
#include "WorldDataMCPMatLandTools.h"
#include "WorldDataMCPNiagaraTools.h"
#include "WorldDataMCPFoliageTools.h"
#include "WorldDataMCPSequencerTools.h"
#include "WorldDataMCPGameplayTools.h"
#include "WorldDataMCPUIDataTools.h"
#include "WorldDataMCPGasTools.h"
#include "WorldDataMCPMaterialInstanceTools.h"
#include "WorldDataMCPPcgKnowledgeTools.h"
// NOTE: the legacy nwiro handler set is archived under Plugins/UEBridgeMCP/Legacy/Nwiro.
// It is intentionally outside this module's Source tree so WorldData MCP is the only
// server compiled and advertised by UEBridgeMCPEditor.

using namespace WorldDataMCP;
using namespace WorldDataMCP::ServerInternal;

DEFINE_LOG_CATEGORY_STATIC(LogWorldDataMCP, Log, All);

TSharedPtr<IHttpRouter> FWorldDataMCPServer::HttpRouter = nullptr;
TArray<FHttpRouteHandle> FWorldDataMCPServer::RouteHandles;
int32 FWorldDataMCPServer::BoundPort = 0;
bool FWorldDataMCPServer::bRunning = false;
TSet<FString> FWorldDataMCPServer::SessionIds;
FString FWorldDataMCPServer::NegotiatedProtocolVersion(TEXT("2025-11-25"));
FCriticalSection FWorldDataMCPServer::SessionMutex;
FString FWorldDataMCPServer::LastError;

namespace
{

	static constexpr const TCHAR* McpSessionIdHeader = TEXT("Mcp-Session-Id");
	static constexpr const TCHAR* McpProtocolVersionHeader = TEXT("Mcp-Protocol-Version");
	static constexpr int32 MaxMcpRequestBodyBytes = 4 * 1024 * 1024;

}
FDateTime FWorldDataMCPServer::StartedAtUtc;
FDateTime FWorldDataMCPServer::LastRefreshAtUtc;

namespace
{

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

	static bool IsTrustedHttpClient(const FHttpServerRequest& Request)
	{
		const FString ExpectedToken = GetConfiguredAccessToken();
		if (ExpectedToken.IsEmpty())
		{
			return false;
		}

		FString ProvidedToken = GetFirstHeaderValue(Request, TEXT("X-WorldData-MCP-Token"));
		if (ProvidedToken.IsEmpty())
		{
			const FString Authorization = GetFirstHeaderValue(Request, TEXT("Authorization"));
			static const FString BearerPrefix(TEXT("Bearer "));
			if (Authorization.StartsWith(BearerPrefix, ESearchCase::IgnoreCase))
			{
				ProvidedToken = Authorization.RightChop(BearerPrefix.Len());
			}
		}

		ProvidedToken.TrimStartAndEndInline();
		return !ProvidedToken.IsEmpty() && ProvidedToken.Equals(ExpectedToken, ESearchCase::CaseSensitive);
	}

	struct FWorldDataPaginationResult
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		TOptional<FString> NextCursor;
	};

	static bool IsAllDigits(const FString& String)
	{
		for (const TCHAR Character : String)
		{
			if (!FChar::IsDigit(Character))
			{
				return false;
			}
		}
		return true;
	}

	static bool ApplyPagination(const TArray<TSharedPtr<FJsonValue>>& AllItems, const TSharedPtr<FJsonObject>& Params, FWorldDataPaginationResult& OutResult, FString& OutError)
	{
		const int32 PageSize = GWorldDataMCPPaginationPageSize;
		if (PageSize <= 0)
		{
			OutResult.Items = AllItems;
			return true;
		}

		int32 Offset = 0;
		if (Params.IsValid())
		{
			FString Cursor;
			if (Params->TryGetStringField(TEXT("cursor"), Cursor) && !Cursor.IsEmpty())
			{
				FString DecodedCursor;
				if (!FBase64::Decode(Cursor, DecodedCursor) || DecodedCursor.IsEmpty() || !IsAllDigits(DecodedCursor))
				{
					OutError = TEXT("Invalid pagination cursor");
					return false;
				}

				const int64 DecodedOffset = FCString::Atoi64(*DecodedCursor);
				if (DecodedOffset < 0 || DecodedOffset > MAX_int32)
				{
					OutError = TEXT("Invalid pagination cursor");
					return false;
				}
				Offset = static_cast<int32>(DecodedOffset);
			}
		}

		Offset = FMath::Clamp(Offset, 0, AllItems.Num());
		const int32 End = static_cast<int32>(FMath::Min(static_cast<int64>(Offset) + PageSize, static_cast<int64>(AllItems.Num())));

		OutResult.Items.Reserve(End - Offset);
		for (int32 Index = Offset; Index < End; ++Index)
		{
			OutResult.Items.Add(AllItems[Index]);
		}

		if (End < AllItems.Num())
		{
			OutResult.NextCursor = FBase64::Encode(FString::FromInt(End));
		}

		return true;
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
	RouteHandles.Add(HttpRouter->BindRoute(FHttpPath(TEXT("/mcp")), EHttpServerRequestVerbs::VERB_DELETE,
		FHttpRequestHandler::CreateStatic(&FWorldDataMCPServer::HandleMCPDelete)));
	RouteHandles.Add(HttpRouter->BindRoute(FHttpPath(TEXT("/mcp")), EHttpServerRequestVerbs::VERB_OPTIONS,
		FHttpRequestHandler::CreateStatic(&FWorldDataMCPServer::HandleMCPOptions)));

	const bool bAllRoutesBound = RouteHandles.Num() == 4 && Algo::AllOf(RouteHandles, [](const FHttpRouteHandle& RouteHandle)
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
		FScopeLock Lock(&SessionMutex);
		SessionIds.Empty();
		NegotiatedProtocolVersion = GetNewestProtocolVersion();
	}
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

FString FWorldDataMCPServer::GetProjectInfoJson()
{
	FString ProtocolVersionSnapshot;
	{
		FScopeLock Lock(&SessionMutex);
		ProtocolVersionSnapshot = NegotiatedProtocolVersion;
	}

	TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
	Info->SetBoolField(TEXT("success"), true);
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
	Info->SetBoolField(TEXT("running"), bRunning);
	Info->SetStringField(TEXT("startedAtUtc"), StartedAtUtc.GetTicks() > 0 ? StartedAtUtc.ToIso8601() : TEXT(""));
	Info->SetStringField(TEXT("lastRefreshAtUtc"), LastRefreshAtUtc.GetTicks() > 0 ? LastRefreshAtUtc.ToIso8601() : TEXT(""));
	Info->SetStringField(TEXT("lastError"), LastError);
	Info->SetStringField(TEXT("clientConfigFile"), GetClientConfigFilePath());
	Info->SetStringField(TEXT("cursorClientConfigFile"), GetCursorClientConfigPath());
	Info->SetStringField(TEXT("savedConfigFile"), GetSavedConfigFilePath());
	Info->SetStringField(TEXT("connectionFile"), GetConnectionFilePath());
	Info->SetStringField(TEXT("trustedClientHeader"), TEXT("X-WorldData-MCP-Token"));
	Info->SetBoolField(TEXT("requireSessionHeader"), GWorldDataMCPRequireSessionHeader != 0);
	Info->SetNumberField(TEXT("paginationPageSize"), GWorldDataMCPPaginationPageSize);
	return JsonObjectToString(Info);
}

FString FWorldDataMCPServer::GetStatusJson()
{
	FString ProtocolVersionSnapshot;
	{
		FScopeLock Lock(&SessionMutex);
		ProtocolVersionSnapshot = NegotiatedProtocolVersion;
	}

	TSharedRef<FJsonObject> Status = MakeShared<FJsonObject>();
	Status->SetBoolField(TEXT("running"), bRunning);
	Status->SetStringField(TEXT("serverName"), GetServerName());
	Status->SetStringField(TEXT("projectId"), GetProjectId());
	Status->SetNumberField(TEXT("port"), BoundPort);
	Status->SetStringField(TEXT("url"), bRunning ? GetMcpUrl() : TEXT(""));
	Status->SetStringField(TEXT("protocolVersion"), ProtocolVersionSnapshot);
	Status->SetArrayField(TEXT("supportedProtocolVersions"), MakeSupportedProtocolVersionValues());
	Status->SetNumberField(TEXT("routeCount"), RouteHandles.Num());
	Status->SetStringField(TEXT("startedAtUtc"), StartedAtUtc.GetTicks() > 0 ? StartedAtUtc.ToIso8601() : TEXT(""));
	Status->SetStringField(TEXT("lastRefreshAtUtc"), LastRefreshAtUtc.GetTicks() > 0 ? LastRefreshAtUtc.ToIso8601() : TEXT(""));
	Status->SetStringField(TEXT("lastError"), LastError);
	Status->SetStringField(TEXT("clientConfigFile"), GetClientConfigFilePath());
	Status->SetStringField(TEXT("cursorClientConfigFile"), GetCursorClientConfigPath());
	Status->SetStringField(TEXT("savedConfigFile"), GetSavedConfigFilePath());
	Status->SetStringField(TEXT("connectionFile"), GetConnectionFilePath());
	Status->SetStringField(TEXT("trustedClientHeader"), TEXT("X-WorldData-MCP-Token"));
	Status->SetBoolField(TEXT("requireSessionHeader"), GWorldDataMCPRequireSessionHeader != 0);
	Status->SetNumberField(TEXT("paginationPageSize"), GWorldDataMCPPaginationPageSize);
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

	// Resources implemented in the extracted-tools module (plugins, source-index, level/current,
	// components, blueprints, pcg graphs, problems, performance, log, viewport). Returns a
	// structured "Unknown resource" error for anything genuinely unrecognized.
	return WorldDataMCP::ExtractedTools::ReadExtendedResource(Uri);
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

bool FWorldDataMCPServer::HandleMCPDelete(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
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

	const FString ProvidedSessionId = GetFirstHeaderValue(Request, McpSessionIdHeader);
	{
		FScopeLock Lock(&SessionMutex);
		if (ProvidedSessionId.IsEmpty() || !SessionIds.Contains(ProvidedSessionId))
		{
			OnComplete(MakeJsonResponse(400, TEXT("{\"error\":\"Missing or unknown MCP session.\"}")));
			return true;
		}

		SessionIds.Remove(ProvidedSessionId);
		if (SessionIds.IsEmpty())
		{
			NegotiatedProtocolVersion = GetNewestProtocolVersion();
		}
	}
	OnComplete(MakeJsonResponse(202, TEXT("{}")));
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

	if (Request.Body.Num() > MaxMcpRequestBodyBytes)
	{
		OnComplete(MakeJsonResponse(413, JsonObjectToString(MakeJsonRpcError(MakeShared<FJsonValueNull>(), -32600, TEXT("MCP request body is too large.")).ToSharedRef())));
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

	TSharedPtr<FJsonObject> JsonRequest = ParseJsonObject(Body);
	if (!JsonRequest.IsValid())
	{
		OnComplete(MakeJsonResponse(400, JsonObjectToString(MakeJsonRpcError(MakeShared<FJsonValueNull>(), -32700, TEXT("Invalid JSON")).ToSharedRef())));
		return true;
	}

	TSharedPtr<FJsonValue> RequestId = JsonRequest->TryGetField(TEXT("id"));
	if (!RequestId.IsValid())
	{
		RequestId = MakeShared<FJsonValueNull>();
	}

	FString Method;
	JsonRequest->TryGetStringField(TEXT("method"), Method);

	auto CompleteJsonRpcError = [&OnComplete, &RequestId](int32 HttpCode, int32 JsonRpcCode, const FString& Message)
	{
		OnComplete(FWorldDataMCPServer::MakeJsonResponse(HttpCode, JsonObjectToString(FWorldDataMCPServer::MakeJsonRpcError(RequestId, JsonRpcCode, Message).ToSharedRef())));
	};

	const bool bIsInitialize = Method == TEXT("initialize");
	const bool bSessionlessMethod = Method.IsEmpty() || bIsInitialize || Method == TEXT("ping");
	const FString ProvidedSessionId = GetFirstHeaderValue(Request, McpSessionIdHeader);
	{
		FScopeLock Lock(&SessionMutex);
		if (!bSessionlessMethod)
		{
			if (!ProvidedSessionId.IsEmpty() && !SessionIds.Contains(ProvidedSessionId))
			{
				CompleteJsonRpcError(404, -32600, TEXT("Unknown or expired MCP session."));
				return true;
			}

			if (GWorldDataMCPRequireSessionHeader != 0)
			{
				if (ProvidedSessionId.IsEmpty())
				{
					CompleteJsonRpcError(400, -32600, FString::Printf(TEXT("Missing required Mcp-Session-Id header for '%s'."), *Method));
					return true;
				}
				if (!SessionIds.Contains(ProvidedSessionId))
				{
					CompleteJsonRpcError(404, -32600, FString::Printf(TEXT("Unknown session id '%s' for '%s'; client should reinitialize."), *ProvidedSessionId, *Method));
					return true;
				}
			}
		}

		const FString HeaderProtocolVersion = GetFirstHeaderValue(Request, McpProtocolVersionHeader);
		if (!bIsInitialize && !HeaderProtocolVersion.IsEmpty() && !SessionIds.IsEmpty() && HeaderProtocolVersion != NegotiatedProtocolVersion)
		{
			CompleteJsonRpcError(400, -32600, FString::Printf(
				TEXT("Mcp-Protocol-Version header '%s' does not match negotiated version '%s'."),
				*HeaderProtocolVersion,
				*NegotiatedProtocolVersion));
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

	const bool bTrustedToolAccess = IsTrustedHttpClient(Request);
	FString NewSessionId;
	TSharedPtr<FJsonObject> Result = ProcessJsonRpc(JsonRequest, bTrustedToolAccess, &NewSessionId);
	FString ResponseBody = JsonObjectToString(Result.ToSharedRef());
	TUniquePtr<FHttpServerResponse> Response = MakeJsonResponse(200, ResponseBody);
	FString ProtocolVersionForResponse;
	{
		FScopeLock Lock(&SessionMutex);
		ProtocolVersionForResponse = NegotiatedProtocolVersion;
	}
	Response->Headers.Add(McpProtocolVersionHeader, { ProtocolVersionForResponse });
	const FString ResponseSessionId = bIsInitialize ? NewSessionId : ProvidedSessionId;
	if (!ResponseSessionId.IsEmpty())
	{
		Response->Headers.Add(McpSessionIdHeader, { ResponseSessionId });
	}
	OnComplete(MoveTemp(Response));
	return true;
}

TSharedPtr<FJsonObject> FWorldDataMCPServer::ProcessJsonRpc(const TSharedPtr<FJsonObject>& Request, bool bTrustedToolAccess, FString* OutNewSessionId)
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
		{
			FScopeLock Lock(&SessionMutex);
			ResultObj = HandleInitialize(ParamsObj);
			const FString NewSessionId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			SessionIds.Add(NewSessionId);
			if (OutNewSessionId)
			{
				*OutNewSessionId = NewSessionId;
			}
		}
	}
	else if (Method == TEXT("notifications/initialized") || Method == TEXT("notifications/cancelled"))
	{
		ResultObj = MakeShared<FJsonObject>();
	}
	else if (Method == TEXT("tools/list"))
	{
		FString Error;
		ResultObj = HandleToolsList(ParamsObj, &Error);
		if (!Error.IsEmpty())
		{
			return MakeJsonRpcError(RequestId, -32602, Error);
		}
	}
	else if (Method == TEXT("tools/call"))
	{
		ResultObj = HandleToolsCall(ParamsObj, bTrustedToolAccess);
	}
	else if (Method == TEXT("resources/list"))
	{
		FString Error;
		ResultObj = HandleResourcesList(ParamsObj, &Error);
		if (!Error.IsEmpty())
		{
			return MakeJsonRpcError(RequestId, -32602, Error);
		}
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
	if (!RequestedVersion.IsEmpty() && IsSupportedProtocolVersion(RequestedVersion))
	{
		return RequestedVersion;
	}

	// Fall back to the newest version we support so older/newer clients can still
	// proceed instead of failing the handshake.
	return GetNewestProtocolVersion();
}

FString FWorldDataMCPServer::GetServerInstructions()
{
	return FString::Printf(TEXT(
		"This MCP server exposes a live Unreal Engine 5 editor session for project '%s'. There are MANY tools; "
		"do NOT scan them all. Read worlddata://context/bootstrap FIRST — it contains the working method and a "
		"task->tools cheat-sheet so you use the few right tools instead of guessing among hundreds.\n"
		"\n"
		"WORKING METHOD (follow this for any non-trivial task):\n"
		"1) RESEARCH the UE-correct approach before acting. You likely do not natively know UE5's exact levers. "
		"Use ue_web_search / ue_fetch_doc for docs, pcg_assist for PCG tasks (it pulls a local PCG knowledge base + web + an LLM and returns a node-by-node plan), "
		"and llm_think to reason. The correct lever is often NOT the obvious one (e.g. a sunset = rotating the Atmosphere Sun Light to a low angle, NOT setting the sky colour).\n"
		"2) PERCEIVE the current state. Read worlddata://context/current-task, then describe_scene (what drives the look) and list_level_actors. Do not act blind.\n"
		"3) ACT with the most HIGH-LEVEL tool that fits. Prefer one intent tool (e.g. pcg_assist, set_light_properties) over orchestrating dozens of low-level verbs.\n"
		"4) VERIFY: capture_viewport and compare to the goal; iterate. This perceive->act->perceive loop matters most for visual tasks.\n"
		"\n"
		"GUARDRAILS (learned from past failures — follow these):\n"
		"- Confirm the CURRENT level before acting: the editor may be on a different level than you assume, and the target actor may live elsewhere (get_level_info / list_level_actors). Re-check after anything that could switch levels.\n"
		"- Do NOT take irreversible actions (delete, overwrite, save-over, migrate geometry) based on your own derived measurements alone. Confirm the problem against user-visible evidence (capture_viewport, or ask) first — a number you computed is not proof the user sees a problem.\n"
		"- If you moved or rotated the viewport camera to frame a screenshot, restore its prior pose afterward (location+rotation, roll back to 0); a left-over roll makes the user's viewport look tilted.\n"
		"- A tool can fail silently: always check result fields like applied/failed/entriesAdded/meshes. Success with an empty or None payload means it did NOT take effect — diagnose before moving on.\n"
		"- Prefer the dedicated tool. Only fall back to execute_python after probing the exact API (dir()/hasattr) — do not guess UE Python method names; each wrong guess is a wasted high-risk call.\n"
		"\n"
		"Gather context with read-only tools first (get_current_project_info, list_level_actors, get_actor_details, find_assets, get_relevant_context, summarize_blueprint, describe_scene). "
		"Mutating/asset-creating/network/LLM tools require the X-WorldData-MCP-Token trusted-client header. "
		"High-risk tools (code execution, console commands, file writes/deletes/moves, destructive editor operations, reflected object/widget writes) also require explicit human approval: "
		"ask the user to approve the exact action, then call with confirmDangerousAction=true and confirmationReason explaining what was approved. "
		"All tools act on the user's currently open editor world; prefer precise filters and small maxResults to keep responses compact."),
		*GetProjectName());
}

TSharedPtr<FJsonObject> FWorldDataMCPServer::HandleInitialize(const TSharedPtr<FJsonObject>& Params)
{
	FString RequestedVersion;
	Params->TryGetStringField(TEXT("protocolVersion"), RequestedVersion);
	NegotiatedProtocolVersion = NegotiateProtocolVersion(RequestedVersion);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("protocolVersion"), NegotiatedProtocolVersion);

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

TSharedPtr<FJsonObject> FWorldDataMCPServer::HandleToolsList(const TSharedPtr<FJsonObject>& Params, FString* OutError)
{
	TArray<TSharedPtr<FJsonValue>> AllTools;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(GetToolDefinitionsJson());
	FJsonSerializer::Deserialize(Reader, AllTools);

	FWorldDataPaginationResult Pagination;
	FString PaginationError;
	if (!ApplyPagination(AllTools, Params, Pagination, PaginationError))
	{
		if (OutError)
		{
			*OutError = PaginationError;
		}
		return MakeShared<FJsonObject>();
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("tools"), MoveTemp(Pagination.Items));
	if (Pagination.NextCursor.IsSet())
	{
		Result->SetStringField(TEXT("nextCursor"), Pagination.NextCursor.GetValue());
	}
	return Result;
}

TSharedPtr<FJsonObject> FWorldDataMCPServer::HandleToolsCall(const TSharedPtr<FJsonObject>& Params, bool bTrustedToolAccess)
{
	FString ToolName;
	if (!Params->TryGetStringField(TEXT("name"), ToolName))
	{
		ToolName = TEXT("<missing>");
	}

	FString ArgsJson = TEXT("{}");
	TSharedPtr<FJsonObject> ArgsForPolicy = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("arguments"), ArgsObj) && ArgsObj && ArgsObj->IsValid())
	{
		ArgsForPolicy = *ArgsObj;
		ArgsJson = JsonObjectToString(ArgsObj->ToSharedRef());
	}

	FString ToolResult;
	if (!bTrustedToolAccess && ToolRequiresTrustedClient(ToolName))
	{
		ToolResult = ErrorJson(FString::Printf(
			TEXT("Tool '%s' requires a trusted MCP client. Use the generated local MCP config with X-WorldData-MCP-Token, or run the action from the in-editor ACP panel."),
			*ToolName));
	}
	else
	{
		FString ConfirmationError;
		if (!ToolArgumentsIncludeHumanConfirmation(ToolName, ArgsForPolicy, ConfirmationError))
		{
			ToolResult = ErrorJson(ConfirmationError);
		}
	}

	if (ToolResult.IsEmpty())
	{
		if (IsInGameThread())
		{
			ToolResult = DispatchTool(ToolName, ArgsJson, bTrustedToolAccess);
		}
		else
		{
			// Marshal to the game thread. If the task has not started before the deadline,
			// cancel it; once it has started, keep waiting so the client never sees a timeout
			// while a mutating tool continues behind its back.
			static constexpr double ToolDispatchTimeoutSeconds = 60.0;

			const FString ToolNameCopy = ToolName;
			const FString ArgsCopy = ArgsJson;
			const bool bTrustedToolAccessCopy = bTrustedToolAccess;
			TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
			TFuture<FString> Future = Promise->GetFuture();
			TSharedRef<FThreadSafeBool, ESPMode::ThreadSafe> bDispatchStillWanted = MakeShared<FThreadSafeBool, ESPMode::ThreadSafe>(true);
			TSharedRef<FThreadSafeBool, ESPMode::ThreadSafe> bDispatchStarted = MakeShared<FThreadSafeBool, ESPMode::ThreadSafe>(false);

			AsyncTask(ENamedThreads::GameThread, [Promise, ToolNameCopy, ArgsCopy, bTrustedToolAccessCopy, bDispatchStillWanted, bDispatchStarted]()
			{
				if (!static_cast<bool>(*bDispatchStillWanted))
				{
					Promise->SetValue(ErrorJson(FString::Printf(
						TEXT("Tool '%s' was cancelled before dispatch because the editor game thread did not start it before the timeout."),
						*ToolNameCopy)));
					return;
				}
				*bDispatchStarted = true;
				Promise->SetValue(DispatchTool(ToolNameCopy, ArgsCopy, bTrustedToolAccessCopy));
			});

			const double DispatchDeadlineSeconds = FPlatformTime::Seconds() + ToolDispatchTimeoutSeconds;
			while (!Future.WaitFor(FTimespan::FromMilliseconds(50)))
			{
				if (FPlatformTime::Seconds() >= DispatchDeadlineSeconds && !static_cast<bool>(*bDispatchStarted))
				{
					*bDispatchStillWanted = false;
					break;
				}
			}

			if (static_cast<bool>(*bDispatchStillWanted) || static_cast<bool>(*bDispatchStarted))
			{
				ToolResult = Future.Get();
			}
			else
			{
				ToolResult = ErrorJson(FString::Printf(
					TEXT("Tool '%s' timed out after %.0f seconds waiting for the editor game thread."),
					*ToolName, ToolDispatchTimeoutSeconds));
			}
		}
	}

	// Surface tool-level failures via the MCP `isError` flag so clients can react,
	// while still returning the structured payload as text content.
	bool bIsError = false;
	FString TextPayload = ToolResult;
	TSharedPtr<FJsonObject> ImageContent;
	// Parsed form of the tool's JSON payload. Kept in scope so it can be echoed back as
	// MCP `structuredContent` (machine-readable result) in addition to the text block.
	TSharedPtr<FJsonObject> StructuredResult = ParseJsonObject(ToolResult);
	if (StructuredResult.IsValid())
	{
		bool bSuccess = true;
		if (StructuredResult->TryGetBoolField(TEXT("success"), bSuccess))
		{
			bIsError = !bSuccess;
		}

		// A tool can attach an inline image via "_imageContent": {mimeType, data(base64)}.
		// Lift it into a proper MCP image content item and drop it from the text payload so
		// the (large) base64 isn't also duplicated as text (or in structuredContent).
		const TSharedPtr<FJsonObject>* ImagePtr = nullptr;
		if (StructuredResult->TryGetObjectField(TEXT("_imageContent"), ImagePtr) && ImagePtr && ImagePtr->IsValid())
		{
			ImageContent = *ImagePtr;
			StructuredResult->RemoveField(TEXT("_imageContent"));
			TextPayload = JsonObjectToString(StructuredResult.ToSharedRef());
		}
	}

	TArray<TSharedPtr<FJsonValue>> Content;
	TSharedRef<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), TextPayload);
	Content.Add(MakeShared<FJsonValueObject>(TextContent));

	if (ImageContent.IsValid())
	{
		FString MimeType;
		FString Data;
		ImageContent->TryGetStringField(TEXT("mimeType"), MimeType);
		ImageContent->TryGetStringField(TEXT("data"), Data);
		if (!Data.IsEmpty())
		{
			TSharedRef<FJsonObject> ImageItem = MakeShared<FJsonObject>();
			ImageItem->SetStringField(TEXT("type"), TEXT("image"));
			ImageItem->SetStringField(TEXT("data"), Data);
			ImageItem->SetStringField(TEXT("mimeType"), MimeType.IsEmpty() ? TEXT("image/png") : MimeType);
			Content.Add(MakeShared<FJsonValueObject>(ImageItem));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("content"), Content);
	Result->SetBoolField(TEXT("isError"), bIsError);

	// MCP 2025-06-18+: when the tool returns a JSON object, also surface it as
	// `structuredContent` so clients get a machine-readable result instead of
	// having to re-parse the text block. The text block stays for back-compat.
	if (StructuredResult.IsValid())
	{
		Result->SetObjectField(TEXT("structuredContent"), StructuredResult);
	}

	return Result;
}

TSharedPtr<FJsonObject> FWorldDataMCPServer::HandleResourcesList(const TSharedPtr<FJsonObject>& Params, FString* OutError)
{
	TSharedPtr<FJsonObject> Parsed = ParseJsonObject(GetResourceListJson());
	if (!Parsed.IsValid())
	{
		return MakeShared<FJsonObject>();
	}

	const TArray<TSharedPtr<FJsonValue>>* AllResources = nullptr;
	if (!Parsed->TryGetArrayField(TEXT("resources"), AllResources) || AllResources == nullptr)
	{
		return Parsed;
	}

	FWorldDataPaginationResult Pagination;
	FString PaginationError;
	if (!ApplyPagination(*AllResources, Params, Pagination, PaginationError))
	{
		if (OutError)
		{
			*OutError = PaginationError;
		}
		return MakeShared<FJsonObject>();
	}

	Parsed->SetArrayField(TEXT("resources"), MoveTemp(Pagination.Items));
	if (Pagination.NextCursor.IsSet())
	{
		Parsed->SetStringField(TEXT("nextCursor"), Pagination.NextCursor.GetValue());
	}
	return Parsed;
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
