#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "WorldDataCodexWebBridge.generated.h"

/**
 * Minimal, deliberately narrow bridge from the bundled HTML page to the
 * editor host.  It is not an MCP endpoint and it never exposes the MCP access
 * token to JavaScript.
 */
UCLASS()
class UEBRIDGEMCPEDITOR_API UWorldDataCodexWebBridge final : public UObject
{
	GENERATED_BODY()

public:
	TFunction<void(const FString& Prompt, const FString& ConversationId, const FString& PermissionMode)> OnSendPrompt;
	TFunction<void(const FString& ConversationId)> OnSelectConversation;
	TFunction<void(const FString& PermissionMode)> OnSetPermissionMode;
	TFunction<void(bool bAllow)> OnResolvePermission;
	TFunction<void(int32 Port)> OnStartServer;
	TFunction<void()> OnStopServer;
	TFunction<void()> OnRefreshConnectionFiles;
	TFunction<void()> OnProvisionCli;
	TFunction<void(const FString& DetailKind)> OnRequestDetail;
	TFunction<void(const FString& CopyKind)> OnCopyToClipboard;
	TFunction<void(const FString& FolderKind)> OnOpenFolder;
	TFunction<void()> OnRotateAccessToken;
	TFunction<void()> OnRequestInitialState;

	UFUNCTION()
	void SendPrompt(const FString& Prompt, const FString& ConversationId, const FString& PermissionMode);

	UFUNCTION()
	void SelectConversation(const FString& ConversationId);

	UFUNCTION()
	void SetPermissionMode(const FString& PermissionMode);

	UFUNCTION()
	void ResolvePermission(bool bAllow);

	UFUNCTION()
	void StartServer(int32 Port);

	UFUNCTION()
	void StopServer();

	UFUNCTION()
	void RefreshConnectionFiles();

	UFUNCTION()
	void ProvisionCli();

	UFUNCTION()
	void RequestDetail(const FString& DetailKind);

	UFUNCTION()
	void CopyToClipboard(const FString& CopyKind);

	UFUNCTION()
	void OpenFolder(const FString& FolderKind);

	UFUNCTION()
	void RotateAccessToken();

	UFUNCTION()
	void RequestInitialState();
};
