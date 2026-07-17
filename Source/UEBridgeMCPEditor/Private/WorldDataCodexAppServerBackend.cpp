#include "WorldDataCodexAppServerBackend.h"

#include "Async/Async.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#include "WorldDataMCPServer.h"
#include "WorldDataAgentLogRedaction.h"

DEFINE_LOG_CATEGORY_STATIC(LogWorldDataCodexAppServer, Log, All);

namespace
{
	static constexpr int32 GMaxAppServerStdoutCharacters = 512 * 1024;
	static constexpr int32 GMaxAppServerFrameCharacters = 256 * 1024;
	static constexpr int32 GMaxDisplayedAppServerTextCharacters = 8 * 1024;

	struct FAppServerSettings
	{
		FString Executable;
		FString ExpectedSha256;
		FString Model;
	};

	static FAppServerSettings GetSettings()
	{
		FAppServerSettings Settings;
		if (GConfig)
		{
			GConfig->GetString(TEXT("UEBridgeMCP.Security"), TEXT("CodexAppServerExecutable"), Settings.Executable, GGameIni);
			GConfig->GetString(TEXT("UEBridgeMCP.Security"), TEXT("CodexAppServerSha256"), Settings.ExpectedSha256, GGameIni);
			GConfig->GetString(TEXT("UEBridgeMCP.Agent"), TEXT("CodexAppServerModel"), Settings.Model, GGameIni);
		}
		Settings.Executable.TrimStartAndEndInline();
		Settings.ExpectedSha256.TrimStartAndEndInline();
		Settings.ExpectedSha256.ToLowerInline();
		Settings.Model.TrimStartAndEndInline();
		return Settings;
	}

	static bool IsCodexAppServerSha256(const FString& Value)
	{
		if (Value.Len() != 64)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!FChar::IsHexDigit(Character))
			{
				return false;
			}
		}
		return true;
	}

	static bool TryGetCodexAppServerFileSha256(const FString& Path, FString& OutHash)
	{
		OutHash.Empty();
#if PLATFORM_WINDOWS
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.IsEmpty())
		{
			return false;
		}

		BCRYPT_ALG_HANDLE Algorithm = nullptr;
		BCRYPT_HASH_HANDLE Hash = nullptr;
		DWORD ObjectLength = 0;
		DWORD ResultLength = 0;
		DWORD HashLength = 0;
		if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
			|| BCryptGetProperty(Algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&ObjectLength), sizeof(ObjectLength), &ResultLength, 0) < 0
			|| BCryptGetProperty(Algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&HashLength), sizeof(HashLength), &ResultLength, 0) < 0)
		{
			if (Algorithm) BCryptCloseAlgorithmProvider(Algorithm, 0);
			return false;
		}

		TArray<uint8> HashObject;
		TArray<uint8> Digest;
		HashObject.SetNumUninitialized(ObjectLength);
		Digest.SetNumUninitialized(HashLength);
		const bool bHashed = BCryptCreateHash(Algorithm, &Hash, HashObject.GetData(), HashObject.Num(), nullptr, 0, 0) >= 0
			&& BCryptHashData(Hash, Bytes.GetData(), Bytes.Num(), 0) >= 0
			&& BCryptFinishHash(Hash, Digest.GetData(), Digest.Num(), 0) >= 0;
		if (Hash) BCryptDestroyHash(Hash);
		BCryptCloseAlgorithmProvider(Algorithm, 0);
		if (!bHashed)
		{
			return false;
		}
		OutHash = BytesToHex(Digest.GetData(), Digest.Num()).ToLower();
		return true;
#else
		return false;
#endif
	}

	static void RedactDelimitedValue(FString& Text, const FString& Key)
	{
		int32 SearchFrom = 0;
		FString Lower = Text.ToLower();
		const FString LowerKey = Key.ToLower();
		while (true)
		{
			const int32 KeyIndex = Lower.Find(LowerKey, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (KeyIndex == INDEX_NONE)
			{
				return;
			}
			int32 ValueStart = KeyIndex + Key.Len();
			while (ValueStart < Text.Len() && (FChar::IsWhitespace(Text[ValueStart]) || Text[ValueStart] == TEXT(':') || Text[ValueStart] == TEXT('=')))
			{
				++ValueStart;
			}
			if (Text.Mid(ValueStart, 7).Equals(TEXT("bearer "), ESearchCase::IgnoreCase))
			{
				ValueStart += 7;
			}
			const TCHAR Quote = ValueStart < Text.Len() && (Text[ValueStart] == TEXT('\"') || Text[ValueStart] == TEXT('\'')) ? Text[ValueStart++] : 0;
			int32 ValueEnd = ValueStart;
			while (ValueEnd < Text.Len()
				&& (Quote ? Text[ValueEnd] != Quote : !FChar::IsWhitespace(Text[ValueEnd]) && Text[ValueEnd] != TEXT(',') && Text[ValueEnd] != TEXT('}') && Text[ValueEnd] != TEXT('&')))
			{
				++ValueEnd;
			}
			if (ValueEnd > ValueStart)
			{
				Text = Text.Left(ValueStart) + TEXT("[REDACTED]") + Text.Mid(ValueEnd);
				Lower = Text.ToLower();
				SearchFrom = ValueStart + 10;
			}
			else
			{
				SearchFrom = ValueStart + 1;
			}
		}
	}

	static FString RedactSensitiveText(FString Text)
	{
		return WorldDataAgentLogRedaction::Redact(MoveTemp(Text));
	}

	static TSharedPtr<FJsonObject> GetObjectFieldOrNull(const TSharedPtr<FJsonObject>& Object, const FString& Name)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(Name, Value) && Value && Value->IsValid() ? *Value : nullptr;
	}

	static FString GetAppServerModeInstruction(EWorldDataCodexPermissionMode Mode)
	{
		switch (Mode)
		{
		case EWorldDataCodexPermissionMode::Plan:
			return TEXT("Host execution mode: Plan. Do not call tools or make project changes; provide an actionable plan only.");
		case EWorldDataCodexPermissionMode::Bypass:
			return TEXT("Host execution mode: Bypass. You may use available MCP tools without waiting for extra host approval. UE MCP capability, revision, and high-risk approval policies still apply.");
		case EWorldDataCodexPermissionMode::Default:
		default:
			return TEXT("Host execution mode: Default. Follow normal permission and approval flows. UE MCP capability, revision, and high-risk approval policies still apply.");
		}
	}
}

FWorldDataCodexAppServerBackend::~FWorldDataCodexAppServerBackend()
{
	Stop();
}

bool FWorldDataCodexAppServerBackend::IsConfigured()
{
	const FAppServerSettings Settings = GetSettings();
	if (Settings.Executable.IsEmpty() || FPaths::IsRelative(Settings.Executable) || !IsCodexAppServerSha256(Settings.ExpectedSha256))
	{
		return false;
	}
	FString Executable = FPaths::ConvertRelativePathToFull(Settings.Executable);
	FPaths::CollapseRelativeDirectories(Executable);
	FPaths::MakePlatformFilename(Executable);
	FString ActualHash;
	return FPaths::FileExists(Executable)
		&& FPaths::GetExtension(Executable).ToLower() == TEXT("exe")
		&& TryGetCodexAppServerFileSha256(Executable, ActualHash)
		&& ActualHash.Equals(Settings.ExpectedSha256, ESearchCase::IgnoreCase);
}

FString FWorldDataCodexAppServerBackend::GetConfiguredModel()
{
	return GetSettings().Model;
}

void FWorldDataCodexAppServerBackend::SetConfiguredModel(const FString& Model)
{
	if (!GConfig)
	{
		return;
	}
	FString SanitizedModel = Model;
	SanitizedModel.TrimStartAndEndInline();
	SanitizedModel.LeftInline(128);
	GConfig->SetString(TEXT("UEBridgeMCP.Agent"), TEXT("CodexAppServerModel"), *SanitizedModel, GGameIni);
	GConfig->Flush(false, GGameIni);
}

FWorldDataAgentBackendCapabilities FWorldDataCodexAppServerBackend::GetCapabilities() const
{
	FWorldDataAgentBackendCapabilities Capabilities;
	// The selected model is written to the project configuration and is applied
	// when a fresh app-server thread is created. We do not claim that an active
	// thread can switch models in place.
	Capabilities.bSupportsModelSelection = true;
	Capabilities.bUsesNativeMcpConfiguration = true;
	// Do not advertise attachment or approval schema support until it has been
	// validated against the exact installed app-server schema.
	return Capabilities;
}

void FWorldDataCodexAppServerBackend::SendPrompt(const FString& Prompt)
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
	StartThreadIfReady();
	StartTurnIfReady();
}

void FWorldDataCodexAppServerBackend::Stop()
{
	++ProcessGeneration;
	if (Process.IsValid())
	{
		Process->Cancel(true);
		Process.Reset();
	}
	StdoutBuffer.Empty();
	PendingPrompt.Empty();
	ThreadId.Empty();
	ActiveExecutableDisplayPath.Empty();
	InitializeRpcId = 0;
	ThreadStartRpcId = 0;
	TurnStartRpcId = 0;
	bInitialized = false;
	bCreatingThread = false;
	bTurnInFlight = false;
}

void FWorldDataCodexAppServerBackend::SetPermissionMode(EWorldDataCodexPermissionMode InMode)
{
	PermissionMode = InMode;
}

EWorldDataCodexPermissionMode FWorldDataCodexAppServerBackend::GetPermissionMode() const
{
	return PermissionMode;
}

void FWorldDataCodexAppServerBackend::RespondToPermission(int32 RequestId, const FString& OptionId)
{
	EmitStatus(TEXT("Codex app-server permission response is not enabled until the installed schema is validated; request remains governed by Codex and UE MCP policies."));
}

bool FWorldDataCodexAppServerBackend::IsRunning() const
{
	return Process.IsValid() && Process->IsRunning();
}

bool FWorldDataCodexAppServerBackend::IsReady() const
{
	return IsRunning() && bInitialized && !ThreadId.IsEmpty();
}

bool FWorldDataCodexAppServerBackend::IsProcessing() const
{
	return bCreatingThread || bTurnInFlight;
}

FString FWorldDataCodexAppServerBackend::GetLastError() const
{
	return LastError;
}

bool FWorldDataCodexAppServerBackend::FindLaunch(FString& OutExecutable, FString& OutArguments, FString& OutDisplayPath) const
{
	OutExecutable.Empty();
	OutArguments.Empty();
	OutDisplayPath.Empty();
	const FAppServerSettings Settings = GetSettings();
	if (!IsConfigured())
	{
		return false;
	}
	FString Executable = FPaths::ConvertRelativePathToFull(Settings.Executable);
	FPaths::CollapseRelativeDirectories(Executable);
	FPaths::MakePlatformFilename(Executable);
	if (!FPaths::FileExists(Executable) || FPaths::GetExtension(Executable).ToLower() != TEXT("exe"))
	{
		return false;
	}
	FString ActualHash;
	if (!TryGetCodexAppServerFileSha256(Executable, ActualHash) || !ActualHash.Equals(Settings.ExpectedSha256, ESearchCase::IgnoreCase))
	{
		UE_LOG(LogWorldDataCodexAppServer, Error, TEXT("Rejected Codex app-server executable due to SHA-256 mismatch: %s"), *Executable);
		return false;
	}
	OutExecutable = Executable;
	OutArguments = TEXT("app-server --listen stdio://");
	OutDisplayPath = Executable;
	return true;
}

bool FWorldDataCodexAppServerBackend::EnsureProcess()
{
	if (IsRunning())
	{
		return true;
	}
	FString Executable;
	FString Arguments;
	FString DisplayPath;
	if (!FindLaunch(Executable, Arguments, DisplayPath))
	{
		Fail(TEXT("Codex app-server requires an absolute native codex.exe and a matching [UEBridgeMCP.Security] CodexAppServerSha256 pin. PATH and script shims are not used."));
		return false;
	}

	FString WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::MakePlatformFilename(WorkingDir);
	ActiveExecutableDisplayPath = DisplayPath;
	Process = MakeShared<FInteractiveProcess>(Executable, Arguments, WorkingDir, true, true);
	TWeakPtr<FWorldDataCodexAppServerBackend> WeakSelf = AsShared();
	const uint64 LaunchGeneration = ++ProcessGeneration;
	Process->OnOutput().BindLambda([WeakSelf, LaunchGeneration](const FString& Output)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, Output, LaunchGeneration]()
		{
			if (const TSharedPtr<FWorldDataCodexAppServerBackend> Self = WeakSelf.Pin())
			{
				if (Self->ProcessGeneration == LaunchGeneration) Self->ConsumeOutput(Output);
			}
		});
	});
	Process->OnCompleted().BindLambda([WeakSelf, LaunchGeneration](int32 ReturnCode, bool bCancelled)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, LaunchGeneration, ReturnCode, bCancelled]()
		{
			if (const TSharedPtr<FWorldDataCodexAppServerBackend> Self = WeakSelf.Pin())
			{
				if (Self->ProcessGeneration != LaunchGeneration) return;
				Self->Process.Reset();
				Self->bInitialized = false;
				Self->bCreatingThread = false;
				Self->bTurnInFlight = false;
				Self->ThreadId.Empty();
				Self->Fail(FString::Printf(TEXT("Codex app-server exited (%s), code=%d, cancelled=%s."), *Self->ActiveExecutableDisplayPath, ReturnCode, bCancelled ? TEXT("true") : TEXT("false")));
			}
		});
	});
	if (!Process->Launch())
	{
		Process.Reset();
		Fail(TEXT("Failed to launch the pinned Codex app-server executable."));
		return false;
	}

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> ClientInfo = MakeShared<FJsonObject>();
	ClientInfo->SetStringField(TEXT("name"), TEXT("worlddata_uebridge_mcp"));
	ClientInfo->SetStringField(TEXT("title"), TEXT("WorldData UEBridgeMCP"));
	ClientInfo->SetStringField(TEXT("version"), TEXT("0.3.0"));
	Params->SetObjectField(TEXT("clientInfo"), ClientInfo);
	InitializeRpcId = SendRequest(TEXT("initialize"), Params);
	EmitStatus(TEXT("Starting official Codex app-server. It uses Codex's native MCP configuration; ACP is not involved."));
	return true;
}

void FWorldDataCodexAppServerBackend::StartThreadIfReady()
{
	if (!bInitialized || !ThreadId.IsEmpty() || bCreatingThread)
	{
		return;
	}
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	const FAppServerSettings Settings = GetSettings();
	if (!Settings.Model.IsEmpty())
	{
		Params->SetStringField(TEXT("model"), Settings.Model);
	}
	bCreatingThread = true;
	ThreadStartRpcId = SendRequest(TEXT("thread/start"), Params);
}

void FWorldDataCodexAppServerBackend::StartTurnIfReady()
{
	if (PendingPrompt.IsEmpty() || ThreadId.IsEmpty() || bTurnInFlight)
	{
		return;
	}
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("threadId"), ThreadId);
	FString WorkingDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::MakePlatformFilename(WorkingDir);
	Params->SetStringField(TEXT("cwd"), WorkingDir);
	TArray<TSharedPtr<FJsonValue>> Input;
	TSharedRef<FJsonObject> TextInput = MakeShared<FJsonObject>();
	TextInput->SetStringField(TEXT("type"), TEXT("text"));
	TextInput->SetStringField(TEXT("text"), FString(TEXT("You are assisting inside Unreal Editor. Prefer the configured WorldData MCP tools/resources when relevant.\n")) + GetAppServerModeInstruction(PermissionMode) + TEXT("\n\n---\nUser message:\n") + PendingPrompt);
	Input.Add(MakeShared<FJsonValueObject>(TextInput));
	Params->SetArrayField(TEXT("input"), Input);
	PendingPrompt.Empty();
	bTurnInFlight = true;
	TurnStartRpcId = SendRequest(TEXT("turn/start"), Params);
	EmitStatus(TEXT("Prompt sent to Codex app-server; streaming turn events."));
}

int32 FWorldDataCodexAppServerBackend::SendRequest(const FString& Method, const TSharedPtr<FJsonObject>& Params)
{
	const int32 Id = NextRpcId++;
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetNumberField(TEXT("id"), Id);
	Message->SetStringField(TEXT("method"), Method);
	if (Params.IsValid()) Message->SetObjectField(TEXT("params"), Params.ToSharedRef());
	SendRaw(JsonToString(Message));
	return Id;
}

void FWorldDataCodexAppServerBackend::SendNotification(const FString& Method, const TSharedPtr<FJsonObject>& Params)
{
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("method"), Method);
	if (Params.IsValid()) Message->SetObjectField(TEXT("params"), Params.ToSharedRef());
	SendRaw(JsonToString(Message));
}

void FWorldDataCodexAppServerBackend::SendRaw(const FString& Json)
{
	if (!IsRunning())
	{
		Fail(TEXT("Codex app-server process is not running."));
		return;
	}
	UE_LOG(LogWorldDataCodexAppServer, Verbose, TEXT("app-server send: %d UTF-16 characters"), Json.Len());
	Process->SendWhenReady(Json + TEXT("\n"));
}

void FWorldDataCodexAppServerBackend::ConsumeOutput(const FString& Output)
{
	if (Output.Len() > GMaxAppServerFrameCharacters || StdoutBuffer.Len() + Output.Len() > GMaxAppServerStdoutCharacters)
	{
		Fail(TEXT("Codex app-server output exceeded the bounded JSONL buffer."));
		Stop();
		return;
	}
	StdoutBuffer += Output;
	int32 LineStart = 0;
	for (int32 Index = 0; Index < StdoutBuffer.Len(); ++Index)
	{
		if (StdoutBuffer[Index] == TEXT('\n'))
		{
			const FString Line = StdoutBuffer.Mid(LineStart, Index - LineStart).TrimStartAndEnd();
			if (!Line.IsEmpty()) ProcessLine(Line);
			LineStart = Index + 1;
		}
	}
	if (LineStart > 0) StdoutBuffer.RightChopInline(LineStart);
}

void FWorldDataCodexAppServerBackend::ProcessLine(const FString& Line)
{
	if (Line.Len() > GMaxAppServerFrameCharacters)
	{
		Fail(TEXT("Codex app-server returned an oversized JSONL frame."));
		Stop();
		return;
	}
	TSharedPtr<FJsonObject> Message;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
	if (!FJsonSerializer::Deserialize(Reader, Message) || !Message.IsValid())
	{
		UE_LOG(LogWorldDataCodexAppServer, Warning, TEXT("app-server stdout contained a non-JSON frame (%d UTF-16 characters)."), Line.Len());
		EmitText(FString::Printf(TEXT("\n\n[app-server] %s\n"), *RedactSensitiveText(Line).Left(GMaxDisplayedAppServerTextCharacters)));
		return;
	}
	if (const TSharedPtr<FJsonValue> Id = Message->TryGetField(TEXT("id")); Id.IsValid() && Id->Type == EJson::Number)
	{
		HandleResponse(static_cast<int32>(Id->AsNumber()), GetObjectFieldOrNull(Message, TEXT("result")), GetObjectFieldOrNull(Message, TEXT("error")));
		return;
	}
	FString Method;
	if (Message->TryGetStringField(TEXT("method"), Method))
	{
		HandleNotification(Method, GetObjectFieldOrNull(Message, TEXT("params")));
	}
}

void FWorldDataCodexAppServerBackend::HandleResponse(int32 Id, const TSharedPtr<FJsonObject>& Result, const TSharedPtr<FJsonObject>& Error)
{
	if (Error.IsValid())
	{
		FString Message;
		Error->TryGetStringField(TEXT("message"), Message);
		Fail(Message.IsEmpty() ? TEXT("Codex app-server returned an unknown JSON-RPC error.") : RedactSensitiveText(Message));
		return;
	}
	if (Id == InitializeRpcId)
	{
		bInitialized = true;
		SendNotification(TEXT("initialized"));
		EmitStatus(TEXT("Codex app-server initialized."));
		StartThreadIfReady();
		return;
	}
	if (Id == ThreadStartRpcId)
	{
		bCreatingThread = false;
		const TSharedPtr<FJsonObject> Thread = GetObjectFieldOrNull(Result, TEXT("thread"));
		if (Thread.IsValid()) Thread->TryGetStringField(TEXT("id"), ThreadId);
		if (ThreadId.IsEmpty())
		{
			Fail(TEXT("Codex app-server thread/start did not return thread.id. Generate and validate the schema for this Codex version."));
			return;
		}
		EmitStatus(TEXT("Codex app-server thread created."));
		StartTurnIfReady();
		return;
	}
	if (Id == TurnStartRpcId)
	{
		EmitStatus(TEXT("Codex turn accepted; waiting for streamed notifications."));
	}
}

void FWorldDataCodexAppServerBackend::HandleNotification(const FString& Method, const TSharedPtr<FJsonObject>& Params)
{
	if (Method == TEXT("item/agentMessage/delta") && Params.IsValid())
	{
		FString Delta;
		if (Params->TryGetStringField(TEXT("delta"), Delta) && !Delta.IsEmpty()) EmitText(Delta);
		return;
	}
	if (Method == TEXT("turn/completed"))
	{
		bTurnInFlight = false;
		EmitStatus(TEXT("Codex turn completed."));
		return;
	}
	if (Method == TEXT("turn/started"))
	{
		EmitStatus(TEXT("Codex turn started."));
		return;
	}
	if (Method == TEXT("item/started") || Method == TEXT("item/completed"))
	{
		EmitStatus(FString::Printf(TEXT("Codex app-server event: %s"), *Method));
	}
}

FString FWorldDataCodexAppServerBackend::JsonToString(const TSharedPtr<FJsonObject>& Object) const
{
	if (!Object.IsValid()) return TEXT("{}");
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Out;
}

void FWorldDataCodexAppServerBackend::Fail(const FString& Message)
{
	LastError = RedactSensitiveText(Message);
	PendingPrompt.Empty();
	ThreadId.Empty();
	bCreatingThread = false;
	bTurnInFlight = false;
	UE_LOG(LogWorldDataCodexAppServer, Warning, TEXT("%s"), *LastError);
	if (OnError.IsBound()) OnError.Execute(LastError);
}

void FWorldDataCodexAppServerBackend::EmitStatus(const FString& Message)
{
	if (OnStatus.IsBound()) OnStatus.Execute(RedactSensitiveText(Message));
}

void FWorldDataCodexAppServerBackend::EmitText(const FString& Text)
{
	if (OnText.IsBound()) OnText.Execute(RedactSensitiveText(Text));
}
