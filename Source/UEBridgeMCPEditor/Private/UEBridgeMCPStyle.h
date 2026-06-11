#pragma once

#include "CoreMinimal.h"
#include "WorldDataMCPServer.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/SlateColor.h"
#include "Widgets/Notifications/SNotificationList.h"

// Shared visual palette and small editor utilities for the WorldData MCP panel.
// Kept separate from the panel widget and module so styling/utility concerns are
// reusable and the editor module file stays focused on lifecycle wiring.
namespace UEBridgeMCP
{
	static const FName PanelTabName(TEXT("UEBridgeMCPPanel"));

	namespace Palette
	{
		static FLinearColor Blend(const FLinearColor& Base, const FLinearColor& Overlay, float Amount)
		{
			const float T = FMath::Clamp(Amount, 0.0f, 1.0f);
			return FLinearColor(
				FMath::Lerp(Base.R, Overlay.R, T),
				FMath::Lerp(Base.G, Overlay.G, T),
				FMath::Lerp(Base.B, Overlay.B, T),
				1.0f);
		}

		static FLinearColor Background() { return FLinearColor::White; }
		static FLinearColor Surface() { return FLinearColor(0.985f, 0.988f, 0.993f, 1.0f); }
		static FLinearColor SurfaceRaised() { return FLinearColor(0.965f, 0.975f, 0.992f, 1.0f); }
		static FLinearColor Border() { return FLinearColor(0.84f, 0.88f, 0.94f, 1.0f); }
		static FLinearColor BorderStrong() { return FLinearColor(0.78f, 0.84f, 0.92f, 1.0f); }
		static FLinearColor Text() { return FLinearColor(0.08f, 0.10f, 0.14f, 1.0f); }
		static FLinearColor TextSoft() { return FLinearColor(0.25f, 0.31f, 0.39f, 1.0f); }
		static FLinearColor TextMuted() { return FLinearColor(0.45f, 0.51f, 0.61f, 1.0f); }
		static FLinearColor TextDisabled() { return FLinearColor(0.62f, 0.67f, 0.74f, 1.0f); }
		static FLinearColor Primary() { return FLinearColor(0.12f, 0.34f, 0.92f, 1.0f); }
		static FLinearColor PrimaryHover() { return FLinearColor(0.16f, 0.40f, 0.96f, 1.0f); }
		static FLinearColor PrimaryPressed() { return FLinearColor(0.08f, 0.25f, 0.72f, 1.0f); }
		static FLinearColor OnPrimary() { return FLinearColor::White; }
		static FLinearColor Success() { return FLinearColor(0.08f, 0.62f, 0.24f, 1.0f); }
		static FLinearColor Warning() { return FLinearColor(0.95f, 0.54f, 0.12f, 1.0f); }
		static FLinearColor Danger() { return FLinearColor(0.78f, 0.18f, 0.12f, 1.0f); }
		static FLinearColor Control() { return FLinearColor(0.93f, 0.95f, 0.98f, 1.0f); }
		static FLinearColor ControlHover() { return FLinearColor(0.96f, 0.98f, 1.0f, 1.0f); }
		static FLinearColor ControlPressed() { return FLinearColor(0.88f, 0.92f, 0.98f, 1.0f); }
		static FLinearColor Selection() { return FLinearColor(0.91f, 0.94f, 1.0f, 1.0f); }
	}

	static FSlateColor GetStatusColor()
	{
		return FWorldDataMCPServer::IsRunning()
			? FSlateColor(Palette::Success())
			: FSlateColor(Palette::Danger());
	}

	static FString BuildClientConfigSnippet()
	{
		return FString::Printf(
			TEXT("{\n")
			TEXT("  \"mcpServers\": {\n")
			TEXT("    \"%s\": {\n")
			TEXT("      \"type\": \"http\",\n")
			TEXT("      \"url\": \"%s\",\n")
			TEXT("      \"tool_timeout_sec\": 120\n")
			TEXT("    }\n")
			TEXT("  }\n")
			TEXT("}"),
			*FWorldDataMCPServer::GetServerName(),
			*FWorldDataMCPServer::GetMcpUrl());
	}

	static FString GetSettingsFilePath()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("settings.json"));
	}

	static void CopyToClipboard(const FString& Text)
	{
		FPlatformApplicationMisc::ClipboardCopy(*Text);
	}

	static void Notify(const FText& Message)
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = 2.5f;
		Info.bFireAndForget = true;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	static void ExploreFileParent(const FString& Path)
	{
		const FString Folder = FPaths::GetPath(Path);
		if (!Folder.IsEmpty())
		{
			FPlatformProcess::ExploreFolder(*Folder);
		}
	}

	static FString StripOuterQuotes(const FString& Text)
	{
		FString Trimmed = Text;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed.Len() >= 2 && Trimmed[0] == TCHAR('"') && Trimmed[Trimmed.Len() - 1] == TCHAR('"'))
		{
			Trimmed = Trimmed.Mid(1, Trimmed.Len() - 2);
			Trimmed.TrimStartAndEndInline();
		}
		return Trimmed;
	}

	static bool PathExists(const FString& Path)
	{
		const FString Trimmed = StripOuterQuotes(Path);
		return !Trimmed.IsEmpty() && (FPaths::FileExists(Trimmed) || IFileManager::Get().FileSize(*Trimmed) >= 0);
	}

	static FString ResolveCommandOnPath(const FString& Command)
	{
		const FString TrimmedCommand = StripOuterQuotes(Command);
		if (TrimmedCommand.IsEmpty())
		{
			return FString();
		}

		int32 ReturnCode = 1;
		FString StdOut;
		FString StdErr;
#if PLATFORM_WINDOWS
		FPlatformProcess::ExecProcess(TEXT("where.exe"), *TrimmedCommand, &ReturnCode, &StdOut, &StdErr);
#else
		FPlatformProcess::ExecProcess(TEXT("which"), *TrimmedCommand, &ReturnCode, &StdOut, &StdErr);
#endif
		if (ReturnCode != 0)
		{
			return FString();
		}

		TArray<FString> Lines;
		StdOut.ParseIntoArrayLines(Lines, true);
		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (PathExists(Line))
			{
				FPaths::MakePlatformFilename(Line);
				return Line;
			}
		}
		return FString();
	}

	static bool OpenExternalUrl(const FString& Url, FString& OutError)
	{
		OutError.Empty();
		FPlatformProcess::LaunchURL(*Url, nullptr, &OutError);
		return OutError.IsEmpty();
	}

	static FString GetCodexCliInstallUrl()
	{
		return TEXT("https://help.openai.com/en/articles/11096431");
	}

	static FString GetCursorCliInstallUrl()
	{
		return TEXT("https://docs.cursor.com/en/cli/installation");
	}

	static FString PrettyJson(const FString& JsonText)
	{
		FString Trimmed = JsonText;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed.IsEmpty())
		{
			return JsonText;
		}

		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);

		if (Trimmed[0] == TCHAR('['))
		{
			TArray<TSharedPtr<FJsonValue>> Array;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
			if (FJsonSerializer::Deserialize(Reader, Array) && FJsonSerializer::Serialize(Array, Writer))
			{
				return Out;
			}
		}
		else if (Trimmed[0] == TCHAR('{'))
		{
			TSharedPtr<FJsonObject> Object;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
			if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid() && FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
			{
				return Out;
			}
		}

		return JsonText;
	}
}
