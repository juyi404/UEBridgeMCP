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
	bool SendMessage(const FString& ThreadId, const FString& Text, const FString& Model);

	UFUNCTION()
	void ResolveApproval(const FString& RequestId, bool bApproved);

private:
	struct FConversationSession
	{
		TArray<FWorldDataConversationItem> Items;
		FWorldDataAgentEvent PendingApproval;
		FString PendingFirstMessage;
		FString ActiveTurnId;
		FString ErrorCode;
		FString ErrorMessage;
		bool bDraft = false;
		bool bCreating = false;
		bool bLoading = false;
		bool bTurnActive = false;
		bool bLoaded = false;

		bool IsBusy() const { return bCreating || bLoading || bTurnActive; }
	};

	void HandleAgentEvent(const FWorldDataAgentEvent& Event);
	FString AddDraftConversation();
	void BindDraftConversation(const FString& DraftId, const FString& ThreadId);
	bool StartThread(const FString& DraftId);
	bool SendTurn(const FString& ThreadId, const FString& Text);
	FConversationSession& GetOrAddSession(const FString& ThreadId);
	FConversationSession* FindSession(const FString& ThreadId);
	const FConversationSession* FindSession(const FString& ThreadId) const;
	FConversationSession* FindSessionForEvent(const FWorldDataAgentEvent& Event);
	void SetUiError(const FString& Code, const FString& Message);

	FDelegateHandle EventHandle;
	TArray<FWorldDataThreadSummary> Threads;
	TMap<FString, FConversationSession> ConversationSessions;
	TMap<FString, FString> PendingRequestThreadIds;
	FString ActiveThreadId;
	FString SelectedModel;
	FString UiErrorCode;
	FString UiErrorMessage;
	FString LastHostSessionId;
	int64 LastHostSequence = 0;
	bool bConfiguring = false;
};
