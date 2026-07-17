#include "IWorldDataAgentUIModule.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SWebBrowser.h"
#include "UObject/StrongObjectPtr.h"
#include "WebBrowserModule.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "WorldDataAgentWebBridge.h"

DEFINE_LOG_CATEGORY_STATIC(LogWorldDataAgentUI, Log, All);

namespace
{
	class SWorldDataAgentWebPanel final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SWorldDataAgentWebPanel) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&)
		{
			IWebBrowserModule* WebBrowserModule = FModuleManager::LoadModulePtr<IWebBrowserModule>(TEXT("WebBrowser"));
			if (WebBrowserModule == nullptr || !WebBrowserModule->IsWebModuleAvailable())
			{
				UE_LOG(LogWorldDataAgentUI, Error, TEXT("The Unreal WebBrowser module could not initialize CEF."));
				ShowStartupError(TEXT("HTML panel startup failed: Unreal WebBrowser/CEF is unavailable. See LogWorldDataAgentUI and LogWebBrowser for details."));
				return;
			}

			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEBridgeMCP"));
			const FString HtmlPath = Plugin.IsValid()
				? FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Web"), TEXT("index.html")))
				: FString();
			if (HtmlPath.IsEmpty() || !FPaths::FileExists(HtmlPath))
			{
				UE_LOG(LogWorldDataAgentUI, Error, TEXT("WorldData Agent HTML entry point is missing: %s"), *HtmlPath);
				ShowStartupError(TEXT("WorldData Agent HTML resources are missing."));
				return;
			}

			Bridge.Reset(NewObject<UWorldDataAgentWebBridge>(GetTransientPackage()));
			Bridge->Initialize();
			FString BrowserPath = HtmlPath;
			BrowserPath.ReplaceInline(TEXT("\\"), TEXT("/"));
			const FString InitialUrl = FString::Printf(TEXT("file:///%s"), *BrowserPath);
			UE_LOG(LogWorldDataAgentUI, Log, TEXT("Opening WorldData Agent HTML panel: %s"), *InitialUrl);
			Browser = SNew(SWebBrowser)
				.InitialURL(InitialUrl)
				.ShowControls(false)
				.ShowAddressBar(false)
				.SupportsTransparency(false)
				.OnLoadStarted(FSimpleDelegate::CreateSP(this, &SWorldDataAgentWebPanel::HandleLoadStarted))
				.OnLoadCompleted(FSimpleDelegate::CreateSP(this, &SWorldDataAgentWebPanel::HandleLoadCompleted))
				.OnLoadError(FSimpleDelegate::CreateSP(this, &SWorldDataAgentWebPanel::HandleLoadError));
			Browser->BindUObject(TEXT("worlddata"), Bridge.Get(), true);
			ChildSlot[Browser.ToSharedRef()];
		}

		virtual ~SWorldDataAgentWebPanel() override
		{
			if (Browser.IsValid() && Bridge.IsValid()) Browser->UnbindUObject(TEXT("worlddata"), Bridge.Get(), true);
			if (Bridge.IsValid()) Bridge->Shutdown();
			Browser.Reset();
			Bridge.Reset();
		}

	private:
		void ShowStartupError(const FString& Message)
		{
			ChildSlot
			[
				SNew(SBorder)
				.Padding(24.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Message))
					.AutoWrapText(true)
				]
			];
		}

		void HandleLoadStarted() const
		{
			UE_LOG(LogWorldDataAgentUI, Log, TEXT("WorldData Agent HTML load started."));
		}

		void HandleLoadCompleted() const
		{
			UE_LOG(LogWorldDataAgentUI, Log, TEXT("WorldData Agent HTML load completed."));
		}

		void HandleLoadError() const
		{
			UE_LOG(LogWorldDataAgentUI, Error, TEXT("WorldData Agent HTML load failed."));
		}

		TSharedPtr<SWebBrowser> Browser;
		TStrongObjectPtr<UWorldDataAgentWebBridge> Bridge;
	};
}

class FWorldDataAgentUIModule final : public IWorldDataAgentUIModule
{
public:
	virtual TSharedRef<SWidget> CreatePanel() override
	{
		return SNew(SWorldDataAgentWebPanel);
	}
};

IMPLEMENT_MODULE(FWorldDataAgentUIModule, WorldDataAgentUI)
