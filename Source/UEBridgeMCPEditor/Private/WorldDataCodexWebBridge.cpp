#include "WorldDataCodexWebBridge.h"

void UWorldDataCodexWebBridge::SendPrompt(const FString& Prompt, const FString& ConversationId, const FString& PermissionMode)
{
	if (OnSendPrompt)
	{
		OnSendPrompt(Prompt, ConversationId, PermissionMode);
	}
}

void UWorldDataCodexWebBridge::SelectConversation(const FString& ConversationId)
{
	if (OnSelectConversation)
	{
		OnSelectConversation(ConversationId);
	}
}

void UWorldDataCodexWebBridge::SetPermissionMode(const FString& PermissionMode)
{
	if (OnSetPermissionMode)
	{
		OnSetPermissionMode(PermissionMode);
	}
}

void UWorldDataCodexWebBridge::SetBackend(const FString& BackendId)
{
	if (OnSetBackend)
	{
		OnSetBackend(BackendId);
	}
}

void UWorldDataCodexWebBridge::ResolvePermission(const bool bAllow)
{
	if (OnResolvePermission)
	{
		OnResolvePermission(bAllow);
	}
}

void UWorldDataCodexWebBridge::StartServer(const int32 Port)
{
	if (OnStartServer)
	{
		OnStartServer(Port);
	}
}

void UWorldDataCodexWebBridge::StopServer()
{
	if (OnStopServer)
	{
		OnStopServer();
	}
}

void UWorldDataCodexWebBridge::RefreshConnectionFiles()
{
	if (OnRefreshConnectionFiles)
	{
		OnRefreshConnectionFiles();
	}
}

void UWorldDataCodexWebBridge::ProvisionCli()
{
	if (OnProvisionCli)
	{
		OnProvisionCli();
	}
}

void UWorldDataCodexWebBridge::RequestDetail(const FString& DetailKind)
{
	if (OnRequestDetail)
	{
		OnRequestDetail(DetailKind);
	}
}

void UWorldDataCodexWebBridge::CopyToClipboard(const FString& CopyKind)
{
	if (OnCopyToClipboard)
	{
		OnCopyToClipboard(CopyKind);
	}
}

void UWorldDataCodexWebBridge::OpenFolder(const FString& FolderKind)
{
	if (OnOpenFolder)
	{
		OnOpenFolder(FolderKind);
	}
}

void UWorldDataCodexWebBridge::RotateAccessToken()
{
	if (OnRotateAccessToken)
	{
		OnRotateAccessToken();
	}
}

void UWorldDataCodexWebBridge::RequestInitialState()
{
	if (OnRequestInitialState)
	{
		OnRequestInitialState();
	}
}
