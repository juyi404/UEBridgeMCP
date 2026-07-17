#include "WorldDataMCPServer.h"

#include "Algo/AllOf.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/Event.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/ThreadSafeBool.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Misc/Crc.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <atomic>

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <wincrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#include "WorldDataMCPCommon.h"
#include "WorldDataMCPResponseContext.h"
#include "WorldDataMCPToolGovernance.h"
#include "WorldDataMCPToolRegistry.h"

using namespace WorldDataMCP;

DEFINE_LOG_CATEGORY_STATIC(LogWorldDataMCP, Log, All);

TSharedPtr<IHttpRouter> FWorldDataMCPServer::HttpRouter = nullptr;
TArray<FHttpRouteHandle> FWorldDataMCPServer::RouteHandles;
int32 FWorldDataMCPServer::BoundPort = 0;
bool FWorldDataMCPServer::bRunning = false;
TMap<FString, FWorldDataMCPSessionState> FWorldDataMCPServer::SessionProtocolVersions;
FString FWorldDataMCPServer::AccessToken;
FString FWorldDataMCPServer::UnsafePythonCapabilityToken;
FDateTime FWorldDataMCPServer::UnsafePythonCapabilityExpiresAtUtc;
FString FWorldDataMCPServer::LastError;

static FString DispatchAuthorizedTool(const FString& ToolName, const TSharedPtr<FJsonObject>& Args);

namespace
{
	static FCriticalSection GMcpStateMutex;
	static FCriticalSection GMcpContextMutex;
	static FString GLastKnownWorldRevision = TEXT("unavailable");

	// Newest first. The server advertises the newest version it supports but will
	// echo back a client's requested version when that version is also supported,
	// per the MCP initialize negotiation rules.
	static const TCHAR* const GSupportedProtocolVersions[] = {
		TEXT("2025-06-18"),
		TEXT("2025-03-26"),
		TEXT("2024-11-05")
	};

	static constexpr int32 GMaxMcpRequestBodyBytes = 1024 * 1024;
	static constexpr int32 GAccessTokenConfigVersion = 3;
	static constexpr int32 GUnsafePythonCapabilityLifetimeMinutes = 10;

	// Multiple CLI clients (Codex, Cursor, Claude Code) may hold sessions against the
	// same editor concurrently. Keep a bounded set so a new initialize does not evict
	// every other client's session.
	static constexpr int32 GMaxConcurrentMcpSessions = 16;
	static constexpr int32 GMcpSessionLifetimeMinutes = 30;
	static constexpr int32 GMaxPendingGameThreadToolDispatches = 32;
	static constexpr int32 GMaxRetainedToolJobs = 128;
	static constexpr int32 GToolJobLifetimeMinutes = 30;
	static constexpr int32 GApprovalLifetimeMinutes = 10;
	static constexpr int32 GMaxPendingToolApprovals = 32;
	static std::atomic<int32> GPendingGameThreadToolDispatches{ 0 };
	static FCriticalSection GMcpJobMutex;

	static bool VerifyLoopbackOnlyListener(const int32 Port, FString& OutError)
	{
		OutError.Empty();
#if PLATFORM_WINDOWS
		const DWORD ProcessId = FPlatformProcess::GetCurrentProcessId();
		bool bFoundListener = false;

		auto InspectIpv4 = [&]() -> bool
		{
			DWORD BufferSize = 0;
			DWORD Result = GetExtendedTcpTable(nullptr, &BufferSize, 1, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
			if (Result != ERROR_INSUFFICIENT_BUFFER)
			{
				OutError = FString::Printf(TEXT("GetExtendedTcpTable(AF_INET) failed with %lu."), Result);
				return false;
			}

			TArray<uint8> Buffer;
			Buffer.SetNumUninitialized(BufferSize);
			Result = GetExtendedTcpTable(Buffer.GetData(), &BufferSize, 1, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
			if (Result != NO_ERROR)
			{
				OutError = FString::Printf(TEXT("GetExtendedTcpTable(AF_INET) failed with %lu."), Result);
				return false;
			}

			const MIB_TCPTABLE_OWNER_PID* Table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(Buffer.GetData());
			for (DWORD Index = 0; Index < Table->dwNumEntries; ++Index)
			{
				const MIB_TCPROW_OWNER_PID& Row = Table->table[Index];
				if (Row.dwOwningPid != ProcessId || ntohs(static_cast<u_short>(Row.dwLocalPort)) != Port)
				{
					continue;
				}

				bFoundListener = true;
				if (Row.dwLocalAddr != htonl(INADDR_LOOPBACK))
				{
					OutError = FString::Printf(TEXT("MCP listener on port %d is not IPv4 loopback (address=0x%08x)."), Port, Row.dwLocalAddr);
					return false;
				}
			}
			return true;
		};

		auto InspectIpv6 = [&]() -> bool
		{
			DWORD BufferSize = 0;
			DWORD Result = GetExtendedTcpTable(nullptr, &BufferSize, 1, AF_INET6, TCP_TABLE_OWNER_PID_LISTENER, 0);
			if (Result == ERROR_NO_DATA)
			{
				return true;
			}
			if (Result != ERROR_INSUFFICIENT_BUFFER)
			{
				OutError = FString::Printf(TEXT("GetExtendedTcpTable(AF_INET6) failed with %lu."), Result);
				return false;
			}

			TArray<uint8> Buffer;
			Buffer.SetNumUninitialized(BufferSize);
			Result = GetExtendedTcpTable(Buffer.GetData(), &BufferSize, 1, AF_INET6, TCP_TABLE_OWNER_PID_LISTENER, 0);
			if (Result != NO_ERROR)
			{
				OutError = FString::Printf(TEXT("GetExtendedTcpTable(AF_INET6) failed with %lu."), Result);
				return false;
			}

			const MIB_TCP6TABLE_OWNER_PID* Table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(Buffer.GetData());
			for (DWORD Index = 0; Index < Table->dwNumEntries; ++Index)
			{
				const MIB_TCP6ROW_OWNER_PID& Row = Table->table[Index];
				if (Row.dwOwningPid != ProcessId || ntohs(static_cast<u_short>(Row.dwLocalPort)) != Port)
				{
					continue;
				}

				bFoundListener = true;
				bool bIsLoopback = true;
				for (int32 ByteIndex = 0; ByteIndex < 15; ++ByteIndex)
				{
					bIsLoopback &= Row.ucLocalAddr[ByteIndex] == 0;
				}
				bIsLoopback &= Row.ucLocalAddr[15] == 1;
				if (!bIsLoopback)
				{
					OutError = FString::Printf(TEXT("MCP listener on port %d is not IPv6 loopback."), Port);
					return false;
				}
			}
			return true;
		};

		if (!InspectIpv4() || !InspectIpv6())
		{
			return false;
		}
		if (!bFoundListener)
		{
			OutError = FString::Printf(TEXT("No TCP listener owned by this editor process was found on MCP port %d."), Port);
			return false;
		}
		return true;
#else
		OutError = TEXT("This platform does not provide a verified loopback listener implementation. MCP refuses to start fail-closed.");
		return false;
#endif
	}

	static bool ProtectTokenForCurrentUser(const FString& Token, FString& OutProtectedToken)
	{
		OutProtectedToken.Empty();
#if PLATFORM_WINDOWS
		const FTCHARToUTF8 Utf8(*Token);
		DATA_BLOB Input{};
		Input.cbData = static_cast<DWORD>(Utf8.Length());
		Input.pbData = reinterpret_cast<BYTE*>(const_cast<ANSICHAR*>(Utf8.Get()));
		DATA_BLOB Output{};
		if (!CryptProtectData(&Input, L"UEBridgeMCP access token", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Output))
		{
			return false;
		}

		TArray<uint8> Bytes;
		Bytes.Append(Output.pbData, static_cast<int32>(Output.cbData));
		LocalFree(Output.pbData);
		OutProtectedToken = FBase64::Encode(Bytes);
		return !OutProtectedToken.IsEmpty();
#else
		return false;
#endif
	}

	static bool UnprotectTokenForCurrentUser(const FString& ProtectedToken, FString& OutToken)
	{
		OutToken.Empty();
#if PLATFORM_WINDOWS
		TArray<uint8> Bytes;
		if (!FBase64::Decode(ProtectedToken, Bytes) || Bytes.Num() == 0)
		{
			return false;
		}

		DATA_BLOB Input{};
		Input.cbData = static_cast<DWORD>(Bytes.Num());
		Input.pbData = Bytes.GetData();
		DATA_BLOB Output{};
		if (!CryptUnprotectData(&Input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Output))
		{
			return false;
		}

		const FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Output.pbData), static_cast<int32>(Output.cbData));
		OutToken = FString(Utf8.Length(), Utf8.Get());
		LocalFree(Output.pbData);
		return !OutToken.IsEmpty();
#else
		return false;
#endif
	}

	// The HTTP worker may time out while the editor thread is about to claim a
	// tool request. A boolean cancellation flag is not enough: the game thread
	// can observe "false", then the worker can time out, and the mutation still
	// starts. This small state machine gives exactly one side the right to claim
	// the queued request.
	enum class EToolDispatchState : int32
	{
		Queued,
		Executing,
		CancelledBeforeDispatch,
		Completed
	};

	enum class EToolJobState : int32
	{
		PreparingApproval,
		AwaitingApproval,
		Queued,
		Executing,
		Completed,
		Cancelled
	};

	struct FToolDispatchRequest
	{
		FToolDispatchRequest()
			: State(static_cast<int32>(EToolDispatchState::Queued))
			, RequestId(FGuid::NewGuid().ToString(EGuidFormats::Digits))
		{
		}

		std::atomic<int32> State;
		FString RequestId;
	};

	struct FToolJob
	{
		FToolJob(const FString& InToolName, const FString& InOwnerSessionId)
			: Id(FGuid::NewGuid().ToString(EGuidFormats::Digits))
			, ToolName(InToolName)
			, OwnerSessionId(InOwnerSessionId)
			, CreatedAtUtc(FDateTime::UtcNow())
			, State(static_cast<int32>(EToolJobState::Queued))
		{
		}

		FString Id;
		FString ToolName;
		FString OwnerSessionId;
		FString ApprovalId;
		FDateTime CreatedAtUtc;
		FDateTime CompletedAtUtc;
		std::atomic<int32> State;
		FCriticalSection ResultMutex;
		FString ResultJson;
	};

	static TMap<FString, TSharedPtr<FToolJob, ESPMode::ThreadSafe>> GToolJobs;

	enum class EApprovalState : int32
	{
		Preparing,
		AwaitingDecision,
		Resolved
	};

	struct FPendingToolApproval
	{
		FPendingToolApproval(
			const WorldDataMCP::ToolGovernance::FInvocation& InInvocation,
			const FString& InArgsJson,
			const TSharedRef<FToolJob, ESPMode::ThreadSafe>& InJob)
			: ApprovalId(FGuid::NewGuid().ToString(EGuidFormats::Digits))
			, Invocation(InInvocation)
			, ArgsJson(InArgsJson)
			, Job(InJob)
			, CreatedAtUtc(FDateTime::UtcNow())
			, ExpiresAtUtc(CreatedAtUtc + FTimespan::FromMinutes(GApprovalLifetimeMinutes))
			, State(static_cast<int32>(EApprovalState::Preparing))
		{
		}

		FString ApprovalId;
		WorldDataMCP::ToolGovernance::FInvocation Invocation;
		FString ArgsJson;
		TSharedRef<FToolJob, ESPMode::ThreadSafe> Job;
		FString TargetSummary;
		FString ChangeSummaryHash;
		FString TargetRevision;
		FDateTime CreatedAtUtc;
		FDateTime ExpiresAtUtc;
		std::atomic<int32> State;
	};

	static FCriticalSection GMcpApprovalMutex;
	static TMap<FString, TSharedPtr<FPendingToolApproval, ESPMode::ThreadSafe>> GPendingToolApprovals;

	static FString GetToolJobStateName(const int32 State)
	{
		switch (static_cast<EToolJobState>(State))
		{
		case EToolJobState::PreparingApproval: return TEXT("preparing_approval");
		case EToolJobState::AwaitingApproval: return TEXT("awaiting_approval");
		case EToolJobState::Queued: return TEXT("queued");
		case EToolJobState::Executing: return TEXT("executing");
		case EToolJobState::Completed: return TEXT("completed");
		case EToolJobState::Cancelled: return TEXT("cancelled");
		default: return TEXT("unknown");
		}
	}

	static FString MakeNonSecretHash(const FString& Value)
	{
		return FMD5::HashAnsiString(*Value).ToLower();
	}

	static FString CaptureWorldRevision()
	{
		FString Revision = WorldDataMCP::FContextRegistry::Get().CaptureWorldRevision();
		if (Revision.IsEmpty())
		{
			Revision = TEXT("unavailable");
		}
		{
			FScopeLock Lock(&GMcpContextMutex);
			GLastKnownWorldRevision = Revision;
		}
		return Revision;
	}

	static FString GetCachedWorldRevision()
	{
		FScopeLock Lock(&GMcpContextMutex);
		return GLastKnownWorldRevision;
	}

	static FString MakeApprovalTargetSummary(const TSharedPtr<FJsonObject>& Arguments)
	{
		if (!Arguments.IsValid())
		{
			return TEXT("target not provided");
		}

		static const TCHAR* const TargetFields[] = {
			TEXT("name"), TEXT("label"), TEXT("assetPath"), TEXT("path"), TEXT("file_path"),
			TEXT("old_path"), TEXT("new_path"), TEXT("child_name"), TEXT("parent_name")
		};
		TArray<FString> Parts;
		for (const TCHAR* Field : TargetFields)
		{
			FString Value;
			if (Arguments->TryGetStringField(Field, Value) && !Value.IsEmpty())
			{
				Parts.Add(FString::Printf(TEXT("%s=%s"), Field, *Value.Left(160)));
			}
		}
		return Parts.IsEmpty() ? TEXT("target not provided") : FString::Join(Parts, TEXT(", "));
	}

	static FString CaptureApprovalTargetRevision(const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments)
	{
		FString Revision = WorldDataMCP::FContextRegistry::Get().CaptureTargetRevision(ToolName, Arguments);
		if (!Revision.IsEmpty())
		{
			return Revision;
		}
		return MakeNonSecretHash(MakeApprovalTargetSummary(Arguments) + TEXT("|") + GetCachedWorldRevision());
	}

	static void CompleteToolJob(const TSharedRef<FToolJob, ESPMode::ThreadSafe>& Job, const FString& ResultJson)
	{
		{
			FScopeLock ResultLock(&Job->ResultMutex);
			Job->ResultJson = ResultJson;
			Job->CompletedAtUtc = FDateTime::UtcNow();
		}
		Job->State.store(static_cast<int32>(EToolJobState::Completed));
	}


	static void PruneExpiredToolJobsLocked(const FDateTime& Now)
	{
		for (auto It = GToolJobs.CreateIterator(); It; ++It)
		{
			const TSharedPtr<FToolJob, ESPMode::ThreadSafe>& Job = It.Value();
			if (!Job.IsValid())
			{
				It.RemoveCurrent();
				continue;
			}
			const int32 State = Job->State.load();
			if (State == static_cast<int32>(EToolJobState::Completed)
				|| State == static_cast<int32>(EToolJobState::Cancelled))
			{
				if (Job->CompletedAtUtc.GetTicks() > 0 && Now - Job->CompletedAtUtc > FTimespan::FromMinutes(GToolJobLifetimeMinutes))
				{
					It.RemoveCurrent();
				}
			}
		}

		while (GToolJobs.Num() > GMaxRetainedToolJobs)
		{
			FString OldestCompletedJobId;
			FDateTime OldestCompletedAtUtc = FDateTime::MaxValue();
			for (const TPair<FString, TSharedPtr<FToolJob, ESPMode::ThreadSafe>>& Pair : GToolJobs)
			{
				if (Pair.Value.IsValid()
					&& Pair.Value->State.load() == static_cast<int32>(EToolJobState::Completed)
					&& Pair.Value->CompletedAtUtc < OldestCompletedAtUtc)
				{
					OldestCompletedAtUtc = Pair.Value->CompletedAtUtc;
					OldestCompletedJobId = Pair.Key;
				}
			}
			if (OldestCompletedJobId.IsEmpty())
			{
				break;
			}
			GToolJobs.Remove(OldestCompletedJobId);
		}
	}

	static FString GetToolJobStatusJson(const FString& JobId, const FString& CallerSessionId)
	{
		TSharedPtr<FToolJob, ESPMode::ThreadSafe> Job;
		{
			FScopeLock Lock(&GMcpJobMutex);
			PruneExpiredToolJobsLocked(FDateTime::UtcNow());
			if (const TSharedPtr<FToolJob, ESPMode::ThreadSafe>* Found = GToolJobs.Find(JobId))
			{
				Job = *Found;
			}
		}
		if (!Job.IsValid())
		{
			return ErrorJson(TEXT("Unknown, expired, or evicted MCP job."));
		}
		if (!Job->OwnerSessionId.IsEmpty() && Job->OwnerSessionId != CallerSessionId)
		{
			return ErrorJson(TEXT("The MCP job belongs to a different session. Recreate the task in the current session."));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("jobId"), Job->Id);
		Result->SetStringField(TEXT("tool"), Job->ToolName);
		Result->SetStringField(TEXT("state"), GetToolJobStateName(Job->State.load()));
		if (!Job->ApprovalId.IsEmpty())
		{
			Result->SetStringField(TEXT("approvalId"), Job->ApprovalId);
		}
		Result->SetStringField(TEXT("createdAtUtc"), Job->CreatedAtUtc.ToIso8601());
		if (Job->CompletedAtUtc.GetTicks() > 0)
		{
			Result->SetStringField(TEXT("completedAtUtc"), Job->CompletedAtUtc.ToIso8601());
		}
		{
			FScopeLock ResultLock(&Job->ResultMutex);
			if (!Job->ResultJson.IsEmpty())
			{
				Result->SetStringField(TEXT("resultJson"), Job->ResultJson);
			}
		}
		return JsonObjectToString(Result);
	}

	static FString GetToolDispatchStateName(const int32 State)
	{
		switch (static_cast<EToolDispatchState>(State))
		{
		case EToolDispatchState::Queued: return TEXT("queued");
		case EToolDispatchState::Executing: return TEXT("executing");
		case EToolDispatchState::CancelledBeforeDispatch: return TEXT("cancelled_before_dispatch");
		case EToolDispatchState::Completed: return TEXT("completed");
		default: return TEXT("unknown");
		}
	}

	static FString GetPreferredProtocolVersion()
	{
		return GSupportedProtocolVersions[0];
	}

	static bool GetProjectSecurityFlag(const TCHAR* Key)
	{
		bool bEnabled = false;
		if (GConfig)
		{
			GConfig->GetBool(TEXT("UEBridgeMCP.Security"), Key, bEnabled, GGameIni);
		}
		return bEnabled;
	}

	static bool IsProjectFileTool(const FString& ToolName)
	{
		return ToolName == TEXT("read_file")
			|| ToolName == TEXT("write_file")
			|| ToolName == TEXT("delete_file")
			|| ToolName == TEXT("rename_file");
	}

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
			if (bGeneratedByThisPlugin)
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

	static FString AddContextEnvelopeToResultJson(
		const FString& ResultJson,
		const WorldDataMCP::ToolGovernance::FCallerContext& Caller,
		const FString& ToolName,
		const FString& ObjectRevision = FString())
	{
		const TSharedPtr<FJsonObject> Result = ParseObject(ResultJson);
		if (!Result.IsValid())
		{
			return ResultJson;
		}

		TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
		FString PreservedObjectRevision = ObjectRevision;
		const TSharedPtr<FJsonObject>* ExistingEnvelope = nullptr;
		if (PreservedObjectRevision.IsEmpty()
			&& Result->TryGetObjectField(TEXT("contextEnvelope"), ExistingEnvelope)
			&& ExistingEnvelope && ExistingEnvelope->IsValid())
		{
			(*ExistingEnvelope)->TryGetStringField(TEXT("objectRevision"), PreservedObjectRevision);
		}
		Envelope->SetStringField(TEXT("schemaVersion"), TEXT("1.0"));
		Envelope->SetStringField(TEXT("taskId"), Caller.TaskId);
		Envelope->SetStringField(TEXT("runId"), Caller.RunId);
		Envelope->SetStringField(TEXT("threadId"), Caller.ThreadId);
		Envelope->SetStringField(TEXT("mcpSessionId"), Caller.SessionId);
		Envelope->SetStringField(TEXT("transactionId"), Caller.TransactionId);
		Envelope->SetStringField(TEXT("worldRevision"), GetCachedWorldRevision());
		Envelope->SetStringField(TEXT("objectRevision"), PreservedObjectRevision);
		Envelope->SetStringField(TEXT("tool"), ToolName);
		if (Caller.ExpectedWorldRevision.Len() > 0)
		{
			Envelope->SetStringField(TEXT("expectedWorldRevision"), Caller.ExpectedWorldRevision);
		}
		if (Caller.ExpectedObjectRevision.Len() > 0)
		{
			Envelope->SetStringField(TEXT("expectedObjectRevision"), Caller.ExpectedObjectRevision);
		}
		FString Id;
		if (Result->TryGetStringField(TEXT("approvalId"), Id))
		{
			Envelope->SetStringField(TEXT("approvalId"), Id);
		}
		Result->SetObjectField(TEXT("contextEnvelope"), Envelope);
		return JsonObjectToString(Result.ToSharedRef());
	}

	static WorldDataMCP::ToolGovernance::FCallerContext ExtractCallerContext(const TSharedPtr<FJsonObject>& Arguments)
	{
		WorldDataMCP::ToolGovernance::FCallerContext Caller;
		const TSharedPtr<FJsonObject>* CallerObject = nullptr;
		if (Arguments.IsValid()
			&& Arguments->TryGetObjectField(TEXT("__worlddataCaller"), CallerObject)
			&& CallerObject && CallerObject->IsValid())
		{
			(*CallerObject)->TryGetStringField(TEXT("sessionId"), Caller.SessionId);
			(*CallerObject)->TryGetStringField(TEXT("principal"), Caller.Principal);
			(*CallerObject)->TryGetStringField(TEXT("clientLabel"), Caller.ClientLabel);
			(*CallerObject)->TryGetStringField(TEXT("clientVersion"), Caller.ClientVersion);
			(*CallerObject)->TryGetStringField(TEXT("scope"), Caller.Scope);
			(*CallerObject)->TryGetStringField(TEXT("taskId"), Caller.TaskId);
			(*CallerObject)->TryGetStringField(TEXT("threadId"), Caller.ThreadId);
			(*CallerObject)->TryGetStringField(TEXT("runId"), Caller.RunId);
			(*CallerObject)->TryGetStringField(TEXT("transactionId"), Caller.TransactionId);
			(*CallerObject)->TryGetStringField(TEXT("expectedWorldRevision"), Caller.ExpectedWorldRevision);
			(*CallerObject)->TryGetStringField(TEXT("expectedObjectRevision"), Caller.ExpectedObjectRevision);
		}
		return Caller;
	}

	static FString GetToolCapabilityError(const FString& ToolName)
	{
		if (IsProjectFileTool(ToolName) && !GetProjectSecurityFlag(TEXT("bEnableProjectFileTools")))
		{
			return TEXT("Project file tools are disabled by default. Set [UEBridgeMCP.Security] bEnableProjectFileTools=true only for a trusted local automation session.");
		}
		if (ToolName == TEXT("set_actor_property") && !GetProjectSecurityFlag(TEXT("bEnableReflectedPropertyWrites")))
		{
			return TEXT("Generic reflected property writes are disabled by default. Prefer dedicated structured tools, or explicitly enable [UEBridgeMCP.Security] bEnableReflectedPropertyWrites=true for a trusted local automation session.");
		}
		if (ToolName == TEXT("execute_python") && !FWorldDataMCPServer::IsUnsafePythonEnabled())
		{
			return TEXT("execute_python is disabled by [UEBridgeMCP.Security] bEnableUnsafePython.");
		}
		return FString();
	}

	static FString QueueToolApproval(const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments, const FString& ArgsJson)
	{
		const WorldDataMCP::ToolGovernance::FCallerContext Caller = ExtractCallerContext(Arguments);
		const TSharedPtr<FJsonObject> ExecutionArguments = ParseObject(ArgsJson);
		if (!ExecutionArguments.IsValid())
		{
			return ErrorJson(TEXT("Invalid arguments JSON for approval."));
		}
		ExecutionArguments->RemoveField(TEXT("__worlddataCaller"));
		const FString ExecutionArgsJson = JsonObjectToString(ExecutionArguments.ToSharedRef());
		const WorldDataMCP::ToolGovernance::FInvocation Invocation =
			WorldDataMCP::ToolGovernance::BeginInvocation(ToolName, Arguments, Caller);
		const FString CapabilityError = GetToolCapabilityError(ToolName);
		if (!CapabilityError.IsEmpty())
		{
			const FString Result = ErrorJson(CapabilityError);
			WorldDataMCP::ToolGovernance::CompleteInvocation(Invocation, Result);
			return Result;
		}

		const TSharedRef<FToolJob, ESPMode::ThreadSafe> Job = MakeShared<FToolJob, ESPMode::ThreadSafe>(ToolName, Caller.SessionId);
		Job->State.store(static_cast<int32>(EToolJobState::PreparingApproval));
		const TSharedRef<FPendingToolApproval, ESPMode::ThreadSafe> Approval =
			MakeShared<FPendingToolApproval, ESPMode::ThreadSafe>(Invocation, ExecutionArgsJson, Job);
		Approval->TargetSummary = MakeApprovalTargetSummary(Arguments);
		Approval->ChangeSummaryHash = MakeNonSecretHash(ExecutionArgsJson);
		Job->ApprovalId = Approval->ApprovalId;
		Approval->Invocation.ApprovalId = Approval->ApprovalId;
		Approval->Invocation.ChangeSummaryHash = Approval->ChangeSummaryHash;

		{
			FScopeLock Lock(&GMcpApprovalMutex);
			if (GPendingToolApprovals.Num() >= GMaxPendingToolApprovals)
			{
				const FString Result = ErrorJson(FString::Printf(TEXT("Editor approval queue is full (%d requests). Retry with backoff."), GMaxPendingToolApprovals));
				WorldDataMCP::ToolGovernance::CompleteInvocation(Invocation, Result);
				return Result;
			}
			GPendingToolApprovals.Add(Approval->ApprovalId, Approval);
		}
		{
			FScopeLock Lock(&GMcpJobMutex);
			PruneExpiredToolJobsLocked(FDateTime::UtcNow());
			GToolJobs.Add(Job->Id, Job);
		}

		WorldDataMCP::ToolGovernance::RecordApprovalEvent(
			Invocation,
			TEXT("awaiting_approval"),
			FString::Printf(TEXT("approvalId=%s expiresAtUtc=%s targetRevision=pending"), *Approval->ApprovalId, *Approval->ExpiresAtUtc.ToIso8601()));

		AsyncTask(ENamedThreads::GameThread, [Approval]()
		{
			const TSharedPtr<FJsonObject> ApprovalArgs = ParseObject(Approval->ArgsJson);
			const FString WorldRevision = CaptureWorldRevision();
			const FString Revision = ApprovalArgs.IsValid()
				? CaptureApprovalTargetRevision(Approval->Invocation.ToolName, ApprovalArgs)
				: FString();
			Approval->Invocation.WorldRevision = WorldRevision;
			if (!ApprovalArgs.IsValid() || Revision.IsEmpty() || WorldRevision == TEXT("unavailable"))
			{
				const FString Result = ErrorJson(TEXT("Unable to capture the editor target revision for approval."));
				CompleteToolJob(Approval->Job, Result);
				WorldDataMCP::ToolGovernance::RecordApprovalEvent(Approval->Invocation, TEXT("approval_capture_failed"), TEXT("target revision capture failed"));
				FScopeLock Lock(&GMcpApprovalMutex);
				GPendingToolApprovals.Remove(Approval->ApprovalId);
				return;
			}
			if (Approval->Invocation.Caller.ExpectedWorldRevision != WorldRevision
				|| Approval->Invocation.Caller.ExpectedObjectRevision != Revision)
			{
				const FString Result = ErrorJson(TEXT("The MCP context is stale: expected world/object revision does not match the current editor target. Read fresh context and create a new change request."));
				CompleteToolJob(Approval->Job, Result);
				WorldDataMCP::ToolGovernance::RecordApprovalEvent(Approval->Invocation, TEXT("context_rejected"), TEXT("expected world or object revision did not match"));
				FScopeLock Lock(&GMcpApprovalMutex);
				GPendingToolApprovals.Remove(Approval->ApprovalId);
				return;
			}

			Approval->TargetRevision = Revision;
			Approval->Invocation.TargetRevision = Revision;
			Approval->State.store(static_cast<int32>(EApprovalState::AwaitingDecision));
			Approval->Job->State.store(static_cast<int32>(EToolJobState::AwaitingApproval));
			WorldDataMCP::ToolGovernance::RecordApprovalEvent(Approval->Invocation, TEXT("approval_ready"), TEXT("target revision captured"));
		});

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetBoolField(TEXT("awaitingApproval"), true);
		Result->SetStringField(TEXT("approvalId"), Approval->ApprovalId);
		Result->SetStringField(TEXT("jobId"), Job->Id);
		Result->SetStringField(TEXT("state"), TEXT("preparing_approval"));
		Result->SetStringField(TEXT("pollTool"), TEXT("get_mcp_job_status"));
		Result->SetStringField(TEXT("expiresAtUtc"), Approval->ExpiresAtUtc.ToIso8601());
		return JsonObjectToString(Result);
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
			TEXT("type")
		};
		return SafeKeys.Contains(Key);
	}

	static TSharedPtr<FJsonObject> MakeCodexPolicySnapshotObject()
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);

		const FString ConfigPath = GetCodexConfigPath();
		Result->SetBoolField(TEXT("exists"), !ConfigPath.IsEmpty() && FPaths::FileExists(ConfigPath));
		Result->SetStringField(TEXT("source"), TEXT("Codex user configuration"));

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

static FString DispatchRegisteredTool(const FString& ToolName, const TSharedPtr<FJsonObject>& Args)
{
	FString Result;
	if (WorldDataMCP::FToolRegistry::Get().Dispatch(ToolName, Args, Result))
	{
		return Result;
	}
	return ErrorJson(FString::Printf(TEXT("No registered UEBridgeMCP tool handler: %s"), *ToolName));
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
		SessionProtocolVersions.Reset();
		// Keep the token empty here; EnsureAccessToken() reloads only a current-version
		// token from Saved/UEBridgeMCP/config.json. A version bump rotates old tokens.
		AccessToken.Empty();
		UnsafePythonCapabilityToken = GenerateAccessToken();
		UnsafePythonCapabilityExpiresAtUtc = FDateTime::UtcNow() + FTimespan::FromMinutes(GUnsafePythonCapabilityLifetimeMinutes);
	}

	{
		FScopeLock JobLock(&GMcpJobMutex);
		GToolJobs.Reset();
	}
	{
		FScopeLock ApprovalLock(&GMcpApprovalMutex);
		GPendingToolApprovals.Reset();
	}
	{
		FScopeLock ContextLock(&GMcpContextMutex);
		GLastKnownWorldRevision = TEXT("unavailable");
	}
	WorldDataMCP::ResponseContext::ResetSnapshots();
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

	FString LoopbackVerificationError;
	if (!VerifyLoopbackOnlyListener(BoundPort, LoopbackVerificationError))
	{
		LastError = FString::Printf(TEXT("MCP server refused to start because its listener could not be verified as loopback-only: %s"), *LoopbackVerificationError);
		UE_LOG(LogWorldDataMCP, Error, TEXT("%s"), *LastError);
		for (const FHttpRouteHandle& RouteHandle : RouteHandles)
		{
			if (HttpRouter.IsValid() && RouteHandle.IsValid())
			{
				HttpRouter->UnbindRoute(RouteHandle);
			}
		}
		// The router has already started its TCP listener. Remove it as well so a
		// failed verification cannot leave a socket open on a non-loopback address.
		HttpModule.StopAllListeners();
		RouteHandles.Reset();
		HttpRouter.Reset();
		BoundPort = 0;
		return;
	}

	bRunning = true;
	StartedAtUtc = FDateTime::UtcNow();
	AsyncTask(ENamedThreads::GameThread, []()
	{
		CaptureWorldRevision();
	});
	RefreshConnectionFiles();

	UE_LOG(LogWorldDataMCP, Log, TEXT("WorldData MCP server '%s' listening at %s"), *GetServerName(), *GetMcpUrl());
}

void FWorldDataMCPServer::Stop()
{
	if (!bRunning)
	{
		{
			FScopeLock Lock(&GMcpStateMutex);
			SessionProtocolVersions.Reset();
			AccessToken.Empty();
			UnsafePythonCapabilityToken.Empty();
		UnsafePythonCapabilityExpiresAtUtc = FDateTime();
		}
		WorldDataMCP::ResponseContext::ResetSnapshots();
		{
			FScopeLock JobLock(&GMcpJobMutex);
			GToolJobs.Reset();
		}
		{
			FScopeLock ApprovalLock(&GMcpApprovalMutex);
			GPendingToolApprovals.Reset();
		}
		{
			FScopeLock ContextLock(&GMcpContextMutex);
			GLastKnownWorldRevision = TEXT("unavailable");
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
		SessionProtocolVersions.Reset();
		AccessToken.Empty();
		UnsafePythonCapabilityToken.Empty();
		UnsafePythonCapabilityExpiresAtUtc = FDateTime();
	}
	WorldDataMCP::ResponseContext::ResetSnapshots();
	{
		FScopeLock JobLock(&GMcpJobMutex);
		GToolJobs.Reset();
	}
	{
		FScopeLock ApprovalLock(&GMcpApprovalMutex);
		GPendingToolApprovals.Reset();
	}
	{
		FScopeLock ContextLock(&GMcpContextMutex);
		GLastKnownWorldRevision = TEXT("unavailable");
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
	// Version 2 and earlier stored a plaintext token. Never keep that legacy
	// secret at rest while waiting for the later startup refresh to overwrite it.
	if (Config->HasField(TEXT("accessToken")))
	{
		IFileManager::Get().Delete(*GetConfigPath(), false, true, true);
	}
	FString SavedProjectId;
	FString SavedProtectedToken;
	FString SavedToken;
	double SavedTokenVersion = 0.0;
	if (Config->TryGetStringField(TEXT("projectId"), SavedProjectId)
		&& SavedProjectId == GetProjectId()
		&& Config->TryGetNumberField(TEXT("accessTokenVersion"), SavedTokenVersion)
		&& static_cast<int32>(SavedTokenVersion) == GAccessTokenConfigVersion
		&& Config->TryGetStringField(TEXT("accessTokenProtected"), SavedProtectedToken)
		&& UnprotectTokenForCurrentUser(SavedProtectedToken, SavedToken)
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

bool FWorldDataMCPServer::RotateAccessToken(FString& OutError)
{
	OutError.Empty();
	if (!bRunning || BoundPort <= 0)
	{
		OutError = TEXT("The MCP server must be running before its access token can be rotated.");
		return false;
	}

	{
		FScopeLock Lock(&GMcpStateMutex);
		AccessToken = GenerateAccessToken();
		UnsafePythonCapabilityToken = GenerateAccessToken();
		UnsafePythonCapabilityExpiresAtUtc = FDateTime::UtcNow() + FTimespan::FromMinutes(GUnsafePythonCapabilityLifetimeMinutes);
		SessionProtocolVersions.Reset();
	}
	WorldDataMCP::ResponseContext::ResetSnapshots();

	// Persist only server-owned state. Writing client configuration is deliberately
	// left to the caller's separate, explicit provisioning action.
	RefreshConnectionFiles();
	if (!LastError.IsEmpty())
	{
		OutError = LastError;
		return false;
	}
	return true;
}

bool FWorldDataMCPServer::IsUnsafePythonEnabled()
{
	return GetProjectSecurityFlag(TEXT("bEnableUnsafePython"));
}

FString FWorldDataMCPServer::GetUnsafePythonCapabilityToken()
{
	if (!bRunning || !IsUnsafePythonEnabled())
	{
		return FString();
	}

	FScopeLock Lock(&GMcpStateMutex);
	const FDateTime Now = FDateTime::UtcNow();
	if (UnsafePythonCapabilityToken.IsEmpty() || UnsafePythonCapabilityExpiresAtUtc <= Now)
	{
		UnsafePythonCapabilityToken = GenerateAccessToken();
		UnsafePythonCapabilityExpiresAtUtc = Now + FTimespan::FromMinutes(GUnsafePythonCapabilityLifetimeMinutes);
	}
	return UnsafePythonCapabilityToken;
}

bool FWorldDataMCPServer::ValidateUnsafePythonCapability(const FString& Candidate)
{
	if (!bRunning || !IsUnsafePythonEnabled())
	{
		return false;
	}

	FScopeLock Lock(&GMcpStateMutex);
	return !UnsafePythonCapabilityToken.IsEmpty()
		&& UnsafePythonCapabilityExpiresAtUtc > FDateTime::UtcNow()
		&& ConstantTimeEquals(UnsafePythonCapabilityToken, Candidate);
}

FString FWorldDataMCPServer::GetProjectInfoJson()
{
	const FString ProtocolSnapshot = GetPreferredProtocolVersion();

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
	const FString ProtocolSnapshot = GetPreferredProtocolVersion();

	TSharedRef<FJsonObject> Status = MakeShared<FJsonObject>();
	Status->SetBoolField(TEXT("running"), bRunning);
	Status->SetStringField(TEXT("serverName"), GetServerName());
	Status->SetStringField(TEXT("projectId"), GetProjectId());
	Status->SetNumberField(TEXT("port"), BoundPort);
	Status->SetStringField(TEXT("url"), bRunning ? GetMcpUrl() : TEXT(""));
	Status->SetStringField(TEXT("currentWorldRevision"), GetCachedWorldRevision());
	Status->SetNumberField(TEXT("routeCount"), RouteHandles.Num());
	Status->SetNumberField(TEXT("pendingGameThreadToolDispatches"), GPendingGameThreadToolDispatches.load());
	Status->SetNumberField(TEXT("maxPendingGameThreadToolDispatches"), GMaxPendingGameThreadToolDispatches);
	{
		FScopeLock JobLock(&GMcpJobMutex);
		PruneExpiredToolJobsLocked(FDateTime::UtcNow());
		Status->SetNumberField(TEXT("retainedToolJobs"), GToolJobs.Num());
		Status->SetNumberField(TEXT("maxRetainedToolJobs"), GMaxRetainedToolJobs);
	}
	Status->SetNumberField(TEXT("pendingEditorApprovals"), GetPendingApprovals().Num());
	Status->SetNumberField(TEXT("maxPendingEditorApprovals"), GMaxPendingToolApprovals);
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

	LastError.Empty();
	SaveConfiguredPort(BoundPort);
	if (!LastError.IsEmpty())
	{
		// Do not publish a connection file when the server could not persist its
		// user-protected credential state. Existing clients can keep using the
		// in-memory token until this editor session ends, but a restart must rotate.
		return;
	}
	WriteProjectConnectionFile();
	LastRefreshAtUtc = FDateTime::UtcNow();
}

void FWorldDataMCPServer::ProvisionClientConfigurations()
{
	if (BoundPort <= 0 || !bRunning)
	{
		LastError = TEXT("Cannot provision client configuration because the MCP server is not running.");
		return;
	}

	// Client configuration contains the access token. Only create it after an
	// explicit action in the editor UI; never modify Claude approval settings.
	WriteClientConfig();
	WriteCodexClientConfig();
	LastRefreshAtUtc = FDateTime::UtcNow();
	LastError.Empty();
}

FString FWorldDataMCPServer::GetToolDefinitionsJson()
{
	const FString RegisteredDefinitions = WorldDataMCP::FToolRegistry::Get().GetRegisteredDefinitionsJson();
	const FString WithResponseControl = WorldDataMCP::ResponseContext::AddResponseControlToToolDefinitions(RegisteredDefinitions);
	return WorldDataMCP::ToolGovernance::AddGovernanceMetadataToToolDefinitions(WithResponseControl);
}

namespace
{
	void RegisterCoreResources()
	{
		auto& Resources = WorldDataMCP::FResourceRegistry::Get();
		Resources.RegisterResource(TEXT("worlddata://project/info"), TEXT("Project Info"), TEXT("Project identity, paths, MCP endpoint, and process details."), []
		{
			return FWorldDataMCPServer::GetProjectInfoJson();
		});
		Resources.RegisterResource(TEXT("worlddata://mcp/governance"), TEXT("MCP Governance"), TEXT("Tool risk matrix, interactive approval policy, and audit-log metadata."), []
		{
			return WorldDataMCP::ToolGovernance::GetPolicySnapshotJson();
		});
		Resources.RegisterResource(TEXT("worlddata://codex/policy-snapshot"), TEXT("Codex Policy Snapshot"), TEXT("Redacted local Codex config policy and MCP server configuration."), []
		{
			return GetCodexPolicySnapshotJson();
		});
	}
}

FString FWorldDataMCPServer::GetResourceListJson()
{
	RegisterCoreResources();
	return WorldDataMCP::FResourceRegistry::Get().GetResourceListJson(TEXT("worlddata://context/bootstrap"));
}

FString FWorldDataMCPServer::ReadResource(const FString& Uri)
{
	RegisterCoreResources();
	FString Result;
	if (WorldDataMCP::FResourceRegistry::Get().Read(Uri, Result))
	{
		return Result;
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
	FWorldDataMCPSessionState RequestSession;
	bool bHasRequestSession = false;
	if (Method != TEXT("initialize"))
	{
		if (ProvidedSessionId.IsEmpty())
		{
			OnComplete(MakeJsonResponse(400, TEXT("{\"error\":\"Missing MCP session. Initialize before sending further requests.\"}")));
			return true;
		}

		bool bKnownSession = false;
		{
			FScopeLock Lock(&GMcpStateMutex);
			const FDateTime Now = FDateTime::UtcNow();
			for (auto It = SessionProtocolVersions.CreateIterator(); It; ++It)
			{
				if (Now - It.Value().LastActivityAtUtc > FTimespan::FromMinutes(GMcpSessionLifetimeMinutes))
				{
					It.RemoveCurrent();
				}
			}
			if (FWorldDataMCPSessionState* Session = SessionProtocolVersions.Find(ProvidedSessionId))
			{
				Session->LastActivityAtUtc = Now;
				RequestSession = *Session;
				bKnownSession = true;
				bHasRequestSession = true;
			}
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

	if (Method == TEXT("tools/call") && bHasRequestSession)
	{
		const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
		if (JsonRequest->TryGetObjectField(TEXT("params"), ParamsObject) && ParamsObject && ParamsObject->IsValid())
		{
			TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject>* ExistingArguments = nullptr;
			if ((*ParamsObject)->TryGetObjectField(TEXT("arguments"), ExistingArguments) && ExistingArguments && ExistingArguments->IsValid())
			{
				Arguments = *ExistingArguments;
			}
			TSharedRef<FJsonObject> Caller = MakeShared<FJsonObject>();
			Caller->SetStringField(TEXT("sessionId"), ProvidedSessionId);
			Caller->SetStringField(TEXT("principal"), RequestSession.Principal);
			Caller->SetStringField(TEXT("clientLabel"), RequestSession.ClientLabel);
			Caller->SetStringField(TEXT("clientVersion"), RequestSession.ClientVersion);
			Caller->SetStringField(TEXT("scope"), RequestSession.Scope);
			Caller->SetStringField(TEXT("taskId"), RequestSession.TaskId);
			Caller->SetStringField(TEXT("threadId"), RequestSession.ThreadId);
			Arguments->SetObjectField(TEXT("__worlddataCaller"), Caller);
			(*ParamsObject)->SetObjectField(TEXT("arguments"), Arguments);
		}
	}

	TSharedPtr<FJsonObject> Result = ProcessJsonRpc(JsonRequest);
	FString ResponseBody = JsonObjectToString(Result.ToSharedRef());
	TUniquePtr<FHttpServerResponse> Response = MakeJsonResponse(200, ResponseBody);
	FString ProtocolSnapshot = GetPreferredProtocolVersion();
	FString ResponseSessionId = ProvidedSessionId;
	if (Method == TEXT("initialize"))
	{
		if (Result->HasField(TEXT("result")))
		{
			const TSharedPtr<FJsonObject> InitializeResult = Result->GetObjectField(TEXT("result"));
			if (InitializeResult.IsValid())
			{
				InitializeResult->TryGetStringField(TEXT("protocolVersion"), ProtocolSnapshot);
			}
		}
		FString ClientLabel = TEXT("unknown_client");
		FString ClientVersion = TEXT("unknown");
		FString RequestedTaskId;
		FString RequestedThreadId;
		const TSharedPtr<FJsonObject>* InitializeParams = nullptr;
		if (JsonRequest->TryGetObjectField(TEXT("params"), InitializeParams) && InitializeParams && InitializeParams->IsValid())
		{
			const TSharedPtr<FJsonObject>* ClientInfo = nullptr;
			if ((*InitializeParams)->TryGetObjectField(TEXT("clientInfo"), ClientInfo) && ClientInfo && ClientInfo->IsValid())
			{
				(*ClientInfo)->TryGetStringField(TEXT("name"), ClientLabel);
				(*ClientInfo)->TryGetStringField(TEXT("version"), ClientVersion);
			}
			const TSharedPtr<FJsonObject>* RequestedContext = nullptr;
			if ((*InitializeParams)->TryGetObjectField(TEXT("worlddataContext"), RequestedContext) && RequestedContext && RequestedContext->IsValid())
			{
				(*RequestedContext)->TryGetStringField(TEXT("taskId"), RequestedTaskId);
				(*RequestedContext)->TryGetStringField(TEXT("threadId"), RequestedThreadId);
			}
		}
		ClientLabel = ClientLabel.Left(128);
		ClientVersion = ClientVersion.Left(128);
		RequestedTaskId = RequestedTaskId.Left(128);
		RequestedThreadId = RequestedThreadId.Left(128);
		ResponseSessionId = RegisterSession(ProtocolSnapshot, ClientLabel, ClientVersion, RequestedTaskId, RequestedThreadId);
	}
	else
	{
		FScopeLock Lock(&GMcpStateMutex);
		ProtocolSnapshot = SessionProtocolVersions.FindRef(ProvidedSessionId).ProtocolVersion;
	}
	Response->Headers.Add(TEXT("MCP-Protocol-Version"), { ProtocolSnapshot });
	if (!ResponseSessionId.IsEmpty())
	{
		Response->Headers.Add(TEXT("MCP-Session-Id"), { ResponseSessionId });
		FScopeLock Lock(&GMcpStateMutex);
		if (const FWorldDataMCPSessionState* Session = SessionProtocolVersions.Find(ResponseSessionId))
		{
			Response->Headers.Add(TEXT("WorldData-Task-Id"), { Session->TaskId });
			Response->Headers.Add(TEXT("WorldData-Thread-Id"), { Session->ThreadId });
		}
	}
	OnComplete(MoveTemp(Response));
	return true;
}

FString FWorldDataMCPServer::RegisterSession(const FString& ProtocolVersion, const FString& ClientLabel, const FString& ClientVersion, const FString& RequestedTaskId, const FString& RequestedThreadId)
{
	FScopeLock Lock(&GMcpStateMutex);
	const FDateTime Now = FDateTime::UtcNow();
	for (auto It = SessionProtocolVersions.CreateIterator(); It; ++It)
	{
		if (Now - It.Value().LastActivityAtUtc > FTimespan::FromMinutes(GMcpSessionLifetimeMinutes))
		{
			It.RemoveCurrent();
		}
	}

	const FString NewSessionId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FWorldDataMCPSessionState& NewSession = SessionProtocolVersions.Add(NewSessionId);
	NewSession.ProtocolVersion = ProtocolVersion;
	NewSession.ClientLabel = ClientLabel.IsEmpty() ? TEXT("unknown_client") : ClientLabel;
	NewSession.ClientVersion = ClientVersion.IsEmpty() ? TEXT("unknown") : ClientVersion;
	NewSession.Principal = FString::Printf(TEXT("local_token_v%d"), GAccessTokenConfigVersion);
	NewSession.Scope = WorldDataMCP::ToolGovernance::RequiresInteractiveApproval(WorldDataMCP::ToolGovernance::EToolRisk::WorkspaceChange)
		? TEXT("authenticated_local_mcp_read_write_requires_editor_approval")
		: TEXT("authenticated_local_mcp_read_write_trusted_automation");
	NewSession.CreatedAtUtc = Now;
	NewSession.LastActivityAtUtc = Now;
	NewSession.TaskId = RequestedTaskId.IsEmpty() ? FString::Printf(TEXT("task_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)) : RequestedTaskId;
	NewSession.ThreadId = RequestedThreadId.IsEmpty() ? FString::Printf(TEXT("thread_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)) : RequestedThreadId;
	while (SessionProtocolVersions.Num() > GMaxConcurrentMcpSessions)
	{
		FString LeastRecentlyUsedSession;
		FDateTime LeastRecentActivity = FDateTime::MaxValue();
		for (const TPair<FString, FWorldDataMCPSessionState>& Pair : SessionProtocolVersions)
		{
			if (Pair.Value.LastActivityAtUtc < LeastRecentActivity)
			{
				LeastRecentActivity = Pair.Value.LastActivityAtUtc;
				LeastRecentlyUsedSession = Pair.Key;
			}
		}
		if (LeastRecentlyUsedSession.IsEmpty())
		{
			break;
		}
		SessionProtocolVersions.Remove(LeastRecentlyUsedSession);
	}
	return NewSessionId;
}

bool FWorldDataMCPServer::IsSessionActiveForApproval(const FString& SessionId)
{
	if (SessionId.IsEmpty())
	{
		return true;
	}
	FScopeLock Lock(&GMcpStateMutex);
	if (const FWorldDataMCPSessionState* Session = SessionProtocolVersions.Find(SessionId))
	{
		return FDateTime::UtcNow() - Session->LastActivityAtUtc <= FTimespan::FromMinutes(GMcpSessionLifetimeMinutes);
	}
	return false;
}

TArray<FWorldDataMCPApprovalSummary> FWorldDataMCPServer::GetPendingApprovals()
{
	const FDateTime Now = FDateTime::UtcNow();
	TArray<TSharedPtr<FPendingToolApproval, ESPMode::ThreadSafe>> Expired;
	TArray<FWorldDataMCPApprovalSummary> Summaries;
	{
		FScopeLock Lock(&GMcpApprovalMutex);
		for (auto It = GPendingToolApprovals.CreateIterator(); It; ++It)
		{
			const TSharedPtr<FPendingToolApproval, ESPMode::ThreadSafe>& Approval = It.Value();
			if (!Approval.IsValid() || Now >= Approval->ExpiresAtUtc)
			{
				if (Approval.IsValid())
				{
					Approval->State.store(static_cast<int32>(EApprovalState::Resolved));
					Expired.Add(Approval);
				}
				It.RemoveCurrent();
				continue;
			}

			FWorldDataMCPApprovalSummary Summary;
			Summary.ApprovalId = Approval->ApprovalId;
			Summary.ToolName = Approval->Invocation.ToolName;
			Summary.Risk = WorldDataMCP::ToolGovernance::GetRiskName(Approval->Invocation.Risk);
			Summary.TargetSummary = Approval->TargetSummary;
			Summary.ChangeSummaryHash = Approval->ChangeSummaryHash;
			Summary.TargetRevision = Approval->TargetRevision;
			Summary.OwnerSessionId = Approval->Invocation.Caller.SessionId;
			Summary.CreatedAtUtc = Approval->CreatedAtUtc;
			Summary.ExpiresAtUtc = Approval->ExpiresAtUtc;
			Summary.bReadyForDecision = Approval->State.load() == static_cast<int32>(EApprovalState::AwaitingDecision);
			Summaries.Add(MoveTemp(Summary));
		}
	}

	for (const TSharedPtr<FPendingToolApproval, ESPMode::ThreadSafe>& Approval : Expired)
	{
		CompleteToolJob(Approval->Job, ErrorJson(TEXT("MCP approval expired before the Unreal Editor user made a decision.")));
		WorldDataMCP::ToolGovernance::RecordApprovalEvent(Approval->Invocation, TEXT("approval_expired"), TEXT("approval TTL elapsed"));
	}
	Summaries.Sort([](const FWorldDataMCPApprovalSummary& A, const FWorldDataMCPApprovalSummary& B)
	{
		return A.CreatedAtUtc < B.CreatedAtUtc;
	});
	return Summaries;
}

bool FWorldDataMCPServer::ResolvePendingApproval(const FString& ApprovalId, const bool bApprove, FString& OutError)
{
	OutError.Empty();
	if (!IsInGameThread())
	{
		OutError = TEXT("MCP approvals must be resolved by the Unreal Editor UI on the game thread.");
		return false;
	}

	TSharedPtr<FPendingToolApproval, ESPMode::ThreadSafe> Approval;
	{
		FScopeLock Lock(&GMcpApprovalMutex);
		if (const TSharedPtr<FPendingToolApproval, ESPMode::ThreadSafe>* Found = GPendingToolApprovals.Find(ApprovalId))
		{
			Approval = *Found;
		}
	}
	if (!Approval.IsValid())
	{
		OutError = TEXT("Unknown, expired, or already-resolved MCP approval.");
		return false;
	}
	if (FDateTime::UtcNow() >= Approval->ExpiresAtUtc)
	{
		Approval->State.store(static_cast<int32>(EApprovalState::Resolved));
		CompleteToolJob(Approval->Job, ErrorJson(TEXT("MCP approval expired before the Unreal Editor user made a decision.")));
		WorldDataMCP::ToolGovernance::RecordApprovalEvent(Approval->Invocation, TEXT("approval_expired"), TEXT("approval TTL elapsed"));
		FScopeLock Lock(&GMcpApprovalMutex);
		GPendingToolApprovals.Remove(ApprovalId);
		OutError = TEXT("This approval has expired. Ask the agent to create a new request.");
		return false;
	}

	int32 ExpectedState = static_cast<int32>(EApprovalState::AwaitingDecision);
	if (!Approval->State.compare_exchange_strong(ExpectedState, static_cast<int32>(EApprovalState::Resolved)))
	{
		OutError = ExpectedState == static_cast<int32>(EApprovalState::Preparing)
			? TEXT("Approval is still capturing the target revision; try again shortly.")
			: TEXT("This approval is already being resolved.");
		return false;
	}
	{
		FScopeLock Lock(&GMcpApprovalMutex);
		GPendingToolApprovals.Remove(ApprovalId);
	}

	if (!bApprove)
	{
		CompleteToolJob(Approval->Job, ErrorJson(TEXT("Tool request was denied by the Unreal Editor user.")));
		WorldDataMCP::ToolGovernance::RecordApprovalEvent(Approval->Invocation, TEXT("approval_denied"), TEXT("denied_by=local_editor_panel"));
		return true;
	}
	if (!IsSessionActiveForApproval(Approval->Invocation.Caller.SessionId))
	{
		CompleteToolJob(Approval->Job, ErrorJson(TEXT("The original MCP session expired before approval. Re-initialize and re-request the change.")));
		WorldDataMCP::ToolGovernance::RecordApprovalEvent(Approval->Invocation, TEXT("approval_invalidated"), TEXT("owner session expired"));
		OutError = TEXT("The original MCP session expired; the request was invalidated.");
		return false;
	}

	const TSharedPtr<FJsonObject> ApprovalArgs = ParseObject(Approval->ArgsJson);
	const FString CurrentRevision = ApprovalArgs.IsValid()
		? CaptureApprovalTargetRevision(Approval->Invocation.ToolName, ApprovalArgs)
		: FString();
	if (CurrentRevision.IsEmpty() || CurrentRevision != Approval->TargetRevision)
	{
		CompleteToolJob(Approval->Job, ErrorJson(TEXT("The approved target revision changed before execution. The request was invalidated; inspect the current state and create a new request.")));
		WorldDataMCP::ToolGovernance::RecordApprovalEvent(Approval->Invocation, TEXT("approval_invalidated"), TEXT("target revision changed"));
		OutError = TEXT("The target revision changed; the request was invalidated.");
		return false;
	}

	Approval->Job->State.store(static_cast<int32>(EToolJobState::Queued));
	WorldDataMCP::ToolGovernance::RecordApprovalEvent(Approval->Invocation, TEXT("approval_granted"), TEXT("approved_by=local_editor_panel"));
	AsyncTask(ENamedThreads::GameThread, [Approval]()
	{
		int32 ExpectedJobState = static_cast<int32>(EToolJobState::Queued);
		if (!Approval->Job->State.compare_exchange_strong(ExpectedJobState, static_cast<int32>(EToolJobState::Executing)))
		{
			CompleteToolJob(Approval->Job, ErrorJson(TEXT("Approved MCP job was cancelled before execution.")));
			return;
		}

		const TSharedPtr<FJsonObject> ApprovedArgs = ParseObject(Approval->ArgsJson);
		FString Result;
		if (!ApprovedArgs.IsValid())
		{
			Result = ErrorJson(TEXT("Approved MCP job arguments could not be parsed."));
		}
		else
		{
			const FString CapabilityError = GetToolCapabilityError(Approval->Invocation.ToolName);
			Result = CapabilityError.IsEmpty()
				? DispatchAuthorizedTool(Approval->Invocation.ToolName, ApprovedArgs)
				: ErrorJson(CapabilityError);
		}
		Approval->Invocation.WorldRevision = CaptureWorldRevision();
		const FString ObjectRevision = ApprovedArgs.IsValid()
			? CaptureApprovalTargetRevision(Approval->Invocation.ToolName, ApprovedArgs)
			: FString();
		Result = AddContextEnvelopeToResultJson(Result, Approval->Invocation.Caller, Approval->Invocation.ToolName, ObjectRevision);
		WorldDataMCP::ToolGovernance::CompleteInvocation(Approval->Invocation, Result);
		CompleteToolJob(Approval->Job, Result);
	});
	return true;
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
		"UEBridgeMCP also includes supplemental tools such as read_log, execute_python, project file tools, PIE controls, and PCG recipe tools. "
		"Every tool accepts an optional responseControl object: maxBytes (default 65536), pageSize (default 50), "
		"cursor for a stable follow-up page, fields for top-level projection, and ifRevision for conditional read-only results. "
		"Every tool result contains contextEnvelope. Before any mutating tool call, copy taskId, threadId, worldRevision, and objectRevision from a fresh relevant result into arguments.worlddataContext; stale or cross-session context is rejected server-side. "
		"Mutating tools return immediately with approvalId and jobId, then wait for a non-modal approval card in the Unreal Editor; poll get_mcp_job_status rather than retrying. "
		"For read-only long-running work, set arguments.worlddataAsync=true; the immediate response contains a jobId that must be polled with get_mcp_job_status. "
		"All tools act on the user's currently open editor world; prefer precise filters and small maxResults to keep responses compact."),
		*GetProjectName());
}

TSharedPtr<FJsonObject> FWorldDataMCPServer::HandleInitialize(const TSharedPtr<FJsonObject>& Params)
{
	FString RequestedVersion;
	Params->TryGetStringField(TEXT("protocolVersion"), RequestedVersion);
	const FString ProtocolVersion = NegotiateProtocolVersion(RequestedVersion);

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

	TSharedPtr<FJsonObject> ToolArguments = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("arguments"), ArgsObj) && ArgsObj && ArgsObj->IsValid())
	{
		ToolArguments = *ArgsObj;
	}
	bool bAsyncRequested = false;
	ToolArguments->TryGetBoolField(TEXT("worlddataAsync"), bAsyncRequested);
	ToolArguments->RemoveField(TEXT("worlddataAsync"));
	FString CallerSessionId;
	const TSharedPtr<FJsonObject>* CallerObject = nullptr;
	if (ToolArguments->TryGetObjectField(TEXT("__worlddataCaller"), CallerObject) && CallerObject && CallerObject->IsValid())
	{
		(*CallerObject)->TryGetStringField(TEXT("sessionId"), CallerSessionId);
	}
	const WorldDataMCP::ToolGovernance::EToolRisk ToolRisk = WorldDataMCP::ToolGovernance::GetRisk(ToolName);
	const bool bMutatingTool = ToolRisk != WorldDataMCP::ToolGovernance::EToolRisk::ReadOnly;
	FString ContextValidationError;
	FString RequestedTaskId;
	FString RequestedThreadId;
	FString ExpectedWorldRevision;
	FString ExpectedObjectRevision;
	const TSharedPtr<FJsonObject>* RequestContext = nullptr;
	if (ToolArguments->TryGetObjectField(TEXT("worlddataContext"), RequestContext) && RequestContext && RequestContext->IsValid())
	{
		(*RequestContext)->TryGetStringField(TEXT("taskId"), RequestedTaskId);
		(*RequestContext)->TryGetStringField(TEXT("threadId"), RequestedThreadId);
		(*RequestContext)->TryGetStringField(TEXT("worldRevision"), ExpectedWorldRevision);
		(*RequestContext)->TryGetStringField(TEXT("objectRevision"), ExpectedObjectRevision);
	}
	ToolArguments->RemoveField(TEXT("worlddataContext"));

	TSharedPtr<FJsonObject> MutableCaller;
	if (CallerObject && CallerObject->IsValid())
	{
		MutableCaller = *CallerObject;
	}
	else
	{
		MutableCaller = MakeShared<FJsonObject>();
		MutableCaller->SetStringField(TEXT("sessionId"), CallerSessionId);
		ToolArguments->SetObjectField(TEXT("__worlddataCaller"), MutableCaller);
	}
	const FString RunId = FString::Printf(TEXT("run_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString TransactionId = bMutatingTool
		? FString::Printf(TEXT("tx_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))
		: FString();
	MutableCaller->SetStringField(TEXT("runId"), RunId);
	MutableCaller->SetStringField(TEXT("transactionId"), TransactionId);
	MutableCaller->SetStringField(TEXT("expectedWorldRevision"), ExpectedWorldRevision);
	MutableCaller->SetStringField(TEXT("expectedObjectRevision"), ExpectedObjectRevision);
	FString SessionTaskId;
	FString SessionThreadId;
	MutableCaller->TryGetStringField(TEXT("taskId"), SessionTaskId);
	MutableCaller->TryGetStringField(TEXT("threadId"), SessionThreadId);
	if (bMutatingTool)
	{
		if (ExpectedWorldRevision.IsEmpty() || ExpectedObjectRevision.IsEmpty())
		{
			ContextValidationError = TEXT("Mutating tools require arguments.worlddataContext.worldRevision and objectRevision from a fresh ContextEnvelope.");
		}
		else if (RequestedTaskId.IsEmpty() || RequestedThreadId.IsEmpty()
			|| RequestedTaskId != SessionTaskId || RequestedThreadId != SessionThreadId)
		{
			ContextValidationError = TEXT("The supplied taskId/threadId do not match this MCP session. Read fresh context and keep one task bound to one session.");
		}
	}

	WorldDataMCP::ResponseContext::FResponseControl ResponseControl;
	FString ResponseControlError;
	const bool bHasValidResponseControl = WorldDataMCP::ResponseContext::ExtractResponseControl(ToolArguments, ResponseControl, ResponseControlError);
	const FString ArgsJson = JsonObjectToString(ToolArguments.ToSharedRef());

	FString ToolResult;
	if (!ContextValidationError.IsEmpty())
	{
		ToolResult = ErrorJson(ContextValidationError);
	}
	else if (ToolName == TEXT("get_mcp_job_status"))
	{
		FString JobId;
		if (!ToolArguments->TryGetStringField(TEXT("jobId"), JobId) || JobId.IsEmpty())
		{
			ToolResult = ErrorJson(TEXT("get_mcp_job_status requires a non-empty jobId."));
		}
		else
		{
			ToolResult = GetToolJobStatusJson(JobId, CallerSessionId);
		}
	}
	else if (!bHasValidResponseControl)
	{
		ToolResult = ErrorJson(ResponseControlError);
	}
	else if (WorldDataMCP::ToolGovernance::RequiresInteractiveApproval(ToolRisk))
	{
		// Mutations never open a modal dialog and never keep this HTTP worker alive.
		// The returned job is resumed only after the editor-panel approval card binds
		// the decision to this session, change hash, target revision, and TTL.
		ToolResult = QueueToolApproval(ToolName, ToolArguments, ArgsJson);
	}
	else if (!ResponseControl.Cursor.IsEmpty())
	{
		FString CursorError;
		if (!WorldDataMCP::ResponseContext::TryResolveCursor(ToolName, ResponseControl.Cursor, ToolResult, CursorError))
		{
			ToolResult = ErrorJson(CursorError);
		}
	}
	else if (bAsyncRequested)
	{
		const int32 PendingBeforeEnqueue = GPendingGameThreadToolDispatches.fetch_add(1);
		if (PendingBeforeEnqueue >= GMaxPendingGameThreadToolDispatches)
		{
			GPendingGameThreadToolDispatches.fetch_sub(1);
			ToolResult = ErrorJson(FString::Printf(
				TEXT("Editor tool queue is full (%d pending requests). Retry this MCP request with backoff."),
				GMaxPendingGameThreadToolDispatches));
		}
		else
		{
			const FString ToolNameCopy = ToolName;
			const FString ArgsCopy = ArgsJson;
			const TSharedRef<FToolJob, ESPMode::ThreadSafe> Job = MakeShared<FToolJob, ESPMode::ThreadSafe>(ToolNameCopy, CallerSessionId);
			{
				FScopeLock Lock(&GMcpJobMutex);
				PruneExpiredToolJobsLocked(FDateTime::UtcNow());
				GToolJobs.Add(Job->Id, Job);
			}

			AsyncTask(ENamedThreads::GameThread, [Job, ToolNameCopy, ArgsCopy]()
			{
				int32 ExpectedState = static_cast<int32>(EToolJobState::Queued);
				if (!Job->State.compare_exchange_strong(ExpectedState, static_cast<int32>(EToolJobState::Executing)))
				{
					FScopeLock ResultLock(&Job->ResultMutex);
					Job->ResultJson = ErrorJson(TEXT("MCP job was cancelled before editor dispatch."));
					Job->CompletedAtUtc = FDateTime::UtcNow();
					GPendingGameThreadToolDispatches.fetch_sub(1);
					return;
				}

				const FString Result = DispatchTool(ToolNameCopy, ArgsCopy);
				{
					FScopeLock ResultLock(&Job->ResultMutex);
					Job->ResultJson = Result;
					Job->CompletedAtUtc = FDateTime::UtcNow();
				}
				Job->State.store(static_cast<int32>(EToolJobState::Completed));
				GPendingGameThreadToolDispatches.fetch_sub(1);
			});

			TSharedRef<FJsonObject> QueuedResult = MakeShared<FJsonObject>();
			QueuedResult->SetBoolField(TEXT("success"), true);
			QueuedResult->SetBoolField(TEXT("queued"), true);
			QueuedResult->SetStringField(TEXT("jobId"), Job->Id);
			QueuedResult->SetStringField(TEXT("state"), TEXT("queued"));
			QueuedResult->SetStringField(TEXT("pollTool"), TEXT("get_mcp_job_status"));
			ToolResult = JsonObjectToString(QueuedResult);
		}
	}
	else if (IsInGameThread())
	{
		ToolResult = DispatchTool(ToolName, ArgsJson);
	}
	else
	{
		// Marshal to the game thread, but never block the HTTP worker forever: if the
		// editor is busy (modal dialog, long operation, PIE transition) we time out and
		// return a structured error instead of hanging the request. The shared promise
		// keeps the async task safe even if we abandon the wait.
		// Mutating calls may open an editor-owned confirmation dialog. Allow a
		// deliberate user decision without prematurely expiring the HTTP request.
		const double ToolDispatchTimeoutSeconds = WorldDataMCP::ToolGovernance::RequiresInteractiveApproval(
			WorldDataMCP::ToolGovernance::GetRisk(ToolName)) ? 300.0 : 60.0;

		const int32 PendingBeforeEnqueue = GPendingGameThreadToolDispatches.fetch_add(1);
		if (PendingBeforeEnqueue >= GMaxPendingGameThreadToolDispatches)
		{
			GPendingGameThreadToolDispatches.fetch_sub(1);
			ToolResult = ErrorJson(FString::Printf(
				TEXT("Editor tool queue is full (%d pending requests). Retry this MCP request with backoff."),
				GMaxPendingGameThreadToolDispatches));
		}
		else
		{
			const FString ToolNameCopy = ToolName;
			const FString ArgsCopy = ArgsJson;
			TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
			TSharedRef<FToolDispatchRequest, ESPMode::ThreadSafe> RequestState = MakeShared<FToolDispatchRequest, ESPMode::ThreadSafe>();
			TFuture<FString> Future = Promise->GetFuture();

			AsyncTask(ENamedThreads::GameThread, [Promise, RequestState, ToolNameCopy, ArgsCopy]()
			{
				int32 ExpectedState = static_cast<int32>(EToolDispatchState::Queued);
				if (!RequestState->State.compare_exchange_strong(ExpectedState, static_cast<int32>(EToolDispatchState::Executing)))
				{
					Promise->SetValue(ErrorJson(FString::Printf(
						TEXT("Tool '%s' was cancelled before dispatch (requestId=%s, state=%s)."),
						*ToolNameCopy,
						*RequestState->RequestId,
						*GetToolDispatchStateName(ExpectedState))));
					GPendingGameThreadToolDispatches.fetch_sub(1);
					return;
				}

				const FString Result = DispatchTool(ToolNameCopy, ArgsCopy);
				RequestState->State.store(static_cast<int32>(EToolDispatchState::Completed));
				Promise->SetValue(Result);
				GPendingGameThreadToolDispatches.fetch_sub(1);
			});

			if (Future.WaitFor(FTimespan::FromSeconds(ToolDispatchTimeoutSeconds)))
			{
				ToolResult = Future.Get();
			}
			else
			{
				int32 ExpectedState = static_cast<int32>(EToolDispatchState::Queued);
				if (RequestState->State.compare_exchange_strong(ExpectedState, static_cast<int32>(EToolDispatchState::CancelledBeforeDispatch)))
				{
					ToolResult = ErrorJson(FString::Printf(
						TEXT("Tool '%s' timed out after %.0f seconds and was cancelled before editor dispatch (requestId=%s)."),
						*ToolName,
						ToolDispatchTimeoutSeconds,
						*RequestState->RequestId));
				}
				else
				{
					const int32 CurrentState = RequestState->State.load();
					ToolResult = ErrorJson(FString::Printf(
						TEXT("Tool '%s' exceeded the %.0f-second HTTP wait after editor execution had already begun (requestId=%s, state=%s). The request was not cancelled; inspect the editor/audit log before retrying."),
						*ToolName,
						ToolDispatchTimeoutSeconds,
						*RequestState->RequestId,
						*GetToolDispatchStateName(CurrentState)));
				}
			}
		}
	}

	if (bHasValidResponseControl && !ToolResult.IsEmpty())
	{
		const bool bReadOnlyTool = ToolName.StartsWith(TEXT("get_"))
			|| ToolName.StartsWith(TEXT("list_"))
			|| ToolName.StartsWith(TEXT("find_"))
			|| ToolName.StartsWith(TEXT("read_"))
			|| ToolName.StartsWith(TEXT("search_"))
			|| ToolName == TEXT("pcg_recipe_library_status");
		ToolResult = WorldDataMCP::ResponseContext::ShapeToolResult(ToolName, ToolResult, ResponseControl, bReadOnlyTool);
	}
	ToolResult = AddContextEnvelopeToResultJson(ToolResult, ExtractCallerContext(ToolArguments), ToolName);

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

static FString DispatchAuthorizedTool(const FString& ToolName, const TSharedPtr<FJsonObject>& Args);

FString FWorldDataMCPServer::DispatchTool(const FString& ToolName, const FString& ArgsJson)
{
	TSharedPtr<FJsonObject> Args = ParseObject(ArgsJson);
	if (!Args.IsValid())
	{
		return ErrorJson(TEXT("Invalid arguments JSON."));
	}

	const WorldDataMCP::ToolGovernance::FCallerContext Caller = ExtractCallerContext(Args);
	Args->RemoveField(TEXT("__worlddataCaller"));
	const FString CurrentWorldRevision = CaptureWorldRevision();
	const FString CurrentObjectRevision = CaptureApprovalTargetRevision(ToolName, Args);

	WorldDataMCP::ToolGovernance::FInvocation Invocation =
		WorldDataMCP::ToolGovernance::BeginInvocation(ToolName, Args, Caller);
	Invocation.WorldRevision = CurrentWorldRevision;
	const FString CapabilityError = GetToolCapabilityError(ToolName);
	if (!CapabilityError.IsEmpty())
	{
		const FString DisabledResult = ErrorJson(CapabilityError);
		WorldDataMCP::ToolGovernance::CompleteInvocation(Invocation, DisabledResult);
		return AddContextEnvelopeToResultJson(DisabledResult, Caller, ToolName, CurrentObjectRevision);
	}
	if (WorldDataMCP::ToolGovernance::RequiresInteractiveApproval(Invocation.Risk))
	{
		const FString ErrorResult = ErrorJson(TEXT("Mutating tools must be resumed through the server-owned non-modal approval job."));
		WorldDataMCP::ToolGovernance::CompleteInvocation(Invocation, ErrorResult);
		return AddContextEnvelopeToResultJson(ErrorResult, Caller, ToolName, CurrentObjectRevision);
	}

	const FString Result = DispatchAuthorizedTool(ToolName, Args);
	WorldDataMCP::ToolGovernance::CompleteInvocation(Invocation, Result);
	return AddContextEnvelopeToResultJson(Result, Caller, ToolName, CurrentObjectRevision);
}

static FString DispatchAuthorizedTool(const FString& ToolName, const TSharedPtr<FJsonObject>& Args)
{
	if (ToolName == TEXT("get_current_project_info"))
	{
		return FWorldDataMCPServer::GetProjectInfoJson();
	}
	if (ToolName == TEXT("get_mcp_governance"))
	{
		return WorldDataMCP::ToolGovernance::GetPolicySnapshotJson();
	}
	if (ToolName == TEXT("get_codex_policy_snapshot"))
	{
		return GetCodexPolicySnapshotJson();
	}

	return DispatchRegisteredTool(ToolName, Args);
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
	const FString ProtocolSnapshot = GetPreferredProtocolVersion();
	FString ProtectedToken;
	if (!ProtectTokenForCurrentUser(Token, ProtectedToken))
	{
		LastError = TEXT("Failed to protect the MCP access token for the current OS user; the token was kept in memory only and will rotate on the next editor start.");
		UE_LOG(LogWorldDataMCP, Error, TEXT("%s"), *LastError);
		return;
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
	Config->SetStringField(TEXT("accessTokenProtected"), ProtectedToken);
	Config->SetNumberField(TEXT("accessTokenVersion"), GAccessTokenConfigVersion);
	Config->SetStringField(TEXT("accessTokenProtection"), TEXT("windows_dpapi_current_user"));
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
	if (!GetProjectSecurityFlag(TEXT("bAllowProjectScopedClientConfig")))
	{
		UE_LOG(LogWorldDataMCP, Log, TEXT("Skipped project-scoped MCP client configuration because it would persist an access token in the workspace. Set [UEBridgeMCP.Security] bAllowProjectScopedClientConfig=true only for a private, ignored workspace."));
		return;
	}

	const FString Token = GetAccessToken();
	const FString ProtocolSnapshot = GetPreferredProtocolVersion();

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
		return ServerName == ManagedServerName;
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

	// Preserve everything the user wrote and replace only this exact server section.
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
		TEXT("# Managed by UEBridgeMCP (%s). Rewritten only after an explicit CLI setup action.\n")
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
	Claude->SetObjectField(TEXT("mcpConfig"), DescribeFile(GetClientConfigFilePath(), TEXT("Project-scoped MCP server (.mcp.json), generated only after explicit setup.")));
	Claude->SetStringField(TEXT("usage"), TEXT("Run `claude` inside the project directory. Claude Code retains its normal per-server approval prompt."));
	Claude->SetStringField(TEXT("approvalPolicy"), TEXT("UEBridgeMCP does not modify Claude approval settings."));
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
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Saved connection state is refreshed automatically; client configuration files are generated only by the explicit CLI setup action.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("The server-owned access token is encrypted with the current Windows user profile. Third-party client configuration is generated only after explicit setup because those clients require a readable HTTP header.")));
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
	const FString ProtocolSnapshot = GetPreferredProtocolVersion();
	Info->SetStringField(TEXT("projectName"), GetProjectName());
	Info->SetStringField(TEXT("projectId"), GetProjectId());
	Info->SetStringField(TEXT("serverName"), GetServerName());
	Info->SetStringField(TEXT("url"), GetMcpUrl());
	Info->SetNumberField(TEXT("port"), BoundPort);
	Info->SetNumberField(TEXT("pid"), static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
	Info->SetStringField(TEXT("protocol"), ProtocolSnapshot);
	Info->SetStringField(TEXT("protocolVersion"), ProtocolSnapshot);
	Info->SetArrayField(TEXT("supportedProtocolVersions"), MakeSupportedProtocolVersionsArray());
	Info->SetStringField(TEXT("accessTokenHeader"), GetAccessTokenHeaderName());
	Info->SetBoolField(TEXT("containsAccessToken"), false);
	Info->SetBoolField(TEXT("requiresAccessToken"), true);
	Info->SetBoolField(TEXT("requiresSessionHeader"), true);
	Info->SetBoolField(TEXT("running"), true);
	Info->SetStringField(TEXT("uproject"), GetProjectFilePath());
	Info->SetStringField(TEXT("projectDir"), GetProjectDir());
	Info->SetStringField(TEXT("writtenAtUtc"), FDateTime::UtcNow().ToIso8601());
	SaferReplaceWriteString(JsonObjectToString(Info, true), GetConnectionPath());
}
