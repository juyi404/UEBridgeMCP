#include "WorldDataMCPAnimTools.h"

#include "WorldDataMCPCommon.h"

#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Factories/AnimMontageFactory.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectIterator.h"

namespace WorldDataMCP
{
namespace AnimTools
{
namespace
{
	IAssetTools& AssetTools()
	{
		return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	}

	UObject* LoadAssetObject(const FString& Path)
	{
		FString Normalized = Path;
		Normalized.TrimStartAndEndInline();
		if (Normalized.IsEmpty())
		{
			return nullptr;
		}
		if (!Normalized.Contains(TEXT(".")))
		{
			Normalized = FString::Printf(TEXT("%s.%s"), *Normalized, *FPaths::GetBaseFilename(Normalized));
		}
		return StaticLoadObject(UObject::StaticClass(), nullptr, *Normalized);
	}

	bool SplitDestPath(const FString& Dest, FString& OutFolder, FString& OutName, FString& OutError)
	{
		FString Path = Dest;
		Path.TrimStartAndEndInline();
		int32 DotIndex = INDEX_NONE;
		if (Path.FindChar(TEXT('.'), DotIndex)) { Path.LeftInline(DotIndex); }
		if (Path.IsEmpty() || !Path.StartsWith(TEXT("/")))
		{
			OutError = TEXT("destPath must be a content path like /Game/Anim/AM_Foo.");
			return false;
		}
		OutName = FPackageName::GetShortName(Path);
		OutFolder = FPackageName::GetLongPackagePath(Path);
		return !OutName.IsEmpty() && !OutFolder.IsEmpty();
	}

	void SaveAsset(UObject* Asset)
	{
		if (Asset)
		{
			Asset->MarkPackageDirty();
			UEditorAssetLibrary::SaveLoadedAsset(Asset, /*bOnlyIfIsDirty*/false);
		}
	}

	// ---- tools ---------------------------------------------------------------------------

	FString CreateMontage(const TSharedPtr<FJsonObject>& Args)
	{
		FString DestPath, SequencePath;
		Args->TryGetStringField(TEXT("destPath"), DestPath);
		Args->TryGetStringField(TEXT("animSequence"), SequencePath);
		FString Folder, Name, Error;
		if (!SplitDestPath(DestPath, Folder, Name, Error))
		{
			return ErrorJson(Error);
		}
		UAnimSequence* Source = Cast<UAnimSequence>(LoadAssetObject(SequencePath));
		if (!Source)
		{
			return ErrorJson(FString::Printf(TEXT("Source anim sequence '%s' not found (required)."), *SequencePath));
		}
		UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
		Factory->TargetSkeleton = Source->GetSkeleton();
		Factory->SourceAnimation = Source;
		UAnimMontage* Montage = Cast<UAnimMontage>(AssetTools().CreateAsset(Name, Folder, UAnimMontage::StaticClass(), Factory));
		if (!Montage)
		{
			return ErrorJson(FString::Printf(TEXT("Failed to create montage at %s."), *DestPath));
		}
		SaveAsset(Montage);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Montage->GetPathName());
		Result->SetStringField(TEXT("source"), Source->GetPathName());
		return SuccessJson(Result);
	}

	FString AddMontageSection(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, SectionName, LinkedSection;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("sectionName"), SectionName);
		Args->TryGetStringField(TEXT("linkedSection"), LinkedSection);
		double StartTime = 0.0;
		Args->TryGetNumberField(TEXT("startTime"), StartTime);
		UAnimMontage* Montage = Cast<UAnimMontage>(LoadAssetObject(AssetPath));
		if (!Montage || SectionName.IsEmpty())
		{
			return ErrorJson(TEXT("'assetPath' (montage) and 'sectionName' are required."));
		}
		if (Montage->GetSectionIndex(FName(*SectionName)) != INDEX_NONE)
		{
			return ErrorJson(FString::Printf(TEXT("Section '%s' already exists."), *SectionName));
		}
		FCompositeSection NewSection;
		NewSection.SectionName = FName(*SectionName);
		NewSection.SetTime(static_cast<float>(StartTime));
		if (!LinkedSection.IsEmpty())
		{
			NewSection.NextSectionName = FName(*LinkedSection);
		}
		Montage->CompositeSections.Add(NewSection);
		SaveAsset(Montage);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Montage->GetPathName());
		Result->SetStringField(TEXT("section"), SectionName);
		Result->SetNumberField(TEXT("sectionCount"), Montage->CompositeSections.Num());
		return SuccessJson(Result);
	}

	FString LinkMontageSections(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, SectionName, NextSection;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("sectionName"), SectionName);
		Args->TryGetStringField(TEXT("nextSection"), NextSection);
		UAnimMontage* Montage = Cast<UAnimMontage>(LoadAssetObject(AssetPath));
		if (!Montage)
		{
			return ErrorJson(FString::Printf(TEXT("Montage '%s' not found."), *AssetPath));
		}
		const int32 Index = Montage->GetSectionIndex(FName(*SectionName));
		if (Index == INDEX_NONE)
		{
			return ErrorJson(FString::Printf(TEXT("Section '%s' not found."), *SectionName));
		}
		Montage->CompositeSections[Index].NextSectionName = FName(*NextSection);
		SaveAsset(Montage);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Montage->GetPathName());
		Result->SetStringField(TEXT("section"), SectionName);
		Result->SetStringField(TEXT("nextSection"), NextSection);
		return SuccessJson(Result);
	}

	FString AddAnimNotify(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, NotifyName, NotifyClass;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("notifyName"), NotifyName);
		Args->TryGetStringField(TEXT("notifyClass"), NotifyClass);
		double TriggerTime = 0.0;
		Args->TryGetNumberField(TEXT("triggerTime"), TriggerTime);
		UAnimSequenceBase* Anim = Cast<UAnimSequenceBase>(LoadAssetObject(AssetPath));
		if (!Anim || NotifyName.IsEmpty())
		{
			return ErrorJson(TEXT("'assetPath' (anim sequence/montage) and 'notifyName' are required."));
		}
		const float ClampedTime = FMath::Clamp(static_cast<float>(TriggerTime), 0.0f, Anim->GetPlayLength());

		FAnimNotifyEvent& NewEvent = Anim->Notifies.AddDefaulted_GetRef();
		NewEvent.NotifyName = FName(*NotifyName);
		NewEvent.Link(Anim, ClampedTime);
		NewEvent.TrackIndex = 0;

		// Optional notify instance (e.g. AnimNotify_PlaySound).
		if (!NotifyClass.IsEmpty())
		{
			UClass* NotifyUClass = FindObject<UClass>(nullptr, *NotifyClass);
			if (!NotifyUClass)
			{
				NotifyUClass = FindObject<UClass>(nullptr, *(FString(TEXT("/Script/Engine.")) + NotifyClass));
			}
			if (!NotifyUClass)
			{
				const FString Prefixed = NotifyClass.StartsWith(TEXT("AnimNotify_")) ? NotifyClass : (TEXT("AnimNotify_") + NotifyClass);
				for (TObjectIterator<UClass> It; It; ++It)
				{
					if (It->IsChildOf(UAnimNotify::StaticClass()) && (It->GetName() == NotifyClass || It->GetName() == Prefixed))
					{
						NotifyUClass = *It;
						break;
					}
				}
			}
			if (NotifyUClass && NotifyUClass->IsChildOf(UAnimNotify::StaticClass()))
			{
				NewEvent.Notify = NewObject<UAnimNotify>(Anim, NotifyUClass);
			}
		}

		Anim->SortNotifies();
		Anim->PostEditChange();
		SaveAsset(Anim);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Anim->GetPathName());
		Result->SetStringField(TEXT("notify"), NotifyName);
		Result->SetNumberField(TEXT("triggerTime"), ClampedTime);
		return SuccessJson(Result);
	}

	FString AddCurve(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, CurveName;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("curveName"), CurveName);
		UAnimSequence* Seq = Cast<UAnimSequence>(LoadAssetObject(AssetPath));
		if (!Seq || CurveName.IsEmpty())
		{
			return ErrorJson(TEXT("'assetPath' (anim sequence) and 'curveName' are required."));
		}
		const FAnimationCurveIdentifier CurveId(FName(*CurveName), ERawCurveTrackTypes::RCT_Float);
		IAnimationDataController& Controller = Seq->GetController();
		Controller.OpenBracket(NSLOCTEXT("WorldDataMCP", "AddCurve", "Add Curve"));
		const bool bAdded = Controller.AddCurve(CurveId, AACF_DefaultCurve);
		Controller.CloseBracket();
		if (!bAdded)
		{
			return ErrorJson(FString::Printf(TEXT("Curve '%s' could not be added (may already exist)."), *CurveName));
		}
		SaveAsset(Seq);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Seq->GetPathName());
		Result->SetStringField(TEXT("curve"), CurveName);
		return SuccessJson(Result);
	}

	FString AddVirtualBone(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, SourceBone, TargetBone;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("sourceBone"), SourceBone);
		Args->TryGetStringField(TEXT("targetBone"), TargetBone);
		USkeleton* Skeleton = Cast<USkeleton>(LoadAssetObject(AssetPath));
		if (!Skeleton || SourceBone.IsEmpty() || TargetBone.IsEmpty())
		{
			return ErrorJson(TEXT("'assetPath' (skeleton), 'sourceBone', 'targetBone' are required."));
		}
		Skeleton->Modify();
		FName NewBoneName;
		if (!Skeleton->AddNewVirtualBone(FName(*SourceBone), FName(*TargetBone), NewBoneName))
		{
			return ErrorJson(TEXT("AddNewVirtualBone failed (bones must exist; may already be linked)."));
		}
		Skeleton->PostEditChange();
		SaveAsset(Skeleton);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Skeleton->GetPathName());
		Result->SetStringField(TEXT("virtualBone"), NewBoneName.ToString());
		return SuccessJson(Result);
	}

	FString AddSocket(const TSharedPtr<FJsonObject>& Args)
	{
		FString AssetPath, SocketName, BoneName;
		Args->TryGetStringField(TEXT("assetPath"), AssetPath);
		Args->TryGetStringField(TEXT("socketName"), SocketName);
		Args->TryGetStringField(TEXT("boneName"), BoneName);
		USkeleton* Skeleton = Cast<USkeleton>(LoadAssetObject(AssetPath));
		if (!Skeleton || SocketName.IsEmpty() || BoneName.IsEmpty())
		{
			return ErrorJson(TEXT("'assetPath' (skeleton), 'socketName', 'boneName' are required."));
		}
		for (const USkeletalMeshSocket* Existing : Skeleton->Sockets)
		{
			if (Existing && Existing->SocketName == FName(*SocketName))
			{
				return ErrorJson(FString::Printf(TEXT("Socket '%s' already exists."), *SocketName));
			}
		}
		Skeleton->Modify();
		USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Skeleton);
		Socket->SocketName = FName(*SocketName);
		Socket->BoneName = FName(*BoneName);
		Skeleton->Sockets.Add(Socket);
		Skeleton->PostEditChange();
		SaveAsset(Skeleton);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("assetPath"), Skeleton->GetPathName());
		Result->SetStringField(TEXT("socket"), SocketName);
		Result->SetStringField(TEXT("bone"), BoneName);
		return SuccessJson(Result);
	}
}

FString GetToolDefinitionsJson()
{
	return TEXT(R"JSON([
{"name":"create_montage","description":"Create an AnimMontage from a source AnimSequence (uses its skeleton).","inputSchema":{"type":"object","properties":{"destPath":{"type":"string","description":"Content path, e.g. /Game/Anim/AM_Foo."},"animSequence":{"type":"string","description":"Source AnimSequence asset path (required)."}},"required":["destPath","animSequence"]},"annotations":{"title":"Create Montage","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_montage_section","description":"Add a named composite section to a montage at startTime, optionally linking to a next section.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"sectionName":{"type":"string"},"startTime":{"type":"number"},"linkedSection":{"type":"string"}},"required":["assetPath","sectionName"]},"annotations":{"title":"Add Montage Section","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"link_montage_sections","description":"Set a montage section's NextSectionName (chain sections).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"sectionName":{"type":"string"},"nextSection":{"type":"string"}},"required":["assetPath","sectionName","nextSection"]},"annotations":{"title":"Link Montage Sections","readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"add_anim_notify","description":"Add a notify to an AnimSequence or Montage at triggerTime. Optional notifyClass (e.g. AnimNotify_PlaySound) instantiates a notify object.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"notifyName":{"type":"string"},"triggerTime":{"type":"number"},"notifyClass":{"type":"string"}},"required":["assetPath","notifyName"]},"annotations":{"title":"Add Anim Notify","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_anim_curve","description":"Add a float animation curve to an AnimSequence (via the animation data controller).","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"curveName":{"type":"string"}},"required":["assetPath","curveName"]},"annotations":{"title":"Add Anim Curve","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_virtual_bone","description":"Add a virtual bone to a Skeleton between sourceBone and targetBone.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"sourceBone":{"type":"string"},"targetBone":{"type":"string"}},"required":["assetPath","sourceBone","targetBone"]},"annotations":{"title":"Add Virtual Bone","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}},
{"name":"add_skeleton_socket","description":"Add a socket to a Skeleton attached to a bone.","inputSchema":{"type":"object","properties":{"assetPath":{"type":"string"},"socketName":{"type":"string"},"boneName":{"type":"string"}},"required":["assetPath","socketName","boneName"]},"annotations":{"title":"Add Skeleton Socket","readOnlyHint":false,"destructiveHint":false,"openWorldHint":false}}
])JSON");
}

bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult)
{
	if (ToolName == TEXT("create_montage")) { OutResult = CreateMontage(Args); return true; }
	if (ToolName == TEXT("add_montage_section")) { OutResult = AddMontageSection(Args); return true; }
	if (ToolName == TEXT("link_montage_sections")) { OutResult = LinkMontageSections(Args); return true; }
	if (ToolName == TEXT("add_anim_notify")) { OutResult = AddAnimNotify(Args); return true; }
	if (ToolName == TEXT("add_anim_curve")) { OutResult = AddCurve(Args); return true; }
	if (ToolName == TEXT("add_virtual_bone")) { OutResult = AddVirtualBone(Args); return true; }
	if (ToolName == TEXT("add_skeleton_socket")) { OutResult = AddSocket(Args); return true; }
	return false;
}
}
}
