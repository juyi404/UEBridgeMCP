#include "SUEBridgeMCPPanel.h"

#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Input/DragAndDrop.h"
#include "InputCoreTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <shellapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#define LOCTEXT_NAMESPACE "UEBridgeMCPEditor"

namespace
{
	constexpr int32 MaxComposerAttachments = 12;
	constexpr int64 MaxInlineAttachmentBytes = 512 * 1024;
	constexpr int32 MaxAttachmentPromptChars = 80000;

	void StripOuterQuotesInline(FString& Value)
	{
		Value.TrimStartAndEndInline();
		while (Value.Len() >= 2)
		{
			const TCHAR First = Value[0];
			const TCHAR Last = Value[Value.Len() - 1];
			if ((First == TCHAR('"') && Last == TCHAR('"')) || (First == TCHAR('\'') && Last == TCHAR('\'')))
			{
				Value = Value.Mid(1, Value.Len() - 2);
				Value.TrimStartAndEndInline();
				continue;
			}
			break;
		}
	}

	FString NormalizeAttachmentCandidate(FString Candidate)
	{
		StripOuterQuotesInline(Candidate);
		if (Candidate.StartsWith(TEXT("file:///"), ESearchCase::IgnoreCase))
		{
			Candidate = Candidate.RightChop(8);
			Candidate.ReplaceInline(TEXT("/"), TEXT("\\"));
		}
		else if (Candidate.StartsWith(TEXT("file://"), ESearchCase::IgnoreCase))
		{
			Candidate = Candidate.RightChop(7);
			Candidate.ReplaceInline(TEXT("/"), TEXT("\\"));
		}

		FPaths::MakePlatformFilename(Candidate);
		Candidate = FPaths::ConvertRelativePathToFull(Candidate);
		FPaths::NormalizeFilename(Candidate);
		FPaths::MakePlatformFilename(Candidate);
		return Candidate;
	}

	bool AddUniquePath(TArray<FString>& Paths, const FString& Candidate)
	{
		FString Normalized = NormalizeAttachmentCandidate(Candidate);
		if (Normalized.IsEmpty() || !FPaths::FileExists(Normalized))
		{
			return false;
		}

		for (const FString& Existing : Paths)
		{
			if (FPaths::IsSamePath(Existing, Normalized))
			{
				return false;
			}
		}

		Paths.Add(MoveTemp(Normalized));
		return true;
	}

#if PLATFORM_WINDOWS
	void ReadClipboardFileDropPaths(TArray<FString>& OutPaths)
	{
		if (!OpenClipboard(GetActiveWindow()))
		{
			return;
		}

		HDROP DropHandle = static_cast<HDROP>(GetClipboardData(CF_HDROP));
		if (DropHandle != nullptr)
		{
			const UINT FileCount = DragQueryFile(DropHandle, 0xFFFFFFFF, nullptr, 0);
			for (UINT Index = 0; Index < FileCount; ++Index)
			{
				const UINT PathLength = DragQueryFile(DropHandle, Index, nullptr, 0);
				if (PathLength == 0)
				{
					continue;
				}

				TArray<TCHAR> Buffer;
				Buffer.SetNumZeroed(static_cast<int32>(PathLength) + 1);
				DragQueryFile(DropHandle, Index, Buffer.GetData(), PathLength + 1);
				AddUniquePath(OutPaths, FString(Buffer.GetData()));
			}
		}

		CloseClipboard();
	}
#endif
}

bool SUEBridgeMCPPanel::IsAttachmentTextFile(const FString& FullPath)
{
	const FString Extension = FPaths::GetExtension(FullPath).ToLower();
	static const TArray<FString> TextExtensions = {
		TEXT("bat"), TEXT("c"), TEXT("cmd"), TEXT("cpp"), TEXT("cs"), TEXT("css"),
		TEXT("csv"), TEXT("h"), TEXT("hpp"), TEXT("html"), TEXT("ini"), TEXT("js"),
		TEXT("json"), TEXT("jsx"), TEXT("log"), TEXT("md"), TEXT("ps1"), TEXT("py"),
		TEXT("sh"), TEXT("toml"), TEXT("ts"), TEXT("tsx"), TEXT("tsv"), TEXT("txt"),
		TEXT("uplugin"), TEXT("uproject"), TEXT("usf"), TEXT("ush"), TEXT("xml"),
		TEXT("yaml"), TEXT("yml")
	};
	return TextExtensions.Contains(Extension);
}

FString SUEBridgeMCPPanel::FormatAttachmentSize(int64 SizeBytes)
{
	if (SizeBytes < 0)
	{
		return TEXT("未知大小");
	}
	if (SizeBytes < 1024)
	{
		return FString::Printf(TEXT("%lld B"), SizeBytes);
	}
	if (SizeBytes < 1024 * 1024)
	{
		return FString::Printf(TEXT("%.1f KB"), static_cast<double>(SizeBytes) / 1024.0);
	}
	return FString::Printf(TEXT("%.1f MB"), static_cast<double>(SizeBytes) / (1024.0 * 1024.0));
}

bool SUEBridgeMCPPanel::TryExtractPastedFilePaths(const FString& ClipboardText, TArray<FString>& OutPaths)
{
	if (ClipboardText.IsEmpty())
	{
		return false;
	}

	const int32 BeforeCount = OutPaths.Num();
	FString NormalizedText = ClipboardText;
	NormalizedText.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	NormalizedText.ReplaceInline(TEXT("\r"), TEXT("\n"));

	TArray<FString> Lines;
	NormalizedText.ParseIntoArrayLines(Lines, true);
	if (Lines.IsEmpty())
	{
		Lines.Add(NormalizedText);
	}

	for (FString Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty())
		{
			continue;
		}

		AddUniquePath(OutPaths, Line);

		TArray<FString> SemicolonParts;
		Line.ParseIntoArray(SemicolonParts, TEXT(";"), true);
		if (SemicolonParts.Num() > 1)
		{
			for (const FString& Part : SemicolonParts)
			{
				AddUniquePath(OutPaths, Part);
			}
		}
	}

	return OutPaths.Num() > BeforeCount;
}

bool SUEBridgeMCPPanel::AddComposerAttachment(const FString& Path, FString* OutError)
{
	if (ComposerAttachments.Num() >= MaxComposerAttachments)
	{
		if (OutError)
		{
			*OutError = TEXT("附件数量已达上限");
		}
		return false;
	}

	FString FullPath = NormalizeAttachmentCandidate(Path);
	if (!FPaths::FileExists(FullPath))
	{
		if (OutError)
		{
			*OutError = TEXT("文件不存在");
		}
		return false;
	}

	for (const FComposerAttachment& Existing : ComposerAttachments)
	{
		if (FPaths::IsSamePath(Existing.Path, FullPath))
		{
			if (OutError)
			{
				*OutError = TEXT("文件已添加");
			}
			return false;
		}
	}

	FComposerAttachment Attachment;
	Attachment.Path = FullPath;
	Attachment.Name = FPaths::GetCleanFilename(FullPath);
	Attachment.SizeBytes = IFileManager::Get().FileSize(*FullPath);

	if (Attachment.SizeBytes < 0)
	{
		Attachment.Error = TEXT("无法读取大小");
	}
	else if (Attachment.SizeBytes > MaxInlineAttachmentBytes)
	{
		Attachment.Error = FString::Printf(TEXT("超过 %s"), *FormatAttachmentSize(MaxInlineAttachmentBytes));
	}
	else if (!IsAttachmentTextFile(FullPath))
	{
		Attachment.Error = TEXT("非文本文件");
	}
	else if (FFileHelper::LoadFileToString(Attachment.Content, *FullPath))
	{
		Attachment.bInlineContent = true;
	}
	else
	{
		Attachment.Error = TEXT("读取失败");
	}

	ComposerAttachments.Add(MoveTemp(Attachment));
	RebuildComposerAttachments();
	return true;
}

int32 SUEBridgeMCPPanel::AddComposerAttachments(const TArray<FString>& Paths)
{
	int32 AddedCount = 0;
	for (const FString& Path : Paths)
	{
		FString Error;
		if (AddComposerAttachment(Path, &Error))
		{
			++AddedCount;
		}
	}

	if (AddedCount > 0)
	{
		SetLastAction(FText::FromString(FString::Printf(TEXT("已附加 %d 个文件。"), AddedCount)));
	}
	return AddedCount;
}

int32 SUEBridgeMCPPanel::AddComposerAttachmentsFromClipboard()
{
	TArray<FString> Paths;

#if PLATFORM_WINDOWS
	ReadClipboardFileDropPaths(Paths);
#endif

	FString ClipboardText;
	FPlatformApplicationMisc::ClipboardPaste(ClipboardText);
	TryExtractPastedFilePaths(ClipboardText, Paths);

	return AddComposerAttachments(Paths);
}

void SUEBridgeMCPPanel::ClearComposerAttachments()
{
	ComposerAttachments.Empty();
	RebuildComposerAttachments();
}

FString SUEBridgeMCPPanel::BuildAttachmentPromptBlock() const
{
	if (ComposerAttachments.IsEmpty())
	{
		return FString();
	}

	FString Block = TEXT("用户附加了以下文件。请把这些内容作为当前消息的一部分；如果只提供了路径，请先根据路径和用户意图判断是否需要读取或询问。\n");
	for (int32 Index = 0; Index < ComposerAttachments.Num(); ++Index)
	{
		const FComposerAttachment& Attachment = ComposerAttachments[Index];
		Block += FString::Printf(
			TEXT("\n[%d] %s\n路径: %s\n大小: %s\n"),
			Index + 1,
			*Attachment.Name,
			*Attachment.Path,
			*FormatAttachmentSize(Attachment.SizeBytes));

		if (Attachment.bInlineContent)
		{
			FString Content = Attachment.Content;
			Content.ReplaceInline(TEXT("```"), TEXT("` ` `"));

			const int32 RemainingChars = MaxAttachmentPromptChars - Block.Len();
			if (RemainingChars < 400)
			{
				Block += TEXT("内容: [后续附件内容已截断]\n");
				break;
			}

			const int32 MaxContentChars = RemainingChars - 220;
			if (Content.Len() > MaxContentChars)
			{
				Content = Content.Left(MaxContentChars) + TEXT("\n[附件内容已截断]\n");
			}

			const FString Extension = FPaths::GetExtension(Attachment.Path);
			Block += FString::Printf(TEXT("内容:\n```%s\n%s\n```\n"), *Extension, *Content);
		}
		else
		{
			const FString Reason = Attachment.Error.IsEmpty() ? TEXT("未内联") : Attachment.Error;
			Block += FString::Printf(TEXT("内容: 未内联（%s），已提供文件路径。\n"), *Reason);
		}
	}

	return Block;
}

FString SUEBridgeMCPPanel::BuildVisibleAttachmentSummary() const
{
	if (ComposerAttachments.IsEmpty())
	{
		return FString();
	}

	FString Summary = TEXT("\n\n附件：");
	for (const FComposerAttachment& Attachment : ComposerAttachments)
	{
		Summary += FString::Printf(
			TEXT("\n- %s (%s)"),
			*Attachment.Name,
			*FormatAttachmentSize(Attachment.SizeBytes));
		if (!Attachment.bInlineContent && !Attachment.Error.IsEmpty())
		{
			Summary += FString::Printf(TEXT(" - %s"), *Attachment.Error);
		}
	}
	return Summary;
}

FReply SUEBridgeMCPPanel::OnAttachFileClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		SetLastAction(LOCTEXT("AttachDialogUnavailable", "文件选择器不可用。"));
		return FReply::Handled();
	}

	TArray<FString> OutFiles;
	const bool bOpened = DesktopPlatform->OpenFileDialog(
		FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared()),
		TEXT("选择要附加的文件"),
		FPaths::ProjectDir(),
		TEXT(""),
		TEXT("All files (*.*)|*.*"),
		EFileDialogFlags::Multiple,
		OutFiles);

	if (bOpened)
	{
		const int32 AddedCount = AddComposerAttachments(OutFiles);
		if (AddedCount == 0)
		{
			SetLastAction(LOCTEXT("NoAttachableFilesSelected", "没有添加新的文件。"));
		}
	}
	FocusComposer();
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnRemoveAttachmentClicked(int32 AttachmentIndex)
{
	if (ComposerAttachments.IsValidIndex(AttachmentIndex))
	{
		ComposerAttachments.RemoveAt(AttachmentIndex);
		RebuildComposerAttachments();
		SetLastAction(LOCTEXT("AttachmentRemovedAction", "已移除附件。"));
	}
	FocusComposer();
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnComposerKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	if ((KeyEvent.IsControlDown() || KeyEvent.IsCommandDown()) && KeyEvent.GetKey() == EKeys::V)
	{
		const int32 AddedCount = AddComposerAttachmentsFromClipboard();
		if (AddedCount > 0)
		{
			FocusComposer();
			return FReply::Handled();
		}
	}
	return FReply::Unhandled();
}

FReply SUEBridgeMCPPanel::OnComposerDragOver(const FGeometry& Geometry, const FDragDropEvent& DragDropEvent)
{
	const TSharedPtr<FExternalDragOperation> ExternalOp = DragDropEvent.GetOperationAs<FExternalDragOperation>();
	return (ExternalOp.IsValid() && (ExternalOp->HasFiles() || ExternalOp->HasText()))
		? FReply::Handled()
		: FReply::Unhandled();
}

FReply SUEBridgeMCPPanel::OnComposerDrop(const FGeometry& Geometry, const FDragDropEvent& DragDropEvent)
{
	const TSharedPtr<FExternalDragOperation> ExternalOp = DragDropEvent.GetOperationAs<FExternalDragOperation>();
	if (!ExternalOp.IsValid())
	{
		return FReply::Unhandled();
	}

	TArray<FString> Paths;
	if (ExternalOp->HasFiles())
	{
		for (const FString& File : ExternalOp->GetFiles())
		{
			AddUniquePath(Paths, File);
		}
	}
	if (ExternalOp->HasText())
	{
		TryExtractPastedFilePaths(ExternalOp->GetText(), Paths);
	}

	const int32 AddedCount = AddComposerAttachments(Paths);
	if (AddedCount > 0)
	{
		FocusComposer();
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

#undef LOCTEXT_NAMESPACE
