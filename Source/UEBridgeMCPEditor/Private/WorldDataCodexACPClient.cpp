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

	static FString GetModeInstruction(EWorldDataCodexPermissionMode Mode)
	{
		switch (Mode)
		{
		case EWorldDataCodexPermissionMode::Plan:
			return TEXT("当前模式：计划模式。不要调用 MCP tools，不要执行任何会修改项目的动作；只给出清晰的步骤计划，并在需要执行时提示用户切换到默认或绕过模式。");
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
		for (const FWorldDataAcpPermissionOption& Option : Options)
		{
			const FString Probe = (Option.Kind + TEXT(" ") + Option.Name + TEXT(" ") + Option.OptionId).ToLower();
			if (Probe.Contains(LowerMatch))
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
		return OptionId.IsEmpty() && Options.Num() > 0 ? Options[0].OptionId : OptionId;
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
	SessionId.Empty();
	PendingPrompt.Empty();
	InitRpcId = 0;
	SessionRpcId = 0;
	PromptRpcId = 0;
	PendingPermissionIds.Empty();
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
	PendingPermissionIds.Remove(RequestId);

	const FString LowerOption = SelectedOptionId.ToLower();
	EmitText(LowerOption.Contains(TEXT("allow")) || LowerOption.Contains(TEXT("approve"))
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
		Fail(TEXT("没有找到 codex-acp adapter。请把 codex-acp.exe 放到项目 Saved/UEBridgeMCP、项目 Binaries、插件 Binaries，或加入 PATH。"));
		return false;
	}

	FString WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::MakePlatformFilename(WorkingDir);

	if (GEngine)
	{
		GEngine->Exec(nullptr, TEXT("Log LogInteractiveProcess Error"));
	}

	Process = MakeShared<FInteractiveProcess>(AdapterBinary, TEXT(""), WorkingDir, true, true);
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
				Self->bInitialized = false;
				Self->bCreatingSession = false;
				Self->bPromptInFlight = false;
				Self->PendingPermissionIds.Empty();
				Self->Fail(FString::Printf(TEXT("codex-acp 已退出，返回码=%d，取消=%s。"), ReturnCode, bCanceled ? TEXT("true") : TEXT("false")));
			}
		});
	});

	if (!Process->Launch())
	{
		Process.Reset();
		Fail(FString::Printf(TEXT("启动 codex-acp 失败：%s"), *AdapterBinary));
		return false;
	}

	EmitStatus(FString::Printf(TEXT("已启动 codex-acp：%s"), *AdapterBinary));

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
		McpServer->SetArrayField(TEXT("headers"), TArray<TSharedPtr<FJsonValue>>());
		McpServers.Add(MakeShared<FJsonValueObject>(McpServer));
	}
	Params->SetArrayField(TEXT("mcpServers"), McpServers);

	bCreatingSession = true;
	SessionRpcId = SendRpc(TEXT("session/new"), Params);
	EmitStatus(TEXT("正在创建 Codex ACP 会话..."));
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

	PendingPrompt.Empty();
	bPromptInFlight = true;
	PromptRpcId = SendRpc(TEXT("session/prompt"), Params);
	EmitStatus(TEXT("已发送到 Codex，等待回复..."));
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
		Fail(TEXT("codex-acp 进程未运行。"));
		return;
	}

	UE_LOG(LogWorldDataCodexACP, Verbose, TEXT("ACP send: %s"), *Json);
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
	UE_LOG(LogWorldDataCodexACP, Verbose, TEXT("ACP recv: %s"), *Line);

	TSharedPtr<FJsonObject> Message;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
	if (!FJsonSerializer::Deserialize(Reader, Message) || !Message.IsValid())
	{
		UE_LOG(LogWorldDataCodexACP, Warning, TEXT("ACP stdout line was not JSON: %s"), *Line);
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
		Fail(GetOptionalString(Error, TEXT("message")).IsEmpty() ? TEXT("ACP 返回未知错误。") : GetOptionalString(Error, TEXT("message")));
		if (Id == PromptRpcId)
		{
			bPromptInFlight = false;
		}
		if (Id == SessionRpcId)
		{
			bCreatingSession = false;
		}
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
		EmitStatus(TEXT("Codex 回复完成。"));
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
			if (PermissionMode == EWorldDataCodexPermissionMode::Plan && McpMethod == TEXT("tools/call"))
			{
				Result->SetObjectField(TEXT("message"), BuildJsonRpcErrorResponse(McpMessage, -32002, TEXT("计划模式已启用：工具调用被阻止。请只输出计划。")));
				EmitText(TEXT("\n\n[系统] 计划模式已阻止一次 MCP 工具调用。\n"));
			}
			else
			{
				Result->SetObjectField(TEXT("message"), FWorldDataMCPServer::ProcessJsonRpc(McpMessage));
			}
		}
		SendRpcResult(Id, Result);
		return;
	}

	if (Method == TEXT("fs/read_text_file") && bHasId && Params.IsValid())
	{
		FString Path = GetOptionalString(Params, TEXT("path"));
		FString Content;
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (FFileHelper::LoadFileToString(Content, *Path))
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
			Result->SetStringField(TEXT("content"), Content);
			SendRpcResult(Id, Result);
		}
		else
		{
			SendRpcError(Id, -32002, FString::Printf(TEXT("无法读取文件：%s"), *Path));
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

		if (bShell)
		{
			SendPermissionOutcome(Id, DenyOptionId);
			EmitText(FString::Printf(TEXT("\n\n[系统] 已阻止 shell/terminal 权限请求：%s\n"), *GetPermissionTitle(ToolCall)));
			return;
		}

		if (PermissionMode == EWorldDataCodexPermissionMode::Bypass)
		{
			SendPermissionOutcome(Id, AllowOptionId);
			EmitText(FString::Printf(TEXT("\n\n[系统] 绕过模式已自动允许权限请求：%s\n"), *GetPermissionTitle(ToolCall)));
			return;
		}

		if (PermissionMode == EWorldDataCodexPermissionMode::Plan)
		{
			SendPermissionOutcome(Id, DenyOptionId);
			EmitText(FString::Printf(TEXT("\n\n[系统] 计划模式已拒绝权限请求：%s\n"), *GetPermissionTitle(ToolCall)));
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
	if (UpdateType == TEXT("agent_message_chunk") && Update->HasField(TEXT("content")))
	{
		TSharedPtr<FJsonObject> Content = Update->GetObjectField(TEXT("content"));
		if (GetOptionalString(Content, TEXT("type")) == TEXT("text"))
		{
			EmitText(GetOptionalString(Content, TEXT("text")));
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
		EmitText(FString::Printf(TEXT("\n\n[工具] %s\n"), *Title));
		return;
	}

	if (UpdateType == TEXT("tool_call_update"))
	{
		const FString Status = GetOptionalString(Update, TEXT("status"));
		if (Status == TEXT("completed") || Status == TEXT("failed"))
		{
			EmitText(FString::Printf(TEXT("\n[工具%s]\n"), Status == TEXT("completed") ? TEXT("完成") : TEXT("失败")));
		}
		return;
	}
}

FString FWorldDataCodexACPClient::FindAdapterBinary() const
{
	const FString EnvPath = FPlatformMisc::GetEnvironmentVariable(TEXT("CODEX_ACP_EXECUTABLE"));
	if (PathExists(EnvPath))
	{
		return EnvPath;
	}

#if PLATFORM_WINDOWS
	const FString ExeName = TEXT("codex-acp.exe");
#else
	const FString ExeName = TEXT("codex-acp");
#endif

	TArray<FString> SearchDirs;
	SearchDirs.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("Binaries")));
	SearchDirs.Add(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP")));
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEBridgeMCP")))
	{
		SearchDirs.Add(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries")));
		SearchDirs.Add(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries"), TEXT("Win64")));
	}

	for (const FString& Dir : SearchDirs)
	{
		FString FullPath = FPaths::Combine(Dir, ExeName);
		FullPath = FPaths::ConvertRelativePathToFull(FullPath);
		FPaths::CollapseRelativeDirectories(FullPath);
		if (PathExists(FullPath))
		{
			return FullPath;
		}
	}

	return ResolveOnPath(ExeName);
}

FString FWorldDataCodexACPClient::ResolveOnPath(const FString& Command) const
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
		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (PathExists(Line))
			{
				return Line;
			}
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
	LastError = Message;
	UE_LOG(LogWorldDataCodexACP, Warning, TEXT("%s"), *Message);
	if (OnError.IsBound())
	{
		OnError.Execute(Message);
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
