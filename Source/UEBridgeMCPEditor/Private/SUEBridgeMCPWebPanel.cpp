#include "SUEBridgeMCPWebPanel.h"

#include "UEBridgeMCPAgentController.h"
#include "UEBridgeMCPCoreModule.h"
#include "UEBridgeMCPStyle.h"
#include "WorldDataCodexWebBridge.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SWebBrowser.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	constexpr int32 MaxPromptCharacters = 64 * 1024;
	constexpr int32 MinMcpPort = 1024;
	constexpr int32 MaxMcpPort = 65535;

	FString ConversationEventText(const FString& Text)
	{
		FString Result = Text;
		Result.TrimStartAndEndInline();
		if (Result.StartsWith(TEXT("[")))
		{
			int32 CloseIndex = INDEX_NONE;
			if (Result.FindChar(TCHAR(']'), CloseIndex))
			{
				FString Remainder = Result.Mid(CloseIndex + 1);
				Remainder.TrimStartAndEndInline();
				return Remainder;
			}
		}
		return Result;
	}
}

void SUEBridgeMCPWebPanel::Construct(const FArguments& InArgs)
{
	FString Html;
	const FString HtmlPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Tools"), TEXT("ue-bridge-preview"), TEXT("index.html"));
	if (!FFileHelper::LoadFileToString(Html, *HtmlPath))
	{
		ChildSlot
		[
			SNew(SBorder)
			.Padding(16.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("无法加载 UEBridgeMCP HTML 界面：%s"), *HtmlPath)))
				.AutoWrapText(true)
			]
		];
		return;
	}

	AgentController = MakeUnique<FUEBridgeMCPAgentController>();
	StartAgentBackend();

	ChildSlot
	[
		SAssignNew(Browser, SWebBrowser)
		.InitialURL(TEXT("worlddata-ui://uebridge-mcp/index.html"))
		.ContentsToLoad(Html)
		.ShowControls(false)
		.ShowAddressBar(false)
		.ShowErrorMessage(true)
		.ShowInitialThrobber(true)
		.SupportsThumbMouseButtonNavigation(false)
		.BackgroundColor(FColor::White)
		.OnLoadCompleted(FSimpleDelegate::CreateSP(this, &SUEBridgeMCPWebPanel::HandleBrowserLoaded))
		.OnBeforeNavigation(SWebBrowser::FOnBeforeBrowse::CreateSP(this, &SUEBridgeMCPWebPanel::HandleBeforeNavigation))
		.OnBeforePopup(FOnBeforePopupDelegate::CreateSP(this, &SUEBridgeMCPWebPanel::HandleBeforePopup))
		.OnSuppressContextMenu(FOnSuppressContextMenu::CreateLambda([] { return true; }))
	];

	CreateWebBridge();
}

SUEBridgeMCPWebPanel::~SUEBridgeMCPWebPanel()
{
	if (Browser.IsValid() && WebBridge.IsValid())
	{
		Browser->UnbindUObject(TEXT("worlddatamcp"), WebBridge.Get(), true);
	}
	WebBridge.Reset();

	if (AgentController.IsValid())
	{
		AgentController->Stop();
	}
}

void SUEBridgeMCPWebPanel::CreateWebBridge()
{
	WebBridge = TStrongObjectPtr<UWorldDataCodexWebBridge>(NewObject<UWorldDataCodexWebBridge>(GetTransientPackage()));
	const TWeakPtr<SUEBridgeMCPWebPanel> WeakThis = StaticCastSharedRef<SUEBridgeMCPWebPanel>(AsShared());

	WebBridge->OnSendPrompt = [WeakThis](const FString& Prompt, const FString& ConversationId, const FString& PermissionMode)
	{
		if (const TSharedPtr<SUEBridgeMCPWebPanel> Self = WeakThis.Pin())
		{
			Self->HandleSendPrompt(Prompt, ConversationId, PermissionMode);
		}
	};
	WebBridge->OnSelectConversation = [WeakThis](const FString& ConversationId)
	{
		if (const TSharedPtr<SUEBridgeMCPWebPanel> Self = WeakThis.Pin())
		{
			Self->HandleSelectConversation(ConversationId);
		}
	};
	WebBridge->OnSetPermissionMode = [WeakThis](const FString& PermissionMode)
	{
		if (const TSharedPtr<SUEBridgeMCPWebPanel> Self = WeakThis.Pin())
		{
			Self->HandleSetPermissionMode(PermissionMode);
		}
	};
	WebBridge->OnResolvePermission = [WeakThis](const bool bAllow)
	{
		if (const TSharedPtr<SUEBridgeMCPWebPanel> Self = WeakThis.Pin())
		{
			Self->HandleResolvePermission(bAllow);
		}
	};
	WebBridge->OnStartServer = [WeakThis](const int32 Port)
	{
		if (const TSharedPtr<SUEBridgeMCPWebPanel> Self = WeakThis.Pin())
		{
			Self->HandleStartServer(Port);
		}
	};
	WebBridge->OnStopServer = [WeakThis]()
	{
		if (const TSharedPtr<SUEBridgeMCPWebPanel> Self = WeakThis.Pin())
		{
			Self->HandleStopServer();
		}
	};
	WebBridge->OnRefreshConnectionFiles = [WeakThis]()
	{
		if (const TSharedPtr<SUEBridgeMCPWebPanel> Self = WeakThis.Pin())
		{
			Self->HandleRefreshConnectionFiles();
		}
	};
	WebBridge->OnProvisionCli = [WeakThis]()
	{
		if (const TSharedPtr<SUEBridgeMCPWebPanel> Self = WeakThis.Pin())
		{
			Self->HandleProvisionCli();
		}
	};
	WebBridge->OnRequestDetail = [WeakThis](const FString& DetailKind)
	{
		if (const TSharedPtr<SUEBridgeMCPWebPanel> Self = WeakThis.Pin())
		{
			Self->HandleRequestDetail(DetailKind);
		}
	};
	WebBridge->OnCopyToClipboard = [WeakThis](const FString& CopyKind)
	{
		if (const TSharedPtr<SUEBridgeMCPWebPanel> Self = WeakThis.Pin())
		{
			Self->HandleCopyToClipboard(CopyKind);
		}
	};
	WebBridge->OnOpenFolder = [WeakThis](const FString& FolderKind)
	{
		if (const TSharedPtr<SUEBridgeMCPWebPanel> Self = WeakThis.Pin())
		{
			Self->HandleOpenFolder(FolderKind);
		}
	};
	WebBridge->OnRotateAccessToken = [WeakThis]()
	{
		if (const TSharedPtr<SUEBridgeMCPWebPanel> Self = WeakThis.Pin())
		{
			Self->HandleRotateAccessToken();
		}
	};
	WebBridge->OnRequestInitialState = [WeakThis]()
	{
		if (const TSharedPtr<SUEBridgeMCPWebPanel> Self = WeakThis.Pin())
		{
			Self->PublishInitialState();
		}
	};

	if (Browser.IsValid())
	{
		// Unreal lower-cases all binding names on the JavaScript side.  Keeping
		// this name lower-case avoids a second, implicit naming convention.
		Browser->BindUObject(TEXT("worlddatamcp"), WebBridge.Get(), true);
	}
}

void SUEBridgeMCPWebPanel::StartAgentBackend()
{
	if (!AgentController.IsValid())
	{
		return;
	}

	AgentBackend = AgentController->Start(
		CurrentPermissionMode,
		FWorldDataAcpTextDelegate::CreateSP(this, &SUEBridgeMCPWebPanel::HandleAcpText),
		FWorldDataAcpStatusDelegate::CreateSP(this, &SUEBridgeMCPWebPanel::HandleAcpStatus),
		FWorldDataAcpErrorDelegate::CreateSP(this, &SUEBridgeMCPWebPanel::HandleAcpError),
		FWorldDataAcpPermissionDelegate::CreateSP(this, &SUEBridgeMCPWebPanel::HandleAcpPermission));
}

void SUEBridgeMCPWebPanel::PublishInitialState()
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	IWorldDataMCPService& Service = GetWorldDataMCPService();
	Payload->SetBoolField(TEXT("serverRunning"), Service.IsRunning());
	Payload->SetNumberField(TEXT("port"), Service.IsRunning() ? Service.GetPort() : Service.LoadConfiguredPort());
	Payload->SetBoolField(TEXT("unsafePythonEnabled"), Service.IsUnsafePythonEnabled());
	Payload->SetStringField(TEXT("backend"), AgentBackend.IsValid() ? AgentBackend->GetDisplayName() : TEXT("Unavailable"));
	Payload->SetBoolField(TEXT("resetConversations"), true);
	DispatchEvent(TEXT("bootstrap"), Payload);
	PublishServerState();
}

void SUEBridgeMCPWebPanel::PublishServerState()
{
	IWorldDataMCPService& Service = GetWorldDataMCPService();
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetBoolField(TEXT("serverRunning"), Service.IsRunning());
	Payload->SetNumberField(TEXT("port"), Service.IsRunning() ? Service.GetPort() : Service.LoadConfiguredPort());
	Payload->SetBoolField(TEXT("unsafePythonEnabled"), Service.IsUnsafePythonEnabled());
	Payload->SetStringField(TEXT("backend"), AgentBackend.IsValid() ? AgentBackend->GetDisplayName() : TEXT("Unavailable"));
	DispatchEvent(TEXT("server_state"), Payload);
}

void SUEBridgeMCPWebPanel::DispatchEvent(const FString& Type, const TSharedPtr<FJsonObject>& Payload)
{
	TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
	Event->SetStringField(TEXT("type"), Type);
	if (Payload.IsValid())
	{
		Event->SetObjectField(TEXT("payload"), Payload);
	}

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Event, Writer))
	{
		return;
	}
	DispatchScript(FString::Printf(TEXT("window.worldDataHost && window.worldDataHost.receive(%s);"), *Json));
}

void SUEBridgeMCPWebPanel::DispatchScript(const FString& Script)
{
	if (!bPageReady || !Browser.IsValid())
	{
		if (PendingScripts.Num() < 128)
		{
			PendingScripts.Add(Script);
		}
		return;
	}
	Browser->ExecuteJavascript(Script);
}

void SUEBridgeMCPWebPanel::DispatchNotice(const FString& Text)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("text"), Text);
	DispatchEvent(TEXT("notice"), Payload);
}

void SUEBridgeMCPWebPanel::DispatchDetail(const FString& Title, const FString& Content)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("title"), Title);
	Payload->SetStringField(TEXT("content"), UEBridgeMCP::PrettyJson(Content));
	DispatchEvent(TEXT("detail"), Payload);
}

void SUEBridgeMCPWebPanel::HandleSendPrompt(const FString& Prompt, const FString& ConversationId, const FString& PermissionMode)
{
	FString TrimmedPrompt = Prompt;
	TrimmedPrompt.TrimStartAndEndInline();
	const FString NormalizedConversationId = NormalizeConversationId(ConversationId);
	if (TrimmedPrompt.IsEmpty())
	{
		DispatchNotice(TEXT("请输入消息后再发送。"));
		return;
	}
	if (TrimmedPrompt.Len() > MaxPromptCharacters)
	{
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("conversationId"), NormalizedConversationId);
		Payload->SetStringField(TEXT("text"), TEXT("消息过长；单条消息最多 64 KiB。"));
		DispatchEvent(TEXT("error"), Payload);
		return;
	}

	HandleSetPermissionMode(PermissionMode);
	if (!AgentBackend.IsValid())
	{
		StartAgentBackend();
	}
	if (!AgentBackend.IsValid())
	{
		HandleAcpError(TEXT("聊天后端尚未初始化。"));
		return;
	}
	if (AgentBackend->IsProcessing())
	{
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("conversationId"), NormalizedConversationId);
		Payload->SetStringField(TEXT("text"), TEXT("Codex 正在处理上一条消息，请等待完成或新建会话。"));
		DispatchEvent(TEXT("error"), Payload);
		return;
	}

	FConversationContext& Context = FindOrAddConversation(NormalizedConversationId);
	ActiveConversationId = NormalizedConversationId;
	AgentBackend->SetPermissionMode(CurrentPermissionMode);
	AgentBackend->SetConversationIdentity(Context.TaskId, Context.ThreadId);
	AgentBackend->SendPrompt(TrimmedPrompt);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("conversationId"), ActiveConversationId);
	Payload->SetStringField(TEXT("text"), TEXT("正在发送到 Codex…"));
	DispatchEvent(TEXT("status"), Payload);
}

void SUEBridgeMCPWebPanel::HandleSelectConversation(const FString& ConversationId)
{
	const FString NextConversationId = NormalizeConversationId(ConversationId);
	if (NextConversationId == ActiveConversationId)
	{
		return;
	}

	const FString PreviousConversationId = ActiveConversationId;
	const bool bWasProcessing = AgentBackend.IsValid() && AgentBackend->IsProcessing();
	if (bWasProcessing)
	{
		AgentBackend->Stop();
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("conversationId"), PreviousConversationId);
		Payload->SetStringField(TEXT("text"), TEXT("上一条 Codex 请求已取消。"));
		DispatchEvent(TEXT("cancelled"), Payload);
	}

	ActiveConversationId = NextConversationId;
	FindOrAddConversation(NextConversationId);
	PendingPermissionId = 0;
	PendingPermissionConversationId.Empty();
	PendingAllowOptionId.Empty();
	PendingDenyOptionId.Empty();
}

void SUEBridgeMCPWebPanel::HandleSetPermissionMode(const FString& PermissionMode)
{
	CurrentPermissionMode = ParsePermissionMode(PermissionMode);
	if (AgentBackend.IsValid())
	{
		AgentBackend->SetPermissionMode(CurrentPermissionMode);
	}
}

void SUEBridgeMCPWebPanel::HandleResolvePermission(const bool bAllow)
{
	if (PendingPermissionId == 0 || !AgentBackend.IsValid())
	{
		DispatchNotice(TEXT("当前没有待确认的权限请求。"));
		return;
	}

	AgentBackend->RespondToPermission(PendingPermissionId, bAllow ? PendingAllowOptionId : PendingDenyOptionId);
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("conversationId"), PendingPermissionConversationId);
	Payload->SetBoolField(TEXT("allowed"), bAllow);
	DispatchEvent(TEXT("permission_resolved"), Payload);
	PendingPermissionId = 0;
	PendingPermissionConversationId.Empty();
	PendingAllowOptionId.Empty();
	PendingDenyOptionId.Empty();
}

void SUEBridgeMCPWebPanel::HandleStartServer(int32 Port)
{
	if (Port < MinMcpPort || Port > MaxMcpPort)
	{
		DispatchNotice(TEXT("请输入 1024 至 65535 之间的端口号。"));
		return;
	}

	IWorldDataMCPService& Service = GetWorldDataMCPService();
	if (Service.IsRunning() && Service.GetPort() != Port)
	{
		Service.Stop();
	}
	if (!Service.IsRunning())
	{
		Service.Start(Port);
	}
	if (Service.IsRunning())
	{
		Service.RefreshConnectionFiles();
	}
	PublishServerState();
	DispatchNotice(Service.IsRunning() ? TEXT("MCP 服务已启动，并刷新了连接文件。") : TEXT("MCP 服务启动失败。"));
}

void SUEBridgeMCPWebPanel::HandleStopServer()
{
	GetWorldDataMCPService().Stop();
	PublishServerState();
	DispatchNotice(TEXT("MCP 服务已停止。"));
}

void SUEBridgeMCPWebPanel::HandleRefreshConnectionFiles()
{
	IWorldDataMCPService& Service = GetWorldDataMCPService();
	if (!Service.IsRunning())
	{
		DispatchNotice(TEXT("服务未运行，无法刷新连接文件。"));
		return;
	}
	Service.RefreshConnectionFiles();
	PublishServerState();
	DispatchNotice(TEXT("连接文件已刷新。"));
}

void SUEBridgeMCPWebPanel::HandleProvisionCli()
{
	IWorldDataMCPService& Service = GetWorldDataMCPService();
	if (!Service.IsRunning())
	{
		Service.StartConfigured();
	}
	if (!Service.IsRunning())
	{
		DispatchNotice(TEXT("MCP 服务启动失败，无法配置 CLI。"));
		DispatchDetail(TEXT("CLI 配置结果"), Service.GetStatusJson());
		return;
	}
	Service.ProvisionClientConfigurations();
	PublishServerState();
	DispatchDetail(TEXT("CLI 配置结果"), Service.GetCliSetupReportJson());
	DispatchNotice(TEXT("本地 CLI 连接配置已更新。"));
}

void SUEBridgeMCPWebPanel::HandleRequestDetail(const FString& DetailKind)
{
	IWorldDataMCPService& Service = GetWorldDataMCPService();
	if (DetailKind == TEXT("status"))
	{
		DispatchDetail(TEXT("服务状态"), Service.GetStatusJson());
	}
	else if (DetailKind == TEXT("project"))
	{
		DispatchDetail(TEXT("项目信息"), Service.GetProjectInfoJson());
	}
	else if (DetailKind == TEXT("bootstrap"))
	{
		DispatchDetail(TEXT("启动上下文"), Service.ReadResource(TEXT("worlddata://context/bootstrap")));
	}
	else if (DetailKind == TEXT("policy"))
	{
		DispatchDetail(TEXT("Codex 策略快照"), Service.ReadResource(TEXT("worlddata://codex/policy-snapshot")));
	}
	else if (DetailKind == TEXT("tools"))
	{
		DispatchDetail(TEXT("工具列表"), Service.GetToolDefinitionsJson());
	}
	else if (DetailKind == TEXT("resources"))
	{
		DispatchDetail(TEXT("资源列表"), Service.GetResourceListJson());
	}
	else
	{
		DispatchNotice(TEXT("不支持的详情类型。"));
	}
}

void SUEBridgeMCPWebPanel::HandleCopyToClipboard(const FString& CopyKind)
{
	IWorldDataMCPService& Service = GetWorldDataMCPService();
	if (CopyKind == TEXT("url") && Service.IsRunning())
	{
		UEBridgeMCP::CopyToClipboard(Service.GetMcpUrl());
		DispatchNotice(TEXT("MCP 连接地址已复制。"));
	}
	else if (CopyKind == TEXT("config") && Service.IsRunning())
	{
		UEBridgeMCP::CopyToClipboard(UEBridgeMCP::BuildClientConfigSnippet());
		DispatchNotice(TEXT("MCP 配置已复制。"));
	}
	else if (CopyKind == TEXT("python"))
	{
		const FString Token = Service.GetUnsafePythonCapabilityToken();
		if (Token.IsEmpty())
		{
			DispatchNotice(TEXT("Unsafe Python 未启用或 MCP 服务未运行。"));
			return;
		}
		UEBridgeMCP::CopyToClipboard(Token);
		DispatchNotice(TEXT("Python capability token 已复制。"));
	}
	else
	{
		DispatchNotice(TEXT("当前内容无法复制。"));
	}
}

void SUEBridgeMCPWebPanel::HandleOpenFolder(const FString& FolderKind)
{
	IWorldDataMCPService& Service = GetWorldDataMCPService();
	if (FolderKind == TEXT("project"))
	{
		UEBridgeMCP::ExploreFileParent(Service.GetClientConfigFilePath());
		DispatchNotice(TEXT("已打开项目目录。"));
	}
	else if (FolderKind == TEXT("saved"))
	{
		UEBridgeMCP::ExploreFileParent(Service.GetConnectionFilePath());
		DispatchNotice(TEXT("已打开 Saved 目录。"));
	}
}

void SUEBridgeMCPWebPanel::HandleRotateAccessToken()
{
	IWorldDataMCPService& Service = GetWorldDataMCPService();
	FString Error;
	if (!Service.RotateAccessToken(Error))
	{
		DispatchNotice(Error.IsEmpty() ? TEXT("MCP Token 轮换失败。") : Error);
		return;
	}
	Service.ProvisionClientConfigurations();
	PublishServerState();
	DispatchDetail(TEXT("MCP Token Rotation"), Service.GetCliSetupReportJson());
	DispatchNotice(TEXT("MCP Token 已轮换，已连接客户端需要重新建立会话。"));
}

void SUEBridgeMCPWebPanel::HandleAcpText(const FString& Text)
{
	if (Text.IsEmpty())
	{
		return;
	}

	FString EventText;
	const FString Role = GetRoleFromAgentText(Text, EventText);
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("conversationId"), GetActiveConversationId());
	Payload->SetStringField(TEXT("role"), Role);
	Payload->SetStringField(TEXT("text"), EventText);
	DispatchEvent(TEXT("message"), Payload);
}

void SUEBridgeMCPWebPanel::HandleAcpStatus(const FString& Text)
{
	const bool bComplete = Text.Contains(TEXT("回复完成"))
		|| Text.Contains(TEXT("turn completed"), ESearchCase::IgnoreCase);
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("conversationId"), GetActiveConversationId());
	Payload->SetStringField(TEXT("text"), Text);
	Payload->SetBoolField(TEXT("complete"), bComplete);
	DispatchEvent(TEXT("status"), Payload);
}

void SUEBridgeMCPWebPanel::HandleAcpError(const FString& Text)
{
	PendingPermissionId = 0;
	PendingPermissionConversationId.Empty();
	PendingAllowOptionId.Empty();
	PendingDenyOptionId.Empty();

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("conversationId"), GetActiveConversationId());
	Payload->SetStringField(TEXT("text"), Text);
	DispatchEvent(TEXT("error"), Payload);
}

void SUEBridgeMCPWebPanel::HandleAcpPermission(const FWorldDataAcpPermissionRequest& Request)
{
	PendingPermissionId = Request.RequestId;
	PendingPermissionConversationId = GetActiveConversationId();
	PendingAllowOptionId = Request.AllowOptionId.IsEmpty() ? TEXT("allow") : Request.AllowOptionId;
	PendingDenyOptionId = Request.DenyOptionId.IsEmpty() ? TEXT("deny") : Request.DenyOptionId;

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("conversationId"), PendingPermissionConversationId);
	Payload->SetStringField(TEXT("title"), Request.Title.IsEmpty() ? TEXT("需要权限确认") : Request.Title);
	Payload->SetStringField(TEXT("tool"), !Request.ToolName.IsEmpty() ? Request.ToolName : Request.ToolCallId);
	Payload->SetStringField(TEXT("description"), TEXT("默认模式下，此 MCP 工具调用会等待你的确认。"));
	DispatchEvent(TEXT("permission"), Payload);
}

bool SUEBridgeMCPWebPanel::HandleBeforeNavigation(const FString& Url, const FWebNavigationRequest& Request) const
{
	// The UI is loaded with LoadString under this private origin.  Do not turn
	// the embedded editor control into a general-purpose browser.
	return !Url.StartsWith(TEXT("worlddata-ui://"), ESearchCase::IgnoreCase);
}

bool SUEBridgeMCPWebPanel::HandleBeforePopup(FString Url, FString Target) const
{
	return true;
}

void SUEBridgeMCPWebPanel::HandleBrowserLoaded()
{
	bPageReady = true;
	PublishInitialState();

	TArray<FString> Scripts = MoveTemp(PendingScripts);
	PendingScripts.Empty();
	for (const FString& Script : Scripts)
	{
		if (Browser.IsValid())
		{
			Browser->ExecuteJavascript(Script);
		}
	}
}

SUEBridgeMCPWebPanel::FConversationContext& SUEBridgeMCPWebPanel::FindOrAddConversation(const FString& ConversationId)
{
	FConversationContext& Context = Conversations.FindOrAdd(NormalizeConversationId(ConversationId));
	if (Context.TaskId.IsEmpty())
	{
		Context.TaskId = FString::Printf(TEXT("task_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Context.ThreadId = FString::Printf(TEXT("thread_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}
	return Context;
}

FString SUEBridgeMCPWebPanel::NormalizeConversationId(const FString& ConversationId) const
{
	FString Result = ConversationId.Left(128);
	Result.TrimStartAndEndInline();
	for (TCHAR& Character : Result)
	{
		if (!FChar::IsAlnum(Character) && Character != TCHAR('-') && Character != TCHAR('_'))
		{
			Character = TCHAR('_');
		}
	}
	return Result.IsEmpty() ? TEXT("conversation-default") : Result;
}

EWorldDataCodexPermissionMode SUEBridgeMCPWebPanel::ParsePermissionMode(const FString& PermissionMode) const
{
	FString Normalized = PermissionMode;
	Normalized.TrimStartAndEndInline();
	Normalized.ToLowerInline();
	if (Normalized == TEXT("plan") || Normalized.Contains(TEXT("计划")))
	{
		return EWorldDataCodexPermissionMode::Plan;
	}
	if (Normalized == TEXT("bypass") || Normalized.Contains(TEXT("绕过")))
	{
		return EWorldDataCodexPermissionMode::Bypass;
	}
	return EWorldDataCodexPermissionMode::Default;
}

FString SUEBridgeMCPWebPanel::GetActiveConversationId() const
{
	return ActiveConversationId.IsEmpty() ? TEXT("conversation-default") : ActiveConversationId;
}

FString SUEBridgeMCPWebPanel::GetRoleFromAgentText(const FString& Text, FString& OutText) const
{
	FString Trimmed = Text;
	Trimmed.TrimStartAndEndInline();
	OutText = Text;
	if (!Trimmed.StartsWith(TEXT("[")))
	{
		return TEXT("assistant");
	}

	int32 CloseIndex = INDEX_NONE;
	if (!Trimmed.FindChar(TCHAR(']'), CloseIndex))
	{
		return TEXT("assistant");
	}

	const FString EventTag = Trimmed.Mid(1, CloseIndex - 1);
	if (EventTag.Contains(TEXT("错误")))
	{
		OutText = ConversationEventText(Trimmed);
		return TEXT("error");
	}
	if (EventTag.Contains(TEXT("工具")))
	{
		OutText = ConversationEventText(Trimmed);
		return TEXT("tool");
	}
	if (EventTag.Contains(TEXT("系统")))
	{
		OutText = ConversationEventText(Trimmed);
		return TEXT("system");
	}
	return TEXT("assistant");
}
