#include "WorldDataCodexACPClient.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WorldDataMCPServer.h"

DEFINE_LOG_CATEGORY_STATIC(LogWorldDataCodexACP, Log, All);

namespace
{
	static bool PathExists(const FString& Path)
	{
		return !Path.IsEmpty() && (FPaths::FileExists(Path) || IFileManager::Get().FileSize(*Path) >= 0);
	}

	// Redact the MCP access token from a JSON-RPC frame before it is written to the editor log.
	// session/new embeds the token as an X-WorldData-MCP-Token header value; logging the raw
	// frame at Verbose would otherwise leak it into Saved/Logs — a path agents are explicitly
	// blocked from reading precisely because it is sensitive.
	static FString RedactAcpFrameForLog(const FString& Frame)
	{
		const FString Token = FWorldDataMCPServer::GetAccessToken();
		if (Token.IsEmpty() || !Frame.Contains(Token, ESearchCase::CaseSensitive))
		{
			return Frame;
		}
		FString Redacted = Frame;
		Redacted.ReplaceInline(*Token, TEXT("***REDACTED***"), ESearchCase::CaseSensitive);
		return Redacted;
	}

	constexpr int32 MaxAcpReadTextFileBytes = 1024 * 1024;

	static FString MakeProjectRelative(FString Path)
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		if (FPaths::MakePathRelativeTo(Path, *ProjectDir))
		{
			Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			return Path;
		}
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Path;
	}

	static bool HasAllowedAcpTextFileExtension(const FString& FullPath)
	{
		const FString Extension = FPaths::GetExtension(FullPath, true).ToLower();
		static const TSet<FString> AllowedExtensions = {
			TEXT(".bat"),
			TEXT(".c"),
			TEXT(".cmd"),
			TEXT(".cpp"),
			TEXT(".cs"),
			TEXT(".css"),
			TEXT(".csv"),
			TEXT(".h"),
			TEXT(".hpp"),
			TEXT(".html"),
			TEXT(".ini"),
			TEXT(".js"),
			TEXT(".json"),
			TEXT(".jsx"),
			TEXT(".md"),
			TEXT(".ps1"),
			TEXT(".py"),
			TEXT(".sh"),
			TEXT(".toml"),
			TEXT(".ts"),
			TEXT(".tsx"),
			TEXT(".tsv"),
			TEXT(".txt"),
			TEXT(".uplugin"),
			TEXT(".uproject"),
			TEXT(".usf"),
			TEXT(".ush"),
			TEXT(".xml"),
			TEXT(".yaml"),
			TEXT(".yml")
		};
		return AllowedExtensions.Contains(Extension);
	}

	static bool ResolveAcpProjectTextFilePath(const FString& InputPath, FString& OutPath, FString& OutError)
	{
		if (InputPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("path is required.");
			return false;
		}

		FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::CollapseRelativeDirectories(ProjectDir);
		FPaths::NormalizeDirectoryName(ProjectDir);

		FString Candidate = FPaths::IsRelative(InputPath)
			? FPaths::Combine(ProjectDir, InputPath)
			: InputPath;
		Candidate = FPaths::ConvertRelativePathToFull(Candidate);
		FPaths::CollapseRelativeDirectories(Candidate);
		FPaths::NormalizeFilename(Candidate);

		const bool bInProject =
			Candidate.Equals(ProjectDir, ESearchCase::IgnoreCase)
			|| Candidate.StartsWith(ProjectDir + TEXT("/"), ESearchCase::IgnoreCase);
		if (!bInProject)
		{
			OutError = FString::Printf(TEXT("Refusing to read outside the project directory: %s"), *InputPath);
			return false;
		}

		FString Relative = MakeProjectRelative(Candidate).ToLower();
		Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
		static const TSet<FString> RestrictedExactPaths = {
			TEXT(".mcp.json"),
			TEXT(".cursor/mcp.json"),
			TEXT("saved/uebridgemcp/config.json"),
			TEXT("saved/uebridgemcp/mcp.json")
		};
		if (RestrictedExactPaths.Contains(Relative))
		{
			OutError = FString::Printf(TEXT("Refusing to read MCP connection/config file: %s"), *MakeProjectRelative(Candidate));
			return false;
		}

		static const TArray<FString> RestrictedPrefixes = {
			TEXT(".git/"),
			TEXT("binaries/"),
			TEXT("deriveddatacache/"),
			TEXT("intermediate/"),
			TEXT("saved/logs/"),
			TEXT("saved/uebridgemcp/")
		};
		for (const FString& Prefix : RestrictedPrefixes)
		{
			if (Relative.StartsWith(Prefix))
			{
				OutError = FString::Printf(TEXT("Refusing to read restricted project path: %s"), *MakeProjectRelative(Candidate));
				return false;
			}
		}

		if (!HasAllowedAcpTextFileExtension(Candidate))
		{
			OutError = FString::Printf(TEXT("Refusing to read non-text or unsupported file type: %s"), *MakeProjectRelative(Candidate));
			return false;
		}

		OutPath = Candidate;
		return true;
	}

	static void AddLaunchableNames(const FString& BaseName, TArray<FString>& OutNames)
	{
#if PLATFORM_WINDOWS
		OutNames.Add(BaseName + TEXT(".exe"));
		OutNames.Add(BaseName + TEXT(".cmd"));
		OutNames.Add(BaseName + TEXT(".bat"));
#endif
		OutNames.Add(BaseName + TEXT(".js"));
		OutNames.Add(BaseName + TEXT(".mjs"));
		OutNames.Add(BaseName + TEXT(".cjs"));
		OutNames.Add(BaseName);
	}

	static void AddAdapterBaseNames(EWorldDataAcpAgent InAgent, TArray<FString>& OutBaseNames)
	{
		if (InAgent == EWorldDataAcpAgent::ClaudeCode)
		{
			OutBaseNames.Add(TEXT("claude-agent-acp"));
			// Keep the older community adapter discoverable for existing local setups,
			// but prefer the official Agent Client Protocol adapter above.
			OutBaseNames.Add(TEXT("acp-claude-code"));
			return;
		}
		if (InAgent == EWorldDataAcpAgent::Cursor)
		{
			OutBaseNames.Add(TEXT("cursor-agent"));
			OutBaseNames.Add(TEXT("agent"));
			return;
		}

		OutBaseNames.Add(TEXT("codex-acp"));
	}

#if PLATFORM_WINDOWS
	static bool TryResolveNodeShimTarget(const FString& ShimPath, FString& OutTarget)
	{
		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *ShimPath))
		{
			return false;
		}

		Content.ReplaceInline(TEXT("/"), TEXT("\\"));
		const FString ShimDir = FPaths::GetPath(ShimPath);

		auto ExtractRelativeTarget = [&Content, &ShimDir, &OutTarget](const FString& Needle) -> bool
		{
			int32 SearchStart = 0;
			while (SearchStart < Content.Len())
			{
				int32 Start = Content.Find(Needle, ESearchCase::IgnoreCase, ESearchDir::FromStart, SearchStart);
				if (Start == INDEX_NONE)
				{
					return false;
				}

				Start += Needle.Len();
				SearchStart = Start;
				int32 End = Start;
				while (End < Content.Len())
				{
					const TCHAR Ch = Content[End];
					if (Ch == TEXT('"') || Ch == TEXT('\'') || Ch == TEXT(' ') || Ch == TEXT('\r') || Ch == TEXT('\n') || Ch == TEXT('\t'))
					{
						break;
					}
					++End;
				}

				const FString RelativeTarget = Content.Mid(Start, End - Start);
				if (!RelativeTarget.Contains(TEXT("node_modules"), ESearchCase::IgnoreCase)
					|| !(RelativeTarget.EndsWith(TEXT(".js"), ESearchCase::IgnoreCase)
						|| RelativeTarget.EndsWith(TEXT(".mjs"), ESearchCase::IgnoreCase)
						|| RelativeTarget.EndsWith(TEXT(".cjs"), ESearchCase::IgnoreCase)))
				{
					SearchStart = End + 1;
					continue;
				}

				OutTarget = FPaths::Combine(ShimDir, RelativeTarget);
				FPaths::CollapseRelativeDirectories(OutTarget);
				FPaths::MakePlatformFilename(OutTarget);
				return true;
			}
			return false;
		};

		return ExtractRelativeTarget(TEXT("%dp0%\\"))
			|| ExtractRelativeTarget(TEXT("$basedir\\"));
	}

	static bool IsLaunchableAdapterCandidate(EWorldDataAcpAgent InAgent, const FString& Candidate)
	{
		if (!PathExists(Candidate))
		{
			return false;
		}

		const FString Ext = FPaths::GetExtension(Candidate).ToLower();
		if (Ext.IsEmpty())
		{
			FString Content;
			if (FFileHelper::LoadFileToString(Content, *Candidate) && Content.StartsWith(TEXT("#!/bin/sh")))
			{
				UE_LOG(LogWorldDataCodexACP, Verbose, TEXT("Skipping Unix shell ACP shim on Windows: %s"), *Candidate);
				return false;
			}
		}

		if (Ext.IsEmpty() || Ext == TEXT("cmd") || Ext == TEXT("bat") || Ext == TEXT("ps1"))
		{
			FString ShimTarget;
			if (TryResolveNodeShimTarget(Candidate, ShimTarget) && !PathExists(ShimTarget))
			{
				UE_LOG(LogWorldDataCodexACP, Warning,
					TEXT("Skipping broken %s ACP adapter shim: %s targets missing module %s"),
					*FWorldDataCodexACPClient::GetAdapterBaseName(InAgent),
					*Candidate,
					*ShimTarget);
				return false;
			}
		}

		return true;
	}
#else
	static bool IsLaunchableAdapterCandidate(EWorldDataAcpAgent InAgent, const FString& Candidate)
	{
		return PathExists(Candidate);
	}
#endif

	static TSharedPtr<FJsonValue> ExtractRpcId(const TSharedPtr<FJsonObject>& Message)
	{
		if (!Message.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonValue>* Field = Message->Values.Find(TEXT("id"));
		return Field ? *Field : nullptr;
	}

	static FString GetOptionalString(const TSharedPtr<FJsonObject>& Object, const FString& Field)
	{
		FString Value;
		return Object.IsValid() && Object->TryGetStringField(Field, Value) ? Value : FString();
	}

	static bool TryGetTextField(const TSharedPtr<FJsonObject>& Object, FString& OutText)
	{
		static const TCHAR* TextFields[] = { TEXT("text"), TEXT("summary"), TEXT("delta") };
		for (const TCHAR* Field : TextFields)
		{
			if (Object.IsValid() && Object->TryGetStringField(Field, OutText) && !OutText.IsEmpty())
			{
				return true;
			}
		}
		return false;
	}

	static bool TryAppendTextFromContentValue(const TSharedPtr<FJsonValue>& Value, FString& OutText)
	{
		if (!Value.IsValid())
		{
			return false;
		}

		if (Value->Type == EJson::String)
		{
			OutText += Value->AsString();
			return !OutText.IsEmpty();
		}

		if (Value->Type == EJson::Object)
		{
			FString Text;
			if (TryGetTextField(Value->AsObject(), Text))
			{
				OutText += Text;
				return true;
			}
			return false;
		}

		if (Value->Type == EJson::Array)
		{
			bool bFoundText = false;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				bFoundText |= TryAppendTextFromContentValue(Item, OutText);
			}
			return bFoundText;
		}

		return false;
	}

	static bool TryGetSessionUpdateText(const TSharedPtr<FJsonObject>& Update, FString& OutText)
	{
		OutText.Empty();
		if (!Update.IsValid())
		{
			return false;
		}

		if (TryGetTextField(Update, OutText))
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Content = Update->TryGetField(TEXT("content"));
		return TryAppendTextFromContentValue(Content, OutText);
	}

	static bool IsReasoningSummaryUpdateType(const FString& UpdateType)
	{
		return UpdateType == TEXT("agent_thought_chunk")
			|| UpdateType == TEXT("agent_reasoning_chunk")
			|| UpdateType == TEXT("agent_reasoning_summary_chunk")
			|| UpdateType == TEXT("agent_summary_chunk");
	}

	// Compact, truncated digest of a tool call's input (ACP 'rawInput') so high-risk calls —
	// especially execute_python scripts — are auditable in the saved transcript. Empty if no input.
	// The digest rides in the tool message text; the pill shows only the name (rest = tooltip).
	static FString ExtractToolInputDigest(const TSharedPtr<FJsonObject>& Update)
	{
		if (!Update.IsValid())
		{
			return FString();
		}
		FString Out;
		const TSharedPtr<FJsonObject>* InputObj = nullptr;
		FString InputStr;
		if (Update->TryGetObjectField(TEXT("rawInput"), InputObj) && InputObj && InputObj->IsValid())
		{
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(InputObj->ToSharedRef(), Writer);
		}
		else if (Update->TryGetStringField(TEXT("rawInput"), InputStr))
		{
			Out = InputStr;
		}
		Out.TrimStartAndEndInline();
		const int32 MaxLen = 4000;
		if (Out.Len() > MaxLen)
		{
			Out = Out.Left(MaxLen) + FString::Printf(TEXT("…(+%d 字符)"), Out.Len() - MaxLen);
		}
		return Out;
	}

	// Compact a tool-call title for display: drop a leading "Tool: " label and the
	// "serverName/" qualifier so an MCP call shows as "get_actor_details" rather than
	// "Tool: world_data_<hash>/get_actor_details".
	static FString CleanToolTitle(FString Title)
	{
		Title.TrimStartAndEndInline();
		Title.RemoveFromStart(TEXT("Tool: "), ESearchCase::IgnoreCase);
		Title.RemoveFromStart(TEXT("Tool："), ESearchCase::IgnoreCase);
		int32 SlashIndex = INDEX_NONE;
		if (Title.FindLastChar(TCHAR('/'), SlashIndex))
		{
			Title.RightChopInline(SlashIndex + 1);
		}
		return Title.TrimStartAndEnd();
	}

	static int32 GetOptionalInt(const TSharedPtr<FJsonObject>& Object, const FString& Field)
	{
		if (!Object.IsValid())
		{
			return 0;
		}

		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
		if (!Value.IsValid())
		{
			return 0;
		}
		if (Value->Type == EJson::Number)
		{
			return static_cast<int32>(Value->AsNumber());
		}
		if (Value->Type == EJson::String)
		{
			return FCString::Atoi(*Value->AsString());
		}
		return 0;
	}

	static bool LooksLikeShellRequest(const TSharedPtr<FJsonObject>& Object)
	{
		const FString Probe = (GetOptionalString(Object, TEXT("title")) + TEXT(" ") +
			GetOptionalString(Object, TEXT("toolCallId")) + TEXT(" ") +
			GetOptionalString(Object, TEXT("name")) + TEXT(" ") +
			GetOptionalString(Object, TEXT("kind"))).ToLower();
		return Probe.Contains(TEXT("terminal"))
			|| Probe.Contains(TEXT("shell"))
			|| Probe.Contains(TEXT("command"))
			|| Probe.Contains(TEXT("exec"));
	}

	static bool LooksLikeWorldDataMcpRequest(const TSharedPtr<FJsonObject>& Object)
	{
		const FString Probe = (GetOptionalString(Object, TEXT("title")) + TEXT(" ") +
			GetOptionalString(Object, TEXT("toolCallId")) + TEXT(" ") +
			GetOptionalString(Object, TEXT("name")) + TEXT(" ") +
			GetOptionalString(Object, TEXT("kind"))).ToLower();
		return Probe.Contains(TEXT("worlddata"))
			|| Probe.Contains(TEXT("world_data"))
			|| Probe.Contains(TEXT("uebridgemcp"))
			|| Probe.Contains(TEXT("collectworlddata"));
	}

	static FString GetModeInstruction(EWorldDataCodexPermissionMode Mode)
	{
		switch (Mode)
		{
		case EWorldDataCodexPermissionMode::Plan:
			return TEXT("当前模式：计划模式。不要调用 MCP tools，不要执行任何会修改项目的动作；只给出清晰的步骤计划，并在需要执行时提示用户切换到默认或绕过模式。");
		case EWorldDataCodexPermissionMode::Focus:
			return TEXT("当前模式：专注模式。先进行多轮可共享推演，输出详细落地计划，并请用户确认是否执行；在用户确认前不要调用 MCP tools，不要修改项目。");
		case EWorldDataCodexPermissionMode::Bypass:
			return TEXT("当前模式：绕过模式。可以直接使用可用的 MCP tools/resources 完成任务，不要等待额外批准；但 shell/terminal 仍然被宿主禁用。");
		case EWorldDataCodexPermissionMode::Default:
		default:
			return TEXT("当前模式：默认。按标准流程工作，需要工具权限时等待用户确认；shell/terminal 被宿主禁用。");
		}
	}

	static TSharedPtr<FJsonObject> BuildJsonRpcErrorResponse(const TSharedPtr<FJsonObject>& Request, int32 Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetNumberField(TEXT("code"), Code);
		Error->SetStringField(TEXT("message"), Message);

		TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		if (Request.IsValid())
		{
			if (const TSharedPtr<FJsonValue>* Id = Request->Values.Find(TEXT("id")))
			{
				Response->SetField(TEXT("id"), *Id);
			}
			else
			{
				Response->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
			}
		}
		Response->SetObjectField(TEXT("error"), Error);
		return Response;
	}

	static TArray<FWorldDataAcpPermissionOption> ExtractPermissionOptions(const TSharedPtr<FJsonObject>& Params)
	{
		TArray<FWorldDataAcpPermissionOption> Options;
		const TArray<TSharedPtr<FJsonValue>>* OptionValues = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("options"), OptionValues) || !OptionValues)
		{
			return Options;
		}

		for (const TSharedPtr<FJsonValue>& OptionValue : *OptionValues)
		{
			TSharedPtr<FJsonObject> OptionObject = OptionValue.IsValid() ? OptionValue->AsObject() : nullptr;
			if (!OptionObject.IsValid())
			{
				continue;
			}

			FWorldDataAcpPermissionOption Option;
			Option.OptionId = GetOptionalString(OptionObject, TEXT("optionId"));
			Option.Name = GetOptionalString(OptionObject, TEXT("name"));
			Option.Kind = GetOptionalString(OptionObject, TEXT("kind"));
			if (!Option.OptionId.IsEmpty())
			{
				Options.Add(MoveTemp(Option));
			}
		}

		return Options;
	}

	static FString FindPermissionOption(const TArray<FWorldDataAcpPermissionOption>& Options, const FString& Match)
	{
		const FString LowerMatch = Match.ToLower();
		auto FieldMatches = [&LowerMatch](const FString& Value)
		{
			FString LowerValue = Value.ToLower();
			LowerValue.TrimStartAndEndInline();
			if (LowerValue == LowerMatch)
			{
				return true;
			}
			if (LowerValue.Contains(TEXT("deny")) || LowerValue.Contains(TEXT("reject")) || LowerValue.Contains(TEXT("disallow")) || LowerValue.Contains(TEXT("disapprove")))
			{
				return false;
			}
			return LowerValue == (LowerMatch + TEXT("-once"))
				|| LowerValue == (LowerMatch + TEXT("_once"))
				|| LowerValue.StartsWith(LowerMatch + TEXT("-"))
				|| LowerValue.StartsWith(LowerMatch + TEXT("_"))
				|| LowerValue.EndsWith(FString(TEXT("-")) + LowerMatch)
				|| LowerValue.EndsWith(FString(TEXT("_")) + LowerMatch);
		};
		for (const FWorldDataAcpPermissionOption& Option : Options)
		{
			if (FieldMatches(Option.Kind) || FieldMatches(Option.Name) || FieldMatches(Option.OptionId))
			{
				return Option.OptionId;
			}
		}
		return FString();
	}

	static FString GetAllowOptionId(const TArray<FWorldDataAcpPermissionOption>& Options)
	{
		FString OptionId = FindPermissionOption(Options, TEXT("allow"));
		if (OptionId.IsEmpty())
		{
			OptionId = FindPermissionOption(Options, TEXT("approve"));
		}
		return OptionId;
	}

	static FString GetDenyOptionId(const TArray<FWorldDataAcpPermissionOption>& Options)
	{
		FString OptionId = FindPermissionOption(Options, TEXT("reject"));
		if (OptionId.IsEmpty())
		{
			OptionId = FindPermissionOption(Options, TEXT("deny"));
		}
		return OptionId.IsEmpty() ? TEXT("deny") : OptionId;
	}

	static FString GetPermissionTitle(const TSharedPtr<FJsonObject>& ToolCall)
	{
		FString Title = GetOptionalString(ToolCall, TEXT("title"));
		if (Title.IsEmpty())
		{
			Title = GetOptionalString(ToolCall, TEXT("name"));
		}
		if (Title.IsEmpty())
		{
			Title = GetOptionalString(ToolCall, TEXT("toolCallId"));
		}
		return Title.IsEmpty() ? TEXT("未知工具请求") : Title;
	}
	static FString GetMcpMessageToolName(const TSharedPtr<FJsonObject>& McpMessage)
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (McpMessage.IsValid() && McpMessage->TryGetObjectField(TEXT("params"), Params) && Params && Params->IsValid())
		{
			return GetOptionalString(*Params, TEXT("name"));
		}
		return FString();
	}

	static FString GetMcpMessageToolCallId(const TSharedPtr<FJsonObject>& McpMessage)
	{
		FString ToolCallId = GetOptionalString(McpMessage, TEXT("toolCallId"));
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (ToolCallId.IsEmpty() && McpMessage.IsValid() && McpMessage->TryGetObjectField(TEXT("params"), Params) && Params && Params->IsValid())
		{
			ToolCallId = GetOptionalString(*Params, TEXT("toolCallId"));
			const TSharedPtr<FJsonObject>* Meta = nullptr;
			if (ToolCallId.IsEmpty() && (*Params)->TryGetObjectField(TEXT("_meta"), Meta) && Meta && Meta->IsValid())
			{
				ToolCallId = GetOptionalString(*Meta, TEXT("toolCallId"));
			}
		}
		return ToolCallId;
	}

	static bool TrySerializeJsonObjectCondensed(const TSharedPtr<FJsonObject>& Object, FString& Out)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		Out.Reset();
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}

	static FString NormalizeToolInputText(FString Text)
	{
		Text.TrimStartAndEndInline();
		if (Text.IsEmpty())
		{
			return FString();
		}

		TSharedPtr<FJsonObject> ParsedObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (FJsonSerializer::Deserialize(Reader, ParsedObject) && ParsedObject.IsValid())
		{
			FString Serialized;
			if (TrySerializeJsonObjectCondensed(ParsedObject, Serialized))
			{
				return Serialized;
			}
		}
		return Text;
	}

	static FString GetToolInputFingerprint(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid())
		{
			return FString();
		}

		for (const TCHAR* FieldName : { TEXT("rawInput"), TEXT("arguments"), TEXT("input"), TEXT("args") })
		{
			const TSharedPtr<FJsonValue> Value = Object->TryGetField(FieldName);
			if (!Value.IsValid())
			{
				continue;
			}

			if (Value->Type == EJson::Object)
			{
				FString Serialized;
				if (TrySerializeJsonObjectCondensed(Value->AsObject(), Serialized))
				{
					return Serialized;
				}
			}
			else if (Value->Type == EJson::String)
			{
				return NormalizeToolInputText(Value->AsString());
			}
			else if (Value->Type == EJson::Boolean)
			{
				return Value->AsBool() ? TEXT("true") : TEXT("false");
			}
			else if (Value->Type == EJson::Number)
			{
				return LexToString(Value->AsNumber());
			}
		}
		return FString();
	}

	static FString GetMcpMessageInputFingerprint(const TSharedPtr<FJsonObject>& McpMessage)
	{
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (McpMessage.IsValid() && McpMessage->TryGetObjectField(TEXT("params"), Params) && Params && Params->IsValid())
		{
			return GetToolInputFingerprint(*Params);
		}
		return FString();
	}

	static bool ApprovedMcpToolCallMatches(
		const FWorldDataApprovedMcpToolCall& Approval,
		const FString& ToolName,
		const FString& ToolCallId,
		const FString& SessionId,
		const FString& InputFingerprint)
	{
		if (!Approval.SessionId.IsEmpty() && !SessionId.IsEmpty() && !Approval.SessionId.Equals(SessionId, ESearchCase::CaseSensitive))
		{
			return false;
		}

		bool bIdentityMatches = false;
		if (!Approval.ToolCallId.IsEmpty() && !ToolCallId.IsEmpty())
		{
			bIdentityMatches = Approval.ToolCallId.Equals(ToolCallId, ESearchCase::CaseSensitive);
		}
		else if (!Approval.ToolName.IsEmpty())
		{
			bIdentityMatches = Approval.ToolName.Equals(ToolName, ESearchCase::CaseSensitive);
		}
		if (!bIdentityMatches)
		{
			return false;
		}

		if (!Approval.InputFingerprint.IsEmpty())
		{
			return !InputFingerprint.IsEmpty() && Approval.InputFingerprint.Equals(InputFingerprint, ESearchCase::CaseSensitive);
		}

		// Tool-name-only approvals are too broad. Without an input fingerprint, require the
		// adapter-provided toolCallId to match exactly.
		return !Approval.ToolCallId.IsEmpty() && !ToolCallId.IsEmpty();
	}
}

FWorldDataCodexACPClient::~FWorldDataCodexACPClient()
{
	Stop();
}

void FWorldDataCodexACPClient::SendPrompt(const FString& Prompt)
{
	const FString Trimmed = Prompt.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return;
	}

	PendingPrompt = Trimmed;
	LastError.Empty();

	if (!EnsureProcess())
	{
		return;
	}

	if (bInitialized)
	{
		StartSessionIfReady();
		SendPendingPromptIfReady();
	}
}

void FWorldDataCodexACPClient::Stop()
{
	if (Process.IsValid())
	{
		Process->Cancel(true);
		Process.Reset();
	}

	StdoutBuffer.Empty();
	RecentAdapterDiagnostics.Empty();
	SessionId.Empty();
	PendingPrompt.Empty();
	InFlightPrompt.Empty();
	InitRpcId = 0;
	SessionRpcId = 0;
	PromptRpcId = 0;
	PendingPermissionIds.Empty();
	PendingPermissionAllowOptionIds.Empty();
	PendingWorldDataPermissionRequests.Empty();
	ApprovedWorldDataMcpToolCalls.Empty();
	bInitialized = false;
	bCreatingSession = false;
	bPromptInFlight = false;
}

void FWorldDataCodexACPClient::SetPermissionMode(EWorldDataCodexPermissionMode InMode)
{
	PermissionMode = InMode;
}

EWorldDataCodexPermissionMode FWorldDataCodexACPClient::GetPermissionMode() const
{
	return PermissionMode;
}

void FWorldDataCodexACPClient::SetAgent(EWorldDataAcpAgent InAgent)
{
	if (InAgent == Agent)
	{
		return;
	}

	Agent = InAgent;

	// A live session is bound to the previously launched adapter, so tear it down.
	// The next SendPrompt() re-runs EnsureProcess() and launches the new adapter.
	if (Process.IsValid())
	{
		Stop();
		EmitStatus(FString::Printf(TEXT("已切换到 %s，将在下次发送时重新连接。"), *GetAgentDisplayName()));
	}
}

EWorldDataAcpAgent FWorldDataCodexACPClient::GetAgent() const
{
	return Agent;
}

void FWorldDataCodexACPClient::SetModel(const FString& InModel)
{
	if (InModel == Model)
	{
		return;
	}

	Model = InModel;

	// The model is read at launch (env var), so a live session keeps the old model until relaunch.
	if (Process.IsValid())
	{
		Stop();
		EmitStatus(FString::Printf(TEXT("已切换模型为 %s，将在下次发送时重新连接。"),
			Model.IsEmpty() ? TEXT("(适配器默认)") : *Model));
	}
}

FString FWorldDataCodexACPClient::GetModel() const
{
	return Model;
}

FString FWorldDataCodexACPClient::GetAgentDisplayName() const
{
	switch (Agent)
	{
	case EWorldDataAcpAgent::Cursor:
		return TEXT("Cursor");
	case EWorldDataAcpAgent::ClaudeCode:
		return TEXT("Claude Code");
	case EWorldDataAcpAgent::Codex:
	default:
		return TEXT("Codex");
	}
}

void FWorldDataCodexACPClient::RespondToPermission(int32 RequestId, const FString& OptionId)
{
	TSharedPtr<FJsonValue>* PendingId = PendingPermissionIds.Find(RequestId);
	if (!PendingId)
	{
		EmitText(TEXT("\n\n[系统] 权限请求已失效或已处理。\n"));
		return;
	}

	const FString SelectedOptionId = OptionId.IsEmpty() ? TEXT("deny") : OptionId;
	SendPermissionOutcome(*PendingId, SelectedOptionId);

	const FString* AllowOptionId = PendingPermissionAllowOptionIds.Find(RequestId);
	const bool bAllowed = AllowOptionId && SelectedOptionId == *AllowOptionId;
	if (bAllowed)
	{
		if (const FWorldDataApprovedMcpToolCall* Approval = PendingWorldDataPermissionRequests.Find(RequestId))
		{
			ApprovedWorldDataMcpToolCalls.Add(*Approval);
		}
	}

	PendingPermissionIds.Remove(RequestId);
	PendingPermissionAllowOptionIds.Remove(RequestId);
	PendingWorldDataPermissionRequests.Remove(RequestId);

	EmitText(bAllowed
		? TEXT("\n\n[系统] 已允许本次工具权限请求。\n")
		: TEXT("\n\n[系统] 已拒绝本次工具权限请求。\n"));
}

bool FWorldDataCodexACPClient::IsRunning() const
{
	return Process.IsValid() && Process->IsRunning();
}

bool FWorldDataCodexACPClient::IsReady() const
{
	return IsRunning() && bInitialized && !SessionId.IsEmpty();
}

bool FWorldDataCodexACPClient::IsProcessing() const
{
	return bPromptInFlight || bCreatingSession || PendingPermissionIds.Num() > 0;
}

FString FWorldDataCodexACPClient::GetLastError() const
{
	return LastError;
}

bool FWorldDataCodexACPClient::EnsureProcess()
{
	if (Process.IsValid() && Process->IsRunning())
	{
		return true;
	}

	const FString AdapterBinary = FindAdapterBinary();
	if (AdapterBinary.IsEmpty())
	{
		const FString AdapterName = GetAdapterBaseName(Agent);
		Fail(FString::Printf(TEXT("没有找到 %s 适配器。请把 %s 可执行文件放到项目 Saved/UEBridgeMCP、项目 Binaries、插件 Binaries，或加入 PATH。"), *AdapterName, *AdapterName));
		return false;
	}

	FString WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::MakePlatformFilename(WorkingDir);
	RecentAdapterDiagnostics.Empty();

	if (GEngine)
	{
		GEngine->Exec(nullptr, TEXT("Log LogInteractiveProcess Error"));
	}

	if (Agent == EWorldDataAcpAgent::ClaudeCode)
	{
		const FString ClaudeCodeExecutable = FindClaudeCodeExecutable();
		if (!ClaudeCodeExecutable.IsEmpty())
		{
			FPlatformMisc::SetEnvironmentVar(TEXT("CLAUDE_CODE_EXECUTABLE"), *ClaudeCodeExecutable);
			EmitStatus(FString::Printf(TEXT("Claude Code CLI：%s"), *ClaudeCodeExecutable));
		}
		else
		{
			EmitStatus(TEXT("未找到 Claude Code CLI；claude-agent-acp 将尝试自行从 PATH 解析 claude。"));
		}
	}
	else if (Agent == EWorldDataAcpAgent::Cursor)
	{
		EmitStatus(FString::Printf(TEXT("Cursor ACP CLI：%s acp"), *AdapterBinary));
	}

	// ANTHROPIC_MODEL is Claude-specific. Keep it out of Codex/Cursor child processes.
	const bool bUseAnthropicModel = Agent == EWorldDataAcpAgent::ClaudeCode;
	FPlatformMisc::SetEnvironmentVar(TEXT("ANTHROPIC_MODEL"), bUseAnthropicModel ? *Model : TEXT(""));
	if (bUseAnthropicModel && !Model.IsEmpty())
	{
		EmitStatus(FString::Printf(TEXT("使用模型：%s"), *Model));
	}

	// The resolved adapter may be a native .exe (codex-acp) OR a Node CLI shim — npm installs
	// ACP adapters as .cmd/.bat wrappers (and sometimes bare .js entries). CreateProcess can only
	// launch a real image directly, so route shims through the right interpreter.
	FString LaunchExe = AdapterBinary;
	FString LaunchArgs;
	const FString ModelLaunchArgs = (!Model.IsEmpty() && Agent != EWorldDataAcpAgent::ClaudeCode)
		? FString::Printf(TEXT(" --model %s"), *Model)
		: FString();
	if (!ModelLaunchArgs.IsEmpty())
	{
		EmitStatus(FString::Printf(TEXT("使用模型：%s"), *Model));
	}
	const FString AdapterSubcommandArgs = Agent == EWorldDataAcpAgent::Cursor
		? ModelLaunchArgs + TEXT(" acp")
		: ModelLaunchArgs;
	{
		const FString Ext = FPaths::GetExtension(AdapterBinary).ToLower();
#if PLATFORM_WINDOWS
		if (Ext == TEXT("cmd") || Ext == TEXT("bat"))
		{
			const FString PowerShellWrapper = FPaths::ChangeExtension(AdapterBinary, TEXT("ps1"));
			if (FPaths::FileExists(PowerShellWrapper))
			{
				LaunchExe = TEXT("powershell.exe");
				LaunchArgs = AdapterSubcommandArgs.IsEmpty()
					? FString::Printf(TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\""), *PowerShellWrapper)
					: FString::Printf(TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\"%s"), *PowerShellWrapper, *AdapterSubcommandArgs);
			}
			else
			{
				LaunchExe = TEXT("cmd.exe");
				LaunchArgs = AdapterSubcommandArgs.IsEmpty()
					? FString::Printf(TEXT("/d /s /c \"\"%s\"\""), *AdapterBinary)
					: FString::Printf(TEXT("/d /s /c \"\"%s\"%s\""), *AdapterBinary, *AdapterSubcommandArgs);
			}
		}
		else
#endif
		if (Ext == TEXT("js") || Ext == TEXT("mjs") || Ext == TEXT("cjs"))
		{
			LaunchExe = TEXT("node");
			LaunchArgs = FString::Printf(TEXT("\"%s\"%s"), *AdapterBinary, *AdapterSubcommandArgs);
		}
		else if (!AdapterSubcommandArgs.IsEmpty())
		{
			LaunchArgs = AdapterSubcommandArgs.TrimStart();
		}
	}

	Process = MakeShared<FInteractiveProcess>(LaunchExe, LaunchArgs, WorkingDir, true, true);
	TWeakPtr<FWorldDataCodexACPClient> WeakSelf = AsShared();

	Process->OnOutput().BindLambda([WeakSelf](const FString& Output)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, Output]()
		{
			if (TSharedPtr<FWorldDataCodexACPClient> Self = WeakSelf.Pin())
			{
				Self->ConsumeOutput(Output);
			}
		});
	});

	Process->OnCompleted().BindLambda([WeakSelf](int32 ReturnCode, bool bCanceled)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, ReturnCode, bCanceled]()
		{
			if (TSharedPtr<FWorldDataCodexACPClient> Self = WeakSelf.Pin())
			{
				Self->Process.Reset();
				Self->StdoutBuffer.Empty();
				Self->SessionId.Empty();
				Self->InFlightPrompt.Empty();
				Self->InitRpcId = 0;
				Self->SessionRpcId = 0;
				Self->PromptRpcId = 0;
				Self->bInitialized = false;
				Self->bCreatingSession = false;
				Self->bPromptInFlight = false;
				Self->PendingPermissionIds.Empty();
				Self->PendingPermissionAllowOptionIds.Empty();
				Self->PendingWorldDataPermissionRequests.Empty();
				Self->ApprovedWorldDataMcpToolCalls.Empty();
				Self->Fail(FString::Printf(TEXT("%s 适配器已退出，返回码=%d，取消=%s。"), *Self->GetAgentDisplayName(), ReturnCode, bCanceled ? TEXT("true") : TEXT("false")));
			}
		});
	});

	if (!Process->Launch())
	{
		Process.Reset();
		Fail(FString::Printf(TEXT("启动 %s 适配器失败：%s"), *GetAgentDisplayName(), *AdapterBinary));
		return false;
	}

	EmitStatus(FString::Printf(TEXT("已启动 %s 适配器：%s"), *GetAgentDisplayName(), *AdapterBinary));
	UE_LOG(LogWorldDataCodexACP, Log, TEXT("Launched %s ACP adapter: %s %s"),
		*GetAgentDisplayName(),
		*LaunchExe,
		*LaunchArgs);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("protocolVersion"), 1);

	TSharedPtr<FJsonObject> Caps = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> FsCaps = MakeShared<FJsonObject>();
	FsCaps->SetBoolField(TEXT("readTextFile"), true);
	FsCaps->SetBoolField(TEXT("writeTextFile"), false);
	Caps->SetObjectField(TEXT("fs"), FsCaps);
	Caps->SetBoolField(TEXT("terminal"), false);
	Params->SetObjectField(TEXT("clientCapabilities"), Caps);

	TSharedPtr<FJsonObject> ClientInfo = MakeShared<FJsonObject>();
	ClientInfo->SetStringField(TEXT("name"), TEXT("worlddata"));
	ClientInfo->SetStringField(TEXT("title"), TEXT("WorldData UEBridgeMCP"));
	ClientInfo->SetStringField(TEXT("version"), TEXT("0.1.0"));
	Params->SetObjectField(TEXT("clientInfo"), ClientInfo);

	InitRpcId = SendRpc(TEXT("initialize"), Params);
	return true;
}

bool FWorldDataCodexACPClient::StartSessionIfReady()
{
	if (!bInitialized || !SessionId.IsEmpty() || bCreatingSession)
	{
		return !SessionId.IsEmpty();
	}

	if (!FWorldDataMCPServer::IsRunning())
	{
		FWorldDataMCPServer::Start(FWorldDataMCPServer::LoadConfiguredPort());
	}

	FString WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::MakePlatformFilename(WorkingDir);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("cwd"), WorkingDir);

	TArray<TSharedPtr<FJsonValue>> McpServers;
	if (FWorldDataMCPServer::IsRunning())
	{
		TSharedPtr<FJsonObject> McpServer = MakeShared<FJsonObject>();
		McpServer->SetStringField(TEXT("type"), TEXT("http"));
		McpServer->SetStringField(TEXT("name"), FWorldDataMCPServer::GetServerName());
		McpServer->SetStringField(TEXT("url"), FWorldDataMCPServer::GetMcpUrl());

		// Send the trusted-client token so the agent can use sensitive/mutating MCP tools
		// (the server gates those behind this header). Without it the in-panel agent could
		// only call read-only tools.
		TArray<TSharedPtr<FJsonValue>> Headers;
		const FString AccessToken = FWorldDataMCPServer::GetAccessToken();
		if (!AccessToken.IsEmpty())
		{
			TSharedPtr<FJsonObject> Header = MakeShared<FJsonObject>();
			Header->SetStringField(TEXT("name"), TEXT("X-WorldData-MCP-Token"));
			Header->SetStringField(TEXT("value"), AccessToken);
			Headers.Add(MakeShared<FJsonValueObject>(Header));
		}
		McpServer->SetArrayField(TEXT("headers"), Headers);
		McpServers.Add(MakeShared<FJsonValueObject>(McpServer));
	}
	Params->SetArrayField(TEXT("mcpServers"), McpServers);

	bCreatingSession = true;
	SessionRpcId = SendRpc(TEXT("session/new"), Params);
	EmitStatus(FString::Printf(TEXT("正在创建 %s ACP 会话..."), *GetAgentDisplayName()));
	return false;
}

void FWorldDataCodexACPClient::SendPendingPromptIfReady()
{
	if (PendingPrompt.IsEmpty() || SessionId.IsEmpty() || bPromptInFlight)
	{
		return;
	}

	FString PromptText =
		FString(TEXT("你是嵌入 Unreal Editor 的 WorldData 助手。优先使用已连接的 WorldData MCP tools/resources 理解当前项目。")) +
		TEXT("\n") + GetModeInstruction(PermissionMode) +
		TEXT("\n显示推理时只提供简短推理摘要，不输出完整私有思考链。") +
		TEXT("\n回答用户时使用中文，必要时说明你调用了哪些 MCP 工具。")
		TEXT("\n\n---\n用户消息：\n") + PendingPrompt;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("sessionId"), SessionId);

	TArray<TSharedPtr<FJsonValue>> Prompt;
	TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
	TextBlock->SetStringField(TEXT("type"), TEXT("text"));
	TextBlock->SetStringField(TEXT("text"), PromptText);
	Prompt.Add(MakeShared<FJsonValueObject>(TextBlock));
	Params->SetArrayField(TEXT("prompt"), Prompt);

	InFlightPrompt = PendingPrompt;
	PendingPrompt.Empty();
	bPromptInFlight = true;
	ApprovedWorldDataMcpToolCalls.Empty();
	PromptRpcId = SendRpc(TEXT("session/prompt"), Params);
	EmitStatus(FString::Printf(TEXT("已发送到 %s，等待回复..."), *GetAgentDisplayName()));
}

int32 FWorldDataCodexACPClient::SendRpc(const FString& Method, const TSharedPtr<FJsonObject>& Params)
{
	const int32 Id = NextRpcId++;
	TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Message->SetNumberField(TEXT("id"), Id);
	Message->SetStringField(TEXT("method"), Method);
	if (Params.IsValid())
	{
		Message->SetObjectField(TEXT("params"), Params);
	}
	SendRaw(JsonToString(Message));
	return Id;
}

void FWorldDataCodexACPClient::SendRpcResult(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Result)
{
	TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Message->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
	Message->SetObjectField(TEXT("result"), Result.IsValid() ? Result : MakeShared<FJsonObject>());
	SendRaw(JsonToString(Message));
}

void FWorldDataCodexACPClient::SendRpcError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& MessageText)
{
	TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetNumberField(TEXT("code"), Code);
	Error->SetStringField(TEXT("message"), MessageText);

	TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Message->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
	Message->SetObjectField(TEXT("error"), Error);
	SendRaw(JsonToString(Message));
}

void FWorldDataCodexACPClient::SendPermissionOutcome(const TSharedPtr<FJsonValue>& Id, const FString& OptionId)
{
	TSharedPtr<FJsonObject> Outcome = MakeShared<FJsonObject>();
	Outcome->SetStringField(TEXT("outcome"), TEXT("selected"));
	Outcome->SetStringField(TEXT("optionId"), OptionId.IsEmpty() ? TEXT("deny") : OptionId);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("outcome"), Outcome);
	SendRpcResult(Id, Result);
}

void FWorldDataCodexACPClient::SendRaw(const FString& Json)
{
	if (!Process.IsValid() || !Process->IsRunning())
	{
		Fail(FString::Printf(TEXT("%s 适配器进程未运行。"), *GetAgentDisplayName()));
		return;
	}

	UE_LOG(LogWorldDataCodexACP, Verbose, TEXT("ACP send: %s"), *RedactAcpFrameForLog(Json));
	Process->SendWhenReady(Json + TEXT("\n"));
}

void FWorldDataCodexACPClient::ConsumeOutput(const FString& Output)
{
	StdoutBuffer += Output;

	// Newline-delimited JSON-RPC frames (the common case). Scan the buffer once and
	// compact it a single time instead of reallocating the remainder for every line.
	int32 LineStart = 0;
	for (int32 Index = 0; Index < StdoutBuffer.Len(); ++Index)
	{
		if (StdoutBuffer[Index] == TEXT('\n'))
		{
			const FString Line = StdoutBuffer.Mid(LineStart, Index - LineStart).TrimStartAndEnd();
			if (!Line.IsEmpty())
			{
				ProcessLine(Line);
			}
			LineStart = Index + 1;
		}
	}
	if (LineStart > 0)
	{
		StdoutBuffer.RightChopInline(LineStart);
	}

	// Fallback for adapters that emit concatenated objects without newline framing.
	while (true)
	{
		int32 ObjectStart = INDEX_NONE;
		StdoutBuffer.FindChar('{', ObjectStart);
		if (ObjectStart == INDEX_NONE)
		{
			break;
		}
		if (ObjectStart > 0)
		{
			StdoutBuffer.RightChopInline(ObjectStart);
		}

		int32 Depth = 0;
		bool bInString = false;
		bool bEscape = false;
		int32 ObjectEnd = INDEX_NONE;
		for (int32 Index = 0; Index < StdoutBuffer.Len(); ++Index)
		{
			const TCHAR Ch = StdoutBuffer[Index];
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
			if (Ch == TEXT('{'))
			{
				++Depth;
			}
			else if (Ch == TEXT('}'))
			{
				--Depth;
				if (Depth == 0)
				{
					ObjectEnd = Index;
					break;
				}
			}
		}

		if (ObjectEnd == INDEX_NONE)
		{
			break;
		}

		const FString Frame = StdoutBuffer.Left(ObjectEnd + 1);
		StdoutBuffer.RightChopInline(ObjectEnd + 1);
		ProcessLine(Frame);
	}
}

void FWorldDataCodexACPClient::ProcessLine(const FString& Line)
{
	UE_LOG(LogWorldDataCodexACP, Verbose, TEXT("ACP recv: %s"), *RedactAcpFrameForLog(Line));

	TSharedPtr<FJsonObject> Message;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
	if (!FJsonSerializer::Deserialize(Reader, Message) || !Message.IsValid())
	{
		UE_LOG(LogWorldDataCodexACP, Warning, TEXT("ACP stdout line was not JSON: %s"), *Line);
		FString Diagnostic = Line.TrimStartAndEnd();
		const int32 MaxDiagnosticLen = 1200;
		if (Diagnostic.Len() > MaxDiagnosticLen)
		{
			Diagnostic = Diagnostic.Left(MaxDiagnosticLen) + FString::Printf(TEXT("...(+%d 字符)"), Diagnostic.Len() - MaxDiagnosticLen);
		}
		if (!Diagnostic.IsEmpty())
		{
			RecentAdapterDiagnostics.Add(Diagnostic);
			const int32 MaxDiagnostics = 8;
			while (RecentAdapterDiagnostics.Num() > MaxDiagnostics)
			{
				RecentAdapterDiagnostics.RemoveAt(0);
			}
		}
		return;
	}

	const TSharedPtr<FJsonValue> Id = ExtractRpcId(Message);
	if (Id.IsValid() && (Message->HasField(TEXT("result")) || Message->HasField(TEXT("error"))))
	{
		if (Id->Type == EJson::Number)
		{
			HandleRpcResponse(static_cast<int32>(Id->AsNumber()),
				Message->HasField(TEXT("result")) ? Message->GetObjectField(TEXT("result")) : nullptr,
				Message->HasField(TEXT("error")) ? Message->GetObjectField(TEXT("error")) : nullptr);
		}
		return;
	}

	if (Message->HasField(TEXT("method")))
	{
		HandleMethod(Message->GetStringField(TEXT("method")), Message);
	}
}

void FWorldDataCodexACPClient::HandleRpcResponse(int32 Id, const TSharedPtr<FJsonObject>& Result, const TSharedPtr<FJsonObject>& Error)
{
	if (Error.IsValid())
	{
		FString ErrorMessage = GetOptionalString(Error, TEXT("message"));
		if (ErrorMessage.IsEmpty())
		{
			ErrorMessage = TEXT("ACP returned an unknown error.");
		}

		FString ErrorDetails;
		const TSharedPtr<FJsonValue> ErrorData = Error->TryGetField(TEXT("data"));
		if (ErrorData.IsValid() && ErrorData->Type == EJson::Object)
		{
			ErrorDetails = GetOptionalString(ErrorData->AsObject(), TEXT("details"));
		}

		if (Id == PromptRpcId)
		{
			bPromptInFlight = false;
			ApprovedWorldDataMcpToolCalls.Empty();
			const bool bSessionNotFound =
				ErrorMessage.Contains(TEXT("Session not found"), ESearchCase::IgnoreCase)
				|| ErrorDetails.Contains(TEXT("Session not found"), ESearchCase::IgnoreCase);
			if (bSessionNotFound)
			{
				if (PendingPrompt.IsEmpty() && !InFlightPrompt.IsEmpty())
				{
					PendingPrompt = InFlightPrompt;
				}
				InFlightPrompt.Empty();
				SessionId.Empty();
				SessionRpcId = 0;
				EmitStatus(TEXT("ACP session was lost; recreating it."));
				StartSessionIfReady();
				SendPendingPromptIfReady();
				return;
			}
			InFlightPrompt.Empty();
		}
		if (Id == SessionRpcId)
		{
			bCreatingSession = false;
			SessionId.Empty();
		}
		Fail(ErrorDetails.IsEmpty()
			? ErrorMessage
			: FString::Printf(TEXT("%s: %s"), *ErrorMessage, *ErrorDetails));
		return;

	}

	if (Id == InitRpcId)
	{
		bInitialized = true;
		EmitStatus(TEXT("ACP 初始化完成。"));
		StartSessionIfReady();
		SendPendingPromptIfReady();
		return;
	}

	if (Id == SessionRpcId)
	{
		bCreatingSession = false;
		SessionId = GetOptionalString(Result, TEXT("sessionId"));
		if (SessionId.IsEmpty())
		{
			Fail(TEXT("session/new 没有返回 sessionId，可能是 codex-acp 协议版本不匹配。"));
			return;
		}
		EmitStatus(TEXT("Codex ACP 会话已创建。"));
		SendPendingPromptIfReady();
		return;
	}

	if (Id == PromptRpcId)
	{
		bPromptInFlight = false;
		InFlightPrompt.Empty();
		ApprovedWorldDataMcpToolCalls.Empty();
		EmitStatus(TEXT("Codex 回复完成。"));
		OnTurnComplete.ExecuteIfBound();
		return;
	}
}

void FWorldDataCodexACPClient::HandleMethod(const FString& Method, const TSharedPtr<FJsonObject>& Message)
{
	TSharedPtr<FJsonObject> Params = Message->HasField(TEXT("params")) ? Message->GetObjectField(TEXT("params")) : nullptr;
	TSharedPtr<FJsonValue> Id = ExtractRpcId(Message);
	const bool bHasId = Id.IsValid();

	if (Method == TEXT("session/update") && Params.IsValid())
	{
		const FString AcpSessionId = GetOptionalString(Params, TEXT("sessionId"));
		TSharedPtr<FJsonObject> Update = Params->HasField(TEXT("update")) ? Params->GetObjectField(TEXT("update")) : nullptr;
		if (Update.IsValid())
		{
			HandleSessionUpdate(AcpSessionId, Update);
		}
		return;
	}

	if (Method == TEXT("mcp/connect") && bHasId)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("connectionId"), FWorldDataMCPServer::GetServerName() + TEXT("-conn"));
		SendRpcResult(Id, Result);
		return;
	}

	if (Method == TEXT("mcp/message") && bHasId && Params.IsValid())
	{
		TSharedPtr<FJsonObject> McpMessage = Params->HasField(TEXT("message")) ? Params->GetObjectField(TEXT("message")) : nullptr;
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (McpMessage.IsValid())
		{
			const FString McpMethod = GetOptionalString(McpMessage, TEXT("method"));
			if ((PermissionMode == EWorldDataCodexPermissionMode::Plan || PermissionMode == EWorldDataCodexPermissionMode::Focus)
				&& McpMethod == TEXT("tools/call"))
			{
				const bool bFocusMode = PermissionMode == EWorldDataCodexPermissionMode::Focus;
				Result->SetObjectField(TEXT("message"), BuildJsonRpcErrorResponse(McpMessage, -32002,
					bFocusMode
						? TEXT("专注模式等待用户确认：工具调用被阻止。请先输出计划并询问是否执行。")
						: TEXT("计划模式已启用：工具调用被阻止。请只输出计划。")));
				EmitText(bFocusMode
					? TEXT("\n\n[系统] 专注模式等待用户确认，已阻止一次 MCP 工具调用。\n")
					: TEXT("\n\n[系统] 计划模式已阻止一次 MCP 工具调用。\n"));
			}
			else
			{
				bool bTrustedToolAccess = PermissionMode == EWorldDataCodexPermissionMode::Bypass;
				if (!bTrustedToolAccess && McpMethod == TEXT("tools/call"))
				{
					const FString McpToolName = GetMcpMessageToolName(McpMessage);
					const FString McpToolCallId = GetMcpMessageToolCallId(McpMessage);
					const FString AcpSessionId = GetOptionalString(Params, TEXT("sessionId"));
					const FString InputFingerprint = GetMcpMessageInputFingerprint(McpMessage);
					for (int32 ApprovalIndex = 0; ApprovalIndex < ApprovedWorldDataMcpToolCalls.Num(); ++ApprovalIndex)
					{
						if (ApprovedMcpToolCallMatches(ApprovedWorldDataMcpToolCalls[ApprovalIndex], McpToolName, McpToolCallId, AcpSessionId, InputFingerprint))
						{
							ApprovedWorldDataMcpToolCalls.RemoveAt(ApprovalIndex);
							bTrustedToolAccess = true;
							break;
						}
					}
				}
				Result->SetObjectField(TEXT("message"), FWorldDataMCPServer::ProcessJsonRpc(McpMessage, bTrustedToolAccess));
			}
		}
		SendRpcResult(Id, Result);
		return;
	}

	if (Method == TEXT("fs/read_text_file") && bHasId && Params.IsValid())
	{
		FString Path = GetOptionalString(Params, TEXT("path"));
		FString FullPath;
		FString Error;
		if (!ResolveAcpProjectTextFilePath(Path, FullPath, Error))
		{
			SendRpcError(Id, -32002, Error);
			return;
		}
		const int64 FileSize = IFileManager::Get().FileSize(*FullPath);
		if (FileSize < 0)
		{
			SendRpcError(Id, -32002, FString::Printf(TEXT("文件不存在：%s"), *MakeProjectRelative(FullPath)));
			return;
		}
		if (FileSize > MaxAcpReadTextFileBytes)
		{
			SendRpcError(Id, -32002, FString::Printf(TEXT("文件超过 %d 字节读取限制：%s"), MaxAcpReadTextFileBytes, *MakeProjectRelative(FullPath)));
			return;
		}

		FString Content;
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (FFileHelper::LoadFileToString(Content, *FullPath))
		{
			const int32 Line = GetOptionalInt(Params, TEXT("line"));
			const int32 Limit = GetOptionalInt(Params, TEXT("limit"));
			if (Line > 0 || Limit > 0)
			{
				TArray<FString> Lines;
				Content.ParseIntoArrayLines(Lines, false);
				const int32 Start = FMath::Max(0, Line - 1);
				const int32 End = Limit > 0 ? FMath::Min(Start + Limit, Lines.Num()) : Lines.Num();

				Content.Empty();
				for (int32 Index = Start; Index < End; ++Index)
				{
					if (!Content.IsEmpty())
					{
						Content += TEXT("\n");
					}
					Content += Lines[Index];
				}
			}
			Result->SetStringField(TEXT("path"), MakeProjectRelative(FullPath));
			Result->SetStringField(TEXT("content"), Content);
			SendRpcResult(Id, Result);
		}
		else
		{
			SendRpcError(Id, -32002, FString::Printf(TEXT("无法读取文件：%s"), *MakeProjectRelative(FullPath)));
		}
		return;
	}

	if (Method == TEXT("terminal/create") && bHasId)
	{
		SendRpcError(Id, -32002, TEXT("Shell/terminal 已在 WorldData MCP 控制台中禁用。请使用 MCP tools/resources。"));
		EmitText(TEXT("\n\n[系统] 已阻止一次 shell/terminal 请求。\n"));
		return;
	}

	if (Method == TEXT("session/request_permission") && bHasId && Params.IsValid())
	{
		const TArray<FWorldDataAcpPermissionOption> Options = ExtractPermissionOptions(Params);
		TSharedPtr<FJsonObject> ToolCall = Params->HasField(TEXT("toolCall")) ? Params->GetObjectField(TEXT("toolCall")) : nullptr;
		const bool bWorldDataMcp = LooksLikeWorldDataMcpRequest(ToolCall);
		const bool bShell = LooksLikeShellRequest(ToolCall);
		FString AllowOptionId = GetAllowOptionId(Options);
		if (AllowOptionId.IsEmpty())
		{
			AllowOptionId = TEXT("allow");
		}
		FString DenyOptionId = GetDenyOptionId(Options);
		if (DenyOptionId.IsEmpty())
		{
			DenyOptionId = TEXT("deny");
		}

		if (bShell && !bWorldDataMcp)
		{
			SendPermissionOutcome(Id, DenyOptionId);
			EmitText(FString::Printf(TEXT("\n\n[系统] 已阻止 shell/terminal 权限请求：%s\n"), *GetPermissionTitle(ToolCall)));
			return;
		}

		if (PermissionMode == EWorldDataCodexPermissionMode::Bypass)
		{
			// Auto-approve silently: emitting a "已自动允许权限请求" card per tool call floods
			// the conversation and splits the tool pills apart. The pill itself shows the call.
			SendPermissionOutcome(Id, AllowOptionId);
			return;
		}

		if (PermissionMode == EWorldDataCodexPermissionMode::Plan || PermissionMode == EWorldDataCodexPermissionMode::Focus)
		{
			SendPermissionOutcome(Id, DenyOptionId);
			if (PermissionMode == EWorldDataCodexPermissionMode::Focus)
			{
				EmitText(FString::Printf(TEXT("\n\n[系统] 专注模式等待用户确认，已拒绝权限请求：%s\n"), *GetPermissionTitle(ToolCall)));
			}
			else
			{
				EmitText(FString::Printf(TEXT("\n\n[系统] 计划模式已拒绝权限请求：%s\n"), *GetPermissionTitle(ToolCall)));
			}
			return;
		}

		if (!OnPermission.IsBound())
		{
			SendPermissionOutcome(Id, DenyOptionId);
			EmitText(FString::Printf(TEXT("\n\n[系统] 默认模式收到权限请求，但面板未绑定权限 UI，已拒绝：%s\n"), *GetPermissionTitle(ToolCall)));
			return;
		}

		FWorldDataAcpPermissionRequest Request;
		Request.RequestId = NextPermissionRequestId++;
		Request.Title = GetPermissionTitle(ToolCall);
		Request.ToolName = GetOptionalString(ToolCall, TEXT("name"));
		Request.ToolCallId = GetOptionalString(ToolCall, TEXT("toolCallId"));
		Request.SessionId = GetOptionalString(Params, TEXT("sessionId"));
		Request.AllowOptionId = AllowOptionId;
		Request.DenyOptionId = DenyOptionId;
		Request.Options = Options;

		PendingPermissionIds.Add(Request.RequestId, Id);
		PendingPermissionAllowOptionIds.Add(Request.RequestId, AllowOptionId);
		if (bWorldDataMcp)
		{
			PendingWorldDataPermissionRequests.Add(Request.RequestId, FWorldDataApprovedMcpToolCall{
				Request.ToolName,
				Request.ToolCallId,
				Request.SessionId,
				GetToolInputFingerprint(ToolCall)
			});
		}
		OnPermission.Execute(Request);
		EmitStatus(FString::Printf(TEXT("等待权限确认：%s"), *Request.Title));
		return;
	}

	if (bHasId)
	{
		SendRpcResult(Id, MakeShared<FJsonObject>());
	}
}

void FWorldDataCodexACPClient::HandleSessionUpdate(const FString& AcpSessionId, const TSharedPtr<FJsonObject>& Update)
{
	const FString UpdateType = GetOptionalString(Update, TEXT("sessionUpdate"));
	if (UpdateType == TEXT("agent_message_chunk"))
	{
		FString Text;
		if (TryGetSessionUpdateText(Update, Text))
		{
			EmitText(Text);
		}
		return;
	}

	// The agent's reasoning summary stream is displayed separately from the final answer.
	if (IsReasoningSummaryUpdateType(UpdateType))
	{
		FString Text;
		if (TryGetSessionUpdateText(Update, Text))
		{
			EmitThought(Text);
		}
		return;
	}

	if (UpdateType == TEXT("tool_call"))
	{
		FString Title = GetOptionalString(Update, TEXT("title"));
		if (Title.IsEmpty())
		{
			Title = GetOptionalString(Update, TEXT("toolCallId"));
		}
		FString Line = FString::Printf(TEXT("\n\n[工具] %s\n"), *CleanToolTitle(Title));
		// Append the call's input (e.g. the execute_python script) so high-risk calls are
		// auditable in the saved transcript. It rides after the name in the tool message text;
		// the pill renders only the first line and exposes the rest as a tooltip.
		const FString InputDigest = ExtractToolInputDigest(Update);
		if (!InputDigest.IsEmpty())
		{
			Line += InputDigest + TEXT("\n");
		}
		EmitText(Line);
		return;
	}

	if (UpdateType == TEXT("tool_call_update"))
	{
		// Successful completions produce no card — the call card already shows the tool ran,
		// and a bare "工具完成" line per call was pure noise. Only surface failures.
		const FString Status = GetOptionalString(Update, TEXT("status"));
		if (Status == TEXT("failed"))
		{
			FString Title = GetOptionalString(Update, TEXT("title"));
			if (Title.IsEmpty())
			{
				Title = GetOptionalString(Update, TEXT("toolCallId"));
			}
			const FString CleanName = CleanToolTitle(Title);
			EmitText(CleanName.IsEmpty()
				? FString(TEXT("\n[工具] 调用失败\n"))
				: FString::Printf(TEXT("\n[工具] %s 调用失败\n"), *CleanName));
		}
		return;
	}
}

FString FWorldDataCodexACPClient::GetAdapterBaseName(EWorldDataAcpAgent InAgent)
{
	switch (InAgent)
	{
	case EWorldDataAcpAgent::Cursor:
		return TEXT("cursor-agent");
	case EWorldDataAcpAgent::ClaudeCode:
		return TEXT("claude-agent-acp");
	case EWorldDataAcpAgent::Codex:
	default:
		return TEXT("codex-acp");
	}
}

FString FWorldDataCodexACPClient::FindAdapterBinary() const
{
	return FindAdapterBinaryForAgent(Agent);
}

FString FWorldDataCodexACPClient::FindAdapterBinaryForAgent(EWorldDataAcpAgent InAgent)
{
	// Per-agent adapter identity: the env override variable and known base binary names.
	// Both agents speak ACP over stdio; only the launched adapter differs.
	TArray<const TCHAR*> EnvVarNames;
	if (InAgent == EWorldDataAcpAgent::ClaudeCode)
	{
		EnvVarNames.Add(TEXT("CLAUDE_ACP_EXECUTABLE"));
	}
	else if (InAgent == EWorldDataAcpAgent::Cursor)
	{
		EnvVarNames.Add(TEXT("CURSOR_ACP_EXECUTABLE"));
		EnvVarNames.Add(TEXT("CURSOR_AGENT_EXECUTABLE"));
	}
	else
	{
		EnvVarNames.Add(TEXT("CODEX_ACP_EXECUTABLE"));
	}

	for (const TCHAR* EnvVarName : EnvVarNames)
	{
		const FString EnvPath = FPlatformMisc::GetEnvironmentVariable(EnvVarName);
		if (IsLaunchableAdapterCandidate(InAgent, EnvPath))
		{
			return EnvPath;
		}
	}

	TArray<FString> SearchDirs;
	SearchDirs.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("Binaries")));
	SearchDirs.Add(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP")));
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEBridgeMCP")))
	{
		SearchDirs.Add(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries")));
		SearchDirs.Add(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries"), TEXT("Win64")));
	}

	TArray<FString> BaseNames;
	AddAdapterBaseNames(InAgent, BaseNames);

	for (const FString& BaseName : BaseNames)
	{
		TArray<FString> LaunchableNames;
		AddLaunchableNames(BaseName, LaunchableNames);

		for (const FString& Dir : SearchDirs)
		{
			for (const FString& LaunchableName : LaunchableNames)
			{
				FString FullPath = FPaths::Combine(Dir, LaunchableName);
				FullPath = FPaths::ConvertRelativePathToFull(FullPath);
				FPaths::CollapseRelativeDirectories(FullPath);
				if (IsLaunchableAdapterCandidate(InAgent, FullPath))
				{
					return FullPath;
				}
			}
		}

		const FString PathMatch = ResolveOnPath(BaseName);
		if (!PathMatch.IsEmpty() && IsLaunchableAdapterCandidate(InAgent, PathMatch))
		{
			return PathMatch;
		}
	}

	return FString();
}

FString FWorldDataCodexACPClient::FindClaudeCodeExecutable()
{
	auto IsSpawnableClaudeCodePath = [](const FString& Candidate) -> bool
	{
		if (!PathExists(Candidate))
		{
			return false;
		}
#if PLATFORM_WINDOWS
		return FPaths::GetExtension(Candidate).ToLower() == TEXT("exe");
#else
		return true;
#endif
	};

	const FString EnvPath = FPlatformMisc::GetEnvironmentVariable(TEXT("CLAUDE_CODE_EXECUTABLE"));
	if (IsSpawnableClaudeCodePath(EnvPath))
	{
		return EnvPath;
	}

	const FString PathMatch = ResolveOnPath(TEXT("claude"));
	if (IsSpawnableClaudeCodePath(PathMatch))
	{
		return PathMatch;
	}

#if PLATFORM_WINDOWS
	TArray<FString> Candidates;
	auto AddCandidate = [&Candidates](FString Candidate)
	{
		if (Candidate.IsEmpty())
		{
			return;
		}
		FPaths::MakePlatformFilename(Candidate);
		Candidates.Add(Candidate);
	};

	auto AddClaudeCodeExecutables = [&AddCandidate](const FString& Root)
	{
		if (Root.IsEmpty())
		{
			return;
		}

		TArray<FString> Matches;
		IFileManager::Get().FindFilesRecursive(Matches, *Root, TEXT("claude.exe"), true, false, false);
		Matches.Sort();
		for (int32 Index = Matches.Num() - 1; Index >= 0; --Index)
		{
			AddCandidate(Matches[Index]);
		}
	};

	const FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
	if (!LocalAppData.IsEmpty())
	{
		const FString ClaudePackageRoaming = FPaths::Combine(
			LocalAppData,
			TEXT("Packages"),
			TEXT("Claude_pzs8sxrjxfjjc"),
			TEXT("LocalCache"),
			TEXT("Roaming"));
		AddCandidate(FPaths::Combine(ClaudePackageRoaming, TEXT("npm"), TEXT("node_modules"), TEXT("@anthropic-ai"), TEXT("claude-code"), TEXT("bin"), TEXT("claude.exe")));
		AddClaudeCodeExecutables(FPaths::Combine(ClaudePackageRoaming, TEXT("Claude"), TEXT("claude-code")));
	}

	const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
	if (!AppData.IsEmpty())
	{
		AddCandidate(FPaths::Combine(AppData, TEXT("npm"), TEXT("node_modules"), TEXT("@anthropic-ai"), TEXT("claude-code"), TEXT("bin"), TEXT("claude.exe")));
		AddClaudeCodeExecutables(FPaths::Combine(AppData, TEXT("Claude"), TEXT("claude-code")));
	}

	for (const FString& Candidate : Candidates)
	{
		if (IsSpawnableClaudeCodePath(Candidate))
		{
			return Candidate;
		}
	}
#endif

	return FString();
}

FString FWorldDataCodexACPClient::ResolveOnPath(const FString& Command)
{
#if PLATFORM_WINDOWS
	int32 ReturnCode = -1;
	FString StdOut;
	FString StdErr;
	FPlatformProcess::ExecProcess(TEXT("where.exe"), *Command, &ReturnCode, &StdOut, &StdErr);
	if (ReturnCode == 0)
	{
		TArray<FString> Lines;
		StdOut.ParseIntoArrayLines(Lines, true);
		// `where` can return several matches (e.g. a bare shell script + .cmd + .ps1). Prefer the
		// one we can actually launch: .exe, then .cmd/.bat, then anything else.
		auto Rank = [](const FString& Path) -> int32
		{
			const FString E = FPaths::GetExtension(Path).ToLower();
			if (E == TEXT("exe")) { return 0; }
			if (E == TEXT("cmd") || E == TEXT("bat")) { return 1; }
			if (E == TEXT("js") || E == TEXT("mjs") || E == TEXT("cjs")) { return 2; }
			return 3;
		};
		FString Best;
		int32 BestRank = MAX_int32;
		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (PathExists(Line))
			{
				const int32 R = Rank(Line);
				if (R < BestRank)
				{
					BestRank = R;
					Best = Line;
				}
			}
		}
		if (!Best.IsEmpty())
		{
			return Best;
		}
	}
#endif
	return FString();
}

FString FWorldDataCodexACPClient::JsonToString(const TSharedPtr<FJsonObject>& Object) const
{
	if (!Object.IsValid())
	{
		return TEXT("{}");
	}

	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Out;
}

void FWorldDataCodexACPClient::Fail(const FString& Message)
{
	FString FullMessage = Message;
	if (!RecentAdapterDiagnostics.IsEmpty())
	{
		FullMessage += TEXT("\n\n最近适配器输出：");
		for (const FString& Diagnostic : RecentAdapterDiagnostics)
		{
			FullMessage += TEXT("\n- ") + Diagnostic;
		}
	}

	LastError = FullMessage;
	UE_LOG(LogWorldDataCodexACP, Warning, TEXT("%s"), *FullMessage);
	if (OnError.IsBound())
	{
		OnError.Execute(FullMessage);
	}
}

void FWorldDataCodexACPClient::EmitStatus(const FString& Message)
{
	if (OnStatus.IsBound())
	{
		OnStatus.Execute(Message);
	}
}

void FWorldDataCodexACPClient::EmitText(const FString& Text)
{
	if (OnText.IsBound())
	{
		OnText.Execute(Text);
	}
}

void FWorldDataCodexACPClient::EmitThought(const FString& Text)
{
	if (OnThought.IsBound())
	{
		OnThought.Execute(Text);
	}
}
