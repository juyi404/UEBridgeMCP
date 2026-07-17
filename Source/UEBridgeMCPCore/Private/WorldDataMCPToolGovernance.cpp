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
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "WorldDataMCPCommon.h"

namespace WorldDataMCP::ToolGovernance
{
	namespace
	{
		FCriticalSection GAuditFileMutex;

		const TSet<FString>& GetWorkspaceChangeTools()
		{
			static const TSet<FString> Tools = {
				TEXT("spawn_actor"), TEXT("transform_actor"),
				TEXT("attach_actor"), TEXT("set_actor_property"), TEXT("play_in_editor"),
				TEXT("stop_pie")
			};
			return Tools;
		}

		const TSet<FString>& GetDestructiveTools()
		{
			static const TSet<FString> Tools = {
				TEXT("delete_actor"), TEXT("save_current_level"), TEXT("create_asset"),
				TEXT("create_blueprint_asset"), TEXT("modify_material_instance"),
				TEXT("create_pcg_graph_from_recipe"), TEXT("write_file"),
				TEXT("delete_file"), TEXT("rename_file")
			};
			return Tools;
		}

		const TArray<FString>& GetKnownToolNames()
		{
			static const TArray<FString> Names = {
				TEXT("get_mcp_governance"), TEXT("get_current_project_info"), TEXT("list_level_actors"),
				TEXT("get_mcp_job_status"),
				TEXT("get_selected_actors"), TEXT("get_actor_details"), TEXT("find_assets"),
				TEXT("read_asset"), TEXT("get_content_summary"), TEXT("select_actor"),
				TEXT("spawn_actor"), TEXT("transform_actor"), TEXT("delete_actor"),
				TEXT("attach_actor"), TEXT("set_actor_property"), TEXT("save_current_level"),
				TEXT("create_asset"), TEXT("create_blueprint_asset"), TEXT("modify_material_instance"),
				TEXT("create_pcg_graph_from_recipe"), TEXT("get_codex_policy_snapshot"),
				TEXT("read_log"), TEXT("execute_python"),
				TEXT("search_assets"), TEXT("find_static_meshes"), TEXT("get_level_actors"),
				TEXT("get_project_info"), TEXT("list_project_modules"), TEXT("get_build_configuration"),
				TEXT("read_file"), TEXT("write_file"), TEXT("delete_file"), TEXT("rename_file"),
				TEXT("play_in_editor"), TEXT("stop_pie"), TEXT("pcg_recipe_library_status"),
				TEXT("search_pcg_recipes"), TEXT("read_pcg_recipe"), TEXT("read_pcg_scene_binding")
			};
			return Names;
		}

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
				Record->SetStringField(TEXT("changeSummaryHash"), Invocation.ChangeSummaryHash);
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
		if (ToolName == TEXT("select_actor"))
		{
			// Selection changes only the local editor UI; it does not mutate the
			// world or project content.
			return EToolRisk::ReadOnly;
		}
		if (ToolName == TEXT("execute_python"))
		{
			return EToolRisk::ArbitraryCode;
		}
		if (GetDestructiveTools().Contains(ToolName))
		{
			return EToolRisk::Destructive;
		}
		if (GetWorkspaceChangeTools().Contains(ToolName))
		{
			return EToolRisk::WorkspaceChange;
		}
		if (ToolName.StartsWith(TEXT("get_"))
			|| ToolName.StartsWith(TEXT("list_"))
			|| ToolName.StartsWith(TEXT("read_"))
			|| ToolName.StartsWith(TEXT("find_"))
			|| ToolName.StartsWith(TEXT("search_"))
			|| ToolName == TEXT("pcg_recipe_library_status"))
		{
			return EToolRisk::ReadOnly;
		}

		// New tools must opt into a precise classification. Until then, require
		// editor approval so a future mutating extension cannot silently inherit
		// read-only treatment.
		return EToolRisk::Destructive;
	}

	FString GetRiskName(const EToolRisk Risk)
	{
		switch (Risk)
		{
		case EToolRisk::ReadOnly: return TEXT("read_only");
		case EToolRisk::WorkspaceChange: return TEXT("workspace_change");
		case EToolRisk::Destructive: return TEXT("destructive");
		case EToolRisk::ArbitraryCode: return TEXT("arbitrary_code");
		default: return TEXT("unknown");
		}
	}

	bool RequiresInteractiveApproval(const EToolRisk Risk)
	{
		return IsInteractiveApprovalEnabled() && Risk != EToolRisk::ReadOnly;
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
		for (const FString& ToolName : GetKnownToolNames())
		{
			const EToolRisk Risk = GetRisk(ToolName);
			TSharedRef<FJsonObject> Tool = MakeShared<FJsonObject>();
			Tool->SetStringField(TEXT("name"), ToolName);
			Tool->SetStringField(TEXT("risk"), GetRiskName(Risk));
			Tool->SetBoolField(TEXT("requiresInteractiveApproval"), RequiresInteractiveApproval(Risk));
			Tool->SetBoolField(TEXT("audited"), true);
			Tools.Add(MakeShared<FJsonValueObject>(Tool));
		}
		Result->SetArrayField(TEXT("tools"), Tools);
		return JsonObjectToString(Result);
	}

	FString AddGovernanceMetadataToToolDefinitions(const FString& DefinitionsJson)
	{
		TArray<TSharedPtr<FJsonValue>> Tools;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(DefinitionsJson);
		if (!FJsonSerializer::Deserialize(Reader, Tools))
		{
			return DefinitionsJson;
		}

		for (const TSharedPtr<FJsonValue>& Value : Tools)
		{
			const TSharedPtr<FJsonObject> Tool = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Tool.IsValid())
			{
				continue;
			}
			FString Name;
			if (!Tool->TryGetStringField(TEXT("name"), Name))
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* ExistingAnnotations = nullptr;
			TSharedPtr<FJsonObject> Annotations;
			if (Tool->TryGetObjectField(TEXT("annotations"), ExistingAnnotations) && ExistingAnnotations && ExistingAnnotations->IsValid())
			{
				Annotations = *ExistingAnnotations;
			}
			else
			{
				Annotations = MakeShared<FJsonObject>();
				Tool->SetObjectField(TEXT("annotations"), Annotations.ToSharedRef());
			}

			const EToolRisk Risk = GetRisk(Name);
			Annotations->SetStringField(TEXT("worldDataRisk"), GetRiskName(Risk));
			Annotations->SetBoolField(TEXT("worldDataInteractiveApproval"), RequiresInteractiveApproval(Risk));
			Annotations->SetBoolField(TEXT("worldDataAudited"), true);
		}

		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Tools, Writer);
		return Out;
	}
}
