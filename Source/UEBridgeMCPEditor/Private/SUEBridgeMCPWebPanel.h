#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"
#include "WorldDataAgentBackend.h"
#include "Widgets/SCompoundWidget.h"

class FUEBridgeMCPAgentController;
class SWebBrowser;
class UWorldDataCodexWebBridge;
struct FWebNavigationRequest;

/**
 * Hosts the bundled HTML conversation UI inside Unreal's CEF browser.  Slate
 * only owns the browser viewport; rendering and interaction live in
 * Tools/ue-bridge-preview/index.html.
 */
class SUEBridgeMCPWebPanel final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUEBridgeMCPWebPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SUEBridgeMCPWebPanel() override;

private:
	struct FConversationContext
	{
		FString TaskId;
		FString ThreadId;
	};

	void CreateWebBridge();
	void StartAgentBackend();
	void PublishInitialState();
	void PublishServerState();
	void DispatchEvent(const FString& Type, const TSharedPtr<class FJsonObject>& Payload = nullptr);
	void DispatchScript(const FString& Script);
	void DispatchNotice(const FString& Text);
	void DispatchDetail(const FString& Title, const FString& Content);

	void HandleSendPrompt(const FString& Prompt, const FString& ConversationId, const FString& PermissionMode);
	void HandleSelectConversation(const FString& ConversationId);
	void HandleSetPermissionMode(const FString& PermissionMode);
	void HandleResolvePermission(bool bAllow);
	void HandleStartServer(int32 Port);
	void HandleStopServer();
	void HandleRefreshConnectionFiles();
	void HandleProvisionCli();
	void HandleRequestDetail(const FString& DetailKind);
	void HandleCopyToClipboard(const FString& CopyKind);
	void HandleOpenFolder(const FString& FolderKind);
	void HandleRotateAccessToken();

	void HandleAcpText(const FString& Text);
	void HandleAcpStatus(const FString& Text);
	void HandleAcpError(const FString& Text);
	void HandleAcpPermission(const FWorldDataAcpPermissionRequest& Request);

	bool HandleBeforeNavigation(const FString& Url, const FWebNavigationRequest& Request) const;
	bool HandleBeforePopup(FString Url, FString Target) const;
	void HandleBrowserLoaded();

	FConversationContext& FindOrAddConversation(const FString& ConversationId);
	FString NormalizeConversationId(const FString& ConversationId) const;
	EWorldDataCodexPermissionMode ParsePermissionMode(const FString& PermissionMode) const;
	FString GetActiveConversationId() const;
	FString GetRoleFromAgentText(const FString& Text, FString& OutText) const;

	TSharedPtr<SWebBrowser> Browser;
	TUniquePtr<FUEBridgeMCPAgentController> AgentController;
	TSharedPtr<IWorldDataAgentBackend> AgentBackend;
	TStrongObjectPtr<UWorldDataCodexWebBridge> WebBridge;
	TMap<FString, FConversationContext> Conversations;
	TArray<FString> PendingScripts;
	FString ActiveConversationId;
	FString PendingPermissionConversationId;
	FString PendingAllowOptionId;
	FString PendingDenyOptionId;
	int32 PendingPermissionId = 0;
	EWorldDataCodexPermissionMode CurrentPermissionMode = EWorldDataCodexPermissionMode::Default;
	bool bPageReady = false;
};
