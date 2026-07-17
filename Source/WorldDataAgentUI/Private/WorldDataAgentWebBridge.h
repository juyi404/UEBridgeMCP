#pragma once

#include "CoreMinimal.h"
#include "WorldDataAgentTypes.h"
#include "UObject/Object.h"
#include "WorldDataAgentWebBridge.generated.h"

UCLASS()
class UWorldDataAgentWebBridge final : public UObject
{
	GENERATED_BODY()

public:
	void Initialize();
	void Shutdown();

	UFUNCTION()
	FString GetState();

	UFUNCTION()
	void ConfigureRuntime();

	UFUNCTION()
	void RefreshThreads();

	UFUNCTION()
	void NewConversation();

	UFUNCTION()
	void ResumeThread(const FString& ThreadId, const FString& Model);

	UFUNCTION()
	void SendMessage(const FString& ThreadId, const FString& Text, const FString& Model);

	UFUNCTION()
	void ResolveApproval(const FString& RequestId, bool bApproved);

private:
	void HandleAgentEvent(const FWorldDataAgentEvent& Event);
	void AddDraftConversation();
	void BindDraftConversation(const FString& ThreadId);
	void StartThreadForPendingMessage();
	void SendTurn(const FString& Text);
	void SetUiError(const FString& Code, const FString& Message);

	FDelegateHandle EventHandle;
	TArray<FWorldDataThreadSummary> Threads;
	TArray<FWorldDataConversationItem> ConversationItems;
	FWorldDataAgentEvent PendingApproval;
	FString ActiveThreadId;
	FString SelectedModel;
	FString PendingFirstMessage;
	FString UiErrorCode;
	FString UiErrorMessage;
	bool bConfiguring = false;
	bool bBusy = false;
	bool bActiveThreadIsDraft = false;
};
