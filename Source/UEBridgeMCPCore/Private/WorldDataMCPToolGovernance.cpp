#include "WorldDataMCPToolGovernance.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Crc.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "WorldDataMCPCommon.h"

namespace WorldDataMCP::ToolGovernance
{
	namespace
	{
		FCriticalSection GAuditFileMutex;

		bool IsInteractiveApprovalEnabled()
		{
			// Secure by default. A project may opt out only for trusted automation by
			// setting bRequireInteractiveApprovalForMutations=false explicitly.
			bool bEnabled = true;
			if (GConfig)
			{
				GConfig->GetBool(TEXT("UEBridgeMCP.Security"), TEXT("bRequireInteractiveApprovalForMutations"), bEnabled, GGameIni);
			}
			return bEnabled;
		}

		FString GetAuditDirectory()
		{
			return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("audit"));
		}

		FString GetAuditFilePath(const FDateTime& Timestamp)
		{
			return FPaths::Combine(GetAuditDirectory(), FString::Printf(TEXT("tool-audit-%s.jsonl"), *Timestamp.ToString(TEXT("%Y%m%d"))));
		}

		TSharedPtr<FJsonObject> ParseObject(const FString& JsonText)
		{
			TSharedPtr<FJsonObject> Parsed;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
			return FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid() ? Parsed : nullptr;
		}

		FString MakeFingerprint(const TSharedPtr<FJsonObject>& Arguments)
		{
			// Do not hash serialized argument values: a fast checksum of a short
			// secret or sensitive value would still enable offline guessing. The
			// audit uses only the stable argument name/type shape for correlation.
			TArray<FString> ArgumentShape;
			if (Arguments.IsValid())
			{
				for (const auto& Pair : Arguments->Values)
				{
					const FString Name(*Pair.Key);
					const int32 Type = Pair.Value.IsValid() ? static_cast<int32>(Pair.Value->Type) : -1;
					ArgumentShape.Add(FString::Printf(TEXT("%s:%d"), *Name, Type));
				}
			}
			ArgumentShape.Sort();
			const FString Shape = FString::Join(ArgumentShape, TEXT("|"));
			const FTCHARToUTF8 Utf8(*Shape);
			return FString::Printf(TEXT("crc32-%08x-%d"), FCrc::MemCrc32(Utf8.Get(), Utf8.Length()), Utf8.Length());
		}

		void WriteAuditRecord(const FInvocation& Invocation, const FString& Outcome, const FString& ResultJson, const FString& Detail)
		{
			const FDateTime Now = FDateTime::UtcNow();
			TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
			Record->SetStringField(TEXT("schemaVersion"), TEXT("1.0"));
			Record->SetStringField(TEXT("timestampUtc"), Now.ToIso8601());
			Record->SetStringField(TEXT("invocationId"), Invocation.Id);
			Record->SetStringField(TEXT("server"), TEXT("UEBridgeMCP"));
			Record->SetStringField(TEXT("callerScope"), Invocation.Caller.Scope.IsEmpty()
				? TEXT("unaudited_in_process_caller")
				: Invocation.Caller.Scope);
			Record->SetStringField(TEXT("sessionId"), Invocation.Caller.SessionId);
			Record->SetStringField(TEXT("principal"), Invocation.Caller.Principal);
			Record->SetStringField(TEXT("clientLabel"), Invocation.Caller.ClientLabel);
			Record->SetStringField(TEXT("clientVersion"), Invocation.Caller.ClientVersion);
			Record->SetStringField(TEXT("scope"), Invocation.Caller.Scope);
			Record->SetStringField(TEXT("taskId"), Invocation.Caller.TaskId);
			Record->SetStringField(TEXT("threadId"), Invocation.Caller.ThreadId);
			Record->SetStringField(TEXT("runId"), Invocation.Caller.RunId);
			Record->SetStringField(TEXT("transactionId"), Invocation.Caller.TransactionId);
			Record->SetStringField(TEXT("expectedWorldRevision"), Invocation.Caller.ExpectedWorldRevision);
			Record->SetStringField(TEXT("expectedObjectRevision"), Invocation.Caller.ExpectedObjectRevision);
			Record->SetStringField(TEXT("worldRevision"), Invocation.WorldRevision);
			Record->SetStringField(TEXT("tool"), Invocation.ToolName);
			Record->SetStringField(TEXT("risk"), GetRiskName(Invocation.Risk));
			Record->SetStringField(TEXT("outcome"), Outcome);
			Record->SetNumberField(TEXT("durationMs"), (Now - Invocation.StartedAtUtc).GetTotalMilliseconds());
			Record->SetStringField(TEXT("argumentShapeFingerprint"), Invocation.ArgumentFingerprint);
			if (!Invocation.ApprovalId.IsEmpty())
			{
				Record->SetStringField(TEXT("approvalId"), Invocation.ApprovalId);
				Record->SetStringField(TEXT("changeSummaryFingerprint"), Invocation.ChangeSummaryFingerprint);
				Record->SetStringField(TEXT("targetRevision"), Invocation.TargetRevision);
			}

			TArray<TSharedPtr<FJsonValue>> ArgumentNames;
			for (const FString& Name : Invocation.ArgumentNames)
			{
				ArgumentNames.Add(MakeShared<FJsonValueString>(Name));
			}
			Record->SetArrayField(TEXT("argumentNames"), ArgumentNames);
			if (!Detail.IsEmpty())
			{
				Record->SetStringField(TEXT("detail"), Detail.Left(512));
			}

			if (!ResultJson.IsEmpty())
			{
				if (const TSharedPtr<FJsonObject> Result = ParseObject(ResultJson))
				{
					bool bSuccess = true;
					if (Result->TryGetBoolField(TEXT("success"), bSuccess))
					{
						Record->SetBoolField(TEXT("success"), bSuccess);
					}
				}
			}

			const FString Line = JsonObjectToString(Record) + LINE_TERMINATOR;
			FScopeLock Lock(&GAuditFileMutex);
			IFileManager::Get().MakeDirectory(*GetAuditDirectory(), true);
			FFileHelper::SaveStringToFile(
				Line,
				*GetAuditFilePath(Now),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
				&IFileManager::Get(),
				FILEWRITE_Append);
		}
	}

	EToolRisk GetRisk(const FString& ToolName)
	{
		FToolMetadata Metadata;
		if (FToolRegistry::Get().FindToolMetadata(ToolName, Metadata))
		{
			return Metadata.Risk;
		}

		// Unknown tools are always fail-closed. HandleToolsCall rejects them before
		// dispatch, but this protects every secondary invocation path as well.
		return EToolRisk::Destructive;
	}

	FString GetRiskName(const EToolRisk Risk)
	{
		return GetToolRiskName(Risk);
	}

	bool RequiresInteractiveApproval(const EToolRisk Risk)
	{
		return IsInteractiveApprovalEnabled() && Risk != EToolRisk::ReadOnly;
	}

	bool RequiresInteractiveApproval(const FString& ToolName)
	{
		FToolMetadata Metadata;
		if (!FToolRegistry::Get().FindToolMetadata(ToolName, Metadata))
		{
			return true;
		}
		return IsInteractiveApprovalEnabled() && Metadata.bRequiresInteractiveApproval;
	}

	FInvocation BeginInvocation(const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments, const FCallerContext& Caller)
	{
		FInvocation Invocation;
		Invocation.Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Invocation.ToolName = ToolName;
		Invocation.Risk = GetRisk(ToolName);
		Invocation.StartedAtUtc = FDateTime::UtcNow();
		Invocation.ArgumentFingerprint = MakeFingerprint(Arguments);
		Invocation.Caller = Caller;
		if (Arguments.IsValid())
		{
			for (const auto& Pair : Arguments->Values)
			{
				Invocation.ArgumentNames.Add(FString(*Pair.Key));
			}
			Invocation.ArgumentNames.Sort();
		}
		return Invocation;
	}

	void RecordApprovalEvent(const FInvocation& Invocation, const FString& Outcome, const FString& Detail)
	{
		WriteAuditRecord(Invocation, Outcome, FString(), Detail);
	}

	void CompleteInvocation(const FInvocation& Invocation, const FString& ResultJson)
	{
		FString Outcome = TEXT("completed");
		if (const TSharedPtr<FJsonObject> Result = ParseObject(ResultJson))
		{
			bool bSuccess = true;
			if (Result->TryGetBoolField(TEXT("success"), bSuccess) && !bSuccess)
			{
				Outcome = TEXT("failed");
			}
		}
		WriteAuditRecord(Invocation, Outcome, ResultJson, FString());
	}

	FString GetPolicySnapshotJson()
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("schemaVersion"), TEXT("1.0"));
		Result->SetStringField(TEXT("serverEnforcement"), TEXT("The server enforces non-modal editor approval for every mutating tool by default."));
		Result->SetBoolField(TEXT("interactiveApprovalForMutations"), IsInteractiveApprovalEnabled());
		Result->SetStringField(TEXT("auditDirectory"), GetAuditDirectory());
		Result->SetStringField(TEXT("auditFormat"), TEXT("redacted JSON Lines; values and secret-capable argument contents are not stored."));

		TArray<TSharedPtr<FJsonValue>> Tools;
		for (const FToolMetadata& Metadata : FToolRegistry::Get().GetRegisteredToolMetadata())
		{
			TSharedRef<FJsonObject> Tool = MakeShared<FJsonObject>();
			Tool->SetStringField(TEXT("name"), Metadata.Name);
			Tool->SetStringField(TEXT("provider"), Metadata.ProviderName);
			Tool->SetStringField(TEXT("risk"), GetRiskName(Metadata.Risk));
			Tool->SetBoolField(TEXT("requiresInteractiveApproval"), RequiresInteractiveApproval(Metadata.Name));
			Tool->SetBoolField(TEXT("audited"), Metadata.bAudited);
			Tool->SetStringField(TEXT("revisionPolicy"), GetToolRevisionPolicyName(Metadata.RevisionPolicy));
			TArray<TSharedPtr<FJsonValue>> Capabilities;
			for (const FString& Capability : Metadata.RequiredCapabilities)
			{
				Capabilities.Add(MakeShared<FJsonValueString>(Capability));
			}
			Tool->SetArrayField(TEXT("requiredCapabilities"), Capabilities);
			Tools.Add(MakeShared<FJsonValueObject>(Tool));
		}
		Result->SetArrayField(TEXT("tools"), Tools);
		return JsonObjectToString(Result);
	}
}
