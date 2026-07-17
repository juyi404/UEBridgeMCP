#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/InteractiveProcess.h"
#include "WorldDataAgentBackend.h"

// Official Codex app-server backend. This speaks app-server JSONL directly;
// it is deliberately not an ACP adapter and never sends ACP session payloads.
class FWorldDataCodexAppServerBackend : public IWorldDataAgentBackend, public TSharedFromThis<FWorldDataCodexAppServerBackend>
{
public:
	~FWorldDataCodexAppServerBackend();

	static bool IsConfigured();
	static FString GetConfiguredModel();
	static void SetConfiguredModel(const FString& Model);

	virtual FString GetBackendId() const override { return TEXT("codex_app_server"); }
	virtual FString GetDisplayName() const override { return TEXT("Codex app-server"); }
	virtual FWorldDataAgentBackendCapabilities GetCapabilities() const override;
	virtual void SendPrompt(const FString& Prompt) override;
	virtual void Stop() override;
	virtual void SetPermissionMode(EWorldDataCodexPermissionMode InMode) override;
	virtual EWorldDataCodexPermissionMode GetPermissionMode() const override;
	virtual void RespondToPermission(int32 RequestId, const FString& OptionId) override;
	virtual bool IsRunning() const override;
	virtual bool IsReady() const override;
	virtual bool IsProcessing() const override;
	virtual FString GetLastError() const override;

private:
	bool EnsureProcess();
	void StartThreadIfReady();
	void StartTurnIfReady();
	bool FindLaunch(FString& OutExecutable, FString& OutArguments, FString& OutDisplayPath) const;

	int32 SendRequest(const FString& Method, const TSharedPtr<FJsonObject>& Params = nullptr);
	void SendNotification(const FString& Method, const TSharedPtr<FJsonObject>& Params = nullptr);
	void SendRaw(const FString& Json);
	void ConsumeOutput(const FString& Output);
	void ProcessLine(const FString& Line);
	void HandleResponse(int32 Id, const TSharedPtr<FJsonObject>& Result, const TSharedPtr<FJsonObject>& Error);
	void HandleNotification(const FString& Method, const TSharedPtr<FJsonObject>& Params);
	FString JsonToString(const TSharedPtr<FJsonObject>& Object) const;
	void Fail(const FString& Message);
	void EmitStatus(const FString& Message);
	void EmitText(const FString& Text);

	TSharedPtr<FInteractiveProcess> Process;
	FString StdoutBuffer;
	FString PendingPrompt;
	FString ThreadId;
	FString LastError;
	FString ActiveExecutableDisplayPath;
	EWorldDataCodexPermissionMode PermissionMode = EWorldDataCodexPermissionMode::Default;
	int32 NextRpcId = 1;
	int32 InitializeRpcId = 0;
	int32 ThreadStartRpcId = 0;
	int32 TurnStartRpcId = 0;
	uint64 ProcessGeneration = 0;
	bool bInitialized = false;
	bool bCreatingThread = false;
	bool bTurnInFlight = false;
};
